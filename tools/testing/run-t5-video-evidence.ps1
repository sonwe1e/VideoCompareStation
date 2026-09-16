#requires -Version 7.0
<#
.SYNOPSIS
    Runs the T5 conditional work order evidence: the playback performance probe on a real
    user-supplied video (default D:\Videos\2026-06-01 23-46-34.mp4) plus a same-machine
    fixture reference, and archives raw output, metrics JSON, traces and timing summaries.

.DESCRIPTION
    T5 ("video smooth playback, evidence-located hotspot fix") may only change playback
    scheduling/caching parameters after T0-style causal evidence points at a hotspot. This
    script produces that evidence with the app's existing gates and does not change playback
    behavior itself.

    Every run saves: command line, raw stdout/stderr, parsed metrics JSON, the trace (when
    traced) and its timing summary. The bundle carries a summary.md with the review-relevant
    columns (display interval P50/P95/P99 + longest pause, UI loop gap, drop ratio, seek P95)
    and an environment.txt with git SHA, build type, exe hash, media hashes and device/display.
#>
[CmdletBinding()]
param(
    [string]$Executable = 'G:\Workspaces\Toy\out\build\release\bin\VCStation.exe',
    [string]$Video = 'D:\Videos\2026-06-01 23-46-34.mp4',
    # The performance entry's argument parser does not honour the quotes Start-Process writes
    # around an argument containing spaces, so a spaced source path is split into extra sources
    # and the run fails as a media error. The 8.3 short name identifies the same file without
    # spaces; VideoRunPath overrides it when the volume has 8.3 generation disabled.
    [string]$VideoRunPath = 'D:\Videos\20BFE9~1.MP4',
    [string]$FixtureRoot = 'G:\Workspaces\Toy\out\evidence-fixtures',
    [string]$EvidenceRoot = 'G:\Workspaces\Toy\out\t5-evidence',
    [string]$Label = 'baseline',
    [int]$Seconds = 8,
    [int]$Repeats = 3,
    [switch]$SkipFixture,
    [switch]$RenderLoopContrast
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "Executable not found at $Executable"
}
if (-not (Test-Path -LiteralPath $Video -PathType Leaf)) {
    throw "Video not found at $Video"
}
if (-not (Test-Path -LiteralPath $VideoRunPath -PathType Leaf)) {
    throw "Video run path not found at $VideoRunPath"
}

Import-Module -Name (Join-Path $PSScriptRoot 'PlaybackTraceGate.psm1') -Force

$runDir = Join-Path $EvidenceRoot $Label
if (Test-Path -LiteralPath $runDir) {
    Remove-Item -LiteralPath $runDir -Recurse -Force
}
[void][System.IO.Directory]::CreateDirectory($runDir)

function Get-FileSha256 {
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-MetricField {
    param($Object, [string]$Name)
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

# --- environment ---------------------------------------------------------------
$gitSha = (& git -C 'G:\Workspaces\Toy' rev-parse HEAD 2>&1 | Select-Object -First 1)
$gitBranch = (& git -C 'G:\Workspaces\Toy' branch --show-current 2>&1 | Select-Object -First 1)
$gpuLines = @(Get-CimInstance Win32_VideoController | ForEach-Object {
        "$($_.Name) | $($_.CurrentHorizontalResolution)x$($_.CurrentVerticalResolution) @ " +
        "$($_.CurrentRefreshRate)Hz | $($_.DriverVersion)"
    })
$cpuName = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
$environmentLines = @(
    "collected_utc: $([DateTime]::UtcNow.ToString('o'))",
    "git_sha: $gitSha ($gitBranch)",
    "executable: $Executable",
    "executable_sha256: $(Get-FileSha256 $Executable)",
    "video: $Video",
    "video_run_path: $VideoRunPath",
    "video_sha256: $(Get-FileSha256 $Video)",
    "video_bytes: $((Get-Item -LiteralPath $Video).Length)",
    "cpu: $cpuName",
    "os: $([System.Environment]::OSVersion.VersionString)"
) + ($gpuLines | ForEach-Object { "gpu: $_" })
[System.IO.File]::WriteAllText(
    (Join-Path $runDir 'environment.txt'),
    (($environmentLines -join "`n") + "`n"),
    [System.Text.UTF8Encoding]::new($false)
)

$runLog = [System.Collections.Generic.List[object]]::new()

function Invoke-T5Run {
    param(
        [Parameter(Mandatory = $true)][string]$RunName,
        [Parameter(Mandatory = $true)][string[]]$Sources,
        [Parameter(Mandatory = $true)][int]$DurationSeconds,
        [string]$Mode = 'side',
        [hashtable]$ExtraEnvironment = @{},
        [switch]$WithTrace
    )
    $runPath = Join-Path $runDir $RunName
    [void][System.IO.Directory]::CreateDirectory($runPath)
    $argsList = @('--ui-performance') + @($Sources) + @('--seconds', "$DurationSeconds", '--mode', $Mode)
    $commandLine = ($argsList | ForEach-Object {
            if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
        }) -join ' '
    [System.IO.File]::WriteAllText(
        (Join-Path $runPath 'command.txt'), $commandLine, [System.Text.UTF8Encoding]::new($false))

    $stdoutFile = Join-Path $runPath 'stdout.txt'
    $stderrFile = Join-Path $runPath 'stderr.txt'
    $tracePath = Join-Path $runPath 'trace.jsonl'
    $hadTrace = Test-Path -LiteralPath 'Env:DVS_PLAYBACK_TRACE'
    $previousTrace = if ($hadTrace) { $env:DVS_PLAYBACK_TRACE } else { $null }
    # Controlled-experiment overrides. Each saved run records what it changed so a comparison can
    # never attribute a difference to an unrecorded environment.
    $restore = @{}
    foreach ($name in $ExtraEnvironment.Keys) {
        $exists = Test-Path -LiteralPath "Env:$name"
        $restore[$name] = if ($exists) { $env.$name } else { $null }
        Set-Item -LiteralPath "Env:$name" -Value $ExtraEnvironment[$name]
    }
    if ($ExtraEnvironment.Count -gt 0) {
        [System.IO.File]::WriteAllText(
            (Join-Path $runPath 'environment-overrides.txt'),
            (($ExtraEnvironment.GetEnumerator() | Sort-Object Name |
                    ForEach-Object { "$($_.Name)=$($_.Value)" }) -join "`n") + "`n",
            [System.Text.UTF8Encoding]::new($false))
    }
    if ($WithTrace) {
        Remove-Item -LiteralPath $tracePath -ErrorAction SilentlyContinue
        $env:DVS_PLAYBACK_TRACE = $tracePath
    } else {
        Remove-Item -LiteralPath 'Env:DVS_PLAYBACK_TRACE' -ErrorAction SilentlyContinue
    }

    $process = Start-Process -FilePath $Executable -ArgumentList $argsList -NoNewWindow -Wait `
        -PassThru -RedirectStandardOutput $stdoutFile -RedirectStandardError $stderrFile
    $exitCode = $process.ExitCode

    if ($WithTrace) {
        if ($hadTrace) { $env:DVS_PLAYBACK_TRACE = $previousTrace }
        else { Remove-Item -LiteralPath 'Env:DVS_PLAYBACK_TRACE' -ErrorAction SilentlyContinue }
    }
    foreach ($name in $restore.Keys) {
        if ($null -eq $restore[$name]) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
        else { Set-Item -LiteralPath "Env:$name" -Value $restore[$name] }
    }

    $metrics = $null
    $resultLine = Get-Content -LiteralPath $stderrFile -ErrorAction SilentlyContinue |
        Where-Object { $_ -like 'DVS_PERFORMANCE_RESULT*' } | Select-Object -Last 1
    if ($null -ne $resultLine) {
        $json = $resultLine.Substring('DVS_PERFORMANCE_RESULT '.Length)
        try {
            $metrics = $json | ConvertFrom-Json
            [System.IO.File]::WriteAllText(
                (Join-Path $runPath 'metrics.json'), $json, [System.Text.UTF8Encoding]::new($false))
        } catch {
            $metrics = $null
        }
    }

    $timing = $null
    if ($WithTrace -and (Test-Path -LiteralPath $tracePath -PathType Leaf)) {
        try {
            $timing = Get-PlaybackTraceTimingSummary -TracePath $tracePath
            [System.IO.File]::WriteAllText(
                (Join-Path $runPath 'trace-timing.json'),
                ($timing | ConvertTo-Json -Depth 6), [System.Text.UTF8Encoding]::new($false))
        } catch {
            $timing = $null
            [System.IO.File]::WriteAllText(
                (Join-Path $runPath 'trace-timing-error.txt'),
                $_.Exception.Message, [System.Text.UTF8Encoding]::new($false))
        }
    }

    $entry = [pscustomobject]@{
        run                        = $RunName
        exit_code                  = $exitCode
        seconds                    = $DurationSeconds
        mode                       = $Mode
        sources                    = ($Sources | ForEach-Object { [System.IO.Path]::GetFileName($_) }) -join '+'
        passed                     = Get-MetricField $metrics 'passed'
        failure                    = Get-MetricField $metrics 'failure'
        screen_refresh_hz          = Get-MetricField $metrics 'screen_refresh_hz'
        all_d3d11va                = Get-MetricField $metrics 'all_d3d11va'
        display_interval_p50_ms    = Get-MetricField $metrics 'display_interval_p50_ms'
        display_interval_p95_ms    = Get-MetricField $metrics 'display_interval_p95_ms'
        display_interval_p99_ms    = Get-MetricField $metrics 'display_interval_p99_ms'
        display_interval_max_ms    = Get-MetricField $metrics 'display_interval_max_ms'
        display_interval_count     = Get-MetricField $metrics 'display_interval_count'
        ui_loop_gap_p50_ms         = Get-MetricField $metrics 'ui_loop_gap_p50_ms'
        ui_loop_gap_p95_ms         = Get-MetricField $metrics 'ui_loop_gap_p95_ms'
        ui_loop_gap_p99_ms         = Get-MetricField $metrics 'ui_loop_gap_p99_ms'
        ui_loop_gap_max_ms         = Get-MetricField $metrics 'ui_loop_gap_max_ms'
        seek_p95_ms                = Get-MetricField $metrics 'seek_p95_ms'
        warm_step_p95_ms           = Get-MetricField $metrics 'warm_step_p95_ms'
        presented_frames           = Get-MetricField $metrics 'presented_frames'
        dropped_frames             = Get-MetricField $metrics 'dropped_frames'
        drop_ratio                 = Get-MetricField $metrics 'drop_ratio'
        peak_frame_bytes           = Get-MetricField $metrics 'peak_frame_bytes'
        decoder_cache_hit_ratio    = Get-MetricField $metrics 'decoder_cache_hit_ratio'
        decoder_average_us         = Get-MetricField $metrics 'decoder_average_us'
        decoder_maximum_us         = Get-MetricField $metrics 'decoder_maximum_us'
        transfer_average_us        = Get-MetricField $metrics 'transfer_average_us'
        transfer_maximum_us        = Get-MetricField $metrics 'transfer_maximum_us'
        render_frame_to_ack_avg_us = Get-MetricField $metrics 'render_frame_to_ack_average_us'
        frameset_assembly_avg_us   = Get-MetricField $metrics 'frameset_assembly_average_us'
        commit_rate_per_second     = if ($null -ne $timing) { $timing.CommitRatePerSecond } else { $null }
        trace_display_interval_p95_us =
            if ($null -ne $timing) { $timing.DisplayIntervalMicroseconds.P95 } else { $null }
        trace_display_interval_p99_us =
            if ($null -ne $timing) { $timing.DisplayIntervalMicroseconds.P99 } else { $null }
        trace_display_interval_max_us =
            if ($null -ne $timing) { $timing.DisplayIntervalMicroseconds.Max } else { $null }
        trace_ready_to_commit_p50_us =
            if ($null -ne $timing) { $timing.ReadyToCommitMicroseconds.P50 } else { $null }
        trace_ready_to_publish_p50_us =
            if ($null -ne $timing) { $timing.ReadyToPublishMicroseconds.P50 } else { $null }
        trace_publish_to_ack_p50_us =
            if ($null -ne $timing) { $timing.PublishToAckMicroseconds.P50 } else { $null }
        trace_publish_to_ack_p95_us =
            if ($null -ne $timing) { $timing.PublishToAckMicroseconds.P95 } else { $null }
        trace_ack_to_commit_p50_us =
            if ($null -ne $timing) { $timing.AckToCommitMicroseconds.P50 } else { $null }
        trace_event_count          = if ($null -ne $timing) { $timing.EventCount } else { $null }
        trace_commit_count         = if ($null -ne $timing) { $timing.SnapshotCommittedCount } else { $null }
    }
    $script:runLog.Add($entry)
    return $entry
}

$hasFixture = -not $SkipFixture
$fixtureA = Join-Path $FixtureRoot 'media\frameid_1080p60_a.mp4'
if ($hasFixture -and -not (Test-Path -LiteralPath $fixtureA -PathType Leaf)) {
    $hasFixture = $false
}

# -RenderLoopContrast runs one video batch per render-loop configuration on the same executable
# and media, so the render-loop scheduling hypothesis can be tested without rebuilding. The
# production default (no override) is measured first as the control.
if ($RenderLoopContrast) {
    $configurations = @(
        @{ Name = 'default'; Env = @{} },
        @{ Name = 'threaded'; Env = @{ QSG_RENDER_LOOP = 'threaded' } },
        @{ Name = 'basic'; Env = @{ QSG_RENDER_LOOP = 'basic' } },
        @{ Name = 'default-novsync'; Env = @{ QSG_NO_VSYNC = '0' } }
    )
    foreach ($configuration in $configurations) {
        for ($repeat = 1; $repeat -le $Repeats; ++$repeat) {
            [void](Invoke-T5Run -RunName "loop-$($configuration.Name)-r$repeat" `
                    -Sources @($VideoRunPath) -DurationSeconds $Seconds `
                    -ExtraEnvironment $configuration.Env -WithTrace)
        }
    }
} else {
    for ($repeat = 1; $repeat -le $Repeats; ++$repeat) {
        [void](Invoke-T5Run -RunName "video-1source-r$repeat" -Sources @($VideoRunPath) `
                -DurationSeconds $Seconds -WithTrace)
    }
    if ($hasFixture) {
        for ($repeat = 1; $repeat -le $Repeats; ++$repeat) {
            [void](Invoke-T5Run -RunName "fixture-1source-r$repeat" -Sources @($fixtureA) `
                    -DurationSeconds $Seconds -WithTrace)
        }
    }
}

# --- summary -------------------------------------------------------------------
$summary = [System.Collections.Generic.List[string]]::new()
[void]$summary.Add("# T5 evidence $Label $((Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))")
[void]$summary.Add('')
[void]$summary.Add("video: $Video (sha256 $(Get-FileSha256 $Video))")
[void]$summary.Add("video run path: $VideoRunPath")
[void]$summary.Add("executable: $Executable (sha256 $(Get-FileSha256 $Executable))")
[void]$summary.Add("git: $gitSha ($gitBranch)")
[void]$summary.Add('')
[void]$summary.Add('| run | src | sec | exit | passed | failure | refresh | disp_p50 | disp_p95 | disp_p99 | disp_max | ui_p95 | ui_max | seek_p95 | drop | commits | commit/s |')
[void]$summary.Add('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
foreach ($entry in $runLog) {
    [void]$summary.Add(
        "| $($entry.run) | $($entry.sources) | $($entry.seconds) | $($entry.exit_code) | " +
        "$($entry.passed) | $($entry.failure) | $($entry.screen_refresh_hz) | " +
        "$($entry.display_interval_p50_ms) | $($entry.display_interval_p95_ms) | " +
        "$($entry.display_interval_p99_ms) | $($entry.display_interval_max_ms) | " +
        "$($entry.ui_loop_gap_p95_ms) | $($entry.ui_loop_gap_max_ms) | $($entry.seek_p95_ms) | " +
        "$($entry.drop_ratio) | $($entry.trace_commit_count) | $($entry.commit_rate_per_second) |")
}
[void]$summary.Add('')
[void]$summary.Add('## Trace pipeline timing (microseconds)')
[void]$summary.Add('')
[void]$summary.Add('| run | ready->commit_p50 | ready->publish_p50 | publish->ack_p50 | publish->ack_p95 | ack->commit_p50 | trace_disp_p95 | trace_disp_max |')
[void]$summary.Add('|---|---|---|---|---|---|---|---|')
foreach ($entry in $runLog) {
    if ($null -eq $entry.trace_event_count) { continue }
    [void]$summary.Add(
        "| $($entry.run) | $($entry.trace_ready_to_commit_p50_us) | " +
        "$($entry.trace_ready_to_publish_p50_us) | $($entry.trace_publish_to_ack_p50_us) | " +
        "$($entry.trace_publish_to_ack_p95_us) | $($entry.trace_ack_to_commit_p50_us) | " +
        "$($entry.trace_display_interval_p95_us) | $($entry.trace_display_interval_max_us) |")
}
[void]$summary.Add('')
[void]$summary.Add('## Decoder / transfer / assembly')
[void]$summary.Add('')
[void]$summary.Add('| run | d3d11va | decode_avg_us | decode_max_us | cache_hit_ratio | transfer_avg_us | transfer_max_us | frame_to_ack_avg_us | assembly_avg_us | peak_frame_bytes |')
[void]$summary.Add('|---|---|---|---|---|---|---|---|---|---|')
foreach ($entry in $runLog) {
    [void]$summary.Add(
        "| $($entry.run) | $($entry.all_d3d11va) | $($entry.decoder_average_us) | " +
        "$($entry.decoder_maximum_us) | $($entry.decoder_cache_hit_ratio) | " +
        "$($entry.transfer_average_us) | $($entry.transfer_maximum_us) | " +
        "$($entry.render_frame_to_ack_avg_us) | $($entry.frameset_assembly_avg_us) | " +
        "$($entry.peak_frame_bytes) |")
}
[System.IO.File]::WriteAllText(
    (Join-Path $runDir 'summary.md'), (($summary -join "`n") + "`n"),
    [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText(
    (Join-Path $runDir 'runs.json'),
    (@($runLog) | ConvertTo-Json -Depth 5), [System.Text.UTF8Encoding]::new($false))

Write-Output "T5_EVIDENCE_OK runDir=$runDir runs=$($runLog.Count)"
