#pragma once

#include "dvs/application/Ports.h"
#include "dvs/domain/Result.h"

#include "SoftwareDecoder.h"
#include "SourceFrameCache.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace dvs::platform {
class FrameBudget;
class GraphicsDeviceBroker;
} // namespace dvs::platform

namespace dvs::media::internal {

enum class SourceDecodePriority {
    Exact,
    Sequential,
    // Reverse interactive step: same decoder/queue treatment as Exact (random access), kept
    // distinct so admission and telemetry can tell reverse-step work from ordinary seeks.
    Reverse,
    Prefetch,
};

struct SourceDecodeRequest final {
    domain::FrameId frameId{0};
    SourceDecodePriority priority = SourceDecodePriority::Exact;
    bool continueSequentially = false;
    std::uint8_t readAheadCount = 0U;
    // Desired Reverse GOP Window length (frames below the reverse target to retain). The actor
    // shrinks this to the byte budget; 0 disables window construction for the request.
    std::uint8_t reverseWindowFrames = 0U;
    // Shared ownership: speculative read-ahead outlives the completion callback that is the
    // provider's last guarantee the referenced flag storage stays alive.
    std::shared_ptr<const std::atomic<bool>> cancellationRequested;
    std::optional<application::FrameRequestContext> context;
};

struct SourceDecodeSubmission final {
    application::PortSubmitResult status = application::PortSubmitResult::Closed;
    std::future<domain::Result<DecodedFrame>> completion;
};

using SourceDecodeCompletion = std::function<void(domain::Result<DecodedFrame>)>;

// One long-lived worker owns one SoftwareDecoder for the complete open session. Its bounded
// priority mailboxes eliminate per-frame thread creation and keep decoder state single-threaded.
class SourceDecodeActor final {
public:
    SourceDecodeActor(domain::SourceId sourceId,
                      domain::MediaDescriptor descriptor,
                      platform::FrameBudget& frameBudget,
                      const std::atomic<bool>* externalInterrupt,
                      bool lowPriority = false,
                      std::size_t cacheCapacityBytes = 0U,
                      std::shared_ptr<platform::GraphicsDeviceBroker> deviceBroker = {});
    ~SourceDecodeActor();

    SourceDecodeActor(const SourceDecodeActor&) = delete;
    SourceDecodeActor& operator=(const SourceDecodeActor&) = delete;
    SourceDecodeActor(SourceDecodeActor&&) = delete;
    SourceDecodeActor& operator=(SourceDecodeActor&&) = delete;

    [[nodiscard]] domain::Status open(const std::atomic<bool>& cancellationRequested);
    [[nodiscard]] SourceDecodeSubmission submit(SourceDecodeRequest request);
    [[nodiscard]] application::PortSubmitResult submit(SourceDecodeRequest request,
                                                       SourceDecodeCompletion completion);
    void close() noexcept;
    void requestInterrupt() noexcept;
    void shutdown() noexcept;

    // Component-test diagnostic: every decode for this actor must execute on this stable worker.
    [[nodiscard]] std::thread::id workerThreadId() const noexcept;
    [[nodiscard]] std::thread::id lastDecodeThreadId() const noexcept;
    [[nodiscard]] std::uint64_t completedDecodeCount() const noexcept;
    [[nodiscard]] media::DecoderBackendStatus backendStatus() const;

private:
    enum class ControlKind {
        Open,
        Close,
    };

    struct DecodeJob final {
        SourceDecodeRequest request;
        SourceDecodeCompletion completion;
    };

    struct ControlJob final {
        ControlKind kind;
        const std::atomic<bool>* cancellationRequested = nullptr;
        std::promise<domain::Status> completion;
    };

    [[nodiscard]] std::deque<DecodeJob>& queueFor(SourceDecodePriority priority) noexcept;
    [[nodiscard]] bool hasPendingLocked() const noexcept;
    [[nodiscard]] std::optional<DecodeJob> takeNextDecodeLocked();
    void run() noexcept;
    void cancelQueuedLocked();
    void completeCanceled(DecodeJob job) noexcept;
    static void complete(DecodeJob job, domain::Result<DecodedFrame> result) noexcept;

    // ADR-003 Reverse GOP Window: one seed-seek + sequential walk fills the source cache with
    // reverse targets below `request.frameId`, so held-backward consumes cache instead of
    // exact-seeking every step. Budget-insufficient builds fall back to per-step Exact.
    void fillReverseGopWindow(const SourceDecodeRequest& request,
                              std::size_t frameBytes,
                              SoftwareDecoder& selectedDecoder,
                              const std::function<void(std::uint64_t)>& recordDecode,
                              const std::function<bool(const SourceDecodeRequest&)>& interrupted,
                              const std::function<void()>& refreshMetrics) noexcept;

    domain::SourceId sourceId_;
    std::int64_t sourceFrameCount_ = 0;
    std::unique_ptr<SoftwareDecoder> decoder_;
    // Bounded frames use an independent exact decoder so random navigation cannot disturb
    // playback state. It uses software below 50 FPS and D3D11VA for 50/60/120 FPS sources, where
    // one long GOP can contain hundreds of frames. Exact prefetch stays on the same decoder.
    std::unique_ptr<SoftwareDecoder> exactDecoder_;
    bool decoderNeedsReopen_ = false;
    bool exactDecoderNeedsReopen_ = false;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<ControlJob> controlQueue_;
    std::deque<DecodeJob> exactQueue_;
    std::deque<DecodeJob> sequentialQueue_;
    std::deque<DecodeJob> prefetchQueue_;
    std::optional<application::FrameRequestContext> latestContext_;
    bool stopping_ = false;
    bool started_ = false;
    std::thread worker_;
    std::thread::id workerThreadId_;
    std::thread::id lastDecodeThreadId_;
    std::uint64_t completedDecodeCount_ = 0U;
    std::uint64_t cacheHitCount_ = 0U;
    std::uint64_t totalDecodeMicroseconds_ = 0U;
    std::uint64_t maximumDecodeMicroseconds_ = 0U;
    std::uint64_t reverseWindowHitCount_ = 0U;
    std::uint64_t reverseWindowBuildCount_ = 0U;
    std::uint64_t reverseWindowBuiltFrameCount_ = 0U;
    std::uint64_t reverseWindowBuildMicroseconds_ = 0U;
    std::uint64_t reverseWindowBuildMaximumMicroseconds_ = 0U;
    std::uint64_t reverseExactFallbackCount_ = 0U;
    media::DecoderBackendStatus backendStatus_;
    SourceFrameCache cache_;
    SourceFrameCacheKey cacheKey_;
};

} // namespace dvs::media::internal
