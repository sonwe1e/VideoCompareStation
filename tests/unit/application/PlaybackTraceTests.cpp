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

TEST(PlaybackTraceTests, SchemaV1KindsAreAppendOnlyAndRoundTripThroughTheBuffer) {
    static_assert(kSchemaV1MaxTraceEventKind == 22U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::CommandAccepted), 0U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::CommandRejected), 1U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::ProviderSubmitted), 2U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::ProviderCanceled), 3U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::FrameSetReady), 4U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::ProviderTerminal), 5U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::RenderPublished), 6U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::PresentationAcknowledged), 7U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::SnapshotCommitted), 8U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::CommandTerminal), 9U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::DecoderSeek), 10U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::DecoderReopen), 11U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::CacheHit), 12U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::DeviceGenerationChanged), 13U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::QmlGrabRequested), 14U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::QmlGrabCompleted), 15U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::RenderDrawStarted), 16U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::RenderAckPublished), 17U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::PlaybackRunStarted), 18U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::PlaybackRunStopped), 19U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::ReverseWindowBuilt), 20U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::ReverseWindowHit), 21U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::ReverseExactFallback), 22U);

    auto buffer = std::make_unique<PlaybackTraceBuffer>();
    RecordingTraceSink sink;
    buffer->setSink(&sink);

    for (std::uint8_t kind = 0U; kind <= kSchemaV1MaxTraceEventKind; ++kind) {
        const auto typed = static_cast<TraceEventKind>(kind);
        ASSERT_TRUE(buffer->record(TraceEvent{
            .identity = TraceIdentity{.request = domain::RequestId{kind}},
            .kind = typed,
            .timestampMicroseconds = kind,
            .payload = kind,
        }));
    }
    EXPECT_EQ(buffer->drainToSink(), static_cast<std::size_t>(kSchemaV1MaxTraceEventKind) + 1U);
    ASSERT_EQ(sink.events.size(), static_cast<std::size_t>(kSchemaV1MaxTraceEventKind) + 1U);
    for (std::uint8_t kind = 0U; kind <= kSchemaV1MaxTraceEventKind; ++kind) {
        EXPECT_EQ(sink.events[kind].kind, static_cast<TraceEventKind>(kind));
        EXPECT_EQ(sink.events[kind].payload, kind);
    }
}

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
