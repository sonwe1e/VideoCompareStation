#pragma once

#include <QColor>
#include <QImage>
#include <QRect>

#include <variant>
#include <vector>

namespace dvs::ui {

// Pure, stateless image edit operations (roadmap S01 step 1: "extract editing into
// pure operation + parameter"). Every function here is deterministic: the same input
// buffer and the same parameters always produce the same output, with no reads of the
// environment, wall-clock time, or randomness. The input buffer is never modified —
// each operation returns a brand-new QImage and the caller decides where the result is
// written (the review original stays immutable by construction).
//
// The controller (ImageEditController) delegates its geometry/pixel work here so the
// interactive preview and a future batch/export executor share one implementation.
// Undo history, selection state, annotations and other UI state are deliberately NOT
// modelled here; those stay owned by the controller.
//
// Coordinates and rectangles are image pixels, identical to the controller's contract.

// Exact geometry transforms, mirroring ImageEditController::ImageTransform without
// pulling its Qt object registration into the pure module.
enum class GeometryTransform {
    MirrorHorizontal,
    FlipVertical,
    RotateClockwise,
    RotateCounterclockwise,
    RotateHalfTurn,
};

// Crops to the intersection of rect and the image; an empty intersection yields an
// empty image. Equivalent to QImage::copy on an already-bounded rect, but expressed as
// a pure function with explicit clamping so recipe replay cannot depend on caller-side
// validation.
[[nodiscard]] QImage cropImageRect(const QImage& source, const QRect& rect);

// Exact axis mirror / flip / quarter-turn rotation with format and alpha preserved.
// All variants return a new buffer; a null input yields a null output.
[[nodiscard]] QImage applyGeometryTransform(const QImage& source, GeometryTransform transform);

// Fills the intersection of rect and the image with an opaque colour. The fill is a
// Source-composited patch, so pixels inside rect become exactly `color` and everything
// outside is byte-identical to the input.
[[nodiscard]] QImage fillImageRect(const QImage& source, const QRect& rect, const QColor& color);

// Clears the intersection of rect and the image to fully transparent pixels; the exact
// pixel equivalent of fillImageRect(..., Qt::transparent), kept as its own operation
// because "erase" is a distinct tool with a distinct undo label.
[[nodiscard]] QImage clearImageRect(const QImage& source, const QRect& rect);

// Pixelates the intersection of rect and the image into opaque blocks. blockSize is
// clamped to [2, min(region edge)] exactly like the controller did inline, so the same
// call is valid with or without caller-side normalization. Nearest-neighbour
// down/up-scaling keeps every block one sampled colour and keeps the region opaque.
[[nodiscard]] QImage mosaicImageRect(const QImage& source, const QRect& rect, int blockSize);

// The effective block size mosaicImageRect uses for the given region, exposed so the
// controller status line and any future recipe executor agree on one clamping rule.
[[nodiscard]] int effectiveMosaicBlockSize(const QRect& region, int blockSize) noexcept;

// ---- Recipe --------------------------------------------------------------------------

// A recipe is an ordered list of the pure operations above plus their parameters: pure
// data with no behaviour, ready for serialization in a later work order. Interactive
// tools (brush, annotation drag, smart cutout) are intentionally absent — they are not
// deterministic pure functions yet.

struct CropOperation final {
    QRect rect;
};

struct GeometryTransformOperation final {
    GeometryTransform transform = GeometryTransform::MirrorHorizontal;
};

struct FillRectOperation final {
    QRect rect;
    QColor color{Qt::red};
};

struct ClearRectOperation final {
    QRect rect;
};

struct MosaicOperation final {
    QRect rect;
    int blockSize = 8;
};

using EditOperation = std::variant<CropOperation,
                                   GeometryTransformOperation,
                                   FillRectOperation,
                                   ClearRectOperation,
                                   MosaicOperation>;

struct EditRecipe final {
    std::vector<EditOperation> steps;
};

// Applies one operation. This is the single seam the controller preview path and the
// future export path both go through.
[[nodiscard]] QImage applyEditOperation(const QImage& source, const EditOperation& operation);

// Applies the recipe's steps in order. An empty recipe returns a copy of the source;
// every step is applied through applyEditOperation, so a recipe and its manually folded
// steps can never diverge.
[[nodiscard]] QImage applyEditRecipe(const QImage& source, const EditRecipe& recipe);

} // namespace dvs::ui
