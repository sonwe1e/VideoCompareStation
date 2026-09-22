#include "dvs/application/PlaybackCoordinator.h"

#include "dvs/application/AlignmentCacheIdentity.h"
#include "dvs/application/PlaybackTrace.h"
#include "dvs/application/PrefetchScheduler.h"
#include "dvs/domain/ComparisonSelection.h"
#include "dvs/domain/ComparisonValidator.h"
#include "dvs/domain/PlaybackContinuityPolicy.h"

#include "AlignmentWorkflow.h"
#include "CoordinatorPublication.h"
#include "PlaybackTiming.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace dvs::application {
namespace {

using namespace std::chrono_literals;

constexpr auto kExactFrameDeadline = 5s;
// Decode/transfer/render are prepared shortly before the canonical boundary. Without this lead,
// requesting only at the boundary makes every presentation round-trip permanently late and turns
// ordinary scheduler jitter into whole-FrameSet catch-up drops.
constexpr auto kPlaybackPresentationLead = 14ms;
constexpr auto kMinimumPlaybackPreparationDelay = 1ms;
constexpr auto kPlaybackProjectionInterval = 33ms;
// Preserve every canonical frame through short decoder/driver stalls. The renderer can drain the
// prepared successors faster than source cadence on a high-refresh display. The tolerance must
// cover the occasional sub-second render-to-ack stalls observed when the process is throttled to a
// few BelowNormal cores (the hardware performance gate runs this way): at 60 fps a 600 ms stall is
// only ~36 frames, and skipping them to recover the wall-clock anchor discards exactly the material
// a review tool exists to inspect. Only a stall beyond this tolerance skips complete FrameSets to
// recover the wall-clock anchor; a sustained shortfall still accumulates past it and is caught.
constexpr auto kPlaybackCatchUpTolerance = 2000ms;
// A held forward step presents every intermediate frame, so the user input target may lead the
// displayed frame by this many frames before the stream reports Busy. This is an input lookahead
// bound, not a decoder cache size: the provider still keeps 1 current + 1 prepared in flight and
// read-aheads internally. Bounding the input prevents an OS auto-repeat from queuing a long tail
// of work that outlives the key release.
constexpr auto kInteractiveStepInputLookahead = 3U;
// Why an active interactive forward-step stream is being torn down. The cancel path (navigation
// superseded, play, session/device change, shutdown) completes pending step commands as
// Canceled/Closed and does NOT set lastError: these are normal navigation events, not coordinator
// errors. The failure path (provider failure, render closed, presentation timeout, mismatched
// FrameSet, unrecoverable graphics failure) sets lastError and fails pending commands. The two
// paths share teardown (cancel timers/provider, clear render, advance generation) but differ on
// lastError and command outcome, which is why stopInteractiveStepRun(error) was split.
enum class InteractiveStepStopReason {
    NavigationSuperseded,
    PlaybackStarted,
    SessionChanged,
    DeviceChanged,
    Shutdown,
};
enum class PendingPhase {
    kOpeningProvider,
    kOpeningFirstFrame,
    kSeekingFrame,
    kClosingProvider,
};

struct CommandIdentity final {
    std::uint64_t sessionId = 0;
    std::uint64_t sessionEpoch = 0;
    std::uint64_t commandId = 0;

    [[nodiscard]] constexpr bool operator==(const CommandIdentity&) const noexcept = default;
};

struct CommandIdentityHash final {
    [[nodiscard]] std::size_t operator()(const CommandIdentity& identity) const noexcept {
        const auto mix = [](const std::size_t seed, const std::uint64_t value) noexcept {
            return seed ^ (std::hash<std::uint64_t>{}(value) + 0x9e3779b97f4a7c15ULL +
                           (seed << 6U) + (seed >> 2U));
        };
        return mix(mix(std::hash<std::uint64_t>{}(identity.sessionId), identity.sessionEpoch),
                   identity.commandId);
    }
};

[[nodiscard]] domain::MediaError probeCoordinatorError(const domain::MediaErrorCode code,
                                                       std::optional<domain::SourceId> source,
                                                       std::string technicalDetail,
                                                       const bool recoverable = true) {
    return domain::makeMediaError(
        code, domain::MediaOperation::kMediaProbe, source, recoverable, std::move(technicalDetail));
}

[[nodiscard]] domain::MediaError coordinatorError(const domain::MediaErrorCode code,
                                                  std::string technicalDetail,
                                                  const bool recoverable = true) {
    return domain::makeMediaError(code,
                                  domain::MediaOperation::kMediaDecode,
                                  std::nullopt,
                                  recoverable,
                                  std::move(technicalDetail));
}

[[nodiscard]] domain::MediaError presentationError(std::string technicalDetail) {
    return domain::makeMediaError(domain::MediaErrorCode::kFramePresentationTimedOut,
                                  domain::MediaOperation::kFramePresentation,
                                  std::nullopt,
                                  true,
                                  std::move(technicalDetail));
}

[[nodiscard]] bool matchesContext(const EventContext& context,
                                  const PlaybackRequestContext& expected) noexcept {
    const auto* const actual = std::get_if<PlaybackRequestContext>(&context);
    return actual != nullptr && *actual == expected;
}

[[nodiscard]] bool matchesContext(const EventContext& context,
                                  const FrameRequestContext& expected) noexcept {
    const auto* const actual = std::get_if<FrameRequestContext>(&context);
    return actual != nullptr && *actual == expected;
}

[[nodiscard]] std::optional<TraceIncomingIdentity>
makeIncomingIdentity(const EventContext& context) noexcept {
    if (const auto* const frame = std::get_if<FrameRequestContext>(&context)) {
        return TraceIncomingIdentity{
            .session = frame->playback.request.sessionId,
            .epoch = frame->playback.request.sessionEpoch,
            .generation = frame->playback.playbackGeneration,
            .device = frame->deviceGeneration,
            .request = frame->playback.request.requestId,
        };
    }
    if (const auto* const playback = std::get_if<PlaybackRequestContext>(&context)) {
        return TraceIncomingIdentity{
            .session = playback->request.sessionId,
            .epoch = playback->request.sessionEpoch,
            .generation = playback->playbackGeneration,
            .device = domain::DeviceGeneration{0U},
            .request = playback->request.requestId,
        };
    }
    if (const auto* const request = std::get_if<RequestContext>(&context)) {
        return TraceIncomingIdentity{
            .session = request->sessionId,
            .epoch = request->sessionEpoch,
            .generation = domain::PlaybackGeneration{0U},
            .device = domain::DeviceGeneration{0U},
            .request = request->requestId,
        };
    }
    return std::nullopt;
}

// Checked signed 64-bit addition. Returns the sum, or nullopt when a + b would over/underflow
// the int64 range, so callers never commit signed-overflow UB into a MediaTime or a tick delta.
class CoordinatorEventTarget {
public:
    virtual ~CoordinatorEventTarget() = default;

    [[nodiscard]] virtual EventPostResult postCritical(ApplicationEvent event) noexcept = 0;
    [[nodiscard]] virtual EventPostResult postRealtime(ApplicationEvent event) noexcept = 0;
    virtual void closeRealtimeIngress() noexcept = 0;
    virtual void closeCriticalIngress() noexcept = 0;
};

// Async adapters may retain this gate, but the gate never owns its coordinator target. detach()
// first prevents new calls and then waits for calls that already crossed the gate, making target
// destruction safe without allowing an adapter worker to own the coordinator itself.
class CoordinatorEventSinkGate final : public IApplicationEventSink {
public:
    explicit CoordinatorEventSinkGate(CoordinatorEventTarget& target) noexcept : target_(&target) {}

    [[nodiscard]] EventPostResult postCritical(ApplicationEvent event) noexcept override {
        CoordinatorEventTarget* const target = enter();
        if (target == nullptr) {
            return EventPostResult::Closed;
        }
        const EventPostResult result = target->postCritical(std::move(event));
        leave();
        return result;
    }

    [[nodiscard]] EventPostResult postRealtime(ApplicationEvent event) noexcept override {
        CoordinatorEventTarget* const target = enter();
        if (target == nullptr) {
            return EventPostResult::Closed;
        }
        const EventPostResult result = target->postRealtime(std::move(event));
        leave();
        return result;
    }

    void closeRealtimeIngress() noexcept override {
        CoordinatorEventTarget* const target = enter();
        if (target == nullptr) {
            return;
        }
        target->closeRealtimeIngress();
        leave();
    }

    void closeCriticalIngress() noexcept override {
        CoordinatorEventTarget* const target = enter();
        if (target == nullptr) {
            return;
        }
        target->closeCriticalIngress();
        leave();
    }

    void detach() noexcept {
        std::unique_lock lock(mutex_);
        target_ = nullptr;
        idle_.wait(lock, [this] { return activeCalls_ == 0U; });
    }

private:
    [[nodiscard]] CoordinatorEventTarget* enter() noexcept {
        std::scoped_lock lock(mutex_);
        if (target_ == nullptr) {
            return nullptr;
        }
        ++activeCalls_;
        return target_;
    }

    void leave() noexcept {
        std::scoped_lock lock(mutex_);
        --activeCalls_;
        if (activeCalls_ == 0U) {
            idle_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable idle_;
    CoordinatorEventTarget* target_ = nullptr;
    std::size_t activeCalls_ = 0U;
};

} // namespace

class PlaybackCoordinator::Impl final : public CoordinatorEventTarget {
public:
    Impl(const domain::SessionId sessionId, Dependencies dependencies)
        : dependencies_(std::move(dependencies)),
          eventSink_(std::make_shared<CoordinatorEventSinkGate>(*this)) {
        state_.sessionId = sessionId;
        publishSnapshot();
        worker_ = std::thread([this] { run(); });
    }

    ~Impl() {
        shutdown();
    }

    [[nodiscard]] std::shared_ptr<IApplicationEventSink> eventSink() const noexcept {
        return eventSink_;
    }

    void shutdown() noexcept {
        shutdownImpl();
    }

    [[nodiscard]] PortSubmitResult submit(PlaybackCommand command) {
        {
            std::scoped_lock lock(ingressMutex_);
            if (shuttingDown_) {
                return PortSubmitResult::Closed;
            }
            if (commands_.size() >= kCommandIngressCapacity) {
                return PortSubmitResult::Busy;
            }
            commands_.push_back(std::move(command));
        }
        condition_.notify_one();
        return PortSubmitResult::Accepted;
    }

    [[nodiscard]] std::shared_ptr<const SessionSnapshot> snapshot() const {
        return publication_.snapshot();
    }

    [[nodiscard]] std::vector<CommandTerminal> takeCompletedCommands() {
        return publication_.takeCompletedCommands();
    }

    [[nodiscard]] std::shared_ptr<const std::vector<SequenceAlignmentResult>>
    acceptedSequenceAlignments() const {
        return publication_.acceptedSequenceAlignments();
    }

    [[nodiscard]] EventPostResult postCritical(ApplicationEvent event) noexcept override {
        {
            std::unique_lock lock(ingressMutex_);
            // A critical event is a terminal or a complete exact frame set. It may wait for bounded
            // queue space, but it must never be discarded merely because the UI is momentarily
            // behind; callers only see Closed once coordinator teardown starts.
            condition_.wait(lock, [this] {
                return shuttingDown_ || criticalIngressClosed_ ||
                       criticalEvents_.size() < kCriticalEventCapacity;
            });
            if (shuttingDown_ || criticalIngressClosed_) {
                return EventPostResult::Closed;
            }
            criticalEvents_.push_back(std::move(event));
        }
        condition_.notify_one();
        return EventPostResult::Accepted;
    }

    [[nodiscard]] EventPostResult postRealtime(ApplicationEvent event) noexcept override {
        {
            std::scoped_lock lock(ingressMutex_);
            if (shuttingDown_ || realtimeIngressClosed_) {
                return EventPostResult::Closed;
            }
            realtimeEvent_ = std::move(event);
        }
        condition_.notify_one();
        return EventPostResult::Accepted;
    }

    void closeRealtimeIngress() noexcept override {
        std::scoped_lock lock(ingressMutex_);
        realtimeIngressClosed_ = true;
        realtimeEvent_.reset();
    }

    void closeCriticalIngress() noexcept override {
        {
            std::scoped_lock lock(ingressMutex_);
            criticalIngressClosed_ = true;
        }
        condition_.notify_all();
    }

private:
    struct PendingCommand final {
        PendingPhase phase;
        CommandContext command;
        PlaybackRequestContext providerContext;
        std::optional<FrameRequestContext> frameContext;
        std::optional<FrameSet> set;
        std::optional<domain::FrameId> expectedFrame;
        std::optional<std::uint64_t> presentationTimerId;
        bool framePublished = false;
        bool providerSucceeded = false;
        bool framePresented = false;
        bool rollbackAttempt = false;
        std::optional<CommandOutcome> terminalOutcomeOverride;
        std::optional<domain::MediaError> terminalErrorOverride;
    };

    struct BackgroundAnalysis final {
        AlignmentAnalysisJobId jobId;
        AlignmentAnalysisKind kind;
        CommandContext command;
        PlaybackRequestContext context;
        std::optional<std::vector<GlobalOffsetEstimate>> estimates;
        std::optional<std::vector<SequenceAlignmentResult>> sequenceResults;
        bool completed = false;
    };

    struct AutomaticAlignmentProposal final {
        AlignmentAnalysisKind kind = AlignmentAnalysisKind::GlobalOffset;
        std::vector<GlobalOffsetEstimate> estimates;
        std::vector<SequenceAlignmentResult> sequenceResults;
    };

    struct AutomaticAlignmentUndoState final {
        std::vector<SourceFrameOffset> offsets;
        std::vector<SequenceAlignmentResult> sequenceMaps;
        bool alignmentRequired = false;
    };

    struct ReadySessionBackup final {
        SessionSnapshot snapshot;
        domain::ValidatedComparisonSet sources;
        domain::CompatibilityReport compatibilityReport;
        std::vector<SourceFrameOffset> alignmentOffsets;
        std::vector<SequenceAlignmentResult> sequenceAlignmentMaps;
        domain::CanonicalTimeline canonicalTimeline;
        std::optional<AutomaticAlignmentProposal> automaticAlignmentProposal;
        std::optional<AutomaticAlignmentUndoState> automaticAlignmentUndo;
        PrefetchScheduler prefetchScheduler;
    };

    struct PendingProbeSlot final {
        RequestContext context;
        domain::SourceId sourceId = 0;
        std::filesystem::path sourcePath;
        domain::ComparisonRole role = domain::ComparisonRole::kPrediction;
        std::string displayName;
        std::optional<domain::MediaDescriptor> descriptor;
        // VFR probes publish the shared canonical-source timeline; CFR probes leave this nullopt.
        // Captured on first arrival so a stale duplicate ProbeCompleted cannot overwrite a good
        // value.
        std::optional<std::shared_ptr<const domain::FrameTimeline>> timeline;
        bool succeeded = false;
    };

    struct PendingProbe final {
        CommandContext command;
        std::vector<PendingProbeSlot> slots;
        std::optional<domain::MediaTime> resumeTime;
        bool preservesReadySession = false;
    };

    struct PendingPlaybackFrame final {
        FrameRequestContext context;
        domain::FrameId expectedFrame;
        std::optional<FrameSet> set;
        std::optional<std::uint64_t> presentationTimerId;
        bool presentationRequested = false;
        bool framePublished = false;
        bool providerSucceeded = false;
        bool framePresented = false;
    };

    struct PlaybackRun final {
        // D06: monotonic run id for trace correlation across recycled frame numbers.
        std::uint64_t runId = 0U;
        PlaybackRequestContext providerContext;
        PlaybackRequestContext cadenceContext;
        domain::FrameId firstTarget;
        domain::FrameId nextMinimum;
        domain::FrameId anchorFrame;
        std::chrono::steady_clock::time_point wallAnchor;
        // Visual playback rate for this run. The cadence scales wall-clock time by 1/speed:
        // due(n) = wallAnchor + (timeline(n) - timeline(anchor)) / speed, and the catch-up
        // projection maps elapsed wall time back through speed. 1.0 keeps the historical
        // real-time behavior bit-identical because the division is by exactly 1.0.
        double speed = 1.0;
        std::optional<std::uint64_t> cadenceTimerId;
        std::optional<domain::FrameId> cadenceTarget;
        std::optional<PendingPlaybackFrame> frame;
        std::optional<PendingPlaybackFrame> preparedFrame;
        bool restartFromEnd = false;
        bool pauseRequested = false;
        // Snapshot of session playback-range authority at run start (updated live by
        // SetPlaybackRangeCommand). Targets must never leave this closed interval.
        std::optional<PlaybackRange> range;
        bool rangeLoop = false;
        std::uint64_t completedLoops = 0U;
    };

    // A forward step stream: consecutive +1 steps that reuse the provider's Sequential decode path
    // so a held key presents every intermediate frame without the Exact-seek cancel/generation
    // storm the old beginStep()->beginSeek() path produced. Deliberately separate from PlaybackRun:
    // realtime playback may catch up to the wall clock and skip whole FrameSets; interactive
    // stepping must present every atomic frame and never skip, and it must not inherit
    // PlaybackRun's catch-up, cadence, or Play/Pause semantics.
    struct PendingInteractiveStep final {
        CommandContext command;
        PendingPlaybackFrame frame;
    };

    // Direction of an interactive ±1 step stream. Forward uses Sequential decode; Reverse uses
    // FrameRequestPriority::Reverse (exact/random-access at the provider) but still keeps one
    // generation and a current+prepared pipeline so held-backward does not cancel per press.
    enum class InteractiveStepDirection {
        Forward,
        Reverse,
    };

    struct InteractiveStepRun final {
        // Stable for the whole run. All frames in one run share this generation, so the provider's
        // Sequential cursor / reverse-window cache survive frame to frame.
        PlaybackRequestContext providerContext;
        InteractiveStepDirection direction = InteractiveStepDirection::Forward;

        // The frame currently committed to the renderer / waiting on provider + presentation.
        std::optional<PendingInteractiveStep> frame;

        // The successor frame already requested from the renderer but not yet current.
        std::optional<PendingInteractiveStep> preparedFrame;

        // Commands whose targets are beyond the prepared slot, accepted but not yet in the
        // pipeline.
        std::deque<CommandContext> queuedCommands;

        // The newest target the user input has been allowed to request. requestedFrame is projected
        // from this so the UI "frame pending" indicator stays correct across the stream.
        domain::FrameId lastQueuedTarget;
    };

    using WorkItem = std::variant<PlaybackCommand, ApplicationEvent>;

    [[nodiscard]] static domain::SessionEpoch increment(const domain::SessionEpoch value) noexcept {
        return domain::SessionEpoch{value.value() + 1U};
    }

    void clearAutomaticProposal() noexcept {
        automaticAlignmentProposal_.reset();
        state_.automaticAlignmentPending = false;
        state_.canConfirmAutomaticAlignment = false;
    }

    void clearAutomaticUndo() noexcept {
        automaticAlignmentUndo_.reset();
        state_.canUndoAutomaticAlignment = false;
    }

    void invalidateAutomaticAlignmentHistory() noexcept {
        clearAutomaticProposal();
        clearAutomaticUndo();
    }

    [[nodiscard]] static domain::PlaybackGeneration
    increment(const domain::PlaybackGeneration value) noexcept {
        return domain::PlaybackGeneration{value.value() + 1U};
    }

    [[nodiscard]] static std::uint64_t increment(const std::uint64_t value) noexcept {
        return value + 1U;
    }

    [[nodiscard]] static domain::TopologyRevision
    increment(const domain::TopologyRevision value) noexcept {
        return domain::TopologyRevision{value.value() + 1U};
    }

    [[nodiscard]] static domain::TimelineRevision
    increment(const domain::TimelineRevision value) noexcept {
        return domain::TimelineRevision{value.value() + 1U};
    }

    [[nodiscard]] bool acceptsCommand(const CommandContext& context) const noexcept {
        return context.sessionId == state_.sessionId && context.sessionEpoch == state_.sessionEpoch;
    }

    [[nodiscard]] PlaybackRequestContext makePlaybackContext() {
        return PlaybackRequestContext{
            .request =
                RequestContext{
                    .sessionId = state_.sessionId,
                    .sessionEpoch = state_.sessionEpoch,
                    .requestId = domain::RequestId{nextRequestId_++},
                },
            .playbackGeneration = state_.playbackGeneration,
        };
    }

    [[nodiscard]] RequestContext makeRequestContext() {
        return RequestContext{
            .sessionId = state_.sessionId,
            .sessionEpoch = state_.sessionEpoch,
            .requestId = domain::RequestId{nextRequestId_++},
        };
    }

    // Builds the current trace identity from coordinator state. Carries every revision the plan's
    // OperationIdentity requires (03§4) so a trace observer can detect a stale commit: any event
    // whose identity revisions differ from the live state was produced against a superseded
    // topology/timeline/generation and must not have committed.
    [[nodiscard]] TraceIdentity
    makeTraceIdentity(std::optional<domain::CommandId> command = {}) const noexcept {
        return TraceIdentity{
            .session = state_.sessionId,
            .epoch = state_.sessionEpoch,
            .topology = topologyRevision_,
            .timeline = timelineRevision_,
            .alignment = domain::AlignmentRevision{state_.alignmentRevision},
            .generation = state_.playbackGeneration,
            .device = state_.deviceGeneration,
            .request = domain::RequestId{0},
            .command = command,
            .run = playbackRun_.has_value() ? playbackRun_->runId : 0U,
        };
    }

    // Command terminals reference the identity under which the command was accepted, not the
    // live state. An open command accepted in the initial epoch may complete only after the
    // first open bumps the epoch; emitting its terminal under the live epoch would orphan both
    // the acceptance and the terminal and break the command exactly-once invariant (03§4).
    [[nodiscard]] TraceIdentity commandTraceIdentity(const CommandContext& context) const noexcept {
        auto identity = makeTraceIdentity(context.commandId);
        identity.epoch = context.sessionEpoch;
        return identity;
    }

    void emitTrace(TraceEventKind kind,
                   const TraceIdentity& identity,
                   std::uint64_t payload = 0U,
                   std::optional<TraceIncomingIdentity> incoming = std::nullopt) {
        PlaybackTrace::instance().record(kind, identity, payload, incoming);
    }

    [[nodiscard]] PlaybackRequestContext currentPlaybackScope() const noexcept {
        return PlaybackRequestContext{
            .request =
                RequestContext{
                    .sessionId = state_.sessionId,
                    .sessionEpoch = state_.sessionEpoch,
                    .requestId = domain::RequestId{0},
                },
            .playbackGeneration = state_.playbackGeneration,
        };
    }

    [[nodiscard]] std::vector<SourceFrameOffset>
    sourceMappingsFor(const domain::FrameId canonicalFrame) const {
        std::vector<SourceFrameOffset> mappings = alignmentOffsets_;
        for (const SequenceAlignmentResult& map : sequenceAlignmentMaps_) {
            if (!canonicalFrame.isValid() ||
                static_cast<std::size_t>(canonicalFrame.value()) >= map.entries.size()) {
                continue;
            }
            const auto segment =
                std::find_if(map.segments.begin(),
                             map.segments.end(),
                             [canonicalFrame](const SequenceAlignmentSegment& value) {
                                 return value.firstCanonicalFrame <= canonicalFrame &&
                                        canonicalFrame <= value.lastCanonicalFrame;
                             });
            if (segment != map.segments.end() &&
                segment->state != AlignmentSegmentState::Accepted) {
                continue;
            }
            const SequenceAlignmentEntry& entry =
                map.entries[static_cast<std::size_t>(canonicalFrame.value())];
            if (entry.canonicalFrameId != canonicalFrame) {
                continue;
            }
            SourceFrameOffset mapping{
                .sourceId = map.sourceId,
                .frames = 0,
                .matchKind = entry.matchKind,
                .confidence = entry.confidence,
            };
            if (entry.sourceFrameId.has_value()) {
                mapping.frames = entry.sourceFrameId->value() - canonicalFrame.value();
            } else {
                mapping.matchKind = FrameMatchKind::Missing;
            }
            const auto existing = std::find_if(
                mappings.begin(), mappings.end(), [&map](const SourceFrameOffset& offset) {
                    return offset.sourceId == map.sourceId;
                });
            if (existing == mappings.end()) {
                mappings.push_back(mapping);
            } else {
                *existing = mapping;
            }
        }
        if (sources_.has_value()) {
            for (const SourceAlignmentAnchors& anchors : state_.manualAlignmentAnchors) {
                const domain::ComparisonSource* const source = sources_->find(anchors.sourceId);
                if (source == nullptr) {
                    continue;
                }
                const auto mapping = mapFrameWithAnchors(
                    anchors, canonicalFrame, source->descriptor.frameCount.value);
                if (!mapping.has_value()) {
                    continue;
                }
                const auto existing = std::find_if(
                    mappings.begin(), mappings.end(), [&anchors](const SourceFrameOffset& offset) {
                        return offset.sourceId == anchors.sourceId;
                    });
                if (existing == mappings.end()) {
                    mappings.push_back(*mapping);
                } else {
                    *existing = *mapping;
                }
            }
        }
        return mappings;
    }

    void submitPrefetch(const std::vector<domain::FrameId>& targets) {
        if (!sources_.has_value()) {
            return;
        }
        for (const domain::FrameId target : targets) {
            const FrameRequest request{
                .context =
                    FrameRequestContext{
                        .playback = makePlaybackContext(),
                        .deviceGeneration = state_.deviceGeneration,
                    },
                .frameId = target,
                .priority = FrameRequestPriority::Prefetch,
                .sourceOffsets = sourceMappingsFor(target),
                .alignmentRevision = state_.alignmentRevision,
            };
            if (dependencies_.directFrameProvider->submit(request, eventSink_) !=
                PortSubmitResult::Accepted) {
                break;
            }
        }
    }

    struct ExactPrefetchWindow final {
        std::size_t ahead = 3U;
        std::size_t behind = 1U;
    };

    [[nodiscard]] ExactPrefetchWindow exactPrefetchWindow() const noexcept {
        if (!canonicalTimeline_.has_value()) {
            return {};
        }
        const auto* const rate = std::get_if<domain::RationalRate>(&*canonicalTimeline_);
        if (rate == nullptr) {
            return {};
        }
        const double fps = rate->displayFps();
        if (fps >= 100.0) {
            // One long GOP can contain hundreds of frames at high rates. Starting even one
            // speculative decode after every exact navigation makes the next navigation cancel
            // and reopen that decoder, which dominates random-seek tail latency.
            return {.ahead = 0U, .behind = 0U};
        }
        // At 50/60 FPS, one exact successor is enough to keep a warm step responsive. A wider
        // speculative window competes with random navigation and repeatedly interrupts FFmpeg
        // while it is reading long-GOP sources.
        return fps >= 50.0 ? ExactPrefetchWindow{.ahead = 1U, .behind = 0U} : ExactPrefetchWindow{};
    }

    void publishSnapshot(const bool notify = true) {
        state_.alignmentOffsets = alignmentOffsets_;
        state_.canonicalTimeline = canonicalTimeline_;
        state_.playbackSpeed =
            playbackRun_.has_value() ? playbackRun_->speed : pendingPlaybackSpeed_.value_or(1.0);
        state_.playbackContinuityPolicy = playbackContinuityPolicy_;
        state_.playbackContinuityPolicyEffective = domain::resolveContinuityPolicy(
            playbackContinuityPolicy_, static_cast<std::size_t>(state_.sources.size()));
        state_.playbackSkippedFrameSets = playbackSkippedFrameSets_;
        state_.activeComparisonPair = activeComparisonPair_;
        state_.playbackRangeIn =
            playbackRange_.has_value() ? std::optional{playbackRange_->inInclusive} : std::nullopt;
        state_.playbackRangeOut =
            playbackRange_.has_value() ? std::optional{playbackRange_->outInclusive} : std::nullopt;
        state_.playbackRangeLoop = playbackRangeLoop_ && playbackRange_.has_value();
        state_.playbackRangeLoopActive =
            playbackRun_.has_value() && playbackRange_.has_value() && playbackRangeLoop_;
        state_.playbackRangeCompletedLoops =
            playbackRun_.has_value() ? playbackRun_->completedLoops : playbackRangeCompletedLoops_;
        // Snapshot committed: carries the displayed frame as payload so a trace can verify the
        // frame only advanced after a matching PresentationACK (ACK-before-commit invariant).
        const std::uint64_t displayed =
            state_.displayedFrame.has_value()
                ? static_cast<std::uint64_t>(state_.displayedFrame->value())
                : UINT64_MAX;
        // Emit a commit only when the displayed canonical position actually changed. Re-committing
        // an unchanged frame after the generation advanced (e.g. interactive-step successor arming)
        // is unrepresentable in the trace contract: a newer identity has no ACK for the older frame
        // (ACK-before-commit) and the older identity is stale once a higher revision was observed
        // (no stale commits). The canvas state publication below still happens on every notify.
        if (state_.displayedFrame != lastCommittedDisplayedFrame_) {
            emitTrace(TraceEventKind::SnapshotCommitted, makeTraceIdentity(), displayed);
            lastCommittedDisplayedFrame_ = state_.displayedFrame;
        }
        publication_.publish(state_, sequenceAlignmentMaps_);
        if (notify) {
            notifyStatePublished();
        }
    }

    void publishPlaybackSnapshot() {
        const auto now = dependencies_.clock->now();
        const bool notify = !lastPlaybackProjectionAt_.has_value() ||
                            now - *lastPlaybackProjectionAt_ >= kPlaybackProjectionInterval;
        publishSnapshot(notify);
        if (notify) {
            lastPlaybackProjectionAt_ = now;
        }
    }

    void completeCommand(const CommandContext& context,
                         const CommandOutcome outcome,
                         std::optional<domain::MediaError> error = std::nullopt) {
        emitTrace(TraceEventKind::CommandTerminal,
                  commandTraceIdentity(context),
                  static_cast<std::uint64_t>(outcome));
        publication_.complete(CommandTerminal{
            .context = context,
            .outcome = outcome,
            .error = std::move(error),
        });
        notifyStatePublished();
    }

    void notifyStatePublished() noexcept {
        if (!dependencies_.statePublished) {
            return;
        }
        try {
            dependencies_.statePublished();
        } catch (...) {
            // Publishing state cannot be allowed to fail coordinator work.
        }
    }

    void publishError(const domain::MediaError& error) {
        state_.lastError = error;
        publishSnapshot();
    }

    [[nodiscard]] bool claimCommand(const CommandContext& context) {
        return seenCommands_
            .insert(CommandIdentity{
                .sessionId = context.sessionId.value(),
                .sessionEpoch = context.sessionEpoch.value(),
                .commandId = context.commandId.value(),
            })
            .second;
    }

    void rejectCommand(const CommandContext& context,
                       const CommandOutcome outcome,
                       domain::MediaError error) {
        emitTrace(TraceEventKind::CommandRejected,
                  commandTraceIdentity(context),
                  static_cast<std::uint64_t>(outcome));
        publishError(error);
        completeCommand(context, outcome, std::move(error));
    }

    void resetToEmpty() {
        state_.sessionState = domain::SessionState::kEmpty;
        state_.playbackState = domain::PlaybackState::kPaused;
        state_.displayedFrame.reset();
        state_.requestedFrame.reset();
        state_.canonicalFrameCount = 0U;
        state_.sources.clear();
        state_.validatedComparison.reset();
        playbackRange_.reset();
        playbackRangeLoop_ = false;
        playbackRangeCompletedLoops_ = 0U;
        state_.presentedSources.clear();
        state_.alignmentEstimates.clear();
        state_.sequenceAlignments.clear();
        state_.alignmentRevision = 0U;
        state_.alignmentAnalysisJobId.reset();
        state_.alignmentAnalysisKind.reset();
        state_.alignmentAnalysisPhase.reset();
        state_.alignmentAnalysisCompletedUnits = 0U;
        state_.alignmentAnalysisWork = {};
        state_.manualAlignmentAnchors.clear();
        state_.compatibilityFindings.clear();
        state_.alignmentRequired = false;
        invalidateAutomaticAlignmentHistory();
        state_.lastError.reset();
        sources_.reset();
        compatibilityReport_.reset();
        alignmentOffsets_.clear();
        sequenceAlignmentMaps_.clear();
        canonicalTimeline_.reset();
        prefetchScheduler_.reset();
    }

    void captureReadySessionForOpenRollback() {
        if (state_.sessionState != domain::SessionState::kReady || !sources_.has_value() ||
            !compatibilityReport_.has_value() || !canonicalTimeline_.has_value() ||
            !state_.displayedFrame.has_value()) {
            openRollback_.reset();
            return;
        }
        openRollback_ = ReadySessionBackup{
            .snapshot = state_,
            .sources = *sources_,
            .compatibilityReport = *compatibilityReport_,
            .alignmentOffsets = alignmentOffsets_,
            .sequenceAlignmentMaps = sequenceAlignmentMaps_,
            .canonicalTimeline = *canonicalTimeline_,
            .automaticAlignmentProposal = automaticAlignmentProposal_,
            .automaticAlignmentUndo = automaticAlignmentUndo_,
            .prefetchScheduler = prefetchScheduler_,
        };
    }

    void failPending(domain::MediaError error, const CommandOutcome outcome) {
        if (!pending_.has_value()) {
            return;
        }
        const PendingCommand failed = std::move(*pending_);
        pending_.reset();

        if (failed.presentationTimerId.has_value()) {
            static_cast<void>(dependencies_.deadlineScheduler->cancel(*failed.presentationTimerId));
        }
        dependencies_.directFrameProvider->cancel(failed.providerContext);
        const bool failedExactFrame = failed.phase == PendingPhase::kOpeningFirstFrame ||
                                      failed.phase == PendingPhase::kSeekingFrame;
        if (failedExactFrame && failed.frameContext.has_value()) {
            dependencies_.renderChannel->clear(failed.providerContext);
            state_.playbackGeneration = increment(state_.playbackGeneration);
        }

        const bool failedOpen = failed.phase == PendingPhase::kOpeningProvider ||
                                failed.phase == PendingPhase::kOpeningFirstFrame;
        if (failedOpen && !failed.rollbackAttempt && openRollback_.has_value()) {
            std::optional<domain::MediaTime> resumeTime;
            if (openRollback_->snapshot.displayedFrame.has_value()) {
                auto time = domain::canonicalFrameStartTime(
                    openRollback_->canonicalTimeline, *openRollback_->snapshot.displayedFrame);
                if (time) {
                    resumeTime = time.value();
                }
            }
            beginOpenValidated(failed.command,
                               openRollback_->sources,
                               openRollback_->compatibilityReport,
                               openRollback_->canonicalTimeline,
                               resumeTime,
                               true,
                               outcome,
                               error);
            return;
        }

        const CommandOutcome terminalOutcome = failed.terminalOutcomeOverride.value_or(outcome);
        domain::MediaError terminalError = failed.terminalErrorOverride.value_or(std::move(error));
        openRollback_.reset();

        if (failed.phase == PendingPhase::kOpeningProvider ||
            failed.phase == PendingPhase::kOpeningFirstFrame ||
            failed.phase == PendingPhase::kClosingProvider) {
            sources_.reset();
            state_.validatedComparison.reset();
            compatibilityReport_.reset();
            alignmentOffsets_.clear();
            state_.alignmentEstimates.clear();
            state_.sequenceAlignments.clear();
            state_.alignmentRevision = 0U;
            state_.manualAlignmentAnchors.clear();
            sequenceAlignmentMaps_.clear();
            invalidateAutomaticAlignmentHistory();
            canonicalTimeline_.reset();
            state_.sessionState = domain::SessionState::kError;
            state_.playbackState = domain::PlaybackState::kPaused;
            state_.displayedFrame.reset();
            state_.requestedFrame.reset();
            state_.canonicalFrameCount = 0U;
            state_.presentedSources.clear();
            state_.sources.clear();
            state_.compatibilityFindings.clear();
            state_.alignmentRequired = false;
        } else {
            state_.sessionState = domain::SessionState::kReady;
            state_.playbackState = domain::PlaybackState::kPaused;
            state_.requestedFrame.reset();
        }
        state_.lastError = terminalError;
        publishSnapshot();
        completeCommand(failed.command, terminalOutcome, std::move(terminalError));
    }

    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
    playbackDue(const domain::FrameId target) const {
        if (!playbackRun_.has_value() || !canonicalTimeline_.has_value() ||
            target < playbackRun_->firstTarget) {
            return std::nullopt;
        }
        // due(n) = wallAnchor + timelineTime(n) - timelineTime(anchorFrame). The two timeline
        // lookups are independent and checked, the per-frame offset is computed with checked exact
        // microsecond arithmetic, and the addition to the wall anchor is checked against the
        // clock's representable range, so any unrepresentable due falls through to the existing
        // arithmetic-overflow path rather than committing signed-overflow/time-point-overflow UB.
        const auto startTarget = domain::canonicalFrameStartTime(*canonicalTimeline_, target);
        const auto startAnchor =
            domain::canonicalFrameStartTime(*canonicalTimeline_, playbackRun_->anchorFrame);
        if (!startTarget || !startAnchor) {
            return std::nullopt;
        }
        const auto deltaMicroseconds = detail::checkedSubtract(startTarget.value().microseconds(),
                                                               startAnchor.value().microseconds());
        if (!deltaMicroseconds.has_value()) {
            return std::nullopt;
        }
        // Speed divides the media-time delta: 2x halves the wall time between frames, 0.5x
        // doubles it. The speed comes from a validated positive ladder, so the scaled delta
        // stays finite; it is re-clamped into int64 through the checked helper below.
        const double scaled = static_cast<double>(*deltaMicroseconds) / playbackRun_->speed;
        if (!std::isfinite(scaled) ||
            scaled > static_cast<double>(std::numeric_limits<int64_t>::max()) ||
            scaled < static_cast<double>(std::numeric_limits<int64_t>::min())) {
            return std::nullopt;
        }
        const auto scaledMicroseconds = static_cast<std::int64_t>(scaled);
        return detail::addDuration(playbackRun_->wallAnchor,
                                   std::chrono::microseconds{scaledMicroseconds});
    }

    [[nodiscard]] std::int64_t playbackCeiling(const PlaybackRun& run) const {
        std::int64_t maximum = state_.canonicalFrameCount == 0U
                                   ? 0
                                   : static_cast<std::int64_t>(state_.canonicalFrameCount - 1U);
        if (run.range.has_value()) {
            maximum = (std::min)(maximum, run.range->outInclusive.value());
        }
        return maximum;
    }

    [[nodiscard]] std::int64_t playbackFloor(const PlaybackRun& run) const {
        return run.range.has_value() ? run.range->inInclusive.value() : 0;
    }

    [[nodiscard]] domain::FrameId
    playbackTargetAt(const std::chrono::steady_clock::time_point now) const {
        if (!playbackRun_.has_value() || !canonicalTimeline_.has_value()) {
            return domain::FrameId{0};
        }
        const PlaybackRun& run = *playbackRun_;
        const std::int64_t maximum = playbackCeiling(run);
        const std::int64_t minimum = playbackFloor(run);
        const domain::PlaybackContinuityPolicy effective = domain::resolveContinuityPolicy(
            playbackContinuityPolicy_, static_cast<std::size_t>(state_.sources.size()));
        // C-07 ReviewEveryFrame: never skip whole FrameSets to recover wall-clock. The cadence
        // slips instead; dropped-frame stats stay zero under this policy.
        if (effective == domain::PlaybackContinuityPolicy::ReviewEveryFrame) {
            return domain::FrameId{
                (std::min)(maximum, (std::max)(minimum, run.nextMinimum.value()))};
        }
        if (const auto nextDue = playbackDue(run.nextMinimum); nextDue.has_value()) {
            const auto catchUpDue = detail::addDuration(
                *nextDue,
                std::chrono::duration_cast<std::chrono::microseconds>(kPlaybackCatchUpTolerance));
            if (catchUpDue.has_value() && now <= *catchUpDue) {
                return domain::FrameId{
                    (std::min)(maximum, (std::max)(minimum, run.nextMinimum.value()))};
            }
        }
        // Catch-up: the media time that should be showing now is the anchored frame's time plus
        // the wall-clock elapsed since anchoring. canonicalFrameAtOrBefore maps it back to a
        // display-order frame, then we clamp so the cadence never regresses and never passes the
        // final frame or a session playback-range Out point.
        const auto startAnchor =
            domain::canonicalFrameStartTime(*canonicalTimeline_, run.anchorFrame);
        if (!startAnchor) {
            return domain::FrameId{
                (std::min)(maximum, (std::max)(minimum, run.nextMinimum.value()))};
        }
        // elapsedMicroseconds can be negative only on a non-monotonic clock read; anchor on zero
        // so the computed media time never walks backward past the anchor frame.
        const std::int64_t elapsedMicroseconds = std::max<std::int64_t>(
            0, std::chrono::duration_cast<std::chrono::microseconds>(now - run.wallAnchor).count());
        const std::uint64_t frameCount = state_.canonicalFrameCount;
        if (frameCount == 0) {
            return domain::FrameId{
                (std::min)(maximum, (std::max)(minimum, run.nextMinimum.value()))};
        }
        domain::FrameId frame = run.nextMinimum;
        // Catch-up scales elapsed wall time by speed before mapping back onto the timeline, the
        // exact inverse of playbackDue's division: at 2x two media seconds pass per wall second.
        // Overflow falls back to the final frame rather than accumulating through UB.
        const double scaledElapsed = static_cast<double>(elapsedMicroseconds) * run.speed;
        std::int64_t mediaElapsedMicroseconds = 0;
        if (!std::isfinite(scaledElapsed) ||
            scaledElapsed > static_cast<double>(std::numeric_limits<int64_t>::max())) {
            mediaElapsedMicroseconds = std::numeric_limits<std::int64_t>::max();
        } else {
            mediaElapsedMicroseconds = static_cast<std::int64_t>(scaledElapsed);
        }
        // The target media time is built from checked addition. Overflow means the wall clock has
        // advanced far past the media end, so fall back to the final frame rather than accumulate
        // through undefined arithmetic.
        const auto targetMedia =
            detail::checkedAdd(startAnchor.value().microseconds(), mediaElapsedMicroseconds);
        if (targetMedia.has_value()) {
            const auto atOrBefore = domain::canonicalFrameAtOrBefore(
                *canonicalTimeline_, domain::MediaTime{*targetMedia});
            if (atOrBefore) {
                frame = atOrBefore.value();
            }
        } else {
            frame = domain::FrameId{maximum};
        }
        return domain::FrameId{(std::min)(
            maximum, (std::max)(minimum, (std::max)(run.nextMinimum.value(), frame.value())))};
    }

    void stopPlayback(std::optional<domain::MediaError> error = std::nullopt,
                      const bool publish = true) {
        if (!playbackRun_.has_value()) {
            return;
        }
        PlaybackRun stopped = std::move(*playbackRun_);
        playbackRun_.reset();
        lastPlaybackProjectionAt_.reset();
        emitTrace(TraceEventKind::PlaybackRunStopped,
                  makeTraceIdentity(),
                  state_.displayedFrame.has_value()
                      ? static_cast<std::uint64_t>(state_.displayedFrame->value())
                      : UINT64_MAX);
        if (stopped.cadenceTimerId.has_value()) {
            static_cast<void>(dependencies_.deadlineScheduler->cancel(*stopped.cadenceTimerId));
        }
        if (stopped.frame.has_value() && stopped.frame->presentationTimerId.has_value()) {
            static_cast<void>(
                dependencies_.deadlineScheduler->cancel(*stopped.frame->presentationTimerId));
        }
        dependencies_.directFrameProvider->cancel(stopped.providerContext);
        if (stopped.frame.has_value() && stopped.frame->framePublished) {
            dependencies_.renderChannel->clear(stopped.providerContext);
        }
        state_.playbackGeneration = increment(state_.playbackGeneration);
        state_.playbackState = domain::PlaybackState::kPaused;
        state_.requestedFrame.reset();
        if (error.has_value()) {
            state_.lastError = std::move(error);
        }
        if (publish) {
            publishSnapshot();
        }
    }

    // --- Interactive forward step stream -------------------------------------------------
    // Consecutive +1 steps share one provider generation so the Sequential decode cursor and the
    // read-ahead cache survive frame to frame. The state machine mirrors PlaybackRun's per-frame
    // lifecycle (arm presentation, submit Sequential request, wait on FrameSetReady + presentation
    // ACK, commit, advance) but never catches up to the wall clock and never skips a frame.

    [[nodiscard]] bool matchesInteractiveStepFrame(const EventContext& context) const noexcept {
        return interactiveStepRun_.has_value() && interactiveStepRun_->frame.has_value() &&
               matchesContext(context, interactiveStepRun_->frame->frame.context);
    }

    [[nodiscard]] bool
    matchesInteractiveStepPreparedFrame(const EventContext& context) const noexcept {
        return interactiveStepRun_.has_value() && interactiveStepRun_->preparedFrame.has_value() &&
               matchesContext(context, interactiveStepRun_->preparedFrame->frame.context);
    }

    [[nodiscard]] PendingPlaybackFrame makeInteractiveStepFrame(const domain::FrameId target) {
        return PendingPlaybackFrame{
            .context =
                FrameRequestContext{
                    .playback = makePlaybackContext(),
                    .deviceGeneration = state_.deviceGeneration,
                },
            .expectedFrame = target,
        };
    }

    [[nodiscard]] PortSubmitResult submitInteractiveStepRequest(PendingPlaybackFrame& frame) {
        const bool reverse = interactiveStepRun_.has_value() &&
                             interactiveStepRun_->direction == InteractiveStepDirection::Reverse;
        const FrameRequest request{
            .context = frame.context,
            .frameId = frame.expectedFrame,
            .priority = reverse ? FrameRequestPriority::Reverse : FrameRequestPriority::Sequential,
            .sourceOffsets = sourceMappingsFor(frame.expectedFrame),
            .alignmentRevision = state_.alignmentRevision,
        };
        return dependencies_.directFrameProvider->submit(request, eventSink_);
    }

    // Arms the presentation deadline for the current interactive frame. Unlike playback there is no
    // cadence clock: the frame renders as soon as the provider publishes the set, and the deadline
    // only bounds how long we wait for the render ACK before failing.
    [[nodiscard]] bool armInteractiveStepPresentation(PendingInteractiveStep& step) {
        PendingPlaybackFrame& frame = step.frame;
        if (frame.presentationRequested) {
            return true;
        }
        const std::uint64_t timerId = nextTimerId_++;
        const DeadlineRequest deadline{
            .context = frame.context.playback,
            .timerId = timerId,
            .due = dependencies_.clock->now() + kExactFrameDeadline,
        };
        const PortSubmitResult deadlineResult =
            dependencies_.deadlineScheduler->schedule(deadline, eventSink_);
        if (deadlineResult != PortSubmitResult::Accepted) {
            // The presentation deadline scheduler is at capacity (transient backpressure): tear the
            // stream down as a normal cancel rather than surfacing a coordinator error.
            cancelInteractiveStepRun(InteractiveStepStopReason::NavigationSuperseded);
            return false;
        }
        frame.presentationTimerId = timerId;
        frame.presentationRequested = true;
        state_.playbackState = domain::PlaybackState::kSeeking;
        return true;
    }

    // Promotes the next queued command to the prepared slot and submits its request, so the
    // provider is already decoding the successor (forward) or predecessor (reverse) while the
    // current frame is presenting. The command is only popped once the request is accepted.
    void submitInteractiveStepSuccessor() {
        if (!interactiveStepRun_.has_value() || !interactiveStepRun_->frame.has_value()) {
            return;
        }
        if (interactiveStepRun_->preparedFrame.has_value() ||
            interactiveStepRun_->queuedCommands.empty()) {
            return;
        }
        const bool reverse = interactiveStepRun_->direction == InteractiveStepDirection::Reverse;
        const std::int64_t current = interactiveStepRun_->frame->frame.expectedFrame.value();
        const std::int64_t nextValue = reverse ? current - 1 : current + 1;
        if (nextValue < 0 || static_cast<std::uint64_t>(nextValue) >= state_.canonicalFrameCount) {
            return;
        }
        PendingPlaybackFrame frame = makeInteractiveStepFrame(domain::FrameId{nextValue});
        if (submitInteractiveStepRequest(frame) != PortSubmitResult::Accepted) {
            return;
        }
        interactiveStepRun_->preparedFrame = PendingInteractiveStep{
            .command = interactiveStepRun_->queuedCommands.front(),
            .frame = std::move(frame),
        };
        interactiveStepRun_->queuedCommands.pop_front();
    }

    void publishInteractiveStepFrameIfReady() {
        if (!interactiveStepRun_.has_value() || !interactiveStepRun_->frame.has_value()) {
            return;
        }
        PendingPlaybackFrame& frame = interactiveStepRun_->frame->frame;
        if (!frame.presentationRequested || !frame.set.has_value() || frame.framePublished) {
            return;
        }
        if (dependencies_.renderChannel->publish(frame.context, *frame.set) ==
            RenderPublishResult::Closed) {
            // The render channel closed before it could accept the frame set: a hard render
            // failure, not a normal navigation — surface it via lastError.
            failInteractiveStepRun(
                coordinatorError(domain::MediaErrorCode::kMediaDecodeFailed,
                                 "The render channel closed before it accepted the "
                                 "interactive step frame set."));
            return;
        }
        frame.framePublished = true;
        emitTrace(TraceEventKind::RenderPublished,
                  makeTraceIdentity(),
                  static_cast<std::uint64_t>(frame.set->canonicalFrameId().value()));
        commitInteractiveStepFrameIfComplete();
    }

    void publishInteractiveSnapshot() {
        const auto now = dependencies_.clock->now();
        const bool notify = !lastInteractiveProjectionAt_.has_value() ||
                            now - *lastInteractiveProjectionAt_ >= kPlaybackProjectionInterval;
        publishSnapshot(notify);
        if (notify) {
            lastInteractiveProjectionAt_ = now;
        }
    }

    // Commits the current interactive frame once it has a set, has rendered, the provider has
    // succeeded, and the render ACK has returned. Advances the pipeline (prepared -> current),
    // binds the next queued command as the new prepared, and completes the just-presented command.
    // On a clean drain (no current, no prepared, no queued commands) the stream ends without
    // touching the generation, leaving the provider's Sequential cursor warm for the next +1.
    void commitInteractiveStepFrameIfComplete() {
        if (!interactiveStepRun_.has_value() || !interactiveStepRun_->frame.has_value()) {
            return;
        }
        PendingInteractiveStep& step = *interactiveStepRun_->frame;
        PendingPlaybackFrame& frame = step.frame;
        if (!frame.set.has_value() || !frame.framePublished || !frame.providerSucceeded ||
            !frame.framePresented || !frame.presentationTimerId.has_value()) {
            return;
        }

        static_cast<void>(dependencies_.deadlineScheduler->cancel(*frame.presentationTimerId));
        const CommandContext command = step.command;
        const domain::FrameId displayedFrame = frame.expectedFrame;

        // Capture the presented sources from the frame being committed BEFORE promoting the
        // prepared frame into the current slot: the move-assignment below destroys the current
        // frame's PendingInteractiveStep, which `step`/`frame` reference.
        std::vector<PresentedSourceState> presentedSources;
        presentedSources.reserve(frame.set->sources().size());
        for (const MappedSourceFrame& source : frame.set->sources()) {
            presentedSources.push_back(PresentedSourceState{
                .sourceId = source.sourceId,
                .sourceFrameId = source.sourceFrameId,
                .matchKind = source.matchKind,
                .alignmentConfidence = source.alignmentConfidence,
                .missingReason = source.missingReason,
                .presentationTime = source.presentationTime,
            });
        }

        interactiveStepRun_->frame = std::move(interactiveStepRun_->preparedFrame);
        interactiveStepRun_->preparedFrame.reset();

        state_.presentedSources = std::move(presentedSources);
        state_.sessionState = domain::SessionState::kReady;
        state_.displayedFrame = displayedFrame;
        state_.lastError.reset();
        publishInteractiveSnapshot();
        completeCommand(command, CommandOutcome::Succeeded);

        if (interactiveStepRun_->frame.has_value()) {
            // The promoted successor becomes the new current frame: arm its presentation and render
            // it (its set and provider success already arrived while it was prepared), then top up
            // the prepared slot from the queue.
            if (!armInteractiveStepPresentation(*interactiveStepRun_->frame)) {
                return;
            }
            publishInteractiveStepFrameIfReady();
            submitInteractiveStepSuccessor();
            state_.requestedFrame = interactiveStepRun_->lastQueuedTarget;
            publishInteractiveSnapshot();
            return;
        }

        // No successor became current. If commands are still queued, the prepared slot had been
        // empty (its request was rejected earlier); submit the next queued target as the current
        // frame.
        if (!interactiveStepRun_->queuedCommands.empty()) {
            const bool reverse =
                interactiveStepRun_->direction == InteractiveStepDirection::Reverse;
            const std::int64_t nextValue =
                reverse ? displayedFrame.value() - 1 : displayedFrame.value() + 1;
            PendingInteractiveStep next{
                .command = interactiveStepRun_->queuedCommands.front(),
                .frame = makeInteractiveStepFrame(domain::FrameId{nextValue}),
            };
            interactiveStepRun_->queuedCommands.pop_front();
            interactiveStepRun_->frame = std::move(next);
            if (!armInteractiveStepPresentation(*interactiveStepRun_->frame)) {
                return;
            }
            if (submitInteractiveStepRequest(interactiveStepRun_->frame->frame) !=
                PortSubmitResult::Accepted) {
                // The provider rejected the request (transient backpressure): the just-popped
                // command would be lost if we only reset the frame. Tear the run down as a cancel
                // so every not-yet-presented command (this one plus the rest of the queue)
                // receives a Canceled terminal, mirroring the first-frame rejection path in
                // beginInteractiveForwardStep.
                cancelInteractiveStepRun(InteractiveStepStopReason::NavigationSuperseded);
                return;
            }
            state_.requestedFrame = interactiveStepRun_->lastQueuedTarget;
            publishInteractiveSnapshot();
            return;
        }

        // Clean drain: nothing left to present. Keep the provider generation warm (do not increment
        // it) so the next +1 reuses the Sequential cursor / reverse-window cache.
        interactiveStepRun_.reset();
        lastInteractiveProjectionAt_.reset();
        state_.playbackState = domain::PlaybackState::kPaused;
        state_.requestedFrame.reset();
        publishInteractiveSnapshot();
    }

    // Shared teardown for both cancel and failure: moves the run out, cancels its presentation
    // timers and provider request, and clears the render channel if the current frame was already
    // published. Returns the moved run (or nullopt when none was engaged) so the caller can
    // complete its pending commands with the outcome appropriate to the stop reason.
    [[nodiscard]] std::optional<InteractiveStepRun> teardownInteractiveStepRun() noexcept {
        if (!interactiveStepRun_.has_value()) {
            return std::nullopt;
        }
        InteractiveStepRun stopped = std::move(*interactiveStepRun_);
        interactiveStepRun_.reset();
        lastInteractiveProjectionAt_.reset();

        if (stopped.frame.has_value() && stopped.frame->frame.presentationTimerId.has_value()) {
            static_cast<void>(
                dependencies_.deadlineScheduler->cancel(*stopped.frame->frame.presentationTimerId));
        }
        if (stopped.preparedFrame.has_value() &&
            stopped.preparedFrame->frame.presentationTimerId.has_value()) {
            static_cast<void>(dependencies_.deadlineScheduler->cancel(
                *stopped.preparedFrame->frame.presentationTimerId));
        }
        dependencies_.directFrameProvider->cancel(stopped.providerContext);
        if (stopped.frame.has_value() && stopped.frame->frame.framePublished) {
            dependencies_.renderChannel->clear(stopped.providerContext);
        }
        return stopped;
    }

    // Completes every not-yet-presented command of a torn-down run (queued, prepared, current) with
    // the given outcome and optional error. The current frame's command is completed
    // unconditionally: commitInteractiveStepFrameIfComplete always reassigns the frame slot right
    // after completing its command, so a still-current frame.command is guaranteed never to have
    // been completed already — gating on framePresented would instead drop the command of a frame
    // that was rendered (render ACK) but whose provider-success event had not yet committed it.
    void completeInteractiveStepCommands(const InteractiveStepRun& stopped,
                                         const CommandOutcome outcome,
                                         std::optional<domain::MediaError> error) {
        const auto completePending = [this, outcome, &error](const CommandContext& context) {
            completeCommand(context, outcome, error);
        };
        for (const CommandContext& queued : stopped.queuedCommands) {
            completePending(queued);
        }
        if (stopped.preparedFrame.has_value()) {
            completePending(stopped.preparedFrame->command);
        }
        if (stopped.frame.has_value()) {
            completePending(stopped.frame->command);
        }
    }

    // Cancellation path: a normal navigation event (backward step, seek, play, first/last, source
    // topology/reference/alignment change) or shutdown supersedes the warm Sequential stream. The
    // not-yet-presented step commands are completed as Canceled, or Closed for an orderly shutdown;
    // lastError is NOT set because nothing went wrong. The generation still advances so any
    // in-flight Sequential results from this run cannot commit.
    void cancelInteractiveStepRun(const InteractiveStepStopReason reason) {
        const std::optional<InteractiveStepRun> stopped = teardownInteractiveStepRun();
        if (!stopped.has_value()) {
            return;
        }
        const CommandOutcome outcome = reason == InteractiveStepStopReason::Shutdown
                                           ? CommandOutcome::Closed
                                           : CommandOutcome::Canceled;
        completeInteractiveStepCommands(*stopped, outcome, std::nullopt);
        state_.playbackGeneration = increment(state_.playbackGeneration);
        state_.playbackState = domain::PlaybackState::kPaused;
        state_.requestedFrame.reset();
        publishSnapshot();
    }

    // Failure path: a provider request failed, the render channel closed, the presentation deadline
    // elapsed, the provider published a mismatched FrameSet, or an unrecoverable graphics/media
    // failure occurred. These are coordinator errors, so lastError IS set (the snapshot banner can
    // surface it) and the not-yet-presented step commands are completed as Failed with the error.
    // The generation advances and the last ACKed displayedFrame is left intact.
    void failInteractiveStepRun(domain::MediaError error) {
        const std::optional<InteractiveStepRun> stopped = teardownInteractiveStepRun();
        if (!stopped.has_value()) {
            return;
        }
        completeInteractiveStepCommands(*stopped, CommandOutcome::Failed, error);
        state_.lastError = error;
        state_.playbackGeneration = increment(state_.playbackGeneration);
        state_.playbackState = domain::PlaybackState::kPaused;
        state_.requestedFrame.reset();
        publishSnapshot();
    }

    // Begins a fresh interactive step stream for the first +1 or -1. Stops any active playback,
    // supersedes an in-flight exact seek, advances the generation once, and submits the first
    // frame. Forward uses Sequential; Reverse uses FrameRequestPriority::Reverse.
    void beginInteractiveStepStream(const StepFramesCommand& command) {
        if (!sources_.has_value() || state_.sessionState != domain::SessionState::kReady) {
            rejectCommand(command.context,
                          CommandOutcome::Failed,
                          coordinatorError(domain::MediaErrorCode::kInvalidArgument,
                                           "A frame step requires a ready comparison set.",
                                           false));
            return;
        }
        const InteractiveStepDirection direction = command.delta > 0
                                                       ? InteractiveStepDirection::Forward
                                                       : InteractiveStepDirection::Reverse;
        // Chain against the newest requested target when present (e.g. reverse after an
        // unpresented forward step), matching the old Exact-seek step base.
        const domain::FrameId base =
            state_.requestedFrame.value_or(state_.displayedFrame.value_or(domain::FrameId{0}));
        if (!base.isValid()) {
            completeCommand(command.context, CommandOutcome::Busy);
            return;
        }
        if (direction == InteractiveStepDirection::Forward) {
            if (static_cast<std::uint64_t>(base.value()) + 1U >= state_.canonicalFrameCount) {
                completeCommand(command.context, CommandOutcome::Busy);
                return;
            }
        } else if (base.value() <= 0) {
            completeCommand(command.context, CommandOutcome::Busy);
            return;
        }
        const domain::FrameId target{
            direction == InteractiveStepDirection::Forward ? base.value() + 1 : base.value() - 1};

        // Exactly one generation advance for the whole run: this is the largest single contributor
        // to eliminating the per-frame generation storm the old Exact-seek path produced.
        dependencies_.directFrameProvider->cancel(currentPlaybackScope());
        state_.playbackGeneration = increment(state_.playbackGeneration);

        const PlaybackRequestContext providerContext = currentPlaybackScope();
        PendingInteractiveStep firstStep{
            .command = command.context,
            .frame = makeInteractiveStepFrame(target),
        };
        interactiveStepRun_ = InteractiveStepRun{
            .providerContext = providerContext,
            .direction = direction,
            .frame = std::move(firstStep),
            .lastQueuedTarget = target,
        };

        if (!armInteractiveStepPresentation(*interactiveStepRun_->frame)) {
            return;
        }
        if (submitInteractiveStepRequest(interactiveStepRun_->frame->frame) !=
            PortSubmitResult::Accepted) {
            // The provider rejected the first request (transient backpressure): tear the run down
            // as a cancel so the step command receives a Canceled terminal.
            cancelInteractiveStepRun(InteractiveStepStopReason::NavigationSuperseded);
            return;
        }
        submitInteractiveStepSuccessor();
        state_.requestedFrame = target;
        publishInteractiveSnapshot();
    }

    // Enqueues a subsequent +1/-1 onto an active interactive stream of the same direction.
    // Honors the input lookahead bound so a keyboard auto-repeat cannot queue unbounded work.
    void enqueueInteractiveStep(const StepFramesCommand& command) {
        if (!interactiveStepRun_.has_value()) {
            beginInteractiveStepStream(command);
            return;
        }
        const InteractiveStepDirection direction = command.delta > 0
                                                       ? InteractiveStepDirection::Forward
                                                       : InteractiveStepDirection::Reverse;
        if (interactiveStepRun_->direction != direction) {
            cancelInteractiveStepRun(InteractiveStepStopReason::NavigationSuperseded);
            beginInteractiveStepStream(command);
            return;
        }
        const domain::FrameId displayed =
            state_.displayedFrame.value_or(state_.requestedFrame.value_or(domain::FrameId{0}));
        const std::int64_t last = interactiveStepRun_->lastQueuedTarget.value();
        const std::int64_t nextValue =
            direction == InteractiveStepDirection::Forward ? last + 1 : last - 1;
        if (nextValue < 0 || static_cast<std::uint64_t>(nextValue) >= state_.canonicalFrameCount) {
            completeCommand(command.context, CommandOutcome::Busy);
            return;
        }
        const std::int64_t lead = direction == InteractiveStepDirection::Forward
                                      ? nextValue - displayed.value()
                                      : displayed.value() - nextValue;
        if (lead > static_cast<std::int64_t>(kInteractiveStepInputLookahead)) {
            completeCommand(command.context, CommandOutcome::Busy);
            return;
        }
        interactiveStepRun_->lastQueuedTarget = domain::FrameId{nextValue};
        interactiveStepRun_->queuedCommands.push_back(command.context);
        state_.requestedFrame = interactiveStepRun_->lastQueuedTarget;
        submitInteractiveStepSuccessor();
        publishInteractiveSnapshot();
    }

    // True when the command extends the active interactive ±1 stream of the same direction and
    // must not cancel that stream (no generation storm on held keys).
    [[nodiscard]] bool extendsActiveInteractiveStepStream(const PlaybackCommand& command) const {
        if (!interactiveStepRun_.has_value()) {
            return false;
        }
        const auto* const step = std::get_if<StepFramesCommand>(&command);
        if (step == nullptr || step->delta == 0) {
            return false;
        }
        const InteractiveStepDirection direction =
            step->delta > 0 ? InteractiveStepDirection::Forward : InteractiveStepDirection::Reverse;
        return interactiveStepRun_->direction == direction &&
               ((direction == InteractiveStepDirection::Forward && step->delta == 1) ||
                (direction == InteractiveStepDirection::Reverse && step->delta == -1));
    }

    // Handles a provider terminal for the interactive run's current or prepared frame. A canceled
    // prepared frame is dropped and its command re-queued so it is submitted later; any failure
    // ends the run. Returns true if the terminal belonged to the interactive run.
    [[nodiscard]] bool handleInteractiveStepTerminal(const RequestTerminal& terminal) {
        const EventContext& terminalContext = std::visit(
            [](const auto& value) -> const EventContext& { return value.context; }, terminal);
        emitTrace(TraceEventKind::ProviderTerminal,
                  makeTraceIdentity(),
                  static_cast<std::uint64_t>(terminal.index()),
                  makeIncomingIdentity(terminalContext));
        if (matchesInteractiveStepFrame(terminalContext)) {
            if (std::holds_alternative<RequestSucceeded>(terminal)) {
                interactiveStepRun_->frame->frame.providerSucceeded = true;
            } else if (const auto* const failed = std::get_if<RequestFailed>(&terminal)) {
                // A provider RequestFailed is a hard decode failure — surface it via lastError.
                failInteractiveStepRun(failed->error);
            } else {
                // The in-flight request was canceled (e.g. superseded by a generation change):
                // transient, not a coordinator error — tear down as a cancel.
                cancelInteractiveStepRun(InteractiveStepStopReason::NavigationSuperseded);
            }
            commitInteractiveStepFrameIfComplete();
            return true;
        }
        if (matchesInteractiveStepPreparedFrame(terminalContext)) {
            if (std::holds_alternative<RequestSucceeded>(terminal)) {
                interactiveStepRun_->preparedFrame->frame.providerSucceeded = true;
            } else {
                // Drop a failed/canceled prepared frame and re-queue its command so it is
                // re-submitted when the pipeline advances, rather than lost.
                CommandContext dropped = interactiveStepRun_->preparedFrame->command;
                interactiveStepRun_->preparedFrame.reset();
                interactiveStepRun_->queuedCommands.push_front(std::move(dropped));
            }
            return true;
        }
        return false;
    }

    [[nodiscard]] bool armPlaybackPresentation(PendingPlaybackFrame& frame) {
        if (frame.presentationRequested) {
            return true;
        }
        const std::uint64_t timerId = nextTimerId_++;
        const DeadlineRequest deadline{
            .context = frame.context.playback,
            .timerId = timerId,
            .due = dependencies_.clock->now() + kExactFrameDeadline,
        };
        const PortSubmitResult deadlineResult =
            dependencies_.deadlineScheduler->schedule(deadline, eventSink_);
        if (deadlineResult != PortSubmitResult::Accepted) {
            stopPlayback(presentationError(
                "The playback frame presentation deadline could not be scheduled."));
            return false;
        }
        frame.presentationTimerId = timerId;
        frame.presentationRequested = true;
        state_.playbackState = domain::PlaybackState::kBuffering;
        state_.requestedFrame = frame.expectedFrame;
        publishSnapshot();
        return true;
    }

    [[nodiscard]] PortSubmitResult submitPlaybackRequest(PendingPlaybackFrame& frame) {
        const FrameRequest request{
            .context = frame.context,
            .frameId = frame.expectedFrame,
            .priority = FrameRequestPriority::Sequential,
            .sourceOffsets = sourceMappingsFor(frame.expectedFrame),
            .alignmentRevision = state_.alignmentRevision,
        };
        return dependencies_.directFrameProvider->submit(request, eventSink_);
    }

    void prepareFollowingPlaybackFrame(const domain::FrameId target) {
        if (!playbackRun_.has_value() || playbackRun_->pauseRequested ||
            playbackRun_->preparedFrame.has_value() || !target.isValid()) {
            return;
        }
        const std::int64_t ceiling = playbackCeiling(*playbackRun_);
        // Never prepare past the session playback-range Out point (or the canonical end).
        if (target.value() >= ceiling) {
            return;
        }
        const domain::FrameId following{target.value() + 1};
        if (following.value() > ceiling) {
            return;
        }
        playbackRun_->preparedFrame = PendingPlaybackFrame{
            .context =
                FrameRequestContext{
                    .playback = makePlaybackContext(),
                    .deviceGeneration = state_.deviceGeneration,
                },
            .expectedFrame = following,
        };
        if (submitPlaybackRequest(*playbackRun_->preparedFrame) != PortSubmitResult::Accepted) {
            playbackRun_->preparedFrame.reset();
        }
    }

    [[nodiscard]] bool submitPlaybackFrame(const domain::FrameId target) {
        if (!playbackRun_.has_value() || playbackRun_->frame.has_value()) {
            return false;
        }
        playbackRun_->frame = PendingPlaybackFrame{
            .context =
                FrameRequestContext{
                    .playback = makePlaybackContext(),
                    .deviceGeneration = state_.deviceGeneration,
                },
            .expectedFrame = target,
        };
        if (!armPlaybackPresentation(*playbackRun_->frame)) {
            return false;
        }
        const PortSubmitResult providerResult = submitPlaybackRequest(*playbackRun_->frame);
        if (providerResult != PortSubmitResult::Accepted) {
            stopPlayback(coordinatorError(
                domain::MediaErrorCode::kMediaDecodeFailed,
                "The direct frame provider did not accept a sequential playback request."));
            return false;
        }
        prepareFollowingPlaybackFrame(target);
        return true;
    }

    [[nodiscard]] bool activatePlaybackTarget(const domain::FrameId target) {
        if (!playbackRun_.has_value() || playbackRun_->frame.has_value()) {
            return false;
        }
        domain::FrameId clamped = target;
        if (playbackRun_->range.has_value()) {
            const std::int64_t ceiling = playbackCeiling(*playbackRun_);
            const std::int64_t floor = playbackFloor(*playbackRun_);
            if (clamped.value() > ceiling) {
                clamped = domain::FrameId{ceiling};
            } else if (clamped.value() < floor) {
                clamped = domain::FrameId{floor};
            }
        }
        if (playbackRun_->preparedFrame.has_value() &&
            playbackRun_->preparedFrame->expectedFrame == clamped) {
            playbackRun_->frame = std::move(playbackRun_->preparedFrame);
            playbackRun_->preparedFrame.reset();
            if (!armPlaybackPresentation(*playbackRun_->frame)) {
                return false;
            }
            publishPlaybackFrameIfReady();
            prepareFollowingPlaybackFrame(clamped);
            return true;
        }
        playbackRun_->preparedFrame.reset();
        return submitPlaybackFrame(clamped);
    }

    [[nodiscard]] bool schedulePlaybackTarget(const domain::FrameId target) {
        if (!playbackRun_.has_value() || playbackRun_->frame.has_value() ||
            playbackRun_->cadenceTimerId.has_value()) {
            return false;
        }
        const auto due = playbackDue(target);
        if (!due.has_value()) {
            stopPlayback(coordinatorError(domain::MediaErrorCode::kArithmeticOverflow,
                                          "The playback frame boundary could not be represented."));
            return false;
        }
        const auto now = dependencies_.clock->now();
        if (*due <= now) {
            const domain::FrameId dueTarget =
                playbackRun_->restartFromEnd ? playbackRun_->firstTarget : playbackTargetAt(now);
            return activatePlaybackTarget(dueTarget);
        }
        const auto earliestRequestDue =
            detail::addDuration(now,
                                std::chrono::duration_cast<std::chrono::microseconds>(
                                    kMinimumPlaybackPreparationDelay));
        // Scale the preparation lead to the current frame interval. A fixed 14 ms lead reaches past
        // the previous frame boundary whenever one frame is shorter than 28 ms (about 36 fps and
        // above), collapsing the window onto the 1 ms floor and stripping the request of its
        // scheduling margin against the boundary. Capping the lead at half the interval keeps the
        // request ahead of the boundary at a fixed offset for high-frame-rate sources while leaving
        // sources at or below roughly 35 fps on the full 14 ms. VFR: the interval derives from the
        // canonical timeline, so irregular spacing is honored frame by frame.
        std::chrono::microseconds presentationLead =
            std::chrono::duration_cast<std::chrono::microseconds>(kPlaybackPresentationLead);
        if (target > playbackRun_->firstTarget) {
            if (const auto previousDue = playbackDue(domain::FrameId{target.value() - 1})) {
                // The clock's native duration is nanoseconds on this toolchain, so the interval
                // must be narrowed to microseconds before it is compared with the lead; comparing
                // raw nanosecond counts would inflate it a thousandfold and never cap anything.
                const auto dueMicroseconds =
                    std::chrono::duration_cast<std::chrono::microseconds>(due->time_since_epoch())
                        .count();
                const auto previousDueMicroseconds =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        previousDue->time_since_epoch())
                        .count();
                if (const auto intervalMicroseconds =
                        detail::checkedSubtract(dueMicroseconds, previousDueMicroseconds);
                    intervalMicroseconds.has_value() && *intervalMicroseconds > 0) {
                    const std::int64_t halfInterval = *intervalMicroseconds / 2;
                    if (halfInterval < presentationLead.count()) {
                        presentationLead = std::chrono::microseconds{halfInterval};
                    }
                }
            }
        }
        const auto preferredRequestDue = detail::addDuration(*due, -presentationLead);
        if (!earliestRequestDue.has_value() || !preferredRequestDue.has_value()) {
            stopPlayback(coordinatorError(domain::MediaErrorCode::kArithmeticOverflow,
                                          "The playback preparation deadline overflowed."));
            return false;
        }
        // Short VFR intervals may be smaller than the normal preparation lead. Keep at least one
        // millisecond between scheduling and dispatch while never moving the request beyond its
        // canonical frame boundary.
        const auto requestDue = std::min(*due, std::max(*preferredRequestDue, *earliestRequestDue));

        const std::uint64_t timerId = nextTimerId_++;
        playbackRun_->cadenceTimerId = timerId;
        playbackRun_->cadenceTarget = target;
        const DeadlineRequest deadline{
            .context = playbackRun_->cadenceContext,
            .timerId = timerId,
            .due = requestDue,
        };
        const PortSubmitResult result =
            dependencies_.deadlineScheduler->schedule(deadline, eventSink_);
        if (result != PortSubmitResult::Accepted) {
            stopPlayback(
                presentationError("The playback cadence deadline could not be scheduled."));
            return false;
        }
        state_.playbackState = domain::PlaybackState::kPlaying;
        state_.requestedFrame.reset();
        publishSnapshot();
        return true;
    }

    void beginPlay(const PlayCommand& command) {
        if (!sources_.has_value() || state_.sessionState != domain::SessionState::kReady ||
            !state_.displayedFrame.has_value()) {
            rejectCommand(command.context,
                          CommandOutcome::Failed,
                          coordinatorError(domain::MediaErrorCode::kInvalidArgument,
                                           "Playback requires a ready comparison set.",
                                           false));
            return;
        }
        if (!state_.graphicsReady) {
            rejectCommand(
                command.context,
                CommandOutcome::Failed,
                domain::makeMediaError(domain::MediaErrorCode::kGraphicsUnavailable,
                                       domain::MediaOperation::kGraphicsInitialization,
                                       std::nullopt,
                                       true,
                                       "Playback requires an available graphics device."));
            return;
        }
        if (state_.canonicalFrameCount <= 1U) {
            rejectCommand(command.context,
                          CommandOutcome::Failed,
                          coordinatorError(domain::MediaErrorCode::kInvalidArgument,
                                           "A one-frame comparison set cannot play continuously.",
                                           false));
            return;
        }

        const std::int64_t canonicalEnd =
            static_cast<std::int64_t>(state_.canonicalFrameCount - 1U);
        const std::int64_t displayedValue = state_.displayedFrame->value();
        const domain::FrameId displayedFrame = *state_.displayedFrame;
        bool restartFromEnd = false;
        domain::FrameId firstTarget{displayedValue + 1};
        domain::FrameId anchorFrame = displayedFrame;

        if (playbackRange_.has_value()) {
            const PlaybackRange& range = *playbackRange_;
            const std::int64_t inValue = range.inInclusive.value();
            const std::int64_t outValue = range.outInclusive.value();
            if (inValue == outValue) {
                if (displayedValue == inValue) {
                    completeCommand(command.context, CommandOutcome::Succeeded);
                    return;
                }
                firstTarget = range.inInclusive;
                anchorFrame = range.inInclusive;
            } else if (displayedValue < inValue || displayedValue > outValue ||
                       (playbackRangeLoop_ && displayedValue == outValue)) {
                // Play from outside the closed range (or loop restart from Out) begins at In.
                firstTarget = range.inInclusive;
                anchorFrame = range.inInclusive;
            } else if (displayedValue == outValue) {
                // Non-loop play already sitting on Out has nothing left to present in-range.
                completeCommand(command.context, CommandOutcome::Succeeded);
                return;
            } else {
                firstTarget = domain::FrameId{displayedValue + 1};
            }
        } else {
            restartFromEnd = displayedValue == canonicalEnd;
            firstTarget = restartFromEnd ? domain::FrameId{0} : domain::FrameId{displayedValue + 1};
        }

        state_.playbackGeneration = increment(state_.playbackGeneration);
        const PlaybackRequestContext providerContext = currentPlaybackScope();
        const PlaybackRequestContext cadenceContext = makePlaybackContext();
        playbackRun_ = PlaybackRun{
            .runId = ++playbackRunId_,
            .providerContext = providerContext,
            .cadenceContext = cadenceContext,
            .firstTarget = firstTarget,
            .nextMinimum = firstTarget,
            .anchorFrame = anchorFrame,
            .wallAnchor = dependencies_.clock->now(),
            // A rate chosen while paused overrides the rate carried by the play command, so
            // setPlaybackRate(2x) followed by play() starts at 2x regardless of defaults.
            .speed = pendingPlaybackSpeed_.value_or(command.speed),
            .restartFromEnd = restartFromEnd,
            .range = playbackRange_,
            .rangeLoop = playbackRangeLoop_ && playbackRange_.has_value(),
            .completedLoops = playbackRangeCompletedLoops_,
        };
        lastPlaybackProjectionAt_ = playbackRun_->wallAnchor;
        state_.lastError.reset();
        emitTrace(TraceEventKind::PlaybackRunStarted,
                  makeTraceIdentity(),
                  static_cast<std::uint64_t>(firstTarget.value()));
        if (!schedulePlaybackTarget(firstTarget)) {
            completeCommand(command.context, CommandOutcome::Failed, state_.lastError);
            return;
        }
        completeCommand(command.context, CommandOutcome::Succeeded);
    }

    void beginPause(const PauseCommand& command) {
        if (!playbackRun_.has_value()) {
            completeCommand(command.context, CommandOutcome::Succeeded);
            return;
        }
        if (!playbackRun_->frame.has_value() || !playbackRun_->frame->framePublished) {
            stopPlayback();
            completeCommand(command.context, CommandOutcome::Succeeded);
            return;
        }

        playbackRun_->pauseRequested = true;
        state_.playbackState = domain::PlaybackState::kPaused;
        publishSnapshot();
        completeCommand(command.context, CommandOutcome::Succeeded);
    }

    // Rate changes take effect immediately during playback by re-anchoring the run on the
    // current displayed frame: due() and the catch-up projection both derive from
    // (anchorFrame, wallAnchor, speed), so re-anchoring at `now` keeps the displayed frame
    // on screen and only rescales the time that elapses after the change. While paused the
    // rate is remembered and applied by the next PlayCommand.
    void beginSetPlaybackRate(const SetPlaybackRateCommand& command) {
        pendingPlaybackSpeed_ = command.speed;
        if (!playbackRun_.has_value() || !state_.displayedFrame.has_value()) {
            completeCommand(command.context, CommandOutcome::Succeeded);
            return;
        }
        playbackRun_->speed = command.speed;
        playbackRun_->anchorFrame = *state_.displayedFrame;
        playbackRun_->wallAnchor = dependencies_.clock->now();
        lastPlaybackProjectionAt_.reset();
        // A frame already in flight keeps its presentation deadline; only the pending cadence
        // timer is rescheduled so the next request fires at the new rate's due time.
        if (!playbackRun_->frame.has_value()) {
            if (playbackRun_->cadenceTimerId.has_value()) {
                static_cast<void>(
                    dependencies_.deadlineScheduler->cancel(*playbackRun_->cadenceTimerId));
                playbackRun_->cadenceTimerId.reset();
            }
            if (playbackRun_->cadenceTarget.has_value()) {
                static_cast<void>(schedulePlaybackTarget(*playbackRun_->cadenceTarget));
            }
        }
        publishSnapshot();
        completeCommand(command.context, CommandOutcome::Succeeded);
    }

    void beginSetPlaybackRange(const SetPlaybackRangeCommand& command) {
        if (command.range.has_value()) {
            if (!command.range->isValid()) {
                rejectCommand(
                    command.context,
                    CommandOutcome::Failed,
                    coordinatorError(domain::MediaErrorCode::kInvalidArgument,
                                     "Playback range requires in <= out on the canonical timeline.",
                                     false));
                return;
            }
            if (state_.canonicalFrameCount == 0U ||
                static_cast<std::uint64_t>(command.range->outInclusive.value()) >=
                    state_.canonicalFrameCount) {
                rejectCommand(
                    command.context,
                    CommandOutcome::Failed,
                    coordinatorError(domain::MediaErrorCode::kInvalidFrameId,
                                     "Playback range lies outside the canonical timeline.",
                                     false));
                return;
            }
            playbackRange_ = command.range;
            playbackRangeLoop_ = command.loop;
        } else {
            playbackRange_.reset();
            playbackRangeLoop_ = false;
            playbackRangeCompletedLoops_ = 0U;
        }
        if (playbackRun_.has_value()) {
            playbackRun_->range = playbackRange_;
            playbackRun_->rangeLoop = playbackRangeLoop_ && playbackRange_.has_value();
            if (!playbackRange_.has_value()) {
                playbackRun_->completedLoops = 0U;
            } else if (state_.displayedFrame.has_value()) {
                const std::int64_t displayedValue = state_.displayedFrame->value();
                const std::int64_t inValue = playbackRange_->inInclusive.value();
                const std::int64_t outValue = playbackRange_->outInclusive.value();
                if (displayedValue < inValue || displayedValue > outValue) {
                    if (playbackRun_->rangeLoop && inValue < outValue) {
                        playbackRun_->anchorFrame = playbackRange_->inInclusive;
                        playbackRun_->firstTarget = playbackRange_->inInclusive;
                        playbackRun_->nextMinimum = playbackRange_->inInclusive;
                        playbackRun_->wallAnchor = dependencies_.clock->now();
                        playbackRun_->restartFromEnd = false;
                        if (playbackRun_->preparedFrame.has_value()) {
                            dependencies_.directFrameProvider->cancel(
                                playbackRun_->providerContext);
                            playbackRun_->preparedFrame.reset();
                        }
                        if (!playbackRun_->frame.has_value()) {
                            if (playbackRun_->cadenceTimerId.has_value()) {
                                static_cast<void>(dependencies_.deadlineScheduler->cancel(
                                    *playbackRun_->cadenceTimerId));
                                playbackRun_->cadenceTimerId.reset();
                            }
                            static_cast<void>(schedulePlaybackTarget(playbackRange_->inInclusive));
                        }
                    } else if (!playbackRun_->rangeLoop) {
                        stopPlayback();
                    }
                } else {
                    const std::int64_t ceiling = playbackCeiling(*playbackRun_);
                    if (playbackRun_->nextMinimum.value() > ceiling) {
                        playbackRun_->nextMinimum = domain::FrameId{ceiling};
                    }
                }
            }
        }
        publishSnapshot();
        completeCommand(command.context, CommandOutcome::Succeeded);
    }

    void beginSetPlaybackContinuityPolicy(const SetPlaybackContinuityPolicyCommand& command) {
        playbackContinuityPolicy_ = command.policy;
        publishSnapshot();
        completeCommand(command.context, CommandOutcome::Succeeded);
    }

    void beginSetActiveComparisonPair(const SetActiveComparisonPairCommand& command) {
        activePairPolicy_ = command.policy;
        if (!sources_.has_value()) {
            activeComparisonPair_.reset();
            state_.activeComparisonPair.reset();
            publishSnapshot();
            completeCommand(command.context, CommandOutcome::Succeeded);
            return;
        }
        if (command.pair.has_value()) {
            const bool firstKnown = sources_->find(command.pair->first) != nullptr;
            const bool secondKnown = sources_->find(command.pair->second) != nullptr;
            if (!command.pair->isValid() || !firstKnown || !secondKnown) {
                rejectCommand(
                    command.context,
                    CommandOutcome::Failed,
                    coordinatorError(domain::MediaErrorCode::kInvalidArgument,
                                     "Active comparison pair must name two distinct loaded "
                                     "sources.",
                                     false));
                return;
            }
            activeComparisonPair_ = command.pair;
        } else {
            activeComparisonPair_ = domain::resolveComparisonPair(sources_->sources(),
                                                                  sources_->referenceSourceId(),
                                                                  std::nullopt,
                                                                  activePairPolicy_);
        }
        state_.activeComparisonPair = activeComparisonPair_;
        publishSnapshot();
        completeCommand(command.context, CommandOutcome::Succeeded);
    }

    void beginStartRangePlayback(const StartRangePlaybackCommand& command) {
        if (!command.range.isValid()) {
            rejectCommand(
                command.context,
                CommandOutcome::Failed,
                coordinatorError(domain::MediaErrorCode::kInvalidArgument,
                                 "Playback range requires in <= out on the canonical timeline.",
                                 false));
            return;
        }
        if (state_.canonicalFrameCount == 0U ||
            static_cast<std::uint64_t>(command.range.outInclusive.value()) >=
                state_.canonicalFrameCount) {
            rejectCommand(command.context,
                          CommandOutcome::Failed,
                          coordinatorError(domain::MediaErrorCode::kInvalidFrameId,
                                           "Playback range lies outside the canonical timeline.",
                                           false));
            return;
        }
        playbackRange_ = command.range;
        playbackRangeLoop_ = command.loop;
        pendingPlaybackSpeed_ = command.speed;
        if (interactiveStepRun_.has_value()) {
            cancelInteractiveStepRun(InteractiveStepStopReason::PlaybackStarted);
        }
        if (playbackRun_.has_value()) {
            stopPlayback();
        }
        if (pendingProbe_.has_value()) {
            completeCommand(command.context, CommandOutcome::Busy);
            return;
        }
        if (pending_.has_value()) {
            if (pending_->phase == PendingPhase::kSeekingFrame) {
                supersedePendingSeek();
            } else {
                completeCommand(command.context, CommandOutcome::Busy);
                return;
            }
        }
        publishSnapshot();
        beginPlay(PlayCommand{
            .context = command.context,
            .speed = command.speed,
        });
    }

    void submitFirstOrSeekFrame(const domain::FrameId frameId, const PendingPhase phase) {
        if (!pending_.has_value()) {
            return;
        }
        PendingCommand& pending = *pending_;
        pending.phase = phase;
        pending.set.reset();
        pending.expectedFrame = frameId;
        pending.presentationTimerId.reset();
        pending.framePublished = false;
        pending.providerSucceeded = false;
        pending.framePresented = false;
        const PlaybackRequestContext playback = makePlaybackContext();
        const FrameRequestContext frameContext{
            .playback = playback,
            .deviceGeneration = state_.deviceGeneration,
        };
        pending.providerContext = playback;
        pending.frameContext = frameContext;
        state_.playbackState = domain::PlaybackState::kSeeking;
        state_.requestedFrame = frameId;
        publishSnapshot();

        const FrameRequest request{
            .context = frameContext,
            .frameId = frameId,
            .priority = FrameRequestPriority::Exact,
            .sourceOffsets = sourceMappingsFor(frameId),
            .alignmentRevision = state_.alignmentRevision,
        };
        const std::uint64_t timerId = nextTimerId_++;
        const DeadlineRequest deadline{
            .context = playback,
            .timerId = timerId,
            .due = dependencies_.clock->now() + kExactFrameDeadline,
        };
        const PortSubmitResult deadlineResult =
            dependencies_.deadlineScheduler->schedule(deadline, eventSink_);
        if (deadlineResult != PortSubmitResult::Accepted) {
            failPending(presentationError("The exact frame deadline could not be scheduled."),
                        deadlineResult == PortSubmitResult::Busy ? CommandOutcome::Busy
                                                                 : CommandOutcome::Closed);
            return;
        }
        pending.presentationTimerId = timerId;
        if (dependencies_.directFrameProvider->submit(request, eventSink_) !=
            PortSubmitResult::Accepted) {
            failPending(coordinatorError(
                            domain::MediaErrorCode::kMediaDecodeFailed,
                            "The direct frame provider did not accept an exact frame request."),
                        CommandOutcome::Busy);
        }
    }

    // Shared post-validation open path. Stores the validated set and compatibility report, builds
    // the provider request from the canonical timeline, and submits the open to the frame
    // provider. Both the direct-descriptor and probed-paths entry points funnel through here.
    void beginOpenValidated(
        CommandContext commandContext,
        domain::ValidatedComparisonSet set,
        domain::CompatibilityReport report,
        domain::CanonicalTimeline timeline,
        const std::optional<domain::MediaTime> resumeTime = std::nullopt,
        const bool rollbackAttempt = false,
        const std::optional<CommandOutcome> terminalOutcomeOverride = std::nullopt,
        const std::optional<domain::MediaError> terminalErrorOverride = std::nullopt) {
        domain::FrameId initialFrame{0};
        if (resumeTime.has_value()) {
            auto mapped = domain::canonicalFrameAtOrBefore(timeline, *resumeTime);
            if (mapped) {
                const std::int64_t maximum =
                    (std::max)(set.canonicalFrameCount() - 1, std::int64_t{0});
                initialFrame =
                    domain::FrameId{std::clamp(mapped.value().value(), std::int64_t{0}, maximum)};
            }
        }
        // D09: freeze the previous source list before the topology swap so the active pair can
        // be remapped by media identity rather than reused slot ordinals.
        std::vector<domain::ComparisonSource> previousSources;
        if (sources_.has_value()) {
            const std::span<const domain::ComparisonSource> current = sources_->sources();
            previousSources.assign(current.begin(), current.end());
        }
        if (sources_.has_value() || state_.displayedFrame.has_value()) {
            const PlaybackRequestContext previousScope = currentPlaybackScope();
            dependencies_.directFrameProvider->cancel(previousScope);
        }
        state_.sessionEpoch = increment(state_.sessionEpoch);
        state_.playbackGeneration = increment(state_.playbackGeneration);
        topologyRevision_ = increment(topologyRevision_);
        timelineRevision_ = increment(timelineRevision_);
        sources_ = std::move(set);
        state_.validatedComparison.reset();
        compatibilityReport_ = std::move(report);
        alignmentOffsets_.clear();
        state_.alignmentEstimates.clear();
        state_.sequenceAlignments.clear();
        state_.alignmentRevision = 0U;
        state_.manualAlignmentAnchors.clear();
        sequenceAlignmentMaps_.clear();
        invalidateAutomaticAlignmentHistory();
        // Topology/timeline rebuild invalidates any previously installed playback range.
        playbackRange_.reset();
        playbackRangeLoop_ = false;
        playbackRangeCompletedLoops_ = 0U;
        canonicalTimeline_ = std::move(timeline);
        prefetchScheduler_.reset();
        state_.sessionState = domain::SessionState::kLoading;
        state_.playbackState = domain::PlaybackState::kSeeking;
        state_.displayedFrame.reset();
        state_.requestedFrame = initialFrame;
        state_.canonicalFrameCount = static_cast<std::uint64_t>(sources_->canonicalFrameCount());
        state_.sources.clear();
        state_.sources.reserve(sources_->sourceCount());
        for (const domain::ComparisonSource& source : sources_->sources()) {
            state_.sources.push_back(SessionSourceView{
                .sourceId = source.id,
                .role = source.role,
                .displayName = source.displayName,
            });
        }
        // C-02/D09: remap the preferred pair through stable media identity first so a reused
        // SourceId slot cannot silently become "the same" media, then re-resolve by policy.
        const std::optional<domain::ComparisonPair> remappedPreferred =
            previousSources.empty()
                ? activeComparisonPair_
                : domain::remapComparisonPairByMediaIdentity(
                      previousSources, activeComparisonPair_, sources_->sources());
        activeComparisonPair_ = domain::resolveComparisonPair(sources_->sources(),
                                                              sources_->referenceSourceId(),
                                                              remappedPreferred,
                                                              activePairPolicy_);
        state_.activeComparisonPair = activeComparisonPair_;
        state_.presentedSources.clear();
        state_.alignmentEstimates.clear();
        state_.sequenceAlignments.clear();
        state_.alignmentRevision = 0U;
        state_.manualAlignmentAnchors.clear();
        state_.compatibilityFindings.clear();
        state_.compatibilityFindings.reserve(
            std::min(compatibilityReport_->findings().size(), kMaximumCompatibilityFindings));
        state_.alignmentRequired = compatibilityReport_->hasAlignmentRequired();
        for (const domain::CompatibilityFinding& finding : compatibilityReport_->findings()) {
            if (state_.compatibilityFindings.size() == kMaximumCompatibilityFindings) {
                break;
            }
            state_.compatibilityFindings.push_back(CompatibilityFindingView{
                .severity = finding.severity,
                .code = finding.code,
                .sources = finding.sources,
            });
        }
        state_.lastError.reset();

        const PlaybackRequestContext context = makePlaybackContext();
        pending_ = PendingCommand{
            .phase = PendingPhase::kOpeningProvider,
            .command = commandContext,
            .providerContext = context,
            .frameContext = std::nullopt,
            .set = std::nullopt,
            .expectedFrame = initialFrame,
            .rollbackAttempt = rollbackAttempt,
            .terminalOutcomeOverride = terminalOutcomeOverride,
            .terminalErrorOverride = terminalErrorOverride,
        };
        publishSnapshot();

        const FrameProviderOpenRequest request{
            .context = context,
            .sources = std::vector<domain::ComparisonSource>(sources_->sources().begin(),
                                                             sources_->sources().end()),
            .canonicalSourceId = sources_->canonicalSourceId(),
            .timeline = *canonicalTimeline_,
        };
        if (dependencies_.directFrameProvider->submit(request, eventSink_) !=
            PortSubmitResult::Accepted) {
            failPending(
                coordinatorError(domain::MediaErrorCode::kMediaDecodeFailed,
                                 "The direct frame provider did not accept the comparison set."),
                CommandOutcome::Busy);
        }
    }

    // Validates a set of pre-probed descriptors and opens the provider. Used by the direct open
    // path (CLI diagnostics and tests) where descriptors arrive already probed.
    void beginOpenDirect(const OpenDirectComparisonCommand& command) {
        auto validation = domain::ComparisonValidator::validate(command.sources);
        if (!validation) {
            if (state_.sessionState == domain::SessionState::kReady && sources_.has_value()) {
                rejectCommand(command.context, CommandOutcome::Failed, validation.error());
                return;
            }
            resetToEmpty();
            state_.sessionState = domain::SessionState::kInvalid;
            state_.playbackState = domain::PlaybackState::kPaused;
            state_.displayedFrame.reset();
            state_.requestedFrame.reset();
            state_.canonicalFrameCount = 0U;
            state_.lastError = validation.error();
            publishSnapshot();
            completeCommand(command.context, CommandOutcome::Failed, validation.error());
            return;
        }

        // The canonical source's descriptor determines the timeline. A direct open of a VFR
        // canonical source arrives without a probed runtime timeline, so it fails clearly here
        // without disturbing an existing ready session.
        const auto& set = validation.value().set;
        const bool canonicalIsVfr = set.canonicalDescriptor().timingConfidence ==
                                    domain::TimingConfidence::kVariableFrameRate;
        if (canonicalIsVfr) {
            rejectCommand(command.context,
                          CommandOutcome::Failed,
                          coordinatorError(domain::MediaErrorCode::kFrameTimelineInvalid,
                                           "A VFR canonical source requires a probed runtime "
                                           "timeline.",
                                           false));
            return;
        }

        const domain::RationalRate canonicalRate = *set.canonicalRate();

        captureReadySessionForOpenRollback();
        beginOpenValidated(command.context,
                           std::move(validation.value().set),
                           std::move(validation.value().report),
                           domain::CanonicalTimeline{canonicalRate});
    }

    // A newer navigation target replaces an in-flight exact seek: cancel its provider scope and
    // presentation timer and complete the superseded command as canceled. The caller dispatches
    // the fresh command, whose beginSeek advances the generation again.
    void supersedePendingSeek() {
        if (!pending_.has_value() || pending_->phase != PendingPhase::kSeekingFrame) {
            return;
        }
        PendingCommand superseded = std::move(*pending_);
        pending_.reset();
        if (superseded.presentationTimerId.has_value()) {
            static_cast<void>(
                dependencies_.deadlineScheduler->cancel(*superseded.presentationTimerId));
        }
        dependencies_.directFrameProvider->cancel(superseded.providerContext);
        completeCommand(superseded.command, CommandOutcome::Canceled);
    }

    // Cancels every in-flight probe of the pending open and completes that command as canceled,
    // so a freshly submitted open can start its own probe set. Late completions from the canceled
    // probes no longer match any pending slot and are ignored.
    void supersedePendingProbes() {
        if (!pendingProbe_.has_value()) {
            return;
        }
        const PendingProbe pending = std::move(*pendingProbe_);
        pendingProbe_.reset();
        for (const auto& slot : pending.slots) {
            dependencies_.mediaProbe->cancel(slot.context);
        }
        completeCommand(pending.command, CommandOutcome::Canceled);
    }

    void failProbe(domain::MediaError error,
                   const CommandOutcome outcome,
                   const domain::SessionState initialFailureState) {
        if (!pendingProbe_.has_value()) {
            return;
        }
        const PendingProbe pending = std::move(*pendingProbe_);
        pendingProbe_.reset();
        for (const auto& slot : pending.slots) {
            dependencies_.mediaProbe->cancel(slot.context);
        }

        if (pending.preservesReadySession) {
            state_.lastError = error;
        } else {
            state_.sessionState = initialFailureState;
            state_.playbackState = domain::PlaybackState::kPaused;
            state_.displayedFrame.reset();
            state_.requestedFrame.reset();
            state_.canonicalFrameCount = 0U;
            state_.presentedSources.clear();
            state_.sources.clear();
            state_.validatedComparison.reset();
            state_.compatibilityFindings.clear();
            state_.alignmentRequired = false;
            state_.lastError = error;
        }
        publishSnapshot();
        completeCommand(pending.command, outcome, std::move(error));
    }

    [[nodiscard]] PendingProbeSlot* probeSlot(const RequestContext& context) noexcept {
        if (!pendingProbe_.has_value()) {
            return nullptr;
        }
        for (auto& slot : pendingProbe_->slots) {
            if (slot.context == context) {
                return &slot;
            }
        }
        return nullptr;
    }

    void finishProbeIfComplete() {
        if (!pendingProbe_.has_value()) {
            return;
        }
        for (const auto& slot : pendingProbe_->slots) {
            if (!slot.succeeded || !slot.descriptor.has_value()) {
                return;
            }
        }

        PendingProbe completed = std::move(*pendingProbe_);
        pendingProbe_.reset();

        // Build the ComparisonSource vector from the completed probe slots, preserving the
        // submission-order source ids, roles, and display names from the original command.
        std::vector<domain::ComparisonSource> comparisonSources;
        comparisonSources.reserve(completed.slots.size());
        for (auto& slot : completed.slots) {
            comparisonSources.push_back(domain::ComparisonSource{
                .id = slot.sourceId,
                .role = slot.role,
                .descriptor = std::move(*slot.descriptor),
                .displayName = std::move(slot.displayName),
            });
        }

        auto validation = domain::ComparisonValidator::validate(comparisonSources);
        if (!validation) {
            if (state_.sessionState == domain::SessionState::kReady && sources_.has_value()) {
                state_.lastError = validation.error();
                publishSnapshot();
                completeCommand(completed.command, CommandOutcome::Failed, validation.error());
            } else {
                resetToEmpty();
                state_.sessionState = domain::SessionState::kInvalid;
                state_.playbackState = domain::PlaybackState::kPaused;
                state_.displayedFrame.reset();
                state_.requestedFrame.reset();
                state_.canonicalFrameCount = 0U;
                state_.lastError = validation.error();
                publishSnapshot();
                completeCommand(completed.command, CommandOutcome::Failed, validation.error());
            }
            return;
        }

        // Build the canonical timeline from the canonical source's descriptor and probe slot.
        // CFR canonical sources carry their rational rate; VFR canonical sources carry the probed
        // shared timeline published alongside the descriptor.
        const auto& set = validation.value().set;
        const domain::SourceId canonicalId = set.canonicalSourceId();
        const bool canonicalIsVfr = set.canonicalDescriptor().timingConfidence ==
                                    domain::TimingConfidence::kVariableFrameRate;

        std::optional<domain::CanonicalTimeline> activeTimeline;
        if (canonicalIsVfr) {
            std::shared_ptr<const domain::FrameTimeline> canonicalTimelinePtr;
            for (const auto& slot : completed.slots) {
                if (slot.sourceId == canonicalId && slot.timeline.has_value() && *slot.timeline) {
                    canonicalTimelinePtr = *slot.timeline;
                    break;
                }
            }
            if (!canonicalTimelinePtr) {
                const domain::MediaError timelineError = probeCoordinatorError(
                    domain::MediaErrorCode::kFrameTimelineInvalid,
                    canonicalId,
                    "The VFR canonical source probe did not publish a runtime timeline.",
                    false);
                if (state_.sessionState == domain::SessionState::kReady && sources_.has_value()) {
                    state_.lastError = timelineError;
                    publishSnapshot();
                    completeCommand(completed.command, CommandOutcome::Failed, timelineError);
                } else {
                    resetToEmpty();
                    state_.sessionState = domain::SessionState::kInvalid;
                    state_.playbackState = domain::PlaybackState::kPaused;
                    state_.displayedFrame.reset();
                    state_.requestedFrame.reset();
                    state_.canonicalFrameCount = 0U;
                    state_.lastError = timelineError;
                    publishSnapshot();
                    completeCommand(completed.command, CommandOutcome::Failed, timelineError);
                }
                return;
            }
            activeTimeline = domain::CanonicalTimeline{std::move(canonicalTimelinePtr)};
        } else {
            if (!set.canonicalRate().has_value()) {
                const domain::MediaError rateError =
                    probeCoordinatorError(domain::MediaErrorCode::kInvalidCfrTiming,
                                          canonicalId,
                                          "The CFR canonical source is missing a rational rate.",
                                          false);
                if (state_.sessionState == domain::SessionState::kReady && sources_.has_value()) {
                    state_.lastError = rateError;
                    publishSnapshot();
                    completeCommand(completed.command, CommandOutcome::Failed, rateError);
                } else {
                    resetToEmpty();
                    state_.sessionState = domain::SessionState::kInvalid;
                    state_.playbackState = domain::PlaybackState::kPaused;
                    state_.displayedFrame.reset();
                    state_.requestedFrame.reset();
                    state_.canonicalFrameCount = 0U;
                    state_.lastError = rateError;
                    publishSnapshot();
                    completeCommand(completed.command, CommandOutcome::Failed, rateError);
                }
                return;
            }
            activeTimeline = domain::CanonicalTimeline{*set.canonicalRate()};
        }

        if (completed.preservesReadySession) {
            captureReadySessionForOpenRollback();
        }
        beginOpenValidated(completed.command,
                           std::move(validation.value().set),
                           std::move(validation.value().report),
                           std::move(*activeTimeline),
                           completed.resumeTime);
    }

    void beginOpenPaths(const OpenComparisonCommand& command) {
        if (command.sources.empty()) {
            rejectCommand(command.context,
                          CommandOutcome::Failed,
                          probeCoordinatorError(domain::MediaErrorCode::kInvalidArgument,
                                                std::nullopt,
                                                "At least one source path is required.",
                                                false));
            return;
        }
        for (const auto& source : command.sources) {
            if (source.path.empty()) {
                rejectCommand(command.context,
                              CommandOutcome::Failed,
                              probeCoordinatorError(domain::MediaErrorCode::kInvalidArgument,
                                                    std::nullopt,
                                                    "All source paths are required.",
                                                    false));
                return;
            }
        }

        const bool preservesReadySession = state_.sessionState == domain::SessionState::kReady &&
                                           sources_.has_value() &&
                                           state_.displayedFrame.has_value();

        // Build one probe slot per submitted source. Source ids are assigned in submission order
        // (0, 1, 2). Each slot gets its own request context for cancellation identity.
        std::vector<PendingProbeSlot> slots;
        slots.reserve(command.sources.size());
        for (std::size_t index = 0; index < command.sources.size(); ++index) {
            slots.push_back(PendingProbeSlot{
                .context = makeRequestContext(),
                .sourceId = static_cast<domain::SourceId>(index),
                .sourcePath = command.sources[index].path,
                .role = command.sources[index].role,
                .displayName = command.sources[index].displayName,
            });
        }

        std::optional<domain::MediaTime> resumeTime;
        if (command.preserveDisplayedTime && state_.displayedFrame.has_value() &&
            canonicalTimeline_.has_value()) {
            auto currentTime =
                domain::canonicalFrameStartTime(*canonicalTimeline_, *state_.displayedFrame);
            if (currentTime) {
                resumeTime = currentTime.value();
            }
        }

        PendingProbe pending{
            .command = command.context,
            .slots = std::move(slots),
            .resumeTime = resumeTime,
            .preservesReadySession = preservesReadySession,
        };

        // Same-path dedup: identical paths among the submitted sources share one probe whose
        // descriptor fills every slot with that path. Only the first slot for each unique path
        // has a probe submitted; shared slots are filled when the primary's probe completes.
        std::vector<std::size_t> primaryIndices;
        for (std::size_t index = 0; index < pending.slots.size(); ++index) {
            bool isDuplicate = false;
            for (const std::size_t primary : primaryIndices) {
                if (pending.slots[primary].sourcePath == pending.slots[index].sourcePath) {
                    isDuplicate = true;
                    break;
                }
            }
            if (!isDuplicate) {
                primaryIndices.push_back(index);
            }
        }

        pendingProbe_ = std::move(pending);

        if (!preservesReadySession) {
            resetToEmpty();
            state_.sessionState = domain::SessionState::kLoading;
            publishSnapshot();
        } else {
            state_.lastError.reset();
            publishSnapshot();
        }

        for (const std::size_t primaryIndex : primaryIndices) {
            PendingProbeSlot& primary = pendingProbe_->slots[primaryIndex];
            const MediaProbeRequest request{
                .context = primary.context,
                .sourceId = primary.sourceId,
                .sourcePath = primary.sourcePath,
            };
            const PortSubmitResult accepted = dependencies_.mediaProbe->submit(request, eventSink_);
            if (accepted != PortSubmitResult::Accepted) {
                failProbe(probeCoordinatorError(domain::MediaErrorCode::kMediaProbeFailed,
                                                primary.sourceId,
                                                "Source probing was not accepted."),
                          accepted == PortSubmitResult::Busy ? CommandOutcome::Busy
                                                             : CommandOutcome::Closed,
                          domain::SessionState::kError);
                return;
            }
        }
    }

    void beginSeek(const CommandContext& command, const domain::FrameId frameId) {
        if (!sources_.has_value() || state_.sessionState != domain::SessionState::kReady) {
            rejectCommand(command,
                          CommandOutcome::Failed,
                          coordinatorError(domain::MediaErrorCode::kInvalidArgument,
                                           "A frame seek requires a ready comparison set.",
                                           false));
            return;
        }
        if (!frameId.isValid() ||
            static_cast<std::uint64_t>(frameId.value()) >= state_.canonicalFrameCount) {
            rejectCommand(command,
                          CommandOutcome::Failed,
                          coordinatorError(domain::MediaErrorCode::kInvalidFrameId,
                                           "The requested frame is outside the canonical timeline.",
                                           false));
            return;
        }

        dependencies_.directFrameProvider->cancel(currentPlaybackScope());
        state_.playbackGeneration = increment(state_.playbackGeneration);
        pending_ = PendingCommand{
            .phase = PendingPhase::kSeekingFrame,
            .command = command,
            .providerContext = currentPlaybackScope(),
            .frameContext = std::nullopt,
            .set = std::nullopt,
        };
        submitFirstOrSeekFrame(frameId, PendingPhase::kSeekingFrame);
    }

    void beginStep(const StepFramesCommand& command) {
        if (command.delta == 0) {
            rejectCommand(command.context,
                          CommandOutcome::Failed,
                          coordinatorError(domain::MediaErrorCode::kInvalidArgument,
                                           "A frame step delta must not be zero.",
                                           false));
            return;
        }
        // Forward +1 and reverse -1 steps run on the interactive step stream: they keep one
        // generation across the whole run. Forward reuses Sequential decode; reverse uses
        // FrameRequestPriority::Reverse (provider-side Reverse GOP Window, ADR-003).
        // Multi-frame jumps keep the Exact-seek path.
        if (command.delta == 1 || command.delta == -1) {
            enqueueInteractiveStep(command);
            return;
        }
        if (!sources_.has_value() || state_.canonicalFrameCount == 0U) {
            beginSeek(command.context, domain::FrameId{0});
            return;
        }

        const std::int64_t maximum = static_cast<std::int64_t>(state_.canonicalFrameCount - 1U);
        // Steps chain against the newest requested target, not the last displayed frame, so a
        // burst of presses walks 101, 102, 103... while earlier requests are still in flight.
        const std::int64_t current =
            state_.requestedFrame.value_or(state_.displayedFrame.value_or(domain::FrameId{0}))
                .value();
        const std::int64_t target =
            command.delta > 0
                ? (command.delta > maximum - current ? maximum : current + command.delta)
                : (command.delta < -current ? 0 : current + command.delta);
        beginSeek(command.context, domain::FrameId{target});
    }

    void beginSetAlignmentOffsets(const SetAlignmentOffsetsCommand& command) {
        if (!sources_.has_value() || state_.sessionState != domain::SessionState::kReady ||
            !state_.displayedFrame.has_value()) {
            rejectCommand(command.context,
                          CommandOutcome::Failed,
                          coordinatorError(domain::MediaErrorCode::kInvalidArgument,
                                           "Alignment offsets require a ready comparison set.",
                                           false));
            return;
        }

        for (std::size_t index = 0U; index < command.sourceOffsets.size(); ++index) {
            const SourceFrameOffset& offset = command.sourceOffsets[index];
            if (sources_->find(offset.sourceId) == nullptr) {
                rejectCommand(command.context,
                              CommandOutcome::Failed,
                              coordinatorError(domain::MediaErrorCode::kInvalidArgument,
                                               "An alignment offset names an unknown source.",
                                               false));
                return;
            }
            const bool duplicate =
                std::any_of(command.sourceOffsets.begin() + static_cast<std::ptrdiff_t>(index + 1U),
                            command.sourceOffsets.end(),
                            [&offset](const SourceFrameOffset& other) {
                                return other.sourceId == offset.sourceId;
                            });
            if (duplicate ||
                (offset.sourceId == sources_->canonicalSourceId() && offset.frames != 0)) {
                rejectCommand(command.context,
                              CommandOutcome::Failed,
                              coordinatorError(
                                  domain::MediaErrorCode::kInvalidArgument,
                                  duplicate
                                      ? "An alignment offset source is duplicated."
                                      : "The canonical source alignment offset must remain zero.",
                                  false));
                return;
            }
        }

        std::vector<SourceFrameOffset> nextOffsets;
        std::copy_if(command.sourceOffsets.begin(),
                     command.sourceOffsets.end(),
                     std::back_inserter(nextOffsets),
                     [](const SourceFrameOffset& offset) { return offset.frames != 0; });
        const auto bySource = [](const SourceFrameOffset& left, const SourceFrameOffset& right) {
            return left.sourceId < right.sourceId;
        };
        std::sort(nextOffsets.begin(), nextOffsets.end(), bySource);
        std::vector<SourceFrameOffset> previousOffsets = alignmentOffsets_;
        std::sort(previousOffsets.begin(), previousOffsets.end(), bySource);
        const bool mappingChanged = nextOffsets != previousOffsets ||
                                    !state_.manualAlignmentAnchors.empty() ||
                                    !sequenceAlignmentMaps_.empty();
        invalidateAutomaticAlignmentHistory();
        alignmentOffsets_ = std::move(nextOffsets);
        sequenceAlignmentMaps_.clear();
        state_.alignmentEstimates.clear();
        state_.sequenceAlignments.clear();
        if (mappingChanged) {
            state_.alignmentRevision = increment(state_.alignmentRevision);
            prefetchScheduler_.reset();
        }
        state_.manualAlignmentAnchors.clear();
        sequenceAlignmentMaps_.clear();
        beginSeek(command.context, *state_.displayedFrame);
    }

    void beginEstimateAlignment(const EstimateAlignmentCommand& command) {
        if (!sources_.has_value() || state_.sessionState != domain::SessionState::kReady ||
            !state_.displayedFrame.has_value()) {
            rejectCommand(command.context,
                          CommandOutcome::Failed,
                          coordinatorError(domain::MediaErrorCode::kInvalidArgument,
                                           "Alignment estimation requires a ready comparison set.",
                                           false));
            return;
        }

        if (!dependencies_.alignmentAnalysisService || analysisJob_.has_value() ||
            !canonicalTimeline_.has_value()) {
            completeCommand(command.context, CommandOutcome::Busy);
            return;
        }
        const PlaybackRequestContext context = makePlaybackContext();
        const AlignmentAnalysisJobId jobId{nextAnalysisJobId_++};
        const std::span<const domain::ComparisonSource> activeSources = sources_->sources();
        const AlignmentEstimateRequest request{
            .context = context,
            .canonicalSourceId = sources_->canonicalSourceId(),
            .jobId = jobId,
            .sources =
                std::vector<domain::ComparisonSource>{activeSources.begin(), activeSources.end()},
            .timeline = canonicalTimeline_,
        };
        if (dependencies_.alignmentAnalysisService->submit(request, eventSink_) !=
            PortSubmitResult::Accepted) {
            completeCommand(
                command.context,
                CommandOutcome::Busy,
                coordinatorError(domain::MediaErrorCode::kMediaDecodeFailed,
                                 "The alignment analysis service did not accept the job."));
            return;
        }
        clearAutomaticProposal();
        analysisJob_ = BackgroundAnalysis{
            .jobId = jobId,
            .kind = AlignmentAnalysisKind::GlobalOffset,
            .command = command.context,
            .context = context,
        };
        state_.lastError.reset();
        state_.alignmentAnalysisJobId = jobId;
        state_.alignmentAnalysisKind = AlignmentAnalysisKind::GlobalOffset;
        state_.alignmentAnalysisPhase.reset();
        state_.alignmentAnalysisCompletedUnits = 0U;
        state_.alignmentAnalysisWork = {};
        publishSnapshot();
    }

    void beginAnalyzeSequence(const AnalyzeSequenceAlignmentCommand& command) {
        if (!sources_.has_value() || state_.sessionState != domain::SessionState::kReady ||
            !state_.displayedFrame.has_value()) {
            rejectCommand(command.context,
                          CommandOutcome::Failed,
                          coordinatorError(domain::MediaErrorCode::kInvalidArgument,
                                           "Sequence analysis requires a ready comparison set.",
                                           false));
            return;
        }

        if (!dependencies_.alignmentAnalysisService || analysisJob_.has_value() ||
            !canonicalTimeline_.has_value()) {
            completeCommand(command.context, CommandOutcome::Busy);
            return;
        }
        const PlaybackRequestContext context = makePlaybackContext();
        const AlignmentAnalysisJobId jobId{nextAnalysisJobId_++};
        const std::span<const domain::ComparisonSource> activeSources = sources_->sources();
        const SequenceAlignmentRequest request{
            .context = context,
            .canonicalSourceId = sources_->canonicalSourceId(),
            .expectedOffsets = alignmentOffsets_,
            .manualAnchors = state_.manualAlignmentAnchors,
            .options =
                SequenceAlignmentOptions{
                    .bandWidth = 16U,
                },
            .jobId = jobId,
            .sources =
                std::vector<domain::ComparisonSource>{activeSources.begin(), activeSources.end()},
            .timeline = canonicalTimeline_,
        };
        if (dependencies_.alignmentAnalysisService->submit(request, eventSink_) !=
            PortSubmitResult::Accepted) {
            completeCommand(
                command.context,
                CommandOutcome::Busy,
                coordinatorError(domain::MediaErrorCode::kMediaDecodeFailed,
                                 "The alignment analysis service did not accept the job."));
            return;
        }
        clearAutomaticProposal();
        analysisJob_ = BackgroundAnalysis{
            .jobId = jobId,
            .kind = AlignmentAnalysisKind::Sequence,
            .command = command.context,
            .context = context,
        };
        state_.lastError.reset();
        state_.alignmentAnalysisJobId = jobId;
        state_.alignmentAnalysisKind = AlignmentAnalysisKind::Sequence;
        state_.alignmentAnalysisPhase.reset();
        state_.alignmentAnalysisCompletedUnits = 0U;
        state_.alignmentAnalysisWork = {};
        publishSnapshot();
    }

    void beginCancelAlignmentAnalysis(const CancelAlignmentAnalysisCommand& command) {
        if (!analysisJob_.has_value() || !dependencies_.alignmentAnalysisService) {
            completeCommand(command.context, CommandOutcome::TooLate);
            return;
        }
        dependencies_.alignmentAnalysisService->cancel(analysisJob_->jobId);
        completeCommand(command.context, CommandOutcome::Succeeded);
    }

    void abandonAlignmentAnalysis(const CancellationReason reason) {
        if (!analysisJob_.has_value() || !dependencies_.alignmentAnalysisService) {
            return;
        }
        const AlignmentAnalysisJobId jobId = analysisJob_->jobId;
        const CommandContext command = analysisJob_->command;
        analysisJob_.reset();
        clearAnalysisProgress();
        dependencies_.alignmentAnalysisService->cancel(jobId);
        completeCommand(command, CommandOutcome::Canceled);
        if (reason == CancellationReason::Shutdown) {
            return;
        }
        publishSnapshot();
    }

    void beginSetManualAnchor(const SetManualAlignmentAnchorCommand& command) {
        if (!sources_.has_value() || state_.sessionState != domain::SessionState::kReady ||
            !state_.displayedFrame.has_value()) {
            rejectCommand(command.context,
                          CommandOutcome::Failed,
                          coordinatorError(domain::MediaErrorCode::kInvalidArgument,
                                           "Manual anchors require a ready comparison set.",
                                           false));
            return;
        }
        const domain::ComparisonSource* const source = sources_->find(command.sourceId);
        if (source == nullptr || command.sourceId == sources_->canonicalSourceId() ||
            !command.anchor.canonicalFrameId.isValid() ||
            static_cast<std::uint64_t>(command.anchor.canonicalFrameId.value()) >=
                state_.canonicalFrameCount ||
            !command.anchor.sourceFrameId.isValid() ||
            command.anchor.sourceFrameId.value() >= source->descriptor.frameCount.value) {
            rejectCommand(
                command.context,
                CommandOutcome::Failed,
                coordinatorError(domain::MediaErrorCode::kInvalidArgument,
                                 "A manual anchor must name valid canonical and source frames.",
                                 false));
            return;
        }

        std::vector<SourceAlignmentAnchors> next = state_.manualAlignmentAnchors;
        auto sourceAnchors =
            std::find_if(next.begin(), next.end(), [&command](const auto& anchors) {
                return anchors.sourceId == command.sourceId;
            });
        if (sourceAnchors == next.end()) {
            next.push_back(SourceAlignmentAnchors{
                .sourceId = command.sourceId,
                .anchors = {command.anchor},
            });
        } else {
            const auto existing =
                std::find_if(sourceAnchors->anchors.begin(),
                             sourceAnchors->anchors.end(),
                             [&command](const ManualAlignmentAnchor& anchor) {
                                 return anchor.canonicalFrameId == command.anchor.canonicalFrameId;
                             });
            if (existing == sourceAnchors->anchors.end()) {
                sourceAnchors->anchors.push_back(command.anchor);
            } else {
                *existing = command.anchor;
            }
            std::sort(sourceAnchors->anchors.begin(),
                      sourceAnchors->anchors.end(),
                      [](const ManualAlignmentAnchor& left, const ManualAlignmentAnchor& right) {
                          return left.canonicalFrameId < right.canonicalFrameId;
                      });
            if (!sourceAnchors->isValid()) {
                rejectCommand(
                    command.context,
                    CommandOutcome::Failed,
                    coordinatorError(domain::MediaErrorCode::kInvalidArgument,
                                     "Manual anchors must stay monotone and may not cross.",
                                     false));
                return;
            }
        }
        const bool mappingChanged = next != state_.manualAlignmentAnchors;
        invalidateAutomaticAlignmentHistory();
        state_.manualAlignmentAnchors = std::move(next);
        if (mappingChanged) {
            state_.alignmentRevision = increment(state_.alignmentRevision);
            prefetchScheduler_.reset();
        }
        beginSeek(command.context, *state_.displayedFrame);
    }

    void beginClearManualAnchors(const ClearManualAlignmentAnchorsCommand& command) {
        if (state_.manualAlignmentAnchors.empty()) {
            completeCommand(command.context, CommandOutcome::Succeeded);
            return;
        }
        if (!sources_.has_value() || state_.sessionState != domain::SessionState::kReady ||
            !state_.displayedFrame.has_value()) {
            rejectCommand(command.context,
                          CommandOutcome::Failed,
                          coordinatorError(domain::MediaErrorCode::kInvalidArgument,
                                           "Manual anchor reset requires a ready comparison set.",
                                           false));
            return;
        }
        invalidateAutomaticAlignmentHistory();
        state_.manualAlignmentAnchors.clear();
        state_.alignmentRevision = increment(state_.alignmentRevision);
        prefetchScheduler_.reset();
        beginSeek(command.context, *state_.displayedFrame);
    }

    void beginClose(const CloseSessionCommand& command) {
        if (!sources_.has_value()) {
            resetToEmpty();
            publishSnapshot();
            completeCommand(command.context, CommandOutcome::Succeeded);
            return;
        }

        const PlaybackRequestContext providerContext = currentPlaybackScope();
        dependencies_.renderChannel->clear(providerContext);
        state_.sessionEpoch = increment(state_.sessionEpoch);
        state_.playbackGeneration = increment(state_.playbackGeneration);
        state_.sessionState = domain::SessionState::kLoading;
        state_.playbackState = domain::PlaybackState::kPaused;
        state_.requestedFrame.reset();
        state_.displayedFrame.reset();
        state_.presentedSources.clear();
        state_.sources.clear();
        state_.validatedComparison.reset();
        state_.alignmentEstimates.clear();
        state_.sequenceAlignments.clear();
        state_.alignmentRevision = 0U;
        state_.manualAlignmentAnchors.clear();
        alignmentOffsets_.clear();
        sequenceAlignmentMaps_.clear();
        invalidateAutomaticAlignmentHistory();
        state_.compatibilityFindings.clear();
        state_.alignmentRequired = false;
        pending_ = PendingCommand{
            .phase = PendingPhase::kClosingProvider,
            .command = command.context,
            .providerContext = providerContext,
            .frameContext = std::nullopt,
            .set = std::nullopt,
        };
        publishSnapshot();

        const FrameProviderCloseRequest request{.context = providerContext};
        if (dependencies_.directFrameProvider->submit(request, eventSink_) !=
            PortSubmitResult::Accepted) {
            failPending(
                coordinatorError(domain::MediaErrorCode::kMediaDecodeFailed,
                                 "The direct frame provider did not accept the close request."),
                CommandOutcome::Busy);
        }
    }

    void handleCommand(PlaybackCommand command) {
        const CommandContext& context = commandContext(command);
        if (!claimCommand(context)) {
            return;
        }
        // Record acceptance for every non-duplicate command, before any admission check, so the
        // trace has exactly one acceptance for every terminal (a clean acceptance<->terminal
        // bijection that proves command exactly-once). Commands failing admission below still
        // reach completeCommand and emit exactly one matching CommandTerminal.
        emitTrace(TraceEventKind::CommandAccepted, makeTraceIdentity(context.commandId));
        if (!acceptsCommand(context)) {
            completeCommand(context,
                            CommandOutcome::Canceled,
                            coordinatorError(domain::MediaErrorCode::kInvalidArgument,
                                             "The command belongs to an obsolete session epoch.",
                                             true));
            return;
        }
        const bool isAlignmentCommand =
            std::holds_alternative<SetAlignmentOffsetsCommand>(command) ||
            std::holds_alternative<EstimateAlignmentCommand>(command) ||
            std::holds_alternative<AnalyzeSequenceAlignmentCommand>(command) ||
            std::holds_alternative<CancelAlignmentAnalysisCommand>(command) ||
            std::holds_alternative<ConfirmAutomaticAlignmentCommand>(command) ||
            std::holds_alternative<UndoAutomaticAlignmentCommand>(command) ||
            std::holds_alternative<RestoreSequenceAlignmentCommand>(command) ||
            std::holds_alternative<SetManualAlignmentAnchorCommand>(command) ||
            std::holds_alternative<ClearManualAlignmentAnchorsCommand>(command);
        if (isAlignmentCommand && sources_.has_value() && sources_->sourceCount() < 2U) {
            completeCommand(
                context,
                CommandOutcome::Failed,
                coordinatorError(domain::MediaErrorCode::kInvalidArgument,
                                 "Alignment commands require at least two review sources.",
                                 false));
            return;
        }
        if (std::holds_alternative<CancelAlignmentAnalysisCommand>(command)) {
            beginCancelAlignmentAnalysis(std::get<CancelAlignmentAnalysisCommand>(command));
            return;
        }
        if (std::holds_alternative<PauseCommand>(command)) {
            if (pending_.has_value() || pendingProbe_.has_value()) {
                completeCommand(context, CommandOutcome::Busy);
            } else {
                beginPause(std::get<PauseCommand>(command));
            }
            return;
        }
        if (std::holds_alternative<SetPlaybackRateCommand>(command)) {
            // Rate changes must pass while a playback run is active; beginSetPlaybackRate
            // re-anchors the cadence instead of disturbing the in-flight frame.
            beginSetPlaybackRate(std::get<SetPlaybackRateCommand>(command));
            return;
        }
        if (std::holds_alternative<SetPlaybackRangeCommand>(command)) {
            // Range installs/updates must pass while a playback run is active so loop authority
            // and Out clamping can change without tearing down the session.
            beginSetPlaybackRange(std::get<SetPlaybackRangeCommand>(command));
            return;
        }
        if (std::holds_alternative<StartRangePlaybackCommand>(command)) {
            beginStartRangePlayback(std::get<StartRangePlaybackCommand>(command));
            return;
        }
        if (std::holds_alternative<SetPlaybackContinuityPolicyCommand>(command)) {
            beginSetPlaybackContinuityPolicy(std::get<SetPlaybackContinuityPolicyCommand>(command));
            return;
        }
        if (std::holds_alternative<SetActiveComparisonPairCommand>(command)) {
            beginSetActiveComparisonPair(std::get<SetActiveComparisonPairCommand>(command));
            return;
        }
        const bool isOpenCommand = std::holds_alternative<OpenComparisonCommand>(command) ||
                                   std::holds_alternative<OpenDirectComparisonCommand>(command);
        const bool invalidatesAnalysis =
            isOpenCommand || std::holds_alternative<CloseSessionCommand>(command) ||
            std::holds_alternative<SetAlignmentOffsetsCommand>(command) ||
            std::holds_alternative<SetManualAlignmentAnchorCommand>(command) ||
            std::holds_alternative<ClearManualAlignmentAnchorsCommand>(command);
        if (invalidatesAnalysis && analysisJob_.has_value()) {
            abandonAlignmentAnalysis(CancellationReason::Superseded);
        }
        if (isOpenCommand && !pending_.has_value() && !playbackRun_.has_value() &&
            pendingProbe_.has_value()) {
            // A new open supersedes in-flight probes instead of bouncing off a Busy gate.
            supersedePendingProbes();
        }
        if (isOpenCommand && playbackRun_.has_value()) {
            // Source-count transitions are controlled session rebuilds. Pause and discard the
            // active cadence before probing the replacement set so old-generation frames cannot
            // mix with the new source topology.
            stopPlayback();
        }
        const bool isNavigationCommand =
            std::holds_alternative<SeekFrameCommand>(command) ||
            std::holds_alternative<StepFramesCommand>(command) ||
            std::holds_alternative<FirstFrameCommand>(command) ||
            std::holds_alternative<LastFrameCommand>(command) ||
            std::holds_alternative<SetAlignmentOffsetsCommand>(command) ||
            std::holds_alternative<EstimateAlignmentCommand>(command) ||
            std::holds_alternative<AnalyzeSequenceAlignmentCommand>(command) ||
            std::holds_alternative<ConfirmAutomaticAlignmentCommand>(command) ||
            std::holds_alternative<UndoAutomaticAlignmentCommand>(command) ||
            std::holds_alternative<RestoreSequenceAlignmentCommand>(command) ||
            std::holds_alternative<SetManualAlignmentAnchorCommand>(command) ||
            std::holds_alternative<ClearManualAlignmentAnchorsCommand>(command);
        // An interactive ±1 of the same direction extends the active stream and must not be
        // treated as a navigational discontinuity that stops playback first. Other commands remain
        // navigation (seek, ±N, direction flip, alignment, …).
        if (isNavigationCommand && !extendsActiveInteractiveStepStream(command)) {
            if (pendingProbe_.has_value()) {
                completeCommand(context, CommandOutcome::Busy);
                return;
            }
            // Manual navigation outside an active range stops the loop but keeps the markers.
            if (playbackRange_.has_value() && playbackRangeLoop_ &&
                state_.displayedFrame.has_value()) {
                const auto outsideRange = [this](const domain::FrameId target) {
                    return target.value() < playbackRange_->inInclusive.value() ||
                           target.value() > playbackRange_->outInclusive.value();
                };
                std::optional<domain::FrameId> navigationTarget;
                if (const auto* seek = std::get_if<SeekFrameCommand>(&command)) {
                    navigationTarget = seek->frameId;
                } else if (const auto* step = std::get_if<StepFramesCommand>(&command)) {
                    const std::int64_t current =
                        state_.requestedFrame
                            .value_or(state_.displayedFrame.value_or(domain::FrameId{0}))
                            .value();
                    navigationTarget = domain::FrameId{current + step->delta};
                }
                if (navigationTarget.has_value() && outsideRange(*navigationTarget)) {
                    playbackRangeLoop_ = false;
                    if (playbackRun_.has_value()) {
                        playbackRun_->rangeLoop = false;
                    }
                }
            }
            // Frame navigation pauses an active playback run first, and supersedes an in-flight
            // exact seek so rapid presses coalesce onto the newest target (USERPLAN 3.1/6.2).
            if (playbackRun_.has_value()) {
                stopPlayback();
            }
            if (pending_.has_value()) {
                if (pending_->phase == PendingPhase::kSeekingFrame) {
                    supersedePendingSeek();
                } else {
                    completeCommand(context, CommandOutcome::Busy);
                    return;
                }
            }
        } else if (pendingProbe_.has_value()) {
            completeCommand(context, CommandOutcome::Busy);
            return;
        }
        // An interactive ±1 of the same direction extends the active stream; any other navigation
        // invalidates the warm cursor and tears the stream down before the new command proceeds.
        if (isNavigationCommand && interactiveStepRun_.has_value() &&
            !extendsActiveInteractiveStepStream(command)) {
            // Preserve the newest requested target across the cancel so a direction-flip step can
            // chain from it (e.g. forward +1 in flight then -1 lands on the same frame - 1).
            const std::optional<domain::FrameId> preservedRequested = state_.requestedFrame;
            cancelInteractiveStepRun(InteractiveStepStopReason::NavigationSuperseded);
            if (std::holds_alternative<StepFramesCommand>(command) &&
                preservedRequested.has_value()) {
                state_.requestedFrame = preservedRequested;
            }
        }
        if (pending_.has_value() || pendingProbe_.has_value() || playbackRun_.has_value()) {
            // Extending an interactive stream must still be admitted: the stream itself is the
            // active media operation, not a Busy condition.
            if (!(isNavigationCommand && extendsActiveInteractiveStepStream(command))) {
                completeCommand(context, CommandOutcome::Busy);
                return;
            }
        }

        std::visit(
            [this](const auto& value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, OpenDirectComparisonCommand>) {
                    beginOpenDirect(value);
                } else if constexpr (std::is_same_v<Value, OpenComparisonCommand>) {
                    beginOpenPaths(value);
                } else if constexpr (std::is_same_v<Value, SeekFrameCommand>) {
                    beginSeek(value.context, value.frameId);
                } else if constexpr (std::is_same_v<Value, StepFramesCommand>) {
                    beginStep(value);
                } else if constexpr (std::is_same_v<Value, FirstFrameCommand>) {
                    beginSeek(value.context, domain::FrameId{0});
                } else if constexpr (std::is_same_v<Value, LastFrameCommand>) {
                    if (state_.canonicalFrameCount == 0U) {
                        rejectCommand(
                            value.context,
                            CommandOutcome::Failed,
                            coordinatorError(domain::MediaErrorCode::kInvalidFrameId,
                                             "The comparison set has no canonical final frame.",
                                             false));
                    } else {
                        beginSeek(value.context,
                                  domain::FrameId{
                                      static_cast<std::int64_t>(state_.canonicalFrameCount - 1U)});
                    }
                } else if constexpr (std::is_same_v<Value, PlayCommand>) {
                    beginPlay(value);
                } else if constexpr (std::is_same_v<Value, PauseCommand>) {
                    // Pause is handled before the general active-operation admission gate.
                } else if constexpr (std::is_same_v<Value, SetPlaybackRangeCommand>) {
                    // Handled before the general active-operation admission gate.
                } else if constexpr (std::is_same_v<Value, StartRangePlaybackCommand>) {
                    // Handled before the general active-operation admission gate.
                } else if constexpr (std::is_same_v<Value, SetPlaybackContinuityPolicyCommand>) {
                    // Handled before the general active-operation admission gate.
                } else if constexpr (std::is_same_v<Value, SetActiveComparisonPairCommand>) {
                    // Handled before the general active-operation admission gate.
                } else if constexpr (std::is_same_v<Value, SetPlaybackRateCommand>) {
                    // Rate changes are handled before the general active-operation admission
                    // gate so they pass while playback runs.
                } else if constexpr (std::is_same_v<Value, SetAlignmentOffsetsCommand>) {
                    beginSetAlignmentOffsets(value);
                } else if constexpr (std::is_same_v<Value, EstimateAlignmentCommand>) {
                    beginEstimateAlignment(value);
                } else if constexpr (std::is_same_v<Value, AnalyzeSequenceAlignmentCommand>) {
                    beginAnalyzeSequence(value);
                } else if constexpr (std::is_same_v<Value, CancelAlignmentAnalysisCommand>) {
                    // Cancellation is handled before the foreground admission gate.
                } else if constexpr (std::is_same_v<Value, ConfirmAutomaticAlignmentCommand>) {
                    beginConfirmAutomaticAlignment(value);
                } else if constexpr (std::is_same_v<Value, UndoAutomaticAlignmentCommand>) {
                    beginUndoAutomaticAlignment(value);
                } else if constexpr (std::is_same_v<Value, RestoreSequenceAlignmentCommand>) {
                    beginRestoreSequenceAlignment(value);
                } else if constexpr (std::is_same_v<Value, SetManualAlignmentAnchorCommand>) {
                    beginSetManualAnchor(value);
                } else if constexpr (std::is_same_v<Value, ClearManualAlignmentAnchorsCommand>) {
                    beginClearManualAnchors(value);
                } else if constexpr (std::is_same_v<Value, CloseSessionCommand>) {
                    beginClose(value);
                }
            },
            std::move(command));
    }

    [[nodiscard]] bool matchesPending(const EventContext& context) const noexcept {
        if (!pending_.has_value()) {
            return false;
        }
        if (pending_->phase == PendingPhase::kOpeningProvider ||
            pending_->phase == PendingPhase::kClosingProvider) {
            return matchesContext(context, pending_->providerContext);
        }
        return pending_->frameContext.has_value() &&
               matchesContext(context, *pending_->frameContext);
    }

    void succeedPending() {
        if (!pending_.has_value()) {
            return;
        }
        const PendingPhase phase = pending_->phase;
        if (phase == PendingPhase::kOpeningProvider) {
            submitFirstOrSeekFrame(pending_->expectedFrame.value_or(domain::FrameId{0}),
                                   PendingPhase::kOpeningFirstFrame);
            return;
        }
        if (phase == PendingPhase::kOpeningFirstFrame || phase == PendingPhase::kSeekingFrame) {
            pending_->providerSucceeded = true;
            commitPresentedFrameIfComplete();
            return;
        }
        const CommandContext command = pending_->command;
        pending_.reset();
        resetToEmpty();
        publishSnapshot();
        completeCommand(command, CommandOutcome::Succeeded);
    }

    void commitPresentedFrameIfComplete() {
        if (!pending_.has_value() || !pending_->set.has_value() || !pending_->framePublished ||
            !pending_->providerSucceeded || !pending_->framePresented ||
            !pending_->presentationTimerId.has_value()) {
            return;
        }

        static_cast<void>(dependencies_.deadlineScheduler->cancel(*pending_->presentationTimerId));
        const CommandContext command = pending_->command;
        const bool rollbackAttempt = pending_->rollbackAttempt;
        const std::optional<CommandOutcome> terminalOutcomeOverride =
            pending_->terminalOutcomeOverride;
        const std::optional<domain::MediaError> terminalErrorOverride =
            pending_->terminalErrorOverride;
        const domain::FrameId displayedFrame = pending_->set->canonicalFrameId();
        state_.presentedSources.clear();
        state_.presentedSources.reserve(pending_->set->sources().size());
        for (const MappedSourceFrame& source : pending_->set->sources()) {
            state_.presentedSources.push_back(PresentedSourceState{
                .sourceId = source.sourceId,
                .sourceFrameId = source.sourceFrameId,
                .matchKind = source.matchKind,
                .alignmentConfidence = source.alignmentConfidence,
                .missingReason = source.missingReason,
                .presentationTime = source.presentationTime,
            });
        }
        pending_.reset();
        if (rollbackAttempt && openRollback_.has_value()) {
            const domain::SessionEpoch sessionEpoch = state_.sessionEpoch;
            const domain::PlaybackGeneration playbackGeneration = state_.playbackGeneration;
            const domain::DeviceGeneration deviceGeneration = state_.deviceGeneration;
            const bool graphicsReady = state_.graphicsReady;
            std::vector<PresentedSourceState> presentedSources = std::move(state_.presentedSources);

            state_ = std::move(openRollback_->snapshot);
            state_.sessionEpoch = sessionEpoch;
            state_.playbackGeneration = playbackGeneration;
            state_.deviceGeneration = deviceGeneration;
            state_.graphicsReady = graphicsReady;
            state_.sessionState = domain::SessionState::kReady;
            state_.playbackState = domain::PlaybackState::kPaused;
            state_.displayedFrame = displayedFrame;
            state_.requestedFrame.reset();
            state_.presentedSources = std::move(presentedSources);
            alignmentOffsets_ = std::move(openRollback_->alignmentOffsets);
            sequenceAlignmentMaps_ = std::move(openRollback_->sequenceAlignmentMaps);
            automaticAlignmentProposal_ = std::move(openRollback_->automaticAlignmentProposal);
            automaticAlignmentUndo_ = std::move(openRollback_->automaticAlignmentUndo);
            prefetchScheduler_ = std::move(openRollback_->prefetchScheduler);
            openRollback_.reset();
            state_.lastError = terminalErrorOverride;
            publishSnapshot();
            completeCommand(command,
                            terminalOutcomeOverride.value_or(CommandOutcome::Failed),
                            terminalErrorOverride);
            const ExactPrefetchWindow window = exactPrefetchWindow();
            submitPrefetch(prefetchScheduler_.afterExact(
                displayedFrame, state_.canonicalFrameCount, window.ahead, window.behind));
            return;
        }
        openRollback_.reset();
        if (!state_.validatedComparison && sources_) {
            state_.validatedComparison =
                std::make_shared<const domain::ValidatedComparisonSet>(*sources_);
        }
        state_.sessionState = domain::SessionState::kReady;
        state_.playbackState = domain::PlaybackState::kPaused;
        state_.displayedFrame = displayedFrame;
        state_.requestedFrame.reset();
        state_.lastError.reset();
        publishSnapshot();
        completeCommand(command, CommandOutcome::Succeeded);
        const ExactPrefetchWindow window = exactPrefetchWindow();
        submitPrefetch(prefetchScheduler_.afterExact(
            displayedFrame, state_.canonicalFrameCount, window.ahead, window.behind));
    }

    [[nodiscard]] bool matchesPlaybackFrame(const EventContext& context) const noexcept {
        return playbackRun_.has_value() && playbackRun_->frame.has_value() &&
               matchesContext(context, playbackRun_->frame->context);
    }

    [[nodiscard]] bool matchesPreparedPlaybackFrame(const EventContext& context) const noexcept {
        return playbackRun_.has_value() && playbackRun_->preparedFrame.has_value() &&
               matchesContext(context, playbackRun_->preparedFrame->context);
    }

    void publishPlaybackFrameIfReady() {
        if (!playbackRun_.has_value() || !playbackRun_->frame.has_value()) {
            return;
        }
        PendingPlaybackFrame& frame = *playbackRun_->frame;
        if (!frame.presentationRequested || !frame.set.has_value() || frame.framePublished) {
            return;
        }
        if (dependencies_.renderChannel->publish(frame.context, *frame.set) ==
            RenderPublishResult::Closed) {
            stopPlayback(coordinatorError(domain::MediaErrorCode::kMediaDecodeFailed,
                                          "The render channel closed during sequential playback."));
            return;
        }
        frame.framePublished = true;
        emitTrace(TraceEventKind::RenderPublished,
                  makeTraceIdentity(),
                  static_cast<std::uint64_t>(frame.set->canonicalFrameId().value()));
        commitPlaybackFrameIfComplete();
    }

    void commitPlaybackFrameIfComplete() {
        if (!playbackRun_.has_value() || !playbackRun_->frame.has_value()) {
            return;
        }
        PendingPlaybackFrame& frame = *playbackRun_->frame;
        if (!frame.set.has_value() || !frame.framePublished || !frame.providerSucceeded ||
            !frame.framePresented || !frame.presentationTimerId.has_value()) {
            return;
        }

        static_cast<void>(dependencies_.deadlineScheduler->cancel(*frame.presentationTimerId));
        const domain::FrameId displayedFrame = frame.expectedFrame;
        const bool pauseRequested = playbackRun_->pauseRequested;
        const bool reachedEnd =
            static_cast<std::uint64_t>(displayedFrame.value()) + 1U >= state_.canonicalFrameCount;
        state_.presentedSources.clear();
        state_.presentedSources.reserve(frame.set->sources().size());
        for (const MappedSourceFrame& source : frame.set->sources()) {
            state_.presentedSources.push_back(PresentedSourceState{
                .sourceId = source.sourceId,
                .sourceFrameId = source.sourceFrameId,
                .matchKind = source.matchKind,
                .alignmentConfidence = source.alignmentConfidence,
                .missingReason = source.missingReason,
                .presentationTime = source.presentationTime,
            });
        }
        playbackRun_->frame.reset();
        state_.displayedFrame = displayedFrame;
        state_.requestedFrame.reset();
        state_.lastError.reset();
        publishPlaybackSnapshot();
        if (playbackRun_->range.has_value()) {
            const PlaybackRange& range = *playbackRun_->range;
            if (displayedFrame.value() >= range.outInclusive.value()) {
                // Out is fully presented. Loop re-anchors inside the kernel; non-loop pauses.
                // Single-frame ranges never spin: they present once and stop.
                if (playbackRun_->rangeLoop &&
                    range.inInclusive.value() < range.outInclusive.value()) {
                    if (playbackRun_->preparedFrame.has_value()) {
                        dependencies_.directFrameProvider->cancel(playbackRun_->providerContext);
                        playbackRun_->preparedFrame.reset();
                    }
                    playbackRun_->completedLoops += 1U;
                    playbackRangeCompletedLoops_ = playbackRun_->completedLoops;
                    playbackRun_->restartFromEnd = false;
                    playbackRun_->anchorFrame = range.inInclusive;
                    playbackRun_->firstTarget = range.inInclusive;
                    playbackRun_->nextMinimum = range.inInclusive;
                    playbackRun_->wallAnchor = dependencies_.clock->now();
                    publishPlaybackSnapshot();
                    static_cast<void>(schedulePlaybackTarget(range.inInclusive));
                    return;
                }
                if (playbackRun_->preparedFrame.has_value()) {
                    dependencies_.directFrameProvider->cancel(playbackRun_->providerContext);
                }
                emitTrace(TraceEventKind::PlaybackRunStopped,
                          makeTraceIdentity(),
                          static_cast<std::uint64_t>(displayedFrame.value()));
                playbackRangeCompletedLoops_ = playbackRun_->completedLoops;
                playbackRun_.reset();
                lastPlaybackProjectionAt_.reset();
                state_.playbackState = domain::PlaybackState::kPaused;
                publishSnapshot();
                return;
            }
        }
        if (pauseRequested || reachedEnd) {
            if (playbackRun_->preparedFrame.has_value()) {
                dependencies_.directFrameProvider->cancel(playbackRun_->providerContext);
            }
            emitTrace(TraceEventKind::PlaybackRunStopped,
                      makeTraceIdentity(),
                      static_cast<std::uint64_t>(displayedFrame.value()));
            playbackRangeCompletedLoops_ = playbackRun_.has_value() ? playbackRun_->completedLoops
                                                                    : playbackRangeCompletedLoops_;
            playbackRun_.reset();
            lastPlaybackProjectionAt_.reset();
            state_.playbackState = domain::PlaybackState::kPaused;
            publishSnapshot();
            return;
        }

        if (playbackRun_->restartFromEnd) {
            // frame 0 of a restart-from-end just committed: re-anchor on the absolute canonical
            // timeline at frame 0 and continue from frame 1.
            playbackRun_->restartFromEnd = false;
            playbackRun_->anchorFrame = domain::FrameId{0};
            playbackRun_->firstTarget = domain::FrameId{1};
            playbackRun_->nextMinimum = domain::FrameId{1};
            playbackRun_->wallAnchor = dependencies_.clock->now();
        } else {
            playbackRun_->nextMinimum = domain::FrameId{displayedFrame.value() + 1};
        }
        if (playbackRun_->range.has_value()) {
            const std::int64_t ceiling = playbackCeiling(*playbackRun_);
            if (playbackRun_->nextMinimum.value() > ceiling) {
                playbackRun_->nextMinimum = domain::FrameId{ceiling};
            }
        }
        const domain::FrameId nextTarget = playbackTargetAt(dependencies_.clock->now());
        // C-07: wall-clock catch-up that advances past nextMinimum skips whole FrameSets.
        if (playbackRun_.has_value() && nextTarget.value() > playbackRun_->nextMinimum.value()) {
            playbackSkippedFrameSets_ +=
                static_cast<std::uint64_t>(nextTarget.value() - playbackRun_->nextMinimum.value());
        }
        static_cast<void>(schedulePlaybackTarget(nextTarget));
    }

    [[nodiscard]] bool handlePlaybackTerminal(const RequestTerminal& terminal) {
        const EventContext& terminalContext = std::visit(
            [](const auto& value) -> const EventContext& { return value.context; }, terminal);
        if (matchesPreparedPlaybackFrame(terminalContext)) {
            if (std::holds_alternative<RequestSucceeded>(terminal)) {
                playbackRun_->preparedFrame->providerSucceeded = true;
            } else {
                playbackRun_->preparedFrame.reset();
            }
            return true;
        }
        if (!matchesPlaybackFrame(terminalContext)) {
            return false;
        }
        if (std::holds_alternative<RequestSucceeded>(terminal)) {
            playbackRun_->frame->providerSucceeded = true;
            commitPlaybackFrameIfComplete();
            return true;
        }
        if (const auto* const failed = std::get_if<RequestFailed>(&terminal)) {
            stopPlayback(failed->error);
            return true;
        }
        stopPlayback(coordinatorError(domain::MediaErrorCode::kMediaDecodeFailed,
                                      "The active sequential playback request was canceled."));
        return true;
    }

    void handleTerminal(const RequestTerminal& terminal) {
        const EventContext& terminalContext = std::visit(
            [](const auto& value) -> const EventContext& { return value.context; }, terminal);
        if (const auto* const requestContext = std::get_if<RequestContext>(&terminalContext)) {
            if (PendingProbeSlot* const slot = probeSlot(*requestContext); slot != nullptr) {
                if (std::holds_alternative<RequestSucceeded>(terminal)) {
                    slot->succeeded = true;
                    // With same-path dedup, also mark shared slots (same path) as succeeded so
                    // that finishProbeIfComplete sees every slot ready.
                    for (auto& other : pendingProbe_->slots) {
                        if (&other != slot && other.sourcePath == slot->sourcePath) {
                            other.succeeded = true;
                        }
                    }
                    finishProbeIfComplete();
                    return;
                }
                if (const auto* const failed = std::get_if<RequestFailed>(&terminal)) {
                    failProbe(failed->error, CommandOutcome::Failed, domain::SessionState::kError);
                    return;
                }
                const auto& canceled = std::get<RequestCanceled>(terminal);
                failProbe(probeCoordinatorError(domain::MediaErrorCode::kMediaProbeFailed,
                                                slot->sourceId,
                                                "Media probing was canceled."),
                          canceled.reason == CancellationReason::Shutdown
                              ? CommandOutcome::Closed
                              : CommandOutcome::Canceled,
                          domain::SessionState::kError);
                return;
            }
        }

        if (handlePlaybackTerminal(terminal)) {
            return;
        }

        if (handleInteractiveStepTerminal(terminal)) {
            return;
        }

        const auto matches = std::visit(
            [this](const auto& value) { return matchesPending(value.context); }, terminal);
        if (!matches) {
            return;
        }

        if (std::holds_alternative<RequestSucceeded>(terminal)) {
            succeedPending();
            return;
        }
        if (const auto* const failed = std::get_if<RequestFailed>(&terminal)) {
            failPending(failed->error, CommandOutcome::Failed);
            return;
        }
        failPending(coordinatorError(domain::MediaErrorCode::kMediaDecodeFailed,
                                     "The direct frame provider canceled the active operation."),
                    CommandOutcome::Canceled);
    }

    void handleProbeCompleted(ProbeCompleted completed) {
        PendingProbeSlot* const slot = probeSlot(completed.context);
        if (slot == nullptr) {
            return;
        }
        if (completed.sourceId != slot->sourceId) {
            failProbe(probeCoordinatorError(domain::MediaErrorCode::kMediaProbeFailed,
                                            slot->sourceId,
                                            "A probe published a descriptor for the wrong source."),
                      CommandOutcome::Failed,
                      domain::SessionState::kError);
            return;
        }
        // The first ProbeCompleted for a slot owns both its descriptor and its timeline. A
        // duplicate ProbeCompleted for the same source that arrives before its separate terminal
        // is ignored wholesale, so it can neither overwrite the descriptor nor fill a
        // previously-null CFR timeline. Stale events are rejected by probeSlot() above.
        if (slot->descriptor.has_value()) {
            return;
        }
        slot->descriptor = std::move(completed.descriptor);
        slot->timeline = std::move(completed.timeline);
        if (const auto* const descriptor = slot->descriptor ? &*slot->descriptor : nullptr;
            descriptor != nullptr) {
            const bool isVfr =
                descriptor->timingConfidence == domain::TimingConfidence::kVariableFrameRate;
            if (isVfr) {
                // A VFR source must publish a runtime timeline whose shared_ptr is non-null before
                // it can be dereferenced; an engaged optional may still hold a null pointer.
                if (!slot->timeline.has_value() || !*slot->timeline) {
                    failProbe(
                        probeCoordinatorError(domain::MediaErrorCode::kMediaProbeFailed,
                                              slot->sourceId,
                                              "A VFR source must publish a runtime timeline."),
                        CommandOutcome::Failed,
                        domain::SessionState::kError);
                    return;
                }
                if ((*slot->timeline)->frameCount() != descriptor->frameCount.value) {
                    failProbe(probeCoordinatorError(
                                  domain::MediaErrorCode::kMediaProbeFailed,
                                  slot->sourceId,
                                  "The VFR timeline length does not match the source frame count."),
                              CommandOutcome::Failed,
                              domain::SessionState::kError);
                    return;
                }
            } else if (slot->timeline.has_value()) {
                failProbe(
                    probeCoordinatorError(domain::MediaErrorCode::kMediaProbeFailed,
                                          slot->sourceId,
                                          "A CFR source must not publish a runtime timeline."),
                    CommandOutcome::Failed,
                    domain::SessionState::kError);
                return;
            }
        }

        // Same-path dedup: copy the validated descriptor and timeline to every other slot that
        // shares this probe's source path, so finishProbeIfComplete sees all slots filled after
        // a single probe's payload and terminal.
        const std::filesystem::path primaryPath = slot->sourcePath;
        for (auto& other : pendingProbe_->slots) {
            if (&other != slot && other.sourcePath == primaryPath &&
                !other.descriptor.has_value()) {
                other.descriptor = slot->descriptor;
                other.timeline = slot->timeline;
            }
        }

        finishProbeIfComplete();
    }

    void handleFrameSet(const FrameSetReady& ready) {
        emitTrace(TraceEventKind::FrameSetReady,
                  makeTraceIdentity(),
                  static_cast<std::uint64_t>(ready.set.canonicalFrameId().value()),
                  makeIncomingIdentity(ready.context));
        if (matchesInteractiveStepFrame(ready.context)) {
            PendingPlaybackFrame& frame = interactiveStepRun_->frame->frame;
            if (ready.set.canonicalFrameId() != frame.expectedFrame) {
                // The provider published a frame set for the wrong canonical frame: a malformed /
                // mismatched FrameSet is a hard failure — surface it via lastError.
                failInteractiveStepRun(coordinatorError(
                    domain::MediaErrorCode::kMediaDecodeFailed,
                    "The provider published a mismatched interactive step frame set."));
                return;
            }
            if (frame.framePublished) {
                return;
            }
            frame.set = ready.set;
            publishInteractiveStepFrameIfReady();
            return;
        }
        if (matchesInteractiveStepPreparedFrame(ready.context)) {
            PendingPlaybackFrame& frame = interactiveStepRun_->preparedFrame->frame;
            if (ready.set.canonicalFrameId() != frame.expectedFrame) {
                // A mismatched prepared frame cannot become the current frame; drop it and re-queue
                // its command so it is re-submitted once the pipeline advances.
                CommandContext dropped = interactiveStepRun_->preparedFrame->command;
                interactiveStepRun_->preparedFrame.reset();
                interactiveStepRun_->queuedCommands.push_front(std::move(dropped));
                return;
            }
            if (!frame.set.has_value()) {
                frame.set = ready.set;
            }
            return;
        }
        if (playbackRun_.has_value() && playbackRun_->frame.has_value() &&
            ready.context == playbackRun_->frame->context) {
            PendingPlaybackFrame& frame = *playbackRun_->frame;
            if (ready.set.canonicalFrameId() != frame.expectedFrame) {
                stopPlayback(coordinatorError(
                    domain::MediaErrorCode::kMediaDecodeFailed,
                    "The provider published a mismatched sequential playback frame set."));
                return;
            }
            if (frame.framePublished) {
                return;
            }
            frame.set = ready.set;
            publishPlaybackFrameIfReady();
            return;
        }
        if (playbackRun_.has_value() && playbackRun_->preparedFrame.has_value() &&
            ready.context == playbackRun_->preparedFrame->context) {
            PendingPlaybackFrame& frame = *playbackRun_->preparedFrame;
            if (ready.set.canonicalFrameId() != frame.expectedFrame) {
                stopPlayback(coordinatorError(
                    domain::MediaErrorCode::kMediaDecodeFailed,
                    "The provider prepared a mismatched sequential playback frame set."));
                return;
            }
            if (!frame.set.has_value()) {
                frame.set = ready.set;
            }
            return;
        }
        if (!pending_.has_value() || !pending_->frameContext.has_value() ||
            ready.context != *pending_->frameContext) {
            return;
        }
        if (!pending_->expectedFrame.has_value() ||
            ready.set.canonicalFrameId() != *pending_->expectedFrame) {
            failPending(
                coordinatorError(domain::MediaErrorCode::kMediaDecodeFailed,
                                 "The provider published a different exact frame than requested."),
                CommandOutcome::Failed);
            return;
        }
        if (pending_->framePublished) {
            return;
        }
        if (dependencies_.renderChannel->publish(ready.context, ready.set) ==
            RenderPublishResult::Closed) {
            failPending(coordinatorError(
                            domain::MediaErrorCode::kMediaDecodeFailed,
                            "The render channel closed before it accepted the complete frame set."),
                        CommandOutcome::Canceled);
            return;
        }
        pending_->set = ready.set;
        pending_->framePublished = true;
        emitTrace(TraceEventKind::RenderPublished,
                  makeTraceIdentity(),
                  static_cast<std::uint64_t>(ready.set.canonicalFrameId().value()));
        commitPresentedFrameIfComplete();
    }

    [[nodiscard]] bool matchesAnalysis(const AlignmentAnalysisJobId jobId,
                                       const PlaybackRequestContext& context,
                                       const AlignmentAnalysisKind kind) const noexcept {
        return analysisJob_.has_value() && analysisJob_->jobId == jobId &&
               analysisJob_->context == context && analysisJob_->kind == kind;
    }

    void clearAnalysisProgress() {
        state_.alignmentAnalysisJobId.reset();
        state_.alignmentAnalysisKind.reset();
        state_.alignmentAnalysisPhase.reset();
        state_.alignmentAnalysisCompletedUnits = 0U;
        state_.alignmentAnalysisWork = {};
    }

    void failAnalysis(domain::MediaError error, const CommandOutcome outcome) {
        if (!analysisJob_.has_value()) {
            return;
        }
        const CommandContext command = analysisJob_->command;
        analysisJob_.reset();
        clearAnalysisProgress();
        state_.lastError = error;
        publishSnapshot();
        completeCommand(command, outcome, std::move(error));
    }

    void handleAnalysisStarted(const AlignmentAnalysisStarted& started) {
        if (!matchesAnalysis(started.jobId, started.context, started.kind)) {
            return;
        }
        state_.alignmentAnalysisPhase = AlignmentAnalysisPhase::CollectingSignatures;
        state_.alignmentAnalysisCompletedUnits = 0U;
        state_.alignmentAnalysisWork = started.work;
        publishSnapshot();
    }

    void handleAnalysisProgress(const AlignmentAnalysisProgress& progress) {
        if (!matchesAnalysis(progress.jobId, progress.context, progress.kind) ||
            progress.work != state_.alignmentAnalysisWork ||
            progress.completedUnits > progress.work.totalUnits ||
            progress.completedUnits < state_.alignmentAnalysisCompletedUnits) {
            return;
        }
        state_.alignmentAnalysisPhase = progress.phase;
        state_.alignmentAnalysisCompletedUnits = progress.completedUnits;
        publishSnapshot();
    }

    void handleAnalysisCompleted(AlignmentAnalysisCompleted completed) {
        if (!matchesAnalysis(completed.jobId, completed.context, completed.kind) ||
            !sources_.has_value()) {
            return;
        }
        if (completed.kind == AlignmentAnalysisKind::GlobalOffset) {
            if (!completed.sequenceResults.empty() ||
                completed.estimates.size() + 1U != sources_->sources().size()) {
                failAnalysis(coordinatorError(domain::MediaErrorCode::kMediaDecodeFailed,
                                              "Global analysis published an invalid result set."),
                             CommandOutcome::Failed);
                return;
            }
            for (std::size_t index = 0U; index < completed.estimates.size(); ++index) {
                const GlobalOffsetEstimate& estimate = completed.estimates[index];
                const bool duplicate = std::any_of(completed.estimates.begin() +
                                                       static_cast<std::ptrdiff_t>(index + 1U),
                                                   completed.estimates.end(),
                                                   [&estimate](const GlobalOffsetEstimate& other) {
                                                       return other.sourceId == estimate.sourceId;
                                                   });
                const bool confidenceValid =
                    estimate.confidence >= 0.0F && estimate.confidence <= 1.0F &&
                    estimate.bestCost >= 0.0F && estimate.bestCost <= 1.0F &&
                    estimate.runnerUpCost >= 0.0F && estimate.runnerUpCost <= 1.0F;
                if (duplicate || sources_->find(estimate.sourceId) == nullptr ||
                    estimate.sourceId == sources_->canonicalSourceId() ||
                    estimate.bestOffset < -64 || estimate.bestOffset > 64 ||
                    estimate.evidenceCount == 0U || !confidenceValid) {
                    failAnalysis(
                        coordinatorError(domain::MediaErrorCode::kMediaDecodeFailed,
                                         "Global analysis published an invalid source result."),
                        CommandOutcome::Failed);
                    return;
                }
            }
            analysisJob_->estimates = std::move(completed.estimates);
        } else {
            if (!completed.estimates.empty() ||
                completed.sequenceResults.size() + 1U != sources_->sources().size()) {
                failAnalysis(coordinatorError(domain::MediaErrorCode::kMediaDecodeFailed,
                                              "Sequence analysis published an invalid result set."),
                             CommandOutcome::Failed);
                return;
            }
            for (std::size_t index = 0U; index < completed.sequenceResults.size(); ++index) {
                const SequenceAlignmentResult& result = completed.sequenceResults[index];
                const domain::ComparisonSource* const source = sources_->find(result.sourceId);
                const bool duplicate = std::any_of(completed.sequenceResults.begin() +
                                                       static_cast<std::ptrdiff_t>(index + 1U),
                                                   completed.sequenceResults.end(),
                                                   [&result](const SequenceAlignmentResult& other) {
                                                       return other.sourceId == result.sourceId;
                                                   });
                const bool summaryValid = result.totalCost >= 0.0F &&
                                          result.meanMatchCost >= 0.0F &&
                                          result.meanMatchCost <= 1.0F &&
                                          result.confidence >= 0.0F && result.confidence <= 1.0F;
                if (source == nullptr || duplicate ||
                    result.sourceId == sources_->canonicalSourceId() || !summaryValid ||
                    result.entries.size() != state_.canonicalFrameCount) {
                    failAnalysis(
                        coordinatorError(domain::MediaErrorCode::kMediaDecodeFailed,
                                         "Sequence analysis published an invalid source result."),
                        CommandOutcome::Failed);
                    return;
                }
                for (std::size_t frame = 0U; frame < result.entries.size(); ++frame) {
                    const SequenceAlignmentEntry& entry = result.entries[frame];
                    const bool confidenceValid =
                        entry.confidence >= 0.0F && entry.confidence <= 1.0F;
                    const bool missing = !entry.sourceFrameId.has_value();
                    if (entry.canonicalFrameId.value() != static_cast<std::int64_t>(frame) ||
                        !confidenceValid ||
                        missing != (entry.matchKind == FrameMatchKind::Missing) ||
                        (entry.sourceFrameId.has_value() &&
                         (!entry.sourceFrameId->isValid() ||
                          entry.sourceFrameId->value() >= source->descriptor.frameCount.value))) {
                        failAnalysis(coordinatorError(
                                         domain::MediaErrorCode::kMediaDecodeFailed,
                                         "Sequence analysis published an invalid frame mapping."),
                                     CommandOutcome::Failed);
                        return;
                    }
                }
                std::size_t nextSegmentFrame = 0U;
                for (const SequenceAlignmentSegment& segment : result.segments) {
                    const bool metricsValid =
                        segment.meanConfidence >= 0.0F && segment.meanConfidence <= 1.0F &&
                        segment.p10Confidence >= 0.0F && segment.p10Confidence <= 1.0F &&
                        segment.anomalyDensity >= 0.0F && segment.anomalyDensity <= 1.0F &&
                        segment.mappingSlope >= 0.0F && segment.mappingSlope <= 4.0F;
                    if (!segment.firstCanonicalFrame.isValid() ||
                        !segment.lastCanonicalFrame.isValid() ||
                        segment.firstCanonicalFrame.value() !=
                            static_cast<std::int64_t>(nextSegmentFrame) ||
                        segment.lastCanonicalFrame < segment.firstCanonicalFrame ||
                        static_cast<std::size_t>(segment.lastCanonicalFrame.value()) >=
                            result.entries.size() ||
                        !metricsValid) {
                        failAnalysis(
                            coordinatorError(
                                domain::MediaErrorCode::kMediaDecodeFailed,
                                "Sequence analysis published an invalid confidence segment."),
                            CommandOutcome::Failed);
                        return;
                    }
                    nextSegmentFrame =
                        static_cast<std::size_t>(segment.lastCanonicalFrame.value()) + 1U;
                }
                if (!result.segments.empty() && nextSegmentFrame != result.entries.size()) {
                    failAnalysis(
                        coordinatorError(domain::MediaErrorCode::kMediaDecodeFailed,
                                         "Sequence confidence segments do not cover the timeline."),
                        CommandOutcome::Failed);
                    return;
                }
            }
            analysisJob_->sequenceResults = std::move(completed.sequenceResults);
        }
        state_.alignmentAnalysisPhase = AlignmentAnalysisPhase::ComputingAlignment;
        state_.alignmentAnalysisCompletedUnits = state_.alignmentAnalysisWork.totalUnits;
        analysisJob_->completed = true;
    }

    void handleAnalysisCanceled(const AlignmentAnalysisCanceled& canceled) {
        if (!matchesAnalysis(canceled.jobId, canceled.context, canceled.kind)) {
            return;
        }
        const CommandContext command = analysisJob_->command;
        analysisJob_.reset();
        clearAnalysisProgress();
        publishSnapshot();
        completeCommand(command, CommandOutcome::Canceled);
    }

    void handleAnalysisFailed(AlignmentAnalysisFailed failed) {
        if (!matchesAnalysis(failed.jobId, failed.context, failed.kind)) {
            return;
        }
        failAnalysis(std::move(failed.error), CommandOutcome::Failed);
    }

    void maybeFinalizeAnalysis() {
        if (!analysisJob_.has_value() || !analysisJob_->completed || !state_.graphicsReady ||
            pending_.has_value() || pendingProbe_.has_value() || playbackRun_.has_value() ||
            !state_.displayedFrame.has_value()) {
            return;
        }

        const CommandContext command = analysisJob_->command;
        if (analysisJob_->kind == AlignmentAnalysisKind::GlobalOffset &&
            analysisJob_->estimates.has_value()) {
            state_.alignmentEstimates = std::move(*analysisJob_->estimates);
            state_.sequenceAlignments.clear();
            const bool hasApplicableEstimate = std::any_of(
                state_.alignmentEstimates.begin(),
                state_.alignmentEstimates.end(),
                [](const GlobalOffsetEstimate& estimate) { return estimate.autoApplicable; });
            automaticAlignmentProposal_ = AutomaticAlignmentProposal{
                .kind = AlignmentAnalysisKind::GlobalOffset,
                .estimates = state_.alignmentEstimates,
            };
            state_.automaticAlignmentPending = true;
            state_.canConfirmAutomaticAlignment =
                hasApplicableEstimate && !state_.alignmentRequired;
        } else if (analysisJob_->kind == AlignmentAnalysisKind::Sequence &&
                   analysisJob_->sequenceResults.has_value()) {
            std::vector<SequenceAlignmentResult> analyzed =
                std::move(*analysisJob_->sequenceResults);
            state_.sequenceAlignments.clear();
            state_.sequenceAlignments.reserve(analyzed.size());
            bool hasReviewableSegment = false;
            for (const SequenceAlignmentResult& result : analyzed) {
                state_.sequenceAlignments.push_back(detail::summarizeSequenceAlignment(result));
                hasReviewableSegment =
                    hasReviewableSegment ||
                    std::any_of(result.segments.begin(),
                                result.segments.end(),
                                [](const SequenceAlignmentSegment& segment) {
                                    return segment.state != AlignmentSegmentState::Rejected;
                                }) ||
                    (result.segments.empty() && result.autoApplicable);
            }
            automaticAlignmentProposal_ = AutomaticAlignmentProposal{
                .kind = AlignmentAnalysisKind::Sequence,
                .sequenceResults = std::move(analyzed),
            };
            state_.automaticAlignmentPending = true;
            state_.canConfirmAutomaticAlignment = hasReviewableSegment;
        } else {
            failAnalysis(coordinatorError(domain::MediaErrorCode::kMediaDecodeFailed,
                                          "Analysis completed without the expected payload."),
                         CommandOutcome::Failed);
            return;
        }

        analysisJob_.reset();
        clearAnalysisProgress();
        publishSnapshot();
        completeCommand(command, CommandOutcome::Succeeded);
    }

    void beginConfirmAutomaticAlignment(const ConfirmAutomaticAlignmentCommand& command) {
        if (!sources_.has_value() || state_.sessionState != domain::SessionState::kReady ||
            !state_.displayedFrame.has_value() || !automaticAlignmentProposal_.has_value() ||
            !state_.canConfirmAutomaticAlignment || analysisJob_.has_value()) {
            rejectCommand(
                command.context,
                CommandOutcome::Failed,
                coordinatorError(
                    domain::MediaErrorCode::kInvalidArgument,
                    state_.alignmentRequired && automaticAlignmentProposal_.has_value() &&
                            automaticAlignmentProposal_->kind == AlignmentAnalysisKind::GlobalOffset
                        ? "Timing mismatches require sequence analysis before "
                          "automatic alignment can be confirmed."
                        : "There is no confirmable automatic alignment proposal.",
                    false));
            return;
        }

        automaticAlignmentUndo_ = AutomaticAlignmentUndoState{
            .offsets = alignmentOffsets_,
            .sequenceMaps = sequenceAlignmentMaps_,
            .alignmentRequired = state_.alignmentRequired,
        };
        const AutomaticAlignmentProposal proposal = std::move(*automaticAlignmentProposal_);
        bool mappingChanged = false;
        if (proposal.kind == AlignmentAnalysisKind::GlobalOffset) {
            const bool hadSequenceMappings = !sequenceAlignmentMaps_.empty();
            sequenceAlignmentMaps_.clear();
            std::vector<SourceFrameOffset> nextOffsets = alignmentOffsets_;
            for (const GlobalOffsetEstimate& estimate : state_.alignmentEstimates) {
                if (!estimate.autoApplicable) {
                    continue;
                }
                const auto existing = std::find_if(
                    nextOffsets.begin(), nextOffsets.end(), [&estimate](const auto& offset) {
                        return offset.sourceId == estimate.sourceId;
                    });
                if (estimate.bestOffset == 0) {
                    if (existing != nextOffsets.end()) {
                        nextOffsets.erase(existing);
                    }
                } else {
                    const SourceFrameOffset accepted{
                        .sourceId = estimate.sourceId,
                        .frames = estimate.bestOffset,
                        .matchKind = FrameMatchKind::AutoAligned,
                        .confidence = estimate.confidence,
                    };
                    if (existing == nextOffsets.end()) {
                        nextOffsets.push_back(accepted);
                    } else {
                        *existing = accepted;
                    }
                }
            }
            const auto bySource = [](const SourceFrameOffset& left,
                                     const SourceFrameOffset& right) {
                return left.sourceId < right.sourceId;
            };
            std::sort(alignmentOffsets_.begin(), alignmentOffsets_.end(), bySource);
            std::sort(nextOffsets.begin(), nextOffsets.end(), bySource);
            mappingChanged = hadSequenceMappings || nextOffsets != alignmentOffsets_;
            alignmentOffsets_ = std::move(nextOffsets);
        } else {
            std::vector<SequenceAlignmentSummary> summaries;
            std::vector<SequenceAlignmentResult> accepted;
            summaries.reserve(proposal.sequenceResults.size());
            accepted.reserve(proposal.sequenceResults.size());
            bool hasRejectedSegment = false;
            for (SequenceAlignmentResult result : proposal.sequenceResults) {
                for (SequenceAlignmentSegment& segment : result.segments) {
                    if (segment.state == AlignmentSegmentState::ReviewRequired) {
                        segment.state = AlignmentSegmentState::Accepted;
                    } else if (segment.state == AlignmentSegmentState::Rejected) {
                        hasRejectedSegment = true;
                    }
                }
                const bool hasAcceptedSegment =
                    result.segments.empty()
                        ? result.autoApplicable
                        : std::any_of(result.segments.begin(),
                                      result.segments.end(),
                                      [](const SequenceAlignmentSegment& segment) {
                                          return segment.state == AlignmentSegmentState::Accepted;
                                      });
                result.autoApplicable = hasAcceptedSegment;
                summaries.push_back(detail::summarizeSequenceAlignment(result));
                if (hasAcceptedSegment) {
                    accepted.push_back(std::move(result));
                }
            }
            mappingChanged = accepted != sequenceAlignmentMaps_;
            state_.sequenceAlignments = std::move(summaries);
            sequenceAlignmentMaps_ = std::move(accepted);
            state_.alignmentRequired = hasRejectedSegment;
        }

        clearAutomaticProposal();
        state_.canUndoAutomaticAlignment = true;
        if (mappingChanged) {
            state_.alignmentRevision = increment(state_.alignmentRevision);
            prefetchScheduler_.reset();
            beginSeek(command.context, *state_.displayedFrame);
        } else {
            publishSnapshot();
            completeCommand(command.context, CommandOutcome::Succeeded);
        }
    }

    void beginUndoAutomaticAlignment(const UndoAutomaticAlignmentCommand& command) {
        if (!sources_.has_value() || state_.sessionState != domain::SessionState::kReady ||
            !state_.displayedFrame.has_value() || !automaticAlignmentUndo_.has_value() ||
            analysisJob_.has_value()) {
            rejectCommand(command.context,
                          CommandOutcome::Failed,
                          coordinatorError(domain::MediaErrorCode::kInvalidArgument,
                                           "There is no automatic alignment change to undo.",
                                           false));
            return;
        }
        clearAutomaticProposal();
        const AutomaticAlignmentUndoState previous = std::move(*automaticAlignmentUndo_);
        automaticAlignmentUndo_.reset();
        const bool mappingChanged = alignmentOffsets_ != previous.offsets ||
                                    sequenceAlignmentMaps_ != previous.sequenceMaps;
        alignmentOffsets_ = previous.offsets;
        sequenceAlignmentMaps_ = previous.sequenceMaps;
        state_.alignmentRequired = previous.alignmentRequired;
        state_.canUndoAutomaticAlignment = false;
        if (mappingChanged) {
            state_.alignmentRevision = increment(state_.alignmentRevision);
            prefetchScheduler_.reset();
            beginSeek(command.context, *state_.displayedFrame);
        } else {
            publishSnapshot();
            completeCommand(command.context, CommandOutcome::Succeeded);
        }
    }

    void beginRestoreSequenceAlignment(const RestoreSequenceAlignmentCommand& command) {
        if (!sources_.has_value() || state_.sessionState != domain::SessionState::kReady ||
            !state_.displayedFrame.has_value() || !command.sequenceResults ||
            analysisJob_.has_value() ||
            !validateDerivedSequenceAlignments(*sources_, *command.sequenceResults)) {
            rejectCommand(command.context,
                          CommandOutcome::Failed,
                          coordinatorError(
                              domain::MediaErrorCode::kInvalidArgument,
                              "The derived sequence-alignment cache is invalid for this session.",
                              false));
            return;
        }

        invalidateAutomaticAlignmentHistory();
        sequenceAlignmentMaps_ = *command.sequenceResults;
        state_.sequenceAlignments.clear();
        state_.sequenceAlignments.reserve(sequenceAlignmentMaps_.size());
        state_.alignmentRequired = false;
        for (const SequenceAlignmentResult& result : sequenceAlignmentMaps_) {
            state_.sequenceAlignments.push_back(detail::summarizeSequenceAlignment(result));
            state_.alignmentRequired =
                state_.alignmentRequired ||
                std::any_of(result.segments.begin(),
                            result.segments.end(),
                            [](const SequenceAlignmentSegment& segment) {
                                return segment.state == AlignmentSegmentState::Rejected;
                            });
        }
        state_.alignmentRevision = increment(state_.alignmentRevision);
        prefetchScheduler_.reset();
        beginSeek(command.context, *state_.displayedFrame);
    }

    void handleFramePresented(const FrameSetPresented& presented) {
        emitTrace(TraceEventKind::PresentationAcknowledged,
                  makeTraceIdentity(),
                  static_cast<std::uint64_t>(presented.frameId.value()),
                  makeIncomingIdentity(presented.context));
        if (matchesInteractiveStepFrame(presented.context) &&
            interactiveStepRun_->frame->frame.framePublished &&
            presented.frameId == interactiveStepRun_->frame->frame.expectedFrame) {
            interactiveStepRun_->frame->frame.framePresented = true;
            commitInteractiveStepFrameIfComplete();
            return;
        }
        if (playbackRun_.has_value() && playbackRun_->frame.has_value() &&
            playbackRun_->frame->framePublished &&
            presented.context == playbackRun_->frame->context &&
            presented.frameId == playbackRun_->frame->expectedFrame) {
            playbackRun_->frame->framePresented = true;
            commitPlaybackFrameIfComplete();
            return;
        }
        if (!pending_.has_value() || !pending_->frameContext.has_value() ||
            !pending_->expectedFrame.has_value() || !pending_->framePublished ||
            presented.context != *pending_->frameContext ||
            presented.frameId != *pending_->expectedFrame) {
            return;
        }
        pending_->framePresented = true;
        commitPresentedFrameIfComplete();
    }

    void handleDeadline(const DeadlineElapsed& elapsed) {
        if (playbackRun_.has_value() && playbackRun_->cadenceTimerId.has_value() &&
            playbackRun_->cadenceTarget.has_value() &&
            elapsed.context == playbackRun_->cadenceContext &&
            elapsed.timerId == *playbackRun_->cadenceTimerId) {
            const domain::FrameId scheduledTarget = *playbackRun_->cadenceTarget;
            playbackRun_->cadenceTimerId.reset();
            playbackRun_->cadenceTarget.reset();
            const domain::FrameId dueTarget =
                playbackRun_->restartFromEnd
                    ? scheduledTarget
                    : std::max(scheduledTarget, playbackTargetAt(dependencies_.clock->now()));
            static_cast<void>(activatePlaybackTarget(dueTarget));
            return;
        }
        if (playbackRun_.has_value() && playbackRun_->frame.has_value() &&
            playbackRun_->frame->presentationTimerId.has_value() &&
            elapsed.context == playbackRun_->frame->context.playback &&
            elapsed.timerId == *playbackRun_->frame->presentationTimerId) {
            stopPlayback(
                presentationError("The playback frame was not presented within five seconds."));
            return;
        }
        if (interactiveStepRun_.has_value() && interactiveStepRun_->frame.has_value() &&
            interactiveStepRun_->frame->frame.presentationTimerId.has_value() &&
            elapsed.context == interactiveStepRun_->frame->frame.context.playback &&
            elapsed.timerId == *interactiveStepRun_->frame->frame.presentationTimerId) {
            // A frame decoded but never presented within the deadline is a presentation failure —
            // surface it via lastError (plan M1.2: presentation timeout is a failure, not a
            // cancel).
            failInteractiveStepRun(presentationError(
                "The interactive step frame was not presented within five seconds."));
            return;
        }
        if (!pending_.has_value() || !pending_->frameContext.has_value() ||
            !pending_->presentationTimerId.has_value() ||
            elapsed.context != pending_->frameContext->playback ||
            elapsed.timerId != *pending_->presentationTimerId) {
            return;
        }
        failPending(presentationError("The exact frame was not presented within five seconds."),
                    CommandOutcome::Failed);
    }

    void handleGraphicsReady(const GraphicsDeviceReady& ready) {
        if (ready.context.deviceGeneration < state_.deviceGeneration) {
            return;
        }
        state_.deviceGeneration = ready.context.deviceGeneration;
        state_.graphicsReady = true;
        emitTrace(TraceEventKind::DeviceGenerationChanged,
                  makeTraceIdentity(),
                  static_cast<std::uint64_t>(state_.deviceGeneration.value()));
        if (state_.lastError.has_value() &&
            (state_.lastError->code == domain::MediaErrorCode::kGraphicsUnavailable ||
             state_.lastError->code == domain::MediaErrorCode::kGraphicsDeviceLost)) {
            state_.lastError.reset();
        }
        publishSnapshot();
    }

    void handleGraphicsFailure(const GraphicsEventContext& context,
                               const domain::MediaError& error) {
        if (context.deviceGeneration < state_.deviceGeneration) {
            return;
        }
        // Surface the error once up front so it is set on every non-stale event regardless of which
        // branch fires — failPending does not touch lastError, and with no run active nothing else
        // would set it. (failInteractiveStepRun/stopPlayback also set it internally and publish
        // immediately; the trailing publishSnapshot remains the authoritative publish of the final
        // graphicsReady/playbackState. A second publish in the interactive/playback branches is
        // idempotent and harmless.)
        state_.lastError = error;
        if (pendingProbe_.has_value()) {
            failProbe(error, CommandOutcome::Failed, domain::SessionState::kError);
        }
        if (pending_.has_value()) {
            failPending(error, CommandOutcome::Failed);
        }
        if (interactiveStepRun_.has_value()) {
            // An unrecoverable graphics/media failure is a hard failure for the interactive stream.
            failInteractiveStepRun(error);
        }
        if (playbackRun_.has_value()) {
            stopPlayback(error, false);
        }
        state_.deviceGeneration = context.deviceGeneration;
        state_.graphicsReady = false;
        state_.playbackState = domain::PlaybackState::kPaused;
        publishSnapshot();
    }

    void handleEvent(ApplicationEvent event) {
        std::visit(
            [this](const auto& value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, RequestTerminal>) {
                    handleTerminal(value);
                } else if constexpr (std::is_same_v<Value, ProbeCompleted>) {
                    handleProbeCompleted(value);
                } else if constexpr (std::is_same_v<Value, FrameSetReady>) {
                    handleFrameSet(value);
                } else if constexpr (std::is_same_v<Value, FrameSetPresented>) {
                    handleFramePresented(value);
                } else if constexpr (std::is_same_v<Value, AlignmentAnalysisStarted>) {
                    handleAnalysisStarted(value);
                } else if constexpr (std::is_same_v<Value, AlignmentAnalysisProgress>) {
                    handleAnalysisProgress(value);
                } else if constexpr (std::is_same_v<Value, AlignmentAnalysisCompleted>) {
                    handleAnalysisCompleted(value);
                } else if constexpr (std::is_same_v<Value, AlignmentAnalysisCanceled>) {
                    handleAnalysisCanceled(value);
                } else if constexpr (std::is_same_v<Value, AlignmentAnalysisFailed>) {
                    handleAnalysisFailed(value);
                } else if constexpr (std::is_same_v<Value, DeadlineElapsed>) {
                    handleDeadline(value);
                } else if constexpr (std::is_same_v<Value, GraphicsDeviceReady>) {
                    handleGraphicsReady(value);
                } else if constexpr (std::is_same_v<Value, GraphicsDeviceUnavailable> ||
                                     std::is_same_v<Value, GraphicsDeviceLost>) {
                    handleGraphicsFailure(value.context, value.error);
                }
            },
            std::move(event));
    }

    [[nodiscard]] bool hasWorkLocked() const noexcept {
        return !criticalEvents_.empty() || !commands_.empty() || realtimeEvent_.has_value();
    }

    [[nodiscard]] WorkItem takeWorkLocked() {
        if (!criticalEvents_.empty()) {
            ApplicationEvent event = std::move(criticalEvents_.front());
            criticalEvents_.pop_front();
            return WorkItem{std::in_place_index<1>, std::move(event)};
        }
        if (!commands_.empty()) {
            PlaybackCommand command = std::move(commands_.front());
            commands_.pop_front();
            return WorkItem{std::in_place_index<0>, std::move(command)};
        }
        ApplicationEvent event = std::move(*realtimeEvent_);
        realtimeEvent_.reset();
        return WorkItem{std::in_place_index<1>, std::move(event)};
    }

    void run() noexcept {
        for (;;) {
            std::optional<WorkItem> work;
            {
                std::unique_lock lock(ingressMutex_);
                condition_.wait(lock, [this] { return shuttingDown_ || hasWorkLocked(); });
                if (!hasWorkLocked()) {
                    if (shuttingDown_) {
                        return;
                    }
                    continue;
                }
                work.emplace(takeWorkLocked());
            }
            condition_.notify_all();
            if (std::holds_alternative<PlaybackCommand>(*work)) {
                handleCommand(std::move(std::get<PlaybackCommand>(*work)));
            } else {
                handleEvent(std::move(std::get<ApplicationEvent>(*work)));
            }
            maybeFinalizeAnalysis();
        }
    }

    void shutdownImpl() noexcept {
        {
            std::scoped_lock lock(ingressMutex_);
            if (shuttingDown_) {
                return;
            }
            shuttingDown_ = true;
            criticalIngressClosed_ = true;
            realtimeIngressClosed_ = true;
            realtimeEvent_.reset();
        }
        condition_.notify_all();
        eventSink_->detach();
        if (worker_.joinable()) {
            worker_.join();
        }
        if (pendingProbe_.has_value()) {
            for (const auto& slot : pendingProbe_->slots) {
                dependencies_.mediaProbe->cancel(slot.context);
            }
        }
        if (pending_.has_value()) {
            if (pending_->presentationTimerId.has_value()) {
                static_cast<void>(
                    dependencies_.deadlineScheduler->cancel(*pending_->presentationTimerId));
            }
            dependencies_.directFrameProvider->cancel(pending_->providerContext);
            if ((pending_->phase == PendingPhase::kOpeningFirstFrame ||
                 pending_->phase == PendingPhase::kSeekingFrame) &&
                pending_->frameContext.has_value()) {
                dependencies_.renderChannel->clear(pending_->providerContext);
            }
        }
        if (analysisJob_.has_value() && dependencies_.alignmentAnalysisService) {
            dependencies_.alignmentAnalysisService->cancel(analysisJob_->jobId);
        }
        if (playbackRun_.has_value()) {
            stopPlayback(std::nullopt, false);
        }
        // Shutdown supersedes any active interactive stream. The cancel path drains every
        // not-yet-presented step command as Closed (Shutdown reason) so shutdown stays bounded and
        // the UI does not retain a dangling frame-pending indicator. This replaces the hand-rolled
        // teardown that previously duplicated stopInteractiveStepRun's logic.
        if (interactiveStepRun_.has_value()) {
            cancelInteractiveStepRun(InteractiveStepStopReason::Shutdown);
        }
    }

    Dependencies dependencies_;
    std::shared_ptr<CoordinatorEventSinkGate> eventSink_;
    SessionSnapshot state_;
    std::optional<domain::ValidatedComparisonSet> sources_;
    std::optional<domain::CompatibilityReport> compatibilityReport_;
    std::vector<SourceFrameOffset> alignmentOffsets_;
    std::vector<SequenceAlignmentResult> sequenceAlignmentMaps_;
    std::optional<domain::CanonicalTimeline> canonicalTimeline_;
    std::optional<PendingCommand> pending_;
    std::optional<ReadySessionBackup> openRollback_;
    std::optional<PendingProbe> pendingProbe_;
    std::optional<PlaybackRun> playbackRun_;
    // Rate chosen while paused; applied by the next PlayCommand so play resumes at this speed.
    std::optional<double> pendingPlaybackSpeed_;
    // Session-level playback range authority. Survives pause; applied by the next PlayCommand and
    // live-updated into an active PlaybackRun.
    std::optional<PlaybackRange> playbackRange_;
    bool playbackRangeLoop_ = false;
    std::uint64_t playbackRangeCompletedLoops_ = 0U;
    // C-07/D03: requested continuity policy (Contextual is smoothness-first RealTime).
    domain::PlaybackContinuityPolicy playbackContinuityPolicy_ =
        domain::PlaybackContinuityPolicy::Contextual;
    std::uint64_t playbackSkippedFrameSets_ = 0U;
    // C-02: session pair + preference policy used to re-resolve after topology changes.
    domain::DefaultPairPolicy activePairPolicy_ = domain::DefaultPairPolicy::PreserveIfAvailable;
    // D06: monotonic playback-run id stamped on TraceIdentity.run for the whole PlaybackRun.
    std::uint64_t playbackRunId_ = 0U;
    std::optional<domain::ComparisonPair> activeComparisonPair_;
    std::optional<InteractiveStepRun> interactiveStepRun_;
    std::optional<std::chrono::steady_clock::time_point> lastPlaybackProjectionAt_;
    std::optional<std::chrono::steady_clock::time_point> lastInteractiveProjectionAt_;
    // Displayed frame of the most recent SnapshotCommitted; identical re-commits are suppressed
    // so every commit carries a payload with a prior ACK under the same identity (trace contract).
    std::optional<domain::FrameId> lastCommittedDisplayedFrame_;
    std::optional<BackgroundAnalysis> analysisJob_;
    std::optional<AutomaticAlignmentProposal> automaticAlignmentProposal_;
    std::optional<AutomaticAlignmentUndoState> automaticAlignmentUndo_;
    PrefetchScheduler prefetchScheduler_;
    std::uint64_t nextRequestId_ = 1U;
    std::uint64_t nextTimerId_ = 1U;
    std::uint64_t nextAnalysisJobId_ = 1U;
    // Topology/timeline revisions (plan OperationIdentity, 03§4). Topology revision tracks the
    // loaded source set; timeline revision tracks the canonical timeline. Both are monotonic and
    // carried on trace events so a trace can prove no stale commit across topology/timeline
    // changes. They are foundational identity for Phase 1+ but populated here so the Phase 0 trace
    // is already meaningful across these boundaries.
    domain::TopologyRevision topologyRevision_{0};
    domain::TimelineRevision timelineRevision_{0};
    std::unordered_set<CommandIdentity, CommandIdentityHash> seenCommands_;

    CoordinatorPublication publication_;

    std::mutex ingressMutex_;
    std::condition_variable condition_;
    std::deque<PlaybackCommand> commands_;
    std::deque<ApplicationEvent> criticalEvents_;
    std::optional<ApplicationEvent> realtimeEvent_;
    bool criticalIngressClosed_ = false;
    bool realtimeIngressClosed_ = false;
    bool shuttingDown_ = false;
    std::thread worker_;
};

std::shared_ptr<PlaybackCoordinator> PlaybackCoordinator::create(const domain::SessionId sessionId,
                                                                 Dependencies dependencies) {
    if (!dependencies.mediaProbe || !dependencies.directFrameProvider ||
        !dependencies.deadlineScheduler || !dependencies.clock || !dependencies.renderChannel) {
        return {};
    }

    return std::shared_ptr<PlaybackCoordinator>(
        new PlaybackCoordinator(sessionId, std::move(dependencies)));
}

PlaybackCoordinator::PlaybackCoordinator(const domain::SessionId sessionId,
                                         Dependencies dependencies)
    : impl_(std::make_unique<Impl>(sessionId, std::move(dependencies))) {}

PlaybackCoordinator::~PlaybackCoordinator() = default;

PortSubmitResult PlaybackCoordinator::submit(PlaybackCommand command) {
    return impl_->submit(std::move(command));
}

std::shared_ptr<const SessionSnapshot> PlaybackCoordinator::snapshot() const {
    return impl_->snapshot();
}

std::vector<CommandTerminal> PlaybackCoordinator::takeCompletedCommands() {
    return impl_->takeCompletedCommands();
}

std::shared_ptr<const std::vector<SequenceAlignmentResult>>
PlaybackCoordinator::acceptedSequenceAlignments() const {
    return impl_->acceptedSequenceAlignments();
}

std::shared_ptr<IApplicationEventSink> PlaybackCoordinator::eventSink() const noexcept {
    return impl_->eventSink();
}

void PlaybackCoordinator::shutdown() noexcept {
    impl_->shutdown();
}

EventPostResult PlaybackCoordinator::postCritical(ApplicationEvent event) noexcept {
    return impl_->postCritical(std::move(event));
}

EventPostResult PlaybackCoordinator::postRealtime(ApplicationEvent event) noexcept {
    return impl_->postRealtime(std::move(event));
}

void PlaybackCoordinator::closeRealtimeIngress() noexcept {
    impl_->closeRealtimeIngress();
}

void PlaybackCoordinator::closeCriticalIngress() noexcept {
    impl_->closeCriticalIngress();
}

} // namespace dvs::application
