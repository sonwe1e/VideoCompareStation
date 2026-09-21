#pragma once

#include "dvs/domain/Identifiers.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace dvs::application {

// The identity scope attached to every trace event. Mirrors the plan's OperationIdentity
// (03_目标架构设计.md §4) so a future analyzer can check command exactly-once,
// ACK-before-commit, and stale-result rejection. The current emission contract is not sufficient
// to prove all three invariants; see trace-schema.md for the explicit gaps.
struct TraceIdentity final {
    domain::SessionId session{0};
    domain::SessionEpoch epoch{0};
    domain::TopologyRevision topology{0};
    domain::TimelineRevision timeline{0};
    domain::AlignmentRevision alignment{0};
    domain::PlaybackGeneration generation{0};
    domain::DeviceGeneration device{0};
    domain::RequestId request{0};
    std::optional<domain::CommandId> command{};

    [[nodiscard]] bool operator==(const TraceIdentity&) const = default;
};

// Async arrival identity captured from the producer's EventContext at handling time. Topology,
// timeline, and alignment revisions are coordinator-owned and therefore stay on TraceIdentity
// only; this subset is what a stale provider result can actually carry.
struct TraceIncomingIdentity final {
    domain::SessionId session{0};
    domain::SessionEpoch epoch{0};
    domain::PlaybackGeneration generation{0};
    domain::DeviceGeneration device{0};
    domain::RequestId request{0};

    [[nodiscard]] bool operator==(const TraceIncomingIdentity&) const = default;
};

enum class TraceEventKind : std::uint8_t {
    CommandAccepted = 0,
    CommandRejected = 1,
    ProviderSubmitted = 2,
    ProviderCanceled = 3,
    FrameSetReady = 4,
    ProviderTerminal = 5,
    RenderPublished = 6,
    PresentationAcknowledged = 7,
    SnapshotCommitted = 8,
    CommandTerminal = 9,
    DecoderSeek = 10,
    DecoderReopen = 11,
    CacheHit = 12,
    DeviceGenerationChanged = 13,
    // UI-side scene-graph image grabs (the timeline thumbnail cache). Additive observation only:
    // the grab path is unchanged, but a gate can now test whether grabbing the viewport while
    // playback is presenting frames delays them, instead of inferring it from the grab's cost.
    QmlGrabRequested = 14,
    QmlGrabCompleted = 15,
    // Renderer-side staging of one published FrameSet: the point where the render thread actually
    // began the draw, and the point where it published the presentation acknowledgement. Together
    // with RenderPublished these separate "the renderer was never scheduled" from "the draw itself
    // was slow", which is the difference between a UI/scene-graph stall and a GPU stall.
    RenderDrawStarted = 16,
    // Payload is the canonical frame id, matching RenderPublished/PresentationAcknowledged.
    RenderAckPublished = 17,
    // Boundaries of one continuous playback run. Payload is the first target frame on start and
    // the last committed frame on stop (UINT64_MAX when nothing was committed). They let an
    // analyzer isolate the Running window from the open/seek/step phases in the same trace, which
    // a display-interval comparison has to do before attributing a stall to playback.
    PlaybackRunStarted = 18,
    PlaybackRunStopped = 19,
    // Reverse GOP Window (ADR-003). Payload is the number of frames retained in the built
    // window, or the reverse target frame id for Hit/Fallback so gates can correlate seeks.
    ReverseWindowBuilt = 20,
    ReverseWindowHit = 21,
    ReverseExactFallback = 22,
};

// A single fixed-size trace event. Kept small and trivially copyable so it can live in a bounded
// ring buffer with no heap allocation and no variable-length payloads on the hot path.
// `payload` carries event-specific data (e.g. a frame id, a request kind) and is interpreted
// per `kind`; see trace-schema.md for the encoding.
struct TraceEvent final {
    TraceIdentity identity;
    TraceEventKind kind{};
    std::uint64_t timestampMicroseconds{0};
    std::uint64_t payload{0};
    std::optional<TraceIncomingIdentity> incoming{};

    [[nodiscard]] bool operator==(const TraceEvent&) const = default;
};

// A platform-owned sink drains trace events for export. The application layer never performs
// disk I/O for tracing; it only hands completed events to a sink on a non-worker thread
// (see TraceSink.h in platform_windows). A null sink means tracing is disabled.
class ITraceSink {
public:
    virtual ~ITraceSink() = default;

    // Returns true only when the complete record was accepted. Export failures are folded into
    // the trace loss count so a successful overflow marker makes the incomplete capture explicit.
    // Sink callbacks must not re-enter PlaybackTrace lifecycle or drain operations.
    [[nodiscard]] virtual bool append(const TraceEvent& event) noexcept = 0;
    [[nodiscard]] virtual bool recordOverflow(std::uint64_t lostCount) noexcept = 0;

    // Completes a capture after all events and overflow records have been submitted. File-backed
    // sinks use this boundary to flush, close, and atomically publish the final output.
    [[nodiscard]] virtual bool finalize() noexcept = 0;
};

// Bounded multi-producer/single-consumer ring buffer. Events are produced by several threads
// (the coordinator worker and, for media-layer events, each per-source decode worker) and
// consumed later by a single export thread. Producers serialize briefly on the queue lock for a
// single-event copy and never touch the sink or perform I/O; drops happen only on capacity
// exhaustion. Queue drops and sink/export failures contribute to the lost count so export can
// flag an incomplete trace. This is a diagnostic facility that is disabled by default.
class PlaybackTraceBuffer final {
public:
    // 16K fills in under 20 s at three 60 fps sources (roughly 900 events/s); the T0 evidence
    // traces run 15 s with 1-3 sources plus the navigation gate's held-step bursts, so 64K
    // gives several times the headroom and keeps overflow a genuine anomaly, not the norm.
    static constexpr std::size_t kCapacity = 65536U;
    static_assert((kCapacity & (kCapacity - 1U)) == 0U, "capacity must be a power of two");

    // Sink installation is a lifecycle operation. Replacing a sink waits for an in-progress
    // drain, so after setSink(nullptr) returns the caller may safely destroy the previous sink.
    // This lifecycle lock is independent of the producer queue lock.
    void setSink(ITraceSink* sink) noexcept;

    // Records an event from any thread. Returns false (and counts it as lost) only when the
    // buffer is full. Serializes with other producers for a single-event copy; never performs I/O.
    [[nodiscard]] bool record(TraceEvent event) noexcept;

    // Drains up to `max` events into `out`, returning the count written. Single-consumer: must
    // only be called from the export thread. Events are removed from the buffer as they are read.
    std::size_t drain(TraceEvent* out, std::size_t max) noexcept;

    // Convenience for the export path: drains events and forwards each to the sink on the
    // consumer thread (never the worker). Failed sink writes count the affected dequeued events
    // as lost. Returns count dequeued.
    std::size_t drainToSink() noexcept;

    [[nodiscard]] std::uint64_t overflowCount() const noexcept;
    void reset() noexcept;

private:
    static constexpr std::size_t indexFor(std::uint64_t position) noexcept {
        return static_cast<std::size_t>(position & (kCapacity - 1U));
    }

    mutable std::mutex mutex_;
    mutable std::mutex sinkMutex_;
    ITraceSink* sink_ = nullptr;
    std::array<TraceEvent, kCapacity> buffer_;
    std::uint64_t head_ = 0U;
    std::uint64_t tail_ = 0U;
    std::atomic<std::uint64_t> overflow_{0U};
    std::uint64_t reportedOverflow_ = 0U;
};

// Process-global trace buffer. Constructed once and accessed via instance(); tests may reset it
// between cases after stopping producers. Recording supports multiple producers and one consumer.
class PlaybackTrace final {
public:
    using Clock = std::uint64_t (*)() noexcept;

    static PlaybackTrace& instance() noexcept;

    void installSink(ITraceSink* sink) noexcept;
    [[nodiscard]] bool enabled() const noexcept;

    // Enables recording with the given monotonic time source (microseconds since an arbitrary
    // epoch). Disabling drops all subsequent records and is the default, leaving only one atomic
    // clock read and branch unless a gate explicitly enables tracing.
    void enable(Clock nowMicroseconds) noexcept;
    void disable() noexcept;

    void record(TraceEventKind kind,
                const TraceIdentity& identity,
                std::uint64_t payload = 0U,
                std::optional<TraceIncomingIdentity> incoming = std::nullopt) noexcept;

    std::size_t drain(TraceEvent* out, std::size_t max) noexcept;
    std::size_t drainToSink() noexcept;
    [[nodiscard]] std::uint64_t overflowCount() const noexcept;
    void reset() noexcept;

private:
    PlaybackTrace() = default;

    std::atomic<Clock> now_{nullptr};
    PlaybackTraceBuffer buffer_;
};

// Convenience: resolve the current monotonic microsecond timestamp for a trace record. Defined
// in the application layer using std::chrono so the domain stays clock-free.
[[nodiscard]] std::uint64_t traceNowMicroseconds() noexcept;

} // namespace dvs::application
