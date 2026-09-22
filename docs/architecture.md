# Target Architecture

Current product scope and issue status: [product intent](product/visual-review.md),
[agent routing guide](agent-guide.md), and [issue ledger](engineering/visual-review-backlog.md).
The phase numbers below refer to the historical architecture rollout, not completion of every
item in the later playback-overhaul plan or the current four-workflow product requirements.

Focused references: [state ownership](architecture/state-ownership.md),
[dependency rules](architecture/dependency-rules.md),
[feature change impact](architecture/feature-change-impact.md), and
[architecture decisions](adr/).

Status: **phases 0–6 implemented and locally validated**. The FrameSet model,
1–3 source validation with compatibility reports, missing-frame semantics, parallel
multi-source decode/render pipeline, three-up/reference-focus layouts, selectable
difference edges, confidence-gated global/sequence alignment, manual anchors, and
timeline diagnostics are in the codebase today. The GUI project open/save/relink loop and
schema v4 were removed — CompareStation manages only the current open 1–3 video session and does
not persist projects. The review surface includes synchronized pan/zoom, ROI,
threshold masks, the four-panel analysis grid, exact-plane difference with a persistent
exactness classification, P010/10-bit software decode, broader YUV/RGB normalization,
transfer metadata, rotation, SAR, and shared-device D3D11VA decode. Decoder-owned NV12/P010
array slices flow directly to plane SRVs while an AVFrame lifetime anchor prevents premature
surface reuse. The visible-window hardware workflow defines single-/two-/three-source 1080p60
five-minute gates and single-/two-/three-source 1080p120 one-minute gates, including the dedicated
two-source 1080p60 path. Those gates require the external fixture root and an interactive 120 Hz
D3D11VA runner; each release SHA must supply its own uploaded evidence before publication. Small
P010 fixtures retain local 10-bit and zero-copy correctness coverage without an active 4K profile.
The historical A/B design is archived in
[design/architecture-overview.md](design/architecture-overview.md).

CompareStation is a frame-exact Windows video workstation. It directly plays one
source, or presents one canonical frame position across two to three sources atomically with
explicit alignment state and pairwise difference maps.

## Core data model (Phase 2)

The hardcoded `FramePair` (two mandatory A/B frames) is replaced by `FrameSet`:

- A review session holds 1–3 `ComparisonSource` entries (`SourceId` +
  `ComparisonRole::Reference | Prediction`), an optional `referenceSource`,
  a `canonicalSource` that defines the frame timeline, and an `AlignmentPolicy`.
- A `FrameSet` carries the canonical `FrameId`/time and one `MappedSourceFrame` per
  loaded source. An entry may be `Missing` — a source lacking that frame is stated
  explicitly; the set is still published atomically. Nothing silently repeats a
  neighbor frame or drops the session.
- `FrameMatchKind` records how each entry maps (`ExactIndex`, `GlobalOffset`,
  `AutoAligned`, `ManualAnchor`, `Missing`) with an alignment confidence, so the UI
  can always say what kind of comparison the user is looking at.
- Difference maps select a `DifferenceEdge` (any two sources) instead of assuming A−B.

Per-source probe/descriptor validation checks whether a video is openable and indexable.
`ComparisonValidator` validates the source set and produces a `CompatibilityReport`
whose findings are graded **Fatal / Warning / Alignment-required**. Mismatched frame
counts, rates, durations, resolutions, or color metadata are warnings that annotate
the session, not reasons to refuse to open it.

## Main pipeline

```text
QML
  ├── ApplicationMenuBar        (explicit menu state and intent signals)
  ├── ReviewShortcuts           (shared ReviewActions and application shortcuts)
  ├── ComparisonViewport        (explicit render state; owns ROI/pan interaction state)
  └── TabbedInspector           (explicit preferences/session inputs)
  │
ReviewSessionFacade / ReviewController / SourceListModel
  ├── active/staged sources, queued intents, startup FIFO, UI chrome, range state
  │
PlaybackCoordinator            (session, epoch, command/request identity,
  ├── alignment workflows       stale-result filtering; one coordinator loop)
  ├── SourceDecodeActor[Reference]
  ├── SourceDecodeActor[Prediction1]
  └── SourceDecodeActor[Prediction2]
  │
FrameSetAssembler              (joins per-source results by request identity)
  │
FrameMailbox                   (latest complete FrameSet only)
  │
ComparisonSurface / D3D11      (2-up, 3-up, reference-focus, analysis-grid)
  │
PresentationAck                (UI frame counter advances only after real paint)
```

Per-source decoder slots publish into a provider-owned completion event queue; no provider worker
blocks waiting for every actor mailbox. Frame latency is `max(T_sources) + T_assemble` instead of
serialized `T_A + T_B` work. The existing frame-provider
priority scheme (control > exact > sequential > prefetch), cancellation by request
identity, and bounded set cache remain intact.

Source-count transitions use the same atomic open pipeline rather than hot-inserting decoder
actors. A replacement open stops active cadence, captures the displayed canonical `MediaTime`,
advances session epoch and playback generation, opens the new 1–3 source topology, maps the saved
time through the new canonical timeline, and presents that complete FrameSet while paused.

A rebuilt session is validated as a running session only; no persistence layer observes
intermediate topology. The session holds the source set, view layout, alignment payload, and
displayed frame, and ends when the app closes.

The per-user startup broker begins listening before QML loads. Valid requests arriving during that
window enter a bounded eight-request FIFO and are acknowledged only after they are either queued or
delivered; registering the handler drains the queue in order exactly once.

The QML shell is canvas-first: the viewport owns the window and menu/source/comparison/inspector/
transport chrome only contributes margins while visible. `ApplicationMenuBar`, `ReviewShortcuts`,
`ReviewInputDialogs`, `ComparisonViewport`, and `TabbedInspector` consume explicit inputs and emit
intent signals instead of accessing the whole Main window. `ReviewActions` remains instantiated
inside `ReviewShortcuts` when chrome is hidden, so transport controls, menus, and shortcuts share
one command path. Active/staged sources, queued requests, chrome, and In/Out range state live in the
session facade; Main only composes them. Full screen and chrome visibility are independent transient
window states and are not persisted.
Entering pure-canvas mode explicitly returns keyboard focus to the viewport and removes its border,
corner radius, surface margin, labels, and analysis controls.

## Frame-step semantics

A/D and Left/Right always mean canonical frame ±1 and go through
the seek/step command path in `PlaybackCoordinator`. Rapid presses coalesce into a newest-target
frame with superseded requests cancelled — never swallowed by a `busy` flag, never a
cache-cursor move. Playing → first frame-step pauses, then seeks.

## Invariants (acceptance criteria)

1. Every video in one display update belongs to the same canonical frame position.
2. Frame-step always means canonical frame ±1, never decode-cache cursor movement.
3. Auto-alignment never silently hides dropped frames, duplicate frames, or count
   differences — each is surfaced in the alignment status.
4. Difference views explicitly distinguish pixel-exact comparisons from resampled,
   color-converted, or auto-aligned ones.

## Module map

The inward-only dependency allow-list stays: `domain` depends on nothing,
`application` only on `domain`, adapters (`media_ffmpeg`, `platform_windows`,
`persistence_json`, `ui_qml`) implement application ports, `app` composes. New target
modules are introduced by capability rather than leaking adapter
types into the core: offset estimation, banded sequence alignment, and manual-anchor
mapping now live in `application`; split TimelineIndexer/FrameCache work remains for
later media/performance phases. `jobs_ffmpeg` was removed in Phase 1.
