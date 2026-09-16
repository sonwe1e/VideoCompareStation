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
        setError(tr("Open a second image to compare."));
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

void ImageReviewController::closeAll() {
    primary_ = QImage();
    secondary_ = QImage();
    diff_ = QImage();
    primaryPath_.clear();
    secondaryPath_.clear();
    errorText_.clear();
    compareMode_ = PrimaryOnly;
    maxAbsDifference_ = 0;
    meanAbsDifference_ = 0.0;
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
    return pixelMap(x, y, image.pixel(x, y));
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
    if (!hasPair()) {
        return;
    }
    if (primary_.size() != secondary_.size()) {
        // Normalize to the primary geometry so pixel inspector coordinates stay aligned.
        secondary_ =
            secondary_.scaled(primary_.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    const int width = primary_.width();
    const int height = primary_.height();
    if (width <= 0 || height <= 0 || width > kMaxImageEdge || height > kMaxImageEdge) {
        setError(tr("Image pair is empty or too large to diff."));
        return;
    }

    const QImage left = primary_.convertToFormat(QImage::Format_ARGB32);
    const QImage right = secondary_.convertToFormat(QImage::Format_ARGB32);
    QImage output(width, height, QImage::Format_ARGB32);
    qint64 sum = 0;
    int peak = 0;
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
            sum += delta;
            peak = std::max(peak, delta);
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
    maxAbsDifference_ = peak;
    meanAbsDifference_ = width * height > 0 ? static_cast<double>(sum) / (width * height) : 0.0;
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
