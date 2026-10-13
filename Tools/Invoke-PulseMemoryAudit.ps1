<#
.SYNOPSIS
    Runs the Pulse memory auditor commandlet headless and returns its exit code, for CI.

.DESCRIPTION
    Locates UnrealEditor-Cmd.exe (from -EnginePath, or from the project's EngineAssociation),
    builds the commandlet argument list from the parameters below, and runs:

        UnrealEditor-Cmd.exe <Project>.uproject -run=Pulse.PulseMemoryAuditor -unattended -nosplash -nop4 -nullrhi

    The auditor reads only the Asset Registry — nothing is loaded — so -nullrhi is safe and a whole
    project takes seconds.

    The script's exit code is the commandlet's exit code:
        0  audit completed (and passed -FailOverBudget if set)
        1  the baseline or a top-level asset was over budget with -FailOverBudget, or a report
           could not be written
        2  bad invocation, or no top-level assets were found

    Reports land in <Project>\Saved\Pulse\MemoryAuditor\MemoryAudit_<timestamp>\ by default. File
    contents carry no timestamp, so two runs on unchanged content diff clean.

.PARAMETER ProjectFile
    Path to the .uproject to audit. Required.

.PARAMETER EnginePath
    UE install root (the directory containing Engine\). Optional: when omitted, the project's
    EngineAssociation is read from the .uproject and common install roots are probed.

.PARAMETER Path
    Package paths top-level assets are taken from (e.g. /Game/Maps). Dependencies are followed
    wherever they lead. Default: the auditor's ScanPaths in DefaultPulse.ini.

.PARAMETER ReportDir
    Overrides the report directory.

.PARAMETER NoSoft
    Skip soft references entirely, so no "may load" rows are reported.

.PARAMETER FailOverBudget
    Exit 1 when the always-loaded baseline or any top-level asset exceeds a budget configured
    under Project Settings > Plugins > Pulse - Memory Auditor > Budget.

.EXAMPLE
    .\Invoke-PulseMemoryAudit.ps1 -ProjectFile F:\Work\MyGame\MyGame.uproject
.EXAMPLE
    .\Invoke-PulseMemoryAudit.ps1 -ProjectFile F:\Work\MyGame\MyGame.uproject -Path /Game/Maps -FailOverBudget
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectFile,
    [string]$EnginePath,
    [string[]]$Path,
    [string]$ReportDir,
    [switch]$NoSoft,
    [switch]$FailOverBudget
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
    $ProjectFile, '-run=Pulse.PulseMemoryAuditor',
    '-unattended', '-nosplash', '-nop4', '-nullrhi'
)
if ($Path) { $CommandletArgs += "-path=$($Path -join ',')" }
if ($ReportDir) { $CommandletArgs += "-reportdir=$ReportDir" }
if ($NoSoft) { $CommandletArgs += '-nosoft' }
if ($FailOverBudget) { $CommandletArgs += '-failoverbudget' }

# ---- Run ------------------------------------------------------------------
Info "Running: UnrealEditor-Cmd.exe $($CommandletArgs -join ' ')"
& $EditorCmd @CommandletArgs
$AuditExitCode = $LASTEXITCODE

$EffectiveReportDir = if ($ReportDir) { $ReportDir } else { Join-Path (Split-Path $ProjectFile -Parent) 'Saved\Pulse\MemoryAuditor' }
switch ($AuditExitCode) {
    0 { Info "Memory audit completed. Reports: $EffectiveReportDir" }
    1 { Write-Host "Pulse: Memory audit FAILED: over budget with -FailOverBudget, or a report could not be written. Reports (if any): $EffectiveReportDir" -ForegroundColor Red }
    2 { Write-Host 'Pulse: Bad invocation or nothing to analyse — check the log above.' -ForegroundColor Red }
    default { Write-Host "Pulse: Commandlet exited with unexpected code $AuditExitCode (engine crash?). Check the log above." -ForegroundColor Red }
}

exit $AuditExitCode
