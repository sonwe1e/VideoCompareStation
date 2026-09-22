#requires -Version 7.0
Set-StrictMode -Version Latest

Import-Module (Join-Path $PSScriptRoot 'CommandLineArgument.psm1') -Force

# Schema-v1 defined TraceEventKind values are 0..22 inclusive (PlaybackTrace.h /
# docs/engineering/trace-schema.md). Unknown kinds stay fail-closed.
$script:SchemaV1MaxKind = 22

function Test-PlaybackTraceFile {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$TracePath,

        [string]$ResultPrefix = 'PLAYBACK_TRACE'
    )

    $isExactInteger = {
        param($Value)

        return $Value -is [sbyte] -or $Value -is [byte] -or
            $Value -is [int16] -or $Value -is [uint16] -or
            $Value -is [int32] -or $Value -is [uint32] -or
            $Value -is [int64] -or $Value -is [uint64] -or
            $Value -is [decimal] -or $Value -is [System.Numerics.BigInteger]
    }

    if (-not (Test-Path -LiteralPath $TracePath -PathType Leaf)) {
        throw "$ResultPrefix`_TRACE_MISSING: expected trace at $TracePath"
    }

    $traceLines = @(Get-Content -LiteralPath $TracePath)
    if ($traceLines.Count -lt 2) {
        throw (
            "$ResultPrefix`_TRACE_INCOMPLETE: expected a header and at least one event at " +
            "$TracePath; found $($traceLines.Count) line(s)."
        )
    }

    try {
        $header = $traceLines[0] | ConvertFrom-Json -ErrorAction Stop
    } catch {
        throw "$ResultPrefix`_TRACE_INVALID_HEADER: $TracePath. $($_.Exception.Message)"
    }
    if ($null -eq $header -or $header -is [ValueType] -or $header -is [string] -or
        $header -is [array]) {
        throw "$ResultPrefix`_TRACE_INVALID_HEADER: expected traceVersion 1 at $TracePath."
    }
    $traceVersionProperty = $header.PSObject.Properties['traceVersion']
    $headerPropertyCount = @($header.PSObject.Properties).Count
    if ($headerPropertyCount -ne 1 -or
        $null -eq $traceVersionProperty -or
        -not (& $isExactInteger $header.traceVersion)) {
        throw "$ResultPrefix`_TRACE_INVALID_HEADER: expected traceVersion 1 at $TracePath."
    }
    try {
        $traceVersion = [int]$header.traceVersion
    } catch {
        throw "$ResultPrefix`_TRACE_INVALID_HEADER: expected traceVersion 1 at $TracePath."
    }
    if ([decimal]$header.traceVersion -ne [decimal]$traceVersion -or $traceVersion -ne 1) {
        throw "$ResultPrefix`_TRACE_INVALID_HEADER: expected traceVersion 1 at $TracePath."
    }

    $eventCount = 0
    [uint64]$overflowCount = 0
    foreach ($line in ($traceLines | Select-Object -Skip 1)) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        try {
            $record = $line | ConvertFrom-Json -ErrorAction Stop
        } catch {
            throw "$ResultPrefix`_TRACE_INVALID_RECORD: $TracePath. $($_.Exception.Message)"
        }
        if ($null -eq $record -or $record -is [ValueType] -or $record -is [string] -or
            $record -is [array]) {
            throw "$ResultPrefix`_TRACE_INVALID_RECORD: expected an object at $TracePath."
        }
        if ($null -ne $record.PSObject.Properties['overflow']) {
            $recordPropertyCount = @($record.PSObject.Properties).Count
            if ($recordPropertyCount -ne 1 -or
                -not (& $isExactInteger $record.overflow)) {
                throw "$ResultPrefix`_TRACE_INVALID_OVERFLOW: malformed marker at $TracePath."
            }
            try {
                $overflow = [uint64]$record.overflow
            } catch {
                throw "$ResultPrefix`_TRACE_INVALID_OVERFLOW: invalid count at $TracePath."
            }
            if ($overflow -eq 0 -or [decimal]$record.overflow -ne [decimal]$overflow) {
                throw "$ResultPrefix`_TRACE_INVALID_OVERFLOW: overflow must be positive at $TracePath."
            }
            if ([uint64]::MaxValue - $overflowCount -lt $overflow) {
                throw "$ResultPrefix`_TRACE_INVALID_OVERFLOW: total overflow exceeds uint64 at $TracePath."
            }
            $overflowCount += $overflow
            continue
        }
        foreach ($requiredField in @(
                't', 'kind', 's', 'e', 'topo', 'tl', 'al', 'gen', 'dev', 'req', 'cmd', 'p')) {
            if ($null -eq $record.PSObject.Properties[$requiredField]) {
                throw (
                    "$ResultPrefix`_TRACE_INVALID_EVENT: an event lacks $requiredField at " +
                    "$TracePath."
                )
            }
        }
        foreach ($numericField in @('t', 's', 'e', 'topo', 'tl', 'al', 'gen', 'dev', 'req', 'p')) {
            $value = $record.$numericField
            if (-not (& $isExactInteger $value)) {
                throw "$ResultPrefix`_TRACE_INVALID_EVENT: $numericField is not numeric at $TracePath."
            }
            try {
                $converted = [uint64]$value
            } catch {
                throw "$ResultPrefix`_TRACE_INVALID_EVENT: invalid $numericField at $TracePath."
            }
            if ([decimal]$value -ne [decimal]$converted) {
                throw "$ResultPrefix`_TRACE_INVALID_EVENT: invalid $numericField at $TracePath."
            }
        }
        # Optional additive incoming-identity fields (schema v1). Either all five are present or
        # none are; partial sets are malformed.
        $incomingFields = @('is', 'ie', 'igen', 'idev', 'ireq')
        $presentIncoming = @(
            $incomingFields | Where-Object {
                $null -ne $record.PSObject.Properties[$_]
            }
        )
        if ($presentIncoming.Count -ne 0 -and $presentIncoming.Count -ne $incomingFields.Count) {
            throw (
                "$ResultPrefix`_TRACE_INVALID_EVENT: partial incoming identity at $TracePath."
            )
        }
        foreach ($incomingField in $incomingFields) {
            if ($null -eq $record.PSObject.Properties[$incomingField]) {
                continue
            }
            $value = $record.$incomingField
            if (-not (& $isExactInteger $value)) {
                throw (
                    "$ResultPrefix`_TRACE_INVALID_EVENT: $incomingField is not numeric at " +
                    "$TracePath."
                )
            }
            try {
                $converted = [uint64]$value
            } catch {
                throw (
                    "$ResultPrefix`_TRACE_INVALID_EVENT: invalid $incomingField at $TracePath."
                )
            }
            if ([decimal]$value -ne [decimal]$converted) {
                throw (
                    "$ResultPrefix`_TRACE_INVALID_EVENT: invalid $incomingField at $TracePath."
                )
            }
        }
        try {
            $kind = [int]$record.kind
        } catch {
            throw "$ResultPrefix`_TRACE_INVALID_EVENT: invalid kind at $TracePath."
        }
        if (-not (& $isExactInteger $record.kind) -or
            [decimal]$record.kind -ne [decimal]$kind -or $kind -lt 0 -or
            $kind -gt $script:SchemaV1MaxKind) {
            throw "$ResultPrefix`_TRACE_INVALID_EVENT: kind is outside schema v1 at $TracePath."
        }
        if ($null -ne $record.cmd) {
            if (-not (& $isExactInteger $record.cmd)) {
                throw "$ResultPrefix`_TRACE_INVALID_EVENT: cmd is not uint64 or null at $TracePath."
            }
            try {
                $command = [uint64]$record.cmd
            } catch {
                throw "$ResultPrefix`_TRACE_INVALID_EVENT: invalid cmd at $TracePath."
            }
            if ([decimal]$record.cmd -ne [decimal]$command) {
                throw "$ResultPrefix`_TRACE_INVALID_EVENT: invalid cmd at $TracePath."
            }
        }
        ++$eventCount
    }
    if ($eventCount -eq 0) {
        throw "$ResultPrefix`_TRACE_INCOMPLETE: no trace event was written to $TracePath."
    }
    if ($overflowCount -gt 0) {
        throw "$ResultPrefix`_TRACE_OVERFLOW: $overflowCount event(s) were lost in $TracePath."
    }

    return [pscustomobject]@{
        LineCount = $traceLines.Count
        EventCount = $eventCount
    }
}

function Test-PlaybackTraceInvariants {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$TracePath,

        [string]$ResultPrefix = 'PLAYBACK_TRACE'
    )

    # Semantic Phase 0 checks on an already structurally valid schema-v1 file.
    # Overflow is fail-closed upstream in Test-PlaybackTraceFile; this function assumes none.
    # Fail closed on any invariant violation. Stale-commit detection is limited to identity
    # regressions visible in the serialized stream (see trace-schema.md).

    $isExactInteger = {
        param($Value)

        return $Value -is [sbyte] -or $Value -is [byte] -or
            $Value -is [int16] -or $Value -is [uint16] -or
            $Value -is [int32] -or $Value -is [uint32] -or
            $Value -is [int64] -or $Value -is [uint64] -or
            $Value -is [decimal] -or $Value -is [System.Numerics.BigInteger]
    }

    if (-not (Test-Path -LiteralPath $TracePath -PathType Leaf)) {
        throw "$ResultPrefix`_TRACE_MISSING: expected trace at $TracePath"
    }

    $traceLines = @(Get-Content -LiteralPath $TracePath)
    if ($traceLines.Count -lt 2) {
        throw (
            "$ResultPrefix`_TRACE_INCOMPLETE: expected a header and at least one event at " +
            "$TracePath; found $($traceLines.Count) line(s)."
        )
    }

    $eventCount = 0
    $commandAcceptedCount = 0
    $commandTerminalCount = 0
    $presentationAckCount = 0
    $snapshotCommitCount = 0
    $commandTerminalMismatchCount = 0
    $ackBeforeCommitViolations = 0
    $staleCommitCount = 0
    $staleArrivalCount = 0
    $staleArrivalPublishedCount = 0
    $maxNoFrame = [decimal][uint64]::MaxValue

    # frame payload -> whether the last FrameSetReady for that frame had matching incoming
    $readyFresh = @{}

    # session|epoch|cmd -> accept count / terminal count
    $commandAccepts = @{}
    $commandTerminals = @{}
    # identity without req/cmd -> set of acked canonical positions
    $ackedFrames = @{}
    # session|epoch -> max gen/topo/tl/dev observed so far
    $sessionMax = @{}

    foreach ($line in ($traceLines | Select-Object -Skip 1)) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        try {
            $record = $line | ConvertFrom-Json -ErrorAction Stop
        } catch {
            throw "$ResultPrefix`_TRACE_INVALID_RECORD: $TracePath. $($_.Exception.Message)"
        }
        if ($null -ne $record.PSObject.Properties['overflow']) {
            throw (
                "$ResultPrefix`_TRACE_OVERFLOW: invariant analysis requires a complete " +
                "capture at $TracePath."
            )
        }

        $session = [uint64]$record.s
        $epoch = [uint64]$record.e
        $topology = [uint64]$record.topo
        $timeline = [uint64]$record.tl
        $alignment = [uint64]$record.al
        $generation = [uint64]$record.gen
        $device = [uint64]$record.dev
        $kind = [int]$record.kind
        $payload = [uint64]$record.p
        $sessionKey = "$session|$epoch"
        $identityKey = "$session|$epoch|$topology|$timeline|$alignment|$generation|$device"

        $incomingStale = $false
        if ($null -ne $record.PSObject.Properties['is']) {
            # Live `req` on coordinator-owned TraceIdentity is intentionally 0, so request is
            # exported for correlation but not part of the stale comparison.
            $incomingStale = [uint64]$record.is -ne $session -or
                [uint64]$record.ie -ne $epoch -or
                [uint64]$record.igen -ne $generation -or
                [uint64]$record.idev -ne $device
            if ($incomingStale) {
                ++$staleArrivalCount
            }
        }

        $max = $sessionMax[$sessionKey]
        if ($null -eq $max) {
            $max = [pscustomobject]@{
                Generation = $generation
                Topology = $topology
                Timeline = $timeline
                Device = $device
            }
            $sessionMax[$sessionKey] = $max
        }

        switch ($kind) {
            0 {
                # CommandAccepted
                if ($null -eq $record.cmd -or -not (& $isExactInteger $record.cmd)) {
                    $commandTerminalMismatchCount++
                } else {
                    $commandId = [uint64]$record.cmd
                    $commandKey = "$session|$epoch|$commandId"
                    if ($null -eq $commandAccepts[$commandKey]) {
                        $commandAccepts[$commandKey] = 0
                    }
                    $commandAccepts[$commandKey] = $commandAccepts[$commandKey] + 1
                    ++$commandAcceptedCount
                }
            }
            4 {
                $readyFresh[$payload.ToString()] = -not $incomingStale
            }
            6 {
                if ($readyFresh[$payload.ToString()] -eq $false) {
                    ++$staleArrivalPublishedCount
                }
            }
            7 {
                ++$presentationAckCount
                if ($null -eq $ackedFrames[$identityKey]) {
                    $ackedFrames[$identityKey] = New-Object 'System.Collections.Generic.HashSet[decimal]'
                }
                [void]$ackedFrames[$identityKey].Add([decimal]$payload)
            }
            8 {
                ++$snapshotCommitCount
                $isStale = $generation -lt $max.Generation -or
                    $topology -lt $max.Topology -or
                    $timeline -lt $max.Timeline -or
                    $device -lt $max.Device
                if ($isStale) {
                    ++$staleCommitCount
                }
                if ([decimal]$payload -ne $maxNoFrame) {
                    $acks = $ackedFrames[$identityKey]
                    if ($null -eq $acks -or -not $acks.Contains([decimal]$payload)) {
                        ++$ackBeforeCommitViolations
                    }
                }
            }
            9 {
                # CommandTerminal
                if ($null -eq $record.cmd -or -not (& $isExactInteger $record.cmd)) {
                    $commandTerminalMismatchCount++
                } else {
                    $commandId = [uint64]$record.cmd
                    $commandKey = "$session|$epoch|$commandId"
                    if ($null -eq $commandTerminals[$commandKey]) {
                        $commandTerminals[$commandKey] = 0
                    }
                    $commandTerminals[$commandKey] = $commandTerminals[$commandKey] + 1
                    ++$commandTerminalCount
                }
            }
            13 {
                # DeviceGenerationChanged — payload is the new device generation.
                if ($payload -gt $device) {
                    $device = $payload
                }
            }
        }

        if ($generation -gt $max.Generation) { $max.Generation = $generation }
        if ($topology -gt $max.Topology) { $max.Topology = $topology }
        if ($timeline -gt $max.Timeline) { $max.Timeline = $timeline }
        if ($device -gt $max.Device) { $max.Device = $device }

        ++$eventCount
    }

    if ($eventCount -eq 0) {
        throw "$ResultPrefix`_TRACE_INCOMPLETE: no trace event was written to $TracePath."
    }

    foreach ($key in $commandAccepts.Keys) {
        $acceptCount = $commandAccepts[$key]
        $terminalCount = 0
        if ($null -ne $commandTerminals[$key]) {
            $terminalCount = $commandTerminals[$key]
        }
        if ($acceptCount -ne 1 -or $terminalCount -ne 1) {
            ++$commandTerminalMismatchCount
        }
    }
    foreach ($key in $commandTerminals.Keys) {
        if ($null -eq $commandAccepts[$key]) {
            ++$commandTerminalMismatchCount
        }
    }

    if ($commandTerminalMismatchCount -ne 0) {
        throw (
            "$ResultPrefix`_COMMAND_TERMINAL_MISMATCH count=$commandTerminalMismatchCount " +
            "accepted=$commandAcceptedCount terminal=$commandTerminalCount path=$TracePath"
        )
    }
    if ($ackBeforeCommitViolations -ne 0) {
        throw (
            "$ResultPrefix`_ACK_BEFORE_COMMIT_VIOLATIONS count=$ackBeforeCommitViolations " +
            "acks=$presentationAckCount commits=$snapshotCommitCount path=$TracePath"
        )
    }
    if ($staleCommitCount -ne 0) {
        throw (
            "$ResultPrefix`_STALE_COMMIT count=$staleCommitCount commits=$snapshotCommitCount " +
            "path=$TracePath"
        )
    }
    if ($staleArrivalPublishedCount -ne 0) {
        throw (
            "$ResultPrefix`_STALE_ARRIVAL_PUBLISHED count=$staleArrivalPublishedCount " +
            "stale_arrivals=$staleArrivalCount path=$TracePath"
        )
    }

    return [pscustomobject]@{
        EventCount = $eventCount
        CommandAcceptedCount = $commandAcceptedCount
        CommandTerminalCount = $commandTerminalCount
        CommandTerminalMismatchCount = $commandTerminalMismatchCount
        PresentationAckCount = $presentationAckCount
        SnapshotCommitCount = $snapshotCommitCount
        AckBeforeCommitViolations = $ackBeforeCommitViolations
        StaleCommitCount = $staleCommitCount
        StaleArrivalCount = $staleArrivalCount
        StaleArrivalPublishedCount = $staleArrivalPublishedCount
        PartialFrameSetCount = 0
    }
}

function Invoke-PlaybackTraceGate {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('navigation', 'comparison-semantics')]
        [string]$Gate,

        [Parameter(Mandatory = $true)]
        [string]$Executable,

        [Parameter(Mandatory = $true)]
        [string[]]$Fixtures,

        [ValidateRange(1, 3600)]
        [int]$DurationSeconds,

        [ValidateSet('side', 'wipe', 'diff')]
        [string]$ComparisonMode = 'side',

        [string]$LogRoot,

        [string]$RunName
    )

    $resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
    $resolvedFixtures = foreach ($fixture in $Fixtures) {
        (Resolve-Path -LiteralPath $fixture).Path
    }

    $repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
    if ($Gate -eq 'navigation') {
        if (-not $RunName) {
            $RunName = 'navigation-gate'
        }
        if (-not $LogRoot) {
            $LogRoot = Join-Path $repositoryRoot 'out\navigation-gate'
        }
        $resultPrefix = 'NAVIGATION_GATE'
        $arguments = @('--ui-performance') + $resolvedFixtures + @(
            '--seconds',
            $DurationSeconds
        )
    } else {
        if (-not $RunName) {
            $RunName = if ($ComparisonMode -eq 'side') {
                'comparison-semantics'
            } else {
                "comparison-semantics-$ComparisonMode"
            }
        }
        if (-not $LogRoot) {
            $LogRoot = Join-Path $repositoryRoot 'out\comparison-semantics-gate'
        }
        $resultPrefix = 'COMPARISON_SEMANTICS_GATE'
        $arguments = @('--ui-performance') + $resolvedFixtures + @(
            '--seconds',
            $DurationSeconds,
            '--mode',
            $ComparisonMode
        )
    }

    # RunName becomes part of three output filenames, including the stale trace removed below.
    # Restrict it to one portable filename stem so it cannot traverse outside LogRoot.
    if ($RunName -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$') {
        throw (
            'RunName must start with an ASCII letter or digit and contain only letters, ' +
            'digits, dot, underscore, or hyphen (maximum 128 characters).'
        )
    }

    New-Item -ItemType Directory -Path $LogRoot -Force | Out-Null
    $resolvedLogRoot = (Resolve-Path -LiteralPath $LogRoot).Path
    $tracePath = Join-Path $resolvedLogRoot "$RunName-trace.jsonl"
    $stderrPath = Join-Path $resolvedLogRoot "$RunName-stderr.log"
    $stdoutPath = Join-Path $resolvedLogRoot "$RunName-stdout.log"

    # A failed launch must not make a stale trace from an earlier run look successful.
    if (Test-Path -LiteralPath $tracePath) {
        Remove-Item -LiteralPath $tracePath -Force
    }

    $traceEnvironmentPath = 'Env:DVS_PLAYBACK_TRACE'
    $hadTraceEnvironment = Test-Path -LiteralPath $traceEnvironmentPath
    $previousTraceEnvironment = if ($hadTraceEnvironment) {
        (Get-Item -LiteralPath $traceEnvironmentPath).Value
    } else {
        $null
    }

    $processExitCode = $null
    $process = $null
    try {
        Set-Item -LiteralPath $traceEnvironmentPath -Value $tracePath

        $gateProcess = Get-Process -Id $PID
        $gateAffinity = $gateProcess.ProcessorAffinity
        $gatePriority = $gateProcess.PriorityClass
        $escapedArguments = @(
            $arguments | ForEach-Object {
                ConvertTo-WindowsCommandLineArgument -Value ([string]$_)
            }
        )
        $process = Start-Process `
            -FilePath $resolvedExecutable `
            -ArgumentList $escapedArguments `
            -RedirectStandardError $stderrPath `
            -RedirectStandardOutput $stdoutPath `
            -PassThru
        # Force Windows PowerShell to retain the native process handle needed by ExitCode even
        # when a short-lived child exits before the Process object is queried again; otherwise
        # [int]$process.ExitCode reads as 0 and a failed gate looks successful.
        [void]$process.Handle
        try {
            $process.ProcessorAffinity = $gateAffinity
            $process.PriorityClass = $gatePriority
        } catch [System.InvalidOperationException] {
            if (-not $process.HasExited) {
                throw
            }
        }

        $timeoutMilliseconds = ([int64]$DurationSeconds + 30L) * 1000L
        if (-not $process.WaitForExit([int]$timeoutMilliseconds)) {
            throw (
                "$resultPrefix`_PROCESS_TIMEOUT: exceeded $timeoutMilliseconds ms " +
                "(duration plus shutdown grace)."
            )
        }
        $process.Refresh()
        $processExitCode = [int]$process.ExitCode
    } finally {
        if ($null -ne $process -and -not $process.HasExited) {
            try {
                $process.Kill()
                $process.WaitForExit()
            } catch {
                Write-Warning "Unable to stop failed gate process $($process.Id): $($_.Exception.Message)"
            }
        }
        if ($hadTraceEnvironment) {
            Set-Item -LiteralPath $traceEnvironmentPath -Value $previousTraceEnvironment
        } else {
            Remove-Item -LiteralPath $traceEnvironmentPath -ErrorAction SilentlyContinue
        }
    }

    if ($null -eq $processExitCode) {
        throw "$resultPrefix`_PROCESS_EXIT_CODE_MISSING"
    }
    if ($processExitCode -ne 0) {
        return [pscustomobject]@{
            ExitCode = $processExitCode
            Message = "$resultPrefix`_PROCESS_FAILED child_exit=$processExitCode"
        }
    }
    $validation = Test-PlaybackTraceFile -TracePath $tracePath -ResultPrefix $resultPrefix
    $invariants = Test-PlaybackTraceInvariants -TracePath $tracePath -ResultPrefix $resultPrefix

    return [pscustomobject]@{
        ExitCode = $processExitCode
        Message = (
            "$resultPrefix`_TRACE_OK path=$tracePath lines=$($validation.LineCount) " +
            "events=$($validation.EventCount) " +
            "command_terminal_mismatch=$($invariants.CommandTerminalMismatchCount) " +
            "ack_before_commit_violations=$($invariants.AckBeforeCommitViolations) " +
            "stale_commit=$($invariants.StaleCommitCount) " +
            "stale_arrival=$($invariants.StaleArrivalCount) " +
            "stale_arrival_published=$($invariants.StaleArrivalPublishedCount) " +
            "partial_frame_set=$($invariants.PartialFrameSetCount)"
        )
    }
}

function Get-PlaybackTraceTimingSummary {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$TracePath
    )

    # T0 evidence baseline: separates preparation, draw submission, and final presentation
    # observation per canonical frame and reports the new-content display interval distribution
    # (P50/P95/P99 + longest pause).
    #   prepare      : FrameSetReady -> RenderPublished (and -> RenderDrawStarted when present)
    #   draw submit  : RenderDrawStarted -> RenderAckPublished
    #   final present: RenderAckPublished -> PresentationAcknowledged -> SnapshotCommitted
    # Legacy hops (ReadyToPublish / PublishToAck / AckToCommit) remain for older captures that
    # lack kinds 16/17. Input must be a structurally valid schema-v1 file (call
    # Test-PlaybackTraceFile first); overflow markers make the capture incomplete, so this
    # analyzer fails closed.

    if (-not (Test-Path -LiteralPath $TracePath -PathType Leaf)) {
        throw "PLAYBACK_TRACE_TIMING_MISSING: expected trace at $TracePath"
    }

    $traceLines = @(Get-Content -LiteralPath $TracePath)
    if ($traceLines.Count -lt 2) {
        throw "PLAYBACK_TRACE_TIMING_INCOMPLETE: expected a header and at least one event at $TracePath"
    }

    # Per-frame event times keyed by identity-with-frame: "s|e|gen|dev|payload". Later events
    # for the same key win (a frame may be republished while a prior ack is still outstanding).
    # Draw-stage events (kinds 16/17) carry empty identity and are keyed by frame payload only
    # (trace-schema.md: correlate renderer-side observations by frame id within one run).
    $readyTimes = @{}
    $publishedTimes = @{}
    $drawStartTimes = @{}
    $drawAckTimes = @{}
    $ackTimes = @{}
    $commitTimes = @{}
    $commitOrder = [System.Collections.Generic.List[object]]::new()
    $eventCount = 0

    foreach ($line in ($traceLines | Select-Object -Skip 1)) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        try {
            $record = $line | ConvertFrom-Json -ErrorAction Stop
        } catch {
            throw "PLAYBACK_TRACE_TIMING_INVALID_RECORD: $TracePath. $($_.Exception.Message)"
        }
        if ($null -ne $record.PSObject.Properties['overflow']) {
            throw "PLAYBACK_TRACE_TIMING_OVERFLOW: incomplete capture at $TracePath"
        }
        $session = [uint64]$record.s
        $epoch = [uint64]$record.e
        $generation = [uint64]$record.gen
        $device = [uint64]$record.dev
        $payload = [uint64]$record.p
        $kind = [int]$record.kind
        $timestamp = [uint64]$record.t
        $identityKey = "$session|$epoch|$generation|$device|$payload"
        switch ($kind) {
            4 { $readyTimes[$identityKey] = $timestamp }
            6 { $publishedTimes[$identityKey] = $timestamp }
            16 { $drawStartTimes[$payload.ToString()] = $timestamp }
            17 { $drawAckTimes[$payload.ToString()] = $timestamp }
            7 { $ackTimes[$identityKey] = $timestamp }
            8 {
                if ($payload -ne [decimal][uint64]::MaxValue) {
                    $commitTimes[$identityKey] = $timestamp
                    [void]$commitOrder.Add([pscustomobject]@{
                            Time = $timestamp
                            IdentityKey = $identityKey
                        })
                }
            }
        }
        ++$eventCount
    }
    if ($eventCount -eq 0) {
        throw "PLAYBACK_TRACE_TIMING_INCOMPLETE: no trace event was written to $TracePath."
    }

    $percentileSummary = {
        param([System.Collections.Generic.List[object]]$Values)

        $count = $Values.Count
        if ($count -eq 0) {
            return [pscustomobject]@{
                Count = 0
                P50 = $null
                P95 = $null
                P99 = $null
                Max = $null
            }
        }
        $sorted = @($Values | Sort-Object)
        # Nearest-rank index mirrors the C++ percentile convention (truncating integer
        # division, not PowerShell's banker's rounding of [int] casts).
        $indexOf = { param($Quantile) [math]::Min([int][math]::Floor($count * $Quantile / 100), $count - 1) }
        $index50 = & $indexOf 50
        $index95 = & $indexOf 95
        $index99 = & $indexOf 99
        return [pscustomobject]@{
            Count = $count
            P50 = [double]$sorted[$index50]
            P95 = [double]$sorted[$index95]
            P99 = [double]$sorted[$index99]
            Max = [double]$sorted[$count - 1]
        }
    }

    $readyToCommit = [System.Collections.Generic.List[object]]::new()
    $publishToCommit = [System.Collections.Generic.List[object]]::new()
    $readyToPublish = [System.Collections.Generic.List[object]]::new()
    $publishToAck = [System.Collections.Generic.List[object]]::new()
    $ackToCommit = [System.Collections.Generic.List[object]]::new()
    $prepareToDrawStart = [System.Collections.Generic.List[object]]::new()
    $drawSubmit = [System.Collections.Generic.List[object]]::new()
    $drawAckToPresent = [System.Collections.Generic.List[object]]::new()
    foreach ($key in $commitTimes.Keys) {
        $commitTime = [uint64]$commitTimes[$key]
        $frameKey = $key.Split('|')[-1]
        if ($readyTimes.ContainsKey($key)) {
            $readyToCommit.Add([uint64]([decimal]$commitTime - [decimal]$readyTimes[$key]))
            if ($publishedTimes.ContainsKey($key)) {
                $readyToPublish.Add([uint64]([decimal]$publishedTimes[$key] - [decimal]$readyTimes[$key]))
            }
            if ($drawStartTimes.ContainsKey($frameKey) -and
                [decimal]$drawStartTimes[$frameKey] -ge [decimal]$readyTimes[$key]) {
                $prepareToDrawStart.Add([uint64]([decimal]$drawStartTimes[$frameKey] -
                                                  [decimal]$readyTimes[$key]))
            }
        }
        if ($publishedTimes.ContainsKey($key)) {
            $publishToCommit.Add([uint64]([decimal]$commitTime - [decimal]$publishedTimes[$key]))
            if ($ackTimes.ContainsKey($key)) {
                $publishToAck.Add([uint64]([decimal]$ackTimes[$key] - [decimal]$publishedTimes[$key]))
            }
        }
        if ($drawStartTimes.ContainsKey($frameKey) -and $drawAckTimes.ContainsKey($frameKey) -and
            [decimal]$drawAckTimes[$frameKey] -ge [decimal]$drawStartTimes[$frameKey]) {
            $drawSubmit.Add([uint64]([decimal]$drawAckTimes[$frameKey] - [decimal]$drawStartTimes[$frameKey]))
        }
        if ($drawAckTimes.ContainsKey($frameKey) -and $ackTimes.ContainsKey($key) -and
            [decimal]$ackTimes[$key] -ge [decimal]$drawAckTimes[$frameKey]) {
            $drawAckToPresent.Add([uint64]([decimal]$ackTimes[$key] - [decimal]$drawAckTimes[$frameKey]))
        }
        if ($ackTimes.ContainsKey($key)) {
            $ackToCommit.Add([uint64]([decimal]$commitTime - [decimal]$ackTimes[$key]))
        }
    }

    $displayIntervals = [System.Collections.Generic.List[object]]::new()
    if ($commitOrder.Count -ge 2) {
        $ordered = @($commitOrder | Sort-Object -Property Time)
        for ($index = 1; $index -lt $ordered.Count; ++$index) {
            $displayIntervals.Add([uint64]([decimal]$ordered[$index].Time -
                                           [decimal]$ordered[$index - 1].Time))
        }
    }

    $commitRatePerSecond = $null
    if ($commitOrder.Count -ge 2) {
        $ordered = @($commitOrder | Sort-Object -Property Time)
        $spanMicroseconds = [decimal]$ordered[$ordered.Count - 1].Time -
            [decimal]$ordered[0].Time
        if ($spanMicroseconds -gt 0) {
            $commitRatePerSecond = [double]($ordered.Count * 1000000 / $spanMicroseconds)
        }
    }

    return [pscustomobject]@{
        EventCount = $eventCount
        FrameSetReadyCount = $readyTimes.Count
        RenderPublishedCount = $publishedTimes.Count
        RenderDrawStartedCount = $drawStartTimes.Count
        RenderAckPublishedCount = $drawAckTimes.Count
        PresentationAcknowledgedCount = $ackTimes.Count
        SnapshotCommittedCount = $commitTimes.Count
        DisplayIntervalMicroseconds = & $percentileSummary $displayIntervals
        ReadyToCommitMicroseconds = & $percentileSummary $readyToCommit
        PublishToCommitMicroseconds = & $percentileSummary $publishToCommit
        ReadyToPublishMicroseconds = & $percentileSummary $readyToPublish
        PublishToAckMicroseconds = & $percentileSummary $publishToAck
        AckToCommitMicroseconds = & $percentileSummary $ackToCommit
        PrepareToDrawStartMicroseconds = & $percentileSummary $prepareToDrawStart
        DrawSubmitMicroseconds = & $percentileSummary $drawSubmit
        DrawAckToPresentMicroseconds = & $percentileSummary $drawAckToPresent
        CommitRatePerSecond = $commitRatePerSecond
    }
}
Export-ModuleMember -Function Invoke-PlaybackTraceGate, Test-PlaybackTraceFile,
Test-PlaybackTraceInvariants, Get-PlaybackTraceTimingSummary,
ConvertTo-WindowsCommandLineArgument
