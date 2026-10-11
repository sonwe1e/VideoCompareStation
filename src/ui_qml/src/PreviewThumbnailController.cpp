#include "dvs/ui/PreviewThumbnailController.h"

#include <QMetaObject>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <utility>

namespace dvs::ui {
namespace {

constexpr int kRequestDebounceMilliseconds = 80;
constexpr std::size_t kMaximumCachedFrames = 256U;

} // namespace

class PreviewThumbnailController::Sink final : public application::IPreviewThumbnailSink {
public:
    explicit Sink(PreviewThumbnailController& owner) : owner_(&owner) {}

    void onPreviewThumbnail(application::PreviewThumbnailResult result) override {
        // Checking closed_ and posting the wake-up under the same lock keeps them atomic:
        // once close() returns, no further invokeMethod can target a destroyed owner.
        // The queued post only enqueues an event, so holding the lock is cheap and safe.
        std::scoped_lock lock(mutex_);
        if (closed_) {
            return;
        }
        results_.push_back(std::move(result));
        QMetaObject::invokeMethod(
            owner_, &PreviewThumbnailController::drainSink, Qt::QueuedConnection);
    }

    void close() noexcept {
        std::scoped_lock lock(mutex_);
        closed_ = true;
        results_.clear();
    }

    [[nodiscard]] std::vector<application::PreviewThumbnailResult> takeResults() {
        std::scoped_lock lock(mutex_);
        return std::exchange(results_, {});
    }

private:
    PreviewThumbnailController* owner_ = nullptr;
    std::mutex mutex_;
    bool closed_ = false;
    std::vector<application::PreviewThumbnailResult> results_;
};

PreviewThumbnailController::PreviewThumbnailController(Dependencies dependencies, QObject* parent)
    : QObject(parent), dependencies_(std::move(dependencies)),
      sink_(std::make_shared<Sink>(*this)) {
    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(kRequestDebounceMilliseconds);
    QObject::connect(debounce_, &QTimer::timeout, this, [this] { scheduleRequest(); });
}

PreviewThumbnailController::~PreviewThumbnailController() {
    stop();
}

bool PreviewThumbnailController::available() const noexcept {
    return dependencies_.service != nullptr && dependencies_.snapshot != nullptr;
}

int PreviewThumbnailController::generation() const noexcept {
    return generation_;
}

void PreviewThumbnailController::request(const qint64 frame) {
    if (stopped_ || frame < 0) {
        return;
    }
    if (hasThumbnail(frame)) {
        return;
    }
    pendingFrame_ = frame;
    if (debounce_ != nullptr) {
        debounce_->start();
    }
}

void PreviewThumbnailController::requestBatch(const QVariantList& frames) {
    if (stopped_) {
        return;
    }
    // The batch always replaces its predecessor: a re-targeted strip must not keep decoding
    // frames from the previous window. The card re-sends the whole visible window, so the
    // queue stays bounded by the strip's cell count (5-9 frames) by construction.
    batchQueue_.clear();
    for (const QVariant& frame : frames) {
        bool converted = false;
        const qint64 value = frame.toLongLong(&converted);
        if (converted && value >= 0 && !hasThumbnail(value)) {
            batchQueue_.push_back(value);
        }
    }
    if (!batchQueue_.empty() && debounce_ != nullptr) {
        debounce_->start();
    }
}

void PreviewThumbnailController::cancelBatch() {
    batchQueue_.clear();
    if (inflightIsBatch_) {
        if (dependencies_.service != nullptr && inflightContext_.has_value()) {
            dependencies_.service->cancel(*inflightContext_);
        }
        inflightContext_.reset();
        inflightFrame_ = -1;
        inflightIsBatch_ = false;
        Q_EMIT stateChanged();
    }
}

QUrl PreviewThumbnailController::urlForFrame(const qint64 frame) {
    if (!hasThumbnail(frame)) {
        return {};
    }
    return QUrl{QStringLiteral("image://timeline-preview/%1?g=%2").arg(frame).arg(generation_)};
}

bool PreviewThumbnailController::hasThumbnail(const qint64 frame) {
    syncSessionIdentity();
    std::scoped_lock lock(cacheMutex_);
    return cache_.find(frame) != cache_.end();
}

QSize PreviewThumbnailController::thumbnailSourceSize(const qint64 frame) {
    syncSessionIdentity();
    std::scoped_lock lock(cacheMutex_);
    const auto found = cache_.find(frame);
    if (found == cache_.end()) {
        return {};
    }
    return QSize{found->second.width, found->second.height};
}

PreviewThumbnailController::RawThumbnail
PreviewThumbnailController::rawForFrame(const qint64 frame) const {
    std::scoped_lock lock(cacheMutex_);
    const auto found = cache_.find(frame);
    return found == cache_.end() ? RawThumbnail{} : found->second;
}

void PreviewThumbnailController::stop() noexcept {
    stopped_ = true;
    batchQueue_.clear();
    inflightIsBatch_ = false;
    if (sink_ != nullptr) {
        sink_->close();
    }
    if (dependencies_.service != nullptr && inflightContext_.has_value()) {
        dependencies_.service->cancel(*inflightContext_);
    }
    inflightContext_.reset();
}

void PreviewThumbnailController::syncSessionIdentity() {
    if (stopped_) {
        return;
    }
    const std::shared_ptr<const application::SessionSnapshot> snapshot =
        dependencies_.snapshot ? dependencies_.snapshot() : nullptr;
    if (!snapshot) {
        return;
    }
    if (!sessionIdentityKnown_) {
        cachedSessionId_ = snapshot->sessionId;
        cachedSessionEpoch_ = snapshot->sessionEpoch;
        sessionIdentityKnown_ = true;
        return;
    }
    if (snapshot->sessionId == cachedSessionId_ && snapshot->sessionEpoch == cachedSessionEpoch_) {
        return;
    }
    // A different session (or a same-session epoch bump that replaced content) now owns the
    // timeline. Cached frames, in-flight work, and previously issued URLs all describe the
    // old session and must never surface for the new one.
    cachedSessionId_ = snapshot->sessionId;
    cachedSessionEpoch_ = snapshot->sessionEpoch;
    batchQueue_.clear();
    inflightIsBatch_ = false;
    if (dependencies_.service != nullptr && inflightContext_.has_value()) {
        dependencies_.service->cancel(*inflightContext_);
    }
    inflightContext_.reset();
    inflightFrame_ = -1;
    {
        std::scoped_lock lock(cacheMutex_);
        cache_.clear();
    }
    ++generation_;
    Q_EMIT stateChanged();
}

void PreviewThumbnailController::drainSink() {
    if (sink_ == nullptr) {
        return;
    }
    for (application::PreviewThumbnailResult& result : sink_->takeResults()) {
        onResult(std::move(result));
    }
    // A finished job (either kind) can leave queued batch frames behind; arm the drain so the
    // low-priority strip continues without another user action.
    if (!stopped_ && pendingFrame_ < 0 && !batchQueue_.empty() && debounce_ != nullptr) {
        debounce_->start();
    }
}

void PreviewThumbnailController::onResult(application::PreviewThumbnailResult result) {
    if (stopped_) {
        return;
    }
    const std::shared_ptr<const application::SessionSnapshot> snapshot =
        dependencies_.snapshot ? dependencies_.snapshot() : nullptr;
    if (!snapshot || !snapshot->displayedFrame.has_value()) {
        return;
    }
    // Stale results from a previous session/epoch must not enter the cache.
    if (result.context.sessionId != snapshot->sessionId ||
        result.context.sessionEpoch != snapshot->sessionEpoch) {
        return;
    }
    // Failed terminal results release only their own request. Retries are user-driven.
    if (inflightContext_.has_value() && *inflightContext_ == result.context) {
        inflightFrame_ = -1;
        inflightContext_.reset();
        inflightIsBatch_ = false;
    }
    if (!result.available || result.rgba.empty() || result.width == 0U || result.height == 0U) {
        return;
    }
    {
        std::scoped_lock lock(cacheMutex_);
        if (cache_.size() >= kMaximumCachedFrames) {
            cache_.erase(cache_.begin());
        }
        cache_[result.frameId.value()] = RawThumbnail{
            std::move(result.rgba),
            static_cast<int>(result.width),
            static_cast<int>(result.height),
        };
    }
    // A superseded job can still deliver after its replacement was submitted. Its result is
    // cached (it is a valid decode of a frame that was requested) but must not clear the
    // newer job's in-flight bookkeeping, or the same frame would be re-submitted and decoded
    // from scratch on the next hover.
    if (!inflightContext_.has_value() || inflightContext_->requestId == result.context.requestId) {
        inflightFrame_ = -1;
        inflightContext_.reset();
        inflightIsBatch_ = false;
    }
    Q_EMIT thumbnailReady(result.frameId.value());
    Q_EMIT stateChanged();
}

void PreviewThumbnailController::scheduleRequest() {
    if (stopped_ || dependencies_.service == nullptr) {
        return;
    }
    const std::shared_ptr<const application::SessionSnapshot> snapshot =
        dependencies_.snapshot ? dependencies_.snapshot() : nullptr;
    if (!snapshot || !snapshot->validatedComparison ||
        snapshot->sessionState != domain::SessionState::kReady) {
        return;
    }
    // Interactive (hover) work always goes first. An interactive target preempts an
    // in-flight batch decode through the same cancel path that supersedes older hovers.
    if (pendingFrame_ >= 0) {
        if (pendingFrame_ == inflightFrame_) {
            return;
        }
        if (hasThumbnail(pendingFrame_)) {
            pendingFrame_ = -1;
            return;
        }

        // Hover preview describes the timeline master (canonical) source of the active set.
        const domain::SourceId canonicalId = snapshot->validatedComparison->canonicalSourceId();
        const domain::ComparisonSource* source = snapshot->validatedComparison->find(canonicalId);
        if (source == nullptr) {
            return;
        }

        application::PreviewThumbnailRequest request{
            .context = application::RequestContext{snapshot->sessionId,
                                                   snapshot->sessionEpoch,
                                                   domain::RequestId{nextRequestId_++}},
            .source = *source,
            .frameId = domain::FrameId{pendingFrame_},
        };
        if (!request.isValid()) {
            pendingFrame_ = -1;
            return;
        }
        if (inflightContext_.has_value()) {
            dependencies_.service->cancel(*inflightContext_);
        }
        if (dependencies_.service->submit(request, sink_) ==
            application::PortSubmitResult::Accepted) {
            inflightFrame_ = pendingFrame_;
            inflightContext_ = request.context;
            inflightIsBatch_ = false;
            pendingFrame_ = -1;
            Q_EMIT stateChanged();
        }
        return;
    }
    // Low-priority batch drain (V04 nearby-frames strip): one decode at a time, only while
    // no interactive request is pending. There is never more than one strip decode in flight,
    // so the prefetch path cannot add resident decode pressure beyond a single hover-sized job.
    while (!batchQueue_.empty()) {
        const qint64 frame = batchQueue_.front();
        if (frame < 0 || hasThumbnail(frame)) {
            batchQueue_.erase(batchQueue_.begin());
            continue;
        }
        if (frame == inflightFrame_) {
            return;
        }

        const domain::SourceId canonicalId = snapshot->validatedComparison->canonicalSourceId();
        const domain::ComparisonSource* source = snapshot->validatedComparison->find(canonicalId);
        if (source == nullptr) {
            // Cannot address the canonical source right now: drop the batch instead of
            // keeping stale frames queued (the card re-requests on its next change).
            batchQueue_.clear();
            return;
        }

        application::PreviewThumbnailRequest request{
            .context = application::RequestContext{snapshot->sessionId,
                                                   snapshot->sessionEpoch,
                                                   domain::RequestId{nextRequestId_++}},
            .source = *source,
            .frameId = domain::FrameId{frame},
        };
        if (!request.isValid()) {
            batchQueue_.clear();
            return;
        }
        if (inflightContext_.has_value()) {
            dependencies_.service->cancel(*inflightContext_);
        }
        if (dependencies_.service->submit(request, sink_) ==
            application::PortSubmitResult::Accepted) {
            inflightFrame_ = frame;
            inflightContext_ = request.context;
            inflightIsBatch_ = true;
            batchQueue_.erase(batchQueue_.begin());
            Q_EMIT stateChanged();
            return;
        }
        batchQueue_.clear();
        return;
    }
}

} // namespace dvs::ui
