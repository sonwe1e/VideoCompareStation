#include "dvs/ui/ImageReviewController.h"

#include <QColor>
#include <QFile>
#include <QImage>
#include <QUrl>

#include <gtest/gtest.h>

namespace {

using dvs::ui::ImageReviewController;

[[nodiscard]] QImage solidImage(const QColor& color) {
    QImage image(4, 4, QImage::Format_ARGB32);
    image.fill(color);
    return image;
}

TEST(ImageReviewControllerTests, LoadsPrimaryAndSamplesChannels) {
    ImageReviewController controller;
    ASSERT_TRUE(
        controller.openPrimaryImage(solidImage(QColor(20, 40, 60)), QStringLiteral("left")));
    EXPECT_TRUE(controller.hasPrimary());
    EXPECT_FALSE(controller.hasSecondary());
    EXPECT_EQ(controller.primaryWidth(), 4);
    EXPECT_EQ(controller.primaryHeight(), 4);

    const QVariantMap pixel = controller.samplePixel(ImageReviewController::PrimarySlot, 1, 2);
    ASSERT_TRUE(pixel.value(QStringLiteral("valid")).toBool());
    EXPECT_EQ(pixel.value(QStringLiteral("r")).toInt(), 20);
    EXPECT_EQ(pixel.value(QStringLiteral("g")).toInt(), 40);
    EXPECT_EQ(pixel.value(QStringLiteral("b")).toInt(), 60);
    EXPECT_EQ(pixel.value(QStringLiteral("x")).toInt(), 1);
    EXPECT_EQ(pixel.value(QStringLiteral("y")).toInt(), 2);
}

TEST(ImageReviewControllerTests, DiffPairComputesPeakAndSignedMidpoint) {
    ImageReviewController controller;
    ASSERT_TRUE(
        controller.openPrimaryImage(solidImage(QColor(20, 40, 60)), QStringLiteral("left")));
    ASSERT_TRUE(
        controller.openSecondaryImage(solidImage(QColor(20, 40, 80)), QStringLiteral("right")));
    EXPECT_TRUE(controller.hasPair());
    EXPECT_EQ(controller.compareMode(), static_cast<int>(ImageReviewController::SideBySide));
    EXPECT_EQ(controller.maxAbsDifference(), 20);

    controller.setCompareMode(ImageReviewController::SignedDifference);
    const QVariantMap signedPixel =
        controller.samplePixel(ImageReviewController::DisplayDiffSlot, 0, 0);
    ASSERT_TRUE(signedPixel.value(QStringLiteral("valid")).toBool());
    // Blue channel: A=60, B=80 → 128 + (60-80)*4 = 48.
    EXPECT_EQ(signedPixel.value(QStringLiteral("b")).toInt(), 48);
    // Red/green unchanged → mid gray.
    EXPECT_EQ(signedPixel.value(QStringLiteral("r")).toInt(), 128);
}

TEST(ImageReviewControllerTests, HighlightAndAbsDifferenceProduceOutput) {
    ImageReviewController controller;
    ASSERT_TRUE(controller.openPrimaryImage(solidImage(QColor(0, 0, 0)), QStringLiteral("left")));
    ASSERT_TRUE(
        controller.openSecondaryImage(solidImage(QColor(10, 10, 10)), QStringLiteral("right")));

    controller.setCompareMode(ImageReviewController::AbsDifference);
    EXPECT_EQ(controller.samplePixel(ImageReviewController::DisplayDiffSlot, 0, 0)
                  .value(QStringLiteral("r"))
                  .toInt(),
              40);

    controller.setCompareMode(ImageReviewController::Highlight);
    const QVariantMap highlight =
        controller.samplePixel(ImageReviewController::DisplayDiffSlot, 0, 0);
    ASSERT_TRUE(highlight.value(QStringLiteral("valid")).toBool());
    EXPECT_GT(highlight.value(QStringLiteral("r")).toInt(), 0);
}

TEST(ImageReviewControllerTests, ZoomClampsAndResetRestoresDefaults) {
    ImageReviewController controller;
    ASSERT_TRUE(controller.openPrimaryImage(solidImage(Qt::red), QStringLiteral("left")));
    controller.setZoom(128.0);
    EXPECT_LE(controller.zoom(), 64.0);
    controller.resetView();
    EXPECT_DOUBLE_EQ(controller.zoom(), 1.0);
    EXPECT_DOUBLE_EQ(controller.panX(), 0.5);
}

TEST(ImageReviewControllerTests, RejectsMissingFile) {
    ImageReviewController controller;
    EXPECT_FALSE(
        controller.openPrimary(QUrl::fromLocalFile(QStringLiteral("Z:/definitely/missing.png"))));
    EXPECT_FALSE(controller.hasPrimary());
    EXPECT_FALSE(controller.errorText().isEmpty());
}

TEST(ImageReviewControllerTests, LoadsUserPngFromDisk) {
    ImageReviewController controller;
    const QString path = QStringLiteral("D:/Pictures/252666c8-0111-4dc8-b70d-768eb4587938.png");
    if (!QFile::exists(path)) {
        GTEST_SKIP() << "fixture image not present";
    }
    const bool ok = controller.openPrimary(QUrl::fromLocalFile(path));
    EXPECT_TRUE(ok) << qPrintable(controller.errorText());
    if (ok) {
        EXPECT_GT(controller.primaryWidth(), 0);
        EXPECT_GT(controller.primaryHeight(), 0);
    }
}

} // namespace
