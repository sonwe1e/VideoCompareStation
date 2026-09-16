> **Status: implemented.** Superseded by the production implementation. Retained for design-history context.

# Direct Review and D3D11 Rendering Design

**Date:** 2026-07-16

**Target:** `DualVideoStudio` direct-review vertical slice

## 1. Context

The repository can probe supported CFR media, validate an A/B pair, decode exact software NV12
frames, coordinate frame-zero and seek commands, and launch a Qt Quick desktop shell. The shell
does not yet select media, bind an application snapshot, or display decoded pixels. The current
CLI comparison path captures frame metadata only.

This slice connects those existing capabilities to a production playback/render boundary. It
must not introduce a `QImage` playback path that would later be discarded. Software-decoded NV12
is uploaded and rendered through the same D3D11 path that later D3D11VA frames will use.

## 2. Goals and Non-Goals

### Goals

- Select local source A and B, probe them asynchronously, and validate them as one pair.
- Present the complete frame-zero pair in a single Qt Quick D3D11 surface.
- Support exact first, previous, next, and last frame commands.
- Expose immutable session state, current/total frame, busy state, and safe error messages to QML.
- Preserve atomic A/B delivery, scoped request identities, bounded waits, and the 256 MiB frame
  budget.
- Leave a reusable GPU rendering boundary for later D3D11VA decode and continuous playback.

### Non-Goals

- Continuous play/pause, playback speed, or timeline scrubbing.
- D3D11VA decode, automatic device-loss recovery, proxy generation, clips, or export.
- `.dvsproj` creation, autosave, relink, or restart restoration.
- New media acceptance rules; this slice retains the existing CFR H.264/H.265, 8-bit 4:2:0 SDR
  constraints and strict pair validation.
- A CPU RGB conversion, `QImage`, or two independently updated video items.

## 3. Architecture and Ownership

### Application

Add an `OpenSourcePathsCommand` carrying a command context and two filesystem paths.
`PlaybackCoordinator` receives an `IMediaProbe` dependency in addition to the existing direct
provider and render channel. It owns the two probe request contexts, accepts completions in either
order, validates the pair, and then reuses the existing provider-open and exact-frame machinery.

Each command still completes exactly once. Probe requests have distinct `RequestId` values under
the active `SessionId` and `SessionEpoch`. A failure cancels the sibling probe and makes later
events stale. The existing descriptor-based open command remains available to focused tests and
non-UI integration code.

The coordinator does not mark an exact frame command complete merely because a provider produced
the pair. It waits for a matching `FramePairPresented` acknowledgement. The existing
`IDeadlineScheduler` contract supplies a five-second deadline for frame zero and every exact step.

### Windows Platform and Rendering

`platform_windows` owns:

- `GraphicsDeviceBroker`, which acquires the Qt scene-graph D3D11 device on the render thread and
  publishes a monotonic `DeviceGeneration`;
- `GpuTransferActor` and immutable GPU NV12 resources charged to `FrameBudget`;
- an `IRenderChannel` implementation with one atomic complete-pair slot;
- a non-blocking presentation acknowledgement mailbox and `RenderAckRelay`; and
- `DualVideoSurface`, a custom Qt Quick item backed by one render node and an NV12 compositor.

Graphics readiness and loss cross the application boundary only as typed generation events; no
D3D pointer or Qt object enters an application snapshot. `GraphicsDeviceReady`,
`GraphicsDeviceUnavailable`, and `GraphicsDeviceLost` carry only `DeviceGeneration` plus a
structured error when unavailable or lost, and reach the coordinator through the platform relay.
`SessionSnapshot::graphicsReady` enables media commands. For this slice, a later loss invalidates
the current device/playback generations and requires restart.

Normalized matrix/range metadata is copied into the immutable concrete frame resource so the
renderer can apply BT.601 or BT.709 and full or limited range without leaking native types through
application contracts.

### UI and Composition Root

`ui_qml` adds a thin `ReviewController`. It converts local `QUrl` values to paths, allocates
command IDs, dispatches commands, drains command terminals, and projects immutable snapshots into
Qt properties. A 16 ms Qt timer performs projection and terminal draining only; it is not a
playback clock. The controller performs no media I/O, decode, GPU work, or blocking wait.

`src/app` constructs and owns the frame budget, probe, provider, device broker, transfer actor,
mailboxes, coordinator, acknowledgement relay, controller, and QML engine. Runtime objects outlive
the QML references that use them, and shutdown is explicitly orchestrated rather than relying on
incidental declaration order.

## 4. Open and Exact-Frame Data Flow

1. QML `FileDialog` instances accept only local URLs. The user selects both paths and explicitly
   invokes **Open Pair**; file extensions are hints, not validation authority.
2. `ReviewController` submits one `OpenSourcePathsCommand`. It rejects missing or non-local
   selections before dispatch without touching the current session. Comparing a file with itself
   remains valid and is left to normal pair validation.
3. The coordinator submits A and B probes concurrently. It requires a `ProbeCompleted` payload
   followed by the successful terminal for each request. Completion order is irrelevant.
4. `SourcePairValidator` checks normalized rate, effective count, and duration tolerance. During
   probe and validation, an existing ready session remains usable. An invalid candidate completes
   with an error and leaves that session unchanged.
5. Once validated, the coordinator advances the session/playback scope, opens the direct provider,
   and requests frame zero. The previously presented pixels remain pinned until the replacement
   frame is actually presented. If later open/decode/render work fails, those pixels may remain as
   last-known output, but the snapshot must not claim the old session is still ready.
6. First/previous/next/last dispatch the existing exact commands. Targets clamp to the canonical
   range; a one-frame pair always remains at frame zero. The UI admits only one exact command at a
   time, while the coordinator remains authoritative for `Busy` and stale-command outcomes.

## 5. GPU Upload and Presentation

Qt Quick selects `QSGRendererInterface::Direct3D11` before any `QQuickWindow` exists. The broker
obtains the native device and immediate context from the scene graph, enables D3D multithread
protection, and rejects a non-D3D11 renderer with a stable graphics-startup error. Media selection
stays disabled until graphics initialization succeeds.

The render channel accepts only a complete CPU NV12 `FramePair`. The transfer actor reserves the
two destination textures, uploads both planes for both sources, and records D3D11 event queries.
Only after both fences complete and the session, playback, and device generations still match may
the prepared GPU pair replace the mailbox slot. Replacing an unpresented entry releases A and B
together. The render thread never waits for a fence or for coordinator queue capacity.

`DualVideoSurface` consumes one prepared pair and draws both sides in one node. Each source is
aspect-fitted within its half of the available item with black letterboxing and no stretch. The
shader samples NV12 directly and applies the resource's normalized color matrix/range. Playback
targets forbid `QImage`, synchronous readback, file I/O, and decode work.

After drawing, the render node writes an acknowledgement into a bounded render-to-relay mailbox.
Exact acknowledgements use a two-entry lossless SPSC ring; the relay performs any backpressured
critical event posting away from the render thread. The coordinator commits `displayedFrame`,
returns to `Ready`/`Paused`, and completes the command only for the matching acknowledgement.

## 6. QML Behavior

The command bar provides **Select A**, **Select B**, and **Open Pair**, with chosen filenames and
one explicit idle/loading/ready/error operation state. Source-specific failures are identified by
their `SourceRole`. The central canvas replaces the two empty panels with one
`DualVideoSurface`; QML overlays labels and loading/error presentation but never owns frame data.

The transport exposes first, previous, next, and last buttons plus `current / total`. `Home`,
`End`, `Left`, and `Right` invoke the same controller methods. Commands are disabled while an open
or exact operation is outstanding. A one-frame timeline disables movement while preserving first
and last behavior. User-visible errors are mapped from `MediaError::userMessageKey`; technical
details remain diagnostic-only. The controller exposes one `ReviewDisplayState` (`Empty`,
`Loading`, `Ready`, `Invalid`, or `Error`) mapped from the immutable snapshot, plus a separate
in-flight flag for command feedback; QML does not reconstruct readiness from unrelated booleans.

## 7. Failure and Shutdown Semantics

- Probe errors identify source A or B. Pair-validation errors identify the pair and preserve an
  already-ready session.
- Public errors add `kGraphicsUnavailable`, `kGraphicsDeviceLost`, and
  `kFramePresentationTimedOut`, plus matching graphics initialization/presentation operations.
  Native HRESULTs appear only in technical diagnostics.
- Decode, upload, fence, or presentation failure retains the last presented pixels, clears the
  outstanding request, and publishes a truthful paused invalid/error snapshot.
- A five-second exact-frame deadline cancels the active playback generation, retains the last
  presented pair, and emits a recoverable timeout error.
- Stale probe, provider, upload, and acknowledgement results are ignored by full scoped identity,
  never by frame number alone.
- Device loss invalidates the device and playback generations, safely pauses the session, and
  requests restart. Automatic graphics recreation is deferred to the D3D11VA milestone.
- Shutdown stops the controller timer, closes coordinator ingress, cancels probe/provider work,
  drains the bounded transfer actor and acknowledgement relay, detaches the surface, and releases
  scene-graph/device resources. Actor and relay waits are limited to two seconds each, and the
  complete application shutdown budget remains seven seconds.

## 8. Testing

Application tests cover A/B completion in either order, single-side failure and cancellation,
incompatible pairs, stale contexts, exact-once command terminals, replacement preservation,
one-frame/boundary steps, presentation acknowledgement, and timeout recovery.

Platform component tests run with WARP and cover atomic mailbox replacement, generation rejection,
NV12 plane upload, BT.601/BT.709 full/limited color bars, aspect geometry, fence ordering, budget
release, acknowledgement-ring pressure, and render-thread nonblocking behavior. A source scan
rejects `QImage` includes in playback/render targets.

Qt tests cover local-URL conversion, controller properties, busy/disabled controls, shortcuts,
one-frame behavior, accessible names, and source-specific error presentation. An end-to-end test
uses deterministic fixtures to open a pair, present frame zero, step forward/backward, jump to
both ends, and assert zero split-pair presentations. Debug and Release tests, format, lint, install
deployment, `--ui-smoke`, and a real window launch remain required.

## 9. Acceptance Criteria

- A valid supported pair selected from the desktop displays its real frame-zero pixels.
- First, previous, next, and last always present the exact same canonical frame on A and B.
- Invalid replacement input cannot destroy a ready session; post-validation failures never report
  a false ready state.
- No GUI/render-thread blocking, partial pair, unbounded wait, stale commit, or frame-budget leak
  is present in the tested paths.
- The installed Release executable launches with all Qt, QML, FFmpeg, and new shader/runtime
  assets deployed beside it.
