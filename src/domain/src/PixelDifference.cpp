#include "dvs/domain/PixelDifference.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace dvs::domain {
namespace {

// BT.709 luma weights, matching the GPU difference shader's commonBt709Luma so the LumaOnly
// policy counts exactly the pixels the threshold filter keeps on screen.
constexpr double kLumaWeightR = 0.2126;
constexpr double kLumaWeightG = 0.7152;
constexpr double kLumaWeightB = 0.0722;

// Mismatch predicate in 8-bit code values, mirroring the GPU filter's `sample >= threshold`
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

} // namespace dvs::domain
