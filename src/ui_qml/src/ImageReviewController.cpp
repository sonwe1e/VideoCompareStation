#include "dvs/ui/ImageReviewController.h"

#include "dvs/ui/StillImageDecoder.h"

#include "ByteLruCache.h"
#include "ImageHeaderProbe.h"

#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QUrlQuery>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <utility>

namespace dvs::ui {
namespace {

constexpr qreal kMinZoom = 0.1;
constexpr qreal kMaxZoom = 64.0;
constexpr int kMaxImageEdge = 8192;
constexpr qint64 kMaxDecodedImageBytes = 128LL * 1024LL * 1024LL;

std::mutex g_loaderMutex;
ImageReviewController::StillImageLoader g_stillImageLoader;
ImageReviewController::StillImageProbe g_stillImageProbe;
std::atomic<quint64> g_loaderRevision{1U};

[[nodiscard]] ImageReviewController::StillImageLoader processStillImageLoader() {
    const std::lock_guard lock{g_loaderMutex};
    return g_stillImageLoader;
}

[[nodiscard]] ImageReviewController::StillImageProbe processStillImageProbe() {
    const std::lock_guard lock{g_loaderMutex};
    return g_stillImageProbe;
}

[[nodiscard]] qreal clampZoom(const qreal value) noexcept {
    if (!std::isfinite(value)) {
        return 1.0;
    }
    return std::clamp(value, kMinZoom, kMaxZoom);
}

[[nodiscard]] int channel8(const int value) noexcept {
    return std::clamp(value, 0, 255);
}

[[nodiscard]] bool isDifferenceMode(const int mode) noexcept {
    return mode >= ImageReviewController::AbsDifference && mode <= ImageReviewController::Highlight;
}

[[nodiscard]] bool dimensionsWithinBudget(const int width, const int height, QString* error) {
    if (width <= 0 || height <= 0) {
        if (error != nullptr) {
            *error = QObject::tr("Image dimensions are empty.");
        }
        return false;
    }
    if (width > kMaxImageEdge || height > kMaxImageEdge) {
        if (error != nullptr) {
            *error = QObject::tr("Image is larger than %1 px on a side.").arg(kMaxImageEdge);
        }
        return false;
    }
    const qint64 decodedBytes = static_cast<qint64>(width) * static_cast<qint64>(height) * 4LL;
    if (decodedBytes > kMaxDecodedImageBytes) {
        if (error != nullptr) {
            *error = QObject::tr("Image exceeds the decoded-image memory budget.");
        }
        return false;
    }
    return true;
}

[[nodiscard]] QString imageContentIdentity(const QImage& image) {
    if (image.isNull()) {
        return {};
    }
    return QStringLiteral("image:%1|%2x%3|%4")
        .arg(image.cacheKey(), 0, 16)
        .arg(image.width())
        .arg(image.height())
        .arg(static_cast<int>(image.format()));
}

[[nodiscard]] QVariantMap pixelMap(const int x, const int y, const QRgb pixel) {
    return QVariantMap{
        {QStringLiteral("valid"), true},
        {QStringLiteral("x"), x},
        {QStringLiteral("y"), y},
        {QStringLiteral("r"), qRed(pixel)},
        {QStringLiteral("g"), qGreen(pixel)},
        {QStringLiteral("b"), qBlue(pixel)},
        {QStringLiteral("a"), qAlpha(pixel)},
        {QStringLiteral("hex"), QColor(pixel).name(QColor::HexRgb).toUpper()},
    };
}

} // namespace

struct DifferenceEntry final {
    ImagePairLoader::DifferenceResult result;
};

struct ImageReviewController::AsyncState final {
    explicit AsyncState(ImageReviewController* owner) : loader(owner) {}

    enum class PendingKind {
        None,
        Primary,
        Secondary,
        Pair,
    };

    ImagePairLoader loader;
    ByteLruCache<DifferenceEntry> diffCache{
        64LL * 1024LL * 1024LL, [](const DifferenceEntry& entry) {
            return entry.result.image.isNull()
                       ? 0
                       : static_cast<qint64>(entry.result.image.sizeInBytes());
        }};
    quint64 activeLoadRequestId = 0;
    PendingKind pendingKind = PendingKind::None;
    int pendingPairId = -1;
    quint64 activeDifferenceRequestId = 0;
    quint64 sourceGeneration = 0;
    bool differencePending = false;
    QString primaryIdentity;
    QString secondaryIdentity;
};

ImageReviewController::ImageReviewController(QObject* parent)
    : QObject(parent), async_(std::make_unique<AsyncState>(this)) {}

ImageReviewController::~ImageReviewController() = default;

void ImageReviewController::setProcessStillImageLoader(StillImageLoader loader) {
    {
        const std::lock_guard lock{g_loaderMutex};
        g_stillImageLoader = std::move(loader);
    }
    ++g_loaderRevision;
}

void ImageReviewController::setProcessStillImageProbe(StillImageProbe probe) {
    {
        const std::lock_guard lock{g_loaderMutex};
        g_stillImageProbe = std::move(probe);
    }
    ++g_loaderRevision;
}

bool ImageReviewController::hasPrimary() const noexcept {
    return !primary_.isNull();
}

bool ImageReviewController::hasSecondary() const noexcept {
    return !secondary_.isNull();
}

bool ImageReviewController::hasPair() const noexcept {
    return hasPrimary() && hasSecondary();
}

int ImageReviewController::primaryWidth() const noexcept {
    return primary_.width();
}

int ImageReviewController::primaryHeight() const noexcept {
    return primary_.height();
}

int ImageReviewController::secondaryWidth() const noexcept {
    return secondary_.width();
}

int ImageReviewController::secondaryHeight() const noexcept {
    return secondary_.height();
}

QString ImageReviewController::primaryPath() const {
    return primaryPath_;
}

QString ImageReviewController::secondaryPath() const {
    return secondaryPath_;
}

int ImageReviewController::compareMode() const noexcept {
    return compareMode_;
}

bool ImageReviewController::openPending() const noexcept {
    return async_ != nullptr && async_->activeLoadRequestId != 0;
}

bool ImageReviewController::diffPending() const noexcept {
    return async_ != nullptr && async_->differencePending;
}

void ImageReviewController::setCompareMode(const int mode) {
    if (mode < PrimaryOnly || mode > Wipe) {
        return;
    }
    if (compareMode_ == mode) {
        return;
    }
    if (mode != PrimaryOnly && mode != SideBySide && !hasPair()) {
        // Preserve an existing failure source (e.g. a failed pair open); only fill the
        // generic hint when nothing else explains the state (T1 error-text retention).
        if (errorText_.isEmpty()) {
            setError(tr("Open a second image to compare."));
        }
        return;
    }
    compareMode_ = mode;
    if (isDifferenceMode(mode)) {
        requestDifferenceForCurrentMode();
        return;
    }
    cancelDifferenceRequest();
    errorText_.clear();
    emit stateChanged();
}

qreal ImageReviewController::wipePosition() const noexcept {
    return wipePosition_;
}

void ImageReviewController::setWipePosition(const qreal position) {
    const qreal clamped = std::max(0.0, std::min(1.0, position));
    if (!qFuzzyCompare(clamped, wipePosition_)) {
        wipePosition_ = clamped;
        emit viewChanged();
    }
}

qreal ImageReviewController::zoom() const noexcept {
    return zoom_;
}

qreal ImageReviewController::panX() const noexcept {
    return panX_;
}

qreal ImageReviewController::panY() const noexcept {
    return panY_;
}

QVariantMap ImageReviewController::cursorPixel() const {
    return cursorPixel_;
}

QString ImageReviewController::errorText() const {
    return errorText_;
}

int ImageReviewController::contentGeneration() const noexcept {
    return contentGeneration_;
}

int ImageReviewController::maxAbsDifference() const noexcept {
    return maxAbsDifference_;
}

double ImageReviewController::meanAbsDifference() const noexcept {
    return meanAbsDifference_;
}

int ImageReviewController::committedPairId() const noexcept {
    return committedPairId_;
}

bool ImageReviewController::hasDiffResult() const noexcept {
    return hasDiffResult_;
}

bool ImageReviewController::diffResampled() const noexcept {
    return diffResampled_;
}

bool ImageReviewController::alphaDifferenceOnly() const noexcept {
    return alphaDifferenceOnly_;
}

bool ImageReviewController::resampleAllowed() const noexcept {
    return resampleAllowed_;
}

void ImageReviewController::setResampleAllowed(const bool allowed) {
    if (resampleAllowed_ == allowed) {
        return;
    }
    resampleAllowed_ = allowed;
    if (isDifferenceMode(compareMode_) && hasPair()) {
        // T4: toggling the policy invalidates any in-flight value, then either hits the
        // matching derived cache or schedules one bounded background computation.
        requestDifferenceForCurrentMode();
        return;
    }
    emit stateChanged();
}

QString ImageReviewController::diffScopeText() const {
    // T2 scope contract: the decoder path is RGBA8 (stb_image / FFmpeg) and the diff
    // statistics cover per-pixel RGB max deltas. 16-bit original code values are not
    // retained, so equality claims are always scoped to decoded RGBA8, never to source
    // code values.
    return tr("解码后 RGBA8；差异统计为 RGB（不含 alpha）");
}

bool ImageReviewController::openPrimaryImage(QImage image, QString pathLabel) {
    if (image.isNull()) {
        setError(tr("Could not open image."));
        return false;
    }
    QString dimensionError;
    if (!dimensionsWithinBudget(image.width(), image.height(), &dimensionError)) {
        setError(dimensionError);
        return false;
    }
    cancelPendingOpen();
    cancelDifferenceRequest();
    ++async_->sourceGeneration;
    resetDifferenceState();
    primary_ = std::move(image);
    primaryPath_ = std::move(pathLabel);
    async_->primaryIdentity = imageContentIdentity(primary_);
    errorText_.clear();
    if (compareMode_ != PrimaryOnly && compareMode_ != SideBySide && !hasPair()) {
        compareMode_ = PrimaryOnly;
    }
    resetView();
    bumpGeneration();
    return true;
}

bool ImageReviewController::openSecondaryImage(QImage image, QString pathLabel) {
    if (image.isNull()) {
        setError(tr("Could not open image."));
        return false;
    }
    QString dimensionError;
    if (!dimensionsWithinBudget(image.width(), image.height(), &dimensionError)) {
        setError(dimensionError);
        return false;
    }
    cancelPendingOpen();
    cancelDifferenceRequest();
    ++async_->sourceGeneration;
    resetDifferenceState();
    secondary_ = std::move(image);
    secondaryPath_ = std::move(pathLabel);
    async_->secondaryIdentity = imageContentIdentity(secondary_);
    errorText_.clear();
    if (compareMode_ == PrimaryOnly && hasPair()) {
        compareMode_ = SideBySide;
    }
    bumpGeneration();
    return true;
}

bool ImageReviewController::openPrimary(const QUrl& url) {
    QImage image;
    QString error;
    if (!loadChecked(url, &image, &error)) {
        setError(error);
        return false;
    }
    return openPrimaryImage(std::move(image),
                            url.isLocalFile() ? url.toLocalFile() : url.toString());
}

bool ImageReviewController::openSecondary(const QUrl& url) {
    QImage image;
    QString error;
    if (!loadChecked(url, &image, &error)) {
        setError(error);
        return false;
    }
    return openSecondaryImage(std::move(image),
                              url.isLocalFile() ? url.toLocalFile() : url.toString());
}

bool ImageReviewController::openPairImages(QImage primary,
                                           QString primaryLabel,
                                           QImage secondary,
                                           QString secondaryLabel,
                                           const int pairId) {
    // Validate both sides before touching any member: a failed candidate must leave the
    // previous committed pair (or the explicit empty state) fully intact (T1 atomicity).
    if (primary.isNull() || secondary.isNull()) {
        setError(tr("Could not open image."));
        return false;
    }
    if (primary.width() > kMaxImageEdge || primary.height() > kMaxImageEdge ||
        secondary.width() > kMaxImageEdge || secondary.height() > kMaxImageEdge) {
        setError(tr("Image is larger than %1 px on a side.").arg(kMaxImageEdge));
        return false;
    }
    if (!dimensionsWithinBudget(primary.width(), primary.height(), &errorText_) ||
        !dimensionsWithinBudget(secondary.width(), secondary.height(), &errorText_)) {
        // dimensionsWithinBudget writes the concrete failure source above.
        emit stateChanged();
        return false;
    }
    cancelPendingOpen();
    cancelDifferenceRequest();
    ++async_->sourceGeneration;
    resetDifferenceState();
    primary_ = std::move(primary);
    secondary_ = std::move(secondary);
    primaryPath_ = std::move(primaryLabel);
    secondaryPath_ = std::move(secondaryLabel);
    async_->primaryIdentity = imageContentIdentity(primary_);
    async_->secondaryIdentity = imageContentIdentity(secondary_);
    committedPairId_ = pairId;
    errorText_.clear();
    compareMode_ = SideBySide;
    resetView();
    bumpGeneration();
    return true;
}

bool ImageReviewController::openPairAtomically(const QUrl& primary,
                                               const QUrl& secondary,
                                               const int pairId) {
    QImage primaryImage;
    QString primaryError;
    if (!loadChecked(primary, &primaryImage, &primaryError)) {
        setError(tr("无法打开 A：%1").arg(primaryError));
        return false;
    }
    QImage secondaryImage;
    QString secondaryError;
    if (!loadChecked(secondary, &secondaryImage, &secondaryError)) {
        setError(tr("无法打开 B：%1").arg(secondaryError));
        return false;
    }
    return openPairImages(std::move(primaryImage),
                          primary.isLocalFile() ? primary.toLocalFile() : primary.toString(),
                          std::move(secondaryImage),
                          secondary.isLocalFile() ? secondary.toLocalFile() : secondary.toString(),
                          pairId);
}

int ImageReviewController::requestOpenPrimary(const QUrl& url) {
    const QString label = url.isLocalFile() ? url.toLocalFile() : url.toString();
    if (!url.isValid() || label.isEmpty()) {
        setError(tr("Invalid image path."));
        return -1;
    }
    cancelPendingOpen();
    async_->loader.cancelPrefetches();
    async_->pendingKind = AsyncState::PendingKind::Primary;
    async_->pendingPairId = -1;
    const quint64 requestId = async_->loader.requestPrimary(
        url, -1, currentDecodePolicy(), [this](ImagePairLoader::Result result) {
            handleLoadFinished(std::move(result));
        });
    async_->activeLoadRequestId = requestId;
    errorText_.clear();
    emit stateChanged();
    return static_cast<int>(requestId);
}

int ImageReviewController::requestOpenSecondary(const QUrl& url) {
    if (!hasPrimary()) {
        setError(tr("Open a first image to compare."));
        return -1;
    }
    const QString label = url.isLocalFile() ? url.toLocalFile() : url.toString();
    if (!url.isValid() || label.isEmpty()) {
        setError(tr("Invalid image path."));
        return -1;
    }
    cancelPendingOpen();
    async_->loader.cancelPrefetches();
    async_->pendingKind = AsyncState::PendingKind::Secondary;
    async_->pendingPairId = committedPairId_;
    const quint64 requestId = async_->loader.requestSecondary(
        url, committedPairId_, currentDecodePolicy(), [this](ImagePairLoader::Result result) {
            handleLoadFinished(std::move(result));
        });
    async_->activeLoadRequestId = requestId;
    errorText_.clear();
    emit stateChanged();
    return static_cast<int>(requestId);
}

int ImageReviewController::requestOpenPair(const QUrl& primary,
                                           const QUrl& secondary,
                                           const int pairId) {
    const QString primaryLabel = primary.isLocalFile() ? primary.toLocalFile() : primary.toString();
    const QString secondaryLabel =
        secondary.isLocalFile() ? secondary.toLocalFile() : secondary.toString();
    if (!primary.isValid() || !secondary.isValid() || primaryLabel.isEmpty() ||
        secondaryLabel.isEmpty()) {
        setError(tr("Invalid image path."));
        return -1;
    }
    cancelPendingOpen();
    async_->loader.cancelPrefetches();
    async_->pendingKind = AsyncState::PendingKind::Pair;
    async_->pendingPairId = pairId;
    const quint64 requestId = async_->loader.requestPair(
        primary, secondary, pairId, currentDecodePolicy(), [this](ImagePairLoader::Result result) {
            handleLoadFinished(std::move(result));
        });
    async_->activeLoadRequestId = requestId;
    errorText_.clear();
    emit stateChanged();
    return static_cast<int>(requestId);
}

void ImageReviewController::cancelPendingOpen() {
    if (async_ == nullptr) {
        return;
    }
    if (async_->activeLoadRequestId != 0) {
        async_->loader.cancel(async_->activeLoadRequestId);
        async_->activeLoadRequestId = 0;
        async_->pendingKind = AsyncState::PendingKind::None;
        async_->pendingPairId = -1;
        emit stateChanged();
    }
}

void ImageReviewController::cancelOpenRequest(const int requestId) {
    if (async_ == nullptr || requestId <= 0) {
        return;
    }
    const quint64 unsignedId = static_cast<quint64>(requestId);
    if (async_->activeLoadRequestId == unsignedId) {
        async_->loader.cancel(unsignedId);
        async_->activeLoadRequestId = 0;
        async_->pendingKind = AsyncState::PendingKind::None;
        async_->pendingPairId = -1;
        emit stateChanged();
    } else {
        async_->loader.cancel(unsignedId);
    }
}

void ImageReviewController::prefetchPair(const QUrl& primary, const QUrl& secondary) {
    if (async_ == nullptr || primary.isEmpty() || secondary.isEmpty() || openPending()) {
        return;
    }
    async_->loader.prefetchPair(primary, secondary, currentDecodePolicy());
}

QVariantMap ImageReviewController::asyncStats() const {
    QVariantMap result;
    if (async_ == nullptr) {
        return result;
    }
    const ImagePairLoader::Stats loaderStats = async_->loader.stats();
    result.insert(QStringLiteral("load_cache_bytes"), loaderStats.cacheBytes);
    result.insert(QStringLiteral("load_cache_budget_bytes"), loaderStats.cacheBudgetBytes);
    result.insert(QStringLiteral("load_cache_entries"), loaderStats.cacheEntries);
    result.insert(QStringLiteral("load_cache_hits"), loaderStats.cacheHits);
    result.insert(QStringLiteral("load_cache_misses"), loaderStats.cacheMisses);
    result.insert(QStringLiteral("active_requests"), loaderStats.activeRequests);
    result.insert(QStringLiteral("worker_thread_count"), loaderStats.maxThreadCount);
    result.insert(QStringLiteral("diff_cache_bytes"), async_->diffCache.currentBytes());
    result.insert(QStringLiteral("diff_cache_budget_bytes"), async_->diffCache.maximumBytes());
    result.insert(QStringLiteral("diff_cache_entries"), async_->diffCache.size());
    result.insert(QStringLiteral("diff_cache_hits"), async_->diffCache.hits());
    result.insert(QStringLiteral("diff_cache_misses"), async_->diffCache.misses());
    result.insert(QStringLiteral("open_pending"), openPending());
    result.insert(QStringLiteral("diff_pending"), diffPending());
    return result;
}

void ImageReviewController::clearAsyncCaches() {
    if (async_ == nullptr) {
        return;
    }
    async_->loader.clearCache();
    async_->diffCache.clear();
    async_->diffCache.resetStats();
    emit stateChanged();
}

void ImageReviewController::closeAll() {
    if (async_ != nullptr) {
        if (async_->activeLoadRequestId != 0) {
            async_->loader.cancel(async_->activeLoadRequestId);
            async_->activeLoadRequestId = 0;
            async_->pendingKind = AsyncState::PendingKind::None;
            async_->pendingPairId = -1;
        }
        cancelDifferenceRequest();
    }
    ++async_->sourceGeneration;
    primary_ = QImage();
    secondary_ = QImage();
    diff_ = QImage();
    primaryPath_.clear();
    secondaryPath_.clear();
    errorText_.clear();
    compareMode_ = PrimaryOnly;
    committedPairId_ = -1;
    maxAbsDifference_ = 0;
    meanAbsDifference_ = 0.0;
    hasDiffResult_ = false;
    diffResampled_ = false;
    alphaDifferenceOnly_ = false;
    async_->primaryIdentity.clear();
    async_->secondaryIdentity.clear();
    clearCursorPixel();
    resetView();
    bumpGeneration();
}

void ImageReviewController::resetView() {
    zoom_ = 1.0;
    panX_ = 0.5;
    panY_ = 0.5;
    emit viewChanged();
}

void ImageReviewController::zoomBy(const qreal factor,
                                   const qreal anchorNormalizedX,
                                   const qreal anchorNormalizedY) {
    if (!std::isfinite(factor) || factor <= 0.0) {
        return;
    }
    const qreal next = clampZoom(zoom_ * factor);
    if (qFuzzyCompare(next, zoom_)) {
        return;
    }
    // Keep the anchor point under the cursor stable while scaling.
    panX_ = anchorNormalizedX - (anchorNormalizedX - panX_) * (zoom_ / next);
    panY_ = anchorNormalizedY - (anchorNormalizedY - panY_) * (zoom_ / next);
    zoom_ = next;
    panX_ = std::clamp(panX_, 0.0, 1.0);
    panY_ = std::clamp(panY_, 0.0, 1.0);
    emit viewChanged();
}

void ImageReviewController::panBy(const qreal deltaNormalizedX, const qreal deltaNormalizedY) {
    if (!std::isfinite(deltaNormalizedX) || !std::isfinite(deltaNormalizedY)) {
        return;
    }
    panX_ = std::clamp(panX_ - deltaNormalizedX / zoom_, 0.0, 1.0);
    panY_ = std::clamp(panY_ - deltaNormalizedY / zoom_, 0.0, 1.0);
    emit viewChanged();
}

void ImageReviewController::setZoom(const qreal value) {
    const qreal next = clampZoom(value);
    if (qFuzzyCompare(next, zoom_)) {
        return;
    }
    zoom_ = next;
    emit viewChanged();
}

QVariantMap ImageReviewController::samplePixel(const int imageSlot,
                                               const qreal imageX,
                                               const qreal imageY) const {
    const QImage image = displayImage(imageSlot);
    if (image.isNull() || !std::isfinite(imageX) || !std::isfinite(imageY)) {
        return {{QStringLiteral("valid"), false}};
    }
    const int x = static_cast<int>(std::floor(imageX));
    const int y = static_cast<int>(std::floor(imageY));
    if (x < 0 || y < 0 || x >= image.width() || y >= image.height()) {
        return {{QStringLiteral("valid"), false}};
    }
    QVariantMap result = pixelMap(x, y, image.pixel(x, y));
    // Sampling source label: original pixels for the primary/secondary slots, derived
    // pixels for the diff slot (T2 原图/派生取样来源).
    result.insert(QStringLiteral("source"),
                  imageSlot == DisplayDiffSlot ? QStringLiteral("diff")
                                               : QStringLiteral("original"));
    return result;
}

void ImageReviewController::updateCursorPixel(const int imageSlot,
                                              const qreal imageX,
                                              const qreal imageY) {
    const QVariantMap next = samplePixel(imageSlot, imageX, imageY);
    if (next == cursorPixel_) {
        return;
    }
    cursorPixel_ = next;
    emit cursorPixelChanged();
}

void ImageReviewController::clearCursorPixel() {
    if (cursorPixel_.isEmpty()) {
        return;
    }
    cursorPixel_ = QVariantMap{};
    emit cursorPixelChanged();
}

QString ImageReviewController::imageUrl(const int imageSlot) const {
    return QStringLiteral("image://vcs-review/%1/%2").arg(imageSlot).arg(contentGeneration_);
}

QImage ImageReviewController::imageForSlot(const int imageSlot) const {
    return displayImage(imageSlot);
}

void ImageReviewController::setError(QString text) {
    errorText_ = std::move(text);
    emit stateChanged();
}

void ImageReviewController::bumpGeneration() {
    ++contentGeneration_;
    emit stateChanged();
}

ImagePairLoader::DecodePolicy ImageReviewController::currentDecodePolicy() const {
    ImagePairLoader::DecodePolicy policy;
    policy.revision = g_loaderRevision.load();
    const std::lock_guard lock{g_loaderMutex};
    policy.loader = g_stillImageLoader;
    policy.probe = g_stillImageProbe;
    return policy;
}

void ImageReviewController::handleLoadFinished(ImagePairLoader::Result result) {
    if (async_ == nullptr || result.requestId != async_->activeLoadRequestId) {
        return; // Stale N after N+1/N+2 or explicit cancellation.
    }
    const AsyncState::PendingKind kind = async_->pendingKind;
    async_->activeLoadRequestId = 0;
    async_->pendingKind = AsyncState::PendingKind::None;
    async_->pendingPairId = -1;
    const int requestId = static_cast<int>(result.requestId);
    const int pairId = result.pairId;

    if (!result.succeeded()) {
        QString detail = result.error;
        if (result.failedSide == 0) {
            detail = tr("无法打开 A：%1").arg(result.error);
        } else if (result.failedSide == 1) {
            detail = tr("无法打开 B：%1").arg(result.error);
        }
        setError(std::move(detail));
        emit openFinished(requestId, pairId, false, errorText_);
        return;
    }

    // Failed candidates leave the committed pair and its pending difference intact.
    cancelDifferenceRequest();
    ++async_->sourceGeneration;
    errorText_.clear();
    switch (kind) {
    case AsyncState::PendingKind::Primary:
        commitLoadedPrimary(std::move(result.primary),
                            std::move(result.primaryLabel),
                            std::move(result.primaryIdentity));
        break;
    case AsyncState::PendingKind::Secondary:
        commitLoadedSecondary(std::move(result.secondary),
                              std::move(result.secondaryLabel),
                              std::move(result.secondaryIdentity));
        break;
    case AsyncState::PendingKind::Pair:
        commitLoadedPair(std::move(result));
        break;
    case AsyncState::PendingKind::None:
        break;
    }
    emit openFinished(requestId, pairId, true, QString{});
}

void ImageReviewController::handleDifferenceFinished(ImagePairLoader::DifferenceResult result,
                                                     const quint64 sourceGeneration,
                                                     const QString& cacheKey) {
    if (async_ == nullptr || result.requestId != async_->activeDifferenceRequestId ||
        sourceGeneration != async_->sourceGeneration || cacheKey != differenceCacheKey()) {
        return;
    }
    async_->activeDifferenceRequestId = 0;
    async_->differencePending = false;
    if (!result.succeeded()) {
        resetDifferenceState();
        setError(result.error);
        return;
    }
    DifferenceEntry entry;
    entry.result = result;
    async_->diffCache.put(cacheKey, std::move(entry));
    applyDifferenceResult(result);
    errorText_.clear();
    bumpGeneration();
}

void ImageReviewController::commitLoadedPrimary(QImage image, QString label, QString identity) {
    primary_ = std::move(image);
    primaryPath_ = std::move(label);
    secondary_ = QImage();
    secondaryPath_.clear();
    async_->primaryIdentity =
        identity.isEmpty() ? imageContentIdentity(primary_) : std::move(identity);
    async_->secondaryIdentity.clear();
    committedPairId_ = -1;
    compareMode_ = PrimaryOnly;
    resetDifferenceState();
    resetView();
    bumpGeneration();
}

void ImageReviewController::commitLoadedSecondary(QImage image, QString label, QString identity) {
    secondary_ = std::move(image);
    secondaryPath_ = std::move(label);
    async_->secondaryIdentity =
        identity.isEmpty() ? imageContentIdentity(secondary_) : std::move(identity);
    resetDifferenceState();
    if (compareMode_ == PrimaryOnly && hasPair()) {
        compareMode_ = SideBySide;
    }
    bumpGeneration();
}

void ImageReviewController::commitLoadedPair(ImagePairLoader::Result result) {
    primary_ = std::move(result.primary);
    secondary_ = std::move(result.secondary);
    primaryPath_ = std::move(result.primaryLabel);
    secondaryPath_ = std::move(result.secondaryLabel);
    async_->primaryIdentity = result.primaryIdentity.isEmpty() ? imageContentIdentity(primary_)
                                                               : std::move(result.primaryIdentity);
    async_->secondaryIdentity = result.secondaryIdentity.isEmpty()
                                    ? imageContentIdentity(secondary_)
                                    : std::move(result.secondaryIdentity);
    committedPairId_ = result.pairId;
    compareMode_ = SideBySide;
    resetDifferenceState();
    resetView();
    bumpGeneration();
}

void ImageReviewController::cancelDifferenceRequest() {
    if (async_ == nullptr) {
        return;
    }
    if (async_->activeDifferenceRequestId != 0) {
        async_->loader.cancel(async_->activeDifferenceRequestId);
        async_->activeDifferenceRequestId = 0;
    }
    async_->differencePending = false;
}

void ImageReviewController::resetDifferenceState() {
    diff_ = QImage();
    maxAbsDifference_ = 0;
    meanAbsDifference_ = 0.0;
    hasDiffResult_ = false;
    diffResampled_ = false;
    alphaDifferenceOnly_ = false;
}

QString ImageReviewController::differenceCacheKey() const {
    const QString primaryIdentity = async_ != nullptr && !async_->primaryIdentity.isEmpty()
                                        ? async_->primaryIdentity
                                        : imageContentIdentity(primary_);
    const QString secondaryIdentity = async_ != nullptr && !async_->secondaryIdentity.isEmpty()
                                          ? async_->secondaryIdentity
                                          : imageContentIdentity(secondary_);
    const bool sizesDiffer = primary_.size() != secondary_.size();
    return QStringLiteral("diff-v1|%1|%2|mode:%3|resample:%4|target:%5x%6|sizesDiffer:%7")
        .arg(primaryIdentity,
             secondaryIdentity,
             QString::number(compareMode_),
             resampleAllowed_ ? QStringLiteral("on") : QStringLiteral("off"))
        .arg(primary_.width())
        .arg(primary_.height())
        .arg(sizesDiffer ? 1 : 0);
}

void ImageReviewController::requestDifferenceForCurrentMode() {
    if (async_ == nullptr) {
        return;
    }
    if (!hasPair() || !isDifferenceMode(compareMode_)) {
        cancelDifferenceRequest();
        resetDifferenceState();
        emit stateChanged();
        return;
    }
    if (primary_.size() != secondary_.size() && !resampleAllowed_) {
        cancelDifferenceRequest();
        resetDifferenceState();
        setError(tr("A 与 B 尺寸不同（%1×%2 与 %3×%4），未启用重采样时不计算逐像素差异。")
                     .arg(primary_.width())
                     .arg(primary_.height())
                     .arg(secondary_.width())
                     .arg(secondary_.height()));
        return;
    }

    DifferenceEntry cached;
    if (async_->diffCache.get(differenceCacheKey(), &cached) && cached.result.succeeded()) {
        cancelDifferenceRequest();
        applyDifferenceResult(cached.result);
        errorText_.clear();
        bumpGeneration();
        return;
    }

    cancelDifferenceRequest();
    resetDifferenceState();
    async_->differencePending = true;
    const quint64 sourceGeneration = async_->sourceGeneration;
    const QString cacheKey = differenceCacheKey();
    async_->activeDifferenceRequestId = async_->loader.requestDifference(
        primary_,
        secondary_,
        compareMode_,
        resampleAllowed_,
        [this, sourceGeneration, cacheKey](ImagePairLoader::DifferenceResult result) {
            handleDifferenceFinished(std::move(result), sourceGeneration, cacheKey);
        });
    emit stateChanged();
}

void ImageReviewController::applyDifferenceResult(const ImagePairLoader::DifferenceResult& result) {
    diff_ = result.image;
    maxAbsDifference_ = result.maxAbsDifference;
    meanAbsDifference_ = result.meanAbsDifference;
    hasDiffResult_ = !diff_.isNull();
    diffResampled_ = result.resampled;
    alphaDifferenceOnly_ = result.alphaDifferenceOnly;
}

QImage ImageReviewController::displayImage(const int slot) const {
    switch (slot) {
    case PrimarySlot:
    case DisplayPrimarySlot:
        return primary_;
    case SecondarySlot:
    case DisplaySecondarySlot:
        return secondary_;
    case DisplayDiffSlot:
        return diff_;
    default:
        return {};
    }
}

bool ImageReviewController::loadChecked(const QUrl& url, QImage* image, QString* error) {
    if (!url.isValid()) {
        if (error) {
            *error = tr("Invalid image path.");
        }
        return false;
    }
    const QString localPath = url.isLocalFile() ? url.toLocalFile() : url.toString();
    if (localPath.isEmpty()) {
        if (error) {
            *error = tr("Invalid image path.");
        }
        return false;
    }

    QImage loaded;
    bool decoded = false;
    {
        QFile file(localPath);
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = file.readAll();
            QSize headerSize;
            const StillImageProbe probe = processStillImageProbe();
            const bool haveHeader =
                (probe && probe(bytes, &headerSize)) || probeImageHeader(bytes, &headerSize);
            if (haveHeader) {
                QString dimensionError;
                if (!dimensionsWithinBudget(
                        headerSize.width(), headerSize.height(), &dimensionError)) {
                    if (error) {
                        *error = dimensionError;
                    }
                    return false;
                }
            }

            const StillImageLoader loader = processStillImageLoader();
            bool loaderFailed = false;
            std::string loaderError;
            if (loader) {
                try {
                    decoded = loader(bytes, &loaded, &loaderError);
                } catch (...) {
                    decoded = false;
                    loaderError = "Still-image loader threw an unknown exception.";
                }
                loaderFailed = !decoded;
            }
            if (!decoded) {
                StillImage still;
                std::string decodeError;
                if (decodeStillImageBytes(reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                                          static_cast<std::size_t>(bytes.size()),
                                          &still,
                                          &decodeError)) {
                    const QImage converted(still.rgba.data(),
                                           still.width,
                                           still.height,
                                           still.width * 4,
                                           QImage::Format_RGBA8888);
                    loaded = converted.copy();
                    decoded = !loaded.isNull();
                } else if (error && !loaderFailed) {
                    *error = QString::fromStdString(decodeError);
                }
            }
            if (!decoded && (!loaded.loadFromData(bytes) || loaded.isNull())) {
                if (error) {
                    if (!loaderError.empty()) {
                        *error = QString::fromStdString(loaderError);
                    } else if (error->isEmpty()) {
                        *error =
                            tr("Could not open image: %1").arg(QFileInfo(localPath).fileName());
                    }
                }
            } else if (!loaded.isNull()) {
                decoded = true;
            }
        }
    }
    if (!decoded && (!loaded.load(localPath) || loaded.isNull())) {
        if (error) {
            *error = tr("Could not open image: %1").arg(QFileInfo(localPath).fileName());
        }
        return false;
    }
    QString dimensionError;
    if (!dimensionsWithinBudget(loaded.width(), loaded.height(), &dimensionError)) {
        if (error) {
            *error = dimensionError;
        }
        return false;
    }
    *image = std::move(loaded);
    return true;
}

} // namespace dvs::ui
