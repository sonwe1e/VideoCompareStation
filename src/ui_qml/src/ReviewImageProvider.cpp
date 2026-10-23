#include "dvs/ui/ReviewImageProvider.h"

#include <QStringList>

#include <cstring>
#include <utility>

namespace dvs::ui {
namespace {

[[nodiscard]] QString tokenFromId(const QString& id) {
    const qsizetype query = id.indexOf(QLatin1Char('?'));
    if (query < 0) {
        return {};
    }
    const QStringList parameters = id.mid(query + 1).split(QLatin1Char('&'), Qt::SkipEmptyParts);
    for (const QString& parameter : parameters) {
        if (parameter.startsWith(QStringLiteral("w="))) {
            return parameter.mid(2);
        }
    }
    return {};
}

} // namespace

ReviewImageProvider::ReviewImageProvider(ImageReviewController* controller)
    : QQuickImageProvider(QQuickImageProvider::Image), controller_(controller) {}

ReviewImageProvider::ReviewImageProvider(ImageReviewController* controller, TokenResolver resolver)
    : QQuickImageProvider(QQuickImageProvider::Image), controller_(controller),
      resolver_(std::move(resolver)) {}

QImage
ReviewImageProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize) {
    if (size) {
        *size = {};
    }
    const QString token = tokenFromId(id);
    ImageReviewController* controller = controller_;
    if (!token.isEmpty()) {
        controller = resolver_ ? resolver_(token) : nullptr;
        if (controller == nullptr) {
            // A stale tab URL must never be served from another task's live buffers.
            return {};
        }
    }
    if (controller == nullptr) {
        return {};
    }
    const qsizetype query = id.indexOf(QLatin1Char('?'));
    const QString idPath = query < 0 ? id : id.left(query);
    const QStringList parts = idPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return {};
    }
    bool ok = false;
    const int slot = parts.constFirst().toInt(&ok);
    if (!ok) {
        return {};
    }
    QImage image = controller->imageForSlot(slot);
    if (image.isNull()) {
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
