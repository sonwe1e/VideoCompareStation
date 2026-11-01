#pragma once

#include "dvs/application/FrameSet.h"
#include "dvs/application/RequestContext.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace dvs::platform {

class FrameBudget;
class FrameMailbox;
class GraphicsDeviceBroker;
class IRenderActivitySink;

enum class GpuTransferSubmitResult {
    Accepted,
    Replaced,
    InvalidSet,
    StaleContext,
    DeviceUnavailable,
    Closed,
};

struct GpuTransferStatistics final {
    std::uint64_t submittedSets = 0U;
    std::uint64_t replacedSets = 0U;
    std::uint64_t publishedSets = 0U;
    std::uint64_t failedSets = 0U;
    std::uint64_t cancelledSets = 0U;
    std::uint64_t completedTransfers = 0U;
    std::uint64_t zeroCopySets = 0U;
    std::uint64_t totalTransferMicroseconds = 0U;
    std::uint64_t maximumTransferMicroseconds = 0U;
    std::uint64_t retiredResources = 0U;
    std::uint64_t deviceLossReports = 0U;
    // Device transitions this actor accepted work for again, i.e. recoveries that did not need a
    // new actor. A non-zero value means submit() revived a tombstoned generation.
    std::uint64_t recoveredGenerations = 0U;
    std::thread::id workerThread;
    std::thread::id lastRetirementThread;
};

// One worker exclusively owns upload commands, event-fence polling, and deferred GPU-resource
// destruction. submit(), clear(), and graphics rendering never call D3D or wait for this actor.
class GpuTransferActor final {
public:
    GpuTransferActor(std::shared_ptr<FrameBudget> frameBudget,
                     std::shared_ptr<GraphicsDeviceBroker> deviceBroker,
                     std::shared_ptr<FrameMailbox> frameMailbox,
                     std::weak_ptr<IRenderActivitySink> renderActivity = {});
    ~GpuTransferActor();

    GpuTransferActor(const GpuTransferActor&) = delete;
    GpuTransferActor& operator=(const GpuTransferActor&) = delete;
    GpuTransferActor(GpuTransferActor&&) = delete;
    GpuTransferActor& operator=(GpuTransferActor&&) = delete;

    [[nodiscard]] GpuTransferSubmitResult submit(const application::FrameRequestContext& context,
                                                 application::FrameSet set) noexcept;

    // Linearized with final mailbox publication. The mailbox tombstone is installed before return,
    // so an already-running upload cannot republish the cleared playback scope.
    [[nodiscard]] bool clear(const application::PlaybackRequestContext& context) noexcept;

    // Control/test-only waits. GUI and render threads must never call either method.
    [[nodiscard]] bool waitUntilIdle(std::chrono::milliseconds timeout) const noexcept;
    [[nodiscard]] bool shutdown(std::chrono::milliseconds timeout) noexcept;

    [[nodiscard]] bool isClosed() const noexcept;
    [[nodiscard]] GpuTransferStatistics statistics() const noexcept;

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace dvs::platform
