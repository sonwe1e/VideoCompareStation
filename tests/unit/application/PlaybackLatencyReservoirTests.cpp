#include "dvs/application/PlaybackLatencyReservoir.h"

#include <cstdint>
#include <gtest/gtest.h>

namespace dvs::application {
namespace {

// The truncation contract every latency percentile in the product shares (coordinator
// plan-to-publish, trace evidence hops, source-decode): count and maximum stay exact for every
// sample, and once the reservoir is full the *largest* samples are retained so a truncated
// percentile biases upward (conservative for stall diagnosis) instead of hiding a tail.
TEST(PlaybackLatencyReservoirTests, TruncationKeepsTheLargestSamplesWithExactCountAndMaximum) {
    PlaybackLatencyReservoir reservoir{5U};
    PlaybackLatencyStats stats;

    for (const std::uint64_t sample : {10U, 50U, 20U, 40U, 30U, 60U, 5U}) {
        reservoir.add(sample, stats);
        reservoir.finalizeStats(stats);
    }

    EXPECT_EQ(stats.count, 7U);
    EXPECT_EQ(stats.maxMicroseconds, 60U);
    EXPECT_EQ(stats.truncatedSampleCount, 2U);
    EXPECT_EQ(reservoir.size(), 5U);
    // Retained set is the five largest samples {20, 30, 40, 50, 60}: the 10 and the 5 were
    // evicted/dropped, not the newest arrivals.
    EXPECT_EQ(stats.p50Microseconds, 40U);
    EXPECT_EQ(stats.p95Microseconds, 60U);
    EXPECT_EQ(stats.p99Microseconds, 60U);
}

TEST(PlaybackLatencyReservoirTests, ZeroCapacityCountsWithoutRetainingSamples) {
    // Guarded behaviour instead of an empty-vector front(): count and maximum remain exact,
    // every sample counts as truncated, and the percentiles stay at the "nothing retained"
    // zero rather than reading undefined memory.
    PlaybackLatencyReservoir reservoir{0U};
    PlaybackLatencyStats stats;

    reservoir.add(7U, stats);
    reservoir.add(9U, stats);
    reservoir.finalizeStats(stats);

    EXPECT_EQ(stats.count, 2U);
    EXPECT_EQ(stats.maxMicroseconds, 9U);
    EXPECT_EQ(stats.truncatedSampleCount, 2U);
    EXPECT_EQ(reservoir.size(), 0U);
    EXPECT_EQ(stats.p50Microseconds, 0U);
    EXPECT_EQ(stats.p95Microseconds, 0U);
    EXPECT_EQ(stats.p99Microseconds, 0U);
}

} // namespace
} // namespace dvs::application
