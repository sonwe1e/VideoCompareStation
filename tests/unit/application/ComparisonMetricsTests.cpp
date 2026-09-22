#include "dvs/application/ComparisonMetrics.h"

#include <gtest/gtest.h>
#include <vector>

namespace dvs::application {
namespace {

[[nodiscard]] domain::Rgba8View makeView(std::vector<std::uint8_t>& buffer) {
    return domain::Rgba8View{
        .pixels = buffer.data(),
        .width = 1,
        .height = 1,
        .strideBytes = 4,
    };
}

} // namespace

TEST(ComparisonMetricsTests, ScoresActivePairWithProvenance) {
    std::vector<std::uint8_t> first{10U, 10U, 10U, 255U};
    std::vector<std::uint8_t> second{20U, 20U, 20U, 255U};
    const auto scored = scoreActivePairRgbAbsolute(
        domain::ComparisonPair{0, 2}, domain::FrameId{7}, makeView(first), makeView(second));
    ASSERT_TRUE(scored.has_value());
    EXPECT_EQ(scored->pair.first, 0U);
    EXPECT_EQ(scored->pair.second, 2U);
    EXPECT_EQ(scored->frameId, domain::FrameId{7});
    EXPECT_EQ(scored->metricId, kRgbAbsoluteMetricId);
    EXPECT_DOUBLE_EQ(scored->metrics.mae, 10.0);
}

TEST(ComparisonMetricsTests, RejectsInvalidPairOrMismatchedGeometry) {
    std::vector<std::uint8_t> pixels{0U, 0U, 0U, 255U};
    EXPECT_FALSE(
        scoreActivePairRgbAbsolute(
            domain::ComparisonPair{1, 1}, domain::FrameId{0}, makeView(pixels), makeView(pixels))
            .has_value());
    EXPECT_FALSE(
        scoreActivePairRgbAbsolute(
            domain::ComparisonPair{0, 1}, domain::FrameId{-1}, makeView(pixels), makeView(pixels))
            .has_value());
}

} // namespace dvs::application
