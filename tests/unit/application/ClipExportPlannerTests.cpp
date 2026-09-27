#include "dvs/application/ClipExport.h"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
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
