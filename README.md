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
