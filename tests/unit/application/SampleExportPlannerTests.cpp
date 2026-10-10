#include "dvs/application/SampleExport.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace dvs::application {
namespace {

// The strict-interior defense in the planner (target strictly between input0 and input1) is not
// unit-tested here: InterpolationProtocol::create enforces ordered inputs with
// 0 < numerator < denominator, and those invariants alone force an exact target to be strictly
// interior, so the defense cannot be reached through any publicly constructible protocol. It
// only guards against a future domain constructor regression.

using domain::GeneratedFrame;
using domain::GtFrameIndex;
using domain::MappingOrigin;
using domain::MissingFrame;
using domain::OutputFrameOrigin;
using domain::RetainedFrame;
using domain::UnknownFrame;

// Phase-zero 60->30->60 decimation over seven GT frames: R0 G R2 G R4 G R6.
[[nodiscard]] domain::InterpolationProtocol phaseZeroProtocol() {
    const auto built =
        domain::buildDecimatedProtocol(domain::DecimationPlan{.gtFrameCount = 7, .stride = 2},
                                       MappingOrigin::kImportedManifest,
                                       domain::TailPolicy::kTrimPartialInterval);
    EXPECT_TRUE(built.hasValue());
    return built.value();
}

TEST(SampleExportPlannerTests, PlansTheKnownFrameListForPhaseZeroDecimation) {
    const auto planned = planSampleTriplet(phaseZeroProtocol(), domain::FrameId{1});
    ASSERT_TRUE(planned.hasValue());

    // Hand-written known list: output position 1 is the first generated instant, bracketed by
    // GT0 and GT2, landing exactly on GT1 at ratio 1/2.
    const SampleTripletPlan expected{
        .input0GtFrame = GtFrameIndex{0},
        .input1GtFrame = GtFrameIndex{2},
        .targetGtFrame = GtFrameIndex{1},
        .targetRatioNumerator = 1,
        .targetRatioDenominator = 2,
        .candidateOutputFrame = domain::FrameId{1},
        .mappingOrigin = MappingOrigin::kImportedManifest,
        .cropRect = std::nullopt,
    };
    EXPECT_EQ(planned.value(), expected);
}

TEST(SampleExportPlannerTests, PlansBothGeneratedTargetsForStrideThreeDecimation) {
    // 60->20->60 decimation over ten GT frames: R0 G G R3 G G R6 G G R9.
    const auto built =
        domain::buildDecimatedProtocol(domain::DecimationPlan{.gtFrameCount = 10, .stride = 3},
                                       MappingOrigin::kManual,
                                       domain::TailPolicy::kTrimPartialInterval);
    ASSERT_TRUE(built.hasValue());

    const auto firstThird = planSampleTriplet(built.value(), domain::FrameId{1});
    ASSERT_TRUE(firstThird.hasValue());
    EXPECT_EQ(firstThird.value(),
              (SampleTripletPlan{.input0GtFrame = GtFrameIndex{0},
                                 .input1GtFrame = GtFrameIndex{3},
                                 .targetGtFrame = GtFrameIndex{1},
                                 .targetRatioNumerator = 1,
                                 .targetRatioDenominator = 3,
                                 .candidateOutputFrame = domain::FrameId{1},
                                 .mappingOrigin = MappingOrigin::kManual,
                                 .cropRect = std::nullopt}));

    const auto secondThird = planSampleTriplet(built.value(), domain::FrameId{2});
    ASSERT_TRUE(secondThird.hasValue());
    EXPECT_EQ(secondThird.value(),
              (SampleTripletPlan{.input0GtFrame = GtFrameIndex{0},
                                 .input1GtFrame = GtFrameIndex{3},
                                 .targetGtFrame = GtFrameIndex{2},
                                 .targetRatioNumerator = 2,
                                 .targetRatioDenominator = 3,
                                 .candidateOutputFrame = domain::FrameId{2},
                                 .mappingOrigin = MappingOrigin::kManual,
                                 .cropRect = std::nullopt}));

    // The first generated instant of the second interval re-anchors on GT3 and GT6.
    const auto nextInterval = planSampleTriplet(built.value(), domain::FrameId{4});
    ASSERT_TRUE(nextInterval.hasValue());
    EXPECT_EQ(nextInterval.value(),
              (SampleTripletPlan{.input0GtFrame = GtFrameIndex{3},
                                 .input1GtFrame = GtFrameIndex{6},
                                 .targetGtFrame = GtFrameIndex{4},
                                 .targetRatioNumerator = 1,
                                 .targetRatioDenominator = 3,
                                 .candidateOutputFrame = domain::FrameId{4},
                                 .mappingOrigin = MappingOrigin::kManual,
                                 .cropRect = std::nullopt}));
}

TEST(SampleExportPlannerTests, RejectsOriginsThatCannotFormATriplet) {
    // Retained output position: a passthrough copy of one GT frame has no input pair.
    const auto retainedPlan = planSampleTriplet(phaseZeroProtocol(), domain::FrameId{0});
    ASSERT_FALSE(retainedPlan.hasValue());
    EXPECT_EQ(retainedPlan.error().code, domain::MediaErrorCode::kInterpolationProtocolInvalid);
    EXPECT_NE(retainedPlan.error().technicalDetail.find("retained"), std::string::npos);

    // Unknown provenance: no committed mapping names the inputs or the target.
    const auto withUnknown = domain::InterpolationProtocol::create(
        5,
        MappingOrigin::kSuggestion,
        std::vector<OutputFrameOrigin>{RetainedFrame{GtFrameIndex{0}},
                                       UnknownFrame{},
                                       GeneratedFrame{GtFrameIndex{0}, GtFrameIndex{2}, 1, 2}});
    ASSERT_TRUE(withUnknown.hasValue());
    const auto unknownPlan = planSampleTriplet(withUnknown.value(), domain::FrameId{1});
    ASSERT_FALSE(unknownPlan.hasValue());
    EXPECT_EQ(unknownPlan.error().code, domain::MediaErrorCode::kInterpolationProtocolInvalid);
    EXPECT_NE(unknownPlan.error().technicalDetail.find("unknown"), std::string::npos);

    // Missing tail position from the builder path: expected, but absent from the asset.
    const auto withTail =
        domain::buildDecimatedProtocol(domain::DecimationPlan{.gtFrameCount = 8, .stride = 2},
                                       MappingOrigin::kManual,
                                       domain::TailPolicy::kMarkPartialIntervalMissing);
    ASSERT_TRUE(withTail.hasValue());
    ASSERT_EQ(withTail.value().frameCount(), 8);
    const auto missingPlan = planSampleTriplet(withTail.value(), domain::FrameId{7});
    ASSERT_FALSE(missingPlan.hasValue());
    EXPECT_EQ(missingPlan.error().code, domain::MediaErrorCode::kInterpolationProtocolInvalid);
    EXPECT_NE(missingPlan.error().technicalDetail.find("missing"), std::string::npos);
}

TEST(SampleExportPlannerTests, ForwardsTheDomainRejectionForOutputFramesOutsideTheProtocol) {
    // Phase-zero over five GT frames yields five output positions: R0 G R2 G R4.
    const auto built =
        domain::buildDecimatedProtocol(domain::DecimationPlan{.gtFrameCount = 5, .stride = 2},
                                       MappingOrigin::kManual,
                                       domain::TailPolicy::kTrimPartialInterval);
    ASSERT_TRUE(built.hasValue());

    const auto negative = planSampleTriplet(built.value(), domain::FrameId{-1});
    ASSERT_FALSE(negative.hasValue());
    EXPECT_EQ(negative.error().code, domain::MediaErrorCode::kInvalidFrameId);

    const auto pastEnd = planSampleTriplet(built.value(), domain::FrameId{5});
    ASSERT_FALSE(pastEnd.hasValue());
    EXPECT_EQ(pastEnd.error().code, domain::MediaErrorCode::kInvalidFrameId);

    // The tail-missing protocol cannot navigate to a position beyond its frame count either:
    // originAt rejects it before any triplet reasoning starts.
    const auto withTail =
        domain::buildDecimatedProtocol(domain::DecimationPlan{.gtFrameCount = 8, .stride = 2},
                                       MappingOrigin::kManual,
                                       domain::TailPolicy::kMarkPartialIntervalMissing);
    ASSERT_TRUE(withTail.hasValue());
    const auto unreachable = planSampleTriplet(withTail.value(), domain::FrameId{8});
    ASSERT_FALSE(unreachable.hasValue());
    EXPECT_EQ(unreachable.error().code, domain::MediaErrorCode::kInvalidFrameId);
}

TEST(SampleExportPlannerTests, RejectsGeneratedInstantsThatMissEveryGtFrame) {
    // (0 * 2 + (3 - 0) * 1) / 2 = 3/2: the generated instant falls between GT frames, which a
    // VFR or 59.94-style time base produces. Planning must refuse instead of rounding.
    const auto fractional = domain::InterpolationProtocol::create(
        4,
        MappingOrigin::kManual,
        std::vector<OutputFrameOrigin>{GeneratedFrame{GtFrameIndex{0}, GtFrameIndex{3}, 1, 2}});
    ASSERT_TRUE(fractional.hasValue());

    const auto planned = planSampleTriplet(fractional.value(), domain::FrameId{0});
    ASSERT_FALSE(planned.hasValue());
    EXPECT_EQ(planned.error().code, domain::MediaErrorCode::kInterpolationProtocolInvalid);
    EXPECT_NE(planned.error().technicalDetail.find("not an integer"), std::string::npos);
}

TEST(SampleExportPlannerTests, AppliesOneFixedCropIdenticallyToTheWholeTriplet) {
    const SampleCropRect crop{.x = 8, .y = 4, .width = 32, .height = 16};
    const auto planned = planSampleTriplet(
        phaseZeroProtocol(), domain::FrameId{1}, crop, SampleFrameSize{.width = 64, .height = 32});
    ASSERT_TRUE(planned.hasValue());

    // The rectangle is carried verbatim: one source-pixel rect for every member.
    ASSERT_TRUE(planned.value().cropRect.has_value());
    EXPECT_EQ(*planned.value().cropRect, crop);

    // Everything except the crop is identical to the uncropped plan.
    const auto uncropped = planSampleTriplet(phaseZeroProtocol(), domain::FrameId{1});
    ASSERT_TRUE(uncropped.hasValue());
    auto cropped = planned.value();
    cropped.cropRect.reset();
    EXPECT_EQ(cropped, uncropped.value());

    // A rectangle that exactly covers the known frame stays legal: the right and bottom edges
    // are half-open, so x + width == frame width sits on the boundary, not past it.
    const auto fullFrame =
        planSampleTriplet(phaseZeroProtocol(),
                          domain::FrameId{1},
                          SampleCropRect{.x = 0, .y = 0, .width = 64, .height = 32},
                          SampleFrameSize{.width = 64, .height = 32});
    ASSERT_TRUE(fullFrame.hasValue());
}

TEST(SampleExportPlannerTests, RejectsCropRectanglesThatCannotNameSourcePixels) {
    struct Example final {
        std::optional<SampleCropRect> crop;
        std::optional<SampleFrameSize> size;
        const char* label;
    };

    const std::int64_t nearMax = std::numeric_limits<std::int64_t>::max() - 4;
    const std::vector<Example> examples{
        Example{SampleCropRect{.x = 0, .y = 0, .width = 65, .height = 32},
                SampleFrameSize{.width = 64, .height = 32},
                "extends past the right frame edge"},
        Example{SampleCropRect{.x = 0, .y = 0, .width = 64, .height = 33},
                SampleFrameSize{.width = 64, .height = 32},
                "extends past the bottom frame edge"},
        Example{SampleCropRect{.x = -1, .y = 0, .width = 8, .height = 8},
                SampleFrameSize{.width = 64, .height = 32},
                "negative origin against a known size"},
        Example{SampleCropRect{.x = -1, .y = 0, .width = 8, .height = 8},
                std::nullopt,
                "negative origin with an unknown size"},
        Example{SampleCropRect{.x = 0, .y = -2, .width = 8, .height = 8},
                std::nullopt,
                "negative y origin with an unknown size"},
        Example{SampleCropRect{.x = 0, .y = 0, .width = -8, .height = 8},
                std::nullopt,
                "negative width"},
        Example{
            SampleCropRect{.x = 0, .y = 0, .width = 0, .height = 8}, std::nullopt, "zero width"},
        Example{SampleCropRect{.x = 0, .y = 0, .width = 8, .height = -8},
                std::nullopt,
                "negative height"},
        Example{SampleCropRect{.x = nearMax, .y = 0, .width = 8, .height = 8},
                std::nullopt,
                "x + width overflows int64"},
        Example{SampleCropRect{.x = 0, .y = nearMax, .width = 8, .height = 8},
                std::nullopt,
                "y + height overflows int64"},
        Example{SampleCropRect{.x = 0, .y = 0, .width = 8, .height = 8},
                SampleFrameSize{.width = 0, .height = 32},
                "known frame size is not positive"},
    };

    for (const auto& example : examples) {
        SCOPED_TRACE(example.label);
        const auto planned =
            planSampleTriplet(phaseZeroProtocol(), domain::FrameId{1}, example.crop, example.size);
        ASSERT_FALSE(planned.hasValue());
        EXPECT_EQ(planned.error().code, domain::MediaErrorCode::kInvalidArgument);
    }
}

} // namespace
} // namespace dvs::application
