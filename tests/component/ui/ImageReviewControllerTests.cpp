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

TEST(ImageReviewControllerTests, WipeModeRequiresPairAndClampsPosition) {
    ImageReviewController controller;
    ASSERT_TRUE(controller.openPrimaryImage(solidImage(Qt::red), QStringLiteral("left")));

    // Without a pair the wipe mode is rejected like the other comparison modes.
    controller.setCompareMode(ImageReviewController::Wipe);
    EXPECT_EQ(controller.compareMode(), static_cast<int>(ImageReviewController::PrimaryOnly));
    EXPECT_FALSE(controller.errorText().isEmpty());

    ASSERT_TRUE(controller.openSecondaryImage(solidImage(Qt::blue), QStringLiteral("right")));
    controller.setCompareMode(ImageReviewController::Wipe);
    EXPECT_EQ(controller.compareMode(), static_cast<int>(ImageReviewController::Wipe));

    // The wipe position clamps to [0, 1] and defaults to the middle.
    EXPECT_DOUBLE_EQ(controller.wipePosition(), 0.5);
    controller.setWipePosition(2.0);
    EXPECT_DOUBLE_EQ(controller.wipePosition(), 1.0);
    controller.setWipePosition(-1.0);
    EXPECT_DOUBLE_EQ(controller.wipePosition(), 0.0);
    controller.setWipePosition(0.25);
    EXPECT_DOUBLE_EQ(controller.wipePosition(), 0.25);

    // Both slots still sample in wipe mode: the left side shows the secondary image.
    const QVariantMap left =
        controller.samplePixel(ImageReviewController::DisplaySecondarySlot, 0, 0);
    ASSERT_TRUE(left.value(QStringLiteral("valid")).toBool());
    EXPECT_EQ(left.value(QStringLiteral("b")).toInt(), 255);
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

TEST(ImageReviewControllerTests, AtomicPairOpenCommitsBothSidesWithIdentity) {
    ImageReviewController controller;
    ASSERT_TRUE(controller.openPairImages(solidImage(QColor(10, 20, 30)),
                                          QStringLiteral("a0.png"),
                                          solidImage(QColor(40, 50, 60)),
                                          QStringLiteral("b0.png"),
                                          /*pairId=*/2));
    EXPECT_TRUE(controller.hasPair());
    EXPECT_EQ(controller.committedPairId(), 2);
    EXPECT_EQ(controller.compareMode(), static_cast<int>(ImageReviewController::SideBySide));
    EXPECT_EQ(controller.primaryPath(), QStringLiteral("a0.png"));
    EXPECT_EQ(controller.secondaryPath(), QStringLiteral("b0.png"));
}

TEST(ImageReviewControllerTests, FailedPairKeepsPreviousPairFully) {
    ImageReviewController controller;
    ASSERT_TRUE(controller.openPairImages(solidImage(QColor(10, 20, 30)),
                                          QStringLiteral("a0.png"),
                                          solidImage(QColor(40, 50, 60)),
                                          QStringLiteral("b0.png"),
                                          /*pairId=*/2));
    const int generationBefore = controller.contentGeneration();

    // Corrupt B: the candidate must not mix a new A with the old B.
    EXPECT_FALSE(controller.openPairImages(solidImage(QColor(1, 1, 1)),
                                           QStringLiteral("a1.png"),
                                           QImage(),
                                           QStringLiteral("b1.png"),
                                           /*pairId=*/3));
    EXPECT_TRUE(controller.hasPair());
    EXPECT_EQ(controller.committedPairId(), 2);
    EXPECT_EQ(controller.primaryPath(), QStringLiteral("a0.png"));
    EXPECT_EQ(controller.secondaryPath(), QStringLiteral("b0.png"));
    EXPECT_EQ(controller.secondaryWidth(), 4);
    EXPECT_EQ(controller.contentGeneration(), generationBefore);
    EXPECT_FALSE(controller.errorText().isEmpty());

    // Corrupt A behaves identically: nothing changes.
    EXPECT_FALSE(controller.openPairImages(QImage(),
                                           QStringLiteral("a2.png"),
                                           solidImage(QColor(2, 2, 2)),
                                           QStringLiteral("b2.png"),
                                           /*pairId=*/3));
    EXPECT_TRUE(controller.hasPair());
    EXPECT_EQ(controller.committedPairId(), 2);
    EXPECT_EQ(controller.primaryPath(), QStringLiteral("a0.png"));
    EXPECT_EQ(controller.secondaryPath(), QStringLiteral("b0.png"));

    // A successful request switches the identity exactly once.
    const int generationMid = controller.contentGeneration();
    ASSERT_TRUE(controller.openPairImages(solidImage(QColor(3, 3, 3)),
                                          QStringLiteral("a3.png"),
                                          solidImage(QColor(4, 4, 4)),
                                          QStringLiteral("b3.png"),
                                          /*pairId=*/4));
    EXPECT_EQ(controller.committedPairId(), 4);
    EXPECT_EQ(controller.primaryPath(), QStringLiteral("a3.png"));
    EXPECT_EQ(controller.secondaryPath(), QStringLiteral("b3.png"));
    EXPECT_EQ(controller.contentGeneration(), generationMid + 1);
}

TEST(ImageReviewControllerTests, FailedFirstPairEntersExplicitMissingState) {
    ImageReviewController controller;
    EXPECT_FALSE(controller.openPairAtomically(
        QUrl::fromLocalFile(QStringLiteral("Z:/definitely/missing_a.png")),
        QUrl::fromLocalFile(QStringLiteral("Z:/definitely/missing_b.png")),
        /*pairId=*/0));
    EXPECT_FALSE(controller.hasPrimary());
    EXPECT_FALSE(controller.hasSecondary());
    EXPECT_FALSE(controller.hasPair());
    EXPECT_EQ(controller.committedPairId(), -1);
    EXPECT_FALSE(controller.errorText().isEmpty());
}

TEST(ImageReviewControllerTests, CloseAllResetsCommittedPairIdentity) {
    ImageReviewController controller;
    ASSERT_TRUE(controller.openPairImages(solidImage(Qt::red),
                                          QStringLiteral("a.png"),
                                          solidImage(Qt::blue),
                                          QStringLiteral("b.png"),
                                          /*pairId=*/5));
    controller.closeAll();
    EXPECT_EQ(controller.committedPairId(), -1);
    EXPECT_FALSE(controller.hasPair());
    EXPECT_FALSE(controller.hasDiffResult());
}

TEST(ImageReviewControllerTests, SecondaryOriginalsSurviveModesAndPrimarySwap) {
    ImageReviewController controller;
    ASSERT_TRUE(controller.openPairImages(solidImage(QColor(10, 20, 30)),
                                          QStringLiteral("a.png"),
                                          solidImage(QColor(40, 50, 60)),
                                          QStringLiteral("b.png"),
                                          /*pairId=*/0));
    const QImage secondaryBefore = controller.imageForSlot(ImageReviewController::SecondarySlot);

    // Equal-size diff previously resampled nothing, but switching modes must never touch
    // the originals regardless of geometry.
    controller.setCompareMode(ImageReviewController::AbsDifference);
    EXPECT_EQ(controller.secondaryWidth(), 4);
    EXPECT_EQ(controller.secondaryHeight(), 4);

    // Replacing only A keeps B's original pixels byte-identical.
    ASSERT_TRUE(controller.openPrimaryImage(solidImage(QColor(1, 2, 3)), QStringLiteral("a2.png")));
    EXPECT_EQ(controller.secondaryPath(), QStringLiteral("b.png"));
    EXPECT_EQ(controller.secondaryWidth(), 4);
    EXPECT_EQ(controller.secondaryHeight(), 4);
    EXPECT_TRUE(controller.imageForSlot(ImageReviewController::SecondarySlot) == secondaryBefore);
}

TEST(ImageReviewControllerTests, UnequalSizesGateDiffUntilResampleOptIn) {
    ImageReviewController controller;
    QImage big(6, 6, QImage::Format_ARGB32);
    big.fill(QColor(10, 20, 30));
    ASSERT_TRUE(controller.openPairImages(big,
                                          QStringLiteral("big.png"),
                                          solidImage(QColor(40, 50, 60)),
                                          QStringLiteral("small.png"),
                                          /*pairId=*/0));
    // The original small image is not stretched by the diff path.
    EXPECT_EQ(controller.secondaryWidth(), 4);
    EXPECT_EQ(controller.secondaryHeight(), 4);

    controller.setCompareMode(ImageReviewController::AbsDifference);
    EXPECT_FALSE(controller.hasDiffResult());
    EXPECT_FALSE(controller.diffResampled());
    EXPECT_EQ(controller.maxAbsDifference(), 0);
    EXPECT_FALSE(controller.errorText().isEmpty());

    // Side-by-side and wipe stay available without a diff or an error.
    controller.setCompareMode(ImageReviewController::Wipe);
    EXPECT_EQ(controller.compareMode(), static_cast<int>(ImageReviewController::Wipe));
    EXPECT_FALSE(controller.hasDiffResult());

    // Opting into resampling computes a labeled derived diff.
    controller.setResampleAllowed(true);
    controller.setCompareMode(ImageReviewController::AbsDifference);
    EXPECT_TRUE(controller.hasDiffResult());
    EXPECT_TRUE(controller.diffResampled());
    EXPECT_GT(controller.maxAbsDifference(), 0);
}

TEST(ImageReviewControllerTests, AlphaOnlyDifferenceIsNotReportedAsEqual) {
    ImageReviewController controller;
    QImage a(4, 4, QImage::Format_ARGB32);
    a.fill(QColor(20, 40, 60, 255));
    QImage b(4, 4, QImage::Format_ARGB32);
    b.fill(QColor(20, 40, 60, 128));
    ASSERT_TRUE(
        controller.openPairImages(a, QStringLiteral("a.png"), b, QStringLiteral("b.png"), 0));
    controller.setCompareMode(ImageReviewController::AbsDifference);
    ASSERT_TRUE(controller.hasDiffResult());
    EXPECT_EQ(controller.maxAbsDifference(), 0);
    EXPECT_TRUE(controller.alphaDifferenceOnly());
    EXPECT_TRUE(controller.diffScopeText().contains(QStringLiteral("RGBA8")));
}

TEST(ImageReviewControllerTests, SamplePixelReportsOriginalVersusDerivedSource) {
    ImageReviewController controller;
    ASSERT_TRUE(controller.openPairImages(solidImage(QColor(20, 40, 60)),
                                          QStringLiteral("a.png"),
                                          solidImage(QColor(20, 40, 80)),
                                          QStringLiteral("b.png"),
                                          /*pairId=*/0));
    controller.setCompareMode(ImageReviewController::AbsDifference);
    const QVariantMap original = controller.samplePixel(ImageReviewController::PrimarySlot, 0, 0);
    const QVariantMap derived =
        controller.samplePixel(ImageReviewController::DisplayDiffSlot, 0, 0);
    EXPECT_EQ(original.value(QStringLiteral("source")).toString(), QStringLiteral("original"));
    EXPECT_EQ(derived.value(QStringLiteral("source")).toString(), QStringLiteral("diff"));
}

} // namespace
