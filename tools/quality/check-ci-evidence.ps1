#requires -Version 7.0
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$verifier = Join-Path $PSScriptRoot '../ci/test-hardware-evidence.ps1'
$sha = '1234567890abcdef1234567890abcdef12345678'
$baseline = @{
    path = '.github/workflows/hardware-performance.yml'
    event = 'workflow_dispatch'
    status = 'completed'
    conclusion = 'success'
    head_sha = $sha
}
& $verifier -Run ([pscustomobject]$baseline) -ExpectedSha $sha
$cases = 1
foreach ($field in @('path', 'event', 'status', 'conclusion', 'head_sha')) {
    $counterexample = $baseline.Clone()
    $counterexample[$field] = 'incorrect'
    $rejected = $false
    try { & $verifier -Run ([pscustomobject]$counterexample) -ExpectedSha $sha }
    catch { $rejected = $true }
    if (-not $rejected) { throw "Accepted incorrect hardware $field." }
    $cases++
}
if ($cases -ne 6) { throw "Expected six evidence cases, ran $cases." }
Write-Host "CI hardware evidence checks passed: $cases cases."
