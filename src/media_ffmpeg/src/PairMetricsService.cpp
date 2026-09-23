#include "dvs/media/PairMetricsService.h"

#include "dvs/application/ComparisonMetrics.h"

#include "PairMetricsDecodeSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace dvs::media {
namespace {

// Samples are published progressively so a timeline lane fills while decoding.
constexpr std::size_t kBatchSampleCount = 16U;

[[nodiscard]] std::optional<std::int64_t>
offsetForSource(const std::vector<application::SourceFrameOffset>& offsets,
                const domain::SourceId sourceId) noexcept {
    const auto found = std::find_if(
        offsets.begin(), offsets.end(), [sourceId](const application::SourceFrameOffset& offset) {
            return offset.sourceId == sourceId;
        });
    if (found == offsets.end()) {
        return std::nullopt;
    }
    return found->frames;
}

// Center-out canonical order: the playhead frame is scored first, then alternating successors
// and predecessors, so the most relevant readout arrives before the window edges.
[[nodiscard]] std::vector<domain::FrameId>
centerOutFrameOrder(const application::PairMetricsRequest& request) noexcept {
    const std::int64_t first = request.firstFrame.value();
    const std::int64_t last = request.lastFrame.value();
    const std::int64_t center = first + (last - first) / 2;
    std::vector<domain::FrameId> order;
    order.reserve(static_cast<std::size_t>(last - first + 1));
    auto append = [&order](const std::int64_t frame) { order.push_back(domain::FrameId{frame}); };
    append(center);
    for (std::int64_t radius = 1;; ++radius) {
        const bool hasAfter = center + radius <= last;
        const bool hasBefore = center - radius >= first;
        if (!hasAfter && !hasBefore) {
            break;
        }
        if (hasAfter) {
            append(center + radius);
        }
        if (hasBefore) {
            append(center - radius);
        }
    }
    return order;
}

} // namespace

class PairMetricsService::Impl final {
public:
    explicit Impl(const std::size_t queueCapacity)
        : queueCapacity_(std::max<std::size_t>(1U, queueCapacity)), worker_([this] { run(); }) {}

    ~Impl() {
        shutdown();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] application::PortSubmitResult
    submit(const application::PairMetricsRequest& request,
           std::shared_ptr<application::IPairMetricsSink> sink) {
        if (!sink || !request.isValid()) {
            return application::PortSubmitResult::Closed;
        }
        auto job = std::make_shared<Job>(request, std::weak_ptr{sink});
        {
            std::scoped_lock lock(mutex_);
            if (closed_) {
                return application::PortSubmitResult::Closed;
            }
            if (queue_.size() >= queueCapacity_) {
                // Latest-wins admission: interactive callers always describe the current need,
                // so the oldest queued job is superseded without an event.
                queue_.pop_front();
            }
            queue_.push_back(std::move(job));
            if (active_) {
                active_->superseded.store(true, std::memory_order_release);
                interruptActiveSessionsLocked();
            }
        }
        condition_.notify_one();
        return application::PortSubmitResult::Accepted;
    }

    void cancel(const application::PlaybackRequestContext& context) noexcept {
        std::scoped_lock lock(mutex_);
        const auto found = std::find_if(queue_.begin(), queue_.end(), [&context](const auto& job) {
            return job->request.context == context;
        });
        if (found != queue_.end()) {
            queue_.erase(found);
        }
        if (active_ && active_->request.context == context) {
            active_->superseded.store(true, std::memory_order_release);
            interruptActiveSessionsLocked();
        }
    }

    [[nodiscard]] std::uint64_t decodedFrameCount() const noexcept {
        return decodedFrameCount_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t publishedBatchCount() const noexcept {
        return publishedBatchCount_.load(std::memory_order_acquire);
    }

private:
    struct Job final {
        application::PairMetricsRequest request;
        std::weak_ptr<application::IPairMetricsSink> sink;
        std::atomic<bool> superseded = false;

        Job(const application::PairMetricsRequest& requestValue,
            std::weak_ptr<application::IPairMetricsSink> sinkValue)
            : request(requestValue), sink(std::move(sinkValue)) {}

        Job(const Job&) = delete;
        Job& operator=(const Job&) = delete;
    };

    // Session slot reuse: an open session survives across jobs while the addressed media,
    // geometry, and frame count stay identical; otherwise the slot reopens. Owned by the
    // worker thread only; the mutex only guards the interruption registry below.
    struct SessionSlot final {
        std::unique_ptr<internal::PairMetricsDecodeSession> session;
    };

    void interruptActiveSessionsLocked() noexcept {
        for (internal::PairMetricsDecodeSession* const session : activeSessions_) {
            session->requestInterrupt();
        }
    }

    void registerActiveSessions() {
        std::scoped_lock lock(mutex_);
        activeSessions_.clear();
        for (SessionSlot& slot : sessions_) {
            if (slot.session != nullptr) {
                activeSessions_.push_back(slot.session.get());
            }
        }
    }

    void run() {
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return closed_ || !queue_.empty(); });
                if (closed_) {
                    return;
                }
                job = queue_.front();
                queue_.pop_front();
                active_ = job;
            }
            execute(*job);
            {
                std::scoped_lock lock(mutex_);
                active_.reset();
                activeSessions_.clear();
            }
        }
    }

    void execute(Job& job) {
        if (!prepareSessions(job)) {
            return;
        }

        application::PairMetricsBatch batch = makeBatch(job);
        for (const domain::FrameId frameId : centerOutFrameOrder(job.request)) {
            if (job.superseded.load(std::memory_order_acquire)) {
                return;
            }
            const auto sample = scoreFrame(job, frameId);
            if (!sample.has_value()) {
                return;
            }
            batch.samples.push_back(*sample);
            if (batch.samples.size() >= kBatchSampleCount) {
                publish(job, std::move(batch));
                if (job.superseded.load(std::memory_order_acquire)) {
                    return;
                }
                batch = makeBatch(job);
            }
        }
        batch.finalBatch = true;
        publish(job, std::move(batch));
    }

    // Returns false when the job must stop (superseded or open failure with a posted event).
    [[nodiscard]] bool prepareSessions(Job& job) {
        if (job.superseded.load(std::memory_order_acquire)) {
            return false;
        }
        sessions_.resize(job.request.sources.size());
        // Allocate every slot first so the interruption registry only ever holds live pointers.
        for (std::size_t index = 0; index < sessions_.size(); ++index) {
            if (sessions_[index].session == nullptr) {
                const domain::ComparisonSource& source = job.request.sources[index];
                sessions_[index].session = std::make_unique<internal::PairMetricsDecodeSession>(
                    source.id, source.descriptor);
            }
        }
        registerActiveSessions();
        for (std::size_t index = 0; index < sessions_.size(); ++index) {
            const domain::ComparisonSource& source = job.request.sources[index];
            internal::PairMetricsDecodeSession& session = *sessions_[index].session;
            if (session.isOpen() && session.matches(source.descriptor)) {
                continue;
            }
            const domain::Status opened = session.open(job.superseded);
            if (!opened) {
                if (!job.superseded.load(std::memory_order_acquire)) {
                    postFailure(job, opened.error());
                }
                return false;
            }
        }
        return true;
    }

    // Scores one canonical frame. Nullopt stops the job silently (superseded) or after a
    // posted failure; non-comparable positions are returned as samples so the UI can show an
    // explicit "not comparable" state instead of a hole.
    [[nodiscard]] std::optional<application::PairMetricsSample>
    scoreFrame(Job& job, const domain::FrameId frameId) {
        application::PairMetricsSample sample;
        sample.canonicalFrameId = frameId;

        std::array<internal::PairMetricsDecodeSession::RgbaFrame, 2U> frames{};
        for (std::size_t index = 0; index < 2U; ++index) {
            const domain::ComparisonSource& source = job.request.sources[index];
            const std::optional<std::int64_t> offset =
                offsetForSource(job.request.offsets, source.id);
            if (!offset.has_value()) {
                sample.comparable = false;
                return sample;
            }
            const std::int64_t mapped = frameId.value() + *offset;
            if (mapped < 0 || mapped >= source.descriptor.frameCount.value) {
                sample.comparable = false;
                return sample;
            }
            auto decoded =
                sessions_[index].session->decodeRgba(domain::FrameId{mapped}, job.superseded);
            if (!decoded) {
                if (job.superseded.load(std::memory_order_acquire)) {
                    return std::nullopt;
                }
                postFailure(job, decoded.error());
                return std::nullopt;
            }
            decodedFrameCount_.fetch_add(1U, std::memory_order_release);
            frames[index] = std::move(decoded).value();
        }
        if (frames[0].isEmpty() || frames[1].isEmpty() || frames[0].width != frames[1].width ||
            frames[0].height != frames[1].height) {
            sample.comparable = false;
            return sample;
        }

        const domain::Rgba8View first{frames[0].pixels.data(),
                                      frames[0].width,
                                      frames[0].height,
                                      static_cast<std::size_t>(frames[0].width) * 4U};
        const domain::Rgba8View second{frames[1].pixels.data(),
                                       frames[1].width,
                                       frames[1].height,
                                       static_cast<std::size_t>(frames[1].width) * 4U};
        const domain::ComparisonPair pair{job.request.sources[0].id, job.request.sources[1].id};
        const auto scored = application::scoreActivePairRgbAbsolute(
            pair, frameId, first, second, job.request.mismatchThreshold);
        if (!scored.has_value()) {
            sample.comparable = false;
            return sample;
        }
        sample.comparable = true;
        sample.metrics = scored->metrics;
        return sample;
    }

    [[nodiscard]] application::PairMetricsBatch makeBatch(const Job& job) const {
        return application::PairMetricsBatch{
            .context = job.request.context,
            .sources = job.request.sources,
            .alignmentRevision = job.request.alignmentRevision,
            .mismatchThreshold = job.request.mismatchThreshold,
            .metricId = std::string{application::kRgbAbsoluteMetricId},
            .samples = {},
            .finalBatch = false,
        };
    }

    void publish(const Job& job, application::PairMetricsBatch&& batch) {
        const std::shared_ptr<application::IPairMetricsSink> sink = job.sink.lock();
        if (sink == nullptr) {
            return;
        }
        publishedBatchCount_.fetch_add(1U, std::memory_order_release);
        sink->onPairMetricsBatch(std::move(batch));
    }

    void postFailure(const Job& job, const domain::MediaError& error) {
        const std::shared_ptr<application::IPairMetricsSink> sink = job.sink.lock();
        if (sink == nullptr) {
            return;
        }
        sink->onPairMetricsFailure(application::PairMetricsFailure{job.request.context, error});
    }

    void shutdown() noexcept {
        {
            std::scoped_lock lock(mutex_);
            if (closed_) {
                return;
            }
            closed_ = true;
            queue_.clear();
            if (active_) {
                active_->superseded.store(true, std::memory_order_release);
                interruptActiveSessionsLocked();
            }
        }
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    const std::size_t queueCapacity_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::shared_ptr<Job>> queue_;
    std::shared_ptr<Job> active_;
    std::vector<internal::PairMetricsDecodeSession*> activeSessions_;
    std::vector<SessionSlot> sessions_;
    std::atomic<bool> closed_ = false;
    std::atomic<std::uint64_t> decodedFrameCount_{0U};
    std::atomic<std::uint64_t> publishedBatchCount_{0U};
    std::thread worker_;
};

PairMetricsService::PairMetricsService(const std::size_t queueCapacity)
    : impl_(std::make_unique<Impl>(queueCapacity)) {}

PairMetricsService::~PairMetricsService() = default;

application::PortSubmitResult
PairMetricsService::submit(const application::PairMetricsRequest& request,
                           std::shared_ptr<application::IPairMetricsSink> sink) {
    return impl_->submit(request, std::move(sink));
}

void PairMetricsService::cancel(const application::PlaybackRequestContext& context) noexcept {
    impl_->cancel(context);
}

std::uint64_t PairMetricsService::decodedFrameCountForTesting() const noexcept {
    return impl_->decodedFrameCount();
}

std::uint64_t PairMetricsService::publishedBatchCountForTesting() const noexcept {
    return impl_->publishedBatchCount();
}

} // namespace dvs::media
