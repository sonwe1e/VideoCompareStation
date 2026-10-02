#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace dvs::domain {

// CPU reference view over tightly packed or strided RGBA8 pixels. Adapters own the buffers;
// analysis never requires a graphics API type.
struct Rgba8View final {
    const std::uint8_t* pixels = nullptr;
    std::size_t width = 0;
    std::size_t height = 0;
    std::size_t strideBytes = 0;

    [[nodiscard]] bool isValid() const noexcept {
        return pixels != nullptr && width > 0 && height > 0 && strideBytes >= width * 4U;
    }
};

// Channel policy for the mismatch predicate. Values and order mirror
// presentation::ThresholdPolicy so the GPU highlight filter and the CPU bad-pixel statistics
// apply the same rule to the same threshold.
enum class MismatchPolicy : std::uint8_t {
    LumaOnly = 0,
    AnyChannel = 1,
    AllChannels = 2,
};

// Provenance-stable RGB absolute metrics for one active ComparisonPair sample.
// Alpha is never averaged into MAE/MSE; the mismatch predicate follows `policy`.
struct PixelDifferenceMetrics final {
    double mae = 0.0;
    double mse = 0.0;
    // kInfinitePsnrDb when mse == 0 for identical buffers.
    double psnrDb = 0.0;
    double maxAbsError = 0.0;
    double mismatchRatio = 0.0;
    std::uint64_t channelSamples = 0;
    std::uint64_t mismatchPixels = 0;
    std::uint64_t pixelCount = 0;
    std::uint8_t bitDepth = 8;

    [[nodiscard]] bool operator==(const PixelDifferenceMetrics&) const = default;
};

// CPU scalar reference for RGB absolute difference (plan phase 5 first batch).
// Dimensions must match. The mismatch predicate mirrors the GPU difference filter on 8-bit
// code values: the policy sample (BT.709 luma delta, max channel delta, or min channel delta)
// must reach `mismatchThreshold`. Threshold 0 never counts identical pixels; under
// MismatchPolicy::AllChannels a pixel still has to differ in every channel, and under
// LumaOnly only the weighted luma delta decides.
[[nodiscard]] std::optional<PixelDifferenceMetrics>
computeRgbAbsoluteMetrics(Rgba8View first,
                          Rgba8View second,
                          std::uint8_t mismatchThreshold = 0U,
                          MismatchPolicy policy = MismatchPolicy::AnyChannel) noexcept;

// Threshold-independent analysis. Each policy stores cumulative counts for all integer
// thresholds [0,255], so a new display predicate needs no pixels and no new analysis pass.
// Only positive policy deltas enter the distribution: luma deltas in (0,1) count at threshold
// zero but not at one. Alpha is excluded, just like the independent scalar reference above.
struct RgbAbsoluteAnalysis final {
    // Only threshold-independent fields are populated here. Read mismatch counts/ratios via
    // metricsAt(), never from this base summary.
    PixelDifferenceMetrics metrics{};
    std::array<std::array<std::uint64_t, 256U>, 3U> mismatchCounts{};

    [[nodiscard]] PixelDifferenceMetrics
    metricsAt(std::uint8_t threshold,
              MismatchPolicy policy = MismatchPolicy::AnyChannel) const noexcept;
    [[nodiscard]] bool operator==(const RgbAbsoluteAnalysis&) const = default;
};

[[nodiscard]] std::optional<RgbAbsoluteAnalysis>
computeRgbAbsoluteAnalysis(Rgba8View first, Rgba8View second) noexcept;

// Identical buffers report mse=0 and psnrDb = kInfinitePsnrDb.
inline constexpr double kInfinitePsnrDb = 1000.0;

} // namespace dvs::domain
