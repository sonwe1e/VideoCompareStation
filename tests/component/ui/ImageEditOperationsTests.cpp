#include "dvs/ui/ImageEditController.h"
#include "dvs/ui/ImageEditOperations.h"

#include <QColor>
#include <QImage>
#include <QUrl>

#include <cstring>
#include <gtest/gtest.h>

namespace {

using dvs::ui::applyEditOperation;
using dvs::ui::applyEditRecipe;
using dvs::ui::applyGeometryTransform;
using dvs::ui::clearImageRect;
using dvs::ui::ClearRectOperation;
using dvs::ui::cropImageRect;
using dvs::ui::CropOperation;
using dvs::ui::EditOperation;
using dvs::ui::EditRecipe;
using dvs::ui::effectiveMosaicBlockSize;
using dvs::ui::fillImageRect;
using dvs::ui::FillRectOperation;
using dvs::ui::GeometryTransform;
using dvs::ui::GeometryTransformOperation;
using dvs::ui::ImageEditController;
using dvs::ui::mosaicImageRect;
using dvs::ui::MosaicOperation;

// Byte-level identity, stronger than QImage::operator== for the S01 "preview and export
// share one executor" invariant: the same size, format and raw buffer bytes.
[[nodiscard]] ::testing::AssertionResult
byteIdentical(const QImage& actual, const QImage& expected, const char* what) {
    if (actual.size() != expected.size()) {
        return ::testing::AssertionFailure()
               << what << ": size " << actual.width() << "x" << actual.height()
               << " != " << expected.width() << "x" << expected.height();
    }
    if (actual.format() != expected.format()) {
        return ::testing::AssertionFailure()
               << what << ": format " << static_cast<int>(actual.format())
               << " != " << static_cast<int>(expected.format());
    }
    if (actual.isNull()) {
        return ::testing::AssertionSuccess();
    }
    const qint64 bytes = actual.sizeInBytes();
    if (std::memcmp(actual.constBits(), expected.constBits(), static_cast<std::size_t>(bytes)) !=
        0) {
        for (int y = 0; y < actual.height(); ++y) {
            for (int x = 0; x < actual.width(); ++x) {
                if (actual.pixelColor(x, y) != expected.pixelColor(x, y)) {
                    return ::testing::AssertionFailure()
                           << what << ": first differing pixel at (" << x << ", " << y
                           << "): " << actual.pixelColor(x, y).rgba64().toArgb32()
                           << " != " << expected.pixelColor(x, y).rgba64().toArgb32();
                }
            }
        }
        return ::testing::AssertionFailure() << what << ": padding bytes differ";
    }
    return ::testing::AssertionSuccess();
}

// Distinct per-pixel colours make every geometry operation observable at pixel level.
[[nodiscard]] QImage cornerImage(const int width, const int height) {
    QImage image(width, height, QImage::Format_ARGB32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.setPixelColor(x, y, QColor(x * 3 % 256, y * 5 % 256, 40, 255));
        }
    }
    return image;
}

// A translucent background proves fill/clear replace pixels (Source composition) rather
// than blending over them.
[[nodiscard]] QImage translucentImage(const int width, const int height) {
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.setPixelColor(x, y, QColor(200, 30, 60, 128));
        }
    }
    return image;
}

// Left half black, right half white: a mosaic must not average the halves into grey.
[[nodiscard]] QImage splitImage(const int width, const int height) {
    QImage image(width, height, QImage::Format_ARGB32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.setPixelColor(x, y, x < width / 2 ? QColor(Qt::black) : QColor(Qt::white));
        }
    }
    return image;
}

[[nodiscard]] QImage gradientImage(const int width, const int height) {
    QImage image(width, height, QImage::Format_ARGB32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int value = (x * 255) / std::max(1, width - 1);
            image.setPixelColor(x, y, QColor(value, value, value));
        }
    }
    return image;
}

[[nodiscard]] QImage deepCopyOf(const QImage& image) {
    return image.copy();
}

void setSource(ImageEditController& controller, const QImage& image) {
    controller.setSourceImageProvider([image](const int slot) {
        static_cast<void>(slot);
        return image;
    });
}

} // namespace

TEST(ImageEditOperationsTests, CropReturnsABoundedNewBufferAndLeavesTheInputAlone) {
    const QImage source = cornerImage(20, 16);
    const QImage before = deepCopyOf(source);

    const QImage cropped = cropImageRect(source, QRect{5, 4, 10, 8});
    ASSERT_FALSE(cropped.isNull());
    EXPECT_EQ(cropped.size(), QSize(10, 8));
    EXPECT_EQ(cropped.format(), source.format());
    EXPECT_EQ(cropped.pixelColor(0, 0), source.pixelColor(5, 4));
    EXPECT_EQ(cropped.pixelColor(9, 7), source.pixelColor(14, 11));
    EXPECT_EQ(cropped.pixelColor(4, 3), source.pixelColor(9, 7));

    // The input is immutable and the result is a separate buffer.
    EXPECT_TRUE(byteIdentical(source, before, "source after crop"));
    EXPECT_NE(cropped.cacheKey(), source.cacheKey());

    // Out-of-bounds rects are clamped to the intersection...
    const QImage clamped = cropImageRect(source, QRect{15, 12, 50, 50});
    EXPECT_EQ(clamped.size(), QSize(5, 4));
    EXPECT_EQ(clamped.pixelColor(0, 0), source.pixelColor(15, 12));
    // ...and a non-intersecting or degenerate rect yields an empty image, never a crash.
    EXPECT_TRUE(cropImageRect(source, QRect{30, 30, 4, 4}).isNull());
    EXPECT_TRUE(cropImageRect(source, QRect{5, 5, 0, 4}).isNull());
    EXPECT_TRUE(cropImageRect(QImage(), QRect{0, 0, 4, 4}).isNull());
}

TEST(ImageEditOperationsTests, GeometryTransformsPermutePixelsExactly) {
    QImage source = cornerImage(12, 10);
    source.setPixelColor(3, 2, QColor(7, 31, 99, 123));
    const QImage before = deepCopyOf(source);
    const struct {
        GeometryTransform transform;
        QSize expectedSize;
    } cases[] = {
        {GeometryTransform::MirrorHorizontal, QSize(12, 10)},
        {GeometryTransform::FlipVertical, QSize(12, 10)},
        {GeometryTransform::RotateClockwise, QSize(10, 12)},
        {GeometryTransform::RotateCounterclockwise, QSize(10, 12)},
        {GeometryTransform::RotateHalfTurn, QSize(12, 10)},
    };
    for (const auto& entry : cases) {
        SCOPED_TRACE(static_cast<int>(entry.transform));
        const QImage result = applyGeometryTransform(source, entry.transform);
        ASSERT_FALSE(result.isNull());
        EXPECT_EQ(result.size(), entry.expectedSize);
        EXPECT_EQ(result.format(), source.format());
        // Every source pixel lands on exactly one target pixel.
        for (int y = 0; y < source.height(); ++y) {
            for (int x = 0; x < source.width(); ++x) {
                QPoint target;
                switch (entry.transform) {
                case GeometryTransform::MirrorHorizontal:
                    target = QPoint{source.width() - 1 - x, y};
                    break;
                case GeometryTransform::FlipVertical:
                    target = QPoint{x, source.height() - 1 - y};
                    break;
                case GeometryTransform::RotateClockwise:
                    target = QPoint{source.height() - 1 - y, x};
                    break;
                case GeometryTransform::RotateCounterclockwise:
                    target = QPoint{y, source.width() - 1 - x};
                    break;
                case GeometryTransform::RotateHalfTurn:
                    target = QPoint{source.width() - 1 - x, source.height() - 1 - y};
                    break;
                }
                EXPECT_EQ(result.pixelColor(target), source.pixelColor(x, y))
                    << "source pixel (" << x << ", " << y << ")";
            }
        }
        EXPECT_TRUE(byteIdentical(source, before, "source after transform"));
        EXPECT_NE(result.cacheKey(), source.cacheKey());
    }
    EXPECT_TRUE(applyGeometryTransform(QImage(), GeometryTransform::MirrorHorizontal).isNull());
}

TEST(ImageEditOperationsTests, FillAndClearReplaceTheRectWithoutBlending) {
    const QImage source = translucentImage(24, 16);
    const QImage before = deepCopyOf(source);
    const QColor fill{10, 200, 90, 255};
    const QRect rect{4, 3, 10, 8};

    const QImage filled = fillImageRect(source, rect, fill);
    EXPECT_EQ(filled.size(), source.size());
    EXPECT_EQ(filled.pixelColor(rect.topLeft()), fill);
    EXPECT_EQ(filled.pixelColor(rect.center()), fill);
    EXPECT_EQ(filled.pixelColor(rect.bottomRight()), fill);
    EXPECT_EQ(filled.pixelColor(rect.left() - 1, rect.top()), source.pixelColor(3, 3));
    EXPECT_EQ(filled.pixelColor(rect.right() + 1, rect.bottom()), source.pixelColor(14, 10));
    EXPECT_TRUE(byteIdentical(source, before, "source after fill"));

    // Clear replaces translucent pixels with fully transparent ones instead of blending
    // towards them: a SourceOver clear would leave a coloured ghost here.
    const QImage cleared = clearImageRect(source, rect);
    EXPECT_EQ(cleared.pixelColor(rect.center()).alpha(), 0);
    EXPECT_EQ(cleared.pixelColor(rect.center()), QColor(Qt::transparent));
    EXPECT_EQ(cleared.pixelColor(rect.right() + 1, rect.top()), source.pixelColor(14, 3));
    EXPECT_TRUE(byteIdentical(source, before, "source after clear"));

    // Out-of-bounds rects clamp; empty rects leave the pixels untouched in a new buffer.
    const QImage clampedFill = fillImageRect(source, QRect{20, 12, 30, 30}, fill);
    EXPECT_EQ(clampedFill.size(), source.size());
    EXPECT_EQ(clampedFill.pixelColor(23, 15), fill);
    EXPECT_EQ(clampedFill.pixelColor(19, 11), source.pixelColor(19, 11));
    const QImage untouched = fillImageRect(source, QRect{40, 40, 4, 4}, fill);
    EXPECT_TRUE(byteIdentical(untouched, source, "fill outside the image"));
    EXPECT_NE(untouched.cacheKey(), source.cacheKey());
    EXPECT_TRUE(fillImageRect(QImage(), rect, fill).isNull());
    EXPECT_TRUE(clearImageRect(QImage(), rect).isNull());
}

TEST(ImageEditOperationsTests, MosaicPixelatesInOpaqueBlocksAndClampsBlockSize) {
    EXPECT_EQ(effectiveMosaicBlockSize(QRect{0, 0, 24, 12}, 8), 8);
    // Below the floor: the smallest supported block is 2 pixels.
    EXPECT_EQ(effectiveMosaicBlockSize(QRect{0, 0, 24, 12}, 1), 2);
    EXPECT_EQ(effectiveMosaicBlockSize(QRect{0, 0, 24, 12}, 0), 2);
    // Above the region: a block never exceeds the region edge.
    EXPECT_EQ(effectiveMosaicBlockSize(QRect{0, 0, 24, 12}, 64), 12);

    const QImage source = gradientImage(48, 24);
    const QImage before = deepCopyOf(source);
    const QImage mosaicked = mosaicImageRect(source, QRect{8, 4, 24, 12}, 8);
    EXPECT_EQ(mosaicked.size(), source.size());
    // One 8x8 block shares one sampled colour, and the block is opaque.
    EXPECT_EQ(mosaicked.pixelColor(8, 4), mosaicked.pixelColor(14, 10));
    EXPECT_EQ(mosaicked.pixelColor(8, 4).alpha(), 255);
    // Pixels outside the rect are byte-identical to the input.
    EXPECT_EQ(mosaicked.pixelColor(40, 20), source.pixelColor(40, 20));
    EXPECT_EQ(mosaicked.pixelColor(7, 4), source.pixelColor(7, 4));
    EXPECT_TRUE(byteIdentical(source, before, "source after mosaic"));

    // A block larger than the region clamps to the region edge, so a two-tone region
    // stays two blocks instead of collapsing into one averaged block. Without the clamp
    // (block 64 on a 24x12 region) both dimensions would scale by max(1, size/64) = 1 and
    // the whole region would share one colour.
    const QImage split = splitImage(24, 12);
    const QImage blocked = mosaicImageRect(split, split.rect(), 64);
    EXPECT_NE(blocked.pixelColor(2, 6), blocked.pixelColor(22, 6))
        << "oversized block collapsed the region into one averaged block";
    // The floor of 2 keeps the smallest blocks distinguishable from a no-op copy: pixels
    // 10 and 11 share one 2x2 block across the colour boundary at x == 12, so 12 lands in
    // the next (white) block.
    const QImage fine = mosaicImageRect(split, split.rect(), 0);
    EXPECT_EQ(fine.pixelColor(10, 6), fine.pixelColor(11, 6));
    EXPECT_NE(fine.pixelColor(11, 6), fine.pixelColor(12, 6));
    EXPECT_TRUE(mosaicImageRect(QImage(), QRect{0, 0, 4, 4}, 8).isNull());
    // A rect fully outside the image returns the pixels untouched in a new buffer.
    const QImage untouched = mosaicImageRect(source, QRect{60, 60, 8, 8}, 8);
    EXPECT_TRUE(byteIdentical(untouched, source, "mosaic outside the image"));
}

TEST(ImageEditOperationsTests, ApplyEditOperationDispatchesEveryRecipeVariant) {
    const QImage source = cornerImage(30, 20);
    const QImage gradient = gradientImage(30, 20);

    EXPECT_TRUE(
        byteIdentical(applyEditOperation(source, EditOperation{CropOperation{{4, 3, 10, 8}}}),
                      cropImageRect(source, QRect{4, 3, 10, 8}),
                      "crop variant"));
    EXPECT_TRUE(byteIdentical(
        applyEditOperation(
            source, EditOperation{GeometryTransformOperation{GeometryTransform::RotateHalfTurn}}),
        applyGeometryTransform(source, GeometryTransform::RotateHalfTurn),
        "transform variant"));
    const QColor fill{12, 34, 56, 255};
    EXPECT_TRUE(byteIdentical(
        applyEditOperation(gradient, EditOperation{FillRectOperation{{4, 3, 10, 8}, fill}}),
        fillImageRect(gradient, QRect{4, 3, 10, 8}, fill),
        "fill variant"));
    EXPECT_TRUE(byteIdentical(
        applyEditOperation(gradient, EditOperation{ClearRectOperation{{4, 3, 10, 8}}}),
        clearImageRect(gradient, QRect{4, 3, 10, 8}),
        "clear variant"));
    EXPECT_TRUE(byteIdentical(
        applyEditOperation(gradient, EditOperation{MosaicOperation{{4, 3, 10, 8}, 4}}),
        mosaicImageRect(gradient, QRect{4, 3, 10, 8}, 4),
        "mosaic variant"));
}

TEST(ImageEditOperationsTests, ApplyEditRecipeFoldsStepsInOrder) {
    const QImage source = cornerImage(40, 30);

    // An empty recipe is the identity, returned as an independent buffer.
    const QImage identity = applyEditRecipe(source, EditRecipe{});
    EXPECT_TRUE(byteIdentical(identity, source, "empty recipe"));
    EXPECT_NE(identity.cacheKey(), source.cacheKey());
    EXPECT_TRUE(applyEditRecipe(QImage(), EditRecipe{}).isNull());

    // Order is part of the recipe: crop-then-mirror is not mirror-then-crop.
    EditRecipe cropThenMirror;
    cropThenMirror.steps.emplace_back(CropOperation{QRect{4, 4, 20, 12}});
    cropThenMirror.steps.emplace_back(
        GeometryTransformOperation{GeometryTransform::MirrorHorizontal});
    EditRecipe mirrorThenCrop;
    mirrorThenCrop.steps.emplace_back(
        GeometryTransformOperation{GeometryTransform::MirrorHorizontal});
    mirrorThenCrop.steps.emplace_back(CropOperation{QRect{4, 4, 20, 12}});
    const QImage ordered = applyEditRecipe(source, cropThenMirror);
    const QImage reversed = applyEditRecipe(source, mirrorThenCrop);
    EXPECT_FALSE(byteIdentical(ordered, reversed, "recipe order"));

    // A recipe is exactly the manual fold of the same steps.
    QImage folded = deepCopyOf(source);
    folded = cropImageRect(folded, QRect{4, 4, 20, 12});
    folded = applyGeometryTransform(folded, GeometryTransform::MirrorHorizontal);
    EXPECT_TRUE(byteIdentical(ordered, folded, "recipe equals manual fold"));
    // The crop-then-mirror recipe really cropped first: its width is the crop width.
    EXPECT_EQ(ordered.width(), 20);
}

// The S01 first-step proof: for every extracted operation the interactive controller
// path and a direct call of the pure function produce byte-identical buffers, so the
// preview and a future export executor can share one implementation.
TEST(ImageEditOperationsTests, ControllerPreviewMatchesPureOperationByteForByte) {
    using ControllerTransform = ImageEditController::ImageTransform;
    const struct {
        GeometryTransform pure;
        ControllerTransform controller;
    } transforms[] = {
        {GeometryTransform::MirrorHorizontal, ControllerTransform::MirrorHorizontal},
        {GeometryTransform::FlipVertical, ControllerTransform::FlipVertical},
        {GeometryTransform::RotateClockwise, ControllerTransform::RotateClockwise},
        {GeometryTransform::RotateCounterclockwise, ControllerTransform::RotateCounterclockwise},
        {GeometryTransform::RotateHalfTurn, ControllerTransform::RotateHalfTurn},
    };

    // Crop.
    {
        ImageEditController controller;
        const QImage source = cornerImage(40, 30);
        setSource(controller, source);
        ASSERT_TRUE(controller.beginSession(
            2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));
        ASSERT_TRUE(controller.cropToImageRect(5, 4, 20, 10));
        EXPECT_TRUE(byteIdentical(controller.editedImage(),
                                  cropImageRect(source, QRect{5, 4, 20, 10}),
                                  "controller crop vs pure crop"));
    }
    // Geometry transforms.
    for (const auto& entry : transforms) {
        SCOPED_TRACE(static_cast<int>(entry.pure));
        ImageEditController controller;
        const QImage source = cornerImage(26, 18);
        setSource(controller, source);
        ASSERT_TRUE(controller.beginSession(
            2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));
        ASSERT_TRUE(controller.transformImage(entry.controller));
        EXPECT_TRUE(byteIdentical(controller.editedImage(),
                                  applyGeometryTransform(source, entry.pure),
                                  "controller transform vs pure transform"));
    }
    // Fill.
    {
        ImageEditController controller;
        const QImage source = translucentImage(30, 24);
        const QColor fill{99, 150, 3, 255};
        setSource(controller, source);
        ASSERT_TRUE(controller.beginSession(
            2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));
        ASSERT_TRUE(controller.fillImageRect(6, 5, 12, 9, fill));
        EXPECT_TRUE(byteIdentical(controller.editedImage(),
                                  fillImageRect(source, QRect{6, 5, 12, 9}, fill),
                                  "controller fill vs pure fill"));
    }
    // Clear.
    {
        ImageEditController controller;
        const QImage source = translucentImage(30, 24);
        setSource(controller, source);
        ASSERT_TRUE(controller.beginSession(
            2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));
        ASSERT_TRUE(controller.clearImageRect(6, 5, 12, 9));
        EXPECT_TRUE(byteIdentical(controller.editedImage(),
                                  clearImageRect(source, QRect{6, 5, 12, 9}),
                                  "controller clear vs pure clear"));
    }
    // Mosaic (block larger than the region, so the clamp path is compared too).
    {
        ImageEditController controller;
        const QImage source = gradientImage(44, 28);
        setSource(controller, source);
        ASSERT_TRUE(controller.beginSession(
            2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));
        ASSERT_TRUE(controller.mosaicImageRect(8, 6, 20, 10, 64));
        EXPECT_TRUE(byteIdentical(controller.editedImage(),
                                  mosaicImageRect(source, QRect{8, 6, 20, 10}, 64),
                                  "controller mosaic vs pure mosaic"));
    }
}

// The same proof at recipe level: a sequence driven through the controller replays
// byte-identically through applyEditRecipe on the untouched original.
TEST(ImageEditOperationsTests, ControllerEditSequenceReplaysThroughTheRecipe) {
    ImageEditController controller;
    const QImage source = cornerImage(60, 40);
    setSource(controller, source);
    ASSERT_TRUE(controller.beginSession(
        2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));

    ASSERT_TRUE(controller.cropToImageRect(6, 4, 40, 24));
    ASSERT_TRUE(controller.mirrorImage(false));
    ASSERT_TRUE(controller.mosaicImageRect(4, 4, 16, 12, 8));
    ASSERT_TRUE(controller.fillImageRect(30, 2, 8, 6, QColor(200, 30, 90, 255)));
    ASSERT_TRUE(controller.rotateImage(1));

    EditRecipe recipe;
    recipe.steps.emplace_back(CropOperation{QRect{6, 4, 40, 24}});
    recipe.steps.emplace_back(GeometryTransformOperation{GeometryTransform::MirrorHorizontal});
    recipe.steps.emplace_back(MosaicOperation{QRect{4, 4, 16, 12}, 8});
    recipe.steps.emplace_back(FillRectOperation{QRect{30, 2, 8, 6}, QColor(200, 30, 90, 255)});
    recipe.steps.emplace_back(GeometryTransformOperation{GeometryTransform::RotateClockwise});

    EXPECT_TRUE(byteIdentical(controller.editedImage(),
                              applyEditRecipe(source, recipe),
                              "controller sequence vs recipe replay"));
    // The original the comparison refers to never moved.
    EXPECT_TRUE(byteIdentical(controller.sourceImage(), source, "source after sequence"));
}
