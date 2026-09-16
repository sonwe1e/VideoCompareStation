param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [Parameter(Mandatory = $true)]
    [string[]]$Fixtures,

    [ValidateSet('side', 'wipe', 'diff')]
    [string]$ComparisonMode = 'side',

    [string]$LogRoot,

    [string]$RunName
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Import-Module (Join-Path $PSScriptRoot 'PlaybackTraceGate.psm1') -Force

$result = Invoke-PlaybackTraceGate `
    -Gate comparison-semantics `
    -Executable $Executable `
    -Fixtures $Fixtures `
    -DurationSeconds 30 `
    -ComparisonMode $ComparisonMode `
    -LogRoot $LogRoot `
    -RunName $RunName
Write-Output $result.Message
exit $result.ExitCode
