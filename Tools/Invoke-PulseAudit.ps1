<#
.SYNOPSIS
    Runs the Pulse performance audit commandlet headless and returns its exit code, for CI.

.DESCRIPTION
    Locates UnrealEditor-Cmd.exe (from -EnginePath, or from the project's EngineAssociation),
    builds the commandlet argument list from the parameters below, and runs:

        UnrealEditor-Cmd.exe <Project>.uproject -run=Pulse.PulseAudit -unattended -nosplash -nop4 -nullrhi

    The script's exit code is the commandlet's exit code:
        0  audit completed; score at or above the -FailUnder gate (or no gate set)
        1  score below -FailUnder, or a report file could not be written
        2  bad invocation

    Reports land in <Project>/Saved/PulseAudit by default (-ReportDir overrides). Two runs on
    unchanged content produce byte-identical reports, so archiving Statistics.json per build and
    diffing with Compare-PulseReport.ps1 gives a regression gate.

.PARAMETER ProjectFile
    Path to the .uproject to audit. Required.

.PARAMETER EnginePath
    UE install root (the directory containing Engine\). Optional: when omitted, the project's
    EngineAssociation is read from the .uproject and common install roots are probed.

.PARAMETER Deep
    Deep tier: load packages for metrics registry tags cannot provide (texture memory, Blueprint
    tick, collision detail, ...). Minutes instead of seconds.

.PARAMETER ShaderStats
    Compile material shaders for instruction counts. Implies -Deep. Opt-in and slow on a cold
    DDC — never make it the CI default. Combine with -Category Material to narrow the run.

.PARAMETER Category
    Categories to audit (e.g. Texture, StaticMesh). Default: all.

.PARAMETER Path
    Package paths to scan (e.g. /Game/Weapons). Default: the ScanPaths in DefaultPulse.ini.

.PARAMETER MinSeverity
    Hide report rows below this severity. Filters rows only — scores never change.

.PARAMETER MaxAssets
    Cap assets per category. Marks the report truncated; truncated reports are not comparable.

.PARAMETER ReportDir
    Report output directory. Default: <Project>/Saved/PulseAudit.

.PARAMETER Format
    Report formats to write: csv, json. Default: both.

.PARAMETER FailUnder
    Exit 1 when the overall score lands below this value. Omit to never fail on score.

.PARAMETER GCFreq
    Packages loaded between garbage collections at Deep tier. Default 100.

.EXAMPLE
    .\Invoke-PulseAudit.ps1 -ProjectFile F:\Work\MyGame\MyGame.uproject
.EXAMPLE
    .\Invoke-PulseAudit.ps1 -ProjectFile F:\Work\MyGame\MyGame.uproject -FailUnder 85 -MinSeverity Medium
.EXAMPLE
    # Deep material audit with instruction counts, against an explicit engine install.
    .\Invoke-PulseAudit.ps1 -ProjectFile F:\Work\MyGame\MyGame.uproject -ShaderStats -Category Material -EnginePath F:\SelfProjects\Unreal\UE_5.8
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectFile,
    [string]$EnginePath,
    [switch]$Deep,
    [switch]$ShaderStats,
    [string[]]$Category,
    [string[]]$Path,
    [ValidateSet('Info', 'Low', 'Medium', 'High', 'Critical')]
    [string]$MinSeverity,
    [int]$MaxAssets,
    [string]$ReportDir,
    [ValidateSet('csv', 'json')]
    [string[]]$Format,
    [double]$FailUnder,
    [int]$GCFreq
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
    Write-Host "Pulse: ERROR: $Message" -ForegroundColor Red
    exit 1
}

function Info([string]$Message) {
    Write-Host "Pulse: $Message" -ForegroundColor Cyan
}

# ---- Locate project -------------------------------------------------------
if (-not (Test-Path $ProjectFile -PathType Leaf)) {
    Fail "Project file not found: $ProjectFile"
}
$ProjectFile = (Resolve-Path $ProjectFile).Path
$ProjectName = [IO.Path]::GetFileNameWithoutExtension($ProjectFile)
Info "Project: $ProjectName ($ProjectFile)"

# ---- Locate engine --------------------------------------------------------
# The engine "root" is the directory that *contains* the Engine\ folder. Given any candidate,
# return that root whether the caller pointed at the root itself or directly at Engine\.
function Resolve-PulseEngineRoot([string]$Candidate) {
    if (-not $Candidate) { return $null }
    if (-not (Test-Path $Candidate)) { return $null }
    $Candidate = (Resolve-Path $Candidate).Path
    if (Test-Path (Join-Path $Candidate 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe')) { return $Candidate }
    if (Test-Path (Join-Path $Candidate 'Binaries\Win64\UnrealEditor-Cmd.exe')) { return (Split-Path $Candidate -Parent) }
    return $null
}

$EngineRoot = $null
if ($EnginePath) {
    $EngineRoot = Resolve-PulseEngineRoot $EnginePath
    if (-not $EngineRoot) { Fail "No engine found at -EnginePath '$EnginePath' (looked for Engine\Binaries\Win64\UnrealEditor-Cmd.exe)." }
}

# Probe common install roots by the project's EngineAssociation (e.g. "5.8").
if (-not $EngineRoot) {
    $Association = (Get-Content $ProjectFile -Raw | ConvertFrom-Json).EngineAssociation
    if (-not $Association) {
        Fail "The .uproject has no EngineAssociation (source-build project?). Pass -EnginePath <UE install root>."
    }
    $ProbeRoots = @(
        "F:\SelfProjects\Unreal\UE_$Association",
        "C:\Program Files\Epic Games\UE_$Association"
    )
    foreach ($Probe in $ProbeRoots) {
        $EngineRoot = Resolve-PulseEngineRoot $Probe
        if ($EngineRoot) { break }
    }
    if (-not $EngineRoot) {
        Fail "No engine found for EngineAssociation '$Association'. Probed: $($ProbeRoots -join '; '). Pass -EnginePath <UE install root>."
    }
}

$EditorCmd = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
Info "Engine: $EngineRoot"

# ---- Build commandlet arguments -------------------------------------------
$CommandletArgs = @(
    $ProjectFile, '-run=Pulse.PulseAudit',
    '-unattended', '-nosplash', '-nop4', '-nullrhi'
)
if ($Deep) { $CommandletArgs += '-deep' }
if ($ShaderStats) { $CommandletArgs += '-shaderstats' }
if ($Category) { $CommandletArgs += "-category=$($Category -join ',')" }
if ($Path) { $CommandletArgs += "-path=$($Path -join ',')" }
if ($MinSeverity) { $CommandletArgs += "-minseverity=$MinSeverity" }
if ($PSBoundParameters.ContainsKey('MaxAssets')) { $CommandletArgs += "-maxassets=$MaxAssets" }
if ($ReportDir) { $CommandletArgs += "-reportdir=$ReportDir" }
if ($Format) { $CommandletArgs += "-format=$($Format -join ',')" }
if ($PSBoundParameters.ContainsKey('FailUnder')) { $CommandletArgs += "-failunder=$FailUnder" }
if ($PSBoundParameters.ContainsKey('GCFreq')) { $CommandletArgs += "-gcfreq=$GCFreq" }

# ---- Run ------------------------------------------------------------------
Info "Running: UnrealEditor-Cmd.exe $($CommandletArgs -join ' ')"
& $EditorCmd @CommandletArgs
$AuditExitCode = $LASTEXITCODE

$EffectiveReportDir = if ($ReportDir) { $ReportDir } else { Join-Path (Split-Path $ProjectFile -Parent) 'Saved\PulseAudit' }
switch ($AuditExitCode) {
    0 { Info "Audit passed. Reports: $EffectiveReportDir" }
    1 { Write-Host "Pulse: Audit FAILED: score below the -FailUnder gate, or a report could not be written. Reports (if any): $EffectiveReportDir" -ForegroundColor Red }
    2 { Write-Host 'Pulse: Bad invocation — check the switch values in the log above.' -ForegroundColor Red }
    default { Write-Host "Pulse: Commandlet exited with unexpected code $AuditExitCode (engine crash?). Check the log above." -ForegroundColor Red }
}

exit $AuditExitCode
