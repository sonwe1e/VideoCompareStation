#requires -Version 7.0
<#
.SYNOPSIS
    Interleaved A/B measurement for the T5 timeline-thumbnail gate: alternates one run of an
    ungated build and one run of a gated build on the same media, machine, display and duration.

.DESCRIPTION
    The playback evidence on the reference machine drifts by more than the effect under test: two
    batches taken minutes apart differ by tens of milliseconds in the display-interval tail even
    with an identical executable. Batching "all before, then all after" therefore cannot separate
    the change from the drift.

    This harness removes that confound by interleaving the two executables round-robin
    (A, B, A, B, ...) and then reporting, for every round, the playback-window display interval
    distribution of each build. Because both builds are measured inside the same time window, a
    consistent per-round difference is attributable to the change and a random one is not.

    Every run saves the raw stdout/stderr, the parsed metrics JSON and the trace. The
    playback-window statistics are computed from the trace by the `PlaybackRunStarted` (kind 18) /
    `PlaybackRunStopped` (kind 19) boundary events, so the open/seek/step phases of the same run
    are excluded.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$UngatedExecutable,
    [Parameter(Mandatory = $true)][string]$GatedExecutable,
    [string]$VideoRunPath = 'D:\Videos\20BFE9~1.MP4',
    [string]$Video = 'D:\Videos\2026-06-01 23-46-34.mp4',
    [string]$EvidenceRoot = 'G:\Workspaces\Toy\out\t5-evidence',
    [string]$Label = 'gate-ab-interleaved',
    [int]$Rounds = 4,
    [int]$Seconds = 8,
    [int]$StallThresholdMs = 40
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

foreach ($executable in @($UngatedExecutable, $GatedExecutable)) {
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "Executable not found at $executable"
    }
}
if (-not (Test-Path -LiteralPath $VideoRunPath -PathType Leaf)) {
    throw "Video not found at $VideoRunPath"
}

# A CompareStation.exe outside its deployment directory cannot load Qt/FFmpeg and dies with
# STATUS_DLL_NOT_FOUND before producing any output. Stage both variants next to the deployment
# they were built against and run them from there.
$deploymentRoot = Split-Path -Parent $UngatedExecutable
if (-not (Test-Path -LiteralPath (Join-Path $deploymentRoot 'Qt6Core.dll') -PathType Leaf)) {
    $deploymentRoot = 'G:\Workspaces\Toy\out\build\release\bin'
    Write-Output "T5_GATE_AB_STAGING staged=$deploymentRoot"
}
$ungatedStaged = Join-Path $deploymentRoot 'CompareStation-t5-ab-ungated.exe'
$gatedStaged = Join-Path $deploymentRoot 'CompareStation-t5-ab-gated.exe'
Copy-Item -LiteralPath $UngatedExecutable -Destination $ungatedStaged -Force
Copy-Item -LiteralPath $GatedExecutable -Destination $gatedStaged -Force
if ((Get-FileHash -LiteralPath $ungatedStaged).Hash -eq (Get-FileHash -LiteralPath $gatedStaged).Hash) {
    throw 'The two A/B executables are identical; the gate is not compiled into one of them.'
}
$UngatedExecutable = $ungatedStaged
$GatedExecutable = $gatedStaged

$runDir = Join-Path $EvidenceRoot $Label
if (Test-Path -LiteralPath $runDir) { Remove-Item -LiteralPath $runDir -Recurse -Force }
[void][System.IO.Directory]::CreateDirectory($runDir)

function Get-FileSha256 {
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

# Computes the playback-window display interval distribution from one trace, using the explicit
# run boundaries rather than inferring them from the frame sequence.
function Get-PlaybackWindowStatistics {
    param([string]$TracePath)

    $eventKinds = @{}
    $boundaries = [System.Collections.Generic.List[object]]::new()
    $commits = [System.Collections.Generic.List[object]]::new()
    $grabs = [System.Collections.Generic.List[object]]::new()
    foreach ($line in (Get-Content -LiteralPath $TracePath | Select-Object -Skip 1)) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $record = $line | ConvertFrom-Json -ErrorAction Stop
        if ($null -ne $record.PSObject.Properties['overflow']) {
            throw "Trace $TracePath contains an overflow marker; capture is incomplete."
        }
        $kind = [int]$record.kind
        if (-not $eventKinds.ContainsKey($kind)) { $eventKinds[$kind] = 0 }
        $eventKinds[$kind] = $eventKinds[$kind] + 1
        $timestamp = [decimal][uint64]$record.t
        if ($kind -eq 18 -or $kind -eq 19) {
            $boundaries.Add([pscustomobject]@{ Kind = $kind; T = $timestamp; P = [uint64]$record.p })
        } elseif ($kind -eq 8 -and [uint64]$record.p -ne [uint64]::MaxValue) {
            $commits.Add([pscustomobject]@{ T = $timestamp; P = [int64]$record.p })
        } elseif ($kind -eq 14) {
            $grabs.Add([pscustomobject]@{ T = $timestamp; P = [int64]$record.p })
        }
    }

    $ordered = @($boundaries | Sort-Object T)
    $result = [ordered]@{
        HasBoundaries   = $ordered.Count -ge 2
        WindowSeconds   = $null
        Commits         = 0
        FramesPerSecond = $null
        P50Ms           = $null
        P95Ms           = $null
        P99Ms           = $null
        MaxMs           = $null
        StallsOverThreshold = $null
        StallsPerSecond = $null
        GrabRequestsTotal = $grabs.Count
        GrabRequestsInWindow = 0
        GrabRequestsGatedOff = 0
    }
    if (-not $result.HasBoundaries) {
        return [pscustomobject]$result
    }

    $start = $ordered[0].T
    $stop = $ordered[$ordered.Count - 1].T
    if ($ordered[0].Kind -ne 18) { throw "Trace $TracePath does not start with a playback run boundary." }
    $windowSeconds = [double](($stop - $start) / 1000000)
    $result.WindowSeconds = [math]::Round($windowSeconds, 2)

    $inWindow = [System.Collections.Generic.List[object]]::new()
    foreach ($commit in $commits) {
        if ($commit.T -ge $start -and $commit.T -le $stop) { $inWindow.Add($commit) }
    }
    $orderedCommits = @($inWindow | Sort-Object T)
    $result.Commits = $orderedCommits.Count
    if ($windowSeconds -gt 0) {
        $result.FramesPerSecond = [math]::Round($orderedCommits.Count / $windowSeconds, 2)
    }
    $intervals = [System.Collections.Generic.List[double]]::new()
    for ($index = 1; $index -lt $orderedCommits.Count; $index++) {
        $intervals.Add([double](($orderedCommits[$index].T - $orderedCommits[$index - 1].T) / 1000))
    }
    if ($intervals.Count -gt 0) {
        $sorted = @($intervals | Sort-Object)
        $result.P50Ms = [math]::Round($sorted[[int][math]::Floor($sorted.Count * 0.50)], 2)
        $result.P95Ms = [math]::Round($sorted[[int][math]::Floor($sorted.Count * 0.95)], 2)
        $result.P99Ms = [math]::Round($sorted[[int][math]::Floor($sorted.Count * 0.99)], 2)
        $result.MaxMs = [math]::Round($sorted[$sorted.Count - 1], 2)
        $stalls = @($intervals | Where-Object { $_ -gt $StallThresholdMs })
        $result.StallsOverThreshold = $stalls.Count
        $result.StallsPerSecond = [math]::Round($stalls.Count / $windowSeconds, 2)
    }
    $inside = 0
    $gatedOff = 0
    foreach ($grab in $grabs) {
        if ($grab.T -ge $start -and $grab.T -le $stop) {
            $inside += 1
            if ($grab.P -ge 1073741824) { $gatedOff += 1 }
        }
    }
    $result.GrabRequestsInWindow = $inside
    $result.GrabRequestsGatedOff = $gatedOff
    return [pscustomobject]$result
}
$runLog = [System.Collections.Generic.List[object]]::new()

function Invoke-GateRun {
    param(
        [Parameter(Mandatory = $true)][string]$RunName,
        [Parameter(Mandatory = $true)][string]$Executable
    )
    $runPath = Join-Path $runDir $RunName
    [void][System.IO.Directory]::CreateDirectory($runPath)
    $argsList = @('--ui-performance', $VideoRunPath, '--seconds', "$Seconds", '--mode', 'side')
    [System.IO.File]::WriteAllText(
        (Join-Path $runPath 'command.txt'),
        (($argsList | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '),
        [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText(
        (Join-Path $runPath 'executable.txt'),
        "$Executable`nsha256=$(Get-FileSha256 $Executable)`n",
        [System.Text.UTF8Encoding]::new($false))

    $stdoutFile = Join-Path $runPath 'stdout.txt'
    $stderrFile = Join-Path $runPath 'stderr.txt'
    $tracePath = Join-Path $runPath 'trace.jsonl'
    Remove-Item -LiteralPath $tracePath -ErrorAction SilentlyContinue
    $env:DVS_PLAYBACK_TRACE = $tracePath
    $process = Start-Process -FilePath $Executable -ArgumentList $argsList -NoNewWindow -Wait `
        -PassThru -RedirectStandardOutput $stdoutFile -RedirectStandardError $stderrFile
    $exitCode = $process.ExitCode
    Remove-Item -LiteralPath 'Env:DVS_PLAYBACK_TRACE' -ErrorAction SilentlyContinue

    $metricsLine = Get-Content -LiteralPath $stderrFile -ErrorAction SilentlyContinue |
        Where-Object { $_ -like 'DVS_PERFORMANCE_RESULT*' } | Select-Object -Last 1
    $metrics = $null
    if ($null -ne $metricsLine) {
        $json = $metricsLine.Substring('DVS_PERFORMANCE_RESULT '.Length)
        try {
            $metrics = $json | ConvertFrom-Json
            [System.IO.File]::WriteAllText(
                (Join-Path $runPath 'metrics.json'), $json, [System.Text.UTF8Encoding]::new($false))
        } catch { $metrics = $null }
    }

    $window = $null
    if (Test-Path -LiteralPath $tracePath -PathType Leaf) {
        $window = Get-PlaybackWindowStatistics -TracePath $tracePath
        [System.IO.File]::WriteAllText(
            (Join-Path $runPath 'playback-window.json'),
            ($window | ConvertTo-Json -Depth 4), [System.Text.UTF8Encoding]::new($false))
    }

    $entry = [pscustomobject]@{
        Run                  = $RunName
        ExitCode             = $exitCode
        WindowSeconds        = if ($null -ne $window) { $window.WindowSeconds } else { $null }
        Commits              = if ($null -ne $window) { $window.Commits } else { $null }
        FramesPerSecond      = if ($null -ne $window) { $window.FramesPerSecond } else { $null }
        P50Ms                = if ($null -ne $window) { $window.P50Ms } else { $null }
        P95Ms                = if ($null -ne $window) { $window.P95Ms } else { $null }
        P99Ms                = if ($null -ne $window) { $window.P99Ms } else { $null }
        MaxMs                = if ($null -ne $window) { $window.MaxMs } else { $null }
        StallsOverThreshold  = if ($null -ne $window) { $window.StallsOverThreshold } else { $null }
        StallsPerSecond      = if ($null -ne $window) { $window.StallsPerSecond } else { $null }
        GrabsInWindow        = if ($null -ne $window) { $window.GrabRequestsInWindow } else { $null }
        GrabsWhileGateClosed = if ($null -ne $window) { $window.GrabRequestsGatedOff } else { $null }
        GrabsTotal           = if ($null -ne $window) { $window.GrabRequestsTotal } else { $null }
        DroppedFrames        = if ($null -ne $metrics) { $metrics.dropped_frames } else { $null }
        DropRatio            = if ($null -ne $metrics) { $metrics.drop_ratio } else { $null }
        SeekP95Ms            = if ($null -ne $metrics) { $metrics.seek_p95_ms } else { $null }
        WarmStepP95Ms        = if ($null -ne $metrics) { $metrics.warm_step_p95_ms } else { $null }
        UiLoopGapMaxMs       = if ($null -ne $metrics) { $metrics.ui_loop_gap_max_ms } else { $null }
    }
    $script:runLog.Add($entry)
    return $entry
}

for ($round = 1; $round -le $Rounds; ++$round) {
    [void](Invoke-GateRun -RunName ("r{0}-ungated" -f $round) -Executable $UngatedExecutable)
    [void](Invoke-GateRun -RunName ("r{0}-gated" -f $round) -Executable $GatedExecutable)
}

$summary = [System.Collections.Generic.List[string]]::new()
[void]$summary.Add("# T5 interleaved gate A/B $((Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))")
[void]$summary.Add('')
[void]$summary.Add("ungated: $UngatedExecutable (sha256 $(Get-FileSha256 $UngatedExecutable))")
[void]$summary.Add("gated  : $GatedExecutable (sha256 $(Get-FileSha256 $GatedExecutable))")
[void]$summary.Add("video  : $Video")
[void]$summary.Add("rounds : $Rounds, seconds per run: $Seconds, stall threshold: $StallThresholdMs ms")
[void]$summary.Add('')
[void]$summary.Add('| run | window s | commits | frames/s | p50 | p95 | p99 | max | stalls | stalls/s | grabs in window (gate closed) |')
[void]$summary.Add('|---|---|---|---|---|---|---|---|---|---|---|')
foreach ($entry in $runLog) {
    [void]$summary.Add(
        "| $($entry.Run) | $($entry.WindowSeconds) | $($entry.Commits) | $($entry.FramesPerSecond) | " +
        "$($entry.P50Ms) | $($entry.P95Ms) | $($entry.P99Ms) | $($entry.MaxMs) | " +
        "$($entry.StallsOverThreshold) | $($entry.StallsPerSecond) | " +
        "$($entry.GrabsInWindow) ($($entry.GrabsWhileGateClosed)) |")
}
[void]$summary.Add('')
[void]$summary.Add('## Per-round paired comparison')
[void]$summary.Add('')
[void]$summary.Add('| round | ungated frames/s | gated frames/s | delta | ungated p95 | gated p95 | delta | ungated stalls | gated stalls |')
[void]$summary.Add('|---|---|---|---|---|---|---|---|---|')
for ($round = 1; $round -le $Rounds; ++$round) {
    $u = $runLog | Where-Object { $_.Run -eq ("r{0}-ungated" -f $round) } | Select-Object -First 1
    $g = $runLog | Where-Object { $_.Run -eq ("r{0}-gated" -f $round) } | Select-Object -First 1
    if ($null -eq $u -or $null -eq $g) { continue }
    [void]$summary.Add(
        "| $round | $($u.FramesPerSecond) | $($g.FramesPerSecond) | " +
        "$([math]::Round([double]$g.FramesPerSecond - [double]$u.FramesPerSecond, 2)) | " +
        "$($u.P95Ms) | $($g.P95Ms) | $([math]::Round([double]$g.P95Ms - [double]$u.P95Ms, 2)) | " +
        "$($u.StallsOverThreshold) | $($g.StallsOverThreshold) |")
}
[void]$summary.Add('')
[void]$summary.Add('## Correctness gates')
[void]$summary.Add('')
foreach ($entry in $runLog) {
    [void]$summary.Add(("{0,-12} drop={1} seek_p95={2} warm_step_p95={3} ui_max={4}" -f `
                $entry.Run, $entry.DropRatio, $entry.SeekP95Ms, $entry.WarmStepP95Ms, $entry.UiLoopGapMaxMs))
}
[System.IO.File]::WriteAllText(
    (Join-Path $runDir 'summary.md'), (($summary -join "`n") + "`n"),
    [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText(
    (Join-Path $runDir 'runs.json'), (@($runLog) | ConvertTo-Json -Depth 5),
    [System.Text.UTF8Encoding]::new($false))

Write-Output "T5_GATE_AB_OK runDir=$runDir runs=$($runLog.Count)"
