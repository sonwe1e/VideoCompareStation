#include "dvs/media/PreviewThumbnailService.h"

#include "PairMetricsDecodeSession.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace dvs::media {
namespace {

struct Job final {
    application::PreviewThumbnailRequest request;
    std::shared_ptr<application::IPreviewThumbnailSink> sink;
};

[[nodiscard]] application::PreviewThumbnailResult
downscaleToRequest(const application::PreviewThumbnailRequest& request,
                   internal::PairMetricsDecodeSession::RgbaFrame frame) {
    application::PreviewThumbnailResult result{
        .context = request.context,
        .frameId = request.frameId,
    };
    if (frame.isEmpty()) {
        return result;
    }
    const std::uint32_t sourceWidth = frame.width;
    const std::uint32_t sourceHeight = frame.height;
    // Fit inside the request box while preserving the source aspect ratio: a 4:3 or portrait
    // source must not be stretched into the (16:9-ish) preview box. Sources smaller than the
    // box are returned unchanged (never upscaled).
    std::uint32_t targetWidth = sourceWidth;
    std::uint32_t targetHeight = sourceHeight;
    if (sourceWidth > request.maxWidth || sourceHeight > request.maxHeight) {
        const double scale = (std::min)(static_cast<double>(request.maxWidth) / sourceWidth,
                                        static_cast<double>(request.maxHeight) / sourceHeight);
        targetWidth = std::max(
            1U, static_cast<std::uint32_t>(static_cast<double>(sourceWidth) * scale + 0.5));
        targetHeight = std::max(
            1U, static_cast<std::uint32_t>(static_cast<double>(sourceHeight) * scale + 0.5));
    }
    if (targetWidth == 0U || targetHeight == 0U) {
        return result;
    }
    if (targetWidth == sourceWidth && targetHeight == sourceHeight) {
        result.rgba = std::move(frame.pixels);
        result.width = sourceWidth;
        result.height = sourceHeight;
        result.available = true;
        return result;
    }
    result.rgba.assign(static_cast<std::size_t>(targetWidth) * targetHeight * 4U, 0U);
    result.width = targetWidth;
    result.height = targetHeight;
    for (std::uint32_t y = 0U; y < targetHeight; ++y) {
        const std::uint32_t y0 = y * sourceHeight / targetHeight;
        const std::uint32_t y1 = std::max(y0 + 1U, (y + 1U) * sourceHeight / targetHeight);
        for (std::uint32_t x = 0U; x < targetWidth; ++x) {
            const std::uint32_t x0 = x * sourceWidth / targetWidth;
            const std::uint32_t x1 = std::max(x0 + 1U, (x + 1U) * sourceWidth / targetWidth);
            std::uint32_t sum[4] = {0U, 0U, 0U, 0U};
            std::uint32_t count = 0U;
            for (std::uint32_t sy = y0; sy < y1; ++sy) {
                for (std::uint32_t sx = x0; sx < x1; ++sx) {
                    const std::size_t index =
                        (static_cast<std::size_t>(sy) * sourceWidth + sx) * 4U;
                    sum[0] += frame.pixels[index + 0U];
                    sum[1] += frame.pixels[index + 1U];
                    sum[2] += frame.pixels[index + 2U];
                    sum[3] += frame.pixels[index + 3U];
                    ++count;
                }
            }
            const std::size_t targetIndex = (static_cast<std::size_t>(y) * targetWidth + x) * 4U;
            const std::uint32_t divisor = count == 0U ? 1U : count;
            result.rgba[targetIndex + 0U] = static_cast<std::uint8_t>(sum[0] / divisor);
            result.rgba[targetIndex + 1U] = static_cast<std::uint8_t>(sum[1] / divisor);
            result.rgba[targetIndex + 2U] = static_cast<std::uint8_t>(sum[2] / divisor);
            result.rgba[targetIndex + 3U] = static_cast<std::uint8_t>(sum[3] / divisor);
        }
    }
    result.available = true;
    return result;
}

} // namespace

class PreviewThumbnailService::Impl final {
public:
    explicit Impl(const std::size_t queueCapacity) : queueCapacity_(queueCapacity) {
        worker_ = std::thread([this] { run(); });
    }

    ~Impl() {
        {
            std::scoped_lock lock(mutex_);
            stopping_ = true;
            interruptRequested_.store(true, std::memory_order_release);
        }
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] application::PortSubmitResult
    submit(const application::PreviewThumbnailRequest& request,
           std::shared_ptr<application::IPreviewThumbnailSink> sink) {
        if (!request.isValid() || sink == nullptr) {
            return application::PortSubmitResult::Closed;
        }
        {
            std::scoped_lock lock(mutex_);
            if (stopping_) {
                return application::PortSubmitResult::Closed;
            }
            if (pending_.has_value() && queueCapacity_ == 0U) {
                return application::PortSubmitResult::Busy;
            }
            interruptRequested_.store(true, std::memory_order_release);
            pending_ = Job{request, std::move(sink)};
        }
        condition_.notify_all();
        return application::PortSubmitResult::Accepted;
    }

    void cancel(const application::RequestContext& context) noexcept {
        std::scoped_lock lock(mutex_);
        if (pending_.has_value() && pending_->request.context == context) {
            pending_.reset();
        }
        if (active_.has_value() && active_->request.context == context) {
            interruptRequested_.store(true, std::memory_order_release);
        }
    }

    [[nodiscard]] std::uint64_t decodedFrameCountForTesting() const noexcept {
        return decodedFrameCount_.load(std::memory_order_acquire);
    }

private:
    void run() {
        for (;;) {
            std::optional<Job> job;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
                if (stopping_ && !pending_.has_value()) {
                    return;
                }
                job = std::move(pending_);
                pending_.reset();
                active_ = job;
                interruptRequested_.store(false, std::memory_order_release);
            }

            application::PreviewThumbnailResult result{
                .context = job->request.context,
                .frameId = job->request.frameId,
            };
            const auto canceled = [this] {
                return interruptRequested_.load(std::memory_order_acquire);
            };
            // Session reuse: consecutive hovers address the same source, so one decoder
            // session stays open across jobs while the descriptor matches; reopening
            // (demuxer + codec init) otherwise dominates hover latency. Worker-owned.
            if (session_ == nullptr || !session_->isOpen() ||
                !session_->matches(job->request.source.descriptor)) {
                session_ = std::make_unique<internal::PairMetricsDecodeSession>(
                    job->request.source.id, job->request.source.descriptor);
                if (!session_->open(interruptRequested_)) {
                    session_.reset();
                }
            }
            if (session_ != nullptr) {
                auto decoded = session_->decodeRgba(job->request.frameId, interruptRequested_);
                if (decoded && !canceled()) {
                    decodedFrameCount_.fetch_add(1U, std::memory_order_release);
                    result = downscaleToRequest(job->request, std::move(decoded).value());
                }
            }
            if (!canceled()) {
                job->sink->onPreviewThumbnail(result);
            }

            std::scoped_lock lock(mutex_);
            if (active_.has_value() && active_->request.context == job->request.context) {
                active_.reset();
            }
        }
    }

    std::size_t queueCapacity_ = 1U;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    bool stopping_ = false;
    std::optional<Job> pending_;
    std::optional<Job> active_;
    std::atomic<bool> interruptRequested_{false};
    std::atomic<std::uint64_t> decodedFrameCount_{0U};
    std::unique_ptr<internal::PairMetricsDecodeSession> session_;
};

PreviewThumbnailService::PreviewThumbnailService(const std::size_t queueCapacity)
    : impl_(std::make_unique<Impl>(queueCapacity)) {}

PreviewThumbnailService::~PreviewThumbnailService() = default;

application::PortSubmitResult
PreviewThumbnailService::submit(const application::PreviewThumbnailRequest& request,
                                std::shared_ptr<application::IPreviewThumbnailSink> sink) {
    return impl_->submit(request, std::move(sink));
}

void PreviewThumbnailService::cancel(const application::RequestContext& context) noexcept {
    impl_->cancel(context);
}

std::uint64_t PreviewThumbnailService::decodedFrameCountForTesting() const noexcept {
    return impl_->decodedFrameCountForTesting();
}

} // namespace dvs::media
