#include "dvs/application/ClipExport.h"
#include "dvs/media/ClipExportWriter.h"

#include "AvRaii.h"

extern "C" {
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace dvs::media {
namespace {

struct PresentedPacket final {
    std::int64_t microseconds = 0;
    std::vector<std::uint8_t> bytes;

    [[nodiscard]] bool operator==(const PresentedPacket&) const noexcept = default;
};

// Compare the actual compressed frame identities and their spacing, independently of the
// writer's packet counter. Each file's first presentation timestamp is its local origin.
[[nodiscard]] std::vector<PresentedPacket> presentedPackets(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    const std::string url{reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, url.c_str(), nullptr, nullptr) < 0) {
        return {};
    }
    internal::AvFormatContextPtr input{raw};
    if (avformat_find_stream_info(input.get(), nullptr) < 0) {
        return {};
    }
    const int streamIndex =
        av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        return {};
    }
    internal::AvPacketPtr packet{av_packet_alloc()};
    if (!packet) {
        return {};
    }
    std::vector<PresentedPacket> result;
    while (av_read_frame(input.get(), packet.get()) >= 0) {
        if (packet->stream_index == streamIndex && packet->pts != AV_NOPTS_VALUE) {
            result.push_back(PresentedPacket{
                .microseconds = packet->pts,
                .bytes = std::vector<std::uint8_t>{packet->data, packet->data + packet->size},
            });
        }
        av_packet_unref(packet.get());
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.microseconds < right.microseconds;
    });
    if (!result.empty()) {
        const std::int64_t origin = result.front().microseconds;
        for (PresentedPacket& value : result) {
            value.microseconds = av_rescale_q(value.microseconds - origin,
                                              input->streams[streamIndex]->time_base,
                                              AVRational{1, AV_TIME_BASE});
        }
    }
    return result;
}

class ClipExportOriginTests : public testing::TestWithParam<std::pair<int, int>> {
protected:
    void SetUp() override {
        static std::atomic<int> sequence{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        workspace_ = std::filesystem::temp_directory_path() /
                     ("dvs_clip_origin_" + std::to_string(stamp) + "_" +
                      std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directories(workspace_);
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(workspace_, ignored);
    }

    std::filesystem::path workspace_;
};

TEST_P(ClipExportOriginTests, ExportsTheSelectedFramesFromANonzeroPresentationOrigin) {
    const auto source = std::filesystem::path{DVS_MEDIA_FIXTURE_DIR} /
                        "h264_nonzero_start_64x48_30fps_12.mp4";
    const auto target = workspace_ / "clip.mp4";
    const std::atomic_bool cancel{false};
    ClipExportWriter writer;
    const auto keyframes = writer.keyframeTimes(source, cancel);
    ASSERT_EQ(keyframes, std::vector<std::int64_t>{0});

    const domain::CanonicalTimeline timeline{domain::RationalRate::create(30, 1).value()};
    const auto [inFrame, outFrame] = GetParam();
    const auto planned = application::planClipExport(
        timeline, 12, {domain::FrameId{inFrame}, domain::FrameId{outFrame}});
    ASSERT_TRUE(planned.hasValue());
    const auto aligned = application::alignClipExportStart(timeline, planned.value(), keyframes);
    ASSERT_TRUE(aligned.hasValue());

    std::vector<double> progress;
    application::ClipExportJob job;
    job.requestId = 91U;
    job.sourcePath = source;
    job.outputPath = target;
    job.plan = aligned.value();
    job.progress = [&progress](const double value) { progress.push_back(value); };
    const auto report = writer.perform(job, cancel);
    ASSERT_EQ(report.outcome, application::ClipExportOutcome::kCompleted)
        << report.technicalDetail;
    EXPECT_EQ(report.firstPresentationMicroseconds, 0);

    auto expected = presentedPackets(source);
    ASSERT_EQ(expected.size(), 12U);
    // Mid-GOP exports pre-roll to frame zero. Reordered frame 7 also needs frame 8 as a
    // reference, so this is deliberately 9 packets rather than silently promising a tight cut.
    const std::size_t expectedCount = outFrame == 4 ? 5U : (outFrame == 7 ? 9U : 12U);
    expected.resize(expectedCount);
    EXPECT_EQ(presentedPackets(target), expected);
    EXPECT_EQ(report.packetsWritten, static_cast<std::int64_t>(expectedCount));
    ASSERT_GE(progress.size(), 3U);
    EXPECT_DOUBLE_EQ(progress.front(), 0.0);
    EXPECT_GT(progress[1], 0.0);
    EXPECT_LT(progress[1], 1.0);
    EXPECT_DOUBLE_EQ(progress.back(), 1.0);
}

INSTANTIATE_TEST_SUITE_P(FirstMiddleLast,
                         ClipExportOriginTests,
                         testing::Values(std::pair{0, 4}, std::pair{3, 7}, std::pair{8, 11}));

} // namespace
} // namespace dvs::media
