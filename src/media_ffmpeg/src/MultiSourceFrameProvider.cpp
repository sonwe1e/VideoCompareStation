#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "dvs/media/MultiSourceFrameProvider.h"

#include "dvs/domain/FrameTimeline.h"
#include "dvs/platform/FrameBudget.h"
#include "dvs/platform/GraphicsDeviceBroker.h"

#include "FrameSetAssembler.h"
#include "FrameSetCacheKey.h"
#include "SourceDecodeActor.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace dvs::media {
namespace {

inline constexpr std::size_t kExactRequestSlots = 1U;
inline constexpr std::size_t kSequentialRequestSlots = 2U;
inline constexpr std::size_t kPrefetchRequestSlots = 8U;
inline constexpr std::size_t kSetTableCapacity = 4U;
inline constexpr std::size_t kMaximumSourceCacheBytes = 12U * 1024U * 1024U;
inline constexpr std::size_t kMaximumFrameSetCacheBytes = 96U * 1024U * 1024U;

enum class ProviderOperationKind {
    kOpen,
    kFrame,
    kClose,
};

enum class ProviderOperationLifecycle {
    kPending,
    kCanceled,
    kTerminalClaimed,
};

using ProviderRequest = std::variant<application::FrameProviderOpenRequest,
                                     application::FrameRequest,
                                     application::FrameProviderCloseRequest>;

struct ProviderOperation final {
    ProviderOperationKind kind;
    ProviderRequest request;
    std::weak_ptr<application::IApplicationEventSink> events;
    std::atomic<ProviderOperationLifecycle> lifecycle = ProviderOperationLifecycle::kPending;
    std::atomic<application::CancellationReason> cancellationReason =
        application::CancellationReason::Superseded;
    // Shared ownership so speculative decode work can keep the flag alive after this operation's
    // last owner (the completion callback) is released.
    std::shared_ptr<std::atomic<bool>> cancellationRequested =
        std::make_shared<std::atomic<bool>>(false);

    ProviderOperation(const ProviderOperationKind kindValue,
                      ProviderRequest requestValue,
                      std::weak_ptr<application::IApplicationEventSink> eventsValue)
        : kind(kindValue), request(std::move(requestValue)), events(std::move(eventsValue)) {}

    [[nodiscard]] bool requestCancellation(const application::CancellationReason reason) noexcept {
        cancellationReason.store(reason, std::memory_order_release);
        ProviderOperationLifecycle expected = ProviderOperationLifecycle::kPending;
        if (!lifecycle.compare_exchange_strong(expected,
                                               ProviderOperationLifecycle::kCanceled,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
            return false;
        }
        cancellationRequested->store(true, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool isCanceled() const noexcept {
        return lifecycle.load(std::memory_order_acquire) == ProviderOperationLifecycle::kCanceled;
    }

    [[nodiscard]] bool claimTerminal() noexcept {
        ProviderOperationLifecycle expected = ProviderOperationLifecycle::kPending;
        return lifecycle.compare_exchange_strong(expected,
                                                 ProviderOperationLifecycle::kTerminalClaimed,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire);
    }

    [[nodiscard]] bool claimCanceledTerminal() noexcept {
        ProviderOperationLifecycle expected = ProviderOperationLifecycle::kCanceled;
        return lifecycle.compare_exchange_strong(expected,
                                                 ProviderOperationLifecycle::kTerminalClaimed,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire);
    }
};

[[nodiscard]] domain::MediaError providerError(const domain::MediaErrorCode code,
                                               std::optional<domain::SourceId> sourceId,
                                               std::string technicalDetail,
                                               const bool recoverable = false) {
    return domain::makeMediaError(code,
                                  domain::MediaOperation::kMediaDecode,
                                  sourceId,
                                  recoverable,
                                  std::move(technicalDetail));
}

[[nodiscard]] const application::PlaybackRequestContext&
playbackContext(const ProviderOperation& operation) {
    switch (operation.kind) {
    case ProviderOperationKind::kOpen:
        return std::get<application::FrameProviderOpenRequest>(operation.request).context;
    case ProviderOperationKind::kFrame:
        return std::get<application::FrameRequest>(operation.request).context.playback;
    case ProviderOperationKind::kClose:
        return std::get<application::FrameProviderCloseRequest>(operation.request).context;
    }
    std::terminate();
}

[[nodiscard]] bool sameSessionEpoch(const application::PlaybackRequestContext& left,
                                    const application::PlaybackRequestContext& right) noexcept {
    return left.request.sessionId == right.request.sessionId &&
           left.request.sessionEpoch == right.request.sessionEpoch;
}

[[nodiscard]] bool samePlaybackScope(const application::PlaybackRequestContext& left,
                                     const application::PlaybackRequestContext& right) noexcept {
    return sameSessionEpoch(left, right) && left.playbackGeneration == right.playbackGeneration;
}

[[nodiscard]] application::EventContext eventContext(const ProviderOperation& operation) {
    if (operation.kind == ProviderOperationKind::kFrame) {
        return application::EventContext{
            std::get<application::FrameRequest>(operation.request).context};
    }
    return application::EventContext{playbackContext(operation)};
}

[[nodiscard]] domain::RequestId requestId(const ProviderOperation& operation) noexcept {
    return playbackContext(operation).request.requestId;
}

void postCritical(const std::weak_ptr<application::IApplicationEventSink>& events,
                  application::ApplicationEvent event) noexcept {
    if (const std::shared_ptr<application::IApplicationEventSink> sink = events.lock()) {
        static_cast<void>(sink->postCritical(std::move(event)));
    }
}

void postCanceled(const std::shared_ptr<ProviderOperation>& operation) noexcept {
    if (!operation->claimCanceledTerminal()) {
        return;
    }
    postCritical(operation->events,
                 application::ApplicationEvent{application::RequestCanceled{
                     .context = eventContext(*operation),
                     .reason = operation->cancellationReason.load(std::memory_order_acquire),
                 }});
}

void postFailed(const std::shared_ptr<ProviderOperation>& operation,
                domain::MediaError error) noexcept {
    if (!operation->claimTerminal()) {
        postCanceled(operation);
        return;
    }
    error.requestId = requestId(*operation);
    postCritical(operation->events,
                 application::ApplicationEvent{application::RequestFailed{
                     .context = eventContext(*operation),
                     .error = std::move(error),
                 }});
}

void postSucceeded(const std::shared_ptr<ProviderOperation>& operation) noexcept {
    if (!operation->claimTerminal()) {
        postCanceled(operation);
        return;
    }
    postCritical(operation->events,
                 application::ApplicationEvent{application::RequestSucceeded{
                     .context = eventContext(*operation),
                 }});
}

void postFrameSetReady(const std::shared_ptr<ProviderOperation>& operation,
                       application::FrameSet set) noexcept {
    const application::FrameRequestContext context =
        std::get<application::FrameRequest>(operation->request).context;
    postCritical(operation->events,
                 application::ApplicationEvent{application::FrameSetReady{
                     .context = context,
                     .set = std::move(set),
                 }});
}

void postClaimedSucceeded(const std::shared_ptr<ProviderOperation>& operation) noexcept {
    const application::FrameRequestContext context =
        std::get<application::FrameRequest>(operation->request).context;
    postCritical(operation->events,
                 application::ApplicationEvent{application::RequestSucceeded{
                     .context = application::EventContext{context},
                 }});
}

void postFrameSetSucceeded(const std::shared_ptr<ProviderOperation>& operation,
                           application::FrameSet set) noexcept {
    if (!operation->claimTerminal()) {
        postCanceled(operation);
        return;
    }
    postFrameSetReady(operation, std::move(set));
    postClaimedSucceeded(operation);
}

[[nodiscard]] std::uint64_t frameDistance(const domain::FrameId left,
                                          const domain::FrameId right) noexcept {
    if (!left.isValid() || !right.isValid()) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left.value() >= right.value() ? static_cast<std::uint64_t>(left.value() - right.value())
                                         : static_cast<std::uint64_t>(right.value() - left.value());
}

[[nodiscard]] internal::SourceDecodePriority
decodePriority(const application::FrameRequestPriority priority) noexcept {
    switch (priority) {
    case application::FrameRequestPriority::Exact:
        return internal::SourceDecodePriority::Exact;
    case application::FrameRequestPriority::Sequential:
        return internal::SourceDecodePriority::Sequential;
    case application::FrameRequestPriority::Prefetch:
        return internal::SourceDecodePriority::Prefetch;
    }
    std::terminate();
}

struct SlotDecodeCompletion final {
    domain::SourceId sourceId;
    domain::FrameId sourceFrameId;
    application::FrameMatchKind matchKind;
    float alignmentConfidence;
    domain::Result<internal::DecodedFrame> result;
};

struct DecodeCompletionEvent final {
    std::shared_ptr<ProviderOperation> operation;
    SlotDecodeCompletion completion;
};

} // namespace

class MultiSourceFrameProvider::Impl final {
public:
    explicit Impl(platform::FrameBudget& frameBudget,
                  const std::size_t requestCapacity,
                  const bool lowPriority,
                  std::shared_ptr<platform::GraphicsDeviceBroker> deviceBroker)
        : frameBudget_(frameBudget), deviceBroker_(std::move(deviceBroker)),
          queueCapacity_(std::max(kSetTableCapacity, requestCapacity)), lowPriority_(lowPriority),
          worker_([this] { run(); }) {
        if (lowPriority_) {
            static_cast<void>(
                SetThreadPriority(worker_.native_handle(), THREAD_PRIORITY_BELOW_NORMAL));
        }
    }

    ~Impl() {
        shutdown();
    }

    [[nodiscard]] application::PortSubmitResult
    submit(const application::FrameProviderOpenRequest& request,
           const std::shared_ptr<application::IApplicationEventSink>& events) {
        if (!events) {
            return application::PortSubmitResult::Closed;
        }
        auto operation = std::make_shared<ProviderOperation>(
            ProviderOperationKind::kOpen,
            ProviderRequest{request},
            std::weak_ptr<application::IApplicationEventSink>{events});
        std::vector<std::shared_ptr<ProviderOperation>> displaced;
        bool interrupt = false;
        {
            std::scoped_lock lock(mutex_);
            if (closed_) {
                return application::PortSubmitResult::Closed;
            }
            cancelQueuedLocked(application::CancellationReason::Superseded, &displaced);
            interrupt = cancelActiveLocked(application::CancellationReason::Superseded);
            controlQueue_.push_back(std::move(operation));
        }
        if (interrupt) {
            interruptRequested_.store(true, std::memory_order_release);
        }
        postCanceledAll(displaced);
        condition_.notify_one();
        return application::PortSubmitResult::Accepted;
    }

    [[nodiscard]] application::PortSubmitResult
    submit(const application::FrameRequest& request,
           const std::shared_ptr<application::IApplicationEventSink>& events) {
        if (!events) {
            return application::PortSubmitResult::Closed;
        }
        auto operation = std::make_shared<ProviderOperation>(
            ProviderOperationKind::kFrame,
            ProviderRequest{request},
            std::weak_ptr<application::IApplicationEventSink>{events});
        std::vector<std::shared_ptr<ProviderOperation>> displaced;
        bool interrupt = false;
        bool busy = false;
        bool staleGeneration = false;
        {
            std::scoped_lock lock(mutex_);
            if (closed_) {
                return application::PortSubmitResult::Closed;
            }

            const application::PlaybackRequestContext& playback = request.context.playback;
            if (!latestFrameContext_.has_value() ||
                !sameSessionEpoch(*latestFrameContext_, playback)) {
                latestFrameContext_ = playback;
            } else if (playback.playbackGeneration.value() <
                       latestFrameContext_->playbackGeneration.value()) {
                staleGeneration = true;
                if (operation->requestCancellation(application::CancellationReason::Superseded)) {
                    displaced.push_back(operation);
                }
            } else if (playback.playbackGeneration != latestFrameContext_->playbackGeneration) {
                latestFrameContext_ = playback;
                generationDeltaCount_.fetch_add(1U, std::memory_order_release);
                cancelOutdatedFrameQueuesLocked(playback, &displaced);
                interrupt = cancelActiveOutdatedFrameLocked(playback);
            }

            if (staleGeneration) {
                // The operation has its one canceled terminal posted after the lock is released.
            } else {
                switch (request.priority) {
                case application::FrameRequestPriority::Exact:
                    latestExactFrame_ = request.frameId;
                    while (exactQueue_.size() >= kExactRequestSlots) {
                        cancelFrontLocked(
                            exactQueue_, application::CancellationReason::Superseded, &displaced);
                    }
                    interrupt = cancelActiveExactLocked() || interrupt;
                    exactQueue_.push_back(std::move(operation));
                    break;
                case application::FrameRequestPriority::Sequential:
                    sequentialRequestCount_.fetch_add(1U, std::memory_order_release);
                    while (sequentialQueue_.size() >= kSequentialRequestSlots) {
                        cancelFrontLocked(sequentialQueue_,
                                          application::CancellationReason::Superseded,
                                          &displaced);
                    }
                    sequentialQueue_.push_back(std::move(operation));
                    break;
                case application::FrameRequestPriority::Prefetch:
                    if (prefetchQueue_.size() >= kPrefetchRequestSlots) {
                        cancelFarthestPrefetchLocked(request.frameId, &displaced);
                    }
                    prefetchQueue_.push_back(std::move(operation));
                    break;
                }
            }

            if (!staleGeneration && queuedCountLocked() > queueCapacity_) {
                auto& queue = queueForPriority(request.priority);
                const std::shared_ptr<ProviderOperation> rejected = std::move(queue.back());
                queue.pop_back();
                if (rejected->requestCancellation(application::CancellationReason::Superseded)) {
                    displaced.push_back(rejected);
                }
                busy = true;
            }
        }
        if (staleGeneration) {
            postCanceledAll(displaced);
            return application::PortSubmitResult::Accepted;
        }
        if (busy) {
            if (interrupt) {
                interruptRequested_.store(true, std::memory_order_release);
            }
            postCanceledAll(displaced);
            return application::PortSubmitResult::Busy;
        }
        if (interrupt) {
            interruptRequested_.store(true, std::memory_order_release);
        }
        postCanceledAll(displaced);
        condition_.notify_one();
        return application::PortSubmitResult::Accepted;
    }

    [[nodiscard]] application::PortSubmitResult
    submit(const application::FrameProviderCloseRequest& request,
           const std::shared_ptr<application::IApplicationEventSink>& events) {
        if (!events) {
            return application::PortSubmitResult::Closed;
        }
        auto operation = std::make_shared<ProviderOperation>(
            ProviderOperationKind::kClose,
            ProviderRequest{request},
            std::weak_ptr<application::IApplicationEventSink>{events});
        std::vector<std::shared_ptr<ProviderOperation>> displaced;
        bool interrupt = false;
        {
            std::scoped_lock lock(mutex_);
            if (closed_) {
                return application::PortSubmitResult::Closed;
            }
            cancelQueuedLocked(application::CancellationReason::Superseded, &displaced);
            interrupt = cancelActiveLocked(application::CancellationReason::Superseded);
            controlQueue_.push_back(std::move(operation));
        }
        if (interrupt) {
            interruptRequested_.store(true, std::memory_order_release);
        }
        postCanceledAll(displaced);
        condition_.notify_one();
        return application::PortSubmitResult::Accepted;
    }

    void cancel(const application::PlaybackRequestContext& context) noexcept {
        std::vector<std::shared_ptr<ProviderOperation>> displaced;
        bool interrupt = false;
        {
            std::scoped_lock lock(mutex_);
            cancelMatchingQueueLocked(
                controlQueue_, context, application::CancellationReason::UserRequested, &displaced);
            cancelMatchingQueueLocked(
                exactQueue_, context, application::CancellationReason::UserRequested, &displaced);
            cancelMatchingQueueLocked(sequentialQueue_,
                                      context,
                                      application::CancellationReason::UserRequested,
                                      &displaced);
            cancelMatchingQueueLocked(prefetchQueue_,
                                      context,
                                      application::CancellationReason::UserRequested,
                                      &displaced);
            if (activeOperation_ != nullptr &&
                samePlaybackScope(playbackContext(*activeOperation_), context)) {
                interrupt = activeOperation_->requestCancellation(
                    application::CancellationReason::UserRequested);
                if (interrupt) {
                    // The active operation is canceled but not displaced into the queue
                    // collection; count it so the cancel total reflects every coordinator cancel.
                    cancelCount_.fetch_add(1U, std::memory_order_release);
                }
            }
        }
        cancelCount_.fetch_add(displaced.size(), std::memory_order_release);
        if (interrupt) {
            interruptRequested_.store(true, std::memory_order_release);
        }
        postCanceledAll(displaced);
        condition_.notify_one();
    }

    [[nodiscard]] std::vector<std::thread::id> decodeWorkerIdsForTesting() const {
        std::vector<std::thread::id> ids;
        ids.reserve(decodeActors_.size());
        for (const auto& actor : decodeActors_) {
            ids.push_back(actor->workerThreadId());
        }
        return ids;
    }

    [[nodiscard]] std::vector<std::uint64_t> decodeCountsForTesting() const {
        std::vector<std::uint64_t> counts;
        counts.reserve(decodeActors_.size());
        for (const auto& actor : decodeActors_) {
            counts.push_back(actor->completedDecodeCount());
        }
        return counts;
    }

    [[nodiscard]] std::uint64_t frameSetCacheHitCountForTesting() const noexcept {
        return frameSetCacheHitCount_.load(std::memory_order_acquire);
    }

    [[nodiscard]] FrameProviderStatistics statistics() const noexcept {
        return FrameProviderStatistics{
            .assembledFrameSets = assembledFrameSets_.load(std::memory_order_acquire),
            .totalAssemblyMicroseconds = totalAssemblyMicroseconds_.load(std::memory_order_acquire),
            .maximumAssemblyMicroseconds =
                maximumAssemblyMicroseconds_.load(std::memory_order_acquire),
            .frameSetCacheHits = frameSetCacheHitCount_.load(std::memory_order_acquire),
            .sequentialRequestCount = sequentialRequestCount_.load(std::memory_order_acquire),
            .cancelCount = cancelCount_.load(std::memory_order_acquire),
            .decoderReopenCount = decoderReopenCount_.load(std::memory_order_acquire),
            .generationDeltaCount = generationDeltaCount_.load(std::memory_order_acquire),
        };
    }

    [[nodiscard]] std::vector<DecoderBackendStatus> decoderBackendStatuses() const {
        std::vector<DecoderBackendStatus> statuses;
        statuses.reserve(decodeActors_.size());
        for (const auto& actor : decodeActors_) {
            statuses.push_back(actor->backendStatus());
        }
        return statuses;
    }

private:
    struct ActiveSession final {
        application::PlaybackRequestContext context;
        std::vector<domain::ComparisonSource> sources;
        domain::SourceId canonicalSourceId = 0;
        domain::CanonicalTimeline timeline;
    };

    struct SetTableEntry final {
        internal::FrameSetCacheKey key;
        application::FrameSet set;
        std::size_t accountedBytes = 0U;
        std::uint64_t insertionOrder;
    };

    struct PendingFrameAssembly final {
        std::shared_ptr<ProviderOperation> operation;
        internal::FrameSetAssembler assembler;
        std::chrono::steady_clock::time_point started;
        std::size_t pendingDecodeCount = 0U;
    };

    [[nodiscard]] std::size_t queuedCountLocked() const noexcept {
        return controlQueue_.size() + exactQueue_.size() + sequentialQueue_.size() +
               prefetchQueue_.size();
    }

    [[nodiscard]] std::deque<std::shared_ptr<ProviderOperation>>&
    queueForPriority(const application::FrameRequestPriority priority) noexcept {
        switch (priority) {
        case application::FrameRequestPriority::Exact:
            return exactQueue_;
        case application::FrameRequestPriority::Sequential:
            return sequentialQueue_;
        case application::FrameRequestPriority::Prefetch:
            return prefetchQueue_;
        }
        std::terminate();
    }

    void
    cancelQueueLocked(std::deque<std::shared_ptr<ProviderOperation>>& queue,
                      const application::CancellationReason reason,
                      std::vector<std::shared_ptr<ProviderOperation>>* const displaced) noexcept {
        while (!queue.empty()) {
            std::shared_ptr<ProviderOperation> operation = std::move(queue.front());
            queue.pop_front();
            if (operation->requestCancellation(reason)) {
                displaced->push_back(std::move(operation));
            }
        }
    }

    void
    cancelQueuedLocked(const application::CancellationReason reason,
                       std::vector<std::shared_ptr<ProviderOperation>>* const displaced) noexcept {
        cancelQueueLocked(controlQueue_, reason, displaced);
        cancelQueueLocked(exactQueue_, reason, displaced);
        cancelQueueLocked(sequentialQueue_, reason, displaced);
        cancelQueueLocked(prefetchQueue_, reason, displaced);
    }

    void
    cancelFrontLocked(std::deque<std::shared_ptr<ProviderOperation>>& queue,
                      const application::CancellationReason reason,
                      std::vector<std::shared_ptr<ProviderOperation>>* const displaced) noexcept {
        if (queue.empty()) {
            return;
        }
        std::shared_ptr<ProviderOperation> operation = std::move(queue.front());
        queue.pop_front();
        if (operation->requestCancellation(reason)) {
            displaced->push_back(std::move(operation));
        }
    }

    void cancelFarthestPrefetchLocked(
        const domain::FrameId incomingFrame,
        std::vector<std::shared_ptr<ProviderOperation>>* const displaced) noexcept {
        if (prefetchQueue_.empty()) {
            return;
        }
        const domain::FrameId anchor = latestExactFrame_.value_or(incomingFrame);
        const auto farthest = std::max_element(
            prefetchQueue_.begin(),
            prefetchQueue_.end(),
            [&anchor](const std::shared_ptr<ProviderOperation>& left,
                      const std::shared_ptr<ProviderOperation>& right) {
                const auto& leftRequest = std::get<application::FrameRequest>(left->request);
                const auto& rightRequest = std::get<application::FrameRequest>(right->request);
                return frameDistance(leftRequest.frameId, anchor) <
                       frameDistance(rightRequest.frameId, anchor);
            });
        std::shared_ptr<ProviderOperation> operation = std::move(*farthest);
        prefetchQueue_.erase(farthest);
        if (operation->requestCancellation(application::CancellationReason::Superseded)) {
            displaced->push_back(std::move(operation));
        }
    }

    [[nodiscard]] bool cancelActiveLocked(const application::CancellationReason reason) noexcept {
        return activeOperation_ != nullptr && activeOperation_->requestCancellation(reason);
    }

    [[nodiscard]] bool cancelActiveExactLocked() noexcept {
        if (activeOperation_ == nullptr ||
            activeOperation_->kind != ProviderOperationKind::kFrame) {
            return false;
        }
        const auto& request = std::get<application::FrameRequest>(activeOperation_->request);
        return request.priority == application::FrameRequestPriority::Exact &&
               activeOperation_->requestCancellation(application::CancellationReason::Superseded);
    }

    void cancelOutdatedFrameQueuesLocked(
        const application::PlaybackRequestContext& newestContext,
        std::vector<std::shared_ptr<ProviderOperation>>* const displaced) noexcept {
        cancelOutdatedFramesLocked(exactQueue_, newestContext, displaced);
        cancelOutdatedFramesLocked(sequentialQueue_, newestContext, displaced);
        cancelOutdatedFramesLocked(prefetchQueue_, newestContext, displaced);
    }

    void cancelOutdatedFramesLocked(
        std::deque<std::shared_ptr<ProviderOperation>>& queue,
        const application::PlaybackRequestContext& newestContext,
        std::vector<std::shared_ptr<ProviderOperation>>* const displaced) noexcept {
        auto iterator = queue.begin();
        while (iterator != queue.end()) {
            const auto& request = std::get<application::FrameRequest>((*iterator)->request);
            if (!sameSessionEpoch(request.context.playback, newestContext) ||
                request.context.playback.playbackGeneration == newestContext.playbackGeneration) {
                ++iterator;
                continue;
            }
            std::shared_ptr<ProviderOperation> canceled = std::move(*iterator);
            iterator = queue.erase(iterator);
            if (canceled->requestCancellation(application::CancellationReason::Superseded)) {
                displaced->push_back(std::move(canceled));
            }
        }
    }

    [[nodiscard]] bool cancelActiveOutdatedFrameLocked(
        const application::PlaybackRequestContext& newestContext) noexcept {
        if (activeOperation_ == nullptr ||
            activeOperation_->kind != ProviderOperationKind::kFrame) {
            return false;
        }
        const auto& request = std::get<application::FrameRequest>(activeOperation_->request);
        return sameSessionEpoch(request.context.playback, newestContext) &&
               request.context.playback.playbackGeneration != newestContext.playbackGeneration &&
               activeOperation_->requestCancellation(application::CancellationReason::Superseded);
    }

    void cancelMatchingQueueLocked(
        std::deque<std::shared_ptr<ProviderOperation>>& queue,
        const application::PlaybackRequestContext& context,
        const application::CancellationReason reason,
        std::vector<std::shared_ptr<ProviderOperation>>* const displaced) noexcept {
        auto iterator = queue.begin();
        while (iterator != queue.end()) {
            const std::shared_ptr<ProviderOperation>& operation = *iterator;
            if (!samePlaybackScope(playbackContext(*operation), context)) {
                ++iterator;
                continue;
            }
            std::shared_ptr<ProviderOperation> canceled = std::move(*iterator);
            iterator = queue.erase(iterator);
            if (canceled->requestCancellation(reason)) {
                displaced->push_back(std::move(canceled));
            }
        }
    }

    static void
    postCanceledAll(const std::vector<std::shared_ptr<ProviderOperation>>& operations) noexcept {
        for (const std::shared_ptr<ProviderOperation>& operation : operations) {
            postCanceled(operation);
        }
    }

    [[nodiscard]] std::shared_ptr<ProviderOperation> takeNextLocked() {
        if (!controlQueue_.empty()) {
            std::shared_ptr<ProviderOperation> operation = std::move(controlQueue_.front());
            controlQueue_.pop_front();
            return operation;
        }
        if (!exactQueue_.empty()) {
            std::shared_ptr<ProviderOperation> operation = std::move(exactQueue_.front());
            exactQueue_.pop_front();
            return operation;
        }
        if (!sequentialQueue_.empty()) {
            std::shared_ptr<ProviderOperation> operation = std::move(sequentialQueue_.front());
            sequentialQueue_.pop_front();
            return operation;
        }
        std::shared_ptr<ProviderOperation> operation = std::move(prefetchQueue_.front());
        prefetchQueue_.pop_front();
        return operation;
    }

    [[nodiscard]] bool hasPendingLocked() const noexcept {
        return !controlQueue_.empty() || !exactQueue_.empty() || !sequentialQueue_.empty() ||
               !prefetchQueue_.empty();
    }

    void closeDecodeActors() noexcept {
        for (auto& actor : decodeActors_) {
            if (actor != nullptr) {
                actor->close();
            }
        }
        decodeActors_.clear();
        activeSession_.reset();
        setTable_.clear();
        setTableRetainedBytes_ = 0U;
        sequentialSetFrame_.reset();
        sequentialSetReady_ = false;
    }

    void executeOpen(const std::shared_ptr<ProviderOperation>& operation) noexcept {
        if (operation->isCanceled()) {
            postCanceled(operation);
            return;
        }

        const auto& request = std::get<application::FrameProviderOpenRequest>(operation->request);

        if (request.sources.empty() || request.sources.size() > 3U) {
            postFailed(operation,
                       providerError(domain::MediaErrorCode::kInvalidArgument,
                                     std::nullopt,
                                     "The frame provider requires 1 to 3 review sources."));
            return;
        }

        const auto canonical = std::find_if(
            request.sources.begin(), request.sources.end(), [&request](const auto& source) {
                return source.id == request.canonicalSourceId;
            });
        if (canonical == request.sources.end()) {
            postFailed(operation,
                       providerError(domain::MediaErrorCode::kInvalidArgument,
                                     request.canonicalSourceId,
                                     "The canonical source is not present in the open request."));
            return;
        }
        const domain::MediaDescriptor& canonicalDescriptor = canonical->descriptor;
        if (domain::isVariableFrameRate(request.timeline)) {
            const auto& variableTimeline =
                std::get<std::shared_ptr<const domain::FrameTimeline>>(request.timeline);
            if (!variableTimeline) {
                postFailed(operation,
                           providerError(domain::MediaErrorCode::kFrameTimelineInvalid,
                                         std::nullopt,
                                         "A variable-frame-rate timeline must be non-null."));
                return;
            }
            if (canonicalDescriptor.frameRate.has_value()) {
                postFailed(operation,
                           providerError(domain::MediaErrorCode::kSourceFrameRateMismatch,
                                         std::nullopt,
                                         "A VFR timeline requires the canonical source to declare "
                                         "no rational frame rate."));
                return;
            }
            if (variableTimeline->frameCount() != canonicalDescriptor.frameCount.value) {
                postFailed(
                    operation,
                    providerError(domain::MediaErrorCode::kInvalidFrameCount,
                                  std::nullopt,
                                  "The VFR timeline frame count does not match the canonical "
                                  "source frame count."));
                return;
            }
        } else if (!canonicalDescriptor.frameRate.has_value() ||
                   std::get<domain::RationalRate>(request.timeline) !=
                       canonicalDescriptor.frameRate.value()) {
            postFailed(
                operation,
                providerError(domain::MediaErrorCode::kSourceFrameRateMismatch,
                              std::nullopt,
                              "The rational timeline must match the canonical source frame rate."));
            return;
        }

        // Reopening a decoder session that is already active (sources opened, then re-opened
        // without an intervening close) counts as a decoder reopen. The healthy held-forward
        // fixture never triggers this, so the gate expects zero. This hook cannot see
        // per-source decoder error-triggered reopens that SourceDecodeActor recovers internally;
        // those are intentionally not counted here.
        if (!decodeActors_.empty()) {
            decoderReopenCount_.fetch_add(1U, std::memory_order_release);
        }
        closeDecodeActors();
        interruptRequested_.store(false, std::memory_order_release);
        try {
            decodeActors_.reserve(request.sources.size());
            const std::size_t cacheCapacity =
                lowPriority_ || request.sources.empty()
                    ? 0U
                    : std::min(kMaximumSourceCacheBytes,
                               frameBudget_.capacityBytes() / (2U * request.sources.size()));
            for (std::size_t slot = 0; slot < request.sources.size(); ++slot) {
                decodeActors_.push_back(
                    std::make_unique<internal::SourceDecodeActor>(request.sources[slot].id,
                                                                  request.sources[slot].descriptor,
                                                                  frameBudget_,
                                                                  &interruptRequested_,
                                                                  lowPriority_,
                                                                  cacheCapacity,
                                                                  deviceBroker_));
            }

            for (std::size_t slot = 0; slot < decodeActors_.size(); ++slot) {
                const domain::Status opened =
                    decodeActors_[slot]->open(*operation->cancellationRequested);
                if (operation->isCanceled()) {
                    closeDecodeActors();
                    postCanceled(operation);
                    return;
                }
                if (!opened) {
                    closeDecodeActors();
                    postFailed(operation, opened.error());
                    return;
                }
            }

            activeSession_ = ActiveSession{
                .context = request.context,
                .sources = request.sources,
                .canonicalSourceId = request.canonicalSourceId,
                .timeline = request.timeline,
            };
            {
                std::scoped_lock lock(mutex_);
                if (!latestFrameContext_.has_value() ||
                    !sameSessionEpoch(*latestFrameContext_, request.context) ||
                    latestFrameContext_->playbackGeneration.value() <
                        request.context.playbackGeneration.value()) {
                    latestFrameContext_ = request.context;
                }
            }
            postSucceeded(operation);
        } catch (const std::exception& exception) {
            closeDecodeActors();
            postFailed(operation,
                       providerError(domain::MediaErrorCode::kMediaDecodeFailed,
                                     std::nullopt,
                                     "Unexpected multi-source provider open exception: " +
                                         std::string{exception.what()},
                                     true));
        } catch (...) {
            closeDecodeActors();
            postFailed(
                operation,
                providerError(domain::MediaErrorCode::kMediaDecodeFailed,
                              std::nullopt,
                              "Unexpected non-standard multi-source provider open exception.",
                              true));
        }
    }

    void cacheSet(const application::FrameRequest& request, const application::FrameSet& set) {
        // Continuous playback has one set in flight and never re-requests an already presented
        // sequential frame. Retaining those CPU resources would make memory grow with playback
        // history. Prefetch is source-cache-only and must never retain a complete FrameSet.
        if (request.priority != application::FrameRequestPriority::Exact) {
            return;
        }

        const internal::FrameSetCacheKey key{
            .sessionEpoch = request.context.playback.request.sessionEpoch,
            .alignmentRevision = request.alignmentRevision,
            .canonicalFrame = request.frameId,
        };
        std::size_t setBytes = 0U;
        for (const application::MappedSourceFrame& source : set.sources()) {
            if (!source.frame.has_value()) {
                continue;
            }
            const std::size_t frameBytes = source.frame->accountedBytes();
            if (frameBytes > (std::numeric_limits<std::size_t>::max)() - setBytes) {
                return;
            }
            setBytes += frameBytes;
        }
        // Keep most of the shared budget available for the published set, the scene graph's
        // retiring generation, and the next set being decoded. A cached 3-source 4K P010 set is
        // large enough to make that transient overlap fail even though steady-state usage fits.
        const std::size_t cacheCapacity =
            std::min(kMaximumFrameSetCacheBytes, frameBudget_.capacityBytes() / 4U);
        if (setBytes > cacheCapacity) {
            return;
        }

        const auto matchesRequest = [&key](const SetTableEntry& entry) { return entry.key == key; };
        for (auto iterator = setTable_.begin(); iterator != setTable_.end();) {
            if (!matchesRequest(*iterator)) {
                ++iterator;
                continue;
            }
            setTableRetainedBytes_ -= iterator->accountedBytes;
            iterator = setTable_.erase(iterator);
        }

        while (!setTable_.empty() && (setTable_.size() >= kSetTableCapacity ||
                                      setTableRetainedBytes_ > cacheCapacity - setBytes)) {
            const auto evictable =
                std::min_element(setTable_.begin(),
                                 setTable_.end(),
                                 [](const SetTableEntry& left, const SetTableEntry& right) {
                                     return left.insertionOrder < right.insertionOrder;
                                 });
            setTableRetainedBytes_ -= evictable->accountedBytes;
            setTable_.erase(evictable);
        }

        setTable_.push_back(SetTableEntry{
            .key = key,
            .set = set,
            .accountedBytes = setBytes,
            .insertionOrder = nextSetInsertionOrder_++,
        });
        setTableRetainedBytes_ += setBytes;
    }

    [[nodiscard]] std::optional<application::FrameSet>
    cachedSet(const application::FrameRequest& request) const {
        if (request.priority != application::FrameRequestPriority::Exact) {
            return std::nullopt;
        }
        const internal::FrameSetCacheKey key{
            .sessionEpoch = request.context.playback.request.sessionEpoch,
            .alignmentRevision = request.alignmentRevision,
            .canonicalFrame = request.frameId,
        };
        const auto cached = std::find_if(setTable_.begin(),
                                         setTable_.end(),
                                         [&key](const auto& entry) { return entry.key == key; });
        if (cached == setTable_.end()) {
            return std::nullopt;
        }
        return cached->set;
    }

    void executeFrame(const std::shared_ptr<ProviderOperation>& operation) noexcept {
        if (operation->isCanceled()) {
            postCanceled(operation);
            return;
        }
        const auto& request = std::get<application::FrameRequest>(operation->request);
        if (!activeSession_.has_value() || decodeActors_.empty()) {
            postFailed(operation,
                       providerError(domain::MediaErrorCode::kMediaDecodeFailed,
                                     std::nullopt,
                                     "No comparison sources are open for this frame request.",
                                     true));
            return;
        }
        std::optional<application::PlaybackRequestContext> latestContext;
        {
            std::scoped_lock lock(mutex_);
            latestContext = latestFrameContext_;
        }
        if (!latestContext.has_value() ||
            !samePlaybackScope(request.context.playback, *latestContext) ||
            !sameSessionEpoch(request.context.playback, activeSession_->context)) {
            static_cast<void>(
                operation->requestCancellation(application::CancellationReason::Superseded));
            postCanceled(operation);
            return;
        }
        if (!samePlaybackScope(request.context.playback, activeSession_->context)) {
            activeSession_->context = request.context.playback;
        }
        if (const std::optional<application::FrameSet> cached = cachedSet(request)) {
            frameSetCacheHitCount_.fetch_add(1U, std::memory_order_release);
            sequentialSetReady_ = false;
            postFrameSetSucceeded(operation, *cached);
            return;
        }
        const auto assemblyStarted = std::chrono::steady_clock::now();

        const auto canonicalTime =
            domain::canonicalFrameStartTime(activeSession_->timeline, request.frameId);
        if (!canonicalTime) {
            domain::MediaError error = canonicalTime.error();
            error.operation = domain::MediaOperation::kMediaDecode;
            error.source = std::nullopt;
            postFailed(operation, std::move(error));
            return;
        }

        interruptRequested_.store(false, std::memory_order_release);
        const bool continueSequentially =
            request.priority == application::FrameRequestPriority::Sequential &&
            sequentialSetReady_ && sequentialSetFrame_.has_value() &&
            request.frameId.value() > sequentialSetFrame_->value();
        sequentialSetReady_ = false;
        try {
            // USERPLAN 3.5: decode every source's frame in parallel (one decode actor per
            // source) and assemble the FrameSet only after all slots have reported, so the
            // per-set latency approaches max(slot decode time) instead of the sum over sources.
            // Each slot owns its decoder and publishes one identity-bearing completion into the
            // provider event queue. The provider worker returns to its event loop immediately.
            std::vector<domain::SourceId> sourceOrder;
            sourceOrder.reserve(activeSession_->sources.size());
            std::transform(activeSession_->sources.begin(),
                           activeSession_->sources.end(),
                           std::back_inserter(sourceOrder),
                           [](const domain::ComparisonSource& source) { return source.id; });
            internal::FrameSetAssembler assembler{
                request.frameId,
                canonicalTime.value(),
                std::move(sourceOrder),
            };
            const auto completeSlot = [&assembler,
                                       &operation](application::MappedSourceFrame entry) {
                if (assembler.complete(std::move(entry))) {
                    return true;
                }
                postFailed(operation,
                           providerError(domain::MediaErrorCode::kMediaDecodeFailed,
                                         std::nullopt,
                                         "A source worker reported an unknown or duplicate slot."));
                return false;
            };
            std::size_t pendingDecodeCount = 0U;

            const auto alignmentFor =
                [&request](
                    const domain::SourceId sourceId) -> const application::SourceFrameOffset* {
                const auto found = std::find_if(
                    request.sourceOffsets.begin(),
                    request.sourceOffsets.end(),
                    [sourceId](const auto& offset) { return offset.sourceId == sourceId; });
                return found == request.sourceOffsets.end() ? nullptr : &*found;
            };
            for (std::size_t index = 0U; index < request.sourceOffsets.size(); ++index) {
                const auto& offset = request.sourceOffsets[index];
                const bool known = std::any_of(
                    activeSession_->sources.begin(),
                    activeSession_->sources.end(),
                    [&offset](const auto& source) { return source.id == offset.sourceId; });
                const bool duplicate = std::any_of(
                    request.sourceOffsets.begin() + static_cast<std::ptrdiff_t>(index + 1U),
                    request.sourceOffsets.end(),
                    [&offset](const auto& other) { return other.sourceId == offset.sourceId; });
                const bool confidenceValid = offset.confidence >= 0.0F && offset.confidence <= 1.0F;
                const bool kindValid =
                    offset.matchKind == application::FrameMatchKind::GlobalOffset ||
                    offset.matchKind == application::FrameMatchKind::AutoAligned ||
                    offset.matchKind == application::FrameMatchKind::ManualAnchor ||
                    offset.matchKind == application::FrameMatchKind::Missing;
                if (!known || duplicate || !confidenceValid || !kindValid ||
                    (offset.matchKind == application::FrameMatchKind::Missing &&
                     offset.frames != 0)) {
                    postFailed(operation,
                               providerError(domain::MediaErrorCode::kInvalidArgument,
                                             offset.sourceId,
                                             "Frame mappings must be valid and name each open "
                                             "source at most once."));
                    return;
                }
            }

            for (std::size_t slot = 0; slot < decodeActors_.size(); ++slot) {
                const domain::ComparisonSource& source = activeSession_->sources[slot];
                const domain::SourceId sourceId = source.id;
                const std::int64_t sourceFrameCount = source.descriptor.frameCount.value;
                const std::int64_t canonicalFrame = request.frameId.value();
                const application::SourceFrameOffset* const alignment = alignmentFor(sourceId);
                const std::int64_t offset = alignment == nullptr ? 0 : alignment->frames;
                if (alignment != nullptr &&
                    alignment->matchKind == application::FrameMatchKind::Missing) {
                    if (!completeSlot(application::MappedSourceFrame{
                            .sourceId = sourceId,
                            .sourceFrameId = std::nullopt,
                            .frame = std::nullopt,
                            .presentationTime = domain::MediaTime{0},
                            .matchKind = application::FrameMatchKind::Missing,
                            .alignmentConfidence = alignment->confidence,
                            .missingReason = application::MissingReason::AlignmentGap,
                        })) {
                        return;
                    }
                    continue;
                }
                const bool underflow = offset < 0 && offset < -canonicalFrame;
                const bool overflow =
                    offset > 0 &&
                    canonicalFrame > (std::numeric_limits<std::int64_t>::max)() - offset;
                const std::int64_t mappedFrame =
                    underflow || overflow ? -1 : canonicalFrame + offset;

                if (!request.frameId.isValid() || mappedFrame < 0 ||
                    mappedFrame >= sourceFrameCount) {
                    if (!completeSlot(application::MappedSourceFrame{
                            .sourceId = sourceId,
                            .sourceFrameId = std::nullopt,
                            .frame = std::nullopt,
                            .presentationTime = domain::MediaTime{0},
                            .matchKind = application::FrameMatchKind::Missing,
                            .alignmentConfidence =
                                alignment == nullptr ? 1.0F : alignment->confidence,
                            .missingReason = mappedFrame < 0
                                                 ? application::MissingReason::BeforeSourceStart
                                                 : application::MissingReason::AfterSourceEnd,
                        })) {
                        return;
                    }
                    continue;
                }

                const bool sequential = continueSequentially;
                const domain::FrameId frameId{mappedFrame};
                const application::FrameMatchKind matchKind =
                    alignment == nullptr
                        ? application::FrameMatchKind::ExactIndex
                        : (offset == 0 &&
                                   alignment->matchKind == application::FrameMatchKind::GlobalOffset
                               ? application::FrameMatchKind::ExactIndex
                               : alignment->matchKind);
                const float alignmentConfidence =
                    alignment == nullptr ? 1.0F : alignment->confidence;
                const application::PortSubmitResult submitted = decodeActors_[slot]->submit(
                    internal::SourceDecodeRequest{
                        .frameId = frameId,
                        .priority = decodePriority(request.priority),
                        .continueSequentially = sequential,
                        .readAheadCount =
                            request.priority == application::FrameRequestPriority::Sequential
                                ? std::uint8_t{3U}
                                : std::uint8_t{0U},
                        .cancellationRequested = operation->cancellationRequested,
                        .context = request.context,
                    },
                    [this, operation, sourceId, frameId, matchKind, alignmentConfidence](
                        domain::Result<internal::DecodedFrame> decoded) mutable {
                        publishDecodeCompletion(operation,
                                                SlotDecodeCompletion{
                                                    .sourceId = sourceId,
                                                    .sourceFrameId = frameId,
                                                    .matchKind = matchKind,
                                                    .alignmentConfidence = alignmentConfidence,
                                                    .result = std::move(decoded),
                                                });
                    });
                if (submitted != application::PortSubmitResult::Accepted) {
                    postFailed(operation,
                               providerError(domain::MediaErrorCode::kMediaDecodeFailed,
                                             sourceId,
                                             "A source decode actor mailbox rejected the frame "
                                             "request.",
                                             true));
                    return;
                }
                ++pendingDecodeCount;
            }

            pendingFrameAssembly_.emplace(PendingFrameAssembly{
                .operation = operation,
                .assembler = std::move(assembler),
                .started = assemblyStarted,
                .pendingDecodeCount = pendingDecodeCount,
            });
            if (pendingDecodeCount == 0U) {
                finishFrameAssembly();
            }
        } catch (const std::exception& exception) {
            postFailed(operation,
                       providerError(domain::MediaErrorCode::kMediaDecodeFailed,
                                     std::nullopt,
                                     "Unexpected multi-source frame decode exception: " +
                                         std::string{exception.what()},
                                     true));
        } catch (...) {
            postFailed(operation,
                       providerError(domain::MediaErrorCode::kMediaDecodeFailed,
                                     std::nullopt,
                                     "Unexpected non-standard multi-source frame decode exception.",
                                     true));
        }
    }

    void publishDecodeCompletion(const std::shared_ptr<ProviderOperation>& operation,
                                 SlotDecodeCompletion completion) noexcept {
        {
            std::scoped_lock lock(mutex_);
            try {
                decodeCompletions_.push_back(
                    DecodeCompletionEvent{operation, std::move(completion)});
            } catch (...) {
                completionPublishFailure_ = operation;
            }
        }
        condition_.notify_one();
    }

    void finishFrameAssembly() noexcept {
        if (!pendingFrameAssembly_.has_value()) {
            return;
        }
        PendingFrameAssembly& assembly = *pendingFrameAssembly_;
        const std::shared_ptr<ProviderOperation> operation = assembly.operation;
        const auto& request = std::get<application::FrameRequest>(operation->request);
        if (operation->isCanceled()) {
            pendingFrameAssembly_.reset();
            postCanceled(operation);
            return;
        }
        if (request.priority == application::FrameRequestPriority::Prefetch) {
            sequentialSetReady_ = false;
            pendingFrameAssembly_.reset();
            postSucceeded(operation);
            return;
        }

        std::optional<application::FrameSet> set = assembly.assembler.finish();
        if (!set) {
            pendingFrameAssembly_.reset();
            postFailed(
                operation,
                providerError(domain::MediaErrorCode::kMediaDecodeFailed,
                              std::nullopt,
                              "The decoded source frames could not form a valid frame set."));
            return;
        }
        const auto assemblyMicroseconds =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - assembly.started)
                                           .count());
        assembledFrameSets_.fetch_add(1U, std::memory_order_release);
        totalAssemblyMicroseconds_.fetch_add(assemblyMicroseconds, std::memory_order_release);
        std::uint64_t maximum = maximumAssemblyMicroseconds_.load(std::memory_order_acquire);
        while (maximum < assemblyMicroseconds &&
               !maximumAssemblyMicroseconds_.compare_exchange_weak(maximum,
                                                                   assemblyMicroseconds,
                                                                   std::memory_order_release,
                                                                   std::memory_order_acquire)) {
        }
        if (operation->isCanceled()) {
            pendingFrameAssembly_.reset();
            set.reset();
            postCanceled(operation);
            return;
        }

        sequentialSetFrame_ = request.frameId;
        sequentialSetReady_ = true;
        if (!operation->claimTerminal()) {
            pendingFrameAssembly_.reset();
            set.reset();
            postCanceled(operation);
            return;
        }
        cacheSet(request, *set);
        postFrameSetReady(operation, std::move(*set));
        pendingFrameAssembly_.reset();
        set.reset();
        postClaimedSucceeded(operation);
    }

    void processDecodeCompletion(DecodeCompletionEvent event) noexcept {
        if (!pendingFrameAssembly_.has_value() ||
            pendingFrameAssembly_->operation != event.operation) {
            return;
        }
        const std::shared_ptr<ProviderOperation> operation = event.operation;
        if (operation->isCanceled()) {
            pendingFrameAssembly_.reset();
            postCanceled(operation);
            return;
        }
        if (!event.completion.result) {
            // Missing is exclusively a mapping-layer result. Decoder failures leave the
            // previously presented FrameSet untouched.
            pendingFrameAssembly_.reset();
            postFailed(operation, event.completion.result.error());
            return;
        }
        if (!pendingFrameAssembly_->assembler.complete(application::MappedSourceFrame{
                .sourceId = event.completion.sourceId,
                .sourceFrameId = event.completion.sourceFrameId,
                .frame = std::move(event.completion.result.value().handle),
                .presentationTime = event.completion.result.value().presentationTime,
                .matchKind = event.completion.matchKind,
                .alignmentConfidence = event.completion.alignmentConfidence,
            })) {
            pendingFrameAssembly_.reset();
            postFailed(operation,
                       providerError(domain::MediaErrorCode::kMediaDecodeFailed,
                                     std::nullopt,
                                     "A source worker reported an unknown or duplicate slot."));
            return;
        }
        if (pendingFrameAssembly_->pendingDecodeCount == 0U) {
            pendingFrameAssembly_.reset();
            postFailed(operation,
                       providerError(domain::MediaErrorCode::kMediaDecodeFailed,
                                     std::nullopt,
                                     "A source worker reported too many completions."));
            return;
        }
        --pendingFrameAssembly_->pendingDecodeCount;
        if (pendingFrameAssembly_->pendingDecodeCount == 0U) {
            finishFrameAssembly();
        }
    }

    void executeClose(const std::shared_ptr<ProviderOperation>& operation) noexcept {
        if (operation->isCanceled()) {
            postCanceled(operation);
            return;
        }
        closeDecodeActors();
        {
            std::scoped_lock lock(mutex_);
            if (latestFrameContext_.has_value() &&
                sameSessionEpoch(
                    *latestFrameContext_,
                    std::get<application::FrameProviderCloseRequest>(operation->request).context)) {
                latestFrameContext_.reset();
            }
        }
        postSucceeded(operation);
    }

    void execute(const std::shared_ptr<ProviderOperation>& operation) noexcept {
        switch (operation->kind) {
        case ProviderOperationKind::kOpen:
            executeOpen(operation);
            return;
        case ProviderOperationKind::kFrame:
            executeFrame(operation);
            return;
        case ProviderOperationKind::kClose:
            executeClose(operation);
            return;
        }
        std::terminate();
    }

    void run() noexcept {
        for (;;) {
            std::shared_ptr<ProviderOperation> operation;
            std::optional<DecodeCompletionEvent> completion;
            std::shared_ptr<ProviderOperation> completionFailure;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] {
                    return !decodeCompletions_.empty() || completionPublishFailure_ != nullptr ||
                           (activeOperation_ == nullptr && hasPendingLocked()) ||
                           (closed_ && activeOperation_ == nullptr);
                });
                if (!decodeCompletions_.empty()) {
                    completion.emplace(std::move(decodeCompletions_.front()));
                    decodeCompletions_.pop_front();
                } else if (completionPublishFailure_ != nullptr) {
                    completionFailure = std::move(completionPublishFailure_);
                } else if (activeOperation_ == nullptr && hasPendingLocked()) {
                    operation = takeNextLocked();
                    activeOperation_ = operation;
                } else if (closed_) {
                    return;
                }
            }

            if (completion.has_value()) {
                operation = completion->operation;
                processDecodeCompletion(std::move(*completion));
            } else if (completionFailure != nullptr) {
                if (pendingFrameAssembly_.has_value() &&
                    pendingFrameAssembly_->operation == completionFailure) {
                    pendingFrameAssembly_.reset();
                    postFailed(completionFailure,
                               providerError(domain::MediaErrorCode::kMediaDecodeFailed,
                                             std::nullopt,
                                             "The provider could not retain a decode completion.",
                                             true));
                }
                operation = std::move(completionFailure);
            } else if (operation != nullptr) {
                execute(operation);
            }

            {
                std::scoped_lock lock(mutex_);
                const bool stillPending = pendingFrameAssembly_.has_value() &&
                                          pendingFrameAssembly_->operation == activeOperation_;
                if (!stillPending && activeOperation_ == operation) {
                    activeOperation_.reset();
                }
            }
            condition_.notify_one();
        }
    }

    void shutdown() noexcept {
        std::vector<std::shared_ptr<ProviderOperation>> displaced;
        bool interrupt = false;
        {
            std::scoped_lock lock(mutex_);
            if (closed_) {
                return;
            }
            closed_ = true;
            cancelQueuedLocked(application::CancellationReason::Shutdown, &displaced);
            interrupt = cancelActiveLocked(application::CancellationReason::Shutdown);
        }
        if (interrupt) {
            interruptRequested_.store(true, std::memory_order_release);
        }
        postCanceledAll(displaced);
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        closeDecodeActors();
    }

    platform::FrameBudget& frameBudget_;
    std::shared_ptr<platform::GraphicsDeviceBroker> deviceBroker_;
    std::size_t queueCapacity_;
    bool lowPriority_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::shared_ptr<ProviderOperation>> controlQueue_;
    std::deque<std::shared_ptr<ProviderOperation>> exactQueue_;
    std::deque<std::shared_ptr<ProviderOperation>> sequentialQueue_;
    std::deque<std::shared_ptr<ProviderOperation>> prefetchQueue_;
    std::deque<DecodeCompletionEvent> decodeCompletions_;
    std::shared_ptr<ProviderOperation> completionPublishFailure_;
    std::shared_ptr<ProviderOperation> activeOperation_;
    std::optional<PendingFrameAssembly> pendingFrameAssembly_;
    std::optional<domain::FrameId> latestExactFrame_;
    std::optional<application::PlaybackRequestContext> latestFrameContext_;
    std::optional<domain::FrameId> sequentialSetFrame_;
    bool sequentialSetReady_ = false;
    bool closed_ = false;
    std::atomic<bool> interruptRequested_ = false;
    std::thread worker_;

    std::vector<std::unique_ptr<internal::SourceDecodeActor>> decodeActors_;
    std::optional<ActiveSession> activeSession_;
    std::deque<SetTableEntry> setTable_;
    std::size_t setTableRetainedBytes_ = 0U;
    std::uint64_t nextSetInsertionOrder_ = 0;
    std::atomic<std::uint64_t> frameSetCacheHitCount_ = 0U;
    std::atomic<std::uint64_t> assembledFrameSets_ = 0U;
    std::atomic<std::uint64_t> totalAssemblyMicroseconds_ = 0U;
    std::atomic<std::uint64_t> maximumAssemblyMicroseconds_ = 0U;
    std::atomic<std::uint64_t> sequentialRequestCount_ = 0U;
    std::atomic<std::uint64_t> cancelCount_ = 0U;
    std::atomic<std::uint64_t> decoderReopenCount_ = 0U;
    std::atomic<std::uint64_t> generationDeltaCount_ = 0U;
};

MultiSourceFrameProvider::MultiSourceFrameProvider(
    platform::FrameBudget& frameBudget,
    const std::size_t requestCapacity,
    const bool lowPriority,
    std::shared_ptr<platform::GraphicsDeviceBroker> deviceBroker)
    : impl_(std::make_unique<Impl>(
          frameBudget, requestCapacity, lowPriority, std::move(deviceBroker))) {}

MultiSourceFrameProvider::~MultiSourceFrameProvider() = default;

application::PortSubmitResult
MultiSourceFrameProvider::submit(const application::FrameProviderOpenRequest& request,
                                 std::shared_ptr<application::IApplicationEventSink> events) {
    return impl_->submit(request, events);
}

application::PortSubmitResult
MultiSourceFrameProvider::submit(const application::FrameRequest& request,
                                 std::shared_ptr<application::IApplicationEventSink> events) {
    return impl_->submit(request, events);
}

application::PortSubmitResult
MultiSourceFrameProvider::submit(const application::FrameProviderCloseRequest& request,
                                 std::shared_ptr<application::IApplicationEventSink> events) {
    return impl_->submit(request, events);
}

void MultiSourceFrameProvider::cancel(const application::PlaybackRequestContext& context) noexcept {
    impl_->cancel(context);
}

std::vector<std::thread::id> MultiSourceFrameProvider::decodeWorkerIdsForTesting() const {
    return impl_->decodeWorkerIdsForTesting();
}

std::vector<std::uint64_t> MultiSourceFrameProvider::decodeCountsForTesting() const {
    return impl_->decodeCountsForTesting();
}

std::uint64_t MultiSourceFrameProvider::frameSetCacheHitCountForTesting() const noexcept {
    return impl_->frameSetCacheHitCountForTesting();
}

std::vector<DecoderBackendStatus> MultiSourceFrameProvider::decoderBackendStatuses() const {
    return impl_->decoderBackendStatuses();
}

FrameProviderStatistics MultiSourceFrameProvider::statistics() const noexcept {
    return impl_->statistics();
}

} // namespace dvs::media
