#pragma once

#include "dvs/ui/ImageReviewController.h"
#include "dvs/ui/PreviewThumbnailController.h"

#include <QQuickImageProvider>

#include <functional>

namespace dvs::ui {

// Serves image://vcs-review/<slot>/<generation>/<view-mode>/<alpha-filter>/<alpha-threshold>/
// <colour-toggle> from ImageReviewController. Every presentation input is part of the id, so
// Qt's image cache cannot hand back a pixmap built for a different view. The optional resolver
// handles the per-task "?w=<token>" suffix used by ImageTaskHost; a token that no longer maps to
// a live task returns no image instead of falling back to another task's pixels.
class ReviewImageProvider final : public QQuickImageProvider {
public:
    using TokenResolver = std::function<ImageReviewController*(const QString& token)>;

    explicit ReviewImageProvider(ImageReviewController* controller);
    ReviewImageProvider(ImageReviewController* controller, TokenResolver resolver);

    [[nodiscard]] QImage
    requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

private:
    ImageReviewController* controller_ = nullptr;
    TokenResolver resolver_;
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
