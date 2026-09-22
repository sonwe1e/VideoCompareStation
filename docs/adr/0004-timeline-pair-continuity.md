# ADR 0004: Timeline master, pair identity, and playback continuity

Status: accepted

Plan references: playback-overhaul C-01 / C-02 / C-07, ADR-003 (SourceKey), ADR-006 (GOP).

## C-01 — Timeline master ≠ Reference

**Decision.** `ValidatedComparisonSet::canonicalSourceId()` is the **timeline master**
(`timelineMasterSourceId`). Default master is the first source in session order. The
`ComparisonRole::kReference` label is only the comparison baseline (UI focus, metrics).
Assigning Reference does **not** rebuild the canonical timeline, frame count, or rate.

**Why.** Coupling made "change GT" silently rewrite Range, alignment, timecode, and VFR
index. Professional review tools treat timeline ownership and comparison baseline as
independent roles.

## C-02 — Active pair by stable identity

**Decision.** Session state stores `domain::ComparisonPair{first, second}` as source ids
(stand-in for stable SourceKey until full key migration). Preferences store only
`DefaultPairPolicy` (ReferenceAndFirstCandidate / LastTwoActiveSources / PreserveIfAvailable).
`resolveComparisonPair` re-derives the pair after topology changes; a preferred pair whose
members disappeared is dropped, never restored as a stale A/B/C ordinal. The renderer still
receives a compact DifferenceEdge ordinal via `comparisonPairEdgeOrdinal` (session-order
projection), never the reverse.

## C-07 — Explicit PlaybackContinuityPolicy

**Decision.** Playback has an explicit continuity mode:

| Policy | Behavior |
|--------|----------|
| ReviewEveryFrame | Never skip whole FrameSets; cadence slips (explicit review) |
| RealTime | Skip complete FrameSets after the 2000 ms catch-up tolerance (continuous default) |
| Contextual | Smoothness-first → RealTime for every source count |

The requested policy and the **effective** policy (after Contextual resolution) are published
on `SessionSnapshot` together with `playbackSkippedFrameSets`. Interactive stepping retains its
own exact-step, cancellation and presentation contract; playback catch-up does not define it.

Implementation audit, 2026-09-22: at `main @ 0a74c46` the status text existed in legacy
`TimelineBar.qml`, while the active shell instantiated `PlayerOsc.qml` without that connection.
The reviewed uncommitted working tree adds the connection and additional counters. Their
measurement semantics and acceptance remain tracked as V-01/V-02 in the
[current ledger](../engineering/visual-review-backlog.md); this ADR does not certify UI delivery
or equate canonical frame-ID gaps with physical display refresh losses.

## Consequences

- Domain unit tests cover pair resolution and continuity resolution without Qt.
- `PlaybackCoordinatorTests.ChangingReferenceDoesNotChangeTimelineMaster` is enabled.
- Full SourceKey/fingerprint migration remains open; session-order `SourceId` is the interim
  identity and must not be confused with a cross-session file key.
