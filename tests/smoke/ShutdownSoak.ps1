#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Executable,

    [Parameter(Mandatory = $true)]
    [string] $SourceA,

    [Parameter(Mandatory = $true)]
    [string] $SourceB,

    [ValidateRange(1, 100)]
    [int] $Iterations = 20
)

$ErrorActionPreference = 'Stop'

foreach ($path in @($Executable, $SourceA, $SourceB)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required shutdown-soak input is missing: $path"
    }
}

for ($iteration = 1; $iteration -le $Iterations; ++$iteration) {
    $process = Start-Process `
        -FilePath $Executable `
        -ArgumentList @('--ui-shutdown-smoke', $SourceA, $SourceB) `
        -WindowStyle Hidden `
        -PassThru

    if (-not $process.WaitForExit(10000)) {
        $process.Kill($true)
        throw "VCStation shutdown soak iteration $iteration exceeded 10 seconds."
    }
    if ($process.ExitCode -ne 0) {
        throw "VCStation shutdown soak iteration $iteration exited with $($process.ExitCode)."
    }
}

Write-Host "VCStation shutdown soak passed $Iterations iterations."
