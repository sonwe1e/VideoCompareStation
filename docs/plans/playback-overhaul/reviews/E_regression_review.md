# E｜Regression / Edge-case Review — VCStation Optimization Plan

**Role:** E — Regression / Edge-case Review (Plan Ch. 08, §VI)
**Review commit:** d04339c (HEAD) / latest release dd21a4c (v1.6.0)
**Toolchain:** C++20, Qt Quick/QML, FFmpeg, D3D11, Google Test, Windows x64
**Scope:** Determine what existing tests guard the invariants the plan must preserve, find gaps where NEW tests are needed, and assess regression risk of the plan's phase changes. Evidence drawn from actual code, tests, and release history.

---

## §1 Existing-Test Inventory per P0 Area

The `tests/` tree contains **50 gtest .cpp files** (49 carrying `TEST`/`TEST_CASE` macros; `ShellInvokeProbe.cpp` has none) holding **~403 test cases**, plus 11 Qt-Test `.qml` files in `component/ui/qml/`. Test counts per directory:

| Directory | .cpp files | Suite / case count |
|---|---|---|
| `unit/application` | 6 | ~104 |
| `unit/domain` | 7 | ~38 |
| `unit/persistence_json` | 2 | ~4 |
| `unit/app` | 2 | ~10 |
| `unit/shell_windows` | 1 | ~3 |
| `unit/presentation_contract` | 1 | ~4 |
| `component/media` | 8 | ~90 |
| `component/platform` | 9 | ~50 |
| `component/ui` | 6 | ~96 |
| `component/shell_windows` | 2 | ~2 (1 file has no tests) |

Below, "count" = number of test cases in that file that meaningfully exercise the P0 area (not necessarily the whole file).

### 1.1 Comparison Semantics / Reference / Pair

| Test file | Representative test names | Count |
|---|---|---|
| `unit/presentation_contract/ComparisonContractTests.cpp` | PreservesPublishedPresentationEnumValues, DescribesExactlyTheCurrentOneTwoAndThreeSourceModes, FallsBackToSingleOrSideBySideForUnavailableModes, ValidatesCompleteComparisonAndViewportState | 4 |
| `unit/domain/ComparisonValidatorTests.cpp` | ReferenceRoleWinsCanonicalSelection (L84), AcceptsTwoIdenticalSourcesWithFirstAsCanonical (L62), AcceptsThreeSourcesAndExposesAllById (L94), RejectsTwoReferenceRoles (L134), RequiresAlignmentForFrameCountMismatchWithoutBlocking (L144) | 12 |
| `unit/application/ComparisonExactnessTests.cpp` | DistinguishesExactDisplaySpatialTemporalAndUnavailableStates, TreatsChromaOrRgbNormalizationAsDisplaySpaceConversion | 2 |
| `component/ui/MainQmlContractTests.cpp` | InstantiatesRootAndSeparatesManualAlignmentStates (persisted Edge0And2 → Edge0And1 resolution, L287-290) | 6 (file total) |
| `component/ui/ReviewControllerTests.cpp` | DispatchesThreeSourcesWithTheSelectedReferenceRole (L404), ChangeReferenceRebuildsTheCanonicalTimelineFromActiveSources (L488), FailedCandidateOpenLeavesActiveSourcesAndReferenceUnchanged (L456) | 38 (file total) |
| `component/ui/ComparisonSurfaceTests.cpp` | WipeUsesTheSelectedPairAndDraggableSplitPosition (L1576), RendersThreeSourcesAndSelectedPairwiseDifferenceEdges (L1532) | 41 (file total) |
| `component/ui/ReviewPreferencesControllerTests.cpp` | DefaultsThenProjectsValidPersistedValues (L128), setDifferenceEdge Edge0And2 persistence (L205-219) | 5 |
| `component/media/MultiSourceFrameProviderTests.cpp` | Pair/complete-set publishing tests | 25 (file total) |

**Concepts with NO dedicated test:** `ValidatedComparisonSet` (only used as a type in fixtures), `ComparisonSelection`, `SourceKey`, `SessionTopology`, `DirectedSourcePair` — these are plan-introduced types; no test exists yet (expected, they are Phase 1 deliverables).

### 1.2 Step / Navigation / Reverse

| Test file | Representative test names | Count |
|---|---|---|
| `unit/application/PlaybackCoordinatorTests.cpp` | RapidForwardStepsEnqueueAndPresentEveryFrame (L3584), ForwardStepUsesOneGenerationAcrossAdjacentCommands (L4145), ForwardStepPresentsEveryIntermediateFrame (L4239), ForwardStepCompletesEachCommandAfterPresentation (L4285), BackwardStepStopsForwardStreamAndUsesExact (L4336), ProviderFailureSurfacesStepStreamError (L4524), SupersedingStepDoesNotSurfaceError (L4562), DeviceLossInvalidatesForwardStepStream (L4604), ForwardStepFallsBackToExactAtDiscontinuity (L4791), ForwardStepPresentationTimeoutFailsStream (L4828), ForwardStepPreparedFrameMismatchRequeuesCommand (L4872) | ~24 |
| `component/ui/ReviewControllerTests.cpp` | GenericStepAndSeekDispatchExactCommandsAndRejectInvalidTargets (L1404), NavigationDispatchesExactVariantsAndClampsUiAtBoundaries (L1370), NavigationStaysAvailableAndLatestContextClearsFramePending (L1008), OneFrameKeepsFirstAndLastButDisablesMovement (L1343) | ~4 |
| `component/media/MultiSourceFrameProviderTests.cpp` | PublishesForwardSequentialPairsAcrossPlaybackGenerations (L624), SequentialPlaybackDoesNotRetainHistoricalCpuPairs (L669) | ~4 |
| `component/media/SourceDecodeActorTests.cpp` | SequentialRequestReusesAPrefetchedSourceFrameWithoutDecodingAgain (L236), SequentialReadAheadFillsOnlyTheSourceFrameCache (L275) | ~3 |
| `component/media/SoftwareDecoderTests.cpp` | ContinuesForwardWithoutSeekingAndFallsBackForReverseTargets (L475), VfrSequentialWalkAcrossLongPtsGapReproducesExactFrames (L601) | ~3 |

**Concepts with NO coverage:** `InteractiveStep`, `beginStep`, `stepFrame` (method-level) — no test references these exact identifiers. Reverse-step sequential stream is a confirmed GAP (only exact-seek path exists for -1; see `BackwardStepStopsForwardStreamAndUsesExact` L4336).

### 1.3 Range Loop

| Test file | Representative test names | Count |
|---|---|---|
| `component/ui/ReviewControllerTests.cpp` | ShellOwnsRangeStateAndPreservesItsOrderingInvariants (L917) — setRangeIn/Out, inFrame/outFrame, rangePlaybackActive, remapRange, clearRange | 1 primary |
| `component/ui/MainQmlContractTests.cpp` | InstantiatesRootAndSeparatesManualAlignmentStates (setInPoint, loopRangeButton/clearRangeButton visibility) | partial |

**Confirmed GAP:** No test exercises a range-loop *command lifecycle* (set range → play → loop → clear → failure). The range-loop authority currently lives in QML (`Main.qml.onCurrentFrameChanged`), which the plan (Phase 3.3) intends to migrate to the kernel. There is no `RangePlaybackWorkflow` / `SetPlaybackRangeCommand` / `SetRangeLoopCommand` test — these are Phase 3 deliverables, and the absence of a failing test today is itself a gap (see §3, `RangeLoopNeverPresentsOutsideInclusiveRange`).

### 1.4 Façade / ViewModel

| Test file | Representative test names | Count |
|---|---|---|
| `component/ui/ReviewControllerTests.cpp` | All 38 cases exercise `ReviewController` + `ReviewShellController` | 38 |
| `component/ui/MainQmlContractTests.cpp` | All 6 cases wire `ReviewSessionFacade`, `ReviewController`, `ReviewShellController`, `ReviewPreferencesController` to live QML | 6 |

**Confirmed GAP:** No `ViewModel` class exists yet (Phase 2 deliverables: `SessionViewModel`, `PlaybackViewModel`, `TimelineViewModel`, `ComparisonViewModel`, etc.). `Facade` is only exercised via `ReviewSessionFacade` in `MainQmlContractTests`. No dedicated façade unit test file.

### 1.5 ACK / FrameSet Atomicity

| Test file | Representative test names | Count |
|---|---|---|
| `component/platform/PresentationAckMailboxTests.cpp` | PreservesTwoEntriesUnderPressureWithoutOverwriting (L39), CloseRejectsNewEntriesButAllowsQueuedEntriesToDrain (L63), SingleProducerAndConsumerTransferEveryEntryInOrder (L81), ConcurrentCloseEitherPublishesBeforeCloseOrRejectsAtomically (L105) | 4 |
| `component/platform/FrameMailboxTests.cpp` | ReplacesThePublishedSetAndAcceptsPartialSets (L126), RejectsStaleGenerationsAndAdvancingGenerationClearsTheSet (L183), SetDerivesIdentityAndRejectsContextFrameSourceAndAliasingErrors (L332) | 8 |
| `component/ui/RenderAckRelayTests.cpp` | CoalescesFrameNotificationsIntoOneQueuedItemUpdate (L157), CountsCanonicalGapsOnlyWithinOnePlaybackScope (L211), CountsRegressionButResetsSequenceForAnotherPlaybackGeneration (L229), AcknowledgesOnlyTheLatestReplacementAndRetriesAFullQueueOnce (L2120) | 8 |
| `unit/application/PlaybackCoordinatorTests.cpp` | ReleasesCpuFrameResourcesAfterPresentationCommit (L3467), RequiresProviderTerminalAndPresentationInEitherOrder (L2994), PresentationTimeoutKeepsPreviousFrameAndRejectsLateAck (L3175), ExactDeadlineBoundsAProviderThatNeverPublishesAFrameSet (L3125) | ~15 |
| `unit/persistence_json/CommitSemanticsTests.cpp` | CancellationBeforePublicationClaimWins (L16), PublicationClaimPreventsLaterCancellation (L25) | 2 |
| `component/media/FrameSetAssemblerTests.cpp` | RefusesPartialSetsAndPublishesInSessionSourceOrder (L18), RejectsUnknownAndDuplicateSourceCompletions (L39) | 2 |
| `component/media/MultiSourceFrameProviderTests.cpp` | OpensDirectSourcesAndPublishesOnlyACompleteExactPair (L264) | ~6 |
| `component/platform/GpuTransferActorTests.cpp` | UploadsPaddedOddNv12PlanesAndPublishesOneImmutablePair (L295), UploadsThreeSourceFrameSetsAtomically (L620) | ~5 |

**Concepts with NO coverage:** `PresentedSourceState` state machine (only used in fixtures). ACK-as-a-standalone concept is otherwise well covered via mailbox/relay tests.

---

## §2 Historical-Regression Coverage

Cross-referenced against plan Ch. 01 §5 (root-cause patterns), Ch. 08 §VI.3 (regression checklist), and release notes v0.1.0–v1.6.0. Verdicts: **COVERED** (named test asserts the invariant), **PARTIAL** (test touches the area but not the exact failure mode), **GAP** (no test).

| # | Historical regression | Release / evidence | Verdict | Covering test (file + name + line) or GAP |
|---|---|---|---|---|
| 1 | Persisted A/C or B/C Pair in 2-source session → Wipe/Diff black screen | v1.4.2, v1.4.3; plan §5.1 | **COVERED** | `component/ui/MainQmlContractTests.cpp` — `InstantiatesRootAndSeparatesManualAlignmentStates` (L264-268 comment; L287-290 asserts Edge0And2 preference resolves to Edge0And1 render state while preference stays Edge0And2) |
| 2 | Render failure ACK'd as black frame | v1.4.2; plan §5.2 | **GAP** | No test asserts a render *failure* is ACK'd/presented as black. `PresentationAckMailboxTests`/`RenderAckRelayTests` cover happy-path ACK + close/drain only. `ComparisonSurfaceWarpTests` covers intentional black (unavailable side, identical frames) but not the fail→black ACK path. |
| 3 | Mode-switch retained frame | v1.4.2; plan §5.2 | **PARTIAL** | `MainQmlContractTests.InstantiatesRootAndSeparatesManualAlignmentStates` exercises view-mode switching (Wipe→SideBySide→ThreeUp) and effective-mode resolution, but does NOT assert the displayed frame is retained across a switch. No explicit retained-frame test. |
| 4 | Wipe rotation/UV/padding | v1.4.2; plan §5.2 | **COVERED** | `component/ui/ComparisonSurfaceWarpTests.cpp` — `WipePreservesDirectionalSamplingAtQuarterTurnRotations` (L1711), `WipeSplitUsesSurfaceCoordinatesAcrossLetterboxing` (L1650), `WipeUsesTheSelectedPairAndDraggableSplitPosition` (L1576), `WipeLeavesOnlyTheUnavailableSideBlack` (L1621); `AppliesRotationBeforeAspectFitAndSampling` (L1235); UV/padding in `GpuTransferActorTests.UploadsPaddedOddNv12PlanesAndPublishesOneImmutablePair` (L295) |
| 5 | Input-focus stealing → shortcut misfire | v1.4.4; plan §5.3 | **COVERED** | `component/ui/MainQmlContractTests.cpp` — `InstantiatesRootAndSeparatesManualAlignmentStates` (L484-502: menu open disables `globalMediaShortcuts`, Key_Right is swallowed; `forceActiveFocus` on sideModeButton disables shortcuts; Tab restores viewport focus and re-enables shortcuts) |
| 6 | Transport binding loop / height collapse | v1.6.0 (M2.3/M2.4); plan §5.5 | **COVERED** | `component/ui/MainQmlContractTests.cpp` — `DockedTransportResolvesContextuallyAndClearsViewport` (L725): asserts docked transport does not intersect viewport, sits at/under viewport bottom, right edge within content width; single→overlay and empty→hidden transitions |
| 7 | Reference/Pair normalization | v1.4.2–1.4.4; plan §5.1 | **COVERED** | `unit/domain/ComparisonValidatorTests.cpp` — `ReferenceRoleWinsCanonicalSelection` (L84), `AcceptsTwoIdenticalSourcesWithFirstAsCanonical` (L62); `MainQmlContractTests` (persisted Edge0And2 → Edge0And1 for 2-source, L287-290) |
| 8 | Step cancel wrongly shown as lastError (v1.6 cancel/fail split) | v1.6.0; plan §5.3 | **COVERED** | `unit/application/PlaybackCoordinatorTests.cpp` — `ProviderFailureSurfacesStepStreamError` (L4524, comment at L4525 explicit about the split), `SupersedingStepDoesNotSurfaceError` (L4562), `ForwardStepPresentationTimeoutFailsStream` (L4828, "classifies a presentation timeout as a failure (not a cancel)") |
| 9 | Range-loop command failure leaving state dangling | v1.4.2; plan §5.3 | **GAP** | `ReviewControllerTests.ShellOwnsRangeStateAndPreservesItsOrderingInvariants` (L917) tests range state transitions, but NO test exercises a range-loop *command failure* and asserts the state machine does not dangle. |
| 10 | Thumbnail late result | v1.4.2; plan §5.3 | **GAP** | No test references thumbnail generation or late thumbnail results. Late *frame* results are covered (`PauseDuringDecodeCancelsGenerationAndIgnoresLateResults` L1935, `PlaybackTimeoutPausesAndRejectsLateProviderResults` L2166, `PresentationTimeoutKeepsPreviousFrameAndRejectsLateAck` L3175) — but not thumbnails. |
| 11 | Source changed-on-disk identity | v1.4.4; plan §5.3 | **COVERED** | `component/ui/ReviewControllerTests.cpp` — `FrozenIdentityStaysStableWhenFileChangesOnDisk` (L842); `component/ui/SourceIdentityTests.cpp` — `ChangedOnDiskRolePublishesStateThroughModel` (L36); `component/platform/SourceIdentityServiceTests.cpp` — `ReportsMismatchesAsRecoverableMediaProbeErrors` (L61) |
| 12 | Device-generation stale resource | v1.4.4, v1.6.0; plan §5.3 | **COVERED** | `unit/application/PlaybackCoordinatorTests.cpp` — `TracksGraphicsReadinessByIndependentDeviceGeneration` (L2408), `GraphicsLossInvalidatesPublishedPlaybackFrameSet` (L2248); `component/platform/FrameMailboxTests.cpp` — `RejectsStaleGenerationsAndAdvancingGenerationClearsTheSet` (L183); `component/platform/GpuTransferActorTests.cpp` — `ClearTombstoneAndDeviceGenerationRejectLateOrStalePairs` (L366); `component/platform/GraphicsDeviceBrokerTests.cpp` — `IgnoresLossReportedByAnObsoleteLeaseGeneration` (L201) |
| 13 | Marker truncation / dense rendering | v1.4.2; plan §5.3 | **PARTIAL** | `component/ui/ReviewControllerTests.cpp` — `ProjectsDisplayFramesGraphicsAndRoleSpecificErrorKeys` (L1070, `alignmentTimelineMarkers` count/positions at L1214-1227). Asserts marker count/positions but NOT truncation under dense conditions. |

**Summary:** 9 COVERED, 2 PARTIAL (#3 mode-switch retained frame, #13 marker truncation), 3 GAP (#2 render-failure ACK as black, #9 range-loop command failure, #10 thumbnail late result). The three gaps are R0/R1 regression hazards that the plan's Phase 3 (render/ACK refactor) and Phase 4 (thumbnail) changes could re-introduce; each needs a named regression test before the relevant phase (see §4).

---

## §3 Phase 0 Failing-Test Feasibility

Source: plan Ch. 07 §Phase 0 ("首批失败/基线测试"). For each, judged whether it can be written TODAY against current (pre-fix) code, what current behavior it would expose, and the target test file.

| # | Test (plan name) | Feasible NOW? | Current behavior it would expose | Target test file |
|---|---|---|---|---|
| 1 | `HeldBackwardPresentsEveryIntermediateFrame` | **Yes** | Reverse step currently enters exact-seek per `BackwardStepStopsForwardStreamAndUsesExact` (L4336); 300×StepFrames(-1) would show generation/cancel storms and possible missing intermediate frames vs. the forward sequential stream (`ForwardStepPresentsEveryIntermediateFrame` L4239). | `unit/application/PlaybackCoordinatorTests.cpp` (alongside the existing forward-step suite, L4145-4872) |
| 2 | `ChangingReferenceDoesNotChangeTimelineMaster` | **Yes** | `ChangeReferenceRebuildsTheCanonicalTimelineFromActiveSources` (L488) shows a reference change currently rebuilds the canonical timeline — the test would expose that canonical rate/frame follows the Reference (the plan's C-01 invariant violation). Mark expected-failure for V2. | `unit/application/PlaybackCoordinatorTests.cpp` or `component/ui/ReviewControllerTests.cpp` |
| 3 | `SessionPairDoesNotLeakAcrossTopology` | **Yes** | `MainQmlContractTests` L287-290 shows 2-source normalizes Edge0And2→Edge0And1, but the plan (EXP-C02) flags the risk that re-adding a 3rd source silently restores A/C. Test would expose ordinal leak across topology changes. | `component/ui/MainQmlContractTests.cpp` (extend `InstantiatesRootAndSeparatesManualAlignmentStates`) |
| 4 | `RangeLoopNeverPresentsOutsideInclusiveRange` | **Yes** | Range loop is currently driven by QML `Main.qml.onCurrentFrameChanged` catch-up; the plan (EXP-C04/H-01) flags catch-up can compute past Out (e.g. 26/30) before QML seeks back. Test would expose frames presented outside [In,Out]. | `component/ui/MainQmlContractTests.cpp` (QML-level) or a new deterministic fake-clock test in `unit/application/` |
| 5 | `ScrubLatestWinsWithoutGenerationStorm` | **Yes** | Current scrubbing may trigger generation increments per frame; test would expose generation storm / non-latest-wins under rapid scrub. | `component/ui/ReviewControllerTests.cpp` |
| 6 | `WipeHandleIsKeyboardAdjustable` | **Yes** | Wipe handle is currently mouse/touch-driven (`setWipePosition`, `WipeUsesTheSelectedPairAndDraggableSplitPosition` L1576); keyboard adjustability is absent. Test would expose missing keyboard semantics. | `component/ui/MainQmlContractTests.cpp` |
| 7 | `TimelineAccessibleValueMatchesPreviewOrCurrentFrame` | **Yes** | Timeline accessibility value vs. preview/current frame; test would expose mismatches under scrub/step. | `component/ui/MainQmlContractTests.cpp` |

**Verdict:** All seven are feasible today. #1-#4 expose real, known asymmetries (reverse exact-seek, reference==canonical, QML range loop). #5-#7 are UI-contract tests against `Main.qml`. None require the plan's new types. The plan should write #1-#4 as failing tests immediately (they document C-01/C-03/H-01), and #5-#7 as QML contract tests. Note: #2 is explicitly a "V2 target red test" the plan says may be marked expected-failure initially.

---

## §4 Risk-Rated Regression Hazards

Per plan Ch. 08 §VI.4 scale: **R0** release blocker (data/frame error, crash, stale commit, partial FrameSet); **R1** high (core interaction failure, unrecoverable black screen, severe perf regression); **R2** medium (recoverable error, a11y/layout regression); **R3** low (copy, non-blocking visual).

| Risk | Rating | Plan phase / change | What breaks | Required regression test |
|---|---|---|---|---|
| **H1** Reference change silently reassigns canonical timeline | **R0** | Phase 1.1 (decouple Reference from canonical) | Displayed frame/rate jumps when user only intended to change the comparison Reference; wrong source becomes timeline master. | `ChangingReferenceDoesNotChangeTimelineMaster` (§3 #2) — `PlaybackCoordinatorTests.cpp` |
| **H2** Pair ordinal leaks across topology (persisted 0-2 → black screen on 2-source) | **R0** | Phase 1.2 (SourceKey/ComparisonSelection) | Re-introduces v1.4.2 black-screen bug; Wipe/Diff show black because an unavailable slot is selected. | `SessionPairDoesNotLeakAcrossTopology` (§3 #3) — `MainQmlContractTests.cpp`; plus a `ComparisonSelectionTests.cpp` (Phase 1 deliverable) |
| **H3** FramePresentationTransaction refactor breaks ACK-before-commit / exactly-once terminal | **R0** | Phase 3.1 (extract FramePresentationTransaction) | Stale commit, partial FrameSet published, or double terminal — directly violates the core invariant the plan calls "most valuable asset." | Event-permutation test (plan §3.2 matrix) in a new `FramePresentationTransactionTests.cpp`; must cover ready/success/ack/deadline/generation-change orderings |
| **H4** Range-loop migration leaves authority split between QML and kernel | **R0** | Phase 3.3 (native Range Loop) | Frames presented outside [In,Out]; loop boundary state dangling after command failure; QML/kernel dueling seeks. | `RangeLoopNeverPresentsOutsideInclusiveRange` (§3 #4); plus a `RangePlaybackWorkflowTests.cpp` for command-failure state (gap #9, §2) |
| **H5** Reverse-step sequential stream drops intermediate frames | **R1** | Phase 3.4 (ReverseStepRun) | Held backward shows visible stutter / missing frames vs. forward; generation/cancel storms. | `HeldBackwardPresentsEveryIntermediateFrame` (§3 #1) — `PlaybackCoordinatorTests.cpp` |
| **H6** ViewModel split introduces binding loop / height collapse / transport overlap | **R1** | Phase 2 (narrow ViewModels) | Transport overlaps reviewed pixels; viewport height collapses; QML binding loop under 120 fps. | Extend `MainQmlContractTests.cpp` with a perf/geometry invariant test (transport ∩ viewport = ∅, viewport height ≥ 0.78×window) |
| **H7** Render-failure path ACKs a black frame (re-introduces v1.4.2) | **R1** | Phase 3.1 / any ACK-path change | Failed render presented as black instead of failing closed / retaining last good frame. | New test in `RenderAckRelayTests.cpp` or `PlaybackCoordinatorTests.cpp`: inject render failure → assert no black-frame ACK, last good frame retained (gap #2, §2) |
| **H8** Mode switch no longer retains displayed frame | **R1** | Phase 1.5 / Phase 2 (QML rewire) | Visible flash/blank when switching Wipe↔SideBySide↔ThreeUp. | New test asserting displayed frame retained across view-mode switch (gap #3, §2) — `MainQmlContractTests.cpp` |
| **H9** Scrub generation storm / non-latest-wins | **R2** | Phase 4 (Scrub commands) | Rapid scrub causes excessive generation increments; final frame not the last requested. | `ScrubLatestWinsWithoutGenerationStorm` (§3 #5) — `ReviewControllerTests.cpp` |
| **H10** Wipe handle not keyboard-operable / a11y value mismatch | **R2** | Phase 4 / Phase 6 (Timeline a11y) | Keyboard-only and screen-reader users cannot adjust Wipe or read Timeline value. | `WipeHandleIsKeyboardAdjustable` + `TimelineAccessibleValueMatchesPreviewOrCurrentFrame` (§3 #6-#7) — `MainQmlContractTests.cpp` |
| **H11** Thumbnail late result changes displayed frame | **R2** | Phase 4 (ThumbnailProvider) | Late thumbnail callback commits to displayedFrame; wrong source image shown. | New test in a `ThumbnailProviderTests.cpp` (Phase 4 deliverable) asserting late result is rejected and displayedFrame unchanged (gap #10, §2) |
| **H12** Marker truncation under dense conditions | **R2** | Phase 4 (C++ marker bucket model) | Markers overlap / truncate when densely packed. | Extend `ProjectsDisplayFramesGraphicsAndRoleSpecificErrorKeys` (L1070) with a dense-marker fixture (gap #13, §2) |

**H1-H4 are R0 and map directly to the plan's stated "most valuable asset" invariants (single serial truth, complete FrameSet, ACK-before-commit, exactly-once terminal).** They must have passing regression tests before the relevant phase merges. H5-H8 are R1 and map to the plan's own historical-regression list (§VI.3). H9-H12 are R2 and are a11y/latency risks the plan's UX review (Ch. 06) flags.

---

## §5 Verdict on the Plan's Test Matrix (Ch. 09)

### 5.1 What Ch. 09 does well

The matrix (09_回归与边界测试矩阵.md, 777 lines) is strong on **dimension enumeration**: Source Topology (§1.1), Playback/Navigation State (§1.2), Media Timing (§1.3), Format (§1.4), Alignment (§1.5), Renderer/GPU (§1.6), UI/Input (§1.7), Resource/Fault (§1.8). Its four-layer strategy — invariants + pairwise + historical bugs + hardware/soak — is the right architecture, and the "Changed dimensions / Affected invariants / Historical bugs touched" PR-minimum template (§十) is exactly the regression-safety gate this role wants.

The invariant list (§二, I-01..I-10) correctly captures the non-negotiable core: complete FrameSet (I-01), ACK-before-commit (I-02), monotonicity (I-03), exactly-once command terminal (I-04), stale isolation (I-05), bounded resources (I-06), rollback (I-07), identity stability (I-08), comparison provenance (I-09), UI non-authority (I-10).

### 5.2 What is missing or under-specified

1. **No mapping from invariants to existing tests.** Ch. 09 lists invariants but does not say which of the ~403 existing cases already prove each one. §1 of this report provides that mapping. The plan should add an "existing coverage" column to I-01..I-10 so Phase 0 knows what is already green vs. what needs a new test.

2. **The three GAPs from §2 are not in the matrix as named regression cases.** Render-failure-ACK-as-black, range-loop-command-failure-dangling, and thumbnail-late-result are absent from the invariant and fault lists. They should be added (they are R0/R1 per §4 H7/H4/H11).

3. **Phase 0 failing tests (Ch. 07) are not cross-referenced in Ch. 09.** The seven §3 tests (`HeldBackwardPresentsEveryIntermediateFrame`, etc.) should appear in the matrix as concrete, named cases tied to invariants I-03 (monotonicity) and I-10 (UI non-authority), not only as prose in Ch. 07.

4. **No reverse-step sequential coverage.** The matrix lists "ReverseStepping" as a state (§1.2) and "held backward 300" as a workload (§8.1), but the event-permutation table (§3.2) and the forward/reverse step table (§4.4) do not enumerate the reverse-step failure modes the plan itself identifies as C-03 (exact-seek asymmetry). The matrix should call out reverse-step event orderings explicitly.

5. **Range-loop authority is under-specified.** §4.6 lists range scenarios but frames them around the current QML-driven loop; it does not state the invariant "range authority is singular and in the kernel" that Phase 3.3 establishes, nor the failure-mode invariant "range-loop command failure never leaves state dangling" (gap #9).

6. **No explicit "ACK-before-commit" permutation test.** I-02 states the invariant, but the matrix does not mandate the specific event-permutation test (ready/success/ack/deadline/generation) that H3 requires. §3.2's FramePresentationTransaction permutation table is the closest, but it is not tied back to I-02.

7. **Test placement table (§九) lists directories that do not exist yet** (`tests/unit/presentation`, `tests/unit/analysis`, `tests/e2e`, `tests/fuzz`, `tests/performance`, `tests/soak`). This is fine as a target, but the plan should note that Phase 0-1 can only use the existing `unit/{domain,application,persistence_json,app,shell_windows}`, `presentation_contract`, `component/{media,platform,ui}`, `component/shell_windows`, `hardware`, `smoke` trees.

### 5.3 Completeness verdict

**The matrix is structurally complete enough to start Phase 0, but it is not yet evidence-anchored.** It enumerates the right dimensions and invariants but (a) does not tie each invariant to existing tests, (b) omits three real historical-regression cases (§2 gaps), and (c) does not name the Phase 0 failing tests as concrete matrix entries.

**Recommended additions before Phase 0 exit:**
- Add an "existing test coverage" column to I-01..I-10 (data in §1 of this report).
- Add the three §2 GAP regressions as named cases (render-failure black ACK, range-loop command failure, thumbnail late result).
- Promote the seven §3 Phase 0 failing tests into the matrix as named, scheduled cases tied to I-03/I-10.
- Add a reverse-step event-permutation row to §3.2 / §4.4.
- Add the singular-range-authority invariant and the no-dangle-after-failure invariant to §4.6.

With those additions, the matrix + this report's §4 risk register together give the Final Verification role (Ch. 08 §VII) the regression-safety evidence it needs: every R0/R1 hazard has a named, targetable regression test, and the existing ~403-case suite already covers 9 of the 13 historical regressions.

---

## Appendix A — Key file paths

- Test root: `tests/`
- Largest/most-loaded suites: `unit/application/PlaybackCoordinatorTests.cpp` (74 cases), `component/ui/ComparisonSurfaceTests.cpp` (41), `component/ui/ReviewControllerTests.cpp` (38), `component/media/MultiSourceFrameProviderTests.cpp` (25), `component/media/SoftwareDecoderTests.cpp` (19), `component/media/MediaProbeTests.cpp` (16), `unit/application/AlignmentTests.cpp` (15), `unit/domain/ComparisonValidatorTests.cpp` (12), `component/media/FrameTimelineIndexTests.cpp` (12).
- Release notes: `docs/releases/{v0.1.0,v1.0.0,v1.1.0,v1.2.0,v1.4.2,v1.4.3,v1.4.4,v1.4.5,v1.6.0}.md`
- Plan chapters: `docs/plans/playback-overhaul/{01..13}_*.md`

## Appendix B — Method note

Findings are based on grep/test-name extraction across all 50 gtest .cpp files, full reads of `PlaybackCoordinatorTests.cpp` (L1-963 partial + name extraction), `ReviewControllerTests.cpp` (L1-510 partial + name extraction), `MainQmlContractTests.cpp` (L255-510), `ComparisonSurfaceTests.cpp` (name extraction + targeted reads), `ComparisonValidatorTests.cpp`, `PresentationAckMailboxTests.cpp`, `FrameMailboxTests.cpp`, `RenderAckRelayTests.cpp`, `ComparisonContractTests.cpp`, `SourceIdentityTests.cpp`, plus the plan chapters 01 §5, 07 §Phase 0, 08 §VI, 09, 12, and all release notes v0.1.0–v1.6.0. No code was modified; no new tests were written (that is Phase 0's task).
