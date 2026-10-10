#pragma once

#include "dvs/application/PlaybackTrace.h"
#include "dvs/domain/Identifiers.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace dvs::application {

// Application-record playback timing evidence (roadmap P09, feeding the D01 evidence panel).
// A PlaybackEvidenceSummary aggregates the stage latencies the existing trace emission contract
// can prove, grouped by playback run, together with the complete-frame-set skip count derived
// from published canonical frame gaps. It is a passive consumer of TraceEvent records: it never
// records events itself, so "sampling off" is simply "do not ingest" and needs no extra switch.
//
// Evidence levels (roadmap section 5, D01) are kept apart on purpose:
// - Application records are what this summary contains: decode readiness, scheduling, complete
//   frame-set skips, and draw/acknowledge timings as recorded by the application.
// - Source time information (file PTS, required frame cadence, duplicate-content candidates) is
//   not present in the trace schema this collector consumes; `sourceTimingStatus` reports it as
//   kUnavailable instead of fabricating zeros.
// - System presentation (OS present time, screen scan-out) is not observable through the current
//   contract: RenderAckPublished is the CPU-side admission of a presentation acknowledgement,
//   not a GPU completion fence and not a physical screen present. `systemPresentationStatus`
//   reports kUnknown. A presentation acknowledgement therefore proves the coordinator accepted
//   the frame, never that the screen displayed it.
// The skip counter below is likewise application-level only: it counts frame sets the
// application never published. It says nothing about source PTS gaps (source level) or about
// frames the compositor dropped (system level); those are reported through the status fields.

// One measured latency hop in the presentation chain. The stages follow the hop table in
// docs/engineering/trace-schema.md ("Metric naming (D06)"); they must not be relabelled as
// "GPU time".
enum class PlaybackEvidenceStage : std::uint8_t {
    // ProviderSubmitted(2) -> SourceDecodeCompleted(24): provider admission to decode
    // completion, including request queueing. Proves the decoder call returned within the
    // measured interval; does not prove the frame was error-free or displayed.
    kSubmitToDecodeCompleted = 0,
    // SourceDecodeCompleted(24) -> FrameSetReady(4): assembly of the multi-source frame set
    // after the most recent in-run decode completion. Proves the coordinator-side assembly
    // gap; the pairing is ordinal (the emission carries no join key between the source-frame
    // payload of 24 and the canonical payload of 4), so with interleaved read-ahead decodes
    // this is the gap after the latest observed completion, not a per-frame identity join.
    kDecodeCompletedToFrameSetReady = 1,
    // RenderPublished(6) -> RenderDrawStarted(16): the scene graph scheduled a render for an
    // already-published frame. A dominating value means window/render-loop scheduling, not
    // GPU work.
    kRenderSchedule = 2,
    // RenderDrawStarted(16) -> RenderAckPublished(17): CPU draw submit and command recording
    // up to admission of the presentation acknowledgement. Not a GPU completion fence and not
    // a screen present.
    kDrawToAckPublished = 3,
    // RenderPublished(6) -> PresentationAcknowledged(7): coordinator-visible draw-to-
    // presentation-confirmation latency, the union of scheduling, draw and ack relay. Proves
    // the acknowledgement reached the coordinator; does not prove the frame reached the screen.
    kRenderToPresentationAck = 4,
};

inline constexpr std::size_t kPlaybackEvidenceStageCount = 5U;

// Per-stage latency aggregate. `count` is completed pairs, not emitted events: one-sided
// events leave count at 0 and surface through the pending counters instead. `max` is exact.
// `p95Microseconds` is the nearest-rank 95th percentile (ceil(0.95 * n)-th smallest sample).
// Samples beyond kMaxStageSamplesPerRun per stage are counted in `truncatedSampleCount` and
// excluded from the percentile reservoir, which then keeps the largest samples and can only
// overestimate p95 - the conservative direction for stall diagnosis.
struct PlaybackLatencyStats final {
    std::uint64_t count = 0U;
    std::uint64_t maxMicroseconds = 0U;
    std::uint64_t p95Microseconds = 0U;
    std::uint64_t truncatedSampleCount = 0U;

    [[nodiscard]] bool operator==(const PlaybackLatencyStats&) const noexcept = default;
};

// Availability of the non-application evidence levels in one summary. The collector never
// coerces a missing level into a zero measurement.
enum class PlaybackEvidenceFieldStatus : std::uint8_t {
    kCollected = 0,
    // The level exists but this collector does not sample it (source PTS timing).
    kUnavailable,
    // No admissible evidence either way in the current contract (physical screen present).
    kUnknown,
};

// Aggregated evidence for exactly one playback run (PlaybackRunStarted(18) to
// PlaybackRunStopped(19)). Statistics never bleed across runs: pairing tables and skip
// baselines are cleared when a run opens.
struct PlaybackRunEvidence final {
    // Run identity copied from the PlaybackRunStarted event that opened this run.
    std::uint64_t runId = 0U;
    domain::SessionId session{0};
    domain::SessionEpoch epoch{0};
    std::uint64_t startedAtMicroseconds = 0U;
    std::uint64_t stoppedAtMicroseconds = 0U;
    // True while the run is still open (no PlaybackRunStopped ingested yet). Pending counters
    // are a live view for an open run and the closing snapshot for a completed run.
    bool open = false;
    // True when the run was closed by the next PlaybackRunStarted instead of a
    // PlaybackRunStopped (a truncated capture), which can also mean the stop event was lost.
    bool truncated = false;

    std::array<PlaybackLatencyStats, kPlaybackEvidenceStageCount> stages{};

    // Complete frame sets the application never published: gaps in the canonical frame ids of
    // consecutive in-run RenderPublished events. Gap-based by definition - a long frame
    // interval with consecutive ids (VFR content, a slow-but-complete frame) is never counted,
    // because absence of a gap is absence of evidence for a skip. Counts neither source PTS
    // gaps nor compositor drops.
    std::uint64_t skippedFrameSetCount = 0U;

    // Snapshot honesty counters for events seen inside this run's window.
    std::uint64_t pendingProviderSubmitCount = 0U;
    std::uint64_t pendingSourceDecodeCount = 0U;
    std::uint64_t pendingPublicationCount = 0U;
    std::uint64_t pendingDrawStartCount = 0U;
    // Events whose payload could not be keyed to an in-run publication (renderer observation
    // events for frames this run never published).
    std::uint64_t unkeyedEventCount = 0U;
    // Pairs whose end timestamp preceded its start timestamp. Cross-thread timestamps are not
    // a total order (trace-schema.md), so such pairs are discarded rather than clamped.
    std::uint64_t discardedNonMonotonicPairCount = 0U;
    // Entries evicted from the bounded pairing tables (see the collector's memory bounds).
    std::uint64_t evictedPendingCount = 0U;
    // Events filtered out by identity discipline (stale epoch/run, foreign session) and events
    // in the non-playback window (run=0 emission while this run was open).
    std::uint64_t staleEventCount = 0U;
    std::uint64_t nonPlaybackWindowEventCount = 0U;

    [[nodiscard]] bool operator==(const PlaybackRunEvidence&) const noexcept = default;
};

// Point-in-time snapshot of everything the collector can prove. Cheap to copy: percentile
// reservoirs stay inside the collector, only aggregates come out.
struct PlaybackEvidenceSummary final {
    // Bounded run history, oldest first; the open run (if any) is the last element. Runs
    // evicted from the history are counted in droppedRunCount, never silently forgotten.
    std::vector<PlaybackRunEvidence> runs;
    std::uint64_t droppedRunCount = 0U;
    // Cross-run totals, including contributions from outside any open run window.
    std::uint64_t cumulativeStaleEventCount = 0U;
    std::uint64_t cumulativeNonPlaybackWindowEventCount = 0U;

    // Source time information (file PTS): this collector does not sample it.
    PlaybackEvidenceFieldStatus sourceTimingStatus = PlaybackEvidenceFieldStatus::kUnavailable;
    // Physical system presentation: no admissible evidence in the current contract.
    PlaybackEvidenceFieldStatus systemPresentationStatus = PlaybackEvidenceFieldStatus::kUnknown;

    [[nodiscard]] bool operator==(const PlaybackEvidenceSummary&) const noexcept = default;
};

// Incremental consumer of the playback trace event stream (P09 minimal loop). Events are fed
// one by one via ingest(), ideally in trace file order - the only ordering the schema defines -
// and a UI pulls summary() at low frequency. Not wired into any producer: call sites decide
// whether sampling is on by ingesting or not.
//
// Thread contract: ingest(), summary() and reset() may be called from any thread; a single
// internal mutex serializes them. ingest() is cheap (bounded pairing tables, sorted-insert
// percentile reservoirs) but not wait-free; do not call it from a real-time audio-style loop.
//
// Memory bounds: only aggregates are retained for completed runs. The open run keeps at most
// kMaxStageSamplesPerRun samples per stage, kMaxPendingProviderSubmits admission timestamps,
// kMaxPendingSourceDecodes decode-start entries, and kMaxPendingPublications frame publications
// - roughly 200 KiB worst case, independent of run length. Raw events are never accumulated.
class PlaybackEvidenceCollector final {
public:
    static constexpr std::size_t kMaxStageSamplesPerRun = 4096U;
    static constexpr std::size_t kMaxPendingProviderSubmits = 1024U;
    static constexpr std::size_t kMaxPendingSourceDecodes = 1024U;
    static constexpr std::size_t kMaxPendingPublications = 256U;
    static constexpr std::size_t kMaxRetainedCompletedRuns = 8U;

    // Constructor and destructor are out-of-line so the incomplete RunState never leaks into
    // other translation units.
    PlaybackEvidenceCollector();
    ~PlaybackEvidenceCollector();

    // Consumes one trace event. Kinds other than the playback-chain and run-boundary kinds are
    // ignored. Identity discipline: events from a stale epoch/run (or a foreign session) are
    // counted as filtered and never enter the open run's statistics; events carrying no run
    // while no run is open - the open/seek/step phases - are counted in the non-playback
    // window instead of being dropped.
    void ingest(const TraceEvent& event);

    // Snapshot of the current evidence. Safe to call from any thread; intended for low-rate
    // UI pulls, not per-frame queries.
    [[nodiscard]] PlaybackEvidenceSummary summary() const;

    // Clears all run history, aggregates and pairing state.
    void reset() noexcept;

private:
    struct RunState;

    void closeActiveRun(std::uint64_t stoppedAtMicroseconds);

    mutable std::mutex mutex_;
    std::vector<PlaybackRunEvidence> completedRuns_;
    std::unique_ptr<RunState> activeRun_;
    std::uint64_t droppedRunCount_ = 0U;
    std::uint64_t cumulativeStaleEventCount_ = 0U;
    std::uint64_t cumulativeNonPlaybackWindowEventCount_ = 0U;
};

} // namespace dvs::application
