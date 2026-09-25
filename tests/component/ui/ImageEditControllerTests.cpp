#include "dvs/ui/ImageEditController.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>
#include <gtest/gtest.h>

namespace {

using dvs::ui::ImageEditController;

// Annotation rendering uses QFont/QFontMetrics, which need a QGuiApplication rather than a
// bare QCoreApplication.
void ensureGuiApplication() {
    if (QGuiApplication::instance() != nullptr) {
        return;
    }
    static int argumentCount = 1;
    static char applicationName[] = "ImageEditControllerTests";
    static char* arguments[] = {applicationName, nullptr};
    static QGuiApplication application{argumentCount, arguments};
    static_cast<void>(application);
}

class CoreApplicationEnvironment final : public ::testing::Environment {
public:
    void SetUp() override {
        ensureGuiApplication();
    }
};

[[maybe_unused]] const bool kCoreApplicationEnvironmentRegistered =
    ::testing::AddGlobalTestEnvironment(new CoreApplicationEnvironment) != nullptr;

// Distinct corner colours make a crop observable by pixel, not only by size.
[[nodiscard]] QImage cornerImage(const int width, const int height) {
    QImage image(width, height, QImage::Format_ARGB32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.setPixelColor(x, y, QColor(x * 3 % 256, y * 5 % 256, 40, 255));
        }
    }
    return image;
}

[[nodiscard]] QImage alphaImage(const int width, const int height) {
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.setPixelColor(x, y, QColor(200, 30, 60, 128));
        }
    }
    return image;
}

void setSource(ImageEditController& controller, const QImage& image) {
    controller.setSourceImageProvider([image](const int slot) {
        static_cast<void>(slot);
        return image;
    });
}

} // namespace

TEST(ImageEditControllerTests, BeginSessionKeepsTheOriginalImmutableWhileCropping) {
    ImageEditController controller;
    const QImage source = cornerImage(40, 30);
    setSource(controller, source);
    QImage providerCopy = source;
    const QImage providerBefore = providerCopy.copy();

    ASSERT_TRUE(controller.beginSession(
        2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));
    EXPECT_TRUE(controller.active());
    EXPECT_FALSE(controller.dirty());
    EXPECT_EQ(controller.imageWidth(), 40);
    EXPECT_EQ(controller.imageHeight(), 30);
    EXPECT_EQ(controller.sourceSlot(), 2);

    ASSERT_TRUE(controller.cropToImageRect(5, 4, 20, 10));
    EXPECT_EQ(controller.imageWidth(), 20);
    EXPECT_EQ(controller.imageHeight(), 10);
    EXPECT_TRUE(controller.dirty());

    // The working copy moved; the immutable original and the provider's buffer did not.
    const QImage edited = controller.editedImage();
    ASSERT_FALSE(edited.isNull());
    EXPECT_EQ(edited.size(), QSize(20, 10));
    EXPECT_EQ(edited.pixelColor(0, 0), source.pixelColor(5, 4));
    EXPECT_EQ(controller.sourceImage().size(), QSize(40, 30));
    EXPECT_EQ(controller.sourceImage().pixelColor(0, 0), source.pixelColor(0, 0));
    EXPECT_EQ(providerCopy, providerBefore);
}

TEST(ImageEditControllerTests, CropClampsToImageBoundsAndRejectsDegenerateRects) {
    ImageEditController controller;
    setSource(controller, cornerImage(20, 20));
    ASSERT_TRUE(controller.beginSession(
        2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));

    // Partly outside: clamped to the intersection.
    ASSERT_TRUE(controller.cropToImageRect(15, 15, 50, 50));
    EXPECT_EQ(controller.imageWidth(), 5);
    EXPECT_EQ(controller.imageHeight(), 5);

    // Fully outside / empty: rejected, image unchanged.
    EXPECT_FALSE(controller.cropToImageRect(50, 50, 4, 4));
    EXPECT_EQ(controller.imageWidth(), 5);
    EXPECT_FALSE(controller.cropToImageRect(0, 0, 0, 5));
    EXPECT_EQ(controller.imageWidth(), 5);

    // Whole-image rect is a no-op, not a history entry: the earlier crop is still the only
    // step, so a single undo returns to the full original size.
    EXPECT_FALSE(controller.cropToImageRect(0, 0, 5, 5));
    EXPECT_EQ(controller.imageWidth(), 5);
    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(controller.imageWidth(), 20);
    EXPECT_FALSE(controller.canUndo());
}

TEST(ImageEditControllerTests, UndoRedoWalkTheCropHistoryWithLabels) {
    ImageEditController controller;
    setSource(controller, cornerImage(40, 40));
    ASSERT_TRUE(controller.beginSession(
        2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));

    ASSERT_TRUE(controller.cropToImageRect(0, 0, 30, 30));
    ASSERT_TRUE(controller.cropToImageRect(0, 0, 20, 20));
    EXPECT_EQ(controller.imageWidth(), 20);
    EXPECT_TRUE(controller.canUndo());
    EXPECT_FALSE(controller.canRedo());
    EXPECT_FALSE(controller.undoLabel().isEmpty());
    EXPECT_EQ(controller.undoLabel(), QStringLiteral("裁剪"));

    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(controller.imageWidth(), 30);
    EXPECT_TRUE(controller.canRedo());
    EXPECT_FALSE(controller.redoLabel().isEmpty());

    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(controller.imageWidth(), 40);
    EXPECT_FALSE(controller.canUndo());
    EXPECT_FALSE(controller.dirty());

    ASSERT_TRUE(controller.redo());
    EXPECT_EQ(controller.imageWidth(), 30);
    ASSERT_TRUE(controller.redo());
    EXPECT_EQ(controller.imageWidth(), 20);
    EXPECT_FALSE(controller.canRedo());
    EXPECT_TRUE(controller.dirty());

    // Exhausted history is a clean failure, not a crash or a silent no-op.
    EXPECT_FALSE(controller.redo());
    EXPECT_FALSE(controller.lastStatus().isEmpty());
}

TEST(ImageEditControllerTests, HistoryLimitBoundsRetainedSteps) {
    ImageEditController controller;
    setSource(controller, cornerImage(60, 60));
    ASSERT_TRUE(controller.beginSession(
        2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));
    controller.setHistoryLimit(2);
    EXPECT_EQ(controller.historyLimit(), 2);

    ASSERT_TRUE(controller.cropToImageRect(0, 0, 50, 50));
    ASSERT_TRUE(controller.cropToImageRect(0, 0, 40, 40));
    ASSERT_TRUE(controller.cropToImageRect(0, 0, 30, 30));
    EXPECT_EQ(controller.imageWidth(), 30);

    // Only the two most recent steps are retained.
    ASSERT_TRUE(controller.undo());
    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(controller.imageWidth(), 50);
    EXPECT_FALSE(controller.canUndo());
}

TEST(ImageEditControllerTests, SaveCopyWritesPngAndTransparencySurvives) {
    ImageEditController controller;
    setSource(controller, alphaImage(24, 12));
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = directory.filePath(QStringLiteral("original.png"));
    ASSERT_TRUE(controller.beginSession(
        2, QStringLiteral("original.png"), QUrl::fromLocalFile(sourcePath)));
    ASSERT_TRUE(controller.cropToImageRect(0, 0, 12, 12));

    const QString copyPath = directory.filePath(QStringLiteral("copy.png"));
    ASSERT_TRUE(controller.saveCopy(QUrl::fromLocalFile(copyPath), false, QColor(Qt::white)));
    EXPECT_TRUE(QFileInfo::exists(copyPath));
    EXPECT_FALSE(controller.dirty());

    const QImage saved{copyPath};
    ASSERT_FALSE(saved.isNull());
    EXPECT_EQ(saved.size(), QSize(12, 12));
    EXPECT_TRUE(saved.hasAlphaChannel());
    EXPECT_EQ(saved.pixelColor(6, 6).alpha(), 128);
    // The original file was never created or touched: edits are copies only.
    EXPECT_FALSE(QFileInfo::exists(sourcePath));
}

TEST(ImageEditControllerTests, SaveCopyRefusesJpegWithAlphaUntilABackgroundIsChosen) {
    ImageEditController controller;
    setSource(controller, alphaImage(16, 16));
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(controller.beginSession(
        2,
        QStringLiteral("original.png"),
        QUrl::fromLocalFile(directory.filePath(QStringLiteral("original.png")))));

    const QString jpegPath = directory.filePath(QStringLiteral("copy.jpg"));
    EXPECT_FALSE(controller.saveCopy(QUrl::fromLocalFile(jpegPath), false, QColor(Qt::white)));
    EXPECT_FALSE(QFileInfo::exists(jpegPath));
    EXPECT_TRUE(controller.lastStatus().contains(QStringLiteral("背景")));

    // The Qt JPEG plugin is optional in this build. When it is present the flattened save
    // must succeed; when it is absent the controller must say so instead of writing a
    // broken file. Either way the PNG fallback always works.
    if (ImageEditController::jpegEncodingAvailable()) {
        ASSERT_TRUE(controller.saveCopy(QUrl::fromLocalFile(jpegPath), true, QColor(12, 200, 60)));
        EXPECT_TRUE(QFileInfo::exists(jpegPath));
        const QImage saved{jpegPath};
        ASSERT_FALSE(saved.isNull());
        EXPECT_FALSE(saved.hasAlphaChannel());
    } else {
        EXPECT_FALSE(controller.saveCopy(QUrl::fromLocalFile(jpegPath), true, QColor(12, 200, 60)));
        EXPECT_FALSE(QFileInfo::exists(jpegPath));
        EXPECT_TRUE(controller.lastStatus().contains(QStringLiteral("JPEG")));
    }
    const QString pngPath = directory.filePath(QStringLiteral("copy.png"));
    ASSERT_TRUE(controller.saveCopy(QUrl::fromLocalFile(pngPath), false, QColor(Qt::white)));
    EXPECT_TRUE(QFileInfo::exists(pngPath));
}

TEST(ImageEditControllerTests, FlattenCompositesAlphaOverTheChosenBackground) {
    const QImage translucent = alphaImage(8, 8);
    const QImage flattened =
        ImageEditController::flattenOntoBackground(translucent, QColor(0, 240, 0));
    ASSERT_FALSE(flattened.isNull());
    EXPECT_EQ(flattened.size(), QSize(8, 8));
    EXPECT_FALSE(flattened.hasAlphaChannel());

    // A=128 over opaque green must land between the source red and the background, not on
    // implicit black: the composite has to keep the green channel dominant here.
    const QColor blended = flattened.pixelColor(4, 4);
    EXPECT_EQ(blended.alpha(), 255);
    EXPECT_GT(blended.green(), blended.red());
    EXPECT_GT(blended.green(), blended.blue());

    // An invalid colour falls back to white rather than producing an undefined buffer.
    const QImage fallback = ImageEditController::flattenOntoBackground(translucent, QColor());
    ASSERT_FALSE(fallback.isNull());
    EXPECT_EQ(fallback.pixelColor(4, 4).alpha(), 255);
    EXPECT_TRUE(ImageEditController::flattenOntoBackground(QImage(), QColor(Qt::white)).isNull());
}

TEST(ImageEditControllerTests, SaveCopyRefusesToOverwriteTheSourceFile) {
    ImageEditController controller;
    setSource(controller, cornerImage(10, 10));
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = directory.filePath(QStringLiteral("original.png"));
    ASSERT_TRUE(controller.beginSession(
        2, QStringLiteral("original.png"), QUrl::fromLocalFile(sourcePath)));
    ASSERT_TRUE(controller.cropToImageRect(0, 0, 5, 5));

    EXPECT_FALSE(controller.saveCopy(QUrl::fromLocalFile(sourcePath), false, QColor(Qt::white)));
    EXPECT_FALSE(QFileInfo::exists(sourcePath));
    EXPECT_TRUE(controller.lastStatus().contains(QStringLiteral("副本")));
}

TEST(ImageEditControllerTests, EndSessionDropsTheWorkingCopyAndTheOriginalStaysReadable) {
    ImageEditController controller;
    const QImage source = cornerImage(18, 9);
    setSource(controller, source);
    ASSERT_TRUE(controller.beginSession(
        3, QStringLiteral("b.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/b.png"))));
    ASSERT_TRUE(controller.cropToImageRect(0, 0, 9, 9));

    controller.endSession();
    EXPECT_FALSE(controller.active());
    EXPECT_FALSE(controller.dirty());
    EXPECT_EQ(controller.imageWidth(), 0);
    EXPECT_TRUE(controller.editedImage().isNull());
    EXPECT_TRUE(controller.sourceImage().isNull());
    EXPECT_FALSE(controller.canUndo());

    // The provider buffer that seeded the session is still intact.
    EXPECT_EQ(source.size(), QSize(18, 9));

    // A new session starts from the original again, not from the discarded working copy.
    ASSERT_TRUE(controller.beginSession(
        3, QStringLiteral("b.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/b.png"))));
    EXPECT_EQ(controller.imageWidth(), 18);
    EXPECT_FALSE(controller.dirty());
}

TEST(ImageEditControllerTests, EditedImageUrlTracksEveryImageChange) {
    ImageEditController controller;
    setSource(controller, cornerImage(12, 12));
    ASSERT_TRUE(controller.beginSession(
        2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));
    const QString beginUrl = controller.editedImageUrl();

    ASSERT_TRUE(controller.cropToImageRect(0, 0, 6, 6));
    const QString croppedUrl = controller.editedImageUrl();
    EXPECT_NE(beginUrl, croppedUrl);

    ASSERT_TRUE(controller.undo());
    const QString undoneUrl = controller.editedImageUrl();
    EXPECT_NE(croppedUrl, undoneUrl);
    EXPECT_NE(beginUrl, undoneUrl);
}

namespace {

[[nodiscard]] QImage solidImage(const int width, const int height, const QColor& color) {
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(color);
    return image;
}

// A horizontal gradient makes both "changed" and "untouched" pixels easy to assert.
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

} // namespace

TEST(ImageEditControllerTests, BrushStrokePaintsAtImageCoordinatesAndUndoesExactly) {
    ImageEditController controller;
    const QImage source = gradientImage(40, 30);
    setSource(controller, source);
    ASSERT_TRUE(controller.beginSession(
        2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));

    // A pre-existing edit outside the stroke proves the undo patch is dirty-rect scoped and
    // does not restore a whole stale image over unrelated pixels.
    ASSERT_TRUE(controller.mosaicImageRect(0, 20, 10, 8, 4));
    const QColor mosaicPixel = controller.editedImage().pixelColor(4, 24);

    ASSERT_TRUE(controller.beginStroke(QColor(255, 0, 0), 8, 1.0));
    EXPECT_TRUE(controller.strokeActive());
    ASSERT_TRUE(controller.strokeTo(5, 5));
    ASSERT_TRUE(controller.strokeTo(20, 5));
    ASSERT_TRUE(controller.strokeTo(30, 12));
    ASSERT_TRUE(controller.endStroke());
    EXPECT_FALSE(controller.strokeActive());

    // Stroke pixels are the brush colour; the untouched gradient and the earlier mosaic
    // survive.
    const QImage painted = controller.editedImage();
    const QColor onStroke = painted.pixelColor(20, 5);
    EXPECT_GT(onStroke.red(), 200);
    EXPECT_LT(onStroke.green(), 60);
    EXPECT_LT(onStroke.blue(), 60);
    EXPECT_EQ(painted.pixelColor(20, 25), source.pixelColor(20, 25));
    EXPECT_EQ(painted.pixelColor(4, 24), mosaicPixel);
    EXPECT_TRUE(controller.dirty());
    EXPECT_EQ(controller.undoLabel(), QStringLiteral("画笔"));

    // Undo restores exactly the stroke footprint (and nothing else); redo re-applies it.
    ASSERT_TRUE(controller.undo());
    const QImage undone = controller.editedImage();
    EXPECT_EQ(undone.pixelColor(20, 5), source.pixelColor(20, 5));
    EXPECT_EQ(undone.pixelColor(4, 24), mosaicPixel);
    ASSERT_TRUE(controller.redo());
    EXPECT_GT(controller.editedImage().pixelColor(20, 5).red(), 200);

    // The immutable original never received a pixel.
    EXPECT_EQ(controller.sourceImage().pixelColor(20, 5), source.pixelColor(20, 5));
}

TEST(ImageEditControllerTests, OneStrokeIsOneUndoStepRegardlessOfSegmentCount) {
    ImageEditController controller;
    setSource(controller, solidImage(32, 32, QColor(0, 0, 0)));
    ASSERT_TRUE(controller.beginSession(
        2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));

    ASSERT_TRUE(controller.beginStroke(QColor(0, 255, 0), 4, 1.0));
    for (int x = 2; x < 30; ++x) {
        ASSERT_TRUE(controller.strokeTo(x, 16));
    }
    ASSERT_TRUE(controller.endStroke());

    // A single undo returns to the pristine image: the whole gesture is one history step.
    ASSERT_TRUE(controller.undo());
    EXPECT_FALSE(controller.canUndo());
    EXPECT_FALSE(controller.dirty());
    EXPECT_EQ(controller.editedImage().pixelColor(16, 16), QColor(0, 0, 0));
}

TEST(ImageEditControllerTests, BrushWidthAndOpacityShapeTheStroke) {
    ImageEditController controller;
    setSource(controller, solidImage(40, 40, QColor(0, 0, 0)));
    ASSERT_TRUE(controller.beginSession(
        2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));

    // Opaque thin stroke: the centre line is the pure brush colour, a pixel 6 px away is not.
    ASSERT_TRUE(controller.beginStroke(QColor(255, 255, 255), 3, 1.0));
    ASSERT_TRUE(controller.strokeTo(5, 20));
    ASSERT_TRUE(controller.strokeTo(35, 20));
    ASSERT_TRUE(controller.endStroke());
    EXPECT_GT(controller.editedImage().pixelColor(20, 20).red(), 240);
    EXPECT_EQ(controller.editedImage().pixelColor(20, 26), QColor(0, 0, 0));
    ASSERT_TRUE(controller.undo());

    // Half-opacity: the same geometry lands as a blend instead of pure white.
    ASSERT_TRUE(controller.beginStroke(QColor(255, 255, 255), 3, 0.5));
    ASSERT_TRUE(controller.strokeTo(5, 20));
    ASSERT_TRUE(controller.strokeTo(35, 20));
    ASSERT_TRUE(controller.endStroke());
    const int blended = controller.editedImage().pixelColor(20, 20).red();
    EXPECT_GT(blended, 90);
    EXPECT_LT(blended, 170);

    // Width is a diameter: a wide stroke covers a pixel the thin one left alone.
    ASSERT_TRUE(controller.undo());
    ASSERT_TRUE(controller.beginStroke(QColor(255, 255, 255), 16, 1.0));
    ASSERT_TRUE(controller.strokeTo(5, 20));
    ASSERT_TRUE(controller.strokeTo(35, 20));
    ASSERT_TRUE(controller.endStroke());
    EXPECT_GT(controller.editedImage().pixelColor(20, 26).red(), 240);
}

TEST(ImageEditControllerTests, MosaicObscuresTheRegionAndUndoes) {
    ImageEditController controller;
    const QImage source = gradientImage(48, 24);
    setSource(controller, source);
    ASSERT_TRUE(controller.beginSession(
        2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));
    const QImage before = controller.editedImage();

    ASSERT_TRUE(controller.mosaicImageRect(8, 4, 24, 12, 8));
    const QImage mosaic = controller.editedImage();
    // The gradient is gone: pixels inside one block share a value, and the region is opaque.
    EXPECT_EQ(mosaic.pixelColor(8, 4), mosaic.pixelColor(14, 10));
    EXPECT_EQ(mosaic.pixelColor(8, 4).alpha(), 255);
    // Outside the rect nothing moved.
    EXPECT_EQ(mosaic.pixelColor(40, 20), before.pixelColor(40, 20));
    EXPECT_EQ(controller.undoLabel(), QStringLiteral("马赛克"));

    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(controller.editedImage(), before);
    ASSERT_TRUE(controller.redo());
    EXPECT_EQ(controller.editedImage(), mosaic);
    EXPECT_EQ(controller.sourceImage(), source);
}

TEST(ImageEditControllerTests, BrushAndMosaicShareOneOrderedHistory) {
    ImageEditController controller;
    setSource(controller, solidImage(40, 40, QColor(20, 20, 20)));
    ASSERT_TRUE(controller.beginSession(
        2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));

    ASSERT_TRUE(controller.beginStroke(QColor(200, 0, 0), 6, 1.0));
    ASSERT_TRUE(controller.strokeTo(10, 10));
    ASSERT_TRUE(controller.strokeTo(30, 10));
    ASSERT_TRUE(controller.endStroke());
    ASSERT_TRUE(controller.mosaicImageRect(6, 24, 20, 10, 6));

    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(controller.undoLabel(), QStringLiteral("画笔"));
    ASSERT_TRUE(controller.undo());
    EXPECT_FALSE(controller.canUndo());
    EXPECT_FALSE(controller.dirty());
    EXPECT_EQ(controller.editedImage().pixelColor(20, 10), QColor(20, 20, 20));
    EXPECT_EQ(controller.editedImage().pixelColor(16, 28), QColor(20, 20, 20));
}

TEST(ImageEditControllerTests, PixelToolsRejectWithoutASession) {
    ImageEditController controller;
    QImage probe = solidImage(8, 8, QColor(0, 0, 0));
    setSource(controller, probe);

    EXPECT_FALSE(controller.beginStroke(QColor(255, 0, 0), 4, 1.0));
    EXPECT_FALSE(controller.strokeTo(1, 1));
    EXPECT_FALSE(controller.endStroke());
    EXPECT_FALSE(controller.mosaicImageRect(0, 0, 4, 4, 4));
    EXPECT_FALSE(controller.strokeActive());
    EXPECT_FALSE(controller.lastStatus().isEmpty());

    // A session that ends mid-stroke cannot leave the stroke flag behind.
    ASSERT_TRUE(controller.beginSession(
        2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));
    ASSERT_TRUE(controller.beginStroke(QColor(255, 0, 0), 4, 1.0));
    ASSERT_TRUE(controller.strokeTo(2, 2));
    controller.endSession();
    EXPECT_FALSE(controller.strokeActive());
    EXPECT_TRUE(controller.editedImage().isNull());
}

TEST(ImageEditControllerTests, FillAndClearEditTheRectAndUndoExactly) {
    ImageEditController controller;
    const QImage source = solidImage(32, 32, QColor(10, 20, 30));
    setSource(controller, source);
    ASSERT_TRUE(controller.beginSession(
        2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));

    ASSERT_TRUE(controller.fillImageRect(4, 4, 10, 10, QColor(220, 40, 40)));
    const QImage filled = controller.editedImage();
    EXPECT_EQ(filled.pixelColor(8, 8), QColor(220, 40, 40));
    EXPECT_EQ(filled.pixelColor(8, 8).alpha(), 255);
    EXPECT_EQ(filled.pixelColor(20, 20), QColor(10, 20, 30));
    EXPECT_EQ(controller.undoLabel(), QStringLiteral("填充"));

    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(controller.editedImage(), source);
    ASSERT_TRUE(controller.redo());
    EXPECT_EQ(controller.editedImage(), filled);

    // Clear punches the rect to transparent without touching anything else.
    ASSERT_TRUE(controller.clearImageRect(4, 4, 10, 10));
    const QImage cleared = controller.editedImage();
    EXPECT_EQ(cleared.pixelColor(8, 8).alpha(), 0);
    EXPECT_EQ(cleared.pixelColor(20, 20), QColor(10, 20, 30));
    EXPECT_EQ(controller.undoLabel(), QStringLiteral("清除"));
    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(controller.editedImage(), filled);

    // Degenerate and out-of-bounds rects are refused, not silently applied.
    EXPECT_FALSE(controller.fillImageRect(0, 0, 0, 5, QColor(Qt::red)));
    EXPECT_FALSE(controller.clearImageRect(100, 100, 4, 4));
    EXPECT_FALSE(controller.fillImageRect(0, 0, 4, 4, QColor()));
}

TEST(ImageEditControllerTests, AnnotationsRenderIntoLedgerImagesButNotTheOriginal) {
    ImageEditController controller;
    const QImage source = solidImage(64, 48, QColor(0, 0, 0));
    setSource(controller, source);
    ASSERT_TRUE(controller.beginSession(
        2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));
    EXPECT_EQ(controller.annotationCount(), 0);
    EXPECT_EQ(controller.selectedAnnotation(), -1);

    // Rectangle outline in red, 3 px wide.
    ASSERT_TRUE(controller.addRectangle(10, 10, 30, 30, QColor(255, 0, 0), 3));
    EXPECT_EQ(controller.annotationCount(), 1);
    EXPECT_EQ(controller.selectedAnnotation(), 0);
    const QImage withRectangle = controller.editedImage();
    // A 3 px pen centres on the path, so the fully covered row is y == 10 (y == 11 is the
    // antialiased half-coverage row).
    EXPECT_GT(withRectangle.pixelColor(20, 10).red(), 180);
    EXPECT_LT(withRectangle.pixelColor(20, 10).green(), 80);
    EXPECT_EQ(withRectangle.pixelColor(20, 20).red(), 0); // outline only, interior untouched

    // Arrow from (10, 40) to (50, 40).
    ASSERT_TRUE(controller.addArrow(10, 40, 50, 40, QColor(0, 255, 0), 3));
    EXPECT_EQ(controller.annotationCount(), 2);
    EXPECT_GT(controller.editedImage().pixelColor(30, 40).green(), 180);

    // Text is drawn at the requested pixel size.
    ASSERT_TRUE(controller.addText(6, 20, QStringLiteral("AB"), QColor(255, 255, 0), 24));
    EXPECT_EQ(controller.annotationCount(), 3);
    const QImage withText = controller.editedImage();
    int textInk = 0;
    for (int y = 0; y < 30; ++y) {
        for (int x = 0; x < 60; ++x) {
            const QColor pixel = withText.pixelColor(x, y);
            if (pixel.red() > 180 && pixel.green() > 180 && pixel.blue() < 90) {
                ++textInk;
            }
        }
    }
    EXPECT_GT(textInk, 10);

    // The immutable original and the saved-copy source never carry annotations.
    EXPECT_EQ(controller.sourceImage(), source);
    EXPECT_FALSE(controller.addText(4, 4, QStringLiteral("   "), QColor(Qt::white), 20));
    EXPECT_FALSE(controller.addRectangle(4, 4, 4, 4, QColor(Qt::white), 2));
}

TEST(ImageEditControllerTests, AnnotationsAreSelectableMovableAndDeletable) {
    ImageEditController controller;
    setSource(controller, solidImage(80, 60, QColor(0, 0, 0)));
    ASSERT_TRUE(controller.beginSession(
        2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));
    ASSERT_TRUE(controller.addRectangle(10, 10, 30, 30, QColor(255, 0, 0), 3));

    // Hit test picks the annotation, a miss clears the selection.
    EXPECT_TRUE(controller.selectAnnotationAt(20, 11));
    EXPECT_EQ(controller.selectedAnnotation(), 0);
    EXPECT_FALSE(controller.selectAnnotationAt(70, 55));
    EXPECT_EQ(controller.selectedAnnotation(), -1);
    EXPECT_FALSE(controller.moveSelectedAnnotation(5, 5));
    EXPECT_FALSE(controller.deleteSelectedAnnotation());

    ASSERT_TRUE(controller.selectAnnotationAt(20, 11));
    ASSERT_TRUE(controller.moveSelectedAnnotation(10, 5));
    // The outline moved: the old top edge is now background, the new centre row is red.
    EXPECT_EQ(controller.editedImage().pixelColor(20, 10).red(), 0);
    EXPECT_GT(controller.editedImage().pixelColor(30, 15).red(), 180);
    ASSERT_TRUE(controller.undo());
    EXPECT_GT(controller.editedImage().pixelColor(20, 10).red(), 180);

    ASSERT_TRUE(controller.deleteSelectedAnnotation());
    EXPECT_EQ(controller.annotationCount(), 0);
    EXPECT_FALSE(controller.editedImage().isNull());
    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(controller.annotationCount(), 1);

    controller.clearAnnotations();
    EXPECT_EQ(controller.annotationCount(), 0);
    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(controller.annotationCount(), 1);
}

TEST(ImageEditControllerTests, AnnotationDragGestureCommitsOneHistoryStep) {
    ImageEditController controller;
    setSource(controller, solidImage(80, 60, QColor(0, 0, 0)));
    ASSERT_TRUE(controller.beginSession(
        2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));
    ASSERT_TRUE(controller.addRectangle(10, 10, 30, 30, QColor(255, 0, 0), 3));

    ASSERT_TRUE(controller.beginAnnotationDrag(20, 11));
    ASSERT_TRUE(controller.dragAnnotationTo(25, 15));
    ASSERT_TRUE(controller.dragAnnotationTo(30, 20));
    ASSERT_TRUE(controller.dragAnnotationTo(35, 25));
    ASSERT_TRUE(controller.endAnnotationDrag());

    // One undo returns to the pre-drag position, not to an intermediate one.
    ASSERT_TRUE(controller.undo());
    EXPECT_GT(controller.editedImage().pixelColor(20, 10).red(), 180);
    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(controller.annotationCount(), 0);

    // A drag with no movement commits nothing, so one undo removes the add itself.
    ASSERT_TRUE(controller.redo());
    ASSERT_TRUE(controller.beginAnnotationDrag(20, 11));
    EXPECT_FALSE(controller.endAnnotationDrag());
    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(controller.annotationCount(), 0);
    EXPECT_FALSE(controller.canUndo());
    EXPECT_FALSE(controller.beginAnnotationDrag(70, 55));
}

TEST(ImageEditControllerTests, CropTranslatesSurvivingAnnotationsAndDropsTheRest) {
    ImageEditController controller;
    setSource(controller, solidImage(80, 80, QColor(0, 0, 0)));
    ASSERT_TRUE(controller.beginSession(
        2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));
    ASSERT_TRUE(controller.addRectangle(40, 40, 60, 60, QColor(255, 0, 0), 3));
    ASSERT_TRUE(controller.addRectangle(2, 2, 8, 8, QColor(0, 255, 0), 3));
    EXPECT_EQ(controller.annotationCount(), 2);

    ASSERT_TRUE(controller.cropToImageRect(20, 20, 40, 40));
    // The far annotation fell outside the kept area; the other followed the crop exactly.
    EXPECT_EQ(controller.annotationCount(), 1);
    const QImage cropped = controller.editedImage();
    EXPECT_EQ(cropped.size(), QSize(40, 40));
    EXPECT_GT(cropped.pixelColor(20, 21).red(), 180);

    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(controller.annotationCount(), 2);
    EXPECT_EQ(controller.editedImage().size(), QSize(80, 80));
    EXPECT_GT(controller.editedImage().pixelColor(40, 41).red(), 180);
}

TEST(ImageEditControllerTests, AnnotationCountIsBounded) {
    ImageEditController controller;
    setSource(controller, solidImage(16, 16, QColor(0, 0, 0)));
    ASSERT_TRUE(controller.beginSession(
        2, QStringLiteral("a.png"), QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.png"))));

    for (int index = 0; index < 64; ++index) {
        ASSERT_TRUE(controller.addRectangle(1, 1, 14, 14, QColor(255, 0, 0), 1)) << index;
    }
    EXPECT_EQ(controller.annotationCount(), 64);
    EXPECT_FALSE(controller.addArrow(1, 1, 14, 14, QColor(255, 0, 0), 1));
    EXPECT_TRUE(controller.lastStatus().contains(QStringLiteral("上限")));
}
