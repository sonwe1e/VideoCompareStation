#include "SourceDecodeActor.h"

#include "dvs/application/PlaybackTrace.h"
#include "dvs/domain/MediaError.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace dvs::media::internal {
namespace {

constexpr std::size_t kExactCapacity = 1U;
constexpr std::size_t kSequentialCapacity = 2U;
constexpr std::size_t kPrefetchCapacity = 8U;
constexpr std::uint8_t kMaximumReadAheadCount = 4U;
// ADR-003 Reverse GOP Window: request-side desire is capped so a single held-backward burst
// cannot ask the actor to walk an unbounded GOP; the actor still shrinks to the byte budget.
constexpr std::uint8_t kMaximumReverseWindowFrames = 48U;
// Soft wall-clock budget for one reverse-window seed + sequential walk. The primary reverse
// frame is already complete; this bound keeps held-backward from stalling the decode worker
// on a long GOP when the next interactive target is already queued.
constexpr std::uint64_t kReverseWindowBuildBudgetMicroseconds = 80'000U;
// D04: the budget covers the complete fill including the mandatory seed decodeExact. A seed
// that already exceeded it skips the sequential walk so the wait is never "seed + full budget".
constexpr std::uint32_t kExactSoftwareThreadCount = 4U;
constexpr std::uint64_t kMaximumSoftwareExactFrameBytes = 8U * 1024U * 1024U;

[[nodiscard]] bool
supportsDedicatedExactDecode(const domain::MediaDescriptor& descriptor) noexcept {
    if (descriptor.extent.width == 0U ||
        descriptor.extent.height >
            kMaximumSoftwareExactFrameBytes / static_cast<std::uint64_t>(descriptor.extent.width)) {
        return false;
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(descriptor.extent.width) *
                                 static_cast<std::uint64_t>(descriptor.extent.height);
    if (pixels > kMaximumSoftwareExactFrameBytes) {
        return false;
    }
    const std::uint64_t bytes = descriptor.bitDepth == 10U ? pixels * 3U : pixels + (pixels / 2U);
    return bytes <= kMaximumSoftwareExactFrameBytes;
}

[[nodiscard]] bool prefersHardwareExactDecode(const domain::MediaDescriptor& descriptor) noexcept {
    return descriptor.frameRate.has_value() && descriptor.frameRate->displayFps() >= 50.0;
}

[[nodiscard]] domain::MediaError
actorError(const domain::SourceId sourceId, std::string detail, const bool recoverable = true) {
    return domain::makeMediaError(domain::MediaErrorCode::kMediaDecodeFailed,
                                  domain::MediaOperation::kMediaDecode,
                                  sourceId,
                                  recoverable,
                                  std::move(detail));
}

// Decode-stage observation (trace kinds 23/24). Payload is the source frame id being decoded;
// the identity `req` carries the source id so a stalled actor is attributable, while
// session/epoch/generation come from the request context when present. A Started without its
// Completed is the signature of a decoder call that never returned.
void emitDecodeStageTrace(const application::TraceEventKind kind,
                          const SourceDecodeRequest& request,
                          const domain::SourceId sourceId,
                          const domain::FrameId frameId) noexcept {
    application::TraceIdentity identity{
        .request = domain::RequestId{static_cast<std::uint64_t>(sourceId)},
    };
    if (request.context.has_value()) {
        identity.session = request.context->playback.request.sessionId;
        identity.epoch = request.context->playback.request.sessionEpoch;
        identity.generation = request.context->playback.playbackGeneration;
    }
    application::PlaybackTrace::instance().record(
        kind, identity, static_cast<std::uint64_t>(frameId.value()));
}

[[nodiscard]] std::size_t capacityFor(const SourceDecodePriority priority) noexcept {
    switch (priority) {
    case SourceDecodePriority::Exact:
    case SourceDecodePriority::Reverse:
        return kExactCapacity;
    case SourceDecodePriority::Sequential:
        return kSequentialCapacity;
    case SourceDecodePriority::Prefetch:
        return kPrefetchCapacity;
    }
    std::terminate();
}

[[nodiscard]] bool olderPlaybackContext(const application::FrameRequestContext& candidate,
                                        const application::FrameRequestContext& newest) noexcept {
    const auto& candidatePlayback = candidate.playback;
    const auto& newestPlayback = newest.playback;
    return candidatePlayback.request.sessionId == newestPlayback.request.sessionId &&
           candidatePlayback.request.sessionEpoch == newestPlayback.request.sessionEpoch &&
           candidatePlayback.playbackGeneration.value() < newestPlayback.playbackGeneration.value();
}

} // namespace

SourceDecodeActor::SourceDecodeActor(const domain::SourceId sourceId,
                                     domain::MediaDescriptor descriptor,
                                     platform::FrameBudget& frameBudget,
                                     const std::atomic<bool>* const externalInterrupt,
                                     const bool lowPriority,
                                     const std::size_t cacheCapacityBytes,
                                     std::shared_ptr<platform::GraphicsDeviceBroker> deviceBroker)
    : sourceId_(sourceId), sourceFrameCount_(descriptor.frameCount.value),
      decoder_(std::make_unique<SoftwareDecoder>(
          sourceId, descriptor, frameBudget, externalInterrupt, deviceBroker)),
      exactDecoder_(supportsDedicatedExactDecode(descriptor)
                        ? std::make_unique<SoftwareDecoder>(sourceId,
                                                            descriptor,
                                                            frameBudget,
                                                            externalInterrupt,
                                                            prefersHardwareExactDecode(descriptor)
                                                                ? std::move(deviceBroker)
                                                                : nullptr,
                                                            kExactSoftwareThreadCount)
                        : nullptr),
      worker_([this] { run(); }), backendStatus_{.sourceId = sourceId}, cache_(cacheCapacityBytes),
      cacheKey_{
          .sourceFingerprint = descriptor.sourceIdentity.has_value()
                                   ? descriptor.sourceIdentity->fingerprintSha256
                                   : descriptor.normalizedPath.generic_string(),
          .sourceFrame = domain::FrameId{0},
          .profile =
              NormalizationProfile{
                  .format = descriptor.bitDepth == 10U ? application::NormalizedFrameFormat::P010_10
                                                       : application::NormalizedFrameFormat::Nv12_8,
                  .width = descriptor.extent.width,
                  .height = descriptor.extent.height,
              },
      } {
    if (lowPriority) {
        static_cast<void>(SetThreadPriority(worker_.native_handle(), THREAD_PRIORITY_BELOW_NORMAL));
    }
    std::unique_lock lock{mutex_};
    condition_.wait(lock, [this] { return started_; });
}

SourceDecodeActor::~SourceDecodeActor() {
    shutdown();
}

domain::Status SourceDecodeActor::open(const std::atomic<bool>& cancellationRequested) {
    ControlJob job{
        .kind = ControlKind::Open,
        .cancellationRequested = &cancellationRequested,
    };
    std::future<domain::Status> completion = job.completion.get_future();
    {
        std::scoped_lock lock{mutex_};
        if (stopping_) {
            return domain::Status::failure(
                actorError(sourceId_, "The source decode actor is closed."));
        }
        controlQueue_.push_back(std::move(job));
    }
    condition_.notify_one();
    return completion.get();
}

SourceDecodeSubmission SourceDecodeActor::submit(SourceDecodeRequest request) {
    auto promise = std::make_shared<std::promise<domain::Result<DecodedFrame>>>();
    std::future<domain::Result<DecodedFrame>> completion = promise->get_future();
    const application::PortSubmitResult status =
        submit(std::move(request), [promise](domain::Result<DecodedFrame> result) {
            promise->set_value(std::move(result));
        });
    return SourceDecodeSubmission{
        .status = status,
        .completion = std::move(completion),
    };
}

application::PortSubmitResult SourceDecodeActor::submit(SourceDecodeRequest request,
                                                        SourceDecodeCompletion completion) {
    if (request.cancellationRequested == nullptr || !request.frameId.isValid() ||
        request.readAheadCount > kMaximumReadAheadCount ||
        request.reverseWindowFrames > kMaximumReverseWindowFrames || !completion) {
        return application::PortSubmitResult::Closed;
    }

    DecodeJob job{
        .request = std::move(request),
        .completion = std::move(completion),
    };
    std::vector<DecodeJob> displaced;
    {
        std::scoped_lock lock{mutex_};
        if (stopping_) {
            return application::PortSubmitResult::Closed;
        }

        if (job.request.context.has_value()) {
            if (latestContext_.has_value() &&
                olderPlaybackContext(*job.request.context, *latestContext_)) {
                return application::PortSubmitResult::Closed;
            }
            if (!latestContext_.has_value() ||
                olderPlaybackContext(*latestContext_, *job.request.context)) {
                latestContext_ = job.request.context;
                const auto discardOlder = [this, &displaced](std::deque<DecodeJob>& queue) {
                    auto iterator = queue.begin();
                    while (iterator != queue.end()) {
                        if (iterator->request.context.has_value() &&
                            olderPlaybackContext(*iterator->request.context, *latestContext_)) {
                            displaced.push_back(std::move(*iterator));
                            iterator = queue.erase(iterator);
                        } else {
                            ++iterator;
                        }
                    }
                };
                discardOlder(exactQueue_);
                discardOlder(sequentialQueue_);
                discardOlder(prefetchQueue_);
            }
        }

        if (job.request.priority == SourceDecodePriority::Exact ||
            job.request.priority == SourceDecodePriority::Reverse) {
            while (!prefetchQueue_.empty()) {
                displaced.push_back(std::move(prefetchQueue_.front()));
                prefetchQueue_.pop_front();
            }
        }

        std::deque<DecodeJob>& queue = queueFor(job.request.priority);
        while (queue.size() >= capacityFor(job.request.priority)) {
            displaced.push_back(std::move(queue.front()));
            queue.pop_front();
        }
        queue.push_back(std::move(job));
    }
    for (DecodeJob& canceled : displaced) {
        completeCanceled(std::move(canceled));
    }
    condition_.notify_one();
    return application::PortSubmitResult::Accepted;
}

void SourceDecodeActor::close() noexcept {
    ControlJob job{.kind = ControlKind::Close};
    std::future<domain::Status> completion = job.completion.get_future();
    {
        std::scoped_lock lock{mutex_};
        if (stopping_) {
            return;
        }
        cancelQueuedLocked();
        controlQueue_.push_back(std::move(job));
    }
    requestInterrupt();
    condition_.notify_one();
    static_cast<void>(completion.get());
}

void SourceDecodeActor::requestInterrupt() noexcept {
    decoder_->requestInterrupt();
    if (exactDecoder_ != nullptr) {
        exactDecoder_->requestInterrupt();
    }
}

void SourceDecodeActor::shutdown() noexcept {
    {
        std::scoped_lock lock{mutex_};
        if (stopping_) {
            return;
        }
        stopping_ = true;
        cancelQueuedLocked();
    }
    requestInterrupt();
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

std::thread::id SourceDecodeActor::workerThreadId() const noexcept {
    std::scoped_lock lock{mutex_};
    return workerThreadId_;
}

std::thread::id SourceDecodeActor::lastDecodeThreadId() const noexcept {
    std::scoped_lock lock{mutex_};
    return lastDecodeThreadId_;
}

std::uint64_t SourceDecodeActor::completedDecodeCount() const noexcept {
    std::scoped_lock lock{mutex_};
    return completedDecodeCount_;
}

media::DecoderBackendStatus SourceDecodeActor::backendStatus() const {
    std::scoped_lock lock{mutex_};
    return backendStatus_;
}

std::deque<SourceDecodeActor::DecodeJob>&
SourceDecodeActor::queueFor(const SourceDecodePriority priority) noexcept {
    switch (priority) {
    case SourceDecodePriority::Exact:
    case SourceDecodePriority::Reverse:
        return exactQueue_;
    case SourceDecodePriority::Sequential:
        return sequentialQueue_;
    case SourceDecodePriority::Prefetch:
        return prefetchQueue_;
    }
    std::terminate();
}

bool SourceDecodeActor::hasPendingLocked() const noexcept {
    return !controlQueue_.empty() || !exactQueue_.empty() || !sequentialQueue_.empty() ||
           !prefetchQueue_.empty();
}

std::optional<SourceDecodeActor::DecodeJob> SourceDecodeActor::takeNextDecodeLocked() {
    const auto take = [](std::deque<DecodeJob>& queue) -> std::optional<DecodeJob> {
        if (queue.empty()) {
            return std::nullopt;
        }
        DecodeJob job = std::move(queue.front());
        queue.pop_front();
        return job;
    };
    if (std::optional<DecodeJob> job = take(exactQueue_)) {
        return job;
    }
    if (std::optional<DecodeJob> job = take(sequentialQueue_)) {
        return job;
    }
    return take(prefetchQueue_);
}

void SourceDecodeActor::run() noexcept {
    {
        std::scoped_lock lock{mutex_};
        workerThreadId_ = std::this_thread::get_id();
        started_ = true;
    }
    condition_.notify_all();

    for (;;) {
        std::optional<ControlJob> control;
        std::optional<DecodeJob> decode;
        {
            std::unique_lock lock{mutex_};
            condition_.wait(lock, [this] { return stopping_ || hasPendingLocked(); });
            if (stopping_) {
                break;
            }
            if (!controlQueue_.empty()) {
                control = std::move(controlQueue_.front());
                controlQueue_.pop_front();
            } else {
                decode = takeNextDecodeLocked();
            }
        }

        if (control.has_value()) {
            if (control->kind == ControlKind::Open && control->cancellationRequested != nullptr) {
                domain::Status status = decoder_->open(*control->cancellationRequested);
                if (status && exactDecoder_ != nullptr) {
                    status = exactDecoder_->open(*control->cancellationRequested);
                }
                decoderNeedsReopen_ = false;
                exactDecoderNeedsReopen_ = false;
                {
                    std::scoped_lock lock{mutex_};
                    completedDecodeCount_ = 0U;
                    cacheHitCount_ = 0U;
                    totalDecodeMicroseconds_ = 0U;
                    maximumDecodeMicroseconds_ = 0U;
                    reverseWindowHitCount_ = 0U;
                    reverseWindowBuildCount_ = 0U;
                    reverseWindowBuiltFrameCount_ = 0U;
                    reverseWindowBuildMicroseconds_ = 0U;
                    reverseWindowBuildMaximumMicroseconds_ = 0U;
                    reverseExactFallbackCount_ = 0U;
                    backendStatus_ = media::DecoderBackendStatus{
                        .sourceId = sourceId_,
                        .backend = decoder_->backend(),
                        .fallbackReason = decoder_->fallbackReason(),
                        .deviceGeneration = decoder_->deviceGeneration(),
                    };
                }
                control->completion.set_value(std::move(status));
            } else {
                decoder_->close();
                if (exactDecoder_ != nullptr) {
                    exactDecoder_->close();
                }
                decoderNeedsReopen_ = false;
                exactDecoderNeedsReopen_ = false;
                cache_.clear();
                {
                    std::scoped_lock lock{mutex_};
                    backendStatus_ = media::DecoderBackendStatus{.sourceId = sourceId_};
                }
                control->completion.set_value(domain::Status::success());
            }
            continue;
        }
        if (!decode.has_value()) {
            continue;
        }

        const bool retainInCache = decode->request.priority == SourceDecodePriority::Exact ||
                                   decode->request.priority == SourceDecodePriority::Reverse ||
                                   decode->request.priority == SourceDecodePriority::Prefetch;
        const auto recordDecode = [this](const std::uint64_t decodeMicroseconds) {
            std::scoped_lock lock{mutex_};
            lastDecodeThreadId_ = std::this_thread::get_id();
            ++completedDecodeCount_;
            totalDecodeMicroseconds_ += decodeMicroseconds;
            maximumDecodeMicroseconds_ = std::max(maximumDecodeMicroseconds_, decodeMicroseconds);
            backendStatus_ = media::DecoderBackendStatus{
                .sourceId = sourceId_,
                .backend = decoder_->backend(),
                .fallbackReason = decoder_->fallbackReason(),
                .deviceGeneration = decoder_->deviceGeneration(),
                .completedDecodeCount = completedDecodeCount_,
                .cacheHitCount = cacheHitCount_,
                .exactSeekCount = decoder_->exactSeekCount() +
                                  (exactDecoder_ != nullptr ? exactDecoder_->exactSeekCount() : 0U),
                .totalDecodeMicroseconds = totalDecodeMicroseconds_,
                .maximumDecodeMicroseconds = maximumDecodeMicroseconds_,
                .reverseWindowHitCount = reverseWindowHitCount_,
                .reverseWindowBuildCount = reverseWindowBuildCount_,
                .reverseWindowBuiltFrameCount = reverseWindowBuiltFrameCount_,
                .reverseWindowBuildMicroseconds = reverseWindowBuildMicroseconds_,
                .reverseWindowBuildMaximumMicroseconds = reverseWindowBuildMaximumMicroseconds_,
                .reverseExactFallbackCount = reverseExactFallbackCount_,
            };
        };
        const auto refreshReverseMetrics = [this] {
            std::scoped_lock lock{mutex_};
            backendStatus_.reverseWindowHitCount = reverseWindowHitCount_;
            backendStatus_.reverseWindowBuildCount = reverseWindowBuildCount_;
            backendStatus_.reverseWindowBuiltFrameCount = reverseWindowBuiltFrameCount_;
            backendStatus_.reverseWindowBuildMicroseconds = reverseWindowBuildMicroseconds_;
            backendStatus_.reverseWindowBuildMaximumMicroseconds =
                reverseWindowBuildMaximumMicroseconds_;
            backendStatus_.reverseExactFallbackCount = reverseExactFallbackCount_;
            backendStatus_.exactSeekCount =
                decoder_->exactSeekCount() +
                (exactDecoder_ != nullptr ? exactDecoder_->exactSeekCount() : 0U);
        };
        const auto reverseWorkInterrupted = [this](const SourceDecodeRequest& request) {
            if (request.cancellationRequested != nullptr &&
                request.cancellationRequested->load(std::memory_order_acquire)) {
                return true;
            }
            const std::scoped_lock lock{mutex_};
            // Stale reverse warmup must yield to playback, a new exact seek, open/close, or
            // sequential work. An already-queued reverse successor is the normal held-backward
            // pipeline the window exists to serve and is not an interruption (D04).
            return stopping_ || !controlQueue_.empty() || !exactQueue_.empty() ||
                   !sequentialQueue_.empty();
        };
        const auto fillReadAhead = [this, &recordDecode](const SourceDecodeRequest& request,
                                                         const std::size_t frameBytes) {
            if (request.priority != SourceDecodePriority::Sequential ||
                request.readAheadCount == 0U || frameBytes == 0U) {
                return;
            }
            const std::size_t cacheFrameCapacity = cache_.capacityBytes() / frameBytes;
            // A one-frame cache cannot provide meaningful look-ahead and requires an extra
            // transient allocation before it can evict its current entry. At 4K P010 that
            // transient overlaps render-generation retirement and can exhaust the shared budget.
            if (cacheFrameCapacity < 2U) {
                cache_.clear();
                return;
            }
            const std::uint8_t effectiveReadAhead = static_cast<std::uint8_t>(
                std::min<std::size_t>(request.readAheadCount, cacheFrameCapacity));
            for (std::uint8_t offset = 1U; offset <= effectiveReadAhead; ++offset) {
                bool urgentWorkQueued = false;
                {
                    const std::scoped_lock lock{mutex_};
                    urgentWorkQueued = stopping_ || !controlQueue_.empty() ||
                                       !exactQueue_.empty() || !sequentialQueue_.empty();
                }
                if (urgentWorkQueued ||
                    request.cancellationRequested->load(std::memory_order_acquire)) {
                    break;
                }

                const std::int64_t base = request.frameId.value();
                if (base > (std::numeric_limits<std::int64_t>::max)() - offset) {
                    break;
                }
                const domain::FrameId candidate{base + offset};
                if (candidate.value() >= sourceFrameCount_) {
                    break;
                }
                cacheKey_.sourceFrame = candidate;
                if (cache_.find(cacheKey_).has_value()) {
                    continue;
                }

                const auto started = std::chrono::steady_clock::now();
                emitDecodeStageTrace(application::TraceEventKind::SourceDecodeStarted,
                                     request,
                                     sourceId_,
                                     candidate);
                domain::Result<DecodedFrame> result =
                    decoder_->decodeSequential(candidate, *request.cancellationRequested);
                emitDecodeStageTrace(application::TraceEventKind::SourceDecodeCompleted,
                                     request,
                                     sourceId_,
                                     candidate);
                const auto elapsed = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count());
                recordDecode(elapsed);
                if (!result) {
                    break;
                }
                cache_.insert(cacheKey_,
                              CachedSourceFrame{
                                  .handle = result.value().handle,
                                  .presentationTime = result.value().presentationTime,
                              });
            }
        };
        cacheKey_.sourceFrame = decode->request.frameId;
        if (std::optional<CachedSourceFrame> cached = cache_.find(cacheKey_)) {
            {
                std::scoped_lock lock{mutex_};
                ++cacheHitCount_;
                if (decode->request.priority == SourceDecodePriority::Reverse) {
                    // A reverse target served from the GOP-window cache is a window hit.
                    ++reverseWindowHitCount_;
                    backendStatus_.reverseWindowHitCount = reverseWindowHitCount_;
                }
                backendStatus_.cacheHitCount = cacheHitCount_;
            }
            if (decode->request.context.has_value()) {
                const auto& request = decode->request.context->playback.request;
                application::PlaybackTrace::instance().record(
                    application::TraceEventKind::CacheHit,
                    application::TraceIdentity{
                        .session = request.sessionId,
                        .epoch = request.sessionEpoch,
                        .request = request.requestId,
                    },
                    static_cast<std::uint64_t>(cacheKey_.sourceFrame.value()));
            }
            const SourceDecodeRequest readAheadRequest = decode->request;
            const std::size_t frameBytes = cached->handle.accountedBytes();
            complete(std::move(*decode),
                     domain::Result<DecodedFrame>::success(DecodedFrame{
                         .handle = std::move(cached->handle),
                         .presentationTime = cached->presentationTime,
                     }));
            fillReadAhead(readAheadRequest, frameBytes);
            // Cache-hit reverse steps do NOT rebuild the GOP window: the window already covers
            // this target. Exhaustion falls through to a Reverse cache-miss, which exact-seeds
            // the next window below the new target (ADR-003).
            continue;
        }

        const bool preferSequentialDecode =
            decode->request.continueSequentially ||
            decode->request.priority == SourceDecodePriority::Prefetch;
        const auto decodeStarted = std::chrono::steady_clock::now();
        const bool useDedicatedExactDecoder =
            exactDecoder_ != nullptr &&
            (decode->request.priority == SourceDecodePriority::Exact ||
             decode->request.priority == SourceDecodePriority::Reverse ||
             decode->request.priority == SourceDecodePriority::Prefetch);
        SoftwareDecoder& selectedDecoder = useDedicatedExactDecoder ? *exactDecoder_ : *decoder_;
        bool& selectedDecoderNeedsReopen =
            useDedicatedExactDecoder ? exactDecoderNeedsReopen_ : decoderNeedsReopen_;
        domain::Result<DecodedFrame> result = domain::Result<DecodedFrame>::failure(
            actorError(sourceId_, "The source decoder could not be reopened after interruption."));
        if (selectedDecoderNeedsReopen) {
            application::PlaybackTrace::instance().record(
                application::TraceEventKind::DecoderReopen,
                application::TraceIdentity{
                    .request = domain::RequestId{static_cast<std::uint64_t>(sourceId_)}},
                static_cast<std::uint64_t>(decode->request.frameId.value()));
            const domain::Status reopened =
                selectedDecoder.open(*decode->request.cancellationRequested);
            if (reopened) {
                selectedDecoderNeedsReopen = false;
            } else {
                result = domain::Result<DecodedFrame>::failure(reopened.error());
            }
        }
        if (!selectedDecoderNeedsReopen) {
            emitDecodeStageTrace(application::TraceEventKind::SourceDecodeStarted,
                                 decode->request,
                                 sourceId_,
                                 decode->request.frameId);
            result = preferSequentialDecode
                         ? selectedDecoder.decodeSequential(decode->request.frameId,
                                                            *decode->request.cancellationRequested)
                         : selectedDecoder.decodeExact(decode->request.frameId,
                                                       *decode->request.cancellationRequested);
            emitDecodeStageTrace(application::TraceEventKind::SourceDecodeCompleted,
                                 decode->request,
                                 sourceId_,
                                 decode->request.frameId);
        }
        if (!result && !selectedDecoder.lastDecodeInterrupted()) {
            // FFmpeg may have stopped while an AVIO packet was only partially consumed. Reopen
            // from the actor before this decoder accepts another request; the decoder itself
            // remains a single-operation component and never mutates its lifecycle recursively.
            // An interrupted decode is excluded: cancellation is a clean stop that already
            // flushed the codec and demuxer state, and every exact request re-seeks before it
            // decodes, so reopening would only re-open the same file for nothing.
            selectedDecoderNeedsReopen = true;
        }
        const auto decodeMicroseconds =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - decodeStarted)
                                           .count());
        if (retainInCache && result) {
            cache_.insert(cacheKey_,
                          CachedSourceFrame{
                              .handle = result.value().handle,
                              .presentationTime = result.value().presentationTime,
                          });
        }
        recordDecode(decodeMicroseconds);
        const SourceDecodeRequest readAheadRequest = decode->request;
        const bool decoded = static_cast<bool>(result);
        const std::size_t frameBytes = decoded ? result.value().handle.accountedBytes() : 0U;
        complete(std::move(*decode), std::move(result));
        if (decoded) {
            fillReadAhead(readAheadRequest, frameBytes);
            fillReverseGopWindow(readAheadRequest,
                                 frameBytes,
                                 selectedDecoder,
                                 recordDecode,
                                 reverseWorkInterrupted,
                                 refreshReverseMetrics);
        }
    }
    decoder_->close();
    if (exactDecoder_ != nullptr) {
        exactDecoder_->close();
    }
}

void SourceDecodeActor::cancelQueuedLocked() {
    const auto cancel = [this](std::deque<DecodeJob>& queue) {
        while (!queue.empty()) {
            DecodeJob job = std::move(queue.front());
            queue.pop_front();
            completeCanceled(std::move(job));
        }
    };
    cancel(exactQueue_);
    cancel(sequentialQueue_);
    cancel(prefetchQueue_);
}

void SourceDecodeActor::completeCanceled(DecodeJob job) noexcept {
    complete(std::move(job),
             domain::Result<DecodedFrame>::failure(
                 actorError(sourceId_, "The queued source decode request was superseded.")));
}

void SourceDecodeActor::complete(DecodeJob job, domain::Result<DecodedFrame> result) noexcept {
    try {
        job.completion(std::move(result));
    } catch (...) {
    }
}

void SourceDecodeActor::fillReverseGopWindow(
    const SourceDecodeRequest& request,
    const std::size_t frameBytes,
    SoftwareDecoder& selectedDecoder,
    const std::function<void(std::uint64_t)>& recordDecode,
    const std::function<bool(const SourceDecodeRequest&)>& interrupted,
    const std::function<void()>& refreshMetrics) noexcept {
    if (request.priority != SourceDecodePriority::Reverse || request.reverseWindowFrames == 0U ||
        frameBytes == 0U || !request.frameId.isValid()) {
        return;
    }
    const std::int64_t base = request.frameId.value();
    if (base <= 0) {
        return;
    }

    const std::size_t cacheFrameCapacity = cache_.capacityBytes() / frameBytes;
    // Held-backward hardware/size gate: a cache that cannot retain a useful multi-frame window
    // must not block reverse steps on speculative builds. Per-step Exact remains correct.
    if (cacheFrameCapacity < 2U) {
        {
            std::scoped_lock lock{mutex_};
            ++reverseExactFallbackCount_;
            backendStatus_.reverseExactFallbackCount = reverseExactFallbackCount_;
        }
        application::PlaybackTrace::instance().record(
            application::TraceEventKind::ReverseExactFallback,
            application::TraceIdentity{
                .request = domain::RequestId{static_cast<std::uint64_t>(sourceId_)}},
            static_cast<std::uint64_t>(base));
        return;
    }

    const std::size_t budgetFrames = static_cast<std::size_t>(std::min<std::uint8_t>(
        request.reverseWindowFrames,
        static_cast<std::uint8_t>((std::min)(
            cacheFrameCapacity, static_cast<std::size_t>(kMaximumReverseWindowFrames)))));
    const std::int64_t windowStart =
        (std::max)(std::int64_t{0}, base - static_cast<std::int64_t>(budgetFrames));

    std::int64_t lowestMissing = base;
    for (std::int64_t candidate = windowStart; candidate < base; ++candidate) {
        cacheKey_.sourceFrame = domain::FrameId{candidate};
        if (!cache_.find(cacheKey_).has_value()) {
            lowestMissing = candidate;
            break;
        }
    }
    if (lowestMissing == base) {
        {
            std::scoped_lock lock{mutex_};
            ++reverseWindowHitCount_;
            backendStatus_.reverseWindowHitCount = reverseWindowHitCount_;
        }
        application::PlaybackTrace::instance().record(
            application::TraceEventKind::ReverseWindowHit,
            application::TraceIdentity{
                .request = domain::RequestId{static_cast<std::uint64_t>(sourceId_)}},
            static_cast<std::uint64_t>(base));
        return;
    }

    if (interrupted(request)) {
        {
            std::scoped_lock lock{mutex_};
            ++reverseExactFallbackCount_;
            backendStatus_.reverseExactFallbackCount = reverseExactFallbackCount_;
        }
        application::PlaybackTrace::instance().record(
            application::TraceEventKind::ReverseExactFallback,
            application::TraceIdentity{
                .request = domain::RequestId{static_cast<std::uint64_t>(sourceId_)}},
            static_cast<std::uint64_t>(base));
        return;
    }

    const auto buildStarted = std::chrono::steady_clock::now();
    // ADR-006/ADR-003: one Exact seed at the lowest uncached reverse target, then sequential
    // walk upward. A long GOP costs one seek instead of one seek per held-backward step.
    cacheKey_.sourceFrame = domain::FrameId{lowestMissing};
    emitDecodeStageTrace(application::TraceEventKind::SourceDecodeStarted,
                         request,
                         sourceId_,
                         domain::FrameId{lowestMissing});
    domain::Result<DecodedFrame> seed =
        selectedDecoder.decodeExact(domain::FrameId{lowestMissing}, *request.cancellationRequested);
    emitDecodeStageTrace(application::TraceEventKind::SourceDecodeCompleted,
                         request,
                         sourceId_,
                         domain::FrameId{lowestMissing});
    if (!seed) {
        {
            std::scoped_lock lock{mutex_};
            ++reverseExactFallbackCount_;
            backendStatus_.reverseExactFallbackCount = reverseExactFallbackCount_;
        }
        application::PlaybackTrace::instance().record(
            application::TraceEventKind::ReverseExactFallback,
            application::TraceIdentity{
                .request = domain::RequestId{static_cast<std::uint64_t>(sourceId_)}},
            static_cast<std::uint64_t>(base));
        return;
    }
    cache_.insert(cacheKey_,
                  CachedSourceFrame{
                      .handle = seed.value().handle,
                      .presentationTime = seed.value().presentationTime,
                  });
    std::uint64_t builtFrames = 1U;

    // D04: a newer seek/playback after the seed abandons the remaining walk immediately.
    if (interrupted(request)) {
        const auto partialMicroseconds =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - buildStarted)
                                           .count());
        {
            std::scoped_lock lock{mutex_};
            ++reverseWindowBuildCount_;
            reverseWindowBuiltFrameCount_ += builtFrames;
            reverseWindowBuildMicroseconds_ += partialMicroseconds;
            reverseWindowBuildMaximumMicroseconds_ =
                std::max(reverseWindowBuildMaximumMicroseconds_, partialMicroseconds);
        }
        refreshMetrics();
        application::PlaybackTrace::instance().record(
            application::TraceEventKind::ReverseWindowBuilt,
            application::TraceIdentity{
                .request = domain::RequestId{static_cast<std::uint64_t>(sourceId_)}},
            builtFrames);
        return;
    }

    for (std::int64_t candidate = lowestMissing + 1; candidate < base; ++candidate) {
        if (interrupted(request)) {
            break;
        }
        const auto elapsed =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - buildStarted)
                                           .count());
        if (elapsed > kReverseWindowBuildBudgetMicroseconds) {
            break;
        }
        const auto decodeStarted = std::chrono::steady_clock::now();
        emitDecodeStageTrace(application::TraceEventKind::SourceDecodeStarted,
                             request,
                             sourceId_,
                             domain::FrameId{candidate});
        domain::Result<DecodedFrame> decoded = selectedDecoder.decodeSequential(
            domain::FrameId{candidate}, *request.cancellationRequested);
        emitDecodeStageTrace(application::TraceEventKind::SourceDecodeCompleted,
                             request,
                             sourceId_,
                             domain::FrameId{candidate});
        const auto decodeElapsed =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - decodeStarted)
                                           .count());
        recordDecode(decodeElapsed);
        if (!decoded) {
            break;
        }
        cacheKey_.sourceFrame = domain::FrameId{candidate};
        if (!cache_.find(cacheKey_).has_value()) {
            cache_.insert(cacheKey_,
                          CachedSourceFrame{
                              .handle = decoded.value().handle,
                              .presentationTime = decoded.value().presentationTime,
                          });
        }
        ++builtFrames;
    }

    const auto buildMicroseconds =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - buildStarted)
                                       .count());
    {
        std::scoped_lock lock{mutex_};
        ++reverseWindowBuildCount_;
        reverseWindowBuiltFrameCount_ += builtFrames;
        reverseWindowBuildMicroseconds_ += buildMicroseconds;
        reverseWindowBuildMaximumMicroseconds_ =
            std::max(reverseWindowBuildMaximumMicroseconds_, buildMicroseconds);
    }
    refreshMetrics();
    application::PlaybackTrace::instance().record(
        application::TraceEventKind::ReverseWindowBuilt,
        application::TraceIdentity{.request =
                                       domain::RequestId{static_cast<std::uint64_t>(sourceId_)}},
        builtFrames);
}

} // namespace dvs::media::internal
