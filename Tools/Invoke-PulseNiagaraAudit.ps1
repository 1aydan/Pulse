<#
.SYNOPSIS
    Runs the Pulse Niagara auditor commandlet headless and returns its exit code, for CI.

.DESCRIPTION
    Locates UnrealEditor-Cmd.exe (from -EnginePath, or from the project's EngineAssociation),
    builds the commandlet argument list from the parameters below, and runs:

        UnrealEditor-Cmd.exe <Project>.uproject -run=Pulse.PulseNiagaraAuditor `
            -unattended -nosplash -nop4 -AllowCommandletRendering

    Unlike Invoke-PulseAudit.ps1 this NEVER passes -nullrhi, and passes -AllowCommandletRendering
    by default. FRendererModule::AllocateScene only builds a real FScene when
    GIsClient && FApp::CanEverRender() && !GUsingNullRHI; without it every world gets a null scene,
    no render proxies are created, and the render-state portion of a Niagara system's game-thread
    cost never happens — understating every measurement. -NoRendering opts out if you need it.

    The script's exit code is the commandlet's exit code:
        0  audit completed (and passed -FailOverBudget if set)
        1  a system was over budget with -FailOverBudget, or a report could not be written
        2  bad invocation, or the measurement stage could not be created

    Reports land in <Project>\Saved\Pulse\NiagaraAuditor\NiagaraAudit_<timestamp>\ by default.

    NOTE distance culling cannot run in a commandlet: Niagara caches the views it culls against
    from a local PlayerController with a real viewport or from the level editor viewport, and a
    commandlet has neither. Costs from this script are UNCULLED, and Run.json records that as
    scalabilityCullingExercised: false. To measure culling by distance, use the editor panel
    (Tools > Pulse Niagara Auditor) or the Pulse.Niagara.Audit console command.

.PARAMETER ProjectFile
    Path to the .uproject to audit. Required.

.PARAMETER EnginePath
    UE install root (the directory containing Engine\). Optional: when omitted, the project's
    EngineAssociation is read from the .uproject and common install roots are probed.

.PARAMETER Path
    Package paths to scan (e.g. /Game/VFX). Default: the ScanPaths in DefaultPulse.ini.

.PARAMETER Quality
    Niagara quality levels to sweep, as fx.Niagara.QualityLevel values (0=Low .. 4=Cinematic).
    Default: the QualityLevelsToMeasure in DefaultPulse.ini. Each added level multiplies run time.

.PARAMETER Instances
    Instances spawned per system. Niagara batches instances of one system into a single
    simulation, so per-instance cost falls as this rises.

.PARAMETER Settle
    Frames ticked and discarded before measurement, absorbing spawn and first-tick costs.

.PARAMETER Frames
    Frames recorded per system per quality level.

.PARAMETER MaxSystems
    Cap on systems measured. Useful for a smoke run on a large library.

.PARAMETER ReportDir
    Overrides the report directory.

.PARAMETER IncludeGpu
    Measure systems with a GPU emitter instead of skipping them. Their simulation never dispatches
    without a scene render, so the resulting cost UNDERSTATES them — opt in knowingly.

.PARAMETER FailOverBudget
    Exit 1 when any system exceeds its per-instance share of the frame budget.

.PARAMETER NoRendering
    Omit -AllowCommandletRendering. Faster to start, but understates cost — see DESCRIPTION.

.EXAMPLE
    .\Invoke-PulseNiagaraAudit.ps1 -ProjectFile F:\Work\MyGame\MyGame.uproject
.EXAMPLE
    .\Invoke-PulseNiagaraAudit.ps1 -ProjectFile F:\Work\MyGame\MyGame.uproject -Path /Game/VFX -Quality 0,3 -FailOverBudget
.EXAMPLE
    # Quick smoke run over twenty systems at Epic only.
    .\Invoke-PulseNiagaraAudit.ps1 -ProjectFile F:\Work\MyGame\MyGame.uproject -Quality 3 -MaxSystems 20 -Frames 60
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectFile,
    [string]$EnginePath,
    [string[]]$Path,
    [int[]]$Quality,
    [int]$Instances,
    [int]$Settle,
    [int]$Frames,
    [int]$MaxSystems,
    [string]$ReportDir,
    [switch]$IncludeGpu,
    [switch]$FailOverBudget,
    [switch]$NoRendering
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
    $ProjectFile, '-run=Pulse.PulseNiagaraAuditor',
    '-unattended', '-nosplash', '-nop4'
)
if (-not $NoRendering) {
    $CommandletArgs += '-AllowCommandletRendering'
} else {
    Write-Host 'Pulse: WARNING: -NoRendering given. Costs will understate every system; see the script help.' -ForegroundColor Yellow
}
if ($Path) { $CommandletArgs += "-path=$($Path -join ',')" }
if ($Quality) { $CommandletArgs += "-quality=$($Quality -join ',')" }
if ($PSBoundParameters.ContainsKey('Instances')) { $CommandletArgs += "-instances=$Instances" }
if ($PSBoundParameters.ContainsKey('Settle')) { $CommandletArgs += "-settle=$Settle" }
if ($PSBoundParameters.ContainsKey('Frames')) { $CommandletArgs += "-frames=$Frames" }
if ($PSBoundParameters.ContainsKey('MaxSystems')) { $CommandletArgs += "-maxsystems=$MaxSystems" }
if ($ReportDir) { $CommandletArgs += "-reportdir=$ReportDir" }
if ($IncludeGpu) { $CommandletArgs += '-includegpu' }
if ($FailOverBudget) { $CommandletArgs += '-failoverbudget' }

# ---- Run ------------------------------------------------------------------
Info "Running: UnrealEditor-Cmd.exe $($CommandletArgs -join ' ')"
Info 'Distance culling is paused for commandlet runs; costs are unculled. See the script help.'
& $EditorCmd @CommandletArgs
$AuditExitCode = $LASTEXITCODE

$EffectiveReportDir = if ($ReportDir) { $ReportDir } else { Join-Path (Split-Path $ProjectFile -Parent) 'Saved\Pulse\NiagaraAuditor' }
switch ($AuditExitCode) {
    0 { Info "Audit complete. Reports: $EffectiveReportDir" }
    1 { Write-Host "Pulse: Audit FAILED: a system was over budget, or a report could not be written. Reports (if any): $EffectiveReportDir" -ForegroundColor Red }
    2 { Write-Host 'Pulse: Bad invocation, or the measurement stage could not be created — check the log above.' -ForegroundColor Red }
    default { Write-Host "Pulse: Commandlet exited with unexpected code $AuditExitCode (engine crash?). Check the log above." -ForegroundColor Red }
}

exit $AuditExitCode
