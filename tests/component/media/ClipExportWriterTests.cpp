#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "dvs/application/ClipExport.h"
#include "dvs/media/ClipExportWriter.h"
#include "dvs/media/MediaProbe.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace dvs::media {
namespace {

constexpr std::int64_t kThirtyFpsFrameMicroseconds = 33'333;

[[nodiscard]] std::filesystem::path fixture(const char* const name) {
    return std::filesystem::path{DVS_MEDIA_FIXTURE_DIR} / name;
}

// Every fixture used below is a single-GOP H.264 MP4, so an export always pre-rolls to frame 0.
// The mid-clip cut arithmetic is covered by the planner's unit tests instead.
class ScopedTempDirectory final {
public:
    explicit ScopedTempDirectory(const std::string& label) {
        static std::atomic<int> sequence{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("dvs_clip_export_" + label + "_" + std::to_string(stamp) + "_" +
                 std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }

    ~ScopedTempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    ScopedTempDirectory(const ScopedTempDirectory&) = delete;
    ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;
    ScopedTempDirectory(ScopedTempDirectory&&) = delete;
    ScopedTempDirectory& operator=(ScopedTempDirectory&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] application::PlaybackRange rangeOf(const std::int64_t inFrame,
                                                 const std::int64_t outFrame) {
    return application::PlaybackRange{domain::FrameId{inFrame}, domain::FrameId{outFrame}};
}

// Mirrors what the controller does on its worker thread: probe the source for its timeline, plan
// the requested range, then move the start onto a real keyframe.
[[nodiscard]] std::optional<application::ClipExportPlan>
alignedPlanFor(const std::filesystem::path& source,
               const application::PlaybackRange range,
               const std::vector<std::int64_t>& keyframes) {
    const auto descriptor = MediaProbe::inspect(source, 0U);
    if (!descriptor.hasValue() || !descriptor.value().frameRate.has_value()) {
        return std::nullopt;
    }
    const domain::CanonicalTimeline timeline{*descriptor.value().frameRate};
    const auto plan =
        application::planClipExport(timeline, descriptor.value().frameCount.value, range);
    if (!plan.hasValue()) {
        return std::nullopt;
    }
    const auto aligned = application::alignClipExportStart(timeline, plan.value(), keyframes);
    if (!aligned.hasValue()) {
        return std::nullopt;
    }
    return aligned.value();
}

[[nodiscard]] application::ClipExportJob makeJob(const std::filesystem::path& source,
                                                 const std::filesystem::path& target,
                                                 const application::ClipExportPlan& plan) {
    application::ClipExportJob job;
    job.requestId = 41U;
    job.sessionId = domain::SessionId{7U};
    job.sessionEpoch = domain::SessionEpoch{2U};
    job.sourcePath = source;
    job.outputPath = target;
    job.plan = plan;
    return job;
}

[[nodiscard]] std::size_t partialFileCount(const std::filesystem::path& directory) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        if (entry.path().filename().string().find(".partial") != std::string::npos) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] std::string readText(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream << text;
}

TEST(ClipExportWriterTests, CopiesTheRequestedRangeIntoAStandaloneClip) {
    const std::filesystem::path source = fixture("h264_a_320x180_30fps_12.mp4");
    const ScopedTempDirectory workspace{"copy"};
    const std::filesystem::path target = workspace.path() / "clip.mp4";

    const std::atomic_bool cancel{false};
    ClipExportWriter writer;
    const std::vector<std::int64_t> keyframes = writer.keyframeTimes(source, cancel);
    // Fixture fact: one sync sample, so every cut of this file pre-rolls to frame 0.
    EXPECT_EQ(keyframes, std::vector<std::int64_t>({0}));

    const auto plan = alignedPlanFor(source, rangeOf(0, 4), keyframes);
    ASSERT_TRUE(plan.has_value());
    ASSERT_TRUE(plan->endMicroseconds.has_value());
    EXPECT_EQ(plan->startMicroseconds, 0);
    EXPECT_EQ(plan->startShiftMicroseconds, 0);
    ASSERT_TRUE(plan->firstExportedFrame.has_value());
    EXPECT_EQ(plan->firstExportedFrame->value(), 0);
    EXPECT_EQ(plan->requestedFrameCount, 5);

    std::vector<double> progress;
    application::ClipExportJob job = makeJob(source, target, *plan);
    job.progress = [&progress](const double value) { progress.push_back(value); };

    const application::ClipExportReport report = writer.perform(job, cancel);

    EXPECT_EQ(report.requestId, 41U);
    EXPECT_EQ(report.outcome, application::ClipExportOutcome::kCompleted);
    EXPECT_EQ(report.packetsWritten, 5);
    EXPECT_EQ(report.firstPresentationMicroseconds, 0);
    EXPECT_TRUE(report.technicalDetail.empty());
    ASSERT_TRUE(std::filesystem::exists(target));
    EXPECT_EQ(partialFileCount(workspace.path()), 0U);
    ASSERT_FALSE(progress.empty());
    EXPECT_DOUBLE_EQ(progress.back(), 1.0);
    for (std::size_t index = 1; index < progress.size(); ++index) {
        EXPECT_LE(progress[index - 1], progress[index]);
    }

    const auto descriptor = MediaProbe::inspect(target, 0U);
    ASSERT_TRUE(descriptor.hasValue()) << descriptor.error().technicalDetail;
    EXPECT_EQ(descriptor.value().frameCount.value, 5);
    EXPECT_EQ(descriptor.value().extent.width, 320U);
    EXPECT_EQ(descriptor.value().extent.height, 180U);
    ASSERT_TRUE(descriptor.value().frameRate.has_value());
    EXPECT_EQ(descriptor.value().frameRate->numerator(), 30);
    EXPECT_EQ(descriptor.value().frameRate->denominator(), 1);
    // Five frames, the last one's own duration included.
    EXPECT_NEAR(static_cast<double>(descriptor.value().duration.microseconds()),
                static_cast<double>(5 * kThirtyFpsFrameMicroseconds),
                2'000.0);
}

TEST(ClipExportWriterTests, PreRollsAMidClipStartOntoTheEarlierKeyframe) {
    const std::filesystem::path source = fixture("h264_a_320x180_30fps_12.mp4");
    const ScopedTempDirectory workspace{"preroll"};
    const std::filesystem::path target = workspace.path() / "clip.mp4";

    const std::atomic_bool cancel{false};
    ClipExportWriter writer;
    const std::vector<std::int64_t> keyframes = writer.keyframeTimes(source, cancel);

    const auto plan = alignedPlanFor(source, rangeOf(3, 7), keyframes);
    ASSERT_TRUE(plan.has_value());
    // Frames 3-7 were asked for; the copy starts at the only keyframe, three frames earlier.
    EXPECT_EQ(plan->startMicroseconds, 0);
    EXPECT_EQ(plan->startShiftMicroseconds, -100'000);
    ASSERT_TRUE(plan->firstExportedFrame.has_value());
    EXPECT_EQ(plan->firstExportedFrame->value(), 0);
    EXPECT_EQ(plan->requestedFrameCount, 5);

    const application::ClipExportReport report =
        writer.perform(makeJob(source, target, *plan), cancel);

    EXPECT_EQ(report.outcome, application::ClipExportOutcome::kCompleted);
    EXPECT_EQ(report.packetsWritten, 9);
    EXPECT_EQ(report.firstPresentationMicroseconds, 0);

    const auto descriptor = MediaProbe::inspect(target, 0U);
    ASSERT_TRUE(descriptor.hasValue()) << descriptor.error().technicalDetail;
    // Frames 0-8 rather than the five asked for: the copy has to carry the packet that presents
    // just past the out point because an earlier frame decodes from it. A reordered stream cannot
    // cut tighter than that without re-encoding.
    EXPECT_EQ(descriptor.value().frameCount.value, 9);
}

TEST(ClipExportWriterTests, RunsToTheEndOfTheStreamWhenThePlanHasNoEndBound) {
    const std::filesystem::path source = fixture("h264_a_320x180_30fps_12.mp4");
    const ScopedTempDirectory workspace{"tail"};
    const std::filesystem::path target = workspace.path() / "clip.mp4";

    const std::atomic_bool cancel{false};
    ClipExportWriter writer;
    const std::vector<std::int64_t> keyframes = writer.keyframeTimes(source, cancel);

    const auto planned = alignedPlanFor(source, rangeOf(8, 11), keyframes);
    ASSERT_TRUE(planned.has_value());
    // A variable-rate tail has no nameable end, which is what an absent bound means to the writer.
    application::ClipExportPlan plan = *planned;
    plan.endMicroseconds.reset();

    const application::ClipExportReport report =
        writer.perform(makeJob(source, target, plan), cancel);

    EXPECT_EQ(report.outcome, application::ClipExportOutcome::kCompleted);
    EXPECT_EQ(report.packetsWritten, 12);
    const auto descriptor = MediaProbe::inspect(target, 0U);
    ASSERT_TRUE(descriptor.hasValue()) << descriptor.error().technicalDetail;
    EXPECT_EQ(descriptor.value().frameCount.value, 12);
}

TEST(ClipExportWriterTests, LeavesNoFileBehindWhenTheExportIsCanceled) {
    const std::filesystem::path source = fixture("h264_a_320x180_30fps_12.mp4");
    const ScopedTempDirectory workspace{"cancel"};
    const std::filesystem::path target = workspace.path() / "clip.mp4";

    // The keyframe query answers with an empty table once it is canceled, which is why the plan is
    // built first and only its execution is asked to stop.
    const std::atomic_bool cancel{false};
    const std::atomic_bool alreadyCanceled{true};
    ClipExportWriter writer;
    const std::vector<std::int64_t> keyframes = writer.keyframeTimes(source, cancel);
    const auto plan = alignedPlanFor(source, rangeOf(0, 4), keyframes);
    ASSERT_TRUE(plan.has_value());

    const application::ClipExportReport report =
        writer.perform(makeJob(source, target, *plan), alreadyCanceled);

    EXPECT_EQ(report.outcome, application::ClipExportOutcome::kCanceled);
    EXPECT_EQ(report.packetsWritten, 0);
    EXPECT_FALSE(std::filesystem::exists(target));
    EXPECT_EQ(partialFileCount(workspace.path()), 0U);
}

TEST(ClipExportWriterTests, ReplacesAnExistingTargetWithTheNewClip) {
    const std::filesystem::path source = fixture("h264_a_320x180_30fps_12.mp4");
    const ScopedTempDirectory workspace{"replace"};
    const std::filesystem::path target = workspace.path() / "clip.mp4";
    writeText(target, "an older clip");

    const std::atomic_bool cancel{false};
    ClipExportWriter writer;
    const std::vector<std::int64_t> keyframes = writer.keyframeTimes(source, cancel);
    const auto plan = alignedPlanFor(source, rangeOf(0, 4), keyframes);
    ASSERT_TRUE(plan.has_value());

    const application::ClipExportReport report =
        writer.perform(makeJob(source, target, *plan), cancel);

    EXPECT_EQ(report.outcome, application::ClipExportOutcome::kCompleted);
    const auto descriptor = MediaProbe::inspect(target, 0U);
    ASSERT_TRUE(descriptor.hasValue()) << descriptor.error().technicalDetail;
    EXPECT_EQ(descriptor.value().frameCount.value, 5);
    EXPECT_EQ(partialFileCount(workspace.path()), 0U);
}

TEST(ClipExportWriterTests, KeepsAnExistingTargetWhenTheSourceIsUnreadable) {
    const ScopedTempDirectory workspace{"failure"};
    const std::filesystem::path source = workspace.path() / "missing.mp4";
    const std::filesystem::path target = workspace.path() / "clip.mp4";
    writeText(target, "an older clip");

    const std::atomic_bool cancel{false};
    ClipExportWriter writer;
    EXPECT_TRUE(writer.keyframeTimes(source, cancel).empty());

    application::ClipExportPlan plan;
    plan.requestedRange = rangeOf(0, 4);
    plan.startMicroseconds = 0;
    plan.endMicroseconds = 100'000;

    const application::ClipExportReport report =
        writer.perform(makeJob(source, target, plan), cancel);

    EXPECT_EQ(report.outcome, application::ClipExportOutcome::kFailed);
    EXPECT_FALSE(report.technicalDetail.empty());
    EXPECT_EQ(readText(target), "an older clip");
    EXPECT_EQ(partialFileCount(workspace.path()), 0U);
}

} // namespace
} // namespace dvs::media
