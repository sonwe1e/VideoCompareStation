#pragma once

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

// Provenance-stable RGB absolute metrics for one active ComparisonPair sample.
// Alpha is never averaged into MAE/MSE; mismatch uses a per-channel absolute threshold.
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
// Dimensions must match. A pixel mismatches when any RGB channel abs error is > 0 and
// >= mismatchThreshold (threshold 0 still requires a non-zero channel delta).
[[nodiscard]] std::optional<PixelDifferenceMetrics> computeRgbAbsoluteMetrics(
    Rgba8View first, Rgba8View second, std::uint8_t mismatchThreshold = 0U) noexcept;

// Identical buffers report mse=0 and psnrDb = kInfinitePsnrDb.
inline constexpr double kInfinitePsnrDb = 1000.0;

} // namespace dvs::domain
