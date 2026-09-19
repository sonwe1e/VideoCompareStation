#requires -Version 7.0
<#
.SYNOPSIS
    Correlates one playback trace into per-frame pipeline stages and reports where the
    display-interval tail comes from: decode/ready, render publish, presentation ack, and
    the coordinator commit, plus the outlier frames themselves.

.DESCRIPTION
    Read-only diagnostic for T5 evidence. It answers the causal question the work order
    requires before any playback scheduling or cache parameter is touched:

      * is the tail inside the producer (ready -> publish), inside the renderer
        (publish -> ack), inside the coordinator (ack -> commit), or is it pure cadence
        (the scheduler deliberately waiting for the canonical frame boundary)?

    Stages per canonical frame (microseconds, from the schema-v1 monotonic `t` field):
      ready_publish  : FrameSetReady -> RenderPublished
      publish_ack    : RenderPublished -> PresentationAcknowledged
      ack_commit     : PresentationAcknowledged -> SnapshotCommitted
      ready_commit   : FrameSetReady -> SnapshotCommitted
      cadence_gap    : SnapshotCommitted(previous) -> SnapshotCommitted(current)

    The `cadence_gap - ready_commit` residual is the time the coordinator spent waiting for
    the next canonical boundary after a frame was already fully produced; a large residual
    means the cadence source (not decode/render) sets the interval, while a large
    ready_publish / publish_ack means the producer or renderer is late.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string[]]$TracePath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-Percentile {
    param([double[]]$Values, [double]$Quantile)
    if ($Values.Count -eq 0) { return $null }
    $sorted = @($Values | Sort-Object)
    $index = [math]::Min([int][math]::Floor($sorted.Count * $Quantile), $sorted.Count - 1)
    return [double]$sorted[$index]
}

function Get-StageSummary {
    param([System.Collections.Generic.List[object]]$Values)
    $array = [double[]]@($Values | ForEach-Object { [double]$_ })
    return [pscustomobject]@{
        Count = $array.Count
        P50   = Get-Percentile $array 0.50
        P90   = Get-Percentile $array 0.90
        P95   = Get-Percentile $array 0.95
        P99   = Get-Percentile $array 0.99
        Max   = if ($array.Count -eq 0) { $null } else { [double](@($array | Sort-Object)[-1]) }
    }
}

$reports = [System.Collections.Generic.List[object]]::new()
foreach ($path in $TracePath) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Trace not found at $path"
    }
    $records = [System.Collections.Generic.List[object]]::new()
    foreach ($line in (Get-Content -LiteralPath $path | Select-Object -Skip 1)) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $record = $line | ConvertFrom-Json -ErrorAction Stop
        if ($null -ne $record.PSObject.Properties['overflow']) {
            throw "Trace $path contains an overflow marker; capture is incomplete."
        }
        $records.Add($record)
    }
    if ($records.Count -eq 0) { throw "Trace $path has no events." }

    $ready = @{}
    $published = @{}
    $acked = @{}
    $committed = @{}
    $commitOrder = [System.Collections.Generic.List[object]]::new()
    $readyOrder = [System.Collections.Generic.List[object]]::new()
    $eventKinds = @{}

    foreach ($record in $records) {
        $kind = [int]$record.kind
        if (-not $eventKinds.ContainsKey($kind)) { $eventKinds[$kind] = 0 }
        $eventKinds[$kind] = $eventKinds[$kind] + 1
        $key = "$([uint64]$record.s)|$([uint64]$record.e)|$([uint64]$record.gen)|$([uint64]$record.dev)|$([uint64]$record.p)"
        $timestamp = [decimal][uint64]$record.t
        switch ($kind) {
            4 {
                $ready[$key] = $timestamp
                $readyOrder.Add([pscustomobject]@{ Time = $timestamp; Key = $key; Payload = [uint64]$record.p })
            }
            6 { $published[$key] = $timestamp }
            7 { $acked[$key] = $timestamp }
            8 {
                if ([uint64]$record.p -ne [uint64]::MaxValue) {
                    $committed[$key] = $timestamp
                    $commitOrder.Add([pscustomobject]@{
                            Time = $timestamp; Key = $key; Payload = [uint64]$record.p
                        })
                }
            }
            default { }
        }
    }

    $readyToPublish = [System.Collections.Generic.List[object]]::new()
    $publishToAck = [System.Collections.Generic.List[object]]::new()
    $ackToCommit = [System.Collections.Generic.List[object]]::new()
    $readyToCommit = [System.Collections.Generic.List[object]]::new()
    $cadenceGap = [System.Collections.Generic.List[object]]::new()
    $residual = [System.Collections.Generic.List[object]]::new()
    $outliers = [System.Collections.Generic.List[object]]::new()

    $ordered = @($commitOrder | Sort-Object -Property Time)
    for ($index = 0; $index -lt $ordered.Count; ++$index) {
        $entry = $ordered[$index]
        $key = $entry.Key
        if ($ready.ContainsKey($key)) {
            $readyToCommit.Add([double](($entry.Time - $ready[$key]) / 1000))
        }
        if ($ready.ContainsKey($key) -and $published.ContainsKey($key)) {
            $readyToPublish.Add([double](($published[$key] - $ready[$key]) / 1000))
        }
        if ($published.ContainsKey($key) -and $acked.ContainsKey($key)) {
            $publishToAck.Add([double](($acked[$key] - $published[$key]) / 1000))
        }
        if ($acked.ContainsKey($key)) {
            $ackToCommit.Add([double](($entry.Time - $acked[$key]) / 1000))
        }
        if ($index -gt 0) {
            $gap = [double](($entry.Time - $ordered[$index - 1].Time) / 1000)
            $cadenceGap.Add($gap)
            if ($ready.ContainsKey($key) -and $ready.ContainsKey($ordered[$index - 1].Key)) {
                $residual.Add([double]($gap - (($entry.Time - $ready[$key]) / 1000)))
            }
            if ($gap -gt 33.34) {
                $outliers.Add([pscustomobject]@{
                        Frame          = [int64]$entry.Payload
                        GapMs          = [math]::Round($gap, 2)
                        ReadyPublishUs = if ($ready.ContainsKey($key) -and $published.ContainsKey($key)) {
                            [math]::Round([double](($published[$key] - $ready[$key])), 1)
                        } else { $null }
                        PublishAckUs   = if ($published.ContainsKey($key) -and $acked.ContainsKey($key)) {
                            [math]::Round([double](($acked[$key] - $published[$key])), 1)
                        } else { $null }
                        AckCommitUs    = if ($acked.ContainsKey($key)) {
                            [math]::Round([double](($entry.Time - $acked[$key])), 1)
                        } else { $null }
                        ReadyCommitMs  = if ($ready.ContainsKey($key)) {
                            [math]::Round([double](($entry.Time - $ready[$key])) / 1000, 2)
                        } else { $null }
                    })
            }
        }
    }

    # Window rate: events per second and commits per second over the traced span.
    $spanSeconds = [double](($records[-1].t - $records[0].t) / 1000000)

    # UI scene-graph grabs (kinds 14/15): pair each request with the next completion and report the
    # span. A grab whose span brackets a late frame's publish window is the causal link between the
    # UI operation and the display-interval tail; the analyzer reports both so the link can be
    # checked instead of assumed.
    $grabRequests = [System.Collections.Generic.List[object]]::new()
    $grabCompletions = [System.Collections.Generic.List[object]]::new()
    $grabSpans = [System.Collections.Generic.List[object]]::new()
    foreach ($record in $records) {
        $kind = [int]$record.kind
        if ($kind -eq 14) {
            $grabRequests.Add([pscustomobject]@{ Time = [decimal][uint64]$record.t; Frame = [uint64]$record.p })
        } elseif ($kind -eq 15) {
            $grabCompletions.Add([pscustomobject]@{ Time = [decimal][uint64]$record.t; Frame = [uint64]$record.p })
        }
    }
    foreach ($request in $grabRequests) {
        $completion = $grabCompletions | Where-Object { $_.Time -ge $request.Time } | Select-Object -First 1
        if ($null -ne $completion) {
            $grabSpans.Add([double](($completion.Time - $request.Time) / 1000))
        }
    }

    # Display-interval histogram (milliseconds) so "how often is playback late" is visible, not just
    # the tail. Buckets are the display-cadence multiples of a 60 fps source.
    $histogram = [ordered]@{ 'le_17' = 0; '17_34' = 0; '34_50' = 0; '50_84' = 0; 'gt_84' = 0 }
    $intervalValues = [double[]]@($cadenceGap)
    foreach ($value in $intervalValues) {
        if ($value -le 17.0) { $histogram['le_17']++ }
        elseif ($value -le 34.0) { $histogram['17_34']++ }
        elseif ($value -le 50.0) { $histogram['34_50']++ }
        elseif ($value -le 84.0) { $histogram['50_84']++ }
        else { $histogram['gt_84']++ }
    }
    $lateFraction = if ($intervalValues.Count -eq 0) {
        $null
    } else {
        [math]::Round(100.0 * ($intervalValues | Where-Object { $_ -gt 17.0 }).Count / $intervalValues.Count, 2)
    }
    # Effective frame rate over the traced span, and the rate implied by the frames that were not
    # late (a late interval indicates at least one canonical frame never reached the display).
    $firstCommit = if ($ordered.Count -gt 0) { $ordered[0].Time } else { $null }
    $lastCommit = if ($ordered.Count -gt 0) { $ordered[-1].Time } else { $null }
    $commitSpanSeconds = if ($null -ne $firstCommit -and $null -ne $lastCommit) {
        [double](($lastCommit - $firstCommit) / 1000000)
    } else { 0.0 }

    $reports.Add([pscustomobject]@{
        Trace                  = $path
        EventCount             = $records.Count
        SpanSeconds            = [math]::Round($spanSeconds, 2)
        CommitSpanSeconds      = [math]::Round($commitSpanSeconds, 2)
        ReadyCount             = $ready.Count
        PublishedCount         = $published.Count
        AckCount               = $acked.Count
        CommitCount            = $committed.Count
        CommitRatePerSecond    = if ($commitSpanSeconds -gt 0) {
            [math]::Round($committed.Count / $commitSpanSeconds, 2)
        } else { $null }
        LateIntervalPercent    = $lateFraction
        IntervalHistogramMs    = $histogram
        CadenceGapMs           = Get-StageSummary $cadenceGap
        ReadyPublishUs         = Get-StageSummary $readyToPublish
        PublishAckUs           = Get-StageSummary $publishToAck
        AckCommitUs            = Get-StageSummary $ackToCommit
        ReadyCommitMs          = Get-StageSummary $readyToCommit
        CadenceResidualMs      = Get-StageSummary $residual
        GrabRequestCount       = $grabRequests.Count
        GrabCompletedCount     = $grabCompletions.Count
        GrabSpanUs             = Get-StageSummary $grabSpans
        EventKinds             = $eventKinds
        OutlierFrames          = @($outliers)
    })
}

foreach ($report in $reports) {
    Write-Output "=== $($report.Trace)"
    Write-Output ("    events={0} span={1}s commit_span={2}s commits={3} commit/s={4} late>17ms={5}%" -f `
            $report.EventCount, $report.SpanSeconds, $report.CommitSpanSeconds, $report.CommitCount,
            $report.CommitRatePerSecond, $report.LateIntervalPercent)
    Write-Output ("    interval histogram (ms): " + (($report.IntervalHistogramMs.GetEnumerator() |
                ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' '))
    foreach ($stage in @('CadenceGapMs', 'ReadyPublishUs', 'PublishAckUs', 'AckCommitUs',
            'ReadyCommitMs', 'CadenceResidualMs', 'GrabSpanUs')) {
        $summary = $report.$stage
        if ($null -eq $summary) { continue }
        Write-Output ("    {0,-18} n={1,-6} p50={2,-10:N2} p90={3,-10:N2} p95={4,-10:N2} p99={5,-10:N2} max={6,-10:N2}" -f `
                $stage, $summary.Count, $summary.P50, $summary.P90, $summary.P95, $summary.P99, $summary.Max)
    }
    if ($report.GrabRequestCount -gt 0) {
        Write-Output ("    UI grabs: requested={0} completed={1}" -f `
                $report.GrabRequestCount, $report.GrabCompletedCount)
    }
    Write-Output ("    outliers (>33.34ms): {0}" -f $report.OutlierFrames.Count)
    foreach ($outlier in ($report.OutlierFrames | Select-Object -First 25)) {
        Write-Output ("      frame={0,-6} gap={1,-8} ready->pub={2,-10} pub->ack={3,-10} ack->commit={4,-10} ready->commit={5}" -f `
                $outlier.Frame, $outlier.GapMs, $outlier.ReadyPublishUs, $outlier.PublishAckUs,
                $outlier.AckCommitUs, $outlier.ReadyCommitMs)
    }
}
