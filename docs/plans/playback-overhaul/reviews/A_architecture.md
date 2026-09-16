# A — Architecture Verification (Chapter 08, §II)

**Role:** Independent architecture verifier. **Method:** every claim traced to code; the plan's prose is not trusted. **HEAD:** d04339c.

---

## Summary of verdicts

| # | Claim | Verdict |
|---|---|---|
| 1 | Layered dependency direction; Domain/Application leak no Qt/FFmpeg/Win32/D3D11 | **CONFIRMED** |
| 2 | Single serial media truth — one `PlaybackCoordinator`, no second media-truth owner | **CONFIRMED** |
| 3 | FrameSet atomicity — one canonical FrameSet per presentation, all sources together | **CONFIRMED** |
| 4 | ACK-as-truth — `displayedFrame` committed only after matching `FrameSetPresented` | **CONFIRMED** |
| 5 | Async identity completeness — session/epoch/topology/timeline/playback/device/request | **PARTIAL** |
| 6 | State-ownership concentration — 4 files + Main.qml oversized | **CONFIRMED** |

Overall architecture diagnosis: **Accept** (with required architecture tests — see §8).

---

## Claim 1 — Layered dependency direction

**Claim:** `domain → application → {presentation_contract, ui_models, ...}`; Domain/Application contain no Qt, FFmpeg, JSON, Win32, or D3D11 types. (`01_现状架构与证据审计.md` §2; `docs/architecture/dependency-rules.md`.)

**Evidence — include-graph audit:**

- Grep for forbidden includes (`Q*/Qt*`, `libav*`/`ffmpeg`, `d3d11`/`D3D*`, `Windows.h`, `nlohmann`/`json.hpp`) across `src/domain/` and `src/application/` — **both headers and `src/` implementation files — returned zero matches.**
- Grep for reverse edges (`#include "dvs/application/...` inside `src/domain/`) — **zero matches.** Domain never pulls application back in.
- `cmake/Architecture.cmake` is an **executable allow-list**: `_dvs_allowed_dependencies` (lines 16–68) encodes the inward-only graph, and `_dvs_reject_forbidden_core_dependency` (lines 70–79) FATALs on any Qt/FFmpeg/nlohmann/Win32/D3D dependency appearing in `dvs_domain` or `dvs_application`. Hidden generator-expression edges are also rejected (lines 96–107).

**Nuance / correction to the plan:** the plan's Mermaid diagram (`01_现状架构与证据审计.md` lines 41–50) draws edges `PC --> Gfx` and `Win --> Persist`, `Gfx --> UiBridge` that **do not appear** in the authoritative `dependency-rules.md` or `Architecture.cmake`. In the real rules, `graphics_d3d11` does **not** depend on `presentation_contract` the way the diagram implies, and `persistence_json` is not under `windows_support` in the allow-list (it allows `dvs_application dvs_windows_support`). The diagram is a hand-wave; the CMake file is the contract. **No dependency violation was found**, but the plan's diagram is an inaccurate rendition of it.

**Verdict: CONFIRMED.** The include graph holds and is machine-enforced at configure time.

---

## Claim 2 — Single serial media truth

**Claim:** `PlaybackCoordinator` is the only producer of `SessionSnapshot` and the only serialized command sink; exactly one coordinator/worker, no second media-truth owner.

**Evidence:**

- `PlaybackCoordinator.h:13` — `class PlaybackCoordinator final`; `PlaybackCoordinator.cpp:225` — `class PlaybackCoordinator::Impl final`. One class, `final`, non-copyable/non-movable (lines 31–34).
- One worker thread: `PlaybackCoordinator.cpp:232` `worker_ = std::thread([this]{ run(); });` — constructed once per `Impl`. `run()` (lines 3711–3733) is a single serial loop dequeuing `PlaybackCommand` then `ApplicationEvent`. Critical events drain before commands before realtime (`takeWorkLocked`, lines 3695–3709).
- One `CoordinatorPublication` owns the published snapshot: `PlaybackCoordinator.cpp:3807` `CoordinatorPublication publication_;`. `snapshot()` (line 262) reads `publication_.snapshot()`; `publishSnapshot()` (lines 634–641) is the only writer of the published state.
- **Single production instantiation:** `src/app/ReviewRuntime.cpp:256` is the only call to `PlaybackCoordinator::create` in production code, and it is guarded by `if (!coordinator_) throw` (lines 273–275). The `ShutdownWork::coordinator` (line 206) is the same moved-from instance.
- The only other `PlaybackCoordinator::create` call is `src/ui_qml/src/DirectCompare.cpp:188` — this is **not** a second media truth: it is a headless test/CLI diagnostic path that constructs a `CapturingRenderChannel` (line 187) and `DescriptorOnlyMediaProbe` (line 191), runs one comparison, and returns. It is never wired into the QML UI and never coexists with the runtime coordinator. It is a legitimate single-use offline sink, not a second owner of session truth.
- Command identity is additionally de-duplicated by `seenCommands_` (`PlaybackCoordinator.cpp:3805`, `CommandIdentity` of session+epoch+commandId) so a command can never be processed twice even if re-queued (`claimCommand`, lines 680–688).

**Verdict: CONFIRMED.** One coordinator, one worker, one publication, one production instantiation. The DirectCompare instance is an offline diagnostic, not a second truth owner.

---

## Claim 3 — FrameSet atomicity

**Claim:** One canonical FrameSet per presentation; all sources update together; an incomplete set is published with explicit `Missing` entries rather than dropped or partially advanced.

**Evidence:**

- `FrameSet.h:39–41` (doc comment): "Every source's frames for one canonical frame position, published atomically. The set always carries one entry per loaded source; an incomplete set is still published with explicit Missing entries instead of being dropped or partially advanced."
- `FrameSet::create` (`FrameSet.h:65–96`) is the **sole factory** and rejects anything that is not well-formed: invalid canonical id / negative time / empty sources → nullopt; duplicate `sourceId` → nullopt; `hasFrame()` inconsistent with `sourceFrameId` → nullopt; a present entry with `matchKind == Missing` or a present entry carrying a `missingReason` → nullopt; an absent entry that is not exactly `matchKind == Missing` with a `missingReason` → nullopt. There is **no** way to build a FrameSet that silently substitutes a neighbor frame.
- The commit paths advance the whole set or nothing:
  - `commitPresentedFrameIfComplete` (lines 2705–2780) copies the **entire** `FrameSet::sources()` into `state_.presentedSources` in one pass (lines 2720–2730) before a single `publishSnapshot()`, then sets `displayedFrame`.
  - `commitPlaybackFrameIfComplete` (lines 2810–2864) and `commitInteractiveStepFrameIfComplete` (lines 1056–1145) do the same bulk copy before publishing.
- There is **no** API on `IRenderChannel` to publish a single source: `Ports.h:195–201` — `publish(const FrameRequestContext&, FrameSet set)` takes the whole set; the comment explicitly states "no method for publishing one source of a set."

**Verdict: CONFIRMED.** Atomicity is enforced by the FrameSet factory and by whole-set publish/commit. No per-source advance path exists.

---

## Claim 4 — ACK-as-truth

**Claim:** `displayedFrame` is committed only after a matching `PresentationACK` (`FrameSetPresented`).

**Evidence — trace of the commit path:**

1. A frame must first be published to the renderer: `pending_->framePublished = true` is set only inside `handleFrameSet` after `renderChannel->publish(...)` returns Accepted (`PlaybackCoordinator.cpp:3105–3114`). The analogous flags in playback/interactive paths are `publishPlaybackFrameIfReady` (line 2800) and `publishInteractiveStepFrameIfReady` (line 1020).
2. The **only** place `framePresented` is set true is `handleFramePresented` (lines 3543–3567), which matches the `FrameSetPresented.context` and **verifies `presented.frameId == expectedFrame`** for each of the interactive (line 3546), playback (line 3554), and pending (line 3562) paths before setting `framePresented = true`.
3. Every `commit*IfComplete` function **gates on `framePresented`**:
   - `commitPresentedFrameIfComplete` line 2708, `commitPlaybackFrameIfComplete` line 2816, `commitInteractiveStepFrameIfComplete` line 1063 — each returns early unless `framePresented` is true.
4. The only assignments to `state_.displayedFrame` (grep lines 1091, 2746, 2772, 2837) are all **inside** those `commit*IfComplete` functions — i.e. only after ACK.
5. ACK origin: `D3d11ComparisonRenderer.cpp:1601–1606` constructs the `FrameSetPresented` after a real `acknowledge()` of a `FrameMailboxPublication`, pushes it to the `PresentationAckMailbox`, and the `RenderAckRelay` (`RenderAckRelay.cpp:181`) relays it into the coordinator's critical event sink. ACKs are serial-guarded by `highestAcknowledgementSerial_` (line 1605), so a late ACK for a superseded frame is rejected at the `frameId == expectedFrame` check in `handleFramePresented`.
6. Presentation timeout is a **failure**, not a silent keep-last-frame: `handleDeadline` lines 3588–3609 call `stopPlayback(presentationError(...))` / `failInteractiveStepRun(presentationError(...))` / `failPending(..., CommandOutcome::Failed)` — matching the v1.6 M1.2 cancel/fail split recorded in memory.

**Verdict: CONFIRMED.** `displayedFrame` moves only after a frameId-matched `FrameSetPresented`; timeout is a failure.

---

## Claim 5 — Async identity completeness

**Claim:** session/epoch/topology/timeline/playback/device/request identities exist and are checked on stale results.

**Evidence — identity types (`domain/Identifiers.h`):**

| Identity | Type | Line |
|---|---|---|
| session | `SessionId` (`CounterId<SessionIdTag>`) | 70 |
| epoch | `SessionEpoch` (`CounterId<SessionEpochTag>`) | 71 |
| playback generation | `PlaybackPlaybackGeneration` | 72 |
| device generation | `DeviceGeneration` (`CounterId<DeviceGenerationTag>`) | 73 |
| request | `RequestId` (`CounterId<RequestIdTag>`) | 74 |
| command | `CommandId` (`CounterId<CommandIdTag>`) | 75 |
| source | `SourceId` (`uint32_t`) | 80 |
| frame | `FrameId` | 14 |

**Topology / timeline are NOT separate identity types.** The plan uses the words "topology" and "timeline" as if they were first-class identities on par with session/epoch. In code, "topology" is the validated source set (`ValidatedComparisonSet`, held in `sources_`, `PlaybackCoordinator.cpp:3786`) and "timeline" is `CanonicalTimeline` (`canonicalTimeline_`, line 3790) — both **state**, not checked identity scopes. They are not part of `EventContext` and are not compared when admitting stale async results. This is a plan overclaim.

**Where identities ARE checked on stale results:**

- `RequestContext` = session + epoch + requestId (`RequestContext.h:9–15`); `PlaybackRequestContext` appends `playbackGeneration` (`RequestContext.h:19–24`); `FrameRequestContext` appends `deviceGeneration` (`RequestContext.h:26–31`). These nest so every async result carries the full scope.
- Admission checks compare **all** members, per the `Events.h:52` comment ("The coordinator must compare all members of that scope before accepting an asynchronous result"):
  - `acceptsCommand` (line 479) — session + epoch.
  - `matchesContext` (lines 124–134) — full `PlaybackRequestContext` / `FrameRequestContext` equality.
  - `matchesPending` (lines 2671–2681), `matchesPlaybackFrame` (line 2782), `matchesInteractiveStepFrame` (line 936), `matchesAnalysis` (lines 3118–3123) — each compares generation/device/jobId as appropriate.
  - `matchesAnalysis` also guards `progress.completedUnits` monotonicity and `work` equality (lines 3155–3161).

**Verdict: PARTIAL.** All real async identity scopes (session, epoch, playback generation, device generation, request, command, source, frame) exist and are rigorously compared on every stale-result admission. The plan's framing of "topology" and "timeline" as checked identities is **not** borne out by the code — they are session state, not request scopes. The substance (stale results are rejected) is correct; the ontology in the plan is slightly wrong.

---

## Claim 6 — State-ownership concentration

**Claim:** `PlaybackCoordinator.cpp`, `ReviewController.cpp`, `ReviewShellController.cpp`, and `Main.qml` are oversized (plan §4).

**Evidence — actual line counts:**

| File | Lines | Role |
|---|---|---|
| `src/application/src/PlaybackCoordinator.cpp` | **3878** | session/open/nav/play/step/align/ACK/device/shutdown |
| `src/ui_qml/src/ReviewController.cpp` | **1928** | full UI state projection + command submission |
| `src/ui_qml/src/ReviewShellController.cpp` | **860** | source intent, topology, reference, range, chrome |
| `src/ui_qml/qml/Main.qml` | **1528** | layout, state derivation, range loop, input, HUD |
| `src/platform_windows/src/D3d11ComparisonRenderer.cpp` | **1691** | layout, color convert, Wipe/Diff, draw resources |

**Q_PROPERTY counts (the plan's "oversized controller" claim):**

- `ReviewController.h`: **54** `Q_PROPERTY` declarations (lines 30–86), of which exactly **1 is `CONSTANT`** (`sources`); the other 53 fire `stateChanged` or `frameStateChanged`. This single C++ object backs **five** QML roles via `ReviewSessionFacade` — `playback`, `alignment`, `notifications` (all three are the **same** `ReviewController*`), plus `session`/`shell` (`ReviewShellController*`) and `comparison` (`ReviewPreferencesController*`). See `ReviewSessionFacade.h:17–22` — the façade explicitly collapses playback/alignment/notifications onto one controller.
- `ReviewShellController.h`: **26** `Q_PROPERTY` declarations.

**Verdict: CONFIRMED.** The concentration is real and slightly understated: the plan's table lists `ReviewShellController.cpp` at an implied large size but it is "only" 860 lines — still large for its narrow intent, but the bigger concern is that `ReviewController` (1928 lines, 54 properties, 3 façade roles) plus `Main.qml` (1528 lines of application state machine) carry the projection + QML state-machine load together.

---

## Dependency-rule violations found

**None.** Full include-graph audit (Qt/FFmpeg/Win32/D3D11/JSON forbidden in domain+application, both headers and `src/`; reverse domain→application edges) returned zero hits, and `cmake/Architecture.cmake` machine-enforces the inward-only graph at configure time.

---

## Plan architectural claims that are factually wrong (or imprecise)

1. **The dependency diagram is inaccurate** (`01_现状架构与证据审计.md` lines 41–50): edges `PC --> Gfx`, `Win --> Persist`, `Gfx --> UiBridge` do not match the authoritative `dependency-rules.md` / `Architecture.cmake`. The diagram is an informal sketch, not the contract. (No actual violation — but the plan should not present the diagram as if it were the rules.)
2. **"Topology / timeline" are not async identities** (plan §3.3): see Claim 5. They are session state, not request scopes checked on stale results. The anti-stale machinery is real; the plan's ontology overcounts it.
3. **The plan presents QML role collapse as a future migration** (`ReviewSessionFacade` "until their properties have moved behind narrow view-model classes") — in fact the collapse is **already** the current state (`ReviewSessionFacade.h:17–22`, playback/alignment/notifications all `ReviewController*`). The plan correctly notes this in §4, but §3 reads as if narrow view-models are the present reality. They are not.

---

## Required architecture tests (gaps the plan should fill)

These are absent or underrepresented and are needed to lock the architectural invariants the plan relies on:

1. **Dependency-edge CI test.** A test/CMake step that parses the actual link graph (or runs `dvs_validate_architecture` in a ctest) and FATALs on any Qt/FFmpeg/Win32/D3D11 edge into `dvs_domain`/`dvs_application`, and on any domain→application reverse edge. Currently this runs at CMake configure time but is **not** a ctest, so CI that skips reconfigure can miss a regression. (Foundation: `cmake/Architecture.cmake:81`.)
2. **No-second-coordinator test.** A test asserting that the production `ReviewRuntime` constructs exactly one `PlaybackCoordinator` and that no second object ever publishes a `SessionSnapshot` into the UI projection path. Guards Claim 2.
3. **FrameSet atomicity / no-partial-advance test.** A test that drives a multi-source session where one source is missing at a canonical position and asserts the published `SessionSnapshot.presentedSources` carries an explicit `Missing` entry rather than dropping the source or advancing `displayedFrame` for the others. Guards Claim 3.
4. **ACK-gated-displayedFrame test.** A test that publishes a `FrameSetReady` (and even a `RequestSucceeded`) for a frame, asserts `displayedFrame` is **unchanged**, then delivers the matching `FrameSetPresented` and asserts `displayedFrame` advances — plus a negative case where a mismatched/non-existent ACK does not advance it. Guards Claim 4.
5. **Stale-by-identity rejection test matrix.** For each identity scope (epoch superseded, playback generation changed, device generation changed, requestId mismatch), a test that a late async result is **not** applied. This is the substance behind Claim 5 and should be made explicit, dropping the "topology/timeline" ontology.
6. **`isConsistent()` on every published snapshot.** `SessionSnapshot::isConsistent()` (`SessionSnapshot.cpp:8–122`) is a thorough structural invariant but is **not** currently asserted on each publish. A test (or a debug-only assertion behind a gate) that calls `isConsistent()` on the snapshot after each state transition would turn the existing invariant into an enforceable architecture test.

---

## Decision

**Accept** the plan's architecture diagnosis.

The core structural claims — inward-only dependencies, single serial coordinator, atomic FrameSet, ACK-gated display, identity-scoped stale rejection, and concentrated ownership — are all **confirmed** against the code. The one partial (Claim 5) is a plan overclaim about ontology, not a defect in the actual machinery, which is rigorous. No dependency violations exist.

**Conditions / required revisions before implementation proceeds:**
- (a) Correct the dependency diagram to match `dependency-rules.md` / `Architecture.cmake`, or replace it with a generated graph from the CMake allow-list.
- (b) Re-word §3.3 to describe the real identity scopes (session/epoch/playback-generation/device-generation/request/command/source/frame) and drop "topology/timeline" as identities.
- (c) Add the six architecture tests above (especially #1 as a ctest, #4, and #6) to the plan's test matrix before the optimization work begins; without them, the invariants the plan relies on have no regression guard.
- (d) The plan's diagnosis of concentrated ownership is accepted as the motivation for the split; the façade role-collapse (playback/alignment/notifications → one `ReviewController`) should be listed as a **first**拆分 target, since it is load-bearing for three of the 54 properties and blocks clean view-model extraction.
