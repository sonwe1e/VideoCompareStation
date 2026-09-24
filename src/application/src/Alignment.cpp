#include "dvs/application/Alignment.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

namespace dvs::application {
namespace {

[[nodiscard]] float normalizedLumaDifference(const FrameLumaSignature& left,
                                             const FrameLumaSignature& right) noexcept {
    std::uint64_t total = 0U;
    for (std::size_t index = 0U; index < left.luma.size(); ++index) {
        total += static_cast<std::uint64_t>(
            std::abs(static_cast<int>(left.luma[index]) - static_cast<int>(right.luma[index])));
    }
    return static_cast<float>(total) /
           static_cast<float>(left.luma.size() * static_cast<std::size_t>(255U));
}

[[nodiscard]] float structuralDistance(const FrameLumaSignature& left,
                                       const FrameLumaSignature& right) noexcept {
    constexpr double kC1 = 6.5025;
    constexpr double kC2 = 58.5225;
    const double inverseCount = 1.0 / static_cast<double>(left.luma.size());
    const double leftMean = std::accumulate(left.luma.begin(), left.luma.end(), 0.0) * inverseCount;
    const double rightMean =
        std::accumulate(right.luma.begin(), right.luma.end(), 0.0) * inverseCount;

    double leftVariance = 0.0;
    double rightVariance = 0.0;
    double covariance = 0.0;
    for (std::size_t index = 0U; index < left.luma.size(); ++index) {
        const double centeredLeft = static_cast<double>(left.luma[index]) - leftMean;
        const double centeredRight = static_cast<double>(right.luma[index]) - rightMean;
        leftVariance += centeredLeft * centeredLeft;
        rightVariance += centeredRight * centeredRight;
        covariance += centeredLeft * centeredRight;
    }
    leftVariance *= inverseCount;
    rightVariance *= inverseCount;
    covariance *= inverseCount;
    const double numerator = ((2.0 * leftMean * rightMean) + kC1) * ((2.0 * covariance) + kC2);
    const double denominator = ((leftMean * leftMean) + (rightMean * rightMean) + kC1) *
                               (leftVariance + rightVariance + kC2);
    if (denominator <= 0.0) {
        return 1.0F;
    }
    const double similarity = std::clamp(numerator / denominator, 0.0, 1.0);
    return static_cast<float>(1.0 - similarity);
}

[[nodiscard]] float gradientDistance(const FrameLumaSignature& left,
                                     const FrameLumaSignature& right) noexcept {
    std::uint64_t total = 0U;
    std::size_t edgeCount = 0U;
    for (std::size_t y = 0U; y < kAlignmentSignatureHeight; ++y) {
        for (std::size_t x = 0U; x < kAlignmentSignatureWidth; ++x) {
            const std::size_t index = (y * kAlignmentSignatureWidth) + x;
            if (x + 1U < kAlignmentSignatureWidth) {
                const int leftGradient =
                    static_cast<int>(left.luma[index + 1U]) - static_cast<int>(left.luma[index]);
                const int rightGradient =
                    static_cast<int>(right.luma[index + 1U]) - static_cast<int>(right.luma[index]);
                total += static_cast<std::uint64_t>(std::abs(leftGradient - rightGradient));
                ++edgeCount;
            }
            if (y + 1U < kAlignmentSignatureHeight) {
                const std::size_t below = index + kAlignmentSignatureWidth;
                const int leftGradient =
                    static_cast<int>(left.luma[below]) - static_cast<int>(left.luma[index]);
                const int rightGradient =
                    static_cast<int>(right.luma[below]) - static_cast<int>(right.luma[index]);
                total += static_cast<std::uint64_t>(std::abs(leftGradient - rightGradient));
                ++edgeCount;
            }
        }
    }
    return edgeCount == 0U ? 1.0F
                           : static_cast<float>(total) /
                                 static_cast<float>(edgeCount * static_cast<std::size_t>(510U));
}

template <std::size_t Size>
[[nodiscard]] float
normalizedFeatureDifference(const std::array<std::uint8_t, Size>& left,
                            const std::array<std::uint8_t, Size>& right) noexcept {
    std::uint64_t total = 0U;
    for (std::size_t index = 0U; index < Size; ++index) {
        total += static_cast<std::uint64_t>(
            std::abs(static_cast<int>(left[index]) - static_cast<int>(right[index])));
    }
    return static_cast<float>(total) / static_cast<float>(Size * 255U);
}

[[nodiscard]] float signatureDistance(const FrameLumaSignature& left,
                                      const FrameLumaSignature& right) noexcept {
    const float hashDistance =
        static_cast<float>(std::popcount(left.perceptualHash ^ right.perceptualHash)) / 64.0F;
    const float varianceDistance = std::abs(left.normalizedVariance - right.normalizedVariance);
    return (0.30F * structuralDistance(left, right)) + (0.15F * gradientDistance(left, right)) +
           (0.08F * hashDistance) + (0.07F * normalizedLumaDifference(left, right)) +
           (0.20F * normalizedFeatureDifference(left.detailBlocks, right.detailBlocks)) +
           (0.15F * normalizedFeatureDifference(left.sobelBlocks, right.sobelBlocks)) +
           (0.05F * varianceDistance);
}

[[nodiscard]] const FrameLumaSignature*
findSignature(const std::span<const FrameLumaSignature> signatures,
              const std::int64_t frameId) noexcept {
    const auto found =
        std::find_if(signatures.begin(), signatures.end(), [frameId](const auto& signature) {
            return signature.frameId.value() == frameId;
        });
    return found == signatures.end() ? nullptr : &*found;
}

[[nodiscard]] std::vector<const FrameLumaSignature*>
selectEvidence(const std::span<const FrameLumaSignature> reference,
               const std::size_t selectedCount) {
    struct RankedSignature final {
        const FrameLumaSignature* signature = nullptr;
        float activity = 0.0F;
    };
    std::vector<RankedSignature> ranked;
    ranked.reserve(reference.size());
    for (std::size_t index = 0U; index < reference.size(); ++index) {
        float activity = gradientDistance(reference[index],
                                          FrameLumaSignature{
                                              .frameId = reference[index].frameId,
                                              .displayTime = std::nullopt,
                                              .luma = {},
                                              .detailBlocks = {},
                                              .sobelBlocks = {},
                                              .normalizedVariance = 0.0F,
                                              .perceptualHash = 0,
                                          });
        if (index > 0U) {
            activity += normalizedLumaDifference(reference[index - 1U], reference[index]);
        }
        if (index + 1U < reference.size()) {
            activity += normalizedLumaDifference(reference[index], reference[index + 1U]);
        }
        ranked.push_back(RankedSignature{
            .signature = &reference[index],
            .activity = activity,
        });
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        return left.activity > right.activity;
    });
    const std::size_t count = std::min(selectedCount, ranked.size());
    std::vector<const FrameLumaSignature*> selected;
    selected.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        selected.push_back(ranked[index].signature);
    }
    return selected;
}

[[nodiscard]] float median(std::vector<float> values) {
    const std::size_t middle = values.size() / 2U;
    std::nth_element(
        values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle), values.end());
    if ((values.size() % 2U) != 0U) {
        return values[middle];
    }
    const float upper = values[middle];
    const float lower =
        *std::max_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle));
    return (lower + upper) * 0.5F;
}

} // namespace

bool FrameLumaSignature::isValid() const noexcept {
    return frameId.isValid() && (!displayTime.has_value() || displayTime->microseconds() >= 0) &&
           std::isfinite(normalizedVariance) && normalizedVariance >= 0.0F &&
           normalizedVariance <= 1.0F;
}

bool GlobalOffsetEstimationOptions::isValid() const noexcept {
    return minimumOffset <= maximumOffset && minimumOffset >= -64 && maximumOffset <= 64 &&
           selectedSampleCount > 0U && selectedSampleCount <= 64U && minimumEvidence > 0U &&
           minimumEvidence <= selectedSampleCount && std::isfinite(minimumConfidence) &&
           minimumConfidence >= 0.0F && minimumConfidence <= 1.0F &&
           std::isfinite(maximumBestCost) && maximumBestCost >= 0.0F && maximumBestCost <= 1.0F;
}

FrameLumaSignature makeFrameLumaSignature(
    const domain::FrameId frameId,
    const std::span<const std::uint8_t, kAlignmentSignaturePixels> luma) noexcept {
    std::array<std::uint8_t, kAlignmentDetailPixels> detail{};
    for (std::size_t y = 0U; y < kAlignmentDetailHeight; ++y) {
        const std::size_t sourceY = y * kAlignmentSignatureHeight / kAlignmentDetailHeight;
        for (std::size_t x = 0U; x < kAlignmentDetailWidth; ++x) {
            const std::size_t sourceX = x * kAlignmentSignatureWidth / kAlignmentDetailWidth;
            detail[(y * kAlignmentDetailWidth) + x] =
                luma[(sourceY * kAlignmentSignatureWidth) + sourceX];
        }
    }
    return makeMultiScaleFrameLumaSignature(frameId, detail);
}

FrameLumaSignature makeMultiScaleFrameLumaSignature(
    const domain::FrameId frameId,
    const std::span<const std::uint8_t, kAlignmentDetailPixels> detailLuma,
    const std::optional<domain::MediaTime> displayTime) noexcept {
    FrameLumaSignature signature{
        .frameId = frameId,
        .displayTime = displayTime,
    };
    for (std::size_t targetY = 0U; targetY < kAlignmentSignatureHeight; ++targetY) {
        const std::size_t sourceY0 = targetY * kAlignmentDetailHeight / kAlignmentSignatureHeight;
        const std::size_t sourceY1 =
            (targetY + 1U) * kAlignmentDetailHeight / kAlignmentSignatureHeight;
        for (std::size_t targetX = 0U; targetX < kAlignmentSignatureWidth; ++targetX) {
            const std::size_t sourceX0 = targetX * kAlignmentDetailWidth / kAlignmentSignatureWidth;
            const std::size_t sourceX1 =
                (targetX + 1U) * kAlignmentDetailWidth / kAlignmentSignatureWidth;
            std::uint64_t sum = 0U;
            std::size_t count = 0U;
            for (std::size_t y = sourceY0; y < sourceY1; ++y) {
                for (std::size_t x = sourceX0; x < sourceX1; ++x) {
                    sum += detailLuma[(y * kAlignmentDetailWidth) + x];
                    ++count;
                }
            }
            signature.luma[(targetY * kAlignmentSignatureWidth) + targetX] =
                static_cast<std::uint8_t>(sum / count);
        }
    }

    double sum = 0.0;
    double squaredSum = 0.0;
    for (const std::uint8_t value : detailLuma) {
        const double sample = static_cast<double>(value);
        sum += sample;
        squaredSum += sample * sample;
    }
    const double count = static_cast<double>(detailLuma.size());
    const double mean = sum / count;
    const double variance = std::max(0.0, (squaredSum / count) - (mean * mean));
    signature.normalizedVariance =
        static_cast<float>(std::clamp(variance / (255.0 * 255.0), 0.0, 1.0));

    for (std::size_t blockY = 0U; blockY < kAlignmentFeatureGridHeight; ++blockY) {
        const std::size_t y0 = blockY * kAlignmentDetailHeight / kAlignmentFeatureGridHeight;
        const std::size_t y1 = (blockY + 1U) * kAlignmentDetailHeight / kAlignmentFeatureGridHeight;
        for (std::size_t blockX = 0U; blockX < kAlignmentFeatureGridWidth; ++blockX) {
            const std::size_t x0 = blockX * kAlignmentDetailWidth / kAlignmentFeatureGridWidth;
            const std::size_t x1 =
                (blockX + 1U) * kAlignmentDetailWidth / kAlignmentFeatureGridWidth;
            std::uint64_t blockSum = 0U;
            std::uint64_t sobelSum = 0U;
            std::size_t blockCount = 0U;
            for (std::size_t y = y0; y < y1; ++y) {
                for (std::size_t x = x0; x < x1; ++x) {
                    const std::size_t index = (y * kAlignmentDetailWidth) + x;
                    blockSum += detailLuma[index];
                    if (x > 0U && x + 1U < kAlignmentDetailWidth && y > 0U &&
                        y + 1U < kAlignmentDetailHeight) {
                        const int horizontal = static_cast<int>(detailLuma[index + 1U]) -
                                               static_cast<int>(detailLuma[index - 1U]);
                        const int vertical =
                            static_cast<int>(detailLuma[index + kAlignmentDetailWidth]) -
                            static_cast<int>(detailLuma[index - kAlignmentDetailWidth]);
                        sobelSum += static_cast<std::uint64_t>(
                            std::min(255, std::abs(horizontal) + std::abs(vertical)));
                    }
                    ++blockCount;
                }
            }
            const std::size_t block = (blockY * kAlignmentFeatureGridWidth) + blockX;
            signature.detailBlocks[block] = static_cast<std::uint8_t>(blockSum / blockCount);
            signature.sobelBlocks[block] = static_cast<std::uint8_t>(sobelSum / blockCount);
        }
    }

    std::uint64_t hashSum = 0U;
    for (std::size_t y = 0U; y < 8U; ++y) {
        for (std::size_t x = 0U; x < 8U; ++x) {
            const std::size_t sourceX = (x * kAlignmentSignatureWidth) / 8U;
            const std::size_t sourceY = (y * kAlignmentSignatureHeight) / 8U;
            hashSum += signature.luma[(sourceY * kAlignmentSignatureWidth) + sourceX];
        }
    }
    const std::uint8_t hashMean = static_cast<std::uint8_t>(hashSum / 64U);
    for (std::size_t y = 0U; y < 8U; ++y) {
        for (std::size_t x = 0U; x < 8U; ++x) {
            const std::size_t sourceX = (x * kAlignmentSignatureWidth) / 8U;
            const std::size_t sourceY = (y * kAlignmentSignatureHeight) / 8U;
            const std::size_t bit = (y * 8U) + x;
            if (signature.luma[(sourceY * kAlignmentSignatureWidth) + sourceX] >= hashMean) {
                signature.perceptualHash |= std::uint64_t{1} << bit;
            }
        }
    }
    return signature;
}

std::optional<GlobalOffsetEstimate>
estimateGlobalOffset(const domain::SourceId targetSourceId,
                     const std::span<const FrameLumaSignature> reference,
                     const std::span<const FrameLumaSignature> target,
                     const GlobalOffsetEstimationOptions& options) noexcept {
    try {
        if (!options.isValid() || reference.size() < options.minimumEvidence ||
            target.size() < options.minimumEvidence ||
            std::any_of(reference.begin(),
                        reference.end(),
                        [](const auto& value) { return !value.isValid(); }) ||
            std::any_of(
                target.begin(), target.end(), [](const auto& value) { return !value.isValid(); })) {
            return std::nullopt;
        }

        const std::vector<const FrameLumaSignature*> selected =
            selectEvidence(reference, options.selectedSampleCount);
        struct Candidate final {
            std::int64_t offset = 0;
            float cost = 1.0F;
            std::size_t evidence = 0U;
        };
        std::vector<Candidate> candidates;
        for (std::int64_t offset = options.minimumOffset; offset <= options.maximumOffset;
             ++offset) {
            std::vector<float> costs;
            costs.reserve(selected.size());
            for (const FrameLumaSignature* const referenceSignature : selected) {
                const std::int64_t referenceFrame = referenceSignature->frameId.value();
                const bool underflow = offset < 0 && offset < -referenceFrame;
                const bool overflow =
                    offset > 0 &&
                    referenceFrame > (std::numeric_limits<std::int64_t>::max)() - offset;
                if (underflow || overflow) {
                    continue;
                }
                const FrameLumaSignature* const targetSignature =
                    findSignature(target, referenceFrame + offset);
                if (targetSignature != nullptr) {
                    costs.push_back(signatureDistance(*referenceSignature, *targetSignature));
                }
            }
            if (costs.size() >= options.minimumEvidence) {
                const std::size_t evidence = costs.size();
                candidates.push_back(Candidate{
                    .offset = offset,
                    .cost = median(std::move(costs)),
                    .evidence = evidence,
                });
            }
            if (offset == options.maximumOffset) {
                break;
            }
        }
        if (candidates.empty()) {
            return std::nullopt;
        }
        std::stable_sort(
            candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
                if (left.cost != right.cost) {
                    return left.cost < right.cost;
                }
                return std::abs(left.offset) < std::abs(right.offset);
            });
        const Candidate& best = candidates.front();
        const float runnerUp = candidates.size() > 1U ? candidates[1U].cost : 1.0F;
        const float denominator = std::max(runnerUp, 0.000001F);
        const float confidence = runnerUp <= best.cost
                                     ? 0.0F
                                     : std::clamp((runnerUp - best.cost) / denominator, 0.0F, 1.0F);
        return GlobalOffsetEstimate{
            .sourceId = targetSourceId,
            .bestOffset = best.offset,
            .bestCost = best.cost,
            .runnerUpCost = runnerUp,
            .confidence = confidence,
            .evidenceCount = best.evidence,
            .autoApplicable = best.evidence >= options.minimumEvidence &&
                              best.cost <= options.maximumBestCost &&
                              confidence >= options.minimumConfidence,
        };
    } catch (...) {
        return std::nullopt;
    }
}

bool SequenceAlignmentOptions::isValid() const noexcept {
    return bandWidth > 0U && bandWidth <= 64U &&
           expectedOffset >= -static_cast<std::int64_t>(bandWidth) &&
           expectedOffset <= static_cast<std::int64_t>(bandWidth) && std::isfinite(gapPenalty) &&
           gapPenalty > 0.0F && gapPenalty <= 1.0F && std::isfinite(duplicateDistance) &&
           duplicateDistance >= 0.0F && duplicateDistance <= 1.0F &&
           std::isfinite(minimumConfidence) && minimumConfidence >= 0.0F &&
           minimumConfidence <= 1.0F && std::isfinite(maximumMeanMatchCost) &&
           maximumMeanMatchCost >= 0.0F && maximumMeanMatchCost <= 1.0F &&
           std::isfinite(sceneCutDistance) && sceneCutDistance >= 0.0F &&
           sceneCutDistance <= 1.0F && segmentLength > 0U && segmentLength <= 10'000U &&
           maximumLowConfidenceRun > 0U && maximumLowConfidenceRun <= segmentLength &&
           std::isfinite(minimumSegmentP10Confidence) && minimumSegmentP10Confidence >= 0.0F &&
           minimumSegmentP10Confidence <= 1.0F && std::isfinite(maximumSegmentAnomalyDensity) &&
           maximumSegmentAnomalyDensity >= 0.0F && maximumSegmentAnomalyDensity <= 1.0F;
}

std::optional<SequenceAlignmentResult>
alignFrameSequences(const domain::SourceId targetSourceId,
                    const std::span<const FrameLumaSignature> reference,
                    const std::span<const FrameLumaSignature> target,
                    const SequenceAlignmentOptions& options,
                    const std::span<const ManualAlignmentAnchor> anchors) noexcept {
    try {
        if (!options.isValid() || reference.empty() || target.empty() ||
            reference.size() >
                static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)()) ||
            target.size() > static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)())) {
            return std::nullopt;
        }
        const auto isContiguous = [](const std::span<const FrameLumaSignature> sequence) {
            for (std::size_t index = 0U; index < sequence.size(); ++index) {
                if (!sequence[index].isValid() ||
                    sequence[index].frameId.value() != static_cast<std::int64_t>(index)) {
                    return false;
                }
            }
            return true;
        };
        if (!isContiguous(reference) || !isContiguous(target)) {
            return std::nullopt;
        }
        const auto hasCompleteTimeline = [](const std::span<const FrameLumaSignature> sequence) {
            const bool any = std::any_of(sequence.begin(), sequence.end(), [](const auto& value) {
                return value.displayTime.has_value();
            });
            if (!any) {
                return false;
            }
            for (std::size_t index = 0U; index < sequence.size(); ++index) {
                if (!sequence[index].displayTime.has_value() ||
                    (index > 0U &&
                     sequence[index - 1U].displayTime >= sequence[index].displayTime)) {
                    return false;
                }
            }
            return true;
        };
        const bool referenceHasTimeline = hasCompleteTimeline(reference);
        const bool targetHasTimeline = hasCompleteTimeline(target);
        const bool anyTimeline =
            std::any_of(reference.begin(),
                        reference.end(),
                        [](const auto& value) { return value.displayTime.has_value(); }) ||
            std::any_of(target.begin(), target.end(), [](const auto& value) {
                return value.displayTime.has_value();
            });
        if (anyTimeline && (!referenceHasTimeline || !targetHasTimeline)) {
            return std::nullopt;
        }
        for (std::size_t index = 0U; index < anchors.size(); ++index) {
            if (!anchors[index].canonicalFrameId.isValid() ||
                !anchors[index].sourceFrameId.isValid() ||
                anchors[index].canonicalFrameId.value() >=
                    static_cast<std::int64_t>(reference.size()) ||
                anchors[index].sourceFrameId.value() >= static_cast<std::int64_t>(target.size()) ||
                (index > 0U &&
                 (anchors[index - 1U].canonicalFrameId >= anchors[index].canonicalFrameId ||
                  anchors[index - 1U].sourceFrameId >= anchors[index].sourceFrameId))) {
                return std::nullopt;
            }
        }

        enum class Step : std::uint8_t {
            None,
            Match,
            TargetMissing,
            TargetExtra,
        };
        struct Cell final {
            float cost = (std::numeric_limits<float>::infinity)();
            Step step = Step::None;
        };
        struct Row final {
            std::size_t firstColumn = 0U;
            std::vector<Cell> cells;

            [[nodiscard]] const Cell* find(const std::size_t column) const noexcept {
                if (column < firstColumn || column - firstColumn >= cells.size()) {
                    return nullptr;
                }
                return &cells[column - firstColumn];
            }

            [[nodiscard]] Cell* find(const std::size_t column) noexcept {
                return const_cast<Cell*>(std::as_const(*this).find(column));
            }
        };

        const std::int64_t targetCount = static_cast<std::int64_t>(target.size());
        const auto boundaryTime = [](const std::span<const FrameLumaSignature> sequence,
                                     const std::size_t prefix) {
            if (prefix < sequence.size()) {
                return sequence[prefix].displayTime->microseconds();
            }
            if (sequence.size() == 1U) {
                return sequence.front().displayTime->microseconds() + 1;
            }
            const std::int64_t last = sequence.back().displayTime->microseconds();
            const std::int64_t previous =
                sequence[sequence.size() - 2U].displayTime->microseconds();
            return last + std::max<std::int64_t>(1, last - previous);
        };
        std::int64_t expectedTimeOffset = 0;
        if (referenceHasTimeline && options.expectedOffset > 0) {
            const std::size_t frame =
                std::min(static_cast<std::size_t>(options.expectedOffset), target.size() - 1U);
            expectedTimeOffset = target[frame].displayTime->microseconds();
        } else if (referenceHasTimeline && options.expectedOffset < 0) {
            const std::size_t frame =
                std::min(static_cast<std::size_t>(-options.expectedOffset), reference.size() - 1U);
            expectedTimeOffset = -reference[frame].displayTime->microseconds();
        }
        const auto baseCenterFor = [&](const std::size_t referencePrefix) {
            if (!referenceHasTimeline) {
                return std::clamp<std::int64_t>(static_cast<std::int64_t>(referencePrefix) +
                                                    options.expectedOffset,
                                                0,
                                                targetCount);
            }
            const std::int64_t guidedTime =
                boundaryTime(reference, referencePrefix) + expectedTimeOffset;
            const auto targetBoundary =
                std::lower_bound(target.begin(),
                                 target.end(),
                                 guidedTime,
                                 [](const FrameLumaSignature& signature, const std::int64_t time) {
                                     return signature.displayTime->microseconds() < time;
                                 });
            return static_cast<std::int64_t>(targetBoundary - target.begin());
        };
        std::vector<std::pair<std::size_t, std::int64_t>> constraints;
        constraints.reserve(anchors.size() + 2U);
        constraints.emplace_back(0U, 0);
        for (const ManualAlignmentAnchor& anchor : anchors) {
            constraints.emplace_back(static_cast<std::size_t>(anchor.canonicalFrameId.value()) + 1U,
                                     anchor.sourceFrameId.value() + 1);
        }
        constraints.emplace_back(reference.size(), targetCount);
        const auto centerFor = [&](const std::size_t referencePrefix) {
            const std::int64_t base = baseCenterFor(referencePrefix);
            const auto right =
                std::lower_bound(constraints.begin(),
                                 constraints.end(),
                                 referencePrefix,
                                 [](const auto& constraint, const std::size_t prefix) {
                                     return constraint.first < prefix;
                                 });
            if (right == constraints.begin()) {
                return right->second;
            }
            if (right == constraints.end()) {
                return constraints.back().second;
            }
            const auto& left = *(right - 1);
            if (right->first == referencePrefix) {
                return right->second;
            }
            const std::int64_t leftCorrection = left.second - baseCenterFor(left.first);
            const std::int64_t rightCorrection = right->second - baseCenterFor(right->first);
            const long double ratio = static_cast<long double>(referencePrefix - left.first) /
                                      static_cast<long double>(right->first - left.first);
            const auto correction = static_cast<std::int64_t>(
                std::llround(static_cast<long double>(leftCorrection) +
                             (static_cast<long double>(rightCorrection - leftCorrection) * ratio)));
            return std::clamp<std::int64_t>(base + correction, 0, targetCount);
        };
        const auto boundsFor = [&](const std::size_t referencePrefix) {
            const std::int64_t center = centerFor(referencePrefix);
            const std::int64_t first =
                std::max<std::int64_t>(0, center - static_cast<std::int64_t>(options.bandWidth));
            const std::int64_t last = std::min<std::int64_t>(
                targetCount, center + static_cast<std::int64_t>(options.bandWidth));
            return std::pair{static_cast<std::size_t>(first),
                             last < first ? static_cast<std::size_t>(first - 1)
                                          : static_cast<std::size_t>(last)};
        };

        std::vector<Row> rows(reference.size() + 1U);
        for (std::size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
            const auto [first, last] = boundsFor(rowIndex);
            if (last < first) {
                return std::nullopt;
            }
            rows[rowIndex].firstColumn = first;
            rows[rowIndex].cells.resize(last - first + 1U);
        }
        Cell* const start = rows.front().find(0U);
        if (start == nullptr) {
            return std::nullopt;
        }
        start->cost = 0.0F;

        const auto crossesAnchor = [&](const std::size_t referencePrefix,
                                       const std::size_t targetPrefix) {
            return std::any_of(anchors.begin(), anchors.end(), [&](const auto& anchor) {
                const std::size_t requiredReference =
                    static_cast<std::size_t>(anchor.canonicalFrameId.value()) + 1U;
                const std::size_t requiredTarget =
                    static_cast<std::size_t>(anchor.sourceFrameId.value()) + 1U;
                return (referencePrefix < requiredReference && targetPrefix >= requiredTarget) ||
                       (referencePrefix >= requiredReference && targetPrefix < requiredTarget);
            });
        };
        const auto isAnchorCell = [&](const std::size_t referencePrefix,
                                      const std::size_t targetPrefix) {
            return std::any_of(anchors.begin(), anchors.end(), [&](const auto& anchor) {
                return referencePrefix ==
                           static_cast<std::size_t>(anchor.canonicalFrameId.value()) + 1U &&
                       targetPrefix == static_cast<std::size_t>(anchor.sourceFrameId.value()) + 1U;
            });
        };
        const auto matchDistance = [&](const std::size_t referenceIndex,
                                       const std::size_t targetIndex) {
            const float spatial = signatureDistance(reference[referenceIndex], target[targetIndex]);
            if (referenceIndex == 0U || targetIndex == 0U) {
                return spatial;
            }
            const float referenceMotion =
                normalizedLumaDifference(reference[referenceIndex - 1U], reference[referenceIndex]);
            const float targetMotion =
                normalizedLumaDifference(target[targetIndex - 1U], target[targetIndex]);
            return (0.85F * spatial) + (0.15F * std::abs(referenceMotion - targetMotion));
        };
        for (std::size_t i = 0U; i < rows.size(); ++i) {
            Row& row = rows[i];
            for (std::size_t local = 0U; local < row.cells.size(); ++local) {
                const std::size_t j = row.firstColumn + local;
                if (i == 0U && j == 0U) {
                    continue;
                }
                if (crossesAnchor(i, j)) {
                    continue;
                }
                Cell& cell = row.cells[local];
                if (i > 0U && j > 0U) {
                    if (const Cell* const diagonal = rows[i - 1U].find(j - 1U);
                        diagonal != nullptr && std::isfinite(diagonal->cost)) {
                        cell.cost = diagonal->cost + matchDistance(i - 1U, j - 1U);
                        cell.step = Step::Match;
                    }
                }
                if (isAnchorCell(i, j)) {
                    continue;
                }
                if (i > 0U) {
                    if (const Cell* const above = rows[i - 1U].find(j);
                        above != nullptr && std::isfinite(above->cost)) {
                        const float cost = above->cost + options.gapPenalty;
                        if (cost < cell.cost) {
                            cell.cost = cost;
                            cell.step = Step::TargetMissing;
                        }
                    }
                }
                if (j > 0U) {
                    if (const Cell* const left = row.find(j - 1U);
                        left != nullptr && std::isfinite(left->cost)) {
                        const float cost = left->cost + options.gapPenalty;
                        if (cost < cell.cost) {
                            cell.cost = cost;
                            cell.step = Step::TargetExtra;
                        }
                    }
                }
            }
        }

        const Cell* const end = rows.back().find(target.size());
        if (end == nullptr || !std::isfinite(end->cost)) {
            return std::nullopt;
        }
        std::vector<std::optional<std::size_t>> mapping(reference.size());
        std::vector<SequenceAlignmentAnomaly> anomalies;
        std::size_t i = reference.size();
        std::size_t j = target.size();
        while (i != 0U || j != 0U) {
            const Cell* const cell = rows[i].find(j);
            if (cell == nullptr || cell->step == Step::None) {
                return std::nullopt;
            }
            switch (cell->step) {
            case Step::Match:
                mapping[i - 1U] = j - 1U;
                --i;
                --j;
                break;
            case Step::TargetMissing:
                mapping[i - 1U] = std::nullopt;
                anomalies.push_back(SequenceAlignmentAnomaly{
                    .kind = SequenceAlignmentAnomalyKind::TargetFrameMissing,
                    .canonicalFrameId = reference[i - 1U].frameId,
                    .sourceFrameId = std::nullopt,
                });
                --i;
                break;
            case Step::TargetExtra: {
                const std::size_t targetIndex = j - 1U;
                float previousDistance = 1.0F;
                float nextDistance = 1.0F;
                if (i > 0U) {
                    previousDistance = signatureDistance(reference[i - 1U], target[targetIndex]);
                }
                if (i < reference.size()) {
                    nextDistance = signatureDistance(reference[i], target[targetIndex]);
                }
                const bool duplicate =
                    std::min(previousDistance, nextDistance) <= options.duplicateDistance;
                std::optional<domain::FrameId> canonicalFrame;
                if (i > 0U && (i == reference.size() || previousDistance <= nextDistance)) {
                    canonicalFrame = reference[i - 1U].frameId;
                } else if (i < reference.size()) {
                    canonicalFrame = reference[i].frameId;
                }
                anomalies.push_back(SequenceAlignmentAnomaly{
                    .kind = duplicate ? SequenceAlignmentAnomalyKind::TargetFrameDuplicate
                                      : SequenceAlignmentAnomalyKind::TargetFrameExtra,
                    .canonicalFrameId = canonicalFrame,
                    .sourceFrameId = target[targetIndex].frameId,
                });
                --j;
                break;
            }
            case Step::None:
                return std::nullopt;
            }
        }
        std::reverse(anomalies.begin(), anomalies.end());

        SequenceAlignmentResult result{
            .sourceId = targetSourceId,
            .entries = {},
            .anomalies = std::move(anomalies),
            .segments = {},
            .totalCost = end->cost,
            .meanMatchCost = 1.0F,
            .confidence = 0.0F,
            .autoApplicable = false,
        };
        result.entries.reserve(reference.size());
        float totalMatchCost = 0.0F;
        float totalConfidence = 0.0F;
        std::size_t matchCount = 0U;
        for (std::size_t referenceIndex = 0U; referenceIndex < mapping.size(); ++referenceIndex) {
            if (!mapping[referenceIndex].has_value()) {
                result.entries.push_back(SequenceAlignmentEntry{
                    .canonicalFrameId = reference[referenceIndex].frameId,
                    .sourceFrameId = std::nullopt,
                    .matchKind = FrameMatchKind::Missing,
                    .confidence = 0.0F,
                });
                continue;
            }

            const std::size_t targetIndex = *mapping[referenceIndex];
            const float cost = matchDistance(referenceIndex, targetIndex);
            float runnerUp = 1.0F;
            const std::int64_t center =
                std::max<std::int64_t>(0, centerFor(referenceIndex + 1U) - 1);
            const std::int64_t first =
                std::max<std::int64_t>(0, center - static_cast<std::int64_t>(options.bandWidth));
            const std::int64_t last = std::min<std::int64_t>(
                targetCount - 1, center + static_cast<std::int64_t>(options.bandWidth));
            for (std::int64_t candidate = first; candidate <= last; ++candidate) {
                if (static_cast<std::size_t>(candidate) == targetIndex) {
                    continue;
                }
                runnerUp = std::min(
                    runnerUp, matchDistance(referenceIndex, static_cast<std::size_t>(candidate)));
            }
            const float margin =
                runnerUp <= cost
                    ? 0.0F
                    : std::clamp((runnerUp - cost) / std::max(runnerUp, 0.000001F), 0.0F, 1.0F);
            const float confidence = margin * (1.0F - cost);
            result.entries.push_back(SequenceAlignmentEntry{
                .canonicalFrameId = reference[referenceIndex].frameId,
                .sourceFrameId = target[targetIndex].frameId,
                .matchKind = FrameMatchKind::AutoAligned,
                .confidence = confidence,
            });
            totalMatchCost += cost;
            totalConfidence += confidence;
            ++matchCount;
        }
        if (matchCount == 0U) {
            return std::nullopt;
        }
        result.meanMatchCost = totalMatchCost / static_cast<float>(matchCount);
        result.confidence = totalConfidence / static_cast<float>(matchCount);
        std::vector<bool> sceneCuts(reference.size(), false);
        for (std::size_t frame = 1U; frame < reference.size(); ++frame) {
            bool sceneCut = signatureDistance(reference[frame - 1U], reference[frame]) >=
                            options.sceneCutDistance;
            if (!sceneCut && mapping[frame - 1U].has_value() && mapping[frame].has_value()) {
                const std::size_t previousTarget = *mapping[frame - 1U];
                const std::size_t currentTarget = *mapping[frame];
                if (currentTarget > previousTarget) {
                    sceneCut = signatureDistance(target[previousTarget], target[currentTarget]) >=
                               options.sceneCutDistance;
                }
            }
            sceneCuts[frame] = sceneCut;
        }

        std::vector<std::size_t> segmentBoundaries{0U};
        for (std::size_t frame = 1U; frame < reference.size(); ++frame) {
            if (sceneCuts[frame] || frame - segmentBoundaries.back() >= options.segmentLength) {
                segmentBoundaries.push_back(frame);
            }
        }
        segmentBoundaries.push_back(reference.size());

        bool hasAcceptedSegment = false;
        result.segments.reserve(segmentBoundaries.size() - 1U);
        for (std::size_t boundary = 1U; boundary < segmentBoundaries.size(); ++boundary) {
            const std::size_t first = segmentBoundaries[boundary - 1U];
            const std::size_t segmentEnd = segmentBoundaries[boundary];
            std::vector<float> confidences;
            confidences.reserve(segmentEnd - first);
            float confidenceTotal = 0.0F;
            std::size_t lowRun = 0U;
            std::size_t maximumLowRun = 0U;
            std::optional<std::size_t> firstMapped;
            std::optional<std::size_t> lastMapped;
            std::size_t mappedCount = 0U;
            for (std::size_t frame = first; frame < segmentEnd; ++frame) {
                const SequenceAlignmentEntry& entry = result.entries[frame];
                confidences.push_back(entry.confidence);
                confidenceTotal += entry.confidence;
                if (!entry.sourceFrameId.has_value() ||
                    entry.confidence < options.minimumConfidence) {
                    ++lowRun;
                    maximumLowRun = std::max(maximumLowRun, lowRun);
                } else {
                    lowRun = 0U;
                }
                if (entry.sourceFrameId.has_value()) {
                    const std::size_t mapped =
                        static_cast<std::size_t>(entry.sourceFrameId->value());
                    firstMapped = firstMapped.value_or(mapped);
                    lastMapped = mapped;
                    ++mappedCount;
                }
            }
            std::sort(confidences.begin(), confidences.end());
            const std::size_t p10Index = static_cast<std::size_t>(
                std::floor(static_cast<double>(confidences.size() - 1U) * 0.10));
            const float meanConfidence = confidenceTotal / static_cast<float>(confidences.size());
            const float p10Confidence = confidences[p10Index];
            const std::size_t anomalyCount = static_cast<std::size_t>(std::count_if(
                result.anomalies.begin(),
                result.anomalies.end(),
                [&](const SequenceAlignmentAnomaly& anomaly) {
                    return anomaly.canonicalFrameId.has_value() &&
                           anomaly.canonicalFrameId->value() >= static_cast<std::int64_t>(first) &&
                           anomaly.canonicalFrameId->value() <
                               static_cast<std::int64_t>(segmentEnd);
                }));
            const float anomalyDensity =
                static_cast<float>(anomalyCount) / static_cast<float>(segmentEnd - first);
            float mappingSlope = 1.0F;
            if (firstMapped.has_value() && lastMapped.has_value() && segmentEnd - first > 1U) {
                mappingSlope = static_cast<float>(*lastMapped - *firstMapped) /
                               static_cast<float>((segmentEnd - first) - 1U);
            }
            const bool sceneCutProximity =
                std::any_of(sceneCuts.begin() + static_cast<std::ptrdiff_t>(first),
                            sceneCuts.begin() + static_cast<std::ptrdiff_t>(segmentEnd),
                            [](const bool value) { return value; });

            AlignmentSegmentState state = AlignmentSegmentState::Accepted;
            if (mappedCount == 0U ||
                anomalyDensity > std::min(1.0F, options.maximumSegmentAnomalyDensity * 2.0F) ||
                mappingSlope <= 0.0F || mappingSlope > 4.0F) {
                state = AlignmentSegmentState::Rejected;
            } else if (p10Confidence < options.minimumSegmentP10Confidence ||
                       maximumLowRun > options.maximumLowConfidenceRun ||
                       anomalyDensity > options.maximumSegmentAnomalyDensity ||
                       (sceneCutProximity && meanConfidence < options.minimumConfidence)) {
                state = AlignmentSegmentState::ReviewRequired;
            } else {
                hasAcceptedSegment = true;
            }
            result.segments.push_back(SequenceAlignmentSegment{
                .firstCanonicalFrame = domain::FrameId{static_cast<std::int64_t>(first)},
                .lastCanonicalFrame = domain::FrameId{static_cast<std::int64_t>(segmentEnd - 1U)},
                .state = state,
                .meanConfidence = meanConfidence,
                .p10Confidence = p10Confidence,
                .maximumLowConfidenceRun = maximumLowRun,
                .anomalyDensity = anomalyDensity,
                .sceneCutProximity = sceneCutProximity,
                .mappingSlope = mappingSlope,
            });
        }
        result.autoApplicable =
            result.meanMatchCost <= options.maximumMeanMatchCost && hasAcceptedSegment;
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

bool SourceAlignmentAnchors::isValid() const noexcept {
    if (anchors.empty()) {
        return false;
    }
    for (std::size_t index = 0U; index < anchors.size(); ++index) {
        const ManualAlignmentAnchor& anchor = anchors[index];
        if (!anchor.canonicalFrameId.isValid() || !anchor.sourceFrameId.isValid()) {
            return false;
        }
        if (index > 0U && (anchors[index - 1U].canonicalFrameId >= anchor.canonicalFrameId ||
                           anchors[index - 1U].sourceFrameId > anchor.sourceFrameId)) {
            return false;
        }
    }
    return true;
}

std::optional<SourceFrameOffset> mapFrameWithAnchors(const SourceAlignmentAnchors& anchors,
                                                     const domain::FrameId canonicalFrame,
                                                     const std::int64_t sourceFrameCount) noexcept {
    if (!anchors.isValid() || !canonicalFrame.isValid() || sourceFrameCount <= 0) {
        return std::nullopt;
    }

    std::int64_t mappedFrame = -1;
    const ManualAlignmentAnchor& first = anchors.anchors.front();
    const ManualAlignmentAnchor& last = anchors.anchors.back();
    if (canonicalFrame <= first.canonicalFrameId) {
        const std::int64_t delta = canonicalFrame.value() - first.canonicalFrameId.value();
        mappedFrame =
            delta < -first.sourceFrameId.value() ? -1 : first.sourceFrameId.value() + delta;
    } else if (canonicalFrame >= last.canonicalFrameId) {
        const std::int64_t delta = canonicalFrame.value() - last.canonicalFrameId.value();
        mappedFrame =
            delta > (std::numeric_limits<std::int64_t>::max)() - last.sourceFrameId.value()
                ? -1
                : last.sourceFrameId.value() + delta;
    } else {
        const auto upper =
            std::upper_bound(anchors.anchors.begin(),
                             anchors.anchors.end(),
                             canonicalFrame,
                             [](const domain::FrameId frame, const ManualAlignmentAnchor& anchor) {
                                 return frame < anchor.canonicalFrameId;
                             });
        if (upper == anchors.anchors.begin() || upper == anchors.anchors.end()) {
            return std::nullopt;
        }
        const ManualAlignmentAnchor& right = *upper;
        const ManualAlignmentAnchor& left = *(upper - 1);
        const std::int64_t canonicalSpan =
            right.canonicalFrameId.value() - left.canonicalFrameId.value();
        const std::int64_t sourceSpan = right.sourceFrameId.value() - left.sourceFrameId.value();
        const std::int64_t position = canonicalFrame.value() - left.canonicalFrameId.value();
        if (sourceSpan != 0 && position > (std::numeric_limits<std::int64_t>::max)() / sourceSpan) {
            return std::nullopt;
        }
        const std::int64_t scaled = ((position * sourceSpan) + (canonicalSpan / 2)) / canonicalSpan;
        mappedFrame = left.sourceFrameId.value() + scaled;
    }

    if (mappedFrame < 0 || mappedFrame >= sourceFrameCount) {
        return SourceFrameOffset{
            .sourceId = anchors.sourceId,
            .frames = 0,
            .matchKind = FrameMatchKind::Missing,
            .confidence = 1.0F,
        };
    }
    return SourceFrameOffset{
        .sourceId = anchors.sourceId,
        .frames = mappedFrame - canonicalFrame.value(),
        .matchKind = FrameMatchKind::ManualAnchor,
        .confidence = 1.0F,
    };
}

bool canonicalTimeBeyondSourceTimeline(const domain::FrameTimeline& timeline,
                                       const domain::MediaTime canonicalTime) noexcept {
    const std::int64_t frameCount = timeline.frameCount();
    if (frameCount <= 0) {
        return true;
    }
    const auto lastStart = timeline.frameStartTime(domain::FrameId{frameCount - 1});
    if (!lastStart.hasValue()) {
        return false;
    }
    const std::int64_t lastStartUs = lastStart.value().microseconds();
    if (canonicalTime.microseconds() < lastStartUs) {
        return false;
    }
    std::optional<std::int64_t> intervalUs;
    if (frameCount >= 2) {
        const auto previousStart = timeline.frameStartTime(domain::FrameId{frameCount - 2});
        if (previousStart.hasValue()) {
            intervalUs = lastStartUs - previousStart.value().microseconds();
        }
    }
    if (!intervalUs.has_value() || *intervalUs <= 0) {
        // A single-frame timeline has no observable cadence; keep the held frame instead of
        // guessing an end.
        return false;
    }
    return canonicalTime.microseconds() >= lastStartUs + *intervalUs;
}

} // namespace dvs::application
