#requires -Version 7.0
<#
.SYNOPSIS
    Measures CompareStation startup with the fixed 2.0 protocol: external window-appearance
    timing (Start-Process plus 1 ms polling) and the internal DVS_STARTUP_TIMING milestone
    log, aggregated as median and P95 per segment.

.DESCRIPTION
    One scenario = Warmup + Rounds launches of the same executable with the same arguments;
    the warmup launch is discarded. DVS_STARTUP_TIMING is set only for the child process.
    Each launch captures:

    - external: spawn -> first visible main window, polled at 1 ms via MainWindowHandle.
      The spawn timestamp is taken immediately before Start-Process, so external times
      include the cmdlet's launch overhead (constant across rounds; compare deltas, not
      absolute values, against other harnesses).
    - aligned: spawn -> first milestone and spawn -> the exec milestone. Every milestone
      prints its absolute steady-clock timestamp; MSVC steady_clock is QPC, the same
      clock as System.Diagnostics.Stopwatch, so the log and the poll loop can be joined.
      An alignment outside the sanity window is reported as null instead of trusted.
    - internal: every [startup] milestone with the delta to its predecessor.
    - first frame: when a launch argument opens media, DVS_PLAYBACK_TRACE captures the
      playback trace; the first trace events of kind 0 (CommandAccepted), 4 (FrameSetReady)
      and 8 (SnapshotCommitted) are reported as spawn-aligned times. The trace clock is the
      same steady clock as the milestone log, so the three layers join without offsets.

    Percentiles are nearest-rank: with 5 measured rounds P95 equals the maximum. The
    sequential launch loop measures the warm-cache path; record that alongside results.
    Raw stderr, a parsed JSON bundle and a Markdown summary are written to
    out/startup-measurements/<label>-<timestamp>. Numbers are only comparable when they
    come from this script on the same machine, build and material.
#>
[CmdletBinding()]
param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\..\out\build\release\bin\CompareStation.exe'),
    [string[]]$LaunchArgument = @(),
    [string]$Label = 'nofile',
    [int]$Rounds = 5,
    [int]$Warmup = 1,
    [int]$WindowTimeoutSeconds = 30,
    [int]$PostWindowGraceMs = 300,
    [string]$OutputRoot = (Join-Path $PSScriptRoot '..\..\out\startup-measurements')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $IsWindows) {
    throw 'Startup measurement requires Windows (window timing via MainWindowHandle).'
}
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "Executable not found: $Executable"
}
$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$processName = [IO.Path]::GetFileNameWithoutExtension($resolvedExecutable)
$alreadyRunning = @(Get-Process -Name $processName -ErrorAction SilentlyContinue)
if ($alreadyRunning.Count -gt 0) {
    throw ("A '$processName' process is already running; the startup broker would forward " +
        'the launch and the measurement would not observe this process at all.')
}

$runStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runDir = Join-Path $OutputRoot "$Label-$runStamp"
[void][IO.Directory]::CreateDirectory($runDir)

$qpcFrequency = [double][Diagnostics.Stopwatch]::Frequency
$qpcSanityMaxUs = 30.0 * 1000000.0

function ConvertTo-MicrosecondsSinceSpawn {
    param([long]$SpawnQpc, [long]$SampleQpc)
    return [math]::Floor((($SampleQpc - $SpawnQpc) * 1000000.0) / $qpcFrequency)
}

function Get-StartupMedian {
    param([double[]]$Values)
    $sorted = @($Values | Sort-Object)
    $count = $sorted.Count
    if ($count -eq 0) { return $null }
    if ($count % 2 -eq 1) { return [double]$sorted[[int](($count - 1) / 2)] }
    return ([double]$sorted[[int]($count / 2 - 1)] + [double]$sorted[[int]($count / 2)]) / 2.0
}

function Get-StartupPercentile {
    # Nearest-rank percentile: with 5 measured rounds P95 is the maximum observation.
    param([double[]]$Values, [double]$Fraction)
    $sorted = @($Values | Sort-Object)
    $count = $sorted.Count
    if ($count -eq 0) { return $null }
    $rank = [int][math]::Ceiling($Fraction * $count)
    if ($rank -lt 1) { $rank = 1 }
    if ($rank -gt $count) { $rank = $count }
    return [double]$sorted[$rank - 1]
}

function Get-StartupAggregate {
    param([double[]]$Values)
    $values = @($Values)
    if ($values.Count -eq 0) { return $null }
    return [pscustomobject]@{
        n = $values.Count
        median_us = Get-StartupMedian $values
        p95_us = Get-StartupPercentile $values 0.95
        min_us = [double]($values | Measure-Object -Minimum).Minimum
        max_us = [double]($values | Measure-Object -Maximum).Maximum
    }
}

function Get-StartupEnvironment {
    $repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
    $gitCommit = 'unknown'
    $gitDirtyEntries = $null
    try {
        $gitCommit = [string](& git -C $repositoryRoot rev-parse HEAD | Select-Object -First 1)
        $gitDirtyEntries = @(& git -C $repositoryRoot status --porcelain).Count
    } catch {
        # git metadata is informative, not load-bearing; keep measuring.
    }
    $videoControllers = @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue |
        ForEach-Object {
            [pscustomobject]@{
                name = $_.Name
                driver_version = $_.DriverVersion
                current_refresh_hz = $_.CurrentRefreshRate
                video_mode = $_.VideoModeDescription
            }
        })
    $operatingSystem = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue
    $processor = Get-CimInstance Win32_Processor -ErrorAction SilentlyContinue | Select-Object -First 1
    return [pscustomobject]@{
        timestamp_utc = (Get-Date).ToUniversalTime().ToString('o')
        label = $Label
        executable = $resolvedExecutable
        executable_sha256 = (Get-FileHash -LiteralPath $resolvedExecutable -Algorithm SHA256).Hash.ToLowerInvariant()
        executable_last_write_utc = ([IO.File]::GetLastWriteTimeUtc($resolvedExecutable)).ToString('o')
        launch_argument = @($LaunchArgument)
        rounds = $Rounds
        warmup = $Warmup
        cache_state = 'warm (sequential launch loop on the same machine)'
        git_commit = $gitCommit
        git_dirty_entries = $gitDirtyEntries
        os = if ($null -ne $operatingSystem) { "$($operatingSystem.Caption) $($operatingSystem.Version)" } else { 'unknown' }
        cpu = if ($null -ne $processor) { $processor.Name } else { 'unknown' }
        gpu = $videoControllers
    }
}

$argumentText = (@($LaunchArgument) | ForEach-Object {
    if ($_ -match '\s') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
}) -join ' '

function Invoke-StartupLaunch {
    param([int]$Index)
    $stderrPath = Join-Path $runDir ('round-{0:d2}.stderr.txt' -f $Index)
    $tracePath = Join-Path $runDir ('round-{0:d2}.trace.jsonl' -f $Index)
    $previousTiming = $env:DVS_STARTUP_TIMING
    $previousTrace = $env:DVS_PLAYBACK_TRACE
    $env:DVS_STARTUP_TIMING = '1'
    $env:DVS_PLAYBACK_TRACE = $tracePath
    $process = $null
    $windowUs = $null
    $timedOut = $false
    $exitCode = $null
    $stderrText = ''
    try {
        $spawnQpc = [Diagnostics.Stopwatch]::GetTimestamp()
        $process = Start-Process -FilePath $resolvedExecutable -ArgumentList $argumentText `
            -RedirectStandardError $stderrPath -PassThru
        $deadline = [DateTime]::UtcNow.AddSeconds($WindowTimeoutSeconds)
        while (-not $process.HasExited) {
            $process.Refresh()
            if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
                $windowUs = ConvertTo-MicrosecondsSinceSpawn $spawnQpc ([Diagnostics.Stopwatch]::GetTimestamp())
                break
            }
            if ([DateTime]::UtcNow -gt $deadline) {
                $timedOut = $true
                break
            }
            [Threading.Thread]::Sleep(1)
        }
        # Let the final milestone (exec) flush, then close gracefully so Start-Process's
        # stderr pump drains to EOF instead of losing the tail on a hard kill.
        [Threading.Thread]::Sleep($PostWindowGraceMs)
        if (-not $process.HasExited) {
            [void]$process.CloseMainWindow()
            if (-not $process.WaitForExit(10000)) {
                $process.Kill($true)
                [void]$process.WaitForExit(5000)
            }
        }
        $exitCode = $process.ExitCode
        # Give the redirected stderr file time to receive everything the pump drained.
        [Threading.Thread]::Sleep(500)
        for ($attempt = 0; $attempt -lt 5 -and $stderrText -eq ''; $attempt++) {
            try {
                $stderrText = [IO.File]::ReadAllText($stderrPath)
            } catch {
                [Threading.Thread]::Sleep(200)
            }
        }
        $process.Dispose()
        $process = $null
    } finally {
        if ($null -ne $process) {
            try {
                if (-not $process.HasExited) { $process.Kill($true) }
            } catch {
                # The process is already gone; nothing to clean up.
            }
        }
        if ($null -ne $previousTiming) {
            $env:DVS_STARTUP_TIMING = $previousTiming
        } else {
            Remove-Item Env:DVS_STARTUP_TIMING -ErrorAction SilentlyContinue
        }
        if ($null -ne $previousTrace) {
            $env:DVS_PLAYBACK_TRACE = $previousTrace
        } else {
            Remove-Item Env:DVS_PLAYBACK_TRACE -ErrorAction SilentlyContinue
        }
    }

    $milestones = [System.Collections.Generic.List[object]]::new()
    foreach ($line in ($stderrText -split "`r?`n")) {
        if ($line -notmatch '^\[startup\]\s+(\S+)\s*\+\s*(\d+)\s*us\s+t=(\d+)\s*us') { continue }
        $milestones.Add([pscustomobject]@{
            Name = $Matches[1]
            SinceFirstUs = [double]$Matches[2]
            AbsoluteUs = [double]$Matches[3]
        })
    }

    $segments = [ordered]@{}
    for ($i = 1; $i -lt $milestones.Count; $i++) {
        $key = "$($milestones[$i - 1].Name) -> $($milestones[$i].Name)"
        $segments[$key] = [math]::Round($milestones[$i].SinceFirstUs - $milestones[$i - 1].SinceFirstUs, 3)
    }

    $spawnQpcUs = [math]::Floor(($spawnQpc * 1000000.0) / $qpcFrequency)
    $spawnToFirstUs = $null
    $spawnToExecUs = $null
    if ($milestones.Count -gt 0) {
        $firstDelta = $milestones[0].AbsoluteUs - $spawnQpcUs
        if ($firstDelta -ge 0 -and $firstDelta -le $qpcSanityMaxUs) {
            $spawnToFirstUs = [math]::Round($firstDelta, 3)
        }
        $execMark = $milestones | Where-Object { $_.Name -eq 'exec' } | Select-Object -First 1
        if ($null -ne $execMark) {
            $execDelta = $execMark.AbsoluteUs - $spawnQpcUs
            if ($execDelta -ge 0 -and $execDelta -le $qpcSanityMaxUs) {
                $spawnToExecUs = [math]::Round($execDelta, 3)
            }
        }
    }

    # Playback trace: first event per requested kind, joined onto the spawn timeline via the
    # shared steady clock. Kinds: 0 CommandAccepted, 4 FrameSetReady, 8 SnapshotCommitted.
    $traceFirstByKind = @{}
    if (Test-Path -LiteralPath $tracePath -PathType Leaf) {
        foreach ($line in [IO.File]::ReadLines($tracePath)) {
            if ($line -notmatch '^\{"t":(\d+),"kind":(\d+),') { continue }
            $kind = [int]$Matches[2]
            if ($traceFirstByKind.ContainsKey($kind)) { continue }
            if ($kind -ne 0 -and $kind -ne 4 -and $kind -ne 8) { continue }
            $delta = [double]$Matches[1] - $spawnQpcUs
            if ($delta -ge 0 -and $delta -le $qpcSanityMaxUs) {
                $traceFirstByKind[$kind] = [math]::Round($delta, 3)
            }
        }
    }

    return [pscustomobject]@{
        Index = $Index
        WindowUs = $windowUs
        TimedOut = $timedOut
        ExitCode = $exitCode
        MilestoneCount = $milestones.Count
        MilestoneOrder = @($milestones | ForEach-Object { $_.Name })
        Milestones = @($milestones)
        Segments = $segments
        SpawnToFirstMarkUs = $spawnToFirstUs
        SpawnToExecUs = $spawnToExecUs
        TracePath = $tracePath
        TraceCommandAcceptedUs = if ($traceFirstByKind.ContainsKey(0)) { $traceFirstByKind[0] } else { $null }
        TraceFrameSetReadyUs = if ($traceFirstByKind.ContainsKey(4)) { $traceFirstByKind[4] } else { $null }
        TraceSnapshotCommittedUs = if ($traceFirstByKind.ContainsKey(8)) { $traceFirstByKind[8] } else { $null }
        Valid = ($null -ne $windowUs) -and (-not $timedOut) -and ($milestones.Count -gt 0)
    }
}

$environment = Get-StartupEnvironment
$totalLaunches = $Warmup + $Rounds
$allRounds = [System.Collections.Generic.List[object]]::new()
for ($i = 1; $i -le $totalLaunches; $i++) {
    Write-Host ("[{0}/{1}] launching: {2} {3}" -f $i, $totalLaunches, $resolvedExecutable, $argumentText)
    $round = Invoke-StartupLaunch -Index $i
    $allRounds.Add($round)
    $role = if ($i -le $Warmup) { 'warmup ' } else { 'measured' }
    $windowText = if ($null -ne $round.WindowUs) { ('{0:N1} ms' -f ($round.WindowUs / 1000.0)) } else { 'n/a' }
    Write-Host ("    {0}: window={1}  milestones={2}  valid={3}" -f $role, $windowText, $round.MilestoneCount, $round.Valid)
    # Let the broker pipe and the OS settle so the next launch is a fresh primary instance.
    [Threading.Thread]::Sleep(500)
}

$measured = @($allRounds | Where-Object { $_.Index -gt $Warmup })
$validMeasured = @($measured | Where-Object { $_.Valid })
if ($validMeasured.Count -lt $Rounds) {
    $invalid = @($measured | Where-Object { -not $_.Valid } | ForEach-Object { $_.Index }) -join ', '
    throw ("Only $($validMeasured.Count)/$Rounds measured rounds were valid (invalid rounds: $invalid). " +
        "Raw logs kept for diagnosis: $runDir")
}

$orders = @($validMeasured | ForEach-Object { $_.MilestoneOrder -join ' | ' } | Select-Object -Unique)
if ($orders.Count -gt 1) {
    Write-Warning 'Milestone order differs between measured rounds; segment aggregates only cover rounds that contain the segment.'
}

$segmentAggregates = [ordered]@{}
foreach ($key in @($validMeasured | ForEach-Object { $_.Segments.Keys } | Sort-Object -Unique)) {
    $values = @($validMeasured | Where-Object { $_.Segments.Contains($key) } |
        ForEach-Object { [double]$_.Segments[$key] })
    $segmentAggregates[$key] = Get-StartupAggregate $values
}

$milestoneAggregates = [ordered]@{}
foreach ($name in @($validMeasured | ForEach-Object { $_.MilestoneOrder } | Select-Object -Unique)) {
    $values = @($validMeasured | ForEach-Object {
        $mark = $_.Milestones | Where-Object { $_.Name -eq $name } | Select-Object -First 1
        if ($null -ne $mark) { [double]$mark.SinceFirstUs }
    })
    $milestoneAggregates[$name] = Get-StartupAggregate $values
}

$externalWindow = Get-StartupAggregate @($validMeasured | ForEach-Object { [double]$_.WindowUs })
$externalFirstMark = Get-StartupAggregate @($validMeasured |
    Where-Object { $null -ne $_.SpawnToFirstMarkUs } | ForEach-Object { [double]$_.SpawnToFirstMarkUs })
$externalExec = Get-StartupAggregate @($validMeasured |
    Where-Object { $null -ne $_.SpawnToExecUs } | ForEach-Object { [double]$_.SpawnToExecUs })

$traceCommandAccepted = Get-StartupAggregate @($validMeasured |
    Where-Object { $null -ne $_.TraceCommandAcceptedUs } |
    ForEach-Object { [double]$_.TraceCommandAcceptedUs })
$traceFrameSetReady = Get-StartupAggregate @($validMeasured |
    Where-Object { $null -ne $_.TraceFrameSetReadyUs } |
    ForEach-Object { [double]$_.TraceFrameSetReadyUs })
$traceSnapshotCommitted = Get-StartupAggregate @($validMeasured |
    Where-Object { $null -ne $_.TraceSnapshotCommittedUs } |
    ForEach-Object { [double]$_.TraceSnapshotCommittedUs })

$summary = [pscustomobject]@{
    environment = $environment
    external = [pscustomobject]@{
        spawn_to_window = $externalWindow
        spawn_to_first_mark = $externalFirstMark
        spawn_to_exec = $externalExec
    }
    first_frame = [pscustomobject]@{
        spawn_to_command_accepted = $traceCommandAccepted
        spawn_to_frame_set_ready = $traceFrameSetReady
        spawn_to_snapshot_committed = $traceSnapshotCommitted
    }
    segments = $segmentAggregates
    milestones_since_first_mark = $milestoneAggregates
    rounds = $allRounds
    run_dir = $runDir
}

$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $runDir 'summary.json') -Encoding utf8

function Format-StartupRow {
    param([string]$Key, $Aggregate)
    if ($null -eq $Aggregate) { return '' }
    return ('{0,-52} {1,9:N1} {2,9:N1} {3,9:N1} {4,9:N1}' -f $Key,
        ($Aggregate.median_us / 1000.0), ($Aggregate.p95_us / 1000.0),
        ($Aggregate.min_us / 1000.0), ($Aggregate.max_us / 1000.0))
}

$report = [System.Text.StringBuilder]::new()
[void]$report.AppendLine("# Startup measurement: $Label")
[void]$report.AppendLine()
[void]$report.AppendLine("- executable: $($environment.executable)")
[void]$report.AppendLine("- git: $($environment.git_commit) (dirty entries: $($environment.git_dirty_entries))")
[void]$report.AppendLine("- gpu: $(@($environment.gpu | ForEach-Object { $_.name }) -join '; ')")
[void]$report.AppendLine("- launch argument: $(if ($argumentText) { $argumentText } else { '(none)' })")
[void]$report.AppendLine("- rounds: $Rounds measured + $Warmup warmup (discarded); cache: $($environment.cache_state)")
[void]$report.AppendLine()
[void]$report.AppendLine('All times in milliseconds. P95 is nearest-rank (the maximum of 5 rounds).')
[void]$report.AppendLine()
[void]$report.AppendLine('## External')
[void]$report.AppendLine()
[void]$report.AppendLine('```')
[void]$report.AppendLine((Format-StartupRow 'spawn -> window (polled 1 ms)' $externalWindow))
[void]$report.AppendLine((Format-StartupRow 'spawn -> first mark (QPC aligned)' $externalFirstMark))
[void]$report.AppendLine((Format-StartupRow 'spawn -> exec mark (QPC aligned)' $externalExec))
[void]$report.AppendLine('```')
[void]$report.AppendLine()
if ($null -ne $traceSnapshotCommitted -or $null -ne $traceFrameSetReady -or
    $null -ne $traceCommandAccepted) {
    [void]$report.AppendLine('## First frame (playback trace)')
    [void]$report.AppendLine()
    [void]$report.AppendLine('```')
    [void]$report.AppendLine((Format-StartupRow 'spawn -> command accepted (kind 0)' $traceCommandAccepted))
    [void]$report.AppendLine((Format-StartupRow 'spawn -> first frame set ready (kind 4)' $traceFrameSetReady))
    [void]$report.AppendLine((Format-StartupRow 'spawn -> first snapshot committed (kind 8)' $traceSnapshotCommitted))
    [void]$report.AppendLine('```')
    [void]$report.AppendLine()
}
[void]$report.AppendLine('## Internal segments')
[void]$report.AppendLine()
[void]$report.AppendLine('```')
[void]$report.AppendLine(('{0,-52} {1,9} {2,9} {3,9} {4,9}' -f 'segment', 'median', 'p95', 'min', 'max'))
foreach ($key in $segmentAggregates.Keys) {
    [void]$report.AppendLine((Format-StartupRow $key $segmentAggregates[$key]))
}
[void]$report.AppendLine('```')
[void]$report.AppendLine()
[void]$report.AppendLine('## Milestones since first mark')
[void]$report.AppendLine()
[void]$report.AppendLine('```')
foreach ($name in $milestoneAggregates.Keys) {
    [void]$report.AppendLine((Format-StartupRow $name $milestoneAggregates[$name]))
}
[void]$report.AppendLine('```')
[void]$report.AppendLine()
[void]$report.AppendLine("Raw per-round stderr, playback traces and JSON: $runDir")

$reportPath = Join-Path $runDir 'summary.md'
$report.ToString() | Set-Content -LiteralPath $reportPath -Encoding utf8
Write-Host ''
Write-Host $report.ToString()
Write-Host "summary: $reportPath"
