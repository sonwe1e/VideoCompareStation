> **Status: implemented.** Superseded by the production implementation. Retained for design-history context.

# Review Navigation, Codec, and Diff Design

## Objective

Make the installed desktop application open without a terminal, accept the reported MPEG-4
Part 2 source, navigate every physical display-order frame exactly, provide practical keyboard and
timeline seeking, and add GPU-rendered side-by-side and diagnostic difference views.

The change preserves the existing atomic A/B presentation contract: a command completes only
after one complete pair is rendered and acknowledged. FFmpeg and D3D11 types remain inside their
adapters, and neither the GUI nor render thread may block on media work.

## Confirmed Product Rules

- A/B sources must have the same rational frame rate and display-order frame count. The existing
  duration tolerance of one frame remains; temporal resampling is out of scope.
- Different spatial resolutions are allowed. Diff mode resamples one source to a selectable A or B
  reference canvas and labels the result as non-pixel-exact.
- Left/Right step backward/forward one frame. Down/Up step backward/forward by a persisted choice
  of 5 or 10 frames; the default is 10.
- The timeline accepts track clicks and thumb drags. A click seeks immediately; dragging previews
  the target frame number and submits exactly one seek on release.
- The top video toolbar selects Side by Side or Diff. Diff offers RGB absolute, luma, chroma, and
  heatmap views, gain values 1x/2x/4x/8x/16x, A/B reference resolution, and nearest/bilinear/
  bicubic filtering. Defaults are RGB absolute, 1x, A, and bilinear.

## Application Entry Points

Build `DualVideoStudio.exe` as a Windows GUI-subsystem executable. It owns only desktop startup and
the hidden UI smoke path, so double-clicking it never creates a console window. Move diagnostic
commands (`--startup-check`, `--probe`, and `--compare`) to the console-subsystem
`DualVideoStudioCli.exe`. Install both beside each other, but present the GUI executable as the user
entry point and document the CLI as a developer diagnostic tool.

GUI startup failures must never become silent: append diagnostics to
`%LocalAppData%/DualVideoStudio/logs/DualVideoStudio.log` and show a concise native/Qt error dialog.
The CLI keeps machine-testable exit codes and stdout/stderr. CTest and package verification use the
correct entry point explicitly.

## Media Capability and Exact Frame Identity

`MediaProbe` will explicitly accept `AV_CODEC_ID_MPEG4` in addition to H.264 and HEVC, but only when
FFmpeg reports an available decoder. All existing CFR, SDR, 8-bit, and 4:2:0 restrictions remain.
This is a tested codec extension, not a blanket acceptance of every FFmpeg decoder.

`FrameId` means the zero-based presentation-order ordinal, not a frame-rate-derived timestamp.
Normal reported-CFR sources retain the current constant-time timestamp calculation. If exact decode
observes a skipped target timestamp or EOF before the requested ordinal, the media adapter builds a
cancellable presentation-timestamp index, validates unique ordered entries against the descriptor,
and retries using the indexed target timestamp. Sources without a reported frame count are indexed
during probing so pair validation and Last never depend on duration rounding. Index construction and
the per-open-source in-memory cache stay inside `media_ffmpeg`; projects persist descriptors, not
timestamp tables.

Exact seeking never substitutes a nearby frame. Invalid, incomplete, duplicated, or contradictory
timestamp data produces a dedicated `frame-timeline-invalid` error, while actual codec failures
remain decode errors. This fixes the verified 628-frame H.264 sample whose final physical frame has a
non-nominal PTS and must still be addressed as ordinal 627.

## Controller, Timeline, and Preferences

Extend `ReviewController` with bounded `stepFrames(qint64)` and `seekFrame(qint64)` invokables.
Previous/Next reuse `stepFrames(-1/+1)`; the coordinator remains the single authority for boundary
clamping, busy gating, command identity, cancellation, presentation acknowledgement, and snapshot
commit.

The timeline maps its normalized pointer position to
`round(position * (totalFrames - 1))`, uses a comfortably sized hit target, and is disabled when no
pair is ready or a command is pending. Dragging never floods the one-command pipeline.

A small preferences controller connects the UI to the existing asynchronous settings repository;
the QML layer does not depend on `persistence_json`. Stable string keys store the large step, view
mode, Diff metric, gain, reference canvas, and filter. Unknown keys are preserved. Missing or invalid
values fall back to defaults and never prevent startup; rapid changes are coalesced before an atomic
save.

## GPU Diff Rendering

Diff is presentation state, not a playback command. `DualVideoSurface` exposes typed view and Diff
properties, snapshots them through the scene-graph synchronization boundary, and triggers redraws
without decoding or publishing another frame pair. Mode and option changes must not create a second
presentation acknowledgement for an already acknowledged publication.

The D3D11 renderer binds both NV12 sources in one draw and applies each source's own normalized
range/matrix transform before comparison:

- RGB: `gain * abs(rgbA - rgbB)` per display-RGB channel.
- Luma: grayscale absolute difference after converting both RGB values to common BT.709 luma.
- Chroma: absolute common-BT.709 Cb/Cr differences encoded into stable blue/red channels.
- Heatmap: maximum RGB-channel difference, after gain, mapped through a fixed black-to-hot ramp.

Values clamp only at final output. Identical pixels render black in every metric. Nearest and
bilinear use clamp-to-edge sampling; bicubic uses deterministic shader taps. With unequal extents,
normalized UVs map both complete images onto the selected reference aspect ratio, and the UI shows
which source and filter were used. The side-by-side renderer and its letterboxing remain unchanged.

## Verification

Automated coverage must include:

- PE subsystem checks proving the GUI is Windows GUI and the CLI is Windows console, plus captured
  CLI diagnostics and a GUI fatal-startup log/dialog path.
- Generated MPEG-4 Part 2 CFR fixtures covering probe and first/middle/last decode while preserving
  rejection of unsupported formats.
- Generated fixtures with reported-frame PTS gaps at the end and middle, missing reported counts,
  non-zero starts, B-frame ordering, invalid/duplicate timestamps, cancellation, and exact frame
  hashes.
- Controller and QML tests for +/-1, persisted 5/10 stepping, boundary clamping, timeline clicks at
  0/50/100 percent, one command per drag, disabled/busy behavior, and settings fallback/round-trip.
- WARP pixel tests for all four Diff metrics, gains, independent color transforms, A/B reference
  canvases, three filters, unequal extents, odd sizes, scissor/opacity, and identical-frame black.
  Switching presentation options must not decode, republish, or duplicate acknowledgement.
- Debug and Release build, full CTest, format, lint, install, package verification, and hidden UI
  smoke. Manual acceptance uses the reported H.264 file at frame 627 and probes the reported MPEG-4
  file, then verifies double-click launch and real-file first/middle/last navigation.

## Out of Scope

Variable-frame-rate media, cross-frame-rate pairing, temporal interpolation, playback-speed controls,
hardware decode, CPU/QImage Diff, persistent timestamp-index files, and perceptual metrics such as
SSIM or VMAF are not part of this milestone.
