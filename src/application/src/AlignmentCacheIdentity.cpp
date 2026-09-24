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

template <typename T> void appendInteger(std::uint64_t& hash, const T value) noexcept {
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    using Unsigned = std::make_unsigned_t<T>;
    const Unsigned converted = static_cast<Unsigned>(value);
    for (std::size_t byte = 0U; byte < sizeof(Unsigned); ++byte) {
        hash ^= static_cast<std::uint8_t>(converted >> (byte * 8U));
        hash *= kFnvPrime;
    }
}

void appendText(std::uint64_t& hash, const std::string_view text) noexcept {
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    for (const unsigned char value : text) {
        hash ^= value;
        hash *= kFnvPrime;
    }
    hash ^= 0xFFU;
    hash *= kFnvPrime;
}

void appendFloat(std::uint64_t& hash, const float value) noexcept {
    appendInteger(hash, std::bit_cast<std::uint32_t>(value));
}

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

[[nodiscard]] std::string hexadecimal(const std::uint64_t value) {
    constexpr std::array<char, 16> kHex{
        '0',
        '1',
        '2',
        '3',
        '4',
        '5',
        '6',
        '7',
        '8',
        '9',
        'a',
        'b',
        'c',
        'd',
        'e',
        'f',
    };
    std::string result(16U, '0');
    for (std::size_t index = 0U; index < result.size(); ++index) {
        const std::size_t shift = (result.size() - 1U - index) * 4U;
        result[index] = kHex[(value >> shift) & 0x0FU];
    }
    return result;
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

std::string makeDerivedAlignmentCacheKey(const domain::ValidatedComparisonSet& sources,
                                         const std::span<const SequenceAlignmentResult> results) {
    constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
    std::uint64_t hash = kFnvOffset;
    appendText(hash, kSequenceAlignmentAlgorithmVersion);
    appendInteger(hash, sources.canonicalSourceId());
    appendInteger(hash, sources.canonicalFrameCount());
    for (const domain::ComparisonSource& source : sources.sources()) {
        appendInteger(hash, source.id);
        appendInteger(hash, source.descriptor.frameCount.value);
        if (source.descriptor.sourceIdentity.has_value()) {
            appendInteger(hash, source.descriptor.sourceIdentity->byteSize);
            appendInteger(hash, source.descriptor.sourceIdentity->modifiedUtcMilliseconds);
            appendText(hash, source.descriptor.sourceIdentity->fingerprintSha256);
        }
    }
    for (const SequenceAlignmentResult& result : results) {
        appendInteger(hash, result.sourceId);
        appendFloat(hash, result.totalCost);
        appendFloat(hash, result.meanMatchCost);
        appendFloat(hash, result.confidence);
        appendInteger(hash, static_cast<std::uint8_t>(result.autoApplicable));
        for (const SequenceAlignmentEntry& entry : result.entries) {
            appendInteger(hash, entry.canonicalFrameId.value());
            appendInteger(hash,
                          entry.sourceFrameId.has_value() ? entry.sourceFrameId->value() : -1);
            appendInteger(hash,
                          static_cast<std::underlying_type_t<FrameMatchKind>>(entry.matchKind));
            appendFloat(hash, entry.confidence);
        }
        for (const SequenceAlignmentSegment& segment : result.segments) {
            appendInteger(hash, segment.firstCanonicalFrame.value());
            appendInteger(hash, segment.lastCanonicalFrame.value());
            appendInteger(
                hash, static_cast<std::underlying_type_t<AlignmentSegmentState>>(segment.state));
            appendFloat(hash, segment.meanConfidence);
            appendFloat(hash, segment.p10Confidence);
            appendInteger(hash, segment.maximumLowConfidenceRun);
            appendFloat(hash, segment.anomalyDensity);
            appendInteger(hash, static_cast<std::uint8_t>(segment.sceneCutProximity));
            appendFloat(hash, segment.mappingSlope);
        }
        for (const SequenceAlignmentAnomaly& anomaly : result.anomalies) {
            appendInteger(
                hash,
                static_cast<std::underlying_type_t<SequenceAlignmentAnomalyKind>>(anomaly.kind));
            appendInteger(hash,
                          anomaly.canonicalFrameId.has_value() ? anomaly.canonicalFrameId->value()
                                                               : -1);
            appendInteger(hash,
                          anomaly.sourceFrameId.has_value() ? anomaly.sourceFrameId->value() : -1);
        }
    }
    return "alignment-v3-" + hexadecimal(hash);
}

} // namespace dvs::application
