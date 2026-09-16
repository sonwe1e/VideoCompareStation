#include "SourceFrameCache.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace dvs::media::internal {
namespace {

class TestFrameResource final : public application::IFrameResource {};

[[nodiscard]] SourceFrameCacheKey makeKey(
    std::string fingerprint,
    const std::int64_t frame,
    const application::NormalizedFrameFormat format = application::NormalizedFrameFormat::Nv12_8,
    const std::uint32_t width = 1920U,
    const std::uint32_t height = 1080U) {
    return SourceFrameCacheKey{
        .sourceFingerprint = std::move(fingerprint),
        .sourceFrame = domain::FrameId{frame},
        .profile =
            NormalizationProfile{
                .format = format,
                .width = width,
                .height = height,
            },
    };
}

[[nodiscard]] CachedSourceFrame makeFrame(const std::size_t bytes,
                                          const std::int64_t presentationMicroseconds) {
    std::optional<application::FrameHandle> handle =
        application::FrameHandle::create(std::make_shared<const TestFrameResource>(),
                                         application::FrameGeometry{
                                             .width = 1U,
                                             .height = 1U,
                                         },
                                         bytes);
    return CachedSourceFrame{
        .handle = std::move(handle).value(),
        .presentationTime = domain::MediaTime{presentationMicroseconds},
    };
}

TEST(SourceFrameCacheTests, FindsEntriesAcrossEveryKeyDimension) {
    SourceFrameCache cache{5U};
    const SourceFrameCacheKey fingerprint = makeKey("fingerprint-b", 7);
    const SourceFrameCacheKey frame = makeKey("fingerprint-a", 8);
    const SourceFrameCacheKey format =
        makeKey("fingerprint-a", 7, application::NormalizedFrameFormat::P010_10);
    const SourceFrameCacheKey width =
        makeKey("fingerprint-a", 7, application::NormalizedFrameFormat::Nv12_8, 1280U);
    const SourceFrameCacheKey height =
        makeKey("fingerprint-a", 7, application::NormalizedFrameFormat::Nv12_8, 1920U, 720U);

    cache.insert(fingerprint, makeFrame(1U, 1));
    cache.insert(frame, makeFrame(1U, 2));
    cache.insert(format, makeFrame(1U, 3));
    cache.insert(width, makeFrame(1U, 4));
    cache.insert(height, makeFrame(1U, 5));

    const auto expectPresentation = [&cache](const SourceFrameCacheKey& key,
                                             const domain::MediaTime expected) {
        const std::optional<CachedSourceFrame> cached = cache.find(key);
        ASSERT_TRUE(cached.has_value());
        EXPECT_EQ(cached->presentationTime, expected);
    };
    expectPresentation(fingerprint, domain::MediaTime{1});
    expectPresentation(frame, domain::MediaTime{2});
    expectPresentation(format, domain::MediaTime{3});
    expectPresentation(width, domain::MediaTime{4});
    expectPresentation(height, domain::MediaTime{5});
}

TEST(SourceFrameCacheTests, HitPromotesEntryBeforeLeastRecentlyUsedEviction) {
    SourceFrameCache cache{3U};
    const SourceFrameCacheKey first = makeKey("source", 1);
    const SourceFrameCacheKey second = makeKey("source", 2);
    const SourceFrameCacheKey third = makeKey("source", 3);
    const SourceFrameCacheKey fourth = makeKey("source", 4);
    cache.insert(first, makeFrame(1U, 1));
    cache.insert(second, makeFrame(1U, 2));
    cache.insert(third, makeFrame(1U, 3));

    ASSERT_TRUE(cache.find(first).has_value());
    cache.insert(fourth, makeFrame(1U, 4));

    EXPECT_FALSE(cache.find(second).has_value());
    EXPECT_TRUE(cache.find(first).has_value());
    EXPECT_TRUE(cache.find(third).has_value());
    EXPECT_TRUE(cache.find(fourth).has_value());
    EXPECT_EQ(cache.retainedBytes(), 3U);
    EXPECT_EQ(cache.entryCount(), 3U);
}

TEST(SourceFrameCacheTests, ReplacingAKeyUpdatesValueAndByteAccounting) {
    SourceFrameCache cache{6U};
    const SourceFrameCacheKey first = makeKey("source", 1);
    const SourceFrameCacheKey second = makeKey("source", 2);
    cache.insert(first, makeFrame(2U, 10));
    cache.insert(second, makeFrame(2U, 20));

    cache.insert(first, makeFrame(4U, 30));

    const std::optional<CachedSourceFrame> replaced = cache.find(first);
    ASSERT_TRUE(replaced.has_value());
    EXPECT_EQ(replaced->presentationTime, domain::MediaTime{30});
    EXPECT_EQ(replaced->handle.accountedBytes(), 4U);
    EXPECT_TRUE(cache.find(second).has_value());
    EXPECT_EQ(cache.retainedBytes(), 6U);
    EXPECT_EQ(cache.entryCount(), 2U);
}

TEST(SourceFrameCacheTests, IgnoresOversizeAndZeroByteFramesAndClearResetsState) {
    SourceFrameCache cache{3U};
    const SourceFrameCacheKey retained = makeKey("source", 1);
    cache.insert(retained, makeFrame(2U, 10));
    cache.insert(retained, makeFrame(4U, 20));
    cache.insert(makeKey("source", 2), makeFrame(0U, 30));

    const std::optional<CachedSourceFrame> original = cache.find(retained);
    ASSERT_TRUE(original.has_value());
    EXPECT_EQ(original->presentationTime, domain::MediaTime{10});
    EXPECT_EQ(cache.retainedBytes(), 2U);
    EXPECT_EQ(cache.entryCount(), 1U);

    cache.clear();

    EXPECT_FALSE(cache.find(retained).has_value());
    EXPECT_EQ(cache.retainedBytes(), 0U);
    EXPECT_EQ(cache.entryCount(), 0U);
}

TEST(SourceFrameCacheTests, ZeroCapacityRetainsNoFrames) {
    SourceFrameCache cache{0U};

    cache.insert(makeKey("source", 1), makeFrame(1U, 10));

    EXPECT_EQ(cache.entryCount(), 0U);
    EXPECT_EQ(cache.retainedBytes(), 0U);
}

TEST(SourceFrameCacheTests, RetainsIndexedLookupsAcrossRehashAndListSplices) {
    constexpr std::size_t kEntryCount = 4096U;
    SourceFrameCache cache{kEntryCount};
    for (std::size_t index = 0U; index < kEntryCount; ++index) {
        cache.insert(
            makeKey("fingerprint-" + std::to_string(index), static_cast<std::int64_t>(index)),
            makeFrame(1U, static_cast<std::int64_t>(index)));
    }

    EXPECT_EQ(cache.entryCount(), kEntryCount);
    EXPECT_EQ(cache.retainedBytes(), kEntryCount);
    for (std::size_t index = 0U; index < kEntryCount; index += 37U) {
        const std::optional<CachedSourceFrame> cached = cache.find(
            makeKey("fingerprint-" + std::to_string(index), static_cast<std::int64_t>(index)));
        ASSERT_TRUE(cached.has_value());
        EXPECT_EQ(cached->presentationTime, domain::MediaTime{static_cast<std::int64_t>(index)});
    }
}

TEST(SourceFrameCacheTests, EvictionAccountingDoesNotOverflowAtMaximumCapacity) {
    constexpr std::size_t kMaximum = std::numeric_limits<std::size_t>::max();
    SourceFrameCache cache{kMaximum};
    const SourceFrameCacheKey first = makeKey("source", 1);
    const SourceFrameCacheKey second = makeKey("source", 2);
    cache.insert(first, makeFrame(kMaximum - 1U, 1));

    cache.insert(second, makeFrame(2U, 2));

    EXPECT_FALSE(cache.find(first).has_value());
    EXPECT_TRUE(cache.find(second).has_value());
    EXPECT_EQ(cache.retainedBytes(), 2U);
}

} // namespace
} // namespace dvs::media::internal
