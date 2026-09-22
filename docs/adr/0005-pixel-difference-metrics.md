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
- FrameHandle still has no CPU pixel mapping; wiring metrics into `SessionSnapshot`/status rail
  waits for an adapter pixel-port (or offline export path), not for more domain formula work.
- Coverage fixture lists every new domain/application translation unit.

## C-02 residual

`PlaybackCoordinatorTests.SessionPairDoesNotLeakAcrossTopology` is enabled: shrink drops a
stale pair member; grow preserves the live post-shrink pair under `PreserveIfAvailable` and
does not resurrect the pre-shrink ordinal.
