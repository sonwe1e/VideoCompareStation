#include "dvs/application/AlignmentWorkEstimator.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>

namespace dvs::application {
namespace {

[[nodiscard]] std::uint64_t saturatedAdd(const std::uint64_t left,
                                         const std::uint64_t right) noexcept {
    return right > (std::numeric_limits<std::uint64_t>::max)() - left
               ? (std::numeric_limits<std::uint64_t>::max)()
               : left + right;
}

[[nodiscard]] const domain::ComparisonSource*
findCanonical(const std::vector<domain::ComparisonSource>& sources,
              const domain::SourceId canonicalSourceId) noexcept {
    const auto found =
        std::find_if(sources.begin(), sources.end(), [canonicalSourceId](const auto& source) {
            return source.id == canonicalSourceId;
        });
    return found == sources.end() ? nullptr : &*found;
}

[[nodiscard]] std::vector<domain::FrameId>
globalAnchors(const AlignmentEstimateRequest& request,
              const domain::ComparisonSource& canonical,
              GlobalOffsetEstimationOptions* const effectiveOptions) {
    const std::int64_t frameCount = canonical.descriptor.frameCount.value;
    const std::int64_t maximumWindow =
        (frameCount - static_cast<std::int64_t>(effectiveOptions->minimumEvidence)) / 2;
    effectiveOptions->minimumOffset = std::max(effectiveOptions->minimumOffset, -maximumWindow);
    effectiveOptions->maximumOffset = std::min(effectiveOptions->maximumOffset, maximumWindow);
    const std::int64_t safeFirst = std::max<std::int64_t>(0, -effectiveOptions->minimumOffset);
    const std::int64_t safeLast =
        frameCount - 1 - std::max<std::int64_t>(0, effectiveOptions->maximumOffset);
    if (safeLast < safeFirst) {
        return {};
    }

    const std::size_t sampleCount = std::min<std::size_t>(
        request.candidateSampleCount, static_cast<std::size_t>(safeLast - safeFirst + 1));
    std::vector<domain::FrameId> anchors;
    anchors.reserve(sampleCount);
    for (std::size_t index = 0U; index < sampleCount; ++index) {
        const std::int64_t frame =
            sampleCount == 1U
                ? safeFirst
                : safeFirst + static_cast<std::int64_t>(
                                  (static_cast<std::uint64_t>(safeLast - safeFirst) * index) /
                                  (sampleCount - 1U));
        if (anchors.empty() || anchors.back().value() != frame) {
            anchors.emplace_back(frame);
        }
    }
    return anchors;
}

[[nodiscard]] std::uint64_t dynamicProgrammingUnits(const std::int64_t referenceCount,
                                                    const std::int64_t targetCount,
                                                    const SequenceAlignmentOptions& options) {
    std::uint64_t units = 0U;
    for (std::int64_t prefix = 0; prefix <= referenceCount; ++prefix) {
        const std::int64_t center = prefix + options.expectedOffset;
        const std::int64_t first =
            std::max<std::int64_t>(0, center - static_cast<std::int64_t>(options.bandWidth));
        const std::int64_t last =
            std::min(targetCount, center + static_cast<std::int64_t>(options.bandWidth));
        if (last >= first) {
            units = saturatedAdd(units, static_cast<std::uint64_t>(last - first + 1));
        }
    }
    return units;
}

} // namespace

AlignmentWorkEstimate estimateAlignmentWork( // NOLINT(bugprone-exception-escape)
    const AlignmentEstimateRequest& request) noexcept {
    try {
        const domain::ComparisonSource* const canonical =
            findCanonical(request.sources, request.canonicalSourceId);
        if (canonical == nullptr || !request.options.isValid() ||
            request.candidateSampleCount < request.options.minimumEvidence ||
            canonical->descriptor.frameCount.value <
                static_cast<std::int64_t>(request.options.minimumEvidence)) {
            return AlignmentWorkEstimate{.unitName = "samples"};
        }

        GlobalOffsetEstimationOptions effectiveOptions = request.options;
        const std::vector<domain::FrameId> anchors =
            globalAnchors(request, *canonical, &effectiveOptions);
        std::uint64_t total = static_cast<std::uint64_t>(anchors.size());
        for (const domain::ComparisonSource& source : request.sources) {
            if (source.id == request.canonicalSourceId) {
                continue;
            }
            std::set<std::int64_t> targetFrames;
            for (const domain::FrameId anchor : anchors) {
                for (std::int64_t offset = effectiveOptions.minimumOffset;
                     offset <= effectiveOptions.maximumOffset;
                     ++offset) {
                    const std::int64_t frame = anchor.value() + offset;
                    if (frame >= 0 && frame < source.descriptor.frameCount.value) {
                        targetFrames.insert(frame);
                    }
                    if (offset == effectiveOptions.maximumOffset) {
                        break;
                    }
                }
            }
            total = saturatedAdd(total, static_cast<std::uint64_t>(targetFrames.size()));
        }
        return AlignmentWorkEstimate{.totalUnits = total, .unitName = "samples"};
    } catch (...) {
        return AlignmentWorkEstimate{.unitName = "samples"};
    }
}

AlignmentWorkEstimate estimateAlignmentWork( // NOLINT(bugprone-exception-escape)
    const SequenceAlignmentRequest& request) noexcept {
    try {
        const domain::ComparisonSource* const canonical =
            findCanonical(request.sources, request.canonicalSourceId);
        if (canonical == nullptr || !request.options.isValid()) {
            return AlignmentWorkEstimate{.unitName = "work units"};
        }

        std::uint64_t total = 0U;
        for (const domain::ComparisonSource& source : request.sources) {
            if (source.descriptor.frameCount.value <= 0) {
                return AlignmentWorkEstimate{.unitName = "work units"};
            }
            total =
                saturatedAdd(total, static_cast<std::uint64_t>(source.descriptor.frameCount.value));
            if (source.id == request.canonicalSourceId) {
                continue;
            }
            SequenceAlignmentOptions options = request.options;
            const auto expected = std::find_if(
                request.expectedOffsets.begin(),
                request.expectedOffsets.end(),
                [&source](const auto& offset) { return offset.sourceId == source.id; });
            options.expectedOffset =
                expected == request.expectedOffsets.end() ? 0 : expected->frames;
            total = saturatedAdd(total,
                                 dynamicProgrammingUnits(canonical->descriptor.frameCount.value,
                                                         source.descriptor.frameCount.value,
                                                         options));
        }
        return AlignmentWorkEstimate{.totalUnits = total, .unitName = "work units"};
    } catch (...) {
        return AlignmentWorkEstimate{.unitName = "work units"};
    }
}

} // namespace dvs::application
