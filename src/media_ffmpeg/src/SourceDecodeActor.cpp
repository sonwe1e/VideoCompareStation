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

[[nodiscard]] std::size_t capacityFor(const SourceDecodePriority priority) noexcept {
    switch (priority) {
    case SourceDecodePriority::Exact:
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
        request.readAheadCount > kMaximumReadAheadCount || !completion) {
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

        if (job.request.priority == SourceDecodePriority::Exact) {
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
            };
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
                domain::Result<DecodedFrame> result =
                    decoder_->decodeSequential(candidate, *request.cancellationRequested);
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
            continue;
        }

        const bool preferSequentialDecode =
            decode->request.continueSequentially ||
            decode->request.priority == SourceDecodePriority::Prefetch;
        const auto decodeStarted = std::chrono::steady_clock::now();
        const bool useDedicatedExactDecoder =
            exactDecoder_ != nullptr &&
            (decode->request.priority == SourceDecodePriority::Exact ||
             decode->request.priority == SourceDecodePriority::Prefetch);
        SoftwareDecoder& selectedDecoder = useDedicatedExactDecoder ? *exactDecoder_ : *decoder_;
        bool& selectedDecoderNeedsReopen =
            useDedicatedExactDecoder ? exactDecoderNeedsReopen_ : decoderNeedsReopen_;
        domain::Result<DecodedFrame> result = domain::Result<DecodedFrame>::failure(
            actorError(sourceId_, "The source decoder could not be reopened after interruption."));
        if (selectedDecoderNeedsReopen) {
            const domain::Status reopened =
                selectedDecoder.open(*decode->request.cancellationRequested);
            if (reopened) {
                selectedDecoderNeedsReopen = false;
            } else {
                result = domain::Result<DecodedFrame>::failure(reopened.error());
            }
        }
        if (!selectedDecoderNeedsReopen) {
            result = preferSequentialDecode
                         ? selectedDecoder.decodeSequential(decode->request.frameId,
                                                            *decode->request.cancellationRequested)
                         : selectedDecoder.decodeExact(decode->request.frameId,
                                                       *decode->request.cancellationRequested);
        }
        if (!result) {
            // FFmpeg may have stopped while an AVIO packet was only partially consumed. Reopen
            // from the actor before this decoder accepts another request; the decoder itself
            // remains a single-operation component and never mutates its lifecycle recursively.
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

} // namespace dvs::media::internal
