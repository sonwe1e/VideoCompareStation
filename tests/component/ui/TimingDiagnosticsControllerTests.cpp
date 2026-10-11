#include "dvs/ui/TimingDiagnosticsController.h"

#include "PresentationStatisticsProbeTestHooks.h"

#include <chrono>
#include <gtest/gtest.h>
#include <thread>

namespace dvs::ui {
namespace {

[[nodiscard]] platform::testing::PresentationStatisticsProbeOverrides failingWindowOverrides() {
    platform::testing::PresentationStatisticsProbeOverrides overrides;
    overrides.failWindowCreation = true;
    return overrides;
}

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

    controller.setMonitoring(false);
    trace.reset();
}

// E01: construction sits on the startup critical path and must not run the presentation
// capability probe. The panel starts from the explicit unprobed state.
TEST(TimingDiagnosticsControllerTests, ConstructionStaysLightAndDefersCapabilityProbe) {
    TimingDiagnosticsController controller;
    controller.setMonitoring(false);

    EXPECT_EQ(controller.systemPresentationStatus(), QStringLiteral("未确认 (未探测)"));
    EXPECT_EQ(controller.driverKind(), QStringLiteral("未知"));
    EXPECT_FALSE(controller.isIndirectDisplayWarning());
    EXPECT_TRUE(controller.presentationDetail().isEmpty());
    EXPECT_EQ(controller.appliedProbeGeneration(), 0U);
    EXPECT_EQ(controller.presentCount(), 0U);
    EXPECT_EQ(controller.presentRefreshCount(), 0U);
}

// E01: the requested probe runs on the controller's worker thread and its graded answer is
// applied asynchronously. The injected window failure keeps the platform answer deterministic.
TEST(TimingDiagnosticsControllerTests, RequestedProbeAppliesItsGenerationResultAsynchronously) {
    TimingDiagnosticsController controller;
    controller.setMonitoring(false);

    {
        const platform::testing::ScopedPresentationStatisticsProbeOverride overrides{
            failingWindowOverrides()};
        controller.requestPresentationProbe();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline &&
               controller.appliedProbeGeneration() == 0U) {
            controller.refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }
    EXPECT_EQ(controller.appliedProbeGeneration(), 1U);
    EXPECT_EQ(controller.systemPresentationStatus(), QStringLiteral("窗口不可用"));
    EXPECT_TRUE(controller.presentationDetail().contains(QStringLiteral("CreateWindowExW")));

    // A completed capability stays the cached answer; repeated requests are free.
    controller.requestPresentationProbe();
    EXPECT_EQ(controller.appliedProbeGeneration(), 1U);
}

// E01: a capability answer whose generation was superseded by a presentation-target handover is
// dropped, never applied to the new target; the panel stays unprobed until a fresh request lands.
TEST(TimingDiagnosticsControllerTests, SupersededProbeResultNeverOverwritesNewTarget) {
    TimingDiagnosticsController controller;
    controller.setMonitoring(false);

    {
        const platform::testing::ScopedPresentationStatisticsProbeOverride overrides{
            failingWindowOverrides()};
        controller.requestPresentationProbe();
        // Handover before the first answer arrives: the in-flight result becomes stale.
        controller.notePresentationTargetReplaced();
        // The failing probe answers within milliseconds; this wait only samples the state after
        // the stale answer has had every chance to arrive. Even a late arrival is dropped below.
        std::this_thread::sleep_for(std::chrono::milliseconds{250});
        controller.refresh();
        EXPECT_EQ(controller.appliedProbeGeneration(), 0U);
        EXPECT_EQ(controller.systemPresentationStatus(), QStringLiteral("未确认 (未探测)"));

        // A fresh request for the new target completes and applies under its own generation.
        // Generations: first request = 1, handover bump = 2, second request = 3.
        controller.requestPresentationProbe();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline &&
               controller.appliedProbeGeneration() == 0U) {
            controller.refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }
    EXPECT_EQ(controller.appliedProbeGeneration(), 3U);
    EXPECT_EQ(controller.systemPresentationStatus(), QStringLiteral("窗口不可用"));
}

} // namespace dvs::ui
