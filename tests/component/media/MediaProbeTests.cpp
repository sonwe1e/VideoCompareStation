#include "dvs/application/MediaPaths.h"
#include "dvs/media/MediaProbe.h"

#include "MediaProbeTestHooks.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace dvs::media {
namespace {

class RecordingEventSink final : public application::IApplicationEventSink {
public:
    RecordingEventSink() {
        events_.reserve(4U);
    }

    [[nodiscard]] application::EventPostResult
    postCritical(application::ApplicationEvent event) noexcept override {
        {
            std::scoped_lock lock(mutex_);
            events_.push_back(std::move(event));
        }
        condition_.notify_all();
        return application::EventPostResult::Accepted;
    }

    [[nodiscard]] application::EventPostResult
    postRealtime(application::ApplicationEvent) noexcept override {
        return application::EventPostResult::Accepted;
    }

    void closeRealtimeIngress() noexcept override {}
    void closeCriticalIngress() noexcept override {}

    [[nodiscard]] bool waitForEventCount(const std::size_t count) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(
            lock, std::chrono::seconds{5}, [this, count] { return events_.size() >= count; });
    }

    [[nodiscard]] std::vector<application::ApplicationEvent> events() const {
        std::scoped_lock lock(mutex_);
        return events_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<application::ApplicationEvent> events_;
};

class WorkerAdmissionBarrier final {
public:
    ~WorkerAdmissionBarrier() {
        release();
    }

    void arriveAndWait() noexcept {
        std::unique_lock lock(mutex_);
        ++admittedWorkers_;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }

    [[nodiscard]] bool waitForAdmissions(const std::size_t count) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(
            lock, std::chrono::seconds{5}, [this, count] { return admittedWorkers_ >= count; });
    }

    void release() noexcept {
        {
            std::scoped_lock lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t admittedWorkers_ = 0U;
    bool released_ = false;
};

void blockAtWorkerAdmission(void* const context) noexcept {
    auto* const barrier = static_cast<WorkerAdmissionBarrier*>(context);
    if (barrier != nullptr) {
        barrier->arriveAndWait();
    }
}

class ScopedWorkerAdmissionRelease final {
public:
    explicit ScopedWorkerAdmissionRelease(WorkerAdmissionBarrier& barrier) noexcept
        : barrier_(barrier) {}

    ~ScopedWorkerAdmissionRelease() {
        barrier_.release();
    }

    ScopedWorkerAdmissionRelease(const ScopedWorkerAdmissionRelease&) = delete;
    ScopedWorkerAdmissionRelease& operator=(const ScopedWorkerAdmissionRelease&) = delete;

private:
    WorkerAdmissionBarrier& barrier_;
};

[[nodiscard]] std::filesystem::path fixture(const std::string_view name) {
    return std::filesystem::path{DVS_MEDIA_FIXTURE_DIR} / std::string{name};
}

[[nodiscard]] application::RequestContext makeContext(const std::uint64_t requestId) {
    return application::RequestContext{
        .sessionId = domain::SessionId{21},
        .sessionEpoch = domain::SessionEpoch{4},
        .requestId = domain::RequestId{requestId},
    };
}

TEST(MediaProbeTests, VerifiesCfrH264WithNormalizedMetadata) {
    const auto result = MediaProbe::inspect(fixture("h264_a_320x180_30fps_12.mp4"), 0U);

    ASSERT_TRUE(result);
    const domain::MediaDescriptor& descriptor = result.value();
    EXPECT_TRUE(descriptor.isValid());
    EXPECT_EQ(descriptor.extent, (domain::MediaExtent{.width = 320U, .height = 180U}));
    EXPECT_EQ(descriptor.frameRate, domain::RationalRate::create(30, 1).value());
    EXPECT_EQ(descriptor.frameCount.value, 12);
    EXPECT_EQ(descriptor.frameCount.origin, domain::FrameCountOrigin::kIndexed);
    EXPECT_EQ(descriptor.codecId, "h264");
    EXPECT_EQ(descriptor.pixelFormatId, "yuv420p");
    EXPECT_EQ(descriptor.bitDepth, 8U);
    EXPECT_EQ(descriptor.colorMetadata.matrix, domain::ColorMatrix::kBt601);
    EXPECT_EQ(descriptor.colorMetadata.range, domain::ColorRange::kLimited);
    EXPECT_TRUE(descriptor.colorMetadata.matrixInferred);
    EXPECT_EQ(descriptor.timingConfidence, domain::TimingConfidence::kVerifiedCfr);
    ASSERT_TRUE(descriptor.sourceIdentity.has_value());
    EXPECT_TRUE(descriptor.sourceIdentity->isComplete());
}

TEST(MediaProbeTests, VerifiesCfrWhenContainerRateDeclarationsDisagree) {
    const auto result =
        MediaProbe::inspect(fixture("h264_disputed_metadata_320x180_30fps_12.mp4"), 0U);

    ASSERT_TRUE(result);
    const domain::MediaDescriptor& descriptor = result.value();
    EXPECT_TRUE(descriptor.isValid());
    EXPECT_EQ(descriptor.frameRate, domain::RationalRate::create(30, 1).value());
    EXPECT_EQ(descriptor.frameCount.value, 12);
    EXPECT_EQ(descriptor.frameCount.origin, domain::FrameCountOrigin::kIndexed);
    EXPECT_EQ(descriptor.timingConfidence, domain::TimingConfidence::kVerifiedCfr);
}

TEST(MediaProbeTests, AcceptsH265AndKeepsDifferentSourceGeometry) {
    const auto result = MediaProbe::inspect(fixture("h265_b_160x90_30fps_12.mp4"), 1U);

    ASSERT_TRUE(result);
    const domain::MediaDescriptor& descriptor = result.value();
    EXPECT_TRUE(descriptor.isValid());
    EXPECT_EQ(descriptor.extent, (domain::MediaExtent{.width = 160U, .height = 90U}));
    EXPECT_EQ(descriptor.codecId, "hevc");
    EXPECT_EQ(descriptor.frameCount.value, 12);
    EXPECT_EQ(descriptor.frameCount.origin, domain::FrameCountOrigin::kIndexed);
    EXPECT_EQ(descriptor.timingConfidence, domain::TimingConfidence::kVerifiedCfr);
}

TEST(MediaProbeTests, AcceptsMpeg4Part2WhenFfmpegProvidesTheDecoder) {
    const auto result = MediaProbe::inspect(fixture("mpeg4_64x48_30fps_12.mp4"), 0U);

    ASSERT_TRUE(result);
    const domain::MediaDescriptor& descriptor = result.value();
    EXPECT_TRUE(descriptor.isValid());
    EXPECT_EQ(descriptor.codecId, "mpeg4");
    EXPECT_EQ(descriptor.pixelFormatId, "yuv420p");
    EXPECT_EQ(descriptor.frameCount.value, 12);
    EXPECT_EQ(descriptor.frameCount.origin, domain::FrameCountOrigin::kIndexed);
    EXPECT_TRUE(descriptor.decodeCapabilities.softwareDecode);
}

TEST(MediaProbeTests, CountsAnUnreportedFrameTotalFromThePresentationTimestampIndex) {
    const auto result = MediaProbe::inspect(fixture("h264_no_count_64x48_30fps_12.mkv"), 1U);

    ASSERT_TRUE(result);
    const domain::MediaDescriptor& descriptor = result.value();
    EXPECT_TRUE(descriptor.isValid());
    EXPECT_EQ(descriptor.frameCount.value, 12);
    EXPECT_EQ(descriptor.frameCount.origin, domain::FrameCountOrigin::kIndexed);
    EXPECT_EQ(descriptor.timingConfidence, domain::TimingConfidence::kVerifiedCfr);
}

TEST(MediaProbeTests, VerifiesCfrForNonzeroStartFromNormalizedIndex) {
    const auto result = MediaProbe::inspect(fixture("h264_nonzero_start_64x48_30fps_12.mp4"), 1U);

    ASSERT_TRUE(result);
    const domain::MediaDescriptor& descriptor = result.value();
    EXPECT_TRUE(descriptor.isValid());
    EXPECT_EQ(descriptor.frameRate, domain::RationalRate::create(30, 1).value());
    EXPECT_EQ(descriptor.frameCount.value, 12);
    EXPECT_EQ(descriptor.frameCount.origin, domain::FrameCountOrigin::kIndexed);
    EXPECT_EQ(descriptor.timingConfidence, domain::TimingConfidence::kVerifiedCfr);
}

TEST(MediaProbeTests, ClassifiesVariableFrameRateWhenNoCandidateMatches) {
    const auto result = MediaProbe::inspect(fixture("h264_vfr_320x180_12.mp4"), 0U);

    ASSERT_TRUE(result);
    const domain::MediaDescriptor& descriptor = result.value();
    EXPECT_TRUE(descriptor.isValid());
    EXPECT_FALSE(descriptor.frameRate.has_value());
    EXPECT_EQ(descriptor.timingConfidence, domain::TimingConfidence::kVariableFrameRate);
    EXPECT_EQ(descriptor.frameCount.value, 12);
    EXPECT_EQ(descriptor.frameCount.origin, domain::FrameCountOrigin::kIndexed);
}

TEST(MediaProbeTests, ClassifiesMiddlePtsGapAsValidVfrAndPreservesFrameCount) {
    const auto result = MediaProbe::inspect(fixture("h264_middle_pts_gap_64x48_30fps_12.mp4"), 0U);

    ASSERT_TRUE(result);
    const domain::MediaDescriptor& descriptor = result.value();
    EXPECT_TRUE(descriptor.isValid());
    EXPECT_FALSE(descriptor.frameRate.has_value());
    EXPECT_EQ(descriptor.timingConfidence, domain::TimingConfidence::kVariableFrameRate);
    EXPECT_EQ(descriptor.frameCount.value, 12);
    EXPECT_EQ(descriptor.frameCount.origin, domain::FrameCountOrigin::kIndexed);
}

TEST(MediaProbeTests, ClassifiesEndPtsGapAsValidVfrAndPreservesFrameCount) {
    const auto result = MediaProbe::inspect(fixture("h264_end_pts_gap_64x48_30fps_12.mp4"), 0U);

    ASSERT_TRUE(result);
    const domain::MediaDescriptor& descriptor = result.value();
    EXPECT_TRUE(descriptor.isValid());
    EXPECT_FALSE(descriptor.frameRate.has_value());
    EXPECT_EQ(descriptor.timingConfidence, domain::TimingConfidence::kVariableFrameRate);
    EXPECT_EQ(descriptor.frameCount.value, 12);
    EXPECT_EQ(descriptor.frameCount.origin, domain::FrameCountOrigin::kIndexed);
}

TEST(MediaProbeTests, AcceptsTenBitYuv420AndPreservesItsStorageContract) {
    const auto tenBit = MediaProbe::inspect(fixture("h265_10bit_320x180_30fps_12.mp4"), 1U);
    ASSERT_TRUE(tenBit);
    EXPECT_EQ(tenBit.value().bitDepth, 10U);
    EXPECT_EQ(tenBit.value().pixelFormatId, "yuv420p10le");
    EXPECT_TRUE(tenBit.value().decodeCapabilities.softwareDecode);
}

TEST(MediaProbeTests, AcceptsYuv422Yuv444AndRgbInputsForVisualNormalization) {
    const std::array<std::pair<const char*, const char*>, 3U> cases{{
        {"h264_yuv422p_64x48_30fps_12.mkv", "yuv422p"},
        {"h264_yuv444p_64x48_30fps_12.mkv", "yuv444p"},
        {"h264_gbrp_64x48_30fps_12.mkv", "gbrp"},
    }};
    for (const auto& [filename, expectedPixelFormat] : cases) {
        const auto result = MediaProbe::inspect(fixture(filename), 1U);
        ASSERT_TRUE(result) << filename;
        EXPECT_EQ(result.value().pixelFormatId, expectedPixelFormat);
        EXPECT_EQ(result.value().bitDepth, 8U);
    }
}

TEST(MediaProbeTests, PreservesRightAngleRotationAndSampleAspectRatio) {
    const auto rotated =
        MediaProbe::inspect(fixture("h264_rot90_320x180_30fps_12.mp4"), domain::SourceId{0U});
    ASSERT_TRUE(rotated);
    EXPECT_EQ(rotated.value().rotationDegrees, 90U);
    EXPECT_EQ(rotated.value().sampleAspectRatio, domain::SampleAspectRatio{});

    const auto anamorphic =
        MediaProbe::inspect(fixture("h264_sar4x3_64x48_30fps_12.mkv"), domain::SourceId{1U});
    ASSERT_TRUE(anamorphic);
    EXPECT_EQ(anamorphic.value().rotationDegrees, 0U);
    EXPECT_EQ(anamorphic.value().sampleAspectRatio,
              (domain::SampleAspectRatio{.numerator = 4U, .denominator = 3U}));
}

TEST(MediaProbeTests, RejectsCorruptMediaBeforeItCanProduceADescriptor) {
    const auto result = MediaProbe::inspect(fixture("corrupt_h264.mp4"), 0U);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::MediaErrorCode::kMediaOpenFailed);
}

TEST(MediaProbeTests, PostsPayloadThenOneSuccessfulTerminalThroughWeakEventSink) {
    MediaProbe probe;
    auto events = std::make_shared<RecordingEventSink>();
    const application::MediaProbeRequest request{
        .context = makeContext(98),
        .sourceId = 0U,
        .sourcePath = fixture("h264_a_320x180_30fps_12.mp4"),
    };

    EXPECT_EQ(probe.submit(request, events), application::PortSubmitResult::Accepted);
    ASSERT_TRUE(events->waitForEventCount(2U));
    const auto recorded = events->events();
    ASSERT_EQ(recorded.size(), 2U);
    ASSERT_TRUE(std::holds_alternative<application::ProbeCompleted>(recorded[0]));
    EXPECT_TRUE(std::holds_alternative<application::RequestSucceeded>(
        std::get<application::RequestTerminal>(recorded[1])));

    const auto& completion = std::get<application::ProbeCompleted>(recorded[0]);
    EXPECT_EQ(completion.context, request.context);
    EXPECT_EQ(completion.sourceId, 0U);
    EXPECT_TRUE(completion.descriptor.isValid());
    EXPECT_EQ(completion.descriptor.frameCount.value, 12);
}

TEST(MediaProbeTests, DropsCompletionSafelyAfterEventSinkExpires) {
    auto probe = std::make_unique<MediaProbe>();
    auto events = std::make_shared<RecordingEventSink>();
    const application::MediaProbeRequest request{
        .context = makeContext(99),
        .sourceId = 1U,
        .sourcePath = fixture("h264_b_160x90_30fps_12.mp4"),
    };

    ASSERT_EQ(probe->submit(request, events), application::PortSubmitResult::Accepted);
    events.reset();
    probe.reset();
    SUCCEED();
}

TEST(MediaProbeTests, AdmitsTwoWorkersAndPostsOneTerminalForEachRequest) {
    WorkerAdmissionBarrier admissionBarrier;
    testing::ScopedMediaProbeWorkerAdmissionHook admissionHook{blockAtWorkerAdmission,
                                                               &admissionBarrier};
    MediaProbe probe{8U};
    ScopedWorkerAdmissionRelease admissionRelease{admissionBarrier};
    auto events = std::make_shared<RecordingEventSink>();
    const application::MediaProbeRequest canceledRequest{
        .context = makeContext(100),
        .sourceId = 0U,
        .sourcePath = fixture("h264_a_320x180_30fps_12.mp4"),
    };
    const application::MediaProbeRequest completedRequest{
        .context = makeContext(101),
        .sourceId = 1U,
        .sourcePath = fixture("h264_b_160x90_30fps_12.mp4"),
    };

    ASSERT_EQ(probe.submit(canceledRequest, events), application::PortSubmitResult::Accepted);
    ASSERT_EQ(probe.submit(completedRequest, events), application::PortSubmitResult::Accepted);
    ASSERT_TRUE(admissionBarrier.waitForAdmissions(2U));

    probe.cancel(canceledRequest.context);
    admissionBarrier.release();

    ASSERT_TRUE(events->waitForEventCount(3U));
    const auto recorded = events->events();
    ASSERT_EQ(recorded.size(), 3U);

    std::size_t canceledTerminals = 0U;
    std::size_t completions = 0U;
    std::size_t successfulTerminals = 0U;
    for (const application::ApplicationEvent& event : recorded) {
        if (const auto* const completion = std::get_if<application::ProbeCompleted>(&event)) {
            EXPECT_EQ(completion->context, completedRequest.context);
            EXPECT_TRUE(completion->descriptor.isValid());
            ++completions;
            continue;
        }

        const auto* const terminal = std::get_if<application::RequestTerminal>(&event);
        ASSERT_NE(terminal, nullptr);
        if (const auto* const canceled = std::get_if<application::RequestCanceled>(terminal)) {
            const auto* const context =
                std::get_if<application::RequestContext>(&canceled->context);
            ASSERT_NE(context, nullptr);
            EXPECT_EQ(*context, canceledRequest.context);
            ++canceledTerminals;
            continue;
        }

        const auto* const succeeded = std::get_if<application::RequestSucceeded>(terminal);
        ASSERT_NE(succeeded, nullptr);
        const auto* const context = std::get_if<application::RequestContext>(&succeeded->context);
        ASSERT_NE(context, nullptr);
        EXPECT_EQ(*context, completedRequest.context);
        ++successfulTerminals;
    }

    EXPECT_EQ(canceledTerminals, 1U);
    EXPECT_EQ(completions, 1U);
    EXPECT_EQ(successfulTerminals, 1U);
}

// MEDIA1/MEDIA2 investigation baseline: AV1 and VP9 fixtures are real encoded files, and the
// current v1 gate rejects them explicitly instead of decoding them under the SDR matrix.
// MEDIA2 may change this expectation together with the support table and the entry lists.
TEST(MediaProbeTests, RejectsAv1WithTheExplicitCodecGate) {
    const auto result = MediaProbe::inspect(fixture("av1_64x48_30fps_12.mkv"), 0U);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::MediaErrorCode::kUnsupportedCodec);
}

TEST(MediaProbeTests, RejectsVp9WithTheExplicitCodecGate) {
    const auto result = MediaProbe::inspect(fixture("vp9_64x48_30fps_12.webm"), 0U);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::MediaErrorCode::kUnsupportedCodec);
}

TEST(MediaProbeTests, WebmIsStillOutsideTheVideoExtensionEntryGate) {
    // The codec gate and the extension gate must agree: a probe that already rejects VP9 must
    // not be reachable through a .webm entry that the rest of the app still refuses.
    EXPECT_FALSE(application::isVideoPath(std::filesystem::path{"clip.webm"}));
    EXPECT_FALSE(application::isVideoPath(std::filesystem::path{"clip.av1"}));
    EXPECT_TRUE(application::isVideoPath(std::filesystem::path{"clip.mkv"}));
    EXPECT_TRUE(application::isVideoPath(std::filesystem::path{"clip.MKV"}));
}

} // namespace
} // namespace dvs::media
