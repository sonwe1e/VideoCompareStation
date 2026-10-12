#include "dvs/ui/TimingDiagnosticsController.h"

#include "PresentationStatisticsProbeTestHooks.h"

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace dvs::ui {
namespace {

// Minimal recording sink for the E02 coexistence test: the monitoring gate must pause only the
// diagnostics observer, never a sink installed on the same global trace.
class RecordingTraceSink final : public application::ITraceSink {
public:
    [[nodiscard]] bool append(const application::TraceEvent& event) noexcept override {
        try {
            events.push_back(event);
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool recordOverflow(const std::uint64_t) noexcept override {
        return true;
    }

    [[nodiscard]] bool finalize() noexcept override {
        return true;
    }

    std::vector<application::TraceEvent> events;
};

// Counting observer for lifecycle tests: proves whether the trace still dispatches to an
// observer that a destroyed controller must not have detached.
class RecordingTraceObserver final : public application::ITraceObserver {
public:
    void onTraceEvent(const application::TraceEvent&) noexcept override {
        count += 1U;
    }

    std::size_t count = 0U;
};

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
    // E02: monitoring defaults off, so the singleton path only reaches the collector after the
    // diagnostics surface turned it on — exactly the production wiring this test mirrors.
    controller.setMonitoring(true);

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

// E02: after setMonitoring(false), events recorded through the global trace stop reaching the
// collector; turning monitoring back on resumes ingest.
TEST(TimingDiagnosticsControllerTests, MonitoringOffStopsNewDiagnosticIngest) {
    application::PlaybackTrace& trace = application::PlaybackTrace::instance();
    trace.reset();

    TimingDiagnosticsController controller;
    EXPECT_FALSE(controller.isMonitoring());
    controller.setMonitoring(true);
    EXPECT_TRUE(trace.enabled());

    const auto id = makeIdentity(1U, 1U);
    trace.record(application::TraceEventKind::PlaybackRunStarted, id, 10U);
    controller.refresh();
    EXPECT_EQ(controller.runId(), 1U);

    controller.setMonitoring(false);
    trace.record(application::TraceEventKind::PlaybackRunStarted, id, 20U);
    trace.record(application::TraceEventKind::ProviderSubmitted, id, 21U);
    controller.refresh();
    // The second run boundary never ingested: the open run is still run 1.
    EXPECT_EQ(controller.runId(), 1U);
    EXPECT_TRUE(controller.hasActivePlayback());

    // Re-enabling resumes ingest; the stopped boundary closes run 1 without opening run 2.
    controller.setMonitoring(true);
    trace.record(application::TraceEventKind::PlaybackRunStopped, id, 30U);
    controller.refresh();
    EXPECT_EQ(controller.runId(), 1U);
    EXPECT_FALSE(controller.hasActivePlayback());

    controller.setMonitoring(false);
    trace.reset();
}

// E02: the in-callback monitoring check is independent of the shared PlaybackTrace gate. Even
// when the process-global gate is opened again by someone else while this controller's
// monitoring stays off, no event may reach the collector.
TEST(TimingDiagnosticsControllerTests, MonitoringOffStillRejectsEventsWhenGateReopens) {
    application::PlaybackTrace& trace = application::PlaybackTrace::instance();
    trace.reset();

    TimingDiagnosticsController controller;
    controller.setMonitoring(true);
    const auto id = makeIdentity(1U, 1U);
    trace.record(application::TraceEventKind::PlaybackRunStarted, id, 10U);
    controller.refresh();
    EXPECT_EQ(controller.runId(), 1U);

    controller.setMonitoring(false);
    // Another trace user reopens the shared gate; this controller's monitoring stays off.
    trace.setObserverEnabled(true);
    trace.record(application::TraceEventKind::PlaybackRunStarted, id, 20U);
    trace.record(application::TraceEventKind::PlaybackRunStopped, id, 21U);
    controller.refresh();
    EXPECT_EQ(controller.runId(), 1U);
    EXPECT_TRUE(controller.hasActivePlayback());

    trace.setObserverEnabled(false);
    trace.reset();
}

// E02: the monitoring gate filters only the diagnostics observer. A concurrently installed
// file sink keeps receiving every event, and the ring buffer keeps them drainable.
TEST(TimingDiagnosticsControllerTests, ObserverGateDoesNotTouchSinks) {
    application::PlaybackTrace& trace = application::PlaybackTrace::instance();
    trace.reset();

    RecordingTraceSink sink;
    trace.installSink(&sink);
    trace.enable(application::traceNowMicroseconds);

    TimingDiagnosticsController controller;
    controller.setMonitoring(true);
    const auto id = makeIdentity(1U, 1U);
    trace.record(application::TraceEventKind::PlaybackRunStarted, id, 10U);
    EXPECT_EQ(trace.drainToSink(), 1U);
    ASSERT_EQ(sink.events.size(), 1U);

    controller.setMonitoring(false);
    trace.record(application::TraceEventKind::ProviderSubmitted, id, 20U);
    EXPECT_EQ(trace.drainToSink(), 1U);
    EXPECT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events.back().kind, application::TraceEventKind::ProviderSubmitted);
    controller.refresh();
    EXPECT_EQ(controller.runId(), 1U);

    controller.setMonitoring(false);
    trace.reset();
}

// E02: a stopped panel must not leave the per-event work it added running for the rest of the
// process. A controller that enabled the trace clock restores the default-off clock on stop
// (when no sink shares it) and re-arms it on the next activation; a shared clock is left alone.
TEST(TimingDiagnosticsControllerTests, MonitoringOffRestoresTheClockItOwnedAndReArmsItOnReturn) {
    application::PlaybackTrace& trace = application::PlaybackTrace::instance();
    trace.reset();
    EXPECT_FALSE(trace.enabled());

    TimingDiagnosticsController controller;
    controller.setMonitoring(true);
    EXPECT_TRUE(trace.enabled());

    controller.setMonitoring(false);
    EXPECT_FALSE(trace.enabled());
    controller.setMonitoring(true);
    EXPECT_TRUE(trace.enabled());
    controller.setMonitoring(false);
    EXPECT_FALSE(trace.enabled());

    // A sink shares the clock: the observer owner must not disable (or re-enable) it then.
    RecordingTraceSink sink;
    trace.installSink(&sink);
    trace.enable(application::traceNowMicroseconds);
    controller.setMonitoring(true);
    controller.setMonitoring(false);
    EXPECT_TRUE(trace.enabled());
    controller.setMonitoring(true);
    controller.setMonitoring(false);
    EXPECT_TRUE(trace.enabled());
    trace.installSink(nullptr);
    trace.reset();
}

// E02: destroying a controller that never monitored must not detach a foreign observer or touch
// the process-global clock, and destroying a bystander while another controller monitors must
// leave that controller's registration intact (ownership-checked removal).
TEST(TimingDiagnosticsControllerTests, DestructionLeavesForeignRegistrationAndClockAlone) {
    application::PlaybackTrace& trace = application::PlaybackTrace::instance();
    trace.reset();
    EXPECT_FALSE(trace.enabled());

    RecordingTraceObserver foreignObserver;
    trace.installObserver(&foreignObserver);
    EXPECT_TRUE(trace.enabled());
    {
        TimingDiagnosticsController bystander;
    }
    trace.record(application::TraceEventKind::PlaybackRunStarted, makeIdentity(1U, 1U), 0U);
    EXPECT_EQ(foreignObserver.count, 1U);
    trace.removeObserver(&foreignObserver);
    trace.reset();

    // The monitoring controller survives a bystander's destruction: its observer still ingests.
    TimingDiagnosticsController monitor;
    monitor.setMonitoring(true);
    {
        TimingDiagnosticsController bystander;
    }
    trace.record(application::TraceEventKind::PlaybackRunStarted, makeIdentity(1U, 1U), 10U);
    monitor.refresh();
    EXPECT_EQ(monitor.runId(), 1U);
    EXPECT_TRUE(monitor.hasActivePlayback());
    trace.reset();
}

// E02: toggling monitoring while a producer thread keeps emitting, and destroying the controller
// mid-stream, must neither crash nor leak ingest past the final stop. Exercises the observer
// gate, the in-callback monitoring check, and the destructor's lifecycle fence together.
TEST(TimingDiagnosticsControllerTests, ConcurrentMonitoringTogglingAndDestructionStaySafe) {
    application::PlaybackTrace& trace = application::PlaybackTrace::instance();
    trace.reset();

    std::atomic<bool> producerRunning{true};
    std::thread producer([&trace, &producerRunning] {
        const auto id = makeIdentity(1U, 1U);
        while (producerRunning.load(std::memory_order_relaxed)) {
            trace.record(application::TraceEventKind::ProviderSubmitted, id, 0U);
            std::this_thread::yield();
        }
    });

    {
        TimingDiagnosticsController controller;
        controller.setMonitoring(true);
        for (int cycle = 0; cycle < 100; ++cycle) {
            controller.setMonitoring(cycle % 2 == 0);
            std::this_thread::yield();
        }
        controller.setMonitoring(false);
        // Let every dispatch that passed the gate before the close land and drain it.
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        controller.refresh();
        const double latencyBefore = controller.decodeLatencyP95Ms();
        const quint64 runIdBefore = controller.runId();
        // A boundary recorded after the final stop must not change any aggregated state.
        trace.record(application::TraceEventKind::PlaybackRunStarted, makeIdentity(7U, 1U), 0U);
        controller.refresh();
        EXPECT_EQ(controller.runId(), runIdBefore);
        EXPECT_DOUBLE_EQ(controller.decodeLatencyP95Ms(), latencyBefore);
    }
    // Destroyed with monitoring still ON while the producer keeps emitting: the destructor's
    // lifecycle fence is the only thing standing between in-flight/continued dispatches and a
    // destructed observer. The gate closes first, the uninstall waits out any callback already
    // inside, and after it returns the freed object is unreachable from the trace.
    {
        TimingDiagnosticsController controller;
        controller.setMonitoring(true);
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }

    producerRunning.store(false, std::memory_order_relaxed);
    producer.join();
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
