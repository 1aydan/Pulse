<#
.SYNOPSIS
    Diffs two Pulse Statistics.json reports: score deltas, new offenders, resolved issues. The
    regression gate for CI.

.DESCRIPTION
    Compares a baseline report (e.g. from main, or the last release) against a current one and
    prints:
      - the overall score delta and per-category score deltas,
      - rule hits present in Current but not Baseline (new offenders, per asset + rule id),
      - rule hits present in Baseline but not Current (resolved).

    The script REFUSES to compare (exit 2) when either report is truncated (-maxassets clipped
    the asset list, so the two runs did not measure the same content) or when the two headers'
    settings hashes differ (thresholds changed between runs, so score deltas would mix real
    regressions with rule changes — re-baseline after a settings edit instead).

    Exit codes:
        0  comparable; no regression (or -FailOnRegression not requested)
        1  -FailOnRegression was given and the overall score dropped or new Critical/High
           issues appeared
        2  reports are not comparable, or could not be read

.PARAMETER Baseline
    Path to the baseline Statistics.json.

.PARAMETER Current
    Path to the current Statistics.json.

.PARAMETER FailOnRegression
    Exit 1 when the overall score dropped or new Critical/High issues appeared.

.EXAMPLE
    .\Compare-PulseReport.ps1 -Baseline main\Statistics.json -Current pr\Statistics.json
.EXAMPLE
    .\Compare-PulseReport.ps1 -Baseline main\Statistics.json -Current pr\Statistics.json -FailOnRegression
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Baseline,
    [Parameter(Mandatory = $true)]
    [string]$Current,
    [switch]$FailOnRegression
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Message, [int]$ExitCode = 1) {
    Write-Host "Pulse: ERROR: $Message" -ForegroundColor Red
    exit $ExitCode
}

function Info([string]$Message) {
    Write-Host "Pulse: $Message" -ForegroundColor Cyan
}

# ---- Load reports ---------------------------------------------------------
foreach ($ReportPath in @($Baseline, $Current)) {
    if (-not (Test-Path $ReportPath -PathType Leaf)) { Fail "Report not found: $ReportPath" 2 }
}
try {
    $BaselineReport = Get-Content $Baseline -Raw | ConvertFrom-Json
    $CurrentReport = Get-Content $Current -Raw | ConvertFrom-Json
}
catch {
    Fail "Failed to parse report JSON: $($_.Exception.Message)" 2
}
foreach ($Pair in @(@($Baseline, $BaselineReport), @($Current, $CurrentReport))) {
    if (-not $Pair[1].header -or $null -eq $Pair[1].overallScore) {
        Fail "$($Pair[0]) does not look like a Pulse Statistics.json (no header/overallScore)." 2
    }
}
Info "Baseline: $Baseline"
Info "Current:  $Current"

# ---- Comparability gates --------------------------------------------------
# A truncated run measured a different asset set; differing settings hashes mean the rules
# themselves changed. Either way a delta would be noise, not signal — refuse rather than mislead.
if ($BaselineReport.header.truncated) {
    Fail 'Baseline report is truncated (-maxassets clipped the run). Truncated reports are not comparable — re-run without -maxassets.' 2
}
if ($CurrentReport.header.truncated) {
    Fail 'Current report is truncated (-maxassets clipped the run). Truncated reports are not comparable — re-run without -maxassets.' 2
}
if ($BaselineReport.header.settingsHash -ne $CurrentReport.header.settingsHash) {
    Fail "Settings hash differs (baseline $($BaselineReport.header.settingsHash), current $($CurrentReport.header.settingsHash)). Thresholds or rule overrides changed between the two runs, so score deltas would mix real regressions with rule changes. Re-baseline after settings edits." 2
}
if ($BaselineReport.header.tier -ne $CurrentReport.header.tier) {
    Write-Warning "Run tiers differ (baseline $($BaselineReport.header.tier), current $($CurrentReport.header.tier)) — deeper tiers evaluate rules shallower ones cannot, so issue deltas may reflect the tier, not the content."
}

# ---- Score deltas ---------------------------------------------------------
function Format-PulseDelta([double]$Delta) {
    return '{0:+0.00;-0.00;+0.00}' -f $Delta
}

$OverallDelta = [double]$CurrentReport.overallScore - [double]$BaselineReport.overallScore
$OverallColor = if ($OverallDelta -lt 0) { 'Red' } elseif ($OverallDelta -gt 0) { 'Green' } else { 'Gray' }
Write-Host ("Pulse: Overall score: {0:0.00} -> {1:0.00} ({2})" -f [double]$BaselineReport.overallScore, [double]$CurrentReport.overallScore, (Format-PulseDelta $OverallDelta)) -ForegroundColor $OverallColor

$BaselineCategories = @{}
foreach ($CategoryResult in $BaselineReport.categories) { $BaselineCategories[[string]$CategoryResult.name] = [double]$CategoryResult.score }
$CurrentCategories = @{}
foreach ($CategoryResult in $CurrentReport.categories) { $CurrentCategories[[string]$CategoryResult.name] = [double]$CategoryResult.score }

$AllCategoryNames = @($BaselineCategories.Keys) + @($CurrentCategories.Keys) | Sort-Object -Unique
foreach ($Name in $AllCategoryNames) {
    if (-not $CurrentCategories.ContainsKey($Name)) {
        Write-Host "Pulse:   ${Name}: only in baseline (category empty or filtered out in current)" -ForegroundColor Yellow
        continue
    }
    if (-not $BaselineCategories.ContainsKey($Name)) {
        Write-Host "Pulse:   ${Name}: only in current (new content, or filtered out in baseline)" -ForegroundColor Yellow
        continue
    }
    $Delta = $CurrentCategories[$Name] - $BaselineCategories[$Name]
    $Color = if ($Delta -lt 0) { 'Red' } elseif ($Delta -gt 0) { 'Green' } else { 'Gray' }
    Write-Host ("Pulse:   {0}: {1:0.00} -> {2:0.00} ({3})" -f $Name, $BaselineCategories[$Name], $CurrentCategories[$Name], (Format-PulseDelta $Delta)) -ForegroundColor $Color
}

# ---- New / resolved rule hits ---------------------------------------------
# Key: "<asset path>|<rule id>" — one entry per rule firing per asset, severity kept for the gate.
function Get-PulseIssueMap($Report) {
    $Map = @{}
    foreach ($CategoryResult in $Report.categories) {
        foreach ($Asset in $CategoryResult.assets) {
            foreach ($Issue in $Asset.issues) {
                $Map["$($Asset.path)|$($Issue.ruleId)"] = [string]$Issue.severity
            }
        }
    }
    return $Map
}

$BaselineIssues = Get-PulseIssueMap $BaselineReport
$CurrentIssues = Get-PulseIssueMap $CurrentReport

$SeverityRank = @{ 'Critical' = 4; 'High' = 3; 'Medium' = 2; 'Low' = 1; 'Info' = 0 }
$NewKeys = @($CurrentIssues.Keys | Where-Object { -not $BaselineIssues.ContainsKey($_) }) |
    Sort-Object @{ Expression = { $SeverityRank[$CurrentIssues[$_]] }; Descending = $true }, { $_ }
$ResolvedKeys = @($BaselineIssues.Keys | Where-Object { -not $CurrentIssues.ContainsKey($_) }) | Sort-Object

if ($NewKeys.Count -gt 0) {
    Write-Host "Pulse: New offenders ($($NewKeys.Count)):" -ForegroundColor Red
    foreach ($Key in $NewKeys) {
        $AssetPath, $RuleId = $Key -split '\|', 2
        Write-Host "Pulse:   [$($CurrentIssues[$Key])] $RuleId  $AssetPath" -ForegroundColor Red
    }
}
else {
    Info 'New offenders: none.'
}

if ($ResolvedKeys.Count -gt 0) {
    Write-Host "Pulse: Resolved ($($ResolvedKeys.Count)):" -ForegroundColor Green
    foreach ($Key in $ResolvedKeys) {
        $AssetPath, $RuleId = $Key -split '\|', 2
        Write-Host "Pulse:   [$($BaselineIssues[$Key])] $RuleId  $AssetPath" -ForegroundColor Green
    }
}
else {
    Info 'Resolved: none.'
}

# ---- Regression gate ------------------------------------------------------
$NewSevereKeys = @($NewKeys | Where-Object { $SeverityRank[$CurrentIssues[$_]] -ge $SeverityRank['High'] })
$bScoreDropped = $OverallDelta -lt -0.0001

if ($FailOnRegression -and ($bScoreDropped -or $NewSevereKeys.Count -gt 0)) {
    $Reasons = @()
    if ($bScoreDropped) { $Reasons += "overall score dropped $(Format-PulseDelta $OverallDelta)" }
    if ($NewSevereKeys.Count -gt 0) { $Reasons += "$($NewSevereKeys.Count) new Critical/High issue(s)" }
    Fail "Regression: $($Reasons -join '; ')." 1
}

Info 'Comparison complete.'
exit 0
