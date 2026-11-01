#include "dvs/ui/ComparisonExportController.h"

#include "dvs/ui/ComparisonSurface.h"

#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QList>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSaveFile>

#include <algorithm>

namespace dvs::ui {
namespace {

[[nodiscard]] QStringList normalizedLines(const QStringList& lines) {
    QStringList result;
    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            result.push_back(trimmed);
        }
    }
    return result;
}

// Maps a scene-space rect into the window capture's device pixels.
[[nodiscard]] QRect sceneRectToCapturePixels(const QRectF& sceneRect,
                                             const QQuickWindow& window,
                                             const QImage& windowCapture) {
    if (windowCapture.isNull() || window.width() <= 0 || window.height() <= 0) {
        return {};
    }
    const qreal scaleX =
        static_cast<qreal>(windowCapture.width()) / static_cast<qreal>(window.width());
    const qreal scaleY =
        static_cast<qreal>(windowCapture.height()) / static_cast<qreal>(window.height());
    const QRect pixels{
        static_cast<int>(std::floor(sceneRect.left() * scaleX)),
        static_cast<int>(std::floor(sceneRect.top() * scaleY)),
        static_cast<int>(std::ceil(sceneRect.width() * scaleX)),
        static_cast<int>(std::ceil(sceneRect.height() * scaleY)),
    };
    return pixels.intersected(windowCapture.rect());
}

} // namespace

ComparisonExportController::ComparisonExportController(QObject* parent) : QObject(parent) {}

QString ComparisonExportController::lastStatus() const {
    return lastStatus_;
}

QImage ComparisonExportController::capturePicture(QQuickItem* const picture, QString* const error) {
    if (picture == nullptr || picture->window() == nullptr) {
        if (error != nullptr) {
            *error = tr("对比画面不可用。");
        }
        return {};
    }
    QQuickWindow* const window = picture->window();

    // Suppress every overlay that would otherwise land inside the captured rect. The viewport
    // draws its chrome (source labels, badges, view commands, banners) beside the picture, and
    // the transport floats over the whole stage one level further up - so the walk climbs from
    // the picture to the window and hides each visible sibling whose scene rect overlaps the
    // picture. Ancestors are never touched: the viewport's own stage-well background behind a
    // letterbox must stay. grabWindow() is a blocking scene-graph sync/render barrier, so the
    // suppressed state is what gets captured and no intermediate frame is ever presented.
    const QRectF pictureSceneRect = picture->mapRectToScene(picture->boundingRect());
    QList<QQuickItem*> suppressed;
    for (QQuickItem* level = picture; level != nullptr;) {
        QQuickItem* const parent = level->parentItem();
        if (parent == nullptr) {
            break;
        }
        for (QQuickItem* sibling : parent->childItems()) {
            if (sibling == level || !sibling->isVisible()) {
                continue;
            }
            // Shrink by a hair so an item that merely touches the picture's edge - the mode
            // bar sitting exactly above the stage - is not suppressed for nothing.
            QRectF siblingRect = sibling->mapRectToScene(sibling->boundingRect());
            siblingRect.adjust(0.5, 0.5, -0.5, -0.5);
            if (!siblingRect.intersects(pictureSceneRect)) {
                continue;
            }
            sibling->setVisible(false);
            suppressed.append(sibling);
        }
        level = parent;
    }

    // The comparison surface is a QQuickRenderNode and synchronizes GUI-thread properties on
    // the next scene-graph frame, so a grab taken right after a state change can still observe
    // the previous one. One discarded pass consumes that frame; the second is the capture.
    static_cast<void>(window->grabWindow());
    const QImage windowCapture = window->grabWindow();

    for (QQuickItem* sibling : suppressed) {
        sibling->setVisible(true);
    }

    if (windowCapture.isNull()) {
        if (error != nullptr) {
            *error = tr("无法抓取当前画面。");
        }
        return {};
    }
    // The capture is the picture's own rect, narrowed to the rendered content when the
    // surface can say where the video actually sits: aspect-fit letterbox is empty stage,
    // not picture, so it must not travel into a shared image.
    QRectF captureSceneRect = picture->mapRectToScene(picture->boundingRect());
    if (const auto* const surface = qobject_cast<const ComparisonSurface*>(picture);
        surface != nullptr) {
        const QRectF content = surface->displayedContentRect();
        if (content.isValid() && !content.isNull()) {
            captureSceneRect = surface->mapRectToScene(content);
        }
    }
    const QRect rect = sceneRectToCapturePixels(captureSceneRect, *window, windowCapture);
    if (rect.isEmpty()) {
        if (error != nullptr) {
            *error = tr("对比画面区域为空。");
        }
        return {};
    }
    return windowCapture.copy(rect);
}

bool ComparisonExportController::copyComparison(QQuickItem* const picture) {
    QString error;
    const QImage capture = capturePicture(picture, &error);
    if (capture.isNull()) {
        setStatus(error);
        return false;
    }
    QGuiApplication::clipboard()->setImage(capture);
    setStatus(tr("已复制对比图"));
    return true;
}

bool ComparisonExportController::saveComparison(QQuickItem* const picture,
                                                const QStringList& provenanceLines,
                                                const QUrl& target) {
    const QString path = target.isLocalFile() ? target.toLocalFile() : target.toString();
    if (path.isEmpty()) {
        setStatus(tr("未选择保存位置。"));
        return false;
    }
    QString error;
    QImage capture = capturePicture(picture, &error);
    if (capture.isNull()) {
        setStatus(error);
        return false;
    }
    // The provenance travels in the PNG text metadata, never in the pixels: the saved image
    // stays the bare comparison picture while the file still records what it shows.
    const QStringList lines = normalizedLines(provenanceLines);
    if (!lines.isEmpty()) {
        capture.setText(QStringLiteral("Description"), lines.join(QLatin1Char('\n')));
    }
    QSaveFile file{path};
    if (!file.open(QIODevice::WriteOnly)) {
        setStatus(tr("无法写入 %1。").arg(QDir::toNativeSeparators(path)));
        return false;
    }
    if (!capture.save(&file, "PNG")) {
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
