#pragma once

#include "dvs/application/Events.h"
#include "dvs/platform/PresentationAckMailbox.h"
#include "dvs/platform/RenderActivitySink.h"

#include <QObject>

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

class QQuickItem;

namespace dvs::application {
class IApplicationEventSink;
}

namespace dvs::ui {

struct RenderAckRelayStatistics final {
    std::uint64_t frameNotifications = 0U;
    std::uint64_t ackNotifications = 0U;
    std::uint64_t ackBackpressureNotifications = 0U;
    std::uint64_t acknowledgementsPopped = 0U;
    std::uint64_t canonicalFrameGaps = 0U;
    std::uint64_t canonicalFrameRegressions = 0U;
    std::uint64_t criticalPostsAccepted = 0U;
    std::uint64_t updateRequests = 0U;
    std::uint64_t renderRetryRequests = 0U;
    std::uint64_t queuedUpdates = 0U;
    std::uint64_t itemUpdates = 0U;
    std::uint64_t frameToRenderSamples = 0U;
    std::uint64_t totalFrameToRenderMicroseconds = 0U;
    std::uint64_t renderToAckSamples = 0U;
    std::uint64_t totalRenderToAckMicroseconds = 0U;
    std::uint64_t frameToAckSamples = 0U;
    std::uint64_t totalFrameToAckMicroseconds = 0U;
    std::uint64_t maximumFrameToAckMicroseconds = 0U;
    std::thread::id workerThread;
    std::thread::id lastCriticalPostThread;
};

// Relays render acknowledgements to the application critical lane without ever blocking the
// render thread. attach() and detach() are GUI-thread operations; notifications are thread-safe.
class RenderAckRelay final : public QObject, public platform::IRenderActivitySink {
public:
    RenderAckRelay(std::shared_ptr<platform::PresentationAckMailbox> acknowledgementMailbox,
                   std::weak_ptr<application::IApplicationEventSink> events,
                   QObject* parent = nullptr);
    ~RenderAckRelay() override;

    RenderAckRelay(const RenderAckRelay&) = delete;
    RenderAckRelay& operator=(const RenderAckRelay&) = delete;
    RenderAckRelay(RenderAckRelay&&) = delete;
    RenderAckRelay& operator=(RenderAckRelay&&) = delete;

    void attach(QQuickItem* item) noexcept;
    void detach() noexcept;

    // Render-thread convenience: only an admitted acknowledgement wakes the relay. Full leaves
    // ownership with the caller so it can retry on the coalesced item update requested by pop().
    [[nodiscard]] platform::PresentationAckPushResult
    tryPublishAcknowledgement(const application::FrameSetPresented& acknowledgement) noexcept;

    void notifyFramePublished() noexcept override;
    void notifyFrameRenderStarted() noexcept override;
    void notifyAckPublished() noexcept override;
    void notifyAckBackpressured() noexcept override;

    // Control-thread only. Closing is immediate; waiting is bounded and queued entries are drained.
    [[nodiscard]] bool shutdown(std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] bool isClosed() const noexcept;
    [[nodiscard]] RenderAckRelayStatistics statistics() const noexcept;

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace dvs::ui
