#pragma once

#include "dvs/application/Alignment.h"
#include "dvs/application/Events.h"
#include "dvs/domain/FrameTimeline.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace dvs::application {

enum class PortSubmitResult {
    Accepted,
    Busy,
    Closed,
};

enum class FrameRequestPriority {
    Exact,
    Sequential,
    // Interactive reverse step (-1 stream). Decode remains exact/random-access at the provider
    // (compressed video cannot decode backwards), but the coordinator keeps one generation and a
    // current+prepared pipeline so held-backward does not storm Exact cancels the way the old
    // per-step seek path did. Providers may warm a reverse window/prefetch for F-1, F-2, …
    Reverse,
    Prefetch,
};

// Why a frame set was, or was not, handed to the render channel. The failure reasons stay
// apart because they are not the same failure: a device that is transitioning (no device, or a
// stale generation) recovers, a closed channel does not, and an unusable frame set points at the
// producer. Collapsing them into Closed reported a graphics transition as a decode failure.
enum class RenderPublishResult {
    Accepted,
    Replaced,
    // The channel or its owner is shut down; nothing is published through it again.
    Closed,
    // No graphics device is in service right now; the same channel publishes once one returns.
    DeviceUnavailable,
    // The request names a device generation or playback scope the channel no longer serves.
    StaleContext,
    // The channel could not inspect the frame set at all, which points at the producer.
    InvalidSet,
};

struct MediaProbeRequest final {
    RequestContext context;
    domain::SourceId sourceId;
    std::filesystem::path sourcePath;
};

struct FrameProviderOpenRequest final {
    PlaybackRequestContext context;
    // The session's review sources, one to three entries in session order. The provider opens
    // one decode slot per entry and publishes one FrameSet entry per slot.
    std::vector<domain::ComparisonSource> sources;
    // Canonical ownership is explicit because Reference is a semantic role, not a source-order
    // convention. Adapters must never infer the canonical source from sources.front().
    domain::SourceId canonicalSourceId = 0;
    // The canonical timeline carries either a rational CFR rate or an immutable, normalized,
    // zero-anchored VFR display-order timeline. The provider and the coordinator share one
    // VFR timeline without persisting derived per-frame data for the whole session.
    domain::CanonicalTimeline timeline;
};

struct FrameRequest final {
    FrameRequestContext context;
    domain::FrameId frameId;
    FrameRequestPriority priority;
    std::vector<SourceFrameOffset> sourceOffsets;
    std::uint64_t alignmentRevision = 0U;
};

// Runs bounded, decoder-backed evidence collection off the coordinator thread. The adapter
// publishes one estimate for every non-canonical source; only the coordinator decides whether a
// high-confidence estimate is applied to the active session.
struct AlignmentEstimateRequest final {
    PlaybackRequestContext context;
    domain::SourceId canonicalSourceId = 0;
    GlobalOffsetEstimationOptions options;
    std::size_t candidateSampleCount = 5U;
    AlignmentAnalysisJobId jobId;
    std::vector<domain::ComparisonSource> sources;
    std::optional<domain::CanonicalTimeline> timeline;
};

struct SequenceAlignmentRequest final {
    PlaybackRequestContext context;
    domain::SourceId canonicalSourceId = 0;
    std::vector<SourceFrameOffset> expectedOffsets;
    std::vector<SourceAlignmentAnchors> manualAnchors;
    SequenceAlignmentOptions options;
    std::size_t maximumFrameCount = 50'000U;
    AlignmentAnalysisJobId jobId;
    std::vector<domain::ComparisonSource> sources;
    std::optional<domain::CanonicalTimeline> timeline;
};

struct FrameProviderCloseRequest final {
    PlaybackRequestContext context;
};

struct SettingsLoadRequest final {
    RequestContext context;
};

struct SettingsSaveRequest final {
    RequestContext context;
    SettingsSnapshot settings;
};

struct DeadlineRequest final {
    PlaybackRequestContext context;
    std::uint64_t timerId;
    std::chrono::steady_clock::time_point due;
};

// Worker and relay threads use the critical lane for terminal, control, and exact-frame events.
// GUI and render threads must not call this interface directly; adapters relay those events on a
// non-render worker. Realtime publication is coalesced by implementation, never by callers.
class IApplicationEventSink {
public:
    virtual ~IApplicationEventSink() = default;

    [[nodiscard]] virtual EventPostResult postCritical(ApplicationEvent event) noexcept = 0;
    [[nodiscard]] virtual EventPostResult postRealtime(ApplicationEvent event) noexcept = 0;

    // The coordinator closes realtime ingress first. Critical ingress closes only after every
    // registered producer is quiescent and the critical queue has drained.
    virtual void closeRealtimeIngress() noexcept = 0;
    virtual void closeCriticalIngress() noexcept = 0;
};

class IMediaProbe {
public:
    virtual ~IMediaProbe() = default;

    // Probing is queued work. Adapters must downgrade this handle before retaining it so a
    // coordinator teardown cannot leave a delayed FFmpeg completion with a dangling sink.
    [[nodiscard]] virtual PortSubmitResult
    submit(const MediaProbeRequest& request, std::shared_ptr<IApplicationEventSink> events) = 0;
    virtual void cancel(const RequestContext& context) noexcept = 0;
};

class IFrameProvider {
public:
    virtual ~IFrameProvider() = default;

    // Frame requests may outlive a coordinator epoch. Retain only a weak sink after admission.
    [[nodiscard]] virtual PortSubmitResult
    submit(const FrameProviderOpenRequest& request,
           std::shared_ptr<IApplicationEventSink> events) = 0;
    [[nodiscard]] virtual PortSubmitResult
    submit(const FrameRequest& request, std::shared_ptr<IApplicationEventSink> events) = 0;
    [[nodiscard]] virtual PortSubmitResult
    submit(const FrameProviderCloseRequest& request,
           std::shared_ptr<IApplicationEventSink> events) = 0;
    virtual void cancel(const PlaybackRequestContext& context) noexcept = 0;
};

// Alignment analysis owns a decode path that is independent from playback. A successful submit
// admits a background job; completion is reported through job events and never reserves the
// coordinator's foreground command slot.
class IAlignmentAnalysisService {
public:
    virtual ~IAlignmentAnalysisService() = default;

    [[nodiscard]] virtual PortSubmitResult
    submit(const AlignmentEstimateRequest& request,
           std::shared_ptr<IApplicationEventSink> events) = 0;
    [[nodiscard]] virtual PortSubmitResult
    submit(const SequenceAlignmentRequest& request,
           std::shared_ptr<IApplicationEventSink> events) = 0;
    virtual void cancel(AlignmentAnalysisJobId jobId) noexcept = 0;
};

class ISettingsRepository {
public:
    virtual ~ISettingsRepository() = default;

    // Queued persistence work must not retain a raw event-sink pointer.
    [[nodiscard]] virtual PortSubmitResult
    submit(const SettingsLoadRequest& request, std::shared_ptr<IApplicationEventSink> events) = 0;
    [[nodiscard]] virtual PortSubmitResult
    submit(const SettingsSaveRequest& request, std::shared_ptr<IApplicationEventSink> events) = 0;
    virtual void cancel(const RequestContext& context) noexcept = 0;
};

class ISteadyClock {
public:
    virtual ~ISteadyClock() = default;

    [[nodiscard]] virtual std::chrono::steady_clock::time_point now() const noexcept = 0;
};

class IDeadlineScheduler {
public:
    virtual ~IDeadlineScheduler() = default;

    [[nodiscard]] virtual PortSubmitResult
    schedule(const DeadlineRequest& request, std::shared_ptr<IApplicationEventSink> events) = 0;

    // A successful cancellation guarantees that this timer cannot post after return.
    [[nodiscard]] virtual bool cancel(std::uint64_t timerId) noexcept = 0;
};

// This is the separate, single-slot path for high-frequency complete frame sets. It deliberately
// has no QML/Qt type and no method for publishing one source of a set.
class IRenderChannel {
public:
    virtual ~IRenderChannel() = default;

    [[nodiscard]] virtual RenderPublishResult publish(const FrameRequestContext& context,
                                                      FrameSet set) noexcept = 0;
    // Installs the scope tombstone that stops an older set from being published or drawn.
    // Returns false when it was refused - the channel is closed, or the caller's scope is not newer
    // than the one already tombstoned - so the caller can tell a stale frame may still be retained
    // instead of assuming the clear took effect.
    [[nodiscard]] virtual bool clear(const PlaybackRequestContext& context) noexcept = 0;
};

} // namespace dvs::application
