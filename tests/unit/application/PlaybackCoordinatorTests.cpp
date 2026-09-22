#include "dvs/application/PlaybackCoordinator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace dvs::application {
namespace {

using namespace std::chrono_literals;

class TestFrameResource final : public IFrameResource {};

class FakeMediaProbe final : public IMediaProbe {
public:
    [[nodiscard]] PortSubmitResult submit(const MediaProbeRequest& request,
                                          std::shared_ptr<IApplicationEventSink> events) override {
        if (!events) {
            return PortSubmitResult::Closed;
        }
        PortSubmitResult result = PortSubmitResult::Accepted;
        {
            std::scoped_lock lock(mutex_);
            const auto iterator = submitResults_.find(request.sourceId);
            if (iterator != submitResults_.end()) {
                result = iterator->second;
            }
            if (result == PortSubmitResult::Accepted) {
                requests_.push_back(CapturedRequest{
                    .request = request,
                    .events = events,
                });
            }
        }
        condition_.notify_all();
        return result;
    }

    void cancel(const RequestContext& context) noexcept override {
        {
            std::scoped_lock lock(mutex_);
            canceled_.push_back(context);
        }
        condition_.notify_all();
    }

    void setSubmitResult(const domain::SourceId sourceId, const PortSubmitResult result) {
        std::scoped_lock lock(mutex_);
        submitResults_[sourceId] = result;
    }

    [[nodiscard]] bool waitForRequestCount(const std::size_t count) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 5s, [this, count] { return requests_.size() >= count; });
    }

    [[nodiscard]] bool waitForCancelCount(const std::size_t count) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 5s, [this, count] { return canceled_.size() >= count; });
    }

    [[nodiscard]] std::optional<MediaProbeRequest> request(const std::size_t index) const {
        std::scoped_lock lock(mutex_);
        if (index >= requests_.size()) {
            return std::nullopt;
        }
        return requests_[index].request;
    }

    [[nodiscard]] std::vector<RequestContext> canceledContexts() const {
        std::scoped_lock lock(mutex_);
        return canceled_;
    }

    [[nodiscard]] std::shared_ptr<IApplicationEventSink>
    lockEventSink(const std::size_t index) const {
        const std::optional<CapturedRequest> captured = capturedRequest(index);
        return captured.has_value() ? captured->events.lock() : nullptr;
    }

    [[nodiscard]] bool postCompleted(const std::size_t index,
                                     domain::MediaDescriptor descriptor) const {
        const std::optional<CapturedRequest> captured = capturedRequest(index);
        const std::shared_ptr<IApplicationEventSink> events =
            captured.has_value() ? captured->events.lock() : nullptr;
        return events && events->postCritical(ApplicationEvent{ProbeCompleted{
                             .context = captured->request.context,
                             .sourceId = captured->request.sourceId,
                             .descriptor = std::move(descriptor),
                         }}) == EventPostResult::Accepted;
    }

    [[nodiscard]] bool postSucceeded(const std::size_t index) const {
        const std::optional<CapturedRequest> captured = capturedRequest(index);
        const std::shared_ptr<IApplicationEventSink> events =
            captured.has_value() ? captured->events.lock() : nullptr;
        return events && events->postCritical(ApplicationEvent{RequestTerminal{RequestSucceeded{
                             .context = EventContext{captured->request.context},
                         }}}) == EventPostResult::Accepted;
    }

    [[nodiscard]] bool postFailed(const std::size_t index, domain::MediaError error) const {
        const std::optional<CapturedRequest> captured = capturedRequest(index);
        const std::shared_ptr<IApplicationEventSink> events =
            captured.has_value() ? captured->events.lock() : nullptr;
        return events && events->postCritical(ApplicationEvent{RequestTerminal{RequestFailed{
                             .context = EventContext{captured->request.context},
                             .error = std::move(error),
                         }}}) == EventPostResult::Accepted;
    }

private:
    struct CapturedRequest final {
        MediaProbeRequest request;
        std::weak_ptr<IApplicationEventSink> events;
    };

    [[nodiscard]] std::optional<CapturedRequest> capturedRequest(const std::size_t index) const {
        std::scoped_lock lock(mutex_);
        if (index >= requests_.size()) {
            return std::nullopt;
        }
        return requests_[index];
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<CapturedRequest> requests_;
    std::vector<RequestContext> canceled_;
    std::map<domain::SourceId, PortSubmitResult> submitResults_;
};

class FakeDeadlineScheduler final : public IDeadlineScheduler {
public:
    [[nodiscard]] PortSubmitResult
    schedule(const DeadlineRequest& request,
             std::shared_ptr<IApplicationEventSink> events) override {
        if (!events) {
            return PortSubmitResult::Closed;
        }
        {
            std::scoped_lock lock(mutex_);
            scheduled_.push_back(ScheduledDeadline{
                .request = request,
                .events = events,
            });
        }
        condition_.notify_all();
        return submitResult_;
    }

    [[nodiscard]] bool cancel(const std::uint64_t timerId) noexcept override {
        {
            std::scoped_lock lock(mutex_);
            canceled_.push_back(timerId);
        }
        condition_.notify_all();
        return cancelResult_;
    }

    [[nodiscard]] bool waitForScheduleCount(const std::size_t count) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 5s, [this, count] { return scheduled_.size() >= count; });
    }

    [[nodiscard]] bool waitForCancelCount(const std::size_t count) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 5s, [this, count] { return canceled_.size() >= count; });
    }

    [[nodiscard]] std::optional<DeadlineRequest> request(const std::size_t index) const {
        std::scoped_lock lock(mutex_);
        if (index >= scheduled_.size()) {
            return std::nullopt;
        }
        return scheduled_[index].request;
    }

    [[nodiscard]] std::size_t scheduleCount() const {
        std::scoped_lock lock(mutex_);
        return scheduled_.size();
    }

    [[nodiscard]] std::size_t cancelCount() const {
        std::scoped_lock lock(mutex_);
        return canceled_.size();
    }

    [[nodiscard]] bool fire(const std::size_t index) const {
        const std::optional<ScheduledDeadline> scheduled = deadline(index);
        const auto events = scheduled.has_value() ? scheduled->events.lock() : nullptr;
        return events && events->postCritical(ApplicationEvent{DeadlineElapsed{
                             .context = scheduled->request.context,
                             .timerId = scheduled->request.timerId,
                         }}) == EventPostResult::Accepted;
    }

private:
    struct ScheduledDeadline final {
        DeadlineRequest request;
        std::weak_ptr<IApplicationEventSink> events;
    };

    [[nodiscard]] std::optional<ScheduledDeadline> deadline(const std::size_t index) const {
        std::scoped_lock lock(mutex_);
        if (index >= scheduled_.size()) {
            return std::nullopt;
        }
        return scheduled_[index];
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<ScheduledDeadline> scheduled_;
    std::vector<std::uint64_t> canceled_;
    PortSubmitResult submitResult_ = PortSubmitResult::Accepted;
    bool cancelResult_ = true;
};

class FakeSteadyClock final : public ISteadyClock {
public:
    explicit FakeSteadyClock(const std::chrono::steady_clock::time_point now =
                                 std::chrono::steady_clock::time_point{std::chrono::seconds{123}})
        : now_(now) {}

    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override {
        std::scoped_lock lock(mutex_);
        return now_;
    }

    void set(const std::chrono::steady_clock::time_point now) noexcept {
        std::scoped_lock lock(mutex_);
        now_ = now;
    }

    void advance(const std::chrono::steady_clock::duration amount) noexcept {
        std::scoped_lock lock(mutex_);
        now_ += amount;
    }

private:
    mutable std::mutex mutex_;
    std::chrono::steady_clock::time_point now_;
};

class FakeFrameProvider final : public IFrameProvider {
public:
    [[nodiscard]] PortSubmitResult submit(const FrameProviderOpenRequest& request,
                                          std::shared_ptr<IApplicationEventSink> events) override {
        if (!events) {
            return PortSubmitResult::Closed;
        }
        {
            std::scoped_lock lock(mutex_);
            openRequests_.push_back(request);
            events_ = std::move(events);
        }
        condition_.notify_all();
        return PortSubmitResult::Accepted;
    }

    [[nodiscard]] PortSubmitResult submit(const FrameRequest& request,
                                          std::shared_ptr<IApplicationEventSink> events) override {
        if (!events) {
            return PortSubmitResult::Closed;
        }
        {
            std::scoped_lock lock(mutex_);
            if (std::find(rejectedFrameIds_.begin(), rejectedFrameIds_.end(), request.frameId) !=
                rejectedFrameIds_.end()) {
                return PortSubmitResult::Closed;
            }
            if (request.priority == FrameRequestPriority::Prefetch) {
                prefetchRequests_.push_back(request);
            } else {
                frameRequests_.push_back(request);
            }
            events_ = std::move(events);
        }
        condition_.notify_all();
        return PortSubmitResult::Accepted;
    }

    [[nodiscard]] PortSubmitResult submit(const FrameProviderCloseRequest& request,
                                          std::shared_ptr<IApplicationEventSink> events) override {
        if (!events) {
            return PortSubmitResult::Closed;
        }
        {
            std::scoped_lock lock(mutex_);
            closeRequests_.push_back(request);
            events_ = std::move(events);
        }
        condition_.notify_all();
        return PortSubmitResult::Accepted;
    }

    void cancel(const PlaybackRequestContext& context) noexcept override {
        {
            std::scoped_lock lock(mutex_);
            canceledContexts_.push_back(context);
        }
        condition_.notify_all();
    }

    // Test seam: any frame request whose frameId is in rejectedFrameIds_ is refused with Closed
    // instead of Accepted, simulating a transient provider rejection / backpressure. Returns the
    // previous set so a test can restore it.
    std::vector<domain::FrameId> setRejectedFrameIds(std::vector<domain::FrameId> frameIds) {
        std::scoped_lock lock(mutex_);
        std::vector<domain::FrameId> previous = std::move(rejectedFrameIds_);
        rejectedFrameIds_ = std::move(frameIds);
        return previous;
    }

    [[nodiscard]] bool waitForOpenRequestCount(const std::size_t count) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(
            lock, 5s, [this, count] { return openRequests_.size() >= count; });
    }

    [[nodiscard]] bool waitForFrameRequestCount(const std::size_t count) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(
            lock, 5s, [this, count] { return frameRequests_.size() >= count; });
    }

    [[nodiscard]] bool waitForPrefetchRequestCount(const std::size_t count) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(
            lock, 5s, [this, count] { return prefetchRequests_.size() >= count; });
    }

    [[nodiscard]] bool waitForCloseRequestCount(const std::size_t count) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(
            lock, 5s, [this, count] { return closeRequests_.size() >= count; });
    }

    [[nodiscard]] bool waitForCancelCount(const std::size_t count) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(
            lock, 5s, [this, count] { return canceledContexts_.size() >= count; });
    }

    [[nodiscard]] std::optional<FrameProviderOpenRequest>
    openRequest(const std::size_t index = 0U) const {
        std::scoped_lock lock(mutex_);
        if (index >= openRequests_.size()) {
            return std::nullopt;
        }
        return openRequests_[index];
    }

    [[nodiscard]] std::optional<FrameRequest> frameRequest(const std::size_t index) const {
        std::scoped_lock lock(mutex_);
        if (index >= frameRequests_.size()) {
            return std::nullopt;
        }
        return frameRequests_[index];
    }

    [[nodiscard]] std::optional<FrameRequest> prefetchRequest(const std::size_t index) const {
        std::scoped_lock lock(mutex_);
        if (index >= prefetchRequests_.size()) {
            return std::nullopt;
        }
        return prefetchRequests_[index];
    }

    [[nodiscard]] std::optional<FrameProviderCloseRequest>
    closeRequest(const std::size_t index = 0U) const {
        std::scoped_lock lock(mutex_);
        if (index >= closeRequests_.size()) {
            return std::nullopt;
        }
        return closeRequests_[index];
    }

    [[nodiscard]] std::vector<PlaybackRequestContext> canceledContexts() const {
        std::scoped_lock lock(mutex_);
        return canceledContexts_;
    }

    [[nodiscard]] std::size_t frameRequestCount() const {
        std::scoped_lock lock(mutex_);
        return frameRequests_.size();
    }

    [[nodiscard]] std::size_t prefetchRequestCount() const {
        std::scoped_lock lock(mutex_);
        return prefetchRequests_.size();
    }

    [[nodiscard]] std::size_t openRequestCount() const {
        std::scoped_lock lock(mutex_);
        return openRequests_.size();
    }

    [[nodiscard]] bool postOpenSucceeded(const FrameProviderOpenRequest& request) const {
        const std::shared_ptr<IApplicationEventSink> events = eventSink();
        return events && events->postCritical(ApplicationEvent{RequestTerminal{RequestSucceeded{
                             .context = EventContext{request.context},
                         }}}) == EventPostResult::Accepted;
    }

    [[nodiscard]] bool postOpenFailed(const FrameProviderOpenRequest& request,
                                      domain::MediaError error) const {
        const std::shared_ptr<IApplicationEventSink> events = eventSink();
        return events && events->postCritical(ApplicationEvent{RequestTerminal{RequestFailed{
                             .context = EventContext{request.context},
                             .error = std::move(error),
                         }}}) == EventPostResult::Accepted;
    }

    [[nodiscard]] bool postFrameReady(const FrameRequest& request, FrameSet set) const {
        const std::shared_ptr<IApplicationEventSink> events = eventSink();
        return events && events->postCritical(ApplicationEvent{FrameSetReady{
                             .context = request.context,
                             .set = std::move(set),
                         }}) == EventPostResult::Accepted;
    }

    [[nodiscard]] bool postFrameSucceeded(const FrameRequest& request) const {
        const std::shared_ptr<IApplicationEventSink> events = eventSink();
        return events && events->postCritical(ApplicationEvent{RequestTerminal{RequestSucceeded{
                             .context = EventContext{request.context},
                         }}}) == EventPostResult::Accepted;
    }

    [[nodiscard]] bool postFrameFailed(const FrameRequest& request,
                                       domain::MediaError error) const {
        const std::shared_ptr<IApplicationEventSink> events = eventSink();
        return events && events->postCritical(ApplicationEvent{RequestTerminal{RequestFailed{
                             .context = EventContext{request.context},
                             .error = std::move(error),
                         }}}) == EventPostResult::Accepted;
    }

    [[nodiscard]] bool postCloseSucceeded(const FrameProviderCloseRequest& request) const {
        const std::shared_ptr<IApplicationEventSink> events = eventSink();
        return events && events->postCritical(ApplicationEvent{RequestTerminal{RequestSucceeded{
                             .context = EventContext{request.context},
                         }}}) == EventPostResult::Accepted;
    }

private:
    [[nodiscard]] std::shared_ptr<IApplicationEventSink> eventSink() const {
        std::scoped_lock lock(mutex_);
        return events_.lock();
    }

    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    std::weak_ptr<IApplicationEventSink> events_;
    std::vector<FrameProviderOpenRequest> openRequests_;
    std::vector<FrameRequest> frameRequests_;
    std::vector<FrameRequest> prefetchRequests_;
    std::vector<FrameProviderCloseRequest> closeRequests_;
    std::vector<PlaybackRequestContext> canceledContexts_;
    std::vector<domain::FrameId> rejectedFrameIds_;
};

class FakeAlignmentAnalysisService final : public IAlignmentAnalysisService {
public:
    [[nodiscard]] PortSubmitResult submit(const AlignmentEstimateRequest& request,
                                          std::shared_ptr<IApplicationEventSink> events) override {
        {
            std::scoped_lock lock(mutex_);
            alignmentRequests_.push_back(request);
            events_ = std::move(events);
        }
        condition_.notify_all();
        return PortSubmitResult::Accepted;
    }

    [[nodiscard]] PortSubmitResult submit(const SequenceAlignmentRequest& request,
                                          std::shared_ptr<IApplicationEventSink> events) override {
        {
            std::scoped_lock lock(mutex_);
            sequenceRequests_.push_back(request);
            events_ = std::move(events);
        }
        condition_.notify_all();
        return PortSubmitResult::Accepted;
    }

    void cancel(const AlignmentAnalysisJobId jobId) noexcept override {
        {
            std::scoped_lock lock(mutex_);
            canceled_.push_back(jobId);
        }
        condition_.notify_all();
    }

    [[nodiscard]] bool waitForSequenceRequestCount(const std::size_t count) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(
            lock, 5s, [this, count] { return sequenceRequests_.size() >= count; });
    }

    [[nodiscard]] bool waitForAlignmentRequestCount(const std::size_t count) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(
            lock, 5s, [this, count] { return alignmentRequests_.size() >= count; });
    }

    [[nodiscard]] std::optional<AlignmentEstimateRequest> alignmentRequest() const {
        std::scoped_lock lock(mutex_);
        return alignmentRequests_.empty() ? std::nullopt : std::optional{alignmentRequests_.back()};
    }

    [[nodiscard]] std::optional<SequenceAlignmentRequest> sequenceRequest() const {
        std::scoped_lock lock(mutex_);
        return sequenceRequests_.empty() ? std::nullopt : std::optional{sequenceRequests_.back()};
    }

    [[nodiscard]] bool waitForCancelCount(const std::size_t count) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 5s, [this, count] { return canceled_.size() >= count; });
    }

    [[nodiscard]] bool postStarted(const SequenceAlignmentRequest& request,
                                   const std::uint64_t totalUnits) {
        const std::shared_ptr<IApplicationEventSink> sink = events();
        return sink && sink->postCritical(ApplicationEvent{AlignmentAnalysisStarted{
                           .jobId = request.jobId,
                           .context = request.context,
                           .kind = AlignmentAnalysisKind::Sequence,
                           .work =
                               AlignmentWorkEstimate{
                                   .totalUnits = totalUnits,
                                   .unitName = "work units",
                               },
                       }}) == EventPostResult::Accepted;
    }

    [[nodiscard]] bool postProgress(const SequenceAlignmentRequest& request,
                                    const std::uint64_t completedUnits,
                                    const std::uint64_t totalUnits) {
        const std::shared_ptr<IApplicationEventSink> sink = events();
        return sink && sink->postRealtime(ApplicationEvent{AlignmentAnalysisProgress{
                           .jobId = request.jobId,
                           .context = request.context,
                           .kind = AlignmentAnalysisKind::Sequence,
                           .phase = AlignmentAnalysisPhase::CollectingSignatures,
                           .completedUnits = completedUnits,
                           .work =
                               AlignmentWorkEstimate{
                                   .totalUnits = totalUnits,
                                   .unitName = "work units",
                               },
                       }}) == EventPostResult::Accepted;
    }

    [[nodiscard]] bool postCompleted(const AlignmentEstimateRequest& request,
                                     std::vector<GlobalOffsetEstimate> estimates) {
        const std::shared_ptr<IApplicationEventSink> sink = events();
        return sink && sink->postCritical(ApplicationEvent{AlignmentAnalysisCompleted{
                           .jobId = request.jobId,
                           .context = request.context,
                           .kind = AlignmentAnalysisKind::GlobalOffset,
                           .estimates = std::move(estimates),
                       }}) == EventPostResult::Accepted;
    }

    [[nodiscard]] bool postCompleted(const SequenceAlignmentRequest& request,
                                     std::vector<SequenceAlignmentResult> results) {
        const std::shared_ptr<IApplicationEventSink> sink = events();
        return sink && sink->postCritical(ApplicationEvent{AlignmentAnalysisCompleted{
                           .jobId = request.jobId,
                           .context = request.context,
                           .kind = AlignmentAnalysisKind::Sequence,
                           .sequenceResults = std::move(results),
                       }}) == EventPostResult::Accepted;
    }

private:
    [[nodiscard]] std::shared_ptr<IApplicationEventSink> events() const {
        std::scoped_lock lock(mutex_);
        return events_;
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<AlignmentEstimateRequest> alignmentRequests_;
    std::vector<SequenceAlignmentRequest> sequenceRequests_;
    std::vector<AlignmentAnalysisJobId> canceled_;
    std::shared_ptr<IApplicationEventSink> events_;
};

class FakeRenderChannel final : public IRenderChannel {
public:
    [[nodiscard]] RenderPublishResult publish(const FrameRequestContext& context,
                                              FrameSet set) noexcept override {
        {
            std::scoped_lock lock(mutex_);
            published_.push_back(PublishedSet{
                .context = context,
                .set = std::move(set),
            });
        }
        condition_.notify_all();
        return RenderPublishResult::Accepted;
    }

    void clear(const PlaybackRequestContext& context) noexcept override {
        {
            std::scoped_lock lock(mutex_);
            clearContexts_.push_back(context);
            for (PublishedSet& published : published_) {
                if (samePlaybackScope(published.context.playback, context)) {
                    published.invalidated = true;
                }
            }
        }
        condition_.notify_all();
    }

    [[nodiscard]] bool waitForPublishedCount(const std::size_t count) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 5s, [this, count] { return published_.size() >= count; });
    }

    [[nodiscard]] bool waitForClearCount(const std::size_t count) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(
            lock, 5s, [this, count] { return clearContexts_.size() >= count; });
    }

    [[nodiscard]] std::vector<PlaybackRequestContext> clearContexts() const {
        std::scoped_lock lock(mutex_);
        return clearContexts_;
    }

    [[nodiscard]] std::size_t publishedCount() const {
        std::scoped_lock lock(mutex_);
        return published_.size();
    }

    [[nodiscard]] std::optional<FrameSetPresented> presentation(const std::size_t index) const {
        std::scoped_lock lock(mutex_);
        if (index >= published_.size()) {
            return std::nullopt;
        }
        return FrameSetPresented{
            .context = published_[index].context,
            .frameId = published_[index].set.canonicalFrameId(),
        };
    }

    [[nodiscard]] std::optional<FrameSetPresented> consumePresentation(const std::size_t index) {
        std::scoped_lock lock(mutex_);
        if (index >= published_.size() || published_[index].invalidated ||
            published_[index].consumed) {
            return std::nullopt;
        }
        published_[index].consumed = true;
        return FrameSetPresented{
            .context = published_[index].context,
            .frameId = published_[index].set.canonicalFrameId(),
        };
    }

private:
    [[nodiscard]] static bool samePlaybackScope(const PlaybackRequestContext& lhs,
                                                const PlaybackRequestContext& rhs) noexcept {
        return lhs.request.sessionId == rhs.request.sessionId &&
               lhs.request.sessionEpoch == rhs.request.sessionEpoch &&
               lhs.playbackGeneration == rhs.playbackGeneration;
    }

    struct PublishedSet final {
        FrameRequestContext context;
        FrameSet set;
        bool invalidated = false;
        bool consumed = false;
    };

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<PublishedSet> published_;
    std::vector<PlaybackRequestContext> clearContexts_;
};

class IdentityOnlyRenderChannel final : public IRenderChannel {
public:
    [[nodiscard]] RenderPublishResult publish(const FrameRequestContext& context,
                                              FrameSet set) noexcept override {
        {
            std::scoped_lock lock(mutex_);
            published_.push_back(PublishedIdentity{
                .context = context,
                .frameId = set.canonicalFrameId(),
            });
        }
        condition_.notify_all();
        return RenderPublishResult::Accepted;
    }

    void clear(const PlaybackRequestContext& context) noexcept override {
        std::scoped_lock lock(mutex_);
        for (PublishedIdentity& published : published_) {
            if (published.context.playback == context) {
                published.invalidated = true;
            }
        }
    }

    [[nodiscard]] bool waitForPublishedCount(const std::size_t count) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 5s, [this, count] { return published_.size() >= count; });
    }

    [[nodiscard]] std::optional<FrameSetPresented> presentation(const std::size_t index) const {
        std::scoped_lock lock(mutex_);
        if (index >= published_.size() || published_[index].invalidated) {
            return std::nullopt;
        }
        return FrameSetPresented{
            .context = published_[index].context,
            .frameId = published_[index].frameId,
        };
    }

private:
    struct PublishedIdentity final {
        FrameRequestContext context;
        domain::FrameId frameId;
        bool invalidated = false;
    };

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<PublishedIdentity> published_;
};

class BlockingRenderChannel final : public IRenderChannel {
public:
    [[nodiscard]] RenderPublishResult publish(const FrameRequestContext&,
                                              FrameSet) noexcept override {
        std::unique_lock lock(mutex_);
        publishBlocked_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
        return RenderPublishResult::Accepted;
    }

    void clear(const PlaybackRequestContext&) noexcept override {}

    [[nodiscard]] bool waitUntilPublishBlocked() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 5s, [this] { return publishBlocked_; });
    }

    void release() {
        {
            std::scoped_lock lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool publishBlocked_ = false;
    bool released_ = false;
};

[[nodiscard]] domain::RationalRate makeRate(const std::int64_t framesPerSecond = 30) {
    auto rate = domain::RationalRate::create(framesPerSecond, 1);
    EXPECT_TRUE(rate.hasValue());
    return std::move(rate).value();
}

[[nodiscard]] domain::MediaDescriptor makeDescriptor(const std::filesystem::path& path,
                                                     const domain::MediaExtent extent,
                                                     const std::int64_t frameCount = 12,
                                                     const std::int64_t framesPerSecond = 30) {
    return domain::MediaDescriptor{
        .normalizedPath = path,
        .extent = extent,
        .frameRate = makeRate(framesPerSecond),
        .frameCount =
            domain::FrameCountInfo{
                .value = frameCount,
                .origin = domain::FrameCountOrigin::kReported,
            },
        .duration = domain::MediaTime{frameCount * 1'000'000 / framesPerSecond},
        .codecId = "h264",
        .pixelFormatId = "yuv420p",
        .bitDepth = 8,
        .decodeCapabilities =
            domain::DecodeCapabilities{
                .softwareDecode = true,
                .d3d11VaDecode = false,
            },
        .timingConfidence = domain::TimingConfidence::kDeclaredCfr,
        .sourceIdentity = std::nullopt,
    };
}

[[nodiscard]] domain::MediaDescriptor makeVfrDescriptor(const std::filesystem::path& path,
                                                        const domain::MediaExtent extent,
                                                        const std::int64_t frameCount,
                                                        const domain::MediaTime duration) {
    return domain::MediaDescriptor{
        .normalizedPath = path,
        .extent = extent,
        .frameRate = std::nullopt,
        .frameCount =
            domain::FrameCountInfo{
                .value = frameCount,
                .origin = domain::FrameCountOrigin::kIndexed,
            },
        .duration = duration,
        .codecId = "h264",
        .pixelFormatId = "yuv420p",
        .bitDepth = 8,
        .decodeCapabilities =
            domain::DecodeCapabilities{
                .softwareDecode = true,
                .d3d11VaDecode = false,
            },
        .timingConfidence = domain::TimingConfidence::kVariableFrameRate,
        .sourceIdentity = std::nullopt,
    };
}

[[nodiscard]] std::shared_ptr<const domain::FrameTimeline> makeVfrTimeline() {
    const auto result = domain::FrameTimeline::create({domain::MediaTime{0},
                                                       domain::MediaTime{10000},
                                                       domain::MediaTime{50000},
                                                       domain::MediaTime{69800}});
    if (!result.hasValue()) {
        return nullptr;
    }
    return std::make_shared<const domain::FrameTimeline>(std::move(result).value());
}

[[nodiscard]] FrameSet makeFrameSet(const domain::FrameId frameId) {
    const FrameGeometry geometryA{
        .width = 320,
        .height = 180,
        .textureRegion = TextureRegion{},
    };
    const FrameGeometry geometryB{
        .width = 160,
        .height = 90,
        .textureRegion = TextureRegion{},
    };
    const auto frameA =
        FrameHandle::create(std::make_shared<const TestFrameResource>(), geometryA, 100);
    const auto frameB =
        FrameHandle::create(std::make_shared<const TestFrameResource>(), geometryB, 100);
    EXPECT_TRUE(frameA.has_value());
    EXPECT_TRUE(frameB.has_value());
    std::vector<MappedSourceFrame> sources;
    sources.push_back(MappedSourceFrame{
        .sourceId = 0,
        .sourceFrameId = frameId,
        .frame = *frameA,
        .presentationTime = domain::MediaTime{frameId.value() * 33'333},
        .matchKind = FrameMatchKind::ExactIndex,
    });
    sources.push_back(MappedSourceFrame{
        .sourceId = 1,
        .sourceFrameId = frameId,
        .frame = *frameB,
        .presentationTime = domain::MediaTime{frameId.value() * 33'333},
        .matchKind = FrameMatchKind::ExactIndex,
    });
    auto set =
        FrameSet::create(frameId, domain::MediaTime{frameId.value() * 33'333}, std::move(sources));
    EXPECT_TRUE(set.has_value());
    return *set;
}

[[nodiscard]] SequenceAlignmentResult
makeMissingFrameSequenceResult(const bool autoApplicable = true) {
    SequenceAlignmentResult result{
        .sourceId = 1U,
        .anomalies =
            {
                SequenceAlignmentAnomaly{
                    .kind = SequenceAlignmentAnomalyKind::TargetFrameMissing,
                    .canonicalFrameId = domain::FrameId{4},
                    .sourceFrameId = std::nullopt,
                },
            },
        .segments =
            {
                SequenceAlignmentSegment{
                    .firstCanonicalFrame = domain::FrameId{0},
                    .lastCanonicalFrame = domain::FrameId{11},
                    .state = autoApplicable ? AlignmentSegmentState::ReviewRequired
                                            : AlignmentSegmentState::Rejected,
                    .meanConfidence = autoApplicable ? 0.82F : 0.08F,
                    .p10Confidence = autoApplicable ? 0.40F : 0.02F,
                    .maximumLowConfidenceRun = autoApplicable ? 1U : 12U,
                    .anomalyDensity = 1.0F / 12.0F,
                    .mappingSlope = 1.0F,
                },
            },
        .totalCost = 0.18F,
        .meanMatchCost = 0.02F,
        .confidence = autoApplicable ? 0.82F : 0.08F,
        .autoApplicable = autoApplicable,
    };
    result.entries.reserve(12U);
    for (std::int64_t frame = 0; frame < 12; ++frame) {
        if (frame == 4) {
            result.entries.push_back(SequenceAlignmentEntry{
                .canonicalFrameId = domain::FrameId{frame},
                .sourceFrameId = std::nullopt,
                .matchKind = FrameMatchKind::Missing,
                .confidence = 0.0F,
            });
        } else {
            result.entries.push_back(SequenceAlignmentEntry{
                .canonicalFrameId = domain::FrameId{frame},
                .sourceFrameId = domain::FrameId{frame < 4 ? frame : frame - 1},
                .matchKind = FrameMatchKind::AutoAligned,
                .confidence = autoApplicable ? 0.90F : 0.08F,
            });
        }
    }
    return result;
}

struct ObservableFrameSet final {
    FrameSet set;
    std::weak_ptr<const IFrameResource> source0;
    std::weak_ptr<const IFrameResource> source1;
};

[[nodiscard]] ObservableFrameSet makeObservableFrameSet(const domain::FrameId frameId) {
    const FrameGeometry geometryA{
        .width = 320,
        .height = 180,
        .textureRegion = TextureRegion{},
    };
    const FrameGeometry geometryB{
        .width = 160,
        .height = 90,
        .textureRegion = TextureRegion{},
    };
    const auto resourceA = std::make_shared<const TestFrameResource>();
    const auto resourceB = std::make_shared<const TestFrameResource>();
    const auto frameA = FrameHandle::create(resourceA, geometryA, 100);
    const auto frameB = FrameHandle::create(resourceB, geometryB, 100);
    EXPECT_TRUE(frameA.has_value());
    EXPECT_TRUE(frameB.has_value());
    std::vector<MappedSourceFrame> sources;
    sources.push_back(MappedSourceFrame{
        .sourceId = 0,
        .sourceFrameId = frameId,
        .frame = *frameA,
        .presentationTime = domain::MediaTime{frameId.value() * 33'333},
        .matchKind = FrameMatchKind::ExactIndex,
    });
    sources.push_back(MappedSourceFrame{
        .sourceId = 1,
        .sourceFrameId = frameId,
        .frame = *frameB,
        .presentationTime = domain::MediaTime{frameId.value() * 33'333},
        .matchKind = FrameMatchKind::ExactIndex,
    });
    auto set =
        FrameSet::create(frameId, domain::MediaTime{frameId.value() * 33'333}, std::move(sources));
    EXPECT_TRUE(set.has_value());
    return ObservableFrameSet{
        .set = std::move(*set),
        .source0 = resourceA,
        .source1 = resourceB,
    };
}

[[nodiscard]] ApplicationEvent ignoredCriticalEvent() {
    return ApplicationEvent{DeadlineElapsed{
        .context =
            PlaybackRequestContext{
                .request =
                    RequestContext{
                        .sessionId = domain::SessionId{1},
                        .sessionEpoch = domain::SessionEpoch{1},
                        .requestId = domain::RequestId{1},
                    },
                .playbackGeneration = domain::PlaybackGeneration{1},
            },
        .timerId = 1U,
    }};
}

template <typename Predicate> [[nodiscard]] bool waitUntil(Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

[[nodiscard]] std::vector<CommandTerminal>
waitForTerminals(const std::shared_ptr<PlaybackCoordinator>& coordinator, const std::size_t count) {
    std::vector<CommandTerminal> terminals;
    const bool completed = waitUntil([&coordinator, &terminals, count] {
        std::vector<CommandTerminal> next = coordinator->takeCompletedCommands();
        terminals.insert(terminals.end(), next.begin(), next.end());
        return terminals.size() >= count;
    });
    EXPECT_TRUE(completed);
    return terminals;
}

void presentPublished(const std::shared_ptr<PlaybackCoordinator>& coordinator,
                      const std::shared_ptr<FakeRenderChannel>& render,
                      const std::size_t index) {
    const auto presented = render->consumePresentation(index);
    ASSERT_TRUE(presented.has_value());
    ASSERT_EQ(coordinator->postCritical(ApplicationEvent{*presented}), EventPostResult::Accepted);
}

void openReady(const std::shared_ptr<PlaybackCoordinator>& coordinator,
               const std::shared_ptr<FakeFrameProvider>& provider,
               const std::shared_ptr<FakeRenderChannel>& render,
               const domain::CommandId commandId = domain::CommandId{1},
               const std::int64_t secondFrameCount = 12,
               const std::int64_t framesPerSecond = 30) {
    const std::shared_ptr<const SessionSnapshot> initial = coordinator->snapshot();
    ASSERT_NE(initial, nullptr);
    ASSERT_EQ(coordinator->submit(OpenDirectComparisonCommand{
                  .context =
                      CommandContext{
                          .sessionId = initial->sessionId,
                          .sessionEpoch = initial->sessionEpoch,
                          .commandId = commandId,
                      },
                  .sources =
                      {
                          domain::ComparisonSource{
                              .id = 0,
                              .role = domain::ComparisonRole::kPrediction,
                              .descriptor =
                                  makeDescriptor("a.mp4",
                                                 domain::MediaExtent{.width = 320, .height = 180},
                                                 12,
                                                 framesPerSecond),
                              .displayName = "a",
                          },
                          domain::ComparisonSource{
                              .id = 1,
                              .role = domain::ComparisonRole::kPrediction,
                              .descriptor =
                                  makeDescriptor("b.mp4",
                                                 domain::MediaExtent{.width = 160, .height = 90},
                                                 secondFrameCount,
                                                 framesPerSecond),
                              .displayName = "b",
                          },
                      },
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForOpenRequestCount(1U));
    const std::optional<FrameProviderOpenRequest> open = provider->openRequest();
    ASSERT_TRUE(open.has_value());
    ASSERT_EQ(open->context.request.sessionEpoch, domain::SessionEpoch{1});
    ASSERT_EQ(open->context.playbackGeneration, domain::PlaybackGeneration{1});
    ASSERT_TRUE(provider->postOpenSucceeded(*open));

    ASSERT_TRUE(provider->waitForFrameRequestCount(1U));
    const std::optional<FrameRequest> frame = provider->frameRequest(0U);
    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(frame->frameId, domain::FrameId{0});
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(frame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(1U));
    EXPECT_TRUE(coordinator->takeCompletedCommands().empty());
    ASSERT_TRUE(provider->postFrameSucceeded(*frame));
    EXPECT_TRUE(coordinator->takeCompletedCommands().empty());
    presentPublished(coordinator, render, 0U);

    const std::vector<CommandTerminal> terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().context.commandId, commandId);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);
}

[[nodiscard]] std::shared_ptr<PlaybackCoordinator>
makeCoordinator(const std::shared_ptr<FakeFrameProvider>& provider,
                const std::shared_ptr<FakeRenderChannel>& render,
                std::shared_ptr<IMediaProbe> mediaProbe,
                std::shared_ptr<IDeadlineScheduler> deadlineScheduler,
                std::shared_ptr<ISteadyClock> clock,
                std::shared_ptr<IAlignmentAnalysisService> analysisService = {});
void markGraphicsReady(const std::shared_ptr<PlaybackCoordinator>& coordinator,
                       domain::DeviceGeneration generation);
[[nodiscard]] CommandContext commandContext(const std::shared_ptr<PlaybackCoordinator>& coordinator,
                                            domain::CommandId commandId);

// C-07/D03: production default Contextual is smoothness-first RealTime. Tests that assert
// sequential / every-frame presentation must opt into ReviewEveryFrame explicitly;
// catch-up tests still call requireRealTimeContinuity after this helper.
void requireReviewEveryFrameContinuity(const std::shared_ptr<PlaybackCoordinator>& coordinator,
                                       const domain::CommandId commandId = domain::CommandId{800}) {
    ASSERT_EQ(coordinator->submit(SetPlaybackContinuityPolicyCommand{
                  .context = commandContext(coordinator, commandId),
                  .policy = domain::PlaybackContinuityPolicy::ReviewEveryFrame,
              }),
              PortSubmitResult::Accepted);
    const std::vector<CommandTerminal> terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);
    const auto snapshot = coordinator->snapshot();
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->playbackContinuityPolicyEffective,
              domain::PlaybackContinuityPolicy::ReviewEveryFrame);
}

void requireRealTimeContinuity(const std::shared_ptr<PlaybackCoordinator>& coordinator,
                               const domain::CommandId commandId = domain::CommandId{900}) {
    ASSERT_EQ(coordinator->submit(SetPlaybackContinuityPolicyCommand{
                  .context = commandContext(coordinator, commandId),
                  .policy = domain::PlaybackContinuityPolicy::RealTime,
              }),
              PortSubmitResult::Accepted);
    const std::vector<CommandTerminal> terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);
    const std::shared_ptr<const SessionSnapshot> snapshot = coordinator->snapshot();
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->playbackContinuityPolicy, domain::PlaybackContinuityPolicy::RealTime);
    EXPECT_EQ(snapshot->playbackContinuityPolicyEffective,
              domain::PlaybackContinuityPolicy::RealTime);
}

TEST(PlaybackCoordinatorTests, CarriesNonFirstReferenceIdentityIntoProviderOpen) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider,
                                             render,
                                             std::make_shared<FakeMediaProbe>(),
                                             std::make_shared<FakeDeadlineScheduler>(),
                                             std::make_shared<FakeSteadyClock>());
    const std::shared_ptr<const SessionSnapshot> initial = coordinator->snapshot();
    ASSERT_NE(initial, nullptr);

    ASSERT_EQ(coordinator->submit(OpenDirectComparisonCommand{
                  .context =
                      CommandContext{
                          .sessionId = initial->sessionId,
                          .sessionEpoch = initial->sessionEpoch,
                          .commandId = domain::CommandId{1},
                      },
                  .sources =
                      {
                          domain::ComparisonSource{
                              .id = 0U,
                              .role = domain::ComparisonRole::kPrediction,
                              .descriptor = makeDescriptor(
                                  "a.mp4", domain::MediaExtent{.width = 320, .height = 180}),
                              .displayName = "Prediction",
                          },
                          domain::ComparisonSource{
                              .id = 1U,
                              .role = domain::ComparisonRole::kReference,
                              .descriptor = makeDescriptor(
                                  "b.mp4", domain::MediaExtent{.width = 160, .height = 90}),
                              .displayName = "Reference",
                          },
                      },
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForOpenRequestCount(1U));
    const std::optional<FrameProviderOpenRequest> open = provider->openRequest();
    ASSERT_TRUE(open.has_value());
    EXPECT_EQ(open->canonicalSourceId, 0U);
    ASSERT_EQ(open->sources.size(), 2U);
    EXPECT_EQ(open->sources[0U].id, 0U);
    EXPECT_EQ(open->sources[1U].id, 1U);
    EXPECT_EQ(open->sources[0U].role, domain::ComparisonRole::kPrediction);
    EXPECT_EQ(open->sources[1U].role, domain::ComparisonRole::kReference);
}

TEST(PlaybackCoordinatorTests, SuccessfulExactPresentationSubmitsBoundedPrefetchRequests) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider,
                                             render,
                                             std::make_shared<FakeMediaProbe>(),
                                             std::make_shared<FakeDeadlineScheduler>(),
                                             std::make_shared<FakeSteadyClock>());
    markGraphicsReady(coordinator, domain::DeviceGeneration{1});
    openReady(coordinator, provider, render);

    ASSERT_TRUE(provider->waitForPrefetchRequestCount(3U));
    for (std::size_t index = 0U; index < 3U; ++index) {
        const std::optional<FrameRequest> request = provider->prefetchRequest(index);
        ASSERT_TRUE(request.has_value());
        EXPECT_EQ(request->priority, FrameRequestPriority::Prefetch);
        EXPECT_EQ(request->frameId, domain::FrameId{static_cast<std::int64_t>(index + 1U)});
        EXPECT_EQ(request->alignmentRevision, 0U);
    }
}

TEST(PlaybackCoordinatorTests, HighFrameRateExactPresentationDoesNotStartSpeculativeDecode) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider,
                                             render,
                                             std::make_shared<FakeMediaProbe>(),
                                             std::make_shared<FakeDeadlineScheduler>(),
                                             std::make_shared<FakeSteadyClock>());
    markGraphicsReady(coordinator, domain::DeviceGeneration{1});
    openReady(coordinator, provider, render, domain::CommandId{1}, 12, 120);

    EXPECT_EQ(provider->prefetchRequestCount(), 0U);
}

TEST(PlaybackCoordinatorTests, SixtyFpsExactPresentationPrefetchesOnlyOneSuccessor) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider,
                                             render,
                                             std::make_shared<FakeMediaProbe>(),
                                             std::make_shared<FakeDeadlineScheduler>(),
                                             std::make_shared<FakeSteadyClock>());
    markGraphicsReady(coordinator, domain::DeviceGeneration{1});
    openReady(coordinator, provider, render, domain::CommandId{1}, 12, 60);

    ASSERT_TRUE(provider->waitForPrefetchRequestCount(1U));
    EXPECT_EQ(provider->prefetchRequestCount(), 1U);
    const std::optional<FrameRequest> request = provider->prefetchRequest(0U);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->frameId, domain::FrameId{1});
}

TEST(PlaybackCoordinatorAlignmentTests, AppliesOffsetsWithoutReopeningAndReseeksAtomically) {
    auto provider = std::make_shared<FakeFrameProvider>();
    auto render = std::make_shared<FakeRenderChannel>();
    auto analysis = std::make_shared<FakeAlignmentAnalysisService>();
    const auto coordinator = makeCoordinator(provider,
                                             render,
                                             std::make_shared<FakeMediaProbe>(),
                                             std::make_shared<FakeDeadlineScheduler>(),
                                             std::make_shared<FakeSteadyClock>(),
                                             analysis);
    markGraphicsReady(coordinator, domain::DeviceGeneration{2});
    openReady(coordinator, provider, render);
    ASSERT_EQ(coordinator->snapshot()->compatibilityFindings.size(), 1U);
    EXPECT_EQ(coordinator->snapshot()->compatibilityFindings.front(),
              (CompatibilityFindingView{
                  .severity = domain::CompatibilitySeverity::kWarning,
                  .code = domain::MediaErrorCode::kSourceResolutionMismatch,
                  .sources = {0U, 1U},
              }));

    ASSERT_EQ(coordinator->submit(SetAlignmentOffsetsCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .sourceOffsets = {SourceFrameOffset{.sourceId = 1U, .frames = 2}},
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const std::optional<FrameRequest> aligned = provider->frameRequest(1U);
    ASSERT_TRUE(aligned.has_value());
    EXPECT_EQ(aligned->frameId, domain::FrameId{0});
    ASSERT_EQ(aligned->sourceOffsets.size(), 1U);
    EXPECT_EQ(aligned->sourceOffsets.front(), (SourceFrameOffset{.sourceId = 1U, .frames = 2}));
    EXPECT_EQ(provider->openRequestCount(), 1U);

    ASSERT_TRUE(provider->postFrameReady(*aligned, makeFrameSet(aligned->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    ASSERT_TRUE(provider->postFrameSucceeded(*aligned));
    presentPublished(coordinator, render, 1U);
    const std::vector<CommandTerminal> terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);
}

TEST(PlaybackCoordinatorAlignmentTests, RejectsANonzeroCanonicalOffset) {
    auto provider = std::make_shared<FakeFrameProvider>();
    auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider,
                                             render,
                                             std::make_shared<FakeMediaProbe>(),
                                             std::make_shared<FakeDeadlineScheduler>(),
                                             std::make_shared<FakeSteadyClock>());
    markGraphicsReady(coordinator, domain::DeviceGeneration{2});
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(SetAlignmentOffsetsCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .sourceOffsets = {SourceFrameOffset{.sourceId = 0U, .frames = 1}},
              }),
              PortSubmitResult::Accepted);
    const std::vector<CommandTerminal> terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Failed);
    EXPECT_EQ(provider->frameRequestCount(), 1U);
}

TEST(PlaybackCoordinatorAlignmentTests,
     AppliesHighConfidenceOffsetOnlyAfterConfirmationAndSupportsUndo) {
    auto provider = std::make_shared<FakeFrameProvider>();
    auto render = std::make_shared<FakeRenderChannel>();
    auto analysis = std::make_shared<FakeAlignmentAnalysisService>();
    const auto coordinator = makeCoordinator(provider,
                                             render,
                                             std::make_shared<FakeMediaProbe>(),
                                             std::make_shared<FakeDeadlineScheduler>(),
                                             std::make_shared<FakeSteadyClock>(),
                                             analysis);
    markGraphicsReady(coordinator, domain::DeviceGeneration{2});
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(EstimateAlignmentCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(analysis->waitForAlignmentRequestCount(1U));
    const auto request = analysis->alignmentRequest();
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->canonicalSourceId, 0U);
    ASSERT_TRUE(analysis->postCompleted(*request,
                                        {GlobalOffsetEstimate{
                                            .sourceId = 1U,
                                            .bestOffset = 2,
                                            .bestCost = 0.08F,
                                            .runnerUpCost = 0.22F,
                                            .confidence = 0.64F,
                                            .evidenceCount = 5U,
                                            .autoApplicable = true,
                                        }}));

    const auto analysisTerminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(analysisTerminals.size(), 1U);
    EXPECT_EQ(analysisTerminals.front().outcome, CommandOutcome::Succeeded);
    EXPECT_EQ(provider->frameRequestCount(), 1U);
    EXPECT_TRUE(coordinator->snapshot()->automaticAlignmentPending);
    EXPECT_TRUE(coordinator->snapshot()->canConfirmAutomaticAlignment);

    ASSERT_EQ(coordinator->submit(ConfirmAutomaticAlignmentCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    EXPECT_TRUE(coordinator->takeCompletedCommands().empty());
    const auto aligned = provider->frameRequest(1U);
    ASSERT_TRUE(aligned.has_value());
    ASSERT_EQ(aligned->sourceOffsets.size(), 1U);
    EXPECT_EQ(aligned->sourceOffsets.front().sourceId, 1U);
    EXPECT_EQ(aligned->sourceOffsets.front().frames, 2);
    EXPECT_EQ(aligned->sourceOffsets.front().matchKind, FrameMatchKind::AutoAligned);
    EXPECT_FLOAT_EQ(aligned->sourceOffsets.front().confidence, 0.64F);

    ASSERT_TRUE(provider->postFrameReady(*aligned, makeFrameSet(aligned->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    ASSERT_TRUE(provider->postFrameSucceeded(*aligned));
    presentPublished(coordinator, render, 1U);
    const auto terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);
    ASSERT_EQ(coordinator->snapshot()->alignmentEstimates.size(), 1U);
    EXPECT_TRUE(coordinator->snapshot()->alignmentEstimates.front().autoApplicable);
    EXPECT_FALSE(coordinator->snapshot()->automaticAlignmentPending);
    EXPECT_TRUE(coordinator->snapshot()->canUndoAutomaticAlignment);

    ASSERT_EQ(coordinator->submit(UndoAutomaticAlignmentCommand{
                  .context = commandContext(coordinator, domain::CommandId{4}),
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(3U));
    const auto restored = provider->frameRequest(2U);
    ASSERT_TRUE(restored.has_value());
    EXPECT_TRUE(restored->sourceOffsets.empty());
    ASSERT_TRUE(provider->postFrameReady(*restored, makeFrameSet(restored->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(3U));
    ASSERT_TRUE(provider->postFrameSucceeded(*restored));
    presentPublished(coordinator, render, 2U);
    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);
    EXPECT_FALSE(coordinator->snapshot()->canUndoAutomaticAlignment);
}

TEST(PlaybackCoordinatorAlignmentTests, KeepsAmbiguousEstimateWithoutChangingMapping) {
    auto provider = std::make_shared<FakeFrameProvider>();
    auto render = std::make_shared<FakeRenderChannel>();
    auto analysis = std::make_shared<FakeAlignmentAnalysisService>();
    const auto coordinator = makeCoordinator(provider,
                                             render,
                                             std::make_shared<FakeMediaProbe>(),
                                             std::make_shared<FakeDeadlineScheduler>(),
                                             std::make_shared<FakeSteadyClock>(),
                                             analysis);
    markGraphicsReady(coordinator, domain::DeviceGeneration{2});
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(EstimateAlignmentCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(analysis->waitForAlignmentRequestCount(1U));
    const auto request = analysis->alignmentRequest();
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(analysis->postCompleted(*request,
                                        {GlobalOffsetEstimate{
                                            .sourceId = 1U,
                                            .bestOffset = -1,
                                            .bestCost = 0.18F,
                                            .runnerUpCost = 0.19F,
                                            .confidence = 0.05F,
                                            .evidenceCount = 5U,
                                            .autoApplicable = false,
                                        }}));

    const auto terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);
    EXPECT_EQ(provider->frameRequestCount(), 1U);
    const auto snapshot = coordinator->snapshot();
    ASSERT_EQ(snapshot->alignmentEstimates.size(), 1U);
    EXPECT_EQ(snapshot->alignmentEstimates.front().bestOffset, -1);
    EXPECT_FALSE(snapshot->alignmentEstimates.front().autoApplicable);
    EXPECT_TRUE(snapshot->automaticAlignmentPending);
    EXPECT_FALSE(snapshot->canConfirmAutomaticAlignment);
}

TEST(PlaybackCoordinatorAlignmentTests,
     AlignmentRequiredRejectsGlobalConfirmationUntilSequenceAnalysis) {
    auto provider = std::make_shared<FakeFrameProvider>();
    auto render = std::make_shared<FakeRenderChannel>();
    auto analysis = std::make_shared<FakeAlignmentAnalysisService>();
    const auto coordinator = makeCoordinator(provider,
                                             render,
                                             std::make_shared<FakeMediaProbe>(),
                                             std::make_shared<FakeDeadlineScheduler>(),
                                             std::make_shared<FakeSteadyClock>(),
                                             analysis);
    markGraphicsReady(coordinator, domain::DeviceGeneration{2});
    openReady(coordinator, provider, render, domain::CommandId{1}, 11);
    ASSERT_TRUE(coordinator->snapshot()->alignmentRequired);

    ASSERT_EQ(coordinator->submit(EstimateAlignmentCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(analysis->waitForAlignmentRequestCount(1U));
    const auto request = analysis->alignmentRequest();
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(analysis->postCompleted(*request,
                                        {GlobalOffsetEstimate{
                                            .sourceId = 1U,
                                            .bestOffset = 1,
                                            .bestCost = 0.05F,
                                            .runnerUpCost = 0.25F,
                                            .confidence = 0.80F,
                                            .evidenceCount = 5U,
                                            .autoApplicable = true,
                                        }}));
    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);
    EXPECT_TRUE(coordinator->snapshot()->automaticAlignmentPending);
    EXPECT_FALSE(coordinator->snapshot()->canConfirmAutomaticAlignment);

    ASSERT_EQ(coordinator->submit(ConfirmAutomaticAlignmentCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
              }),
              PortSubmitResult::Accepted);
    const auto terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Failed);
    EXPECT_EQ(provider->frameRequestCount(), 1U);
}

TEST(PlaybackCoordinatorAlignmentTests,
     ConfirmsSequenceMapBeforePublishingExplicitMissingMappings) {
    auto provider = std::make_shared<FakeFrameProvider>();
    auto render = std::make_shared<FakeRenderChannel>();
    auto analysis = std::make_shared<FakeAlignmentAnalysisService>();
    const auto coordinator = makeCoordinator(provider,
                                             render,
                                             std::make_shared<FakeMediaProbe>(),
                                             std::make_shared<FakeDeadlineScheduler>(),
                                             std::make_shared<FakeSteadyClock>(),
                                             analysis);
    markGraphicsReady(coordinator, domain::DeviceGeneration{2});
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(AnalyzeSequenceAlignmentCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(analysis->waitForSequenceRequestCount(1U));
    const auto request = analysis->sequenceRequest();
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->canonicalSourceId, 0U);
    EXPECT_EQ(request->options.bandWidth, 16U);
    ASSERT_TRUE(analysis->postCompleted(*request, {makeMissingFrameSequenceResult()}));

    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);
    EXPECT_EQ(provider->frameRequestCount(), 1U);
    EXPECT_TRUE(coordinator->snapshot()->automaticAlignmentPending);

    ASSERT_EQ(coordinator->submit(ConfirmAutomaticAlignmentCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const auto mappedCurrent = provider->frameRequest(1U);
    ASSERT_TRUE(mappedCurrent.has_value());
    ASSERT_EQ(mappedCurrent->sourceOffsets.size(), 1U);
    EXPECT_EQ(mappedCurrent->sourceOffsets.front().sourceId, 1U);
    EXPECT_EQ(mappedCurrent->sourceOffsets.front().frames, 0);
    EXPECT_EQ(mappedCurrent->sourceOffsets.front().matchKind, FrameMatchKind::AutoAligned);
    ASSERT_TRUE(provider->postFrameReady(*mappedCurrent, makeFrameSet(mappedCurrent->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    ASSERT_TRUE(provider->postFrameSucceeded(*mappedCurrent));
    presentPublished(coordinator, render, 1U);
    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);

    ASSERT_EQ(coordinator->submit(SeekFrameCommand{
                  .context = commandContext(coordinator, domain::CommandId{4}),
                  .frameId = domain::FrameId{4},
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(3U));
    const auto missing = provider->frameRequest(2U);
    ASSERT_TRUE(missing.has_value());
    ASSERT_EQ(missing->sourceOffsets.size(), 1U);
    EXPECT_EQ(missing->sourceOffsets.front().sourceId, 1U);
    EXPECT_EQ(missing->sourceOffsets.front().matchKind, FrameMatchKind::Missing);
    EXPECT_EQ(missing->sourceOffsets.front().frames, 0);

    const auto snapshot = coordinator->snapshot();
    ASSERT_EQ(snapshot->sequenceAlignments.size(), 1U);
    ASSERT_EQ(snapshot->sequenceAlignments.front().anomalies.size(), 1U);
    EXPECT_TRUE(snapshot->sequenceAlignments.front().autoApplicable);
}

TEST(PlaybackCoordinatorAlignmentTests,
     RestoresValidatedDerivedSequenceMapWithoutCreatingAProposal) {
    auto provider = std::make_shared<FakeFrameProvider>();
    auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider,
                                             render,
                                             std::make_shared<FakeMediaProbe>(),
                                             std::make_shared<FakeDeadlineScheduler>(),
                                             std::make_shared<FakeSteadyClock>());
    markGraphicsReady(coordinator, domain::DeviceGeneration{2});
    openReady(coordinator, provider, render);
    SequenceAlignmentResult restoredResult = makeMissingFrameSequenceResult();
    restoredResult.segments.front().state = AlignmentSegmentState::Accepted;
    const auto cached = std::make_shared<const std::vector<SequenceAlignmentResult>>(
        std::vector<SequenceAlignmentResult>{std::move(restoredResult)});

    ASSERT_EQ(coordinator->submit(RestoreSequenceAlignmentCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .sequenceResults = cached,
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const auto restored = provider->frameRequest(1U);
    ASSERT_TRUE(restored.has_value());
    ASSERT_EQ(restored->sourceOffsets.size(), 1U);
    EXPECT_EQ(restored->sourceOffsets.front().matchKind, FrameMatchKind::AutoAligned);
    ASSERT_TRUE(provider->postFrameReady(*restored, makeFrameSet(restored->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    ASSERT_TRUE(provider->postFrameSucceeded(*restored));
    presentPublished(coordinator, render, 1U);
    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);

    const auto snapshot = coordinator->snapshot();
    EXPECT_FALSE(snapshot->automaticAlignmentPending);
    EXPECT_FALSE(snapshot->canConfirmAutomaticAlignment);
    ASSERT_EQ(snapshot->sequenceAlignments.size(), 1U);
    const auto published = coordinator->acceptedSequenceAlignments();
    ASSERT_NE(published, nullptr);
    EXPECT_EQ(*published, *cached);
}

TEST(PlaybackCoordinatorAlignmentTests, KeepsAmbiguousSequenceDiagnosticsWithoutReseeking) {
    auto provider = std::make_shared<FakeFrameProvider>();
    auto render = std::make_shared<FakeRenderChannel>();
    auto analysis = std::make_shared<FakeAlignmentAnalysisService>();
    const auto coordinator = makeCoordinator(provider,
                                             render,
                                             std::make_shared<FakeMediaProbe>(),
                                             std::make_shared<FakeDeadlineScheduler>(),
                                             std::make_shared<FakeSteadyClock>(),
                                             analysis);
    markGraphicsReady(coordinator, domain::DeviceGeneration{2});
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(AnalyzeSequenceAlignmentCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(analysis->waitForSequenceRequestCount(1U));
    const auto request = analysis->sequenceRequest();
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(analysis->postCompleted(*request, {makeMissingFrameSequenceResult(false)}));

    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);
    EXPECT_EQ(provider->frameRequestCount(), 1U);
    ASSERT_EQ(coordinator->snapshot()->sequenceAlignments.size(), 1U);
    EXPECT_FALSE(coordinator->snapshot()->sequenceAlignments.front().autoApplicable);
}

TEST(PlaybackCoordinatorAlignmentTests,
     ManualAnchorsOverrideAutomaticMapsAndRejectCrossingAnchors) {
    auto provider = std::make_shared<FakeFrameProvider>();
    auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider,
                                             render,
                                             std::make_shared<FakeMediaProbe>(),
                                             std::make_shared<FakeDeadlineScheduler>(),
                                             std::make_shared<FakeSteadyClock>());
    markGraphicsReady(coordinator, domain::DeviceGeneration{2});
    openReady(coordinator, provider, render);

    const auto submitAnchor = [&](const domain::CommandId commandId,
                                  const std::int64_t canonical,
                                  const std::int64_t source) {
        return coordinator->submit(SetManualAlignmentAnchorCommand{
            .context = commandContext(coordinator, commandId),
            .sourceId = 1U,
            .anchor =
                ManualAlignmentAnchor{
                    .canonicalFrameId = domain::FrameId{canonical},
                    .sourceFrameId = domain::FrameId{source},
                },
        });
    };

    ASSERT_EQ(submitAnchor(domain::CommandId{2}, 4, 5), PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const auto firstAnchor = provider->frameRequest(1U);
    ASSERT_TRUE(firstAnchor.has_value());
    ASSERT_EQ(firstAnchor->sourceOffsets.size(), 1U);
    EXPECT_EQ(firstAnchor->sourceOffsets.front().frames, 1);
    EXPECT_EQ(firstAnchor->sourceOffsets.front().matchKind, FrameMatchKind::ManualAnchor);
    ASSERT_TRUE(provider->postFrameReady(*firstAnchor, makeFrameSet(firstAnchor->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    ASSERT_TRUE(provider->postFrameSucceeded(*firstAnchor));
    presentPublished(coordinator, render, 1U);
    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);

    ASSERT_EQ(submitAnchor(domain::CommandId{3}, 8, 10), PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(3U));
    const auto secondAnchor = provider->frameRequest(2U);
    ASSERT_TRUE(secondAnchor.has_value());
    ASSERT_TRUE(provider->postFrameReady(*secondAnchor, makeFrameSet(secondAnchor->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(3U));
    ASSERT_TRUE(provider->postFrameSucceeded(*secondAnchor));
    presentPublished(coordinator, render, 2U);
    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);

    ASSERT_EQ(coordinator->submit(SeekFrameCommand{
                  .context = commandContext(coordinator, domain::CommandId{4}),
                  .frameId = domain::FrameId{6},
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(4U));
    const auto interpolated = provider->frameRequest(3U);
    ASSERT_TRUE(interpolated.has_value());
    ASSERT_EQ(interpolated->sourceOffsets.size(), 1U);
    EXPECT_EQ(interpolated->sourceOffsets.front().frames, 2);
    EXPECT_EQ(interpolated->sourceOffsets.front().matchKind, FrameMatchKind::ManualAnchor);

    // Supersede the unfinished seek before validating a crossing anchor.
    ASSERT_EQ(submitAnchor(domain::CommandId{5}, 6, 4), PortSubmitResult::Accepted);
    const auto terminals = waitForTerminals(coordinator, 2U);
    ASSERT_EQ(terminals.size(), 2U);
    EXPECT_EQ(terminals.back().outcome, CommandOutcome::Failed);
    ASSERT_EQ(coordinator->snapshot()->manualAlignmentAnchors.size(), 1U);
    EXPECT_EQ(coordinator->snapshot()->manualAlignmentAnchors.front().anchors.size(), 2U);
}

void openVfrReady(const std::shared_ptr<PlaybackCoordinator>& coordinator,
                  const std::shared_ptr<FakeMediaProbe>& probe,
                  const std::shared_ptr<FakeFrameProvider>& provider,
                  const std::shared_ptr<FakeRenderChannel>& render,
                  const std::shared_ptr<const domain::FrameTimeline>& timeline,
                  const domain::MediaDescriptor& descriptorA,
                  const domain::MediaDescriptor& descriptorB,
                  const domain::CommandId commandId = domain::CommandId{1}) {
    const std::shared_ptr<const SessionSnapshot> initial = coordinator->snapshot();
    ASSERT_NE(initial, nullptr);
    ASSERT_EQ(coordinator->submit(OpenComparisonCommand{
                  .context =
                      CommandContext{
                          .sessionId = initial->sessionId,
                          .sessionEpoch = initial->sessionEpoch,
                          .commandId = commandId,
                      },
                  .sources =
                      {
                          OpenComparisonSource{
                              .path = "C:/media/a.mp4",
                              .role = domain::ComparisonRole::kPrediction,
                              .displayName = "a",
                          },
                          OpenComparisonSource{
                              .path = "C:/media/b.mp4",
                              .role = domain::ComparisonRole::kPrediction,
                              .displayName = "b",
                          },
                      },
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(probe->waitForRequestCount(2U));
    const std::optional<MediaProbeRequest> requestA = probe->request(0U);
    const std::optional<MediaProbeRequest> requestB = probe->request(1U);
    ASSERT_TRUE(requestA.has_value());
    ASSERT_TRUE(requestB.has_value());

    // Source 0 is VFR: its probe must publish the shared runtime timeline alongside the
    // descriptor. The coordinator captures that timeline and shares it with the provider.
    const std::shared_ptr<IApplicationEventSink> sinkA = probe->lockEventSink(0U);
    ASSERT_NE(sinkA, nullptr);
    ASSERT_EQ(sinkA->postCritical(ApplicationEvent{ProbeCompleted{
                  .context = requestA->context,
                  .sourceId = domain::SourceId{0},
                  .descriptor = descriptorA,
                  .timeline = timeline,
              }}),
              EventPostResult::Accepted);
    ASSERT_TRUE(probe->postSucceeded(0U));
    // Source 1 is the equal-count CFR pair; it must not publish a runtime timeline.
    ASSERT_TRUE(probe->postCompleted(1U, descriptorB));
    ASSERT_TRUE(probe->postSucceeded(1U));

    ASSERT_TRUE(provider->waitForOpenRequestCount(1U));
    const std::optional<FrameProviderOpenRequest> open = provider->openRequest();
    ASSERT_TRUE(open.has_value());
    ASSERT_TRUE(domain::isVariableFrameRate(open->timeline));
    ASSERT_EQ(std::get<std::shared_ptr<const domain::FrameTimeline>>(open->timeline).get(),
              timeline.get());
    ASSERT_TRUE(provider->postOpenSucceeded(*open));
    ASSERT_TRUE(provider->waitForFrameRequestCount(1U));
    const std::optional<FrameRequest> frame = provider->frameRequest(0U);
    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(frame->frameId, domain::FrameId{0});
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(frame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(1U));
    ASSERT_TRUE(provider->postFrameSucceeded(*frame));
    presentPublished(coordinator, render, 0U);
    const std::vector<CommandTerminal> terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().context.commandId, commandId);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);
}

[[nodiscard]] std::shared_ptr<PlaybackCoordinator>
makeCoordinator(const std::shared_ptr<FakeFrameProvider>& provider,
                const std::shared_ptr<FakeRenderChannel>& render,
                std::shared_ptr<IMediaProbe> mediaProbe = std::make_shared<FakeMediaProbe>(),
                std::shared_ptr<IDeadlineScheduler> deadlineScheduler =
                    std::make_shared<FakeDeadlineScheduler>(),
                std::shared_ptr<ISteadyClock> clock = std::make_shared<FakeSteadyClock>(),
                std::shared_ptr<IAlignmentAnalysisService> analysisService) {
    auto coordinator =
        PlaybackCoordinator::create(domain::SessionId{91},
                                    PlaybackCoordinator::Dependencies{
                                        .mediaProbe = std::move(mediaProbe),
                                        .directFrameProvider = provider,
                                        .alignmentAnalysisService = std::move(analysisService),
                                        .deadlineScheduler = std::move(deadlineScheduler),
                                        .clock = std::move(clock),
                                        .renderChannel = render,
                                    });
    // D03: production Contextual is smoothness-first RealTime. Unit tests assert sequential /
    // every-frame presentation and must opt into ReviewEveryFrame here; catch-up tests call
    // requireRealTimeContinuity afterwards to switch back.
    requireReviewEveryFrameContinuity(coordinator, domain::CommandId{799});
    return coordinator;
}

void markGraphicsReady(const std::shared_ptr<PlaybackCoordinator>& coordinator,
                       const domain::DeviceGeneration generation = domain::DeviceGeneration{2}) {
    ASSERT_EQ(coordinator->postCritical(ApplicationEvent{GraphicsDeviceReady{
                  .context = GraphicsEventContext{.deviceGeneration = generation},
              }}),
              EventPostResult::Accepted);
    ASSERT_TRUE(waitUntil([&coordinator, generation] {
        const auto snapshot = coordinator->snapshot();
        return snapshot->graphicsReady && snapshot->deviceGeneration == generation;
    }));
}

TEST(PlaybackCoordinatorAlignmentTests, BackgroundSequenceAnalysisReportsProgressAndAllowsSeek) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto analysis = std::make_shared<FakeAlignmentAnalysisService>();
    const auto coordinator = makeCoordinator(provider,
                                             render,
                                             std::make_shared<FakeMediaProbe>(),
                                             std::make_shared<FakeDeadlineScheduler>(),
                                             std::make_shared<FakeSteadyClock>(),
                                             analysis);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(AnalyzeSequenceAlignmentCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(analysis->waitForSequenceRequestCount(1U));
    const auto request = analysis->sequenceRequest();
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->sources.size(), 2U);
    EXPECT_TRUE(request->timeline.has_value());
    ASSERT_TRUE(analysis->postStarted(*request, 20U));
    ASSERT_TRUE(analysis->postProgress(*request, 5U, 20U));
    ASSERT_TRUE(waitUntil([&coordinator] {
        const auto snapshot = coordinator->snapshot();
        return snapshot->alignmentAnalysisJobId.has_value() &&
               snapshot->alignmentAnalysisCompletedUnits == 5U &&
               snapshot->alignmentAnalysisWork.totalUnits == 20U &&
               snapshot->alignmentAnalysisPhase == AlignmentAnalysisPhase::CollectingSignatures;
    }));

    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));

    ASSERT_EQ(coordinator->submit(CancelAlignmentAnalysisCommand{
                  .context = commandContext(coordinator, domain::CommandId{4}),
              }),
              PortSubmitResult::Accepted);
    EXPECT_TRUE(analysis->waitForCancelCount(1U));
}

[[nodiscard]] CommandContext commandContext(const std::shared_ptr<PlaybackCoordinator>& coordinator,
                                            const domain::CommandId commandId) {
    const auto snapshot = coordinator->snapshot();
    return CommandContext{
        .sessionId = snapshot->sessionId,
        .sessionEpoch = snapshot->sessionEpoch,
        .commandId = commandId,
    };
}

TEST(PlaybackCoordinatorTests, PlayUsesAbsoluteRationalCadenceAndSequentialFrameSets) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler, clock);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render);

    const auto ready = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(PlayCommand{
                  .context =
                      CommandContext{
                          .sessionId = ready->sessionId,
                          .sessionEpoch = ready->sessionEpoch,
                          .commandId = domain::CommandId{2},
                      },
              }),
              PortSubmitResult::Accepted);
    const auto playTerminal = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(playTerminal.size(), 1U);
    EXPECT_EQ(playTerminal.front().outcome, CommandOutcome::Succeeded);
    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    const auto firstCadence = scheduler->request(1U);
    ASSERT_TRUE(firstCadence.has_value());
    EXPECT_EQ(firstCadence->due, clock->now() + 19'334us);
    ASSERT_TRUE(waitUntil([&coordinator] {
        return coordinator->snapshot()->playbackState == domain::PlaybackState::kPlaying;
    }));
    EXPECT_EQ(provider->frameRequestCount(), 1U);

    clock->set(firstCadence->due);
    ASSERT_TRUE(scheduler->fire(1U));
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const auto frame = provider->frameRequest(1U);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->frameId, domain::FrameId{1});
    EXPECT_EQ(frame->priority, FrameRequestPriority::Sequential);
    ASSERT_TRUE(provider->waitForFrameRequestCount(3U));
    const auto prepared = provider->frameRequest(2U);
    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(prepared->frameId, domain::FrameId{2});
    EXPECT_EQ(prepared->priority, FrameRequestPriority::Sequential);
    ASSERT_TRUE(waitUntil([&coordinator] {
        return coordinator->snapshot()->playbackState == domain::PlaybackState::kBuffering;
    }));

    ASSERT_TRUE(provider->postFrameSucceeded(*frame));
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(frame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    ASSERT_TRUE(provider->postFrameReady(*prepared, makeFrameSet(prepared->frameId)));
    ASSERT_TRUE(provider->postFrameSucceeded(*prepared));
    EXPECT_EQ(render->publishedCount(), 2U);
    presentPublished(coordinator, render, 1U);
    ASSERT_TRUE(scheduler->waitForScheduleCount(4U));
    ASSERT_TRUE(waitUntil([&coordinator] {
        const auto snapshot = coordinator->snapshot();
        return snapshot->displayedFrame == domain::FrameId{1} &&
               snapshot->playbackState == domain::PlaybackState::kPlaying;
    }));
    const auto secondCadence = scheduler->request(3U);
    ASSERT_TRUE(secondCadence.has_value());
    EXPECT_EQ(secondCadence->due, firstCadence->due + 33'333us);
    clock->set(secondCadence->due);
    ASSERT_TRUE(scheduler->fire(3U));
    ASSERT_TRUE(render->waitForPublishedCount(3U));
    ASSERT_TRUE(provider->waitForFrameRequestCount(4U));
    const auto following = provider->frameRequest(3U);
    ASSERT_TRUE(following.has_value());
    EXPECT_EQ(following->frameId, domain::FrameId{3});
}

TEST(PlaybackCoordinatorTests, HighFrameRatePlayCapsThePreparationLeadAtHalfTheFrameInterval) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler, clock);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render, domain::CommandId{1}, 12, 120);

    const auto ready = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(PlayCommand{
                  .context =
                      CommandContext{
                          .sessionId = ready->sessionId,
                          .sessionEpoch = ready->sessionEpoch,
                          .commandId = domain::CommandId{2},
                      },
              }),
              PortSubmitResult::Accepted);
    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);

    // The first frame of a run has no in-run predecessor, so the full lead applies and the
    // 14000us reach past the anchor collapses onto the 1000us preparation floor.
    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    const auto firstCadence = scheduler->request(1U);
    ASSERT_TRUE(firstCadence.has_value());
    EXPECT_EQ(firstCadence->due, clock->now() + 1'000us);

    // 120 fps: consecutive frames are 8333us apart, so half the interval (4166us) is below the
    // 14000us constant. The second request is capped at half the interval and stays ahead of its
    // boundary by that amount: 16667 - 4166 = 12501us after the anchor. The fixed lead would have
    // reached past the previous boundary and collapsed onto the floor again.
    clock->set(firstCadence->due);
    ASSERT_TRUE(scheduler->fire(1U));
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const std::optional<FrameRequest> frame = provider->frameRequest(1U);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->frameId, domain::FrameId{1});
    EXPECT_EQ(frame->priority, FrameRequestPriority::Sequential);
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(frame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    ASSERT_TRUE(provider->postFrameSucceeded(*frame));
    presentPublished(coordinator, render, 1U);

    ASSERT_TRUE(scheduler->waitForScheduleCount(4U));
    const auto secondCadence = scheduler->request(3U);
    ASSERT_TRUE(secondCadence.has_value());
    EXPECT_EQ(secondCadence->due, clock->now() + 11'501us);
    EXPECT_GT(secondCadence->due, clock->now() + 4'000us);
}

TEST(PlaybackCoordinatorTests, SixtyFpsPlayCapsThePreparationLeadAtHalfTheFrameInterval) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler, clock);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render, domain::CommandId{1}, 12, 60);

    const auto ready = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(PlayCommand{
                  .context =
                      CommandContext{
                          .sessionId = ready->sessionId,
                          .sessionEpoch = ready->sessionEpoch,
                          .commandId = domain::CommandId{2},
                      },
              }),
              PortSubmitResult::Accepted);
    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);

    // 60 fps: the first frame's full 14000us lead still clears the anchor, leaving 16667 -
    // 14000 = 2667us.
    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    const auto firstCadence = scheduler->request(1U);
    ASSERT_TRUE(firstCadence.has_value());
    EXPECT_EQ(firstCadence->due, clock->now() + 2'667us);

    // Frames are 16667us apart, so half the interval (8333us) is below the constant and the lead
    // is capped: the second request keeps 8333us of margin before its boundary instead of the
    // 14000us that would leave only 2667us of it.
    clock->set(firstCadence->due);
    ASSERT_TRUE(scheduler->fire(1U));
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const std::optional<FrameRequest> frame = provider->frameRequest(1U);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->frameId, domain::FrameId{1});
    EXPECT_EQ(frame->priority, FrameRequestPriority::Sequential);
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(frame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    ASSERT_TRUE(provider->postFrameSucceeded(*frame));
    presentPublished(coordinator, render, 1U);

    ASSERT_TRUE(scheduler->waitForScheduleCount(4U));
    const auto secondCadence = scheduler->request(3U);
    ASSERT_TRUE(secondCadence.has_value());
    EXPECT_EQ(secondCadence->due, clock->now() + 22'334us);
    EXPECT_GT(secondCadence->due, clock->now() + 8'000us);
}

TEST(PlaybackCoordinatorTests, ThirtyFpsPlayKeepsTheFullPreparationLead) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler, clock);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render);

    const auto ready = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(PlayCommand{
                  .context =
                      CommandContext{
                          .sessionId = ready->sessionId,
                          .sessionEpoch = ready->sessionEpoch,
                          .commandId = domain::CommandId{2},
                      },
              }),
              PortSubmitResult::Accepted);
    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);

    // 30 fps: half the interval (16667us) exceeds the 14000us constant, so the full lead stays in
    // place and the request lands 33334 - 14000 = 19334us after the anchor.
    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    const auto firstCadence = scheduler->request(1U);
    ASSERT_TRUE(firstCadence.has_value());
    EXPECT_EQ(firstCadence->due, clock->now() + 19'334us);
}

TEST(PlaybackCoordinatorTests, VfrPlayCapsThePreparationLeadAtTheLocalFrameInterval) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto probe = std::make_shared<FakeMediaProbe>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render, probe, scheduler, clock);

    const std::shared_ptr<const domain::FrameTimeline> timeline = makeVfrTimeline();
    ASSERT_NE(timeline, nullptr);
    const domain::MediaDescriptor descriptorA =
        makeVfrDescriptor("C:/media/a.mp4",
                          domain::MediaExtent{.width = 320, .height = 180},
                          4,
                          domain::MediaTime{69800});
    const domain::MediaDescriptor descriptorB =
        makeDescriptor("C:/media/b.mp4", domain::MediaExtent{.width = 160, .height = 90}, 4);

    openVfrReady(coordinator, probe, provider, render, timeline, descriptorA, descriptorB);
    markGraphicsReady(coordinator);

    const std::shared_ptr<const SessionSnapshot> ready = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(PlayCommand{
                  .context =
                      CommandContext{
                          .sessionId = ready->sessionId,
                          .sessionEpoch = ready->sessionEpoch,
                          .commandId = domain::CommandId{2},
                      },
              }),
              PortSubmitResult::Accepted);
    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);

    // The VFR timeline starts frames at 0/10000/50000/69800us. Frame 1 is one millisecond after
    // the anchor, so its request collapses onto the 1000us preparation floor either way.
    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    const auto firstCadence = scheduler->request(1U);
    ASSERT_TRUE(firstCadence.has_value());
    EXPECT_EQ(firstCadence->due, clock->now() + 1'000us);

    clock->set(firstCadence->due);
    ASSERT_TRUE(scheduler->fire(1U));
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const std::optional<FrameRequest> frame = provider->frameRequest(1U);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(frame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    presentPublished(coordinator, render, 1U);
    ASSERT_TRUE(provider->postFrameSucceeded(*frame));

    // Frame 2 is 40000us after frame 1, so half that interval (20000us) still exceeds the
    // constant and the full 14000us lead applies: 50000 - 14000 = 36000us.
    ASSERT_TRUE(scheduler->waitForScheduleCount(4U));
    const auto secondCadence = scheduler->request(3U);
    ASSERT_TRUE(secondCadence.has_value());
    EXPECT_EQ(secondCadence->due, firstCadence->due + 35'000us);

    clock->set(secondCadence->due);
    ASSERT_TRUE(scheduler->fire(3U));
    ASSERT_TRUE(provider->waitForFrameRequestCount(3U));
    const std::optional<FrameRequest> next = provider->frameRequest(2U);
    ASSERT_TRUE(next.has_value());
    ASSERT_TRUE(provider->postFrameReady(*next, makeFrameSet(next->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(3U));
    presentPublished(coordinator, render, 2U);
    ASSERT_TRUE(provider->postFrameSucceeded(*next));

    // Frame 3 is only 19800us after frame 2, so the capped lead (9900us) applies against the
    // irregular spacing: 69800 - 9900 = 59900us, not the flat 55800us a fixed lead would use.
    ASSERT_TRUE(scheduler->waitForScheduleCount(6U));
    const auto thirdCadence = scheduler->request(5U);
    ASSERT_TRUE(thirdCadence.has_value());
    EXPECT_EQ(thirdCadence->due, clock->now() + 23'900us);
}

TEST(PlaybackCoordinatorTests, PlayCommandScalesCadenceBySpeed) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler, clock);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render);

    const auto ready = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(PlayCommand{
                  .context =
                      CommandContext{
                          .sessionId = ready->sessionId,
                          .sessionEpoch = ready->sessionEpoch,
                          .commandId = domain::CommandId{2},
                      },
                  .speed = 2.0,
              }),
              PortSubmitResult::Accepted);
    const auto playTerminal = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(playTerminal.size(), 1U);
    EXPECT_EQ(playTerminal.front().outcome, CommandOutcome::Succeeded);
    EXPECT_DOUBLE_EQ(coordinator->snapshot()->playbackSpeed, 2.0);

    // At 2x the first cadence fires at half the 33'334us single-frame interval, minus the
    // 14ms presentation lead: 33'334/2 - 14'000 = 2'667us.
    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    const auto firstCadence = scheduler->request(1U);
    ASSERT_TRUE(firstCadence.has_value());
    EXPECT_EQ(firstCadence->due, clock->now() + 2'667us);
}

TEST(PlaybackCoordinatorTests, SetPlaybackRateWhilePlayingReanchorsCadence) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler, clock);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render);

    const auto ready = coordinator->snapshot();
    const CommandContext playContext{
        .sessionId = ready->sessionId,
        .sessionEpoch = ready->sessionEpoch,
        .commandId = domain::CommandId{2},
    };
    ASSERT_EQ(coordinator->submit(PlayCommand{.context = playContext}), PortSubmitResult::Accepted);
    ASSERT_EQ(waitForTerminals(coordinator, 1U).size(), 1U);
    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    ASSERT_TRUE(waitUntil([&coordinator] {
        return coordinator->snapshot()->playbackState == domain::PlaybackState::kPlaying;
    }));

    // Halve the speed while playing: the run re-anchors on the displayed frame (frame 0) and
    // the pending cadence is rescheduled, so the next due is the slowed first-frame interval
    // from the new anchor minus the presentation lead: 33'334/0.5 would be a full second for
    // frame 1's due at 0.5x, but the immediate target is still frame 1 → 66'668 - 14'000.
    ASSERT_EQ(coordinator->submit(SetPlaybackRateCommand{
                  .context =
                      CommandContext{
                          .sessionId = ready->sessionId,
                          .sessionEpoch = ready->sessionEpoch,
                          .commandId = domain::CommandId{3},
                      },
                  .speed = 0.5,
              }),
              PortSubmitResult::Accepted);
    ASSERT_EQ(waitForTerminals(coordinator, 1U).size(), 1U);
    EXPECT_DOUBLE_EQ(coordinator->snapshot()->playbackSpeed, 0.5);

    // The rescheduled cadence is the third scheduler entry (index 2): openReady's open request
    // used index 0, the initial play cadence used index 1.
    ASSERT_TRUE(scheduler->waitForScheduleCount(3U));
    const auto reanchored = scheduler->request(2U);
    ASSERT_TRUE(reanchored.has_value());
    EXPECT_EQ(reanchored->due, clock->now() + 52'668us);
}

TEST(PlaybackCoordinatorTests, StepBeforeCadenceCancelsRunAndRejectsTheStaleTick) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(
                  PlayCommand{.context = commandContext(coordinator, domain::CommandId{2})}),
              PortSubmitResult::Accepted);
    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);
    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    ASSERT_EQ(coordinator->snapshot()->playbackState, domain::PlaybackState::kPlaying);

    // A frame step during playback pauses the run first, then seeks the next frame
    // (USERPLAN 3.1/6.2) instead of bouncing off a Busy gate.
    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(waitUntil([&coordinator] {
        return coordinator->snapshot()->playbackState == domain::PlaybackState::kSeeking;
    }));
    EXPECT_EQ(coordinator->snapshot()->requestedFrame, domain::FrameId{1});
    ASSERT_TRUE(provider->waitForCancelCount(1U));
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const auto seek = provider->frameRequest(1U);
    ASSERT_TRUE(seek.has_value());
    EXPECT_EQ(seek->frameId, domain::FrameId{1});

    ASSERT_TRUE(provider->postFrameReady(*seek, makeFrameSet(seek->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    ASSERT_TRUE(provider->postFrameSucceeded(*seek));
    presentPublished(coordinator, render, 1U);
    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);
    EXPECT_EQ(coordinator->snapshot()->displayedFrame, domain::FrameId{1});

    // The stale cadence tick from the canceled run cannot request another frame.
    ASSERT_TRUE(scheduler->fire(1U));
    EXPECT_EQ(provider->frameRequestCount(), 2U);
    EXPECT_EQ(coordinator->snapshot()->displayedFrame, domain::FrameId{1});
}

TEST(PlaybackCoordinatorTests, PauseDuringDecodeCancelsGenerationAndIgnoresLateResults) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler, clock);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(
                  PlayCommand{.context = commandContext(coordinator, domain::CommandId{2})}),
              PortSubmitResult::Accepted);
    static_cast<void>(waitForTerminals(coordinator, 1U));
    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    const auto cadence = scheduler->request(1U);
    ASSERT_TRUE(cadence.has_value());
    clock->set(cadence->due);
    ASSERT_TRUE(scheduler->fire(1U));
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const auto lateFrame = provider->frameRequest(1U);
    ASSERT_TRUE(lateFrame.has_value());

    ASSERT_EQ(coordinator->submit(
                  PauseCommand{.context = commandContext(coordinator, domain::CommandId{3})}),
              PortSubmitResult::Accepted);
    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);
    EXPECT_EQ(coordinator->snapshot()->playbackState, domain::PlaybackState::kPaused);
    EXPECT_FALSE(coordinator->snapshot()->requestedFrame.has_value());
    EXPECT_TRUE(provider->waitForCancelCount(1U));

    ASSERT_TRUE(provider->postFrameReady(*lateFrame, makeFrameSet(lateFrame->frameId)));
    ASSERT_TRUE(provider->postFrameSucceeded(*lateFrame));
    markGraphicsReady(coordinator, domain::DeviceGeneration{3});
    EXPECT_EQ(render->publishedCount(), 1U);
    EXPECT_EQ(coordinator->snapshot()->displayedFrame, domain::FrameId{0});
}

TEST(PlaybackCoordinatorTests, PauseAfterPublicationDrainsOnlyThatAtomicFrameSet) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler, clock);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(
                  PlayCommand{.context = commandContext(coordinator, domain::CommandId{2})}),
              PortSubmitResult::Accepted);
    static_cast<void>(waitForTerminals(coordinator, 1U));
    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    const auto cadence = scheduler->request(1U);
    ASSERT_TRUE(cadence.has_value());
    clock->set(cadence->due);
    ASSERT_TRUE(scheduler->fire(1U));
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const auto frame = provider->frameRequest(1U);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(frame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));

    ASSERT_EQ(coordinator->submit(
                  PauseCommand{.context = commandContext(coordinator, domain::CommandId{3})}),
              PortSubmitResult::Accepted);
    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);
    EXPECT_EQ(coordinator->snapshot()->playbackState, domain::PlaybackState::kPaused);
    EXPECT_EQ(coordinator->snapshot()->requestedFrame, domain::FrameId{1});

    presentPublished(coordinator, render, 1U);
    ASSERT_TRUE(provider->postFrameSucceeded(*frame));
    ASSERT_TRUE(waitUntil([&coordinator] {
        const auto snapshot = coordinator->snapshot();
        return snapshot->displayedFrame == domain::FrameId{1} &&
               !snapshot->requestedFrame.has_value();
    }));
    EXPECT_EQ(coordinator->snapshot()->playbackState, domain::PlaybackState::kPaused);
    EXPECT_EQ(scheduler->scheduleCount(), 3U);
    // The published frame drains atomically while one unpublished successor is already decoding.
    EXPECT_EQ(provider->frameRequestCount(), 3U);
}

TEST(PlaybackCoordinatorTests, SlowDecodeDropsCompleteFrameSetsWithOnePreparedSuccessor) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler, clock);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render);
    requireRealTimeContinuity(coordinator);

    ASSERT_EQ(coordinator->submit(
                  PlayCommand{.context = commandContext(coordinator, domain::CommandId{2})}),
              PortSubmitResult::Accepted);
    static_cast<void>(waitForTerminals(coordinator, 1U));
    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    const auto cadence = scheduler->request(1U);
    ASSERT_TRUE(cadence.has_value());
    clock->set(cadence->due);
    ASSERT_TRUE(scheduler->fire(1U));
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const auto firstPlaybackFrame = provider->frameRequest(1U);
    ASSERT_TRUE(firstPlaybackFrame.has_value());
    EXPECT_EQ(firstPlaybackFrame->frameId, domain::FrameId{1});
    ASSERT_TRUE(provider->waitForFrameRequestCount(3U));
    const auto preparedFrame = provider->frameRequest(2U);
    ASSERT_TRUE(preparedFrame.has_value());
    EXPECT_EQ(preparedFrame->frameId, domain::FrameId{2});

    clock->advance(2500ms);
    ASSERT_TRUE(
        provider->postFrameReady(*firstPlaybackFrame, makeFrameSet(firstPlaybackFrame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    ASSERT_TRUE(provider->postFrameSucceeded(*firstPlaybackFrame));
    presentPublished(coordinator, render, 1U);
    ASSERT_TRUE(provider->waitForFrameRequestCount(4U));
    const auto skippedPlaybackFrame = provider->frameRequest(3U);
    ASSERT_TRUE(skippedPlaybackFrame.has_value());
    EXPECT_EQ(skippedPlaybackFrame->frameId, domain::FrameId{11});
    EXPECT_EQ(skippedPlaybackFrame->priority, FrameRequestPriority::Sequential);
    ASSERT_TRUE(waitUntil([&coordinator] {
        const auto snapshot = coordinator->snapshot();
        return snapshot->displayedFrame == domain::FrameId{1} &&
               snapshot->requestedFrame == domain::FrameId{11};
    }));
}

TEST(PlaybackCoordinatorTests, SubHalfSecondPlaybackDelayUsesPreparedSuccessorWithoutDropping) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler, clock);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(
                  PlayCommand{.context = commandContext(coordinator, domain::CommandId{2})}),
              PortSubmitResult::Accepted);
    static_cast<void>(waitForTerminals(coordinator, 1U));
    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    const auto cadence = scheduler->request(1U);
    ASSERT_TRUE(cadence.has_value());
    clock->set(cadence->due);
    ASSERT_TRUE(scheduler->fire(1U));
    ASSERT_TRUE(provider->waitForFrameRequestCount(3U));
    const auto active = provider->frameRequest(1U);
    const auto prepared = provider->frameRequest(2U);
    ASSERT_TRUE(active.has_value());
    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(active->frameId, domain::FrameId{1});
    EXPECT_EQ(prepared->frameId, domain::FrameId{2});

    clock->advance(400ms);
    ASSERT_TRUE(provider->postFrameReady(*active, makeFrameSet(active->frameId)));
    ASSERT_TRUE(provider->postFrameSucceeded(*active));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    presentPublished(coordinator, render, 1U);
    ASSERT_TRUE(waitUntil([&coordinator] {
        const auto snapshot = coordinator->snapshot();
        return snapshot->displayedFrame == domain::FrameId{1} &&
               snapshot->requestedFrame == domain::FrameId{2};
    }));
    ASSERT_EQ(provider->frameRequestCount(), 4U);
    const auto following = provider->frameRequest(3U);
    ASSERT_TRUE(following.has_value());
    EXPECT_EQ(following->frameId, domain::FrameId{3});
}

TEST(PlaybackCoordinatorTests, FinalFrameAutoPausesAndPlayFromEndRestartsAtZero) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler, clock);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render);
    requireRealTimeContinuity(coordinator);

    ASSERT_EQ(coordinator->submit(
                  PlayCommand{.context = commandContext(coordinator, domain::CommandId{2})}),
              PortSubmitResult::Accepted);
    static_cast<void>(waitForTerminals(coordinator, 1U));
    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    const auto cadence = scheduler->request(1U);
    ASSERT_TRUE(cadence.has_value());
    clock->set(cadence->due + 2500ms);
    ASSERT_TRUE(scheduler->fire(1U));
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const auto finalFrame = provider->frameRequest(1U);
    ASSERT_TRUE(finalFrame.has_value());
    EXPECT_EQ(finalFrame->frameId, domain::FrameId{11});
    ASSERT_TRUE(provider->postFrameSucceeded(*finalFrame));
    ASSERT_TRUE(provider->postFrameReady(*finalFrame, makeFrameSet(finalFrame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    presentPublished(coordinator, render, 1U);
    ASSERT_TRUE(waitUntil(
        [&coordinator] { return coordinator->snapshot()->displayedFrame == domain::FrameId{11}; }));
    EXPECT_EQ(coordinator->snapshot()->playbackState, domain::PlaybackState::kPaused);

    ASSERT_EQ(coordinator->submit(
                  PlayCommand{.context = commandContext(coordinator, domain::CommandId{3})}),
              PortSubmitResult::Accepted);
    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);
    ASSERT_TRUE(provider->waitForFrameRequestCount(3U));
    const auto replayFrame = provider->frameRequest(2U);
    ASSERT_TRUE(replayFrame.has_value());
    EXPECT_EQ(replayFrame->frameId, domain::FrameId{0});
    EXPECT_EQ(replayFrame->priority, FrameRequestPriority::Sequential);
    ASSERT_TRUE(scheduler->waitForScheduleCount(4U));

    ASSERT_TRUE(provider->postFrameSucceeded(*replayFrame));
    ASSERT_TRUE(provider->postFrameReady(*replayFrame, makeFrameSet(replayFrame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(3U));
    presentPublished(coordinator, render, 2U);
    ASSERT_TRUE(scheduler->waitForScheduleCount(5U));
    const auto replayNextCadence = scheduler->request(4U);
    ASSERT_TRUE(replayNextCadence.has_value());
    EXPECT_EQ(replayNextCadence->due, clock->now() + 19'334us);
    clock->set(replayNextCadence->due);
    ASSERT_TRUE(scheduler->fire(4U));
    ASSERT_TRUE(provider->waitForFrameRequestCount(4U));
    const auto nextFrame = provider->frameRequest(3U);
    ASSERT_TRUE(nextFrame.has_value());
    EXPECT_EQ(nextFrame->frameId, domain::FrameId{1});
    EXPECT_EQ(nextFrame->priority, FrameRequestPriority::Sequential);
}

TEST(PlaybackCoordinatorTests, PlaybackTimeoutPausesAndRejectsLateProviderResults) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler, clock);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(
                  PlayCommand{.context = commandContext(coordinator, domain::CommandId{2})}),
              PortSubmitResult::Accepted);
    static_cast<void>(waitForTerminals(coordinator, 1U));
    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    const auto cadence = scheduler->request(1U);
    ASSERT_TRUE(cadence.has_value());
    clock->set(cadence->due);
    ASSERT_TRUE(scheduler->fire(1U));
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    ASSERT_TRUE(scheduler->waitForScheduleCount(3U));
    const auto lateFrame = provider->frameRequest(1U);
    ASSERT_TRUE(lateFrame.has_value());

    ASSERT_TRUE(scheduler->fire(2U));
    ASSERT_TRUE(waitUntil([&coordinator] {
        const auto snapshot = coordinator->snapshot();
        return snapshot->lastError.has_value() &&
               snapshot->lastError->code == domain::MediaErrorCode::kFramePresentationTimedOut;
    }));
    EXPECT_EQ(coordinator->snapshot()->playbackState, domain::PlaybackState::kPaused);
    EXPECT_EQ(coordinator->snapshot()->displayedFrame, domain::FrameId{0});
    EXPECT_FALSE(coordinator->snapshot()->requestedFrame.has_value());

    ASSERT_TRUE(provider->postFrameReady(*lateFrame, makeFrameSet(lateFrame->frameId)));
    ASSERT_TRUE(provider->postFrameSucceeded(*lateFrame));
    markGraphicsReady(coordinator, domain::DeviceGeneration{3});
    EXPECT_EQ(render->publishedCount(), 1U);
    EXPECT_EQ(coordinator->snapshot()->displayedFrame, domain::FrameId{0});
}

TEST(PlaybackCoordinatorTests, PlaybackProviderFailureKeepsLastCommittedFrameSet) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler, clock);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(
                  PlayCommand{.context = commandContext(coordinator, domain::CommandId{2})}),
              PortSubmitResult::Accepted);
    static_cast<void>(waitForTerminals(coordinator, 1U));
    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    const auto cadence = scheduler->request(1U);
    ASSERT_TRUE(cadence.has_value());
    clock->set(cadence->due);
    ASSERT_TRUE(scheduler->fire(1U));
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const auto frame = provider->frameRequest(1U);
    ASSERT_TRUE(frame.has_value());
    const domain::MediaError failure =
        domain::makeMediaError(domain::MediaErrorCode::kMediaDecodeFailed,
                               domain::MediaOperation::kMediaDecode,
                               std::nullopt,
                               true,
                               "Synthetic sequential decode failure.");
    ASSERT_TRUE(provider->postFrameFailed(*frame, failure));
    ASSERT_TRUE(waitUntil([&coordinator] {
        const auto snapshot = coordinator->snapshot();
        return snapshot->lastError.has_value() &&
               snapshot->lastError->code == domain::MediaErrorCode::kMediaDecodeFailed;
    }));
    EXPECT_EQ(coordinator->snapshot()->sessionState, domain::SessionState::kReady);
    EXPECT_EQ(coordinator->snapshot()->playbackState, domain::PlaybackState::kPaused);
    EXPECT_EQ(coordinator->snapshot()->displayedFrame, domain::FrameId{0});
    EXPECT_FALSE(coordinator->snapshot()->requestedFrame.has_value());
    EXPECT_EQ(render->publishedCount(), 1U);
}

TEST(PlaybackCoordinatorTests, GraphicsLossInvalidatesPublishedPlaybackFrameSet) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler, clock);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(
                  PlayCommand{.context = commandContext(coordinator, domain::CommandId{2})}),
              PortSubmitResult::Accepted);
    static_cast<void>(waitForTerminals(coordinator, 1U));
    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    const auto cadence = scheduler->request(1U);
    ASSERT_TRUE(cadence.has_value());
    clock->set(cadence->due);
    ASSERT_TRUE(scheduler->fire(1U));
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const auto frame = provider->frameRequest(1U);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(frame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    const auto latePresentation = render->presentation(1U);
    ASSERT_TRUE(latePresentation.has_value());

    const domain::MediaError deviceLost =
        domain::makeMediaError(domain::MediaErrorCode::kGraphicsDeviceLost,
                               domain::MediaOperation::kGraphicsInitialization,
                               std::nullopt,
                               true,
                               "Device loss during continuous playback.");
    ASSERT_EQ(coordinator->postCritical(ApplicationEvent{GraphicsDeviceLost{
                  .context = GraphicsEventContext{.deviceGeneration = domain::DeviceGeneration{2}},
                  .error = deviceLost,
              }}),
              EventPostResult::Accepted);
    ASSERT_TRUE(waitUntil([&coordinator] {
        const auto snapshot = coordinator->snapshot();
        return !snapshot->graphicsReady && snapshot->lastError.has_value() &&
               snapshot->lastError->code == domain::MediaErrorCode::kGraphicsDeviceLost;
    }));
    ASSERT_TRUE(render->waitForClearCount(1U));
    EXPECT_FALSE(render->consumePresentation(1U).has_value());
    EXPECT_EQ(coordinator->snapshot()->playbackState, domain::PlaybackState::kPaused);
    EXPECT_EQ(coordinator->snapshot()->displayedFrame, domain::FrameId{0});

    ASSERT_EQ(coordinator->postCritical(ApplicationEvent{*latePresentation}),
              EventPostResult::Accepted);
    ASSERT_TRUE(provider->postFrameSucceeded(*frame));
    markGraphicsReady(coordinator, domain::DeviceGeneration{3});
    EXPECT_EQ(coordinator->snapshot()->displayedFrame, domain::FrameId{0});
}

TEST(PlaybackCoordinatorTests, ShutdownCancelsAndInvalidatesPublishedPlaybackFrameSet) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler, clock);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(
                  PlayCommand{.context = commandContext(coordinator, domain::CommandId{2})}),
              PortSubmitResult::Accepted);
    static_cast<void>(waitForTerminals(coordinator, 1U));
    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    const auto cadence = scheduler->request(1U);
    ASSERT_TRUE(cadence.has_value());
    clock->set(cadence->due);
    ASSERT_TRUE(scheduler->fire(1U));
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const auto frame = provider->frameRequest(1U);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(frame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));

    coordinator.reset();
    ASSERT_TRUE(render->waitForClearCount(1U));
    EXPECT_FALSE(render->consumePresentation(1U).has_value());
    EXPECT_TRUE(provider->waitForCancelCount(1U));
    EXPECT_GE(scheduler->cancelCount(), 2U);
}

TEST(PlaybackCoordinatorTests, AdapterCallbackLeaseCannotOwnCoordinatorDuringShutdown) {
    const auto probe = std::make_shared<FakeMediaProbe>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    auto coordinator = makeCoordinator(provider, render, probe);
    ASSERT_NE(coordinator, nullptr);

    const auto initial = coordinator->snapshot();
    ASSERT_NE(initial, nullptr);
    ASSERT_EQ(coordinator->submit(OpenComparisonCommand{
                  .context =
                      CommandContext{
                          .sessionId = initial->sessionId,
                          .sessionEpoch = initial->sessionEpoch,
                          .commandId = domain::CommandId{1},
                      },
                  .sources =
                      {
                          OpenComparisonSource{
                              .path = "C:/media/a.mp4",
                              .role = domain::ComparisonRole::kPrediction,
                              .displayName = "a",
                          },
                          OpenComparisonSource{
                              .path = "C:/media/b.mp4",
                              .role = domain::ComparisonRole::kPrediction,
                              .displayName = "b",
                          },
                      },
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(probe->waitForRequestCount(2U));
    std::shared_ptr<IApplicationEventSink> callbackLease = probe->lockEventSink(0U);
    ASSERT_NE(callbackLease, nullptr);

    std::mutex workerMutex;
    std::condition_variable workerCondition;
    bool workerHoldsLease = false;
    bool releaseWorker = false;
    std::atomic postResult{EventPostResult::Accepted};
    std::thread adapterWorker{[events = std::move(callbackLease),
                               &workerMutex,
                               &workerCondition,
                               &workerHoldsLease,
                               &releaseWorker,
                               &postResult] {
        {
            std::unique_lock lock(workerMutex);
            workerHoldsLease = true;
            workerCondition.notify_all();
            workerCondition.wait(lock, [&releaseWorker] { return releaseWorker; });
        }
        postResult.store(events->postCritical(ignoredCriticalEvent()), std::memory_order_release);
    }};

    {
        std::unique_lock lock(workerMutex);
        EXPECT_TRUE(
            workerCondition.wait_for(lock, 5s, [&workerHoldsLease] { return workerHoldsLease; }));
    }
    const std::weak_ptr<PlaybackCoordinator> weakCoordinator = coordinator;
    coordinator.reset();
    EXPECT_TRUE(weakCoordinator.expired());

    {
        std::scoped_lock lock(workerMutex);
        releaseWorker = true;
    }
    workerCondition.notify_all();
    adapterWorker.join();
    EXPECT_EQ(postResult.load(std::memory_order_acquire), EventPostResult::Closed);
}

TEST(PlaybackCoordinatorTests, TracksGraphicsReadinessByIndependentDeviceGeneration) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    ASSERT_FALSE(coordinator->snapshot()->graphicsReady);

    ASSERT_EQ(coordinator->postCritical(ApplicationEvent{GraphicsDeviceReady{
                  .context = GraphicsEventContext{.deviceGeneration = domain::DeviceGeneration{2}},
              }}),
              EventPostResult::Accepted);
    ASSERT_TRUE(waitUntil([&coordinator] {
        const auto snapshot = coordinator->snapshot();
        return snapshot->graphicsReady && snapshot->deviceGeneration == domain::DeviceGeneration{2};
    }));

    const domain::MediaError deviceLost =
        domain::makeMediaError(domain::MediaErrorCode::kGraphicsDeviceLost,
                               domain::MediaOperation::kGraphicsInitialization,
                               std::nullopt,
                               true,
                               "ID3D11Device::GetDeviceRemovedReason returned 0x887A0005.");
    ASSERT_EQ(coordinator->postCritical(ApplicationEvent{GraphicsDeviceLost{
                  .context = GraphicsEventContext{.deviceGeneration = domain::DeviceGeneration{3}},
                  .error = deviceLost,
              }}),
              EventPostResult::Accepted);
    ASSERT_TRUE(waitUntil([&coordinator] {
        const auto snapshot = coordinator->snapshot();
        return !snapshot->graphicsReady && snapshot->lastError.has_value() &&
               snapshot->deviceGeneration == domain::DeviceGeneration{3};
    }));

    ASSERT_EQ(coordinator->postCritical(ApplicationEvent{GraphicsDeviceReady{
                  .context = GraphicsEventContext{.deviceGeneration = domain::DeviceGeneration{2}},
              }}),
              EventPostResult::Accepted);
    const domain::MediaError generationBarrier =
        domain::makeMediaError(domain::MediaErrorCode::kGraphicsDeviceLost,
                               domain::MediaOperation::kGraphicsInitialization,
                               std::nullopt,
                               true,
                               "The current device generation remains unavailable.");
    ASSERT_EQ(coordinator->postCritical(ApplicationEvent{GraphicsDeviceLost{
                  .context = GraphicsEventContext{.deviceGeneration = domain::DeviceGeneration{3}},
                  .error = generationBarrier,
              }}),
              EventPostResult::Accepted);
    ASSERT_TRUE(waitUntil([&coordinator, &generationBarrier] {
        const auto snapshot = coordinator->snapshot();
        return snapshot->lastError.has_value() &&
               snapshot->lastError->technicalDetail == generationBarrier.technicalDetail;
    }));
    EXPECT_FALSE(coordinator->snapshot()->graphicsReady);
    EXPECT_EQ(coordinator->snapshot()->deviceGeneration, domain::DeviceGeneration{3});

    ASSERT_EQ(coordinator->postCritical(ApplicationEvent{GraphicsDeviceReady{
                  .context = GraphicsEventContext{.deviceGeneration = domain::DeviceGeneration{4}},
              }}),
              EventPostResult::Accepted);
    ASSERT_TRUE(waitUntil([&coordinator] {
        const auto snapshot = coordinator->snapshot();
        return snapshot->graphicsReady && !snapshot->lastError.has_value() &&
               snapshot->deviceGeneration == domain::DeviceGeneration{4};
    }));
}

TEST(PlaybackCoordinatorTests, OpensPathsAfterBThenAProbePayloadsAndTerminals) {
    const auto probe = std::make_shared<FakeMediaProbe>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render, probe);
    ASSERT_NE(coordinator, nullptr);

    const auto initial = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(OpenComparisonCommand{
                  .context =
                      CommandContext{
                          .sessionId = initial->sessionId,
                          .sessionEpoch = initial->sessionEpoch,
                          .commandId = domain::CommandId{1},
                      },
                  .sources =
                      {
                          OpenComparisonSource{
                              .path = "C:/media/a.mp4",
                              .role = domain::ComparisonRole::kPrediction,
                              .displayName = "a",
                          },
                          OpenComparisonSource{
                              .path = "C:/media/b.mp4",
                              .role = domain::ComparisonRole::kPrediction,
                              .displayName = "b",
                          },
                      },
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(probe->waitForRequestCount(2U));
    const auto requestA = probe->request(0U);
    const auto requestB = probe->request(1U);
    ASSERT_TRUE(requestA.has_value());
    ASSERT_TRUE(requestB.has_value());
    EXPECT_EQ(requestA->sourceId, domain::SourceId{0});
    EXPECT_EQ(requestB->sourceId, domain::SourceId{1});
    EXPECT_NE(requestA->context.requestId, requestB->context.requestId);

    ASSERT_TRUE(probe->postCompleted(
        1U, makeDescriptor("C:/media/b.mp4", domain::MediaExtent{.width = 160, .height = 90})));
    ASSERT_TRUE(probe->postSucceeded(1U));
    ASSERT_EQ(coordinator->postCritical(ApplicationEvent{GraphicsDeviceReady{
                  .context = GraphicsEventContext{.deviceGeneration = domain::DeviceGeneration{1}},
              }}),
              EventPostResult::Accepted);
    ASSERT_TRUE(waitUntil([&coordinator] { return coordinator->snapshot()->graphicsReady; }));
    EXPECT_EQ(provider->openRequestCount(), 0U);

    ASSERT_TRUE(probe->postCompleted(
        0U, makeDescriptor("C:/media/a.mp4", domain::MediaExtent{.width = 320, .height = 180})));
    ASSERT_TRUE(probe->postSucceeded(0U));
    ASSERT_TRUE(provider->waitForOpenRequestCount(1U));
    const auto open = provider->openRequest();
    ASSERT_TRUE(open.has_value());
    EXPECT_EQ(open->sources[0].descriptor.normalizedPath, std::filesystem::path{"C:/media/a.mp4"});
    EXPECT_EQ(open->sources[1].descriptor.normalizedPath, std::filesystem::path{"C:/media/b.mp4"});
    EXPECT_EQ(open->context.request.sessionEpoch, domain::SessionEpoch{1});
    ASSERT_TRUE(provider->postOpenSucceeded(*open));
    ASSERT_TRUE(provider->waitForFrameRequestCount(1U));
    const auto frame = provider->frameRequest(0U);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(domain::FrameId{0})));
    ASSERT_TRUE(render->waitForPublishedCount(1U));
    ASSERT_TRUE(provider->postFrameSucceeded(*frame));
    presentPublished(coordinator, render, 0U);
    const auto terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().context.commandId, domain::CommandId{1});
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);
    EXPECT_EQ(coordinator->snapshot()->displayedFrame, domain::FrameId{0});
}

TEST(PlaybackCoordinatorTests, AcceptsSamePathAndOpensProviderExactlyOnceWithDedupedProbe) {
    const auto probe = std::make_shared<FakeMediaProbe>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render, probe);
    const auto initial = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(OpenComparisonCommand{
                  .context =
                      CommandContext{
                          .sessionId = initial->sessionId,
                          .sessionEpoch = initial->sessionEpoch,
                          .commandId = domain::CommandId{1},
                      },
                  .sources =
                      {
                          OpenComparisonSource{
                              .path = "C:/media/same.mp4",
                              .role = domain::ComparisonRole::kPrediction,
                              .displayName = "a",
                          },
                          OpenComparisonSource{
                              .path = "C:/media/same.mp4",
                              .role = domain::ComparisonRole::kPrediction,
                              .displayName = "b",
                          },
                      },
              }),
              PortSubmitResult::Accepted);
    // Same-path dedup: only one probe is submitted for the shared path.
    ASSERT_TRUE(probe->waitForRequestCount(1U));
    const auto descriptor =
        makeDescriptor("C:/media/same.mp4", domain::MediaExtent{.width = 320, .height = 180});
    ASSERT_TRUE(probe->postCompleted(0U, descriptor));
    ASSERT_TRUE(probe->postSucceeded(0U));
    ASSERT_EQ(coordinator->postCritical(ApplicationEvent{GraphicsDeviceReady{
                  .context = GraphicsEventContext{.deviceGeneration = domain::DeviceGeneration{2}},
              }}),
              EventPostResult::Accepted);
    ASSERT_TRUE(waitUntil([&coordinator] {
        return coordinator->snapshot()->deviceGeneration == domain::DeviceGeneration{2};
    }));
    EXPECT_EQ(provider->openRequestCount(), 1U);
    const auto open = provider->openRequest();
    ASSERT_TRUE(open.has_value());
    EXPECT_EQ(open->sources[0].descriptor.normalizedPath,
              open->sources[1].descriptor.normalizedPath);
    ASSERT_TRUE(provider->postOpenSucceeded(*open));
    ASSERT_TRUE(provider->waitForFrameRequestCount(1U));
    const auto frame = provider->frameRequest(0U);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(domain::FrameId{0})));
    ASSERT_TRUE(render->waitForPublishedCount(1U));
    ASSERT_TRUE(provider->postFrameSucceeded(*frame));
    presentPublished(coordinator, render, 0U);
    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);
}

TEST(PlaybackCoordinatorTests, ASecondOpenSupersedesInFlightProbes) {
    const auto probe = std::make_shared<FakeMediaProbe>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render, probe);
    const auto initial = coordinator->snapshot();
    const CommandContext firstContext{
        .sessionId = initial->sessionId,
        .sessionEpoch = initial->sessionEpoch,
        .commandId = domain::CommandId{1},
    };
    ASSERT_EQ(coordinator->submit(OpenComparisonCommand{
                  .context = firstContext,
                  .sources =
                      {
                          OpenComparisonSource{.path = "C:/media/first-a.mp4",
                                               .role = domain::ComparisonRole::kPrediction,
                                               .displayName = "a"},
                          OpenComparisonSource{.path = "C:/media/first-b.mp4",
                                               .role = domain::ComparisonRole::kPrediction,
                                               .displayName = "b"},
                      },
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(probe->waitForRequestCount(2U));

    ASSERT_EQ(coordinator->submit(OpenComparisonCommand{
                  .context =
                      CommandContext{
                          .sessionId = initial->sessionId,
                          .sessionEpoch = initial->sessionEpoch,
                          .commandId = domain::CommandId{2},
                      },
                  .sources =
                      {
                          OpenComparisonSource{.path = "C:/media/second-a.mp4",
                                               .role = domain::ComparisonRole::kPrediction,
                                               .displayName = "a"},
                          OpenComparisonSource{.path = "C:/media/second-b.mp4",
                                               .role = domain::ComparisonRole::kPrediction,
                                               .displayName = "b"},
                      },
              }),
              PortSubmitResult::Accepted);

    // The superseded command completes as canceled and both in-flight probes are canceled;
    // the new open starts its own probe set for the new paths.
    ASSERT_TRUE(probe->waitForCancelCount(2U));
    ASSERT_TRUE(probe->waitForRequestCount(4U));
    std::vector<CommandTerminal> terminals;
    ASSERT_TRUE(waitUntil([&] {
        auto drained = coordinator->takeCompletedCommands();
        if (drained.empty()) {
            return false;
        }
        terminals = std::move(drained);
        return true;
    }));
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().context, firstContext);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Canceled);
    EXPECT_EQ(probe->request(2U)->sourcePath, std::filesystem::path{"C:/media/second-a.mp4"});
    EXPECT_EQ(probe->request(3U)->sourcePath, std::filesystem::path{"C:/media/second-b.mp4"});
}

TEST(PlaybackCoordinatorTests, ReplacementOpenMapsDisplayedMediaTimeOntoNewCanonicalRate) {
    const auto probe = std::make_shared<FakeMediaProbe>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render, probe);
    openReady(coordinator, provider, render);

    const auto ready = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(SeekFrameCommand{
                  .context =
                      CommandContext{
                          .sessionId = ready->sessionId,
                          .sessionEpoch = ready->sessionEpoch,
                          .commandId = domain::CommandId{2},
                      },
                  .frameId = domain::FrameId{9},
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const auto seek = provider->frameRequest(1U);
    ASSERT_TRUE(seek.has_value());
    ASSERT_TRUE(provider->postFrameReady(*seek, makeFrameSet(domain::FrameId{9})));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    ASSERT_TRUE(provider->postFrameSucceeded(*seek));
    presentPublished(coordinator, render, 1U);
    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);
    ASSERT_EQ(coordinator->snapshot()->displayedFrame, domain::FrameId{9});

    const auto positioned = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(OpenComparisonCommand{
                  .context =
                      CommandContext{
                          .sessionId = positioned->sessionId,
                          .sessionEpoch = positioned->sessionEpoch,
                          .commandId = domain::CommandId{3},
                      },
                  .sources =
                      {
                          OpenComparisonSource{.path = "C:/media/replacement-a.mp4",
                                               .role = domain::ComparisonRole::kReference,
                                               .displayName = "a"},
                          OpenComparisonSource{.path = "C:/media/replacement-b.mp4",
                                               .role = domain::ComparisonRole::kPrediction,
                                               .displayName = "b"},
                      },
                  .preserveDisplayedTime = true,
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(probe->waitForRequestCount(2U));
    ASSERT_TRUE(
        probe->postCompleted(0U,
                             makeDescriptor("C:/media/replacement-a.mp4",
                                            domain::MediaExtent{.width = 320, .height = 180},
                                            120,
                                            60)));
    ASSERT_TRUE(probe->postSucceeded(0U));
    ASSERT_TRUE(
        probe->postCompleted(1U,
                             makeDescriptor("C:/media/replacement-b.mp4",
                                            domain::MediaExtent{.width = 320, .height = 180},
                                            120,
                                            60)));
    ASSERT_TRUE(probe->postSucceeded(1U));

    ASSERT_TRUE(provider->waitForOpenRequestCount(2U));
    const auto replacement = provider->openRequest(1U);
    ASSERT_TRUE(replacement.has_value());
    ASSERT_TRUE(provider->postOpenSucceeded(*replacement));
    ASSERT_TRUE(provider->waitForFrameRequestCount(3U));
    const auto resumed = provider->frameRequest(2U);
    ASSERT_TRUE(resumed.has_value());
    EXPECT_EQ(resumed->frameId, domain::FrameId{18});
}

TEST(PlaybackCoordinatorTests, ProbeFailureCancelsSiblingAndPreservesReadyReplacementSession) {
    const auto probe = std::make_shared<FakeMediaProbe>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render, probe);
    openReady(coordinator, provider, render);
    const auto ready = coordinator->snapshot();

    ASSERT_EQ(coordinator->submit(OpenComparisonCommand{
                  .context =
                      CommandContext{
                          .sessionId = ready->sessionId,
                          .sessionEpoch = ready->sessionEpoch,
                          .commandId = domain::CommandId{2},
                      },
                  .sources =
                      {
                          OpenComparisonSource{
                              .path = "C:/media/replacement-a.mp4",
                              .role = domain::ComparisonRole::kPrediction,
                              .displayName = "a",
                          },
                          OpenComparisonSource{
                              .path = "C:/media/replacement-b.mp4",
                              .role = domain::ComparisonRole::kPrediction,
                              .displayName = "b",
                          },
                      },
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(probe->waitForRequestCount(2U));
    const domain::MediaError failure =
        domain::makeMediaError(domain::MediaErrorCode::kMediaProbeFailed,
                               domain::MediaOperation::kMediaProbe,
                               domain::SourceId{0},
                               true,
                               "Source 0 could not be inspected.");
    ASSERT_TRUE(probe->postFailed(0U, failure));
    const auto terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Failed);
    ASSERT_TRUE(probe->waitForCancelCount(2U));
    const auto canceled = probe->canceledContexts();
    const auto requestB = probe->request(1U);
    ASSERT_TRUE(requestB.has_value());
    EXPECT_NE(std::find(canceled.begin(), canceled.end(), requestB->context), canceled.end());

    const auto after = coordinator->snapshot();
    EXPECT_EQ(after->sessionState, domain::SessionState::kReady);
    EXPECT_EQ(after->displayedFrame, domain::FrameId{0});
    EXPECT_EQ(provider->openRequestCount(), 1U);
    EXPECT_TRUE(render->clearContexts().empty());

    ASSERT_TRUE(
        probe->postCompleted(1U,
                             makeDescriptor("C:/media/replacement-b.mp4",
                                            domain::MediaExtent{.width = 160, .height = 90})));
    ASSERT_TRUE(probe->postSucceeded(1U));
    ASSERT_EQ(coordinator->postCritical(ApplicationEvent{GraphicsDeviceReady{
                  .context = GraphicsEventContext{.deviceGeneration = domain::DeviceGeneration{3}},
              }}),
              EventPostResult::Accepted);
    ASSERT_TRUE(waitUntil([&coordinator] {
        return coordinator->snapshot()->deviceGeneration == domain::DeviceGeneration{3};
    }));
    EXPECT_EQ(provider->openRequestCount(), 1U);
}

TEST(PlaybackCoordinatorTests, DifferingFrameCountsSucceedWithCanonicalFrameCount) {
    const auto probe = std::make_shared<FakeMediaProbe>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render, probe);
    const auto initial = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(OpenComparisonCommand{
                  .context =
                      CommandContext{
                          .sessionId = initial->sessionId,
                          .sessionEpoch = initial->sessionEpoch,
                          .commandId = domain::CommandId{1},
                      },
                  .sources =
                      {
                          OpenComparisonSource{
                              .path = "C:/media/a.mp4",
                              .role = domain::ComparisonRole::kPrediction,
                              .displayName = "a",
                          },
                          OpenComparisonSource{
                              .path = "C:/media/b.mp4",
                              .role = domain::ComparisonRole::kPrediction,
                              .displayName = "b",
                          },
                      },
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(probe->waitForRequestCount(2U));
    // Source 0 has 12 frames; source 1 has 11 frames. The new validator treats frame-count
    // differences as a CompatibilityReport finding, not a fatal failure.
    ASSERT_TRUE(probe->postCompleted(
        0U, makeDescriptor("C:/media/a.mp4", domain::MediaExtent{.width = 320, .height = 180})));
    ASSERT_TRUE(probe->postSucceeded(0U));
    ASSERT_TRUE(probe->postCompleted(
        1U, makeDescriptor("C:/media/b.mp4", domain::MediaExtent{.width = 160, .height = 90}, 11)));
    ASSERT_TRUE(probe->postSucceeded(1U));
    ASSERT_TRUE(provider->waitForOpenRequestCount(1U));
    const auto open = provider->openRequest();
    ASSERT_TRUE(open.has_value());
    ASSERT_TRUE(provider->postOpenSucceeded(*open));
    ASSERT_TRUE(provider->waitForFrameRequestCount(1U));
    const auto frame = provider->frameRequest(0U);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(domain::FrameId{0})));
    ASSERT_TRUE(render->waitForPublishedCount(1U));
    ASSERT_TRUE(provider->postFrameSucceeded(*frame));
    presentPublished(coordinator, render, 0U);
    const auto terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);
    // canonicalFrameCount comes from the canonical source (first source, id 0, frame count 12).
    EXPECT_EQ(coordinator->snapshot()->canonicalFrameCount, 12U);
    EXPECT_EQ(coordinator->snapshot()->sessionState, domain::SessionState::kReady);
    EXPECT_EQ(provider->openRequestCount(), 1U);
}

TEST(PlaybackCoordinatorTests, ProbeAdmissionFailureCancelsAcceptedSiblingExactlyOnce) {
    const auto probe = std::make_shared<FakeMediaProbe>();
    probe->setSubmitResult(domain::SourceId{1}, PortSubmitResult::Busy);
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render, probe);
    const auto initial = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(OpenComparisonCommand{
                  .context =
                      CommandContext{
                          .sessionId = initial->sessionId,
                          .sessionEpoch = initial->sessionEpoch,
                          .commandId = domain::CommandId{1},
                      },
                  .sources =
                      {
                          OpenComparisonSource{
                              .path = "C:/media/a.mp4",
                              .role = domain::ComparisonRole::kPrediction,
                              .displayName = "a",
                          },
                          OpenComparisonSource{
                              .path = "C:/media/b.mp4",
                              .role = domain::ComparisonRole::kPrediction,
                              .displayName = "b",
                          },
                      },
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(probe->waitForRequestCount(1U));
    const auto terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Busy);
    EXPECT_EQ(provider->openRequestCount(), 0U);
    EXPECT_EQ(coordinator->snapshot()->sessionState, domain::SessionState::kError);
}

TEST(PlaybackCoordinatorTests, PostValidationOpenFailureRestoresPriorReadySession) {
    const auto probe = std::make_shared<FakeMediaProbe>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render, probe);
    openReady(coordinator, provider, render);
    const auto ready = coordinator->snapshot();
    ASSERT_EQ(render->publishedCount(), 1U);

    ASSERT_EQ(coordinator->submit(OpenComparisonCommand{
                  .context =
                      CommandContext{
                          .sessionId = ready->sessionId,
                          .sessionEpoch = ready->sessionEpoch,
                          .commandId = domain::CommandId{2},
                      },
                  .sources =
                      {
                          OpenComparisonSource{
                              .path = "C:/media/new-a.mp4",
                              .role = domain::ComparisonRole::kPrediction,
                              .displayName = "a",
                          },
                          OpenComparisonSource{
                              .path = "C:/media/new-b.mp4",
                              .role = domain::ComparisonRole::kPrediction,
                              .displayName = "b",
                          },
                      },
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(probe->waitForRequestCount(2U));
    ASSERT_TRUE(probe->postCompleted(
        0U,
        makeDescriptor("C:/media/new-a.mp4", domain::MediaExtent{.width = 320, .height = 180})));
    ASSERT_TRUE(probe->postSucceeded(0U));
    ASSERT_TRUE(probe->postCompleted(
        1U, makeDescriptor("C:/media/new-b.mp4", domain::MediaExtent{.width = 160, .height = 90})));
    ASSERT_TRUE(probe->postSucceeded(1U));
    ASSERT_TRUE(provider->waitForOpenRequestCount(2U));
    EXPECT_TRUE(render->clearContexts().empty());
    EXPECT_EQ(coordinator->snapshot()->sessionState, domain::SessionState::kLoading);
    const auto replacementOpen = provider->openRequest(1U);
    ASSERT_TRUE(replacementOpen.has_value());
    const domain::MediaError openFailure =
        domain::makeMediaError(domain::MediaErrorCode::kMediaDecodeFailed,
                               domain::MediaOperation::kMediaDecode,
                               std::nullopt,
                               true,
                               "The replacement decoder could not be opened.");
    ASSERT_TRUE(provider->postOpenFailed(*replacementOpen, openFailure));

    ASSERT_TRUE(provider->waitForOpenRequestCount(3U));
    const auto rollbackOpen = provider->openRequest(2U);
    ASSERT_TRUE(rollbackOpen.has_value());
    ASSERT_EQ(rollbackOpen->sources.size(), 2U);
    EXPECT_EQ(rollbackOpen->sources[0].descriptor.normalizedPath, "a.mp4");
    EXPECT_EQ(rollbackOpen->sources[1].descriptor.normalizedPath, "b.mp4");
    ASSERT_TRUE(provider->postOpenSucceeded(*rollbackOpen));

    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const auto rollbackFrame = provider->frameRequest(1U);
    ASSERT_TRUE(rollbackFrame.has_value());
    EXPECT_EQ(rollbackFrame->frameId, domain::FrameId{0});
    ASSERT_TRUE(provider->postFrameReady(*rollbackFrame, makeFrameSet(rollbackFrame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    ASSERT_TRUE(provider->postFrameSucceeded(*rollbackFrame));
    presentPublished(coordinator, render, 1U);

    const auto terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Failed);
    ASSERT_TRUE(terminals.front().error.has_value());
    EXPECT_EQ(terminals.front().error->technicalDetail, openFailure.technicalDetail);

    const auto restored = coordinator->snapshot();
    ASSERT_EQ(restored->sessionState, domain::SessionState::kReady);
    ASSERT_TRUE(restored->displayedFrame.has_value());
    EXPECT_EQ(*restored->displayedFrame, domain::FrameId{0});
    ASSERT_TRUE(restored->validatedComparison);
    ASSERT_EQ(restored->validatedComparison->sourceCount(), 2U);
    EXPECT_EQ(restored->validatedComparison->sources()[0].descriptor.normalizedPath, "a.mp4");
    EXPECT_EQ(restored->validatedComparison->sources()[1].descriptor.normalizedPath, "b.mp4");
    ASSERT_TRUE(restored->lastError.has_value());
    EXPECT_EQ(restored->lastError->technicalDetail, openFailure.technicalDetail);
    EXPECT_EQ(render->publishedCount(), 2U);
}

TEST(PlaybackCoordinatorTests, RequiresProviderTerminalAndPresentationInEitherOrder) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler);
    const auto initial = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(OpenDirectComparisonCommand{
                  .context =
                      CommandContext{
                          .sessionId = initial->sessionId,
                          .sessionEpoch = initial->sessionEpoch,
                          .commandId = domain::CommandId{1},
                      },
                  .sources =
                      {
                          domain::ComparisonSource{
                              .id = 0,
                              .role = domain::ComparisonRole::kPrediction,
                              .descriptor = makeDescriptor(
                                  "a.mp4", domain::MediaExtent{.width = 320, .height = 180}),
                              .displayName = "a",
                          },
                          domain::ComparisonSource{
                              .id = 1,
                              .role = domain::ComparisonRole::kPrediction,
                              .descriptor = makeDescriptor(
                                  "b.mp4", domain::MediaExtent{.width = 160, .height = 90}),
                              .displayName = "b",
                          },
                      },
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForOpenRequestCount(1U));
    const auto open = provider->openRequest();
    ASSERT_TRUE(open.has_value());
    ASSERT_TRUE(provider->postOpenSucceeded(*open));
    ASSERT_TRUE(provider->waitForFrameRequestCount(1U));
    const auto frame = provider->frameRequest(0U);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(domain::FrameId{0})));
    ASSERT_TRUE(render->waitForPublishedCount(1U));
    ASSERT_TRUE(scheduler->waitForScheduleCount(1U));

    presentPublished(coordinator, render, 0U);
    const auto loading = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(FirstFrameCommand{
                  .context =
                      CommandContext{
                          .sessionId = loading->sessionId,
                          .sessionEpoch = loading->sessionEpoch,
                          .commandId = domain::CommandId{2},
                      },
              }),
              PortSubmitResult::Accepted);
    const auto busy = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(busy.size(), 1U);
    EXPECT_EQ(busy.front().context.commandId, domain::CommandId{2});
    EXPECT_EQ(busy.front().outcome, CommandOutcome::Busy);
    EXPECT_FALSE(coordinator->snapshot()->displayedFrame.has_value());

    ASSERT_TRUE(provider->postFrameSucceeded(*frame));
    const auto completed = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(completed.size(), 1U);
    EXPECT_EQ(completed.front().context.commandId, domain::CommandId{1});
    EXPECT_EQ(completed.front().outcome, CommandOutcome::Succeeded);
    EXPECT_EQ(coordinator->snapshot()->displayedFrame, domain::FrameId{0});
    EXPECT_TRUE(scheduler->waitForCancelCount(1U));

    ASSERT_TRUE(scheduler->fire(0U));
    ASSERT_EQ(coordinator->postCritical(ApplicationEvent{GraphicsDeviceReady{
                  .context = GraphicsEventContext{.deviceGeneration = domain::DeviceGeneration{4}},
              }}),
              EventPostResult::Accepted);
    ASSERT_TRUE(waitUntil([&coordinator] {
        return coordinator->snapshot()->deviceGeneration == domain::DeviceGeneration{4};
    }));
    EXPECT_TRUE(coordinator->takeCompletedCommands().empty());
    EXPECT_EQ(coordinator->snapshot()->displayedFrame, domain::FrameId{0});
}

TEST(PlaybackCoordinatorTests, AcceptsProviderSuccessBeforeTheFrameSet) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    const auto initial = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(OpenDirectComparisonCommand{
                  .context =
                      CommandContext{
                          .sessionId = initial->sessionId,
                          .sessionEpoch = initial->sessionEpoch,
                          .commandId = domain::CommandId{1},
                      },
                  .sources =
                      {
                          domain::ComparisonSource{
                              .id = 0,
                              .role = domain::ComparisonRole::kPrediction,
                              .descriptor = makeDescriptor(
                                  "a.mp4", domain::MediaExtent{.width = 320, .height = 180}),
                              .displayName = "a",
                          },
                          domain::ComparisonSource{
                              .id = 1,
                              .role = domain::ComparisonRole::kPrediction,
                              .descriptor = makeDescriptor(
                                  "b.mp4", domain::MediaExtent{.width = 160, .height = 90}),
                              .displayName = "b",
                          },
                      },
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForOpenRequestCount(1U));
    const auto open = provider->openRequest();
    ASSERT_TRUE(open.has_value());
    ASSERT_TRUE(provider->postOpenSucceeded(*open));
    ASSERT_TRUE(provider->waitForFrameRequestCount(1U));
    const auto frame = provider->frameRequest(0U);
    ASSERT_TRUE(frame.has_value());

    ASSERT_TRUE(provider->postFrameSucceeded(*frame));
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(frame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(1U));
    EXPECT_TRUE(coordinator->takeCompletedCommands().empty());
    presentPublished(coordinator, render, 0U);
    const auto terminal = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminal.size(), 1U);
    EXPECT_EQ(terminal.front().outcome, CommandOutcome::Succeeded);
    EXPECT_EQ(coordinator->snapshot()->displayedFrame, domain::FrameId{0});
}

TEST(PlaybackCoordinatorTests, ExactDeadlineBoundsAProviderThatNeverPublishesAFrameSet) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler);
    const auto initial = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(OpenDirectComparisonCommand{
                  .context =
                      CommandContext{
                          .sessionId = initial->sessionId,
                          .sessionEpoch = initial->sessionEpoch,
                          .commandId = domain::CommandId{1},
                      },
                  .sources =
                      {
                          domain::ComparisonSource{
                              .id = 0,
                              .role = domain::ComparisonRole::kPrediction,
                              .descriptor = makeDescriptor(
                                  "a.mp4", domain::MediaExtent{.width = 320, .height = 180}),
                              .displayName = "a",
                          },
                          domain::ComparisonSource{
                              .id = 1,
                              .role = domain::ComparisonRole::kPrediction,
                              .descriptor = makeDescriptor(
                                  "b.mp4", domain::MediaExtent{.width = 160, .height = 90}),
                              .displayName = "b",
                          },
                      },
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForOpenRequestCount(1U));
    const auto open = provider->openRequest();
    ASSERT_TRUE(open.has_value());
    ASSERT_TRUE(provider->postOpenSucceeded(*open));
    ASSERT_TRUE(provider->waitForFrameRequestCount(1U));
    ASSERT_TRUE(scheduler->waitForScheduleCount(1U));

    ASSERT_TRUE(scheduler->fire(0U));
    const auto terminal = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminal.size(), 1U);
    ASSERT_TRUE(terminal.front().error.has_value());
    EXPECT_EQ(terminal.front().error->code, domain::MediaErrorCode::kFramePresentationTimedOut);
    EXPECT_EQ(coordinator->snapshot()->sessionState, domain::SessionState::kError);
    EXPECT_FALSE(coordinator->snapshot()->displayedFrame.has_value());
    EXPECT_TRUE(provider->waitForCancelCount(1U));
}

TEST(PlaybackCoordinatorTests, PresentationTimeoutKeepsPreviousFrameAndRejectsLateAck) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler, clock);
    ASSERT_EQ(coordinator->postCritical(ApplicationEvent{GraphicsDeviceReady{
                  .context = GraphicsEventContext{.deviceGeneration = domain::DeviceGeneration{2}},
              }}),
              EventPostResult::Accepted);
    ASSERT_TRUE(waitUntil([&coordinator] { return coordinator->snapshot()->graphicsReady; }));
    openReady(coordinator, provider, render);

    const auto ready = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(SeekFrameCommand{
                  .context =
                      CommandContext{
                          .sessionId = ready->sessionId,
                          .sessionEpoch = ready->sessionEpoch,
                          .commandId = domain::CommandId{2},
                      },
                  .frameId = domain::FrameId{7},
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const auto frame = provider->frameRequest(1U);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(frame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    ASSERT_TRUE(provider->postFrameSucceeded(*frame));
    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    const auto lateAcknowledgement = render->presentation(1U);
    ASSERT_TRUE(lateAcknowledgement.has_value());
    const auto deadline = scheduler->request(1U);
    ASSERT_TRUE(deadline.has_value());
    EXPECT_EQ(deadline->due, clock->now() + 5s);
    EXPECT_TRUE(coordinator->takeCompletedCommands().empty());

    ASSERT_TRUE(scheduler->fire(1U));
    const auto timedOut = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(timedOut.size(), 1U);
    ASSERT_TRUE(timedOut.front().error.has_value());
    EXPECT_EQ(timedOut.front().error->code, domain::MediaErrorCode::kFramePresentationTimedOut);
    EXPECT_EQ(timedOut.front().error->operation, domain::MediaOperation::kFramePresentation);
    EXPECT_EQ(coordinator->snapshot()->sessionState, domain::SessionState::kReady);
    EXPECT_EQ(coordinator->snapshot()->displayedFrame, domain::FrameId{0});
    ASSERT_TRUE(render->waitForClearCount(1U));
    EXPECT_FALSE(render->consumePresentation(1U).has_value());
    ASSERT_EQ(render->clearContexts().front(), frame->context.playback);

    ASSERT_EQ(coordinator->postCritical(ApplicationEvent{*lateAcknowledgement}),
              EventPostResult::Accepted);
    const domain::MediaError deviceLost =
        domain::makeMediaError(domain::MediaErrorCode::kGraphicsDeviceLost,
                               domain::MediaOperation::kGraphicsInitialization,
                               std::nullopt,
                               true,
                               "Device generation barrier after the late acknowledgement.");
    ASSERT_EQ(coordinator->postCritical(ApplicationEvent{GraphicsDeviceLost{
                  .context = GraphicsEventContext{.deviceGeneration = domain::DeviceGeneration{2}},
                  .error = deviceLost,
              }}),
              EventPostResult::Accepted);
    ASSERT_TRUE(waitUntil([&coordinator] {
        const auto snapshot = coordinator->snapshot();
        return snapshot->lastError.has_value() &&
               snapshot->lastError->code == domain::MediaErrorCode::kGraphicsDeviceLost;
    }));
    EXPECT_TRUE(coordinator->takeCompletedCommands().empty());
    EXPECT_EQ(coordinator->snapshot()->displayedFrame, domain::FrameId{0});
}

TEST(PlaybackCoordinatorTests, ShutdownInvalidatesAPublishedPendingExactFrameSet) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    auto coordinator = makeCoordinator(provider, render);
    openReady(coordinator, provider, render);

    const auto ready = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(SeekFrameCommand{
                  .context =
                      CommandContext{
                          .sessionId = ready->sessionId,
                          .sessionEpoch = ready->sessionEpoch,
                          .commandId = domain::CommandId{2},
                      },
                  .frameId = domain::FrameId{7},
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const auto frame = provider->frameRequest(1U);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(frame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    ASSERT_TRUE(render->presentation(1U).has_value());
    coordinator.reset();

    ASSERT_TRUE(render->waitForClearCount(1U));
    EXPECT_EQ(render->clearContexts().front(), frame->context.playback);
    EXPECT_FALSE(render->consumePresentation(1U).has_value());
}

TEST(PlaybackCoordinatorTests, GraphicsLossInvalidatesAPublishedPendingExactFrameSet) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_EQ(coordinator->postCritical(ApplicationEvent{GraphicsDeviceReady{
                  .context = GraphicsEventContext{.deviceGeneration = domain::DeviceGeneration{2}},
              }}),
              EventPostResult::Accepted);
    ASSERT_TRUE(waitUntil([&coordinator] { return coordinator->snapshot()->graphicsReady; }));
    openReady(coordinator, provider, render);

    const auto ready = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(SeekFrameCommand{
                  .context =
                      CommandContext{
                          .sessionId = ready->sessionId,
                          .sessionEpoch = ready->sessionEpoch,
                          .commandId = domain::CommandId{2},
                      },
                  .frameId = domain::FrameId{7},
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const auto frame = provider->frameRequest(1U);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(frame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));

    const domain::MediaError deviceLost =
        domain::makeMediaError(domain::MediaErrorCode::kGraphicsDeviceLost,
                               domain::MediaOperation::kGraphicsInitialization,
                               std::nullopt,
                               true,
                               "Device loss while an exact frame set awaited presentation.");
    ASSERT_EQ(coordinator->postCritical(ApplicationEvent{GraphicsDeviceLost{
                  .context = GraphicsEventContext{.deviceGeneration = domain::DeviceGeneration{2}},
                  .error = deviceLost,
              }}),
              EventPostResult::Accepted);

    const auto terminal = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminal.size(), 1U);
    EXPECT_EQ(terminal.front().outcome, CommandOutcome::Failed);
    ASSERT_TRUE(render->waitForClearCount(1U));
    EXPECT_EQ(render->clearContexts().front(), frame->context.playback);
    EXPECT_FALSE(render->consumePresentation(1U).has_value());
    EXPECT_FALSE(coordinator->snapshot()->graphicsReady);
    EXPECT_EQ(coordinator->snapshot()->displayedFrame, domain::FrameId{0});
}

TEST(PlaybackCoordinatorTests, ClampsAllEndpointCommandsForAOneFrameComparisonSet) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    const auto initial = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(OpenDirectComparisonCommand{
                  .context =
                      CommandContext{
                          .sessionId = initial->sessionId,
                          .sessionEpoch = initial->sessionEpoch,
                          .commandId = domain::CommandId{1},
                      },
                  .sources =
                      {
                          domain::ComparisonSource{
                              .id = 0,
                              .role = domain::ComparisonRole::kPrediction,
                              .descriptor = makeDescriptor(
                                  "a.mp4", domain::MediaExtent{.width = 320, .height = 180}, 1),
                              .displayName = "a",
                          },
                          domain::ComparisonSource{
                              .id = 1,
                              .role = domain::ComparisonRole::kPrediction,
                              .descriptor = makeDescriptor(
                                  "b.mp4", domain::MediaExtent{.width = 160, .height = 90}, 1),
                              .displayName = "b",
                          },
                      },
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForOpenRequestCount(1U));
    const auto open = provider->openRequest();
    ASSERT_TRUE(open.has_value());
    ASSERT_TRUE(provider->postOpenSucceeded(*open));
    ASSERT_TRUE(provider->waitForFrameRequestCount(1U));
    const auto openingFrame = provider->frameRequest(0U);
    ASSERT_TRUE(openingFrame.has_value());
    ASSERT_TRUE(provider->postFrameReady(*openingFrame, makeFrameSet(domain::FrameId{0})));
    ASSERT_TRUE(render->waitForPublishedCount(1U));
    ASSERT_TRUE(provider->postFrameSucceeded(*openingFrame));
    presentPublished(coordinator, render, 0U);
    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);
    ASSERT_EQ(coordinator->snapshot()->canonicalFrameCount, 1U);

    // -1 at the only frame is a boundary Busy (reverse stream), not a clamped Exact seek to 0.
    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .delta = -1,
              }),
              PortSubmitResult::Accepted);
    const auto reverseBusy = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(reverseBusy.size(), 1U);
    EXPECT_EQ(reverseBusy.front().outcome, CommandOutcome::Busy);
    EXPECT_EQ(provider->frameRequestCount(), 1U);
    // A +1 step on a one-frame set is at the canonical boundary: Busy, no frame request.
    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    const auto boundaryBusy = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(boundaryBusy.size(), 1U);
    EXPECT_EQ(boundaryBusy.front().context.commandId, domain::CommandId{3});
    EXPECT_EQ(boundaryBusy.front().outcome, CommandOutcome::Busy);
    EXPECT_EQ(provider->frameRequestCount(), 1U);
    markGraphicsReady(coordinator);
    ASSERT_EQ(coordinator->submit(PlayCommand{
                  .context = commandContext(coordinator, domain::CommandId{4}),
              }),
              PortSubmitResult::Accepted);
    const auto rejectedPlay = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(rejectedPlay.size(), 1U);
    EXPECT_EQ(rejectedPlay.front().outcome, CommandOutcome::Failed);
    ASSERT_TRUE(rejectedPlay.front().error.has_value());
    EXPECT_EQ(rejectedPlay.front().error->code, domain::MediaErrorCode::kInvalidArgument);
    EXPECT_EQ(provider->frameRequestCount(), 1U);
    EXPECT_EQ(coordinator->snapshot()->playbackState, domain::PlaybackState::kPaused);
}

TEST(PlaybackCoordinatorTests, OpensDirectSourcesOnlyAfterACompleteSetAndProviderTerminal) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);

    openReady(coordinator, provider, render);

    const std::shared_ptr<const SessionSnapshot> snapshot = coordinator->snapshot();
    ASSERT_NE(snapshot, nullptr);
    EXPECT_TRUE(snapshot->isConsistent());
    EXPECT_EQ(snapshot->sessionState, domain::SessionState::kReady);
    EXPECT_EQ(snapshot->playbackState, domain::PlaybackState::kPaused);
    EXPECT_EQ(snapshot->displayedFrame, domain::FrameId{0});
    EXPECT_FALSE(snapshot->requestedFrame.has_value());
    EXPECT_EQ(snapshot->canonicalFrameCount, 12U);
}

TEST(PlaybackCoordinatorTests, ReleasesCpuFrameResourcesAfterPresentationCommit) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<IdentityOnlyRenderChannel>();
    const auto coordinator = PlaybackCoordinator::create(
        domain::SessionId{91},
        PlaybackCoordinator::Dependencies{
            .mediaProbe = std::make_shared<FakeMediaProbe>(),
            .directFrameProvider = provider,
            .deadlineScheduler = std::make_shared<FakeDeadlineScheduler>(),
            .clock = std::make_shared<FakeSteadyClock>(),
            .renderChannel = render,
        });
    ASSERT_NE(coordinator, nullptr);

    const auto initial = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(OpenDirectComparisonCommand{
                  .context =
                      CommandContext{
                          .sessionId = initial->sessionId,
                          .sessionEpoch = initial->sessionEpoch,
                          .commandId = domain::CommandId{1},
                      },
                  .sources =
                      {
                          domain::ComparisonSource{
                              .id = 0,
                              .role = domain::ComparisonRole::kPrediction,
                              .descriptor = makeDescriptor(
                                  "a.mp4", domain::MediaExtent{.width = 320, .height = 180}),
                              .displayName = "a",
                          },
                          domain::ComparisonSource{
                              .id = 1,
                              .role = domain::ComparisonRole::kPrediction,
                              .descriptor = makeDescriptor(
                                  "b.mp4", domain::MediaExtent{.width = 160, .height = 90}),
                              .displayName = "b",
                          },
                      },
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForOpenRequestCount(1U));
    const auto open = provider->openRequest();
    ASSERT_TRUE(open.has_value());
    ASSERT_TRUE(provider->postOpenSucceeded(*open));
    ASSERT_TRUE(provider->waitForFrameRequestCount(1U));
    const auto frame = provider->frameRequest(0U);
    ASSERT_TRUE(frame.has_value());

    auto observable = makeObservableFrameSet(frame->frameId);
    const std::weak_ptr<const IFrameResource> source0 = observable.source0;
    const std::weak_ptr<const IFrameResource> source1 = observable.source1;
    ASSERT_TRUE(provider->postFrameReady(*frame, std::move(observable.set)));
    ASSERT_TRUE(render->waitForPublishedCount(1U));
    EXPECT_FALSE(source0.expired());
    EXPECT_FALSE(source1.expired());

    ASSERT_TRUE(provider->postFrameSucceeded(*frame));
    const auto presented = render->presentation(0U);
    ASSERT_TRUE(presented.has_value());
    ASSERT_EQ(coordinator->postCritical(ApplicationEvent{*presented}), EventPostResult::Accepted);
    const auto terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);

    EXPECT_TRUE(source0.expired());
    EXPECT_TRUE(source1.expired());
    const auto snapshot = coordinator->snapshot();
    EXPECT_EQ(snapshot->sessionState, domain::SessionState::kReady);
    EXPECT_EQ(snapshot->displayedFrame, frame->frameId);
}

TEST(PlaybackCoordinatorTests, SeekAdvancesGenerationCancelsOldScopeAndPublishesTheExactFrameSet) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);

    const std::shared_ptr<const SessionSnapshot> beforeSeek = coordinator->snapshot();
    ASSERT_NE(beforeSeek, nullptr);
    ASSERT_EQ(coordinator->submit(SeekFrameCommand{
                  .context =
                      CommandContext{
                          .sessionId = beforeSeek->sessionId,
                          .sessionEpoch = beforeSeek->sessionEpoch,
                          .commandId = domain::CommandId{2},
                      },
                  .frameId = domain::FrameId{7},
              }),
              PortSubmitResult::Accepted);

    ASSERT_TRUE(provider->waitForCancelCount(1U));
    const std::vector<PlaybackRequestContext> canceled = provider->canceledContexts();
    ASSERT_EQ(canceled.size(), 1U);
    EXPECT_EQ(canceled.front().playbackGeneration, beforeSeek->playbackGeneration);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const std::optional<FrameRequest> request = provider->frameRequest(1U);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->frameId, domain::FrameId{7});
    EXPECT_EQ(request->context.playback.playbackGeneration, domain::PlaybackGeneration{2});
    ASSERT_TRUE(provider->postFrameReady(*request, makeFrameSet(request->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    ASSERT_TRUE(provider->postFrameSucceeded(*request));
    presentPublished(coordinator, render, 1U);

    const std::vector<CommandTerminal> terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().context.commandId, domain::CommandId{2});
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);
    const std::shared_ptr<const SessionSnapshot> afterSeek = coordinator->snapshot();
    ASSERT_NE(afterSeek, nullptr);
    EXPECT_TRUE(afterSeek->isConsistent());
    EXPECT_EQ(afterSeek->displayedFrame, domain::FrameId{7});
    EXPECT_EQ(afterSeek->playbackGeneration, domain::PlaybackGeneration{2});
}

TEST(PlaybackCoordinatorTests, RapidForwardStepsEnqueueAndPresentEveryFrame) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);
    const domain::PlaybackGeneration generationAtStart =
        coordinator->snapshot()->playbackGeneration;

    // Two +1 steps in flight: the stream must present every intermediate frame (0 -> 1 -> 2)
    // instead of superseding the first seek, and advance the generation exactly once for the whole
    // run (v1.5: no per-frame generation storm).
    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);

    ASSERT_TRUE(provider->waitForFrameRequestCount(3U));
    const std::optional<FrameRequest> first = provider->frameRequest(1U);
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(first->priority, FrameRequestPriority::Sequential);
    const std::optional<FrameRequest> prepared = provider->frameRequest(2U);
    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(prepared->frameId, domain::FrameId{2});

    // Present frame 1: the first step's command succeeds and the prepared frame 2 is promoted.
    ASSERT_TRUE(provider->postFrameReady(*first, makeFrameSet(first->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    ASSERT_TRUE(provider->postFrameSucceeded(*first));
    presentPublished(coordinator, render, 1U);

    std::vector<CommandTerminal> terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().context.commandId, domain::CommandId{2});
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);
    EXPECT_EQ(coordinator->snapshot()->displayedFrame, domain::FrameId{1});

    // Present frame 2: the second step's command succeeds. Both intermediate frames were shown.
    ASSERT_TRUE(provider->postFrameReady(*prepared, makeFrameSet(prepared->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(3U));
    ASSERT_TRUE(provider->postFrameSucceeded(*prepared));
    presentPublished(coordinator, render, 2U);

    terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().context.commandId, domain::CommandId{3});
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);
    const std::shared_ptr<const SessionSnapshot> after = coordinator->snapshot();
    ASSERT_NE(after, nullptr);
    EXPECT_TRUE(after->isConsistent());
    EXPECT_EQ(after->displayedFrame, domain::FrameId{2});
    EXPECT_EQ(after->requestedFrame, std::nullopt);
    // The whole two-step run advanced the generation exactly once.
    EXPECT_EQ(after->playbackGeneration.value() - generationAtStart.value(), 1U);
}

TEST(PlaybackCoordinatorTests, StepDuringPlaybackPausesBeforeSeekingTheTarget) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler);
    ASSERT_NE(coordinator, nullptr);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(
                  PlayCommand{.context = commandContext(coordinator, domain::CommandId{2})}),
              PortSubmitResult::Accepted);
    std::vector<CommandTerminal> terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);
    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    ASSERT_TRUE(waitUntil([&coordinator] {
        return coordinator->snapshot()->playbackState == domain::PlaybackState::kPlaying;
    }));

    // Frame navigation during playback pauses first, then seeks the explicit target.
    ASSERT_EQ(coordinator->submit(SeekFrameCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
                  .frameId = domain::FrameId{5},
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(waitUntil([&coordinator] {
        return coordinator->snapshot()->playbackState == domain::PlaybackState::kSeeking;
    }));
    EXPECT_EQ(coordinator->snapshot()->requestedFrame, domain::FrameId{5});

    std::optional<FrameRequest> seekRequest;
    ASSERT_TRUE(waitUntil([&] {
        for (std::size_t index = provider->frameRequestCount(); index-- > 0U;) {
            const std::optional<FrameRequest> candidate = provider->frameRequest(index);
            if (candidate.has_value() && candidate->frameId == domain::FrameId{5}) {
                seekRequest = candidate;
                return true;
            }
        }
        return false;
    }));
    ASSERT_TRUE(seekRequest.has_value());
    ASSERT_TRUE(provider->postFrameReady(*seekRequest, makeFrameSet(seekRequest->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    ASSERT_TRUE(provider->postFrameSucceeded(*seekRequest));
    presentPublished(coordinator, render, 1U);

    terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().context.commandId, domain::CommandId{3});
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);
    const std::shared_ptr<const SessionSnapshot> after = coordinator->snapshot();
    ASSERT_NE(after, nullptr);
    EXPECT_TRUE(after->isConsistent());
    EXPECT_EQ(after->playbackState, domain::PlaybackState::kPaused);
    EXPECT_EQ(after->displayedFrame, domain::FrameId{5});
}

TEST(PlaybackCoordinatorTests, SuppressesDuplicateCommandsWithoutASecondProviderRequest) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);

    const std::shared_ptr<const SessionSnapshot> beforeSeek = coordinator->snapshot();
    ASSERT_NE(beforeSeek, nullptr);
    const SeekFrameCommand duplicate{
        .context =
            CommandContext{
                .sessionId = beforeSeek->sessionId,
                .sessionEpoch = beforeSeek->sessionEpoch,
                .commandId = domain::CommandId{2},
            },
        .frameId = domain::FrameId{1},
    };
    ASSERT_EQ(coordinator->submit(duplicate), PortSubmitResult::Accepted);
    ASSERT_EQ(coordinator->submit(duplicate), PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const std::optional<FrameRequest> firstRequest = provider->frameRequest(1U);
    ASSERT_TRUE(firstRequest.has_value());
    ASSERT_TRUE(provider->postFrameReady(*firstRequest, makeFrameSet(firstRequest->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    ASSERT_TRUE(provider->postFrameSucceeded(*firstRequest));
    presentPublished(coordinator, render, 1U);

    const std::vector<CommandTerminal> firstTerminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(firstTerminals.size(), 1U);
    EXPECT_EQ(firstTerminals.front().context.commandId, domain::CommandId{2});
    EXPECT_EQ(firstTerminals.front().outcome, CommandOutcome::Succeeded);

    const std::shared_ptr<const SessionSnapshot> afterFirst = coordinator->snapshot();
    ASSERT_NE(afterFirst, nullptr);
    ASSERT_EQ(coordinator->submit(SeekFrameCommand{
                  .context =
                      CommandContext{
                          .sessionId = afterFirst->sessionId,
                          .sessionEpoch = afterFirst->sessionEpoch,
                          .commandId = domain::CommandId{3},
                      },
                  .frameId = domain::FrameId{2},
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(3U));
    EXPECT_EQ(provider->frameRequestCount(), 3U);
    const std::optional<FrameRequest> secondRequest = provider->frameRequest(2U);
    ASSERT_TRUE(secondRequest.has_value());
    ASSERT_TRUE(provider->postFrameReady(*secondRequest, makeFrameSet(secondRequest->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(3U));
    ASSERT_TRUE(provider->postFrameSucceeded(*secondRequest));
    presentPublished(coordinator, render, 2U);
    const std::vector<CommandTerminal> secondTerminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(secondTerminals.size(), 1U);
    EXPECT_EQ(secondTerminals.front().context.commandId, domain::CommandId{3});
    EXPECT_EQ(secondTerminals.front().outcome, CommandOutcome::Succeeded);
}

TEST(PlaybackCoordinatorTests, InvalidReplacementPreservesTheDisplayedReadySession) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);

    const std::shared_ptr<const SessionSnapshot> ready = coordinator->snapshot();
    ASSERT_NE(ready, nullptr);
    // Submit a replacement with an invalid descriptor (zero extent). The ComparisonValidator
    // rejects individually invalid descriptors as a fatal error, preserving the existing session.
    ASSERT_EQ(coordinator
                  ->submit(
                      OpenDirectComparisonCommand{
                          .context =
                              CommandContext{
                                  .sessionId = ready->sessionId,
                                  .sessionEpoch = ready->sessionEpoch,
                                  .commandId = domain::CommandId{2},
                              },
                          .sources =
                              {
                                  domain::ComparisonSource{
                                      .id = 0,
                                      .role = domain::ComparisonRole::kPrediction,
                                      .descriptor =
                                          domain::MediaDescriptor{
                                              .normalizedPath = "new-a.mp4",
                                              .extent =
                                                  domain::MediaExtent{.width = 0, .height = 0},
                                              .frameRate = makeRate(),
                                              .frameCount =
                                                  domain::FrameCountInfo{
                                                      .value = 12,
                                                      .origin = domain::FrameCountOrigin::kReported,
                                                  },
                                              .duration = domain::MediaTime{400000},
                                              .codecId = "h264",
                                              .pixelFormatId = "yuv420p",
                                              .bitDepth = 8,
                                              .timingConfidence = domain::TimingConfidence::kDeclaredCfr,
                                          },
                                      .displayName = "new-a",
                                  },
                                  domain::ComparisonSource{
                                      .id = 1,
                                      .role = domain::ComparisonRole::kPrediction,
                                      .descriptor = makeDescriptor(
                                          "new-b.mp4",
                                          domain::MediaExtent{.width = 160, .height = 90}),
                                      .displayName = "new-b",
                                  },
                              },
                      }),
              PortSubmitResult::Accepted);

    const std::vector<CommandTerminal> terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Failed);
    ASSERT_TRUE(terminals.front().error.has_value());
    EXPECT_EQ(terminals.front().error->code, domain::MediaErrorCode::kInvalidMediaDescriptor);
    EXPECT_EQ(provider->frameRequestCount(), 1U);
    const std::shared_ptr<const SessionSnapshot> after = coordinator->snapshot();
    ASSERT_NE(after, nullptr);
    EXPECT_TRUE(after->isConsistent());
    EXPECT_EQ(after->sessionState, domain::SessionState::kReady);
    EXPECT_EQ(after->displayedFrame, domain::FrameId{0});
    ASSERT_TRUE(after->lastError.has_value());
    EXPECT_EQ(after->lastError->code, domain::MediaErrorCode::kInvalidMediaDescriptor);
    EXPECT_TRUE(render->clearContexts().empty());
}

TEST(PlaybackCoordinatorTests, CloseClearsTheRenderChannelAndAdvancesTheSessionEpoch) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);

    const std::shared_ptr<const SessionSnapshot> ready = coordinator->snapshot();
    ASSERT_NE(ready, nullptr);
    ASSERT_EQ(coordinator->submit(CloseSessionCommand{
                  .context =
                      CommandContext{
                          .sessionId = ready->sessionId,
                          .sessionEpoch = ready->sessionEpoch,
                          .commandId = domain::CommandId{2},
                      },
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForCloseRequestCount(1U));
    const std::optional<FrameProviderCloseRequest> close = provider->closeRequest();
    ASSERT_TRUE(close.has_value());
    ASSERT_TRUE(render->waitForClearCount(1U));
    const std::vector<PlaybackRequestContext> clears = render->clearContexts();
    ASSERT_EQ(clears.size(), 1U);
    EXPECT_EQ(clears.front(), close->context);
    ASSERT_TRUE(provider->postCloseSucceeded(*close));

    const std::vector<CommandTerminal> terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);
    const std::shared_ptr<const SessionSnapshot> after = coordinator->snapshot();
    ASSERT_NE(after, nullptr);
    EXPECT_TRUE(after->isConsistent());
    EXPECT_EQ(after->sessionState, domain::SessionState::kEmpty);
    EXPECT_EQ(after->sessionEpoch, domain::SessionEpoch{2});
    EXPECT_EQ(after->playbackGeneration, domain::PlaybackGeneration{2});
}

TEST(PlaybackCoordinatorTests, RejectsAnObsoleteEpochWithoutDispatchingProviderWork) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(SeekFrameCommand{
                  .context =
                      CommandContext{
                          .sessionId = domain::SessionId{91},
                          .sessionEpoch = domain::SessionEpoch{0},
                          .commandId = domain::CommandId{2},
                      },
                  .frameId = domain::FrameId{1},
              }),
              PortSubmitResult::Accepted);
    const std::vector<CommandTerminal> terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Canceled);
    EXPECT_EQ(provider->frameRequestCount(), 1U);
}

TEST(PlaybackCoordinatorTests, PreservesCriticalTerminalsWhenTheBoundedQueueIsFull) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<BlockingRenderChannel>();
    const auto coordinator = PlaybackCoordinator::create(
        domain::SessionId{91},
        PlaybackCoordinator::Dependencies{
            .mediaProbe = std::make_shared<FakeMediaProbe>(),
            .directFrameProvider = provider,
            .deadlineScheduler = std::make_shared<FakeDeadlineScheduler>(),
            .clock = std::make_shared<FakeSteadyClock>(),
            .renderChannel = render,
        });
    ASSERT_NE(coordinator, nullptr);

    ASSERT_EQ(coordinator->submit(OpenDirectComparisonCommand{
                  .context =
                      CommandContext{
                          .sessionId = domain::SessionId{91},
                          .sessionEpoch = domain::SessionEpoch{0},
                          .commandId = domain::CommandId{1},
                      },
                  .sources =
                      {
                          domain::ComparisonSource{
                              .id = 0,
                              .role = domain::ComparisonRole::kPrediction,
                              .descriptor = makeDescriptor(
                                  "a.mp4", domain::MediaExtent{.width = 320, .height = 180}),
                              .displayName = "a",
                          },
                          domain::ComparisonSource{
                              .id = 1,
                              .role = domain::ComparisonRole::kPrediction,
                              .descriptor = makeDescriptor(
                                  "b.mp4", domain::MediaExtent{.width = 160, .height = 90}),
                              .displayName = "b",
                          },
                      },
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForOpenRequestCount(1U));
    const std::optional<FrameProviderOpenRequest> open = provider->openRequest();
    ASSERT_TRUE(open.has_value());
    ASSERT_TRUE(provider->postOpenSucceeded(*open));
    ASSERT_TRUE(provider->waitForFrameRequestCount(1U));
    const std::optional<FrameRequest> frame = provider->frameRequest(0U);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(frame->frameId)));
    ASSERT_TRUE(render->waitUntilPublishBlocked());

    for (std::size_t index = 0U; index < kCriticalEventCapacity; ++index) {
        ASSERT_EQ(coordinator->postCritical(ignoredCriticalEvent()), EventPostResult::Accepted);
    }

    std::thread releaser([&render] {
        std::this_thread::sleep_for(100ms);
        render->release();
    });
    EXPECT_EQ(coordinator->postCritical(ignoredCriticalEvent()), EventPostResult::Accepted);
    releaser.join();

    ASSERT_TRUE(provider->postFrameSucceeded(*frame));
    ASSERT_EQ(coordinator->postCritical(ApplicationEvent{FrameSetPresented{
                  .context = frame->context,
                  .frameId = frame->frameId,
              }}),
              EventPostResult::Accepted);
    const std::vector<CommandTerminal> terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);
}

TEST(PlaybackCoordinatorTests, VfrProbePayloadOpensWithSharedTimelineAndReachesReady) {
    const auto probe = std::make_shared<FakeMediaProbe>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render, probe);

    const std::shared_ptr<const domain::FrameTimeline> timeline = makeVfrTimeline();
    ASSERT_NE(timeline, nullptr);
    const domain::MediaDescriptor descriptorA =
        makeVfrDescriptor("C:/media/a.mp4",
                          domain::MediaExtent{.width = 320, .height = 180},
                          4,
                          domain::MediaTime{69800});
    const domain::MediaDescriptor descriptorB =
        makeDescriptor("C:/media/b.mp4", domain::MediaExtent{.width = 160, .height = 90}, 4);

    openVfrReady(coordinator, probe, provider, render, timeline, descriptorA, descriptorB);

    // The provider open request carries the VFR CanonicalTimeline that the probe published.
    const std::optional<FrameProviderOpenRequest> open = provider->openRequest();
    ASSERT_TRUE(open.has_value());
    EXPECT_TRUE(domain::isVariableFrameRate(open->timeline));
    EXPECT_EQ(std::get<std::shared_ptr<const domain::FrameTimeline>>(open->timeline).get(),
              timeline.get());

    const std::shared_ptr<const SessionSnapshot> ready = coordinator->snapshot();
    EXPECT_EQ(ready->sessionState, domain::SessionState::kReady);
    EXPECT_EQ(ready->displayedFrame, domain::FrameId{0});
    EXPECT_EQ(ready->canonicalFrameCount, 4U);
}

TEST(PlaybackCoordinatorTests, VfrContinuousPlayUsesAbsoluteNonuniformDeadlinesRelativeToAnchor) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto probe = std::make_shared<FakeMediaProbe>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render, probe, scheduler, clock);

    const std::shared_ptr<const domain::FrameTimeline> timeline = makeVfrTimeline();
    ASSERT_NE(timeline, nullptr);
    const domain::MediaDescriptor descriptorA =
        makeVfrDescriptor("C:/media/a.mp4",
                          domain::MediaExtent{.width = 320, .height = 180},
                          4,
                          domain::MediaTime{69800});
    const domain::MediaDescriptor descriptorB =
        makeDescriptor("C:/media/b.mp4", domain::MediaExtent{.width = 160, .height = 90}, 4);

    openVfrReady(coordinator, probe, provider, render, timeline, descriptorA, descriptorB);
    markGraphicsReady(coordinator);

    const std::shared_ptr<const SessionSnapshot> ready = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(PlayCommand{
                  .context =
                      CommandContext{
                          .sessionId = ready->sessionId,
                          .sessionEpoch = ready->sessionEpoch,
                          .commandId = domain::CommandId{2},
                      },
              }),
              PortSubmitResult::Accepted);
    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);

    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    const std::optional<DeadlineRequest> firstCadence = scheduler->request(1U);
    ASSERT_TRUE(firstCadence.has_value());
    EXPECT_EQ(firstCadence->due, clock->now() + 1000us);
    EXPECT_EQ(coordinator->snapshot()->playbackState, domain::PlaybackState::kPlaying);
    EXPECT_EQ(provider->frameRequestCount(), 1U);

    clock->set(firstCadence->due);
    ASSERT_TRUE(scheduler->fire(1U));
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const std::optional<FrameRequest> frame = provider->frameRequest(1U);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->frameId, domain::FrameId{1});
    EXPECT_EQ(frame->priority, FrameRequestPriority::Sequential);

    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(frame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    presentPublished(coordinator, render, 1U);
    ASSERT_TRUE(provider->postFrameSucceeded(*frame));
    ASSERT_TRUE(waitUntil(
        [&coordinator] { return coordinator->snapshot()->displayedFrame == domain::FrameId{1}; }));

    ASSERT_TRUE(scheduler->waitForScheduleCount(4U));
    const std::optional<DeadlineRequest> secondCadence = scheduler->request(3U);
    ASSERT_TRUE(secondCadence.has_value());
    EXPECT_EQ(secondCadence->due, firstCadence->due + 35000us);
    EXPECT_EQ(provider->frameRequestCount(), 3U);
    EXPECT_EQ(coordinator->snapshot()->displayedFrame, domain::FrameId{1});
    EXPECT_FALSE(coordinator->snapshot()->requestedFrame.has_value());
    EXPECT_EQ(coordinator->snapshot()->playbackState, domain::PlaybackState::kPlaying);
}

TEST(PlaybackCoordinatorTests,
     VfrSlowDecodeCatchUpSkipsCompleteFrameSetAndPreservesFinalSemantics) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto probe = std::make_shared<FakeMediaProbe>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render, probe, scheduler, clock);

    const std::shared_ptr<const domain::FrameTimeline> timeline = makeVfrTimeline();
    ASSERT_NE(timeline, nullptr);
    const domain::MediaDescriptor descriptorA =
        makeVfrDescriptor("C:/media/a.mp4",
                          domain::MediaExtent{.width = 320, .height = 180},
                          4,
                          domain::MediaTime{69800});
    const domain::MediaDescriptor descriptorB =
        makeDescriptor("C:/media/b.mp4", domain::MediaExtent{.width = 160, .height = 90}, 4);

    openVfrReady(coordinator, probe, provider, render, timeline, descriptorA, descriptorB);
    markGraphicsReady(coordinator);
    requireRealTimeContinuity(coordinator, domain::CommandId{901});

    const std::shared_ptr<const SessionSnapshot> ready = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(PlayCommand{
                  .context =
                      CommandContext{
                          .sessionId = ready->sessionId,
                          .sessionEpoch = ready->sessionEpoch,
                          .commandId = domain::CommandId{2},
                      },
              }),
              PortSubmitResult::Accepted);
    EXPECT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);

    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    const std::optional<DeadlineRequest> firstCadence = scheduler->request(1U);
    ASSERT_TRUE(firstCadence.has_value());

    clock->set(firstCadence->due + 2500ms);
    ASSERT_TRUE(scheduler->fire(1U));
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const std::optional<FrameRequest> catchUp = provider->frameRequest(1U);
    ASSERT_TRUE(catchUp.has_value());
    EXPECT_EQ(catchUp->frameId, domain::FrameId{3});
    EXPECT_EQ(catchUp->priority, FrameRequestPriority::Sequential);

    ASSERT_TRUE(provider->postFrameReady(*catchUp, makeFrameSet(catchUp->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    presentPublished(coordinator, render, 1U);
    ASSERT_TRUE(provider->postFrameSucceeded(*catchUp));
    ASSERT_TRUE(waitUntil(
        [&coordinator] { return coordinator->snapshot()->displayedFrame == domain::FrameId{3}; }));
    EXPECT_EQ(coordinator->snapshot()->playbackState, domain::PlaybackState::kPaused);
}

// ---------------------------------------------------------------------------
// v1.5 interactive forward-step stream tests (plan section 16, application unit)
// ---------------------------------------------------------------------------

namespace {

// Drives one interactive step frame to the render ACK on the worker thread: FrameSetReady, render
// publish, provider success, and render ACK. `renderIndex` is the render-published-set index to
// ACK. Returns once the render ACK has been posted; the caller verifies the command terminal
// separately (the terminal is emitted after the ACK commits the frame).
void presentInteractiveStep(const std::shared_ptr<PlaybackCoordinator>& coordinator,
                            const std::shared_ptr<FakeFrameProvider>& provider,
                            const std::shared_ptr<FakeRenderChannel>& render,
                            const std::optional<FrameRequest>& request,
                            const std::size_t renderIndex) {
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(provider->postFrameReady(*request, makeFrameSet(request->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(renderIndex + 1U));
    ASSERT_TRUE(provider->postFrameSucceeded(*request));
    presentPublished(coordinator, render, renderIndex);
}

} // namespace

TEST(PlaybackCoordinatorTests, ForwardStepUsesOneGenerationAcrossAdjacentCommands) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);
    const domain::PlaybackGeneration generationAtStart =
        coordinator->snapshot()->playbackGeneration;

    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);

    // Both in-flight frames (current + prepared) must carry the single-advanced generation.
    ASSERT_TRUE(provider->waitForFrameRequestCount(3U));
    for (std::size_t index = 1U; index < 3U; ++index) {
        const std::optional<FrameRequest> request = provider->frameRequest(index);
        ASSERT_TRUE(request.has_value());
        EXPECT_EQ(request->context.playback.playbackGeneration,
                  domain::PlaybackGeneration{generationAtStart.value() + 1U});
    }
}

TEST(PlaybackCoordinatorTests, ForwardStepDoesNotSupersedeInFlightSequentialFrame) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);

    // The first command must not be canceled (no supersede), and the provider scope must not have
    // been canceled for the first frame.
    std::vector<CommandTerminal> terminals = coordinator->takeCompletedCommands();
    for (const auto& terminal : terminals) {
        EXPECT_NE(terminal.outcome, CommandOutcome::Canceled);
    }
    // At most one provider-context cancellation may have occurred (the stream-start generation
    // bump), not a per-frame cancel/reseek storm.
    EXPECT_LE(provider->canceledContexts().size(), 1U);

    // The second step's target (frame 2) must have been submitted as a prepared Sequential frame.
    ASSERT_TRUE(provider->waitForFrameRequestCount(3U));
    const std::optional<FrameRequest> prepared = provider->frameRequest(2U);
    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(prepared->frameId, domain::FrameId{2});
    EXPECT_EQ(prepared->priority, FrameRequestPriority::Sequential);
}

TEST(PlaybackCoordinatorTests, ForwardStepQueuesBoundedLookahead) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);

    // Submit +1 steps until one reports Busy. The lookahead bound is 3 frames ahead of the
    // displayed frame (0): targets 1, 2, 3 queue; target 4 is Busy.
    bool sawBusy = false;
    domain::CommandId id{2};
    for (int i = 0; i < 10 && !sawBusy; ++i) {
        ASSERT_EQ(coordinator->submit(
                      StepFramesCommand{.context = commandContext(coordinator, id), .delta = 1}),
                  PortSubmitResult::Accepted);
        const std::vector<CommandTerminal> terminals = coordinator->takeCompletedCommands();
        for (const auto& terminal : terminals) {
            if (terminal.outcome == CommandOutcome::Busy) {
                sawBusy = true;
            }
        }
        id = domain::CommandId{id.value() + 1U};
        std::this_thread::yield();
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(sawBusy);
}

// Every intermediate frame must be presented, never skipped.
TEST(PlaybackCoordinatorTests, ForwardStepPresentsEveryIntermediateFrame) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);

    constexpr std::size_t stepCount = 5U;
    // Submit and present incrementally: the input lookahead bound only allows a limited lead over
    // the displayed frame, so each +1 is submitted as the previous frame presents and the window
    // slides forward. Every intermediate frame must be presented, never skipped.
    std::vector<domain::FrameId> presentedIds;
    for (std::size_t i = 0U; i < stepCount; ++i) {
        const domain::FrameId target{static_cast<std::int64_t>(1 + i)};
        ASSERT_EQ(coordinator->submit(StepFramesCommand{
                      .context = commandContext(coordinator, domain::CommandId{2 + i}),
                      .delta = 1,
                  }),
                  PortSubmitResult::Accepted);
        std::size_t foundIndex = 0U;
        ASSERT_TRUE(waitUntil([&provider, target, &foundIndex] {
            const std::size_t count = provider->frameRequestCount();
            for (std::size_t index = count; index-- > 0U;) {
                const auto r = provider->frameRequest(index);
                if (r.has_value() && r->frameId == target) {
                    foundIndex = index;
                    return true;
                }
            }
            return false;
        }));
        const std::optional<FrameRequest> request = provider->frameRequest(foundIndex);
        ASSERT_TRUE(request.has_value());
        presentInteractiveStep(coordinator, provider, render, request, foundIndex);
        EXPECT_TRUE(waitUntil(
            [&coordinator, target] { return coordinator->snapshot()->displayedFrame == target; }));
        presentedIds.push_back(coordinator->snapshot()->displayedFrame.value());
    }
    const std::vector<domain::FrameId> expected{domain::FrameId{1},
                                                domain::FrameId{2},
                                                domain::FrameId{3},
                                                domain::FrameId{4},
                                                domain::FrameId{5}};
    EXPECT_EQ(presentedIds, expected);
}

// Each step command completes with Succeeded only after its frame is actually presented.
TEST(PlaybackCoordinatorTests, ForwardStepCompletesEachCommandAfterPresentation) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);

    // Before any presentation, neither command may have succeeded.
    for (const auto& terminal : coordinator->takeCompletedCommands()) {
        EXPECT_NE(terminal.outcome, CommandOutcome::Succeeded);
    }

    // Wait for the two step frames to be submitted (current + prepared), then present them.
    ASSERT_TRUE(provider->waitForFrameRequestCount(3U));
    const std::optional<FrameRequest> first = provider->frameRequest(1U);
    ASSERT_TRUE(first.has_value());
    presentInteractiveStep(coordinator, provider, render, first, 1U);
    EXPECT_TRUE(waitUntil(
        [&coordinator] { return coordinator->snapshot()->displayedFrame == domain::FrameId{1}; }));

    std::vector<CommandTerminal> terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().context.commandId, domain::CommandId{2});
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);

    // After frame 1 commits, frame 2 (the prepared frame) is promoted to current and re-presented.
    ASSERT_TRUE(provider->waitForFrameRequestCount(3U));
    const std::optional<FrameRequest> second = provider->frameRequest(2U);
    ASSERT_TRUE(second.has_value());
    presentInteractiveStep(coordinator, provider, render, second, 2U);
    EXPECT_TRUE(waitUntil(
        [&coordinator] { return coordinator->snapshot()->displayedFrame == domain::FrameId{2}; }));

    terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().context.commandId, domain::CommandId{3});
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);
}

// A backward step while the forward stream is active tears the stream down and starts a reverse
// interactive stream (FrameRequestPriority::Reverse), not a per-step Exact-seek storm.
TEST(PlaybackCoordinatorTests, BackwardStepStopsForwardStreamAndUsesReverse) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));

    // Present the forward step so the displayed frame is 1, then reverse.
    const auto forward = provider->frameRequest(1U);
    ASSERT_TRUE(forward.has_value());
    EXPECT_EQ(forward->priority, FrameRequestPriority::Sequential);
    presentInteractiveStep(coordinator, provider, render, forward, 1U);
    ASSERT_TRUE(waitUntil(
        [&coordinator] { return coordinator->snapshot()->displayedFrame == domain::FrameId{1}; }));

    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
                  .delta = -1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(3U));
    const std::optional<FrameRequest> backward = provider->frameRequest(2U);
    ASSERT_TRUE(backward.has_value());
    EXPECT_EQ(backward->frameId, domain::FrameId{0});
    EXPECT_EQ(backward->priority, FrameRequestPriority::Reverse);

    presentInteractiveStep(coordinator, provider, render, backward, 2U);
    EXPECT_TRUE(waitUntil(
        [&coordinator] { return coordinator->snapshot()->displayedFrame == domain::FrameId{0}; }));
}

// A random seek discontinues the forward stream.
TEST(PlaybackCoordinatorTests, RandomSeekStopsForwardStream) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));

    ASSERT_EQ(coordinator->submit(SeekFrameCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
                  .frameId = domain::FrameId{4},
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(waitUntil([&coordinator] {
        return coordinator->snapshot()->playbackState == domain::PlaybackState::kSeeking;
    }));
    ASSERT_TRUE(provider->waitForFrameRequestCount(3U));
    const std::optional<FrameRequest> seek = provider->frameRequest(2U);
    ASSERT_TRUE(seek.has_value());
    EXPECT_EQ(seek->frameId, domain::FrameId{4});
    EXPECT_EQ(seek->priority, FrameRequestPriority::Exact);

    presentInteractiveStep(coordinator, provider, render, seek, 1U);
    EXPECT_TRUE(waitUntil(
        [&coordinator] { return coordinator->snapshot()->displayedFrame == domain::FrameId{4}; }));
}

// Play discontinues the forward stream.
TEST(PlaybackCoordinatorTests, PlayStopsForwardStepStream) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const domain::PlaybackGeneration generationAfterStep =
        coordinator->snapshot()->playbackGeneration;

    ASSERT_EQ(coordinator->submit(
                  PlayCommand{.context = commandContext(coordinator, domain::CommandId{3})}),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(waitUntil([&coordinator] {
        return coordinator->snapshot()->playbackState == domain::PlaybackState::kPlaying;
    }));
    // Play supersedes the interactive stream and advances the generation.
    EXPECT_GT(coordinator->snapshot()->playbackGeneration.value(), generationAfterStep.value());
}

// At the canonical end boundary, a +1 reports Busy and submits no frame past the last frame.
TEST(PlaybackCoordinatorTests, ForwardStepAtEndDoesNotQueuePastBoundary) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);

    // Step all the way to the final frame (openReady uses 12 frames; displayed is 0).
    for (std::size_t i = 0U; i < 11U; ++i) {
        ASSERT_EQ(coordinator->submit(StepFramesCommand{
                      .context = commandContext(coordinator, domain::CommandId{2 + i}),
                      .delta = 1,
                  }),
                  PortSubmitResult::Accepted);
        ASSERT_TRUE(provider->waitForFrameRequestCount(2U + i));
        const std::optional<FrameRequest> request = provider->frameRequest(1U + i);
        ASSERT_TRUE(request.has_value());
        presentInteractiveStep(coordinator, provider, render, request, 1U + i);
    }
    EXPECT_TRUE(waitUntil(
        [&coordinator] { return coordinator->snapshot()->displayedFrame == domain::FrameId{11}; }));

    // One more +1 is at the boundary: it must report Busy and not submit a 12th frame.
    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{100}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    // Drain any outstanding terminals from the 11 successful steps, then wait for the boundary
    // command's Busy terminal.
    std::vector<CommandTerminal> drained = coordinator->takeCompletedCommands();
    ASSERT_TRUE(waitUntil([&coordinator, &drained] {
        for (const auto& t : coordinator->takeCompletedCommands()) {
            drained.push_back(t);
        }
        return std::any_of(drained.begin(), drained.end(), [](const CommandTerminal& t) {
            return t.context.commandId == domain::CommandId{100};
        });
    }));
    const auto it = std::find_if(drained.begin(), drained.end(), [](const CommandTerminal& t) {
        return t.context.commandId == domain::CommandId{100};
    });
    ASSERT_NE(it, drained.end());
    EXPECT_EQ(it->outcome, CommandOutcome::Busy);
    // Open frame (1) plus the 11 step frames that were submitted and presented: no 12th frame is
    // queued past the boundary.
    EXPECT_EQ(provider->frameRequestCount(), 12U);
}

// A provider failure while the stream is active ends the stream and fails pending commands.
TEST(PlaybackCoordinatorTests, ProviderFailureFailsPendingStepCommands) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(3U));
    const std::optional<FrameRequest> current = provider->frameRequest(1U);
    ASSERT_TRUE(current.has_value());

    // Fail the current frame's provider request.
    ASSERT_TRUE(
        provider->postFrameFailed(*current,
                                  domain::makeMediaError(domain::MediaErrorCode::kMediaDecodeFailed,
                                                         domain::MediaOperation::kMediaDecode,
                                                         std::nullopt,
                                                         false,
                                                         "test decode failure.")));

    std::vector<CommandTerminal> terminals = waitForTerminals(coordinator, 2U);
    ASSERT_EQ(terminals.size(), 2U);
    for (const auto& terminal : terminals) {
        EXPECT_EQ(terminal.outcome, CommandOutcome::Failed);
    }
    EXPECT_TRUE(waitUntil([&coordinator] {
        return coordinator->snapshot()->playbackState == domain::PlaybackState::kPaused;
    }));
}

// Plan M1.2: a real provider failure must surface the error via the snapshot's lastError so the UI
// banner can show it (failInteractiveStepRun sets lastError). Before the cancel/fail split this was
// conflated with cancels that must NOT set lastError.
TEST(PlaybackCoordinatorTests, ProviderFailureSurfacesStepStreamError) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const std::optional<FrameRequest> current = provider->frameRequest(1U);
    ASSERT_TRUE(current.has_value());

    // Fail the current frame's provider request with a non-recoverable decode error.
    const domain::MediaError failure =
        domain::makeMediaError(domain::MediaErrorCode::kMediaDecodeFailed,
                               domain::MediaOperation::kMediaDecode,
                               std::nullopt,
                               false,
                               "decode failure");
    ASSERT_TRUE(provider->postFrameFailed(*current, failure));

    // The stream must tear down and lastError must surface the provider error on the snapshot.
    EXPECT_TRUE(waitUntil([&coordinator] {
        return coordinator->snapshot()->playbackState == domain::PlaybackState::kPaused &&
               coordinator->snapshot()->lastError.has_value();
    }));
    ASSERT_TRUE(coordinator->snapshot()->lastError.has_value());
    EXPECT_EQ(coordinator->snapshot()->lastError->code, domain::MediaErrorCode::kMediaDecodeFailed);
    EXPECT_EQ(coordinator->snapshot()->lastError->technicalDetail, "decode failure");

    const std::vector<CommandTerminal> terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Failed);
}

// Plan M1.2: superseding the stream with a backward step (Previous) is a normal navigation cancel —
// it must NOT set lastError. This is the distinguishing invariant from
// ProviderFailureSurfacesStepStreamError.
TEST(PlaybackCoordinatorTests, SupersedingStepDoesNotSurfaceError) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));

    // A backward step supersedes the forward stream. lastError must remain unset.
    const std::size_t requestsBeforeSeek = provider->frameRequestCount();
    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
                  .delta = -1,
              }),
              PortSubmitResult::Accepted);

    // Reverse stream chains from the unpresented forward target (1) down to frame 0.
    ASSERT_TRUE(provider->waitForFrameRequestCount(requestsBeforeSeek + 1U));
    const auto seek = provider->frameRequest(requestsBeforeSeek);
    ASSERT_TRUE(seek.has_value());
    ASSERT_EQ(seek->frameId, domain::FrameId{0});
    EXPECT_EQ(seek->priority, FrameRequestPriority::Reverse);
    ASSERT_TRUE(provider->postFrameReady(*seek, makeFrameSet(seek->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    ASSERT_TRUE(provider->postFrameSucceeded(*seek));
    presentPublished(coordinator, render, 1U);

    EXPECT_TRUE(waitUntil([&coordinator] {
        return coordinator->snapshot()->playbackState == domain::PlaybackState::kPaused;
    }));
    EXPECT_FALSE(coordinator->snapshot()->lastError.has_value());
}

// Device loss invalidates the forward stream.
TEST(PlaybackCoordinatorTests, DeviceLossInvalidatesForwardStepStream) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));

    const domain::MediaError deviceLost =
        domain::makeMediaError(domain::MediaErrorCode::kGraphicsDeviceLost,
                               domain::MediaOperation::kGraphicsInitialization,
                               std::nullopt,
                               false,
                               "device lost");
    ASSERT_EQ(coordinator->postCritical(ApplicationEvent{GraphicsDeviceLost{
                  .context = GraphicsEventContext{.deviceGeneration = domain::DeviceGeneration{3}},
                  .error = deviceLost,
              }}),
              EventPostResult::Accepted);
    ASSERT_TRUE(waitUntil([&coordinator] {
        return !coordinator->snapshot()->graphicsReady &&
               coordinator->snapshot()->playbackState == domain::PlaybackState::kPaused;
    }));
    EXPECT_EQ(coordinator->snapshot()->deviceGeneration, domain::DeviceGeneration{3});
}

// Shutdown cancels the forward stream boundedly, completing pending commands as Closed.
// Regression test for the "no successor became current, but commands are still queued" path in
// commitInteractiveStepFrameIfComplete: when the current frame commits with no prepared successor
// and the next queued frame's submit is rejected by the provider, the queued step command must be
// completed (Canceled) — never silently dropped, which would leave the UI's frame-pending indicator
// dangling and strand the run.
TEST(PlaybackCoordinatorTests, ForwardStepNoSuccessorSubmitRejectionCompletesQueuedCommand) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);

    // Refuse every submit for frame 2: the prepared slot can never fill, so when frame 1 commits
    // the stream must re-submit frame 2 from the queued-commands branch — and that re-submit is the
    // one that gets rejected here.
    provider->setRejectedFrameIds({domain::FrameId{2}});

    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{4}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);

    // Frame 0 (open) and frame 1 (the first step) are submitted; frame 2 is refused, so the count
    // holds at 2. Wait until both enqueues have been processed (their frame-2 submits rejected) so
    // the prepared slot is empty but the queue holds commands 3 and 4 when frame 1 commits.
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    ASSERT_TRUE(waitUntil([&coordinator] {
        const auto snapshot = coordinator->snapshot();
        return snapshot->requestedFrame == domain::FrameId{3};
    }));
    const std::optional<FrameRequest> firstStep = provider->frameRequest(1U);
    ASSERT_TRUE(firstStep.has_value());
    ASSERT_EQ(firstStep->frameId, domain::FrameId{1});

    // Present frame 1: it commits, there is no prepared successor, and the branch re-submits
    // frame 2 — which the provider rejects.
    ASSERT_TRUE(provider->postFrameReady(*firstStep, makeFrameSet(firstStep->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    ASSERT_TRUE(provider->postFrameSucceeded(*firstStep));
    presentPublished(coordinator, render, 1U);

    // Command 2 (frame 1) succeeds; commands 3 and 4 (queued, then re-subjected to the rejection)
    // must both be completed as Canceled — not lost.
    std::vector<CommandTerminal> terminals = waitForTerminals(coordinator, 3U);
    ASSERT_EQ(terminals.size(), 3U);
    std::map<domain::CommandId, CommandOutcome> outcomes;
    for (const auto& terminal : terminals) {
        outcomes.emplace(terminal.context.commandId, terminal.outcome);
    }
    ASSERT_EQ(outcomes[domain::CommandId{2}], CommandOutcome::Succeeded);
    ASSERT_EQ(outcomes[domain::CommandId{3}], CommandOutcome::Canceled);
    ASSERT_EQ(outcomes[domain::CommandId{4}], CommandOutcome::Canceled);
    EXPECT_TRUE(waitUntil([&coordinator] {
        return coordinator->snapshot()->playbackState == domain::PlaybackState::kPaused;
    }));
    EXPECT_EQ(coordinator->snapshot()->requestedFrame, std::nullopt);
}

// A frame can be presented (render ACK) before the provider-success event arrives, so
// framePresented is true while the command is not yet committed (commit also needs
// providerSucceeded). A discontinuity in that window tears the stream down; the not-yet-
// presented guard in stopInteractiveStepRun must not silently drop that command — it must still
// receive a terminal (Canceled) so the UI's frame-pending indicator clears.
TEST(PlaybackCoordinatorTests, ForwardStepPresentedThenInterruptedCompletesCommand) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);

    // +1 → frame 1 current.
    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U)); // open frame 0 + step frame 1
    const std::optional<FrameRequest> step = provider->frameRequest(1U);
    ASSERT_TRUE(step.has_value());
    ASSERT_EQ(step->frameId, domain::FrameId{1});

    // Make frame 1's set ready and render-publish it, then ACK the render (framePresented=true)
    // WITHOUT posting provider success: the frame is presented but the command is not committed.
    ASSERT_TRUE(provider->postFrameReady(*step, makeFrameSet(step->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U)); // open frame 0 + step frame 1
    presentPublished(coordinator, render, 1U);

    // A backward step is a discontinuity that tears the stream down while frame 1 is presented
    // but uncommitted. The step command (cmd 2) must still receive a terminal, not be dropped.
    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
                  .delta = -1,
              }),
              PortSubmitResult::Accepted);

    bool sawStepCommand = false;
    const bool completed = waitUntil([&coordinator, &sawStepCommand] {
        for (const auto& terminal : coordinator->takeCompletedCommands()) {
            if (terminal.context.commandId == domain::CommandId{2}) {
                sawStepCommand = true;
                EXPECT_EQ(terminal.outcome, CommandOutcome::Canceled);
            }
        }
        return sawStepCommand;
    });
    ASSERT_TRUE(completed);
    EXPECT_TRUE(sawStepCommand);
}

TEST(PlaybackCoordinatorTests, ShutdownCancelsForwardStepStreamBoundedly) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(3U));

    coordinator->shutdown();
    // Both pending step commands must be completed as Closed after shutdown drains.
    std::vector<CommandTerminal> terminals;
    ASSERT_TRUE(waitUntil([&coordinator, &terminals] {
        terminals = coordinator->takeCompletedCommands();
        return !terminals.empty();
    }));
    for (const auto& terminal : terminals) {
        EXPECT_EQ(terminal.outcome, CommandOutcome::Closed);
    }
}

// Plan section 6: a +1 step may always use Sequential priority, because the provider falls back to
// an Exact decode whenever continuity is unavailable. The very first +1 step runs with cold
// continuity (no warm decoder cursor yet), so it exercises that fallback end-to-end: the
// coordinator must submit a Sequential request and still present the exact target frame.
TEST(PlaybackCoordinatorTests, ForwardStepFallsBackToExactAtDiscontinuity) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);

    // A single +1 step from a cold start: no warm sequential cursor exists yet.
    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);

    // The cold-start step must request Sequential priority (continuity is not a prerequisite), and
    // the provider's internal Exact fallback must still yield the exact frame.
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U)); // open frame 0 + step frame 1
    const std::optional<FrameRequest> step = provider->frameRequest(1U);
    ASSERT_TRUE(step.has_value());
    EXPECT_EQ(step->priority, FrameRequestPriority::Sequential);
    EXPECT_EQ(step->frameId, domain::FrameId{1});

    presentInteractiveStep(coordinator, provider, render, step, 1U);
    EXPECT_TRUE(waitUntil(
        [&coordinator] { return coordinator->snapshot()->displayedFrame == domain::FrameId{1}; }));

    const std::vector<CommandTerminal> terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().context.commandId, domain::CommandId{2});
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);
}

// The presentation deadline guards an interactive step frame: if the render ACK never arrives
// before the deadline elapses, the stream must tear down. Plan M1.2 classifies a presentation
// timeout as a failure (not a cancel): the step command must complete as Failed and the error must
// surface via the snapshot's lastError so the UI banner can show it. The FakeDeadlineScheduler.fire
// seam drives the handleDeadline branch for the interactive run.
TEST(PlaybackCoordinatorTests, ForwardStepPresentationTimeoutFailsStream) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider,
                                             render,
                                             std::make_shared<FakeMediaProbe>(),
                                             scheduler,
                                             std::make_shared<FakeSteadyClock>());
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);

    // Wait for the step frame's presentation deadline to be scheduled. The open frame armed one
    // during openReady; the step frame's is the most recently scheduled.
    ASSERT_TRUE(waitUntil([&scheduler] { return scheduler->scheduleCount() >= 2U; }));
    ASSERT_TRUE(scheduler->fire(scheduler->scheduleCount() - 1U));

    // The presentation-timeout failure tears the stream down, leaves the coordinator paused, and
    // surfaces the error via lastError.
    EXPECT_TRUE(waitUntil([&coordinator] {
        return coordinator->snapshot()->playbackState == domain::PlaybackState::kPaused &&
               coordinator->snapshot()->lastError.has_value();
    }));
    ASSERT_TRUE(coordinator->snapshot()->lastError.has_value());
    EXPECT_EQ(coordinator->snapshot()->lastError->code,
              domain::MediaErrorCode::kFramePresentationTimedOut);
    const std::vector<CommandTerminal> terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.size(), 1U);
    EXPECT_EQ(terminals.front().context.commandId, domain::CommandId{2});
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Failed);
    ASSERT_TRUE(terminals.front().error.has_value());
    EXPECT_EQ(terminals.front().error->code, domain::MediaErrorCode::kFramePresentationTimedOut);
}

// A prepared successor frame whose FrameSetReady carries the wrong canonical frame id cannot become
// the current frame. The branch must drop it and re-queue its command (rather than silently losing
// it) so the frame is re-submitted once the pipeline advances (plan section 4.3). This drives the
// untested prepared-frame mismatch path in handleFrameSet.
TEST(PlaybackCoordinatorTests, ForwardStepPreparedFrameMismatchRequeuesCommand) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
                  .delta = 1,
              }),
              PortSubmitResult::Accepted);

    // open frame 0 + step frame 1 (current) + step frame 2 (prepared).
    ASSERT_TRUE(provider->waitForFrameRequestCount(3U));
    const std::optional<FrameRequest> current = provider->frameRequest(1U);
    ASSERT_TRUE(current.has_value());
    const std::optional<FrameRequest> prepared = provider->frameRequest(2U);
    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(prepared->frameId, domain::FrameId{2});

    // The prepared frame's set arrives with a mismatched canonical frame id: it must be dropped and
    // its command re-queued, not presented.
    ASSERT_TRUE(provider->postFrameReady(*prepared, makeFrameSet(domain::FrameId{99})));

    // Present the current frame 1; committing it promotes the re-queued command and re-submits
    // frame 2 as the new current frame (the prepared slot was dropped).
    presentInteractiveStep(coordinator, provider, render, current, 1U);
    EXPECT_TRUE(waitUntil(
        [&coordinator] { return coordinator->snapshot()->displayedFrame == domain::FrameId{1}; }));

    // The re-submitted frame 2 request must carry Sequential priority and the correct frame id.
    ASSERT_TRUE(provider->waitForFrameRequestCount(4U));
    const std::optional<FrameRequest> resubmitted = provider->frameRequest(3U);
    ASSERT_TRUE(resubmitted.has_value());
    EXPECT_EQ(resubmitted->frameId, domain::FrameId{2});
    EXPECT_EQ(resubmitted->priority, FrameRequestPriority::Sequential);

    // The resubmitted frame 2 renders at index 2 (open frame 0, frame 1, frame 2).
    presentInteractiveStep(coordinator, provider, render, resubmitted, 2U);
    EXPECT_TRUE(waitUntil(
        [&coordinator] { return coordinator->snapshot()->displayedFrame == domain::FrameId{2}; }));

    // Both step commands must have succeeded: cmd 2 (frame 1) and the re-queued cmd 3 (frame 2).
    // Collect all terminals rather than assuming an order/count, then assert each outcome.
    std::vector<CommandTerminal> terminals;
    ASSERT_TRUE(waitUntil([&coordinator, &terminals] {
        for (const auto& terminal : coordinator->takeCompletedCommands()) {
            terminals.push_back(terminal);
        }
        return std::any_of(terminals.begin(), terminals.end(), [](const CommandTerminal& t) {
            return t.context.commandId == domain::CommandId{3};
        });
    }));
    std::map<domain::CommandId, CommandOutcome> outcomes;
    for (const auto& terminal : terminals) {
        outcomes.emplace(terminal.context.commandId, terminal.outcome);
    }
    ASSERT_EQ(outcomes[domain::CommandId{2}], CommandOutcome::Succeeded);
    ASSERT_EQ(outcomes[domain::CommandId{3}], CommandOutcome::Succeeded);
}

// Phase 0 baseline: held-backward stepping. Today every -1 step enters the Exact-seek path
// C-03: held-backward presents every intermediate canonical frame via the reverse interactive
// stream (one generation, FrameRequestPriority::Reverse), not per-step Exact seeks.
TEST(PlaybackCoordinatorTests, HeldBackwardPresentsEveryIntermediateFrame) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render, domain::CommandId{1}, /*secondFrameCount=*/12);

    ASSERT_EQ(coordinator->submit(SeekFrameCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .frameId = domain::FrameId{6},
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const auto seekRequest = provider->frameRequest(1U);
    ASSERT_TRUE(seekRequest.has_value());
    ASSERT_EQ(seekRequest->frameId, domain::FrameId{6});
    presentInteractiveStep(coordinator, provider, render, seekRequest, 1U);
    ASSERT_TRUE(waitUntil(
        [&coordinator] { return coordinator->snapshot()->displayedFrame == domain::FrameId{6}; }));

    constexpr std::size_t stepCount = 5U;
    std::vector<domain::FrameId> presentedIds;
    std::size_t renderIndex = 2U;
    domain::FrameId lastPresented = domain::FrameId{6};
    for (std::size_t i = 0U; i < stepCount; ++i) {
        const domain::FrameId target{lastPresented.value() - 1};
        const std::size_t requestsBefore = provider->frameRequestCount();
        ASSERT_EQ(coordinator->submit(StepFramesCommand{
                      .context = commandContext(coordinator, domain::CommandId{3 + i}),
                      .delta = -1,
                  }),
                  PortSubmitResult::Accepted);
        ASSERT_TRUE(provider->waitForFrameRequestCount(requestsBefore + 1U));
        std::optional<FrameRequest> matched;
        for (std::size_t index = provider->frameRequestCount(); index-- > 0U;) {
            const auto request = provider->frameRequest(index);
            if (request.has_value() && request->frameId == target) {
                matched = request;
                break;
            }
        }
        ASSERT_TRUE(matched.has_value());
        EXPECT_EQ(matched->priority, FrameRequestPriority::Reverse);
        presentInteractiveStep(coordinator, provider, render, matched, renderIndex);
        EXPECT_TRUE(waitUntil(
            [&coordinator, target] { return coordinator->snapshot()->displayedFrame == target; }));
        presentedIds.push_back(target);
        lastPresented = target;
        ++renderIndex;
    }
    const std::vector<domain::FrameId> expected{domain::FrameId{5},
                                                domain::FrameId{4},
                                                domain::FrameId{3},
                                                domain::FrameId{2},
                                                domain::FrameId{1}};
    EXPECT_EQ(presentedIds, expected);
}

TEST(PlaybackCoordinatorTests, ReverseStepUsesOneGenerationAcrossAdjacentCommands) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    openReady(coordinator, provider, render);
    ASSERT_EQ(coordinator->submit(SeekFrameCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .frameId = domain::FrameId{8},
              }),
              PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    presentInteractiveStep(coordinator, provider, render, provider->frameRequest(1U), 1U);
    ASSERT_TRUE(waitUntil(
        [&coordinator] { return coordinator->snapshot()->displayedFrame == domain::FrameId{8}; }));

    const domain::PlaybackGeneration generationAtStart =
        coordinator->snapshot()->playbackGeneration;
    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
                  .delta = -1,
              }),
              PortSubmitResult::Accepted);
    ASSERT_EQ(coordinator->submit(StepFramesCommand{
                  .context = commandContext(coordinator, domain::CommandId{4}),
                  .delta = -1,
              }),
              PortSubmitResult::Accepted);

    ASSERT_TRUE(provider->waitForFrameRequestCount(4U));
    for (std::size_t index = 2U; index < 4U; ++index) {
        const std::optional<FrameRequest> request = provider->frameRequest(index);
        ASSERT_TRUE(request.has_value());
        EXPECT_EQ(request->priority, FrameRequestPriority::Reverse);
        EXPECT_EQ(request->context.playback.playbackGeneration,
                  domain::PlaybackGeneration{generationAtStart.value() + 1U});
    }
    const auto current = provider->frameRequest(2U);
    const auto prepared = provider->frameRequest(3U);
    ASSERT_TRUE(current.has_value());
    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(current->frameId, domain::FrameId{7});
    EXPECT_EQ(prepared->frameId, domain::FrameId{6});
}

// Phase 0 baseline: a session's active Pair must not leak across topology changes. Today the Pair
// Reference source IS the canonical source (ComparisonValidator.cpp:195), so changing Reference
// (which today means re-opening with a different source carrying the Reference role) rebuilds the
// timeline. This test opens with source 0 as Reference (canonical = 30 frames), then re-opens with
// source 1 as Reference (12 frames), and asserts the canonical frame count is unchanged. It is
// expected to FAIL on the current code (today canonical follows the new Reference: 30 -> 12) and
// pass once Phase 1 decouples Reference from TimelineMaster.
TEST(PlaybackCoordinatorTests, ChangingReferenceDoesNotChangeTimelineMaster) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    markGraphicsReady(coordinator);

    // First open: source 0 is Reference (30 frames). Timeline master is session-order first
    // (also 0), so canonical frame count is 30 either way.
    const std::shared_ptr<const SessionSnapshot> initial = coordinator->snapshot();
    ASSERT_EQ(
        coordinator->submit(OpenDirectComparisonCommand{
            .context =
                CommandContext{
                    .sessionId = initial->sessionId,
                    .sessionEpoch = initial->sessionEpoch,
                    .commandId = domain::CommandId{1},
                },
            .sources =
                {
                    domain::ComparisonSource{
                        .id = 0U,
                        .role = domain::ComparisonRole::kReference,
                        .descriptor = makeDescriptor(
                            "a.mp4", domain::MediaExtent{.width = 320, .height = 180}, 30, 30),
                        .displayName = "A",
                    },
                    domain::ComparisonSource{
                        .id = 1U,
                        .role = domain::ComparisonRole::kPrediction,
                        .descriptor = makeDescriptor(
                            "b.mp4", domain::MediaExtent{.width = 160, .height = 90}, 12, 30),
                        .displayName = "B",
                    },
                },
        }),
        PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForOpenRequestCount(1U));
    std::optional<FrameProviderOpenRequest> open = provider->openRequest();
    ASSERT_TRUE(open.has_value());
    EXPECT_EQ(open->canonicalSourceId, 0U);
    ASSERT_TRUE(provider->postOpenSucceeded(*open));
    ASSERT_TRUE(provider->waitForFrameRequestCount(1U));
    std::optional<FrameRequest> frame = provider->frameRequest(0U);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(frame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(1U));
    ASSERT_TRUE(provider->postFrameSucceeded(*frame));
    presentPublished(coordinator, render, 0U);
    ASSERT_EQ(waitForTerminals(coordinator, 1U).size(), 1U);
    const std::uint64_t canonicalBefore = coordinator->snapshot()->canonicalFrameCount;
    ASSERT_EQ(canonicalBefore, 30U);

    // Re-open with source 1 as Reference (12 frames). C-01: timeline master stays session-order
    // first (source 0, 30 frames) even though Reference moved to the 12-frame source.
    const std::shared_ptr<const SessionSnapshot> before = coordinator->snapshot();
    ASSERT_EQ(
        coordinator->submit(OpenDirectComparisonCommand{
            .context =
                CommandContext{
                    .sessionId = before->sessionId,
                    .sessionEpoch = before->sessionEpoch,
                    .commandId = domain::CommandId{2},
                },
            .sources =
                {
                    domain::ComparisonSource{
                        .id = 0U,
                        .role = domain::ComparisonRole::kPrediction,
                        .descriptor = makeDescriptor(
                            "a.mp4", domain::MediaExtent{.width = 320, .height = 180}, 30, 30),
                        .displayName = "A",
                    },
                    domain::ComparisonSource{
                        .id = 1U,
                        .role = domain::ComparisonRole::kReference,
                        .descriptor = makeDescriptor(
                            "b.mp4", domain::MediaExtent{.width = 160, .height = 90}, 12, 30),
                        .displayName = "B",
                    },
                },
        }),
        PortSubmitResult::Accepted);
    ASSERT_TRUE(provider->waitForOpenRequestCount(2U));
    open = provider->openRequest(1U);
    ASSERT_TRUE(open.has_value());
    EXPECT_EQ(open->canonicalSourceId, 0U);
    ASSERT_TRUE(provider->postOpenSucceeded(*open));
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    frame = provider->frameRequest(1U);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(frame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    ASSERT_TRUE(provider->postFrameSucceeded(*frame));
    presentPublished(coordinator, render, 1U);
    ASSERT_EQ(waitForTerminals(coordinator, 1U).size(), 1U);

    const auto after = coordinator->snapshot();
    ASSERT_NE(after->validatedComparison, nullptr);
    EXPECT_EQ(after->canonicalFrameCount, canonicalBefore);
    EXPECT_EQ(after->canonicalFrameCount, 30U);
    EXPECT_EQ(after->validatedComparison->canonicalSourceId(), 0U);
    EXPECT_EQ(after->validatedComparison->timelineMasterSourceId(), 0U);
    ASSERT_TRUE(after->validatedComparison->referenceSourceId().has_value());
    EXPECT_EQ(*after->validatedComparison->referenceSourceId(), 1U);
}

TEST(PlaybackCoordinatorTests, SetPlaybackRangePublishesSnapshotAndRejectsInvalidBounds) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler, clock);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render);

    ASSERT_EQ(coordinator->submit(SetPlaybackRangeCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .range =
                      PlaybackRange{
                          .inInclusive = domain::FrameId{2},
                          .outInclusive = domain::FrameId{5},
                      },
                  .loop = true,
              }),
              PortSubmitResult::Accepted);
    ASSERT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);
    const auto installed = coordinator->snapshot();
    ASSERT_TRUE(installed->playbackRangeIn.has_value());
    ASSERT_TRUE(installed->playbackRangeOut.has_value());
    EXPECT_EQ(*installed->playbackRangeIn, domain::FrameId{2});
    EXPECT_EQ(*installed->playbackRangeOut, domain::FrameId{5});
    EXPECT_TRUE(installed->playbackRangeLoop);
    EXPECT_TRUE(installed->isConsistent());

    ASSERT_EQ(coordinator->submit(SetPlaybackRangeCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
                  .range =
                      PlaybackRange{
                          .inInclusive = domain::FrameId{8},
                          .outInclusive = domain::FrameId{4},
                      },
                  .loop = true,
              }),
              PortSubmitResult::Accepted);
    const auto rejected = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(rejected.front().outcome, CommandOutcome::Failed);
    EXPECT_EQ(*coordinator->snapshot()->playbackRangeIn, domain::FrameId{2});

    ASSERT_EQ(coordinator->submit(SetPlaybackRangeCommand{
                  .context = commandContext(coordinator, domain::CommandId{4}),
                  .range = std::nullopt,
                  .loop = false,
              }),
              PortSubmitResult::Accepted);
    ASSERT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);
    const auto cleared = coordinator->snapshot();
    EXPECT_FALSE(cleared->playbackRangeIn.has_value());
    EXPECT_FALSE(cleared->playbackRangeOut.has_value());
    EXPECT_FALSE(cleared->playbackRangeLoop);
    EXPECT_TRUE(cleared->isConsistent());
}

// H-01: wall-clock catch-up / sequential prepare must never request a frame past range Out.
TEST(PlaybackCoordinatorTests, RangeLoopCatchUpNeverPresentsPastOutAndReanchorsInKernel) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler, clock);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render, domain::CommandId{1}, 12, 30);

    ASSERT_EQ(coordinator->submit(SetPlaybackRangeCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .range =
                      PlaybackRange{
                          .inInclusive = domain::FrameId{0},
                          .outInclusive = domain::FrameId{2},
                      },
                  .loop = true,
              }),
              PortSubmitResult::Accepted);
    ASSERT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);

    ASSERT_EQ(coordinator->submit(PlayCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
              }),
              PortSubmitResult::Accepted);
    ASSERT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);
    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    const auto firstCadence = scheduler->request(1U);
    ASSERT_TRUE(firstCadence.has_value());
    clock->set(firstCadence->due);
    ASSERT_TRUE(scheduler->fire(1U));
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const auto firstPlayback = provider->frameRequest(1U);
    ASSERT_TRUE(firstPlayback.has_value());
    EXPECT_LE(firstPlayback->frameId.value(), 2);
    ASSERT_TRUE(provider->postFrameSucceeded(*firstPlayback));
    ASSERT_TRUE(provider->postFrameReady(*firstPlayback, makeFrameSet(firstPlayback->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    presentPublished(coordinator, render, 1U);
    ASSERT_TRUE(waitUntil(
        [&coordinator] { return coordinator->snapshot()->displayedFrame == domain::FrameId{1}; }));

    // Stall past catch-up tolerance; every subsequent provider request must stay <= Out.
    clock->advance(std::chrono::seconds{10});
    for (std::size_t index = 0U; index < scheduler->scheduleCount(); ++index) {
        static_cast<void>(scheduler->fire(index));
    }
    for (std::size_t attempt = 0U; attempt < 30U; ++attempt) {
        const std::size_t requestCount = provider->frameRequestCount();
        for (std::size_t index = 0U; index < requestCount; ++index) {
            const auto request = provider->frameRequest(index);
            if (!request.has_value()) {
                continue;
            }
            EXPECT_LE(request->frameId.value(), 2)
                << "Range playback must never request a frame past Out";
            if (request->frameId.value() > 2) {
                continue;
            }
            const auto before = render->publishedCount();
            static_cast<void>(provider->postFrameSucceeded(*request));
            static_cast<void>(provider->postFrameReady(*request, makeFrameSet(request->frameId)));
            if (render->publishedCount() > before) {
                presentPublished(coordinator, render, before);
            }
        }
        for (std::size_t index = 0U; index < scheduler->scheduleCount(); ++index) {
            static_cast<void>(scheduler->fire(index));
        }
        const auto snapshot = coordinator->snapshot();
        if (snapshot->displayedFrame.has_value() && snapshot->displayedFrame->value() == 2 &&
            snapshot->playbackState == domain::PlaybackState::kPaused) {
            break;
        }
        if (snapshot->displayedFrame.has_value() && snapshot->displayedFrame->value() == 0 &&
            snapshot->playbackState == domain::PlaybackState::kPlaying) {
            // Loop re-anchored at In after presenting Out — still must never have passed Out.
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    for (std::size_t index = 0U; index < provider->frameRequestCount(); ++index) {
        const auto request = provider->frameRequest(index);
        if (request.has_value()) {
            EXPECT_LE(request->frameId.value(), 2);
        }
    }
    EXPECT_TRUE(coordinator->snapshot()->isConsistent());
}

TEST(PlaybackCoordinatorTests, RangeWithoutLoopPausesAtOutInsteadOfPassingIt) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler, clock);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render, domain::CommandId{1}, 12, 30);

    ASSERT_EQ(coordinator->submit(SetPlaybackRangeCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .range =
                      PlaybackRange{
                          .inInclusive = domain::FrameId{0},
                          .outInclusive = domain::FrameId{1},
                      },
                  .loop = false,
              }),
              PortSubmitResult::Accepted);
    ASSERT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);
    ASSERT_EQ(coordinator->submit(PlayCommand{
                  .context = commandContext(coordinator, domain::CommandId{3}),
              }),
              PortSubmitResult::Accepted);
    ASSERT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);
    ASSERT_TRUE(scheduler->waitForScheduleCount(2U));
    const auto cadence = scheduler->request(1U);
    ASSERT_TRUE(cadence.has_value());
    clock->set(cadence->due);
    ASSERT_TRUE(scheduler->fire(1U));
    ASSERT_TRUE(provider->waitForFrameRequestCount(2U));
    const auto frame = provider->frameRequest(1U);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->frameId, domain::FrameId{1});
    ASSERT_TRUE(provider->postFrameSucceeded(*frame));
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(frame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(2U));
    presentPublished(coordinator, render, 1U);
    ASSERT_TRUE(waitUntil([&coordinator] {
        const auto snapshot = coordinator->snapshot();
        return snapshot->displayedFrame == domain::FrameId{1} &&
               snapshot->playbackState == domain::PlaybackState::kPaused;
    }));
    for (std::size_t index = 0U; index < provider->frameRequestCount(); ++index) {
        const auto request = provider->frameRequest(index);
        if (request.has_value()) {
            EXPECT_LE(request->frameId.value(), 1);
        }
    }
    EXPECT_TRUE(coordinator->snapshot()->isConsistent());
}

TEST(PlaybackCoordinatorTests, StartRangePlaybackFromOutsideRangeBeginsAtIn) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler, clock);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render, domain::CommandId{1}, 12, 30);
    // openReady leaves displayed at frame 0 — outside the range we will install.
    const std::size_t requestsBeforeRangePlay = provider->frameRequestCount();

    ASSERT_EQ(coordinator->submit(StartRangePlaybackCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .range =
                      PlaybackRange{
                          .inInclusive = domain::FrameId{2},
                          .outInclusive = domain::FrameId{4},
                      },
                  .loop = true,
              }),
              PortSubmitResult::Accepted);
    const auto terminals = waitForTerminals(coordinator, 1U);
    ASSERT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);
    const auto snapshot = coordinator->snapshot();
    ASSERT_TRUE(snapshot->playbackRangeIn.has_value());
    EXPECT_EQ(*snapshot->playbackRangeIn, domain::FrameId{2});
    EXPECT_EQ(*snapshot->playbackRangeOut, domain::FrameId{4});
    EXPECT_TRUE(snapshot->playbackRangeLoop);
    // Immediate In activation lands in Buffering while the first FrameSet is in flight.
    EXPECT_TRUE(snapshot->playbackState == domain::PlaybackState::kPlaying ||
                snapshot->playbackState == domain::PlaybackState::kBuffering);
    EXPECT_TRUE(snapshot->isConsistent());

    for (std::size_t index = 0U; index < scheduler->scheduleCount(); ++index) {
        static_cast<void>(scheduler->fire(index));
    }
    static_cast<void>(waitUntil([&provider, requestsBeforeRangePlay] {
        return provider->frameRequestCount() > requestsBeforeRangePlay;
    }));
    bool sawInRangeTarget = false;
    for (std::size_t index = requestsBeforeRangePlay; index < provider->frameRequestCount();
         ++index) {
        const auto request = provider->frameRequest(index);
        if (!request.has_value()) {
            continue;
        }
        EXPECT_LE(request->frameId.value(), 4)
            << "Range playback must never request a frame past Out";
        if (request->frameId == domain::FrameId{2}) {
            sawInRangeTarget = true;
        }
    }
    EXPECT_TRUE(sawInRangeTarget) << "Playing a range from outside must begin at In (frame 2)";
}

TEST(PlaybackCoordinatorTests, SingleFrameRangePlayDoesNotSpinAHighSpeedLoop) {
    const auto scheduler = std::make_shared<FakeDeadlineScheduler>();
    const auto clock = std::make_shared<FakeSteadyClock>();
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator =
        makeCoordinator(provider, render, std::make_shared<FakeMediaProbe>(), scheduler, clock);
    markGraphicsReady(coordinator);
    openReady(coordinator, provider, render, domain::CommandId{1}, 12, 30);

    // Single-frame range already sitting on In==Out must complete without starting a run.
    ASSERT_EQ(coordinator->submit(StartRangePlaybackCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .range =
                      PlaybackRange{
                          .inInclusive = domain::FrameId{0},
                          .outInclusive = domain::FrameId{0},
                      },
                  .loop = true,
              }),
              PortSubmitResult::Accepted);
    const auto terminals = waitForTerminals(coordinator, 1U);
    EXPECT_EQ(terminals.front().outcome, CommandOutcome::Succeeded);
    EXPECT_EQ(coordinator->snapshot()->playbackState, domain::PlaybackState::kPaused);
    EXPECT_TRUE(coordinator->snapshot()->playbackRangeIn.has_value());
    EXPECT_EQ(*coordinator->snapshot()->playbackRangeIn, domain::FrameId{0});
    EXPECT_EQ(*coordinator->snapshot()->playbackRangeOut, domain::FrameId{0});
}

// C-02: active Pair is re-resolved from stable source identities after topology changes.
// A preferred pair whose members disappeared must not be restored as a stale ordinal when the
// missing source is later re-added (v1.4.2 black-screen class of bugs).
namespace {

void completeDirectOpen(const std::shared_ptr<PlaybackCoordinator>& coordinator,
                        const std::shared_ptr<FakeFrameProvider>& provider,
                        const std::shared_ptr<FakeRenderChannel>& render,
                        const std::size_t openIndex,
                        const std::size_t frameIndex,
                        const std::size_t renderIndex) {
    ASSERT_TRUE(provider->waitForOpenRequestCount(openIndex + 1U));
    const auto open = provider->openRequest(openIndex);
    ASSERT_TRUE(open.has_value());
    ASSERT_TRUE(provider->postOpenSucceeded(*open));
    ASSERT_TRUE(provider->waitForFrameRequestCount(frameIndex + 1U));
    const auto frame = provider->frameRequest(frameIndex);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(provider->postFrameReady(*frame, makeFrameSet(frame->frameId)));
    ASSERT_TRUE(render->waitForPublishedCount(renderIndex + 1U));
    ASSERT_TRUE(provider->postFrameSucceeded(*frame));
    presentPublished(coordinator, render, renderIndex);
    ASSERT_EQ(waitForTerminals(coordinator, 1U).size(), 1U);
}

[[nodiscard]] bool snapshotHasSource(const SessionSnapshot& snapshot, const domain::SourceId id) {
    return std::any_of(snapshot.sources.begin(), snapshot.sources.end(), [id](const auto& source) {
        return source.sourceId == id;
    });
}

} // namespace

TEST(PlaybackCoordinatorTests, SessionPairDoesNotLeakAcrossTopology) {
    const auto provider = std::make_shared<FakeFrameProvider>();
    const auto render = std::make_shared<FakeRenderChannel>();
    const auto coordinator = makeCoordinator(provider, render);
    ASSERT_NE(coordinator, nullptr);
    markGraphicsReady(coordinator);

    const auto source = [](const domain::SourceId id, const char* name) {
        return domain::ComparisonSource{
            .id = id,
            .role = domain::ComparisonRole::kPrediction,
            .descriptor =
                makeDescriptor(name, domain::MediaExtent{.width = 320, .height = 180}, 12, 30),
            .displayName = name,
        };
    };

    const std::shared_ptr<const SessionSnapshot> initial = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(OpenDirectComparisonCommand{
                  .context =
                      CommandContext{
                          .sessionId = initial->sessionId,
                          .sessionEpoch = initial->sessionEpoch,
                          .commandId = domain::CommandId{1},
                      },
                  .sources = {source(0U, "a.mp4"), source(1U, "b.mp4"), source(2U, "c.mp4")},
              }),
              PortSubmitResult::Accepted);
    completeDirectOpen(coordinator, provider, render, 0U, 0U, 0U);

    // Pin the active pair to A/C (0,2) — the ordinal that would black-screen on a 2-source set.
    ASSERT_EQ(coordinator->submit(SetActiveComparisonPairCommand{
                  .context = commandContext(coordinator, domain::CommandId{2}),
                  .pair = domain::ComparisonPair{0U, 2U},
                  .policy = domain::DefaultPairPolicy::PreserveIfAvailable,
              }),
              PortSubmitResult::Accepted);
    ASSERT_EQ(waitForTerminals(coordinator, 1U).front().outcome, CommandOutcome::Succeeded);
    const auto pinned = coordinator->snapshot();
    ASSERT_TRUE(pinned->activeComparisonPair.has_value());
    EXPECT_EQ(pinned->activeComparisonPair->first, 0U);
    EXPECT_EQ(pinned->activeComparisonPair->second, 2U);

    // Shrink: drop source 2. Stale (0,2) must not survive.
    const auto beforeShrink = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(OpenDirectComparisonCommand{
                  .context =
                      CommandContext{
                          .sessionId = beforeShrink->sessionId,
                          .sessionEpoch = beforeShrink->sessionEpoch,
                          .commandId = domain::CommandId{3},
                      },
                  .sources = {source(0U, "a.mp4"), source(1U, "b.mp4")},
              }),
              PortSubmitResult::Accepted);
    completeDirectOpen(coordinator, provider, render, 1U, 1U, 1U);
    const auto shrunk = coordinator->snapshot();
    ASSERT_TRUE(shrunk->activeComparisonPair.has_value());
    EXPECT_TRUE(snapshotHasSource(*shrunk, shrunk->activeComparisonPair->first));
    EXPECT_TRUE(snapshotHasSource(*shrunk, shrunk->activeComparisonPair->second));
    EXPECT_FALSE(shrunk->activeComparisonPair->contains(2U));

    // Grow: re-add source 2. PreserveIfAvailable must keep the live post-shrink pair (0,1),
    // never resurrect the stale (0,2) preference from before the shrink.
    const auto liveAfterShrink = *shrunk->activeComparisonPair;
    const auto beforeGrow = coordinator->snapshot();
    ASSERT_EQ(coordinator->submit(OpenDirectComparisonCommand{
                  .context =
                      CommandContext{
                          .sessionId = beforeGrow->sessionId,
                          .sessionEpoch = beforeGrow->sessionEpoch,
                          .commandId = domain::CommandId{4},
                      },
                  .sources = {source(0U, "a.mp4"), source(1U, "b.mp4"), source(2U, "c.mp4")},
              }),
              PortSubmitResult::Accepted);
    completeDirectOpen(coordinator, provider, render, 2U, 2U, 2U);
    const auto grown = coordinator->snapshot();
    ASSERT_TRUE(grown->activeComparisonPair.has_value());
    EXPECT_EQ(grown->activeComparisonPair->first, liveAfterShrink.first);
    EXPECT_EQ(grown->activeComparisonPair->second, liveAfterShrink.second);
    EXPECT_TRUE(snapshotHasSource(*grown, grown->activeComparisonPair->first));
    EXPECT_TRUE(snapshotHasSource(*grown, grown->activeComparisonPair->second));
}

} // namespace
} // namespace dvs::application
