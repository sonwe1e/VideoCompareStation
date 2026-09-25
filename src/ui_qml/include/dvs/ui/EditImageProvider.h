#pragma once

#include <QQuickImageProvider>

namespace dvs::ui {

class ImageEditController;

// Serves the editing working copy to QML. The URL only carries a revision so the QImage
// cache is invalidated on every edit; the pixels always come from the live controller.
class EditImageProvider final : public QQuickImageProvider {
public:
    explicit EditImageProvider(ImageEditController* controller);

    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

private:
    ImageEditController* controller_;
};

} // namespace dvs::ui
