> **Status: implemented.** Superseded by the production implementation. Retained for design-history context.

# Continuous Playback, CFR Verification, and Dark Controls Design

**Date:** 2026-07-17

**Target:** DualVideoStudio direct-review workflow

## 1. Context

The current review slice opens a compatible A/B source pair and presents exact frames atomically.
First, last, seek, and step operations are deliberately command-oriented: each operation decodes
one pair, waits for a matching presentation acknowledgement, and then returns to `Paused`. This is
correct for exact navigation but cannot provide continuous playback. Repeating those commands from
QML would also make the UI busy, accumulate timer drift, and seek to a keyframe for every frame.

Media probing currently treats exact equality of FFmpeg's `avg_frame_rate` and `r_frame_rate` as a
necessary CFR condition. Those fields describe different concepts, so valid CFR files can be
rejected when their container metadata disagrees. The Difference toolbar also relies on native
ComboBox popup delegates; under the Windows dark style, the delegate can combine a dark background
with dark system text.

## 2. Goals and Non-Goals

### Goals

- Add real continuous play and pause at the pair's canonical rational frame rate.
- Keep every displayed update atomic: A and B always share one canonical `FrameId`.
- Maintain real-time cadence by dropping complete pairs when decoding falls behind.
- Add a sequential FFmpeg decode path that avoids seek/flush work between forward frames.
- Accept CFR sources whose rate metadata conflicts when their presentation timestamps prove a
  constant cadence, while continuing to reject verified VFR media.
- Make every toolbar ComboBox readable and keyboard-accessible under Windows dark mode.
- Preserve stale-request rejection, bounded waits, the 256 MiB frame budget, and non-blocking GUI
  and render threads.

### Non-Goals

- Audio playback, looping, reverse playback, speed controls, or seek-while-playing.
- Cross-frame-rate pairing, temporal interpolation, or general VFR playback.
- A multi-frame decode-ahead buffer, D3D11VA decode, or a new proxy workflow.
- Changing Difference metrics, GPU composition formulas, or exact-navigation semantics.

## 3. Chosen Approach

Playback timing belongs to `PlaybackCoordinator`, using the existing monotonic clock and deadline
scheduler. QML remains a projection and command surface; its 16 ms timer never becomes a playback
clock. Only one sequential pair may be in flight. This bounds memory and simplifies pause,
cancellation, and acknowledgement ordering. When late, the coordinator calculates the current
wall-clock target and skips intermediate frame IDs as complete pairs.

Two alternatives are rejected. A QML or `ReviewController` timer that repeatedly invokes `next()`
would drift and force exact-frame command overhead. A larger decode-ahead ring could improve
throughput, but it adds eviction, latency, and cancellation complexity before hardware decoding is
available. The single-in-flight design establishes the correct contracts without precluding a
future bounded buffer.

## 4. Playback Architecture and State

`PlaybackCommand` gains `PlayCommand` and `PauseCommand`. The coordinator completes both commands
promptly; continuous playback is represented by a separate internal `PlaybackRun`, not by keeping a
UI command outstanding. The run owns an anchor frame, an absolute steady-clock anchor, an optional
cadence timer, and at most one pending playback frame. Exact open/seek/step work retains its current
`pending_` path and five-second presentation contract.

Play is admitted only for a ready, graphics-enabled source pair with a displayed frame and no
outstanding exact or playback frame. A one-frame source cannot play. If the displayed frame is the
last frame, Play restarts from frame zero; otherwise it starts with the next frame. The snapshot is
`Playing` while waiting for a future boundary and `Buffering` after a boundary while decode,
upload, or presentation is outstanding. UI `playing` is true for either state.

For target frame `n`, the due time is derived from the canonical `RationalRate` and the absolute
anchor, never by repeatedly adding rounded durations. At each scheduling point, the coordinator
maps elapsed steady time back to a canonical frame. The next target is at least the displayed frame
plus one and may be later if the clock has advanced. Intermediate targets are dropped only as
complete A/B pairs. There is no partial-source publication.

Playback requests use `FrameRequestPriority::Sequential` and full session, playback, request, and
device identity. Each accepted target has a bounded presentation deadline. Provider success,
`FramePairReady`, and `FramePairPresented` may arrive in any order; the frame commits only after all
three matching conditions hold. The next cadence decision is made after that commit. The final
frame commits and automatically transitions to `Paused`.

Pause cancels a pending cadence timer immediately. If decoding has not published a pair, it cancels
that generation and returns to `Paused`. If an atomic pair has already entered the render path, the
coordinator marks pause requested, publishes a paused snapshot, and allows only that pair to settle;
`requestedFrame` keeps navigation disabled until its acknowledgement arrives. No subsequent frame
is scheduled. Opening, exact navigation, and timeline seeking remain disabled during Playing,
Buffering, or this short pause drain. Difference presentation settings remain live.

Space toggles Play/Pause. The transport places a Play/Pause button between Previous and Next and
provides truthful accessible names. Continuous playback does not set the controller's general
`busy` flag, so the loading overlay does not cover the video.

## 5. Sequential Decode Path

`SoftwareDecoder` gains a sequential decode operation and persistent decode-cursor state. An exact
request, backward target, dirty cursor, or discontinuity continues to use `av_seek_frame` followed
by `avcodec_flush_buffers`. A forward sequential request first drains frames already buffered by
the codec, then reads packets until the target presentation timestamp is reached. It does not seek
or flush between adjacent forward targets.

`DirectFrameProvider` selects this path only for `Sequential` priority. Pair-level continuity is
committed only after both decoders produce the requested frame. If A succeeds but B fails or is
canceled, the provider marks sequential continuity dirty so the next request performs an exact
resynchronization on both sides. Exact and prefetch requests retain their existing behavior.

The fast path must handle a skipped forward target by decoding and discarding intervening frames,
honor cancellation between packet and frame operations, and release all discarded frame-budget
reservations. No decoder or FFmpeg type crosses the media adapter boundary.

## 6. CFR Classification and Timeline Verification

The normal path remains fast. When positive normalized `avg_frame_rate` and `r_frame_rate` are
equal, the probe accepts the declaration and records `TimingConfidence::kDeclaredCfr`. This
preserves current behavior for ordinary files and for declared-CFR files whose packet timeline has
an isolated anomaly already handled by the exact-frame index fallback.

When either field is missing or the fields disagree, probing enters a verification path. It builds
the complete display-order PTS index and checks the indexed count against a positive reported count.
Candidate rates come from `av_guess_frame_rate`, `avg_frame_rate`, and `r_frame_rate`, with invalid
and duplicate candidates removed.

For each candidate, frame zero anchors the sequence. Every later PTS must equal one of the two
integer stream-time-base ticks surrounding its exact rational boundary (`floor` or `ceil`). A broad
plus/minus-one-tick tolerance is not used because a coarse time base could then hide an entire
cadence error. A passing FFmpeg-guessed candidate is preferred; otherwise the passing candidate
with the smallest total residual is selected deterministically. No passing candidate produces
`kInvalidCfrTiming`. A one-frame source is accepted when it has one timestamp and at least one
positive candidate because no varying interval exists to disprove.

Verified sources record `TimingConfidence::kVerifiedCfr`, use the selected normalized rational
rate, and use the timestamp index for exact decode rather than arithmetic based on disputed
metadata. Only disputed sources pay the O(N) packet scan and approximately eight bytes per indexed
frame. Probe cancellation remains bounded and posts one terminal outcome.

The currently installed Release CLI already probes
`D:\Videos\2026-06-01_23-46-34_interp.mp4` as MPEG-4, 40/1, 419 frames and
`D:\Videos\2026-06-01 23-46-34.mp4` as H.264, 60/1, 628 frames. They are individually valid but
cannot form a pair because their canonical rates and counts differ. Real-file acceptance therefore
tests each file against itself unless a rate-matched partner is supplied.

## 7. Dark Toolbar ComboBox

A reusable `ToolbarCombo.qml` is built from `QtQuick.Templates` instead of inheriting native popup
delegates. It explicitly draws its content text, arrow indicator, focus border, popup background,
and item delegates. Normal rows use the application panel color with light text; hovered, keyboard-
highlighted, and selected rows use a blue highlight with light text. Disabled text uses the existing
muted palette.

The component preserves Tab focus, arrow navigation, Enter activation, Escape dismissal, outside-
click dismissal, and accessible names. View, Metric, Gain, Canvas, Filter, and Step all use this
component, so readability is independent of Windows, Fusion, or Basic control styles.

## 8. Failure Semantics

- Decode, upload, device, or presentation failure stops playback, keeps the last fully presented
  pair, clears the pending target, and publishes a recoverable error.
- A matching playback-frame timeout cancels that generation and pauses. Late provider and render
  events are ignored by full scoped identity.
- Device loss pauses playback before publishing graphics failure state.
- A rejected Pause or Play command cannot leave a timer or provider request orphaned.
- Shutdown cancels cadence and presentation timers before closing provider and event ingress.
- CFR verification distinguishes unsupported timing from I/O, cancellation, frame-count, and
  malformed-timeline errors; technical FFmpeg details remain outside user-facing text.

## 9. Testing and Acceptance

Application tests cover rational absolute deadlines, no cumulative drift, slow-decode paired-frame
skips, one in-flight request, pause before a tick, pause during decode, pause after publication,
event reordering, stale generations, last-frame auto-pause, replay from the end, timeout, failure,
and graphics loss. Controller/QML tests cover properties, button states, Space toggling, disabled
navigation during playback, no busy overlay, and live Difference options.

Media tests cover adjacent sequential decode, forward skips, exact fallback, cancellation,
resynchronization after one-side failure, H.264 and MPEG-4 frame identity, and frame-budget release.
CFR tests cover 30 fps at a fine time base, quantized 33/34-tick cadence, 30000/1001 rates,
non-zero starts, B-frame display ordering, missing metadata, genuine VFR, reported-count conflicts,
single-frame input, cancellation, and integer boundaries.

Toolbar tests run under Windows and Basic styles, open the popup, verify normal and highlighted
text/background contrast of at least 4.5:1, and exercise keyboard selection and dismissal. Existing
format, lint, Debug, Release, WARP render, UI smoke, install, PE-subsystem, and shutdown tests remain
required.

Acceptance requires a packaged Release GUI with no console window that can play and pause a
compatible 1080p source pair, keeps A/B frame IDs atomic, maintains wall-clock cadence through
paired drops, stops cleanly at the final frame, accepts timestamp-proven CFR metadata conflicts,
rejects the VFR fixture, and displays every toolbar option legibly in Windows dark mode.
