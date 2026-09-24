#include "dvs/domain/ComparisonValidator.h"
#include "dvs/ui/PreviewThumbnailController.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>
#include <QUrl>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <utility>
#include <vector>

namespace dvs::ui {
namespace {

using namespace std::chrono_literals;

void ensureCoreApplication() {
    if (QCoreApplication::instance() != nullptr) {
        return;
    }
    static int argumentCount = 1;
    static char applicationName[] = "PreviewThumbnailControllerTests";
    static char* arguments[] = {applicationName, nullptr};
    static QCoreApplication application{argumentCount, arguments};
    static_cast<void>(application);
}

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate predicate, const std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        QThread::msleep(1U);
    }
    return predicate();
}

[[nodiscard]] std::shared_ptr<const application::SessionSnapshot>
readySnapshot(const std::uint64_t sessionId) {
    const auto rate = domain::RationalRate::create(30, 1);
    EXPECT_TRUE(rate);
    std::vector<domain::ComparisonSource> sources;
    sources.push_back(domain::ComparisonSource{
        .id = domain::SourceId{0U},
        .role = domain::ComparisonRole::kReference,
        .descriptor =
            domain::MediaDescriptor{
                .normalizedPath = std::filesystem::path{"preview.mp4"},
                .extent = domain::MediaExtent{.width = 1'920U, .height = 1'080U},
                .frameRate = rate.value(),
                .frameCount =
                    domain::FrameCountInfo{
                        .value = 64,
                        .origin = domain::FrameCountOrigin::kReported,
                    },
                .duration = domain::MediaTime{640'000},
                .codecId = "h264",
                .pixelFormatId = "nv12",
                .bitDepth = 8U,
                .decodeCapabilities =
                    domain::DecodeCapabilities{
                        .softwareDecode = true,
                        .d3d11VaDecode = true,
                    },
                .timingConfidence = domain::TimingConfidence::kVerifiedCfr,
                .sourceIdentity = std::nullopt,
            },
        .displayName = "Source A",
    });
    auto validated = domain::ComparisonValidator::validate(std::move(sources));
    EXPECT_TRUE(validated);

    auto snapshot = std::make_shared<application::SessionSnapshot>();
    snapshot->sessionId = domain::SessionId{sessionId};
    snapshot->sessionEpoch = domain::SessionEpoch{3U};
    snapshot->sessionState = domain::SessionState::kReady;
    snapshot->playbackState = domain::PlaybackState::kPaused;
    snapshot->displayedFrame = domain::FrameId{0};
    snapshot->validatedComparison =
        std::make_shared<const domain::ValidatedComparisonSet>(std::move(validated).value().set);
    snapshot->canonicalFrameCount = 64U;
    return snapshot;
}

class FakePreviewThumbnailService final : public application::IPreviewThumbnailService {
public:
    [[nodiscard]] application::PortSubmitResult
    submit(const application::PreviewThumbnailRequest& request,
           std::shared_ptr<application::IPreviewThumbnailSink> sink) override {
        submitted.push_back(request);
        sink_ = std::move(sink);
        return application::PortSubmitResult::Accepted;
    }

    void cancel(const application::RequestContext& context) noexcept override {
        cancelledContexts.push_back(context);
    }

    void deliver(const application::PreviewThumbnailResult& result) {
        ASSERT_TRUE(sink_ != nullptr);
        sink_->onPreviewThumbnail(result);
    }

    std::vector<application::PreviewThumbnailRequest> submitted;
    std::vector<application::RequestContext> cancelledContexts;

private:
    std::shared_ptr<application::IPreviewThumbnailSink> sink_;
};

[[nodiscard]] application::PreviewThumbnailResult
thumbnailResult(const application::PreviewThumbnailRequest& request) {
    // RequestContext has a deleted default constructor, so the result is fully designated.
    return application::PreviewThumbnailResult{
        .context = request.context,
        .frameId = request.frameId,
        .rgba = std::vector<std::uint8_t>(4U * 2U * 2U, 0xFFU),
        .width = 2U,
        .height = 2U,
        .available = true,
    };
}

TEST(PreviewThumbnailControllerTests, SessionChangeDropsCachedThumbnailsAndBustsUrlGeneration) {
    ensureCoreApplication();
    const auto sessionA = readySnapshot(101U);
    const auto sessionB = readySnapshot(202U);
    std::shared_ptr<const application::SessionSnapshot> current = sessionA;
    FakePreviewThumbnailService service;
    PreviewThumbnailController controller{PreviewThumbnailController::Dependencies{
        .snapshot = [&current] { return current; },
        .service = &service,
    }};

    controller.request(10);
    ASSERT_TRUE(waitUntil([&] { return service.submitted.size() == 1U; }));
    service.deliver(thumbnailResult(service.submitted.front()));
    ASSERT_TRUE(waitUntil([&] { return controller.hasThumbnail(10); }));
    const QUrl firstUrl = controller.urlForFrame(10);
    EXPECT_FALSE(firstUrl.toString().isEmpty());
    EXPECT_EQ(controller.generation(), 1);

    // Switching sessions must drop the previous session's thumbnails and change the URLs so
    // neither the controller cache nor the QML image cache can serve the old video's frame.
    current = sessionB;
    EXPECT_FALSE(controller.hasThumbnail(10));
    EXPECT_TRUE(controller.urlForFrame(10).toString().isEmpty());
    EXPECT_GT(controller.generation(), 1);

    // A late result from the previous session must not repopulate the cache.
    service.deliver(thumbnailResult(service.submitted.front()));
    for (int spin = 0; spin < 20; ++spin) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        QThread::msleep(1U);
    }
    EXPECT_FALSE(controller.hasThumbnail(10));

    // The new session decodes its own frame under the new URL generation.
    controller.request(10);
    ASSERT_TRUE(waitUntil([&] { return service.submitted.size() == 2U; }));
    EXPECT_EQ(service.submitted.at(1U).context.sessionId, domain::SessionId{202U});
    service.deliver(thumbnailResult(service.submitted.at(1U)));
    ASSERT_TRUE(waitUntil([&] { return controller.hasThumbnail(10); }));
    EXPECT_NE(controller.urlForFrame(10).toString(), firstUrl.toString());
}

TEST(PreviewThumbnailControllerTests, SessionChangeCancelsStaleInflightAndAllowsResubmit) {
    ensureCoreApplication();
    const auto sessionA = readySnapshot(101U);
    const auto sessionB = readySnapshot(202U);
    std::shared_ptr<const application::SessionSnapshot> current = sessionA;
    FakePreviewThumbnailService service;
    PreviewThumbnailController controller{PreviewThumbnailController::Dependencies{
        .snapshot = [&current] { return current; },
        .service = &service,
    }};

    // Leave one decode in flight for session A (the service never delivers it).
    controller.request(20);
    ASSERT_TRUE(waitUntil([&] { return service.submitted.size() == 1U; }));

    current = sessionB;
    controller.request(20);
    // The stale in-flight request is cancelled and the same frame is re-requested for the
    // new session instead of being skipped as "already in flight".
    ASSERT_TRUE(waitUntil([&] { return service.submitted.size() == 2U; }));
    EXPECT_EQ(service.submitted.at(1U).context.sessionId, domain::SessionId{202U});
    ASSERT_FALSE(service.cancelledContexts.empty());
    EXPECT_EQ(service.cancelledContexts.front().sessionId, domain::SessionId{101U});
}

TEST(PreviewThumbnailControllerTests, SupersededLateResultDoesNotClearNewerInflight) {
    ensureCoreApplication();
    const auto session = readySnapshot(101U);
    std::shared_ptr<const application::SessionSnapshot> current = session;
    FakePreviewThumbnailService service;
    PreviewThumbnailController controller{PreviewThumbnailController::Dependencies{
        .snapshot = [&current] { return current; },
        .service = &service,
    }};

    // First hover leaves frame 5 in flight.
    controller.request(5);
    ASSERT_TRUE(waitUntil([&] { return service.submitted.size() == 1U; }));

    // Hovering another frame supersedes it: the old context is cancelled and frame 7 is now
    // the in-flight decode.
    controller.request(7);
    ASSERT_TRUE(waitUntil([&] { return service.submitted.size() == 2U; }));
    ASSERT_FALSE(service.cancelledContexts.empty());

    // The superseded decode delivers late. Its thumbnail is cached (it is a valid decode of a
    // requested frame), but it must not clear the newer job's in-flight bookkeeping —
    // otherwise re-hovering frame 7 would resubmit and decode it from scratch.
    service.deliver(thumbnailResult(service.submitted.front()));
    ASSERT_TRUE(waitUntil([&] { return controller.hasThumbnail(5); }));

    controller.request(7);
    for (int spin = 0; spin < 20; ++spin) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        QThread::msleep(1U);
    }
    EXPECT_EQ(service.submitted.size(), 2U);
    EXPECT_FALSE(controller.hasThumbnail(7));
}

TEST(PreviewThumbnailControllerTests, FailedDecodeAllowsSameFrameRetry) {
    ensureCoreApplication();
    const auto session = readySnapshot(101U);
    FakePreviewThumbnailService service;
    PreviewThumbnailController controller{PreviewThumbnailController::Dependencies{
        .snapshot = [&session] { return session; }, .service = &service}};
    controller.request(10);
    ASSERT_TRUE(waitUntil([&] { return service.submitted.size() == 1U; }));
    auto failed = thumbnailResult(service.submitted.front());
    failed.available = false;
    failed.rgba.clear();
    service.deliver(failed);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    EXPECT_FALSE(controller.hasThumbnail(10));
    controller.request(10);
    ASSERT_TRUE(waitUntil([&] { return service.submitted.size() == 2U; }));
    service.deliver(thumbnailResult(service.submitted.back()));
    ASSERT_TRUE(waitUntil([&] { return controller.hasThumbnail(10); }));
}

TEST(PreviewThumbnailControllerTests, LateFailurePreservesNewerInflightRequest) {
    ensureCoreApplication();
    const auto session = readySnapshot(101U);
    FakePreviewThumbnailService service;
    PreviewThumbnailController controller{PreviewThumbnailController::Dependencies{
        .snapshot = [&session] { return session; }, .service = &service}};
    controller.request(5);
    ASSERT_TRUE(waitUntil([&] { return service.submitted.size() == 1U; }));
    controller.request(7);
    ASSERT_TRUE(waitUntil([&] { return service.submitted.size() == 2U; }));
    auto failed = thumbnailResult(service.submitted.front());
    failed.available = false;
    service.deliver(failed);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    controller.request(7);
    for (int spin = 0; spin < 120; ++spin) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        QThread::msleep(1U);
    }
    EXPECT_EQ(service.submitted.size(), 2U);
    service.deliver(thumbnailResult(service.submitted.back()));
    ASSERT_TRUE(waitUntil([&] { return controller.hasThumbnail(7); }));
}

} // namespace
} // namespace dvs::ui
