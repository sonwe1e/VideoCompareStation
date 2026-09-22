#include "dvs/application/ComparisonMetrics.h"

namespace dvs::application {

std::optional<ActivePairFrameMetrics>
scoreActivePairRgbAbsolute(const domain::ComparisonPair pair,
                           const domain::FrameId frameId,
                           const domain::Rgba8View first,
                           const domain::Rgba8View second,
                           const std::uint8_t mismatchThreshold) noexcept {
    if (!pair.isValid() || !frameId.isValid()) {
        return std::nullopt;
    }
    const auto metrics = domain::computeRgbAbsoluteMetrics(first, second, mismatchThreshold);
    if (!metrics.has_value()) {
        return std::nullopt;
    }
    return ActivePairFrameMetrics{
        .pair = pair,
        .frameId = frameId,
        .metrics = *metrics,
        .metricId = kRgbAbsoluteMetricId,
    };
}

} // namespace dvs::application
