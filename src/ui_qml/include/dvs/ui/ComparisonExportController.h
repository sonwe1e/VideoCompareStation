#pragma once

#include <QImage>
#include <QObject>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QUrl>

class QQuickItem;
class QQuickWindow;

namespace dvs::ui {

// Shareable comparison capture: the comparison viewport exactly as presented (a display
// result, never original code values) with an annotation bar naming the compared sources
// and the observation context. Copying puts the composed image on the clipboard; saving
// writes a PNG transactionally.
class ComparisonExportController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString lastStatus READ lastStatus NOTIFY statusChanged)
public:
    explicit ComparisonExportController(QObject* parent = nullptr);

    [[nodiscard]] QString lastStatus() const;

    // Captures the viewport item's current presentation, appends the annotation bar and
    // copies the composed image to the clipboard.
    Q_INVOKABLE bool copyComparison(QQuickItem* viewport, const QStringList& captionLines);

    // Same composition, written as PNG to the local file named by target. The write is
    // transactional: a failed save never truncates or replaces an existing file.
    Q_INVOKABLE bool
    saveComparison(QQuickItem* viewport, const QStringList& captionLines, const QUrl& target);

    // Maps the item's logical rect into the window capture's device pixels.
    [[nodiscard]] static QRect viewportPixelRect(const QQuickItem& viewport,
                                                 const QQuickWindow& window,
                                                 const QImage& windowCapture);
    // Annotation bar height in device pixels for the given caption line count.
    [[nodiscard]] static int captionBarHeight(int lineCount, qreal devicePixelRatio);
    // Crops the capture to the viewport rect and appends the caption bar (neutral gray,
    // elided lines). Returns a null image when the crop is empty.
    [[nodiscard]] static QImage composeLabeledCapture(const QImage& windowCapture,
                                                      const QRect& viewportPixelRect,
                                                      const QStringList& captionLines,
                                                      qreal devicePixelRatio);

signals:
    void statusChanged();

private:
    [[nodiscard]] QImage
    composeFromViewport(QQuickItem* viewport, const QStringList& captionLines, QString* error);
    void setStatus(QString text);

    QString lastStatus_;
};

} // namespace dvs::ui
