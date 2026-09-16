[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$FilePath,

    [Parameter(Position = 1, ValueFromRemainingArguments = $true)]
    [string[]]$ArgumentList = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$resourceProfile = if ([string]::IsNullOrWhiteSpace($env:DVS_EXECUTION_RESOURCE_PROFILE)) {
    'Background'
} else {
    $env:DVS_EXECUTION_RESOURCE_PROFILE
}
if ($resourceProfile -notin @('Background', 'Interactive')) {
    throw "Unsupported DVS_EXECUTION_RESOURCE_PROFILE '$resourceProfile'."
}

$maxConcurrency = 4
$logicalProcessorCount = [Math]::Min([Environment]::ProcessorCount, 63)
$processorIds = [System.Collections.Generic.List[int]]::new()
$currentProcess = Get-Process -Id $PID

switch ($resourceProfile) {
    'Background' {
        $processorCount = [Math]::Min($maxConcurrency, $logicalProcessorCount)
        $firstProcessor = $logicalProcessorCount - $processorCount
        $affinityMask = [Int64]0
        for ($processorId = $firstProcessor; $processorId -lt $logicalProcessorCount; ++$processorId) {
            $affinityMask = $affinityMask -bor ([Int64]1 -shl $processorId)
            $processorIds.Add($processorId)
        }
        $currentProcess.ProcessorAffinity = [IntPtr]$affinityMask
        $currentProcess.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::BelowNormal
    }
    'Interactive' {
        $currentProcess.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::Normal
        $affinityMask = [Int64]$currentProcess.ProcessorAffinity.ToInt64()
        for ($processorId = 0; $processorId -lt $logicalProcessorCount; ++$processorId) {
            if (($affinityMask -band ([Int64]1 -shl $processorId)) -ne 0) {
                $processorIds.Add($processorId)
            }
        }
        $processorCount = $processorIds.Count
        if ($processorCount -ne $logicalProcessorCount) {
            throw (
                "Interactive resource profile requires all $logicalProcessorCount processors; " +
                "inherited affinity contains $processorCount."
            )
        }
    }
}

$env:CMAKE_BUILD_PARALLEL_LEVEL = $maxConcurrency.ToString(
    [Globalization.CultureInfo]::InvariantCulture
)
$env:CTEST_PARALLEL_LEVEL = '1'
$env:VCPKG_MAX_CONCURRENCY = $maxConcurrency.ToString(
    [Globalization.CultureInfo]::InvariantCulture
)
$env:DVS_GATE_RESOURCE_PROFILE = "$(($resourceProfile).ToLowerInvariant())-$processorCount-cpu"

$profileMessage = (
    'DVS_RESOURCE_PROFILE profile={0} concurrency={1} ctest=1 ' +
    'priority={2} processors={3}'
) -f @(
    $env:DVS_GATE_RESOURCE_PROFILE,
    $maxConcurrency,
    $currentProcess.PriorityClass,
    ($processorIds -join ',')
)
Write-Host $profileMessage

& $FilePath @ArgumentList
$commandExitCode = if ($null -eq $LASTEXITCODE) { 0 } else { $LASTEXITCODE }
exit $commandExitCode
