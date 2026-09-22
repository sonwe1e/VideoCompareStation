#requires -Version 7.0
<#
.SYNOPSIS
    Runs the T0 evidence baseline experiments (P1-P4) and produces an evidence bundle:
    environment manifest, per-run raw output, metrics JSON, trace timing summaries, and a
    human-readable summary. The app itself is unchanged in behavior; this script only
    observes and archives.

.DESCRIPTION
    P1  Base playback: 1/2/3-source 1080p60 short trace runs + long aggregate runs.
    P2  Display-path contrast: 2-source side/wipe/diff modes with trace.
    P3  Precise navigation: 3 seek/step rounds plus the navigation trace gate invariants.
    P4  Image & mixed tasks: folder comparison evidence including failure fixtures.

    Every performance run saves: command line, raw stdout/stderr, parsed metrics JSON, the
    trace (when enabled) and its timing summary. Environment (git SHA, build params, device,
    display, disk, backend, fixture hashes) is archived in environment.json. Runs on displays
    below 120 Hz are marked valid_cadence_evidence=false; the reference hardware gate remains
    the authority for cadence claims.
#>
[CmdletBinding()]
param(
    [string]$Executable = 'G:\Workspaces\Toy\out\build\release\bin\VCStation.exe',
    [string]$FixtureRoot = 'G:\Workspaces\Toy\out\evidence-fixtures',
    [string]$EvidenceRoot = 'G:\Workspaces\Toy\out\evidence',
    [string]$BuildType = 'Release',
    [int]$ShortSeconds = 15,
    [int]$LongSeconds = 60,
    [switch]$SkipLong,
    [switch]$SkipImages,
    [string]$Ffmpeg = 'D:\Documents\SoftWare\ffmpeg\bin\ffmpeg.exe',
    [string]$Ffprobe = 'D:\Documents\SoftWare\ffmpeg\bin\ffprobe.exe'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "Executable not found at $Executable"
}
$contractPath = Join-Path $FixtureRoot 'contract.json'
if (-not (Test-Path -LiteralPath $contractPath -PathType Leaf)) {
    throw "Fixture contract not found at $contractPath; run generate-evidence-fixtures.ps1 first."
}
$mediaA = Join-Path $FixtureRoot 'media\frameid_1080p60_a.mp4'
$mediaB = Join-Path $FixtureRoot 'media\frameid_1080p60_b.mp4'
$mediaC = Join-Path $FixtureRoot 'media\frameid_1080p60_c.mp4'
foreach ($needed in @($mediaA, $mediaB, $mediaC)) {
    if (-not (Test-Path -LiteralPath $needed -PathType Leaf)) {
        throw "Missing fixture $needed"
    }
}
$imageLeft = Join-Path $FixtureRoot 'image-pairs\L'
$imageRight = Join-Path $FixtureRoot 'image-pairs\R'
if (-not (Test-Path -LiteralPath $imageLeft) -or -not (Test-Path -LiteralPath $imageRight)) {
    throw 'Image-pair fixture folders are missing.'
}

Import-Module -Name 'G:\Workspaces\Toy\tools\testing\PlaybackTraceGate.psm1' -Force

$runDir = Join-Path $EvidenceRoot ("baseline-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
[void][System.IO.Directory]::CreateDirectory($runDir)

function Get-FileSha256 {
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

# StrictMode-safe optional field access: metrics/image JSON omit optional keys (e.g. failure)
# when the run passes, and PSObject property access on a missing member throws under Latest.
function Get-MetricField {
    param($Object, [string]$Name)
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Get-EnvironmentManifest {
    $fixtureDrive = [System.IO.Path]::GetPathRoot((Resolve-Path -LiteralPath $FixtureRoot).Path).TrimEnd('\')
    $diskInfo = $null
    try {
        $volume = Get-Volume -DriveLetter ($fixtureDrive.TrimEnd(':')) -ErrorAction Stop
        $diskInfo = [pscustomobject]@{
            drive = $fixtureDrive
            file_system = $volume.FileSystem
            size_bytes = $volume.Size
            size_remaining_bytes = $volume.SizeRemaining
        }
        try {
            $physical = Get-PhysicalDisk | Where-Object {
                $_.DeviceId -in (Get-Partition -DriveLetter ($fixtureDrive.TrimEnd(':')) -ErrorAction Stop |
                        Get-Disk -ErrorAction Stop).Number
            } | Select-Object -First 1
            if ($null -ne $physical) {
                $diskInfo | Add-Member -NotePropertyName media_type -NotePropertyValue $physical.MediaType
                $diskInfo | Add-Member -NotePropertyName bus_type -NotePropertyValue $physical.BusType
            }
        } catch {
            $diskInfo | Add-Member -NotePropertyName media_type -NotePropertyValue 'unknown'
            $diskInfo | Add-Member -NotePropertyName bus_type -NotePropertyValue 'unknown'
        }
    } catch {
        $diskInfo = [pscustomobject]@{ drive = $fixtureDrive; error = $_.Exception.Message }
    }

    $gitSha = 'unknown'
    $gitBranch = 'unknown'
    $gitDirty = $null
    try {
        $gitSha = (& git -C 'G:\Workspaces\Toy' rev-parse HEAD 2>&1 | Select-Object -First 1)
        $gitBranch = (& git -C 'G:\Workspaces\Toy' branch --show-current 2>&1 | Select-Object -First 1)
        $dirtyLines = @(& git -C 'G:\Workspaces\Toy' status --porcelain 2>&1)
        $gitDirty = $dirtyLines.Count -gt 0
    } catch {
        $gitDirty = $null
    }

    $gpuList = @()
    try {
        $gpuList = @(Get-CimInstance Win32_VideoController | ForEach-Object {
                [pscustomobject]@{
                    name = $_.Name
                    driver_version = $_.DriverVersion
                    current_refresh_hz = $_.CurrentRefreshRate
                    current_resolution = "$($_.CurrentHorizontalResolution)x$($_.CurrentVerticalResolution)"
                    pnp_device_id = $_.PNPDeviceID
                }
            })
    } catch {
        $gpuList = @()
    }

    $cpuName = 'unknown'
    $totalRamBytes = $null
    try {
        $cpuName = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
        $totalRamBytes = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
    } catch {
    }

    $fileInfo = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($Executable)
    return [pscustomobject]@{
        collected_utc = [DateTime]::UtcNow.ToString('o')
        git_sha = $gitSha
        git_branch = $gitBranch
        git_dirty = $gitDirty
        repository = 'G:\Workspaces\Toy'
        build_type = $BuildType
        executable = $Executable
        executable_sha256 = Get-FileSha256 $Executable
        executable_file_version = $fileInfo.FileVersion
        os = [System.Environment]::OSVersion.VersionString
        cpu = $cpuName
        total_ram_bytes = $totalRamBytes
        gpu = $gpuList
        fixture_root = (Resolve-Path -LiteralPath $FixtureRoot).Path
        fixture_contract_sha256 = Get-FileSha256 $contractPath
        fixture_disk = $diskInfo
    }
}

$environment = Get-EnvironmentManifest
$environmentPath = Join-Path $runDir 'environment.json'
[System.IO.File]::WriteAllText(
    $environmentPath,
    ($environment | ConvertTo-Json -Depth 6),
    [System.Text.UTF8Encoding]::new($false)
)

$runLog = [System.Collections.Generic.List[object]]::new()

function Invoke-PerformanceRun {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RunName,

        [Parameter(Mandatory = $true)]
        [string[]]$Sources,

        [Parameter(Mandatory = $true)]
        [int]$Seconds,

        [string]$Mode = 'side',

        [switch]$WithTrace
    )
    $runPath = Join-Path $runDir $RunName
    [void][System.IO.Directory]::CreateDirectory($runPath)
    $argsList = @('--ui-performance') + @($Sources) + @('--seconds', "$Seconds", '--mode', $Mode)
    $commandLine = ($argsList | ForEach-Object {
            if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
        }) -join ' '
    [System.IO.File]::WriteAllText(
        (Join-Path $runPath 'command.txt'),
        $commandLine,
        [System.Text.UTF8Encoding]::new($false)
    )

    $stdoutFile = Join-Path $runPath 'stdout.txt'
    $stderrFile = Join-Path $runPath 'stderr.txt'
    $tracePath = Join-Path $runPath 'trace.jsonl'
    $previousTrace = $null
    $hadTrace = Test-Path -LiteralPath 'Env:DVS_PLAYBACK_TRACE'
    if ($hadTrace) {
        $previousTrace = $env:DVS_PLAYBACK_TRACE
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
        if ($hadTrace) {
            $env:DVS_PLAYBACK_TRACE = $previousTrace
        } else {
            Remove-Item -LiteralPath 'Env:DVS_PLAYBACK_TRACE' -ErrorAction SilentlyContinue
        }
    }

    $metrics = $null
    $resultLine = Get-Content -LiteralPath $stderrFile -ErrorAction SilentlyContinue |
        Where-Object { $_ -like 'DVS_PERFORMANCE_RESULT*' } | Select-Object -Last 1
    if ($null -ne $resultLine) {
        $json = $resultLine.Substring('DVS_PERFORMANCE_RESULT '.Length)
        try {
            $metrics = $json | ConvertFrom-Json
            [System.IO.File]::WriteAllText(
                (Join-Path $runPath 'metrics.json'),
                $json,
                [System.Text.UTF8Encoding]::new($false)
            )
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
                ($timing | ConvertTo-Json -Depth 6),
                [System.Text.UTF8Encoding]::new($false)
            )
        } catch {
            $timing = $null
        }
    }

    $entry = [pscustomobject]@{
        run = $RunName
        exit_code = $exitCode
        seconds = $Seconds
        mode = $Mode
        sources = ($Sources | ForEach-Object { [System.IO.Path]::GetFileName($_) }) -join '+'
        with_trace = $WithTrace
        passed = Get-MetricField $metrics 'passed'
        failure = Get-MetricField $metrics 'failure'
        screen_refresh_hz = Get-MetricField $metrics 'screen_refresh_hz'
        all_d3d11va = Get-MetricField $metrics 'all_d3d11va'
        display_interval_p50_ms = Get-MetricField $metrics 'display_interval_p50_ms'
        display_interval_p95_ms = Get-MetricField $metrics 'display_interval_p95_ms'
        display_interval_p99_ms = Get-MetricField $metrics 'display_interval_p99_ms'
        display_interval_max_ms = Get-MetricField $metrics 'display_interval_max_ms'
        ui_loop_gap_p50_ms = Get-MetricField $metrics 'ui_loop_gap_p50_ms'
        ui_loop_gap_p95_ms = Get-MetricField $metrics 'ui_loop_gap_p95_ms'
        ui_loop_gap_p99_ms = Get-MetricField $metrics 'ui_loop_gap_p99_ms'
        ui_loop_gap_max_ms = Get-MetricField $metrics 'ui_loop_gap_max_ms'
        seek_p95_ms = Get-MetricField $metrics 'seek_p95_ms'
        warm_step_p95_ms = Get-MetricField $metrics 'warm_step_p95_ms'
        presented_frames = Get-MetricField $metrics 'presented_frames'
        dropped_frames = Get-MetricField $metrics 'dropped_frames'
        drop_ratio = Get-MetricField $metrics 'drop_ratio'
        commit_rate_per_second = if ($null -ne $timing) { $timing.CommitRatePerSecond } else { $null }
        trace_display_interval_p50_us =
            if ($null -ne $timing) { $timing.DisplayIntervalMicroseconds.P50 } else { $null }
        trace_display_interval_p99_us =
            if ($null -ne $timing) { $timing.DisplayIntervalMicroseconds.P99 } else { $null }
        trace_ready_to_commit_p50_us =
            if ($null -ne $timing) { $timing.ReadyToCommitMicroseconds.P50 } else { $null }
        trace_publish_to_ack_p50_us =
            if ($null -ne $timing) { $timing.PublishToAckMicroseconds.P50 } else { $null }
        trace_ack_to_commit_p50_us =
            if ($null -ne $timing) { $timing.AckToCommitMicroseconds.P50 } else { $null }
        trace_prepare_to_draw_start_p50_us =
            if ($null -ne $timing) { $timing.PrepareToDrawStartMicroseconds.P50 } else { $null }
        trace_draw_submit_p50_us =
            if ($null -ne $timing) { $timing.DrawSubmitMicroseconds.P50 } else { $null }
        trace_draw_ack_to_present_p50_us =
            if ($null -ne $timing) { $timing.DrawAckToPresentMicroseconds.P50 } else { $null }
        trace_render_draw_started_count =
            if ($null -ne $timing) { $timing.RenderDrawStartedCount } else { $null }
        trace_render_ack_published_count =
            if ($null -ne $timing) { $timing.RenderAckPublishedCount } else { $null }
        trace_event_count = if ($null -ne $timing) { $timing.EventCount } else { $null }
        valid_cadence_evidence =
            $null -ne $metrics -and (Get-MetricField $metrics 'screen_refresh_hz') -ge 120
    }
    $script:runLog.Add($entry)
    return $entry
}

# --- P1: base playback, 1/2/3-source, short trace runs first, then long aggregates ---
$shortRuns = @()
$shortRuns += Invoke-PerformanceRun -RunName 'p1-1source-short' -Sources @($mediaA) `
    -Seconds $ShortSeconds -WithTrace
$shortRuns += Invoke-PerformanceRun -RunName 'p1-2source-short' -Sources @($mediaA, $mediaB) `
    -Seconds $ShortSeconds -WithTrace
$shortRuns += Invoke-PerformanceRun -RunName 'p1-3source-short' -Sources @($mediaA, $mediaB, $mediaC) `
    -Seconds $ShortSeconds -WithTrace

if (-not $SkipLong) {
    foreach ($profile in @(
            @{ Name = 'p1-1source-long'; Sources = @($mediaA) },
            @{ Name = 'p1-2source-long'; Sources = @($mediaA, $mediaB) },
            @{ Name = 'p1-3source-long'; Sources = @($mediaA, $mediaB, $mediaC) }
        )) {
        [void](Invoke-PerformanceRun -RunName $profile.Name -Sources $profile.Sources `
                -Seconds $LongSeconds)
    }
}

# --- P2: display-path contrast across comparison modes (2-source, trace) ---
foreach ($mode in @('side', 'wipe', 'diff')) {
    [void](Invoke-PerformanceRun -RunName "p2-2source-$mode" -Sources @($mediaA, $mediaB) `
            -Seconds $ShortSeconds -Mode $mode -WithTrace)
}

# --- P3: precise navigation, 3 rounds cold/hot + trace gate invariants ---
for ($round = 1; $round -le 3; ++$round) {
    [void](Invoke-PerformanceRun -RunName "p3-round$round" -Sources @($mediaA, $mediaB) `
            -Seconds $ShortSeconds)
}
$navigationResult = $null
try {
    $navigationResult = Invoke-PlaybackTraceGate -Gate navigation -Executable $Executable `
        -Fixtures @($mediaA, $mediaB) -DurationSeconds 8 -LogRoot $runDir `
        -RunName 'p3-navigation-gate'
    [System.IO.File]::WriteAllText(
        (Join-Path $runDir 'p3-navigation-gate-result.txt'),
        ($navigationResult | ConvertTo-Json -Depth 4),
        [System.Text.UTF8Encoding]::new($false)
    )
} catch {
    $navigationResult = [pscustomobject]@{ error = $_.Exception.Message }
}

# --- P4: image folder evidence (skippable) ---
$imageResult = $null
if (-not $SkipImages) {
    $imageRunPath = Join-Path $runDir 'p4-images'
    [void][System.IO.Directory]::CreateDirectory($imageRunPath)
    $imageArgs = @('--ui-image-folder', (Resolve-Path -LiteralPath $imageLeft).Path,
        (Resolve-Path -LiteralPath $imageRight).Path)
    [System.IO.File]::WriteAllText(
        (Join-Path $imageRunPath 'command.txt'),
        ($imageArgs -join ' '),
        [System.Text.UTF8Encoding]::new($false)
    )
    $imageProcess = Start-Process -FilePath $Executable -ArgumentList $imageArgs -NoNewWindow `
        -Wait -PassThru -RedirectStandardOutput (Join-Path $imageRunPath 'stdout.txt') `
        -RedirectStandardError (Join-Path $imageRunPath 'stderr.txt')
    $imageExit = $imageProcess.ExitCode
    $imageLine = Get-Content -LiteralPath (Join-Path $imageRunPath 'stderr.txt') -ErrorAction SilentlyContinue |
        Where-Object { $_ -like 'DVS_IMAGE_RESULT*' } | Select-Object -Last 1
    if ($null -ne $imageLine) {
        $imageJson = $imageLine.Substring('DVS_IMAGE_RESULT '.Length)
        try {
            $imageResult = $imageJson | ConvertFrom-Json
            [System.IO.File]::WriteAllText(
                (Join-Path $imageRunPath 'images.json'),
                $imageJson,
                [System.Text.UTF8Encoding]::new($false)
            )
        } catch {
            $imageResult = $null
        }
    }
    # Keep the same property shape as performance entries so the summary loops stay uniform.
    $script:runLog.Add([pscustomobject]@{
            run = 'p4-images'
            exit_code = $imageExit
            seconds = $null
            mode = 'images'
            sources = $null
            with_trace = $false
            passed = Get-MetricField $imageResult 'passed'
            failure = Get-MetricField $imageResult 'failure'
            screen_refresh_hz = Get-MetricField $imageResult 'screen_refresh_hz'
            all_d3d11va = $null
            display_interval_p50_ms = $null
            display_interval_p95_ms = $null
            display_interval_p99_ms = $null
            display_interval_max_ms = $null
            ui_loop_gap_p50_ms = Get-MetricField $imageResult 'ui_loop_gap_p50_ms'
            ui_loop_gap_p95_ms = Get-MetricField $imageResult 'ui_loop_gap_p95_ms'
            ui_loop_gap_p99_ms = Get-MetricField $imageResult 'ui_loop_gap_p99_ms'
            ui_loop_gap_max_ms = Get-MetricField $imageResult 'ui_loop_gap_max_ms'
            seek_p95_ms = $null
            warm_step_p95_ms = $null
            presented_frames = $null
            dropped_frames = $null
            drop_ratio = $null
            commit_rate_per_second = $null
            trace_display_interval_p50_us = $null
            trace_display_interval_p99_us = $null
            trace_ready_to_commit_p50_us = $null
            trace_publish_to_ack_p50_us = $null
            trace_ack_to_commit_p50_us = $null
            trace_prepare_to_draw_start_p50_us = $null
            trace_draw_submit_p50_us = $null
            trace_draw_ack_to_present_p50_us = $null
            trace_render_draw_started_count = $null
            trace_render_ack_published_count = $null
            trace_event_count = $null
            pair_count = Get-MetricField $imageResult 'pair_count'
            load_folders_ms = Get-MetricField $imageResult 'load_folders_ms'
            valid_cadence_evidence = $false
        })
}

# --- Cross-check P4 row hashes against the fixture contract ---
$contract = Get-Content -LiteralPath $contractPath -Raw | ConvertFrom-Json
$hashMismatches = [System.Collections.Generic.List[string]]::new()
if ($null -ne $imageResult -and $null -ne $imageResult.rows) {
    foreach ($row in $imageResult.rows) {
        foreach ($side in @('Left', 'Right')) {
            $shaField = if ($side -eq 'Left') { 'leftSha256' } else { 'rightSha256' }
            $sha = Get-MetricField $row $shaField
            if ([string]::IsNullOrEmpty($sha)) {
                continue
            }
            $fileName = [string](Get-MetricField $row 'fileName')
            $contractEntry = $contract.images | Where-Object {
                $_.file -eq $fileName -and
                $_.side -eq $(if ($side -eq 'Left') { 'L' } else { 'R' })
            } | Select-Object -First 1
            if ($null -ne $contractEntry -and $contractEntry.sha256 -ne $sha) {
                $hashMismatches.Add("row $($row.row) $side $fileName hash mismatch")
            }
        }
    }
}
[System.IO.File]::WriteAllText(
    (Join-Path $runDir 'hash-mismatches.txt'),
    (($hashMismatches -join "`n") + "`n"),
    [System.Text.UTF8Encoding]::new($false)
)

# --- Summary ---
$summaryLines = [System.Collections.Generic.List[string]]::new()
[void]$summaryLines.Add("# T0 evidence baseline $((Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))")
[void]$summaryLines.Add('')
[void]$summaryLines.Add("| run | mode | src | sec | exit | passed | refresh | vaid-cadence |")
[void]$summaryLines.Add('|---|---|---|---|---|---|---|---|')
foreach ($entry in $runLog) {
    [void]$summaryLines.Add(
        "| $($entry.run) | $($entry.mode) | $($entry.sources) | $($entry.seconds) | " +
        "$($entry.exit_code) | $($entry.passed) | $($entry.screen_refresh_hz) | " +
        "$($entry.valid_cadence_evidence) |"
    )
}
[void]$summaryLines.Add('')
[void]$summaryLines.Add('## Key metrics (P50/P95/P99 + longest pause, not average FPS)')
[void]$summaryLines.Add('')
[void]$summaryLines.Add('| run | disp_p50 | disp_p95 | disp_p99 | disp_max | ui_p50 | ui_p95 | ui_p99 | ui_max | seek_p95 | drop |')
[void]$summaryLines.Add('|---|---|---|---|---|---|---|---|---|---|---|')
foreach ($entry in $runLog) {
    if ($null -eq $entry.display_interval_p50_ms) {
        continue
    }
    [void]$summaryLines.Add(
        "| $($entry.run) | $($entry.display_interval_p50_ms) | $($entry.display_interval_p95_ms) | " +
        "$($entry.display_interval_p99_ms) | $($entry.display_interval_max_ms) | " +
        "$($entry.ui_loop_gap_p50_ms) | $($entry.ui_loop_gap_p95_ms) | $($entry.ui_loop_gap_p99_ms) | " +
        "$($entry.ui_loop_gap_max_ms) | $($entry.seek_p95_ms) | $($entry.drop_ratio) |"
    )
}
[void]$summaryLines.Add('')
[void]$summaryLines.Add('## Trace pipeline timing (prepare / draw submit / final present, microseconds)')
[void]$summaryLines.Add('')
[void]$summaryLines.Add(
    '| run | commits | commit_rate | disp_p50 | disp_p99 | ready->commit_p50 | ' +
    'prepare->draw_p50 | draw_submit_p50 | draw_ack->present_p50 | ack->commit_p50 |'
)
[void]$summaryLines.Add('|---|---|---|---|---|---|---|---|---|---|')
foreach ($entry in $runLog) {
    if ($null -eq $entry.trace_event_count) {
        continue
    }
    [void]$summaryLines.Add(
        "| $($entry.run) | $($entry.trace_event_count) | $($entry.commit_rate_per_second) | " +
        "$($entry.trace_display_interval_p50_us) | $($entry.trace_display_interval_p99_us) | " +
        "$($entry.trace_ready_to_commit_p50_us) | $($entry.trace_prepare_to_draw_start_p50_us) | " +
        "$($entry.trace_draw_submit_p50_us) | $($entry.trace_draw_ack_to_present_p50_us) | " +
        "$($entry.trace_ack_to_commit_p50_us) |"
    )
}
[void]$summaryLines.Add('')
[void]$summaryLines.Add('## Environment')
[void]$summaryLines.Add('')
[void]$summaryLines.Add("- git: $($environment.git_sha) ($($environment.git_branch)) dirty=$($environment.git_dirty)")
[void]$summaryLines.Add("- build: $($environment.build_type), exe sha256 $($environment.executable_sha256)")
[void]$summaryLines.Add("- os: $($environment.os)")
[void]$summaryLines.Add("- cpu: $($environment.cpu)")
if ($null -ne $environment.total_ram_bytes) {
    [void]$summaryLines.Add("- ram: $([math]::Round($environment.total_ram_bytes / 1GB, 1)) GiB")
}
foreach ($gpu in $environment.gpu) {
    [void]$summaryLines.Add("- gpu: $($gpu.name) | $($gpu.current_resolution) @ $($gpu.current_refresh_hz)Hz | $($gpu.driver_version)")
}
if ($null -ne $environment.fixture_disk) {
    [void]$summaryLines.Add(
        "- fixture disk: $(Get-MetricField $environment.fixture_disk 'drive') " +
        "$(Get-MetricField $environment.fixture_disk 'media_type') " +
        "$(Get-MetricField $environment.fixture_disk 'file_system')"
    )
}
[void]$summaryLines.Add("- fixtures contract sha256: $($environment.fixture_contract_sha256)")
[void]$summaryLines.Add('')
[void]$summaryLines.Add('## Notes')
[void]$summaryLines.Add('')
[void]$summaryLines.Add('- display adapters are recorded in environment.json (name/driver/resolution/refresh);')
[void]$summaryLines.Add('  per self-hosted-runner.md, virtual/indirect adapters (GameViewer/Oray/etc.) do not qualify')
[void]$summaryLines.Add('  as cadence evidence regardless of the refresh the app reports; the reference 120 Hz+')
[void]$summaryLines.Add('  hardware gate remains the authority for cadence claims.')
[void]$summaryLines.Add('- runs with valid_cadence_evidence=false ran on a display the app reported below 120 Hz.')
if ($hashMismatches.Count -gt 0) {
    [void]$summaryLines.Add('- P4 hash cross-check FAILED:')
    foreach ($mismatch in $hashMismatches) {
        [void]$summaryLines.Add("  - $mismatch")
    }
} else {
    [void]$summaryLines.Add('- P4 row hashes match the fixture contract.')
}
if ($null -ne $navigationResult) {
    [void]$summaryLines.Add("- navigation gate: $($navigationResult | ConvertTo-Json -Compress)")
}
[System.IO.File]::WriteAllText(
    (Join-Path $runDir 'summary.md'),
    ($summaryLines -join "`n"),
    [System.Text.UTF8Encoding]::new($false)
)

# --- Manifest ---
$manifestEntries = @()
foreach ($file in (Get-ChildItem -LiteralPath $runDir -Recurse -File)) {
    $relative = $file.FullName.Substring($runDir.Length).TrimStart('\')
    $manifestEntries += [pscustomobject]@{
        file = $relative
        size_bytes = $file.Length
        sha256 = Get-FileSha256 $file.FullName
    }
}
[System.IO.File]::WriteAllText(
    (Join-Path $runDir 'manifest.json'),
    (@{ run_dir = $runDir; generated_utc = [DateTime]::UtcNow.ToString('o'); artifacts = @($manifestEntries) } |
        ConvertTo-Json -Depth 5),
    [System.Text.UTF8Encoding]::new($false)
)

Write-Output "EVIDENCE_BASELINE_OK runDir=$runDir runs=$($runLog.Count) hashMismatches=$($hashMismatches.Count)"
