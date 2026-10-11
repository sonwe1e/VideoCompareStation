#include "dvs/ui/TimingDiagnosticsController.h"

#include <gtest/gtest.h>

namespace dvs::ui {
namespace {

application::TraceIdentity makeIdentity(const std::uint64_t runId = 1U,
                                        const std::uint64_t reqId = 1U) {
    application::TraceIdentity id;
    id.session = domain::SessionId{1U};
    id.epoch = domain::SessionEpoch{1U};
    id.generation = domain::PlaybackGeneration{1U};
    id.run = runId;
    id.request = domain::RequestId{reqId};
    return id;
}

application::TraceEvent makeEvent(const application::TraceEventKind kind,
                                  const std::uint64_t timestampMicros,
                                  const std::uint64_t payload = 0U,
                                  const std::uint64_t runId = 1U,
                                  const std::uint64_t reqId = 1U) {
    return application::TraceEvent{
        .identity = makeIdentity(runId, reqId),
        .kind = kind,
        .timestampMicroseconds = timestampMicros,
        .payload = payload,
        .incoming = std::nullopt,
    };
}

} // namespace

TEST(TimingDiagnosticsControllerTests, InitialStateWithoutRunsReportsZeroLatencies) {
    TimingDiagnosticsController controller;
    controller.setMonitoring(false);
    controller.refresh();

    EXPECT_FALSE(controller.hasActivePlayback());
    EXPECT_EQ(controller.runId(), 0U);
    EXPECT_EQ(controller.skippedFrameSetCount(), 0U);
    EXPECT_DOUBLE_EQ(controller.decodeLatencyP95Ms(), 0.0);
    EXPECT_DOUBLE_EQ(controller.decodeLatencyMaxMs(), 0.0);
    EXPECT_DOUBLE_EQ(controller.renderScheduleP95Ms(), 0.0);
    EXPECT_DOUBLE_EQ(controller.drawToAckP95Ms(), 0.0);
    EXPECT_FALSE(controller.systemPresentationStatus().isEmpty());
    EXPECT_FALSE(controller.driverKind().isEmpty());
}

TEST(TimingDiagnosticsControllerTests, IngestingPlaybackTraceEventsUpdatesLatenciesAndSkips) {
    TimingDiagnosticsController controller;
    controller.setMonitoring(false);

    // Open run
    controller.ingestTraceEvent(
        makeEvent(application::TraceEventKind::PlaybackRunStarted, 10'000U, 10U, 1U, 1U));

    // Decode stage: ProviderSubmitted (10'000) -> SourceDecodeCompleted (34'000) => 24 ms
    controller.ingestTraceEvent(
        makeEvent(application::TraceEventKind::ProviderSubmitted, 10'000U, 10U, 1U, 1U));
    controller.ingestTraceEvent(
        makeEvent(application::TraceEventKind::SourceDecodeCompleted, 34'000U, 10U, 1U, 1U));

    // Publication: frame 10 published
    controller.ingestTraceEvent(
        makeEvent(application::TraceEventKind::RenderPublished, 40'000U, 10U, 1U, 1U));

    // Gap: frame 12 published (skipping canonical frame 11)
    controller.ingestTraceEvent(
        makeEvent(application::TraceEventKind::RenderPublished, 56'000U, 12U, 1U, 2U));

    // Render draw and ACK: RenderPublished(56'000) -> RenderDrawStarted(58'000) => 2 ms
    // RenderDrawStarted(58'000) -> RenderAckPublished(62'000) => 4 ms
    // RenderPublished(56'000) -> PresentationAcknowledged(64'000) => 8 ms
    controller.ingestTraceEvent(
        makeEvent(application::TraceEventKind::RenderDrawStarted, 58'000U, 12U, 1U, 2U));
    controller.ingestTraceEvent(
        makeEvent(application::TraceEventKind::RenderAckPublished, 62'000U, 12U, 1U, 2U));
    controller.ingestTraceEvent(
        makeEvent(application::TraceEventKind::PresentationAcknowledged, 64'000U, 12U, 1U, 2U));

    EXPECT_TRUE(controller.hasActivePlayback());
    EXPECT_EQ(controller.runId(), 1U);
    EXPECT_EQ(controller.skippedFrameSetCount(), 1U);
    EXPECT_NEAR(controller.decodeLatencyP95Ms(), 24.0, 0.1);
    EXPECT_NEAR(controller.decodeLatencyMaxMs(), 24.0, 0.1);
    EXPECT_NEAR(controller.renderScheduleP95Ms(), 2.0, 0.1);
    EXPECT_NEAR(controller.drawToAckP95Ms(), 4.0, 0.1);
    EXPECT_NEAR(controller.renderToAckP95Ms(), 8.0, 0.1);

    // Reset clears aggregated evidence
    controller.reset();
    EXPECT_FALSE(controller.hasActivePlayback());
    EXPECT_EQ(controller.skippedFrameSetCount(), 0U);
    EXPECT_DOUBLE_EQ(controller.decodeLatencyP95Ms(), 0.0);
}

TEST(TimingDiagnosticsControllerTests, PassiveObserverReceivesEventsFromPlaybackTraceSingleton) {
    application::PlaybackTrace& trace = application::PlaybackTrace::instance();
    trace.reset();

    TimingDiagnosticsController controller;
    controller.setMonitoring(false);

    // Emitting through global PlaybackTrace should reach controller via observer
    const auto id = makeIdentity(1U, 1U);
    trace.record(application::TraceEventKind::PlaybackRunStarted, id, 1U);
    trace.record(application::TraceEventKind::ProviderSubmitted, id, 1U);
    trace.record(application::TraceEventKind::SourceDecodeCompleted, id, 1U);
    trace.record(application::TraceEventKind::PlaybackRunStopped, id, 1U);

    controller.refresh();
    EXPECT_FALSE(controller.hasActivePlayback());
    EXPECT_EQ(controller.runId(), 1U);

    trace.reset();
}

} // namespace dvs::ui
