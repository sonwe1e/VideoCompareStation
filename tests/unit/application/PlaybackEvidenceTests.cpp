#include "dvs/application/PlaybackEvidence.h"

#include <atomic>
#include <cstdint>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace dvs::application {
namespace {

[[nodiscard]] TraceIdentity
coordinatorScope(const std::uint64_t session, const std::uint64_t epoch, const std::uint64_t run) {
    return TraceIdentity{
        .session = domain::SessionId{session},
        .epoch = domain::SessionEpoch{epoch},
        .generation = domain::PlaybackGeneration{1U},
        .request = domain::RequestId{0U},
        .run = run,
    };
}

// The provider emission carries the request context but no playback-run id (the provider does
// not know it), so its scope is session/epoch only - mirroring MultiSourceFrameProvider.
[[nodiscard]] TraceIdentity providerScope(const std::uint64_t session, const std::uint64_t epoch) {
    return TraceIdentity{
        .session = domain::SessionId{session},
        .epoch = domain::SessionEpoch{epoch},
        .generation = domain::PlaybackGeneration{1U},
        .request = domain::RequestId{17U},
    };
}

// Decode-actor events key their identity request by source id and carry no run.
[[nodiscard]] TraceIdentity
decodeScope(const std::uint64_t session, const std::uint64_t epoch, const std::uint64_t sourceId) {
    return TraceIdentity{
        .session = domain::SessionId{session},
        .epoch = domain::SessionEpoch{epoch},
        .generation = domain::PlaybackGeneration{1U},
        .request = domain::RequestId{sourceId},
    };
}

[[nodiscard]] TraceEvent traceEvent(const TraceEventKind kind,
                                    const std::uint64_t timestampMicroseconds,
                                    const std::uint64_t payload,
                                    const TraceIdentity& identity) {
    return TraceEvent{
        .identity = identity,
        .kind = kind,
        .timestampMicroseconds = timestampMicroseconds,
        .payload = payload,
    };
}

[[nodiscard]] const PlaybackLatencyStats& stageStats(const PlaybackRunEvidence& run,
                                                     const PlaybackEvidenceStage stage) {
    return run.stages[static_cast<std::size_t>(stage)];
}

TEST(PlaybackEvidenceTests, EmptySummaryReportsNoRunsAndLevelsWithoutFabricatedZeros) {
    PlaybackEvidenceCollector collector;
    const PlaybackEvidenceSummary summary = collector.summary();

    EXPECT_TRUE(summary.runs.empty());
    EXPECT_EQ(summary.droppedRunCount, 0U);
    EXPECT_EQ(summary.cumulativeStaleEventCount, 0U);
    EXPECT_EQ(summary.cumulativeNonPlaybackWindowEventCount, 0U);
    // Source PTS timing is not in the trace schema; the physical screen present is not
    // observable through the application records. Both are statuses, never zero measurements.
    EXPECT_EQ(summary.sourceTimingStatus, PlaybackEvidenceFieldStatus::kUnavailable);
    EXPECT_EQ(summary.systemPresentationStatus, PlaybackEvidenceFieldStatus::kUnknown);
}

TEST(PlaybackEvidenceTests, LocatesAnInjectedDecodeDelayInTheSubmitStageOnly) {
    PlaybackEvidenceCollector collector;
    constexpr std::uint64_t kInjectedDecodeDelay = 250U;

    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStarted, 0U, 0U, coordinatorScope(1, 1, 1)));
    for (std::uint64_t frame = 0U; frame < 10U; ++frame) {
        const std::uint64_t base = 1'000U + frame * 10U;
        collector.ingest(
            traceEvent(TraceEventKind::ProviderSubmitted, base, 0U, providerScope(1, 1)));
        collector.ingest(traceEvent(TraceEventKind::SourceDecodeCompleted,
                                    base + kInjectedDecodeDelay,
                                    500U + frame,
                                    decodeScope(1, 1, 7U)));
        // Every later hop closes with zero elapsed time, so any latency leaking into another
        // stage would show up as a non-zero max there.
        collector.ingest(traceEvent(TraceEventKind::FrameSetReady,
                                    base + kInjectedDecodeDelay,
                                    100U + frame,
                                    coordinatorScope(1, 1, 1)));
        collector.ingest(traceEvent(TraceEventKind::RenderPublished,
                                    base + kInjectedDecodeDelay,
                                    100U + frame,
                                    coordinatorScope(1, 1, 1)));
        collector.ingest(traceEvent(TraceEventKind::RenderDrawStarted,
                                    base + kInjectedDecodeDelay,
                                    100U + frame,
                                    TraceIdentity{}));
        collector.ingest(traceEvent(TraceEventKind::RenderAckPublished,
                                    base + kInjectedDecodeDelay,
                                    100U + frame,
                                    TraceIdentity{}));
        collector.ingest(traceEvent(TraceEventKind::PresentationAcknowledged,
                                    base + kInjectedDecodeDelay,
                                    100U + frame,
                                    coordinatorScope(1, 1, 1)));
    }
    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStopped, 2'000U, 0U, coordinatorScope(1, 1, 1)));

    const PlaybackEvidenceSummary summary = collector.summary();
    ASSERT_EQ(summary.runs.size(), 1U);
    const PlaybackRunEvidence& run = summary.runs.front();
    ASSERT_FALSE(run.open);

    const PlaybackLatencyStats& decode =
        stageStats(run, PlaybackEvidenceStage::kSubmitToDecodeCompleted);
    EXPECT_EQ(decode.count, 10U);
    EXPECT_EQ(decode.maxMicroseconds, kInjectedDecodeDelay);
    EXPECT_EQ(decode.p95Microseconds, kInjectedDecodeDelay);
    EXPECT_EQ(decode.truncatedSampleCount, 0U);

    for (const PlaybackEvidenceStage untouched :
         {PlaybackEvidenceStage::kDecodeCompletedToFrameSetReady,
          PlaybackEvidenceStage::kRenderSchedule,
          PlaybackEvidenceStage::kDrawToAckPublished,
          PlaybackEvidenceStage::kRenderToPresentationAck}) {
        const PlaybackLatencyStats& stats = stageStats(run, untouched);
        EXPECT_EQ(stats.count, 10U) << "every hop closed once per frame";
        EXPECT_EQ(stats.maxMicroseconds, 0U) << "the decode injection must not pollute this stage";
        EXPECT_EQ(stats.p95Microseconds, 0U);
    }
    EXPECT_EQ(run.skippedFrameSetCount, 0U);
    EXPECT_EQ(run.staleEventCount, 0U);
    EXPECT_EQ(run.nonPlaybackWindowEventCount, 0U);
}

TEST(PlaybackEvidenceTests, LocatesAssemblyScheduleDrawAndPresentationDelaysSeparately) {
    PlaybackEvidenceCollector collector;
    constexpr std::uint64_t kAssemblyDelay = 40U;
    constexpr std::uint64_t kScheduleDelay = 7U;
    constexpr std::uint64_t kDrawDelay = 5U;
    constexpr std::uint64_t kPresentationDelay = 50U;

    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStarted, 0U, 0U, coordinatorScope(1, 1, 1)));

    // Assembly: decode completes at 10, the set is ready at 50.
    collector.ingest(traceEvent(TraceEventKind::ProviderSubmitted, 0U, 0U, providerScope(1, 1)));
    collector.ingest(
        traceEvent(TraceEventKind::SourceDecodeCompleted, 10U, 500U, decodeScope(1, 1, 7U)));
    collector.ingest(
        traceEvent(TraceEventKind::FrameSetReady, 50U, 100U, coordinatorScope(1, 1, 1)));

    // Presentation chain for canonical frame 200 with one delay injected per hop.
    collector.ingest(
        traceEvent(TraceEventKind::RenderPublished, 100U, 200U, coordinatorScope(1, 1, 1)));
    collector.ingest(traceEvent(TraceEventKind::RenderDrawStarted, 107U, 200U, TraceIdentity{}));
    collector.ingest(traceEvent(TraceEventKind::RenderAckPublished, 112U, 200U, TraceIdentity{}));
    collector.ingest(traceEvent(
        TraceEventKind::PresentationAcknowledged, 150U, 200U, coordinatorScope(1, 1, 1)));

    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStopped, 200U, 0U, coordinatorScope(1, 1, 1)));

    const PlaybackEvidenceSummary summary = collector.summary();
    ASSERT_EQ(summary.runs.size(), 1U);
    const PlaybackRunEvidence& run = summary.runs.front();

    const PlaybackLatencyStats& submitToDecode =
        stageStats(run, PlaybackEvidenceStage::kSubmitToDecodeCompleted);
    ASSERT_EQ(submitToDecode.count, 1U);
    EXPECT_EQ(submitToDecode.maxMicroseconds, 10U);

    const PlaybackLatencyStats& assembly =
        stageStats(run, PlaybackEvidenceStage::kDecodeCompletedToFrameSetReady);
    ASSERT_EQ(assembly.count, 1U);
    EXPECT_EQ(assembly.maxMicroseconds, kAssemblyDelay);

    const PlaybackLatencyStats& schedule = stageStats(run, PlaybackEvidenceStage::kRenderSchedule);
    ASSERT_EQ(schedule.count, 1U);
    EXPECT_EQ(schedule.maxMicroseconds, kScheduleDelay);

    const PlaybackLatencyStats& draw = stageStats(run, PlaybackEvidenceStage::kDrawToAckPublished);
    ASSERT_EQ(draw.count, 1U);
    EXPECT_EQ(draw.maxMicroseconds, kDrawDelay);

    const PlaybackLatencyStats& presentation =
        stageStats(run, PlaybackEvidenceStage::kRenderToPresentationAck);
    ASSERT_EQ(presentation.count, 1U);
    EXPECT_EQ(presentation.maxMicroseconds, kPresentationDelay);
    EXPECT_EQ(presentation.p95Microseconds, kPresentationDelay);
}

TEST(PlaybackEvidenceTests, ComputesNearestRankP95OverSortedSamples) {
    PlaybackEvidenceCollector collector;
    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStarted, 0U, 0U, coordinatorScope(1, 1, 1)));
    // Delays 1..20: the nearest-rank 95th percentile of 20 samples is the 19th smallest.
    for (std::uint64_t sample = 0U; sample < 20U; ++sample) {
        collector.ingest(
            traceEvent(TraceEventKind::ProviderSubmitted, sample, 0U, providerScope(1, 1)));
        collector.ingest(traceEvent(TraceEventKind::SourceDecodeCompleted,
                                    sample + sample + 1U,
                                    600U + sample,
                                    decodeScope(1, 1, 7U)));
    }
    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStopped, 100U, 0U, coordinatorScope(1, 1, 1)));

    const PlaybackEvidenceSummary summary = collector.summary();
    const PlaybackLatencyStats& decode =
        stageStats(summary.runs.front(), PlaybackEvidenceStage::kSubmitToDecodeCompleted);
    EXPECT_EQ(decode.count, 20U);
    EXPECT_EQ(decode.maxMicroseconds, 20U);
    EXPECT_EQ(decode.p95Microseconds, 19U);
}

TEST(PlaybackEvidenceTests, PairsSubmissionsWithCompletionsInAdmissionOrder) {
    PlaybackEvidenceCollector collector;
    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStarted, 0U, 0U, coordinatorScope(1, 1, 1)));
    // FIFO pairing yields delays {101, 100}; a stack (LIFO) pairing would yield {1, 200} and a
    // different maximum, so the max pins the pairing order.
    collector.ingest(traceEvent(TraceEventKind::ProviderSubmitted, 0U, 0U, providerScope(1, 1)));
    collector.ingest(traceEvent(TraceEventKind::ProviderSubmitted, 100U, 0U, providerScope(1, 1)));
    collector.ingest(
        traceEvent(TraceEventKind::SourceDecodeCompleted, 101U, 601U, decodeScope(1, 1, 7U)));
    collector.ingest(
        traceEvent(TraceEventKind::SourceDecodeCompleted, 200U, 602U, decodeScope(1, 1, 7U)));
    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStopped, 300U, 0U, coordinatorScope(1, 1, 1)));

    const PlaybackEvidenceSummary summary = collector.summary();
    const PlaybackLatencyStats& decode =
        stageStats(summary.runs.front(), PlaybackEvidenceStage::kSubmitToDecodeCompleted);
    EXPECT_EQ(decode.count, 2U);
    EXPECT_EQ(decode.maxMicroseconds, 101U);
    EXPECT_EQ(decode.p95Microseconds, 101U);
}

TEST(PlaybackEvidenceTests, AssemblyPairsWithTheLatestDecodeCompletion) {
    PlaybackEvidenceCollector collector;
    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStarted, 0U, 0U, coordinatorScope(1, 1, 1)));
    collector.ingest(
        traceEvent(TraceEventKind::SourceDecodeCompleted, 10U, 700U, decodeScope(1, 1, 7U)));
    collector.ingest(
        traceEvent(TraceEventKind::SourceDecodeCompleted, 20U, 701U, decodeScope(1, 1, 7U)));
    collector.ingest(
        traceEvent(TraceEventKind::FrameSetReady, 23U, 100U, coordinatorScope(1, 1, 1)));
    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStopped, 30U, 0U, coordinatorScope(1, 1, 1)));

    const PlaybackEvidenceSummary summary = collector.summary();
    const PlaybackLatencyStats& assembly =
        stageStats(summary.runs.front(), PlaybackEvidenceStage::kDecodeCompletedToFrameSetReady);
    EXPECT_EQ(assembly.count, 1U);
    EXPECT_EQ(assembly.maxMicroseconds, 3U);
}

TEST(PlaybackEvidenceTests, CountsSkippedFrameSetsFromPublishGapsOnly) {
    PlaybackEvidenceCollector collector;
    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStarted, 0U, 0U, coordinatorScope(1, 1, 1)));
    const auto publish = [&collector](const std::uint64_t at, const std::uint64_t frame) {
        collector.ingest(
            traceEvent(TraceEventKind::RenderPublished, at, frame, coordinatorScope(1, 1, 1)));
    };

    // The first publish of a run is the baseline; frames before it are not a gap.
    publish(100U, 3U);
    publish(110U, 4U);
    // 5 and 6 are never published: exactly two skipped frame sets.
    publish(120U, 7U);
    publish(130U, 8U);

    // A republish of the newest frame is allowed and is not a skip.
    publish(140U, 8U);
    // A ten-second interval between consecutive ids (VFR / long frame content) is not a skip:
    // skips are defined by id gaps, never by time intervals.
    publish(10'000'130U, 9U);

    const PlaybackEvidenceSummary summary = collector.summary();
    ASSERT_EQ(summary.runs.size(), 1U);
    EXPECT_EQ(summary.runs.front().skippedFrameSetCount, 2U);
}

TEST(PlaybackEvidenceTests, ExcludesStaleEpochRunAndSessionEventsFromRunStatistics) {
    PlaybackEvidenceCollector collector;
    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStarted, 0U, 0U, coordinatorScope(1, 2, 5)));

    // Old epoch (and its old run): filtered, no sample may result.
    collector.ingest(
        traceEvent(TraceEventKind::RenderPublished, 100U, 50U, coordinatorScope(1, 1, 4)));
    collector.ingest(
        traceEvent(TraceEventKind::PresentationAcknowledged, 130U, 50U, coordinatorScope(1, 1, 4)));
    // Old run under the current epoch: filtered as well.
    collector.ingest(
        traceEvent(TraceEventKind::RenderPublished, 150U, 51U, coordinatorScope(1, 2, 4)));
    // Foreign session under the same epoch/run: filtered.
    collector.ingest(
        traceEvent(TraceEventKind::RenderPublished, 160U, 52U, coordinatorScope(2, 2, 5)));
    // A stop for the old run while run 5 is open: filtered, does not close the active run.
    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStopped, 170U, 0U, coordinatorScope(1, 2, 4)));
    // Provider event from the old epoch: filtered.
    collector.ingest(traceEvent(TraceEventKind::ProviderSubmitted, 180U, 0U, providerScope(1, 1)));
    // Decode event with no request context (zeros): scope unknown, non-playback window.
    collector.ingest(traceEvent(TraceEventKind::SourceDecodeStarted, 190U, 900U, TraceIdentity{}));
    // Coordinator event emitted without a run (seek/step phase) during the open run.
    collector.ingest(
        traceEvent(TraceEventKind::FrameSetReady, 200U, 53U, coordinatorScope(1, 2, 0)));

    // In-scope pair: the only sample that may exist.
    collector.ingest(
        traceEvent(TraceEventKind::RenderPublished, 300U, 60U, coordinatorScope(1, 2, 5)));
    collector.ingest(
        traceEvent(TraceEventKind::PresentationAcknowledged, 330U, 60U, coordinatorScope(1, 2, 5)));

    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStopped, 400U, 0U, coordinatorScope(1, 2, 5)));

    const PlaybackEvidenceSummary summary = collector.summary();
    ASSERT_EQ(summary.runs.size(), 1U);
    const PlaybackRunEvidence& run = summary.runs.front();
    EXPECT_FALSE(run.open);

    const PlaybackLatencyStats& presentation =
        stageStats(run, PlaybackEvidenceStage::kRenderToPresentationAck);
    EXPECT_EQ(presentation.count, 1U);
    EXPECT_EQ(presentation.maxMicroseconds, 30U);
    for (const PlaybackEvidenceStage untouched :
         {PlaybackEvidenceStage::kSubmitToDecodeCompleted,
          PlaybackEvidenceStage::kDecodeCompletedToFrameSetReady,
          PlaybackEvidenceStage::kRenderSchedule,
          PlaybackEvidenceStage::kDrawToAckPublished}) {
        EXPECT_EQ(stageStats(run, untouched).count, 0U);
    }

    // Old epoch pair (2) + old-run publish (1) + foreign session (1) + old-run stop (1) +
    // old-epoch provider submit (1) = 6 filtered events.
    EXPECT_EQ(run.staleEventCount, 6U);
    // Context-less decode start (1) + run-zero FrameSetReady (1).
    EXPECT_EQ(run.nonPlaybackWindowEventCount, 2U);
    EXPECT_EQ(summary.cumulativeStaleEventCount, 6U);
    EXPECT_EQ(summary.cumulativeNonPlaybackWindowEventCount, 2U);
    EXPECT_EQ(run.pendingSourceDecodeCount, 0U);
}

TEST(PlaybackEvidenceTests, OpensAndClosesRunsWithoutCrossRunBleed) {
    PlaybackEvidenceCollector collector;

    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStarted, 0U, 0U, coordinatorScope(1, 1, 1)));
    collector.ingest(traceEvent(TraceEventKind::ProviderSubmitted, 10U, 0U, providerScope(1, 1)));
    collector.ingest(
        traceEvent(TraceEventKind::SourceDecodeCompleted, 110U, 800U, decodeScope(1, 1, 7U)));
    collector.ingest(
        traceEvent(TraceEventKind::RenderPublished, 120U, 3U, coordinatorScope(1, 1, 1)));
    collector.ingest(
        traceEvent(TraceEventKind::RenderPublished, 121U, 4U, coordinatorScope(1, 1, 1)));
    // A submit with no completion stays pending and must be frozen into run 1's evidence.
    collector.ingest(traceEvent(TraceEventKind::ProviderSubmitted, 200U, 0U, providerScope(1, 1)));
    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStopped, 300U, 0U, coordinatorScope(1, 1, 1)));

    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStarted, 400U, 0U, coordinatorScope(1, 1, 2)));
    // First publish of run 2 re-baselines the skip counter even though run 1 ended at frame 4.
    collector.ingest(
        traceEvent(TraceEventKind::RenderPublished, 410U, 12U, coordinatorScope(1, 1, 2)));
    collector.ingest(
        traceEvent(TraceEventKind::PresentationAcknowledged, 410U, 12U, coordinatorScope(1, 1, 2)));
    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStopped, 500U, 0U, coordinatorScope(1, 1, 2)));

    const PlaybackEvidenceSummary summary = collector.summary();
    ASSERT_EQ(summary.runs.size(), 2U);
    const PlaybackRunEvidence& first = summary.runs[0];
    const PlaybackRunEvidence& second = summary.runs[1];

    EXPECT_EQ(first.runId, 1U);
    EXPECT_FALSE(first.open);
    EXPECT_FALSE(first.truncated);
    EXPECT_EQ(stageStats(first, PlaybackEvidenceStage::kSubmitToDecodeCompleted).count, 1U);
    EXPECT_EQ(stageStats(first, PlaybackEvidenceStage::kSubmitToDecodeCompleted).maxMicroseconds,
              100U);
    EXPECT_EQ(first.skippedFrameSetCount, 0U);
    EXPECT_EQ(first.pendingProviderSubmitCount, 1U);
    EXPECT_EQ(first.pendingPublicationCount, 2U);

    EXPECT_EQ(second.runId, 2U);
    EXPECT_FALSE(second.open);
    EXPECT_EQ(stageStats(second, PlaybackEvidenceStage::kSubmitToDecodeCompleted).count, 0U);
    EXPECT_EQ(stageStats(second, PlaybackEvidenceStage::kRenderToPresentationAck).count, 1U);
    EXPECT_EQ(stageStats(second, PlaybackEvidenceStage::kRenderToPresentationAck).maxMicroseconds,
              0U);
    EXPECT_EQ(second.skippedFrameSetCount, 0U);
    EXPECT_EQ(second.pendingProviderSubmitCount, 0U);

    EXPECT_EQ(summary.cumulativeStaleEventCount, 0U);
    EXPECT_EQ(summary.cumulativeNonPlaybackWindowEventCount, 0U);
}

TEST(PlaybackEvidenceTests, OneSidedEventsProducePendingCountsInsteadOfFalseLatency) {
    PlaybackEvidenceCollector collector;
    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStarted, 0U, 0U, coordinatorScope(1, 1, 1)));

    // A reversed pair (completion timestamp before its admission): discarded, never clamped.
    collector.ingest(traceEvent(TraceEventKind::ProviderSubmitted, 500U, 0U, providerScope(1, 1)));
    collector.ingest(
        traceEvent(TraceEventKind::SourceDecodeCompleted, 400U, 900U, decodeScope(1, 1, 7U)));
    // A decode Started with no Completed is the hang signature and stays pending.
    collector.ingest(
        traceEvent(TraceEventKind::SourceDecodeStarted, 2'000U, 901U, decodeScope(1, 1, 7U)));
    // A submit with no completion.
    collector.ingest(
        traceEvent(TraceEventKind::ProviderSubmitted, 3'000U, 0U, providerScope(1, 1)));
    // A publication with no presentation acknowledgement, but with a draw observed.
    collector.ingest(
        traceEvent(TraceEventKind::RenderPublished, 4'000U, 20U, coordinatorScope(1, 1, 1)));
    collector.ingest(traceEvent(TraceEventKind::RenderDrawStarted, 4'005U, 20U, TraceIdentity{}));
    // A renderer observation for a frame this run never published cannot be keyed.
    collector.ingest(traceEvent(TraceEventKind::RenderAckPublished, 4'006U, 21U, TraceIdentity{}));

    const PlaybackEvidenceSummary summary = collector.summary();
    ASSERT_EQ(summary.runs.size(), 1U);
    const PlaybackRunEvidence& run = summary.runs.front();
    ASSERT_TRUE(run.open);

    EXPECT_EQ(run.discardedNonMonotonicPairCount, 1U);
    EXPECT_EQ(run.pendingSourceDecodeCount, 1U);
    EXPECT_EQ(run.pendingProviderSubmitCount, 1U);
    EXPECT_EQ(run.pendingPublicationCount, 1U);
    EXPECT_EQ(run.pendingDrawStartCount, 1U);
    EXPECT_EQ(run.unkeyedEventCount, 1U);

    for (const PlaybackEvidenceStage noSample :
         {PlaybackEvidenceStage::kSubmitToDecodeCompleted,
          PlaybackEvidenceStage::kDecodeCompletedToFrameSetReady,
          PlaybackEvidenceStage::kDrawToAckPublished,
          PlaybackEvidenceStage::kRenderToPresentationAck}) {
        EXPECT_EQ(stageStats(run, noSample).count, 0U);
        EXPECT_EQ(stageStats(run, noSample).maxMicroseconds, 0U);
        EXPECT_EQ(stageStats(run, noSample).p95Microseconds, 0U);
    }
    // The draw was observed for the published frame, so exactly that hop is measurable.
    const PlaybackLatencyStats& schedule = stageStats(run, PlaybackEvidenceStage::kRenderSchedule);
    EXPECT_EQ(schedule.count, 1U);
    EXPECT_EQ(schedule.maxMicroseconds, 5U);
}

TEST(PlaybackEvidenceTests, EventsOutsideAnyRunWindowLandInTheNonPlaybackWindow) {
    PlaybackEvidenceCollector collector;
    // Open/seek/step-phase events before (and between) runs: counted, never attributed.
    collector.ingest(traceEvent(TraceEventKind::FrameSetReady, 5U, 1U, coordinatorScope(1, 1, 0)));
    collector.ingest(
        traceEvent(TraceEventKind::RenderPublished, 6U, 1U, coordinatorScope(1, 1, 0)));
    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStarted, 7U, 0U, coordinatorScope(1, 1, 0)));

    const PlaybackEvidenceSummary summary = collector.summary();
    EXPECT_TRUE(summary.runs.empty());
    EXPECT_EQ(summary.cumulativeNonPlaybackWindowEventCount, 3U);
    EXPECT_EQ(summary.cumulativeStaleEventCount, 0U);

    collector.reset();
    const PlaybackEvidenceSummary afterReset = collector.summary();
    EXPECT_TRUE(afterReset.runs.empty());
    EXPECT_EQ(afterReset.cumulativeNonPlaybackWindowEventCount, 0U);
}

TEST(PlaybackEvidenceTests, TruncatesThePublicationTableAndCountsTheEviction) {
    PlaybackEvidenceCollector collector;
    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStarted, 0U, 0U, coordinatorScope(1, 1, 1)));
    for (std::uint64_t frame = 0U;
         frame <= static_cast<std::uint64_t>(PlaybackEvidenceCollector::kMaxPendingPublications);
         ++frame) {
        collector.ingest(
            traceEvent(TraceEventKind::RenderPublished, frame, frame, coordinatorScope(1, 1, 1)));
    }

    const PlaybackEvidenceSummary summary = collector.summary();
    const PlaybackRunEvidence& run = summary.runs.front();
    EXPECT_EQ(run.pendingPublicationCount,
              static_cast<std::uint64_t>(PlaybackEvidenceCollector::kMaxPendingPublications));
    EXPECT_EQ(run.evictedPendingCount, 1U);
    EXPECT_EQ(run.skippedFrameSetCount, 0U);
}

TEST(PlaybackEvidenceTests, ConcurrentIngestPairsEverySampleUnderTheMutex) {
    PlaybackEvidenceCollector collector;
    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStarted, 0U, 0U, coordinatorScope(1, 1, 1)));

    constexpr int kThreads = 4;
    constexpr int kPairsPerThread = 250;
    constexpr std::uint64_t kSubmitAt = 1'000U;
    constexpr std::uint64_t kDelay = 100U;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int thread = 0; thread < kThreads; ++thread) {
        threads.emplace_back([&collector] {
            for (int pair = 0; pair < kPairsPerThread; ++pair) {
                // Identical timestamps from every thread: whichever interleaving the mutex
                // serializes, every FIFO pair spans exactly the injected delay.
                collector.ingest(traceEvent(
                    TraceEventKind::ProviderSubmitted, kSubmitAt, 0U, providerScope(1, 1)));
                collector.ingest(traceEvent(TraceEventKind::SourceDecodeCompleted,
                                            kSubmitAt + kDelay,
                                            950U,
                                            decodeScope(1, 1, 7U)));
            }
        });
    }
    for (std::thread& worker : threads) {
        worker.join();
    }
    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStopped, 100'000U, 0U, coordinatorScope(1, 1, 1)));

    const PlaybackEvidenceSummary summary = collector.summary();
    ASSERT_EQ(summary.runs.size(), 1U);
    const PlaybackRunEvidence& run = summary.runs.front();
    const PlaybackLatencyStats& decode =
        stageStats(run, PlaybackEvidenceStage::kSubmitToDecodeCompleted);
    EXPECT_EQ(decode.count, static_cast<std::uint64_t>(kThreads * kPairsPerThread));
    EXPECT_EQ(decode.maxMicroseconds, kDelay);
    EXPECT_EQ(decode.p95Microseconds, kDelay);
    EXPECT_EQ(run.pendingProviderSubmitCount, 0U);
    EXPECT_EQ(run.staleEventCount, 0U);
}

// E03: the latency aggregate must expose nearest-rank P50/P95/P99 so one slow hop cannot hide
// behind an average or a single percentile. 11 samples of 100..1100 us pin all three ranks.
TEST(PlaybackEvidenceTests, StagePercentilesAreNearestRankP50P95P99) {
    PlaybackEvidenceCollector collector;
    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStarted, 0U, 0U, coordinatorScope(1, 1, 1)));
    for (std::uint64_t sample = 0U; sample < 11U; ++sample) {
        // Submit-to-decode sample value: 100 * (sample + 1) microseconds.
        const std::uint64_t submittedAt = 10'000U + sample * 10U;
        const std::uint64_t sampleMicroseconds = 100U * (sample + 1U);
        collector.ingest(
            traceEvent(TraceEventKind::ProviderSubmitted, submittedAt, 0U, providerScope(1, 1)));
        collector.ingest(traceEvent(TraceEventKind::SourceDecodeCompleted,
                                    submittedAt + sampleMicroseconds,
                                    500U + sample,
                                    decodeScope(1, 1, 7U)));
    }
    // The open run's live snapshot must already carry the percentiles (a UI polls summary()
    // mid-run); a value only appearing after the stop would hide in-flight degradation.
    {
        const PlaybackEvidenceSummary open = collector.summary();
        ASSERT_EQ(open.runs.size(), 1U);
        ASSERT_TRUE(open.runs.front().open);
        const PlaybackLatencyStats& live =
            stageStats(open.runs.front(), PlaybackEvidenceStage::kSubmitToDecodeCompleted);
        ASSERT_EQ(live.count, 11U);
        EXPECT_EQ(live.p50Microseconds, 600U);
        EXPECT_EQ(live.p95Microseconds, 1'100U);
        EXPECT_EQ(live.p99Microseconds, 1'100U);
    }
    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStopped, 100'000U, 0U, coordinatorScope(1, 1, 1)));

    const PlaybackEvidenceSummary summary = collector.summary();
    ASSERT_EQ(summary.runs.size(), 1U);
    const PlaybackRunEvidence& run = summary.runs.front();
    const PlaybackLatencyStats& decode =
        stageStats(run, PlaybackEvidenceStage::kSubmitToDecodeCompleted);
    ASSERT_EQ(decode.count, 11U);
    EXPECT_EQ(decode.maxMicroseconds, 1'100U);
    // Nearest rank on 11 sorted samples {100..1100}: ceil(0.50*11)=6th -> 600;
    // ceil(0.95*11)=11th -> 1100; ceil(0.99*11)=11th -> 1100.
    EXPECT_EQ(decode.p50Microseconds, 600U);
    EXPECT_EQ(decode.p95Microseconds, 1'100U);
    EXPECT_EQ(decode.p99Microseconds, 1'100U);
    EXPECT_EQ(decode.truncatedSampleCount, 0U);
}

// E03 分层呈现: a publication with no presentation acknowledgement has no confirmed-presentation
// evidence. The render-to-ack stage must stay unmeasured (count 0), the unfinished publication
// must surface through the pending counter, and the summary must keep reporting the system
// presentation layer as unknown - never a fabricated zero-latency or zero-drop reading.
TEST(PlaybackEvidenceTests, PublishedFramesWithoutAcknowledgementKeepPresentationUnmeasured) {
    PlaybackEvidenceCollector collector;
    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStarted, 0U, 0U, coordinatorScope(1, 1, 1)));
    for (std::uint64_t frame = 0U; frame < 3U; ++frame) {
        const std::uint64_t base = 1'000U + frame * 100U;
        collector.ingest(
            traceEvent(TraceEventKind::ProviderSubmitted, base, 0U, providerScope(1, 1)));
        collector.ingest(traceEvent(TraceEventKind::SourceDecodeCompleted,
                                    base + 10U,
                                    500U + frame,
                                    decodeScope(1, 1, 7U)));
        collector.ingest(traceEvent(
            TraceEventKind::FrameSetReady, base + 11U, 100U + frame, coordinatorScope(1, 1, 1)));
        collector.ingest(traceEvent(
            TraceEventKind::RenderPublished, base + 12U, 100U + frame, coordinatorScope(1, 1, 1)));
        // Draw and renderer-side ack exist, but the coordinator never received the presentation
        // acknowledgement for these frames.
        collector.ingest(traceEvent(
            TraceEventKind::RenderDrawStarted, base + 20U, 100U + frame, TraceIdentity{}));
        collector.ingest(traceEvent(
            TraceEventKind::RenderAckPublished, base + 25U, 100U + frame, TraceIdentity{}));
    }
    collector.ingest(
        traceEvent(TraceEventKind::PlaybackRunStopped, 2'000U, 0U, coordinatorScope(1, 1, 1)));

    const PlaybackEvidenceSummary summary = collector.summary();
    ASSERT_EQ(summary.runs.size(), 1U);
    const PlaybackRunEvidence& run = summary.runs.front();
    ASSERT_FALSE(run.open);
    // Publish happened (frame ids advanced), so the pipeline demonstrably ran...
    const PlaybackLatencyStats& schedule = stageStats(run, PlaybackEvidenceStage::kRenderSchedule);
    EXPECT_EQ(schedule.count, 3U);
    // ...but the confirmed-presentation hop never closed: zero completed pairs, all percentiles
    // at zero because nothing was measured, and the publications still pending.
    const PlaybackLatencyStats& presentation =
        stageStats(run, PlaybackEvidenceStage::kRenderToPresentationAck);
    EXPECT_EQ(presentation.count, 0U);
    EXPECT_EQ(presentation.p50Microseconds, 0U);
    EXPECT_EQ(presentation.p95Microseconds, 0U);
    EXPECT_EQ(presentation.p99Microseconds, 0U);
    EXPECT_EQ(presentation.maxMicroseconds, 0U);
    EXPECT_EQ(run.pendingPublicationCount, 3U);
    // The swap-chain / physical present layer stays unknown, not zero.
    EXPECT_EQ(summary.systemPresentationStatus, PlaybackEvidenceFieldStatus::kUnknown);
}

} // namespace
} // namespace dvs::application
