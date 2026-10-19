#requires -Version 7.0
# Scope this policy to the wrapper and its children; never change the user's shell permanently.
function Enter-BuildResourcePolicy {
    $process = [Diagnostics.Process]::GetCurrentProcess()
    $state = [pscustomobject]@{
        Process = $process
        Affinity = $process.ProcessorAffinity
        Priority = $process.PriorityClass
        Jobs = 1
        CpuCount = 1
        Environment = @{}
    }
    $names = @('CMAKE_BUILD_PARALLEL_LEVEL', 'VCPKG_MAX_CONCURRENCY',
        'CMAKE_INSTALL_PARALLEL_LEVEL', 'OMP_NUM_THREADS', 'OPENBLAS_NUM_THREADS',
        'MKL_NUM_THREADS', 'DVS_GATE_RESOURCE_PROFILE')
    foreach ($name in $names) {
        $state.Environment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    }
    try {
        # Stay inside inherited affinity (including a CI runner's restrictions). Reserve
        # at least half the available CPUs. Four CPUs leave room for Qt/WARP startup
        # and its shutdown deadlines; compilation still gets at most two jobs.
        $ids = @(for ($index = 0; $index -lt 64; $index++) {
            if (($state.Affinity.ToInt64() -band ([long]1 -shl $index)) -ne 0) { $index }
        })
        $state.CpuCount = [Math]::Min(4, [Math]::Max(1, [Math]::Floor($ids.Count / 2)))
        $state.Jobs = [Math]::Min(2, $state.CpuCount)
        $mask = [long]0
        # Do not force every GUI test onto the tail CPUs of a hybrid workstation.
        # Keep the same bounded budget while using the first inherited CPU IDs.
        foreach ($id in ($ids | Select-Object -First $state.CpuCount)) {
            $mask = $mask -bor ([long]1 -shl $id)
        }
        $process.ProcessorAffinity = [IntPtr]$mask
        $process.PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal
        foreach ($name in $names | Where-Object { $_ -ne 'DVS_GATE_RESOURCE_PROFILE' }) {
            [Environment]::SetEnvironmentVariable($name, [string]$state.Jobs, 'Process')
        }
        $env:DVS_GATE_RESOURCE_PROFILE = "background-$($state.CpuCount)-cpu"
        Write-Host "build resources: CPUs=$($state.CpuCount)/$($ids.Count); jobs=$($state.Jobs); priority=BelowNormal; tests=1 unless -TestJobs is supplied"
        return $state
    } catch {
        Exit-BuildResourcePolicy $state
        throw
    }
}

function Exit-BuildResourcePolicy {
    param($State)
    if ($null -eq $State) { return }
    try {
        $State.Process.ProcessorAffinity = $State.Affinity
        $State.Process.PriorityClass = $State.Priority
        foreach ($name in $State.Environment.Keys) {
            [Environment]::SetEnvironmentVariable($name, $State.Environment[$name], 'Process')
        }
    } finally {
        $State.Process.Dispose()
    }
}
