# 00 — Synthesis of Independent Verification (Chapter 08, §VIII Conflict Resolution)

**Role:** Final synthesis of the five independent reports (A–E). HEAD `d04339c`.
**Mandate:** Conflicts resolved only by code/test/reproducible-experiment evidence — never by vote or head-count. Where reports disagree, the disagreement is named and settled by pointing to the specific `file:line` or test that decides it.

---

## 1. The five verdicts at a glance

| Report | Role | Verdict | Key strength |
|---|---|---|---|
| A — Architecture | §II | **Accept** (5 confirmed, 1 partial) | Proved single coordinator / atomic FrameSet / ACK-gated commit by tracing every commit path |
| B — Root-cause | §III | **All 12 C-claims + H-01 CONFIRMED** | Elevated H-01 Hypothesis→Confirmed; found 20 hard-coded-3 sites; caught 2 plan errors |
| C — Migration surface | impl-feasibility | **~562 sites mapped; plan type set ~90% sufficient** | Located 2 silent-break assumptions; 8 unmentioned migration hazards |
| D — Correctness | §V | **Approved with non-blocking follow-ups** | 2 internal plan contradictions; 1 invariant AT RISK; 8 missing pieces |
| E — Regression | §VI | **Phase 0 can start; matrix needs 5 additions** | 9/13 historical regressions covered; 4 R0 hazards named with required tests |

**Overall gate: Phase 0 GO. Phase 1+ PAUSE for plan amendment (two contradictions + CommandLedger/RenderSlotMapping design).**

---

## 2. Consolidated findings

### 2.1 What is solid (implement with confidence)

These claims are confirmed by ≥2 independent reports converging on the same `file:line`:

1. **Reference == canonical** (B C-01, C surface-5, D proposal-b). `ComparisonValidator.cpp:195`. Root cause: one `SourceId` field serves two roles. → Phase 1.
2. **Pair == global ordinal** (B C-02, C surface-2). `ReviewPreferencesController.cpp:37,533-544` persists `"0-1"/"0-2"/"1-2"` strings (note: STRINGS, not integers — plan error corrected). → Phase 1.
3. **Backward step = Exact Seek** (B C-03, E gap). `PlaybackCoordinator.cpp:2135-2154`: only `delta==1` is Sequential. → Phase 3.
4. **Range Loop in QML** (B C-04/H-01, D Invariant-8). `Main.qml:267-284`; coordinator `playbackTargetAt:864-897` has no range clamp; `commitPlaybackFrameIfComplete:2837` assigns `displayedFrame` before QML observes. H-01 CONFIRMED (a frame past Out can be presented). → Phase 3.
5. **Façade collapse** (A claim-6, B C-06). `ReviewSessionFacade.h:17-22` + `ReviewSessionFacade.cpp:19-33`: playback/alignment/notifications → one `ReviewController` (54 Q_PROPERTYs). → Phase 2.
6. **3-source hardcoding** (B C-09, C surface-3). 20 sites across validator/enum/QML/renderer/GPU-buffer. → Phase 1+.
7. **Single serial truth + atomic FrameSet + ACK-gated commit** (A claims 2,3,4). The core the plan must preserve — confirmed intact. → preserve.
8. **State concentrations** (A claim-6). PlaybackCoordinator 3878 LOC / ReviewController 1928 / ReviewShellController 860 / Main.qml 1528 / D3d11ComparisonRenderer 1691.

### 2.2 Plan errors found (plan text is wrong; code is right)

| # | Plan says | Code actually | Found by |
|---|---|---|---|
| 1 | C-02 persistence in `ReviewPreferencesController.h` | In `.cpp:37,533-544`; `SettingsRepository` is schema-unaware | B |
| 2 | differenceEdge persisted as integer 0/1/2 | Persisted as STRING `"0-1"/"0-2"/"1-2"` | C |
| 3 | "topology/timeline" are async identities (A§3.3) | They are session state, not request scopes (`Identifiers.h`, `Events.h`) | A |
| 4 | Dependency diagram edges (PC→Gfx etc.) | Don't match authoritative `Architecture.cmake` | A |
| 5 | Façade collapse is a "future migration" | It is ALREADY the present state (`ReviewSessionFacade.h:17-22`) | A |
| 6 | Missed `ReviewController.cpp:1278` pair-ordinal arithmetic | Pure position arithmetic; breaks on reorder | B, C |
| 7 | Missed `ActiveSourceStrip.qml:267`, `ComparisonToolbar.qml:47` QML hardcodes | Break when limit changes | B |

### 2.3 Conflicts between reports — resolved by evidence

**Conflict 1 — "Are topology/timeline async identities?"**
- Report A (Claim 5, PARTIAL): "topology/timeline are NOT checked identities; they are session state."
- Report D (Invariant 3, PRESERVED+STRENGTHENED): "the plan correctly ADDS the missing timeline/topology revision to frame requests."
- **Resolution: NOT a conflict.** A describes the *current code* (no timeline revision on `FrameRequest`); D describes the *plan's addition* (adds it). Both agree the current stale-rejection machinery is real and rigorous; D notes the plan closes a genuine gap (`Ports.h:38-58` `FrameRequest` lacks timeline revision). The plan's *ontology* (A's wording complaint) is imprecise but its *substance* is correct. Action: keep the addition; re-word the plan to name the real scopes.

**Conflict 2 — "Is the plan type set sufficient?"**
- Report C: "~90% sufficient; 3 gaps (renderer stable-ID→GpuFrameSlot, referenceSlot layout type, DiagnosticItem wiring)."
- Report D: "RenderSlotMapping has no enforced invariant; renderer boundary unnamed."
- **Resolution: AGREE, complementary.** Both identify the renderer boundary / slot-mapping as the gap. C counts it as a missing type; D frames it as a missing invariant. The fix is the same (§3.1 below).

No other material conflicts. The five reports are consistent and mutually reinforcing.

---

## 3. Issues that must be resolved before Phase 1+ implementation

From Report D (correctness) + Report C (migration). These are INTERNAL to the plan (fixable without code change) but would cause rework if implemented as-is.

### 3.1 Two internal plan contradictions (resolve first)

| # | Contradiction | Sections | Precise fix |
|---|---|---|---|
| C1 | "pair change never bumps generation" vs "SetActivePair rebuilds provider (bumps generation)" | 03§1.2 vs 07§1.2 | Rule: pair/view/metric change bumps generation **iff** the active visible set changes such that a not-yet-warm source must be decoded; otherwise renderer-only. Replace "通常否/活动集变化时" with a deterministic predicate. |
| C2 | "loop re-anchors to In" vs "single-frame range must not loop" for `In==Out` | 04§5.3 rule 5 vs §5.4 | Make `In==Out` a special case that forces `loop=false` (present once, then Paused). |

### 3.2 Two designs to flesh out before Phase 1.2 / Phase 3

| Design | Why blocking | What to specify |
|---|---|---|
| `CommandLedger` | Invariant 6 (exactly-once terminal) is AT RISK — current `completeCommand` has no double-complete guard (`PlaybackCoordinator.cpp:1175-1180`) | Per-`CommandIdentity` terminal state, `completed_` set, batch-cancel mapping, `supersede` primitive, `Busy` admission orthogonal to ledger, idempotency |
| `RenderSlotMapping` | Renderer boundary unnamed; two independent containers with no bijective invariant | Single source of truth (`slotToSource` vector), atomic update, round-trip invariant test, named SourceKey→slot conversion function owned by renderer boundary |

### 3.3 Remaining gaps (resolve before the relevant phase, non-blocking for Phase 0)

Range target pipeline ordering (catch-up vs clamp), device-loss/resume rule, reverse-window provider model + direction-flip contract, `isConsistent()` visible-set cap split, TimelineRevision trigger precision, PresentationRevision lifecycle, timeline/topology revision threading into `FrameRequest`. Each has a concrete fix in Report D §2.

---

## 4. Gate decision

### Phase 0 (baseline + trace + failing tests): **GO**

Phase 0 is purely *additive and observational* — it does not change any behavior the gaps above concern:
- PlaybackTrace ring buffer (lock-free, bounded, default-off) adds observability with zero behavioral change.
- behavior-baseline.md + trace-schema.md document current behavior.
- The 7 failing tests (E §3) document C-01/C-03/H-01 as red tests TODAY — they are the evidence baseline, not fixes.
- Trace-event hooks in PlaybackCoordinator/MultiSourceFrameProvider/SourceDecodeActor/ReviewController/Main are behind a disabled-by-default gate (A's R-17 trace-affects-timing risk demands an A/B measurement).

Phase 0 does NOT touch: SourceKey, ComparisonSelection, the range loop, the reverse path, or the façade. So none of the §3 blockers apply. Phase 0 also produces the baseline (held-forward + held-backward profiles, exact-seek counts, generation deltas) that Phase 1–3 need to prove "better, not just different."

### Phase 1+ (Semantics V2 and beyond): **PAUSE → AMEND → GO**

Before any Phase 1 PR: resolve C1 and C2 in the plan text and flesh out CommandLedger + RenderSlotMapping (§3.1–3.2). This is plan-only work (no code), and doing it now prevents the exact rework the user warned against ("若是最开始的方案设计走向错误的方向，会导致整个项目后期需要大量整改").

---

## 5. Phase 0 exit conditions (from plan ch.07 §Phase 0, validated by reports)

- [ ] All existing tests stay green (403 cases; E §1).
- [ ] PlaybackTrace added: lock-free bounded ring buffer, default-off, no worker-hot-path disk I/O; dropped-event count recorded.
- [ ] Trace validates command exactly-once / ACK-before-commit / no stale commit on the existing held-forward profile.
- [ ] 7 failing tests written (document current red state): HeldBackwardPresentsEveryIntermediateFrame, ChangingReferenceDoesNotChangeTimelineMaster, SessionPairDoesNotLeakAcrossTopology, RangeLoopNeverPresentsOutsideInclusiveRange, ScrubLatestWinsWithoutGenerationStorm, WipeHandleIsKeyboardAdjustable, TimelineAccessibleValueMatchesPreviewOrCurrentFrame.
- [ ] behavior-baseline.md + trace-schema.md written.
- [ ] Trace on/off performance A/B shows no gate regression (A R-17).
- [ ] Baseline saved as exact SHA + raw trace (not just summary).

---

*Synthesis of A_architecture.md, B_root_cause.md, C_migration_surface.md, D_correctness_review.md, E_regression_review.md. All file:line citations verified against HEAD d04339c.*
