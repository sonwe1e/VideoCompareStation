#include "dvs/domain/PixelDifference.h"

#include <array>
#include <cmath>
#include <gtest/gtest.h>
#include <vector>

namespace dvs::domain {
namespace {

[[nodiscard]] Rgba8View
makeView(std::vector<std::uint8_t>& buffer, const std::size_t width, const std::size_t height) {
    return Rgba8View{
        .pixels = buffer.data(),
        .width = width,
        .height = height,
        .strideBytes = width * 4U,
    };
}

} // namespace

TEST(PixelDifferenceTests, IdenticalBuffersReportZeroErrorAndInfinitePsnr) {
    std::vector<std::uint8_t> first(2U * 2U * 4U, 64U);
    std::vector<std::uint8_t> second = first;
    const auto metrics = computeRgbAbsoluteMetrics(makeView(first, 2, 2), makeView(second, 2, 2));
    ASSERT_TRUE(metrics.has_value());
    EXPECT_DOUBLE_EQ(metrics->mae, 0.0);
    EXPECT_DOUBLE_EQ(metrics->mse, 0.0);
    EXPECT_DOUBLE_EQ(metrics->psnrDb, kInfinitePsnrDb);
    EXPECT_DOUBLE_EQ(metrics->maxAbsError, 0.0);
    EXPECT_EQ(metrics->mismatchPixels, 0U);
    EXPECT_EQ(metrics->channelSamples, 2U * 2U * 3U);
}

TEST(PixelDifferenceTests, KnownChannelDeltaProducesExpectedMaeMseAndPsnr) {
    // 1x1 RGBA: RGB delta of +10 on all three channels; alpha differs but is ignored.
    std::vector<std::uint8_t> first{10U, 20U, 30U, 255U};
    std::vector<std::uint8_t> second{20U, 30U, 40U, 0U};
    const auto metrics = computeRgbAbsoluteMetrics(makeView(first, 1, 1), makeView(second, 1, 1));
    ASSERT_TRUE(metrics.has_value());
    EXPECT_DOUBLE_EQ(metrics->mae, 10.0);
    EXPECT_DOUBLE_EQ(metrics->mse, 100.0);
    EXPECT_DOUBLE_EQ(metrics->maxAbsError, 10.0);
    EXPECT_EQ(metrics->mismatchPixels, 1U);
    EXPECT_DOUBLE_EQ(metrics->mismatchRatio, 1.0);
    const double expectedPsnr = 10.0 * std::log10((255.0 * 255.0) / 100.0);
    EXPECT_NEAR(metrics->psnrDb, expectedPsnr, 1e-9);
}

TEST(PixelDifferenceTests, MismatchThresholdIgnoresSubThresholdChannels) {
    std::vector<std::uint8_t> first{10U, 10U, 10U, 255U, 10U, 10U, 10U, 255U};
    std::vector<std::uint8_t> second{12U, 10U, 10U, 255U, 10U, 10U, 10U, 255U};
    const auto strict =
        computeRgbAbsoluteMetrics(makeView(first, 2, 1), makeView(second, 2, 1), 0U);
    ASSERT_TRUE(strict.has_value());
    EXPECT_EQ(strict->mismatchPixels, 1U);

    const auto tolerant =
        computeRgbAbsoluteMetrics(makeView(first, 2, 1), makeView(second, 2, 1), 3U);
    ASSERT_TRUE(tolerant.has_value());
    EXPECT_EQ(tolerant->mismatchPixels, 0U);
    EXPECT_GT(tolerant->mae, 0.0);
}

TEST(PixelDifferenceTests, RejectsMismatchedOrInvalidViews) {
    std::vector<std::uint8_t> first(16U, 0U);
    std::vector<std::uint8_t> smaller(8U, 0U);
    EXPECT_FALSE(
        computeRgbAbsoluteMetrics(makeView(first, 2, 2), makeView(smaller, 2, 1)).has_value());
    EXPECT_FALSE(computeRgbAbsoluteMetrics(Rgba8View{}, makeView(first, 2, 2)).has_value());
}

TEST(PixelDifferenceTests, StridePaddingIsIgnored) {
    // 2x1 with 16-byte stride: only the first 8 bytes are active pixels.
    std::array<std::uint8_t, 16> first{
        0U, 0U, 0U, 255U, 0U, 0U, 0U, 255U, 9U, 9U, 9U, 9U, 9U, 9U, 9U, 9U};
    std::array<std::uint8_t, 16> second{
        0U, 0U, 0U, 255U, 5U, 5U, 5U, 255U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U};
    const Rgba8View firstView{first.data(), 2, 1, 16};
    const Rgba8View secondView{second.data(), 2, 1, 16};
    const auto metrics = computeRgbAbsoluteMetrics(firstView, secondView);
    ASSERT_TRUE(metrics.has_value());
    // Second pixel RGB delta is 5 on each channel → abs sum 15 over 6 channel samples.
    EXPECT_NEAR(metrics->mae, 15.0 / 6.0, 1e-9);
    EXPECT_DOUBLE_EQ(metrics->maxAbsError, 5.0);
    EXPECT_EQ(metrics->mismatchPixels, 1U);
}

} // namespace dvs::domain
