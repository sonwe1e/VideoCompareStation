#include "dvs/application/PlaybackEvidence.h"

#include "dvs/application/PlaybackLatencyReservoir.h"

#include <algorithm>
#include <deque>
#include <optional>

namespace dvs::application {
namespace {

[[nodiscard]] bool sameScope(const TraceIdentity& identity,
                             const domain::SessionId session,
                             const domain::SessionEpoch epoch,
                             const std::uint64_t run) noexcept {
    return identity.session == session && identity.epoch == epoch && identity.run == run;
}

enum class ScopeClass : std::uint8_t {
    kInRun,
    kNonPlaybackWindow,
    kStale,
};

// Classifies one playback-chain event against the open run's scope. Coordinator-side kinds
// (4/6/7) carry the live run id: run 0 means "emitted while no run was active" (open/seek/step
// phases) and lands in the non-playback window; any other mismatch is stale. Provider and
// decode kinds (2/23/24) never carry a run - the provider and the decode actors do not know
// it - so their scope is the session/epoch of the request context; a fully absent context
// (zeros) cannot be attributed and stays in the non-playback window rather than being guessed
// into the run. Renderer observation kinds (16/17) carry no identity at all and are keyed by
// frame payload against this run's publications (trace-schema.md).
[[nodiscard]] ScopeClass classifyChainEvent(const TraceEvent& event,
                                            const domain::SessionId session,
                                            const domain::SessionEpoch epoch,
                                            const std::uint64_t run) noexcept {
    switch (event.kind) {
    case TraceEventKind::FrameSetReady:
    case TraceEventKind::RenderPublished:
    case TraceEventKind::PresentationAcknowledged:
        if (event.identity.run == 0U) {
            return ScopeClass::kNonPlaybackWindow;
        }
        return sameScope(event.identity, session, epoch, run) ? ScopeClass::kInRun
                                                              : ScopeClass::kStale;
    case TraceEventKind::ProviderSubmitted:
    case TraceEventKind::SourceDecodeStarted:
    case TraceEventKind::SourceDecodeCompleted:
        if (event.identity.session == session && event.identity.epoch == epoch) {
            return ScopeClass::kInRun;
        }
        if (event.identity.session == domain::SessionId{0U} &&
            event.identity.epoch == domain::SessionEpoch{0U}) {
            return ScopeClass::kNonPlaybackWindow;
        }
        return ScopeClass::kStale;
    case TraceEventKind::RenderDrawStarted:
    case TraceEventKind::RenderAckPublished:
        return ScopeClass::kInRun;
    default:
        break;
    }
    return ScopeClass::kNonPlaybackWindow;
}

} // namespace

// Open-run accumulator: the published evidence fields plus the bounded pairing state that
// produces them. Everything here is cleared when the next run opens.
struct PlaybackEvidenceCollector::RunState final {
    PlaybackRunEvidence evidence;

    // Open SourceDecodeStarted entries: (source id, source frame id) -> Started timestamp.
    struct DecodeStart {
        std::uint64_t sourceId;
        std::uint64_t frame;
        std::uint64_t startedAt;
    };
    std::deque<DecodeStart> decodeStarts;
    // Provider admission timestamps awaiting a SourceDecodeCompleted, oldest first.
    std::deque<std::uint64_t> providerSubmits;
    // (canonical frame id, published timestamp, draw-start timestamp), in insertion order.
    struct Publication {
        std::uint64_t frame;
        std::uint64_t publishedAt;
        std::optional<std::uint64_t> drawStartedAt;
    };
    std::deque<Publication> publications;
    std::optional<std::uint64_t> latestDecodeCompletion;
    std::optional<std::uint64_t> lastPublishedFrame;

    std::array<PlaybackLatencyReservoir, kPlaybackEvidenceStageCount> reservoirs{
        PlaybackLatencyReservoir{kMaxStageSamplesPerRun},
        PlaybackLatencyReservoir{kMaxStageSamplesPerRun},
        PlaybackLatencyReservoir{kMaxStageSamplesPerRun},
        PlaybackLatencyReservoir{kMaxStageSamplesPerRun},
        PlaybackLatencyReservoir{kMaxStageSamplesPerRun},
    };

    explicit RunState(const TraceEvent& start) {
        evidence.runId = start.identity.run;
        evidence.session = start.identity.session;
        evidence.epoch = start.identity.epoch;
        evidence.open = true;
    }

    [[nodiscard]] std::deque<Publication>::iterator findPublication(const std::uint64_t frame) {
        return std::find_if(publications.begin(),
                            publications.end(),
                            [frame](const Publication& p) { return p.frame == frame; });
    }

    void addSample(const PlaybackEvidenceStage stage,
                   const std::uint64_t start,
                   const std::uint64_t end) {
        if (end < start) {
            // Cross-thread timestamps are not a total order; a reversed pair is discarded
            // rather than clamped into a fabricated latency.
            evidence.discardedNonMonotonicPairCount += 1U;
            return;
        }
        reservoirs[static_cast<std::size_t>(stage)].add(
            end - start, evidence.stages[static_cast<std::size_t>(stage)]);
    }
};

PlaybackEvidenceCollector::PlaybackEvidenceCollector() = default;

PlaybackEvidenceCollector::~PlaybackEvidenceCollector() = default;

void PlaybackEvidenceCollector::ingest(const TraceEvent& event) {
    const std::scoped_lock lock(mutex_);

    switch (event.kind) {
    case TraceEventKind::PlaybackRunStarted: {
        if (event.identity.run == 0U) {
            cumulativeNonPlaybackWindowEventCount_ += 1U;
            return;
        }
        if (activeRun_ != nullptr) {
            // A start while a run is still open means the stop was lost (truncated capture).
            // The open run is frozen as truncated and its tables are not merged anywhere.
            activeRun_->evidence.truncated = true;
            closeActiveRun();
        }
        activeRun_ = std::make_unique<RunState>(event);
        return;
    }
    case TraceEventKind::PlaybackRunStopped: {
        if (activeRun_ == nullptr || !sameScope(event.identity,
                                                activeRun_->evidence.session,
                                                activeRun_->evidence.epoch,
                                                activeRun_->evidence.runId)) {
            // A stop for another scope is stale (the run it belongs to is not open); a stop
            // with no open run is a non-playback-window event.
            if (activeRun_ != nullptr && event.identity.run != 0U) {
                activeRun_->evidence.staleEventCount += 1U;
                cumulativeStaleEventCount_ += 1U;
            } else {
                cumulativeNonPlaybackWindowEventCount_ += 1U;
            }
            return;
        }
        closeActiveRun();
        return;
    }
    case TraceEventKind::ProviderSubmitted:
    case TraceEventKind::FrameSetReady:
    case TraceEventKind::RenderPublished:
    case TraceEventKind::PresentationAcknowledged:
    case TraceEventKind::RenderDrawStarted:
    case TraceEventKind::RenderAckPublished:
    case TraceEventKind::SourceDecodeStarted:
    case TraceEventKind::SourceDecodeCompleted:
        break;
    default:
        // Kinds outside the playback chain are not this collector's concern.
        return;
    }

    if (activeRun_ == nullptr) {
        // Outside any run window: the open/seek/step phases. Counted, not attributed.
        cumulativeNonPlaybackWindowEventCount_ += 1U;
        return;
    }
    RunState& run = *activeRun_;
    switch (
        classifyChainEvent(event, run.evidence.session, run.evidence.epoch, run.evidence.runId)) {
    case ScopeClass::kStale:
        run.evidence.staleEventCount += 1U;
        cumulativeStaleEventCount_ += 1U;
        return;
    case ScopeClass::kNonPlaybackWindow:
        run.evidence.nonPlaybackWindowEventCount += 1U;
        cumulativeNonPlaybackWindowEventCount_ += 1U;
        return;
    case ScopeClass::kInRun:
        break;
    }

    switch (event.kind) {
    case TraceEventKind::ProviderSubmitted: {
        // Payload is the request priority; the admission timestamp is what pairs with the
        // next SourceDecodeCompleted. FIFO across sources: the provider and decode emission
        // carry no shared key (request id versus source frame id), so the pairing is ordinal
        // and measures admission-to-decode-complete including queueing.
        run.providerSubmits.push_back(event.timestampMicroseconds);
        if (run.providerSubmits.size() > kMaxPendingProviderSubmits) {
            run.providerSubmits.pop_front();
            run.evidence.evictedPendingCount += 1U;
        }
        return;
    }
    case TraceEventKind::SourceDecodeStarted: {
        // Keyed by (source id from the identity request, source frame id from the payload);
        // an entry left behind by the run's end is a decoder call that never returned.
        const std::uint64_t sourceId = event.identity.request.value();
        const std::uint64_t frame = event.payload;
        const auto existing =
            std::find_if(run.decodeStarts.begin(), run.decodeStarts.end(), [&](const auto& entry) {
                return entry.sourceId == sourceId && entry.frame == frame;
            });
        if (existing != run.decodeStarts.end()) {
            existing->startedAt = event.timestampMicroseconds;
            return;
        }
        run.decodeStarts.push_back(
            RunState::DecodeStart{sourceId, frame, event.timestampMicroseconds});
        if (run.decodeStarts.size() > kMaxPendingSourceDecodes) {
            run.decodeStarts.pop_front();
            run.evidence.evictedPendingCount += 1U;
        }
        return;
    }
    case TraceEventKind::SourceDecodeCompleted: {
        const std::uint64_t sourceId = event.identity.request.value();
        const std::uint64_t frame = event.payload;
        const auto started =
            std::find_if(run.decodeStarts.begin(), run.decodeStarts.end(), [&](const auto& entry) {
                return entry.sourceId == sourceId && entry.frame == frame;
            });
        if (started != run.decodeStarts.end()) {
            run.decodeStarts.erase(started);
        }
        if (!run.providerSubmits.empty()) {
            run.addSample(PlaybackEvidenceStage::kSubmitToDecodeCompleted,
                          run.providerSubmits.front(),
                          event.timestampMicroseconds);
            run.providerSubmits.pop_front();
        }
        run.latestDecodeCompletion = event.timestampMicroseconds;
        return;
    }
    case TraceEventKind::FrameSetReady: {
        if (run.latestDecodeCompletion.has_value()) {
            run.addSample(PlaybackEvidenceStage::kDecodeCompletedToFrameSetReady,
                          *run.latestDecodeCompletion,
                          event.timestampMicroseconds);
            run.latestDecodeCompletion.reset();
        }
        return;
    }
    case TraceEventKind::RenderPublished: {
        const std::uint64_t frame = event.payload;
        if (run.lastPublishedFrame.has_value()) {
            const std::uint64_t previous = *run.lastPublishedFrame;
            if (frame > previous) {
                // A gap between consecutive published canonical ids is a frame set the
                // application never published. Gap-based by definition: a long interval
                // between consecutive ids is never a skip, and there is no inference before
                // the first publication of a run.
                if (frame - previous > 1U) {
                    run.evidence.skippedFrameSetCount += frame - previous - 1U;
                }
            }
            // Every publication becomes the next baseline, including a rewind (loop restart or
            // a backward seek). Keeping the high-water mark instead would miss every gap after
            // the playhead wrapped, because the ids would not exceed the pre-rewind maximum.
            if (frame != previous) {
                run.lastPublishedFrame = frame;
            }
        } else {
            run.lastPublishedFrame = frame;
        }
        const auto existing = run.findPublication(frame);
        if (existing != run.publications.end()) {
            // Republishing an unpublished frame is allowed; the fresh timestamp pairs with
            // the next draw while any draw already started keeps its own pairing.
            existing->publishedAt = event.timestampMicroseconds;
            return;
        }
        run.publications.push_back(
            RunState::Publication{frame, event.timestampMicroseconds, std::nullopt});
        if (run.publications.size() > kMaxPendingPublications) {
            run.publications.pop_front();
            run.evidence.evictedPendingCount += 1U;
        }
        return;
    }
    case TraceEventKind::RenderDrawStarted: {
        const auto publication = run.findPublication(event.payload);
        if (publication == run.publications.end()) {
            // The renderer drew a frame this run never published (a recycled frame id from
            // another phase); it cannot be keyed into this run's statistics.
            run.evidence.unkeyedEventCount += 1U;
            return;
        }
        run.addSample(PlaybackEvidenceStage::kRenderSchedule,
                      publication->publishedAt,
                      event.timestampMicroseconds);
        publication->drawStartedAt = event.timestampMicroseconds;
        return;
    }
    case TraceEventKind::RenderAckPublished: {
        const auto publication = run.findPublication(event.payload);
        if (publication == run.publications.end()) {
            run.evidence.unkeyedEventCount += 1U;
            return;
        }
        if (publication->drawStartedAt.has_value()) {
            run.addSample(PlaybackEvidenceStage::kDrawToAckPublished,
                          *publication->drawStartedAt,
                          event.timestampMicroseconds);
        }
        return;
    }
    case TraceEventKind::PresentationAcknowledged: {
        const auto publication = run.findPublication(event.payload);
        if (publication == run.publications.end()) {
            run.evidence.unkeyedEventCount += 1U;
            return;
        }
        run.addSample(PlaybackEvidenceStage::kRenderToPresentationAck,
                      publication->publishedAt,
                      event.timestampMicroseconds);
        run.publications.erase(publication);
        return;
    }
    default:
        return;
    }
}

PlaybackEvidenceSummary PlaybackEvidenceCollector::summary() const {
    const std::scoped_lock lock(mutex_);

    PlaybackEvidenceSummary result;
    result.runs = completedRuns_;
    result.droppedRunCount = droppedRunCount_;
    result.cumulativeStaleEventCount = cumulativeStaleEventCount_;
    result.cumulativeNonPlaybackWindowEventCount = cumulativeNonPlaybackWindowEventCount_;
    if (activeRun_ == nullptr) {
        return result;
    }

    PlaybackRunEvidence live = activeRun_->evidence;
    for (std::size_t stage = 0U; stage < kPlaybackEvidenceStageCount; ++stage) {
        activeRun_->reservoirs[stage].finalizeStats(live.stages[stage]);
    }
    live.pendingProviderSubmitCount = activeRun_->providerSubmits.size();
    live.pendingSourceDecodeCount = activeRun_->decodeStarts.size();
    live.pendingPublicationCount = activeRun_->publications.size();
    live.pendingDrawStartCount = static_cast<std::uint64_t>(std::count_if(
        activeRun_->publications.begin(), activeRun_->publications.end(), [](const auto& p) {
            return p.drawStartedAt.has_value();
        }));
    result.runs.push_back(live);
    return result;
}

void PlaybackEvidenceCollector::reset() noexcept {
    const std::scoped_lock lock(mutex_);
    completedRuns_.clear();
    activeRun_.reset();
    droppedRunCount_ = 0U;
    cumulativeStaleEventCount_ = 0U;
    cumulativeNonPlaybackWindowEventCount_ = 0U;
}

void PlaybackEvidenceCollector::closeActiveRun() {
    RunState& run = *activeRun_;
    run.evidence.open = false;
    for (std::size_t stage = 0U; stage < kPlaybackEvidenceStageCount; ++stage) {
        run.reservoirs[stage].finalizeStats(run.evidence.stages[stage]);
    }
    run.evidence.pendingProviderSubmitCount = run.providerSubmits.size();
    run.evidence.pendingSourceDecodeCount = run.decodeStarts.size();
    run.evidence.pendingPublicationCount = run.publications.size();
    run.evidence.pendingDrawStartCount = static_cast<std::uint64_t>(
        std::count_if(run.publications.begin(), run.publications.end(), [](const auto& p) {
            return p.drawStartedAt.has_value();
        }));

    completedRuns_.push_back(run.evidence);
    if (completedRuns_.size() > kMaxRetainedCompletedRuns) {
        completedRuns_.erase(completedRuns_.begin());
        droppedRunCount_ += 1U;
    }
    activeRun_.reset();
}

} // namespace dvs::application
