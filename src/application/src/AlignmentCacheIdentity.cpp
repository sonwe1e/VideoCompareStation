#include "dvs/application/AlignmentCacheIdentity.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace dvs::application {
namespace {

[[nodiscard]] bool unitFloat(const float value) noexcept {
    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}

[[nodiscard]] bool validMatchKind(const FrameMatchKind kind) noexcept {
    return kind == FrameMatchKind::ExactIndex || kind == FrameMatchKind::GlobalOffset ||
           kind == FrameMatchKind::AutoAligned || kind == FrameMatchKind::ManualAnchor ||
           kind == FrameMatchKind::Missing || kind == FrameMatchKind::TimeAligned;
}

[[nodiscard]] bool validSegmentState(const AlignmentSegmentState state) noexcept {
    return state == AlignmentSegmentState::Accepted ||
           state == AlignmentSegmentState::ReviewRequired ||
           state == AlignmentSegmentState::Rejected;
}

[[nodiscard]] bool validAnomalyKind(const SequenceAlignmentAnomalyKind kind) noexcept {
    return kind == SequenceAlignmentAnomalyKind::TargetFrameMissing ||
           kind == SequenceAlignmentAnomalyKind::TargetFrameExtra ||
           kind == SequenceAlignmentAnomalyKind::TargetFrameDuplicate;
}

} // namespace

bool validateDerivedSequenceAlignments(
    const domain::ValidatedComparisonSet& sources,
    const std::span<const SequenceAlignmentResult> results) noexcept {
    if (results.empty() || results.size() >= sources.sourceCount()) {
        return false;
    }
    const std::size_t canonicalFrameCount = static_cast<std::size_t>(sources.canonicalFrameCount());
    for (std::size_t resultIndex = 0U; resultIndex < results.size(); ++resultIndex) {
        const SequenceAlignmentResult& result = results[resultIndex];
        const domain::ComparisonSource* const source = sources.find(result.sourceId);
        const bool duplicate =
            std::any_of(results.begin() + static_cast<std::ptrdiff_t>(resultIndex + 1U),
                        results.end(),
                        [&result](const SequenceAlignmentResult& other) {
                            return other.sourceId == result.sourceId;
                        });
        if (source == nullptr || result.sourceId == sources.canonicalSourceId() || duplicate ||
            result.entries.size() != canonicalFrameCount || !std::isfinite(result.totalCost) ||
            result.totalCost < 0.0F || !unitFloat(result.meanMatchCost) ||
            !unitFloat(result.confidence)) {
            return false;
        }
        for (std::size_t frame = 0U; frame < result.entries.size(); ++frame) {
            const SequenceAlignmentEntry& entry = result.entries[frame];
            const bool missing = !entry.sourceFrameId.has_value();
            if (entry.canonicalFrameId.value() != static_cast<std::int64_t>(frame) ||
                !unitFloat(entry.confidence) || !validMatchKind(entry.matchKind) ||
                missing != (entry.matchKind == FrameMatchKind::Missing) ||
                (entry.sourceFrameId.has_value() &&
                 (!entry.sourceFrameId->isValid() ||
                  entry.sourceFrameId->value() >= source->descriptor.frameCount.value))) {
                return false;
            }
        }
        std::size_t nextSegmentFrame = 0U;
        for (const SequenceAlignmentSegment& segment : result.segments) {
            if (!validSegmentState(segment.state) ||
                segment.state == AlignmentSegmentState::ReviewRequired ||
                !unitFloat(segment.meanConfidence) || !unitFloat(segment.p10Confidence) ||
                !unitFloat(segment.anomalyDensity) || !std::isfinite(segment.mappingSlope) ||
                segment.mappingSlope < 0.0F || segment.mappingSlope > 4.0F ||
                !segment.firstCanonicalFrame.isValid() || !segment.lastCanonicalFrame.isValid() ||
                segment.firstCanonicalFrame.value() !=
                    static_cast<std::int64_t>(nextSegmentFrame) ||
                segment.lastCanonicalFrame < segment.firstCanonicalFrame ||
                static_cast<std::size_t>(segment.lastCanonicalFrame.value()) >=
                    result.entries.size()) {
                return false;
            }
            nextSegmentFrame = static_cast<std::size_t>(segment.lastCanonicalFrame.value()) + 1U;
        }
        if (!result.segments.empty() && nextSegmentFrame != result.entries.size()) {
            return false;
        }
        for (const SequenceAlignmentAnomaly& anomaly : result.anomalies) {
            if (!validAnomalyKind(anomaly.kind) ||
                (anomaly.canonicalFrameId.has_value() &&
                 (!anomaly.canonicalFrameId->isValid() ||
                  static_cast<std::size_t>(anomaly.canonicalFrameId->value()) >=
                      canonicalFrameCount)) ||
                (anomaly.sourceFrameId.has_value() &&
                 (!anomaly.sourceFrameId->isValid() ||
                  anomaly.sourceFrameId->value() >= source->descriptor.frameCount.value))) {
                return false;
            }
        }
    }
    return true;
}

} // namespace dvs::application
