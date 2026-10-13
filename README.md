# Pulse

Read-only performance audit for Unreal Engine 5 projects. Pulse scans every asset — via a
commandlet for CI and an editor panel for people — and emits deterministic CSV + JSON reports of
performance issues, per-asset scores, and overall project health. It never modifies an asset,
never fails a build unless explicitly asked to (`-failunder`), and produces byte-identical output
for unchanged content, so reports diff cleanly over time.

## Why

- Performance debt accumulates invisibly. Nobody notices the 8K texture, the 150k-triangle mesh
  with no LODs, or the 600 Blueprints ticking every frame until frame time is already gone — and
  finding them then means manual profiling sessions.
- Pulse makes that census automatic, repeatable, and diffable: run it on every commit, gate merges
  on the score, and diff two reports to see exactly which assets regressed.
- It complements, not replaces, Unreal Insights (runtime profiling) and Data Validation
  (correctness). Pulse is a static content audit: what is *in* the project, and what it will cost.

## Supported engine versions

| Engine | Status |
|---|---|
| UE 5.8 | Supported |
| UE 5.7 | Supported |
| UE 5.6 and earlier | Untested |

`main` builds against both 5.7 and 5.8 from the same source with **no version guards** — the
plugin ships no `EngineVersion` in its `.uplugin`, so it stays version-agnostic. If a future
change forces 5.8-only APIs, a `release/5.7` branch will be cut from the last commit that still
builds on 5.7.

## Setup

1. Drop `Pulse/` into your project's `Plugins/` folder and enable it in the `.uproject` (or via
   Edit → Plugins).
2. Configure thresholds under **Project Settings → Plugins → Pulse**. Settings land in
   `Config/DefaultPulse.ini` under `[/Script/Pulse.PulseSettings]` — check that file in, so CI and
   every editor agree on what "healthy" means. A threshold edit shows up in reports as one changed
   `SettingsHash` header line, never as unexplained row churn.
3. Two escape hatches let a project tune Pulse without forking it: `DisabledRules` suppresses a
   rule entirely, `RuleSeverityOverrides` re-ranks one (e.g. demote `Texture.NonPowerOfTwo` to
   `Info` for a UI-heavy project).

## Usage

```
UnrealEditor-Cmd.exe <Project>.uproject -run=Pulse.PulseAudit -unattended -nosplash -nop4 -nullrhi
```

That line alone runs a Fast-tier audit of the whole project and writes reports to
`Saved/Pulse/`. Switches:

| Switch | Effect |
|---|---|
| `-deep` | Deep tier: loads packages for metrics registry tags cannot provide |
| `-shaderstats` | Compiles material shaders for instruction counts. Implies `-deep`. Opt-in and slow — never make it the CI default |
| `-category=A,B` | Audit only the named categories, e.g. `Texture,StaticMesh` |
| `-path=/Game/X` | Restrict the scan to these package paths |
| `-minseverity=S` | Hide report rows below severity `S` (`Info`/`Low`/`Medium`/`High`/`Critical`). Filters rows only — never scores |
| `-maxassets=N` | Cap assets per category (a prefix of the sorted list). Marks the report truncated |
| `-reportdir=D` | Report output directory (default `Saved/Pulse`) |
| `-format=csv,json` | Which report formats to write (default both) |
| `-failunder=F` | Exit 1 when the overall score lands below `F` |
| `-gcfreq=N` | Packages loaded between garbage collections at Deep tier (default 100) |

Exit codes:

| Code | Meaning |
|---|---|
| 0 | Audit completed; score at or above the `-failunder` gate (or no gate set) |
| 1 | Score below `-failunder`, or a report file could not be written |
| 2 | Bad invocation (unknown category, malformed switch value, ...) |

## Scan tiers

| Tier | Switch | What it reads | Cost |
|---|---|---|---|
| Fast | *(default)* | Asset Registry tags only — **zero package loads** | Whole project in seconds. The CI default |
| Deep | `-deep` | Loads packages for texture memory, Blueprint tick/components, collision detail, Niagara emitters, non-World-Partition level actors | Minutes; memory bounded by a GC cadence |
| ShaderStats | `-shaderstats` | Deep plus material shader compilation for instruction counts | Slowest by far on a cold DDC. Explicitly opt-in |

Every metric records the tier that produced it, and the report header records the tier of the run
— a Fast report can never be mistaken for a Deep one. Deep-tier CSV columns are present but empty
in a Fast run, so the header row is identical across tiers and diffs show only real change.

### Cooked texture memory needs rendering enabled

`Texture.MemoryBytes`, `CookedWidth`, `CookedHeight`, and `NumMips` come from the texture's built
platform data, and the engine only builds that when `FApp::CanEverRender()` is true. That is false
whenever `-nullrhi` is passed **or** the process is a commandlet without `-AllowCommandletRendering`
— so the standard headless invocation above cannot measure them. To collect them, drop `-nullrhi`
and add `-AllowCommandletRendering`:

```
UnrealEditor-Cmd.exe <Project>.uproject -run=Pulse.PulseAudit -deep -category=Texture ^
    -unattended -nosplash -nop4 -AllowCommandletRendering
```

Expect a slower start, since enabling rendering makes the engine compile its default material
shaders during boot. When these metrics are unavailable Pulse **omits them entirely** rather than
writing zeros, and logs one warning naming the flags — an unmeasured texture must never be
mistaken for a free one. Every other Deep metric works fine under `-nullrhi`.

## Reports

Written to `Saved/Pulse/` (`-reportdir=` overrides):

| File | Contents |
|---|---|
| `Summary.csv` | Per-category scores and headline counters |
| `Assets_<Category>.csv` | One row per asset; columns come from the collector's metric schema |
| `Issues.csv` | One row per rule hit: asset, rule id, severity, message, recommendation |
| `Statistics.json` | The full report — header, categories, assets, metrics, issues |
| `PulseAudit_<timestamp>.json` | Snapshot copy of `Statistics.json`, pruned to `MaxHistoricalSnapshots` |

**Determinism guarantee:** two runs on unchanged content produce byte-identical reports. No
timestamps appear in file content — the generation time lives only in the header's dedicated
field and the snapshot *filename* — and all sorting, float formatting, and column ordering are
fixed. The header carries a `SettingsHash` (MD5 over the threshold block), so a settings edit is
one visible header-line change instead of silent churn, and `Compare-PulseReport.ps1` refuses to
diff reports whose hashes differ.

## Scoring

Three levels, each bounded and monotone:

- **Per asset** — issues sum to a penalty `P` (Info 0, Low 3, Medium 8, High 20, Critical 45),
  then `S = 100 · exp(−P / 60)`. Exponential rather than clamped-linear so badly broken assets
  keep a meaningful order all the way down instead of piling up at zero.
- **Per category** — depth × breadth: `S = (100 − MeanDeficit) · (1 − 0.5 · FailShare)`, where
  `MeanDeficit` is the average per-asset score deficit and `FailShare` is the fraction of assets
  with at least one Low-or-worse issue.
- **Project** — a fixed-weight blend of non-empty category scores (Level weighs 1.2 by default;
  empty categories are excluded, not scored 100).

The intuition, at 1,000 assets in a category: **one asset with 40 Medium issues costs 0.15
points; 40 assets with one Medium issue each cost 2.49** — sixteen times more for the same issue
count. That is deliberate: one broken asset is one fix, breadth is forty fixes. And because the
blend uses fixed weights instead of asset counts, importing 500 clean props cannot inflate the
score.

All weights and constants are exposed in Project Settings.

## Niagara auditor

`Pulse.PulseAudit` measures nothing — it reads what assets declare. The Niagara auditor is the
opposite: a separate commandlet that **spawns** every Niagara system under a path, ticks it, and
records what it actually cost.

```
UnrealEditor-Cmd.exe <Project>.uproject -run=Pulse.PulseNiagaraAuditor ^
    -path=/Game/VFX -quality=0,3 -AllowCommandletRendering -unattended -nosplash -nop4
```

It is deliberately **not** part of the asset audit. That audit is static, deterministic and
byte-diffable; this one reports measured milliseconds, which are none of those. Its settings live in a
separate `UPulseNiagaraAuditorSettings` section for the same reason — folding them into
`UPulseSettings` would make a spawn-count tweak change the audit's `SettingsHash`.

### Two front ends

| | Commandlet | **Tools → Pulse → Niagara Auditor** |
|---|---|---|
| World | Private, created and ticked by the auditor | A **PIE session** it starts and ends |
| Quality sweep | Yes | Yes |
| **Distance culling** | **Paused** — see below | **Live**, against the player's view |
| Distance axis | Unavailable | Yes |
| GPU-emitter systems | Skipped by default | Measured |

The panel runs in PIE rather than in the editor world, and that is the point: PIE gives a real local
`PlayerController`, which is the *first* branch of `FNiagaraWorldManager::PrepareCachedViewInfo` —
the same path a shipping build takes — instead of the editor-viewport fallback. Tick groups, world
type and game mode are the game's. The driver starts the session, puts the controller into spectator
state, and ends the session when the run finishes or is cancelled.

Both run the same `FPulseNiagaraAuditDriver`, stepped unbounded by the commandlet and on a frame
budget by the panel — the same split `FPulseScanDriver` makes for the asset audit.

### What it measures

Game-thread cost per system, from `FParticlePerfStats` — the same source the Niagara Debugger's
Outliner reads. For each system, at each quality level and distance:

- spawn `InstancesPerSystem` instances,
- tick `SettleFrames` (discarded — spawn and first-tick costs land here),
- tick `MeasureFrames`, recording a per-frame cost series,
- report **avg / min / max in milliseconds**, for the whole population and per instance.

Render-thread and GPU costs are **not** measured, and are absent rather than zero.

Averages are taken over every frame in the window; **min and max are taken over frames that did
work**, and the rest are reported as `IdleFrames`. Niagara attributes concurrent tick work on the
frame the async task completes, so a system ticking every frame still books occasional zero frames
followed by double-weight ones. The window total is unaffected, so the average holds — but a
minimum over the raw series would read `0.0000` and suggest the effect is sometimes free. A high
`IdleFrames` count means the spread is a sampling artefact and only the average should be trusted.

### Why the commandlet pauses distance culling

`FNiagaraWorldManager` culls by comparing its closest cached view against the effect type's
`MaxDistance`. Those cached views come from exactly two places: a local `PlayerController` with a
real viewport, or the level editor viewport. A commandlet has neither — so the view list is empty,
the closest distance stays `FLT_MAX`, and **every system with a distance-culling Effect Type would
cull on its first tick and report as nearly free**.

That failure reads as "this effect is cheap", and it hits well-authored content hardest, since a
system needs an Effect Type to be culled at all. So the commandlet spawns its instances with
`SetAllowScalability(false)`, which keeps them out of the scalability manager that does the culling
while leaving quality-level emitter enablement — decided on `UNiagaraSystem` through the platform
set — completely untouched. It then measures uncculled cost and records
`scalabilityCullingExercised: false` in `Run.json` as data rather than prose.

To measure culling, use the panel — the level editor viewport is rendering, so the views exist and
instances placed at a distance are culled exactly as they would be in game.

### Quality sweep

Niagara reads its own `fx.Niagara.QualityLevel`, which is *not* moved by
`Scalability::SetQualityLevels` unless the project's `Scalability.ini` maps `EffectsQuality` onto
it. The auditor sets it explicitly through `FNiagaraPlatformSet::SetNiagaraQualityLevelOverride`
and **reads it back**, failing the run on disagreement — otherwise every level would measure
identically and nothing would say so.

One CSV per level, named from `UNiagaraSettings::QualityLevels` so a project that renamed or added
levels gets its own names on the files:

```
Saved/Pulse/NiagaraAuditor/NiagaraAudit_<timestamp>/
    Low.csv           per-system detail at that level
    Epic.csv
    Comparison.csv    one row per system, avg/min/max side by side per level
    Run.json          engine, machine, RHI, and every measurement parameter
```

`Comparison.csv` is the file the run exists to produce. A single system's milliseconds are hard to
judge; a system that costs the same at Low as at Epic, or the same far away as up close, is an
unambiguous defect. Two columns say so directly:

- **`FlatScalability`** — cost barely drops between the lowest and highest quality level, so the
  quality settings are not doing anything.
- **`FlatDistanceCulling`** — cost barely drops between the nearest and farthest distance, so
  significance culling is authored but never engaging. Panel runs only.

One distance is a sample, not a test: a far reading that comes back cheap cannot distinguish
"culling works" from "this system was always cheap". That is why `MeasureDistances` defaults to a
near and a far value rather than one.

### The stage

Modelled on PSO Forge's capture stage: **the content is pinned in world space and the camera moves
to frame it**, never the other way round.

- The stage origin is fixed once at the start of a run. Grid, floor, lights and the camera track are
  all expressed relative to it, so the geometry of a run is identical wherever in the map it happens.
- A transient `ACameraActor` is spawned and made the player controller's view target via
  `SetViewTargetWithBlend`, exactly as PSO Forge does with its capture camera. That is what locks the
  view: you are rendering through an actor the stage owns and moves, so no pawn input can change what
  is being measured against. Move and look input are disabled too, for anything that drives the pawn
  directly.
- **Distance moves the camera, not the effects.** For each distance sample the camera slides back
  along −X and looks level down +X. The population therefore sits in the same place, on the same
  floor, under the same lights at every distance — only the viewing distance changes, which is the
  one thing the distance axis is supposed to vary.
- Instances form a centred grid in the YZ plane at a single depth, so every instance in a population
  is the same distance from the camera. Niagara culls per instance, so a grid with depth would
  straddle the cull radius and measure where the boundary fell rather than what the effect costs. A
  warning fires when the grid's half-extent is large next to the distance being measured.
- The grid is lifted by its own half-height plus a clearance, so its bottom row stands on the floor
  rather than sinking through it. A fixed height would not do — a tall grid extends further down than
  any sensible constant.
- The floor sits at the stage origin and is scaled to cover the farthest camera position plus the
  grid, so it reaches back under the camera at maximum distance. Without that, far samples would hang
  past its edge and lose the shadows and depth-fade the near samples had — a rendering difference
  along the very axis being compared.

### Test map

Measuring inside whatever level is open means that level's content competes for the game thread, so
the same system measures differently depending on the map. Set `TestMap` in settings and the panel
offers an **Open test map** button — it prompts to save first, then loads, and never swaps the level
on its own. When you are in that map the auditor also spawns the environment rig (lights, sky, fog,
floor) on the assumption it is bare; in your working level it does not, since that already has
lighting.

Running outside the test map is allowed and warned about, not blocked.

**Assign `APulseAuditGameMode` as the test map's GameMode Override** in World Settings, the same way
PSO Forge's capture map uses `APSOForgeGameMode`. It is deliberately not Niagara-specific — anything
Pulse measures by spawning into a play session wants the same empty stage. It gives a spectator pawn, no HUD and no
gameplay. Your project's default game mode is not free — it spawns a character with physics and
animation ticking, usually a HUD, often gameplay subsystems, all competing for the game thread the
auditor is measuring. It cannot be forced programmatically: `FRequestPlaySessionParams` has no
game-mode field, and writing `DefaultGameMode` onto the editor world would dirty your map.

Not assigning it still works — the auditor forces spectator state at PIE start regardless — you just
also measure whatever else your game mode starts.

`APulseAuditGameMode` lives in Pulse's Editor module, so a map referencing it is a development map
and must not be shipped; cooking one would leave the World Settings reference unresolvable.

### Console commands

```
Pulse.Niagara.Start [/Game/VFX]    start a run in the current level, report written on completion
Pulse.Niagara.Cancel               cancel and write the partial report
Pulse.Niagara.Status               log progress
```

Same driver as the panel, and the same viewport overlay — useful from `-ExecCmds` or with the tab
closed.

### Viewport overlay

While a run is live, a heads-up panel is drawn over the level viewport showing the system currently
on the stage, its quality level and distance, the instance count, and the cost accumulating live,
with a cancel button. During the settle frames it says `settling...` rather than showing a figure —
those frames are discarded, so a number there would be one the report never contains.

An editor run is something you watch: the systems spawn in front of the camera, and "is this the
expensive one?" gets asked while looking at the viewport, not at a docked tab.

The panel also has a **Niagara HUD** button, which toggles Niagara's own `fx.Niagara.Debug.Hud`.
Pulse reports what a system costs; that HUD shows whether it is alive, its scalability state, and
whether it was culled — which is what you want the moment a number looks wrong. (The Niagara
Debugger's editor half is private to `NiagaraEditor` and cannot be driven from a plugin, so the
console command is the supported route to the same display.)

### CI

`Tools/Invoke-PulseNiagaraAudit.ps1` wraps the commandlet the way `Invoke-PulseAudit.ps1` wraps the
asset audit, resolving the engine from the project's `EngineAssociation` and returning the
commandlet's exit code:

```powershell
.\Plugins\Pulse\Tools\Invoke-PulseNiagaraAudit.ps1 -ProjectFile C:\Path\To\YourGame.uproject -Path /Game/VFX -FailOverBudget
```

It passes `-AllowCommandletRendering` and never `-nullrhi`, for the reason in the next section.

### Switches

| Switch | Effect |
|---|---|
| `-path=/Game/VFX` | Paths to scan. Default: the auditor's `ScanPaths` |
| `-quality=0,3` | Niagara quality levels to sweep. Default: Low and Epic |
| `-instances=N` | Instances spawned per system |
| `-settle=N` / `-frames=N` | Discarded and recorded frame counts |
| `-maxsystems=N` | Cap on systems measured |
| `-reportdir=<dir>` | Override the report directory |
| `-includegpu` | Measure GPU-emitter systems despite the caveat above |
| `-failoverbudget` | Exit 1 when any system exceeds its per-instance frame-budget share |

Do **not** pass `-nullrhi`, and prefer `-AllowCommandletRendering`: the same `FApp::CanEverRender()`
gate described under [Cooked texture memory](#cooked-texture-memory-needs-rendering-enabled)
applies here.

### Caveats

- Measurements are machine-dependent. Thresholds are a share of the frame budget
  (`PerformanceTarget`) rather than raw milliseconds so they survive a hardware change, but the raw
  milliseconds in the CSVs are only comparable against runs on the same machine.
- **Commandlet and panel numbers are not comparable to each other.** One is uncculled in an empty
  private world; the other is culled inside your open level. `Run.json` records which, on every run.
- One instance is not scene cost. `InstancesPerSystem` chooses the operating point: Niagara batches
  instances of one system into a single simulation, so per-instance cost falls as that count rises.
- Systems whose data interfaces need real context — skeletal mesh sampling, collision queries, data
  channels — measure lower than they cost in a real level, more so in the commandlet's empty world.
- The panel spawns transient actors into your open level and holds a realtime override on the
  viewport while it runs. Both are removed when the run ends or is cancelled, and nothing is saved.

## Memory auditor

The asset audit judges assets one at a time. The memory auditor answers a different question: **what
does loading this keep in memory, and why?** It ranks every top-level asset by the RAM and VRAM its
hard references keep resident, and for each one lists the dependencies responsible along with the
reference chain that pulls each of them in.

```
UnrealEditor-Cmd.exe <Project>.uproject -run=Pulse.PulseMemoryAuditor -unattended -nosplash -nop4 -nullrhi
```

Or open **Tools → Pulse → Memory Auditor** and press **Analyze**. Both read only the Asset Registry —
nothing is loaded — so a whole project takes seconds, and `-nullrhi` is fine.

### Top-level assets

| Kind | Where it comes from |
|---|---|
| **Always loaded** | The Maps & Modes default GameMode and GameInstance class, plus `AlwaysLoadedAssets`. Resident in every map |
| **Map** | Every map under the scan paths |
| **Primary asset** | Everything the Asset Manager lists under the scan paths |
| **Configured** | Every asset of an `AdditionalRootClasses` class, e.g. a data asset class |
| **Blueprint** | Every Blueprint under the scan paths, Widget and Anim Blueprints included (`bIncludeBlueprints`, on by default). Editor Utility Blueprints are skipped |
| **Inspected** | Dropped on the panel, picked with right-click **Inspect**, or named with `-inspect=` |

An asset that qualifies twice is listed once, under the first kind in that table; an inspected asset
that is also a map stays a map and is marked *(inspected)*. Dependencies are followed wherever they
lead, including engine and plugin content; the scan paths only choose the top-level assets.

The first three kinds are **load roots**: something other than a reference loads them. The rest are
questions — "what would loading this Blueprint cost?" — about assets that normally load because
something references them. Only load roots shape Unique and Reach, so listing every Blueprint does not
change a single map's numbers.

### The numbers

| Column | Top-level row | Dependency row |
|---|---|---|
| **RAM / VRAM** | Everything it keeps resident: itself plus its whole hard-reference closure | The package itself |
| **Unique RAM / VRAM** | What leaves memory if nothing loads it any more | The same, for that dependency |
| **Reach** | Packages it loads | How many load roots (maps, primary assets, always-loaded) load it |
| **Src** | Share of the total computed from tags rather than disk size | `Tags`, `Disk` or `?` |
| **Via** | Why it is top-level | The shortest reference chain from the top-level asset, flagged `[+N other referencers]` when more than one package holds it |

**Unique** is the column to act on. A Blueprint showing 800 MB total and 2 MB unique is fine: that
memory is shared and would be loaded anyway. One showing 300 MB unique is the problem. Unique is the
asset's retained size: its subtree in the dominator tree of the hard-reference graph, the same measure
a heap profiler reports, computed over the load roots. It accounts for overlap between them, so a
pawn that the GameMode references still gets credit for what only the pawn pulls in. A Blueprint or
inspected asset that no load root reaches is measured in a second pass that adds the question roots,
so it still gets a figure without moving anyone else's.

The summary line reports the **baseline**: the union of what the always-loaded assets keep resident.
That memory is paid in every map before the map loads anything, which makes it the most important
number in the report.

### What counts and what does not

- **Hard references only** count toward the totals, and only game dependencies; editor-only
  references never load in a cooked build.
- **Soft references** are listed greyed as *may load* (toggle **Show may-load**) and never count.
  Code decides whether they load.
- **Streaming** bytes are never counted as resident. A streaming texture keeps only its smallest
  mips (the engine's resident-mip floor) in memory while referenced; the rest streams on demand. The
  same goes for Nanite pages, mesh distance fields, virtual textures and compressed audio. The
  streaming figure appears in tooltips and in the CSVs.
- **World Partition** maps: actors stream per cell, so they are *may load*, and a partitioned map's
  total is its persistent level only. A non-partitioned map that stores actors one file per actor
  loads them all with the map, so they are counted.

### Estimates, not measurements

Sizes come from registry tags:

| Class | Estimate |
|---|---|
| Texture2D | Built pixel format × dimensions, clamped by `MaxTextureSize` and `LODBias`, with the mip chain; split by the engine's `IsPossibleToStream` rules |
| StaticMesh / SkeletalMesh | Vertex and index buffers from vertex/triangle counts across the LOD chain (each LOD assumed to carry half the previous one), plus skin weights |
| AnimSequence | The engine's own compressed-size tag |
| SoundWave | Duration × sample rate × channels as 16-bit PCM, divided by `AudioCompressionRatio`, as streaming |
| Everything else | Editor package size on disk, bucketed by class (textures and built lighting to VRAM, the rest to RAM) |

Disk size includes editor-only data such as Blueprint and material graphs, so it usually overstates.
Device-profile texture LOD settings, platform-specific formats and shader maps are not modelled. Use
the numbers to **rank and to find the chain**, not to budget to the byte; every row says where its
size came from.

### Switches

| Switch | Effect |
|---|---|
| `-path=/Game/A,/Game/B` | Take top-level assets only from these paths. Default: the auditor's `ScanPaths` |
| `-inspect=/Game/A/BP_X,...` | Add these assets as top-level and write an asset report for each. Not limited to the scan paths |
| `-reportdir=<dir>` | Override the report directory (default `Saved/Pulse/MemoryAuditor`) |
| `-nosoft` | Skip soft references entirely (no *may load* rows) |
| `-failoverbudget` | Exit 1 when the baseline or any top-level asset exceeds a configured budget |

Exit codes follow the other commandlets: 0 ran, 1 over budget with `-failoverbudget` (or a report
could not be written), 2 bad invocation or nothing to analyse. `Tools/Invoke-PulseMemoryAudit.ps1`
wraps it for CI.

### Inspecting one asset

The workflow for cutting a specific asset down:

1. Open **Tools → Pulse → Memory Auditor**.
2. Drag the asset in from the Content Browser (or right-click any row and choose **Inspect**). The
   analysis re-runs with it as a top-level asset, pinned at the top of the list and expanded.
3. Right-click it and choose **Export Asset Report**. The folder opens when it is written.
4. Hand `Summary.md` to whoever is going to cut the references.

```
Saved/Pulse/MemoryAuditor/AssetReport_<Asset>_<timestamp>/
    Summary.md         totals, then the 30 biggest cuts: what each saves, every referencer, the chain
    Dependencies.csv   every dependency, uncapped, may-load included
```

The report ranks cuts by **Saves**: how much *this asset's* total drops if a dependency stops loading,
from a dominator tree over the asset's own closure. That is a different question from the game-wide
Unique. A texture a map also loads has zero game-wide Unique, yet cutting this asset's reference to it
still shrinks this asset, and that is what someone trimming a character wants to see.

**Clear** drops the results and every inspected asset. **Stop Inspecting** on a pinned row removes just
that one. `-inspect=/Game/Characters/BP_Player` on the commandlet writes the same report for CI.

### Breaking a chain

Expand a top-level asset, sort by **Unique VRAM** or **Unique RAM**, and read **Via** for the
biggest rows. Via is the shortest route; when it ends in `[+N other referencers]`, hover it for the
full list of packages that reference the asset directly. The asset leaves memory only once every one
of them drops the reference (switches it to a soft reference, or stops referencing it). A common fix
is a cast to a Blueprint class in a frequently loaded Blueprint: casting hard-references the target
class and everything it references. Replacing the cast with an interface or a native base class
breaks that link. Right-click a row to open the Reference Viewer on it, or to copy the chain and
referencer list into a ticket.

### Reports

`Saved/Pulse/MemoryAuditor/MemoryAudit_<timestamp>/`:

| File | Contents |
|---|---|
| `Roots.csv` | One row per top-level asset: total, unique and may-load bytes, package counts, over-budget flag |
| `Dependencies.csv` | Each top-level asset's largest dependencies (`MaxDependenciesPerRootInReport`, hard and soft capped separately), each with its shortest reference chain (`Via`) and every package that references it directly (`Referencers`, `ReferencedBy`) |
| `Assets.csv` | Every package any top-level asset loads: own size, retained size, how many top-level assets reach it |
| `Run.json` | Baseline, totals, the settings used, and what the estimates do and do not include |

Byte columns are raw integers. File contents carry no timestamp or machine name, so an unchanged
project produces byte-identical files and a diff shows only real change.

### Settings

**Project Settings → Plugins → Pulse - Memory Auditor**, stored in `Config/DefaultPulse.ini` under
`[/Script/Pulse.PulseMemoryAuditorSettings]`. A separate section from `UPulseSettings`, so a budget
change never moves the asset audit's `SettingsHash`. Budgets are in megabytes; 0 disables a check.

## Editor panel

**Tools → Pulse** opens one window for every Pulse tool, with a row of buttons across the top to
switch between **Asset Audit**, **Niagara Auditor** and **Memory Auditor**. Each tool keeps running
and keeps its results while another is showing, so a Niagara measurement is not interrupted by a look
at the memory report, and the window reopens on whichever tool was showing last.

The **Asset Audit** section runs the same scan as the commandlet, interactively:

- Tier selector (Fast / Deep / ShaderStats) with a cost warning on the latter two, category and
  minimum-severity filters, progress bar and cancel.
- Results in a sortable list; **double-click any row to sync the Content Browser to the asset**.
- The scan runs on a per-frame time budget, so the editor stays responsive even at Deep tier.
- "Export report" writes through the same report writer as the commandlet — the files are
  byte-identical for the same tier.

## CI integration

`Tools/Invoke-PulseAudit.ps1` wraps the commandlet for CI — it locates the engine from the
project's `EngineAssociation`, builds the argument list, and returns the commandlet's exit code:

```powershell
.\Plugins\Pulse\Tools\Invoke-PulseAudit.ps1 -ProjectFile C:\Path\To\YourGame.uproject -FailUnder 85
```

For regression gating between branches or builds, diff two `Statistics.json` files:

```powershell
.\Plugins\Pulse\Tools\Compare-PulseReport.ps1 -Baseline main\Statistics.json -Current pr\Statistics.json -FailOnRegression
```

The compare script prints overall and per-category score deltas, lists new and resolved rule hits
per asset, and (with `-FailOnRegression`) exits 1 when the overall score dropped or new
Critical/High issues appeared. It refuses to compare truncated reports or reports produced under
different settings — those deltas would be noise, not signal.

### Turning categories off

Every category is audited by default. To switch one off permanently, add its name to
**Project Settings → Plugins → Pulse → Categories → Disabled Categories** (a dropdown of the
registered categories, so plugin-provided collectors appear too). A disabled category is not
enumerated, scored, or written at all — it never shows up as an empty report section and never
affects the overall score, and the run logs which categories it skipped.

Use this when the intent is permanent ("this project has no Niagara worth auditing"); use
`-category=` to narrow a single run.

## Known limitations

- **Fast-tier texture dimensions are the *source* size**, from the registry's `Dimensions` tag
  (`GetImportedSize()`), not the cooked size — metrics are named `SourceWidth`/`SourceHeight` to
  make that unmistakable. An 8K source clamped by `MaxTextureSize=512` is fine, which is why the
  oversize rule reads `MaxTextureSize` and `LODGroup` as co-inputs and drops severity when either
  clamps. The true cooked footprint requires `-deep`.
- **The Niagara rule says "cannot be pooled", never "pooling is disabled"** — the engine has no
  pooling on/off switch on the system; pooling is chosen per spawn call (`ENCPoolMethod`). What
  the asset controls is capacity: `MaxPoolSize`/`PoolPrimeSize` both 0 means no spawn call can
  ever pool the system, and that is exactly what Pulse flags.
- **Shader stats require actual shader compilation.** On a cold DDC this is the dominant cost of a
  `-shaderstats` run; materials are pre-warmed in parallel batches, but there is no free path to
  instruction counts. Narrow with `-category=Material`, and expect warm-DDC reruns to be fast.
- **The World Partition actor census counts actor descriptors** (external-actor packages), which
  is what exists without loading the map. Runtime-spawned actors are invisible to any static
  audit; non-World-Partition maps need `-deep` for their actor counts.

## Changelog

See [CHANGELOG.md](CHANGELOG.md).

## License

MIT — see [LICENSE](LICENSE).
