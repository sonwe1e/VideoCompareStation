#include "dvs/ui/ImagePairLoader.h"

#include "ByteLruCache.h"
#include "ImageHeaderProbe.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QRunnable>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <utility>
namespace dvs::ui {
namespace {
constexpr int kMaximumImageEdge = 8192;
constexpr qint64 kMaximumDecodedBytes = 128LL * 1024LL * 1024LL;
constexpr qint64 kMaximumEncodedBytes = 512LL * 1024LL * 1024LL;
class FunctionRunnable final : public QRunnable {
public:
    explicit FunctionRunnable(std::function<void()> function) : function_(std::move(function)) {
        setAutoDelete(true);
    }
    void run() override {
        function_();
    }

private:
    std::function<void()> function_;
};
[[nodiscard]] QString sourceLabel(const QUrl& url) {
    return url.isLocalFile() ? url.toLocalFile() : url.toString();
}
[[nodiscard]] QString fileIdentity(const QUrl& url, const std::uint64_t policyRevision) {
    const QString label = sourceLabel(url);
    if (label.isEmpty()) {
        return {};
    }
    const QFileInfo info{label};
    return QStringLiteral("file:%1|size:%2|mtime:%3|policy:%4")
        .arg(QDir::cleanPath(info.absoluteFilePath()))
        .arg(info.size())
        .arg(info.lastModified().toMSecsSinceEpoch())
        .arg(policyRevision);
}
[[nodiscard]] bool dimensionsWithinBudget(const int width, const int height, QString* error) {
    if (width <= 0 || height <= 0) {
        if (error) {
            *error = QObject::tr("Image dimensions are empty.");
        }
        return false;
    }
    if (width > kMaximumImageEdge || height > kMaximumImageEdge) {
        if (error) {
            *error = QObject::tr("Image is larger than %1 px on a side.").arg(kMaximumImageEdge);
        }
        return false;
    }
    const qint64 decodedBytes = static_cast<qint64>(width) * static_cast<qint64>(height) * 4LL;
    if (decodedBytes > kMaximumDecodedBytes) {
        if (error) {
            *error = QObject::tr("Image exceeds the decoded-image memory budget.");
        }
        return false;
    }
    return true;
}
[[nodiscard]] bool loadImageFromDisk(const QUrl& url,
                                     const ImagePairLoader::DecodePolicy& policy,
                                     QImage* image,
                                     QImage* nativeImage,
                                     StillImageSourceInfo* info,
                                     QString* identity,
                                     QString* error,
                                     const std::atomic_bool& cancelled) {
    if (image == nullptr) {
        return false;
    }
    if (cancelled.load()) {
        return false;
    }
    const QString label = sourceLabel(url);
    if (label.isEmpty()) {
        if (error) {
            *error = QObject::tr("Invalid image path.");
        }
        return false;
    }
    if (identity != nullptr) {
        *identity = fileIdentity(url, policy.revision);
    }
    QFile file(label);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QObject::tr("Could not open image: %1").arg(QFileInfo(label).fileName());
        }
        return false;
    }
    if (file.size() > kMaximumEncodedBytes) {
        if (error) {
            *error = QObject::tr("Image file is too large to read.");
        }
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.isEmpty()) {
        if (error) {
            *error = QObject::tr("Could not open image: %1").arg(QFileInfo(label).fileName());
        }
        return false;
    }
    if (cancelled.load()) {
        return false;
    }
    QSize headerSize;
    bool haveHeader = false;
    if (policy.probe) {
        haveHeader = policy.probe(bytes, &headerSize);
    }
    if (!haveHeader) {
        haveHeader = probeImageHeader(bytes, &headerSize);
    }
    if (haveHeader) {
        QString dimensionError;
        if (!dimensionsWithinBudget(headerSize.width(), headerSize.height(), &dimensionError)) {
            if (error) {
                *error = dimensionError;
            }
            return false;
        }
    }
    if (cancelled.load()) {
        return false;
    }
    QImage decoded;
    QImage nativeDecoded;
    bool decodedOk = false;
    std::string decoderError;
    if (policy.loader) {
        try {
            decodedOk = policy.loader(bytes, &decoded, &nativeDecoded, info, &decoderError);
        } catch (const std::exception& exception) {
            decoderError = exception.what();
            decodedOk = false;
        } catch (...) {
            decoderError = "Still-image loader threw an unknown exception.";
            decodedOk = false;
        }
    }
    if (cancelled.load()) {
        return false;
    }
    if (!decodedOk) {
        decoded = QImage{};
        if (!decoded.loadFromData(bytes) || decoded.isNull()) {
            if (error) {
                *error =
                    decoderError.empty()
                        ? QObject::tr("Could not open image: %1").arg(QFileInfo(label).fileName())
                        : QString::fromStdString(decoderError);
            }
            return false;
        }
        // Qt's own loader gives no source provenance beyond the decoded format.
        if (info != nullptr) {
            info->sourceFormat = QStringLiteral("rgba8 (qt)");
            info->channels = 4;
            info->hasAlpha = decoded.hasAlphaChannel();
        }
    }
    if (decoded.isNull()) {
        if (error) {
            *error = QObject::tr("Decoded image is empty.");
        }
        return false;
    }
    QString dimensionError;
    if (!dimensionsWithinBudget(decoded.width(), decoded.height(), &dimensionError)) {
        if (error) {
            *error = dimensionError;
        }
        return false;
    }
    if (cancelled.load()) {
        return false;
    }
    if (decoded.format() != QImage::Format_RGBA8888) {
        decoded = decoded.convertToFormat(QImage::Format_RGBA8888);
    }
    if (decoded.isNull()) {
        if (error) {
            *error = QObject::tr("Decoded image conversion failed.");
        }
        return false;
    }
    if (info != nullptr) {
        // Only fall back to the buffer when the source provenance is unknown: a loader-
        // reported gray source (hasAlpha=false) must not be re-labeled alpha just because
        // the display buffer is RGBA8.
        if (info->sourceFormat.isEmpty()) {
            info->hasAlpha = decoded.hasAlphaChannel();
            info->channels = 4;
        }
        // Display conversion happened whenever the source bit depth, channel layout or
        // format differs from plain 8-bit RGBA; the provider fills the real source fields.
        info->displayConverted =
            info->bitDepth != 8 || info->channels != 4 ||
            (!info->sourceFormat.isEmpty() && info->sourceFormat != QStringLiteral("rgba"));
    }
    *image = std::move(decoded);
    if (nativeImage != nullptr) {
        *nativeImage = std::move(nativeDecoded);
    }
    return true;
}
[[nodiscard]] int clampChannel(const int value) noexcept {
    return std::clamp(value, 0, 255);
}
[[nodiscard]] QRgb absDiffPixel(const QRgb a, const QRgb b, const int gain) noexcept {
    const int red = clampChannel(std::abs(qRed(a) - qRed(b)) * gain);
    const int green = clampChannel(std::abs(qGreen(a) - qGreen(b)) * gain);
    const int blue = clampChannel(std::abs(qBlue(a) - qBlue(b)) * gain);
    return qRgb(red, green, blue);
}
[[nodiscard]] QRgb signedDiffPixel(const QRgb a, const QRgb b, const int gain) noexcept {
    const int red = clampChannel(128 + (qRed(a) - qRed(b)) * gain);
    const int green = clampChannel(128 + (qGreen(a) - qGreen(b)) * gain);
    const int blue = clampChannel(128 + (qBlue(a) - qBlue(b)) * gain);
    return qRgb(red, green, blue);
}
[[nodiscard]] QRgb highlightPixel(const QRgb base, const QRgb other, const int gain) noexcept {
    const int delta = std::max({std::abs(qRed(base) - qRed(other)),
                                std::abs(qGreen(base) - qGreen(other)),
                                std::abs(qBlue(base) - qBlue(other))});
    if (delta <= 0) {
        return base | 0xff000000;
    }
    const int strength = clampChannel(delta * gain);
    const int red = clampChannel(qRed(base) + (255 - qRed(base)) * strength / 255);
    const int green = clampChannel(qGreen(base) + (40 - qGreen(base)) * strength / 255);
    const int blue = clampChannel(qBlue(base) + (90 - qBlue(base)) * strength / 255);
    return qRgb(red, green, blue);
}
// Alpha difference map: brightness encodes |alphaA - alphaB| × gain as a grayscale value.
// Both buffers carry straight (unassociated) alpha, so the subtraction is representation-safe.
[[nodiscard]] QRgb alphaDiffPixel(const QRgb a, const QRgb b, const int gain) noexcept {
    const int delta = clampChannel(std::abs(qAlpha(a) - qAlpha(b)) * gain);
    return qRgb(delta, delta, delta);
}
[[nodiscard]] ImagePairLoader::DifferenceResult
computeDifference(QImage primary,
                  QImage secondary,
                  const int compareMode,
                  const bool resampleAllowed,
                  const std::atomic_bool& cancelled,
                  const std::function<void(int)>& rowObserver) {
    ImagePairLoader::DifferenceResult result;
    if (primary.isNull() || secondary.isNull()) {
        result.error = QObject::tr("Image pair is empty.");
        return result;
    }
    if (primary.width() <= 0 || primary.height() <= 0 || primary.width() > kMaximumImageEdge ||
        primary.height() > kMaximumImageEdge || secondary.width() <= 0 || secondary.height() <= 0 ||
        secondary.width() > kMaximumImageEdge || secondary.height() > kMaximumImageEdge) {
        result.error = QObject::tr("Image pair is empty or too large to diff.");
        return result;
    }
    if (cancelled.load()) {
        return result;
    }
    const QImage left = primary.convertToFormat(QImage::Format_ARGB32);
    if (cancelled.load()) {
        return result;
    }
    QImage right = secondary.convertToFormat(QImage::Format_ARGB32);
    if (cancelled.load()) {
        return result;
    }
    if (right.size() != left.size()) {
        if (!resampleAllowed) {
            result.error =
                QObject::tr("A 与 B 尺寸不同（%1×%2 与 %3×%4），未启用重采样时不计算逐像素差异。")
                    .arg(primary.width())
                    .arg(primary.height())
                    .arg(secondary.width())
                    .arg(secondary.height());
            return result;
        }
        right = right.scaled(left.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        result.resampled = true;
    }
    if (cancelled.load()) {
        return result;
    }
    QImage output(left.width(), left.height(), QImage::Format_ARGB32);
    if (output.isNull()) {
        result.error = QObject::tr("Could not allocate difference image.");
        return result;
    }
    qint64 sum = 0;
    qint64 alphaSum = 0;
    qint64 alphaChanged = 0;
    int peak = 0;
    int peakAlpha = 0;
    bool hasAlpha = false;
    constexpr int kGain = 4;
    for (int y = 0; y < left.height(); ++y) {
        if (rowObserver) {
            rowObserver(y);
        }
        if (cancelled.load()) {
            return result;
        }
        const auto* leftLine = reinterpret_cast<const QRgb*>(left.constScanLine(y));
        const auto* rightLine = reinterpret_cast<const QRgb*>(right.constScanLine(y));
        auto* outLine = reinterpret_cast<QRgb*>(output.scanLine(y));
        for (int x = 0; x < left.width(); ++x) {
            const QRgb a = leftLine[x];
            const QRgb b = rightLine[x];
            const int delta = std::max({std::abs(qRed(a) - qRed(b)),
                                        std::abs(qGreen(a) - qGreen(b)),
                                        std::abs(qBlue(a) - qBlue(b))});
            const int alphaDelta = std::abs(qAlpha(a) - qAlpha(b));
            sum += delta;
            peak = std::max(peak, delta);
            peakAlpha = std::max(peakAlpha, alphaDelta);
            // Alpha statistics cover the straight (unassociated) alpha of both decoded
            // buffers, so a transparency regression is visible even when RGB matches.
            alphaSum += alphaDelta;
            if (alphaDelta > 0) {
                ++alphaChanged;
            }
            if (qAlpha(a) < 255 || qAlpha(b) < 255) {
                hasAlpha = true;
            }
            switch (compareMode) {
            case 3: // ImageReviewController::SignedDifference
                outLine[x] = signedDiffPixel(a, b, kGain);
                break;
            case 4: // ImageReviewController::Highlight
                outLine[x] = highlightPixel(a, b, kGain);
                break;
            case 6: // ImageReviewController::AlphaDifference
                outLine[x] = alphaDiffPixel(a, b, kGain);
                break;
            case 2: // ImageReviewController::AbsDifference
            default:
                outLine[x] = absDiffPixel(a, b, kGain);
                break;
            }
        }
    }
    const qint64 pixelCount =
        static_cast<qint64>(left.width()) * static_cast<qint64>(left.height());
    result.image = std::move(output);
    result.maxAbsDifference = peak;
    result.meanAbsDifference = pixelCount > 0 ? static_cast<double>(sum) / pixelCount : 0.0;
    result.peakAlphaDifference = peakAlpha;
    result.meanAlphaDifference = pixelCount > 0 ? static_cast<double>(alphaSum) / pixelCount : 0.0;
    result.alphaChangedPixels = alphaChanged;
    result.hasAlpha = hasAlpha;
    result.alphaDifferenceOnly = peak == 0 && peakAlpha > 0;
    return result;
}
struct RequestState final {
    quint64 requestId = 0;
    bool prefetch = false;
    bool difference = false;
    std::atomic_bool cancelled{false};
    ImagePairLoader::ResultHandler resultHandler;
    ImagePairLoader::DifferenceHandler differenceHandler;
};
// Decoded image plus its source provenance, cached together so a cache hit never loses the
// metadata that separates display-converted samples from original code values. The native
// RGBA64 sidecar rides along for high-bit-depth sources.
struct DecodedCacheEntry final {
    QImage image;
    QImage native;
    StillImageSourceInfo info;
};
struct LoadJob final {
    enum class Kind {
        Primary,
        Secondary,
        Pair,
    };
    quint64 requestId = 0;
    Kind kind = Kind::Primary;
    bool prefetch = false;
    QUrl primaryUrl;
    QUrl secondaryUrl;
    int pairId = -1;
    QString primaryIdentity;
    QString secondaryIdentity;
    std::optional<DecodedCacheEntry> cachedPrimary;
    std::optional<DecodedCacheEntry> cachedSecondary;
    ImagePairLoader::DecodePolicy policy;
};
struct DifferenceJob final {
    quint64 requestId = 0;
    QImage primary;
    QImage secondary;
    int compareMode = 0;
    bool resample = false;
};
} // namespace
class ImagePairLoader::Impl final {
public:
    explicit Impl(ImagePairLoader* owner) : owner_(owner) {
        pool_.setMaxThreadCount(2);
        pool_.setExpiryTimeout(3000);
        cache_.setMaximumBytes(128LL * 1024LL * 1024LL);
    }
    ~Impl() {
        shutdown();
    }
    void shutdown() {
        cancelAll();
        pool_.waitForDone();
        active_.clear();
    }
    quint64 requestPrimary(const QUrl& primary,
                           const int pairId,
                           const DecodePolicy& policy,
                           ResultHandler handler) {
        LoadJob job;
        job.kind = LoadJob::Kind::Primary;
        job.primaryUrl = primary;
        job.pairId = pairId;
        job.policy = policy;
        job.primaryIdentity = identityFor(primary, policy.revision);
        job.cachedPrimary = lookup(job.primaryIdentity);
        return enqueueLoad(std::move(job), std::move(handler), /*prefetch=*/false);
    }
    quint64 requestSecondary(const QUrl& secondary,
                             const int pairId,
                             const DecodePolicy& policy,
                             ResultHandler handler) {
        LoadJob job;
        job.kind = LoadJob::Kind::Secondary;
        job.secondaryUrl = secondary;
        job.pairId = pairId;
        job.policy = policy;
        job.secondaryIdentity = identityFor(secondary, policy.revision);
        job.cachedSecondary = lookup(job.secondaryIdentity);
        return enqueueLoad(std::move(job), std::move(handler), /*prefetch=*/false);
    }
    quint64 requestPair(const QUrl& primary,
                        const QUrl& secondary,
                        const int pairId,
                        const DecodePolicy& policy,
                        ResultHandler handler) {
        LoadJob job;
        job.kind = LoadJob::Kind::Pair;
        job.primaryUrl = primary;
        job.secondaryUrl = secondary;
        job.pairId = pairId;
        job.policy = policy;
        job.primaryIdentity = identityFor(primary, policy.revision);
        job.secondaryIdentity = identityFor(secondary, policy.revision);
        job.cachedPrimary = lookup(job.primaryIdentity);
        job.cachedSecondary = lookup(job.secondaryIdentity);
        return enqueueLoad(std::move(job), std::move(handler), /*prefetch=*/false);
    }
    quint64 prefetchPair(const QUrl& primary, const QUrl& secondary, const DecodePolicy& policy) {
        LoadJob job;
        job.kind = LoadJob::Kind::Pair;
        job.prefetch = true;
        job.primaryUrl = primary;
        job.secondaryUrl = secondary;
        job.policy = policy;
        job.primaryIdentity = identityFor(primary, policy.revision);
        job.secondaryIdentity = identityFor(secondary, policy.revision);
        job.cachedPrimary = lookup(job.primaryIdentity);
        job.cachedSecondary = lookup(job.secondaryIdentity);
        return enqueueLoad(std::move(job), {}, /*prefetch=*/true);
    }
    quint64 requestDifference(QImage primary,
                              QImage secondary,
                              const int compareMode,
                              const bool resample,
                              DifferenceHandler handler) {
        DifferenceJob job;
        job.primary = std::move(primary);
        job.secondary = std::move(secondary);
        job.compareMode = compareMode;
        job.resample = resample;
        const quint64 requestId = nextRequestId_++;
        job.requestId = requestId;
        auto state = std::make_shared<RequestState>();
        state->requestId = requestId;
        state->difference = true;
        state->differenceHandler = std::move(handler);
        active_.emplace(requestId, state);
        schedule(state,
                 [this,
                  requestId,
                  job = std::move(job),
                  state,
                  observer = differenceRowObserver_]() mutable {
                     DifferenceResult result;
                     result.requestId = requestId;
                     if (!state->cancelled.load()) {
                         result = computeDifference(std::move(job.primary),
                                                    std::move(job.secondary),
                                                    job.compareMode,
                                                    job.resample,
                                                    state->cancelled,
                                                    observer);
                         result.requestId = requestId;
                     }
                     postDifferenceResult(std::move(result), std::move(state));
                 });
        return requestId;
    }
    void cancel(const quint64 requestId) {
        const auto iterator = active_.find(requestId);
        if (iterator != active_.end()) {
            iterator->second->cancelled.store(true);
            if (pending_.erase(requestId) != 0) {
                active_.erase(iterator);
            }
        }
    }
    void cancelAll() {
        for (auto& [id, state] : active_) {
            static_cast<void>(id);
            state->cancelled.store(true);
        }
        for (const auto& [id, job] : pending_) {
            static_cast<void>(job);
            active_.erase(id);
        }
        pending_.clear();
    }
    void cancelPrefetches() {
        for (auto iterator = active_.begin(); iterator != active_.end();) {
            const auto state = iterator++->second;
            if (state->prefetch) {
                cancel(state->requestId);
            }
        }
    }
    void setDifferenceRowObserverForTesting(std::function<void(int)> observer) {
        differenceRowObserver_ = std::move(observer);
    }
    void setCacheBudgetBytes(const qint64 bytes) {
        cache_.setMaximumBytes(bytes);
    }
    [[nodiscard]] qint64 cacheBudgetBytes() const noexcept {
        return cache_.maximumBytes();
    }
    void clearCache() {
        cache_.clear();
        cache_.resetStats();
    }
    [[nodiscard]] Stats stats() const noexcept {
        Stats result;
        result.cacheBytes = cache_.currentBytes();
        result.cacheBudgetBytes = cache_.maximumBytes();
        result.cacheEntries = cache_.size();
        result.cacheHits = cache_.hits();
        result.cacheMisses = cache_.misses();
        result.activeRequests = static_cast<int>(active_.size());
        result.maxThreadCount = pool_.maxThreadCount();
        result.pendingRequests = static_cast<int>(pending_.size());
        return result;
    }

private:
    void schedule(const std::shared_ptr<RequestState>& state, std::function<void()> job) {
        // At most two running jobs and one pending job per class. Replacing a queued
        // candidate releases its images immediately rather than feeding QThreadPool a backlog.
        for (auto iterator = pending_.begin(); iterator != pending_.end();) {
            const auto old = active_.at(iterator->first);
            if (old->prefetch == state->prefetch && old->difference == state->difference) {
                old->cancelled.store(true);
                active_.erase(iterator->first);
                iterator = pending_.erase(iterator);
            } else {
                ++iterator;
            }
        }
        pending_.emplace(state->requestId, std::move(job));
        dispatch();
    }
    void dispatch() {
        while (running_ < pool_.maxThreadCount() && !pending_.empty()) {
            auto next = pending_.begin();
            for (auto iterator = pending_.begin(); iterator != pending_.end(); ++iterator) {
                if (!active_.at(iterator->first)->prefetch) {
                    next = iterator;
                    break;
                }
            }
            auto job = std::move(next->second);
            pending_.erase(next);
            ++running_;
            pool_.start(new FunctionRunnable(std::move(job)));
        }
    }
    [[nodiscard]] QString cacheKey(const QString& identity) const {
        return identity + QStringLiteral("|decoded-rgba8");
    }
    [[nodiscard]] QString identityFor(const QUrl& url, const std::uint64_t revision) const {
        if (!url.isValid()) {
            return {};
        }
        const QString identity = fileIdentity(url, revision);
        return identity.isEmpty()
                   ? QStringLiteral("url:%1|policy:%2").arg(url.toString()).arg(revision)
                   : identity;
    }
    [[nodiscard]] std::optional<DecodedCacheEntry> lookup(const QString& identity) {
        if (identity.isEmpty()) {
            return std::nullopt;
        }
        DecodedCacheEntry entry;
        if (cache_.get(cacheKey(identity), &entry) && !entry.image.isNull()) {
            return entry;
        }
        return std::nullopt;
    }
    [[nodiscard]] quint64 enqueueLoad(LoadJob job, ResultHandler handler, const bool prefetch) {
        cancelPrefetches();
        job.requestId = nextRequestId_++;
        auto state = std::make_shared<RequestState>();
        state->requestId = job.requestId;
        state->prefetch = prefetch;
        state->resultHandler = std::move(handler);
        const quint64 requestId = job.requestId;
        active_.emplace(requestId, state);
        schedule(state, [this, job = std::move(job), state]() mutable {
            executeLoad(std::move(job), std::move(state));
        });
        return requestId;
    }
    void executeLoad(LoadJob job, std::shared_ptr<RequestState> state) {
        Result result;
        result.requestId = job.requestId;
        result.pairId = job.pairId;
        result.pair = job.kind == LoadJob::Kind::Pair;
        if (!state->cancelled.load()) {
            if (job.kind != LoadJob::Kind::Secondary && !job.cachedPrimary.has_value()) {
                QString error;
                QString identity;
                if (!loadImageFromDisk(job.primaryUrl,
                                       job.policy,
                                       &result.primary,
                                       &result.primaryNative,
                                       &result.primaryInfo,
                                       &identity,
                                       &error,
                                       state->cancelled)) {
                    result.failedSide = 0;
                    result.error = error;
                }
                if (job.primaryIdentity.isEmpty()) {
                    job.primaryIdentity = identity;
                }
            } else if (job.cachedPrimary.has_value()) {
                result.primary = job.cachedPrimary->image;
                result.primaryNative = job.cachedPrimary->native;
                result.primaryInfo = job.cachedPrimary->info;
            }
            if (!state->cancelled.load() && result.error.isEmpty() &&
                job.kind != LoadJob::Kind::Primary && !job.cachedSecondary.has_value()) {
                QString error;
                QString identity;
                if (!loadImageFromDisk(job.secondaryUrl,
                                       job.policy,
                                       &result.secondary,
                                       &result.secondaryNative,
                                       &result.secondaryInfo,
                                       &identity,
                                       &error,
                                       state->cancelled)) {
                    result.failedSide = 1;
                    result.error = error;
                }
                if (job.secondaryIdentity.isEmpty()) {
                    job.secondaryIdentity = identity;
                }
            } else if (job.cachedSecondary.has_value()) {
                result.secondary = job.cachedSecondary->image;
                result.secondaryNative = job.cachedSecondary->native;
                result.secondaryInfo = job.cachedSecondary->info;
            }
        }
        if (job.kind == LoadJob::Kind::Primary) {
            result.secondaryOnly = false;
        } else if (job.kind == LoadJob::Kind::Secondary) {
            result.secondaryOnly = true;
        }
        result.primaryLabel =
            job.kind == LoadJob::Kind::Secondary ? QString{} : sourceLabel(job.primaryUrl);
        result.secondaryLabel =
            job.kind == LoadJob::Kind::Primary ? QString{} : sourceLabel(job.secondaryUrl);
        result.primaryIdentity = job.primaryIdentity;
        result.secondaryIdentity = job.secondaryIdentity;
        postLoadResult(std::move(result), std::move(state));
    }
    void postLoadResult(Result result, std::shared_ptr<RequestState> state) {
        QMetaObject::invokeMethod(
            owner_,
            [this, result = std::move(result), state = std::move(state)]() mutable {
                finishLoad(std::move(result), std::move(state));
            },
            Qt::QueuedConnection);
    }
    void finishLoad(Result result, const std::shared_ptr<RequestState>& state) {
        active_.erase(result.requestId);
        --running_;
        dispatch();
        if (!state || state->cancelled.load()) {
            return;
        }
        if (result.succeeded()) {
            if (!result.primary.isNull() && !result.primaryIdentity.isEmpty()) {
                cache_.put(
                    cacheKey(result.primaryIdentity),
                    DecodedCacheEntry{result.primary, result.primaryNative, result.primaryInfo});
            }
            if (!result.secondary.isNull() && !result.secondaryIdentity.isEmpty()) {
                cache_.put(cacheKey(result.secondaryIdentity),
                           DecodedCacheEntry{
                               result.secondary, result.secondaryNative, result.secondaryInfo});
            }
        }
        if (!state->prefetch && state->resultHandler) {
            state->resultHandler(std::move(result));
        }
    }
    void postDifferenceResult(DifferenceResult result, std::shared_ptr<RequestState> state) {
        QMetaObject::invokeMethod(
            owner_,
            [this, result = std::move(result), state = std::move(state)]() mutable {
                finishDifference(std::move(result), std::move(state));
            },
            Qt::QueuedConnection);
    }
    void finishDifference(DifferenceResult result, const std::shared_ptr<RequestState>& state) {
        active_.erase(result.requestId);
        --running_;
        dispatch();
        if (!state || state->cancelled.load()) {
            return;
        }
        if (state->differenceHandler) {
            state->differenceHandler(std::move(result));
        }
    }
    ImagePairLoader* owner_ = nullptr;
    QThreadPool pool_;
    ByteLruCache<DecodedCacheEntry> cache_{
        128LL * 1024LL * 1024LL, [](const DecodedCacheEntry& entry) {
            // The RGBA64 sidecar is part of the entry's footprint, not an extra cache: a
            // high-bit-depth image that no longer fits evicts itself as one unit.
            return static_cast<qint64>(entry.image.sizeInBytes()) +
                   static_cast<qint64>(entry.native.sizeInBytes());
        }};
    std::map<quint64, std::shared_ptr<RequestState>> active_;
    std::map<quint64, std::function<void()>> pending_;
    std::function<void(int)> differenceRowObserver_;
    int running_ = 0;
    quint64 nextRequestId_ = 1;
};
ImagePairLoader::ImagePairLoader(QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(this)) {}
ImagePairLoader::~ImagePairLoader() {
    impl_->shutdown();
}
quint64 ImagePairLoader::requestPrimary(const QUrl& primary,
                                        const int pairId,
                                        const DecodePolicy& policy,
                                        ResultHandler handler) {
    return impl_->requestPrimary(primary, pairId, policy, std::move(handler));
}
quint64 ImagePairLoader::requestSecondary(const QUrl& secondary,
                                          const int pairId,
                                          const DecodePolicy& policy,
                                          ResultHandler handler) {
    return impl_->requestSecondary(secondary, pairId, policy, std::move(handler));
}
quint64 ImagePairLoader::requestPair(const QUrl& primary,
                                     const QUrl& secondary,
                                     const int pairId,
                                     const DecodePolicy& policy,
                                     ResultHandler handler) {
    return impl_->requestPair(primary, secondary, pairId, policy, std::move(handler));
}
quint64 ImagePairLoader::prefetchPair(const QUrl& primary,
                                      const QUrl& secondary,
                                      const DecodePolicy& policy) {
    return impl_->prefetchPair(primary, secondary, policy);
}
quint64 ImagePairLoader::requestDifference(QImage primary,
                                           QImage secondary,
                                           const int compareMode,
                                           const bool resample,
                                           DifferenceHandler handler) {
    return impl_->requestDifference(
        std::move(primary), std::move(secondary), compareMode, resample, std::move(handler));
}
void ImagePairLoader::setDifferenceRowObserverForTesting(std::function<void(int)> observer) {
    impl_->setDifferenceRowObserverForTesting(std::move(observer));
}
void ImagePairLoader::cancel(const quint64 requestId) {
    impl_->cancel(requestId);
}
void ImagePairLoader::cancelAll() {
    impl_->cancelAll();
}
void ImagePairLoader::cancelPrefetches() {
    impl_->cancelPrefetches();
}
void ImagePairLoader::setCacheBudgetBytes(const qint64 bytes) {
    impl_->setCacheBudgetBytes(bytes);
}
qint64 ImagePairLoader::cacheBudgetBytes() const noexcept {
    return impl_->cacheBudgetBytes();
}
void ImagePairLoader::clearCache() {
    impl_->clearCache();
}
ImagePairLoader::Stats ImagePairLoader::stats() const noexcept {
    return impl_->stats();
}
} // namespace dvs::ui
