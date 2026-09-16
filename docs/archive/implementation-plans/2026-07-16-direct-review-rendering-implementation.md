> **Status: executed.** Implemented and shipped. Retained for design-history context.

# Direct Review and D3D11 Rendering Implementation Plan

## Objective

Deliver the approved first review slice: choose two local CFR videos, probe and validate them asynchronously, decode an exact frame pair, upload it to D3D11, and display it atomically in one Qt Quick surface. Home, End, Left, and Right must move exactly once and a command must not complete until the corresponding pair is presented.

The implementation keeps domain/application code free of Qt and D3D types. CPU decoding remains the accepted baseline; the new GPU boundary is reusable by a later D3D11VA decoder. No `QImage`, CPU RGB conversion, or render-thread waiting is permitted.

## Baseline and Test Loop

Run from an x64 Visual Studio developer shell with `VCPKG_ROOT` set:

```powershell
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
```

For each task: add the narrow failing test, build that target, implement the minimum production change, rerun its test filter, then run `ctest --preset dev`. Use `git diff --check` before every checkpoint.

## Task 1: Extend Application Contracts

**Modify:**

- `src/application/include/dvs/application/Commands.h`
- `src/application/include/dvs/application/Events.h`
- `src/application/include/dvs/application/SessionSnapshot.h`
- `src/application/src/PlaybackCoordinator.cpp`
- `src/application/src/SessionSnapshot.cpp`
- `src/domain/include/dvs/domain/MediaError.h`
- `src/domain/src/MediaError.cpp`
- `tests/unit/application/ContractCompileTests.cpp`
- `tests/unit/domain/MediaDescriptorAndErrorTests.cpp`

Add `OpenSourcePathsCommand` with normalized local paths, a session-independent `GraphicsEventContext` containing only `deviceGeneration`, typed graphics-ready/unavailable/lost events, `graphicsReady` in the snapshot, and public presentation timeout/graphics error codes and operations. Startup graphics events do not carry a session/epoch or native handle; unavailable/lost events carry a normalized `MediaError`, with HRESULT text restricted to `technicalDetail`. Add compile-time variant membership checks and snapshot consistency tests before implementation. Update the coordinator visitor in the same checkpoint so every new variant is handled explicitly and all existing callers still compile.

**Checkpoint:** `feat(application): define direct review presentation contracts`

## Task 2: Orchestrate Probe and Pair Validation

**Modify:**

- `src/application/include/dvs/application/PlaybackCoordinator.h`
- `src/application/src/PlaybackCoordinator.cpp`
- `src/platform_windows/CMakeLists.txt`
- `tests/component/platform/CMakeLists.txt`
- `src/ui_qml/src/DirectCompare.cpp`
- `tests/unit/application/PlaybackCoordinatorTests.cpp`

**Create:**

- `src/platform_windows/include/dvs/platform/SteadyDeadlineScheduler.h`
- `src/platform_windows/src/SteadyDeadlineScheduler.cpp`
- `tests/component/platform/SteadyDeadlineSchedulerTests.cpp`

First implement one deadline scheduler worker with a condition variable, linearizable cancellation, and non-blocking rescheduling; do not create or join one sleeping thread per timer. Then inject `IMediaProbe`, `IFrameProvider`, `IRenderChannel`, and `IDeadlineScheduler`. Update every coordinator factory/caller in this checkpoint, including the CLI direct-comparison adapter, using explicit headless test doubles where graphics acknowledgement is required. For a path command, start A and B probes with distinct request contexts, accept either completion order, cancel the sibling on failure, validate only a complete candidate, then open the provider exactly once. Preserve the prior ready session while a new pair is only probing or fails validation. Reject every stale callback by full command/request/device context.

Tests cover A-first, B-first, probe failure, cancellation, incompatible media, exact-once provider open, same-file comparison, replacement success/failure, and stale events.

**Checkpoint:** `feat(application): probe and validate source paths`

## Task 3: Make Presentation Acknowledgement Authoritative

**Modify:**

- `src/application/src/PlaybackCoordinator.cpp`
- `src/ui_qml/src/DirectCompare.cpp`
- `tests/unit/application/PlaybackCoordinatorTests.cpp`

Submitting an exact frame request starts a five-second presentation deadline so a provider that never publishes cannot leave the command unbounded. Commit `displayedFrame` and emit command success only after both the provider terminal and a matching `FramePairPresented`. A matching timeout cancels the generation, retains the last rendered pixels, and emits `kFramePresentationTimedOut`; late acknowledgements are ignored. Clamp First/Previous/Next/Last, including one-frame media, and allow only one active UI command. Adapt the CLI comparison channel to acknowledge the captured pair explicitly so its smoke test still exercises the real completion contract.

**Checkpoint:** `feat(application): complete exact commands after presentation`

## Task 4: Carry Color Metadata and Enforce the Frame Budget

**Modify:**

- `src/platform_windows/include/dvs/platform/CpuNv12FrameResource.h`
- `src/platform_windows/include/dvs/platform/FrameResourceFactory.h`
- `src/platform_windows/src/CpuNv12FrameResource.cpp`
- `src/platform_windows/src/FrameResourceFactory.cpp`
- `src/media_ffmpeg/src/SoftwareDecoder.cpp`
- `tests/component/platform/CpuNv12FrameResourceTests.cpp`
- `tests/component/platform/FrameBudgetTests.cpp`
- `tests/component/media/SoftwareDecoderTests.cpp`

Copy the already-normalized `MediaDescriptor::colorMetadata` into each immutable decoded resource. Do not reinterpret FFmpeg fields or invent a second fallback in the decoder/render path: `MediaProbe` remains the sole normalization point and the documented height-based BT.601/709 compatibility rule remains authoritative. Account for both planes, both sources, upload staging, and retained front/back pairs under the 256 MiB budget. Reject allocations before copying bytes.

**Checkpoint:** `feat(media): preserve nv12 color metadata and budget`

## Task 5: Define GPU Pair Ownership and Non-blocking Mailboxes

**Create:**

- `src/platform_windows/include/dvs/platform/FrameMailbox.h`
- `src/platform_windows/include/dvs/platform/PresentationAckMailbox.h`
- `src/platform_windows/src/GpuFrameResource.h`
- `src/platform_windows/src/GpuFramePair.h`
- `src/platform_windows/src/FrameMailbox.cpp`
- `src/platform_windows/src/PresentationAckMailbox.cpp`
- `src/platform_windows/src/GpuFrameResource.cpp`
- `src/platform_windows/src/GpuFramePair.cpp`
- `tests/component/platform/FrameMailboxTests.cpp`
- `tests/component/platform/PresentationAckMailboxTests.cpp`

**Modify:** `src/platform_windows/CMakeLists.txt`, `tests/component/platform/CMakeLists.txt`

Define the platform-private GPU frame/pair ownership model before the upload actor: a pair owns both sources and their D3D resources, a published mailbox value pins one complete pair, replacement releases the displaced reservation, and device-generation mismatch drops the entire pair. Budget reservations explicitly cover live CPU planes, staging, GPU textures, and retained front/back pairs and are released by RAII on every failure path. Implement an atomic latest-complete-pair mailbox for producer-to-render transfer and a lossless two-entry SPSC acknowledgement queue. Neither consumer may block. Tests include overwrite behavior, generation filtering, queue pressure, reservation release, and shutdown.

**Checkpoint:** `feat(platform): add presentation mailboxes and deadlines`

## Task 6: Build the D3D11 Device and Upload Path

**Create:**

- `src/platform_windows/include/dvs/platform/GraphicsDeviceBroker.h`
- `src/platform_windows/include/dvs/platform/GpuTransferActor.h`
- `src/platform_windows/include/dvs/platform/D3d11RenderChannel.h`
- `src/platform_windows/src/GraphicsDeviceBroker.cpp`
- `src/platform_windows/src/GpuTransferActor.cpp`
- `src/platform_windows/src/D3d11RenderChannel.cpp`
- `src/ui_qml/include/dvs/ui/GraphicsBackend.h`
- `src/ui_qml/src/GraphicsBackend.cpp`
- `tests/component/ui/MinimalQuickRenderHarness.h`
- `tests/component/platform/GraphicsDeviceBrokerTests.cpp`
- `tests/component/platform/GpuTransferActorTests.cpp`

**Modify:** `src/platform_windows/CMakeLists.txt`, `tests/component/platform/CMakeLists.txt`

Also modify `src/ui_qml/CMakeLists.txt`, `tests/component/CMakeLists.txt`, and `tests/CMakeLists.txt` to build the graphics bootstrap and hidden Quick harness in this checkpoint.

Add `configureGraphicsBackend()` and call it before any `QGuiApplication` construction. The test harness creates a real off-screen but exposed `QQuickWindow`, initializes its scene graph with WARP, and invokes the broker on the render thread; a truly hidden window is not a reliable scene-graph bootstrap. Task 6 must not fake or independently create the production Qt device. Acquire Qt Quick's D3D11 device/context through the public renderer interface, require multithread protection, and track a monotonic device generation. The transfer actor uploads complete A/B NV12 plane textures off the render thread, records D3D11 event queries, and publishes only fence-complete immutable GPU pairs. Device removal posts a typed loss event; WARP-backed tests verify texture dimensions/content, fence polling, generation rejection, budget release, and bounded shutdown.

GPU-pair retirement is deferred back to the transfer actor: releasing the render publication must never perform the final D3D COM destruction on the render thread. The actor drains retired pairs before releasing the Qt device during bounded shutdown.

**Checkpoint:** `feat(platform): upload atomic nv12 pairs with d3d11`

## Task 7: Render One Atomic Split Surface

**Create:**

- `src/ui_qml/include/dvs/ui/DualVideoSurface.h`
- `src/ui_qml/src/DualVideoSurface.cpp`
- `src/ui_qml/shaders/Nv12ToRgb.hlsl`
- `src/ui_qml/shaders/Compose.hlsl`
- `src/ui_qml/src/RenderAckRelay.cpp`
- `tests/component/ui/DualVideoSurfaceTests.cpp`

**Modify:**

- `src/ui_qml/CMakeLists.txt`
- `tests/component/CMakeLists.txt`
- `tests/CMakeLists.txt`

Keep `DualVideoSurface` as a thin Qt-facing `QQuickItem`; the Windows platform backend owns D3D resources, transfer, mailbox consumption, and the render-node implementation so native types do not leak into the controller/application layers. The item consumes only complete GPU pairs, renders source A into the left half and B into the right half, aspect-fits each source with black letterboxing, and applies matrix/range-aware NV12 conversion. The render node performs no decoding, allocation-heavy copies, I/O, or waits. After drawing both halves it pushes one acknowledgement; a GUI-thread relay posts the critical event.

Use build-generated shader bytecode or Qt's supported offline shader pipeline—never compile shaders per frame. WARP tests inspect deterministic pixels for full/limited range, BT.601/709, unequal aspect ratios, odd viewport sizes, zero-width split gaps, mailbox replacement, and acknowledgement pressure. Add a source guard that fails if playback/render code includes `QImage`.

**Checkpoint:** `feat(ui): render atomic dual nv12 surface`

## Task 8: Expose an Immutable Review Controller

**Create:**

- `src/ui_qml/include/dvs/ui/ReviewController.h`
- `src/ui_qml/src/ReviewController.cpp`
- `tests/component/ui/ReviewControllerTests.cpp`

Convert local `QUrl` values to canonical paths, assign command IDs, dispatch commands, drain terminal results, and project immutable snapshots into Qt properties. Expose selected filenames, operation state, current/total frames, `busy`, `graphicsReady`, source-specific error keys, and slots for open/first/previous/next/last. A 16 ms Qt timer may only drain/project state; it is not a playback clock.

Tests cover local/non-local URLs, missing paths, busy gating, source-role errors, one-frame state, stale terminals, and keyboard command dispatch.

**Checkpoint:** `feat(ui): expose review workflow to qml`

## Task 9: Replace the Shell with the Review Workflow

**Modify:**

- `src/ui_qml/qml/Main.qml`
- `src/ui_qml/dvs_ui_qml_resources.qrc`

Add two `FileDialog` selectors, explicit **Open Pair**, the single `DualVideoSurface`, loading/error overlays, source names, frame counter, and four exact navigation buttons. Bind Home/End/Left/Right, disable commands while busy or graphics is unavailable, and provide accessible names. Keep the previous dark visual language but remove all fake preview/status values.

**Checkpoint:** `feat(ui): connect direct review controls`

## Task 10: Compose, Shut Down, and Exercise the Real App

**Modify:**

- `src/app/CMakeLists.txt`
- `src/ui_qml/src/DesktopApplication.cpp`
- `src/app/Main.cpp`
- `src/ui_qml/src/DirectCompare.cpp`
- `tests/smoke/CMakeLists.txt`
- `README.md`

**Create:**

- `src/app/ReviewRuntime.h`
- `src/app/ReviewRuntime.cpp`

Create `src/app/ReviewRuntime.{h,cpp}` as the one composition root owning probe, provider, coordinator, scheduler, broker, transfer actor, channel, acknowledgement relay, controller, and surface bridge. Refactor the UI entry into a small `DesktopApplication` lifecycle object: `Main.cpp` calls `configureGraphicsBackend()` before it constructs the hidden `QGuiApplication` implementation, then passes non-owning controller/surface interfaces to the QML host. The UI adapter never owns media/provider workers. Publish graphics readiness only after scene-graph initialization. Shut down in the approved order: timer/ingress, probes/provider, actor/relay, surface, scene graph, with two-second adapter and seven-second total bounds.

Extend deterministic smoke coverage to open fixture A/B, wait for frame zero, step once, jump to last/first, and verify the presented indices. Update the README milestone and local run command.

**Checkpoint:** `feat(app): deliver direct review vertical slice`

## Task 11: Release Verification

Run all required gates:

```powershell
cmake --build --preset dev
ctest --preset dev --output-on-failure
cmake --build --preset dev --target format-check
cmake --build --preset dev --target lint
cmake --build --preset release
ctest --preset release --output-on-failure
cmake --install out/build/release
out\install\release\DualVideoStudio.exe --ui-smoke
```

Then launch the installed executable with a real window, confirm it remains open, select the two H.264 fixtures, and verify frame 0, next, previous, last, and first. Record any hardware-only test exclusions explicitly; WARP coverage remains mandatory in normal CI.

**Final checkpoint:** `test: verify direct review release slice`

## Completion Criteria

The slice is complete only when the installed executable stays open, can render the deterministic A/B fixture pair without `QImage`, reports exact frame indices only after presentation acknowledgement, preserves the prior session on invalid replacement, handles D3D11 unavailability/device loss truthfully, and passes Debug, Release, format, lint, smoke, and WARP-backed tests.
