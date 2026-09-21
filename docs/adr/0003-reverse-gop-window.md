# ADR 0003: Reverse interactive step uses a bounded GOP window

Status: accepted

Plan reference: `docs/plans/playback-overhaul` ADR-006 / §04.3 (C-03).

## Decision

Compressed video cannot decode backwards cheaply. Held-backward must not Exact-seek every
`-1` step. After a Reverse request for frame `F`, each source decode actor builds a bounded
**Reverse GOP Window** into its byte-limited source cache:

1. Choose `windowStart = max(0, F - reverseWindowFrames)`, further shrunk by cache capacity.
2. Find the lowest uncached frame in `[windowStart, F)`.
3. If none: count a window hit and stop.
4. Otherwise one `decodeExact(seed)` (one seek to the covering keyframe) then
   `decodeSequential` upward until `F`, the time budget, cancellation, or control/sequential
   interruption.
5. Insert every walked frame into the source cache. Later reverse steps inside the window are
   cache hits (zero additional exact seeks).

If the cache cannot retain at least two frames, or the seed decode fails, the actor counts
`reverseExactFallbackCount` and leaves per-step Exact as the correct path. Multi-source
sessions never block one source's window on another: each actor builds independently, and
alignment `Missing` slots still assemble as missing FrameSet entries.

## Alternatives

1. Exact seek every `-1` (status quo before this ADR) — correct but P95 far from forward.
2. Unlimited history cache — unbounded memory.
3. FFmpeg reverse decode — not supported for H.264/HEVC long GOP.
4. Offline reverse proxies — disk and preprocessing cost.

## Consequences

- Held-backward on long GOP costs roughly one seek per window, not one per frame.
- Window length is a request desire (`reverseWindowFrames`, default 24, max 48) and a byte
  budget in the actor; 4K/P010 naturally shrink the window or fall back Exact.
- Metrics: `reverseWindowHitCount`, `reverseWindowBuildCount`, `reverseWindowBuiltFrameCount`,
  `reverseWindowBuildMicroseconds` / maximum, `reverseExactFallbackCount`.
- Trace: `ReverseWindowBuilt` / `ReverseWindowHit` / `ReverseExactFallback`.
- Presentation correctness is unchanged: reverse targets still assemble full FrameSets and
  wait for presentation ACK; the window only warms source-side decode cache.
