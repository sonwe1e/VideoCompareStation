#requires -Version 7.0
<#
.SYNOPSIS
    Compares two T5 evidence bundles collected on the same machine, media and duration, and reports
    the display-interval tail, the correctness gates and the resource counters side by side.

.DESCRIPTION
    T5 acceptance requires the improvement to exceed run-to-run variance on the same build, media,
    display and duration. This script reads runs.json from each bundle, prints every run, and
    computes the per-bundle mean and spread (min/max) of the reported distributions so a
    before/after difference can be compared against the noise floor.

    Deliberately avoids parameterised helper functions for the per-run data: Windows PowerShell 7
    returns the filtered array as a scalar in some binding combinations, which turned a sample
    count into a "property Count not found" failure. Everything is inlined instead.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Before,
    [Parameter(Mandatory = $true)][string]$After,
    [string]$RunPattern = 'video-'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$beforeFile = Join-Path $Before 'runs.json'
$afterFile = Join-Path $After 'runs.json'
foreach ($file in @($beforeFile, $afterFile)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "runs.json not found at $file"
    }
}

$beforeAll = [System.Collections.Generic.List[object]]::new()
foreach ($entry in @(Get-Content -LiteralPath $beforeFile -Raw | ConvertFrom-Json)) {
    $beforeAll.Add($entry)
}
$afterAll = [System.Collections.Generic.List[object]]::new()
foreach ($entry in @(Get-Content -LiteralPath $afterFile -Raw | ConvertFrom-Json)) {
    $afterAll.Add($entry)
}

$beforeRuns = [System.Collections.Generic.List[object]]::new()
foreach ($entry in $beforeAll) {
    if ($entry.run -like "$RunPattern*") { $beforeRuns.Add($entry) }
}
$afterRuns = [System.Collections.Generic.List[object]]::new()
foreach ($entry in $afterAll) {
    if ($entry.run -like "$RunPattern*") { $afterRuns.Add($entry) }
}
if ($beforeRuns.Count -eq 0 -or $afterRuns.Count -eq 0) {
    throw "No runs matched '$RunPattern*' in one of the bundles."
}

$fields = @(
    'display_interval_p50_ms', 'display_interval_p95_ms', 'display_interval_p99_ms',
    'display_interval_max_ms', 'ui_loop_gap_p95_ms', 'ui_loop_gap_max_ms',
    'seek_p95_ms', 'warm_step_p95_ms', 'drop_ratio', 'commit_rate_per_second',
    'decoder_average_us', 'frameset_assembly_avg_us', 'peak_frame_bytes',
    'trace_ready_to_commit_p50_us', 'trace_publish_to_ack_p50_us'
)

function Get-FieldStats {
    param([System.Collections.Generic.List[object]]$Runs, [string]$Field)

    $numbers = [System.Collections.Generic.List[double]]::new()
    foreach ($run in $Runs) {
        $property = $run.PSObject.Properties[$Field]
        if ($null -eq $property -or $null -eq $property.Value) { continue }
        $numbers.Add([double]$property.Value)
    }
    if ($numbers.Count -eq 0) {
        return [pscustomobject]@{ Samples = 0; Mean = $null; Min = $null; Max = $null }
    }
    $sorted = @($numbers | Sort-Object)
    return [pscustomobject]@{
        Samples = $numbers.Count
        Mean    = [math]::Round(($numbers | Measure-Object -Average).Average, 2)
        Min     = [math]::Round($sorted[0], 2)
        Max     = [math]::Round($sorted[$sorted.Count - 1], 2)
    }
}

Write-Output "# T5 comparison: $RunPattern*"
Write-Output ""
Write-Output "before: $Before  ($($beforeRuns.Count) runs)"
Write-Output "after : $After  ($($afterRuns.Count) runs)"
Write-Output ""
Write-Output '| metric | before mean (min-max) | after mean (min-max) | delta |'
Write-Output '|---|---|---|---|'
foreach ($field in $fields) {
    $b = Get-FieldStats -Runs $beforeRuns -Field $field
    $a = Get-FieldStats -Runs $afterRuns -Field $field
    if ($b.Samples -eq 0 -and $a.Samples -eq 0) { continue }
    $delta = if ($null -ne $b.Mean -and $null -ne $a.Mean) {
        [math]::Round([double]$a.Mean - [double]$b.Mean, 2)
    } else { 'n/a' }
    Write-Output ("| {0} | {1} ({2}-{3}) | {4} ({5}-{6}) | {7} |" -f `
            $field, $b.Mean, $b.Min, $b.Max, $a.Mean, $a.Min, $a.Max, $delta)
}

Write-Output ""
Write-Output '## Per-run rows'
Write-Output ''
Write-Output '| bundle | run | passed | disp_p50 | disp_p95 | disp_p99 | disp_max | commits | commit/s | drop | ui_p95 | ui_max |'
Write-Output '|---|---|---|---|---|---|---|---|---|---|---|---|'
foreach ($bundle in @(
        [pscustomobject]@{ Name = 'before'; Runs = $beforeRuns },
        [pscustomobject]@{ Name = 'after'; Runs = $afterRuns })) {
    foreach ($run in $bundle.Runs) {
        Write-Output ("| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} | {11} |" -f `
                $bundle.Name, $run.run, $run.passed, $run.display_interval_p50_ms,
                $run.display_interval_p95_ms, $run.display_interval_p99_ms,
                $run.display_interval_max_ms, $run.trace_commit_count,
                $run.commit_rate_per_second, $run.drop_ratio, $run.ui_loop_gap_p95_ms,
                $run.ui_loop_gap_max_ms)
    }
}

Write-Output ""
Write-Output '## Correctness gates (must stay satisfied in every run)'
Write-Output ''
foreach ($bundle in @(
        [pscustomobject]@{ Name = 'before'; Runs = $beforeRuns },
        [pscustomobject]@{ Name = 'after'; Runs = $afterRuns })) {
    foreach ($run in $bundle.Runs) {
        Write-Output ("{0,-7} {1,-22} passed={2} drop={3} seek_p95={4} warm_step_p95={5} failure={6}" -f `
                $bundle.Name, $run.run, $run.passed, $run.drop_ratio, $run.seek_p95_ms,
                $run.warm_step_p95_ms, $run.failure)
    }
}
