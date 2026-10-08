#include "dvs/media/AlignmentAnalysisService.h"

#include "dvs/application/AlignmentWorkEstimator.h"

#include "SignatureCache.h"
#include "SignatureDecodeSession.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace dvs::media {
namespace {

using AnalysisRequest =
    std::variant<application::AlignmentEstimateRequest, application::SequenceAlignmentRequest>;

[[nodiscard]] const application::PlaybackRequestContext&
context(const AnalysisRequest& request) noexcept { // NOLINT(bugprone-exception-escape): std::visit
                                                   // over a valueless-safe variant
    return std::visit(
        [](const auto& value) -> const application::PlaybackRequestContext& {
            return value.context;
        },
        request);
}

[[nodiscard]] application::AlignmentAnalysisJobId jobId( // NOLINT(bugprone-exception-escape)
    const AnalysisRequest& request) noexcept {
    return std::visit([](const auto& value) { return value.jobId; }, request);
}

[[nodiscard]] application::AlignmentAnalysisKind kind(const AnalysisRequest& request) noexcept {
    return std::holds_alternative<application::AlignmentEstimateRequest>(request)
               ? application::AlignmentAnalysisKind::GlobalOffset
               : application::AlignmentAnalysisKind::Sequence;
}

[[nodiscard]] domain::MediaError
serviceError(std::string detail, const std::optional<domain::SourceId> sourceId = std::nullopt) {
    return domain::makeMediaError(domain::MediaErrorCode::kMediaDecodeFailed,
                                  domain::MediaOperation::kMediaDecode,
                                  sourceId,
                                  true,
                                  std::move(detail));
}

void post(const std::weak_ptr<application::IApplicationEventSink>& events,
          application::ApplicationEvent event,
          const bool realtime = false) noexcept {
    if (const std::shared_ptr<application::IApplicationEventSink> sink = events.lock()) {
        if (realtime) {
            static_cast<void>(sink->postRealtime(std::move(event)));
        } else {
            static_cast<void>(sink->postCritical(std::move(event)));
        }
    }
}

struct AnalysisOperation final {
    AnalysisRequest request;
    std::weak_ptr<application::IApplicationEventSink> events;
    std::atomic<bool> canceled = false;

    AnalysisOperation(AnalysisRequest requestValue,
                      std::weak_ptr<application::IApplicationEventSink> eventsValue)
        : request(std::move(requestValue)), events(std::move(eventsValue)) {}
};

[[nodiscard]] const domain::ComparisonSource*
findSource(const std::vector<domain::ComparisonSource>& sources,
           const domain::SourceId sourceId) noexcept {
    const auto found = std::find_if(sources.begin(), sources.end(), [sourceId](const auto& source) {
        return source.id == sourceId;
    });
    return found == sources.end() ? nullptr : &*found;
}

[[nodiscard]] std::uint64_t saturatedAdd(const std::uint64_t left,
                                         const std::uint64_t right) noexcept {
    return right > (std::numeric_limits<std::uint64_t>::max)() - left
               ? (std::numeric_limits<std::uint64_t>::max)()
               : left + right;
}

[[nodiscard]] std::uint64_t
dynamicProgrammingUnits(const std::int64_t referenceCount,
                        const std::int64_t targetCount,
                        const application::SequenceAlignmentOptions& options) noexcept {
    std::uint64_t units = 0U;
    for (std::int64_t prefix = 0; prefix <= referenceCount; ++prefix) {
        const std::int64_t center = prefix + options.expectedOffset;
        const std::int64_t first =
            std::max<std::int64_t>(0, center - static_cast<std::int64_t>(options.bandWidth));
        const std::int64_t last =
            std::min(targetCount, center + static_cast<std::int64_t>(options.bandWidth));
        if (last >= first) {
            units = saturatedAdd(units, static_cast<std::uint64_t>(last - first + 1));
        }
    }
    return units;
}

struct GlobalPlan final {
    application::GlobalOffsetEstimationOptions options;
    std::vector<domain::FrameId> anchors;
};

[[nodiscard]] std::optional<GlobalPlan>
buildGlobalPlan(const application::AlignmentEstimateRequest& request) {
    const domain::ComparisonSource* const canonical =
        findSource(request.sources, request.canonicalSourceId);
    if (canonical == nullptr || !request.options.isValid() ||
        request.candidateSampleCount < request.options.minimumEvidence ||
        canonical->descriptor.frameCount.value <
            static_cast<std::int64_t>(request.options.minimumEvidence)) {
        return std::nullopt;
    }

    GlobalPlan plan{.options = request.options};
    const std::int64_t canonicalFrameCount = canonical->descriptor.frameCount.value;
    const std::int64_t maximumWindow =
        (canonicalFrameCount - static_cast<std::int64_t>(plan.options.minimumEvidence)) / 2;
    plan.options.minimumOffset = std::max(plan.options.minimumOffset, -maximumWindow);
    plan.options.maximumOffset = std::min(plan.options.maximumOffset, maximumWindow);
    const std::int64_t safeFirst = std::max<std::int64_t>(0, -plan.options.minimumOffset);
    const std::int64_t safeLast =
        canonicalFrameCount - 1 - std::max<std::int64_t>(0, plan.options.maximumOffset);
    if (safeLast < safeFirst ||
        static_cast<std::uint64_t>(safeLast - safeFirst + 1) < request.options.minimumEvidence) {
        return std::nullopt;
    }

    const std::size_t sampleCount = std::min<std::size_t>(
        request.candidateSampleCount, static_cast<std::size_t>(safeLast - safeFirst + 1));
    plan.anchors.reserve(sampleCount);
    for (std::size_t index = 0U; index < sampleCount; ++index) {
        const std::int64_t frame =
            sampleCount == 1U
                ? safeFirst
                : safeFirst + static_cast<std::int64_t>(
                                  (static_cast<std::uint64_t>(safeLast - safeFirst) * index) /
                                  (sampleCount - 1U));
        if (plan.anchors.empty() || plan.anchors.back().value() != frame) {
            plan.anchors.emplace_back(frame);
        }
    }
    return plan;
}

[[nodiscard]] std::vector<domain::FrameId>
targetFrames(const domain::ComparisonSource& source,
             const std::vector<domain::FrameId>& anchors,
             const application::GlobalOffsetEstimationOptions& options) {
    std::set<std::int64_t> unique;
    for (const domain::FrameId anchor : anchors) {
        for (std::int64_t offset = options.minimumOffset; offset <= options.maximumOffset;
             ++offset) {
            const std::int64_t frame = anchor.value() + offset;
            if (frame >= 0 && frame < source.descriptor.frameCount.value) {
                unique.insert(frame);
            }
            if (offset == options.maximumOffset) {
                break;
            }
        }
    }
    std::vector<domain::FrameId> frames;
    frames.reserve(unique.size());
    for (const std::int64_t frame : unique) {
        frames.emplace_back(frame);
    }
    return frames;
}

} // namespace

class AlignmentAnalysisService::Impl final {
public:
    explicit Impl(const std::size_t queueCapacity)
        : queueCapacity_(std::max<std::size_t>(1U, queueCapacity)), worker_([this] { run(); }) {}

    ~Impl() {
        shutdown();
    }

    [[nodiscard]] application::PortSubmitResult
    submit(AnalysisRequest request,
           const std::shared_ptr<application::IApplicationEventSink>& events) {
        if (!events || jobId(request).value == 0U) {
            return application::PortSubmitResult::Closed;
        }
        const auto operation = std::make_shared<AnalysisOperation>(
            std::move(request), std::weak_ptr<application::IApplicationEventSink>{events});
        {
            std::scoped_lock lock(mutex_);
            if (closed_) {
                return application::PortSubmitResult::Closed;
            }
            if (queue_.size() >= queueCapacity_) {
                return application::PortSubmitResult::Busy;
            }
            queue_.push_back(operation);
        }
        condition_.notify_one();
        return application::PortSubmitResult::Accepted;
    }

    void cancel(const application::AlignmentAnalysisJobId requestedJobId) noexcept {
        std::shared_ptr<AnalysisOperation> queued;
        std::shared_ptr<AnalysisOperation> active;
        {
            std::scoped_lock lock(mutex_);
            const auto found =
                std::find_if(queue_.begin(), queue_.end(), [requestedJobId](const auto& item) {
                    return jobId(item->request) == requestedJobId;
                });
            if (found != queue_.end()) {
                queued = std::move(*found);
                queue_.erase(found);
                queued->canceled.store(true, std::memory_order_release);
            }
            if (active_ && jobId(active_->request) == requestedJobId) {
                active = active_;
                active->canceled.store(true, std::memory_order_release);
                for (internal::SignatureDecodeSession* const decoder : activeDecoders_) {
                    decoder->requestInterrupt();
                }
            }
        }
        if (queued) {
            postCanceled(*queued, application::CancellationReason::UserRequested);
        }
    }

    [[nodiscard]] std::uint64_t decodedSignatureCount() const noexcept {
        return decodedSignatureCount_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t openSessionCount() const noexcept {
        return openSessionCount_.load(std::memory_order_acquire);
    }

private:
    class RegisteredDecoder final {
    public:
        RegisteredDecoder(Impl& owner,
                          const domain::ComparisonSource& source,
                          const std::atomic<bool>& cancellation)
            : owner_(owner), decoder_(source.id, source.descriptor) {
            status_ = decoder_.open(cancellation);
            if (status_) {
                std::scoped_lock lock(owner_.mutex_);
                owner_.activeDecoders_.push_back(&decoder_);
                owner_.openSessionCount_.fetch_add(1U, std::memory_order_release);
                registered_ = true;
            }
        }

        ~RegisteredDecoder() {
            if (registered_) {
                std::scoped_lock lock(owner_.mutex_);
                const auto found = std::find(
                    owner_.activeDecoders_.begin(), owner_.activeDecoders_.end(), &decoder_);
                if (found != owner_.activeDecoders_.end()) {
                    owner_.activeDecoders_.erase(found);
                }
                decoder_.close();
                owner_.openSessionCount_.fetch_sub(1U, std::memory_order_release);
            }
        }

        [[nodiscard]] const domain::Status& status() const noexcept {
            return status_;
        }

        [[nodiscard]] internal::SignatureDecodeSession& decoder() noexcept {
            return decoder_;
        }

    private:
        Impl& owner_;
        internal::SignatureDecodeSession decoder_;
        domain::Status status_ = domain::Status::success();
        bool registered_ = false;
    };

    struct ProgressState final {
        application::AlignmentWorkEstimate work;
        std::uint64_t completed = 0U;
        std::uint64_t lastPublished = 0U;
    };

    void postCanceled(const AnalysisOperation& operation,
                      const application::CancellationReason reason) const noexcept {
        post(operation.events,
             application::ApplicationEvent{application::AlignmentAnalysisCanceled{
                 .jobId = jobId(operation.request),
                 .context = context(operation.request),
                 .kind = kind(operation.request),
                 .reason = reason,
             }});
    }

    void postFailed(const AnalysisOperation& operation, domain::MediaError error) const noexcept {
        error.requestId = context(operation.request).request.requestId;
        post(operation.events,
             application::ApplicationEvent{application::AlignmentAnalysisFailed{
                 .jobId = jobId(operation.request),
                 .context = context(operation.request),
                 .kind = kind(operation.request),
                 .error = std::move(error),
             }});
    }

    void postStarted(const AnalysisOperation& operation,
                     const application::AlignmentWorkEstimate& work) const noexcept {
        post(operation.events,
             application::ApplicationEvent{application::AlignmentAnalysisStarted{
                 .jobId = jobId(operation.request),
                 .context = context(operation.request),
                 .kind = kind(operation.request),
                 .work = work,
             }});
    }

    void advanceProgress(const AnalysisOperation& operation,
                         ProgressState& progress,
                         const std::uint64_t units,
                         const application::AlignmentAnalysisPhase phase,
                         const bool force = false) const noexcept {
        progress.completed =
            std::min(progress.work.totalUnits, saturatedAdd(progress.completed, units));
        if (!force && progress.completed != progress.work.totalUnits &&
            progress.completed - progress.lastPublished < 32U) {
            return;
        }
        progress.lastPublished = progress.completed;
        post(operation.events,
             application::ApplicationEvent{application::AlignmentAnalysisProgress{
                 .jobId = jobId(operation.request),
                 .context = context(operation.request),
                 .kind = kind(operation.request),
                 .phase = phase,
                 .completedUnits = progress.completed,
                 .work = progress.work,
             }},
             true);
    }

    [[nodiscard]] bool canceled(const AnalysisOperation& operation) const noexcept {
        return operation.canceled.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::optional<std::vector<application::FrameLumaSignature>>
    collectFrames(const AnalysisOperation& operation,
                  const domain::ComparisonSource& source,
                  const std::vector<domain::FrameId>& frames,
                  ProgressState& progress,
                  domain::MediaError* const failure) {
        std::vector<application::FrameLumaSignature> signatures;
        signatures.reserve(frames.size());
        std::vector<domain::FrameId> missing;
        missing.reserve(frames.size());
        for (const domain::FrameId frame : frames) {
            const auto cached = signatureCache_.find(source.descriptor, frame);
            if (cached.has_value()) {
                signatures.push_back(*cached);
                advanceProgress(operation,
                                progress,
                                1U,
                                application::AlignmentAnalysisPhase::CollectingSignatures);
            } else {
                missing.push_back(frame);
            }
        }
        if (!missing.empty()) {
            RegisteredDecoder registered{*this, source, operation.canceled};
            if (!registered.status()) {
                *failure = registered.status().error();
                return std::nullopt;
            }
            for (const domain::FrameId frame : missing) {
                auto signature = registered.decoder().decodeSignature(frame, operation.canceled);
                if (!signature) {
                    *failure = signature.error();
                    return std::nullopt;
                }
                decodedSignatureCount_.fetch_add(1U, std::memory_order_release);
                signatureCache_.store(source.descriptor, signature.value());
                signatures.push_back(std::move(signature).value());
                advanceProgress(operation,
                                progress,
                                1U,
                                application::AlignmentAnalysisPhase::CollectingSignatures);
            }
        }
        std::sort(signatures.begin(), signatures.end(), [](const auto& left, const auto& right) {
            return left.frameId < right.frameId;
        });
        advanceProgress(operation,
                        progress,
                        0U,
                        application::AlignmentAnalysisPhase::CollectingSignatures,
                        true);
        return signatures;
    }

    [[nodiscard]] std::optional<std::vector<application::FrameLumaSignature>>
    collectRange(const AnalysisOperation& operation,
                 const domain::ComparisonSource& source,
                 ProgressState& progress,
                 domain::MediaError* const failure) {
        const std::int64_t frameCount = source.descriptor.frameCount.value;
        auto cached = signatureCache_.findRange(source.descriptor, frameCount);
        if (cached.has_value()) {
            advanceProgress(operation,
                            progress,
                            static_cast<std::uint64_t>(frameCount),
                            application::AlignmentAnalysisPhase::CollectingSignatures,
                            true);
            return cached;
        }

        RegisteredDecoder registered{*this, source, operation.canceled};
        if (!registered.status()) {
            *failure = registered.status().error();
            return std::nullopt;
        }
        auto signatures = registered.decoder().decodeRange(
            domain::FrameId{0},
            frameCount,
            operation.canceled,
            [this, &operation, &progress](const std::uint64_t units) {
                decodedSignatureCount_.fetch_add(units, std::memory_order_release);
                advanceProgress(operation,
                                progress,
                                units,
                                application::AlignmentAnalysisPhase::CollectingSignatures);
            });
        if (!signatures) {
            *failure = signatures.error();
            return std::nullopt;
        }
        signatureCache_.storeRange(source.descriptor, signatures.value());
        advanceProgress(operation,
                        progress,
                        0U,
                        application::AlignmentAnalysisPhase::CollectingSignatures,
                        true);
        return std::move(signatures).value();
    }

    void completeGlobal(const AnalysisOperation& operation,
                        std::vector<application::GlobalOffsetEstimate> estimates) const noexcept {
        post(operation.events,
             application::ApplicationEvent{application::AlignmentAnalysisCompleted{
                 .jobId = jobId(operation.request),
                 .context = context(operation.request),
                 .kind = application::AlignmentAnalysisKind::GlobalOffset,
                 .estimates = std::move(estimates),
             }});
    }

    void
    completeSequence(const AnalysisOperation& operation,
                     std::vector<application::SequenceAlignmentResult> results) const noexcept {
        post(operation.events,
             application::ApplicationEvent{application::AlignmentAnalysisCompleted{
                 .jobId = jobId(operation.request),
                 .context = context(operation.request),
                 .kind = application::AlignmentAnalysisKind::Sequence,
                 .sequenceResults = std::move(results),
             }});
    }

    void executeGlobal(const AnalysisOperation& operation,
                       const application::AlignmentEstimateRequest& request) {
        const std::optional<GlobalPlan> plan = buildGlobalPlan(request);
        const application::AlignmentWorkEstimate work = application::estimateAlignmentWork(request);
        if (!plan.has_value() || work.totalUnits == 0U) {
            postFailed(operation,
                       serviceError("The global alignment request cannot form a valid sample "
                                    "plan."));
            return;
        }
        postStarted(operation, work);
        ProgressState progress{.work = work};
        domain::MediaError failure = serviceError("Signature collection failed.");
        const domain::ComparisonSource* const canonical =
            findSource(request.sources, request.canonicalSourceId);
        auto reference = collectFrames(operation, *canonical, plan->anchors, progress, &failure);
        if (!reference.has_value()) {
            canceled(operation)
                ? postCanceled(operation, application::CancellationReason::UserRequested)
                : postFailed(operation, std::move(failure));
            return;
        }

        std::vector<application::GlobalOffsetEstimate> estimates;
        estimates.reserve(request.sources.size() - 1U);
        for (const domain::ComparisonSource& source : request.sources) {
            if (source.id == request.canonicalSourceId) {
                continue;
            }
            auto target = collectFrames(operation,
                                        source,
                                        targetFrames(source, plan->anchors, plan->options),
                                        progress,
                                        &failure);
            if (!target.has_value()) {
                canceled(operation)
                    ? postCanceled(operation, application::CancellationReason::UserRequested)
                    : postFailed(operation, std::move(failure));
                return;
            }
            advanceProgress(operation,
                            progress,
                            0U,
                            application::AlignmentAnalysisPhase::ComputingAlignment,
                            true);
            const auto estimate =
                application::estimateGlobalOffset(source.id, *reference, *target, plan->options);
            if (!estimate.has_value()) {
                postFailed(operation,
                           serviceError("Collected signatures could not produce a global "
                                        "alignment estimate.",
                                        source.id));
                return;
            }
            estimates.push_back(*estimate);
        }
        if (canceled(operation)) {
            postCanceled(operation, application::CancellationReason::UserRequested);
            return;
        }
        advanceProgress(operation,
                        progress,
                        work.totalUnits - progress.completed,
                        application::AlignmentAnalysisPhase::ComputingAlignment,
                        true);
        completeGlobal(operation, std::move(estimates));
    }

    [[nodiscard]] bool validateSequence(const application::SequenceAlignmentRequest& request,
                                        domain::MediaError* const error) const {
        if (!request.options.isValid() || request.maximumFrameCount == 0U ||
            request.maximumFrameCount > 100'000U ||
            findSource(request.sources, request.canonicalSourceId) == nullptr) {
            *error = serviceError("The sequence alignment request is invalid.");
            return false;
        }
        for (std::size_t index = 0U; index < request.expectedOffsets.size(); ++index) {
            const application::SourceFrameOffset& offset = request.expectedOffsets[index];
            const bool known = findSource(request.sources, offset.sourceId) != nullptr;
            const bool duplicate = std::any_of(
                request.expectedOffsets.begin() + static_cast<std::ptrdiff_t>(index + 1U),
                request.expectedOffsets.end(),
                [&offset](const auto& other) { return other.sourceId == offset.sourceId; });
            if (!known || duplicate || offset.sourceId == request.canonicalSourceId ||
                std::abs(offset.frames) > static_cast<std::int64_t>(request.options.bandWidth)) {
                *error = serviceError(
                    "Sequence expected offsets must uniquely name open non-canonical sources.",
                    offset.sourceId);
                return false;
            }
        }
        for (std::size_t index = 0U; index < request.manualAnchors.size(); ++index) {
            const application::SourceAlignmentAnchors& anchors = request.manualAnchors[index];
            const domain::ComparisonSource* const source =
                findSource(request.sources, anchors.sourceId);
            const bool duplicate = std::any_of(
                request.manualAnchors.begin() + static_cast<std::ptrdiff_t>(index + 1U),
                request.manualAnchors.end(),
                [&anchors](const auto& other) { return other.sourceId == anchors.sourceId; });
            if (source == nullptr || duplicate || anchors.sourceId == request.canonicalSourceId ||
                !anchors.isValid()) {
                *error =
                    serviceError("Sequence anchors must uniquely name open non-canonical sources.",
                                 anchors.sourceId);
                return false;
            }
            const domain::ComparisonSource* const canonical =
                findSource(request.sources, request.canonicalSourceId);
            if (std::any_of(anchors.anchors.begin(),
                            anchors.anchors.end(),
                            [&](const application::ManualAlignmentAnchor& anchor) {
                                return anchor.canonicalFrameId.value() >=
                                           canonical->descriptor.frameCount.value ||
                                       anchor.sourceFrameId.value() >=
                                           source->descriptor.frameCount.value;
                            })) {
                *error =
                    serviceError("Sequence anchors are outside the source timeline.", source->id);
                return false;
            }
        }
        for (const domain::ComparisonSource& source : request.sources) {
            if (source.descriptor.frameCount.value <= 0 ||
                source.descriptor.frameCount.value >
                    static_cast<std::int64_t>(request.maximumFrameCount)) {
                *error =
                    serviceError("A sequence source exceeds the supported frame count.", source.id);
                return false;
            }
        }
        return true;
    }

    void executeSequence(const AnalysisOperation& operation,
                         const application::SequenceAlignmentRequest& request) {
        domain::MediaError failure = serviceError("The sequence request is invalid.");
        const application::AlignmentWorkEstimate work = application::estimateAlignmentWork(request);
        if (!validateSequence(request, &failure) || work.totalUnits == 0U) {
            postFailed(operation, std::move(failure));
            return;
        }
        postStarted(operation, work);
        ProgressState progress{.work = work};
        const domain::ComparisonSource* const canonical =
            findSource(request.sources, request.canonicalSourceId);
        auto reference = collectRange(operation, *canonical, progress, &failure);
        if (!reference.has_value()) {
            canceled(operation)
                ? postCanceled(operation, application::CancellationReason::UserRequested)
                : postFailed(operation, std::move(failure));
            return;
        }

        std::vector<application::SequenceAlignmentResult> results;
        results.reserve(request.sources.size() - 1U);
        for (const domain::ComparisonSource& source : request.sources) {
            if (source.id == request.canonicalSourceId) {
                continue;
            }
            auto target = collectRange(operation, source, progress, &failure);
            if (!target.has_value()) {
                canceled(operation)
                    ? postCanceled(operation, application::CancellationReason::UserRequested)
                    : postFailed(operation, std::move(failure));
                return;
            }
            application::SequenceAlignmentOptions options = request.options;
            const auto expected = std::find_if(
                request.expectedOffsets.begin(),
                request.expectedOffsets.end(),
                [&source](const auto& offset) { return offset.sourceId == source.id; });
            options.expectedOffset =
                expected == request.expectedOffsets.end() ? 0 : expected->frames;
            const auto requestedAnchors =
                std::find_if(request.manualAnchors.begin(),
                             request.manualAnchors.end(),
                             [&source](const auto& value) { return value.sourceId == source.id; });
            const std::span<const application::ManualAlignmentAnchor> anchors =
                requestedAnchors == request.manualAnchors.end()
                    ? std::span<const application::ManualAlignmentAnchor>{}
                    : std::span<const application::ManualAlignmentAnchor>{
                          requestedAnchors->anchors};
            advanceProgress(operation,
                            progress,
                            0U,
                            application::AlignmentAnalysisPhase::ComputingAlignment,
                            true);
            const auto result =
                application::alignFrameSequences(source.id, *reference, *target, options, anchors);
            if (!result.has_value()) {
                postFailed(operation,
                           serviceError("Collected signatures could not produce a sequence "
                                        "alignment map.",
                                        source.id));
                return;
            }
            results.push_back(*result);
            advanceProgress(operation,
                            progress,
                            dynamicProgrammingUnits(static_cast<std::int64_t>(reference->size()),
                                                    static_cast<std::int64_t>(target->size()),
                                                    options),
                            application::AlignmentAnalysisPhase::ComputingAlignment,
                            true);
        }
        if (canceled(operation)) {
            postCanceled(operation, application::CancellationReason::UserRequested);
            return;
        }
        advanceProgress(operation,
                        progress,
                        work.totalUnits - progress.completed,
                        application::AlignmentAnalysisPhase::ComputingAlignment,
                        true);
        completeSequence(operation, std::move(results));
    }

    void execute(const AnalysisOperation& operation) {
        std::visit(
            [this, &operation](const auto& request) {
                using Request = std::decay_t<decltype(request)>;
                if constexpr (std::is_same_v<Request, application::AlignmentEstimateRequest>) {
                    executeGlobal(operation, request);
                } else {
                    executeSequence(operation, request);
                }
            },
            operation.request);
    }

    void run() {
        static_cast<void>(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL));
        for (;;) {
            std::shared_ptr<AnalysisOperation> operation;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return closed_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (closed_) {
                        return;
                    }
                    continue;
                }
                operation = std::move(queue_.front());
                queue_.pop_front();
                active_ = operation;
            }
            try {
                const bool hasMetadata = std::visit(
                    [](const auto& request) {
                        return request.sources.size() >= 2U && request.timeline.has_value();
                    },
                    operation->request);
                if (!hasMetadata) {
                    postFailed(*operation,
                               serviceError("The analysis job is missing source metadata."));
                } else if (canceled(*operation)) {
                    postCanceled(*operation, application::CancellationReason::UserRequested);
                } else {
                    execute(*operation);
                }
            } catch (const std::exception& exception) {
                postFailed(*operation,
                           serviceError("Unexpected analysis service exception: " +
                                        std::string{exception.what()}));
            } catch (...) {
                postFailed(*operation, serviceError("Unexpected analysis service exception."));
            }
            {
                std::scoped_lock lock(mutex_);
                if (active_ == operation) {
                    active_.reset();
                }
            }
        }
    }

    void shutdown() noexcept {
        std::deque<std::shared_ptr<AnalysisOperation>> queued;
        std::shared_ptr<AnalysisOperation> active;
        {
            std::scoped_lock lock(mutex_);
            if (closed_) {
                return;
            }
            closed_ = true;
            queued = std::move(queue_);
            active = active_;
            if (active) {
                active->canceled.store(true, std::memory_order_release);
                for (internal::SignatureDecodeSession* const decoder : activeDecoders_) {
                    decoder->requestInterrupt();
                }
            }
        }
        for (const auto& operation : queued) {
            postCanceled(*operation, application::CancellationReason::Shutdown);
        }
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    std::size_t queueCapacity_;
    internal::SignatureCache signatureCache_;
    std::atomic<std::uint64_t> decodedSignatureCount_ = 0U;
    std::atomic<std::uint64_t> openSessionCount_ = 0U;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::shared_ptr<AnalysisOperation>> queue_;
    std::shared_ptr<AnalysisOperation> active_;
    std::vector<internal::SignatureDecodeSession*> activeDecoders_;
    bool closed_ = false;
    std::thread worker_;
};

AlignmentAnalysisService::AlignmentAnalysisService(const std::size_t queueCapacity)
    : impl_(std::make_unique<Impl>(queueCapacity)) {}

AlignmentAnalysisService::~AlignmentAnalysisService() = default;

application::PortSubmitResult
AlignmentAnalysisService::submit(const application::AlignmentEstimateRequest& request,
                                 std::shared_ptr<application::IApplicationEventSink> events) {
    return impl_->submit(AnalysisRequest{request}, events);
}

application::PortSubmitResult
AlignmentAnalysisService::submit(const application::SequenceAlignmentRequest& request,
                                 std::shared_ptr<application::IApplicationEventSink> events) {
    return impl_->submit(AnalysisRequest{request}, events);
}

void AlignmentAnalysisService::cancel(
    const application::AlignmentAnalysisJobId jobIdValue) noexcept {
    impl_->cancel(jobIdValue);
}

std::uint64_t AlignmentAnalysisService::decodedSignatureCountForTesting() const noexcept {
    return impl_->decodedSignatureCount();
}

std::uint64_t AlignmentAnalysisService::openSessionCountForTesting() const noexcept {
    return impl_->openSessionCount();
}

} // namespace dvs::media
