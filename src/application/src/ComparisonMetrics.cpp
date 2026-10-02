#include "dvs/application/ComparisonMetrics.h"

namespace dvs::application {

std::optional<ActivePairFrameMetrics>
scoreActivePairRgbAbsolute(const domain::ComparisonPair pair,
                           const domain::FrameId frameId,
                           const domain::Rgba8View first,
                           const domain::Rgba8View second,
                           const std::uint8_t mismatchThreshold,
                           const domain::MismatchPolicy mismatchPolicy) noexcept {
    if (!pair.isValid() || !frameId.isValid()) {
        return std::nullopt;
    }
    const auto metrics =
        domain::computeRgbAbsoluteMetrics(first, second, mismatchThreshold, mismatchPolicy);
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

std::optional<ActivePairFrameAnalysis>
analyzeActivePairRgbAbsolute(const domain::ComparisonPair pair,
                             const domain::FrameId frameId,
                             const domain::Rgba8View first,
                             const domain::Rgba8View second) noexcept {
    if (!pair.isValid() || !frameId.isValid()) {
        return std::nullopt;
    }
    const auto analysis = domain::computeRgbAbsoluteAnalysis(first, second);
    if (!analysis.has_value()) {
        return std::nullopt;
    }
    return ActivePairFrameAnalysis{
        .pair = pair, .frameId = frameId, .analysis = *analysis, .metricId = kRgbAbsoluteMetricId};
}

} // namespace dvs::application
