#pragma once

#include "dvs/application/FrameSet.h"
#include "dvs/application/RequestContext.h"
#include "dvs/domain/ComparisonSelection.h"
#include "dvs/domain/CompatibilityReport.h"
#include "dvs/domain/FrameTimeline.h"
#include "dvs/domain/MediaError.h"
#include "dvs/domain/PlaybackContinuityPolicy.h"
#include "dvs/domain/ValidatedComparisonSet.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dvs::application {

inline constexpr std::size_t kMaximumCompatibilityFindings = 15U;

struct SessionSourceView final {
    domain::SourceId sourceId = 0;
    domain::ComparisonRole role = domain::ComparisonRole::kPrediction;
    std::string displayName;

    [[nodiscard]] bool operator==(const SessionSourceView&) const = default;
};

// Probed per-source VFR display timeline. The shared mapping resolver consults these before
// descriptor frame rates for both playback and analysis.
struct SourceTimelineView final {
    domain::SourceId sourceId = 0;
    std::shared_ptr<const domain::FrameTimeline> timeline;

    [[nodiscard]] bool operator==(const SourceTimelineView&) const = default;
};

struct CompatibilityFindingView final {
    domain::CompatibilitySeverity severity = domain::CompatibilitySeverity::kWarning;
    domain::MediaErrorCode code = domain::MediaErrorCode::kInvalidArgument;
    std::vector<domain::SourceId> sources;

    [[nodiscard]] bool operator==(const CompatibilityFindingView&) const = default;
};

struct PresentedSourceState final {
    domain::SourceId sourceId = 0;
    std::optional<domain::FrameId> sourceFrameId;
    FrameMatchKind matchKind = FrameMatchKind::ExactIndex;
    float alignmentConfidence = 1.0F;
    std::optional<MissingReason> missingReason;
    // Source-side presentation timestamp in microseconds, valid when sourceFrameId is set.
    // Mirror of MappedSourceFrame::presentationTime on the committed frame set.
    domain::MediaTime presentationTime{0};

    [[nodiscard]] bool operator==(const PresentedSourceState&) const = default;
};

// A complete immutable UI-facing state. Frame resources stay on IRenderChannel; this snapshot
// exposes only frame identities and media state so views cannot observe a partial A/B update.
struct SessionSnapshot final {
    domain::SessionId sessionId{0};
    domain::SessionEpoch sessionEpoch{0};
    domain::PlaybackGeneration playbackGeneration{0};
    domain::DeviceGeneration deviceGeneration{0};
    bool graphicsReady = false;
    domain::SessionState sessionState = domain::SessionState::kEmpty;
    domain::PlaybackState playbackState = domain::PlaybackState::kPaused;
    // Visual playback rate: 1.0 real time. Published so the view layer can show and restore
    // the selected speed across pause/play cycles.
    double playbackSpeed = 1.0;
    // C-07/D03: active continuity mode (Contextual resolves to smoothness-first RealTime).
    // ReviewEveryFrame is the explicit retain-every-FrameSet review option.
    domain::PlaybackContinuityPolicy playbackContinuityPolicy =
        domain::PlaybackContinuityPolicy::Contextual;
    domain::PlaybackContinuityPolicy playbackContinuityPolicyEffective =
        domain::PlaybackContinuityPolicy::RealTime;
    // Player-side complete FrameSet skips (wall-clock catch-up). Distinct from source-content
    // duplicates and from display/present gaps: those are reported separately so a stall is never
    // misread as algorithm judder.
    std::uint64_t playbackSkippedFrameSets = 0U;
    // Same counter scoped to the current Play interval (reset when a PlaybackRun starts).
    std::uint64_t playbackRunSkippedFrameSets = 0U;
    // Target visual rate (1.0 = real time). Mirror of playbackSpeed for status-rail symmetry
    // with playbackPresentationRate.
    double playbackTargetRate = 1.0;
    // Measured presentation rate in media-seconds per wall-second since the run anchor. 1.0
    // keeps media time with the wall clock at 1x; 2.0 is 2x. Zero when not playing.
    double playbackPresentationRate = 0.0;
    // Positive when the displayed media position lags the wall-clock target (behind schedule).
    // Negative means ahead (possible after catch-up overshoot).
    std::int64_t playbackLagMicroseconds = 0;
    // True while the real-time policy is actively skipping whole FrameSets to recover the
    // wall-clock anchor. Always false under ReviewEveryFrame.
    bool playbackCatchingUp = false;
    // C-02: session comparison pair as stable source identities; projected to DifferenceEdge
    // only at the renderer boundary.
    std::optional<domain::ComparisonPair> activeComparisonPair;
    // Kernel-owned playback range. When set, presentation targets are clamped to [in, out].
    std::optional<domain::FrameId> playbackRangeIn;
    std::optional<domain::FrameId> playbackRangeOut;
    bool playbackRangeLoop = false;
    bool playbackRangeLoopActive = false;
    std::uint64_t playbackRangeCompletedLoops = 0U;
    std::optional<domain::FrameId> displayedFrame;
    std::optional<domain::FrameId> requestedFrame;
    std::uint64_t canonicalFrameCount = 0;
    std::optional<domain::CanonicalTimeline> canonicalTimeline;
    std::vector<SourceTimelineView> sourceTimelines;
    std::vector<SessionSourceView> sources;
    std::shared_ptr<const domain::ValidatedComparisonSet> validatedComparison;
    std::vector<PresentedSourceState> presentedSources;
    std::vector<SourceFrameOffset> alignmentOffsets;
    std::vector<GlobalOffsetEstimate> alignmentEstimates;
    std::uint64_t alignmentRevision = 0U;
    // Shared immutable mapping: O(1) snapshot publication, no per-frame entries copy.
    std::shared_ptr<const std::vector<SequenceAlignmentResult>> sequenceAlignmentMaps;
    std::vector<SequenceAlignmentSummary> sequenceAlignments;
    std::optional<AlignmentAnalysisJobId> alignmentAnalysisJobId;
    std::optional<AlignmentAnalysisKind> alignmentAnalysisKind;
    std::optional<AlignmentAnalysisPhase> alignmentAnalysisPhase;
    std::uint64_t alignmentAnalysisCompletedUnits = 0U;
    AlignmentWorkEstimate alignmentAnalysisWork;
    std::vector<SourceAlignmentAnchors> manualAlignmentAnchors;
    std::vector<CompatibilityFindingView> compatibilityFindings;
    AlignmentMode alignmentMode = AlignmentMode::FrameIndex;
    bool alignmentRequired = false;
    bool automaticAlignmentPending = false;
    bool canConfirmAutomaticAlignment = false;
    bool canUndoAutomaticAlignment = false;
    std::optional<domain::MediaError> lastError;

    [[nodiscard]] bool isConsistent() const noexcept;
};

} // namespace dvs::application
