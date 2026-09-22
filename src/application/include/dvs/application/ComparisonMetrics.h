#pragma once

#include "dvs/domain/ComparisonSelection.h"
#include "dvs/domain/FrameTimeline.h"
#include "dvs/domain/PixelDifference.h"

#include <optional>
#include <string_view>

namespace dvs::application {

// Adapter-neutral metric identity. UI/export must never invent a formula name.
inline constexpr std::string_view kRgbAbsoluteMetricId = "cpu-rgb-absolute-v1";

// One current-frame sample scored for the session's active ComparisonPair.
struct ActivePairFrameMetrics final {
    domain::ComparisonPair pair{};
    domain::FrameId frameId{0};
    domain::PixelDifferenceMetrics metrics{};
    std::string_view metricId = kRgbAbsoluteMetricId;

    [[nodiscard]] bool operator==(const ActivePairFrameMetrics&) const = default;
};

// Pure scoring helper. Pixel buffers come from adapters; this never owns decode or GPU types.
// Returns nullopt when the pair is invalid or either view is unusable/mismatched.
[[nodiscard]] std::optional<ActivePairFrameMetrics>
scoreActivePairRgbAbsolute(domain::ComparisonPair pair,
                           domain::FrameId frameId,
                           domain::Rgba8View first,
                           domain::Rgba8View second,
                           std::uint8_t mismatchThreshold = 0U) noexcept;

} // namespace dvs::application
