#pragma once

#include "dvs/domain/FrameTimeline.h"
#include "dvs/domain/Identifiers.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dvs::application {

struct AlignmentAnalysisJobId final {
    std::uint64_t value = 0U;

    [[nodiscard]] constexpr bool operator==(const AlignmentAnalysisJobId&) const = default;
};

enum class AlignmentAnalysisKind {
    GlobalOffset,
    Sequence,
};

enum class AlignmentAnalysisPhase {
    CollectingSignatures,
    ComputingAlignment,
};

struct AlignmentWorkEstimate final {
    std::uint64_t totalUnits = 0U;
    std::string unitName;

    [[nodiscard]] bool operator==(const AlignmentWorkEstimate&) const = default;
};

inline constexpr std::size_t kAlignmentSignatureWidth = 16U;
inline constexpr std::size_t kAlignmentSignatureHeight = 9U;
inline constexpr std::size_t kAlignmentSignaturePixels =
    kAlignmentSignatureWidth * kAlignmentSignatureHeight;
inline constexpr std::size_t kAlignmentDetailWidth = 64U;
inline constexpr std::size_t kAlignmentDetailHeight = 36U;
inline constexpr std::size_t kAlignmentDetailPixels =
    kAlignmentDetailWidth * kAlignmentDetailHeight;
inline constexpr std::size_t kAlignmentFeatureGridWidth = 8U;
inline constexpr std::size_t kAlignmentFeatureGridHeight = 8U;
inline constexpr std::size_t kAlignmentFeatureGridCells =
    kAlignmentFeatureGridWidth * kAlignmentFeatureGridHeight;

enum class AlignmentMode {
    FrameIndex,
    Timestamp,
    ManualAnchor,
};

// How a source's frame was mapped to the canonical frame position. The UI presents this state so
// auto-aligned views are never mistaken for strict same-frame comparisons.
enum class FrameMatchKind {
    ExactIndex = 0,
    GlobalOffset = 1,
    AutoAligned = 2,
    ManualAnchor = 3,
    Missing = 4,
    TimeAligned = 5,
};

// Explicit mapping from a canonical frame i to source frame i + offset. Zero means strict-index
// matching. A negative or out-of-range mapped index becomes Missing; it is never clamped or
// replaced with a neighboring frame.
struct SourceFrameOffset final {
    domain::SourceId sourceId = 0;
    std::int64_t frames = 0;
    FrameMatchKind matchKind = FrameMatchKind::GlobalOffset;
    float confidence = 1.0F;

    [[nodiscard]] bool operator==(const SourceFrameOffset&) const = default;
};

// Adapter-neutral, low-resolution luma evidence. Media adapters downsample decoder-owned planes
// into this fixed value before invoking the application algorithm.
struct FrameLumaSignature final {
    domain::FrameId frameId{0};
    // Normalized source display time. Legacy/synthetic callers may omit it; sequence alignment
    // then falls back to ordinal guidance.
    std::optional<domain::MediaTime> displayTime;
    std::array<std::uint8_t, kAlignmentSignaturePixels> luma{};
    // Compact descriptors extracted from a 64x36 luma level. Keeping statistics instead of the
    // full plane bounds a 50,000-frame signature cache while retaining local structure.
    std::array<std::uint8_t, kAlignmentFeatureGridCells> detailBlocks{};
    std::array<std::uint8_t, kAlignmentFeatureGridCells> sobelBlocks{};
    float normalizedVariance = 0.0F;
    std::uint64_t perceptualHash = 0;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] bool operator==(const FrameLumaSignature&) const = default;
};

struct GlobalOffsetEstimationOptions final {
    std::int64_t minimumOffset = -16;
    std::int64_t maximumOffset = 16;
    std::size_t selectedSampleCount = 9U;
    std::size_t minimumEvidence = 3U;
    float minimumConfidence = 0.12F;
    float maximumBestCost = 0.35F;

    [[nodiscard]] bool isValid() const noexcept;
};

struct GlobalOffsetEstimate final {
    domain::SourceId sourceId = 0;
    std::int64_t bestOffset = 0;
    float bestCost = 1.0F;
    float runnerUpCost = 1.0F;
    float confidence = 0.0F;
    std::size_t evidenceCount = 0U;
    bool autoApplicable = false;

    [[nodiscard]] bool operator==(const GlobalOffsetEstimate&) const = default;
};

enum class SequenceAlignmentAnomalyKind {
    TargetFrameMissing,
    TargetFrameExtra,
    TargetFrameDuplicate,
};

struct SequenceAlignmentEntry final {
    domain::FrameId canonicalFrameId{0};
    std::optional<domain::FrameId> sourceFrameId;
    FrameMatchKind matchKind = FrameMatchKind::AutoAligned;
    float confidence = 0.0F;

    [[nodiscard]] bool operator==(const SequenceAlignmentEntry&) const = default;
};

struct SequenceAlignmentAnomaly final {
    SequenceAlignmentAnomalyKind kind = SequenceAlignmentAnomalyKind::TargetFrameMissing;
    std::optional<domain::FrameId> canonicalFrameId;
    std::optional<domain::FrameId> sourceFrameId;

    [[nodiscard]] bool operator==(const SequenceAlignmentAnomaly&) const = default;
};

struct SequenceAlignmentOptions final {
    std::int64_t expectedOffset = 0;
    std::size_t bandWidth = 8U;
    float gapPenalty = 0.18F;
    float duplicateDistance = 0.08F;
    float minimumConfidence = 0.30F;
    float maximumMeanMatchCost = 0.35F;
    float sceneCutDistance = 0.42F;
    std::size_t segmentLength = 120U;
    std::size_t maximumLowConfidenceRun = 12U;
    float minimumSegmentP10Confidence = 0.12F;
    float maximumSegmentAnomalyDensity = 0.15F;

    [[nodiscard]] bool isValid() const noexcept;
};

enum class AlignmentSegmentState {
    Accepted,
    ReviewRequired,
    Rejected,
};

struct SequenceAlignmentSegment final {
    domain::FrameId firstCanonicalFrame{0};
    domain::FrameId lastCanonicalFrame{0};
    AlignmentSegmentState state = AlignmentSegmentState::ReviewRequired;
    float meanConfidence = 0.0F;
    float p10Confidence = 0.0F;
    std::size_t maximumLowConfidenceRun = 0U;
    float anomalyDensity = 0.0F;
    bool sceneCutProximity = false;
    float mappingSlope = 1.0F;

    [[nodiscard]] bool operator==(const SequenceAlignmentSegment&) const = default;
};

struct SequenceAlignmentResult final {
    domain::SourceId sourceId = 0;
    std::vector<SequenceAlignmentEntry> entries;
    std::vector<SequenceAlignmentAnomaly> anomalies;
    std::vector<SequenceAlignmentSegment> segments;
    float totalCost = 0.0F;
    float meanMatchCost = 1.0F;
    float confidence = 0.0F;
    bool autoApplicable = false;

    [[nodiscard]] bool operator==(const SequenceAlignmentResult&) const = default;
};

// UI-facing analysis projection. It intentionally excludes the O(N) entries vector; the
// coordinator retains the mapping and publishes a shared immutable reference for analysis.
// UI summaries carry only bounded review evidence needed by the 16 ms projection path.
struct SequenceAlignmentLowConfidenceRun final {
    domain::FrameId firstCanonicalFrame{0};
    domain::FrameId lastCanonicalFrame{0};
    float minimumConfidence = 0.0F;

    [[nodiscard]] bool operator==(const SequenceAlignmentLowConfidenceRun&) const = default;
};

struct SequenceAlignmentSummary final {
    domain::SourceId sourceId = 0;
    std::vector<SequenceAlignmentAnomaly> anomalies;
    std::size_t anomalyCount = 0U;
    std::vector<SequenceAlignmentLowConfidenceRun> lowConfidenceRuns;
    std::vector<SequenceAlignmentSegment> segments;
    float totalCost = 0.0F;
    float meanMatchCost = 1.0F;
    float confidence = 0.0F;
    bool autoApplicable = false;

    [[nodiscard]] bool operator==(const SequenceAlignmentSummary&) const = default;
};

struct ManualAlignmentAnchor final {
    domain::FrameId canonicalFrameId{0};
    domain::FrameId sourceFrameId{0};

    [[nodiscard]] bool operator==(const ManualAlignmentAnchor&) const = default;
};

struct SourceAlignmentAnchors final {
    domain::SourceId sourceId = 0;
    std::vector<ManualAlignmentAnchor> anchors;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] bool operator==(const SourceAlignmentAnchors&) const = default;
};

[[nodiscard]] FrameLumaSignature
makeFrameLumaSignature(domain::FrameId frameId,
                       std::span<const std::uint8_t, kAlignmentSignaturePixels> luma) noexcept;

[[nodiscard]] FrameLumaSignature makeMultiScaleFrameLumaSignature(
    domain::FrameId frameId,
    std::span<const std::uint8_t, kAlignmentDetailPixels> detailLuma,
    std::optional<domain::MediaTime> displayTime = std::nullopt) noexcept;

// Returns no estimate for malformed inputs. Ambiguous but otherwise valid evidence returns a
// result with autoApplicable=false so the UI can offer the best candidate without silently
// changing the mapping.
[[nodiscard]] std::optional<GlobalOffsetEstimate>
estimateGlobalOffset(domain::SourceId targetSourceId,
                     std::span<const FrameLumaSignature> reference,
                     std::span<const FrameLumaSignature> target,
                     const GlobalOffsetEstimationOptions& options = {}) noexcept;

// Builds a monotone mapping inside a narrow band around expectedOffset. Storage and compute are
// O(N*bandWidth); no unconstrained N-by-M matrix is allocated. Ambiguous evidence still returns
// diagnostics, but autoApplicable remains false.
[[nodiscard]] std::optional<SequenceAlignmentResult>
alignFrameSequences(domain::SourceId targetSourceId,
                    std::span<const FrameLumaSignature> reference,
                    std::span<const FrameLumaSignature> target,
                    const SequenceAlignmentOptions& options = {},
                    std::span<const ManualAlignmentAnchor> anchors = {}) noexcept;

// Outside the first/last anchor, the nearest anchor's offset is extended. Between anchors, the
// source position is interpolated monotonically and rounded to the nearest frame.
[[nodiscard]] std::optional<SourceFrameOffset>
mapFrameWithAnchors(const SourceAlignmentAnchors& anchors,
                    domain::FrameId canonicalFrame,
                    std::int64_t sourceFrameCount) noexcept;

// Timestamp alignment: a VFR timeline's last frame has no successor to bound its display
// interval, so its end is estimated from the last inter-frame gap. A canonical time at or
// past that end has no frame in this source and must be treated as Missing — the same rule
// the CFR path gets from its out-of-range bounds check — instead of holding the last frame.
// Times inside the indexed range are never "beyond" (frameAtOrBefore already resolved them),
// and a cadence that cannot be estimated keeps the held last frame rather than guessing.
[[nodiscard]] bool canonicalTimeBeyondSourceTimeline(const domain::FrameTimeline& timeline,
                                                     domain::MediaTime canonicalTime) noexcept;

} // namespace dvs::application
