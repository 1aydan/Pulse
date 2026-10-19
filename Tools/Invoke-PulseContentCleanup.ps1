<#
.SYNOPSIS
    Runs the Pulse content cleanup commandlet headless and returns its exit code, for CI.

.DESCRIPTION
    Lists content nothing that ships can load, leftover redirectors, and assets imported more than
    once. Read-only: nothing is loaded or deleted, so -nullrhi is safe.

        UnrealEditor-Cmd.exe <Project>.uproject -run=Pulse.PulseContentCleanup -unattended -nosplash -nop4 -nullrhi

    Reports land in <Project>\Saved\Pulse\ContentCleanup\ContentCleanup_<timestamp>\ by default.

    The script's exit code is the commandlet's exit code:
        0  ran
        1  the report could not be written
        2  bad invocation, or no roots were found

.PARAMETER ProjectFile
    Path to the .uproject to audit. Required.

.PARAMETER EnginePath
    UE install root (the directory containing Engine\). Optional: when omitted, the project's
    EngineAssociation is read from the .uproject and common install roots are probed.

.PARAMETER Path
    Package paths whose assets are reported (e.g. /Game/Art). Reachability is always project-wide.

.PARAMETER ReportDir
    Overrides the report directory.

.PARAMETER AllMaps
    Treat every map as shipping even when the project lists Maps to Cook.

.PARAMETER NoSearch
    Skip searching .ini and C++ files for asset paths.

.EXAMPLE
    .\Invoke-PulseContentCleanup.ps1 -ProjectFile F:\Work\MyGame\MyGame.uproject
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectFile,
    [string]$EnginePath,
    [string[]]$Path,
    [string]$ReportDir,
    [switch]$AllMaps,
    [switch]$NoSearch
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
    $ProjectFile, '-run=Pulse.PulseContentCleanup',
    '-unattended', '-nosplash', '-nop4', '-nullrhi'
)
if ($Path) { $CommandletArgs += "-path=$($Path -join ',')" }
if ($ReportDir) { $CommandletArgs += "-reportdir=$ReportDir" }
if ($AllMaps) { $CommandletArgs += '-allmaps' }
if ($NoSearch) { $CommandletArgs += '-nosearch' }

# ---- Run ------------------------------------------------------------------
Info "Running: UnrealEditor-Cmd.exe $($CommandletArgs -join ' ')"
& $EditorCmd @CommandletArgs
$AuditExitCode = $LASTEXITCODE

$EffectiveReportDir = if ($ReportDir) { $ReportDir } else { Join-Path (Split-Path $ProjectFile -Parent) 'Saved\Pulse\ContentCleanup' }
switch ($AuditExitCode) {
    0 { Info "Content cleanup completed. Reports: $EffectiveReportDir" }
    1 { Write-Host "Pulse: the report could not be written. Check the log above." -ForegroundColor Red }
    2 { Write-Host 'Pulse: Bad invocation or nothing to analyse. Check the log above.' -ForegroundColor Red }
    default { Write-Host "Pulse: Commandlet exited with unexpected code $AuditExitCode (engine crash?). Check the log above." -ForegroundColor Red }
}

exit $AuditExitCode
