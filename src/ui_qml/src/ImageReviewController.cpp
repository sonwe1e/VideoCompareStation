#include "dvs/ui/ImageReviewController.h"

#include "dvs/ui/StillImageDecoder.h"

#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <utility>

namespace dvs::ui {
namespace {

constexpr qreal kMinZoom = 0.1;
constexpr qreal kMaxZoom = 64.0;
constexpr int kMaxImageEdge = 8192;

ImageReviewController::StillImageLoader g_stillImageLoader;

[[nodiscard]] qreal clampZoom(const qreal value) noexcept {
    if (!std::isfinite(value)) {
        return 1.0;
    }
    return std::clamp(value, kMinZoom, kMaxZoom);
}

[[nodiscard]] int channel8(const int value) noexcept {
    return std::clamp(value, 0, 255);
}

[[nodiscard]] QRgb absDiffPixel(const QRgb a, const QRgb b, const int gain) noexcept {
    const int red = channel8(std::abs(qRed(a) - qRed(b)) * gain);
    const int green = channel8(std::abs(qGreen(a) - qGreen(b)) * gain);
    const int blue = channel8(std::abs(qBlue(a) - qBlue(b)) * gain);
    return qRgb(red, green, blue);
}

[[nodiscard]] QRgb signedDiffPixel(const QRgb a, const QRgb b, const int gain) noexcept {
    const int red = channel8(128 + (qRed(a) - qRed(b)) * gain);
    const int green = channel8(128 + (qGreen(a) - qGreen(b)) * gain);
    const int blue = channel8(128 + (qBlue(a) - qBlue(b)) * gain);
    return qRgb(red, green, blue);
}

[[nodiscard]] QRgb highlightPixel(const QRgb base, const QRgb other, const int gain) noexcept {
    const int delta = std::max({std::abs(qRed(base) - qRed(other)),
                                std::abs(qGreen(base) - qGreen(other)),
                                std::abs(qBlue(base) - qBlue(other))});
    if (delta <= 0) {
        return base | 0xff000000;
    }
    const int strength = channel8(delta * gain);
    const int red = channel8(qRed(base) + (255 - qRed(base)) * strength / 255);
    const int green = channel8(qGreen(base) + (40 - qGreen(base)) * strength / 255);
    const int blue = channel8(qBlue(base) + (90 - qBlue(base)) * strength / 255);
    return qRgb(red, green, blue);
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

ImageReviewController::ImageReviewController(QObject* parent) : QObject(parent) {}

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
    errorText_.clear();
    recomputeDifference();
    // Bump the content revision so QML image sources tied to imageUrl(slot) re-fetch
    // the freshly recomputed diff instead of showing the previous mode's image.
    bumpGeneration();
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
    recomputeDifference();
    bumpGeneration();
}

QString ImageReviewController::diffScopeText() const {
    // T2 scope contract: the decoder path is RGBA8 (stb_image) and the diff statistics
    // cover per-pixel RGB max deltas. 16-bit original code values are not retained, so
    // equality claims are always scoped to decoded RGBA8, never to source code values.
    return tr("解码后 RGBA8；差异统计为 RGB（不含 alpha）");
}

bool ImageReviewController::openPrimaryImage(QImage image, QString pathLabel) {
    if (image.isNull()) {
        setError(tr("Could not open image."));
        return false;
    }
    if (image.width() > kMaxImageEdge || image.height() > kMaxImageEdge) {
        setError(tr("Image is larger than %1 px on a side.").arg(kMaxImageEdge));
        return false;
    }
    primary_ = std::move(image);
    primaryPath_ = std::move(pathLabel);
    errorText_.clear();
    if (compareMode_ != PrimaryOnly && compareMode_ != SideBySide && !hasPair()) {
        compareMode_ = PrimaryOnly;
    }
    recomputeDifference();
    resetView();
    bumpGeneration();
    return true;
}

bool ImageReviewController::openSecondaryImage(QImage image, QString pathLabel) {
    if (image.isNull()) {
        setError(tr("Could not open image."));
        return false;
    }
    if (image.width() > kMaxImageEdge || image.height() > kMaxImageEdge) {
        setError(tr("Image is larger than %1 px on a side.").arg(kMaxImageEdge));
        return false;
    }
    secondary_ = std::move(image);
    secondaryPath_ = std::move(pathLabel);
    errorText_.clear();
    recomputeDifference();
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
    primary_ = std::move(primary);
    secondary_ = std::move(secondary);
    primaryPath_ = std::move(primaryLabel);
    secondaryPath_ = std::move(secondaryLabel);
    committedPairId_ = pairId;
    errorText_.clear();
    compareMode_ = SideBySide;
    maxAbsDifference_ = 0;
    meanAbsDifference_ = 0.0;
    hasDiffResult_ = false;
    diffResampled_ = false;
    alphaDifferenceOnly_ = false;
    recomputeDifference();
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

void ImageReviewController::closeAll() {
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

void ImageReviewController::recomputeDifference() {
    maxAbsDifference_ = 0;
    meanAbsDifference_ = 0.0;
    diff_ = QImage();
    hasDiffResult_ = false;
    diffResampled_ = false;
    alphaDifferenceOnly_ = false;
    if (!hasPair()) {
        return;
    }
    const int width = primary_.width();
    const int height = primary_.height();
    if (width <= 0 || height <= 0 || width > kMaxImageEdge || height > kMaxImageEdge) {
        setError(tr("Image pair is empty or too large to diff."));
        return;
    }

    // T2: originals are immutable. The comparison works on converted copies only; an
    // unequal-size pair is resampled to the primary geometry only after the user opts in
    // (setResampleAllowed), and the persistent diffResampled flag labels the derivation.
    const QImage left = primary_.convertToFormat(QImage::Format_ARGB32);
    QImage right = secondary_.convertToFormat(QImage::Format_ARGB32);
    if (right.size() != left.size()) {
        if (!resampleAllowed_) {
            if (compareMode_ == AbsDifference || compareMode_ == SignedDifference ||
                compareMode_ == Highlight) {
                setError(tr("A 与 B 尺寸不同（%1×%2 与 %3×%4），未启用重采样时不计算逐像素差异。")
                             .arg(primary_.width())
                             .arg(primary_.height())
                             .arg(secondary_.width())
                             .arg(secondary_.height()));
            }
            return;
        }
        right = right.scaled(left.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        diffResampled_ = true;
    }

    QImage output(width, height, QImage::Format_ARGB32);
    qint64 sum = 0;
    int peak = 0;
    int peakAlpha = 0;
    constexpr int kGain = 4;
    for (int y = 0; y < height; ++y) {
        const auto* leftLine = reinterpret_cast<const QRgb*>(left.constScanLine(y));
        const auto* rightLine = reinterpret_cast<const QRgb*>(right.constScanLine(y));
        auto* outLine = reinterpret_cast<QRgb*>(output.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const QRgb a = leftLine[x];
            const QRgb b = rightLine[x];
            const int delta = std::max({std::abs(qRed(a) - qRed(b)),
                                        std::abs(qGreen(a) - qGreen(b)),
                                        std::abs(qBlue(a) - qBlue(b))});
            const int alphaDelta = std::abs(qAlpha(a) - qAlpha(b));
            sum += delta;
            peak = std::max(peak, delta);
            peakAlpha = std::max(peakAlpha, alphaDelta);
            switch (compareMode_) {
            case SignedDifference:
                outLine[x] = signedDiffPixel(a, b, kGain);
                break;
            case Highlight:
                outLine[x] = highlightPixel(a, b, kGain);
                break;
            case AbsDifference:
            default:
                outLine[x] = absDiffPixel(a, b, kGain);
                break;
            }
        }
    }
    diff_ = std::move(output);
    hasDiffResult_ = true;
    maxAbsDifference_ = peak;
    meanAbsDifference_ = width * height > 0 ? static_cast<double>(sum) / (width * height) : 0.0;
    // RGB deltas are the statistics' scope; a pair whose only code-value difference is
    // alpha must never be reported as equal (T2).
    alphaDifferenceOnly_ = peak == 0 && peakAlpha > 0;
}

void ImageReviewController::bumpGeneration() {
    ++contentGeneration_;
    emit stateChanged();
}

void ImageReviewController::setProcessStillImageLoader(StillImageLoader loader) {
    g_stillImageLoader = std::move(loader);
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
            }
        }
    }
    if (!decoded && (!loaded.load(localPath) || loaded.isNull())) {
        if (error) {
            *error = tr("Could not open image: %1").arg(QFileInfo(localPath).fileName());
        }
        return false;
    }
    if (loaded.width() > kMaxImageEdge || loaded.height() > kMaxImageEdge) {
        if (error) {
            *error = tr("Image is larger than %1 px on a side.").arg(kMaxImageEdge);
        }
        return false;
    }
    *image = std::move(loaded);
    return true;
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

} // namespace dvs::ui
