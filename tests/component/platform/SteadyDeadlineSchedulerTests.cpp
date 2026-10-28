#include "dvs/platform/SteadyDeadlineScheduler.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace dvs::platform {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] application::PlaybackRequestContext makeContext(const std::uint64_t requestId) {
    return application::PlaybackRequestContext{
        .request =
            application::RequestContext{
                .sessionId = domain::SessionId{17},
                .sessionEpoch = domain::SessionEpoch{3},
                .requestId = domain::RequestId{requestId},
            },
        .playbackGeneration = domain::PlaybackGeneration{8},
    };
}

[[nodiscard]] application::DeadlineRequest makeRequest(const std::uint64_t timerId,
                                                       const std::chrono::milliseconds delay) {
    return application::DeadlineRequest{
        .context = makeContext(timerId),
        .timerId = timerId,
        .due = std::chrono::steady_clock::now() + delay,
    };
}

class RecordingEventSink final : public application::IApplicationEventSink {
public:
    [[nodiscard]] application::EventPostResult
    postCritical(application::ApplicationEvent event) noexcept override {
        try {
            application::EventPostResult result = application::EventPostResult::Accepted;
            {
                std::scoped_lock lock(mutex_);
                ++postAttempts_;
                if (closed_) {
                    result = application::EventPostResult::Closed;
                } else if (const auto* const elapsed =
                               std::get_if<application::DeadlineElapsed>(&event)) {
                    deadlines_.push_back(*elapsed);
                }
            }
            condition_.notify_all();
            return result;
        } catch (...) {
            condition_.notify_all();
            return application::EventPostResult::Closed;
        }
    }

    [[nodiscard]] application::EventPostResult
    postRealtime(application::ApplicationEvent) noexcept override {
        return application::EventPostResult::Accepted;
    }

    void closeRealtimeIngress() noexcept override {}

    void closeCriticalIngress() noexcept override {
        {
            std::scoped_lock lock(mutex_);
            closed_ = true;
        }
        condition_.notify_all();
    }

    [[nodiscard]] bool waitForDeadlineCount(const std::size_t count,
                                            const std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(
            lock, timeout, [this, count] { return deadlines_.size() >= count; });
    }

    [[nodiscard]] bool waitForPostAttempts(const std::size_t count,
                                           const std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this, count] { return postAttempts_ >= count; });
    }

    [[nodiscard]] std::vector<application::DeadlineElapsed> deadlines() const {
        std::scoped_lock lock(mutex_);
        return deadlines_;
    }

    [[nodiscard]] std::size_t postAttempts() const {
        std::scoped_lock lock(mutex_);
        return postAttempts_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<application::DeadlineElapsed> deadlines_;
    std::size_t postAttempts_ = 0;
    bool closed_ = false;
};

class BlockingEventSink final : public application::IApplicationEventSink {
public:
    [[nodiscard]] application::EventPostResult
    postCritical(application::ApplicationEvent) noexcept override {
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
        return application::EventPostResult::Accepted;
    }

    [[nodiscard]] application::EventPostResult
    postRealtime(application::ApplicationEvent) noexcept override {
        return application::EventPostResult::Accepted;
    }

    void closeRealtimeIngress() noexcept override {}
    void closeCriticalIngress() noexcept override {}

    [[nodiscard]] bool waitUntilEntered() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 2s, [this] { return entered_; });
    }

    void release() {
        {
            std::scoped_lock lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
};

TEST(SteadyDeadlineSchedulerTests, PostsDueDeadlineWithoutMakingScheduleWait) {
    SteadyDeadlineScheduler scheduler;
    const auto events = std::make_shared<RecordingEventSink>();
    const application::DeadlineRequest request = makeRequest(41U, 40ms);

    const auto submittedAt = std::chrono::steady_clock::now();
    EXPECT_EQ(scheduler.schedule(request, events), application::PortSubmitResult::Accepted);
    EXPECT_LT(std::chrono::steady_clock::now() - submittedAt, 100ms);
    ASSERT_TRUE(events->waitForDeadlineCount(1U));

    const auto recorded = events->deadlines();
    ASSERT_EQ(recorded.size(), 1U);
    EXPECT_EQ(recorded.front().context, request.context);
    EXPECT_EQ(recorded.front().timerId, request.timerId);
}

TEST(SteadyDeadlineSchedulerTests, SuccessfulCancellationLinearizesBeforePosting) {
    SteadyDeadlineScheduler scheduler;
    const auto events = std::make_shared<RecordingEventSink>();
    const application::DeadlineRequest request = makeRequest(42U, 200ms);

    ASSERT_EQ(scheduler.schedule(request, events), application::PortSubmitResult::Accepted);
    EXPECT_TRUE(scheduler.cancel(request.timerId));
    std::this_thread::sleep_for(300ms);
    EXPECT_TRUE(events->deadlines().empty());
    EXPECT_FALSE(scheduler.cancel(request.timerId));
}

TEST(SteadyDeadlineSchedulerTests, ClaimedDeadlineCannotBeCanceledAndPostsAtMostOnce) {
    SteadyDeadlineScheduler scheduler;
    const auto events = std::make_shared<BlockingEventSink>();
    const application::DeadlineRequest request = makeRequest(43U, 0ms);

    ASSERT_EQ(scheduler.schedule(request, events), application::PortSubmitResult::Accepted);
    ASSERT_TRUE(events->waitUntilEntered());
    EXPECT_FALSE(scheduler.cancel(request.timerId));
    EXPECT_EQ(scheduler.schedule(makeRequest(request.timerId, 50ms), events),
              application::PortSubmitResult::Busy);
    events->release();
}

TEST(SteadyDeadlineSchedulerTests, DeliversEarlierDeadlineFirstAndReplacesDuplicateTimerId) {
    SteadyDeadlineScheduler scheduler;
    const auto events = std::make_shared<RecordingEventSink>();

    const application::DeadlineRequest later = makeRequest(44U, 300ms);
    const application::DeadlineRequest earlier = makeRequest(45U, 40ms);
    ASSERT_EQ(scheduler.schedule(later, events), application::PortSubmitResult::Accepted);
    ASSERT_EQ(scheduler.schedule(earlier, events), application::PortSubmitResult::Accepted);

    const application::DeadlineRequest replacement = makeRequest(44U, 80ms);
    ASSERT_EQ(scheduler.schedule(replacement, events), application::PortSubmitResult::Accepted);
    ASSERT_TRUE(events->waitForDeadlineCount(2U));
    std::this_thread::sleep_for(300ms);

    const auto recorded = events->deadlines();
    ASSERT_EQ(recorded.size(), 2U);
    EXPECT_EQ(recorded[0].timerId, earlier.timerId);
    EXPECT_EQ(recorded[1].timerId, replacement.timerId);
}

TEST(SteadyDeadlineSchedulerTests, DropsExpiredOrClosedSinksAndContinuesServingLiveSinks) {
    SteadyDeadlineScheduler scheduler;
    {
        auto expired = std::make_shared<RecordingEventSink>();
        ASSERT_EQ(scheduler.schedule(makeRequest(46U, 30ms), expired),
                  application::PortSubmitResult::Accepted);
    }

    const auto closed = std::make_shared<RecordingEventSink>();
    closed->closeCriticalIngress();
    ASSERT_EQ(scheduler.schedule(makeRequest(47U, 40ms), closed),
              application::PortSubmitResult::Accepted);
    ASSERT_TRUE(closed->waitForPostAttempts(1U));
    EXPECT_TRUE(closed->deadlines().empty());

    const auto live = std::make_shared<RecordingEventSink>();
    ASSERT_EQ(scheduler.schedule(makeRequest(48U, 30ms), live),
              application::PortSubmitResult::Accepted);
    EXPECT_TRUE(live->waitForDeadlineCount(1U));
}

TEST(SteadyDeadlineSchedulerTests, DestructorWakesWorkerAndDropsPendingDeadline) {
    const auto events = std::make_shared<RecordingEventSink>();
    const auto startedAt = std::chrono::steady_clock::now();
    {
        SteadyDeadlineScheduler scheduler;
        ASSERT_EQ(scheduler.schedule(makeRequest(49U, 5s), events),
                  application::PortSubmitResult::Accepted);
    }
    EXPECT_LT(std::chrono::steady_clock::now() - startedAt, 250ms);
    std::this_thread::sleep_for(100ms);
    EXPECT_TRUE(events->deadlines().empty());
}

TEST(SteadyDeadlineSchedulerTests, DispatchesShortDeadlinesWithoutSystemTimerQuantumDelay) {
    // The playback cadence arms deadlines only a few milliseconds ahead of their due time
    // (14 ms lead at 60 fps, half a frame at 120 fps). Waiting on the default system timer
    // quantum (~15.6 ms) would swallow that whole margin and make cadence systematically
    // late, so short deadlines must be dispatched near their due time instead.
    SteadyDeadlineScheduler scheduler;
    const auto events = std::make_shared<RecordingEventSink>();
    constexpr int kSamples = 12;
    std::vector<std::chrono::milliseconds> latenessSamples;
    latenessSamples.reserve(kSamples);
    for (int index = 0; index < kSamples; ++index) {
        const auto submittedAt = std::chrono::steady_clock::now();
        ASSERT_EQ(
            scheduler.schedule(makeRequest(static_cast<std::uint64_t>(60U + index), 4ms), events),
            application::PortSubmitResult::Accepted);
        ASSERT_TRUE(events->waitForDeadlineCount(static_cast<std::size_t>(index) + 1U, 2s));
        latenessSamples.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - submittedAt - 4ms));
    }
    std::sort(latenessSamples.begin(), latenessSamples.end());
    // Median: the old condition-variable wait spent ~half a quantum asleep past the deadline
    // (median ~8 ms over-sleep with a 4 ms deadline); the high-resolution wait stays in the
    // low single digits. The bound leaves scheduler and test-thread jitter headroom.
    const auto medianLateness = latenessSamples[latenessSamples.size() / 2U];
    EXPECT_LE(medianLateness.count(), 6);
    EXPECT_LE(latenessSamples.back().count(), 30);
}

} // namespace
} // namespace dvs::platform
