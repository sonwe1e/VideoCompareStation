#include "dvs/domain/ComparisonSelection.h"
#include "dvs/domain/PlaybackContinuityPolicy.h"

#include <gtest/gtest.h>
#include <optional>
#include <vector>

namespace dvs::domain {
namespace {

[[nodiscard]] RationalRate makeRate() {
    auto result = RationalRate::create(30, 1);
    EXPECT_TRUE(result.hasValue());
    return std::move(result).value();
}

[[nodiscard]] ComparisonSource makeSource(const SourceId id,
                                          const ComparisonRole role = ComparisonRole::kPrediction) {
    return ComparisonSource{
        .id = id,
        .role = role,
        .descriptor =
            MediaDescriptor{
                .normalizedPath = "s" + std::to_string(id) + ".mp4",
                .extent = MediaExtent{.width = 320, .height = 180},
                .frameRate = makeRate(),
                .frameCount = FrameCountInfo{.value = 12, .origin = FrameCountOrigin::kReported},
                .duration = MediaTime{400'000},
                .codecId = "h264",
                .pixelFormatId = "yuv420p",
                .bitDepth = 8,
                .colorMetadata = ColorMetadata{},
                .decodeCapabilities =
                    DecodeCapabilities{.softwareDecode = true, .d3d11VaDecode = false},
                .timingConfidence = TimingConfidence::kDeclaredCfr,
                .sourceIdentity = std::nullopt,
            },
        .displayName = "s" + std::to_string(id),
    };
}

} // namespace

TEST(ComparisonSelectionTests, PreserveKeepsLivePreferredPair) {
    const std::vector<ComparisonSource> sources{
        makeSource(0),
        makeSource(1),
        makeSource(2),
    };
    const ComparisonPair preferred{0, 2};
    const ComparisonPair resolved = resolveComparisonPair(
        sources, SourceId{0}, preferred, DefaultPairPolicy::PreserveIfAvailable);
    EXPECT_EQ(resolved.first, 0U);
    EXPECT_EQ(resolved.second, 2U);
}

TEST(ComparisonSelectionTests, TopologyShrinkDropsStalePairAndResolvesLastTwo) {
    const std::vector<ComparisonSource> shrunk{
        makeSource(0),
        makeSource(1),
    };
    // Preferred A/C (0,2) is gone after removing source 2.
    const ComparisonPair preferred{0, 2};
    const ComparisonPair resolved = resolveComparisonPair(
        shrunk, SourceId{0}, preferred, DefaultPairPolicy::PreserveIfAvailable);
    EXPECT_EQ(resolved.first, 0U);
    EXPECT_EQ(resolved.second, 1U);
}

TEST(ComparisonSelectionTests, ReferencePolicyPicksReferenceAndFirstCandidate) {
    const std::vector<ComparisonSource> sources{
        makeSource(0),
        makeSource(1, ComparisonRole::kReference),
        makeSource(2),
    };
    const ComparisonPair resolved = resolveComparisonPair(
        sources, SourceId{1}, std::nullopt, DefaultPairPolicy::ReferenceAndFirstCandidate);
    EXPECT_EQ(resolved.first, 1U);
    EXPECT_EQ(resolved.second, 0U);
}

TEST(ComparisonSelectionTests, EdgeOrdinalProjectsSessionOrderPair) {
    const std::vector<ComparisonSource> sources{
        makeSource(0),
        makeSource(1),
        makeSource(2),
    };
    const auto edge01 = comparisonPairEdgeOrdinal(sources, ComparisonPair{0, 1});
    ASSERT_TRUE(edge01.has_value());
    EXPECT_EQ(*edge01, 0U);
    const auto edge02 = comparisonPairEdgeOrdinal(sources, ComparisonPair{0, 2});
    ASSERT_TRUE(edge02.has_value());
    EXPECT_EQ(*edge02, 1U);
    const auto edge12 = comparisonPairEdgeOrdinal(sources, ComparisonPair{1, 2});
    ASSERT_TRUE(edge12.has_value());
    EXPECT_EQ(*edge12, 2U);
    EXPECT_FALSE(comparisonPairEdgeOrdinal(sources, ComparisonPair{0, 9}).has_value());
}

TEST(ComparisonSelectionTests, ContinuityPolicyResolvesContextualBySourceCount) {
    EXPECT_EQ(resolveContinuityPolicy(PlaybackContinuityPolicy::Contextual, 1U),
              PlaybackContinuityPolicy::RealTime);
    EXPECT_EQ(resolveContinuityPolicy(PlaybackContinuityPolicy::Contextual, 2U),
              PlaybackContinuityPolicy::ReviewEveryFrame);
    EXPECT_EQ(resolveContinuityPolicy(PlaybackContinuityPolicy::RealTime, 3U),
              PlaybackContinuityPolicy::RealTime);
}

} // namespace dvs::domain
