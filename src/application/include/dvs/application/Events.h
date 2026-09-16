#pragma once

#include "dvs/application/FrameSet.h"
#include "dvs/application/RequestContext.h"
#include "dvs/domain/FrameTimeline.h"
#include "dvs/domain/MediaDescriptor.h"
#include "dvs/domain/MediaError.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace dvs::application {

inline constexpr std::size_t kCriticalEventCapacity = 256;
inline constexpr std::size_t kRealtimeEventCapacity = 32;
inline constexpr std::size_t kCommandIngressCapacity = 64;

enum class EventPostResult {
    Accepted,
    Closed,
};

enum class CancellationReason {
    Superseded,
    UserRequested,
    Shutdown,
};

enum class CommandOutcome {
    Succeeded,
    Failed,
    Canceled,
    Busy,
    Closed,
    TooLate,
};

// A terminal retains the most-specific scope supplied by the originating port. The coordinator
// must compare all members of that scope before accepting an asynchronous result.
using EventContext = std::variant<RequestContext, PlaybackRequestContext, FrameRequestContext>;

// Graphics device notifications deliberately have no session identity: a device can become
// available before a session starts, and a later loss must invalidate every session using it.
struct GraphicsEventContext final {
    domain::DeviceGeneration deviceGeneration;

    [[nodiscard]] constexpr bool operator==(const GraphicsEventContext&) const = default;
};

struct RequestSucceeded final {
    EventContext context;
};

struct RequestFailed final {
    EventContext context;
    domain::MediaError error;
};

struct RequestCanceled final {
    EventContext context;
    CancellationReason reason;
};

using RequestTerminal = std::variant<RequestSucceeded, RequestFailed, RequestCanceled>;

struct CommandTerminal final {
    CommandContext context;
    CommandOutcome outcome;
    std::optional<domain::MediaError> error;
};

struct ProbeCompleted final {
    RequestContext context;
    domain::SourceId sourceId;
    domain::MediaDescriptor descriptor;
    // VFR sources publish one normalized, zero-anchored display-order timeline that the
    // coordinator shares with the frame provider. CFR sources leave this nullopt.
    std::optional<std::shared_ptr<const domain::FrameTimeline>> timeline;
};

struct FrameSetReady final {
    FrameRequestContext context;
    FrameSet set;
};

struct FrameSetPresented final {
    FrameRequestContext context;
    domain::FrameId frameId;
};

struct AlignmentAnalysisStarted final {
    AlignmentAnalysisJobId jobId;
    PlaybackRequestContext context;
    AlignmentAnalysisKind kind;
    AlignmentWorkEstimate work;
};

struct AlignmentAnalysisProgress final {
    AlignmentAnalysisJobId jobId;
    PlaybackRequestContext context;
    AlignmentAnalysisKind kind;
    AlignmentAnalysisPhase phase = AlignmentAnalysisPhase::CollectingSignatures;
    std::uint64_t completedUnits = 0U;
    AlignmentWorkEstimate work;
};

struct AlignmentAnalysisCompleted final {
    AlignmentAnalysisJobId jobId;
    PlaybackRequestContext context;
    AlignmentAnalysisKind kind;
    std::vector<GlobalOffsetEstimate> estimates;
    std::vector<SequenceAlignmentResult> sequenceResults;
};

struct AlignmentAnalysisCanceled final {
    AlignmentAnalysisJobId jobId;
    PlaybackRequestContext context;
    AlignmentAnalysisKind kind;
    CancellationReason reason;
};

struct AlignmentAnalysisFailed final {
    AlignmentAnalysisJobId jobId;
    PlaybackRequestContext context;
    AlignmentAnalysisKind kind;
    domain::MediaError error;
};

struct GraphicsDeviceReady final {
    GraphicsEventContext context;
};

struct GraphicsDeviceUnavailable final {
    GraphicsEventContext context;
    domain::MediaError error;
};

struct GraphicsDeviceLost final {
    GraphicsEventContext context;
    domain::MediaError error;
};

struct DeadlineElapsed final {
    PlaybackRequestContext context;
    std::uint64_t timerId;
};

// Settings are intentionally key/value data at this boundary. JSON parsing and persistence stay
// in an adapter, and the coordinator can evolve the known key set without leaking a JSON type.
struct SettingsSnapshot final {
    std::map<std::string, std::string, std::less<>> values;
};

struct SettingsLoaded final {
    RequestContext context;
    SettingsSnapshot settings;
};

using ApplicationEvent = std::variant<RequestTerminal,
                                      CommandTerminal,
                                      ProbeCompleted,
                                      FrameSetReady,
                                      FrameSetPresented,
                                      AlignmentAnalysisStarted,
                                      AlignmentAnalysisProgress,
                                      AlignmentAnalysisCompleted,
                                      AlignmentAnalysisCanceled,
                                      AlignmentAnalysisFailed,
                                      GraphicsDeviceReady,
                                      GraphicsDeviceUnavailable,
                                      GraphicsDeviceLost,
                                      DeadlineElapsed,
                                      SettingsLoaded>;

} // namespace dvs::application
