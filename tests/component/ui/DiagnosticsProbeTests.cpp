#include "dvs/application/PlaybackTrace.h"
#include "dvs/ui/DiagnosticsProbe.h"

#include <QString>

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <vector>

namespace dvs::ui {
namespace {

using application::PlaybackTrace;
using application::TraceEvent;
using application::TraceEventKind;

[[nodiscard]] std::uint64_t fixedTraceClock() noexcept {
    return 4242U;
}

class RecordingTraceSink final : public application::ITraceSink {
public:
    [[nodiscard]] bool append(const TraceEvent& event) noexcept override {
        events.push_back(event);
        return true;
    }

    [[nodiscard]] bool recordOverflow(const std::uint64_t) noexcept override {
        return true;
    }

    [[nodiscard]] bool finalize() noexcept override {
        return true;
    }

    std::vector<TraceEvent> events;
};

class DiagnosticsProbeTests : public ::testing::Test {
protected:
    void SetUp() override {
        PlaybackTrace::instance().reset();
    }

    void TearDown() override {
        PlaybackTrace::instance().disable();
        PlaybackTrace::instance().installSink(nullptr);
        PlaybackTrace::instance().reset();
    }

    // Enables tracing with a test sink installed so a probe call can be observed without touching
    // the real export path.
    void enableTracing(RecordingTraceSink& sink) {
        PlaybackTrace::instance().installSink(&sink);
        PlaybackTrace::instance().enable(fixedTraceClock);
        ASSERT_TRUE(PlaybackTrace::instance().enabled());
    }

    [[nodiscard]] static std::vector<TraceEvent> drainEvents() {
        std::vector<TraceEvent> events(8U);
        const std::size_t count = PlaybackTrace::instance().drain(events.data(), events.size());
        events.resize(count);
        return events;
    }
};

// The bridge exists so an evidence gate can correlate a UI scene-graph grab with playback timing;
// the kinds are append-only schema values, so their numeric assignment must not drift. This is a
// plain TEST (the suite below is fixture-based, and GoogleTest forbids mixing the two in one
// suite).
TEST(DiagnosticsProbeTraceKindTests, GrabStagesMapToTheDocumentedTraceKinds) {
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::QmlGrabRequested), 14U);
    EXPECT_EQ(static_cast<std::uint8_t>(TraceEventKind::QmlGrabCompleted), 15U);
}

TEST_F(DiagnosticsProbeTests, RecordsGrabRequestAndCompletionWithTheFramePayload) {
    RecordingTraceSink sink;
    enableTracing(sink);

    DiagnosticsProbe probe;
    probe.record(QStringLiteral("grab-requested"), 96.0);
    probe.record(QStringLiteral("grab-completed"), 96.0);

    const std::vector<TraceEvent> events = drainEvents();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].kind, TraceEventKind::QmlGrabRequested);
    EXPECT_EQ(events[0].payload, 96U);
    EXPECT_EQ(events[1].kind, TraceEventKind::QmlGrabCompleted);
    EXPECT_EQ(events[1].payload, 96U);
    // A UI observation carries no session/playback identity; analyzers correlate it by timestamp.
    EXPECT_EQ(events[0].identity.session.value(), 0U);
    EXPECT_EQ(events[0].identity.generation.value(), 0U);
    EXPECT_FALSE(events[0].incoming.has_value());

    // The bridge only enqueues: the bounded buffer is the producer side, and the export thread
    // forwards drained events to the sink. Recording must never touch the sink itself.
    EXPECT_TRUE(sink.events.empty());
    EXPECT_EQ(PlaybackTrace::instance().drainToSink(), 0U);
}

TEST_F(DiagnosticsProbeTests, EncodesAnUnknownFrameAsTheDocumentedSentinel) {
    RecordingTraceSink sink;
    enableTracing(sink);

    DiagnosticsProbe probe;
    probe.record(QStringLiteral("grab-requested"));

    const std::vector<TraceEvent> events = drainEvents();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].payload, (std::numeric_limits<std::uint64_t>::max)());
}

TEST_F(DiagnosticsProbeTests, IgnoresAnUnrecognizedStage) {
    RecordingTraceSink sink;
    enableTracing(sink);

    DiagnosticsProbe probe;
    probe.record(QStringLiteral("not-a-stage"), 1.0);

    EXPECT_TRUE(drainEvents().empty());
}

// With tracing disabled (the default production state) the bridge must stay a no-op so the
// playback path pays no cost.
TEST_F(DiagnosticsProbeTests, IsANoOpWhenTracingIsDisabled) {
    RecordingTraceSink sink;
    PlaybackTrace::instance().installSink(&sink);
    PlaybackTrace::instance().disable();

    DiagnosticsProbe probe;
    probe.record(QStringLiteral("grab-requested"), 5.0);
    probe.record(QStringLiteral("grab-completed"), 5.0);

    EXPECT_TRUE(drainEvents().empty());
    EXPECT_TRUE(sink.events.empty());
}

// Stage names are a fixed vocabulary: a near-miss must not silently become a recorded event.
TEST_F(DiagnosticsProbeTests, RequiresAnExactStageName) {
    RecordingTraceSink sink;
    enableTracing(sink);

    DiagnosticsProbe probe;
    probe.record(QStringLiteral("grab-request"), 3.0);
    probe.record(QStringLiteral("Grab-Requested"), 3.0);

    EXPECT_TRUE(drainEvents().empty());
}

} // namespace
} // namespace dvs::ui
