#include "dvs/domain/InterpolationProtocol.h"

#include <gtest/gtest.h>
#include <vector>

namespace dvs::domain {
namespace {

TEST(InterpolationProtocolTests, BuildsPhaseZeroDecimationMatchingTheKnownFrameList) {
    const auto built = buildDecimatedProtocol(DecimationPlan{.gtFrameCount = 7, .stride = 2},
                                              MappingOrigin::kImportedManifest,
                                              TailPolicy::kTrimPartialInterval);
    ASSERT_TRUE(built);
    EXPECT_EQ(built.value().frameCount(), 7);
    EXPECT_EQ(built.value().gtFrameCount(), 7);
    EXPECT_EQ(built.value().mappingOrigin(), MappingOrigin::kImportedManifest);

    auto expected =
        InterpolationProtocol::create(7,
                                      MappingOrigin::kImportedManifest,
                                      std::vector<OutputFrameOrigin>{
                                          RetainedFrame{GtFrameIndex{0}},
                                          GeneratedFrame{GtFrameIndex{0}, GtFrameIndex{2}, 1, 2},
                                          RetainedFrame{GtFrameIndex{2}},
                                          GeneratedFrame{GtFrameIndex{2}, GtFrameIndex{4}, 1, 2},
                                          RetainedFrame{GtFrameIndex{4}},
                                          GeneratedFrame{GtFrameIndex{4}, GtFrameIndex{6}, 1, 2},
                                          RetainedFrame{GtFrameIndex{6}},
                                      });
    ASSERT_TRUE(expected);
    EXPECT_EQ(built.value(), expected.value());

    const auto firstGenerated = built.value().originAt(FrameId{1});
    ASSERT_TRUE(firstGenerated);
    const auto* generated = std::get_if<GeneratedFrame>(&firstGenerated.value());
    ASSERT_NE(generated, nullptr);
    EXPECT_EQ(generated->input0GtFrame, GtFrameIndex{0});
    EXPECT_EQ(generated->input1GtFrame, GtFrameIndex{2});
    EXPECT_EQ(generated->targetRatioNumerator, 1);
    EXPECT_EQ(generated->targetRatioDenominator, 2);
}

TEST(InterpolationProtocolTests, BuildsPhaseOneDecimationLeavingHeadFramesUnrepresented) {
    const auto built =
        buildDecimatedProtocol(DecimationPlan{.gtFrameCount = 8, .stride = 2, .phase = 1},
                               MappingOrigin::kManual,
                               TailPolicy::kTrimPartialInterval);
    ASSERT_TRUE(built);
    EXPECT_EQ(built.value().frameCount(), 7);

    const auto first = built.value().originAt(FrameId{0});
    ASSERT_TRUE(first);
    const auto* retained = std::get_if<RetainedFrame>(&first.value());
    ASSERT_NE(retained, nullptr);
    EXPECT_EQ(retained->gtFrame, GtFrameIndex{1});

    // GT frame 0 precedes the first retained frame and has no output position.
    const auto last = built.value().originAt(FrameId{6});
    ASSERT_TRUE(last);
    const auto* lastRetained = std::get_if<RetainedFrame>(&last.value());
    ASSERT_NE(lastRetained, nullptr);
    EXPECT_EQ(lastRetained->gtFrame, GtFrameIndex{7});
}

TEST(InterpolationProtocolTests, BuildsMultipleGeneratedTargetsPerIntervalForStrideThree) {
    const auto built = buildDecimatedProtocol(DecimationPlan{.gtFrameCount = 10, .stride = 3},
                                              MappingOrigin::kImportedManifest,
                                              TailPolicy::kTrimPartialInterval);
    ASSERT_TRUE(built);
    EXPECT_EQ(built.value().frameCount(), 10);

    auto expected =
        InterpolationProtocol::create(10,
                                      MappingOrigin::kImportedManifest,
                                      std::vector<OutputFrameOrigin>{
                                          RetainedFrame{GtFrameIndex{0}},
                                          GeneratedFrame{GtFrameIndex{0}, GtFrameIndex{3}, 1, 3},
                                          GeneratedFrame{GtFrameIndex{0}, GtFrameIndex{3}, 2, 3},
                                          RetainedFrame{GtFrameIndex{3}},
                                          GeneratedFrame{GtFrameIndex{3}, GtFrameIndex{6}, 1, 3},
                                          GeneratedFrame{GtFrameIndex{3}, GtFrameIndex{6}, 2, 3},
                                          RetainedFrame{GtFrameIndex{6}},
                                          GeneratedFrame{GtFrameIndex{6}, GtFrameIndex{9}, 1, 3},
                                          GeneratedFrame{GtFrameIndex{6}, GtFrameIndex{9}, 2, 3},
                                          RetainedFrame{GtFrameIndex{9}},
                                      });
    ASSERT_TRUE(expected);
    EXPECT_EQ(built.value(), expected.value());
}

TEST(InterpolationProtocolTests, TrimsThePartialTailIntervalWhenTheSequenceEndsEarly) {
    const auto built = buildDecimatedProtocol(DecimationPlan{.gtFrameCount = 8, .stride = 2},
                                              MappingOrigin::kManual,
                                              TailPolicy::kTrimPartialInterval);
    ASSERT_TRUE(built);
    EXPECT_EQ(built.value().frameCount(), 7);

    const auto last = built.value().originAt(FrameId{6});
    ASSERT_TRUE(last);
    const auto* retained = std::get_if<RetainedFrame>(&last.value());
    ASSERT_NE(retained, nullptr);
    EXPECT_EQ(retained->gtFrame, GtFrameIndex{6});
}

TEST(InterpolationProtocolTests, MarksThePartialTailIntervalAsMissing) {
    const auto built = buildDecimatedProtocol(DecimationPlan{.gtFrameCount = 8, .stride = 2},
                                              MappingOrigin::kManual,
                                              TailPolicy::kMarkPartialIntervalMissing);
    ASSERT_TRUE(built);
    EXPECT_EQ(built.value().frameCount(), 8);

    const auto tail = built.value().originAt(FrameId{7});
    ASSERT_TRUE(tail);
    const auto* missing = std::get_if<MissingFrame>(&tail.value());
    ASSERT_NE(missing, nullptr);
    ASSERT_TRUE(missing->expectedGtFrame.has_value());
    EXPECT_EQ(*missing->expectedGtFrame, GtFrameIndex{7});
}

TEST(InterpolationProtocolTests, RejectsMalformedDecimationPlans) {
    const auto emptyCount = buildDecimatedProtocol(DecimationPlan{.gtFrameCount = 0},
                                                   MappingOrigin::kManual,
                                                   TailPolicy::kTrimPartialInterval);
    ASSERT_FALSE(emptyCount);
    EXPECT_EQ(emptyCount.error().code, MediaErrorCode::kInvalidArgument);

    const auto unitStride = buildDecimatedProtocol(DecimationPlan{.gtFrameCount = 7, .stride = 1},
                                                   MappingOrigin::kManual,
                                                   TailPolicy::kTrimPartialInterval);
    ASSERT_FALSE(unitStride);
    EXPECT_EQ(unitStride.error().code, MediaErrorCode::kInvalidArgument);

    const auto phaseAtStride =
        buildDecimatedProtocol(DecimationPlan{.gtFrameCount = 7, .stride = 2, .phase = 2},
                               MappingOrigin::kManual,
                               TailPolicy::kTrimPartialInterval);
    ASSERT_FALSE(phaseAtStride);
    EXPECT_EQ(phaseAtStride.error().code, MediaErrorCode::kInvalidArgument);

    const auto endsBeforeFirstRetained =
        buildDecimatedProtocol(DecimationPlan{.gtFrameCount = 3, .stride = 5, .phase = 3},
                               MappingOrigin::kManual,
                               TailPolicy::kTrimPartialInterval);
    ASSERT_FALSE(endsBeforeFirstRetained);
    EXPECT_EQ(endsBeforeFirstRetained.error().code, MediaErrorCode::kInvalidArgument);
}

TEST(InterpolationProtocolTests, RejectsInconsistentOriginLists) {
    const auto empty = InterpolationProtocol::create(7, MappingOrigin::kManual, {});
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, MediaErrorCode::kInterpolationProtocolInvalid);

    const auto zeroRatio = InterpolationProtocol::create(
        7,
        MappingOrigin::kManual,
        std::vector<OutputFrameOrigin>{GeneratedFrame{GtFrameIndex{0}, GtFrameIndex{2}, 0, 2}});
    ASSERT_FALSE(zeroRatio);

    const auto endpointRatio = InterpolationProtocol::create(
        7,
        MappingOrigin::kManual,
        std::vector<OutputFrameOrigin>{GeneratedFrame{GtFrameIndex{0}, GtFrameIndex{2}, 2, 2}});
    ASSERT_FALSE(endpointRatio);

    const auto zeroDenominator = InterpolationProtocol::create(
        7,
        MappingOrigin::kManual,
        std::vector<OutputFrameOrigin>{GeneratedFrame{GtFrameIndex{0}, GtFrameIndex{2}, 1, 0}});
    ASSERT_FALSE(zeroDenominator);

    const auto equalInputs = InterpolationProtocol::create(
        7,
        MappingOrigin::kManual,
        std::vector<OutputFrameOrigin>{GeneratedFrame{GtFrameIndex{2}, GtFrameIndex{2}, 1, 2}});
    ASSERT_FALSE(equalInputs);

    const auto reversedInputs = InterpolationProtocol::create(
        7,
        MappingOrigin::kManual,
        std::vector<OutputFrameOrigin>{GeneratedFrame{GtFrameIndex{4}, GtFrameIndex{2}, 1, 2}});
    ASSERT_FALSE(reversedInputs);

    const auto retainedOutOfRange = InterpolationProtocol::create(
        5, MappingOrigin::kManual, std::vector<OutputFrameOrigin>{RetainedFrame{GtFrameIndex{5}}});
    ASSERT_FALSE(retainedOutOfRange);

    const auto generatedOutOfRange = InterpolationProtocol::create(
        5,
        MappingOrigin::kManual,
        std::vector<OutputFrameOrigin>{GeneratedFrame{GtFrameIndex{0}, GtFrameIndex{6}, 1, 2}});
    ASSERT_FALSE(generatedOutOfRange);

    // An unknown GT count disables range checks; the arithmetic bound still applies.
    const auto unknownCountWithinBound = InterpolationProtocol::create(
        0,
        MappingOrigin::kManual,
        std::vector<OutputFrameOrigin>{RetainedFrame{GtFrameIndex{999'999'999}}});
    EXPECT_TRUE(unknownCountWithinBound);

    const auto unknownCountPastBound = InterpolationProtocol::create(
        0,
        MappingOrigin::kManual,
        std::vector<OutputFrameOrigin>{RetainedFrame{GtFrameIndex{1'000'000'000}}});
    ASSERT_FALSE(unknownCountPastBound);

    const auto duplicateImpliedPosition = InterpolationProtocol::create(
        7,
        MappingOrigin::kManual,
        std::vector<OutputFrameOrigin>{RetainedFrame{GtFrameIndex{2}},
                                       GeneratedFrame{GtFrameIndex{0}, GtFrameIndex{4}, 1, 2}});
    ASSERT_FALSE(duplicateImpliedPosition);

    const auto decreasingImpliedPosition = InterpolationProtocol::create(
        7,
        MappingOrigin::kManual,
        std::vector<OutputFrameOrigin>{RetainedFrame{GtFrameIndex{2}},
                                       GeneratedFrame{GtFrameIndex{0}, GtFrameIndex{4}, 1, 4}});
    ASSERT_FALSE(decreasingImpliedPosition);

    const auto increasingImpliedPosition = InterpolationProtocol::create(
        7,
        MappingOrigin::kManual,
        std::vector<OutputFrameOrigin>{RetainedFrame{GtFrameIndex{2}},
                                       GeneratedFrame{GtFrameIndex{0}, GtFrameIndex{4}, 3, 4}});
    EXPECT_TRUE(increasingImpliedPosition);

    // Unknown and missing positions carry no GT time and never break the ordering check.
    const auto unknownBetweenRetained = InterpolationProtocol::create(
        7,
        MappingOrigin::kManual,
        std::vector<OutputFrameOrigin>{
            RetainedFrame{GtFrameIndex{0}}, UnknownFrame{}, RetainedFrame{GtFrameIndex{1}}});
    EXPECT_TRUE(unknownBetweenRetained);

    const auto presentFramesStillOrderedAroundMissing = InterpolationProtocol::create(
        7,
        MappingOrigin::kManual,
        std::vector<OutputFrameOrigin>{
            RetainedFrame{GtFrameIndex{1}}, MissingFrame{}, RetainedFrame{GtFrameIndex{0}}});
    ASSERT_FALSE(presentFramesStillOrderedAroundMissing);
}

TEST(InterpolationProtocolTests, ReducesTargetTimeRatiosToLowestTerms) {
    auto unreduced = InterpolationProtocol::create(
        9,
        MappingOrigin::kManual,
        std::vector<OutputFrameOrigin>{GeneratedFrame{GtFrameIndex{0}, GtFrameIndex{4}, 2, 4}});
    ASSERT_TRUE(unreduced);
    const auto origin = unreduced.value().originAt(FrameId{0});
    ASSERT_TRUE(origin);
    const auto* generated = std::get_if<GeneratedFrame>(&origin.value());
    ASSERT_NE(generated, nullptr);
    EXPECT_EQ(generated->targetRatioNumerator, 1);
    EXPECT_EQ(generated->targetRatioDenominator, 2);

    auto reduced = InterpolationProtocol::create(
        9,
        MappingOrigin::kManual,
        std::vector<OutputFrameOrigin>{GeneratedFrame{GtFrameIndex{0}, GtFrameIndex{4}, 1, 2}});
    ASSERT_TRUE(reduced);
    EXPECT_EQ(unreduced.value(), reduced.value());
}

TEST(InterpolationProtocolTests, NavigatesToGeneratedFramesAndSkipsMissingOrUnknownPositions) {
    const auto built = buildDecimatedProtocol(DecimationPlan{.gtFrameCount = 10, .stride = 3},
                                              MappingOrigin::kManual,
                                              TailPolicy::kTrimPartialInterval);
    ASSERT_TRUE(built);
    const auto& protocol = built.value();
    // Layout: R0 G G R3 G G R6 G G R9.
    EXPECT_EQ(protocol.nextGeneratedFrame(FrameId{-1}), FrameId{1});
    EXPECT_EQ(protocol.nextGeneratedFrame(FrameId{0}), FrameId{1});
    EXPECT_EQ(protocol.nextGeneratedFrame(FrameId{2}), FrameId{4});
    EXPECT_EQ(protocol.nextGeneratedFrame(FrameId{8}), std::nullopt);
    EXPECT_EQ(protocol.nextGeneratedFrame(FrameId{9}), std::nullopt);
    EXPECT_EQ(protocol.previousGeneratedFrame(FrameId{10}), FrameId{8});
    EXPECT_EQ(protocol.previousGeneratedFrame(FrameId{9}), FrameId{8});
    EXPECT_EQ(protocol.previousGeneratedFrame(FrameId{3}), FrameId{2});
    EXPECT_EQ(protocol.previousGeneratedFrame(FrameId{1}), std::nullopt);
    EXPECT_EQ(protocol.previousGeneratedFrame(FrameId{-1}), std::nullopt);

    // A model that dropped one generated frame leaves a missing position with no frame to show.
    auto withGap = InterpolationProtocol::create(
        5,
        MappingOrigin::kImportedManifest,
        std::vector<OutputFrameOrigin>{RetainedFrame{GtFrameIndex{0}},
                                       MissingFrame{GtFrameIndex{1}},
                                       RetainedFrame{GtFrameIndex{2}},
                                       GeneratedFrame{GtFrameIndex{2}, GtFrameIndex{4}, 1, 2},
                                       RetainedFrame{GtFrameIndex{4}}});
    ASSERT_TRUE(withGap);
    EXPECT_EQ(withGap.value().nextGeneratedFrame(FrameId{0}), FrameId{3});
    EXPECT_EQ(withGap.value().previousGeneratedFrame(FrameId{4}), FrameId{3});
    EXPECT_EQ(withGap.value().previousGeneratedFrame(FrameId{3}), std::nullopt);
}

TEST(InterpolationProtocolTests, OriginAtRejectsOutOfRangeOutputFrames) {
    const auto built = buildDecimatedProtocol(DecimationPlan{.gtFrameCount = 5, .stride = 2},
                                              MappingOrigin::kManual,
                                              TailPolicy::kTrimPartialInterval);
    ASSERT_TRUE(built);

    const auto negative = built.value().originAt(FrameId{-1});
    ASSERT_FALSE(negative);
    EXPECT_EQ(negative.error().code, MediaErrorCode::kInvalidFrameId);

    const auto pastEnd = built.value().originAt(FrameId{5});
    ASSERT_FALSE(pastEnd);
    EXPECT_EQ(pastEnd.error().code, MediaErrorCode::kInvalidFrameId);
}

TEST(InterpolationProtocolTests, PreservesUnknownProvenanceWithoutCoercion) {
    auto withUnknown = InterpolationProtocol::create(
        5,
        MappingOrigin::kSuggestion,
        std::vector<OutputFrameOrigin>{RetainedFrame{GtFrameIndex{0}},
                                       UnknownFrame{},
                                       GeneratedFrame{GtFrameIndex{0}, GtFrameIndex{2}, 1, 2}});
    ASSERT_TRUE(withUnknown);
    EXPECT_EQ(withUnknown.value().mappingOrigin(), MappingOrigin::kSuggestion);

    const auto middle = withUnknown.value().originAt(FrameId{1});
    ASSERT_TRUE(middle);
    EXPECT_TRUE(std::holds_alternative<UnknownFrame>(middle.value()));

    EXPECT_EQ(withUnknown.value().nextGeneratedFrame(FrameId{0}), FrameId{2});
}

} // namespace
} // namespace dvs::domain
