#include "dvs/ui/ImageEditController.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QUrl>

#include <gtest/gtest.h>

namespace {

using dvs::ui::ImageEditController;

void ensureCoreApplication() {
    if (QCoreApplication::instance() != nullptr) {
        return;
    }
    static int argumentCount = 1;
    static char applicationName[] = "ImageEditControllerTests";
    static char* arguments[] = {applicationName, nullptr};
    static QCoreApplication application{argumentCount, arguments};
    static_cast<void>(application);
}

class CoreApplicationEnvironment final : public ::testing::Environment {
public:
    void SetUp() override {
        ensureCoreApplication();
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
