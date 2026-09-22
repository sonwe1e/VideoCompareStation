#include "dvs/domain/ComparisonSelection.h"
#include "dvs/domain/PlaybackContinuityPolicy.h"

#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>

namespace dvs::domain {
namespace {

[[nodiscard]] RationalRate makeRate() {
    auto result = RationalRate::create(30, 1);
    EXPECT_TRUE(result.hasValue());
    return std::move(result).value();
}

[[nodiscard]] ComparisonSource
makeSource(const SourceId id,
           std::string path,
           const ComparisonRole role = ComparisonRole::kPrediction,
           std::optional<SourceFileIdentity> identity = std::nullopt) {
    const std::string displayName = path;
    return ComparisonSource{
        .id = id,
        .role = role,
        .descriptor =
            MediaDescriptor{
                .normalizedPath = std::move(path),
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
                .sourceIdentity = identity,
            },
        .displayName = displayName,
    };
}

[[nodiscard]] ComparisonSource makeSource(const SourceId id,
                                          const ComparisonRole role = ComparisonRole::kPrediction) {
    return makeSource(id, "s" + std::to_string(id) + ".mp4", role, std::nullopt);
}

[[nodiscard]] SourceFileIdentity
makeIdentity(const std::uint64_t byteSize, const std::int64_t modifiedMs, std::string fingerprint) {
    return SourceFileIdentity{
        .byteSize = byteSize,
        .modifiedUtcMilliseconds = modifiedMs,
        .fingerprintSha256 = std::move(fingerprint),
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

TEST(ComparisonSelectionTests, RemapKeepsPairByMediaPathAcrossSlotReuse) {
    // Previous session: A=0, B=1, C=2 with pair B/C. After removing A, B=0, C=1.
    const std::vector<ComparisonSource> previous{
        makeSource(0, "a.mp4"),
        makeSource(1, "b.mp4"),
        makeSource(2, "c.mp4"),
    };
    const std::vector<ComparisonSource> next{
        makeSource(0, "b.mp4"),
        makeSource(1, "c.mp4"),
    };
    const auto remapped = remapComparisonPairByMediaIdentity(previous, ComparisonPair{1, 2}, next);
    ASSERT_TRUE(remapped.has_value());
    EXPECT_EQ(remapped->first, 0U);
    EXPECT_EQ(remapped->second, 1U);
}

TEST(ComparisonSelectionTests, RemapDropsPairWhenReplacementIsDifferentMedia) {
    // Preferred pair A/B (slots 0,1). Replacing B with C leaves the same slot numbers but
    // different media — the pair must not be preserved as if it were still A/B.
    const std::vector<ComparisonSource> previous{
        makeSource(0, "a.mp4"),
        makeSource(1, "b.mp4"),
    };
    const std::vector<ComparisonSource> next{
        makeSource(0, "a.mp4"),
        makeSource(1, "c.mp4"),
    };
    EXPECT_FALSE(
        remapComparisonPairByMediaIdentity(previous, ComparisonPair{0, 1}, next).has_value());
}

TEST(ComparisonSelectionTests, RemapPrefersCompleteSourceFileIdentity) {
    const auto identityA = makeIdentity(100, 1'000, std::string(64, 'a'));
    const auto identityB = makeIdentity(200, 2'000, std::string(64, 'b'));
    const auto identityB2 = makeIdentity(200, 2'000, std::string(64, 'b'));
    const std::vector<ComparisonSource> previous{
        makeSource(0, "same.mp4", ComparisonRole::kPrediction, identityA),
        makeSource(1, "same.mp4", ComparisonRole::kPrediction, identityB),
    };
    // Same path on disk but distinct fingerprints: slots must not be treated as the same media.
    const auto identityOther = makeIdentity(200, 2'000, std::string(64, 'c'));
    const std::vector<ComparisonSource> nextSamePathDifferentContent{
        makeSource(0, "same.mp4", ComparisonRole::kPrediction, identityOther),
        makeSource(1, "other.mp4", ComparisonRole::kPrediction, identityA),
    };
    EXPECT_FALSE(remapComparisonPairByMediaIdentity(
                     previous, ComparisonPair{0, 1}, nextSamePathDifferentContent)
                     .has_value());

    const std::vector<ComparisonSource> nextIdentityMatch{
        makeSource(0, "other.mp4", ComparisonRole::kPrediction, identityB2),
        makeSource(1, "same.mp4", ComparisonRole::kPrediction, identityA),
    };
    const auto remapped =
        remapComparisonPairByMediaIdentity(previous, ComparisonPair{0, 1}, nextIdentityMatch);
    ASSERT_TRUE(remapped.has_value());
    EXPECT_EQ(remapped->first, 1U);
    EXPECT_EQ(remapped->second, 0U);
}

TEST(ComparisonSelectionTests, ContinuityPolicyResolvesContextualToSmoothnessFirst) {
    EXPECT_EQ(resolveContinuityPolicy(PlaybackContinuityPolicy::Contextual, 1U),
              PlaybackContinuityPolicy::RealTime);
    EXPECT_EQ(resolveContinuityPolicy(PlaybackContinuityPolicy::Contextual, 2U),
              PlaybackContinuityPolicy::RealTime);
    EXPECT_EQ(resolveContinuityPolicy(PlaybackContinuityPolicy::Contextual, 3U),
              PlaybackContinuityPolicy::RealTime);
    EXPECT_EQ(resolveContinuityPolicy(PlaybackContinuityPolicy::RealTime, 3U),
              PlaybackContinuityPolicy::RealTime);
    EXPECT_EQ(resolveContinuityPolicy(PlaybackContinuityPolicy::ReviewEveryFrame, 1U),
              PlaybackContinuityPolicy::ReviewEveryFrame);
    EXPECT_EQ(resolveContinuityPolicy(PlaybackContinuityPolicy::ReviewEveryFrame, 2U),
              PlaybackContinuityPolicy::ReviewEveryFrame);
}

} // namespace dvs::domain
