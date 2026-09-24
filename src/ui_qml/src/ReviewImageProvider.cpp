#include "dvs/ui/ReviewImageProvider.h"

#include <cstring>

namespace dvs::ui {

ReviewImageProvider::ReviewImageProvider(ImageReviewController* controller)
    : QQuickImageProvider(QQuickImageProvider::Image), controller_(controller) {}

QImage
ReviewImageProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize) {
    if (controller_ == nullptr) {
        if (size) {
            *size = {};
        }
        return {};
    }
    const QStringList parts = id.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        if (size) {
            *size = {};
        }
        return {};
    }
    bool ok = false;
    const int slot = parts.constFirst().toInt(&ok);
    if (!ok) {
        if (size) {
            *size = {};
        }
        return {};
    }
    QImage image = controller_->imageForSlot(slot);
    if (image.isNull()) {
        if (size) {
            *size = {};
        }
        return {};
    }
    if (size) {
        *size = image.size();
    }
    if (requestedSize.isValid() && !requestedSize.isEmpty() && requestedSize != image.size()) {
        image = image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return image;
}

TimelinePreviewImageProvider::TimelinePreviewImageProvider(PreviewThumbnailController* controller)
    : QQuickImageProvider(QQuickImageProvider::Image), controller_(controller) {}

QImage TimelinePreviewImageProvider::requestImage(const QString& id,
                                                  QSize* size,
                                                  const QSize& requestedSize) {
    if (controller_ == nullptr) {
        if (size) {
            *size = {};
        }
        return {};
    }
    bool ok = false;
    const qint64 frame = id.section(QLatin1Char('?'), 0, 0).toLongLong(&ok);
    if (!ok) {
        if (size) {
            *size = {};
        }
        return {};
    }
    QImage image;
    const auto raw = controller_->rawForFrame(frame);
    if (!raw.rgba.empty() && raw.width > 0 && raw.height > 0) {
        image = QImage(raw.width, raw.height, QImage::Format_RGBA8888);
        if (!image.isNull()) {
            for (int y = 0; y < image.height(); ++y) {
                std::memcpy(image.scanLine(y),
                            raw.rgba.data() + static_cast<std::size_t>(y) *
                                                  static_cast<std::size_t>(raw.width) * 4U,
                            static_cast<std::size_t>(raw.width) * 4U);
            }
        }
    }
    if (image.isNull()) {
        if (size) {
            *size = {};
        }
        return {};
    }
    if (size) {
        *size = image.size();
    }
    if (requestedSize.isValid() && !requestedSize.isEmpty() && requestedSize != image.size()) {
        image = image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return image;
}

} // namespace dvs::ui
