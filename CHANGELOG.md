# Changelog

All notable changes to Pulse are recorded here. Versions follow the `VersionName` field in
`Pulse.uplugin`.

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
