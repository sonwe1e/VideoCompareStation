# ADR 0005: CPU pixel-difference metrics foundation

Status: accepted

Plan references: playback-overhaul phase 5 (Comparison Analytics), C-01/C-02 pair identity.

## Decision

Comparison analytics starts with an **adapter-neutral CPU scalar reference** for RGB absolute
pixel difference on the session's active `ComparisonPair`.

- Domain owns `Rgba8View` + `PixelDifferenceMetrics` + `computeRgbAbsoluteMetrics`
  (`src/domain/.../PixelDifference.*`). Formula id: `cpu-rgb-absolute-v1`.
- Application owns the pure scorer `scoreActivePairRgbAbsolute` that binds a live
  `ComparisonPair` + `FrameId` to a metric sample (`ComparisonMetrics.h`). Provenance is part
  of the DTO; UI/export must not invent formula names.
- Alpha is never averaged into MAE/MSE. PSNR uses MAX=255 for 8-bit samples; identical buffers
  publish `kInfinitePsnrDb` rather than NaN.
- Missing/mismatched geometry returns `nullopt` — unavailable samples are never averaged.

## Consequences

- GPU/SIMD backends must differential-test against this reference before shipping.
- FrameHandle still has no CPU pixel mapping. Video analysis uses the independent
  `IPairMetricsService` worker and `PairMetricsController` projection; it does not read back
  render frames or put FFmpeg/graphics types into the core.
- Coverage fixture lists every new domain/application translation unit.

## Current video sample space

The video metric `cpu-rgb-absolute-v1` describes **decoded-converted full-frame RGBA8 RGB
values**, not original file code values or the current screen difference. Alpha is ignored;
10-bit inputs are also reduced to 8-bit RGBA before scoring (PSNR MAX remains 255).

- `PairMetricsDecodeSession::rgbaFromFrame` converts each decoded source format directly to
  full-range RGBA8 with swscale at the decoded width/height. It uses the current matrix/range
  handling, not a common transfer-function/primaries transform or complete color management.
- `SoftwareDecoder` independently normalizes playback to NV12/P010. Renderer sampling and
  this direct RGBA8 conversion need not yield identical RGB values. Sharing threshold/channel
  policy definitions does not guarantee that CPU bad-pixel counts equal highlighted screen pixels.
- `PairMetricsService::scoreFrame` compares the two whole decoded buffers at matching `(x,y)`
  coordinates and requires equal decoded width/height. It neither applies display rotation nor
  corrects sample aspect ratio (SAR), and does not spatially resample one source to align it with
  the other. Equal decoded dimensions alone do not guarantee corresponding displayed geometry.
  Dimension mismatch remains unavailable, even if the display can scale the sources to fit.
- `PairMetricsRequest` has no ROI, viewport scale or display-resampling input. Selecting an ROI,
  zooming/panning, display rotation, display spatial resampling and display gain do not change
  this full-frame statistic. Frame/time mapping still selects which decoded frames are compared.
  Threshold/channel policy changes query the bad-pixel distribution, not MAE/MSE/PSNR/max error.

The inspector states this scope below the current-frame metrics title. This clarification does
not add ROI metrics, original-RGB/native-depth analysis, geometry alignment or a new cache key;
those would need a separate contract and verification before changing the fixed analysis input.

## Reusable threshold-independent analysis

`computeRgbAbsoluteAnalysis` adds an adapter-neutral `RgbAbsoluteAnalysis`: the unchanged RGB
MAE/MSE/PSNR/max-error summary plus three cumulative 256-bin bad-pixel count distributions.
`metricsAt(threshold, policy)` returns the same metrics as the independent scalar scorer without
reading pixels again. `analyzeActivePairRgbAbsolute` binds the same pair/frame/formula provenance.
The scalar implementation remains the correctness reference, not a compatibility wrapper.

For nonnegative RGBA8 deltas, integer truncation equals floor. Only strictly positive deltas enter
the bins, so fractional signed BT.709 luma in `(0,1)` counts at threshold zero only; identical
pixels and alpha-only changes never count. The original double luma weights and evaluation order
are retained. Geometry/invalid-view failures still return `nullopt`.

A video request/batch carries work and mapping identity, not a display threshold or policy. The
controller retains analysis by immutable material/mapping identity and queries its current display
predicate. Cache scope excludes transient request/generation identity, but accepting a callback
requires the full current `PlaybackRequestContext`; input changes cancel work and invalidate
analysis. Full-frame RGBA8 conversion/formula are fixed in this build; future ROI or selectable
conversion/formula inputs must participate in analysis identity before reuse is allowed.

See [threshold reuse verification](../engineering/pair-metrics-threshold-reuse.md) for scalar
boundary checks, actual work counters, cache bounds, mutation evidence and first-analysis cost.

## C-02 residual

`PlaybackCoordinatorTests.SessionPairDoesNotLeakAcrossTopology` is enabled: shrink drops a
stale pair member; grow preserves the live post-shrink pair under `PreserveIfAvailable` and
does not resurrect the pre-shrink ordinal.
