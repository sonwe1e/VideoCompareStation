# D. Independent Correctness Review — Plan vs. Code Ground Truth

> Reviewer role: Chapter 08, §V (Independent Correctness). Applied to the **plan itself** (no implementation yet). Repo `sonwe1e/Toy`, HEAD `d04339c`. Code ground truth taken from `src/application/src/PlaybackCoordinator.cpp`, `src/application/include/dvs/application/SessionSnapshot.h`, `src/application/include/dvs/application/FrameSet.h`, `src/application/include/dvs/application/RequestContext.h`, `src/domain/include/dvs/domain/ComparisonSource.h`, `src/domain/include/dvs/domain/ValidatedComparisonSet.h`, `src/domain/include/dvs/domain/Identifiers.h`, `src/presentation_contract/include/dvs/presentation/ComparisonContract.h`, `src/media_ffmpeg/src/MultiSourceFrameProvider.cpp`, `src/ui_qml/src/ComparisonSurface.cpp`, `src/platform_windows/include/dvs/platform/D3d11ComparisonRenderer.h`.
>
> Method: for each invariant and each stress-test proposal, the plan's proposed types/contracts are checked against the actual current behavior that must be preserved. Judgments: **PRESERVED / AT RISK / BREAKS** (invariants) and **CONSISTENT / GAP / CONTRADICTION** (proposals).

---

## Part 1 — The 10 Unbreakable Invariants

### Invariant 1 — One complete canonical FrameSet per presentation (no A-updated-but-B-not)

**Verdict: PRESERVED.**

Plan 03 §5.1 keeps `FrameSet` atomic with exactly the visible set; §5.2 mandates atomic active-set switch with old set held until new set is available. This matches the current contract in `FrameSet.h:39-41` ("one entry per loaded source; an incomplete set is still published with explicit Missing entries instead of being dropped or partially advanced") and `FrameSet::create` rejecting duplicate `sourceId` and requiring every entry to be either fully present or explicitly `Missing` (`FrameSet.h:65-96`). The `commitPresentedFrameIfComplete` / `commitPlaybackFrameIfComplete` paths build `presentedSources` from the whole set before committing `displayedFrame` (`PlaybackCoordinator.cpp:2705-2780, 2810-2840`). The plan's `MissingFrameResource` (04 §3.4) is a superset of the current `MissingReason` model, not a regression.

### Invariant 2 — `displayedFrame` committed only after matching PresentationACK

**Verdict: PRESERVED.**

Plan 03 §7.3 requires `PresentationAck{frame, presentation, device}` and commits only when all match; 04 §2.1 encodes the three-way condition `FrameSetReady + RequestSucceeded + PresentationACK`. Current code enforces exactly this: `commitPresentedFrameIfComplete` returns unless `framePresented && providerSucceeded && framePublished && presentationTimerId` are all set (`PlaybackCoordinator.cpp:2706-2709`), and `handleFramePresented` sets `framePresented` only when `presented.context == *pending_->frameContext && presented.frameId == *pending_->expectedFrame` (`PlaybackCoordinator.cpp:3559-3565`). The plan's `PresentationRevision` gating is a strict strengthening.

### Invariant 3 — All async results carry sufficient identity; stale results rejected

**Verdict: PRESERVED.**

Plan 03 §4 `OperationIdentity{session, epoch, topology, timeline, alignment, playback, device, request}` is a superset of the current `RequestContext / PlaybackRequestContext / FrameRequestContext` stack (`RequestContext.h:9-31`), which already carries `session+epoch+requestId`, `+playbackGeneration`, `+deviceGeneration`. The current `matchesContext` / `matchesPlaybackFrame` / `acceptsCommand` discipline (`PlaybackCoordinator.cpp:479-481, 2782-2790`) is precisely the "revision mismatch → result may only terminate itself" rule the plan codifies. The per-task minimal-subset table (§4) is a safe refinement, not a relaxation.

### Invariant 4 — GUI/render threads never block on decode/probe/analysis/IO

**Verdict: PRESERVED.**

Plan 03 §3.1 keeps a single worker/event loop; workflows emit effects but never publish. Current code already runs all decode/probe/IO off the `worker_` thread (`PlaybackCoordinator.cpp:232`), with GUI ingress bounded by `kCommandIngressCapacity` and `claimCommand` dedup (`PlaybackCoordinator.cpp:247-260, 680-688`). The plan does not introduce any new blocking wait on the GUI or render thread.

### Invariant 5 — All queues/caches/read-ahead/analysis bounded

**Verdict: PRESERVED.**

Plan 03 §6.2 `FrameBudget` with per-class limits and the 256 MiB global constraint; 04 §3.5 bounds the reverse window by bytes + time-span (not a fixed frame count); 04 §7.3 bounds the thumbnail LRU by entries + bytes. Current code already bounds `kCommandIngressCapacity`, `exactPrefetchWindow`, and `kMaximumCompatibilityFindings` (`SessionSnapshot.h:18`). The plan makes bounding explicit for every new structure.

### Invariant 6 — One command → exactly one terminal; normal cancel ≠ media failure

**Verdict: AT RISK.**

Plan 03 §3.3 proposes `CommandLedger{register, complete, isPending}` and 04 §2.2 defines `CommandOutcome{Succeeded, Busy, Canceled, Failed, Closed}` with cancel ≠ failure. The current code enforces exactly-once **by flow control and context matching**, not by a central ledger: `claimCommand` dedups admission (`PlaybackCoordinator.cpp:680-688`), and `completeCommand` is called once per command through carefully partitioned paths. The v1.6 cancel/fail split is real and load-bearing:

- `supersedePendingSeek` (`PlaybackCoordinator.cpp:1790-1802`) cancels the in-flight seek's provider + timer and completes it as `Canceled` — a normal cancel, no `lastError`.
- `cancelInteractiveStepRun` completes queued commands as `Canceled` with **no** `lastError`; `failInteractiveStepRun` completes them as `Failed` **with** `lastError` (`PlaybackCoordinator.cpp:1198-1234`).
- Presentation timeout is classified as `Failed` (v1.6 M1.2 decision), `PlaybackCoordinator.cpp:3596-3609`.

**Risks in the plan:**
1. The `CommandLedger` API as sketched (`register/complete/isPending`) does not obviously subsume the **multi-command teardown** case: `completeInteractiveStepCommands` completes queued + prepared + current commands in one call (`PlaybackCoordinator.cpp:1181-1196`). A ledger must allow one *event* to terminate N commands while still guaranteeing each command's terminal is emitted exactly once.
2. `Busy`-at-admission commands (e.g. step past last frame, `PlaybackCoordinator.cpp:1254`) are completed without ever registering with a provider. The plan must specify whether these register-then-complete or bypass the ledger.
3. The "frame ACKed but provider terminal late" case (04 §2.1, 03 §3.3) is handled today by keeping `pending_->command` alive until both `framePresented` and `providerSucceeded` are set (`PlaybackCoordinator.cpp:2706-2709`). The ledger must preserve this "ACK does not release command ownership" semantics, or a late provider terminal could be dropped and the command lost.

The invariant is not broken by the plan, but the `CommandLedger` contract is underspecified relative to the three live edge cases above. See also stress test (e).

### Invariant 7 — Domain/Application leak no Qt/FFmpeg/Win32/D3D11 types

**Verdict: PRESERVED.**

Plan 03 §2 domain types are pure C++ (`SourceKey`, `TimelineSelection`, `ComparisonProvenance`). Current `domain/` and `application/` headers include no Qt/FFmpeg/Win32/D3D11. The plan's render-pass model (§7.2) keeps GPU resources inside the renderer and exposes only pure-value pass configs.

### Invariant 8 — UI only emits intent + projects state; owns no media clock/range loop/async lifecycle truth

**Verdict: PRESERVED (as a migration target; currently violated in the codebase, which the plan fixes).**

Plan 03 §8.2 forbids QML from owning playback clock / Range Loop / async terminal / topology transaction; 04 §5 moves Range Loop into Application. **Current code violates this**: Range Loop lives in `Main.qml` (acknowledged as a risk in 04 §5.1), and `ReviewController` / `ReviewShellController` own media state. The plan's target design explicitly repairs the violation, so the invariant is preserved by construction once migration completes. This is the single area where the plan is *more* correct than the current code.

### Invariant 9 — Difference/Metric report comparison domain + exactness

**Verdict: PRESERVED (and the plan repairs a current deficiency).**

Plan 03 §2.3 `ComparisonProvenance{domain, temporal, spatial, colorTransform, filter, roi, version}` requires every visual/quantified result to carry domain + exactness. Current `ComparisonViewConfig` (`ComparisonContract.h:85-100`) carries `differenceMetric/gain/filter/edge` but **no** domain, temporal/spatial exactness, or color-transform id, and the D3D11 renderer computes difference between "the first two slots" with no provenance (`D3d11ComparisonRenderer.h:213-214`). The plan strengthens the contract; it does not weaken it.

### Invariant 10 — Topology switch failure preserves last available Ready Session

**Verdict: PRESERVED.**

Plan 03 §5.2 step 7 ("失败则保留旧 active set") and the `openRollback_` model. Current code implements exactly this: `captureReadySessionForOpenRollback` snapshots the ready session (`PlaybackCoordinator.cpp:727-745`), and `failPending` restores it on open failure unless already a rollback attempt (`PlaybackCoordinator.cpp:767-785`). The `preservesReadySession` flag (`PlaybackCoordinator.cpp:2008-2010`) gates the snapshot. The plan's wording is consistent with the live rollback path.

---

## Part 2 — Stress-Test of Plan Proposals

### (a) SourceKey vs SourceSlot vs RenderSlotMapping

**Plan:** 03 §1.1. `SourceKey` (stable, `uint64` value type) / `SourceFingerprint` (cross-session file identity) / `SourceSlot` (transient) / `RenderSlotMapping{revision, slotToSource, sourceToSlot}` (per `PresentationRevision`).

**Verdict: GAP.**

**What the plan gets right.** Decoupling stable `SourceKey` from transient slot is the correct fix for the current conflation where `SourceId` is simultaneously a logical identity and a `0..2` slot index (`Identifiers.h:80`, assigned in submission order at `PlaybackCoordinator.cpp:2016-2023`, then used as the index into `decodeActors_` at `MultiSourceFrameProvider.cpp:1090-1092`). Reordering and deleting a non-reference source are handled by regenerating the mapping per revision, and the plan's "all derived selections normalize in one transaction" (§1.2) is the right discipline.

**Edge case that exposes the gap — the renderer still speaks slot ordinals.** The current `D3d11ComparisonRenderer` consumes `referenceSlot` (a `0/1/2` ordinal) and `differenceEdge` (`DifferenceEdge::Between0And1/2`, `ComparisonContract.h:35-42, 89`), and `computeSurfacePanelLayout` lays out "the first two slots' textures left/right" and "computes the diff between the first two slots" (`D3d11ComparisonRenderer.h:131-134, 213-214`). The plan says "Pair uses SourceKey, converted to two slots before entering the renderer" (§7.1) but **never specifies the conversion**:

- If `Reference=KeyC` maps to slot 2 and `Candidate=KeyA` maps to slot 0, what `differenceEdge` ordinal is sent? The current enum assumes the pair occupies adjacent slots 0/1 or 0/2 or 1/2. A non-contiguous mapping (slot 2 vs slot 0) has no representation.
- `referenceSlot` in `ReferenceFocus` mode must be derived from the `SourceKey` reference, but the plan does not state the slot-assignment policy (which SourceKey → which slot).
- `RenderSlotMapping` stores **both** `slotToSource` (vector) and `sourceToSlot` (map) (§1.1). These are two views of one bijection, but the plan states no invariant that they are consistent. A bug could make `slotToSource[0]=KeyA` while `sourceToSlot[KeyA]=1`, producing exactly the "one SourceKey maps to two slots / one slot maps to two keys" corruption the test asks about.

**Concrete fix.** (1) Define a pure `RenderSlotMapping::create(slotToSource)` that derives `sourceToSlot` internally and validates bijection, mirroring `FrameSet::create` — never expose the two structures independently. (2) Specify the slot-assignment policy (e.g. slot 0 = TimelineMaster, slot 1 = Reference, slot 2 = Candidate, or "visibleSources order"). (3) Define how a `SourceKey` pair + reference is projected to the renderer's `referenceSlot`/`differenceEdge` ordinals, and restrict `DifferenceEdge` to the assigned slots or replace it with a `SourceKey`-relative pair descriptor.

### (b) ComparisonSelection / TimelineSelection / FocusSelection independence

**Plan:** 03 §1.2 claims "only timeline master or mapping change increments `TimelineRevision`" and "changing pair/view/metric does **not** bump playback generation, does not reopen provider." Tested against the Phase 1.2 command table (07 §1.2).

**Verdict: CONTRADICTION.**

**The contradiction.** The command table in 07 §1.2 says `SetActivePair` has "Topology/provider rebuild = **活动集变化时**" (when the active set changes). But 03 §1.2 and the completion criterion 03 §12 say "ActivePair 改变不递增 playback generation" (absolute). These cannot both be true once the active set is bounded (03 §5.1: "real-time FrameSet only covers the active visible set"; `visibleSources` separated from `sources`; Phase 6 allows N loaded sources with only 1-3/4 visible).

**Edge case.** Load sources `[Master=M, Reference=R, Candidate=C1, Candidate=C2]` with active pair `(R, C1)` and visible set `{M, R, C1}`. `SetActivePair(R, C2)` changes the active set to `{M, R, C2}`. C2 is loaded but not warm in the current visible set, so the provider must be rebuilt and `playbackGeneration` bumped — contradicting the absolute claim. The same applies to `SetFocusedSource` (table: "活动集变化时" rebuilds).

**Secondary finding.** `SetReference` is marked "provider rebuild = 通常否" (usually no). With `Reference != TimelineMaster`, changing the reference alone does not touch the timeline, so "usually no" is correct **only if** the new reference is already in the visible set. If the new reference is outside the visible set, the active set changes and a rebuild is required — the table's "通常否" is ambiguous and should be "否，除非新 Reference 不在活动集内".

**Concrete fix.** Rewrite the invariant as: "Changing pair/reference/focus bumps playback generation **only if** the new selection requires a source outside the current active visible set; otherwise it is renderer-only." Make the command table's rebuild column deterministic (a function of `newVisible ⊆ currentVisible`) rather than "通常否 / 活动集变化时".

### (c) Native Range Loop

**Plan:** 04 §5. `PlaybackRange{inInclusive, outInclusive}` (+ `loop`) lives inside `PlaybackRun`; applied **before** selecting the next target so `Out+1` is never produced (§5.3 rule 2). VFR uses canonical timeline time (rule 7). `in==out` is a single-frame range (§5.4).

**Verdict: GAP.**

**Edge cases.**
1. **VFR media.** Rule 7 correctly mandates canonical-timeline time over fixed fps. Consistent with current `playbackTargetAt` using `canonicalFrameStartTime` (`PlaybackCoordinator.cpp:825-848`). No gap.
2. **Catch-up interaction.** Current `RealTime` catch-up computes `dueTarget = max(scheduledTarget, playbackTargetAt(now))` (`PlaybackCoordinator.cpp:3577-3581`), which can jump **past** `Out`. The plan says the range is applied "before selecting next target" but does not specify **where** the clamp sits relative to the catch-up `max`. If catch-up yields `Out+5`, do we present `Out` (clamp) or pause at `Out`? The plan must state: range is a clamp on the computed target (`min(target, Out)`), and reaching `Out` is the terminal boundary. Currently unspecified.
3. **Device-loss mid-range.** Current `handleGraphicsReady` / device-generation change invalidates in-flight frames (`PlaybackCoordinator.cpp:3612+`). The plan's `OperationIdentity.device` (§3) covers identity, but says nothing about whether a range loop **survives** a device change (re-anchored at the same canonical position?) or is cleared. Must be specified.
4. **Range where `in==out`.** §5.4 handles this explicitly (single-frame, no seek loop, `completedLoops` not incremented). Consistent.

**Concrete fix.** (1) Specify that `PlaybackRange` clamps the **post-catch-up** target: `target = min(rawTarget, outInclusive)`, and `target == outInclusive` is a boundary that triggers pause/loop-anchor. (2) Specify device-loss semantics: a range loop is either re-anchored at the current canonical position on the new device, or cleared with a diagnostic — pick one and make it a contract.

### (d) Reverse Step window

**Plan:** 04 §3. `ReverseWindow{source, inclusiveStart, inclusiveEnd, framesInDisplayOrder, bytes, decoderRevision}`; `ReverseStepRun{lastQueuedTarget, queuedCommands, current, prepared, sourceWindows}`. Decode forward from keyframe, present backward, bounded GOP window, budget-constrained (§3.5).

**Verdict: GAP.**

**Edge cases.**
1. **Multi-source, each with a different GOP.** The plan's per-source `sourceWindows` (§3.2) is the right call, but §3.4's "complete FrameSet assembly" requires **all** sources to have the mapped frame. When source A's window is exhausted but source B's is not, does the canonical reverse step **block** until A's previous window is built, or present A as `Missing`? Blocking breaks the "no missing frame" expectation for reverse; presenting `Missing` breaks the FrameSet-completeness contract. The plan does not choose. Current `MultiSourceFrameProvider` assembles the FrameSet only after all slots report (`MultiSourceFrameProvider.cpp:1035-1050`), so the provider already blocks — but the plan's async window-build must decide whether the canonical step waits or emits `Missing`.
2. **Very long GOP / all-intra.** All-intra means every frame is a keyframe, so a "GOP window" could be the entire clip. §3.5's byte budget caps this, with fallback to Exact Seek. Consistent — but the plan should state that the window is bounded by **bytes first**, and that an all-intra source's window may be as small as a few frames at 4K/P010.
3. **Seek from middle of a window.** §3.3 step 1 locates the keyframe before `N-1`; step 8 cancels the window on random seek. Consistent.
4. **Direction flip at window edge.** When the user flips from reverse to forward at `inclusiveStart`, the forward `Sequential` cursor must be re-anchored. The plan does not specify whether the forward stream resumes from `inclusiveStart` (reusing the already-decoded window) or re-seeks. Current `beginInteractiveForwardStep` cancels the prior run and advances generation (`PlaybackCoordinator.cpp:1238-1288`); the plan should preserve this.

**Concrete fix.** (1) Specify the multi-source window-exhaustion policy: prefer "build the next window **before** the current one is exhausted (look-ahead), and if it isn't ready, emit the source as `Missing` for that one frame rather than blocking the canonical step" — and record `reverse_exact_fallback_count`. (2) State the direction-flip rule: flipping direction tears down the `ReverseStepRun` and starts a fresh forward run, advancing generation, exactly as the current `cancelInteractiveStepRun` + `beginInteractiveForwardStep` sequence does.

### (e) Command exactly-once terminal — CommandLedger vs. v1.6 supersede

**Plan:** 03 §3.3 `CommandLedger{register, complete, isPending}`; 04 §2.2 `CommandOutcome`. Tested against the live `supersedePendingSeek` (v1.6 cancel/fail split) at `PlaybackCoordinator.cpp:1790-1802`.

**Verdict: GAP (not a contradiction — the ledger can subsume the current logic, but the plan does not show how).**

**Current reality.** `supersedePendingSeek` (1790-1802): if a seek is in `kSeekingFrame` phase, it cancels the provider scope + presentation timer and completes the superseded command as `Canceled`. The fresh command then calls `beginSeek`, which advances `playbackGeneration` again (2111). The superseded command's identity is `superseded.command`; its terminal is emitted exactly once via `completeCommand`. The v1.6 split is preserved by two distinct teardown paths: `cancelInteractiveStepRun` (Canceled, no `lastError`) vs `failInteractiveStepRun` (Failed, with `lastError`), 1198-1234.

**Gap.** The plan's `CommandLedger` is described only by three methods and a test-invariant list. It does not demonstrate that the ledger can express:
- **Supersession**: command B legally terminates command A as `Canceled` while B itself proceeds to its own terminal. `register(B); complete(A, Canceled)` — but A was never registered through the ledger if A was accepted via the older `claimCommand` path. The plan must specify whether `register` is the single admission gate (replacing `claimCommand`) and whether a command can be completed without having been registered (as today with `Busy`).
- **Multi-command teardown**: one navigation event terminates N queued step commands at once (`completeInteractiveStepCommands`, 1181-1196). The ledger's `complete` is per-`CommandId`; the plan must show that a single user intent can emit N terminals without violating "exactly one terminal per command" or "one command → one terminal" (the invariant is per-command, so this is allowed, but the ledger API must support bulk teardown).
- **ACK-before-terminal ownership** (v1.6 M1.2): a frame whose `PresentationACK` arrived but whose provider terminal is late must keep the command pending. The plan mentions this (§2.1, §3.3) but does not map it to ledger state (`isPending` must remain true until both ACK and provider terminal arrive).

**Concrete fix.** Define `CommandLedger` as the **single** admission + terminal authority (replacing both `claimCommand` and the ad-hoc `completeCommand` calls). Specify: (a) `register` is called once on admission, before any provider interaction; (b) `complete(id, outcome, error?)` is idempotent-guarded (second call is a no-op) and is the only way a terminal is emitted; (c) a command may be completed as `Canceled` by a superseding command **without** the superseded command ever having registered, provided the superseded command's `CommandContext` is tracked in the pending slot; (d) `isPending(id)` stays true until both presentation ACK and provider terminal for that command's frame have arrived or been superseded. Show the `supersedePendingSeek` → `register(B); complete(A, Canceled)` reduction explicitly.

---

## Part 3 — Internal Contradictions in the Plan

1. **Pair-change generation bump.** 03 §1.2 + 03 §12 say changing the active pair never increments playback generation; 07 §1.2 command table says `SetActivePair` rebuilds the provider "when the active set changes" (which implies a generation bump). These are incompatible once the visible set is bounded and the new pair includes a source outside it. (Detailed in stress test (b).)

2. **"Renderer receives validated active pair slots, does not guess" (03 §12) vs. renderer still consumes `referenceSlot`/`differenceEdge` ordinals** (`ComparisonContract.h:89, 95`; `D3d11ComparisonRenderer.h:119, 133`). The plan never specifies the `SourceKey → slot-ordinal` projection, so the renderer would still "guess" or the enum would silently mis-map. (Detailed in stress test (a).)

3. **Reverse window completeness vs. multi-source FrameSet completeness.** 04 §3.4 requires a complete FrameSet (all sources) per canonical step, but per-source `sourceWindows` deplete at different rates. The plan does not resolve whether the canonical step blocks or emits `Missing` at a window boundary. (Detailed in stress test (d).)

---

## Part 4 — Missing Pieces Required Before Implementation Starts

1. **`SourceKey → renderer slot-ordinal` projection.** A deterministic slot-assignment policy and a `RenderSlotMapping::create` bijection validator (03 §1.1). Without it, the renderer cannot be wired and `DifferenceEdge` is undefined for non-contiguous pairs.

2. **Deterministic provider-rebuild predicate for comparison commands.** Replace "通常否 / 活动集变化时" (07 §1.2) with a precise function of `newVisible ⊆ currentVisible`, and align the 03 §1.2 / §12 invariants with it.

3. **Range-loop clamp position + device-loss semantics.** Specify that `PlaybackRange` clamps the post-catch-up target and define whether a range loop survives a device-generation change (04 §5.3).

4. **Reverse window multi-source exhaustion + direction-flip policy.** Choose block-vs-`Missing` at per-source window boundaries and specify forward-cursor re-anchor on direction flip (04 §3.4/§3.6).

5. **`CommandLedger` full contract.** Single admission gate, idempotent `complete`, supersession (`complete` of an unregistered-but-tracked command), bulk teardown, and ACK-before-terminal pending state (03 §3.3). Must subsume `claimCommand`, `supersedePendingSeek`, and `completeInteractiveStepCommands`.

6. **`PresentationRevision` definition and ACK matching.** The plan introduces `PresentationRevision` (§1.1, §7.3) but does not define how it is generated, monotonicity guarantees, or how a "ViewMode-only redraw of retained frame" gets its own revision without advancing `displayedFrame` (§7.3). Needed before `PresentationAck` matching can be implemented.

7. **`SourceFingerprint` generation and cache-key migration.** §1.1 introduces `SourceFingerprint` for cross-session file identity and §7.2 requires thumbnail cache keys to include it. The plan must specify how a fingerprint is computed (hash of path? of content? of descriptor?) and the cache-invalidation rule on fingerprint change (07 data-migration: "old cache keys lacking SourceKey/revision/provenance are invalidated").

---

## Part 5 — Overall Verdict

**Approved with non-blocking follow-ups.**

The plan is structurally sound and preserves the 10 invariants (8 clearly preserved, Invariant 6 "at risk" only because the `CommandLedger` contract is underspecified relative to live v1.6 edge cases, and Invariant 8 is preserved as a migration target that repairs a current violation). The five stress tests surface **no outright breakage** of the architecture, but they do surface one **internal contradiction** (pair-change generation bump) and four **completeness gaps** (SourceKey→slot projection, range-loop clamp/device-loss, reverse-window multi-source exhaustion, CommandLedger contract) that are cheap to fix now and expensive to discover mid-implementation.

Recommended order for the follow-ups: fix the internal contradiction (b) and the `CommandLedger` contract (e) first — they are P0 (Phase 1.2 / Phase 3) load-bearing — then the SourceKey→slot projection (a) before any renderer work, then the range (c) and reverse-window (d) edge cases before their respective Phase 3 implementations.
