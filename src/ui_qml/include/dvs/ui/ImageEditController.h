#pragma once

#include <QColor>
#include <QImage>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>
#include <memory>

namespace dvs::ui {

// Lightweight still-image editing over the review workspace. The decoded original stays
// immutable; every tool edits a working copy, every operation is undoable through a
// bounded history, and saving always writes a new file. Crop and annotation geometry is
// expressed in image pixels, so a zoomed or panned viewport cannot drift a selection.
//
// Editing is deliberately separate from comparison: the workspace may show the edited
// copy, but the difference pipeline and metrics keep using the committed originals.
class ImageEditController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY stateChanged)
    Q_PROPERTY(int revision READ revision NOTIFY imageChanged)
    Q_PROPERTY(int sourceSlot READ sourceSlot NOTIFY stateChanged)
    Q_PROPERTY(QString sourceLabel READ sourceLabel NOTIFY stateChanged)
    Q_PROPERTY(int imageWidth READ imageWidth NOTIFY imageChanged)
    Q_PROPERTY(int imageHeight READ imageHeight NOTIFY imageChanged)
    Q_PROPERTY(bool imageHasAlpha READ imageHasAlpha NOTIFY imageChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(QString undoLabel READ undoLabel NOTIFY historyChanged)
    Q_PROPERTY(QString redoLabel READ redoLabel NOTIFY historyChanged)
    // Cache-busting URL of the working copy for the dvs-edit image provider; changes with
    // every edit so a QML Image binding reloads without any manual invalidation.
    Q_PROPERTY(QString editedImageUrl READ editedImageUrl NOTIFY imageChanged)
    // True while a brush stroke is being drawn (the workspace shows a live preview).
    Q_PROPERTY(bool strokeActive READ strokeActive NOTIFY stateChanged)
    // Flat annotation list state: count and selected index (-1 when nothing is selected).
    // The newest annotation is drawn last, so it sits on top.
    Q_PROPERTY(int annotationCount READ annotationCount NOTIFY annotationsChanged)
    Q_PROPERTY(int selectedAnnotation READ selectedAnnotation NOTIFY annotationsChanged)
    Q_PROPERTY(QString lastStatus READ lastStatus NOTIFY statusChanged)
public:
    // Decoded original for one display slot, injected by the composition root so the
    // controller never reaches into the review workspace directly.
    using SourceImageProvider = std::function<QImage(int slot)>;

    explicit ImageEditController(QObject* parent = nullptr);
    ~ImageEditController() override;

    void setSourceImageProvider(SourceImageProvider provider);

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] int revision() const noexcept;
    [[nodiscard]] int sourceSlot() const noexcept;
    [[nodiscard]] QString sourceLabel() const;
    [[nodiscard]] int imageWidth() const noexcept;
    [[nodiscard]] int imageHeight() const noexcept;
    [[nodiscard]] bool imageHasAlpha() const noexcept;
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    [[nodiscard]] QString undoLabel() const;
    [[nodiscard]] QString redoLabel() const;
    [[nodiscard]] QString lastStatus() const;

    // Starts an edit session from the committed original of the given display slot. The
    // original buffer is copied once and never modified; all tools work on the copy.
    Q_INVOKABLE bool beginSession(int slot, const QString& label, const QUrl& sourceUrl);
    // Ends the session. Dirty edits are simply dropped; the original file was never
    // written to, so nothing else has to be restored.
    Q_INVOKABLE void endSession();
    // Crops the working copy to an image-pixel rect, clamped to the current image. The
    // rect is in image coordinates, never viewport coordinates.
    Q_INVOKABLE bool cropToImageRect(int x, int y, int width, int height);
    // Brush. One stroke is one undo step; the stroke is painted live into the working copy
    // and committed with a dirty-rect patch, so history never stores a full image per
    // stroke. Points are image pixels: a zoomed or panned viewport cannot drift them.
    // Opacity is 0..1 and width is in image pixels.
    Q_INVOKABLE bool beginStroke(const QColor& color, int width, qreal opacity);
    Q_INVOKABLE bool strokeTo(int x, int y);
    Q_INVOKABLE bool endStroke();
    [[nodiscard]] bool strokeActive() const noexcept;
    // Mosaic: pixelates an image-pixel rect into opaque blocks of the given size.
    Q_INVOKABLE bool mosaicImageRect(int x, int y, int width, int height, int blockSize);
    // Fill / clear an image-pixel rect: an opaque colour block (the review's preferred way
    // to mask sensitive content) or transparent pixels. Both are undoable pixel edits.
    Q_INVOKABLE bool fillImageRect(int x, int y, int width, int height, const QColor& color);
    Q_INVOKABLE bool clearImageRect(int x, int y, int width, int height);
    // Annotations live above the pixel edits as a flat, bounded list — deliberately not a
    // layer panel. They are rendered into the displayed and saved image but never into the
    // comparison metrics or the difference pipeline. Geometry is in image pixels.
    Q_INVOKABLE bool
    addArrow(int fromX, int fromY, int toX, int toY, const QColor& color, int width);
    Q_INVOKABLE bool
    addRectangle(int fromX, int fromY, int toX, int toY, const QColor& color, int width);
    Q_INVOKABLE bool addText(int x, int y, const QString& text, const QColor& color, int pixelSize);
    Q_INVOKABLE bool selectAnnotationAt(int x, int y);
    // Drag gesture: the live preview never touches the history — one gesture commits one
    // move step on release.
    Q_INVOKABLE bool beginAnnotationDrag(int x, int y);
    Q_INVOKABLE bool dragAnnotationTo(int x, int y);
    Q_INVOKABLE bool endAnnotationDrag();
    Q_INVOKABLE bool moveSelectedAnnotation(int deltaX, int deltaY);
    Q_INVOKABLE bool deleteSelectedAnnotation();
    Q_INVOKABLE void clearAnnotations();
    Q_INVOKABLE bool undo();
    Q_INVOKABLE bool redo();
    // Cache-busting URL of the working copy for the dvs-edit image provider.
    [[nodiscard]] QString editedImageUrl() const;
    // Writes the working copy as a new file. PNG keeps transparency; JPEG cannot, so a
    // JPEG target with alpha present requires flattenAlpha plus an opaque background.
    // Writing to the session's own source file is refused: edits are saved as a copy.
    Q_INVOKABLE bool saveCopy(const QUrl& target, bool flattenAlpha, const QColor& background);
    // Composites the working copy onto an opaque background (JPEG has no alpha). Exposed
    // because the background rule must hold even when this build cannot encode JPEG.
    [[nodiscard]] static QImage flattenOntoBackground(const QImage& image,
                                                      const QColor& background);
    // Whether this build can encode JPEG at all (the Qt JPEG plugin is optional).
    [[nodiscard]] static bool jpegEncodingAvailable();
    // Bounded history depth (default 8). Exposed for tests.
    void setHistoryLimit(int steps);
    [[nodiscard]] int historyLimit() const noexcept;
    [[nodiscard]] int annotationCount() const noexcept;
    [[nodiscard]] int selectedAnnotation() const noexcept;

    // Provider and test access to the current buffers.
    [[nodiscard]] QImage editedImage() const;
    [[nodiscard]] QImage sourceImage() const;

signals:
    void stateChanged();
    void imageChanged();
    void historyChanged();
    void annotationsChanged();
    void statusChanged();

private:
    class Impl;

    void setStatus(QString text);

    std::unique_ptr<Impl> impl_;
};

} // namespace dvs::ui
