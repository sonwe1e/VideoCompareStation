#include "dvs/application/PlaybackTrace.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dvs::application {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] TraceEvent traceEvent(const std::uint64_t payload) {
    return TraceEvent{
        .identity = TraceIdentity{.request = domain::RequestId{payload}},
        .kind = TraceEventKind::CacheHit,
        .timestampMicroseconds = payload,
        .payload = payload,
    };
}

class RecordingTraceSink final : public ITraceSink {
public:
    [[nodiscard]] bool append(const TraceEvent& event) noexcept override {
        try {
            events.push_back(event);
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool recordOverflow(const std::uint64_t lostCount) noexcept override {
        overflow += lostCount;
        return true;
    }

    [[nodiscard]] bool finalize() noexcept override {
        return true;
    }

    std::vector<TraceEvent> events;
    std::uint64_t overflow = 0U;
};

class BlockingTraceSink final : public ITraceSink {
public:
    [[nodiscard]] bool append(const TraceEvent&) noexcept override {
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
        return true;
    }

    [[nodiscard]] bool recordOverflow(std::uint64_t) noexcept override {
        return true;
    }

    [[nodiscard]] bool finalize() noexcept override {
        return true;
    }

    [[nodiscard]] bool waitUntilEntered(const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return entered_; });
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

class FailingTraceSink final : public ITraceSink {
public:
    [[nodiscard]] bool append(const TraceEvent&) noexcept override {
        return false;
    }

    [[nodiscard]] bool recordOverflow(const std::uint64_t lostCount) noexcept override {
        overflow += lostCount;
        return true;
    }

    [[nodiscard]] bool finalize() noexcept override {
        return true;
    }

    std::uint64_t overflow = 0U;
};

[[nodiscard]] std::uint64_t fixedTraceTime() noexcept {
    return 42U;
}

class PlaybackTraceSingletonTests : public ::testing::Test {
protected:
    void SetUp() override {
        PlaybackTrace::instance().reset();
    }

    void TearDown() override {
        PlaybackTrace& trace = PlaybackTrace::instance();
        trace.disable();
        trace.reset();
    }

    RecordingTraceSink sink;
};

} // namespace

TEST(PlaybackTraceTests, BoundsTheQueueAndReportsEachOverflowOnlyOnce) {
    auto buffer = std::make_unique<PlaybackTraceBuffer>();
    RecordingTraceSink sink;
    buffer->setSink(&sink);

    for (std::size_t index = 0U; index < PlaybackTraceBuffer::kCapacity; ++index) {
        ASSERT_TRUE(buffer->record(traceEvent(index)));
    }
    EXPECT_FALSE(buffer->record(traceEvent(100'001U)));
    EXPECT_FALSE(buffer->record(traceEvent(100'002U)));
    EXPECT_EQ(buffer->overflowCount(), 2U);

    EXPECT_EQ(buffer->drainToSink(), PlaybackTraceBuffer::kCapacity);
    EXPECT_EQ(sink.events.size(), PlaybackTraceBuffer::kCapacity);
    EXPECT_EQ(sink.overflow, 2U);

    EXPECT_EQ(buffer->drainToSink(), 0U);
    EXPECT_EQ(sink.overflow, 2U);

    ASSERT_TRUE(buffer->record(traceEvent(100'003U)));
    EXPECT_EQ(buffer->drainToSink(), 1U);
    EXPECT_EQ(sink.events.back().payload, 100'003U);
    EXPECT_EQ(sink.overflow, 2U);
}

TEST(PlaybackTraceTests, SinkIoDoesNotHoldTheProducerQueueLock) {
    auto buffer = std::make_unique<PlaybackTraceBuffer>();
    BlockingTraceSink sink;
    buffer->setSink(&sink);
    ASSERT_TRUE(buffer->record(traceEvent(1U)));

    std::thread consumer{[&buffer] { static_cast<void>(buffer->drainToSink()); }};
    if (!sink.waitUntilEntered(1s)) {
        sink.release();
        consumer.join();
        FAIL() << "The trace sink was not entered before the test deadline.";
    }

    std::future<bool> producer =
        std::async(std::launch::async, [&buffer] { return buffer->record(traceEvent(2U)); });
    const std::future_status status = producer.wait_for(1s);
    sink.release();

    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_TRUE(producer.get());
    consumer.join();
}

TEST(PlaybackTraceTests, RemovingSinkWaitsForAnInProgressDrain) {
    auto buffer = std::make_unique<PlaybackTraceBuffer>();
    BlockingTraceSink sink;
    buffer->setSink(&sink);
    ASSERT_TRUE(buffer->record(traceEvent(1U)));

    std::thread consumer{[&buffer] { static_cast<void>(buffer->drainToSink()); }};
    if (!sink.waitUntilEntered(1s)) {
        sink.release();
        consumer.join();
        FAIL() << "The trace sink was not entered before the test deadline.";
    }

    std::future<void> removal =
        std::async(std::launch::async, [&buffer] { buffer->setSink(nullptr); });
    EXPECT_EQ(removal.wait_for(50ms), std::future_status::timeout);

    sink.release();
    consumer.join();
    EXPECT_EQ(removal.wait_for(1s), std::future_status::ready);
    removal.get();
}

TEST(PlaybackTraceTests, SinkFailureMarksTheDequeuedBatchAsLost) {
    auto buffer = std::make_unique<PlaybackTraceBuffer>();
    FailingTraceSink sink;
    buffer->setSink(&sink);
    ASSERT_TRUE(buffer->record(traceEvent(1U)));
    ASSERT_TRUE(buffer->record(traceEvent(2U)));

    EXPECT_EQ(buffer->drainToSink(), 2U);
    EXPECT_EQ(buffer->overflowCount(), 2U);
    EXPECT_EQ(sink.overflow, 2U);
}

TEST_F(PlaybackTraceSingletonTests,
       DisabledGlobalTraceSkipsRecordsAndUsesTheInstalledClockWhenEnabled) {
    PlaybackTrace& trace = PlaybackTrace::instance();
    trace.installSink(&sink);

    trace.record(TraceEventKind::CacheHit, TraceIdentity{}, 1U);
    EXPECT_EQ(trace.drainToSink(), 0U);

    trace.enable(fixedTraceTime);
    EXPECT_TRUE(trace.enabled());
    trace.record(TraceEventKind::CacheHit, TraceIdentity{}, 2U);
    trace.disable();
    EXPECT_FALSE(trace.enabled());
    EXPECT_EQ(trace.drainToSink(), 1U);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events.front().timestampMicroseconds, 42U);
    EXPECT_EQ(sink.events.front().payload, 2U);
}

} // namespace dvs::application
