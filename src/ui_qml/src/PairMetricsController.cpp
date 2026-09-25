#include "dvs/ui/PairMetricsController.h"

#include "dvs/application/ComparisonMetrics.h"
#include "dvs/application/FrameMapping.h"
#include "dvs/ui/ReviewController.h"

#include <QMetaObject>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>

namespace dvs::ui {
namespace {

// Playhead-centered sampling window when the timeline lane is enabled.
constexpr std::int64_t kLaneRadiusFrames = 150;
// Debounce between user interaction and submitting a metrics request.
constexpr int kRequestDebounceMilliseconds = 150;
// Hard cache bound. A session needing more samples clears and resamples around the playhead.
constexpr std::size_t kMaximumCachedSamples = 65'536U;

[[nodiscard]] bool sameSourcePair(const domain::SourceId first,
                                  const domain::SourceId second,
                                  const domain::SourceId scopeFirst,
                                  const domain::SourceId scopeSecond) noexcept {
    return (first == scopeFirst && second == scopeSecond) ||
           (first == scopeSecond && second == scopeFirst);
}

[[nodiscard]] std::vector<std::int64_t>
buildMappedSourceFrames(const application::SessionSnapshot& snapshot,
                        const std::vector<domain::ComparisonSource>& sources,
                        const domain::FrameId firstFrame,
                        const domain::FrameId lastFrame) {
    const auto frameCount = static_cast<std::size_t>(lastFrame.value() - firstFrame.value() + 1);
    std::vector<std::int64_t> mapped(frameCount * sources.size(), -1);
    const application::FrameMappingContext context{
        .sources = snapshot.validatedComparison.get(),
        .canonicalTimeline = snapshot.canonicalTimeline,
        .offsets = snapshot.alignmentOffsets,
        .mode = snapshot.alignmentMode,
        .sequenceMaps =
            snapshot.sequenceAlignmentMaps
                ? std::span<const application::SequenceAlignmentResult>{*snapshot
                                                                             .sequenceAlignmentMaps}
                : std::span<const application::SequenceAlignmentResult>{},
        .anchors = snapshot.manualAlignmentAnchors,
        .timelines = snapshot.sourceTimelines,
    };
    for (std::size_t index = 0; index < frameCount; ++index) {
        const domain::FrameId canonical{firstFrame.value() + static_cast<std::int64_t>(index)};
        const auto mappings = application::resolveSourceFrameMappings(context, canonical);
        for (std::size_t slot = 0; slot < sources.size(); ++slot) {
            const auto& source = sources[slot];
            const auto mapping =
                std::find_if(mappings.begin(), mappings.end(), [&source](const auto& value) {
                    return value.sourceId == source.id;
                });
            if (mapping != mappings.end() &&
                mapping->matchKind == application::FrameMatchKind::Missing) {
                continue;
            }
            const auto offset = mapping == mappings.end() ? 0 : mapping->frames;
            const auto frame = canonical.value();
            if ((offset < 0 && offset < -frame) ||
                (offset > 0 && frame > (std::numeric_limits<std::int64_t>::max)() - offset)) {
                continue;
            }
            const auto target = frame + offset;
            if (source.descriptor.frameCount.value > 0 &&
                target >= source.descriptor.frameCount.value) {
                continue;
            }
            mapped[index * sources.size() + slot] = target;
        }
    }
    return mapped;
}

} // namespace

PairMetricsController::Sink::Sink(PairMetricsController& owner) : owner_(&owner) {}

PairMetricsController::Sink::~Sink() = default;

void PairMetricsController::Sink::onPairMetricsBatch(application::PairMetricsBatch batch) {
    bool wake = false;
    {
        std::scoped_lock lock(mutex_);
        if (closed_) {
            return;
        }
        batches_.push_back(std::move(batch));
        wake = true;
    }
    if (wake) {
        QMetaObject::invokeMethod(owner_, &PairMetricsController::drainSink, Qt::QueuedConnection);
    }
}

void PairMetricsController::Sink::onPairMetricsFailure(application::PairMetricsFailure failure) {
    {
        std::scoped_lock lock(mutex_);
        if (closed_) {
            return;
        }
        if (!failure_.has_value()) {
            failure_ = std::move(failure);
        }
    }
    QMetaObject::invokeMethod(owner_, &PairMetricsController::drainSink, Qt::QueuedConnection);
}

void PairMetricsController::Sink::close() noexcept {
    std::scoped_lock lock(mutex_);
    closed_ = true;
    batches_.clear();
    failure_.reset();
}

std::optional<application::PairMetricsFailure> PairMetricsController::Sink::takeFailure() {
    std::scoped_lock lock(mutex_);
    return std::move(failure_);
}

std::vector<application::PairMetricsBatch> PairMetricsController::Sink::takeBatches() {
    std::scoped_lock lock(mutex_);
    std::vector<application::PairMetricsBatch> batches;
    batches.reserve(batches_.size());
    while (!batches_.empty()) {
        batches.push_back(std::move(batches_.front()));
        batches_.pop_front();
    }
    return batches;
}

PairMetricsController::PairMetricsController(Dependencies dependencies, QObject* parent)
    : QObject(parent), dependencies_(std::move(dependencies)),
      sink_(std::make_shared<Sink>(*this)) {
    metricId_ = QString::fromStdString(std::string{application::kRgbAbsoluteMetricId});
    requestTimer_ = new QTimer(this);
    requestTimer_->setSingleShot(true);
    requestTimer_->setInterval(kRequestDebounceMilliseconds);
    QObject::connect(requestTimer_, &QTimer::timeout, this, [this] { submitRequest(); });
}

PairMetricsController::~PairMetricsController() {
    stop();
}

bool PairMetricsController::available() const noexcept {
    return available_;
}

bool PairMetricsController::laneEnabled() const noexcept {
    return laneEnabled_;
}

void PairMetricsController::setLaneEnabled(const bool value) {
    if (laneEnabled_ == value) {
        return;
    }
    laneEnabled_ = value;
    emit stateChanged();
    scheduleRequest();
}

bool PairMetricsController::sampling() const noexcept {
    return sampling_;
}

const PairMetricsController::Sample* PairMetricsController::currentSample() const noexcept {
    if (!dependencies_.snapshot) {
        return nullptr;
    }
    const std::shared_ptr<const application::SessionSnapshot> snapshot = dependencies_.snapshot();
    if (!snapshot || !snapshot->displayedFrame.has_value()) {
        return nullptr;
    }
    const auto found = samples_.find(snapshot->displayedFrame->value());
    return found == samples_.end() ? nullptr : &found->second;
}

bool PairMetricsController::hasCurrentSample() const noexcept {
    return currentSample() != nullptr;
}

bool PairMetricsController::currentComparable() const noexcept {
    const Sample* const sample = currentSample();
    return sample != nullptr && sample->comparable;
}

qreal PairMetricsController::currentMae() const noexcept {
    const Sample* const sample = currentSample();
    return sample != nullptr ? sample->metrics.mae : 0.0;
}

qreal PairMetricsController::currentMse() const noexcept {
    const Sample* const sample = currentSample();
    return sample != nullptr ? sample->metrics.mse : 0.0;
}

qreal PairMetricsController::currentPsnrDb() const noexcept {
    const Sample* const sample = currentSample();
    return sample != nullptr ? sample->metrics.psnrDb : 0.0;
}

qreal PairMetricsController::currentMaxAbsError() const noexcept {
    const Sample* const sample = currentSample();
    return sample != nullptr ? sample->metrics.maxAbsError : 0.0;
}

qreal PairMetricsController::currentMismatchRatio() const noexcept {
    const Sample* const sample = currentSample();
    return sample != nullptr ? sample->metrics.mismatchRatio : 0.0;
}

qulonglong PairMetricsController::currentMismatchPixels() const noexcept {
    const Sample* const sample = currentSample();
    return sample != nullptr ? sample->metrics.mismatchPixels : 0U;
}

qulonglong PairMetricsController::currentPixelCount() const noexcept {
    const Sample* const sample = currentSample();
    return sample != nullptr ? sample->metrics.pixelCount : 0U;
}

QString PairMetricsController::metricId() const {
    return metricId_;
}

QString PairMetricsController::errorKey() const {
    return errorKey_;
}

int PairMetricsController::threshold() const noexcept {
    return threshold_;
}

void PairMetricsController::setThreshold(const int value) {
    const int clamped = std::clamp(value, 0, 255);
    if (threshold_ == clamped) {
        return;
    }
    threshold_ = clamped;
    emit thresholdChanged();
    refresh();
}

int PairMetricsController::thresholdPolicy() const noexcept {
    return static_cast<int>(thresholdPolicy_);
}

void PairMetricsController::setThresholdPolicy(const int value) {
    // Mirrors presentation::ThresholdPolicy values; unsupported values are ignored instead of
    // being clamped so the statistics never apply a rule the highlight does not show.
    const auto policy = static_cast<domain::MismatchPolicy>(value);
    if (value < static_cast<int>(domain::MismatchPolicy::LumaOnly) ||
        value > static_cast<int>(domain::MismatchPolicy::AllChannels) ||
        thresholdPolicy_ == policy) {
        return;
    }
    thresholdPolicy_ = policy;
    emit thresholdPolicyChanged();
    refresh();
}

qint64 PairMetricsController::sampleCount() const noexcept {
    return static_cast<qint64>(samples_.size());
}

qint64 PairMetricsController::sampleFirstFrame() const noexcept {
    return sampleFirstFrame_;
}

qint64 PairMetricsController::sampleLastFrame() const noexcept {
    return sampleLastFrame_;
}

qreal PairMetricsController::sampleMaxMae() const noexcept {
    return sampleMaxMae_;
}

void PairMetricsController::attachReviewController(ReviewController& controller) {
    QObject::connect(
        &controller, &ReviewController::stateChanged, this, [this] { onReviewStateChanged(); });
    QObject::connect(&controller, &ReviewController::frameStateChanged, this, [this] {
        onReviewFrameStateChanged();
    });
}

void PairMetricsController::refresh() {
    std::shared_ptr<const application::SessionSnapshot> snapshot;
    if (dependencies_.snapshot) {
        snapshot = dependencies_.snapshot();
    }

    std::optional<Scope> next;
    bool available = false;
    if (snapshot && snapshot->validatedComparison && snapshot->activeComparisonPair.has_value() &&
        snapshot->validatedComparison->sourceCount() >= 2U) {
        const domain::ComparisonPair& pair = *snapshot->activeComparisonPair;
        next = Scope{snapshot->sessionId,
                     snapshot->sessionEpoch,
                     pair.first,
                     pair.second,
                     snapshot->alignmentRevision,
                     threshold_,
                     thresholdPolicy_};
        available = true;
    }
    const bool availabilityChanged = available != available_;
    available_ = available;
    applyScopeChange(next);
    if (availabilityChanged) {
        emit stateChanged();
    }
    onReviewFrameStateChanged();
}

void PairMetricsController::clear() {
    applyScopeChange(std::nullopt);
    available_ = false;
    emit stateChanged();
    emit samplesChanged();
}

qreal PairMetricsController::maeAt(const qint64 frame) const {
    const auto found = samples_.find(frame);
    return found == samples_.end() ? -1.0 : found->second.metrics.mae;
}

bool PairMetricsController::comparableAt(const qint64 frame) const {
    const auto found = samples_.find(frame);
    return found != samples_.end() && found->second.comparable;
}

QVariantList PairMetricsController::peakFrames(const int maximumCount,
                                               const qreal neighborhoodFrames) const {
    QVariantList peaks;
    if (maximumCount <= 0 || samples_.empty() || neighborhoodFrames <= 0.0) {
        return peaks;
    }
    struct Candidate final {
        std::int64_t frame = 0;
        double mae = 0.0;
    };
    std::vector<Candidate> candidates;
    for (const auto& [frame, sample] : samples_) {
        if (!sample.comparable || sample.metrics.mae <= 0.0) {
            continue;
        }
        // A frame is a peak only when it strictly dominates every comparable neighbor in the
        // window; plateaus are deliberately not peaks (ties disqualify both sides) so a flat
        // region never produces a marker per frame.
        bool isPeak = true;
        const auto lower = samples_.lower_bound(
            static_cast<std::int64_t>(static_cast<qreal>(frame) - neighborhoodFrames));
        for (auto iterator = lower;
             iterator != samples_.end() &&
             static_cast<qreal>(iterator->first) <= static_cast<qreal>(frame) + neighborhoodFrames;
             ++iterator) {
            if (iterator->first == frame) {
                continue;
            }
            if (!iterator->second.comparable) {
                continue;
            }
            if (iterator->second.metrics.mae >= sample.metrics.mae) {
                isPeak = false;
                break;
            }
        }
        if (isPeak) {
            candidates.push_back(Candidate{frame, sample.metrics.mae});
        }
    }
    std::sort(candidates.begin(),
              candidates.end(),
              [](const Candidate& left, const Candidate& right) { return left.mae > right.mae; });
    const std::size_t limit =
        std::min<std::size_t>(candidates.size(), static_cast<std::size_t>(maximumCount));
    for (std::size_t index = 0; index < limit; ++index) {
        QVariantMap entry;
        entry.insert(QStringLiteral("frame"), QVariant::fromValue<qint64>(candidates[index].frame));
        entry.insert(QStringLiteral("mae"), QVariant::fromValue<qreal>(candidates[index].mae));
        peaks.append(entry);
    }
    return peaks;
}

QVariantList PairMetricsController::samplePoints(const int maximumPoints) const {
    QVariantList points;
    if (maximumPoints <= 0 || samples_.empty() || sampleFirstFrame_ < 0 ||
        sampleLastFrame_ < sampleFirstFrame_) {
        return points;
    }
    const std::int64_t span = sampleLastFrame_ - sampleFirstFrame_ + 1;
    const std::int64_t bucketCount =
        std::min<std::int64_t>(span, static_cast<std::int64_t>(maximumPoints));
    if (bucketCount <= 0) {
        return points;
    }
    const double bucketWidth = static_cast<double>(span) / static_cast<double>(bucketCount);
    std::int64_t bucketStart = sampleFirstFrame_;
    for (std::int64_t bucket = 0; bucket < bucketCount; ++bucket) {
        const double rawEnd =
            static_cast<double>(sampleFirstFrame_) + static_cast<double>(bucket + 1) * bucketWidth;
        const std::int64_t bucketEnd =
            (bucket + 1 == bucketCount)
                ? sampleLastFrame_
                : std::min<std::int64_t>(sampleLastFrame_, static_cast<std::int64_t>(rawEnd) - 1);
        bool anyComparable = false;
        std::int64_t strongestFrame = bucketStart;
        double strongestMae = -1.0;
        for (auto iterator = samples_.lower_bound(bucketStart);
             iterator != samples_.end() && iterator->first <= bucketEnd;
             ++iterator) {
            if (iterator->second.comparable) {
                anyComparable = true;
                if (iterator->second.metrics.mae > strongestMae) {
                    strongestMae = iterator->second.metrics.mae;
                    strongestFrame = iterator->first;
                }
            }
        }
        QVariantMap entry;
        entry.insert(QStringLiteral("frame"), QVariant::fromValue<qint64>(strongestFrame));
        entry.insert(QStringLiteral("mae"),
                     QVariant::fromValue<qreal>(anyComparable ? strongestMae : 0.0));
        entry.insert(QStringLiteral("comparable"), QVariant::fromValue(anyComparable));
        points.append(entry);
        bucketStart = bucketEnd + 1;
        if (bucketStart > sampleLastFrame_) {
            break;
        }
    }
    return points;
}

void PairMetricsController::stop() noexcept {
    if (requestTimer_ != nullptr) {
        requestTimer_->stop();
    }
    if (sink_ != nullptr) {
        sink_->close();
    }
    if (hasLastRequest_ && dependencies_.service != nullptr) {
        dependencies_.service->cancel(lastRequestContext_);
    }
    hasLastRequest_ = false;
}

void PairMetricsController::onReviewStateChanged() {
    refresh();
}

void PairMetricsController::onReviewFrameStateChanged() {
    emit stateChanged();
    scheduleRequest();
}

void PairMetricsController::scheduleRequest() {
    if (requestTimer_ == nullptr) {
        return;
    }
    if (!requestTimer_->isActive()) {
        requestTimer_->start(kRequestDebounceMilliseconds);
    }
}

void PairMetricsController::applyScopeChange(const std::optional<Scope>& next) {
    if (next == scope_) {
        return;
    }
    if (hasLastRequest_ && dependencies_.service != nullptr) {
        dependencies_.service->cancel(lastRequestContext_);
    }
    hasLastRequest_ = false;
    resetInflight();
    samples_.clear();
    sampleFirstFrame_ = -1;
    sampleLastFrame_ = -1;
    sampleMaxMae_ = 0.0;
    sampling_ = false;
    errorKey_.clear();
    scope_ = next;
    emit samplesChanged();
}

void PairMetricsController::resetInflight() noexcept {
    inflightFirst_ = -1;
    inflightLast_ = -1;
}

void PairMetricsController::submitRequest() {
    if (!scope_.has_value() || !available_) {
        sampling_ = false;
        emit stateChanged();
        return;
    }
    std::shared_ptr<const application::SessionSnapshot> snapshot;
    if (dependencies_.snapshot) {
        snapshot = dependencies_.snapshot();
    }
    if (!snapshot || snapshot->validatedComparison == nullptr ||
        !snapshot->displayedFrame.has_value() || snapshot->canonicalFrameCount <= 0U ||
        !snapshot->activeComparisonPair.has_value()) {
        emit stateChanged();
        return;
    }

    const domain::ComparisonSource* const first =
        snapshot->validatedComparison->find(scope_->firstSource);
    const domain::ComparisonSource* const second =
        snapshot->validatedComparison->find(scope_->secondSource);
    if (first == nullptr || second == nullptr) {
        emit stateChanged();
        return;
    }
    const std::int64_t center = snapshot->displayedFrame->value();
    const std::int64_t count = static_cast<std::int64_t>(snapshot->canonicalFrameCount);
    const std::int64_t radius = laneEnabled_ ? kLaneRadiusFrames : 0;
    const std::int64_t windowFirst = std::max<std::int64_t>(0, center - radius);
    const std::int64_t windowLast = std::min<std::int64_t>(count - 1, center + radius);

    // Find the contiguous run of unsampled frames nearest the playhead. Frames already cached
    // or covered by the in-flight request are not requested again.
    std::int64_t runFirst = -1;
    std::int64_t runLast = -1;
    std::int64_t bestRunFirst = -1;
    std::int64_t bestRunLast = -1;
    std::int64_t bestRunDistance = std::numeric_limits<std::int64_t>::max();
    const auto closeRun = [&](const std::int64_t firstFrame, const std::int64_t lastFrame) {
        if (firstFrame < 0) {
            return;
        }
        const std::int64_t distance = center < firstFrame  ? firstFrame - center
                                      : center > lastFrame ? center - lastFrame
                                                           : 0;
        if (distance < bestRunDistance) {
            bestRunDistance = distance;
            bestRunFirst = firstFrame;
            bestRunLast = lastFrame;
        }
    };
    for (std::int64_t frame = windowFirst; frame <= windowLast; ++frame) {
        const bool pending =
            inflightFirst_ >= 0 && frame >= inflightFirst_ && frame <= inflightLast_;
        const bool missing = samples_.find(frame) == samples_.end() && !pending;
        if (missing) {
            if (runFirst < 0) {
                runFirst = frame;
            }
            runLast = frame;
        } else {
            closeRun(runFirst, runLast);
            runFirst = -1;
            runLast = -1;
        }
    }
    closeRun(runFirst, runLast);
    if (bestRunFirst < 0) {
        sampling_ = false;
        emit stateChanged();
        return;
    }

    application::PairMetricsRequest request{
        .context =
            application::PlaybackRequestContext{
                application::RequestContext{
                    scope_->sessionId, scope_->sessionEpoch, domain::RequestId{nextRequestId_++}},
                snapshot->playbackGeneration},
        .sources = {*first, *second},
        .offsets = snapshot->alignmentOffsets,
        .mappedSourceFrames = buildMappedSourceFrames(*snapshot,
                                                      {*first, *second},
                                                      domain::FrameId{bestRunFirst},
                                                      domain::FrameId{bestRunLast}),
        .alignmentRevision = snapshot->alignmentRevision,
        .firstFrame = domain::FrameId{bestRunFirst},
        .lastFrame = domain::FrameId{bestRunLast},
        .mismatchThreshold = static_cast<std::uint8_t>(threshold_),
        .mismatchPolicy = thresholdPolicy_,
    };

    if (dependencies_.service == nullptr) {
        errorKey_ = QStringLiteral("pairMetricsUnavailable");
        emit stateChanged();
        return;
    }
    const application::PortSubmitResult result = dependencies_.service->submit(request, sink_);
    if (result == application::PortSubmitResult::Accepted) {
        sampling_ = true;
        inflightFirst_ = bestRunFirst;
        inflightLast_ = bestRunLast;
        lastRequestContext_ = request.context;
        hasLastRequest_ = true;
        errorKey_.clear();
    } else {
        errorKey_ = QStringLiteral("pairMetricsUnavailable");
        sampling_ = false;
    }
    emit stateChanged();
}

void PairMetricsController::drainSink() {
    if (sink_ == nullptr) {
        return;
    }
    const std::optional<application::PairMetricsFailure> failure = sink_->takeFailure();
    const std::vector<application::PairMetricsBatch> batches = sink_->takeBatches();

    bool stateChangedNow = false;
    if (failure.has_value()) {
        errorKey_ = QString::fromStdString(failure->error.userMessageKey);
        sampling_ = false;
        resetInflight();
        stateChangedNow = true;
    }

    bool samplesChangedNow = false;
    for (const application::PairMetricsBatch& batch : batches) {
        if (!scope_.has_value() || batch.context.request.sessionId != scope_->sessionId ||
            batch.context.request.sessionEpoch != scope_->sessionEpoch ||
            batch.alignmentRevision != scope_->alignmentRevision ||
            batch.mismatchThreshold != static_cast<std::uint8_t>(threshold_) ||
            batch.mismatchPolicy != thresholdPolicy_ || batch.sources.size() != 2U ||
            !sameSourcePair(batch.sources[0].id,
                            batch.sources[1].id,
                            scope_->firstSource,
                            scope_->secondSource)) {
            continue;
        }
        if (!batch.metricId.empty()) {
            metricId_ = QString::fromStdString(batch.metricId);
        }
        for (const application::PairMetricsSample& sample : batch.samples) {
            const std::int64_t frame = sample.canonicalFrameId.value();
            samples_.insert_or_assign(frame, Sample{sample.comparable, sample.metrics});
            if (sample.comparable) {
                sampleMaxMae_ = std::max<qreal>(sampleMaxMae_, sample.metrics.mae);
            }
            if (sampleFirstFrame_ < 0 || frame < sampleFirstFrame_) {
                sampleFirstFrame_ = frame;
            }
            if (frame > sampleLastFrame_) {
                sampleLastFrame_ = frame;
            }
            samplesChangedNow = true;
        }
        if (batch.finalBatch) {
            sampling_ = false;
            resetInflight();
        }
        stateChangedNow = true;
    }

    if (samples_.size() > kMaximumCachedSamples) {
        if (hasLastRequest_ && dependencies_.service != nullptr) {
            dependencies_.service->cancel(lastRequestContext_);
        }
        hasLastRequest_ = false;
        resetInflight();
        samples_.clear();
        sampleFirstFrame_ = -1;
        sampleLastFrame_ = -1;
        sampleMaxMae_ = 0.0;
        samplesChangedNow = true;
    }

    if (stateChangedNow) {
        emit stateChanged();
    }
    if (samplesChangedNow) {
        emit samplesChanged();
    }
}

} // namespace dvs::ui
