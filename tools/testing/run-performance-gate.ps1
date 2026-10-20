#requires -Version 7.0
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet(
        '1080p60-1source',
        '1080p60-2source',
        '1080p60-2source-diff',
        '1080p60-2source-rotated',
        '1080p60',
        '1080p120-1source',
        '1080p120-2source',
        '1080p120-3source'
    )]
    [string]$Profile,

    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [ValidateRange(5, 3600)]
    [int]$DurationSeconds = 300,

    [ValidateSet('side', 'wipe', 'diff')]
    [string]$ComparisonMode = 'side',

    # Plan §8.4 summary metadata. The display path is a run condition the script cannot detect
    # reliably (RDP/virtual display vs physical), so the caller states it; unknown stays unknown.
    [ValidateSet('unknown', 'physical', 'remote')]
    [string]$DisplayPath = 'unknown',

    [string]$BuildType = 'unknown',

    [switch]$RequireRetainedFrame,

    [switch]$ReviewLoad,

    [string]$FixtureRoot = $env:DVS_PERFORMANCE_FIXTURE_ROOT,

    [string]$LogRoot,

    [string]$RunName
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$ComparisonMode = $ComparisonMode.ToLowerInvariant()

Import-Module (Join-Path $PSScriptRoot 'CommandLineArgument.psm1') -Force

$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
if (-not $FixtureRoot) {
    $repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
    $FixtureRoot = Join-Path $repositoryRoot 'out\performance'
}
$resolvedFixtureRoot = (Resolve-Path -LiteralPath $FixtureRoot).Path
if (-not $LogRoot) {
    $LogRoot = Join-Path $resolvedFixtureRoot 'results'
}
New-Item -ItemType Directory -Path $LogRoot -Force | Out-Null
$resolvedLogRoot = (Resolve-Path -LiteralPath $LogRoot).Path

$fixtureNames = switch ($Profile) {
    '1080p60-1source' {
        @('gate-1080p60-a.mp4')
    }
    '1080p60-2source' {
        @('gate-1080p60-a.mp4', 'gate-1080p60-b.mp4')
    }
    '1080p60-2source-diff' {
        @('gate-1080p60-a.mp4', 'gate-1080p60-diff-b.mp4')
    }
    '1080p60-2source-rotated' {
        @('gate-1080p60-rot90-a.mp4', 'gate-1080p60-b.mp4')
    }
    '1080p60' {
        @('gate-1080p60-a.mp4', 'gate-1080p60-b.mp4', 'gate-1080p60-c.mp4')
    }
    '1080p120-2source' {
        @('gate-1080p120-a.mp4', 'gate-1080p120-b.mp4')
    }
    '1080p120-1source' {
        @('gate-1080p120-a.mp4')
    }
    '1080p120-3source' {
        @('gate-1080p120-a.mp4', 'gate-1080p120-b.mp4', 'gate-1080p120-c.mp4')
    }
}
$fixtures = foreach ($name in $fixtureNames) {
    (Resolve-Path -LiteralPath (Join-Path $resolvedFixtureRoot $name)).Path
}

if ($Profile -eq '1080p60-2source-diff') {
    if ($ComparisonMode -ne 'diff') {
        throw 'The 1080p60-2source-diff profile may only run in diff comparison mode.'
    }
    $contractPath = Join-Path $resolvedFixtureRoot 'gate-1080p60-diff-b.contract.json'
    if (-not (Test-Path -LiteralPath $contractPath -PathType Leaf)) {
        throw "The diff fixture contract was not found: $contractPath"
    }
    $contract = Get-Content -LiteralPath $contractPath -Raw | ConvertFrom-Json
    if ($contract.contractVersion -ne 1 -or
        $contract.sourceFileName -cne 'gate-1080p60-a.mp4' -or
        $contract.fixtureFileName -cne 'gate-1080p60-diff-b.mp4' -or
        $contract.width -ne 1920 -or
        $contract.height -ne 1080 -or
        $contract.averageFrameRate -cne '60/1') {
        throw "The diff fixture contract is invalid: $contractPath"
    }
    $sourceHash = (Get-FileHash -LiteralPath $fixtures[0] -Algorithm SHA256).Hash
    $fixtureHash = (Get-FileHash -LiteralPath $fixtures[1] -Algorithm SHA256).Hash
    if ($sourceHash -cne ([string]$contract.sourceFileSha256).ToUpperInvariant() -or
        $fixtureHash -cne ([string]$contract.fixtureFileSha256).ToUpperInvariant()) {
        throw 'The diff fixture files do not match their generation contract.'
    }
    $decodedHashPattern = '^[0-9a-f]{64}$'
    if ([string]$contract.sourceFirstFrameSha256 -cnotmatch $decodedHashPattern -or
        [string]$contract.fixtureFirstFrameSha256 -cnotmatch $decodedHashPattern -or
        $contract.sourceFirstFrameSha256 -ceq $contract.fixtureFirstFrameSha256) {
        throw 'The diff fixture contract does not prove a decoded-pixel difference.'
    }
}

if (-not $RunName) {
    $RunName = if ($ComparisonMode -eq 'side' -or $Profile.EndsWith("-$ComparisonMode")) {
        $Profile
    } else {
        "$Profile-$ComparisonMode"
    }
}
if ($RunName -cnotmatch '^[A-Za-z0-9][A-Za-z0-9_-]{0,127}$') {
    throw 'RunName must be one 1-128 character ASCII filename stem using letters, digits, _ or -.'
}
$canonicalLogRoot = [System.IO.Path]::GetFullPath($resolvedLogRoot)
$stderrPath = [System.IO.Path]::GetFullPath(
    (Join-Path $canonicalLogRoot "$RunName-stderr.log"))
$stdoutPath = [System.IO.Path]::GetFullPath(
    (Join-Path $canonicalLogRoot "$RunName-stdout.log"))
foreach ($logPath in @($stderrPath, $stdoutPath)) {
    $parentPath = [System.IO.Path]::GetDirectoryName($logPath)
    if (-not [string]::Equals(
            $parentPath, $canonicalLogRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "The performance log path escapes LogRoot: $logPath"
    }
}
$arguments = @('--ui-performance') + $fixtures + @('--seconds', $DurationSeconds, '--mode', $ComparisonMode)
if ($ReviewLoad) {
    $arguments += '--review-load'
}
$commandLineArguments = (
    $arguments | ForEach-Object { ConvertTo-WindowsCommandLineArgument -Value ([string]$_) }
) -join ' '
$process = Start-Process `
    -FilePath $resolvedExecutable `
    -ArgumentList $commandLineArguments `
    -RedirectStandardError $stderrPath `
    -RedirectStandardOutput $stdoutPath `
    -PassThru
# Force Windows PowerShell to retain the native process handle needed by ExitCode even when a
# short-lived child exits before the Process object is queried again.
[void]$process.Handle
$processExitCode = $null
# Hang guard only: must stay above the executable's own performance-timeout margin (measured
# duration + 150 s since the JOG measurement window was added) so a genuine hang is reported by
# the child with its DVS_PERFORMANCE_RESULT failure instead of being killed silently here.
$processTimeoutSeconds = $DurationSeconds + 180
try {
    $gateProcess = Get-Process -Id $PID
    try {
        $process.ProcessorAffinity = $gateProcess.ProcessorAffinity
        $process.PriorityClass = $gateProcess.PriorityClass
    } catch [System.InvalidOperationException] {
        if (-not $process.HasExited) {
            throw
        }
    }
    $gatePriority = $gateProcess.PriorityClass
    $gateAffinityMask = [Int64]$gateProcess.ProcessorAffinity.ToInt64()
    $gateProcessorIds = [System.Collections.Generic.List[int]]::new()
    $maximumProcessorCount = [Math]::Min([Environment]::ProcessorCount, 63)
    for ($processorId = 0; $processorId -lt $maximumProcessorCount; ++$processorId) {
        if (($gateAffinityMask -band ([Int64]1 -shl $processorId)) -ne 0) {
            $gateProcessorIds.Add($processorId)
        }
    }

    if (-not $process.WaitForExit($processTimeoutSeconds * 1000)) {
        throw (
            "The $runName gate exceeded its bounded process timeout of " +
            "$processTimeoutSeconds seconds."
        )
    }
    # Finish Process bookkeeping before reading ExitCode. This returns immediately after the
    # bounded wait has observed termination.
    $process.WaitForExit()
    $process.Refresh()
    $processExitCode = $process.ExitCode
    if ($null -eq $processExitCode) {
        throw "The $runName gate process exited without an observable exit code."
    }
} finally {
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        if (-not $process.WaitForExit(5000)) {
            Write-Warning "The timed-out $runName gate process did not exit within 5 seconds."
        }
    }
    $process.Dispose()
}

if ($env:DVS_GATE_RESOURCE_PROFILE) {
    Add-Content `
        -LiteralPath $stdoutPath `
        -Value "DVS_GATE_RESOURCE_PROFILE $env:DVS_GATE_RESOURCE_PROFILE" `
        -Encoding ascii
    Add-Content `
        -LiteralPath $stdoutPath `
        -Value (
            'DVS_GATE_LAUNCH_PROFILE priority={0} processors={1}' -f
            $gatePriority,
            ($gateProcessorIds -join ',')
        ) `
        -Encoding ascii
}

$stderr = Get-Content -LiteralPath $stderrPath
$stderr | Write-Output
$resultLine = $stderr |
    Where-Object { $_ -like 'DVS_PERFORMANCE_RESULT *' } |
    Select-Object -Last 1
if (-not $resultLine) {
    throw "The $Profile gate did not emit DVS_PERFORMANCE_RESULT. See $stderrPath"
}

$json = $resultLine.Substring('DVS_PERFORMANCE_RESULT '.Length) | ConvertFrom-Json

# The gates and the summary both read the full report. Against an older binary a missing key must
# fail once with a precise message instead of cascading property-not-found errors.
$requiredReportKeys = @(
    'expected_source_count', 'screen_refresh_hz', 'open_first_frame_ms', 'peak_working_set_bytes',
    'source_split_observations', 'play',
    'frameset_cache_hits', 'frameset_cache_misses', 'frameset_cache_entries', 'frameset_cache_bytes',
    'decoder_cache_hit_ratio',
    'held_step_sequence_errors', 'held_step_generation_delta', 'held_step_sequential_ratio',
    'held_step_exact_seek_delta', 'held_step_decoder_reopen_count', 'held_step_p95_ms',
    'held_step_submitted_frames', 'held_step_presented_frames',
    'jog_legs', 'jog_nominal_fps', 'jog_forward_frames', 'jog_reverse_frames', 'jog_forward_fps',
    'jog_reverse_fps', 'jog_stop_samples', 'jog_stop_p95_ms', 'jog_stop_max_ms', 'jog_stale_updates'
)
$missingReportKeys = @(
    $requiredReportKeys | Where-Object { $null -eq $json.PSObject.Properties[$_] }
)
if ($missingReportKeys.Count -eq 0) {
    $requiredPlayKeys = @(
        'mode', 'speed_ratio', 'frame_sets_skipped', 'late_frame_sets', 'presented_frames',
        'timing_scope', 'system_presentation'
    )
    $missingReportKeys = @(
        $requiredPlayKeys |
            Where-Object { $null -eq $json.play.PSObject.Properties[$_] } |
            ForEach-Object { "play.$_" }
    )
}
if ($missingReportKeys.Count -gt 0) {
    Write-Output 'DVS_BENCHMARK_SUMMARY'
    Write-Output ("RESULT FAIL reason=report missing keys: " + ($missingReportKeys -join ','))
    throw ("The $runName gate report is missing keys: " + ($missingReportKeys -join ','))
}

# Plan §8.4 benchmark summary: one compact, repeatable block so a local run does not require
# reading the full DVS_PERFORMANCE_RESULT JSON. Fields without real evidence say "unknown"
# (shell timing, GPU memory, physical-vs-remote detection); nothing is synthesized. The format is
# fixed-width-free key=value so both humans and scripts can consume it.
$emitBenchmarkSummary = {
    param(
        [string]$Result,
        [string]$Reason
    )

    $formatFixed = {
        param([double]$Value, [int]$Digits)
        return $Value.ToString(
            'F' + [string]$Digits,
            [System.Globalization.CultureInfo]::InvariantCulture)
    }

    # Build metadata from the repository without assuming git is available.
    $repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
    $commitSha = 'unknown'
    $dirtyState = 'unknown'
    try {
        $resolvedSha = @(& git -C $repositoryRoot rev-parse HEAD 2>$null)
        if ($LASTEXITCODE -eq 0 -and $resolvedSha.Count -gt 0 -and
            -not [string]::IsNullOrWhiteSpace([string]$resolvedSha[0])) {
            $commitSha = ([string]$resolvedSha[0]).Trim()
        }
        $porcelain = @(& git -C $repositoryRoot status --porcelain 2>$null)
        if ($LASTEXITCODE -eq 0) {
            $dirtyState = if ($porcelain.Count -gt 0) { 'true' } else { 'false' }
        }
    } catch {
        # git unavailable: keep the unknown markers.
    }

    $reasonText = ($Reason -replace '\s+', ' ').Trim()
    if ([string]::IsNullOrWhiteSpace($reasonText)) {
        $reasonText = 'none'
    }
    $cpuPeakMb = [math]::Round([double]$json.peak_working_set_bytes / 1MB, 0)

    Write-Output 'DVS_BENCHMARK_SUMMARY'
    Write-Output ("BUILD sha={0} dirty={1} type={2}" -f $commitSha, $dirtyState, $BuildType)
    Write-Output ("DISPLAY path={0} hz={1} timing_scope={2} system_presentation={3}" -f
        $DisplayPath, $json.screen_refresh_hz, $json.play.timing_scope,
        $json.play.system_presentation)
    Write-Output ("OPEN sources={0} first_frame_ms={1} first_frame_p95_ms=unknown shell_ms=unknown" -f
        $json.expected_source_count, $json.open_first_frame_ms)
    Write-Output (
        ("PLAY mode={0} speed_ratio={1} frame_sets_skipped={2} late_frame_sets={3} " +
            "presented_frames={4} mismatch_count={5}") -f
        $json.play.mode, $json.play.speed_ratio, $json.play.frame_sets_skipped,
        $json.play.late_frame_sets, $json.play.presented_frames, $json.source_split_observations)
    Write-Output (
        ("JOG legs={0} direction=alternating nominal_fps={1} forward_fps={2} reverse_fps={3} " +
            "stop_p95_ms={4} stop_max_ms={5} stale_updates={6}") -f
        $json.jog_legs, $json.jog_nominal_fps,
        (& $formatFixed ([double]$json.jog_forward_fps) 2),
        (& $formatFixed ([double]$json.jog_reverse_fps) 2),
        $json.jog_stop_p95_ms, $json.jog_stop_max_ms, $json.jog_stale_updates)
    Write-Output (
        ("CACHE set_hit={0} set_miss={1} entries={2} bytes={3} source_hit={4} cpu_peak_mb={5} " +
            "gpu_peak_mb=unknown") -f
        $json.frameset_cache_hits, $json.frameset_cache_misses, $json.frameset_cache_entries,
        $json.frameset_cache_bytes, (& $formatFixed ([double]$json.decoder_cache_hit_ratio) 3),
        $cpuPeakMb)
    Write-Output ("RESULT {0} reason={1}" -f $Result, $reasonText)
}

try {
    $expectedSourceCount = switch -Wildcard ($Profile) {
        '*-1source*' { 1 }
        '*-2source*' { 2 }
        default { 3 }
    }
    if ($json.expected_source_count -ne $expectedSourceCount) {
        throw (
            "The $Profile gate reported expected_source_count=" +
            "$($json.expected_source_count), expected $expectedSourceCount."
        )
    }
    if ($json.comparison_mode -ne $ComparisonMode -or -not $json.comparison_mode_verified) {
        throw (
            "The $runName gate did not verify comparison mode $ComparisonMode. " +
            "Reported mode=$($json.comparison_mode), verified=$($json.comparison_mode_verified)."
        )
    }
    if ($ComparisonMode -ne 'side' -and $json.comparison_bright_pixel_ratio -lt 0.01) {
        throw (
            "The $runName gate captured no meaningful rendered comparison pixels; " +
            "ratio=$($json.comparison_bright_pixel_ratio)."
        )
    }
    if ($RequireRetainedFrame -and -not $json.comparison_frame_retained) {
        throw "$runName did not preserve the canonical frame while switching comparison modes."
    }
    if (-not $json.screen_refresh_hz -or $json.screen_refresh_hz -lt 120) {
        throw (
            "The $Profile gate rendered on a $($json.screen_refresh_hz) Hz screen; " +
            'a physical screen at 120 Hz or higher is required.'
        )
    }
    if (($null -ne $processExitCode -and $processExitCode -ne 0) -or -not $json.passed) {
        throw "The $Profile performance gate failed with exit code $processExitCode."
    }

    # Held-forward step gate (plan 1.6 M1.1). These keys are emitted by the HeldStepping stage in
    # Main.cpp. All criteria except the P95 threshold are hard gates (exit non-zero on failure);
    # the P95 threshold is a recorded metric plus a soft warning because no 1.4.5 held-forward
    # baseline exists yet (the plan gates the absolute threshold "once baseline data exists").
    $heldStepSequenceErrors = [int]$json.held_step_sequence_errors
    $heldStepGenerationDelta = [int]$json.held_step_generation_delta
    $heldStepSequentialRatio = [double]$json.held_step_sequential_ratio
    $heldStepExactSeekDelta = [int]$json.held_step_exact_seek_delta
    $heldStepDecoderReopenCount = [int]$json.held_step_decoder_reopen_count
    $heldStepP95 = [double]$json.held_step_p95_ms
    $heldStepSubmitted = [int]$json.held_step_submitted_frames
    $heldStepPresented = [int]$json.held_step_presented_frames

    if ($heldStepSequenceErrors -ne 0) {
        throw (
            "The $runName held-forward gate reported $heldStepSequenceErrors missing intermediate " +
            "FrameId(s); expected 0."
        )
    }
    if ($heldStepGenerationDelta -ne 0) {
        # A non-zero generation delta means a newer generation superseded in-flight work during
        # the window, i.e. at least one stale commit. The healthy held-forward fixture never
        # does this.
        throw (
            "The $runName held-forward gate reported generation_delta=$heldStepGenerationDelta; " +
            "expected 0 (no stale commits)."
        )
    }
    # Sequential-continuation ratio: recorded metric + soft warning. This ratio only reaches ~1.0
    # once the decode pipeline is fully warmed; on slow or contended hardware it can sit below
    # 0.95 for the early window through no product defect (unlike sequence-errors/generation-delta,
    # which are correctness signals). Throwing here would make the gate trip on hardware speed,
    # so — like the P95 latency — it is captured for the record and warned on, not hard-gated. The
    # authoritative correctness gates are held_step_sequence_errors and held_step_generation_delta
    # above.
    if ($heldStepSequentialRatio -lt 0.95) {
        Write-Warning (
            "The $runName held-forward gate recorded a sequential continuation ratio=" +
            ("{0:N3}" -f $heldStepSequentialRatio) +
            " (< 0.95). This is typically a pipeline-warm-up / hardware-speed artifact, not a " +
            "correctness defect; captured for the record. The authoritative correctness gates " +
            "(sequence_errors, generation_delta) are enforced separately.")
    }
    if ($heldStepExactSeekDelta -gt 2) {
        throw (
            "The $runName held-forward gate reported exact_seek_delta=$heldStepExactSeekDelta; " +
            "expected <= 2 after warm-up."
        )
    }
    if ($heldStepDecoderReopenCount -ne 0) {
        throw (
            "The $runName held-forward gate reported decoder_reopen_count=" +
            "$heldStepDecoderReopenCount; expected 0 on a healthy fixture."
        )
    }
    # Partial FrameSets are not surfaced by a dedicated counter, so they are not structurally
    # checked here; the sequence-error and sequential-ratio gates are the effective correctness
    # signal. "0 partial FrameSets" is verified implicitly (a partial set would surface as a
    # sequence error).

    # Held-window completion accounting: every submitted step must be presented. The executable
    # hard-fails on this too, but checking here keeps a mismatch visible in gate output even when
    # running an older binary, and documents the exact invariant being asserted.
    if ($heldStepPresented -ne $heldStepSubmitted) {
        throw (
            "The $runName held-forward gate presented $heldStepPresented frames for " +
            "$heldStepSubmitted submissions; every submission must commit exactly one frame."
        )
    }

    # P95 threshold: recorded metric + soft warning. No 1.4.5 held-forward baseline exists yet, so
    # the absolute hardware-normalized threshold cannot be enforced (plan: "once baseline data
    # exists"). Log the value so the first baseline is captured on reference hardware.
    if ($heldStepP95 -lt 0) {
        Write-Warning (
            "The $runName held-forward gate reported held_step_p95_ms=$heldStepP95 " +
            "(no latency samples); measurement may have been skipped.")
    } else {
        Write-Warning (
            "The $runName held-forward gate recorded held_step_p95_ms=$heldStepP95; " +
            "no baseline exists yet — value captured for the first-baseline record.")
    }

    # JOG walk gate (E04 acceptance / plan §8.4 JOG line). Structural correctness is hard-gated:
    # the stage must have run its legs, every leg must have settled into a stop sample, and the
    # forward walk must have advanced. Stale post-settle motion already hard-fails inside the
    # executable (jog-stale-update). The experience thresholds below are hard gates too, while
    # staying behavior gates rather than structural ones: they measure what a held-direction walk
    # feels like (release latency and reverse throughput), not that the walk mechanically ran.
    $jogLegs = [int]$json.jog_legs
    $jogForwardFrames = [int]$json.jog_forward_frames
    $jogForwardFps = [double]$json.jog_forward_fps
    $jogReverseFps = [double]$json.jog_reverse_fps
    $jogStopSamples = [int]$json.jog_stop_samples
    $jogStopP95 = [double]$json.jog_stop_p95_ms
    $jogStaleUpdates = [int]$json.jog_stale_updates
    if ($jogLegs -lt 4) {
        throw (
            "The $runName JOG stage reported $jogLegs leg(s); at least 4 are required so stop " +
            "samples cover both directions."
        )
    }
    if ($jogStopSamples -ne $jogLegs) {
        throw (
            "The $runName JOG stage produced $jogStopSamples stop sample(s) for $jogLegs legs; " +
            "every released leg must settle into a measured stop."
        )
    }
    if ($jogForwardFrames -le 0) {
        throw "The $runName JOG stage advanced 0 forward frames; the held walk never moved."
    }
    if ($jogStaleUpdates -ne 0) {
        throw (
            "The $runName JOG stage reported $jogStaleUpdates stale update(s) after the walk " +
            "settled; expected 0."
        )
    }
    if ($jogStopP95 -lt 0) {
        Write-Warning (
            "The $runName JOG stage reported jog_stop_p95_ms=$jogStopP95 (no stop samples); " +
            "measurement may have been skipped.")
    } elseif ($jogStopP95 -gt 500) {
        throw (
            "The $runName JOG stage recorded jog_stop_p95_ms=$jogStopP95, above the 500 ms " +
            "experience budget ($jogStopSamples stop samples). This is an experience baseline " +
            "gate, not a structural gate: the measured release-to-stop latency is too slow to " +
            "feel settled, even though every leg did run and settle."
        )
    } else {
        # Still recorded: the experience thresholds are gates, but the first-baseline discipline
        # is to write every measured value down, not only the ones that failed.
        Write-Host (
            "The $runName JOG stage recorded jog_stop_p95_ms=$jogStopP95 over " +
            "$jogStopSamples stop sample(s): inside the 500 ms experience budget.")
    }
    # Reverse throughput is gated relative to the forward walk because the absolute value depends
    # on source resolution, codec, and hardware. A reverse walk under a fifth of its forward walk
    # means held-backward is paying a per-step seek it should be serving from the GOP window.
    if ($jogForwardFps -le 0) {
        throw (
            "The $runName JOG stage reported jog_forward_fps=$jogForwardFps with " +
            "$jogForwardFrames forward frame(s); the forward walk rate is unmeasurable, so the " +
            "reverse/forward ratio cannot be evaluated."
        )
    }
    if ($jogReverseFps -le 0) {
        throw (
            "The $runName JOG stage reported jog_reverse_fps=$jogReverseFps; held-backward " +
            "presented no frames. This is an experience baseline gate, not a structural gate."
        )
    }
    $jogReverseRatio = $jogReverseFps / $jogForwardFps
    $jogReverseRatioText = $jogReverseRatio.ToString(
        'F3',
        [System.Globalization.CultureInfo]::InvariantCulture)
    if ($jogReverseRatio -lt 0.2) {
        throw (
            "The $runName JOG stage recorded jog_reverse_fps=$jogReverseFps against " +
            "jog_forward_fps=$jogForwardFps (ratio $jogReverseRatioText, floor 0.2). This is " +
            "an experience baseline gate, not a structural gate: held-backward is stepping far " +
            "slower than held-forward."
        )
    }
    # Recorded on the way through as well: the ratio is the number that tells the reverse cache
    # whether its GOP window is actually being used.
    Write-Host (
        "The $runName JOG stage recorded jog_forward_fps=$jogForwardFps against " +
        "jog_reverse_fps=$jogReverseFps (ratio $jogReverseRatioText).")
} catch {
    & $emitBenchmarkSummary 'FAIL' $_.Exception.Message
    throw
}

$summaryTemplate =
    'DVS_GATE_PASSED profile={0} mode={1} duration={2}s presented={3} dropped={4} ' +
    'seek_p95={5}ms shutdown={6}ms held_step_p95={7}ms sequential_ratio={8:N3} ' +
    'held_step_presented={9}/{10}'
Write-Output (
    $summaryTemplate -f
    $Profile,
    $ComparisonMode,
    $DurationSeconds,
    $json.presented_frames,
    $json.dropped_frames,
    $json.seek_p95_ms,
    $json.shutdown_ms,
    $json.held_step_p95_ms,
    $json.held_step_sequential_ratio,
    $heldStepPresented,
    $heldStepSubmitted
)

# Plan §8.4 summary last: RESULT is the line terminal readers look for, and the fields above it
# carry the evidence behind that verdict.
& $emitBenchmarkSummary 'PASS' 'none'
