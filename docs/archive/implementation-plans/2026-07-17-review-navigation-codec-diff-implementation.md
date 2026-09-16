> **Status: executed.** Implemented and shipped. Retained for design-history context.

# Review Navigation, Codec, and Diff Implementation Plan

## Baseline and Test Loop

Run from the repository root in the configured x64 environment:

```powershell
cmake --build --preset dev
ctest --preset dev --output-on-failure
cmake --build --preset dev --target format-check
cmake --build --preset dev --target lint
```

Add a narrow failing test before each behavior change, rerun its target, then run the full Debug
suite. Finish with Release build/test/install/package verification and the two supplied real files.

## Task 1: Extend Media Capability and Frame Identity

In `media_ffmpeg`, explicitly admit MPEG-4 Part 2 only when FFmpeg exposes a decoder while retaining
all pixel-format, color, SDR, and CFR checks. Add generated MPEG-4 fixtures and probe/decode tests.

Define exact decode by display-order ordinal. For sources without `nb_frames`, build a cancellable
ordered packet-PTS index during probe and use its count. For reported-count sources, retain the fast
CFR timestamp path; on timestamp skip or early EOF, lazily build and validate the in-memory index,
then retry the exact ordinal. Add a structured timeline error and tests for end/middle gaps, non-zero
start, B-frame ordering, duplicate/missing PTS, cancellation, and exact frame content.

**Checkpoint:** `feat(media): support mpeg4 and indexed exact frames`

## Task 2: Split GUI and Diagnostic Entry Points

Refactor shared startup helpers so `DualVideoStudio.exe` is always a Windows GUI-subsystem target
and `DualVideoStudioCli.exe` is a console target owning `--startup-check`, `--probe`, and `--compare`.
Keep hidden UI smoke on the GUI target. Install both executables and update smoke/package checks,
README, and contributor commands.

Add LocalAppData file logging and a concise dialog for fatal GUI startup errors. Test PE subsystems,
captured CLI output/exit codes, and package contents.

**Checkpoint:** `feat(app): separate desktop and diagnostic entry points`

## Task 3: Add Generic Exact Navigation

Expose `ReviewController::stepFrames(qint64)` and `seekFrame(qint64)`; make existing previous/next
delegate to the generic step path. Keep clamping, single-pending-command gating, and presentation
completion in `PlaybackCoordinator`.

Replace the passive progress decoration with an accessible interactive timeline. Track clicks seek
immediately; drag updates only a target-frame preview and submits once on release. Bind Left/Right to
-1/+1 and Down/Up to -N/+N. Add controller and QML smoke coverage for edges, busy state, clicks at
0/50/100 percent, and one command per drag.

**Checkpoint:** `feat(ui): add exact keyboard and timeline seeking`

## Task 4: Persist Review Preferences

Add a UI-facing preferences controller backed through the existing `ISettingsRepository` port and a
dedicated event sink. Persist the large step (5/10), presentation mode, Diff metric, gain, reference
canvas, and filter with stable string keys. Preserve unknown keys, coalesce rapid saves, cancel during
shutdown, and fall back to documented defaults on invalid data without blocking startup.

**Checkpoint:** `feat(ui): persist review presentation preferences`

## Task 5: Add GPU Difference Rendering

Add typed Side-by-side/Diff and option properties to `DualVideoSurface`, snapshot them into
`SurfaceRenderState`, and redraw without touching playback commands or publications. Extend the
offline HLSL build and D3D11 renderer with a four-SRV Diff draw supporting RGB absolute, BT.709 luma,
BT.709 chroma, heatmap, five gains, A/B reference canvas, and nearest/bilinear/bicubic sampling.

Keep the existing split path unchanged. WARP tests validate known pixels, identical black, independent
source color transforms, unequal extents, odd sizes, clipping/opacity, and no duplicate ACK after
option-only redraws.

**Checkpoint:** `feat(render): add configurable gpu difference views`

## Task 6: Integrate the Top Toolbar

Add the compact toolbar above the surface. Show Diff-only controls contextually and label spatially
resampled results as non-pixel-exact. Bind it to the surface and preferences controller without
triggering new decode commands. Verify keyboard focus does not consume navigation shortcuts and the
layout remains usable at the current minimum window size.

**Checkpoint:** `feat(ui): expose comparison and diff controls`

## Task 7: Release Verification

Run Debug/Release builds, all CTest suites, format, lint, install, ZIP/MSI gates, hidden UI smoke, CLI
smokes, and repository diff checks. Manually verify double-click launch has no terminal. Probe
`D:\Videos\2026-06-01_23-46-34_interp.mp4`; compare
`D:\Videos\2026-06-01 23-46-34.mp4` with itself at frames 0, middle, and 627; then exercise timeline,
5/10 stepping, Last, both top-level views, all Diff metrics, gains, references, and filters.

**Final checkpoint:** `test: verify review diagnostics release`
