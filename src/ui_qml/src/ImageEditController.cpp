#include "dvs/ui/ImageEditController.h"

#include <QBuffer>
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPainter>
#include <QPen>
#include <QPoint>
#include <QRect>
#include <QSaveFile>
#include <QUndoCommand>
#include <QUndoStack>

#include <algorithm>
#include <utility>

namespace dvs::ui {
namespace {

constexpr int kDefaultHistoryLimit = 8;
constexpr int kMaximumHistoryLimit = 64;
constexpr int kMaximumImageEdge = 16384;
// A live brush preview bumps the provider revision, and the QML Image re-uploads the whole
// buffer on each bump. Throttling keeps a stroke responsive without flooding the GPU.
constexpr int kStrokePreviewIntervalMs = 66;
constexpr int kMaximumBrushWidth = 256;
constexpr int kMaximumMosaicBlock = 64;

[[nodiscard]] QString targetSuffix(const QUrl& target) {
    const QString path = target.isLocalFile() ? target.toLocalFile() : target.toString();
    return QFileInfo{path}.suffix().toLower();
}

} // namespace

class ImageEditController::Impl final {
public:
    // One undoable crop. A crop that removed pixels can only be undone from the previous
    // buffer, so each step retains the pre-crop image; the bounded history limit keeps
    // that retention predictable (8 steps by default). Pixel tools added later can store a
    // dirty-rect patch instead and mix into the same stack.
    class CropCommand final : public QUndoCommand {
    public:
        CropCommand(Impl* owner, const QRect rect)
            : owner_(owner), rect_(rect), previous_(owner->working) {
            setText(ImageEditController::tr("裁剪"));
        }

        void undo() override {
            owner_->working = previous_;
        }

        void redo() override {
            owner_->working = owner_->working.copy(rect_);
        }

    private:
        Impl* owner_;
        QRect rect_;
        QImage previous_;
    };

    // One undoable pixel edit that stayed inside a rect (brush stroke, mosaic). Only the
    // dirty rect is retained, so a stroke costs its own footprint instead of a full image.
    class PatchCommand final : public QUndoCommand {
    public:
        PatchCommand(Impl* owner, const QRect rect, QImage before, QImage after, QString label)
            : owner_(owner), rect_(rect), before_(std::move(before)), after_(std::move(after)) {
            setText(std::move(label));
        }

        void undo() override {
            blit(before_);
        }

        void redo() override {
            blit(after_);
        }

    private:
        void blit(const QImage& patch) {
            if (patch.isNull() || rect_.isEmpty()) {
                return;
            }
            QPainter painter(&owner_->working);
            // Source (not SourceOver) so undo restores alpha exactly rather than blending.
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.drawImage(rect_.topLeft(), patch);
        }

        Impl* owner_;
        QRect rect_;
        QImage before_;
        QImage after_;
    };

    SourceImageProvider sourceProvider;
    QImage original;
    QImage working;
    int slot = -1;
    QString label;
    QString sourcePath;
    int revision = 0;
    QString status;
    QUndoStack history;
    // Live brush stroke: the base buffer is transient (released when the stroke commits)
    // and only the stroke's dirty rect enters the history.
    QImage strokeBase;
    QRect strokeBounds;
    QPoint strokeLast{-1, -1};
    QElapsedTimer strokePreviewClock;
    bool strokeOpen = false;
    QColor strokeColor{Qt::red};
    int strokeWidth = 4;
    qreal strokeOpacity = 1.0;

    [[nodiscard]] QRect strokeDirtyRect() const {
        if (!strokeBounds.isValid()) {
            return {};
        }
        const int margin = (strokeWidth / 2) + 2;
        return strokeBounds.adjusted(-margin, -margin, margin, margin)
            .intersected(QRect{0, 0, working.width(), working.height()});
    }

    void paintSegment(const QPoint& from, const QPoint& to) {
        QPainter painter(&working);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setOpacity(std::clamp(strokeOpacity, 0.05, 1.0));
        if (from == to) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(strokeColor);
            painter.drawEllipse(QPointF{from}, strokeWidth / 2.0, strokeWidth / 2.0);
            return;
        }
        QPen pen{strokeColor};
        pen.setWidth(strokeWidth);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.drawLine(from, to);
    }
};

ImageEditController::ImageEditController(QObject* parent) : QObject(parent), impl_(nullptr) {
    impl_ = std::make_unique<Impl>();
    impl_->history.setUndoLimit(kDefaultHistoryLimit);
    QObject::connect(&impl_->history, &QUndoStack::indexChanged, this, [this] {
        ++impl_->revision;
        emit imageChanged();
        emit historyChanged();
        emit stateChanged();
    });
    QObject::connect(
        &impl_->history, &QUndoStack::cleanChanged, this, [this] { emit stateChanged(); });
}

ImageEditController::~ImageEditController() = default;

void ImageEditController::setSourceImageProvider(SourceImageProvider provider) {
    impl_->sourceProvider = std::move(provider);
}

bool ImageEditController::active() const noexcept {
    return impl_->slot >= 0 && !impl_->working.isNull();
}

bool ImageEditController::dirty() const noexcept {
    return active() && !impl_->history.isClean();
}

int ImageEditController::revision() const noexcept {
    return impl_->revision;
}

int ImageEditController::sourceSlot() const noexcept {
    return impl_->slot;
}

QString ImageEditController::sourceLabel() const {
    return impl_->label;
}

int ImageEditController::imageWidth() const noexcept {
    return impl_->working.width();
}

int ImageEditController::imageHeight() const noexcept {
    return impl_->working.height();
}

bool ImageEditController::imageHasAlpha() const noexcept {
    return !impl_->working.isNull() && impl_->working.hasAlphaChannel();
}

bool ImageEditController::canUndo() const noexcept {
    return impl_->history.canUndo();
}

bool ImageEditController::canRedo() const noexcept {
    return impl_->history.canRedo();
}

QString ImageEditController::undoLabel() const {
    return impl_->history.undoText();
}

QString ImageEditController::redoLabel() const {
    return impl_->history.redoText();
}

QString ImageEditController::lastStatus() const {
    return impl_->status;
}

void ImageEditController::setHistoryLimit(const int steps) {
    const int bounded = std::clamp(steps, 1, kMaximumHistoryLimit);
    if (impl_->history.undoLimit() == bounded) {
        return;
    }
    impl_->history.setUndoLimit(bounded);
}

int ImageEditController::historyLimit() const noexcept {
    return impl_->history.undoLimit();
}

bool ImageEditController::beginSession(const int slot,
                                       const QString& label,
                                       const QUrl& sourceUrl) {
    if (!impl_->sourceProvider) {
        setStatus(tr("编辑功能不可用。"));
        return false;
    }
    const QImage original = impl_->sourceProvider(slot);
    if (original.isNull()) {
        setStatus(tr("没有可编辑的图片。"));
        return false;
    }
    if (original.width() > kMaximumImageEdge || original.height() > kMaximumImageEdge) {
        setStatus(tr("图片单边超过 %1 像素，暂不支持编辑。").arg(kMaximumImageEdge));
        return false;
    }
    // Two independent buffers: the decoded original is kept immutable, every tool works on
    // the working copy. Nothing below ever writes to `original`.
    impl_->original = original.copy();
    impl_->working = original.copy();
    impl_->slot = slot;
    impl_->label = label;
    impl_->sourcePath = sourceUrl.isLocalFile() ? sourceUrl.toLocalFile() : sourceUrl.toString();
    impl_->history.clear();
    impl_->history.setClean();
    ++impl_->revision;
    emit stateChanged();
    emit imageChanged();
    emit historyChanged();
    setStatus(tr("编辑模式：原图保持不变，所有修改另存副本。"));
    return true;
}

void ImageEditController::endSession() {
    if (impl_->slot < 0 && impl_->working.isNull()) {
        return;
    }
    impl_->history.clear();
    impl_->original = QImage();
    impl_->working = QImage();
    impl_->slot = -1;
    impl_->label.clear();
    impl_->sourcePath.clear();
    // A session can end mid-stroke (workspace closed); the stroke preview buffer must not
    // outlive it.
    impl_->strokeOpen = false;
    impl_->strokeBase = QImage();
    impl_->strokeBounds = QRect{};
    impl_->strokeLast = QPoint{-1, -1};
    impl_->history.setClean();
    ++impl_->revision;
    emit stateChanged();
    emit imageChanged();
    emit historyChanged();
    setStatus(tr("已退出编辑模式。"));
}

bool ImageEditController::cropToImageRect(const int x,
                                          const int y,
                                          const int width,
                                          const int height) {
    if (!active()) {
        setStatus(tr("没有正在编辑的图片。"));
        return false;
    }
    if (width <= 0 || height <= 0) {
        setStatus(tr("裁剪区域为空。"));
        return false;
    }
    const QRect wanted{x, y, width, height};
    const QRect bounded =
        wanted.intersected(QRect{0, 0, impl_->working.width(), impl_->working.height()});
    if (bounded.isEmpty()) {
        setStatus(tr("裁剪区域超出图片范围。"));
        return false;
    }
    if (bounded.topLeft() == QPoint{0, 0} && bounded.size() == impl_->working.size()) {
        setStatus(tr("裁剪区域与当前图片相同。"));
        return false;
    }
    impl_->history.push(new Impl::CropCommand(impl_.get(), bounded));
    setStatus(tr("已裁剪为 %1×%2").arg(bounded.width()).arg(bounded.height()));
    return true;
}

bool ImageEditController::beginStroke(const QColor& color, const int width, const qreal opacity) {
    if (!active()) {
        setStatus(tr("没有正在编辑的图片。"));
        return false;
    }
    if (!color.isValid()) {
        setStatus(tr("画笔颜色无效。"));
        return false;
    }
    if (impl_->strokeOpen) {
        // Never leave a stale stroke open: it would blur two gestures into one history step.
        endStroke();
    }
    impl_->strokeColor = color;
    impl_->strokeWidth = std::clamp(width, 1, kMaximumBrushWidth);
    impl_->strokeOpacity = std::clamp(opacity, 0.05, 1.0);
    // Transient base buffer for the undo patch; released as soon as the stroke commits.
    impl_->strokeBase = impl_->working;
    impl_->strokeBounds = QRect{};
    impl_->strokeLast = QPoint{-1, -1};
    impl_->strokeOpen = true;
    impl_->strokePreviewClock.start();
    emit stateChanged();
    return true;
}

bool ImageEditController::strokeTo(const int x, const int y) {
    if (!impl_->strokeOpen) {
        setStatus(tr("没有正在进行的笔画。"));
        return false;
    }
    if (impl_->working.isNull()) {
        return false;
    }
    const QPoint point{std::clamp(x, 0, impl_->working.width() - 1),
                       std::clamp(y, 0, impl_->working.height() - 1)};
    if (impl_->strokeLast.x() < 0) {
        impl_->strokeLast = point;
        impl_->strokeBounds = QRect{point, point};
        impl_->paintSegment(point, point);
    } else {
        impl_->strokeBounds = impl_->strokeBounds.united(QRect{impl_->strokeLast, point});
        impl_->paintSegment(impl_->strokeLast, point);
        impl_->strokeLast = point;
    }
    if (!impl_->strokePreviewClock.isValid() ||
        impl_->strokePreviewClock.elapsed() >= kStrokePreviewIntervalMs) {
        impl_->strokePreviewClock.restart();
        ++impl_->revision;
        emit imageChanged();
    }
    return true;
}

bool ImageEditController::endStroke() {
    if (!impl_->strokeOpen) {
        setStatus(tr("没有正在进行的笔画。"));
        return false;
    }
    impl_->strokeOpen = false;
    const QRect dirty = impl_->strokeDirtyRect();
    if (!dirty.isEmpty()) {
        impl_->history.push(new Impl::PatchCommand(impl_.get(),
                                                   dirty,
                                                   impl_->strokeBase.copy(dirty),
                                                   impl_->working.copy(dirty),
                                                   tr("画笔")));
    } else {
        ++impl_->revision;
        emit imageChanged();
    }
    impl_->strokeBase = QImage();
    impl_->strokeBounds = QRect{};
    impl_->strokeLast = QPoint{-1, -1};
    emit stateChanged();
    setStatus(dirty.isEmpty() ? tr("笔画没有产生改动。") : tr("已完成笔画。"));
    return true;
}

bool ImageEditController::strokeActive() const noexcept {
    return impl_->strokeOpen;
}

bool ImageEditController::mosaicImageRect(
    const int x, const int y, const int width, const int height, const int blockSize) {
    if (!active()) {
        setStatus(tr("没有正在编辑的图片。"));
        return false;
    }
    if (width <= 0 || height <= 0) {
        setStatus(tr("马赛克区域为空。"));
        return false;
    }
    const QRect bounded = QRect{x, y, width, height}.intersected(
        QRect{0, 0, impl_->working.width(), impl_->working.height()});
    if (bounded.isEmpty()) {
        setStatus(tr("马赛克区域超出图片范围。"));
        return false;
    }
    const int block =
        std::clamp(blockSize, 2, std::max(2, std::min(bounded.width(), bounded.height())));
    // Nearest-neighbour down/up scaling is the deterministic pixelate: each block keeps one
    // sampled colour, and the blocks stay opaque so the region really is obscured.
    const QImage pixelated =
        impl_->working.copy(bounded)
            .scaled(std::max(1, bounded.width() / block),
                    std::max(1, bounded.height() / block),
                    Qt::IgnoreAspectRatio,
                    Qt::FastTransformation)
            .scaled(bounded.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
    QImage after = impl_->working.copy(bounded);
    {
        QPainter painter(&after);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(0, 0, pixelated);
    }
    impl_->history.push(new Impl::PatchCommand(
        impl_.get(), bounded, impl_->working.copy(bounded), std::move(after), tr("马赛克")));
    setStatus(tr("已对 %1×%2 区域应用马赛克（块 %3 像素）")
                  .arg(bounded.width())
                  .arg(bounded.height())
                  .arg(block));
    return true;
}

bool ImageEditController::undo() {
    if (!impl_->history.canUndo()) {
        setStatus(tr("没有可撤销的操作。"));
        return false;
    }
    impl_->history.undo();
    setStatus(tr("已撤销：%1").arg(impl_->history.redoText()));
    return true;
}

bool ImageEditController::redo() {
    if (!impl_->history.canRedo()) {
        setStatus(tr("没有可重做的操作。"));
        return false;
    }
    impl_->history.redo();
    setStatus(tr("已重做：%1").arg(impl_->history.undoText()));
    return true;
}

QString ImageEditController::editedImageUrl() const {
    return QStringLiteral("image://dvs-edit/%1").arg(impl_->revision);
}

bool ImageEditController::saveCopy(const QUrl& target,
                                   const bool flattenAlpha,
                                   const QColor& background) {
    if (!active()) {
        setStatus(tr("没有正在编辑的图片。"));
        return false;
    }
    const QString path = target.isLocalFile() ? target.toLocalFile() : target.toString();
    if (path.isEmpty()) {
        setStatus(tr("未选择保存位置。"));
        return false;
    }
    // Edits always land in a copy: overwriting the source would silently destroy the
    // material the comparison still refers to.
    if (!impl_->sourcePath.isEmpty()) {
        const QString sourceAbsolute = QFileInfo{impl_->sourcePath}.absoluteFilePath();
        const QString targetAbsolute = QFileInfo{path}.absoluteFilePath();
        if (!sourceAbsolute.isEmpty() &&
            sourceAbsolute.compare(targetAbsolute, Qt::CaseInsensitive) == 0) {
            setStatus(tr("请另存为副本：原图保持不变。"));
            return false;
        }
    }
    QImage output = impl_->working;
    const QString suffix = targetSuffix(target);
    const bool jpeg = suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg");
    if (jpeg && output.hasAlphaChannel()) {
        if (!flattenAlpha) {
            setStatus(tr("JPEG 不支持透明，请先选择合成背景。"));
            return false;
        }
        output = flattenOntoBackground(output, background);
    }
    if (jpeg && !jpegEncodingAvailable()) {
        // The Qt JPEG plugin is optional; without it QImageWriter would silently produce
        // nothing. Say so instead of writing a broken file.
        setStatus(tr("当前构建未包含 JPEG 编码器，请另存为 PNG。"));
        return false;
    }
    QSaveFile file{path};
    if (!file.open(QIODevice::WriteOnly)) {
        setStatus(tr("无法写入 %1。").arg(QDir::toNativeSeparators(path)));
        return false;
    }
    if (!output.save(&file, jpeg ? "JPEG" : "PNG")) {
        setStatus(tr("无法编码图片：%1").arg(QDir::toNativeSeparators(path)));
        return false;
    }
    if (!file.commit()) {
        setStatus(tr("保存失败：%1").arg(QDir::toNativeSeparators(path)));
        return false;
    }
    // The saved copy becomes the new clean baseline for the dirty indicator.
    impl_->history.setClean();
    setStatus(tr("已另存副本：%1").arg(QDir::toNativeSeparators(path)));
    return true;
}

QImage ImageEditController::editedImage() const {
    return impl_->working;
}

QImage ImageEditController::flattenOntoBackground(const QImage& image, const QColor& background) {
    if (image.isNull()) {
        return {};
    }
    QImage flattened(image.size(), QImage::Format_RGB32);
    flattened.fill(background.isValid() ? background : QColor(Qt::white));
    QPainter painter(&flattened);
    painter.drawImage(0, 0, image);
    painter.end();
    return flattened;
}

bool ImageEditController::jpegEncodingAvailable() {
    // Probing with QImage::save keeps this translation unit free of QImageWriter, whose
    // qimageiohandler.h pulls QtCore/qplugin.h — a header clang-tidy cannot parse as a
    // constant expression in this toolchain. The probe exercises the same encoder path a
    // real save would use, so it cannot claim a capability the save lacks.
    QImage probe(1, 1, QImage::Format_RGB32);
    probe.fill(Qt::white);
    QBuffer buffer;
    if (!buffer.open(QIODevice::WriteOnly)) {
        return false;
    }
    return probe.save(&buffer, "JPEG");
}

QImage ImageEditController::sourceImage() const {
    return impl_->original;
}

void ImageEditController::setStatus(QString text) {
    if (impl_->status == text) {
        return;
    }
    impl_->status = std::move(text);
    emit statusChanged();
}

} // namespace dvs::ui
