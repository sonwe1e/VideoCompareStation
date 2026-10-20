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

// Shareable comparison capture: the comparison picture exactly as presented - the rendered
// comparison surface with every piece of app chrome suppressed for the duration of the grab,
// and no annotation bar. The capture is a display result, never original code values.
// Copying puts the bare image on the clipboard; saving writes a PNG transactionally and
// carries the provenance in the file's text metadata instead of in the pixels.
class ComparisonExportController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString lastStatus READ lastStatus NOTIFY statusChanged)
public:
    explicit ComparisonExportController(QObject* parent = nullptr);

    [[nodiscard]] QString lastStatus() const;

    // Captures the picture item's current presentation with its sibling overlays suppressed,
    // and copies the bare image to the clipboard.
    Q_INVOKABLE bool copyComparison(QQuickItem* picture);

    // Same capture, written as PNG to the local file named by target. The write is
    // transactional: a failed save never truncates or replaces an existing file. The
    // provenance lines travel in the PNG text metadata, never in the pixels.
    Q_INVOKABLE bool
    saveComparison(QQuickItem* picture, const QStringList& provenanceLines, const QUrl& target);

private:
    [[nodiscard]] QImage capturePicture(QQuickItem* picture, QString* error);
    void setStatus(QString text);

    QString lastStatus_;

signals:
    void statusChanged();
};

} // namespace dvs::ui
