# Time Alignment

Status: **implemented** (shipped in v1.6.0). Strict index, explicit manual global
offsets, confidence-gated automatic global offsets, bounded full-sequence mapping,
manual anchors, and timeline diagnostics are available.

Product interpretation (2026-09-22): strict frame-index equality is not proof of equal media time
when rates or timestamps differ. For 30 fps input versus 60 fps prediction/GT, retain explicit
index/offset/anchor behavior and state the chosen mapping; do not silently interpolate to hide
model errors. Time-based comparison and the precision of its UI labels remain V-03 in the
[current issue ledger](engineering/visual-review-backlog.md). Reference and timeline master are
separate roles; see [ADR 0004](adr/0004-timeline-pair-continuity.md).

Two comparison modes make sure automatic alignment can never mask inference errors:

- **Strict Index Mode** (default, for model-output review): canonical frame `i` maps
  to frame `i` of every source. A source without that frame shows `Missing frame` —
  no interpolation, no last-frame repetition, no silent trimming. Frame-count
  disagreement is itself a finding worth seeing.
- **Aligned Capture Mode** (for screen recordings, re-encodes, shifted videos):
  allows global offsets, extra/missing frames at the ends or in the middle, VFR, and
  manual anchors. Every automatic mapping states its status, e.g.
  `Prediction 2: Auto-aligned, offset +1, one missing frame at 824`.

## Level 1 — Metadata and strict index check (implemented)

Opening a video produces the display-order frame count, a per-frame PTS index,
CFR/VFR classification, start time, frame-rate candidates, and
resolution/rotation/SAR/color metadata. The existing `MediaProbe` + `FrameTimeline`
machinery does this and normalizes VFR timelines to the first frame — it is kept.

Refinement: probing scans all timestamps today, so opening three long videos is
O(total frames). Plan: fast metadata probe first, first frame shown early when CFR
metadata is trustworthy, full index built in the background with per-source progress
on the source cards; a full index stays mandatory for VFR, unknown counts, or suspect
timestamps.

## Level 2 — Global offset estimation (implemented)

For videos that differ by one or two frames or are shifted as a whole, search a small
offset range (≈ [−16, 16]) minimizing

    E(δ) = median over sampled frames S of D(R_i, P_{i+δ})

with images reduced to low-resolution luma and the distance mixing structure, edges,
and perceptual hash terms:

    D = α(1 − SSIM) + β‖∇Y_R − ∇Y_P‖₁ + γ·D_pHash

The media adapter collects fixed 16×9 luma signatures on its worker and publishes
adapter-neutral evidence/results through the application port. The estimator chooses
high-activity samples, uses the median so a single broken interpolated frame or scene
cut cannot dominate, and compares the best candidate with the runner-up. The normal
search is [−16, 16]; very short clips shrink that window while preserving the minimum
evidence count.

The coordinator is the only component allowed to apply a result. A clear winner is
stored as `AutoAligned` with its confidence and triggers an exact re-presentation of
the current complete `FrameSet` before the command succeeds. An ambiguous result never
changes the mapping: the UI displays it as a suggestion for manual review.

## Level 3 — Dropped and duplicated frame detection (implemented)

When no single offset explains the video, banded dynamic programming builds a
monotone mapping C(i, j) with transitions for match, reference-missing
(i−1, j), and prediction-missing/duplicate (i, j−1). Real cases differ by one or two
frames, so the search stays inside a band of width W ≈ 8–16 around the estimated
offset — O(N·W), not O(N²). The resulting `AlignmentMap` records exactly which
canonical frame maps to which source frame, and where frames are missing or repeated.

The UI exposes this as **Find drops**. The media worker decodes display-order frames
sequentially, reduces each frame to the same fixed 16×9 luma signature used by global
alignment, and submits adapter-neutral evidence to the application algorithm. The default
request limit is 50,000 frames and the application rejects requests above the hard
100,000-frame safety cap. Confident maps re-present the current complete `FrameSet`
atomically; low-confidence results are diagnostic only.

## Level 4 — Manual anchors (implemented)

Automatic alignment is always overridable with anchors such as
`Reference frame 824 ↔ Prediction 2 frame 825`; between anchors the mapping is
piecewise monotone. The timeline shows the global offset, drop points, duplicate
points, manual anchors, and low-confidence regions — strictly more useful for
model-output review than a bare ± time-shift control.

Anchors are upserted per source, must have strictly increasing canonical positions and
nondecreasing source positions, and override sequence/global mappings. The nearest
anchor offset extends outside the first and last points; between points, source positions
are rounded from monotone piecewise interpolation. Invalid crossing anchors fail without
changing the active map.

Timeline projection is capped at 256 markers. Missing, duplicate, extra, and anchor
markers are preserved first; contiguous low-confidence entries are coalesced to their
region start so long clips do not create an unbounded QML model.
