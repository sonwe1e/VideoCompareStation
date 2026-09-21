#requires -Version 7.0
<#
.SYNOPSIS
    Attributes the display-interval tail of one playback trace to the renderer's internal staging:
    whether a late frame was never handed to the render thread, or was handed over and then held.

.DESCRIPTION
    T5 bisection aid. It joins, per canonical frame:
      FrameSetReady (4) -> RenderPublished (6) -> RenderDrawStarted (16) -> RenderAckPublished (17)
      -> PresentationAcknowledged (7) -> SnapshotCommitted (8)
    and reports the distribution of each hop, plus the hop that dominates every interval longer
    than the given threshold. A dominant published->drawStarted hop means the scene graph did not
    schedule a render (UI/render-loop or window-level stall); a dominant drawStarted->ackPublished
    hop means the draw itself was slow (GPU/device); a dominant ready->published hop means the
    producer was late.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string[]]$TracePath,
    [double]$StallThresholdMs = 40.0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-Percentile {
    param([double[]]$Values, [double]$Quantile)
    if ($Values.Count -eq 0) { return $null }
    $sorted = @($Values | Sort-Object)
    return [double]$sorted[[math]::Min([int][math]::Floor($sorted.Count * $Quantile), $sorted.Count - 1)]
}

function Get-Summary {
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

foreach ($path in $TracePath) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Trace not found at $path" }
    $ready = @{}
    $published = @{}
    $drawStarted = @{}
    $ackPublished = @{}
    $acked = @{}
    $committed = @{}
    $commitOrder = [System.Collections.Generic.List[object]]::new()
    $kinds = @{}

    foreach ($line in (Get-Content -LiteralPath $path | Select-Object -Skip 1)) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $record = $line | ConvertFrom-Json -ErrorAction Stop
        if ($null -ne $record.PSObject.Properties['overflow']) {
            throw "Trace $path contains an overflow marker; capture is incomplete."
        }
        $kind = [int]$record.kind
        $kinds[$kind] = 1 + $(if ($kinds.ContainsKey($kind)) { $kinds[$kind] } else { 0 })
        # Renderer-side events carry the default (all-zero) identity because the render thread has no
        # playback scope; the pipeline events carry the live identity. The canonical frame id in the
        # payload is therefore the join key across both, and one trace covers one playback run.
        $key = [uint64]$record.p
        $timestamp = [decimal][uint64]$record.t
        switch ($kind) {
            4 { $ready[$key] = $timestamp }
            6 { $published[$key] = $timestamp }
            16 { $drawStarted[$key] = $timestamp }
            17 { $ackPublished[$key] = $timestamp }
            7 { $acked[$key] = $timestamp }
            8 {
                if ([uint64]$record.p -ne [uint64]::MaxValue) {
                    $committed[$key] = $timestamp
                    $commitOrder.Add([pscustomobject]@{ T = $timestamp; Key = $key; P = [uint64]$record.p })
                }
            }
            default { }
        }
    }

    $hops = [ordered]@{
        ready_published    = [System.Collections.Generic.List[object]]::new()
        published_draw     = [System.Collections.Generic.List[object]]::new()
        draw_ackpublished  = [System.Collections.Generic.List[object]]::new()
        ackpublished_acked = [System.Collections.Generic.List[object]]::new()
        acked_commit       = [System.Collections.Generic.List[object]]::new()
    }
    $stalls = [System.Collections.Generic.List[object]]::new()
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $ordered = @($commitOrder | Sort-Object T)
    for ($index = 1; $index -lt $ordered.Count; $index++) {
        $previous = $ordered[$index - 1]
        $current = $ordered[$index]
        $created = $false
        foreach ($map in @($ready, $published, $drawStarted, $ackPublished, $acked)) {
            if (-not $map.ContainsKey($current.Key)) { $created = $true }
        }
        $gap = [double](($current.T - $previous.T)) / 1000
        $key = $current.Key
        $r2p = if ($ready.ContainsKey($key) -and $published.ContainsKey($key)) { [double](($published[$key] - $ready[$key])) / 1000 } else { $null }
        $p2d = if ($published.ContainsKey($key) -and $drawStarted.ContainsKey($key)) { [double](($drawStarted[$key] - $published[$key])) / 1000 } else { $null }
        $d2a = if ($drawStarted.ContainsKey($key) -and $ackPublished.ContainsKey($key)) { [double](($ackPublished[$key] - $drawStarted[$key])) / 1000 } else { $null }
        $a2k = if ($ackPublished.ContainsKey($key) -and $acked.ContainsKey($key)) { [double](($acked[$key] - $ackPublished[$key])) / 1000 } else { $null }
        $k2c = if ($acked.ContainsKey($key)) { [double](($current.T - $acked[$key])) / 1000 } else { $null }
        foreach ($pair in @(@('ready_published', $r2p), @('published_draw', $p2d), @('draw_ackpublished', $d2a),
                @('ackpublished_acked', $a2k), @('acked_commit', $k2c))) {
            if ($null -ne $pair[1]) { $hops[$pair[0]].Add([double]$pair[1]) }
        }
        if (-not $created -and $gap -ge $StallThresholdMs) {
            $dominant = 'ready->published'
            $best = if ($null -ne $r2p) { $r2p } else { -1 }
            if ($null -ne $p2d -and $p2d -gt $best) { $dominant = 'published->drawStarted'; $best = $p2d }
            if ($null -ne $d2a -and $d2a -gt $best) { $dominant = 'drawStarted->ackPublished'; $best = $d2a }
            if ($null -ne $a2k -and $a2k -gt $best) { $dominant = 'ackPublished->acked'; $best = $a2k }
            $stalls.Add([pscustomobject]@{
                    Frame = [int64]$current.P
                    GapMs = [math]::Round($gap, 1)
                    Dominant = $dominant
                    ReadyPublishedMs = if ($null -ne $r2p) { [math]::Round($r2p, 2) } else { $null }
                    PublishedDrawMs = if ($null -ne $p2d) { [math]::Round($p2d, 2) } else { $null }
                    DrawAckPublishedMs = if ($null -ne $d2a) { [math]::Round($d2a, 2) } else { $null }
                    AckPublishedAckedMs = if ($null -ne $a2k) { [math]::Round($a2k, 2) } else { $null }
                    AckedCommitMs = if ($null -ne $k2c) { [math]::Round($k2c, 2) } else { $null }
                })
        }
    }

    Write-Output "=== $path"
    Write-Output ("    kinds: " + (($kinds.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ' '))
    foreach ($name in $hops.Keys) {
        $summary = Get-Summary $hops[$name]
        if ($summary.Count -eq 0) { Write-Output ("    {0,-20} no samples" -f $name); continue }
        Write-Output ("    {0,-20} n={1,-5} p50={2,-8:N2} p90={3,-8:N2} p95={4,-8:N2} p99={5,-8:N2} max={6,-8:N2} ms" -f `
                $name, $summary.Count, $summary.P50, $summary.P90, $summary.P95, $summary.P99, $summary.Max)
    }
    Write-Output ("    intervals >= {0}ms: {1}" -f $StallThresholdMs, $stalls.Count)
    $groups = $stalls | Group-Object Dominant | Sort-Object Count -Descending
    foreach ($group in $groups) {
        Write-Output ("      dominant {0,-28} {1} intervals" -f $group.Name, $group.Count)
    }
    foreach ($stall in ($stalls | Select-Object -First 20)) {
        Write-Output ("      frame={0,-6} gap={1,-7} dom={2,-26} r->pub={3,-8} pub->draw={4,-8} draw->ackpub={5,-8} ackpub->ack={6,-8} ack->commit={7}" -f `
                $stall.Frame, $stall.GapMs, $stall.Dominant, $stall.ReadyPublishedMs,
                $stall.PublishedDrawMs, $stall.DrawAckPublishedMs, $stall.AckPublishedAckedMs,
                $stall.AckedCommitMs)
    }
}
