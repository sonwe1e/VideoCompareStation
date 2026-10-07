#include "dvs/application/ClipExport.h"
#include "dvs/media/ClipExportWriter.h"

#include "AvRaii.h"

extern "C" {
#include <libavutil/dict.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
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
[[nodiscard]] std::vector<PresentedPacket> presentedPackets(
    const std::filesystem::path& path, std::int64_t* const rawOrigin = nullptr) {
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
        if (rawOrigin != nullptr) {
            *rawOrigin = origin;
        }
        for (PresentedPacket& value : result) {
            value.microseconds = av_rescale_q(value.microseconds - origin,
                                              input->streams[streamIndex]->time_base,
                                              AVRational{1, AV_TIME_BASE});
        }
    }
    return result;
}

// Reuse the checked-in 12-frame closed GOP without adding an encoder or CLI dependency.
// At 30000/1001 fps, individual frame endpoints need rounding in the microsecond plan.
[[nodiscard]] bool writeRepeatedGops(const std::filesystem::path& source,
                                     const std::filesystem::path& target,
                                     const int gopCount,
                                     const std::int64_t originTicks,
                                     const AVRational rate = AVRational{30000, 1001},
                                     const bool keyframesOnly = false,
                                     const char* const format = "mp4") {
    const auto sourceUtf8 = source.u8string();
    const std::string sourceUrl{reinterpret_cast<const char*>(sourceUtf8.data()),
                                sourceUtf8.size()};
    AVFormatContext* rawInput = nullptr;
    if (avformat_open_input(&rawInput, sourceUrl.c_str(), nullptr, nullptr) < 0) {
        return false;
    }
    internal::AvFormatContextPtr input{rawInput};
    if (avformat_find_stream_info(input.get(), nullptr) < 0 || input->nb_streams != 1) {
        return false;
    }
    AVStream* const inputStream = input->streams[0];
    std::vector<internal::AvPacketPtr> packets;
    internal::AvPacketPtr packet{av_packet_alloc()};
    if (!packet) {
        return false;
    }
    while (av_read_frame(input.get(), packet.get()) >= 0) {
        if (packet->pts == AV_NOPTS_VALUE || packet->dts == AV_NOPTS_VALUE) {
            return false;
        }
        packets.emplace_back(av_packet_clone(packet.get()));
        if (!packets.back()) {
            return false;
        }
        av_packet_unref(packet.get());
    }
    if (packets.size() != 12U) {
        return false;
    }
    if (keyframesOnly) {
        if ((packets.front()->flags & AV_PKT_FLAG_KEY) == 0) {
            return false;
        }
        // Repeating an independent IDR gives one-frame GOPs without an encoder dependency.
        packets.resize(1U);
    }

    const auto targetUtf8 = target.u8string();
    const std::string targetUrl{reinterpret_cast<const char*>(targetUtf8.data()),
                                targetUtf8.size()};
    AVFormatContext* rawOutput = nullptr;
    if (avformat_alloc_output_context2(&rawOutput, nullptr, format, targetUrl.c_str()) < 0 ||
        rawOutput == nullptr) {
        return false;
    }
    internal::AvOutputFormatContextPtr output{rawOutput};
    AVStream* const outputStream = avformat_new_stream(output.get(), nullptr);
    if (outputStream == nullptr ||
        avcodec_parameters_copy(outputStream->codecpar, inputStream->codecpar) < 0) {
        return false;
    }
    outputStream->time_base = AVRational{1, rate.num};
    outputStream->avg_frame_rate = rate;
    outputStream->codecpar->codec_tag = 0;
    if (avio_open2(&output->pb, targetUrl.c_str(), AVIO_FLAG_WRITE, nullptr, nullptr) < 0) {
        return false;
    }
    AVDictionary* options = nullptr;
    if (std::string_view{format} == "mp4") {
        av_dict_set_int(&options, "movie_timescale", rate.num, 0);
        av_dict_set_int(&options, "video_track_timescale", rate.num, 0);
    }
    const int headerResult = avformat_write_header(output.get(), &options);
    av_dict_free(&options);
    if (headerResult < 0) {
        return false;
    }
    for (int gop = 0; gop < gopCount; ++gop) {
        const std::int64_t offset =
            originTicks + static_cast<std::int64_t>(gop) *
                              static_cast<std::int64_t>(packets.size()) * rate.den;
        for (const auto& original : packets) {
            if (av_packet_ref(packet.get(), original.get()) < 0) {
                return false;
            }
            const auto framePts =
                av_rescale_q(packet->pts, inputStream->time_base, AVRational{1, 30});
            const auto frameDts =
                av_rescale_q(packet->dts, inputStream->time_base, AVRational{1, 30});
            packet->pts = keyframesOnly ? offset : offset + framePts * rate.den;
            packet->dts = keyframesOnly ? offset : offset + frameDts * rate.den;
            packet->duration = rate.den;
            packet->pos = -1;
            av_packet_rescale_ts(packet.get(), AVRational{1, rate.num}, outputStream->time_base);
            if (av_interleaved_write_frame(output.get(), packet.get()) < 0) {
                return false;
            }
        }
    }
    return av_write_trailer(output.get()) >= 0;
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

TEST_P(ClipExportOriginTests, FindsEveryIndexedKeyframeIncludingTheFinalGop) {
    const auto fixture = std::filesystem::path{DVS_MEDIA_FIXTURE_DIR} /
                         "h264_a_320x180_30fps_12.mp4";
    const domain::CanonicalTimeline timeline{domain::RationalRate::create(30000, 1001).value()};
    const auto [inFrame, outFrame] = GetParam();
    const std::atomic_bool cancel{false};
    ClipExportWriter writer;
    for (const int gopCount : {1, 6}) {
        for (const std::int64_t origin : {0, 150000, 150004}) {
            SCOPED_TRACE(testing::Message() << "gops=" << gopCount << " origin=" << origin);
            const auto source = workspace_ / "indexed.mp4";
            const auto target = workspace_ / "last-gop.mp4";
            ASSERT_TRUE(writeRepeatedGops(fixture, source, gopCount, origin));
            const auto keyframes = writer.keyframeTimes(source, cancel);
            std::vector<std::int64_t> expectedKeys;
            for (int gop = 0; gop < gopCount; ++gop) {
                expectedKeys.push_back(static_cast<std::int64_t>(gop) * 400400);
            }
            EXPECT_EQ(keyframes, expectedKeys);

            const int firstFrame = (gopCount - 1) * 12;
            const auto planned = application::planClipExport(
                timeline,
                gopCount * 12,
                {domain::FrameId{firstFrame + inFrame}, domain::FrameId{firstFrame + outFrame}});
            ASSERT_TRUE(planned.hasValue());
            const auto aligned =
                application::alignClipExportStart(timeline, planned.value(), keyframes);
            ASSERT_TRUE(aligned.hasValue());
            EXPECT_EQ(aligned.value().startMicroseconds, expectedKeys.back());

            application::ClipExportJob job;
            job.requestId = 92U;
            job.sourcePath = source;
            job.outputPath = target;
            job.plan = aligned.value();
            const auto report = writer.perform(job, cancel);
            ASSERT_EQ(report.outcome, application::ClipExportOutcome::kCompleted)
                << report.technicalDetail;
            EXPECT_EQ(report.firstPresentationMicroseconds, expectedKeys.back());

            std::int64_t actualOrigin = AV_NOPTS_VALUE;
            auto expected = presentedPackets(source, &actualOrigin);
            EXPECT_EQ(actualOrigin, origin);
            ASSERT_EQ(expected.size(), static_cast<std::size_t>(gopCount * 12));
            expected.erase(expected.begin(), expected.begin() + firstFrame);
            // Frame 7 still requires frame 8, even when cutting inside the final GOP.
            const std::size_t count = outFrame == 4 ? 5U : (outFrame == 7 ? 9U : 12U);
            expected.resize(count);
            const auto firstTime = expected.front().microseconds;
            for (auto& value : expected) {
                value.microseconds -= firstTime;
            }
            EXPECT_EQ(presentedPackets(target), expected);
            EXPECT_EQ(report.packetsWritten, static_cast<std::int64_t>(count));
        }
    }
}

TEST_P(ClipExportOriginTests, FindsKeyframesLoadedByTheFirstMatroskaSeek) {
    const auto fixture = std::filesystem::path{DVS_MEDIA_FIXTURE_DIR} /
                         "h264_a_320x180_30fps_12.mp4";
    const auto [inFrame, outFrame] = GetParam();
    const domain::CanonicalTimeline timeline{domain::RationalRate::create(30, 1).value()};
    const std::atomic_bool cancel{false};
    ClipExportWriter writer;
    for (const int gopCount : {1, 3, 6}) {
        for (const std::int64_t origin : {0, 150, 154}) {
            SCOPED_TRACE(testing::Message() << "GOPs=" << gopCount << " origin=" << origin);
            const auto source = workspace_ / "lazy-cues.mkv";
            const auto target = workspace_ / "lazy-cues-clip.mkv";
            ASSERT_TRUE(writeRepeatedGops(
                fixture, source, gopCount, origin, AVRational{30, 1}, false, "matroska"));
            std::vector<std::int64_t> expectedKeys;
            for (int gop = 0; gop < gopCount; ++gop) {
                expectedKeys.push_back(static_cast<std::int64_t>(gop) * 400000);
            }
            const auto keys = writer.keyframeTimes(source, cancel);
            EXPECT_EQ(keys, expectedKeys);
            const int firstFrame = (gopCount - 1) * 12;
            const auto planned = application::planClipExport(
                timeline,
                gopCount * 12,
                {domain::FrameId{firstFrame + inFrame}, domain::FrameId{firstFrame + outFrame}});
            ASSERT_TRUE(planned.hasValue());
            const auto aligned = application::alignClipExportStart(timeline, planned.value(), keys);
            ASSERT_TRUE(aligned.hasValue());
            EXPECT_EQ(aligned.value().startMicroseconds, expectedKeys.back());

            application::ClipExportJob job;
            job.requestId = 94U;
            job.sourcePath = source;
            job.outputPath = target;
            job.plan = aligned.value();
            const auto report = writer.perform(job, cancel);
            ASSERT_EQ(report.outcome, application::ClipExportOutcome::kCompleted)
                << report.technicalDetail;
            EXPECT_EQ(report.firstPresentationMicroseconds, expectedKeys.back());

            auto expected = presentedPackets(source);
            ASSERT_EQ(expected.size(), static_cast<std::size_t>(gopCount * 12));
            expected.erase(expected.begin(), expected.begin() + firstFrame);
            // Keep the B-frame's forward reference when the out point requires it.
            const std::size_t count = outFrame == 4 ? 5U : (outFrame == 7 ? 9U : 12U);
            expected.resize(count);
            const auto firstTime = expected.front().microseconds;
            for (auto& value : expected) {
                value.microseconds -= firstTime;
            }
            EXPECT_EQ(presentedPackets(target), expected);
            EXPECT_EQ(report.packetsWritten, static_cast<std::int64_t>(count));
        }
    }
}

TEST_P(ClipExportOriginTests, ReportsTheActualFrameAtRoundedPacketTimes) {
    const auto fixture = std::filesystem::path{DVS_MEDIA_FIXTURE_DIR} /
                         "h264_a_320x180_30fps_12.mp4";
    const auto [inFrame, outFrame] = GetParam();
    const int firstFrame = inFrame + 1;
    const std::atomic_bool cancel{false};
    ClipExportWriter writer;
    for (const auto rate : {AVRational{24000, 1001}, AVRational{30000, 1001},
                           AVRational{60000, 1001}, AVRational{25, 1}}) {
        const domain::CanonicalTimeline timeline{
            domain::RationalRate::create(rate.num, rate.den).value()};
        for (const std::int64_t origin : {0, rate.num * 5, rate.num * 5 + 4}) {
            SCOPED_TRACE(testing::Message() << rate.num << '/' << rate.den << " origin=" << origin);
            const auto source = workspace_ / "rounded.mp4";
            const auto target = workspace_ / "rounded-clip.mp4";
            ASSERT_TRUE(writeRepeatedGops(fixture, source, 12, origin, rate, true));
            const auto keys = writer.keyframeTimes(source, cancel);
            ASSERT_EQ(keys.size(), 12U);
            const auto planned = application::planClipExport(
                timeline, 12, {domain::FrameId{firstFrame}, domain::FrameId{outFrame}});
            ASSERT_TRUE(planned.hasValue());
            const auto aligned = application::alignClipExportStart(timeline, planned.value(), keys);
            ASSERT_TRUE(aligned.hasValue());
            EXPECT_EQ(aligned.value().firstExportedFrame, domain::FrameId{firstFrame});
            EXPECT_EQ(aligned.value().startMicroseconds,
                      keys[static_cast<std::size_t>(firstFrame)]);
            EXPECT_EQ(aligned.value().endMicroseconds, planned.value().endMicroseconds);

            application::ClipExportJob job;
            job.requestId = 93U;
            job.sourcePath = source;
            job.outputPath = target;
            job.plan = aligned.value();
            const auto report = writer.perform(job, cancel);
            ASSERT_EQ(report.outcome, application::ClipExportOutcome::kCompleted)
                << report.technicalDetail;
            EXPECT_EQ(report.firstPresentationMicroseconds, aligned.value().startMicroseconds);
            EXPECT_EQ(report.packetsWritten, outFrame - firstFrame + 1);
            std::int64_t actualOrigin = AV_NOPTS_VALUE;
            auto expected = presentedPackets(source, &actualOrigin);
            EXPECT_EQ(actualOrigin, origin);
            ASSERT_EQ(expected.size(), 12U);
            expected.resize(static_cast<std::size_t>(outFrame + 1));
            expected.erase(expected.begin(), expected.begin() + firstFrame);
            const auto actual = presentedPackets(target);
            ASSERT_EQ(actual.size(), expected.size());
            for (std::size_t index = 0; index < actual.size(); ++index) {
                // The IDRs deliberately have identical pixels; packet time and position in the
                // source establish the ordinal, while bytes prove a lossless remux.
                EXPECT_EQ(actual[index].bytes, expected[index].bytes);
                EXPECT_EQ(actual[index].microseconds,
                          av_rescale_q(static_cast<std::int64_t>(index),
                                       AVRational{rate.den, rate.num},
                                       AVRational{1, AV_TIME_BASE}));
            }
        }
    }
}

INSTANTIATE_TEST_SUITE_P(FirstMiddleLast,
                         ClipExportOriginTests,
                         testing::Values(std::pair{0, 4}, std::pair{3, 7}, std::pair{8, 11}));

} // namespace
} // namespace dvs::media
