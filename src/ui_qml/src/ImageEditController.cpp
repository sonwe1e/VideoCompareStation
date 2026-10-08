#include "dvs/ui/ImageEditController.h"

#include <QBuffer>
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPen>
#include <QPoint>
#include <QRect>
#include <QSaveFile>
#include <QUndoCommand>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace dvs::ui {
namespace {

constexpr int kDefaultHistoryLimit = 8;
constexpr int kMaximumHistoryLimit = 64;
constexpr int kMaximumImageEdge = 16384;
constexpr int kMinimumImageEdge = 1;
// Clean index sentinel: -1 means "nothing on the stack is the saved state".
constexpr int kNotClean = -1;
// Crop, resize and pad each retain a whole pre-step buffer, so depth alone cannot bound
// memory on a large image. The budget matches the decoded-frame cache ceiling; a step
// bigger than the budget is still kept (history must stay usable), it just means the next
// step prunes everything older.
constexpr qint64 kDefaultHistoryByteBudget = 256LL * 1024 * 1024;
[[nodiscard]] qint64 imageBytes(const QImage& image) noexcept {
    return static_cast<qint64>(image.sizeInBytes());
}
// A live brush preview bumps the provider revision, and the QML Image re-uploads the whole
// buffer on each bump. Throttling keeps a stroke responsive without flooding the GPU.
constexpr int kStrokePreviewIntervalMs = 66;
constexpr int kMaximumBrushWidth = 256;
constexpr int kMaximumMosaicBlock = 64;
// Annotations are a flat list with a hard cap, deliberately not a layer system.
constexpr int kMaximumAnnotations = 64;
constexpr int kMaximumAnnotationWidth = 64;
constexpr int kMaximumTextSize = 512;
constexpr int kAnnotationHitTolerance = 5;

[[nodiscard]] QFont annotationFont(const int pixelSize) {
    QFont font;
    font.setPixelSize(std::clamp(pixelSize, 8, kMaximumTextSize));
    font.setWeight(QFont::DemiBold);
    return font;
}

[[nodiscard]] QString targetSuffix(const QUrl& target) {
    const QString path = target.isLocalFile() ? target.toLocalFile() : target.toString();
    return QFileInfo{path}.suffix().toLower();
}

} // namespace

class ImageEditController::Impl final {
public:
    explicit Impl(ImageEditController& owner) : controller(owner) {}

    // The controller emits through here: a history step changes pixels, annotations and the
    // undo/redo labels all at once, so one place has to notify all of them.
    ImageEditController& controller;

    enum class AnnotationKind : std::uint8_t {
        Arrow,
        Rectangle,
        Text,
    };

    // A flat annotation record. Geometry is in image pixels of the current base image.
    struct Annotation final {
        AnnotationKind kind = AnnotationKind::Arrow;
        QPoint from;
        QPoint to;
        QColor color{Qt::red};
        int width = 2;
        int size = 24;
        QString text;
    };

    // A history step that keeps whole image buffers alive. The byte budget needs to know how
    // much each retained step costs, and only these steps can be large.
    class BufferRetainingCommand : public QUndoCommand {
    public:
        using QUndoCommand::QUndoCommand;

        [[nodiscard]] virtual qint64 retainedBytes() const noexcept = 0;
    };

    // The undo stack, with the one thing QUndoStack cannot do for us: a byte budget.
    //
    // QUndoStack only bounds depth, and its setUndoLimit() is documented to take effect only
    // while the stack is empty, so a stack holding several crops or resamples cannot be made
    // to release the oldest buffers at all. Crop, resize and pad each keep a whole previous
    // image alive, which on a large photo is the difference between a few megabytes and a
    // gigabyte, so depth alone is not a sufficient bound here.
    //
    // Semantics match QUndoStack otherwise: pushing after an undo discards the redo tail,
    // an index marks the current position, and a clean index records the saved state. Pruning
    // always drops the oldest commands first and never the command just pushed, so the
    // newest edit stays undoable even when it alone exceeds the budget.
    class History final {
    public:
        // Called after any change to the current index: a push, an undo or a redo.
        std::function<void()> onChanged;

        void push(std::unique_ptr<QUndoCommand> command) {
            command->redo();
            const qint64 retained = retainedBytesOf(*command);
            if (index_ < std::ssize(commands_)) {
                // Anything that had been undone can no longer be redone.
                commands_.erase(commands_.begin() + index_, commands_.end());
                if (cleanIndex_ > index_) {
                    cleanIndex_ = kNotClean;
                }
            }
            commands_.push_back(std::move(command));
            ++index_;
            retainedBytes_ += retained;
            prune();
            notify();
        }

        void undo() {
            if (!canUndo()) {
                return;
            }
            --index_;
            commands_[static_cast<std::size_t>(index_)]->undo();
            notify();
        }

        void redo() {
            if (!canRedo()) {
                return;
            }
            commands_[static_cast<std::size_t>(index_)]->redo();
            ++index_;
            notify();
        }

        void clear() {
            commands_.clear();
            index_ = 0;
            cleanIndex_ = 0;
            retainedBytes_ = 0;
        }

        void setClean() noexcept {
            cleanIndex_ = index_;
        }

        [[nodiscard]] bool isClean() const noexcept {
            return cleanIndex_ == index_;
        }

        [[nodiscard]] bool canUndo() const noexcept {
            return index_ > 0;
        }

        [[nodiscard]] bool canRedo() const noexcept {
            return index_ < std::ssize(commands_);
        }

        [[nodiscard]] QString undoText() const {
            return canUndo() ? commands_[static_cast<std::size_t>(index_ - 1)]->text() : QString();
        }

        [[nodiscard]] QString redoText() const {
            return canRedo() ? commands_[static_cast<std::size_t>(index_)]->text() : QString();
        }

    public:
        // Requested depth. Lowering it prunes immediately, like QUndoStack would at push time.
        void setDepthLimit(const int limit) {
            depthLimit_ = std::max(1, limit);
            prune();
        }

        [[nodiscard]] int depthLimit() const noexcept {
            return depthLimit_;
        }

        void setByteBudget(const qint64 budget) noexcept {
            byteBudget_ = budget;
        }

        [[nodiscard]] qint64 byteBudget() const noexcept {
            return byteBudget_;
        }

        [[nodiscard]] qint64 retainedBytes() const noexcept {
            return retainedBytes_;
        }

        // Drops the oldest steps until both bounds hold. The newest step always survives: the
        // depth loop stops at one command and the byte loop stops once it is the only one
        // left, which is why a single oversized edit is still undoable.
        void prune() {
            while (count() > depthLimit_) {
                dropOldest();
            }
            while (byteBudget_ > 0 && retainedBytes_ > byteBudget_ && count() > 1) {
                dropOldest();
            }
            // Dropping resets the index; the running total might have been charging commands
            // that the redo-tail erase already removed.
            recountRetainedBytes();
        }

    private:
        [[nodiscard]] int count() const noexcept {
            return static_cast<int>(commands_.size());
        }

        void dropOldest() {
            commands_.erase(commands_.begin());
            --index_;
            if (cleanIndex_ >= 0) {
                --cleanIndex_;
            }
        }

        void recountRetainedBytes() noexcept {
            qint64 total = 0;
            for (const std::unique_ptr<QUndoCommand>& command : commands_) {
                total += retainedBytesOf(*command);
            }
            retainedBytes_ = total;
        }

        [[nodiscard]] static qint64 retainedBytesOf(const QUndoCommand& command) noexcept {
            const auto* buffer = dynamic_cast<const BufferRetainingCommand*>(&command);
            return buffer != nullptr ? buffer->retainedBytes() : 0;
        }

        void notify() {
            if (onChanged) {
                onChanged();
            }
        }

        std::vector<std::unique_ptr<QUndoCommand>> commands_;
        int index_ = 0;
        int cleanIndex_ = 0;
        int depthLimit_ = kDefaultHistoryLimit;
        qint64 byteBudget_ = 0;
        qint64 retainedBytes_ = 0;
    };

    // One undoable crop. A crop that removed pixels can only be undone from the previous
    // buffer, so each step retains the pre-crop image; the bounded history limit keeps
    // that retention predictable (8 steps by default). Annotations are geometry, so the
    // step stores their previous and translated positions instead of any pixels.
    class CropCommand final : public BufferRetainingCommand {
    public:
        CropCommand(Impl* owner, const QRect rect)
            : owner_(owner), rect_(rect), previousImage_(owner->working),
              previous_(owner->annotations) {
            setText(ImageEditController::tr("裁剪"));
            next_ = cropped(previous_, rect_);
        }

        void undo() override {
            owner_->working = previousImage_;
            owner_->annotations = previous_;
            owner_->selectedAnnotation = -1;
        }

        void redo() override {
            owner_->working = owner_->working.copy(rect_);
            owner_->annotations = next_;
            owner_->selectedAnnotation = -1;
        }

        // Bytes this step keeps alive until it leaves the history.
        [[nodiscard]] qint64 retainedBytes() const noexcept override {
            return imageBytes(previousImage_);
        }

    private:
        // Translates annotations into the cropped coordinate system and drops the ones left
        // entirely outside; survivors keep their relative position on the kept pixels.
        [[nodiscard]] static std::vector<Annotation> cropped(const std::vector<Annotation>& source,
                                                             const QRect& rect) {
            const QRect kept{QPoint{0, 0}, rect.size()};
            std::vector<Annotation> result;
            for (const Annotation& annotation : source) {
                Annotation shifted = annotation;
                shifted.from -= rect.topLeft();
                shifted.to -= rect.topLeft();
                if (!kept.intersects(annotationBounds(shifted))) {
                    continue;
                }
                result.push_back(std::move(shifted));
            }
            return result;
        }

        Impl* owner_;
        QRect rect_;
        QImage previousImage_;
        std::vector<Annotation> previous_;
        std::vector<Annotation> next_;
    };

    // One undoable resample ("缩放"): the pixel dimensions change, so the step retains the
    // previous buffer exactly like a crop. Annotations are geometry, so their coordinates
    // and text sizes are scaled instead of pixel-copied; the same rule keeps a later crop
    // from drifting them.
    class ResampleCommand final : public BufferRetainingCommand {
    public:
        ResampleCommand(Impl* owner, const QSize target, const bool smooth)
            : owner_(owner), target_(target), smooth_(smooth), previousImage_(owner->working),
              previous_(owner->annotations) {
            setText(ImageEditController::tr("缩放"));
            next_ = scaled(previous_, previousImage_.size(), target_);
        }

        void undo() override {
            owner_->working = previousImage_;
            owner_->annotations = previous_;
            owner_->selectedAnnotation = -1;
        }

        void redo() override {
            owner_->working =
                previousImage_.scaled(target_,
                                      Qt::IgnoreAspectRatio,
                                      smooth_ ? Qt::SmoothTransformation : Qt::FastTransformation);
            owner_->annotations = next_;
            owner_->selectedAnnotation = -1;
        }

        // Bytes this step keeps alive until it leaves the history.
        [[nodiscard]] qint64 retainedBytes() const noexcept override {
            return imageBytes(previousImage_);
        }

    private:
        // Scales annotation geometry by the same factor as the pixels. Coordinates round to
        // the nearest pixel of the new canvas and text size keeps at least one pixel so a
        // heavily downscaled label stays selectable and deletable.
        [[nodiscard]] static std::vector<Annotation>
        scaled(const std::vector<Annotation>& source, const QSize from, const QSize target) {
            std::vector<Annotation> result;
            if (from.isEmpty() || target.isEmpty()) {
                return result;
            }
            const qreal scaleX = static_cast<qreal>(target.width()) / from.width();
            const qreal scaleY = static_cast<qreal>(target.height()) / from.height();
            result.reserve(source.size());
            for (const Annotation& annotation : source) {
                Annotation scaled = annotation;
                scaled.from = QPoint{static_cast<int>(std::lround(annotation.from.x() * scaleX)),
                                     static_cast<int>(std::lround(annotation.from.y() * scaleY))};
                scaled.to = QPoint{static_cast<int>(std::lround(annotation.to.x() * scaleX)),
                                   static_cast<int>(std::lround(annotation.to.y() * scaleY))};
                scaled.width = std::max(
                    1, static_cast<int>(std::lround(annotation.width * std::min(scaleX, scaleY))));
                const qreal textScale =
                    annotation.kind == AnnotationKind::Text ? scaleY : std::min(scaleX, scaleY);
                scaled.size =
                    std::max(1, static_cast<int>(std::lround(annotation.size * textScale)));
                result.push_back(std::move(scaled));
            }
            return result;
        }

        Impl* owner_;
        QSize target_;
        bool smooth_ = true;
        QImage previousImage_;
        std::vector<Annotation> previous_;
        std::vector<Annotation> next_;
    };

    // One undoable canvas change ("填充"): the working copy is pasted into the centre of a
    // new width x height canvas and the surrounding pixels are filled with a chosen colour.
    // Annotations shift by the centring offset; nothing is dropped, because the image itself
    // is never cut.
    class CanvasCommand final : public BufferRetainingCommand {
    public:
        CanvasCommand(Impl* owner, const QSize target, const QColor& fill)
            : owner_(owner), target_(target), fill_(fill), previousImage_(owner->working),
              previous_(owner->annotations) {
            setText(ImageEditController::tr("填充画布"));
            next_ = shifted(previous_, previousImage_.size(), target_);
        }

        void undo() override {
            owner_->working = previousImage_;
            owner_->annotations = previous_;
            owner_->selectedAnnotation = -1;
        }

        void redo() override {
            QImage canvas{target_, previousImage_.format()};
            canvas.fill(fill_);
            const QPoint offset{(target_.width() - previousImage_.width()) / 2,
                                (target_.height() - previousImage_.height()) / 2};
            QPainter painter(&canvas);
            // Source keeps the pasted pixels exactly as they are, transparent ones included;
            // SourceOver would blend them with the fill and silently change the image.
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.drawImage(offset, previousImage_);
            painter.end();
            owner_->working = std::move(canvas);
            owner_->annotations = next_;
            owner_->selectedAnnotation = -1;
        }

        // Bytes this step keeps alive until it leaves the history.
        [[nodiscard]] qint64 retainedBytes() const noexcept override {
            return imageBytes(previousImage_);
        }

    private:
        [[nodiscard]] static std::vector<Annotation>
        shifted(const std::vector<Annotation>& source, const QSize from, const QSize target) {
            const QPoint offset{(target.width() - from.width()) / 2,
                                (target.height() - from.height()) / 2};
            std::vector<Annotation> result;
            result.reserve(source.size());
            for (const Annotation& annotation : source) {
                Annotation moved = annotation;
                moved.from += offset;
                moved.to += offset;
                result.push_back(std::move(moved));
            }
            return result;
        }

        Impl* owner_;
        QSize target_;
        QColor fill_;
        QImage previousImage_;
        std::vector<Annotation> previous_;
        std::vector<Annotation> next_;
    };

    // One undoable pixel edit that stayed inside a rect (brush stroke, mosaic). Only the
    // dirty rect is retained, so a stroke costs its own footprint instead of a full image.
    class PatchCommand final : public QUndoCommand {
    public:
        PatchCommand(
            Impl* owner, const QRect rect, QImage before, QImage after, const QString& label)
            : owner_(owner), rect_(rect), before_(std::move(before)), after_(std::move(after)) {
            setText(label);
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

    // Annotations are geometry only, so their history steps are tiny. The stack guarantees
    // strict LIFO order, which makes index-based insert/erase safe.
    class AddAnnotationCommand final : public QUndoCommand {
    public:
        AddAnnotationCommand(Impl* owner, Annotation annotation)
            : owner_(owner), annotation_(std::move(annotation)) {
            setText(ImageEditController::tr("标注"));
        }

        void undo() override {
            if (!owner_->annotations.empty()) {
                owner_->annotations.pop_back();
            }
            owner_->selectedAnnotation = -1;
        }

        void redo() override {
            owner_->annotations.push_back(annotation_);
            owner_->selectedAnnotation = static_cast<int>(owner_->annotations.size()) - 1;
        }

    private:
        Impl* owner_;
        Annotation annotation_;
    };

    class MoveAnnotationCommand final : public QUndoCommand {
    public:
        MoveAnnotationCommand(Impl* owner,
                              const int index,
                              const QPoint oldFrom,
                              const QPoint oldTo,
                              const QPoint newFrom,
                              const QPoint newTo)
            : owner_(owner), index_(index), oldFrom_(oldFrom), oldTo_(oldTo), newFrom_(newFrom),
              newTo_(newTo) {
            setText(ImageEditController::tr("移动标注"));
        }

        void undo() override {
            apply(oldFrom_, oldTo_);
        }

        void redo() override {
            apply(newFrom_, newTo_);
        }

    private:
        void apply(const QPoint& from, const QPoint& to) {
            if (index_ < 0 || index_ >= static_cast<int>(owner_->annotations.size())) {
                return;
            }
            owner_->annotations[static_cast<std::size_t>(index_)].from = from;
            owner_->annotations[static_cast<std::size_t>(index_)].to = to;
        }

        Impl* owner_;
        int index_;
        QPoint oldFrom_;
        QPoint oldTo_;
        QPoint newFrom_;
        QPoint newTo_;
    };

    class DeleteAnnotationCommand final : public QUndoCommand {
    public:
        DeleteAnnotationCommand(Impl* owner, const int index, Annotation annotation)
            : owner_(owner), index_(index), annotation_(std::move(annotation)) {
            setText(ImageEditController::tr("删除标注"));
        }

        void undo() override {
            if (index_ < 0 || index_ > static_cast<int>(owner_->annotations.size())) {
                return;
            }
            owner_->annotations.insert(owner_->annotations.begin() + index_, annotation_);
            owner_->selectedAnnotation = index_;
        }

        void redo() override {
            if (index_ < 0 || index_ >= static_cast<int>(owner_->annotations.size())) {
                return;
            }
            owner_->annotations.erase(owner_->annotations.begin() + index_);
            owner_->selectedAnnotation = -1;
        }

    private:
        Impl* owner_;
        int index_;
        Annotation annotation_;
    };

    class ClearAnnotationsCommand final : public QUndoCommand {
    public:
        ClearAnnotationsCommand(Impl* owner, std::vector<Annotation> previous)
            : owner_(owner), previous_(std::move(previous)) {
            setText(ImageEditController::tr("清除标注"));
        }

        void undo() override {
            owner_->annotations = previous_;
            owner_->selectedAnnotation = -1;
        }

        void redo() override {
            owner_->annotations.clear();
            owner_->selectedAnnotation = -1;
        }

    private:
        Impl* owner_;
        std::vector<Annotation> previous_;
    };

    SourceImageProvider sourceProvider;
    QImage original;
    // Pixel edits live in `working`; `composite` is working + annotations, and it is what
    // the provider serves and saveCopy writes. Keeping them apart is what lets annotations
    // stay movable and keeps them out of the difference pipeline.
    QImage working;
    QImage composite;
    std::vector<Annotation> annotations;
    int selectedAnnotation = -1;
    int slot = -1;
    QString label;
    QString sourcePath;
    int revision = 0;
    QString status;
    History history;
    // The depth the user asked for. The byte budget may lower the effective limit, so the
    // requested depth is kept separately and re-applied to every new session.
    int sessionHistoryLimit = kDefaultHistoryLimit;
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
    // Annotation drag gesture: like a brush stroke, the live preview never touches the
    // history; one gesture commits one move step.
    int draggingAnnotation = -1;
    QPoint dragStartPoint;
    QPoint dragOriginalFrom;
    QPoint dragOriginalTo;
    QPoint dragAppliedDelta;
    QElapsedTimer annotationPreviewClock;

    [[nodiscard]] bool canAddAnnotation() const {
        return annotations.size() < static_cast<std::size_t>(kMaximumAnnotations);
    }

    [[nodiscard]] QPoint clampPoint(const QPoint& point) const {
        return QPoint{std::clamp(point.x(), 0, std::max(0, working.width() - 1)),
                      std::clamp(point.y(), 0, std::max(0, working.height() - 1))};
    }

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

    // ---- Annotations ---------------------------------------------------------------

    [[nodiscard]] static QRect annotationBounds(const Annotation& annotation) {
        switch (annotation.kind) {
        case AnnotationKind::Arrow:
        case AnnotationKind::Rectangle: {
            const int margin = (annotation.width / 2) + kAnnotationHitTolerance;
            return QRect{annotation.from, annotation.to}.normalized().adjusted(
                -margin, -margin, margin, margin);
        }
        case AnnotationKind::Text: {
            const QFontMetrics metrics{annotationFont(annotation.size)};
            return metrics.boundingRect(annotation.text).translated(annotation.from);
        }
        }
        return {};
    }

    [[nodiscard]] static qreal
    distanceToSegment(const QPointF& point, const QPointF& start, const QPointF& end) {
        const QPointF segment = end - start;
        const qreal lengthSquared = (segment.x() * segment.x()) + (segment.y() * segment.y());
        if (lengthSquared <= 0.0) {
            return std::hypot(point.x() - start.x(), point.y() - start.y());
        }
        qreal t = ((point.x() - start.x()) * segment.x() + (point.y() - start.y()) * segment.y()) /
                  lengthSquared;
        t = std::clamp(t, 0.0, 1.0);
        const QPointF projection{start.x() + (t * segment.x()), start.y() + (t * segment.y())};
        return std::hypot(point.x() - projection.x(), point.y() - projection.y());
    }

    static void paintArrow(QPainter& painter, const Annotation& annotation) {
        painter.drawLine(annotation.from, annotation.to);
        const QPointF delta = QPointF{annotation.to - annotation.from};
        const qreal length = std::hypot(delta.x(), delta.y());
        if (length < 1.0) {
            return;
        }
        const qreal head = std::max<qreal>(8.0, annotation.width * 3.0);
        const qreal angle = std::atan2(delta.y(), delta.x());
        constexpr qreal kSpread = 0.45;
        for (const qreal sign : {1.0, -1.0}) {
            const QPointF tip{annotation.to.x() - (std::cos(angle + (sign * kSpread)) * head),
                              annotation.to.y() - (std::sin(angle + (sign * kSpread)) * head)};
            painter.drawLine(QPointF{annotation.to}, tip);
        }
    }

    void paintAnnotations(QPainter& painter) const {
        painter.setRenderHint(QPainter::Antialiasing, true);
        for (const Annotation& annotation : annotations) {
            QPen pen{annotation.color};
            pen.setWidth(std::max(1, annotation.width));
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            switch (annotation.kind) {
            case AnnotationKind::Arrow:
                paintArrow(painter, annotation);
                break;
            case AnnotationKind::Rectangle:
                painter.drawRect(QRect{annotation.from, annotation.to}.normalized());
                break;
            case AnnotationKind::Text:
                painter.setFont(annotationFont(annotation.size));
                painter.drawText(annotation.from, annotation.text);
                break;
            }
        }
    }

    // Pushes one history step. All steps go through here so the byte budget and the
    // change notification stay in one place: the History charges whatever the command
    // retains and prunes the oldest buffers when that exceeds the budget.
    void pushStep(std::unique_ptr<QUndoCommand> command) {
        history.push(std::move(command));
    }

    // Everything the history changes has to make the composite, the undo/redo labels and the
    // dirty flag agree again. One hook, so pruning inside the history cannot leave the UI
    // showing a step that is gone.
    void extendHistory() {
        rebuildComposite();
        ++revision;
        emit controller.stateChanged();
        emit controller.imageChanged();
        emit controller.annotationsChanged();
        emit controller.historyChanged();
    }

    // Rebuilds the displayed/saved image from the pixel edits plus the annotation list. A
    // list without annotations shares the working buffer instead of copying it.
    void rebuildComposite() {
        if (working.isNull()) {
            composite = QImage();
            return;
        }
        if (annotations.empty()) {
            composite = working;
            return;
        }
        composite = working.copy();
        QPainter painter(&composite);
        paintAnnotations(painter);
    }

    [[nodiscard]] int annotationHitTest(const QPoint& point) const {
        for (int index = static_cast<int>(annotations.size()) - 1; index >= 0; --index) {
            const Annotation& annotation = annotations[static_cast<std::size_t>(index)];
            switch (annotation.kind) {
            case AnnotationKind::Arrow:
                if (distanceToSegment(
                        QPointF{point}, QPointF{annotation.from}, QPointF{annotation.to}) <=
                    (annotation.width / 2.0) + kAnnotationHitTolerance) {
                    return index;
                }
                break;
            case AnnotationKind::Rectangle: {
                const QRect bounds = annotationBounds(annotation);
                if (bounds.contains(point)) {
                    return index;
                }
                break;
            }
            case AnnotationKind::Text:
                if (annotationBounds(annotation).contains(point)) {
                    return index;
                }
                break;
            }
        }
        return -1;
    }

    void clearAnnotationState() {
        annotations.clear();
        selectedAnnotation = -1;
        draggingAnnotation = -1;
        dragAppliedDelta = QPoint{0, 0};
    }
};

ImageEditController::ImageEditController(QObject* parent) : QObject(parent), impl_(nullptr) {
    impl_ = std::make_unique<Impl>(*this);
    impl_->history.setDepthLimit(kDefaultHistoryLimit);
    impl_->history.setByteBudget(kDefaultHistoryByteBudget);
    // Every history step may have changed pixels, annotations, or both; the composite is
    // what the provider serves, so rebuild it before anything can read it. The hook also
    // fires from inside a push, so it must not push anything itself.
    impl_->history.onChanged = [this] { impl_->extendHistory(); };
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

int ImageEditController::canvasWidth() const noexcept {
    return impl_->working.width();
}

int ImageEditController::canvasHeight() const noexcept {
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

int ImageEditController::annotationCount() const noexcept {
    return static_cast<int>(impl_->annotations.size());
}

int ImageEditController::selectedAnnotation() const noexcept {
    return impl_->selectedAnnotation;
}

QString ImageEditController::lastStatus() const {
    return impl_->status;
}

void ImageEditController::setHistoryLimit(const int steps) {
    const int bounded = std::clamp(steps, 1, kMaximumHistoryLimit);
    impl_->sessionHistoryLimit = bounded;
    if (impl_->history.depthLimit() == bounded) {
        return;
    }
    impl_->history.setDepthLimit(bounded);
}

int ImageEditController::historyLimit() const noexcept {
    return impl_->history.depthLimit();
}

void ImageEditController::setHistoryByteBudget(const qint64 bytes) {
    impl_->history.setByteBudget(bytes <= 0 ? 0 : bytes);
    impl_->history.prune();
}

qint64 ImageEditController::historyByteBudget() const noexcept {
    return impl_->history.byteBudget();
}

int ImageEditController::maximumImageEdge() noexcept {
    return kMaximumImageEdge;
}

int ImageEditController::minimumImageEdge() noexcept {
    return kMinimumImageEdge;
}

int ImageEditController::maximumEdge() const noexcept {
    return kMaximumImageEdge;
}

int ImageEditController::minimumEdge() const noexcept {
    return kMinimumImageEdge;
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
    impl_->clearAnnotationState();
    impl_->slot = slot;
    impl_->label = label;
    impl_->sourcePath = sourceUrl.isLocalFile() ? sourceUrl.toLocalFile() : sourceUrl.toString();
    impl_->history.clear();
    // A fresh session starts from the user's configured depth, not from whatever the byte
    // budget shrank it to during the previous image.
    impl_->history.setDepthLimit(impl_->sessionHistoryLimit);
    impl_->history.setClean();
    impl_->rebuildComposite();
    ++impl_->revision;
    emit stateChanged();
    emit imageChanged();
    emit annotationsChanged();
    emit historyChanged();
    setStatus(tr("编辑模式：原图保持不变，所有修改另存副本。"));
    return true;
}

void ImageEditController::endSession() {
    if (impl_->slot < 0 && impl_->working.isNull()) {
        return;
    }
    impl_->history.clear();
    impl_->history.setDepthLimit(impl_->sessionHistoryLimit);
    impl_->original = QImage();
    impl_->working = QImage();
    impl_->composite = QImage();
    impl_->clearAnnotationState();
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
    emit annotationsChanged();
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
    impl_->pushStep(std::make_unique<Impl::CropCommand>(impl_.get(), bounded));
    setStatus(tr("已裁剪为 %1×%2").arg(bounded.width()).arg(bounded.height()));
    return true;
}

bool ImageEditController::resizeImage(const int width, const int height, const bool smooth) {
    if (!active()) {
        setStatus(tr("没有正在编辑的图片。"));
        return false;
    }
    const int targetWidth = std::clamp(width, kMinimumImageEdge, kMaximumImageEdge);
    const int targetHeight = std::clamp(height, kMinimumImageEdge, kMaximumImageEdge);
    if (targetWidth == impl_->working.width() && targetHeight == impl_->working.height()) {
        setStatus(
            width != targetWidth || height != targetHeight
                ? tr("尺寸已钳制到 %1×%2，与当前图片相同。").arg(targetWidth).arg(targetHeight)
                : tr("尺寸与当前图片相同。"));
        return false;
    }
    impl_->pushStep(std::make_unique<Impl::ResampleCommand>(
        impl_.get(), QSize{targetWidth, targetHeight}, smooth));
    setStatus(tr("已缩放为 %1×%2").arg(targetWidth).arg(targetHeight));
    return true;
}

bool ImageEditController::padToCanvas(const int width, const int height, const QColor& fillColor) {
    if (!active()) {
        setStatus(tr("没有正在编辑的图片。"));
        return false;
    }
    if (!fillColor.isValid()) {
        setStatus(tr("填充颜色无效。"));
        return false;
    }
    // Padding means "put the image in the middle of a bigger canvas"; a smaller canvas would
    // silently cut pixels, which is what crop is for.
    if (width < impl_->working.width() || height < impl_->working.height()) {
        setStatus(tr("画布不能小于当前图片（%1×%2）。")
                      .arg(impl_->working.width())
                      .arg(impl_->working.height()));
        return false;
    }
    const int targetWidth = std::clamp(width, kMinimumImageEdge, kMaximumImageEdge);
    const int targetHeight = std::clamp(height, kMinimumImageEdge, kMaximumImageEdge);
    if (targetWidth == impl_->working.width() && targetHeight == impl_->working.height()) {
        setStatus(tr("画布与当前图片相同。"));
        return false;
    }
    if (fillColor.alpha() < 255 && !impl_->working.hasAlphaChannel()) {
        setStatus(tr("当前图片没有透明通道，无法用透明色填充；请先转换为带透明通道的图片，"
                     "或选择不透明颜色。"));
        return false;
    }
    impl_->pushStep(std::make_unique<Impl::CanvasCommand>(
        impl_.get(), QSize{targetWidth, targetHeight}, fillColor));
    setStatus(tr("已填充为 %1×%2 画布，原图居中。").arg(targetWidth).arg(targetHeight));
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
        impl_->rebuildComposite();
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
        impl_->pushStep(std::make_unique<Impl::PatchCommand>(impl_.get(),
                                                             dirty,
                                                             impl_->strokeBase.copy(dirty),
                                                             impl_->working.copy(dirty),
                                                             tr("画笔")));
    } else {
        impl_->rebuildComposite();
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
    impl_->pushStep(std::make_unique<Impl::PatchCommand>(
        impl_.get(), bounded, impl_->working.copy(bounded), std::move(after), tr("马赛克")));
    setStatus(tr("已对 %1×%2 区域应用马赛克（块 %3 像素）")
                  .arg(bounded.width())
                  .arg(bounded.height())
                  .arg(block));
    return true;
}

bool ImageEditController::fillImageRect(
    const int x, const int y, const int width, const int height, const QColor& color) {
    if (!active()) {
        setStatus(tr("没有正在编辑的图片。"));
        return false;
    }
    if (!color.isValid()) {
        setStatus(tr("填充颜色无效。"));
        return false;
    }
    const QRect bounded = QRect{x, y, width, height}.intersected(
        QRect{0, 0, impl_->working.width(), impl_->working.height()});
    if (bounded.isEmpty()) {
        setStatus(tr("填充区域为空或超出图片范围。"));
        return false;
    }
    // An opaque colour block is the review's preferred way to mask sensitive content.
    QImage after = impl_->working.copy(bounded);
    after.fill(color);
    impl_->pushStep(std::make_unique<Impl::PatchCommand>(
        impl_.get(), bounded, impl_->working.copy(bounded), std::move(after), tr("填充")));
    setStatus(tr("已填充 %1×%2 区域").arg(bounded.width()).arg(bounded.height()));
    return true;
}

bool ImageEditController::clearImageRect(const int x,
                                         const int y,
                                         const int width,
                                         const int height) {
    if (!active()) {
        setStatus(tr("没有正在编辑的图片。"));
        return false;
    }
    const QRect bounded = QRect{x, y, width, height}.intersected(
        QRect{0, 0, impl_->working.width(), impl_->working.height()});
    if (bounded.isEmpty()) {
        setStatus(tr("清除区域为空或超出图片范围。"));
        return false;
    }
    QImage after = impl_->working.copy(bounded);
    after.fill(Qt::transparent);
    impl_->pushStep(std::make_unique<Impl::PatchCommand>(
        impl_.get(), bounded, impl_->working.copy(bounded), std::move(after), tr("清除")));
    setStatus(tr("已把 %1×%2 区域清除为透明").arg(bounded.width()).arg(bounded.height()));
    return true;
}

bool ImageEditController::addArrow(const int fromX,
                                   const int fromY,
                                   const int toX,
                                   const int toY,
                                   const QColor& color,
                                   const int width) {
    if (!active()) {
        setStatus(tr("没有正在编辑的图片。"));
        return false;
    }
    if (!color.isValid()) {
        setStatus(tr("标注颜色无效。"));
        return false;
    }
    if (!impl_->canAddAnnotation()) {
        setStatus(tr("标注数量已达上限（%1）。").arg(kMaximumAnnotations));
        return false;
    }
    const QPoint from = impl_->clampPoint(QPoint{fromX, fromY});
    const QPoint to = impl_->clampPoint(QPoint{toX, toY});
    if (std::hypot(static_cast<qreal>(to.x() - from.x()), static_cast<qreal>(to.y() - from.y())) <
        2.0) {
        setStatus(tr("箭头太短，请拖出更长的线段。"));
        return false;
    }
    Impl::Annotation annotation;
    annotation.kind = Impl::AnnotationKind::Arrow;
    annotation.from = from;
    annotation.to = to;
    annotation.color = color;
    annotation.width = std::clamp(width, 1, kMaximumAnnotationWidth);
    impl_->pushStep(
        std::make_unique<Impl::AddAnnotationCommand>(impl_.get(), std::move(annotation)));
    setStatus(tr("已添加箭头标注。"));
    return true;
}

bool ImageEditController::addRectangle(const int fromX,
                                       const int fromY,
                                       const int toX,
                                       const int toY,
                                       const QColor& color,
                                       const int width) {
    if (!active()) {
        setStatus(tr("没有正在编辑的图片。"));
        return false;
    }
    if (!color.isValid()) {
        setStatus(tr("标注颜色无效。"));
        return false;
    }
    if (!impl_->canAddAnnotation()) {
        setStatus(tr("标注数量已达上限（%1）。").arg(kMaximumAnnotations));
        return false;
    }
    const QRect rect =
        QRect{impl_->clampPoint(QPoint{fromX, fromY}), impl_->clampPoint(QPoint{toX, toY})}
            .normalized();
    if (rect.width() < 2 || rect.height() < 2) {
        setStatus(tr("矩形太小，请拖出更大的区域。"));
        return false;
    }
    Impl::Annotation annotation;
    annotation.kind = Impl::AnnotationKind::Rectangle;
    annotation.from = rect.topLeft();
    annotation.to = rect.bottomRight();
    annotation.color = color;
    annotation.width = std::clamp(width, 1, kMaximumAnnotationWidth);
    impl_->pushStep(
        std::make_unique<Impl::AddAnnotationCommand>(impl_.get(), std::move(annotation)));
    setStatus(tr("已添加矩形标注。"));
    return true;
}

bool ImageEditController::addText(
    const int x, const int y, const QString& text, const QColor& color, const int pixelSize) {
    if (!active()) {
        setStatus(tr("没有正在编辑的图片。"));
        return false;
    }
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        setStatus(tr("请先输入标注文字。"));
        return false;
    }
    if (!color.isValid()) {
        setStatus(tr("标注颜色无效。"));
        return false;
    }
    if (!impl_->canAddAnnotation()) {
        setStatus(tr("标注数量已达上限（%1）。").arg(kMaximumAnnotations));
        return false;
    }
    Impl::Annotation annotation;
    annotation.kind = Impl::AnnotationKind::Text;
    annotation.from = impl_->clampPoint(QPoint{x, y});
    annotation.to = annotation.from;
    annotation.color = color;
    annotation.size = std::clamp(pixelSize, 8, kMaximumTextSize);
    annotation.width = std::max(1, annotation.size / 12);
    annotation.text = trimmed;
    impl_->pushStep(
        std::make_unique<Impl::AddAnnotationCommand>(impl_.get(), std::move(annotation)));
    setStatus(tr("已添加文字标注。"));
    return true;
}

bool ImageEditController::selectAnnotationAt(const int x, const int y) {
    if (!active()) {
        setStatus(tr("没有正在编辑的图片。"));
        return false;
    }
    const int index = impl_->annotationHitTest(impl_->clampPoint(QPoint{x, y}));
    if (index != impl_->selectedAnnotation) {
        impl_->selectedAnnotation = index;
        emit annotationsChanged();
    }
    if (index < 0) {
        setStatus(tr("该位置没有标注。"));
        return false;
    }
    setStatus(tr("已选中标注 %1（可拖动或删除）。").arg(index + 1));
    return true;
}

bool ImageEditController::beginAnnotationDrag(const int x, const int y) {
    if (!active()) {
        setStatus(tr("没有正在编辑的图片。"));
        return false;
    }
    const int index = impl_->annotationHitTest(impl_->clampPoint(QPoint{x, y}));
    if (index < 0) {
        if (impl_->selectedAnnotation != -1) {
            impl_->selectedAnnotation = -1;
            emit annotationsChanged();
        }
        setStatus(tr("该位置没有可拖动的标注。"));
        return false;
    }
    impl_->draggingAnnotation = index;
    impl_->selectedAnnotation = index;
    impl_->dragStartPoint = QPoint{x, y};
    impl_->dragAppliedDelta = QPoint{0, 0};
    impl_->dragOriginalFrom = impl_->annotations[static_cast<std::size_t>(index)].from;
    impl_->dragOriginalTo = impl_->annotations[static_cast<std::size_t>(index)].to;
    impl_->annotationPreviewClock.start();
    emit annotationsChanged();
    return true;
}

bool ImageEditController::dragAnnotationTo(const int x, const int y) {
    const int index = impl_->draggingAnnotation;
    if (index < 0 || index >= annotationCount()) {
        return false;
    }
    // The bounds must come from the gesture's starting geometry, not from the live position,
    // or clamping would fight the drag as it moves.
    Impl::Annotation original = impl_->annotations[static_cast<std::size_t>(index)];
    original.from = impl_->dragOriginalFrom;
    original.to = impl_->dragOriginalTo;
    const QRect bounds = Impl::annotationBounds(original);
    const QRect imageRect{0, 0, impl_->working.width(), impl_->working.height()};
    int deltaX = x - impl_->dragStartPoint.x();
    int deltaY = y - impl_->dragStartPoint.y();
    // Keep the whole annotation inside the picture so a drag can never push it out of reach.
    deltaX =
        std::clamp(deltaX, imageRect.left() - bounds.left(), imageRect.right() - bounds.right());
    deltaY =
        std::clamp(deltaY, imageRect.top() - bounds.top(), imageRect.bottom() - bounds.bottom());
    impl_->annotations[static_cast<std::size_t>(index)].from =
        impl_->dragOriginalFrom + QPoint{deltaX, deltaY};
    impl_->annotations[static_cast<std::size_t>(index)].to =
        impl_->dragOriginalTo + QPoint{deltaX, deltaY};
    impl_->dragAppliedDelta = QPoint{deltaX, deltaY};
    if (!impl_->annotationPreviewClock.isValid() ||
        impl_->annotationPreviewClock.elapsed() >= kStrokePreviewIntervalMs) {
        impl_->annotationPreviewClock.restart();
        impl_->rebuildComposite();
        ++impl_->revision;
        emit imageChanged();
    }
    return true;
}

bool ImageEditController::endAnnotationDrag() {
    const int index = impl_->draggingAnnotation;
    impl_->draggingAnnotation = -1;
    if (index < 0 || index >= annotationCount()) {
        return false;
    }
    if (impl_->dragAppliedDelta.isNull()) {
        return false;
    }
    // One gesture is one history step: the live preview above never touched the stack.
    impl_->annotations[static_cast<std::size_t>(index)].from = impl_->dragOriginalFrom;
    impl_->annotations[static_cast<std::size_t>(index)].to = impl_->dragOriginalTo;
    impl_->pushStep(std::make_unique<Impl::MoveAnnotationCommand>(
        impl_.get(),
        index,
        impl_->dragOriginalFrom,
        impl_->dragOriginalTo,
        impl_->dragOriginalFrom + impl_->dragAppliedDelta,
        impl_->dragOriginalTo + impl_->dragAppliedDelta));
    impl_->dragAppliedDelta = QPoint{0, 0};
    setStatus(tr("已移动标注。"));
    return true;
}

bool ImageEditController::moveSelectedAnnotation(const int deltaX, const int deltaY) {
    if (!active()) {
        setStatus(tr("没有正在编辑的图片。"));
        return false;
    }
    const int index = impl_->selectedAnnotation;
    if (index < 0 || index >= annotationCount()) {
        setStatus(tr("请先选中一个标注。"));
        return false;
    }
    const Impl::Annotation& annotation = impl_->annotations[static_cast<std::size_t>(index)];
    const QRect bounds = Impl::annotationBounds(annotation);
    const QRect imageRect{0, 0, impl_->working.width(), impl_->working.height()};
    const int clampedX =
        std::clamp(deltaX, imageRect.left() - bounds.left(), imageRect.right() - bounds.right());
    const int clampedY =
        std::clamp(deltaY, imageRect.top() - bounds.top(), imageRect.bottom() - bounds.bottom());
    if (clampedX == 0 && clampedY == 0) {
        setStatus(tr("标注已在画面边界内。"));
        return false;
    }
    impl_->pushStep(
        std::make_unique<Impl::MoveAnnotationCommand>(impl_.get(),
                                                      index,
                                                      annotation.from,
                                                      annotation.to,
                                                      annotation.from + QPoint{clampedX, clampedY},
                                                      annotation.to + QPoint{clampedX, clampedY}));
    setStatus(tr("已移动标注。"));
    return true;
}

bool ImageEditController::deleteSelectedAnnotation() {
    if (!active()) {
        setStatus(tr("没有正在编辑的图片。"));
        return false;
    }
    const int index = impl_->selectedAnnotation;
    if (index < 0 || index >= annotationCount()) {
        setStatus(tr("请先选中一个标注。"));
        return false;
    }
    impl_->pushStep(std::make_unique<Impl::DeleteAnnotationCommand>(
        impl_.get(), index, impl_->annotations[static_cast<std::size_t>(index)]));
    setStatus(tr("已删除标注。"));
    return true;
}

void ImageEditController::clearAnnotations() {
    if (!active() || impl_->annotations.empty()) {
        return;
    }
    impl_->pushStep(
        std::make_unique<Impl::ClearAnnotationsCommand>(impl_.get(), impl_->annotations));
    setStatus(tr("已清除全部标注。"));
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
    QImage output = editedImage();
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
    // The displayed/saved image is the pixel edits plus the annotation list; annotations
    // never touch the comparison buffers.
    return impl_->composite.isNull() ? impl_->working : impl_->composite;
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
