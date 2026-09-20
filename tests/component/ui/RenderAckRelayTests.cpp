#include "dvs/application/Ports.h"
#include "dvs/ui/RenderAckRelay.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QQuickItem>
#include <QThread>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace dvs::ui {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] application::FrameSetPresented makeAcknowledgement(const std::uint64_t requestId,
                                                                 const std::int64_t frameId) {
    return application::FrameSetPresented{
        .context =
            application::FrameRequestContext{
                .playback =
                    application::PlaybackRequestContext{
                        .request =
                            application::RequestContext{
                                .sessionId = domain::SessionId{12U},
                                .sessionEpoch = domain::SessionEpoch{3U},
                                .requestId = domain::RequestId{requestId},
                            },
                        .playbackGeneration = domain::PlaybackGeneration{5U},
                    },
                .deviceGeneration = domain::DeviceGeneration{7U},
            },
        .frameId = domain::FrameId{frameId},
    };
}

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate predicate, const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        QThread::yieldCurrentThread();
    }
    return predicate();
}

class RecordingEventSink final : public application::IApplicationEventSink {
public:
    [[nodiscard]] application::EventPostResult
    postCritical(application::ApplicationEvent event) noexcept override {
        const std::lock_guard lock{mutex_};
        criticalEvents_.push_back(std::move(event));
        lastCriticalThread_ = std::this_thread::get_id();
        condition_.notify_all();
        return criticalClosed_ ? application::EventPostResult::Closed
                               : application::EventPostResult::Accepted;
    }

    [[nodiscard]] application::EventPostResult
    postRealtime(application::ApplicationEvent) noexcept override {
        return realtimeClosed_.load(std::memory_order_acquire)
                   ? application::EventPostResult::Closed
                   : application::EventPostResult::Accepted;
    }

    void closeRealtimeIngress() noexcept override {
        realtimeClosed_.store(true, std::memory_order_release);
    }

    void closeCriticalIngress() noexcept override {
        const std::lock_guard lock{mutex_};
        criticalClosed_ = true;
    }

    [[nodiscard]] bool waitForCriticalCount(const std::size_t count,
                                            const std::chrono::milliseconds timeout) const {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(
            lock, timeout, [this, count] { return criticalEvents_.size() >= count; });
    }

    [[nodiscard]] std::vector<application::ApplicationEvent> criticalEvents() const {
        const std::lock_guard lock{mutex_};
        return criticalEvents_;
    }

    [[nodiscard]] std::thread::id lastCriticalThread() const {
        const std::lock_guard lock{mutex_};
        return lastCriticalThread_;
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    std::vector<application::ApplicationEvent> criticalEvents_;
    std::thread::id lastCriticalThread_;
    std::atomic<bool> realtimeClosed_{false};
    bool criticalClosed_ = false;
};

class BlockingEventSink final : public application::IApplicationEventSink {
public:
    [[nodiscard]] application::EventPostResult
    postCritical(application::ApplicationEvent) noexcept override {
        std::unique_lock lock{mutex_};
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
        returned_ = true;
        condition_.notify_all();
        return application::EventPostResult::Accepted;
    }

    [[nodiscard]] application::EventPostResult
    postRealtime(application::ApplicationEvent) noexcept override {
        return application::EventPostResult::Accepted;
    }

    void closeRealtimeIngress() noexcept override {}
    void closeCriticalIngress() noexcept override {}

    [[nodiscard]] bool waitUntilEntered(const std::chrono::milliseconds timeout) const {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, timeout, [this] { return entered_; });
    }

    void release() noexcept {
        const std::lock_guard lock{mutex_};
        released_ = true;
        condition_.notify_all();
    }

    [[nodiscard]] bool waitUntilReturned(const std::chrono::milliseconds timeout) const {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, timeout, [this] { return returned_; });
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
    bool returned_ = false;
};

TEST(RenderAckRelayTests, CoalescesFrameNotificationsIntoOneQueuedItemUpdate) {
    auto mailbox = std::make_shared<platform::PresentationAckMailbox>();
    auto events = std::make_shared<RecordingEventSink>();
    RenderAckRelay relay{mailbox, events};
    QQuickItem item;
    relay.attach(&item);

    for (std::size_t index = 0U; index < 64U; ++index) {
        relay.notifyFramePublished();
    }

    ASSERT_TRUE(waitUntil([&relay] { return relay.statistics().itemUpdates == 1U; }, 1s));
    const RenderAckRelayStatistics statistics = relay.statistics();
    EXPECT_EQ(statistics.frameNotifications, 64U);
    EXPECT_EQ(statistics.updateRequests, 64U);
    EXPECT_EQ(statistics.queuedUpdates, 1U);
    EXPECT_EQ(statistics.itemUpdates, 1U);
    EXPECT_TRUE(relay.shutdown(2s));
}

TEST(RenderAckRelayTests, DrainsAcknowledgementAndPostsOnlyFromRelayWorker) {
    auto mailbox = std::make_shared<platform::PresentationAckMailbox>();
    auto events = std::make_shared<RecordingEventSink>();
    RenderAckRelay relay{mailbox, events};
    QQuickItem item;
    relay.attach(&item);
    const std::thread::id renderCaller = std::this_thread::get_id();
    const application::FrameSetPresented acknowledgement = makeAcknowledgement(9U, 42);

    ASSERT_EQ(relay.tryPublishAcknowledgement(acknowledgement),
              platform::PresentationAckPushResult::Accepted);
    ASSERT_TRUE(events->waitForCriticalCount(1U, 1s));
    ASSERT_TRUE(waitUntil([&relay] { return relay.statistics().criticalPostsAccepted == 1U; }, 1s));

    const std::vector<application::ApplicationEvent> posted = events->criticalEvents();
    ASSERT_EQ(posted.size(), 1U);
    const auto* const presented = std::get_if<application::FrameSetPresented>(&posted.front());
    ASSERT_NE(presented, nullptr);
    EXPECT_EQ(presented->context, acknowledgement.context);
    EXPECT_EQ(presented->frameId, acknowledgement.frameId);

    const RenderAckRelayStatistics statistics = relay.statistics();
    EXPECT_EQ(statistics.ackNotifications, 1U);
    EXPECT_EQ(statistics.acknowledgementsPopped, 1U);
    EXPECT_EQ(statistics.criticalPostsAccepted, 1U);
    EXPECT_EQ(statistics.ackBackpressureNotifications, 0U);
    EXPECT_EQ(statistics.updateRequests, 0U);
    EXPECT_EQ(statistics.renderRetryRequests, 0U);
    EXPECT_EQ(statistics.workerThread, statistics.lastCriticalPostThread);
    EXPECT_EQ(statistics.workerThread, events->lastCriticalThread());
    EXPECT_NE(statistics.workerThread, renderCaller);
    EXPECT_TRUE(relay.shutdown(2s));
}

TEST(RenderAckRelayTests, CountsCanonicalGapsOnlyWithinOnePlaybackScope) {
    auto mailbox = std::make_shared<platform::PresentationAckMailbox>();
    auto events = std::make_shared<RecordingEventSink>();
    RenderAckRelay relay{mailbox, events};

    ASSERT_EQ(relay.tryPublishAcknowledgement(makeAcknowledgement(1U, 10)),
              platform::PresentationAckPushResult::Accepted);
    ASSERT_EQ(relay.tryPublishAcknowledgement(makeAcknowledgement(2U, 12)),
              platform::PresentationAckPushResult::Accepted);
    ASSERT_TRUE(events->waitForCriticalCount(2U, 1s));

    const RenderAckRelayStatistics statistics = relay.statistics();
    EXPECT_EQ(statistics.acknowledgementsPopped, 2U);
    EXPECT_EQ(statistics.canonicalFrameGaps, 1U);
    EXPECT_EQ(statistics.canonicalFrameRegressions, 0U);
    EXPECT_TRUE(relay.shutdown(2s));
}

TEST(RenderAckRelayTests, CountsRegressionButResetsSequenceForAnotherPlaybackGeneration) {
    auto mailbox = std::make_shared<platform::PresentationAckMailbox>();
    auto events = std::make_shared<RecordingEventSink>();
    RenderAckRelay relay{mailbox, events};

    ASSERT_EQ(relay.tryPublishAcknowledgement(makeAcknowledgement(1U, 10)),
              platform::PresentationAckPushResult::Accepted);
    ASSERT_EQ(relay.tryPublishAcknowledgement(makeAcknowledgement(2U, 9)),
              platform::PresentationAckPushResult::Accepted);
    ASSERT_TRUE(events->waitForCriticalCount(2U, 1s));
    application::FrameSetPresented nextScope = makeAcknowledgement(3U, 40);
    nextScope.context.playback.playbackGeneration = domain::PlaybackGeneration{6U};
    ASSERT_EQ(relay.tryPublishAcknowledgement(nextScope),
              platform::PresentationAckPushResult::Accepted);
    ASSERT_TRUE(events->waitForCriticalCount(3U, 1s));

    const RenderAckRelayStatistics statistics = relay.statistics();
    EXPECT_EQ(statistics.canonicalFrameGaps, 0U);
    EXPECT_EQ(statistics.canonicalFrameRegressions, 1U);
    EXPECT_TRUE(relay.shutdown(2s));
}

TEST(RenderAckRelayTests, BackpressureQueuesOneRenderRetryAfterCriticalPostReturns) {
    auto mailbox = std::make_shared<platform::PresentationAckMailbox>();
    auto events = std::make_shared<BlockingEventSink>();
    RenderAckRelay relay{mailbox, events};
    QQuickItem item;
    relay.attach(&item);

    ASSERT_EQ(relay.tryPublishAcknowledgement(makeAcknowledgement(1U, 1)),
              platform::PresentationAckPushResult::Accepted);
    ASSERT_TRUE(events->waitUntilEntered(1s));

    ASSERT_EQ(relay.tryPublishAcknowledgement(makeAcknowledgement(2U, 2)),
              platform::PresentationAckPushResult::Accepted);
    ASSERT_EQ(relay.tryPublishAcknowledgement(makeAcknowledgement(3U, 3)),
              platform::PresentationAckPushResult::Accepted);
    ASSERT_EQ(relay.tryPublishAcknowledgement(makeAcknowledgement(4U, 4)),
              platform::PresentationAckPushResult::Accepted);
    ASSERT_EQ(relay.tryPublishAcknowledgement(makeAcknowledgement(5U, 5)),
              platform::PresentationAckPushResult::Accepted);
    EXPECT_EQ(relay.tryPublishAcknowledgement(makeAcknowledgement(6U, 6)),
              platform::PresentationAckPushResult::Full);

    events->release();
    ASSERT_TRUE(events->waitUntilReturned(1s));
    EXPECT_TRUE(waitUntil([&relay] { return relay.statistics().itemUpdates == 1U; }, 1s));
    // #2..#5 drain; the rejected #6 stays with the producer until the renderer retries it, so the
    // relay pops exactly five acknowledgements while queueing one coalesced render retry.
    ASSERT_TRUE(
        waitUntil([&relay] { return relay.statistics().acknowledgementsPopped == 5U; }, 1s));
    EXPECT_EQ(relay.statistics().ackBackpressureNotifications, 1U);
    EXPECT_EQ(relay.statistics().renderRetryRequests, 1U);
    EXPECT_EQ(relay.statistics().updateRequests, 1U);
    EXPECT_TRUE(relay.shutdown(2s));
}

TEST(RenderAckRelayTests, DetachTombstonesAlreadyQueuedAndFutureItemUpdates) {
    auto mailbox = std::make_shared<platform::PresentationAckMailbox>();
    auto events = std::make_shared<RecordingEventSink>();
    RenderAckRelay relay{mailbox, events};
    QQuickItem item;
    relay.attach(&item);

    relay.notifyFramePublished();
    relay.detach();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    EXPECT_EQ(relay.statistics().itemUpdates, 0U);

    const std::uint64_t queuedBefore = relay.statistics().queuedUpdates;
    relay.notifyFramePublished();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    EXPECT_EQ(relay.statistics().queuedUpdates, queuedBefore);
    EXPECT_EQ(relay.statistics().itemUpdates, 0U);
    EXPECT_TRUE(relay.shutdown(2s));
}

TEST(RenderAckRelayTests, ShutdownClosesAndDrainsAlreadyAdmittedAcknowledgements) {
    auto mailbox = std::make_shared<platform::PresentationAckMailbox>();
    auto events = std::make_shared<RecordingEventSink>();
    RenderAckRelay relay{mailbox, events};

    ASSERT_EQ(mailbox->tryPush(makeAcknowledgement(1U, 10)),
              platform::PresentationAckPushResult::Accepted);
    ASSERT_EQ(mailbox->tryPush(makeAcknowledgement(2U, 11)),
              platform::PresentationAckPushResult::Accepted);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_TRUE(relay.shutdown(2s));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
    EXPECT_TRUE(relay.isClosed());
    EXPECT_TRUE(mailbox->isClosed());
    EXPECT_TRUE(mailbox->isDrained());
    EXPECT_EQ(events->criticalEvents().size(), 2U);
    EXPECT_EQ(relay.statistics().acknowledgementsPopped, 2U);
    EXPECT_EQ(relay.tryPublishAcknowledgement(makeAcknowledgement(3U, 12)),
              platform::PresentationAckPushResult::Closed);
}

TEST(RenderAckRelayTests, TimedOutShutdownIsNotWaitedAgainByDestructor) {
    auto mailbox = std::make_shared<platform::PresentationAckMailbox>();
    auto events = std::make_shared<BlockingEventSink>();
    auto relay = std::make_unique<RenderAckRelay>(mailbox, events);
    ASSERT_EQ(relay->tryPublishAcknowledgement(makeAcknowledgement(1U, 1)),
              platform::PresentationAckPushResult::Accepted);
    ASSERT_TRUE(events->waitUntilEntered(1s));

    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(relay->shutdown(50ms));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);

    const auto destructionStarted = std::chrono::steady_clock::now();
    relay.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - destructionStarted, 250ms);

    events->release();
    EXPECT_TRUE(events->waitUntilReturned(1s));
}

} // namespace
} // namespace dvs::ui
