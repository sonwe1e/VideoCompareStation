#pragma once

#include <QObject>
#include <QString>

namespace dvs::ui {

// Text-clipboard capability for the pixel-probe colour copy. Kept as its own adapter
// instead of living inside ImageReviewController for two reasons: the controller is
// exercised by headless GoogleTest suites that only build a QCoreApplication (touching
// QGuiApplication::clipboard() there is undefined), and the clipboard is an outer
// platform capability that the image workspace consumes through a guarded optional
// context property, exactly like the comparison export controller.
//
// Qt's standard clipboard is the only dependency; nothing third-party is introduced.
class ColorClipboardController final : public QObject {
    Q_OBJECT

public:
    explicit ColorClipboardController(QObject* parent = nullptr);

    // Writes the text and verifies the write by reading it back, so a failure can never
    // be reported as a success. Returns false (and leaves statusText explaining why) when
    // no GUI application owns a clipboard or the platform refuses the write.
    Q_INVOKABLE bool copyText(const QString& text);

    // Short user-facing result of the last attempt, already localized.
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    [[nodiscard]] QString statusText() const;

signals:
    void statusChanged();

private:
    void setStatus(QString text);

    QString statusText_;
};

} // namespace dvs::ui
