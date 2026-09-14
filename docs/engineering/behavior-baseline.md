# Behavior Baseline (Phase 0)

> Frozen behavioral facts about the current `main @ d04339c` playback/compare engine, recorded
> before the optimization plan changes anything. Each item is tagged **C** (confirmed from code),
> **H** (high-probability, needs hardware/runtime confirmation), or **G** (capability gap). The
> Phase 0 trace exists to make the **C** items observable and to give every later phase a "did it
> get better or just different" measuring stick.

## Reference / canonical timeline (C-01) — P0

- `ComparisonValidator.cpp:195`: `canonicalId = referenceId.value_or(sources.front().id)`. The
  Reference source **is** the canonical timeline master.
- Consequence: changing Reference rebuilds the canonical timeline, frame count, frame rate, Range
  remap, alignment baseline, one-second-step size, and timecode — far more than "change the GT".
- Baseline metric: a `SetReference`-style change today alters `canonicalFrameCount` and
  `canonicalRate`. After Phase 1, it must not.

## Active Pair as global ordinal (C-02) — P0

- `ReviewPreferencesController.cpp:37,533-544`: the active pair is persisted as a global
  `review.difference-edge` string `"0-1"/"0-2"/"1-2"` (slot ordinals), not stable SourceKeys.
- `ReviewShellController.cpp:847-855`: on a 2-source session the invalid edge is projected to
  `0-1` but the original preference is **retained**, so re-adding a 3rd source silently restores the
  old pair.
- Baseline metric: a 3-source → 2-source → 3-source cycle today restores the stale ordinal pair.
  After Phase 1, the pair must be expressed in stable SourceKeys and never leak across topology.

## Forward / reverse step asymmetry (C-03) — P0

- `PlaybackCoordinator.cpp:2135-2154` (`beginStep`): only `delta == 1` uses the provider's
  `Sequential` decode path; backward (`-1`) and multi-frame jumps use `beginSeek` → Exact.
- Baseline profile (held-forward, plan 1.6 M1.1, `Main.cpp` `Stage::HeldStepping`): 300
  consecutive `+1` steps, 25–35 Hz, measuring `held_step_p50/p99`, sequence errors, generation delta,
  exact-seek delta, decoder reopen. Forward today is a smooth Sequential stream.
- Baseline profile (held-backward, **new** Phase 0 test): 300 consecutive `-1` steps. Today each is
  an Exact seek → generation/cancel storm, higher latency, possible missing intermediate frames.
  After Phase 3, held-backward must match held-forward (0 missing intermediate, 0 stale commit,
  0 partial FrameSet, warm-up decoder reopen = 0).

## Range Loop authority (C-04) / out-of-bounds risk (H-01) — P0

- `Main.qml:267-284` (`onCurrentFrameChanged`) is the **only** range-loop driver; the coordinator has
  no Range concept. `playbackTargetAt` (`PlaybackCoordinator.cpp:850-898`) clamps to the canonical
  end but **not** to the Out point, and `commitPlaybackFrameIfComplete` (`:2810-2864`) only checks
  the canonical end.
- H-01 (confirmed by code structure): after a stall > `kPlaybackCatchUpTolerance` (2000ms), catch-up
  can select a target past Out; that frame is committed (`displayedFrame` assigned at `:2837`) before
  QML observes it and seeks back — so a frame outside [In,Out] can be presented. Needs a fake-clock
  red test to reproduce deterministically.
- Baseline metric: with a deterministic fake clock + renderer ACK, a >2000ms stall during range
  playback today can present `Out+Δ`. After Phase 3, `displayedFrame` must never exceed `Out`.

## FrameSet atomicity + ACK-as-truth (plan invariants 1, 2) — preserved

- `FrameSet::create` is the sole factory and rejects partial/duplicate/malformed sets. There is no
  per-source publish on `IRenderChannel`. `displayedFrame` is only assigned inside
  `commit*IfComplete` functions, each gated on `framePresented`, which is only set when a
  `FrameSetPresented` matches both context and frame id.
- Baseline: these are already correct; the trace must keep proving them.

## State-ownership concentration (C-06) — P0

- `PlaybackCoordinator.cpp` 3878 LOC; `ReviewController.cpp` 1928 LOC with **54** `Q_PROPERTY`s;
  `ReviewShellController.cpp` 860 LOC; `Main.qml` 1528 LOC; `D3d11ComparisonRenderer.cpp` 1691 LOC.
- `ReviewSessionFacade.h:17-22`: `playback`/`alignment`/`notifications` all point at the same
  `ReviewController`. Baseline: adding one property touches controller + view + signal + QML.

## Hidden catch-up (C-07) — P1

- `kPlaybackCatchUpTolerance = 2000ms` (`PlaybackCoordinator.cpp:51`). Within it, every canonical
  frame is presented (clock slips); beyond it, whole FrameSets are skipped. The switch is implicit —
  no UI shows which mode is active, no dropped/late count is exposed.

## 3-source hardcoding (C-09) — P0

- `kMaximumSources = 3U` plus ~20 sites across validator, presentation-contract, QML properties,
  `DifferenceEdge`, renderer slots, GPU buffers, and `app` error aggregation (enumerated in the
  migration-surface survey). Raising the constant alone breaks arrays, enums, and the renderer.

## Thumbnail (C-05) — P1

- `TimelineThumbnailCache.qml:48` uses `grabToImage` on the already-displayed canvas; hovering an
  never-played position yields no preview. No independent thumbnail decoder.

## Accessibility (C-08) — P1

- `WipeHandle.qml`: no `activeFocusOnTab`, no `Accessible.role`/value, no cursor keys.
- `TimelineTracks.qml`: has `Accessible.role`/`name` but no `Accessible.value`/keyboard.

## Performance baseline (existing, must not regress)

- Held-forward step gate (plan 1.6 M1.1): 300 × +1 at 25–35 Hz; hard gates 0 missing intermediate
  FrameIds, 0 stale commits, 0 decoder reopen. Phase 0 records the raw trace + exact-seek/sequential
  request counts as the comparison baseline for Phase 3's held-backward work.
- Cold/warm exact seek, 1080p60/120 hardware gates: unchanged by Phase 0 (trace is default-off).

## Trace-observable acceptance criteria (Phase 0 exit)

A valid Phase 0 trace of a held-forward and a held-backward run must satisfy, exportable from one
JSONL file and checked by `Test-PlaybackTraceInvariants`:

- `command_terminal_mismatch_count = 0` (every accepted command has exactly one terminal).
- `ack_before_commit_violations = 0` (no `SnapshotCommitted` before its `PresentationAcknowledged`).
- `stale_commit_count = 0` (commit identities do not regress within a session/epoch).
- `partial_frame_set_count = 0` (reported as 0; multi-source FrameSet shape is not reconstructible
  from the single-line stream and remains a C++ factory invariant).
- `overflow_count` recorded; trace on/off A/B shows no gate regression.
