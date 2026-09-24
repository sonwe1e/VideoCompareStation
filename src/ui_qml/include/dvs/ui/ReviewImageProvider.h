#pragma once

#include "dvs/ui/ImageReviewController.h"
#include "dvs/ui/PreviewThumbnailController.h"

#include <QQuickImageProvider>

namespace dvs::ui {

// Serves image://vcs-review/<slot>/<generation> from ImageReviewController.
class ReviewImageProvider final : public QQuickImageProvider {
public:
    explicit ReviewImageProvider(ImageReviewController* controller);

    [[nodiscard]] QImage
    requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

private:
    ImageReviewController* controller_ = nullptr;
};

// Serves image://timeline-preview/<frame>?g=<generation> from PreviewThumbnailController.
class TimelinePreviewImageProvider final : public QQuickImageProvider {
public:
    explicit TimelinePreviewImageProvider(PreviewThumbnailController* controller);

    [[nodiscard]] QImage
    requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

private:
    PreviewThumbnailController* controller_ = nullptr;
};

} // namespace dvs::ui
