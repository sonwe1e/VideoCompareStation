#include "dvs/presentation/ComparisonContract.h"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>

namespace dvs::presentation {
namespace {

TEST(ComparisonContractTests, PreservesPublishedPresentationEnumValues) {
    EXPECT_EQ(static_cast<std::uint8_t>(ViewMode::SideBySide), 0U);
    EXPECT_EQ(static_cast<std::uint8_t>(ViewMode::Single), 6U);
    EXPECT_EQ(static_cast<std::uint8_t>(ViewMode::Fade), 7U);
    EXPECT_EQ(static_cast<std::uint8_t>(DifferenceMetric::ExactPlanes), 4U);
    EXPECT_EQ(static_cast<std::uint8_t>(DifferenceMetric::SignedSubtract), 5U);
    EXPECT_EQ(static_cast<std::uint8_t>(DifferenceMetric::Highlight), 6U);
    EXPECT_EQ(static_cast<std::uint8_t>(DifferenceMetric::Crossfade), 7U);
    EXPECT_EQ(static_cast<std::uint8_t>(DifferenceGain::Gain16x), 4U);
    EXPECT_EQ(static_cast<std::uint8_t>(DifferenceEdge::Between1And2), 2U);
    EXPECT_EQ(static_cast<std::uint8_t>(DifferenceFilter::Bicubic), 2U);
    EXPECT_EQ(static_cast<std::uint8_t>(ThresholdPolicy::AllChannels), 2U);
}

TEST(ComparisonContractTests, DescribesExactlyTheCurrentOneTwoAndThreeSourceModes) {
    EXPECT_EQ(comparisonModeDescriptors().size(), 8U);

    EXPECT_TRUE(isViewModeAvailable(ViewMode::Single, 1U));
    EXPECT_FALSE(isViewModeAvailable(ViewMode::SideBySide, 1U));

    for (const ViewMode mode :
         std::array{ViewMode::SideBySide, ViewMode::Difference, ViewMode::Wipe, ViewMode::Fade}) {
        EXPECT_TRUE(isViewModeAvailable(mode, 2U));
    }
    EXPECT_FALSE(isViewModeAvailable(ViewMode::ThreeUp, 2U));

    for (const ViewMode mode : std::array{ViewMode::SideBySide,
                                          ViewMode::ThreeUp,
                                          ViewMode::ReferenceFocus,
                                          ViewMode::Difference,
                                          ViewMode::AnalysisGrid,
                                          ViewMode::Wipe,
                                          ViewMode::Fade}) {
        EXPECT_TRUE(isViewModeAvailable(mode, 3U));
    }
    EXPECT_FALSE(isViewModeAvailable(ViewMode::Single, 3U));
}

TEST(ComparisonContractTests, FallsBackToSingleOrSideBySideForUnavailableModes) {
    EXPECT_EQ(effectiveViewMode(ViewMode::Difference, 0U), ViewMode::Single);
    EXPECT_EQ(effectiveViewMode(ViewMode::ThreeUp, 2U), ViewMode::SideBySide);
    EXPECT_EQ(effectiveViewMode(ViewMode::Single, 3U), ViewMode::SideBySide);
    EXPECT_EQ(effectiveViewMode(ViewMode::Wipe, 2U), ViewMode::Wipe);
}

TEST(ComparisonContractTests, ValidatesCompleteComparisonAndViewportState) {
    ComparisonViewConfig config;
    EXPECT_TRUE(config.isValid());

    config.threshold = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(config.isValid());
    config.threshold = 0.5F;
    config.viewport.roiEnabled = true;
    config.viewport.roiLeft = config.viewport.roiRight;
    EXPECT_FALSE(config.isValid());
}

} // namespace
} // namespace dvs::presentation
