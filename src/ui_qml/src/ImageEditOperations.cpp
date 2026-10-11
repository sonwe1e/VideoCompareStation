#include "dvs/ui/ImageEditOperations.h"

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QRect>
#include <QTransform>

#include <algorithm>
#include <type_traits>

namespace dvs::ui {
namespace {

// Blits a patch back with Source composition so alpha is replaced exactly rather than
// blended; the controller's undo patches use the same rule and the two must stay
// byte-compatible.
void blitSource(QImage& target, const QRect& rect, const QImage& patch) {
    QPainter painter(&target);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(rect.topLeft(), patch);
}

} // namespace

QImage cropImageRect(const QImage& source, const QRect& rect) {
    if (source.isNull()) {
        return {};
    }
    const QRect bounded = rect.intersected(source.rect());
    if (bounded.isEmpty()) {
        return {};
    }
    return source.copy(bounded);
}

QImage applyGeometryTransform(const QImage& source, const GeometryTransform transform) {
    if (source.isNull()) {
        return {};
    }
    switch (transform) {
    case GeometryTransform::MirrorHorizontal:
        return source.flipped(Qt::Horizontal);
    case GeometryTransform::FlipVertical:
        return source.flipped(Qt::Vertical);
    case GeometryTransform::RotateClockwise:
        return source.transformed(QTransform{}.rotate(90), Qt::FastTransformation);
    case GeometryTransform::RotateCounterclockwise:
        return source.transformed(QTransform{}.rotate(-90), Qt::FastTransformation);
    case GeometryTransform::RotateHalfTurn:
        return source.flipped(Qt::Horizontal | Qt::Vertical);
    }
    return source.copy();
}

QImage fillImageRect(const QImage& source, const QRect& rect, const QColor& color) {
    if (source.isNull()) {
        return {};
    }
    const QRect bounded = rect.intersected(source.rect());
    if (bounded.isEmpty()) {
        return source.copy();
    }
    // The patch is filled exactly like the controller filled its undo patch, then blitted
    // with Source composition, so the result matches the pre-extraction behaviour byte
    // for byte — inside the rect the pixels are exactly `color`, outside they are
    // untouched, and formats without an alpha channel keep their layout.
    QImage patch = source.copy(bounded);
    patch.fill(color);
    QImage result = source.copy();
    blitSource(result, bounded, patch);
    return result;
}

QImage clearImageRect(const QImage& source, const QRect& rect) {
    if (source.isNull()) {
        return {};
    }
    const QRect bounded = rect.intersected(source.rect());
    if (bounded.isEmpty()) {
        return source.copy();
    }
    QImage patch = source.copy(bounded);
    patch.fill(Qt::transparent);
    QImage result = source.copy();
    blitSource(result, bounded, patch);
    return result;
}

int effectiveMosaicBlockSize(const QRect& region, const int blockSize) noexcept {
    return std::clamp(blockSize, 2, std::max(2, std::min(region.width(), region.height())));
}

QImage mosaicImageRect(const QImage& source, const QRect& rect, const int blockSize) {
    if (source.isNull()) {
        return {};
    }
    const QRect bounded = rect.intersected(source.rect());
    if (bounded.isEmpty()) {
        return source.copy();
    }
    const int block = effectiveMosaicBlockSize(bounded, blockSize);
    // Nearest-neighbour down/up scaling is the deterministic pixelate: each block keeps
    // one sampled colour, and the blocks stay opaque so the region really is obscured.
    const QImage pixelated =
        source.copy(bounded)
            .scaled(std::max(1, bounded.width() / block),
                    std::max(1, bounded.height() / block),
                    Qt::IgnoreAspectRatio,
                    Qt::FastTransformation)
            .scaled(bounded.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
    QImage result = source.copy();
    blitSource(result, bounded, pixelated);
    return result;
}

QImage applyEditOperation(const QImage& source, const EditOperation& operation) {
    // std::visit with an explicitly deduced auto so the overload set below participates
    // in ADL-free, warning-free dispatch.
    return std::visit(
        [&source](const auto& op) {
            using Op = std::decay_t<decltype(op)>;
            if constexpr (std::is_same_v<Op, CropOperation>) {
                return cropImageRect(source, op.rect);
            } else if constexpr (std::is_same_v<Op, GeometryTransformOperation>) {
                return applyGeometryTransform(source, op.transform);
            } else if constexpr (std::is_same_v<Op, FillRectOperation>) {
                return fillImageRect(source, op.rect, op.color);
            } else if constexpr (std::is_same_v<Op, ClearRectOperation>) {
                return clearImageRect(source, op.rect);
            } else {
                static_assert(std::is_same_v<Op, MosaicOperation>,
                              "unhandled EditOperation alternative");
                return mosaicImageRect(source, op.rect, op.blockSize);
            }
        },
        operation);
}

QImage applyEditRecipe(const QImage& source, const EditRecipe& recipe) {
    QImage current = source.copy();
    for (const EditOperation& operation : recipe.steps) {
        current = applyEditOperation(current, operation);
    }
    return current;
}

} // namespace dvs::ui
