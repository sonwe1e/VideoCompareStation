#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace dvs::presentation {

enum class ViewMode : std::uint8_t {
    SideBySide = 0,
    ThreeUp = 1,
    ReferenceFocus = 2,
    Difference = 3,
    AnalysisGrid = 4,
    Wipe = 5,
    Single = 6,
    Fade = 7,
};

enum class DifferenceMetric : std::uint8_t {
    RgbAbsolute = 0,
    Luma = 1,
    Chroma = 2,
    Heatmap = 3,
    ExactPlanes = 4,
    SignedSubtract = 5,
    Highlight = 6,
    Crossfade = 7,
};

enum class DifferenceGain : std::uint8_t {
    Gain1x = 0,
    Gain2x = 1,
    Gain4x = 2,
    Gain8x = 3,
    Gain16x = 4,
};

enum class DifferenceEdge : std::uint8_t {
    Edge0And1 = 0,
    Edge0And2 = 1,
    Edge1And2 = 2,
    Between0And1 = Edge0And1,
    Between0And2 = Edge0And2,
    Between1And2 = Edge1And2,
};

enum class DifferenceFilter : std::uint8_t {
    Nearest = 0,
    Bilinear = 1,
    Bicubic = 2,
};

enum class ThresholdPolicy : std::uint8_t {
    LumaOnly = 0,
    AnyChannel = 1,
    AllChannels = 2,
};

struct ComparisonModeDescriptor final {
    ViewMode mode = ViewMode::SideBySide;
    std::uint8_t minimumSourceCount = 2U;
    std::uint8_t maximumSourceCount = 3U;
    bool usesPair = false;
    bool supportsThreshold = false;
    bool supportsRoi = true;

    [[nodiscard]] constexpr bool supportsSourceCount(const std::size_t sourceCount) const noexcept {
        return sourceCount >= minimumSourceCount && sourceCount <= maximumSourceCount;
    }

    [[nodiscard]] constexpr bool operator==(const ComparisonModeDescriptor&) const = default;
};

struct ViewportState final {
    float centerX = 0.5F;
    float centerY = 0.5F;
    float scale = 1.0F;
    bool roiEnabled = false;
    float roiLeft = 0.0F;
    float roiTop = 0.0F;
    float roiRight = 1.0F;
    float roiBottom = 1.0F;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] bool operator==(const ViewportState&) const = default;
};

struct ComparisonViewConfig final {
    ViewMode mode = ViewMode::SideBySide;
    DifferenceMetric differenceMetric = DifferenceMetric::RgbAbsolute;
    DifferenceGain differenceGain = DifferenceGain::Gain1x;
    DifferenceEdge differenceEdge = DifferenceEdge::Between0And1;
    DifferenceFilter differenceFilter = DifferenceFilter::Bilinear;
    bool thresholdEnabled = false;
    float threshold = 0.0F;
    ThresholdPolicy thresholdPolicy = ThresholdPolicy::AnyChannel;
    float wipePosition = 0.5F;
    std::uint8_t referenceSlot = 0U;
    ViewportState viewport;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] bool operator==(const ComparisonViewConfig&) const = default;
};

[[nodiscard]] std::span<const ComparisonModeDescriptor> comparisonModeDescriptors() noexcept;
[[nodiscard]] const ComparisonModeDescriptor* comparisonModeDescriptor(ViewMode mode) noexcept;
[[nodiscard]] bool isViewModeAvailable(ViewMode mode, std::size_t sourceCount) noexcept;
[[nodiscard]] ViewMode effectiveViewMode(ViewMode requested, std::size_t sourceCount) noexcept;

} // namespace dvs::presentation
