#include "dvs/domain/PixelDifference.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace dvs::domain {
namespace {

// BT.709 luma weights share the GPU difference shader's commonBt709Luma definition. The CPU
// predicate acts on its supplied RGBA8 buffers; different conversion/sampling paths mean its
// counted pixels need not match the on-screen threshold highlight.
constexpr double kLumaWeightR = 0.2126;
constexpr double kLumaWeightG = 0.7152;
constexpr double kLumaWeightB = 0.0722;

// Mismatch predicate in supplied 8-bit RGB values, sharing the GPU filter's `sample >= threshold`
// comparison. The `sample > 0` guard keeps threshold 0 from counting identical pixels; under
// AllChannels the pixel must differ in every channel, under LumaOnly only the weighted luma
// delta of the signed channel differences decides.
[[nodiscard]] bool policyMismatch(const int deltaR,
                                  const int deltaG,
                                  const int deltaB,
                                  const std::uint8_t mismatchThreshold,
                                  const MismatchPolicy policy) noexcept {
    const double absR = std::abs(deltaR);
    const double absG = std::abs(deltaG);
    const double absB = std::abs(deltaB);
    const double threshold = static_cast<double>(mismatchThreshold);
    switch (policy) {
    case MismatchPolicy::LumaOnly: {
        const double lumaDelta = std::abs(kLumaWeightR * static_cast<double>(deltaR) +
                                          kLumaWeightG * static_cast<double>(deltaG) +
                                          kLumaWeightB * static_cast<double>(deltaB));
        return lumaDelta > 0.0 && lumaDelta >= threshold;
    }
    case MismatchPolicy::AllChannels: {
        const double smallest = std::min(absR, std::min(absG, absB));
        return smallest > 0.0 && smallest >= threshold;
    }
    case MismatchPolicy::AnyChannel:
        break;
    }
    const double largest = std::max(absR, std::max(absG, absB));
    return largest > 0.0 && largest >= threshold;
}

} // namespace

std::optional<PixelDifferenceMetrics>
computeRgbAbsoluteMetrics(const Rgba8View first,
                          const Rgba8View second,
                          const std::uint8_t mismatchThreshold,
                          const MismatchPolicy policy) noexcept {
    if (!first.isValid() || !second.isValid() || first.width != second.width ||
        first.height != second.height) {
        return std::nullopt;
    }

    PixelDifferenceMetrics metrics{};
    metrics.bitDepth = 8;
    metrics.pixelCount = first.width * first.height;
    metrics.channelSamples = metrics.pixelCount * 3U;

    double absSum = 0.0;
    double squareSum = 0.0;
    double maxAbs = 0.0;
    std::uint64_t mismatchPixels = 0U;

    for (std::size_t y = 0; y < first.height; ++y) {
        const std::uint8_t* firstRow = first.pixels + y * first.strideBytes;
        const std::uint8_t* secondRow = second.pixels + y * second.strideBytes;
        for (std::size_t x = 0; x < first.width; ++x) {
            const std::size_t offset = x * 4U;
            int channelDeltas[3] = {0, 0, 0};
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                const int delta = static_cast<int>(firstRow[offset + channel]) -
                                  static_cast<int>(secondRow[offset + channel]);
                channelDeltas[channel] = delta;
                const double absolute = static_cast<double>(std::abs(delta));
                absSum += absolute;
                squareSum += absolute * absolute;
                maxAbs = std::max(maxAbs, absolute);
            }
            const bool pixelMismatch = policyMismatch(
                channelDeltas[0], channelDeltas[1], channelDeltas[2], mismatchThreshold, policy);
            if (pixelMismatch) {
                ++mismatchPixels;
            }
        }
    }

    metrics.mae =
        metrics.channelSamples > 0U ? absSum / static_cast<double>(metrics.channelSamples) : 0.0;
    metrics.mse =
        metrics.channelSamples > 0U ? squareSum / static_cast<double>(metrics.channelSamples) : 0.0;
    metrics.maxAbsError = maxAbs;
    metrics.mismatchPixels = mismatchPixels;
    metrics.mismatchRatio = metrics.pixelCount > 0U ? static_cast<double>(mismatchPixels) /
                                                          static_cast<double>(metrics.pixelCount)
                                                    : 0.0;
    if (metrics.mse <= 0.0) {
        metrics.psnrDb = kInfinitePsnrDb;
    } else {
        constexpr double kMaxSample = 255.0;
        metrics.psnrDb = 10.0 * std::log10((kMaxSample * kMaxSample) / metrics.mse);
    }
    return metrics;
}

PixelDifferenceMetrics RgbAbsoluteAnalysis::metricsAt(const std::uint8_t threshold,
                                                      const MismatchPolicy policy) const noexcept {
    std::size_t slot = 1U;
    if (policy == MismatchPolicy::LumaOnly) {
        slot = 0U;
    } else if (policy == MismatchPolicy::AllChannels) {
        slot = 2U;
    }
    PixelDifferenceMetrics result = metrics;
    result.mismatchPixels = mismatchCounts[slot][threshold];
    result.mismatchRatio = result.pixelCount > 0U ? static_cast<double>(result.mismatchPixels) /
                                                        static_cast<double>(result.pixelCount)
                                                  : 0.0;
    return result;
}

std::optional<RgbAbsoluteAnalysis> computeRgbAbsoluteAnalysis(const Rgba8View first,
                                                              const Rgba8View second) noexcept {
    if (!first.isValid() || !second.isValid() || first.width != second.width ||
        first.height != second.height) {
        return std::nullopt;
    }
    RgbAbsoluteAnalysis analysis;
    analysis.metrics.pixelCount = first.width * first.height;
    analysis.metrics.channelSamples = analysis.metrics.pixelCount * 3U;
    double absSum = 0.0;
    double squareSum = 0.0;
    for (std::size_t y = 0; y < first.height; ++y) {
        const auto* firstRow = first.pixels + y * first.strideBytes;
        const auto* secondRow = second.pixels + y * second.strideBytes;
        for (std::size_t x = 0; x < first.width; ++x) {
            const auto offset = x * 4U;
            const int deltaR = static_cast<int>(firstRow[offset]) - secondRow[offset];
            const int deltaG = static_cast<int>(firstRow[offset + 1U]) - secondRow[offset + 1U];
            const int deltaB = static_cast<int>(firstRow[offset + 2U]) - secondRow[offset + 2U];
            const int absoluteR = std::abs(deltaR);
            const int absoluteG = std::abs(deltaG);
            const int absoluteB = std::abs(deltaB);
            absSum += static_cast<double>(absoluteR);
            absSum += static_cast<double>(absoluteG);
            absSum += static_cast<double>(absoluteB);
            squareSum += static_cast<double>(deltaR * deltaR);
            squareSum += static_cast<double>(deltaG * deltaG);
            squareSum += static_cast<double>(deltaB * deltaB);
            const int maximum = std::max(absoluteR, std::max(absoluteG, absoluteB));
            const int minimum = std::min(absoluteR, std::min(absoluteG, absoluteB));
            analysis.metrics.maxAbsError =
                std::max(analysis.metrics.maxAbsError, static_cast<double>(maximum));
            if (maximum > 0) {
                ++analysis.mismatchCounts[1][static_cast<std::size_t>(maximum)];
            }
            if (minimum > 0) {
                ++analysis.mismatchCounts[2][static_cast<std::size_t>(minimum)];
            }
            const double lumaDelta = std::abs(kLumaWeightR * static_cast<double>(deltaR) +
                                              kLumaWeightG * static_cast<double>(deltaG) +
                                              kLumaWeightB * static_cast<double>(deltaB));
            if (lumaDelta > 0.0) {
                // A positive, bounded delta truncates exactly like floor, without a per-pixel
                // libm call. Fractional luma below one belongs only to threshold zero.
                const auto bin = static_cast<std::size_t>(std::min(255.0, lumaDelta));
                ++analysis.mismatchCounts[0][bin];
            }
        }
    }
    for (auto& counts : analysis.mismatchCounts) {
        for (std::size_t threshold = counts.size() - 1U; threshold > 0U; --threshold) {
            counts[threshold - 1U] += counts[threshold];
        }
    }
    analysis.metrics.mae = absSum / static_cast<double>(analysis.metrics.channelSamples);
    analysis.metrics.mse = squareSum / static_cast<double>(analysis.metrics.channelSamples);
    analysis.metrics.psnrDb = analysis.metrics.mse <= 0.0
                                  ? kInfinitePsnrDb
                                  : 10.0 * std::log10((255.0 * 255.0) / analysis.metrics.mse);
    return analysis;
}

} // namespace dvs::domain
