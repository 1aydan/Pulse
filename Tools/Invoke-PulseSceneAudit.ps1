<#
.SYNOPSIS
    Runs the Pulse scene audit commandlet headless and returns its exit code, for CI.

.DESCRIPTION
    Loads each map (Maps to Cook by default, or every map under the scan paths) with its streaming
    sublevels and reports repeated non-Nanite meshes, stacked duplicates, stationary light overlap,
    shadow-casting movable lights and props with no draw distance. Nothing is modified or saved.

        UnrealEditor-Cmd.exe <Project>.uproject -run=Pulse.PulseSceneAudit -unattended -nosplash -nop4 -nullrhi

    Reports land in <Project>\Saved\Pulse\SceneAuditor\SceneAudit_<timestamp>\ by default.

    The script's exit code is the commandlet's exit code:
        0  ran (and nothing reached -FailOn)
        1  a finding at or above -FailOn, or the report could not be written
        2  bad invocation, or no maps to audit

.PARAMETER ProjectFile
    Path to the .uproject to audit. Required.

.PARAMETER EnginePath
    UE install root (the directory containing Engine\). Optional: when omitted, the project's
    EngineAssociation is read from the .uproject and common install roots are probed.

.PARAMETER Maps
    Map package names to audit (e.g. /Game/Maps/L_Hub). Default: Maps to Cook, or every map.

.PARAMETER Path
    Package paths to find maps under when Maps to Cook is empty or ignored.

.PARAMETER AllMaps
    Ignore Maps to Cook and audit every map under the scan paths.

.PARAMETER ReportDir
    Overrides the report directory.

.PARAMETER FailOn
    Exit 1 when any finding is at or above this severity.

.EXAMPLE
    .\Invoke-PulseSceneAudit.ps1 -ProjectFile F:\Work\MyGame\MyGame.uproject -FailOn High
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectFile,
    [string]$EnginePath,
    [string[]]$Maps,
    [string[]]$Path,
    [switch]$AllMaps,
    [string]$ReportDir,
    [ValidateSet('Info', 'Low', 'Medium', 'High', 'Critical')]
    [string]$FailOn
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
    Write-Host "Pulse: ERROR: $Message" -ForegroundColor Red
    exit 2
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
        "F:\SelfProjects\Unreal\Engine\UE_$Association",
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
    $ProjectFile, '-run=Pulse.PulseSceneAudit',
    '-unattended', '-nosplash', '-nop4', '-nullrhi'
)
if ($Maps) { $CommandletArgs += "-maps=$($Maps -join ',')" }
if ($Path) { $CommandletArgs += "-path=$($Path -join ',')" }
if ($AllMaps) { $CommandletArgs += '-allmaps' }
if ($ReportDir) { $CommandletArgs += "-reportdir=$ReportDir" }
if ($FailOn) { $CommandletArgs += "-failon=$FailOn" }

# ---- Run ------------------------------------------------------------------
Info "Running: UnrealEditor-Cmd.exe $($CommandletArgs -join ' ')"
& $EditorCmd @CommandletArgs
$AuditExitCode = $LASTEXITCODE

$EffectiveReportDir = if ($ReportDir) { $ReportDir } else { Join-Path (Split-Path $ProjectFile -Parent) 'Saved\Pulse\SceneAuditor' }
switch ($AuditExitCode) {
    0 { Info "Scene audit completed. Reports: $EffectiveReportDir" }
    1 { Write-Host "Pulse: Scene audit FAILED: a finding at or above -FailOn, or the report could not be written. Reports (if any): $EffectiveReportDir" -ForegroundColor Red }
    2 { Write-Host 'Pulse: Bad invocation or no maps. Check the log above.' -ForegroundColor Red }
    default { Write-Host "Pulse: Commandlet exited with unexpected code $AuditExitCode (engine crash?). Check the log above." -ForegroundColor Red }
}

exit $AuditExitCode
