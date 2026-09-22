#include "dvs/application/ComparisonExactness.h"
#include "dvs/application/SessionSnapshot.h"
#include "dvs/domain/ComparisonValidator.h"

#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace dvs::application {
namespace {

[[nodiscard]] domain::MediaDescriptor descriptor(const domain::MediaExtent extent,
                                                 const domain::ColorMatrix matrix) {
    auto rate = domain::RationalRate::create(30, 1);
    EXPECT_TRUE(rate);
    return domain::MediaDescriptor{
        .normalizedPath = "C:/media/source.mp4",
        .extent = extent,
        .frameRate = std::move(rate).value(),
        .frameCount =
            domain::FrameCountInfo{
                .value = 10,
                .origin = domain::FrameCountOrigin::kIndexed,
            },
        .duration = domain::MediaTime{333'333},
        .codecId = "h264",
        .pixelFormatId = "nv12",
        .bitDepth = 8U,
        .colorMetadata =
            domain::ColorMetadata{
                .matrix = matrix,
                .range = domain::ColorRange::kLimited,
            },
        .decodeCapabilities =
            domain::DecodeCapabilities{
                .softwareDecode = true,
            },
        .timingConfidence = domain::TimingConfidence::kVerifiedCfr,
    };
}

[[nodiscard]] SessionSnapshot
snapshot(const domain::MediaExtent secondExtent = {1'920U, 1'080U},
         const domain::ColorMatrix secondMatrix = domain::ColorMatrix::kBt709) {
    auto validated = domain::ComparisonValidator::validate({
        domain::ComparisonSource{
            .id = 0U,
            .role = domain::ComparisonRole::kReference,
            .descriptor = descriptor({1'920U, 1'080U}, domain::ColorMatrix::kBt709),
            .displayName = "Reference",
        },
        domain::ComparisonSource{
            .id = 1U,
            .role = domain::ComparisonRole::kPrediction,
            .descriptor = descriptor(secondExtent, secondMatrix),
            .displayName = "Prediction",
        },
    });
    EXPECT_TRUE(validated);
    SessionSnapshot result;
    result.displayedFrame = domain::FrameId{0};
    result.validatedComparison =
        std::make_shared<const domain::ValidatedComparisonSet>(std::move(validated).value().set);
    result.presentedSources = {
        PresentedSourceState{
            .sourceId = 0U,
            .sourceFrameId = domain::FrameId{0},
            .matchKind = FrameMatchKind::ExactIndex,
        },
        PresentedSourceState{
            .sourceId = 1U,
            .sourceFrameId = domain::FrameId{0},
            .matchKind = FrameMatchKind::ExactIndex,
        },
    };
    return result;
}

TEST(ComparisonExactnessTests, DistinguishesExactDisplaySpatialTemporalAndUnavailableStates) {
    SessionSnapshot exact = snapshot();
    EXPECT_EQ(comparisonExactness(exact, 0U, 1U), ComparisonExactness::ExactCodeValue);

    const SessionSnapshot converted = snapshot({1'920U, 1'080U}, domain::ColorMatrix::kBt601);
    EXPECT_EQ(comparisonExactness(converted, 0U, 1U), ComparisonExactness::DisplaySpaceConverted);

    const SessionSnapshot spatial = snapshot({1'280U, 720U});
    EXPECT_EQ(comparisonExactness(spatial, 0U, 1U), ComparisonExactness::SpatiallyResampled);

    SessionSnapshot temporal = snapshot();
    temporal.presentedSources[1].matchKind = FrameMatchKind::AutoAligned;
    EXPECT_EQ(comparisonExactness(temporal, 0U, 1U), ComparisonExactness::TemporallyAligned);

    SessionSnapshot unavailable = snapshot();
    unavailable.presentedSources[1].sourceFrameId.reset();
    unavailable.presentedSources[1].matchKind = FrameMatchKind::Missing;
    unavailable.presentedSources[1].missingReason = MissingReason::AlignmentGap;
    EXPECT_EQ(comparisonExactness(unavailable, 0U, 1U), ComparisonExactness::Unavailable);
}

TEST(ComparisonExactnessTests, TreatsChromaOrRgbNormalizationAsDisplaySpaceConversion) {
    SessionSnapshot normalized = snapshot();
    ASSERT_TRUE(normalized.validatedComparison);
    const std::span<const domain::ComparisonSource> currentSources =
        normalized.validatedComparison->sources();
    std::vector<domain::ComparisonSource> sources{currentSources.begin(), currentSources.end()};
    sources[0U].descriptor.pixelFormatId = "yuv444p";
    sources[1U].descriptor.pixelFormatId = "yuv444p";
    auto validation = domain::ComparisonValidator::validate(std::move(sources));
    ASSERT_TRUE(validation);
    normalized.validatedComparison =
        std::make_shared<const domain::ValidatedComparisonSet>(std::move(validation).value().set);

    EXPECT_EQ(comparisonExactness(normalized, 0U, 1U), ComparisonExactness::DisplaySpaceConverted);
}

TEST(ComparisonExactnessTests, MarksRgbAnd422InputsAsDisplaySpaceConverted) {
    // RGB and 4:2:2 sources are normalized onto the NV12/P010 4:2:0 display path, so their
    // sampled pixel values are display-converted, never original code values. The marking
    // must stay visible for every such input, not just 4:4:4.
    for (const char* const format : {"rgb24", "yuv422p", "yuv444p", "rgba"}) {
        SessionSnapshot normalized = snapshot();
        ASSERT_TRUE(normalized.validatedComparison);
        const std::span<const domain::ComparisonSource> currentSources =
            normalized.validatedComparison->sources();
        std::vector<domain::ComparisonSource> sources{currentSources.begin(), currentSources.end()};
        sources[0U].descriptor.pixelFormatId = format;
        sources[1U].descriptor.pixelFormatId = format;
        auto validation = domain::ComparisonValidator::validate(std::move(sources));
        ASSERT_TRUE(validation);
        normalized.validatedComparison = std::make_shared<const domain::ValidatedComparisonSet>(
            std::move(validation).value().set);
        EXPECT_EQ(comparisonExactness(normalized, 0U, 1U),
                  ComparisonExactness::DisplaySpaceConverted)
            << "format=" << format;
    }
}

TEST(ComparisonExactnessTests, ReportsEveryInexactDimensionInsteadOfOnlyTheTopReason) {
    // A pair can be temporally aligned, spatially resampled and display-space converted
    // at once; the single enum names only the first, so the UI needs the decomposition.
    SessionSnapshot multi = snapshot({1'280U, 720U}, domain::ColorMatrix::kBt601);
    multi.presentedSources[1].matchKind = FrameMatchKind::AutoAligned;
    const ComparisonExactnessDimensions dimensions = comparisonExactnessDimensions(multi, 0U, 1U);
    EXPECT_TRUE(dimensions.available);
    EXPECT_FALSE(dimensions.temporalExact);
    EXPECT_FALSE(dimensions.spatialExact);
    EXPECT_FALSE(dimensions.pixelExact);
    // The enum still resolves to the highest-priority reason only.
    EXPECT_EQ(comparisonExactness(multi, 0U, 1U), ComparisonExactness::TemporallyAligned);

    const SessionSnapshot exact = snapshot();
    const ComparisonExactnessDimensions exactDimensions =
        comparisonExactnessDimensions(exact, 0U, 1U);
    EXPECT_TRUE(exactDimensions.available);
    EXPECT_TRUE(exactDimensions.temporalExact);
    EXPECT_TRUE(exactDimensions.spatialExact);
    EXPECT_TRUE(exactDimensions.pixelExact);

    SessionSnapshot unavailable = snapshot();
    unavailable.presentedSources[1].sourceFrameId.reset();
    unavailable.presentedSources[1].matchKind = FrameMatchKind::Missing;
    const ComparisonExactnessDimensions unavailableDimensions =
        comparisonExactnessDimensions(unavailable, 0U, 1U);
    EXPECT_FALSE(unavailableDimensions.available);
    EXPECT_FALSE(unavailableDimensions.temporalExact);
    EXPECT_FALSE(unavailableDimensions.spatialExact);
    EXPECT_FALSE(unavailableDimensions.pixelExact);
}

} // namespace
} // namespace dvs::application
