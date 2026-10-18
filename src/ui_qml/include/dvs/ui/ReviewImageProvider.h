#pragma once

#include "dvs/ui/ImageReviewController.h"
#include "dvs/ui/PreviewThumbnailController.h"

#include <QQuickImageProvider>

namespace dvs::ui {

// Serves image://vcs-review/<slot>/<generation>/<view-mode>/<alpha-filter>/<alpha-threshold>/
// <colour-toggle> from ImageReviewController. Every presentation input is part of the id, so
// Qt's image cache cannot hand back a pixmap built for a different view.
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
