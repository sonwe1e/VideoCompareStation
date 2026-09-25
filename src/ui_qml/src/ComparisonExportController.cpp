#include "dvs/ui/ComparisonExportController.h"

#include <QClipboard>
#include <QColor>
#include <QDir>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QPainter>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSaveFile>

#include <algorithm>

namespace dvs::ui {
namespace {

// Neutral gray annotation bar: the app chrome is dark-themed, but the exported capture
// must not tint color judgment, so the bar uses true neutral grays.
constexpr QColor kBarBackground{0x1a, 0x1a, 0x1a};
constexpr QColor kBarSeparator{0x30, 0x30, 0x30};
constexpr QColor kBarText{0xed, 0xed, 0xed};

[[nodiscard]] QStringList normalizedCaption(const QStringList& captionLines) {
    QStringList result;
    for (const QString& line : captionLines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            result.push_back(trimmed);
        }
    }
    return result;
}

} // namespace

ComparisonExportController::ComparisonExportController(QObject* parent) : QObject(parent) {}

QString ComparisonExportController::lastStatus() const {
    return lastStatus_;
}

int ComparisonExportController::captionBarHeight(const int lineCount,
                                                 const qreal devicePixelRatio) {
    if (lineCount <= 0) {
        return 0;
    }
    const qreal scale = std::max(1.0, devicePixelRatio);
    const int padding = qRound(7.0 * scale);
    const int lineHeight = qRound(21.0 * scale);
    return padding * 2 + lineCount * lineHeight;
}

QRect ComparisonExportController::viewportPixelRect(const QQuickItem& viewport,
                                                    const QQuickWindow& window,
                                                    const QImage& windowCapture) {
    if (windowCapture.isNull() || window.width() <= 0 || window.height() <= 0) {
        return {};
    }
    const QPointF topLeft = viewport.mapToScene(QPointF{});
    const qreal scaleX =
        static_cast<qreal>(windowCapture.width()) / static_cast<qreal>(window.width());
    const qreal scaleY =
        static_cast<qreal>(windowCapture.height()) / static_cast<qreal>(window.height());
    const QRect pixelBounds{
        static_cast<int>(std::floor(topLeft.x() * scaleX)),
        static_cast<int>(std::floor(topLeft.y() * scaleY)),
        static_cast<int>(std::ceil(viewport.width() * scaleX)),
        static_cast<int>(std::ceil(viewport.height() * scaleY)),
    };
    return pixelBounds.intersected(windowCapture.rect());
}

QImage ComparisonExportController::composeLabeledCapture(const QImage& windowCapture,
                                                         const QRect& viewportRect,
                                                         const QStringList& captionLines,
                                                         const qreal devicePixelRatio) {
    if (windowCapture.isNull()) {
        return {};
    }
    const QRect bounded = viewportRect.intersected(windowCapture.rect());
    if (bounded.isEmpty()) {
        return {};
    }
    const QImage viewportCapture = windowCapture.copy(bounded);
    const QStringList caption = normalizedCaption(captionLines);
    if (caption.isEmpty()) {
        return viewportCapture;
    }
    const qreal scale = std::max(1.0, devicePixelRatio);
    const int barHeight = captionBarHeight(static_cast<int>(caption.size()), scale);
    QImage composed(
        viewportCapture.width(), viewportCapture.height() + barHeight, QImage::Format_RGBA8888);
    if (composed.isNull()) {
        return {};
    }
    composed.fill(Qt::transparent);
    QPainter painter(&composed);
    painter.drawImage(0, 0, viewportCapture);
    const int barTop = viewportCapture.height();
    painter.fillRect(0, barTop, composed.width(), barHeight, kBarBackground);
    painter.fillRect(0, barTop, composed.width(), qMax(1, qRound(scale)), kBarSeparator);
    QFont font = painter.font();
    font.setPixelSize(std::max(9, qRound(13.0 * scale)));
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.setPen(kBarText);
    const QFontMetrics metrics{font};
    const int padding = qRound(7.0 * scale);
    const int lineHeight = qRound(21.0 * scale);
    const int textWidth = composed.width() - padding * 2;
    int baseline = barTop + padding + metrics.ascent();
    for (const QString& line : caption) {
        painter.drawText(QPoint{padding, baseline},
                         metrics.elidedText(line, Qt::ElideMiddle, qMax(1, textWidth)));
        baseline += lineHeight;
    }
    painter.end();
    return composed;
}

QImage ComparisonExportController::composeFromViewport(QQuickItem* const viewport,
                                                       const QStringList& captionLines,
                                                       QString* const error) {
    if (viewport == nullptr || viewport->window() == nullptr) {
        if (error != nullptr) {
            *error = tr("对比画面不可用。");
        }
        return {};
    }
    QQuickWindow* const window = viewport->window();
    const QImage windowCapture = window->grabWindow();
    if (windowCapture.isNull()) {
        if (error != nullptr) {
            *error = tr("无法抓取当前画面。");
        }
        return {};
    }
    const QRect rect = viewportPixelRect(*viewport, *window, windowCapture);
    if (rect.isEmpty()) {
        if (error != nullptr) {
            *error = tr("对比画面区域为空。");
        }
        return {};
    }
    QImage composed = composeLabeledCapture(
        windowCapture, rect, captionLines, window->effectiveDevicePixelRatio());
    if (composed.isNull() && error != nullptr) {
        *error = tr("无法合成对比图。");
    }
    return composed;
}

bool ComparisonExportController::copyComparison(QQuickItem* const viewport,
                                                const QStringList& captionLines) {
    QString error;
    const QImage composed = composeFromViewport(viewport, captionLines, &error);
    if (composed.isNull()) {
        setStatus(error);
        return false;
    }
    QGuiApplication::clipboard()->setImage(composed);
    setStatus(tr("已复制对比图（含来源与观察标注）"));
    return true;
}

bool ComparisonExportController::saveComparison(QQuickItem* const viewport,
                                                const QStringList& captionLines,
                                                const QUrl& target) {
    const QString path = target.isLocalFile() ? target.toLocalFile() : target.toString();
    if (path.isEmpty()) {
        setStatus(tr("未选择保存位置。"));
        return false;
    }
    QString error;
    const QImage composed = composeFromViewport(viewport, captionLines, &error);
    if (composed.isNull()) {
        setStatus(error);
        return false;
    }
    QSaveFile file{path};
    if (!file.open(QIODevice::WriteOnly)) {
        setStatus(tr("无法写入 %1。").arg(QDir::toNativeSeparators(path)));
        return false;
    }
    if (!composed.save(&file, "PNG")) {
        setStatus(tr("无法编码 PNG：%1").arg(QDir::toNativeSeparators(path)));
        return false;
    }
    if (!file.commit()) {
        setStatus(tr("保存失败：%1").arg(QDir::toNativeSeparators(path)));
        return false;
    }
    setStatus(tr("已保存对比图：%1").arg(QDir::toNativeSeparators(path)));
    return true;
}

void ComparisonExportController::setStatus(QString text) {
    if (lastStatus_ == text) {
        return;
    }
    lastStatus_ = std::move(text);
    emit statusChanged();
}

} // namespace dvs::ui
