#include "dvs/domain/RationalRate.h"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>

namespace dvs::domain {

TEST(RationalRateTests, NormalizesAndConvertsCanonicalTimelineValues) {
    const auto rateResult = RationalRate::create(60'000, 2'002);
    ASSERT_TRUE(rateResult.hasValue());
    const RationalRate rate = rateResult.value();

    EXPECT_EQ(rate.numerator(), 30'000);
    EXPECT_EQ(rate.denominator(), 1'001);
    EXPECT_NEAR(rate.displayFps(), 29.97002997, 0.00000001);

    const auto frameZero = rate.frameStartTime(FrameId{0});
    ASSERT_TRUE(frameZero.hasValue());
    EXPECT_EQ(frameZero.value().microseconds(), 0);

    const auto frameOne = rate.frameStartTime(FrameId{1});
    ASSERT_TRUE(frameOne.hasValue());
    EXPECT_EQ(frameOne.value().microseconds(), 33'367);
    const auto roundTrip = rate.frameAtOrBefore(frameOne.value());
    ASSERT_TRUE(roundTrip.hasValue());
    EXPECT_EQ(roundTrip.value(), FrameId{1});

    const auto interval = rate.frameIntervalCeiling();
    ASSERT_TRUE(interval.hasValue());
    EXPECT_EQ(interval.value().microseconds(), 33'367);
}

TEST(RationalRateTests, UsesFloorForFrameBoundaryConversions) {
    const auto rateResult = RationalRate::create(25, 1);
    ASSERT_TRUE(rateResult.hasValue());
    const RationalRate rate = rateResult.value();

    const auto start = rate.frameStartTime(FrameId{1});
    ASSERT_TRUE(start.hasValue());
    EXPECT_EQ(start.value().microseconds(), 40'000);

    const auto atBoundary = rate.frameAtOrBefore(MediaTime{40'000});
    ASSERT_TRUE(atBoundary.hasValue());
    EXPECT_EQ(atBoundary.value(), FrameId{1});

    const auto beforeBoundary = rate.frameAtOrBefore(MediaTime{39'999});
    ASSERT_TRUE(beforeBoundary.hasValue());
    EXPECT_EQ(beforeBoundary.value(), FrameId{0});

    const auto negativeTime = rate.frameAtOrBefore(MediaTime{-1});
    ASSERT_FALSE(negativeTime.hasValue());
    EXPECT_EQ(negativeTime.error().code, MediaErrorCode::kInvalidArgument);

    const auto invalidFrame = rate.frameStartTime(FrameId{-1});
    ASSERT_FALSE(invalidFrame.hasValue());
    EXPECT_EQ(invalidFrame.error().code, MediaErrorCode::kInvalidFrameId);
}

TEST(RationalRateTests, RoundsPresentationTimesWithoutChangingCanonicalCeilings) {
    const auto fractional = RationalRate::create(30000, 1001).value();
    const std::array expected{std::int64_t{0}, std::int64_t{33367}, std::int64_t{66733}};
    for (std::int64_t frame = 0; frame < 3; ++frame) {
        const auto rounded = fractional.frameStartTimeRounded(FrameId{frame});
        ASSERT_TRUE(rounded.hasValue());
        EXPECT_EQ(rounded.value().microseconds(), expected[static_cast<std::size_t>(frame)]);
    }
    EXPECT_EQ(fractional.frameStartTime(FrameId{2}).value().microseconds(), 66734);
    EXPECT_EQ(fractional.frameAtOrBefore(MediaTime{66733}).value(), FrameId{1});
    EXPECT_EQ(RationalRate::create(128, 1).value().frameStartTimeRounded(FrameId{1}).value(),
              MediaTime{7813});
    EXPECT_EQ(RationalRate::create(25, 1).value().frameStartTimeRounded(FrameId{1}).value(),
              MediaTime{40000});
    EXPECT_EQ(fractional.frameStartTimeRounded(FrameId{2'000'000'000}).value(),
              MediaTime{66'733'333'333'333});
}

TEST(RationalRateTests, RejectsInvalidAndOverflowingRoundedPresentationTimes) {
    const auto rate = RationalRate::create(1, 1).value();
    for (const auto frame : {std::int64_t{-1}, std::numeric_limits<std::int64_t>::max()}) {
        const auto invalid = rate.frameStartTimeRounded(FrameId{frame});
        ASSERT_FALSE(invalid.hasValue());
        EXPECT_EQ(invalid.error().code, MediaErrorCode::kInvalidFrameId);
    }
    const auto overflow =
        rate.frameStartTimeRounded(FrameId{std::numeric_limits<std::int64_t>::max() - 1});
    ASSERT_FALSE(overflow.hasValue());
    EXPECT_EQ(overflow.error().code, MediaErrorCode::kArithmeticOverflow);

    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto exactMaximum = RationalRate::create(1'000'000, maximum).value();
    const auto exact = exactMaximum.frameStartTimeRounded(FrameId{1});
    ASSERT_TRUE(exact.hasValue());
    EXPECT_EQ(exact.value().microseconds(), maximum);
    const auto nearMaximum = RationalRate::create(2'000'000, 6'148'914'691'236'517'205).value();
    const auto carryOverflow = nearMaximum.frameStartTimeRounded(FrameId{3});
    ASSERT_FALSE(carryOverflow.hasValue());
    EXPECT_EQ(carryOverflow.error().code, MediaErrorCode::kArithmeticOverflow);
}

TEST(RationalRateTests, RejectsInvalidRatesAndUnrepresentableConversions) {
    EXPECT_FALSE(RationalRate::create(0, 1).hasValue());
    EXPECT_FALSE(RationalRate::create(1, 0).hasValue());
    EXPECT_FALSE(RationalRate::create(-1, 1).hasValue());
    EXPECT_FALSE(RationalRate::create(1, -1).hasValue());

    const auto rateResult = RationalRate::create(1, 1);
    ASSERT_TRUE(rateResult.hasValue());
    const auto overflow =
        rateResult.value().frameStartTime(FrameId{std::numeric_limits<std::int64_t>::max() - 1});
    ASSERT_FALSE(overflow.hasValue());
    EXPECT_EQ(overflow.error().code, MediaErrorCode::kArithmeticOverflow);
}

} // namespace dvs::domain
