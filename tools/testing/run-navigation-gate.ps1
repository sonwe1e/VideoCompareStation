param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [Parameter(Mandatory = $true)]
    [string[]]$Fixtures,

    [ValidateRange(1, 3600)]
    [int]$DurationSeconds = 60,

    [string]$LogRoot,

    [string]$RunName
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Import-Module (Join-Path $PSScriptRoot 'PlaybackTraceGate.psm1') -Force

$result = Invoke-PlaybackTraceGate `
    -Gate navigation `
    -Executable $Executable `
    -Fixtures $Fixtures `
    -DurationSeconds $DurationSeconds `
    -LogRoot $LogRoot `
    -RunName $RunName
Write-Output $result.Message
exit $result.ExitCode
