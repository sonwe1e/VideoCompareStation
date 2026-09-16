> **Status: executed.** Implemented and shipped. Retained for design-history context.

# Continuous Playback, CFR Verification, and Dark Controls Implementation Plan

**Date:** 2026-07-17

**Design:** `docs/archive/designs/2026-07-17-continuous-playback-cfr-dark-controls-design.md`

**Goal:** Deliver real pair-atomic play/pause, metadata-conflict CFR verification, and readable
dark toolbar menus without weakening exact-frame or threading contracts.

## 1. CFR Verification

**Files:**

- Modify `src/media_ffmpeg/src/FrameTimelineIndex.h`
- Modify `src/media_ffmpeg/src/FrameTimelineIndex.cpp`
- Modify `src/media_ffmpeg/src/MediaProbe.cpp`
- Modify `src/media_ffmpeg/src/SoftwareDecoder.cpp`
- Modify `tests/component/media/FrameTimelineIndexTests.cpp`
- Modify `tests/component/media/MediaProbeTests.cpp`
- Modify fixture generation/manifest only when a deterministic conflicting-metadata fixture is
  required

Add pure cadence-candidate validation tests first, including quantized 30 fps, 30000/1001,
non-zero PTS, true VFR, one frame, and overflow. Implement disputed-metadata fallback indexing and
candidate selection. Preserve the matching-declaration fast path. Mark fallback results
`kVerifiedCfr`, validate reported count, and make verified sources use their display-order PTS index
during exact decoding.

## 2. Sequential Decoder Fast Path

**Files:**

- Modify `src/media_ffmpeg/src/SoftwareDecoder.h`
- Modify `src/media_ffmpeg/src/SoftwareDecoder.cpp`
- Modify `src/media_ffmpeg/src/DirectFrameProvider.cpp`
- Modify `tests/component/media/SoftwareDecoderTests.cpp`
- Modify `tests/component/media/DirectFrameProviderTests.cpp`

Add tests proving adjacent and skipped forward requests avoid exact-seek behavior while preserving
frame identity. Add persistent decoder cursor/drain state and `decodeSequential`. Exact, reverse,
dirty, or discontinuous requests fall back to exact seeking. Select the fast path only for
`Sequential`; dirty both sides after partial-pair failure or cancellation. Verify cancellation and
frame-budget release.

## 3. Playback Coordinator

**Files:**

- Modify `src/application/include/dvs/application/Commands.h`
- Modify `src/application/src/PlaybackCoordinator.cpp`
- Modify `src/application/src/SessionSnapshot.cpp` only if an additional consistency invariant is
  needed
- Modify `tests/unit/application/ContractCompileTests.cpp`
- Modify `tests/unit/application/PlaybackCoordinatorTests.cpp`

Add Play/Pause command contract tests before implementation. Introduce a playback run separate from
exact-command pending state. Schedule absolute rational frame boundaries through the existing
steady scheduler, permit one sequential pair in flight, skip complete pairs when late, and require
provider/publish/presentation completion before advancing. Cover pause at every pipeline phase,
stale callbacks, timeout/error/device loss, end auto-pause, and replay from the end.

## 4. Controller and QML Playback UI

**Files:**

- Modify `src/ui_qml/include/dvs/ui/ReviewController.h`
- Modify `src/ui_qml/src/ReviewController.cpp`
- Modify `src/ui_qml/qml/Main.qml`
- Modify `tests/component/ui/ReviewControllerTests.cpp`
- Modify `tests/component/ui/DualVideoSurfaceTests.cpp` or the existing UI shell smoke harness

Project `playing`, `canPlay`, and `canPause`; expose `play()`, `pause()`, and `togglePlayback()`.
Playback must not set general busy. Insert the transport button, bind Space, disable navigation and
timeline during active/draining playback, and keep Difference controls live. Test accessible labels,
state transitions, shortcut behavior, and absence of the loading overlay.

## 5. Dark Toolbar ComboBox

**Files:**

- Add `src/ui_qml/qml/ToolbarCombo.qml`
- Modify `src/ui_qml/qml/Main.qml`
- Modify `src/ui_qml/dvs_ui_qml_resources.qrc`
- Modify `src/ui_qml/CMakeLists.txt` if the QML module file list requires it
- Add focused UI tests under `tests/component/ui`

Build a `QtQuick.Templates` ComboBox with explicit dark text, delegates, highlight, popup, focus,
indicator, and accessibility. Replace all inline toolbar combo instances. Test Windows and Basic
styles, keyboard operation, popup dismissal, and at least 4.5:1 normal/highlight contrast.

## 6. Integration and Release Verification

Run repository formatting before compilation, then:

```powershell
cmake --build --preset dev
ctest --preset dev --output-on-failure
cmake --build --preset dev --target format-check
cmake --build --preset dev --target lint
cmake --build --preset release
ctest --preset release --output-on-failure
cmake --install out/build/release --prefix out/install/release
```

Probe and compare each known real video against itself with `DualVideoStudioCli.exe`; verify first,
middle, and final frames. Run packaged UI smoke and PE-subsystem checks. Launch the installed GUI,
exercise play/pause and Difference menus in dark mode, and report any validation that cannot be
automated. Preserve the existing license/provenance package gate.
