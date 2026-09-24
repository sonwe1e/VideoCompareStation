#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "dvs/application/PreviewThumbnail.h"
#include "dvs/media/MediaProbe.h"
#include "dvs/media/PreviewThumbnailService.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace dvs::media {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] std::filesystem::path fixture(const char* const name) {
    return std::filesystem::path{DVS_MEDIA_FIXTURE_DIR} / name;
}

[[nodiscard]] domain::ComparisonSource probeSource(const std::filesystem::path& path,
                                                   const domain::SourceId sourceId) {
    const auto descriptor = MediaProbe::inspect(path, sourceId);
    EXPECT_TRUE(descriptor.hasValue());
    return domain::ComparisonSource{.id = sourceId,
                                    .role = domain::ComparisonRole::kPrediction,
                                    .descriptor = descriptor.value(),
                                    .displayName = "Hover source"};
}

// The sink is invoked on the service worker thread, so the collection is mutex-guarded and
// completion-driven.
class CollectingSink final : public application::IPreviewThumbnailSink {
public:
    void onPreviewThumbnail(application::PreviewThumbnailResult result) override {
        std::scoped_lock lock(mutex_);
        result_ = std::move(result);
        completed_ = true;
        condition_.notify_all();
    }

    [[nodiscard]] bool waitForCompletion(const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return completed_; });
    }

    [[nodiscard]] std::optional<application::PreviewThumbnailResult> take() {
        std::scoped_lock lock(mutex_);
        return result_;
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool completed_ = false;
    std::optional<application::PreviewThumbnailResult> result_;
};

[[nodiscard]] application::PreviewThumbnailRequest
makeRequest(const domain::ComparisonSource& source,
            const std::uint64_t requestId,
            const std::int64_t frame,
            const std::uint32_t maxWidth,
            const std::uint32_t maxHeight) {
    return application::PreviewThumbnailRequest{
        .context =
            application::RequestContext{
                domain::SessionId{1},
                domain::SessionEpoch{1},
                domain::RequestId{requestId},
            },
        .source = source,
        .frameId = domain::FrameId{frame},
        .maxWidth = maxWidth,
        .maxHeight = maxHeight,
    };
}

TEST(PreviewThumbnailServiceTests, DownscalePreservesSourceAspectRatio) {
    const domain::ComparisonSource source =
        probeSource(fixture("h264_a_320x180_30fps_12.mp4"), domain::SourceId{1});
    PreviewThumbnailService service;
    const auto sink = std::make_shared<CollectingSink>();

    // 320x180 inside a 100x90 box: width is the binding dimension, so the aspect-preserving
    // target is 100x56 (180 * 100 / 320 = 56.25 -> 56), not a stretched 100x90.
    const auto request = makeRequest(source, 1U, 3, 100U, 90U);
    ASSERT_TRUE(request.isValid());
    ASSERT_EQ(service.submit(request, sink), application::PortSubmitResult::Accepted);
    ASSERT_TRUE(sink->waitForCompletion(5s));

    const auto result = sink->take();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->context, request.context);
    EXPECT_EQ(result->frameId, request.frameId);
    EXPECT_TRUE(result->available);
    EXPECT_EQ(result->width, 100U);
    EXPECT_EQ(result->height, 56U);
    ASSERT_EQ(result->rgba.size(), static_cast<std::size_t>(100U) * 56U * 4U);
}

TEST(PreviewThumbnailServiceTests, ConsecutiveHoversDecodeThroughReusedSession) {
    const domain::ComparisonSource source =
        probeSource(fixture("h264_a_320x180_30fps_12.mp4"), domain::SourceId{1});
    PreviewThumbnailService service;

    const auto firstSink = std::make_shared<CollectingSink>();
    const auto firstRequest = makeRequest(source, 1U, 2, 176U, 99U);
    ASSERT_TRUE(firstRequest.isValid());
    ASSERT_EQ(service.submit(firstRequest, firstSink), application::PortSubmitResult::Accepted);
    ASSERT_TRUE(firstSink->waitForCompletion(5s));
    ASSERT_TRUE(firstSink->take().value().available);

    // The second hover addresses the same source: the kept decoder session must serve it.
    const auto secondSink = std::make_shared<CollectingSink>();
    const auto secondRequest = makeRequest(source, 2U, 5, 176U, 99U);
    ASSERT_EQ(service.submit(secondRequest, secondSink), application::PortSubmitResult::Accepted);
    ASSERT_TRUE(secondSink->waitForCompletion(5s));
    const auto second = secondSink->take();
    ASSERT_TRUE(second.has_value());
    EXPECT_TRUE(second->available);
    EXPECT_EQ(second->frameId, domain::FrameId{5});
    EXPECT_EQ(service.decodedFrameCountForTesting(), 2U);
}

} // namespace
} // namespace dvs::media
