#include "dvs/presentation/ComparisonContract.h"

#include <array>
#include <cmath>

namespace dvs::presentation {
namespace {

constexpr std::array<ComparisonModeDescriptor, 8U> kModeDescriptors{{
    {.mode = ViewMode::SideBySide, .minimumSourceCount = 2U, .maximumSourceCount = 3U},
    {.mode = ViewMode::ThreeUp, .minimumSourceCount = 3U, .maximumSourceCount = 3U},
    {.mode = ViewMode::ReferenceFocus, .minimumSourceCount = 3U, .maximumSourceCount = 3U},
    {.mode = ViewMode::Difference,
     .minimumSourceCount = 2U,
     .maximumSourceCount = 3U,
     .usesPair = true,
     .supportsThreshold = true},
    {.mode = ViewMode::AnalysisGrid,
     .minimumSourceCount = 3U,
     .maximumSourceCount = 3U,
     .usesPair = true,
     .supportsThreshold = true},
    {.mode = ViewMode::Wipe, .minimumSourceCount = 2U, .maximumSourceCount = 3U, .usesPair = true},
    {.mode = ViewMode::Single, .minimumSourceCount = 1U, .maximumSourceCount = 1U},
    {.mode = ViewMode::Fade, .minimumSourceCount = 2U, .maximumSourceCount = 3U, .usesPair = true},
}};

template <typename Enum>
[[nodiscard]] constexpr bool enumInRange(const Enum value, const Enum maximum) noexcept {
    return static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(maximum);
}

} // namespace

bool ViewportState::isValid() const noexcept {
    const bool transformValid = std::isfinite(centerX) && std::isfinite(centerY) &&
                                std::isfinite(scale) && centerX >= 0.0F && centerX <= 1.0F &&
                                centerY >= 0.0F && centerY <= 1.0F && scale >= 1.0F &&
                                scale <= 64.0F;
    const bool roiValid = std::isfinite(roiLeft) && std::isfinite(roiTop) &&
                          std::isfinite(roiRight) && std::isfinite(roiBottom) && roiLeft >= 0.0F &&
                          roiTop >= 0.0F && roiRight <= 1.0F && roiBottom <= 1.0F &&
                          roiLeft < roiRight && roiTop < roiBottom;
    return transformValid && (!roiEnabled || roiValid);
}

bool ComparisonViewConfig::isValid() const noexcept {
    return comparisonModeDescriptor(mode) != nullptr &&
           enumInRange(differenceMetric, DifferenceMetric::Crossfade) &&
           enumInRange(differenceGain, DifferenceGain::Gain16x) &&
           enumInRange(differenceEdge, DifferenceEdge::Between1And2) &&
           enumInRange(differenceFilter, DifferenceFilter::Bicubic) &&
           enumInRange(thresholdPolicy, ThresholdPolicy::AllChannels) && std::isfinite(threshold) &&
           threshold >= 0.0F && threshold <= 1.0F && std::isfinite(wipePosition) &&
           wipePosition >= 0.0F && wipePosition <= 1.0F && referenceSlot < 3U && viewport.isValid();
}

std::span<const ComparisonModeDescriptor> comparisonModeDescriptors() noexcept {
    return kModeDescriptors;
}

const ComparisonModeDescriptor* comparisonModeDescriptor(const ViewMode mode) noexcept {
    const std::size_t index = static_cast<std::size_t>(mode);
    return index < kModeDescriptors.size() ? &kModeDescriptors[index] : nullptr;
}

bool isViewModeAvailable(const ViewMode mode, const std::size_t sourceCount) noexcept {
    const ComparisonModeDescriptor* const descriptor = comparisonModeDescriptor(mode);
    return descriptor != nullptr && descriptor->supportsSourceCount(sourceCount);
}

ViewMode effectiveViewMode(const ViewMode requested, const std::size_t sourceCount) noexcept {
    if (isViewModeAvailable(requested, sourceCount)) {
        return requested;
    }
    return sourceCount <= 1U ? ViewMode::Single : ViewMode::SideBySide;
}

} // namespace dvs::presentation
