#include "dvs/domain/ComparisonValidator.h"

#include <gtest/gtest.h>
#include <utility>

namespace dvs::domain {
namespace {

[[nodiscard]] RationalRate makeRate(const std::int64_t numerator = 30,
                                    const std::int64_t denominator = 1) {
    auto result = RationalRate::create(numerator, denominator);
    EXPECT_TRUE(result.hasValue());
    return std::move(result).value();
}

[[nodiscard]] MediaDescriptor
makeDescriptor(const std::int64_t frameCount = 90,
               const std::optional<RationalRate>& rate = makeRate(),
               const MediaExtent extent = MediaExtent{.width = 1'920, .height = 1'080},
               const ColorMetadata color = ColorMetadata{},
               const std::optional<MediaTime> duration = std::nullopt) {
    return MediaDescriptor{
        .normalizedPath = "source.mp4",
        .extent = extent,
        .frameRate = rate,
        .frameCount = FrameCountInfo{.value = frameCount, .origin = FrameCountOrigin::kReported},
        .duration = duration.value_or(MediaTime{frameCount * 1'000'000 / 30}),
        .codecId = "h264",
        .pixelFormatId = "yuv420p",
        .bitDepth = 8,
        .colorMetadata = color,
        .decodeCapabilities = DecodeCapabilities{.softwareDecode = true, .d3d11VaDecode = false},
        .timingConfidence = TimingConfidence::kDeclaredCfr,
        .sourceIdentity = std::nullopt,
    };
}

[[nodiscard]] ComparisonSource makeSource(const SourceId id,
                                          const ComparisonRole role = ComparisonRole::kPrediction,
                                          MediaDescriptor descriptor = makeDescriptor()) {
    return ComparisonSource{
        .id = id,
        .role = role,
        .descriptor = std::move(descriptor),
        .displayName = "source-" + std::to_string(id),
    };
}

[[nodiscard]] bool hasFinding(const CompatibilityReport& report,
                              const MediaErrorCode code,
                              const CompatibilitySeverity severity) {
    for (const CompatibilityFinding& finding : report.findings()) {
        if (finding.code == code && finding.severity == severity) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(ComparisonValidatorTests, AcceptsTwoIdenticalSourcesWithFirstAsCanonical) {
    const auto result = ComparisonValidator::validate({makeSource(0), makeSource(1)});

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().set.sourceCount(), 2U);
    EXPECT_EQ(result.value().set.canonicalSourceId(), 0U);
    EXPECT_FALSE(result.value().set.referenceSourceId().has_value());
    EXPECT_TRUE(result.value().report.isEmpty());
    EXPECT_EQ(result.value().set.canonicalFrameCount(), 90);
    ASSERT_TRUE(result.value().set.canonicalRate().has_value());
    EXPECT_EQ(*result.value().set.canonicalRate(), makeRate());
}

TEST(ComparisonValidatorTests, AcceptsSingleReviewSourceAsCanonical) {
    const auto result = ComparisonValidator::validate({makeSource(0)});

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().set.sourceCount(), 1U);
    EXPECT_EQ(result.value().set.canonicalSourceId(), 0U);
    EXPECT_TRUE(result.value().report.isEmpty());
}

TEST(ComparisonValidatorTests, ReferenceRoleDoesNotStealTimelineMaster) {
    const auto result =
        ComparisonValidator::validate({makeSource(0), makeSource(1, ComparisonRole::kReference)});

    ASSERT_TRUE(result.hasValue());
    // C-01: timeline master stays session-order first; Reference is comparison baseline only.
    EXPECT_EQ(result.value().set.canonicalSourceId(), 0U);
    EXPECT_EQ(result.value().set.timelineMasterSourceId(), 0U);
    ASSERT_TRUE(result.value().set.referenceSourceId().has_value());
    EXPECT_EQ(*result.value().set.referenceSourceId(), 1U);
}

TEST(ComparisonValidatorTests, AcceptsThreeSourcesAndExposesAllById) {
    const auto result = ComparisonValidator::validate(
        {makeSource(0, ComparisonRole::kReference), makeSource(1), makeSource(2)});

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().set.sourceCount(), 3U);
    ASSERT_NE(result.value().set.find(2), nullptr);
    EXPECT_EQ(result.value().set.find(2)->displayName, "source-2");
    EXPECT_EQ(result.value().set.find(7), nullptr);
}

TEST(ComparisonValidatorTests, RejectsEmptyOrMoreThanThreeSources) {
    const auto empty = ComparisonValidator::validate({});
    ASSERT_FALSE(empty.hasValue());
    EXPECT_EQ(empty.error().code, MediaErrorCode::kInvalidArgument);

    const auto quadruple =
        ComparisonValidator::validate({makeSource(0), makeSource(1), makeSource(2), makeSource(3)});
    ASSERT_FALSE(quadruple.hasValue());
    EXPECT_EQ(quadruple.error().code, MediaErrorCode::kInvalidArgument);
}

TEST(ComparisonValidatorTests, RejectsDuplicateSourceIds) {
    const auto result = ComparisonValidator::validate({makeSource(0), makeSource(0)});

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, MediaErrorCode::kInvalidArgument);
}

TEST(ComparisonValidatorTests, RejectsInvalidDescriptorAndNamesTheSource) {
    auto invalid = makeSource(1);
    invalid.descriptor.codecId.clear();

    const auto result = ComparisonValidator::validate({makeSource(0), std::move(invalid)});

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, MediaErrorCode::kInvalidMediaDescriptor);
    EXPECT_NE(result.error().technicalDetail.find("Source 1"), std::string::npos);
}

TEST(ComparisonValidatorTests, RejectsTwoReferenceRoles) {
    const auto result = ComparisonValidator::validate({
        makeSource(0, ComparisonRole::kReference),
        makeSource(1, ComparisonRole::kReference),
    });

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, MediaErrorCode::kInvalidArgument);
}

TEST(ComparisonValidatorTests, RequiresAlignmentForFrameCountMismatchWithoutBlocking) {
    const auto result = ComparisonValidator::validate(
        {makeSource(0, ComparisonRole::kReference, makeDescriptor(90)),
         makeSource(1, ComparisonRole::kPrediction, makeDescriptor(89))});

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().set.canonicalFrameCount(), 90);
    EXPECT_TRUE(hasFinding(result.value().report,
                           MediaErrorCode::kSourceFrameCountMismatch,
                           CompatibilitySeverity::kAlignmentRequired));
    EXPECT_TRUE(result.value().report.hasAlignmentRequired());
    EXPECT_FALSE(result.value().report.hasFatal());
}

TEST(ComparisonValidatorTests, RequiresAlignmentForTimingButWarnsForVisualMismatches) {
    const auto rateMismatch =
        makeDescriptor(90, makeRate(60), MediaExtent{.width = 1'920, .height = 1'080});
    const auto result = ComparisonValidator::validate(
        {makeSource(0), makeSource(1, ComparisonRole::kPrediction, rateMismatch)});

    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(hasFinding(result.value().report,
                           MediaErrorCode::kSourceFrameRateMismatch,
                           CompatibilitySeverity::kAlignmentRequired));
    EXPECT_TRUE(result.value().report.hasAlignmentRequired());

    const auto durationMismatch = makeDescriptor(90,
                                                 makeRate(),
                                                 MediaExtent{.width = 1'920, .height = 1'080},
                                                 ColorMetadata{},
                                                 MediaTime{3'033'335});
    const auto durationResult = ComparisonValidator::validate(
        {makeSource(0), makeSource(1, ComparisonRole::kPrediction, durationMismatch)});
    ASSERT_TRUE(durationResult.hasValue());
    EXPECT_TRUE(hasFinding(durationResult.value().report,
                           MediaErrorCode::kSourceDurationMismatch,
                           CompatibilitySeverity::kAlignmentRequired));

    const auto resolutionMismatch =
        makeDescriptor(90, makeRate(), MediaExtent{.width = 1'280, .height = 720});
    const auto resolutionResult = ComparisonValidator::validate(
        {makeSource(0), makeSource(1, ComparisonRole::kPrediction, resolutionMismatch)});
    ASSERT_TRUE(resolutionResult.hasValue());
    EXPECT_TRUE(hasFinding(resolutionResult.value().report,
                           MediaErrorCode::kSourceResolutionMismatch,
                           CompatibilitySeverity::kWarning));

    const auto colorMismatch =
        makeDescriptor(90,
                       makeRate(),
                       MediaExtent{.width = 1'920, .height = 1'080},
                       ColorMetadata{.matrix = ColorMatrix::kBt709, .range = ColorRange::kFull});
    const auto colorResult = ComparisonValidator::validate(
        {makeSource(0), makeSource(1, ComparisonRole::kPrediction, colorMismatch)});
    ASSERT_TRUE(colorResult.hasValue());
    EXPECT_TRUE(hasFinding(colorResult.value().report,
                           MediaErrorCode::kSourceColorMetadataMismatch,
                           CompatibilitySeverity::kWarning));
}

TEST(ComparisonValidatorTests, ReportsEstimatedFrameCountFromAnySource) {
    auto estimated = makeSource(1);
    estimated.descriptor.frameCount.origin = FrameCountOrigin::kEstimated;

    const auto result = ComparisonValidator::validate({makeSource(0), std::move(estimated)});

    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().set.hasEstimatedFrameCount());
}

TEST(ComparisonValidatorTests, AcceptsVfrSourcesWithoutNominalRates) {
    auto vfrA = makeSource(0);
    vfrA.descriptor.frameRate = std::nullopt;
    vfrA.descriptor.timingConfidence = TimingConfidence::kVariableFrameRate;
    auto vfrB = makeSource(1);
    vfrB.descriptor.frameRate = std::nullopt;
    vfrB.descriptor.timingConfidence = TimingConfidence::kVariableFrameRate;

    const auto result = ComparisonValidator::validate({std::move(vfrA), std::move(vfrB)});

    ASSERT_TRUE(result.hasValue());
    EXPECT_FALSE(result.value().set.canonicalRate().has_value());
    EXPECT_TRUE(result.value().report.isEmpty());
}

} // namespace dvs::domain
