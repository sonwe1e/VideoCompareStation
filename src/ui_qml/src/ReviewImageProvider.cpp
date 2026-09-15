#include "dvs/ui/ReviewImageProvider.h"

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

} // namespace dvs::ui
