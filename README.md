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

| | Commandlet | **Tools → Pulse Niagara Auditor** |
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

## Editor panel

**Tools → Pulse Audit** runs the same scan interactively:

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
