#include "dvs/ui/ColorClipboardController.h"

#include <QClipboard>
#include <QGuiApplication>

namespace dvs::ui {

ColorClipboardController::ColorClipboardController(QObject* parent) : QObject(parent) {}

bool ColorClipboardController::copyText(const QString& text) {
    if (text.isEmpty()) {
        setStatus(tr("没有可复制的颜色样本。"));
        return false;
    }
    // The clipboard belongs to a GUI application. A headless embedding (unit tests,
    // CLI helpers) has none, and QGuiApplication::clipboard() is undefined there.
    auto* const application = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    if (application == nullptr) {
        setStatus(tr("当前运行环境没有剪贴板。"));
        return false;
    }
    QClipboard* const clipboard = application->clipboard();
    if (clipboard == nullptr) {
        setStatus(tr("当前平台没有可用剪贴板。"));
        return false;
    }
    clipboard->setText(text);
    // Read the value back: reporting success without it would be a claim, not an
    // observation.
    if (clipboard->text() != text) {
        setStatus(tr("复制失败：剪贴板未接受该内容。"));
        return false;
    }
    setStatus(tr("已复制 %1").arg(text));
    return true;
}

QString ColorClipboardController::statusText() const {
    return statusText_;
}

void ColorClipboardController::setStatus(QString text) {
    if (statusText_ == text) {
        return;
    }
    statusText_ = std::move(text);
    Q_EMIT statusChanged();
}

} // namespace dvs::ui
