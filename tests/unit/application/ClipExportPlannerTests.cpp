#include "dvs/application/ClipExport.h"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace dvs::application {
namespace {

constexpr std::int64_t kThirtyFpsFrameMicroseconds = 33'333;

// 30 fps constant rate: frame N starts at the first whole microsecond at or after N * 1'000'000 /
// 30 us, so frame 8 starts at 266'667 us and frame 12 at 400'000 us.
[[nodiscard]] domain::CanonicalTimeline cfrThirty() {
    const auto rate = domain::RationalRate::create(30, 1);
    EXPECT_TRUE(rate.hasValue());
    return domain::CanonicalTimeline{rate.value()};
}

// Constant-rate timeline with an exact frame duration, so keyframe arithmetic in the alignment
// tests is readable (frame N starts at N * 33'333 us).
[[nodiscard]] domain::CanonicalTimeline cfrExact() {
    const auto rate = domain::RationalRate::create(1'000'000, 33'333);
    EXPECT_TRUE(rate.hasValue());
    return domain::CanonicalTimeline{rate.value()};
}

[[nodiscard]] domain::CanonicalTimeline vfr(const std::int64_t frameCount) {
    std::vector<domain::MediaTime> times;
    times.reserve(static_cast<std::size_t>(frameCount));
    for (std::int64_t frame = 0; frame < frameCount; ++frame) {
        times.emplace_back(frame * kThirtyFpsFrameMicroseconds);
    }
    const auto timeline = domain::FrameTimeline::create(std::move(times));
    EXPECT_TRUE(timeline.hasValue());
    return domain::CanonicalTimeline{
        std::make_shared<const domain::FrameTimeline>(timeline.value())};
}

[[nodiscard]] domain::CanonicalTimeline unavailableVfr() {
    return domain::CanonicalTimeline{std::shared_ptr<const domain::FrameTimeline>{}};
}

TEST(ClipExportPlannerTests, PlansTheExclusiveEndFromTheFollowingFrameStart) {
    const PlaybackRange requested{.inInclusive = domain::FrameId{3},
                                  .outInclusive = domain::FrameId{7}};
    const auto planned = planClipExport(cfrThirty(), 12, requested);
    ASSERT_TRUE(planned.hasValue());
    const ClipExportPlan& plan = planned.value();
    EXPECT_EQ(plan.requestedRange, requested);
    EXPECT_EQ(plan.requestedFrameCount, 5);
    EXPECT_EQ(plan.startMicroseconds, 100'000);
    // Frame 8 starts at 8 / 30 s. The clip must include frame 7 and stop before frame 8, so the
    // exclusive end is frame 8's start rather than the out point's own timestamp.
    EXPECT_TRUE(plan.endMicroseconds.has_value());
    EXPECT_EQ(*plan.endMicroseconds, 266'667);
    EXPECT_EQ(plan.startShiftMicroseconds, 0);
    ASSERT_TRUE(plan.firstExportedFrame.has_value());
    EXPECT_EQ(plan.firstExportedFrame->value(), 3);
}

TEST(ClipExportPlannerTests, RunsToTheEndOfTheStreamFromTheLastFrame) {
    const PlaybackRange requested{.inInclusive = domain::FrameId{10},
                                  .outInclusive = domain::FrameId{11}};
    const auto constant = planClipExport(cfrThirty(), 12, requested);
    ASSERT_TRUE(constant.hasValue());
    EXPECT_TRUE(constant.value().endMicroseconds.has_value());
    EXPECT_EQ(*constant.value().endMicroseconds, 400'000);

    // A variable-rate timeline cannot name the instant after its last frame, so the writer is
    // told to copy to the end of the stream instead of guessing a duration.
    const auto variable = planClipExport(vfr(12), 12, requested);
    ASSERT_TRUE(variable.hasValue());
    EXPECT_FALSE(variable.value().endMicroseconds.has_value());
    EXPECT_EQ(variable.value().requestedFrameCount, 2);
}

TEST(ClipExportPlannerTests, RejectsRangesAndTimelinesItCannotPlan) {
    const PlaybackRange valid{.inInclusive = domain::FrameId{0},
                              .outInclusive = domain::FrameId{1}};

    const auto emptyTimeline = planClipExport(cfrThirty(), 0, valid);
    ASSERT_FALSE(emptyTimeline.hasValue());
    EXPECT_EQ(emptyTimeline.error().code, domain::MediaErrorCode::kInvalidFrameCount);

    const auto beyondEnd = planClipExport(
        cfrThirty(),
        12,
        PlaybackRange{.inInclusive = domain::FrameId{11}, .outInclusive = domain::FrameId{12}});
    ASSERT_FALSE(beyondEnd.hasValue());
    EXPECT_EQ(beyondEnd.error().code, domain::MediaErrorCode::kFrameOutOfRange);

    const auto inverted = planClipExport(
        cfrThirty(),
        12,
        PlaybackRange{.inInclusive = domain::FrameId{7}, .outInclusive = domain::FrameId{3}});
    ASSERT_FALSE(inverted.hasValue());
    EXPECT_EQ(inverted.error().code, domain::MediaErrorCode::kInvalidFrameId);

    const auto unresolved = planClipExport(unavailableVfr(), 12, valid);
    ASSERT_FALSE(unresolved.hasValue());
    EXPECT_EQ(unresolved.error().code, domain::MediaErrorCode::kFrameTimelineInvalid);
}

TEST(ClipExportPlannerTests, AlignsTheStartDownToTheLatestKeyframe) {
    const auto planned = planClipExport(
        cfrExact(),
        12,
        PlaybackRange{.inInclusive = domain::FrameId{5}, .outInclusive = domain::FrameId{9}});
    ASSERT_TRUE(planned.hasValue());
    ASSERT_EQ(planned.value().startMicroseconds, 166'665);

    const std::array<std::int64_t, 4U> keyframes{0, 100'000, 133'332, 266'664};
    const auto aligned = alignClipExportStart(cfrExact(), planned.value(), keyframes);
    ASSERT_TRUE(aligned.hasValue());

    // Frame 5 starts mid-GOP, so the copy pre-rolls to the keyframe of frame 4; the frames the
    // user asked for are still all inside the clip.
    EXPECT_EQ(aligned.value().startMicroseconds, 133'332);
    EXPECT_EQ(aligned.value().startShiftMicroseconds, 133'332 - 166'665);
    ASSERT_TRUE(aligned.value().firstExportedFrame.has_value());
    EXPECT_EQ(aligned.value().firstExportedFrame->value(), 4);
    EXPECT_EQ(aligned.value().requestedFrameCount, 5);
}

TEST(ClipExportPlannerTests, KeepsTheStartWhenTheInPointIsAlreadyAKeyframe) {
    const auto planned = planClipExport(
        cfrExact(),
        12,
        PlaybackRange{.inInclusive = domain::FrameId{3}, .outInclusive = domain::FrameId{6}});
    ASSERT_TRUE(planned.hasValue());

    const std::array<std::int64_t, 2U> keyframes{0, 99'999};
    const auto aligned = alignClipExportStart(cfrExact(), planned.value(), keyframes);
    ASSERT_TRUE(aligned.hasValue());
    EXPECT_EQ(aligned.value().startShiftMicroseconds, 0);
    EXPECT_EQ(aligned.value().startMicroseconds, 99'999);
}

TEST(ClipExportPlannerTests, MovesForwardToTheFirstKeyframeWhenTheRequestPrecedesIt) {
    const PlaybackRange requested{.inInclusive = domain::FrameId{0},
                                  .outInclusive = domain::FrameId{9}};
    const auto planned = planClipExport(cfrExact(), 12, requested);
    ASSERT_TRUE(planned.hasValue());

    const std::array<std::int64_t, 2U> keyframes{33'333, 99'999};
    const auto aligned = alignClipExportStart(cfrExact(), planned.value(), keyframes);
    ASSERT_TRUE(aligned.hasValue());
    EXPECT_EQ(aligned.value().startMicroseconds, 33'333);
    EXPECT_EQ(aligned.value().startShiftMicroseconds, 33'333);
    ASSERT_TRUE(aligned.value().firstExportedFrame.has_value());
    EXPECT_EQ(aligned.value().firstExportedFrame->value(), 1);
}

TEST(ClipExportPlannerTests, NamesNearestMicrosecondRoundedKeyframeBoundaries) {
    struct Example final {
        std::int64_t numerator;
        std::int64_t denominator;
        std::int64_t frame;
        std::int64_t time;
    };
    const std::array examples{
        Example{24000, 1001, 16, 667333},
        Example{24000, 1001, 32, 1334667},
        Example{30000, 1001, 2, 66733},
        Example{60000, 1001, 1, 16683},
        Example{60000, 2002, 2, 66733},
        Example{25, 1, 1, 40000},
        Example{30, 1, 1, 33333},
        Example{60, 1, 2, 33333},
        Example{128, 1, 1, 7813},
        Example{30000, 1001, 0, 0},
        Example{30000, 1001, 2'000'000'000, 66'733'333'333'333},
    };
    for (const auto& example : examples) {
        SCOPED_TRACE(testing::Message() << example.numerator << '/' << example.denominator
                                       << " frame=" << example.frame);
        const domain::CanonicalTimeline timeline{
            domain::RationalRate::create(example.numerator, example.denominator).value()};
        const auto planned = planClipExport(
            timeline,
            example.frame + 5,
            {domain::FrameId{example.frame + 1}, domain::FrameId{example.frame + 3}});
        ASSERT_TRUE(planned.hasValue());
        const std::array keys{example.time};
        const auto aligned = alignClipExportStart(timeline, planned.value(), keys);
        ASSERT_TRUE(aligned.hasValue());
        EXPECT_EQ(aligned.value().firstExportedFrame, domain::FrameId{example.frame});
        EXPECT_EQ(aligned.value().startMicroseconds, example.time);
        EXPECT_EQ(aligned.value().startShiftMicroseconds,
                  example.time - planned.value().startMicroseconds);
        EXPECT_EQ(aligned.value().endMicroseconds, planned.value().endMicroseconds);
        EXPECT_EQ(aligned.value().requestedRange, planned.value().requestedRange);
        EXPECT_EQ(aligned.value().requestedFrameCount, planned.value().requestedFrameCount);
    }
}

TEST(ClipExportPlannerTests, KeepsTimesOutsideRoundedBoundariesOnThePrecedingFrame) {
    struct Example final {
        std::int64_t numerator;
        std::int64_t denominator;
        std::int64_t time;
        std::int64_t frame;
    };
    const std::array examples{
        Example{25, 1, 39999, 0},
        Example{30000, 1001, 66732, 1},
        Example{30000, 1001, 33366, 0},
        Example{128, 1, 7812, 0},
        Example{30000, 1001, 50000, 1},
        Example{30000, 1001, 66734, 2},
    };
    for (const auto& example : examples) {
        SCOPED_TRACE(testing::Message() << example.numerator << '/' << example.denominator
                                       << " time=" << example.time);
        const domain::CanonicalTimeline timeline{
            domain::RationalRate::create(example.numerator, example.denominator).value()};
        const auto planned =
            planClipExport(timeline, 100, {domain::FrameId{50}, domain::FrameId{99}});
        ASSERT_TRUE(planned.hasValue());
        const std::array keys{example.time};
        const auto aligned = alignClipExportStart(timeline, planned.value(), keys);
        ASSERT_TRUE(aligned.hasValue());
        EXPECT_EQ(aligned.value().firstExportedFrame, domain::FrameId{example.frame});
        EXPECT_EQ(aligned.value().startMicroseconds, example.time);
    }
}

TEST(ClipExportPlannerTests, LeavesVariableFrameTimingAndUnavailableFrameMetadataUnchanged) {
    const auto variable = vfr(12);
    const auto planned = planClipExport(variable, 12, {domain::FrameId{5}, domain::FrameId{11}});
    ASSERT_TRUE(planned.hasValue());
    for (const auto key : {33332, 33333}) {
        const std::array keys{std::int64_t{key}};
        const auto aligned = alignClipExportStart(variable, planned.value(), keys);
        ASSERT_TRUE(aligned.hasValue());
        EXPECT_EQ(aligned.value().firstExportedFrame, domain::FrameId{key == 33332 ? 0 : 1});
        EXPECT_FALSE(aligned.value().endMicroseconds.has_value());
    }
    const std::array negative{std::int64_t{-1}};
    const auto aligned = alignClipExportStart(cfrThirty(), planned.value(), negative);
    ASSERT_TRUE(aligned.hasValue());
    EXPECT_FALSE(aligned.value().firstExportedFrame.has_value());

    const std::array zero{std::int64_t{0}};
    const auto unavailable = alignClipExportStart(unavailableVfr(), planned.value(), zero);
    ASSERT_TRUE(unavailable.hasValue());
    EXPECT_FALSE(unavailable.value().firstExportedFrame.has_value());

    auto unbounded = planned.value();
    unbounded.startMicroseconds = std::numeric_limits<std::int64_t>::max();
    const std::array huge{unbounded.startMicroseconds};
    const auto overflow = alignClipExportStart(cfrThirty(), unbounded, huge);
    ASSERT_TRUE(overflow.hasValue());
    EXPECT_FALSE(overflow.value().firstExportedFrame.has_value());
    EXPECT_EQ(overflow.value().startMicroseconds, huge.front());
}

TEST(ClipExportPlannerTests, DoesNotGuessBetweenFramesSharingOneRoundedMicrosecond) {
    const domain::CanonicalTimeline timeline{domain::RationalRate::create(3'000'000, 1).value()};
    const auto planned = planClipExport(timeline, 12, {domain::FrameId{6}, domain::FrameId{11}});
    ASSERT_TRUE(planned.hasValue());
    for (const std::int64_t key : {0, 1}) {
        const std::array keys{key};
        const auto aligned = alignClipExportStart(timeline, planned.value(), keys);
        ASSERT_TRUE(aligned.hasValue());
        EXPECT_EQ(aligned.value().firstExportedFrame, domain::FrameId{key * 3});
        EXPECT_EQ(aligned.value().startMicroseconds, key);
    }
}

TEST(ClipExportPlannerTests, RejectsRangesWithoutAUsableKeyframe) {
    const auto planned = planClipExport(
        cfrExact(),
        12,
        PlaybackRange{.inInclusive = domain::FrameId{3}, .outInclusive = domain::FrameId{4}});
    ASSERT_TRUE(planned.hasValue());

    const std::array<std::int64_t, 1U> disjoint{266'664};
    const auto noKeyframe = alignClipExportStart(cfrExact(), planned.value(), disjoint);
    ASSERT_FALSE(noKeyframe.hasValue());
    EXPECT_EQ(noKeyframe.error().code, domain::MediaErrorCode::kFrameOutOfRange);

    const std::span<const std::int64_t> empty;
    const auto noTable = alignClipExportStart(cfrExact(), planned.value(), empty);
    ASSERT_FALSE(noTable.hasValue());
    EXPECT_EQ(noTable.error().code, domain::MediaErrorCode::kMediaProbeFailed);
}

} // namespace
} // namespace dvs::application
