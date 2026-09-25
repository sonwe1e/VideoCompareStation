#include "dvs/ui/EditImageProvider.h"

#include "dvs/ui/ImageEditController.h"

namespace dvs::ui {

EditImageProvider::EditImageProvider(ImageEditController* controller)
    : QQuickImageProvider(QQuickImageProvider::Image), controller_(controller) {}

QImage EditImageProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize) {
    static_cast<void>(id);
    if (controller_ == nullptr) {
        if (size != nullptr) {
            *size = {};
        }
        return {};
    }
    QImage image = controller_->editedImage();
    if (image.isNull()) {
        if (size != nullptr) {
            *size = {};
        }
        return {};
    }
    if (size != nullptr) {
        *size = image.size();
    }
    if (requestedSize.isValid() && !requestedSize.isEmpty() && requestedSize != image.size()) {
        image = image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return image;
}

} // namespace dvs::ui
