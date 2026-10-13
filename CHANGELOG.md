# Changelog

All notable changes to Pulse are recorded here. Versions follow the `VersionName` field in
`Pulse.uplugin`.

## 0.3.0

### Changed

- The three editor tools now share one window, **Tools → Pulse**, with segmented buttons across the
  top for **Asset Audit**, **Niagara Auditor** and **Memory Auditor**, replacing three separate
  Tools menu entries. Each tool's panel is unchanged.
- Every panel is built once and kept alive while another is showing, so a running Niagara
  measurement is not cancelled by switching sections, and each tool keeps its results across
  switches. The window reopens on the section shown last, per user and per project.
- The old per-tool tab ids (`PulseAudit`, `PulseNiagaraAuditor`, `PulseMemoryAuditor`) stay
  registered but hidden from menus, so an editor layout saved with one of them open restores as the
  Pulse window on that tool instead of losing the tab.

### Fixed

- The Niagara tool's icon is looked up across every registered style. It lives in the Niagara
  editor's own style set, not the app style the tab previously asked, so it never resolved.

## 0.2.0

### Added

- `Pulse.PulseMemoryAuditor` commandlet and **Tools → Pulse Memory Auditor** panel: ranks every
  top-level asset — maps, primary assets, and the always-loaded set — by the RAM and VRAM its hard
  references keep resident, and lists each one's dependencies with the shortest reference chain that
  loads them. Registry-only: no package is loaded, so a whole project analyses in seconds and the
  reports are byte-identical across reruns on unchanged content.
- Two costs per top-level asset. **Total** is everything it hard-references; **Unique** is what
  leaves memory if nothing loads it any more, computed as its subtree in the dominator tree of the
  hard-reference graph — the "retained size" a heap profiler reports — so a root that shares a
  material library with everything else is not blamed for it.
- An always-loaded **baseline**: the union of what the Maps & Modes GameMode and GameInstance and
  any configured `AlwaysLoadedAssets` keep resident, i.e. the memory paid in every map before the
  map loads anything.
- Sizes estimated from registry tags. Textures from their built pixel format, dimensions, max size,
  LOD bias and mip settings, split into resident mips and streaming mips using the engine's own
  `IsPossibleToStream` rules and resident-mip floor; virtual textures count as streaming. Static and
  skeletal meshes from vertex and triangle counts across their LOD chain, with Nanite pages and
  distance fields as streaming. Animations from the engine's compressed-size tag; sound waves as
  streamed compressed audio. Everything else falls back to editor disk size, labelled `DiskSize` on
  every row so an estimate is never mistaken for a measurement.
- Streaming bytes are reported separately and never counted as resident: a hard reference to a
  streaming texture keeps only its low mips in memory.
- Soft references, and World Partition actors (which stream per cell), are reported as **may load**
  and never counted in the totals. Actors of a non-partitioned one-file-per-actor map load with the
  map and are counted.
- Reports in `Saved/Pulse/MemoryAuditor/MemoryAudit_<timestamp>/`: `Roots.csv`, `Dependencies.csv`
  (each root's largest dependencies with their reference chain), `Assets.csv`, and `Run.json`.
- Breaking a chain needs every reference, not just one: each dependency lists all the packages in
  that root's closure that reference it directly (`Referencers`, `ReferencedBy` in
  `Dependencies.csv`; `[+N other referencers]` and a full list in the panel's Via tooltip). The
  shortest chain shows how it gets loaded; the referencer list shows what has to change for it to
  stop loading.
- `UPulseMemoryAuditorSettings` in `Config/DefaultPulse.ini`: which kinds of asset count as
  top-level, extra always-loaded assets, `AdditionalRootClasses` (e.g. every Blueprint), the audio
  compression ratio, and optional RAM/VRAM budgets for the baseline and per top-level asset, gated
  in CI with `-failoverbudget`.
- The panel ranks largest-first with sortable RAM, VRAM, Unique and Reach columns, filters by kind
  and name, and expands each asset to its dependency list. Double-click browses to an asset without
  loading it; the context menu opens the engine's Reference Viewer or copies the reference chain.
- `Tools/Invoke-PulseMemoryAudit.ps1`, the CI wrapper for the memory commandlet.
- `Pulse.PulseNiagaraAuditor` commandlet: measures what Niagara systems actually cost by spawning
  them on a headless stage and ticking them, rather than reading what they declare. Game-thread
  cost per system from `FParticlePerfStats` — the source the Niagara Debugger's Outliner reads —
  reported as avg/min/max over a recorded per-frame series, for the whole population and per
  instance.
- Quality sweep with one CSV per level plus `Comparison.csv`, which puts every level side by side
  per system and flags `FlatScalability` where a system costs the same at Low as at Epic. Level
  names come from `UNiagaraSettings::QualityLevels`, so renamed or added levels are labelled
  correctly. The level is set through `FNiagaraPlatformSet::SetNiagaraQualityLevelOverride` and
  read back, because `Scalability::SetQualityLevels` does not move `fx.Niagara.QualityLevel` unless
  the project's `Scalability.ini` maps it — without the read-back every level would measure
  identically and nothing would report it.
- `UPulseNiagaraAuditorSettings` in `Config/DefaultPulse.ini`: spawn count, settle and measure
  frame counts, fixed frame delta, per-system timeout, quality levels, distances, test map, and
  frame-budget thresholds driven by `EPulsePerformanceTarget`. Deliberately a separate
  `UDeveloperSettings` from `UPulseSettings` so measurement knobs cannot change the asset audit's
  `SettingsHash`.
- Render-thread and GPU costs are omitted rather than reported as zero, and GPU-emitter systems are
  skipped by default (`-includegpu` overrides): a headless run issues no scene render, so those
  figures would understate cost rather than merely be missing.
- **Tools → Pulse Niagara Auditor** editor panel, running the same `FPulseNiagaraAuditDriver` on a
  frame budget so the editor stays responsive. Results sort most-expensive-first, double-click syncs
  the Content Browser, and Export writes the same files the commandlet does.
- Panel and console runs measure inside a **PIE session** the driver starts and ends, with the player
  controller put into spectator state, rather than in the editor world. PIE provides a real local
  `PlayerController` — the first branch of `FNiagaraWorldManager::PrepareCachedViewInfo`, and the
  same path a shipping build takes — instead of the editor-viewport fallback, and gives game tick
  groups and world type. The session starts at the editor camera's position, since a bare test map
  usually has no PlayerStart.
- `APulseAuditGameMode`: spectator pawn, no HUD, no gameplay. Shared stage infrastructure rather than
  part of the Niagara tool — anything Pulse measures by spawning into a play session wants the same
  empty stage. Assign it as a measurement map's GameMode Override the way PSO Forge's capture map
  uses `APSOForgeGameMode`; a project's default game mode otherwise ticks a character, a HUD and its
  subsystems alongside what is being measured. Optional, since spectator state is forced at PIE start
  either way, and editor-only, so a map referencing it must not be shipped.
- Distance axis (panel only): `MeasureDistances` places each system at a set of distances from the
  viewport camera, and `Comparison.csv` gains `DistanceSavingPercent` and `FlatDistanceCulling` to
  flag systems whose cost does not drop with distance — significance culling authored but never
  engaging. Defaults to a near and a far value, because one distance is a sample rather than a test.
- Commandlet runs pause distance culling and say so in `Run.json`. `FNiagaraWorldManager` sources
  cull distance from a local `PlayerController` with a real viewport or from the editor viewport; a
  commandlet has neither, so left enabled every system with a distance-culling Effect Type would
  cull on its first tick and report as nearly free — worst on the best-authored content, since a
  system needs an Effect Type to be culled at all.
- Environment rig (directional light, sky light, sky atmosphere, point light, height fog, floor),
  built only when `FApp::CanEverRender()`, so lit and translucent renderers are not measured against
  an empty void. It anchors to the captured view rather than the world origin, and the floor scales
  to cover the farthest configured distance plus the grid — a system at 20,000 cm would otherwise
  hang past the end of a fixed floor and lose the shadows and depth-fade the near samples had.
- Instances are laid out as a centred, camera-facing grid at a single depth, so every instance in a
  population is the same distance from the view. Niagara culls per instance, so a grid with depth
  would straddle the cull radius and measure the boundary rather than the effect. A warning fires
  when the grid's half-extent is large next to the distance being measured.
- The stage follows PSO Forge's arrangement: content pinned at a fixed world transform, with a
  transient `ACameraActor` made the player controller's view target through `SetViewTargetWithBlend`
  and moved back per distance sample. Distance now moves the camera rather than the effects, so the
  population sits in the same place, on the same floor, under the same lights at every distance.
  Move and look input are disabled as well, for anything that drives the pawn directly.
- Fixes far samples spawning below the floor. Placing the grid at `ViewLocation + Forward * Distance`
  sank it by `Distance * sin(pitch)` for any downward camera pitch — metres at 500 cm, tens of metres
  at 20,000 cm — so far samples lost the shadows and depth-fade the near ones had, along the very
  axis being compared. With the stage pinned and the camera moving, pitch cannot enter the geometry.
- The grid is lifted by its own half-height plus a clearance so its bottom row stands on the floor,
  and the floor is sized from the farthest camera position so it reaches back under the camera.
- Costs are reported in **milliseconds**, the same unit as the frame budget every threshold is
  expressed in, at four decimals so a cheap per-instance figure does not round to zero.
- Averages cover every frame in the window, but min and max cover only frames that did work, with
  the remainder reported as `IdleFrames`. Niagara attributes concurrent tick work on the frame the
  async task completes, so a system ticking every frame still books occasional zero frames; a
  minimum over the raw series read `0.0000` and suggested the effect was sometimes free. A sample
  whose every frame was idle is now reported as unmeasured rather than as zero cost.
- `TestMap` setting and an **Open test map** button on the panel, which prompts to save before
  loading and never swaps the level on its own. The environment rig is spawned in the editor only
  when the open level is that test map, since a working level already has lighting.
- `Pulse.Niagara.Start [path]`, `Pulse.Niagara.Cancel` and `Pulse.Niagara.Status` console commands,
  driving the same audit as the panel with progress to the log.
- Viewport overlay while a run is live, in the spirit of `SPSOForgeOverlay`: current system, quality
  level, distance, instance count, live cost and a cancel button, drawn over the level viewport by
  both the panel and the console commands. Settle frames show `settling...` rather than a figure the
  report will never contain.
- **Niagara HUD** button on the panel, toggling `fx.Niagara.Debug.Hud`. Pulse reports cost; that HUD
  reports liveness, scalability state and culling — the questions a surprising cost raises. The
  Niagara Debugger's editor half is private to `NiagaraEditor`, so the console command is the
  supported route to the same display.
- `Tools/Invoke-PulseNiagaraAudit.ps1`, the CI wrapper for the Niagara commandlet. It passes
  `-AllowCommandletRendering` and never `-nullrhi`, since a null scene understates every measurement.
- The commandlet sets `IsClient = true`, unlike the asset audit's. `FRendererModule::AllocateScene`
  only builds a real `FScene` when `GIsClient && FApp::CanEverRender() && !GUsingNullRHI`; without
  it every world gets an `FNULLSceneInterface` and the render-state portion of a system's
  game-thread cost never happens, understating every measurement.

## 0.1.0

### Added

- `Pulse.PulseAudit` commandlet: headless, read-only performance audit of every asset in a
  project, built for CI (`-unattended -nullrhi`, exit codes 0/1/2, `-failunder` score gate).
- Three scan tiers. Fast (default) reads Asset Registry tags only — zero package loads, whole
  project in seconds. `-deep` loads packages for what tags cannot provide, with a bounded GC
  cadence. `-shaderstats` (implies `-deep`, opt-in) compiles material shaders in parallel batches
  for instruction counts.
- Seven collectors: Texture, StaticMesh, SkeletalMesh, Material, Blueprint, Niagara, Level —
  including a zero-load World Partition actor census via actor descriptors. Collectors register
  through `IModularFeatures`, so external modules can add their own.
- Deterministic reports: `Summary.csv`, `Assets_<Category>.csv`, `Issues.csv`,
  `Statistics.json`, plus pruned historical snapshots. Byte-identical across reruns on unchanged
  content; no timestamps in file content; `SettingsHash` in the header so threshold edits are one
  visible line, not row churn.
- Scoring: per-asset exponential decay over severity weights, per-category depth x breadth,
  fixed-weight project blend — breadth costs more than depth, and clean content cannot inflate
  the score.
- `UPulseSettings` in `Config/DefaultPulse.ini`: per-category thresholds, scoring weights, scan
  paths and exclusions, `DisabledRules`, and `RuleSeverityOverrides`.
- **Tools → Pulse Audit** editor panel: tier and severity filters, budgeted per-frame scanning so
  the editor stays responsive, double-click to sync the Content Browser, and report export
  byte-identical to the commandlet's.
- `Tools/Invoke-PulseAudit.ps1` (headless CI wrapper returning the commandlet's exit code) and
  `Tools/Compare-PulseReport.ps1` (report diff with new/resolved rule hits and a
  `-FailOnRegression` gate).
