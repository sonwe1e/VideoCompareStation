#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [object]$Run,
    [Parameter(Mandatory)] [string]$ExpectedSha
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($ExpectedSha -cnotmatch '^[0-9a-f]{40}$') { throw 'Expected a full release commit SHA.' }
$requirements = @{
    path = '.github/workflows/hardware-performance.yml'
    event = 'workflow_dispatch'
    status = 'completed'
    conclusion = 'success'
    head_sha = $ExpectedSha
}
$checks = 0
foreach ($entry in $requirements.GetEnumerator()) {
    if ($Run.($entry.Key) -cne $entry.Value) {
        throw "Hardware evidence mismatch: $($entry.Key)."
    }
    $checks++
}
if ($checks -ne 5) { throw "Expected five hardware identity checks, ran $checks." }
Write-Host "Verified successful manual hardware run for $ExpectedSha."
