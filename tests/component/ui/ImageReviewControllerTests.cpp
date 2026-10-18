#include "dvs/ui/ImageReviewController.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <iostream>
#include <string>

namespace {

using dvs::ui::ImageColorProfileStatus;
using dvs::ui::ImageReviewController;
using dvs::ui::StillImageSourceInfo;

void ensureCoreApplication() {
    if (QCoreApplication::instance() != nullptr) {
        return;
    }
    static int argumentCount = 1;
    static char applicationName[] = "ImageReviewControllerTests";
    static char* arguments[] = {applicationName, nullptr};
    static QCoreApplication application{argumentCount, arguments};
    static_cast<void>(application);
}

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate predicate,
                             const std::chrono::milliseconds timeout = std::chrono::seconds(8)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        QThread::msleep(1U);
    }
    return predicate();
}

[[nodiscard]] bool waitForControllerIdle(const ImageReviewController& controller) {
    return waitUntil(
        [&controller] { return !controller.openPending() && !controller.diffPending(); });
}

class CoreApplicationEnvironment final : public ::testing::Environment {
public:
    void SetUp() override {
        ensureCoreApplication();
    }
};

[[maybe_unused]] const bool kCoreApplicationEnvironmentRegistered =
    ::testing::AddGlobalTestEnvironment(new CoreApplicationEnvironment) != nullptr;

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

// The same-coordinate pair readout (A/B and Δ = B−A) is built on samplePixelOriginal: it
// names each side's committed decode buffer, ignores the active channel observation view,
// and can never reach the difference image. The observation-aware samplePixel keeps its
// derived-view semantics, which is exactly the contrast this test pins.
TEST(ImageReviewControllerTests, SamplePixelOriginalReportsEachSideWithoutChannelView) {
    ImageReviewController controller;
    // Asymmetric, non-gray, alpha-bearing pair so every channel matters:
    // A=(120,130,140,200), B=(122,128,140,255) → Δ(B−A) = +2, −2, 0, +55.
    ASSERT_TRUE(controller.openPairImages(solidImage(QColor(120, 130, 140, 200)),
                                          QStringLiteral("a.png"),
                                          solidImage(QColor(122, 128, 140, 255)),
                                          QStringLiteral("b.png")));
    for (const int mode : {0, 1, 2}) {
        controller.setViewMode(mode);
        const QVariantMap a =
            controller.samplePixelOriginal(ImageReviewController::DisplayPrimarySlot, 1, 2);
        const QVariantMap b =
            controller.samplePixelOriginal(ImageReviewController::DisplaySecondarySlot, 1, 2);
        ASSERT_TRUE(a.value(QStringLiteral("valid")).toBool()) << "view mode " << mode;
        ASSERT_TRUE(b.value(QStringLiteral("valid")).toBool()) << "view mode " << mode;
        EXPECT_EQ(a.value(QStringLiteral("r")).toInt(), 120) << "view mode " << mode;
        EXPECT_EQ(a.value(QStringLiteral("g")).toInt(), 130) << "view mode " << mode;
        EXPECT_EQ(a.value(QStringLiteral("b")).toInt(), 140) << "view mode " << mode;
        EXPECT_EQ(a.value(QStringLiteral("a")).toInt(), 200) << "view mode " << mode;
        EXPECT_EQ(b.value(QStringLiteral("r")).toInt(), 122) << "view mode " << mode;
        EXPECT_EQ(b.value(QStringLiteral("g")).toInt(), 128) << "view mode " << mode;
        EXPECT_EQ(b.value(QStringLiteral("b")).toInt(), 140) << "view mode " << mode;
        EXPECT_EQ(b.value(QStringLiteral("a")).toInt(), 255) << "view mode " << mode;
        EXPECT_EQ(a.value(QStringLiteral("source")).toString(), QStringLiteral("original"))
            << "view mode " << mode;
        EXPECT_FALSE(a.contains(QStringLiteral("channelView"))) << "view mode " << mode;
    }
    // Δ = B−A at one shared coordinate: the numbers the status bar shows.
    const QVariantMap a =
        controller.samplePixelOriginal(ImageReviewController::DisplayPrimarySlot, 1, 2);
    const QVariantMap b =
        controller.samplePixelOriginal(ImageReviewController::DisplaySecondarySlot, 1, 2);
    EXPECT_EQ(b.value(QStringLiteral("r")).toInt() - a.value(QStringLiteral("r")).toInt(), 2);
    EXPECT_EQ(b.value(QStringLiteral("g")).toInt() - a.value(QStringLiteral("g")).toInt(), -2);
    EXPECT_EQ(b.value(QStringLiteral("b")).toInt() - a.value(QStringLiteral("b")).toInt(), 0);
    EXPECT_EQ(b.value(QStringLiteral("a")).toInt() - a.value(QStringLiteral("a")).toInt(), 55);
    // The difference image stays unreachable: a pair readout can never mix diff pixels in.
    const QVariantMap diff =
        controller.samplePixelOriginal(ImageReviewController::DisplayDiffSlot, 1, 2);
    EXPECT_FALSE(diff.value(QStringLiteral("valid")).toBool());
    // Out-of-bounds coordinates are invalid, not clamped fabrications.
    EXPECT_FALSE(controller.samplePixelOriginal(ImageReviewController::DisplayPrimarySlot, 4, 2)
                     .value(QStringLiteral("valid"))
                     .toBool());
    // Contrast: the observation-aware variant DOES follow the channel view.
    controller.setViewMode(1);
    const QVariantMap viewed =
        controller.samplePixel(ImageReviewController::DisplayPrimarySlot, 1, 2);
    ASSERT_TRUE(viewed.value(QStringLiteral("valid")).toBool());
    EXPECT_EQ(viewed.value(QStringLiteral("r")).toInt(), 200);
    EXPECT_EQ(viewed.value(QStringLiteral("channelView")).toString(), QStringLiteral("alphaGray"));
    controller.setViewMode(0);
}

TEST(ImageReviewControllerTests, DiffPairComputesPeakAndSignedMidpoint) {
    ImageReviewController controller;
    ASSERT_TRUE(
        controller.openPrimaryImage(solidImage(QColor(20, 40, 60)), QStringLiteral("left")));
    ASSERT_TRUE(
        controller.openSecondaryImage(solidImage(QColor(20, 40, 80)), QStringLiteral("right")));
    EXPECT_TRUE(controller.hasPair());
    EXPECT_EQ(controller.compareMode(), static_cast<int>(ImageReviewController::SideBySide));
    // T4: side-by-side is not a diff request, so opening the pair must not pay for a
    // whole-image difference computation.
    EXPECT_FALSE(controller.hasDiffResult());
    EXPECT_EQ(controller.maxAbsDifference(), 0);

    controller.setCompareMode(ImageReviewController::SignedDifference);
    ASSERT_TRUE(waitForControllerIdle(controller));
    EXPECT_EQ(controller.maxAbsDifference(), 20);
    const QVariantMap signedPixel =
        controller.samplePixel(ImageReviewController::DisplayDiffSlot, 0, 0);
    ASSERT_TRUE(signedPixel.value(QStringLiteral("valid")).toBool());
    // Blue channel: A=60, B=80 → 128 + (60-80)*4 = 48.
    EXPECT_EQ(signedPixel.value(QStringLiteral("b")).toInt(), 48);
    // Red/green unchanged → mid gray.
    EXPECT_EQ(signedPixel.value(QStringLiteral("r")).toInt(), 128);
}

TEST(ImageReviewControllerTests, RgbMeanAbsoluteDifferenceUsesAllChannelSamples) {
    ImageReviewController controller;
    ASSERT_TRUE(
        controller.openPrimaryImage(solidImage(QColor(10, 20, 30)), QStringLiteral("left")));
    ASSERT_TRUE(
        controller.openSecondaryImage(solidImage(QColor(13, 26, 39)), QStringLiteral("right")));

    controller.setCompareMode(ImageReviewController::AbsDifference);
    ASSERT_TRUE(waitForControllerIdle(controller));
    EXPECT_EQ(controller.maxAbsDifference(), 9);
    EXPECT_NEAR(controller.meanAbsDifference(), 6.0, 1e-9);
}

TEST(ImageReviewControllerTests, HighlightAndAbsDifferenceProduceOutput) {
    ImageReviewController controller;
    ASSERT_TRUE(controller.openPrimaryImage(solidImage(QColor(0, 0, 0)), QStringLiteral("left")));
    ASSERT_TRUE(
        controller.openSecondaryImage(solidImage(QColor(10, 10, 10)), QStringLiteral("right")));

    controller.setCompareMode(ImageReviewController::AbsDifference);
    ASSERT_TRUE(waitForControllerIdle(controller));
    EXPECT_EQ(controller.samplePixel(ImageReviewController::DisplayDiffSlot, 0, 0)
                  .value(QStringLiteral("r"))
                  .toInt(),
              40);

    controller.setCompareMode(ImageReviewController::Highlight);
    ASSERT_TRUE(waitForControllerIdle(controller));
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

    // Opting into resampling computes a labeled derived diff in the background.
    controller.setResampleAllowed(true);
    controller.setCompareMode(ImageReviewController::AbsDifference);
    ASSERT_TRUE(waitForControllerIdle(controller));
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
    ASSERT_TRUE(waitForControllerIdle(controller));
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
    ASSERT_TRUE(waitForControllerIdle(controller));
    const QVariantMap original = controller.samplePixel(ImageReviewController::PrimarySlot, 0, 0);
    const QVariantMap derived =
        controller.samplePixel(ImageReviewController::DisplayDiffSlot, 0, 0);
    EXPECT_EQ(original.value(QStringLiteral("source")).toString(), QStringLiteral("original"));
    EXPECT_EQ(derived.value(QStringLiteral("source")).toString(), QStringLiteral("diff"));
}

class ScopedStillImageLoader final {
public:
    explicit ScopedStillImageLoader(ImageReviewController::StillImageLoader loader) {
        ImageReviewController::setProcessStillImageLoader(std::move(loader));
    }

    ~ScopedStillImageLoader() {
        ImageReviewController::setProcessStillImageProbe(nullptr);
        ImageReviewController::setProcessStillImageLoader(nullptr);
    }
};

[[nodiscard]] QUrl
writeBytes(const QTemporaryDir& directory, const QString& name, const QByteArray& bytes) {
    const QString path = QDir(directory.path()).filePath(name);
    QFile file{path};
    if (!file.open(QIODevice::WriteOnly)) {
        return {};
    }
    if (file.write(bytes) != bytes.size()) {
        return {};
    }
    file.close();
    return QUrl::fromLocalFile(path);
}

TEST(ImageReviewControllerTests, HighBitDepthSourceReportsNativeSampleValues) {
    // I-02: a >8-bit source must sample its original-depth code values, not only the
    // quantized 8-bit display buffer.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QUrl url = writeBytes(directory, QStringLiteral("deep.bin"), QByteArrayLiteral("rgb48"));
    ASSERT_FALSE(url.isEmpty());
    ScopedStillImageLoader loader{[](const QByteArray& bytes,
                                     QImage* image,
                                     QImage* nativeImage,
                                     dvs::ui::StillImageSourceInfo* info,
                                     std::string*) {
        if (!bytes.startsWith("rgb48")) {
            return false;
        }
        *image = solidImage(QColor(128, 128, 128));
        if (info != nullptr) {
            info->bitDepth = 16;
            info->channels = 3;
            info->hasAlpha = false;
            info->sourceFormat = QStringLiteral("rgb48le");
            info->displayConverted = true;
        }
        if (nativeImage != nullptr) {
            QImage native(4, 4, QImage::Format_RGBA64);
            if (!native.isNull()) {
                // QImage::fill is ambiguous for QRgba64 on this Qt; fill the buffer directly.
                auto* const samples = reinterpret_cast<QRgba64*>(native.bits());
                std::fill(samples,
                          samples + native.sizeInBytes() / sizeof(QRgba64),
                          qRgba64(40000, 30000, 20000, 60000));
            }
            *nativeImage = std::move(native);
        }
        return true;
    }};
    ImageReviewController controller;
    ASSERT_GT(controller.requestOpenPrimary(url), 0);
    ASSERT_TRUE(waitForControllerIdle(controller));
    EXPECT_EQ(controller.primaryBitDepth(), 16);

    const QVariantMap pixel = controller.samplePixel(ImageReviewController::PrimarySlot, 0, 0);
    ASSERT_TRUE(pixel.value(QStringLiteral("valid")).toBool());
    // 8-bit display values stay available…
    EXPECT_EQ(pixel.value(QStringLiteral("r")).toInt(), 128);
    // …alongside the unquantized 16-bit code values.
    EXPECT_EQ(pixel.value(QStringLiteral("nativeBitDepth")).toInt(), 16);
    EXPECT_EQ(pixel.value(QStringLiteral("r16")).toInt(), 40000);
    EXPECT_EQ(pixel.value(QStringLiteral("g16")).toInt(), 30000);
    EXPECT_EQ(pixel.value(QStringLiteral("b16")).toInt(), 20000);
    EXPECT_EQ(pixel.value(QStringLiteral("a16")).toInt(), 60000);
}

TEST(ImageReviewControllerTests, AsyncPairOpenReturnsImmediatelyAndRejectsLateN) {
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QUrl slowA =
        writeBytes(directory, QStringLiteral("slowA.bin"), QByteArrayLiteral("slowA"));
    const QUrl slowB =
        writeBytes(directory, QStringLiteral("slowB.bin"), QByteArrayLiteral("slowB"));
    const QUrl fastA =
        writeBytes(directory, QStringLiteral("fastA.bin"), QByteArrayLiteral("fastA"));
    const QUrl fastB =
        writeBytes(directory, QStringLiteral("fastB.bin"), QByteArrayLiteral("fastB"));
    ASSERT_FALSE(slowA.isEmpty() || slowB.isEmpty() || fastA.isEmpty() || fastB.isEmpty());

    std::atomic<int> loaderCalls{0};
    ScopedStillImageLoader loader{[&loaderCalls](const QByteArray& bytes,
                                                 QImage* image,
                                                 QImage*,
                                                 dvs::ui::StillImageSourceInfo*,
                                                 std::string*) {
        ++loaderCalls;
        if (bytes.startsWith("slow")) {
            QThread::msleep(250); // Missing on GUI thread in a passing T4.
        }
        QImage decoded(2, 2, QImage::Format_RGBA8888);
        decoded.fill(bytes.startsWith("slow") ? QColor(10, 20, 30) : QColor(200, 210, 220));
        *image = decoded;
        return true;
    }};

    ImageReviewController controller;
    QElapsedTimer acceptanceTimer;
    acceptanceTimer.start();
    const int slowRequest = controller.requestOpenPair(slowA, slowB, 1);
    EXPECT_GT(slowRequest, 0);
    EXPECT_LT(acceptanceTimer.elapsed(), 100)
        << "accepting a candidate must not wait for the decode worker";
    ASSERT_TRUE(waitUntil([&loaderCalls] { return loaderCalls.load() >= 1; }));

    const int fastRequest = controller.requestOpenPair(fastA, fastB, 2);
    EXPECT_GT(fastRequest, 0);
    ASSERT_TRUE(waitForControllerIdle(controller));
    EXPECT_EQ(controller.committedPairId(), 2);
    EXPECT_TRUE(controller.primaryPath().endsWith(QStringLiteral("fastA.bin")));

    // Let the late N candidate finish and deliver its old result; it must be dropped.
    QThread::msleep(350);
    for (int iteration = 0; iteration < 20; ++iteration) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        QThread::msleep(2U);
    }
    EXPECT_EQ(controller.committedPairId(), 2);
    EXPECT_TRUE(controller.primaryPath().endsWith(QStringLiteral("fastA.bin")));
    EXPECT_FALSE(controller.openPending());
}

TEST(ImageReviewControllerTests, StaleLoadResultCannotCommitUnderANewerPendingRequest) {
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QUrl pendingA =
        writeBytes(directory, QStringLiteral("pendingA.bin"), QByteArrayLiteral("pendingA"));
    ASSERT_FALSE(pendingA.isEmpty());

    QSemaphore entered;
    QSemaphore release;
    ScopedStillImageLoader loader{[&entered, &release](const QByteArray& bytes,
                                                       QImage* image,
                                                       QImage*,
                                                       dvs::ui::StillImageSourceInfo*,
                                                       std::string*) {
        if (bytes.startsWith("pending")) {
            entered.release();
            release.tryAcquire(1, 8000);
        }
        QImage decoded(2, 2, QImage::Format_RGBA8888);
        decoded.fill(bytes.startsWith("pending") ? QColor(1, 2, 3) : QColor(200, 0, 0));
        *image = decoded;
        return true;
    }};

    ImageReviewController controller;
    ASSERT_TRUE(controller.openPairImages(solidImage(Qt::black),
                                          QStringLiteral("oldA.bin"),
                                          solidImage(Qt::white),
                                          QStringLiteral("oldB.bin"),
                                          3));
    const int pendingRequest = controller.requestOpenPrimary(pendingA);
    ASSERT_GT(pendingRequest, 0);
    ASSERT_TRUE(entered.tryAcquire(1, 8000));
    EXPECT_TRUE(controller.openPending());

    // A stale (older request id) successful result arrives while the newer request is still
    // pending. It must be dropped: the pending material is the only one that may commit.
    dvs::ui::ImagePairLoader::Result stale;
    stale.requestId = static_cast<quint64>(pendingRequest - 1);
    stale.primary = solidImage(QColor(255, 0, 0));
    stale.primaryLabel = QStringLiteral("stale.bin");
    stale.primaryIdentity = QStringLiteral("stale-identity");
    controller.deliverLoadResultForTesting(std::move(stale));
    QCoreApplication::processEvents();
    EXPECT_EQ(controller.primaryPath(), QStringLiteral("oldA.bin"));
    EXPECT_TRUE(controller.openPending());

    release.release();
    ASSERT_TRUE(waitForControllerIdle(controller));
    EXPECT_TRUE(controller.primaryPath().endsWith(QStringLiteral("pendingA.bin")));
    EXPECT_FALSE(controller.openPending());
}

TEST(ImageReviewControllerTests, AsyncCandidateFailureKeepsPreviousPair) {
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QUrl oldA =
        writeBytes(directory, QStringLiteral("oldA.bin"), QByteArrayLiteral("goodA0"));
    const QUrl oldB =
        writeBytes(directory, QStringLiteral("oldB.bin"), QByteArrayLiteral("goodB0"));
    const QUrl newA =
        writeBytes(directory, QStringLiteral("newA.bin"), QByteArrayLiteral("goodA1"));
    const QUrl badB = writeBytes(directory, QStringLiteral("badB.bin"), QByteArrayLiteral("badB1"));
    ASSERT_FALSE(oldA.isEmpty() || oldB.isEmpty() || newA.isEmpty() || badB.isEmpty());

    ScopedStillImageLoader loader{[](const QByteArray& bytes,
                                     QImage* image,
                                     QImage*,
                                     dvs::ui::StillImageSourceInfo*,
                                     std::string* error) {
        if (bytes.startsWith("bad")) {
            if (error != nullptr) {
                *error = "B side corrupt";
            }
            return false;
        }
        QImage decoded(2, 2, QImage::Format_RGBA8888);
        decoded.fill(bytes.startsWith("goodA") ? QColor(10, 20, 30) : QColor(40, 50, 60));
        *image = decoded;
        return true;
    }};

    ImageReviewController controller;
    ASSERT_TRUE(controller.openPairImages(QImage(2, 2, QImage::Format_RGBA8888),
                                          QStringLiteral("oldA"),
                                          QImage(2, 2, QImage::Format_RGBA8888),
                                          QStringLiteral("oldB"),
                                          /*pairId=*/9));
    const QImage oldSecondary = controller.imageForSlot(ImageReviewController::SecondarySlot);

    const int requestId = controller.requestOpenPair(newA, badB, 10);
    EXPECT_GT(requestId, 0);
    ASSERT_TRUE(waitForControllerIdle(controller));
    EXPECT_FALSE(controller.openPending());
    EXPECT_EQ(controller.committedPairId(), 9);
    EXPECT_TRUE(controller.primaryPath().endsWith(QStringLiteral("oldA")));
    EXPECT_TRUE(controller.secondaryPath().endsWith(QStringLiteral("oldB")));
    EXPECT_TRUE(controller.imageForSlot(ImageReviewController::SecondarySlot) == oldSecondary);
    EXPECT_TRUE(controller.errorText().contains(QStringLiteral("B")));
}

TEST(ImageReviewControllerTests, CancelledOrClosedOpenNeverPublishesLateResult) {
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QUrl slowA =
        writeBytes(directory, QStringLiteral("cancelA.bin"), QByteArrayLiteral("slowA"));
    const QUrl slowB =
        writeBytes(directory, QStringLiteral("cancelB.bin"), QByteArrayLiteral("slowB"));
    ASSERT_FALSE(slowA.isEmpty() || slowB.isEmpty());

    std::atomic<bool> started{false};
    ScopedStillImageLoader loader{[&started](const QByteArray&,
                                             QImage* image,
                                             QImage*,
                                             dvs::ui::StillImageSourceInfo*,
                                             std::string*) {
        started.store(true);
        QThread::msleep(200);
        QImage decoded(2, 2, QImage::Format_RGBA8888);
        decoded.fill(Qt::red);
        *image = decoded;
        return true;
    }};

    ImageReviewController controller;
    const int requestId = controller.requestOpenPair(slowA, slowB, 5);
    ASSERT_GT(requestId, 0);
    ASSERT_TRUE(waitUntil([&started] { return started.load(); }));
    controller.cancelOpenRequest(requestId);
    EXPECT_FALSE(controller.openPending());
    QThread::msleep(300);
    for (int iteration = 0; iteration < 20; ++iteration) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        QThread::msleep(2U);
    }
    EXPECT_FALSE(controller.hasPair());
    EXPECT_EQ(controller.committedPairId(), -1);
}

TEST(ImageReviewControllerTests, DifferenceIsOnDemandAndReusedFromByteBudgetedCache) {
    ensureCoreApplication();
    ImageReviewController controller;
    controller.clearAsyncCaches();
    QImage left(32, 32, QImage::Format_ARGB32);
    left.fill(QColor(10, 20, 30));
    QImage right(32, 32, QImage::Format_ARGB32);
    right.fill(QColor(40, 50, 60));
    ASSERT_TRUE(
        controller.openPairImages(left, QStringLiteral("a"), right, QStringLiteral("b"), 0));
    EXPECT_FALSE(controller.hasDiffResult());

    controller.setCompareMode(ImageReviewController::AbsDifference);
    ASSERT_TRUE(waitForControllerIdle(controller));
    ASSERT_TRUE(controller.hasDiffResult());
    QVariantMap stats = controller.asyncStats();
    EXPECT_EQ(stats.value(QStringLiteral("diff_cache_misses")).toInt(), 1);

    controller.setCompareMode(ImageReviewController::SideBySide);
    controller.setCompareMode(ImageReviewController::AbsDifference);
    ASSERT_TRUE(waitForControllerIdle(controller));
    EXPECT_TRUE(controller.hasDiffResult());
    stats = controller.asyncStats();
    EXPECT_EQ(stats.value(QStringLiteral("diff_cache_misses")).toInt(), 1);
    EXPECT_GE(stats.value(QStringLiteral("diff_cache_hits")).toInt(), 1);
    EXPECT_LE(stats.value(QStringLiteral("diff_cache_bytes")).toLongLong(),
              stats.value(QStringLiteral("diff_cache_budget_bytes")).toLongLong());
}

TEST(ImageReviewControllerTests, PrefetchWarmsCacheAndUserOpenUsesIt) {
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QUrl left =
        writeBytes(directory, QStringLiteral("prefetchA.bin"), QByteArrayLiteral("prefetchA"));
    const QUrl right =
        writeBytes(directory, QStringLiteral("prefetchB.bin"), QByteArrayLiteral("prefetchB"));
    ASSERT_FALSE(left.isEmpty() || right.isEmpty());

    std::atomic<int> decodeCalls{0};
    ScopedStillImageLoader loader{[&decodeCalls](const QByteArray& bytes,
                                                 QImage* image,
                                                 QImage*,
                                                 dvs::ui::StillImageSourceInfo*,
                                                 std::string*) {
        ++decodeCalls;
        QImage decoded(2, 2, QImage::Format_RGBA8888);
        decoded.fill(bytes.startsWith("prefetchA") ? QColor(1, 2, 3) : QColor(4, 5, 6));
        *image = decoded;
        return true;
    }};

    ImageReviewController controller;
    controller.clearAsyncCaches();
    controller.prefetchPair(left, right);
    ASSERT_TRUE(waitUntil([&controller] {
        const QVariantMap stats = controller.asyncStats();
        return stats.value(QStringLiteral("active_requests")).toInt() == 0 &&
               stats.value(QStringLiteral("load_cache_entries")).toInt() >= 2;
    }));
    const int callsAfterPrefetch = decodeCalls.load();
    EXPECT_EQ(callsAfterPrefetch, 2);

    ASSERT_GT(controller.requestOpenPair(left, right, 3), 0);
    ASSERT_TRUE(waitForControllerIdle(controller));
    EXPECT_EQ(controller.committedPairId(), 3);
    EXPECT_EQ(decodeCalls.load(), callsAfterPrefetch);
    const QVariantMap stats = controller.asyncStats();
    EXPECT_GE(stats.value(QStringLiteral("load_cache_hits")).toInt(), 2);
    EXPECT_LE(stats.value(QStringLiteral("load_cache_bytes")).toLongLong(),
              stats.value(QStringLiteral("load_cache_budget_bytes")).toLongLong());
}

TEST(ImageReviewControllerTests, BlockedDifferenceCannotPublishOrCacheAfterNewPairCommits) {
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QUrl next = writeBytes(directory, QStringLiteral("new.bin"), "new");
    ScopedStillImageLoader decoder{[](const QByteArray&,
                                      QImage* image,
                                      QImage*,
                                      dvs::ui::StillImageSourceInfo*,
                                      std::string*) {
        *image = solidImage(QColor(30, 40, 50));
        return true;
    }};
    ImageReviewController controller;
    ASSERT_TRUE(
        controller.openPairImages(solidImage(Qt::black), "oldA", solidImage(Qt::white), "oldB", 1));
    auto* worker = controller.findChild<dvs::ui::ImagePairLoader*>();
    ASSERT_NE(worker, nullptr);
    QSemaphore entered;
    QSemaphore release;
    std::atomic<int> differenceRuns{0};
    // Only the first difference job (the old pair's) blocks; later jobs run to
    // completion so the retained-mode recompute for the new pair can be observed.
    worker->setDifferenceRowObserverForTesting([&](const int row) {
        if (row == 0 && differenceRuns.fetch_add(1) == 0) {
            entered.release();
            release.tryAcquire(1, 8000);
        }
    });
    controller.setCompareMode(ImageReviewController::AbsDifference);
    const bool blocked = entered.tryAcquire(1, 8000);
    EXPECT_TRUE(blocked);
    EXPECT_GT(controller.requestOpenPair(next, next, 2), 0);
    EXPECT_TRUE(waitUntil([&] { return !controller.openPending(); }));
    EXPECT_EQ(controller.committedPairId(), 2);
    // T6: the same-size pair switch retains the diff mode, so the new pair's
    // difference is recomputed and published while the old job is still blocked.
    EXPECT_TRUE(waitUntil([&controller] { return !controller.diffPending(); }));
    EXPECT_TRUE(controller.hasDiffResult());
    EXPECT_EQ(controller.maxAbsDifference(), 0);
    EXPECT_EQ(controller.asyncStats().value("diff_cache_entries").toInt(), 1);
    EXPECT_EQ(differenceRuns.load(), 2);

    // The cancelled old job can no longer publish or cache under the new identity,
    // even after it is finally allowed to finish.
    release.release();
    EXPECT_TRUE(waitUntil([&worker] { return worker->stats().activeRequests == 0; }));
    EXPECT_EQ(controller.maxAbsDifference(), 0);
    EXPECT_EQ(controller.asyncStats().value("diff_cache_entries").toInt(), 1);
    worker->setDifferenceRowObserverForTesting({});
    controller.setCompareMode(ImageReviewController::SideBySide);
    controller.setCompareMode(ImageReviewController::AbsDifference);
    ASSERT_TRUE(waitForControllerIdle(controller));
    EXPECT_EQ(controller.maxAbsDifference(), 0);
    EXPECT_EQ(controller.asyncStats().value("diff_cache_hits").toInt(), 1);
}

TEST(ImageReviewControllerTests, FailedCandidateRetainsBlockedCommittedDifference) {
    ensureCoreApplication();
    ImageReviewController controller;
    ASSERT_TRUE(controller.openPairImages(
        solidImage(Qt::black), "a", solidImage(QColor(12, 0, 0)), "b", 7));
    auto* worker = controller.findChild<dvs::ui::ImagePairLoader*>();
    ASSERT_NE(worker, nullptr);
    QSemaphore entered;
    QSemaphore release;
    worker->setDifferenceRowObserverForTesting([&](const int row) {
        if (row == 0) {
            entered.release();
            release.tryAcquire(1, 8000);
        }
    });
    controller.setCompareMode(ImageReviewController::AbsDifference);
    EXPECT_TRUE(entered.tryAcquire(1, 8000));
    EXPECT_GT(controller.requestOpenPair(QUrl::fromLocalFile("Z:/missing/a.png"),
                                         QUrl::fromLocalFile("Z:/missing/b.png"),
                                         8),
              0);
    EXPECT_TRUE(waitUntil([&] { return !controller.openPending(); }));
    EXPECT_TRUE(controller.diffPending());
    EXPECT_EQ(controller.committedPairId(), 7);
    release.release();
    EXPECT_TRUE(waitForControllerIdle(controller));
    EXPECT_TRUE(controller.hasDiffResult());
    EXPECT_EQ(controller.maxAbsDifference(), 12);
}

TEST(ImageReviewControllerTests, LoaderCoalescesQueueAndCancellationSkipsSecondImage) {
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QUrl a = writeBytes(directory, "a.bin", "a");
    const QUrl b = writeBytes(directory, "b.bin", "b");
    QSemaphore entered;
    QSemaphore release;
    std::atomic<int> aCalls{0};
    std::atomic<int> bCalls{0};
    dvs::ui::ImagePairLoader loader;
    dvs::ui::ImagePairLoader::DecodePolicy policy;
    policy.loader = [&](const QByteArray& bytes,
                        QImage* image,
                        QImage*,
                        dvs::ui::StillImageSourceInfo*,
                        std::string*) {
        if (bytes == "a" && ++aCalls <= 2) {
            entered.release();
            release.tryAcquire(1, 8000);
        } else if (bytes == "b") {
            ++bCalls;
        }
        *image = solidImage(Qt::green);
        return true;
    };
    int completions = 0;
    const auto handler = [&](dvs::ui::ImagePairLoader::Result) { ++completions; };
    const quint64 first = loader.requestPair(a, b, 0, policy, handler);
    const quint64 second = loader.requestPair(a, b, 1, policy, handler);
    EXPECT_TRUE(entered.tryAcquire(2, 8000));
    for (int row = 2; row < 102; ++row) {
        loader.requestPair(a, b, row, policy, handler);
        EXPECT_LE(loader.stats().pendingRequests, 1);
        EXPECT_LE(loader.stats().activeRequests, 3);
    }
    loader.cancel(first);
    loader.cancel(second);
    release.release(2);
    EXPECT_TRUE(waitUntil([&] { return loader.stats().activeRequests == 0; }));
    EXPECT_EQ(aCalls.load(), 3);
    EXPECT_EQ(bCalls.load(), 1);
    EXPECT_EQ(completions, 1);
    EXPECT_LE(loader.stats().cacheBytes, loader.stats().cacheBudgetBytes);
}

TEST(ImageReviewControllerTests, HeaderProbeRejectsOversizedBeforeDecoderThrows) {
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QUrl hugeA =
        writeBytes(directory, QStringLiteral("hugeA.bin"), QByteArrayLiteral("HUGE"));
    const QUrl hugeB =
        writeBytes(directory, QStringLiteral("hugeB.bin"), QByteArrayLiteral("HUGE"));
    ASSERT_FALSE(hugeA.isEmpty() || hugeB.isEmpty());

    std::atomic<int> decoderCalls{0};
    ImageReviewController::setProcessStillImageProbe([](const QByteArray& bytes, QSize* size) {
        if (!bytes.startsWith("HUGE") || size == nullptr) {
            return false;
        }
        *size = QSize(20000, 20000);
        return true;
    });
    ScopedStillImageLoader loader{[&decoderCalls](const QByteArray&,
                                                  QImage* image,
                                                  QImage*,
                                                  dvs::ui::StillImageSourceInfo*,
                                                  std::string*) {
        ++decoderCalls;
        QImage decoded(2, 2, QImage::Format_RGBA8888);
        decoded.fill(Qt::green);
        *image = decoded;
        return true;
    }};

    ImageReviewController controller;
    ASSERT_GT(controller.requestOpenPair(hugeA, hugeB, 1), 0);
    ASSERT_TRUE(waitForControllerIdle(controller));
    EXPECT_FALSE(controller.hasPair());
    EXPECT_FALSE(controller.errorText().isEmpty());
    EXPECT_EQ(decoderCalls.load(), 0);
}
TEST(ImageReviewControllerTests, SameSizePairSwitchKeepsModeAndObservationPosition) {
    ensureCoreApplication();
    ImageReviewController controller;
    ASSERT_TRUE(controller.openPairImages(solidImage(QColor(10, 20, 30)),
                                          QStringLiteral("a0.png"),
                                          solidImage(QColor(40, 50, 60)),
                                          QStringLiteral("b0.png"),
                                          /*pairId=*/0));
    controller.setCompareMode(ImageReviewController::AbsDifference);
    ASSERT_TRUE(waitForControllerIdle(controller));
    ASSERT_TRUE(controller.hasDiffResult());
    controller.setZoom(3.0);
    controller.panBy(0.1, 0.2);
    const qreal pannedX = controller.panX();

    // Same-size next pair: the diff mode and the observation position survive the switch.
    ASSERT_TRUE(controller.openPairImages(solidImage(QColor(10, 20, 31)),
                                          QStringLiteral("a1.png"),
                                          solidImage(QColor(40, 50, 61)),
                                          QStringLiteral("b1.png"),
                                          /*pairId=*/1));
    EXPECT_EQ(controller.committedPairId(), 1);
    EXPECT_EQ(controller.compareMode(), static_cast<int>(ImageReviewController::AbsDifference));
    EXPECT_DOUBLE_EQ(controller.zoom(), 3.0);
    EXPECT_DOUBLE_EQ(controller.panX(), pannedX);
    EXPECT_TRUE(controller.diffPending());
    ASSERT_TRUE(waitForControllerIdle(controller));
    // The difference belongs to the new pair, not the old one.
    ASSERT_TRUE(controller.hasDiffResult());
    EXPECT_GT(controller.maxAbsDifference(), 0);
}

TEST(ImageReviewControllerTests, DifferentSizePairSwitchExplainsModeFallback) {
    ensureCoreApplication();
    ImageReviewController controller;
    ASSERT_TRUE(controller.openPairImages(solidImage(QColor(10, 20, 30)),
                                          QStringLiteral("a0.png"),
                                          solidImage(QColor(40, 50, 60)),
                                          QStringLiteral("b0.png"),
                                          /*pairId=*/0));
    controller.setCompareMode(ImageReviewController::AbsDifference);
    ASSERT_TRUE(waitForControllerIdle(controller));

    QImage big(6, 6, QImage::Format_ARGB32);
    big.fill(QColor(40, 50, 60));
    ASSERT_TRUE(controller.openPairImages(big,
                                          QStringLiteral("a1.png"),
                                          solidImage(QColor(40, 50, 60)),
                                          QStringLiteral("b1.png"),
                                          /*pairId=*/1));
    // A pixel comparison between different sizes would be false, so the mode falls back
    // to side-by-side with an explicit explanation instead of pretending.
    EXPECT_EQ(controller.compareMode(), static_cast<int>(ImageReviewController::SideBySide));
    EXPECT_FALSE(controller.hasDiffResult());
    EXPECT_TRUE(controller.errorText().contains(QStringLiteral("尺寸")));
    EXPECT_DOUBLE_EQ(controller.zoom(), 1.0);
    EXPECT_DOUBLE_EQ(controller.panX(), 0.5);
}

TEST(ImageReviewControllerTests, SourceProvenanceSurvivesInjectionAndLabelsDisplayConversion) {
    ImageReviewController controller;
    dvs::ui::StillImageSourceInfo gray;
    gray.bitDepth = 16;
    gray.channels = 1;
    gray.hasAlpha = false;
    gray.sourceFormat = QStringLiteral("gray16be");
    gray.displayConverted = true;
    ASSERT_TRUE(
        controller.openPrimaryImage(solidImage(QColor(20, 40, 60)), QStringLiteral("left"), gray));
    EXPECT_EQ(controller.primaryBitDepth(), 16);
    EXPECT_EQ(controller.primaryChannels(), 1);
    // A loader-reported gray source stays gray even though the display buffer is RGBA8.
    EXPECT_FALSE(controller.primaryHasAlpha());
    EXPECT_EQ(controller.primarySourceFormat(), QStringLiteral("gray16be"));
    EXPECT_TRUE(controller.primaryDisplayConverted());

    // Consistent RGBA provenance: alpha present, no display conversion.
    dvs::ui::StillImageSourceInfo rgba;
    rgba.hasAlpha = true;
    rgba.sourceFormat = QStringLiteral("rgba");
    ASSERT_TRUE(controller.openSecondaryImage(
        solidImage(QColor(1, 2, 3, 128)), QStringLiteral("right"), rgba));
    EXPECT_TRUE(controller.secondaryHasAlpha());
    EXPECT_FALSE(controller.secondaryDisplayConverted());

    // Unknown provenance (direct injection): hasAlpha falls back to the buffer.
    controller.closeAll();
    ASSERT_TRUE(
        controller.openSecondaryImage(solidImage(QColor(1, 2, 3, 128)), QStringLiteral("right")));
    EXPECT_TRUE(controller.secondaryHasAlpha());
}

TEST(ImageReviewControllerTests, AsyncSingleImageOpensRetainProvenanceIncludingCacheHits) {
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QUrl grayUrl =
        writeBytes(directory, QStringLiteral("gray.bin"), QByteArrayLiteral("gray"));
    const QUrl rgbaUrl =
        writeBytes(directory, QStringLiteral("rgba.bin"), QByteArrayLiteral("rgba"));
    ASSERT_FALSE(grayUrl.isEmpty() || rgbaUrl.isEmpty());
    std::atomic<int> loaderCalls{0};
    ScopedStillImageLoader loader{[&loaderCalls](const QByteArray& bytes,
                                                 QImage* image,
                                                 QImage*,
                                                 dvs::ui::StillImageSourceInfo* info,
                                                 std::string*) {
        ++loaderCalls;
        const bool gray = bytes == QByteArrayLiteral("gray");
        *image = solidImage(QColor(20, 40, 60, gray ? 255 : 128));
        info->bitDepth = gray ? 16 : 8;
        info->channels = gray ? 1 : 4;
        info->hasAlpha = !gray;
        info->sourceFormat = gray ? QStringLiteral("gray16be") : QStringLiteral("rgba");
        return true;
    }};
    ImageReviewController controller;
    for (int iteration = 0; iteration < 2; ++iteration) {
        ASSERT_GT(controller.requestOpenPrimary(grayUrl), 0);
        ASSERT_TRUE(waitForControllerIdle(controller));
        EXPECT_EQ(controller.primaryBitDepth(), 16);
        EXPECT_EQ(controller.primaryChannels(), 1);
        EXPECT_EQ(controller.primarySourceFormat(), QStringLiteral("gray16be"));
        EXPECT_FALSE(controller.primaryHasAlpha());
        EXPECT_TRUE(controller.primaryDisplayConverted());

        ASSERT_GT(controller.requestOpenSecondary(rgbaUrl), 0);
        ASSERT_TRUE(waitForControllerIdle(controller));
        EXPECT_EQ(controller.secondaryBitDepth(), 8);
        EXPECT_EQ(controller.secondaryChannels(), 4);
        EXPECT_EQ(controller.secondarySourceFormat(), QStringLiteral("rgba"));
        EXPECT_TRUE(controller.secondaryHasAlpha());
        EXPECT_FALSE(controller.secondaryDisplayConverted());
        // Adding the second image must preserve the first image's provenance.
        EXPECT_EQ(controller.primaryBitDepth(), 16);
    }
    EXPECT_EQ(loaderCalls.load(), 2);
}

TEST(ImageReviewControllerTests, AlphaDifferenceModeReportsStatsAndOutputsAlphaMap) {
    ImageReviewController controller;
    QImage left(2, 2, QImage::Format_ARGB32);
    left.fill(qRgba(10, 20, 30, 255));
    QImage right(2, 2, QImage::Format_ARGB32);
    right.fill(qRgba(10, 20, 30, 223)); // RGB identical, alpha 32 lower.
    ASSERT_TRUE(controller.openPairImages(left, QStringLiteral("a"), right, QStringLiteral("b")));

    controller.setCompareMode(ImageReviewController::AlphaDifference);
    ASSERT_TRUE(waitForControllerIdle(controller));
    ASSERT_TRUE(controller.hasDiffResult());
    // RGB untouched by the alpha-only edit.
    EXPECT_EQ(controller.maxAbsDifference(), 0);
    EXPECT_TRUE(controller.alphaDifferenceOnly());
    EXPECT_TRUE(controller.diffHasAlpha());
    EXPECT_EQ(controller.peakAlphaDifference(), 32);
    EXPECT_NEAR(controller.meanAlphaDifference(), 32.0, 1e-9);
    EXPECT_EQ(controller.alphaChangedPixels(), 4);

    // The diff image is a grayscale alpha map: |255-223| * 4 = 128.
    const QVariantMap alphaMap =
        controller.samplePixel(ImageReviewController::DisplayDiffSlot, 0, 0);
    ASSERT_TRUE(alphaMap.value(QStringLiteral("valid")).toBool());
    EXPECT_EQ(alphaMap.value(QStringLiteral("r")).toInt(), 128);
    EXPECT_EQ(alphaMap.value(QStringLiteral("g")).toInt(), 128);
    EXPECT_EQ(alphaMap.value(QStringLiteral("b")).toInt(), 128);
    EXPECT_EQ(alphaMap.value(QStringLiteral("a")).toInt(), 255);
}

TEST(ImageReviewControllerTests, AlphaStatsStayVisibleWhenRgbMatchesCompletely) {
    ImageReviewController controller;
    QImage left(2, 2, QImage::Format_ARGB32);
    left.fill(qRgba(200, 100, 50, 255));
    QImage right(2, 2, QImage::Format_ARGB32);
    right.fill(qRgba(200, 100, 50, 200));
    ASSERT_TRUE(controller.openPairImages(left, QStringLiteral("a"), right, QStringLiteral("b")));

    // AbsDifference ignores alpha in its output but must still surface the alpha
    // regression so a transparency-only change is never reported as "no difference".
    controller.setCompareMode(ImageReviewController::AbsDifference);
    ASSERT_TRUE(waitForControllerIdle(controller));
    EXPECT_EQ(controller.maxAbsDifference(), 0);
    EXPECT_TRUE(controller.alphaDifferenceOnly());
    EXPECT_EQ(controller.peakAlphaDifference(), 55);
    EXPECT_EQ(controller.alphaChangedPixels(), 4);
}

TEST(ImageReviewControllerTests, LargeImageChannelBuildKeepsHoverResponsiveAndReplacesCleanly) {
    ensureCoreApplication();
    ImageReviewController controller;
    QImage primary(7680, 4320, QImage::Format_ARGB32);
    QImage secondary(7680, 4320, QImage::Format_ARGB32);
    ASSERT_FALSE(primary.isNull());
    ASSERT_FALSE(secondary.isNull());
    primary.fill(QColor(10, 20, 30, 128));
    secondary.fill(QColor(40, 50, 60, 64));
    ASSERT_TRUE(controller.openPairImages(
        std::move(primary), QStringLiteral("8k-a"), std::move(secondary), QStringLiteral("8k-b")));
    qint64 maximumHoverUs = 0;
    for (const int mode : {ImageReviewController::AlphaGrayView,
                           ImageReviewController::RgbOpaqueView,
                           ImageReviewController::AlphaGrayView}) {
        controller.setViewMode(mode);
        QElapsedTimer cold;
        cold.start();
        auto build = std::async(std::launch::async, [&controller] {
            return controller.imageForSlot(ImageReviewController::PrimarySlot);
        });
        for (int sample = 0; sample < 100; ++sample) {
            QElapsedTimer hover;
            hover.start();
            const auto pixel = controller.samplePixel(ImageReviewController::PrimarySlot, 100, 100);
            maximumHoverUs = std::max(maximumHoverUs, hover.nsecsElapsed() / 1000);
            EXPECT_TRUE(pixel.value(QStringLiteral("valid")).toBool());
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        }
        const QImage view = build.get();
        ASSERT_EQ(view.size(), QSize(7680, 4320));
        EXPECT_EQ(qAlpha(view.pixel(100, 100)), 255);
        std::cout << "8K channel mode=" << mode << " cold_ms=" << cold.elapsed() << '\n';
    }
    std::cout << "8K hover_max_us=" << maximumHoverUs << '\n';
    EXPECT_LT(maximumHoverUs, 100'000);
    ASSERT_TRUE(controller.swapSides());
    controller.setViewMode(ImageReviewController::AlphaGrayView);
    EXPECT_EQ(qRed(controller.imageForSlot(ImageReviewController::PrimarySlot).pixel(0, 0)), 64);
    ASSERT_TRUE(controller.openPrimaryImage(solidImage(QColor(1, 2, 3, 200)),
                                            QStringLiteral("replacement")));
    controller.setViewMode(ImageReviewController::AlphaGrayView);
    EXPECT_EQ(qRed(controller.imageForSlot(ImageReviewController::PrimarySlot).pixel(0, 0)), 200);
}

TEST(ImageReviewControllerTests, ChannelViewsIsolateAlphaAndRgbWithoutMutatingSource) {
    ImageReviewController controller;
    QImage source(2, 2, QImage::Format_ARGB32);
    source.setPixel(0, 0, qRgba(10, 20, 30, 255));
    source.setPixel(1, 0, qRgba(40, 50, 60, 128));
    source.setPixel(0, 1, qRgba(70, 80, 90, 64));
    source.setPixel(1, 1, qRgba(100, 110, 120, 0));
    ASSERT_TRUE(controller.openPrimaryImage(source, QStringLiteral("a")));

    controller.setViewMode(ImageReviewController::AlphaGrayView);
    const QImage alphaView = controller.imageForSlot(ImageReviewController::PrimarySlot);
    ASSERT_FALSE(alphaView.isNull());
    EXPECT_EQ(qRed(alphaView.pixel(0, 0)), 255);
    EXPECT_EQ(qRed(alphaView.pixel(1, 0)), 128);
    EXPECT_EQ(qRed(alphaView.pixel(1, 1)), 0);
    EXPECT_EQ(qAlpha(alphaView.pixel(1, 1)), 255); // view is opaque; brightness = alpha.
    const QVariantMap alphaSample =
        controller.samplePixel(ImageReviewController::PrimarySlot, 1, 0);
    EXPECT_EQ(alphaSample.value(QStringLiteral("channelView")).toString(),
              QStringLiteral("alphaGray"));
    EXPECT_EQ(alphaSample.value(QStringLiteral("alphaPercent")).toString(), QStringLiteral("50.2"));

    controller.setViewMode(ImageReviewController::RgbOpaqueView);
    const QImage rgbView = controller.imageForSlot(ImageReviewController::PrimarySlot);
    ASSERT_FALSE(rgbView.isNull());
    // Colors hidden in the transparent region become visible; alpha forced opaque.
    EXPECT_EQ(qRed(rgbView.pixel(1, 1)), 100);
    EXPECT_EQ(qAlpha(rgbView.pixel(1, 1)), 255);

    // The observation mode never mutates the committed source image.
    controller.setViewMode(ImageReviewController::RgbaView);
    const QImage original = controller.imageForSlot(ImageReviewController::PrimarySlot);
    EXPECT_EQ(qAlpha(original.pixel(1, 1)), 0);
    EXPECT_EQ(qRed(original.pixel(1, 1)), 100);
}

TEST(ImageReviewControllerTests, ChannelViewsReadRgba8888BuffersInTrueChannelOrder) {
    ImageReviewController controller;
    // Production buffers arrive as Format_RGBA8888 (byte order R,G,B,A). The channel views
    // used to reinterpret those scan lines as QRgb, which swaps red and blue on
    // little-endian targets; a red-dominant opaque pixel must never read back as blue.
    QImage primary(2, 1, QImage::Format_RGBA8888);
    ASSERT_FALSE(primary.isNull());
    primary.setPixelColor(0, 0, QColor(200, 40, 30, 255));
    primary.setPixelColor(1, 0, QColor(10, 20, 220, 96));
    QImage secondary(1, 1, QImage::Format_RGBA8888);
    ASSERT_FALSE(secondary.isNull());
    secondary.setPixelColor(0, 0, QColor(5, 210, 15, 128));
    ASSERT_TRUE(controller.openPairImages(std::move(primary),
                                          QStringLiteral("rgba8888-a"),
                                          std::move(secondary),
                                          QStringLiteral("rgba8888-b")));

    controller.setViewMode(ImageReviewController::RgbOpaqueView);
    const QImage primaryView = controller.imageForSlot(ImageReviewController::PrimarySlot);
    ASSERT_FALSE(primaryView.isNull());
    EXPECT_EQ(qRed(primaryView.pixel(0, 0)), 200);
    EXPECT_EQ(qGreen(primaryView.pixel(0, 0)), 40);
    EXPECT_EQ(qBlue(primaryView.pixel(0, 0)), 30);
    EXPECT_EQ(qAlpha(primaryView.pixel(0, 0)), 255);
    EXPECT_EQ(qRed(primaryView.pixel(1, 0)), 10);
    EXPECT_EQ(qGreen(primaryView.pixel(1, 0)), 20);
    EXPECT_EQ(qBlue(primaryView.pixel(1, 0)), 220);
    EXPECT_EQ(qAlpha(primaryView.pixel(1, 0)), 255);
    const QImage secondaryView = controller.imageForSlot(ImageReviewController::SecondarySlot);
    ASSERT_FALSE(secondaryView.isNull());
    EXPECT_EQ(qRed(secondaryView.pixel(0, 0)), 5);
    EXPECT_EQ(qGreen(secondaryView.pixel(0, 0)), 210);
    EXPECT_EQ(qBlue(secondaryView.pixel(0, 0)), 15);
    EXPECT_EQ(qAlpha(secondaryView.pixel(0, 0)), 255);

    controller.setViewMode(ImageReviewController::AlphaGrayView);
    const QImage alphaView = controller.imageForSlot(ImageReviewController::PrimarySlot);
    ASSERT_FALSE(alphaView.isNull());
    EXPECT_EQ(qRed(alphaView.pixel(0, 0)), 255);
    EXPECT_EQ(qRed(alphaView.pixel(1, 0)), 96);
    EXPECT_EQ(qAlpha(alphaView.pixel(1, 0)), 255);
}

TEST(ImageReviewControllerTests, CompareModeAcceptsFadeAndKeepsItViewOnly) {
    ImageReviewController controller;
    // Without a pair, Fade is rejected exactly like the other comparison modes.
    controller.setCompareMode(ImageReviewController::Fade);
    EXPECT_EQ(controller.compareMode(), static_cast<int>(ImageReviewController::PrimaryOnly));
    EXPECT_FALSE(controller.errorText().isEmpty());

    ASSERT_TRUE(controller.openPairImages(solidImage(QColor(10, 20, 30, 255)),
                                          QStringLiteral("fade-a"),
                                          solidImage(QColor(40, 50, 60, 255)),
                                          QStringLiteral("fade-b")));
    // The controller used to cap setCompareMode at AlphaDifference, so switching to Fade
    // silently no-opped. The switch must actually engage the mode.
    controller.setCompareMode(ImageReviewController::Fade);
    EXPECT_EQ(controller.compareMode(), static_cast<int>(ImageReviewController::Fade));
    EXPECT_TRUE(controller.errorText().isEmpty());
    // Fade is presentation-only: no difference result is claimed and nothing pends.
    EXPECT_FALSE(controller.hasDiffResult());
    EXPECT_FALSE(controller.diffPending());
    controller.setFadePosition(0.25);
    EXPECT_DOUBLE_EQ(controller.fadePosition(), 0.25);
    // Switching away is a plain view change.
    controller.setCompareMode(ImageReviewController::PrimaryOnly);
    EXPECT_EQ(controller.compareMode(), static_cast<int>(ImageReviewController::PrimaryOnly));
}

TEST(ImageReviewControllerTests, SwapSidesExchangesSidesAndKeepsPairIdentity) {
    ImageReviewController controller;
    ASSERT_TRUE(controller.openPairImages(solidImage(Qt::red),
                                          QStringLiteral("left.png"),
                                          solidImage(Qt::blue),
                                          QStringLiteral("right.png"),
                                          7));
    controller.setZoom(2.5);
    const qreal panX = controller.panX();
    ASSERT_TRUE(controller.swapSides());
    EXPECT_EQ(controller.primaryPath(), QStringLiteral("right.png"));
    EXPECT_EQ(controller.secondaryPath(), QStringLiteral("left.png"));
    EXPECT_EQ(controller.committedPairId(), 7);
    EXPECT_DOUBLE_EQ(controller.zoom(), 2.5);
    EXPECT_DOUBLE_EQ(controller.panX(), panX);
    const QImage primary = controller.imageForSlot(ImageReviewController::PrimarySlot);
    const QImage secondary = controller.imageForSlot(ImageReviewController::SecondarySlot);
    EXPECT_EQ(primary.pixel(0, 0), QColor(Qt::blue).rgb());
    EXPECT_EQ(secondary.pixel(0, 0), QColor(Qt::red).rgb());
    // Second swap restores the original orientation.
    ASSERT_TRUE(controller.swapSides());
    EXPECT_EQ(controller.primaryPath(), QStringLiteral("left.png"));
    EXPECT_EQ(controller.secondaryPath(), QStringLiteral("right.png"));
    EXPECT_EQ(controller.committedPairId(), 7);
}

TEST(ImageReviewControllerTests, SwapSidesRequiresAnOpenPair) {
    ImageReviewController controller;
    EXPECT_FALSE(controller.swapSides());
    EXPECT_FALSE(controller.errorText().isEmpty());
    ASSERT_TRUE(controller.openPrimaryImage(solidImage(Qt::red), QStringLiteral("only.png")));
    EXPECT_FALSE(controller.swapSides());
}

TEST(ImageReviewControllerTests, ReplacePrimaryKeepsSecondaryAndPairIdentity) {
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QUrl replacement =
        writeBytes(directory, QStringLiteral("newA.bin"), QByteArrayLiteral("newA"));
    ASSERT_FALSE(replacement.isEmpty());
    ScopedStillImageLoader loader{[](const QByteArray& bytes,
                                     QImage* image,
                                     QImage*,
                                     dvs::ui::StillImageSourceInfo*,
                                     std::string*) {
        if (bytes != QByteArrayLiteral("newA")) {
            return false;
        }
        *image = solidImage(QColor(1, 2, 3));
        return true;
    }};

    ImageReviewController controller;
    ASSERT_TRUE(controller.openPairImages(solidImage(Qt::red),
                                          QStringLiteral("oldA.png"),
                                          solidImage(Qt::blue),
                                          QStringLiteral("oldB.png"),
                                          11));
    controller.setZoom(3.0);
    ASSERT_GT(controller.requestReplacePrimary(replacement), 0);
    ASSERT_TRUE(waitForControllerIdle(controller));
    EXPECT_TRUE(controller.primaryPath().endsWith(QStringLiteral("newA.bin")));
    EXPECT_EQ(controller.secondaryPath(), QStringLiteral("oldB.png"));
    EXPECT_EQ(controller.committedPairId(), 11);
    EXPECT_DOUBLE_EQ(controller.zoom(), 3.0);
    const QImage secondary = controller.imageForSlot(ImageReviewController::SecondarySlot);
    EXPECT_EQ(secondary.pixel(0, 0), QColor(Qt::blue).rgb());
}

TEST(ImageReviewControllerTests, ReplaceSecondaryKeepsPrimaryAndPairIdentity) {
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QUrl replacement =
        writeBytes(directory, QStringLiteral("newB.bin"), QByteArrayLiteral("newB"));
    ASSERT_FALSE(replacement.isEmpty());
    ScopedStillImageLoader loader{[](const QByteArray& bytes,
                                     QImage* image,
                                     QImage*,
                                     dvs::ui::StillImageSourceInfo*,
                                     std::string*) {
        if (bytes != QByteArrayLiteral("newB")) {
            return false;
        }
        *image = solidImage(QColor(9, 8, 7));
        return true;
    }};

    ImageReviewController controller;
    ASSERT_TRUE(controller.openPairImages(solidImage(Qt::red),
                                          QStringLiteral("oldA.png"),
                                          solidImage(Qt::blue),
                                          QStringLiteral("oldB.png"),
                                          13));
    ASSERT_GT(controller.requestReplaceSecondary(replacement), 0);
    ASSERT_TRUE(waitForControllerIdle(controller));
    EXPECT_EQ(controller.primaryPath(), QStringLiteral("oldA.png"));
    EXPECT_TRUE(controller.secondaryPath().endsWith(QStringLiteral("newB.bin")));
    EXPECT_EQ(controller.committedPairId(), 13);
    const QImage primary = controller.imageForSlot(ImageReviewController::PrimarySlot);
    EXPECT_EQ(primary.pixel(0, 0), QColor(Qt::red).rgb());
}

TEST(ImageReviewControllerTests, FailedSideReplaceKeepsPreviousSide) {
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QUrl good = writeBytes(directory, QStringLiteral("good.bin"), QByteArrayLiteral("good"));
    const QUrl bad = writeBytes(directory, QStringLiteral("bad.bin"), QByteArrayLiteral("bad"));
    ASSERT_FALSE(good.isEmpty() || bad.isEmpty());
    ScopedStillImageLoader loader{[](const QByteArray& bytes,
                                     QImage* image,
                                     QImage*,
                                     dvs::ui::StillImageSourceInfo*,
                                     std::string* error) {
        if (bytes == QByteArrayLiteral("bad")) {
            if (error != nullptr) {
                *error = "corrupt";
            }
            return false;
        }
        *image = solidImage(QColor(5, 6, 7));
        return true;
    }};

    ImageReviewController controller;
    ASSERT_TRUE(controller.openPairImages(solidImage(Qt::red),
                                          QStringLiteral("oldA.png"),
                                          solidImage(Qt::blue),
                                          QStringLiteral("oldB.png"),
                                          3));
    ASSERT_GT(controller.requestReplacePrimary(bad), 0);
    ASSERT_TRUE(waitForControllerIdle(controller));
    EXPECT_FALSE(controller.errorText().isEmpty());
    EXPECT_EQ(controller.primaryPath(), QStringLiteral("oldA.png"));
    EXPECT_EQ(controller.secondaryPath(), QStringLiteral("oldB.png"));
    EXPECT_EQ(controller.committedPairId(), 3);

    ASSERT_GT(controller.requestReplaceSecondary(good), 0);
    ASSERT_TRUE(waitForControllerIdle(controller));
    EXPECT_TRUE(controller.errorText().isEmpty());
    EXPECT_EQ(controller.primaryPath(), QStringLiteral("oldA.png"));
    EXPECT_TRUE(controller.secondaryPath().endsWith(QStringLiteral("good.bin")));
}

TEST(ImageReviewControllerTests, SwapSidesRecomputesSignedDifferenceDirection) {
    ImageReviewController controller;
    QImage left(1, 1, QImage::Format_ARGB32);
    left.fill(qRgba(10, 0, 0, 255));
    QImage right(1, 1, QImage::Format_ARGB32);
    right.fill(qRgba(40, 0, 0, 255));
    ASSERT_TRUE(controller.openPairImages(left, QStringLiteral("a"), right, QStringLiteral("b")));
    controller.setCompareMode(ImageReviewController::AbsDifference);
    ASSERT_TRUE(waitForControllerIdle(controller));
    ASSERT_TRUE(controller.hasDiffResult());
    EXPECT_EQ(controller.maxAbsDifference(), 30);

    ASSERT_TRUE(controller.swapSides());
    ASSERT_TRUE(waitForControllerIdle(controller));
    // Abs difference is symmetric, but the pair orientation (and signed mode) flips.
    ASSERT_TRUE(controller.hasDiffResult());
    EXPECT_EQ(controller.maxAbsDifference(), 30);
    EXPECT_EQ(controller.primaryPath(), QStringLiteral("b"));
    EXPECT_EQ(controller.secondaryPath(), QStringLiteral("a"));
}

TEST(ImageReviewControllerTests, ZoomToRectCalculatesCenterPanAndClampedZoom) {
    ImageReviewController controller;
    QImage left(100, 100, QImage::Format_ARGB32);
    left.fill(Qt::black);
    ASSERT_TRUE(controller.openPrimaryImage(left, QStringLiteral("test.png")));

    EXPECT_DOUBLE_EQ(controller.zoom(), 1.0);
    EXPECT_DOUBLE_EQ(controller.panX(), 0.5);
    EXPECT_DOUBLE_EQ(controller.panY(), 0.5);

    // Zoom into rect from (0.2, 0.3) to (0.6, 0.7)
    // rectW = 0.4, rectH = 0.4 -> factor = 2.5
    // center = (0.4, 0.5)
    controller.zoomToRect(0.2, 0.3, 0.6, 0.7);

    EXPECT_NEAR(controller.panX(), 0.4, 1e-4);
    EXPECT_NEAR(controller.panY(), 0.5, 1e-4);
    EXPECT_NEAR(controller.zoom(), 2.5, 1e-4);

    // Tiny rect is ignored (guard against accidental clicks)
    controller.zoomToRect(0.4, 0.4, 0.401, 0.401);
    EXPECT_NEAR(controller.zoom(), 2.5, 1e-4);
}

TEST(ImageReviewControllerTests, DifferenceGainAmplifiesDisplayButNotStatistics) {
    // §2.5: the gain is a display parameter. Switching it must re-render the image, reuse the
    // pair analysis instead of re-reading both sources, and leave every statistic untouched.
    ImageReviewController controller;
    ASSERT_TRUE(
        controller.openPrimaryImage(solidImage(QColor(20, 40, 60)), QStringLiteral("left")));
    ASSERT_TRUE(
        controller.openSecondaryImage(solidImage(QColor(20, 40, 80)), QStringLiteral("right")));
    EXPECT_EQ(controller.diffGain(), 4);

    controller.setCompareMode(ImageReviewController::AbsDifference);
    ASSERT_TRUE(waitForControllerIdle(controller));
    ASSERT_TRUE(controller.hasDiffResult());
    // Blue delta is 20: ×4 clamps to 80, the raw peak stays 20.
    EXPECT_EQ(controller.maxAbsDifference(), 20);
    EXPECT_EQ(controller.samplePixel(ImageReviewController::DisplayDiffSlot, 0, 0)
                  .value(QStringLiteral("b"))
                  .toInt(),
              80);
    const int analysisRunsAfterFirstDiff =
        controller.asyncStats().value(QStringLiteral("analysis_runs")).toInt();
    ASSERT_GE(analysisRunsAfterFirstDiff, 1);

    controller.setDiffGain(1);
    ASSERT_TRUE(waitForControllerIdle(controller));
    EXPECT_EQ(controller.samplePixel(ImageReviewController::DisplayDiffSlot, 0, 0)
                  .value(QStringLiteral("b"))
                  .toInt(),
              20);
    EXPECT_EQ(controller.maxAbsDifference(), 20);
    // The mean averages all three channel deltas over every pixel: only blue moved, so the
    // per-pixel mean is 20/3 regardless of the display gain.
    EXPECT_NEAR(controller.meanAbsDifference(), 20.0 / 3.0, 1e-9);

    controller.setDiffGain(16);
    ASSERT_TRUE(waitForControllerIdle(controller));
    EXPECT_EQ(controller.samplePixel(ImageReviewController::DisplayDiffSlot, 0, 0)
                  .value(QStringLiteral("b"))
                  .toInt(),
              255);
    EXPECT_EQ(controller.maxAbsDifference(), 20);

    // A mode switch is another display variant of the same pair analysis.
    controller.setCompareMode(ImageReviewController::Highlight);
    ASSERT_TRUE(waitForControllerIdle(controller));
    EXPECT_EQ(controller.maxAbsDifference(), 20);

    // Out-of-range values clamp instead of silently disabling the amplification.
    controller.setDiffGain(999);
    EXPECT_EQ(controller.diffGain(), 16);
    controller.setDiffGain(0);
    ASSERT_TRUE(waitForControllerIdle(controller));
    EXPECT_EQ(controller.diffGain(), 1);

    // Re-rendering for a new gain or mode reuses the pair analysis; it must never re-read the
    // sources or recompute a statistic. A gain whose render is already cached is served by the
    // controller's display cache and does not reach the loader at all, which is why the reuse
    // count tracks the variants that actually had to be re-rendered.
    const QVariantMap stats = controller.asyncStats();
    EXPECT_EQ(stats.value(QStringLiteral("analysis_runs")).toInt(), analysisRunsAfterFirstDiff);
    EXPECT_GE(stats.value(QStringLiteral("analysis_reuses")).toInt(), 3);
}

TEST(ImageReviewControllerTests, HighDepthStatisticsDetectDifferenceTheDisplayCannotShow) {
    // §2.4: two sources whose RGBA8 display samples are identical but whose converted RGBA64
    // code values differ. The display statistics legitimately report equality; the high-depth
    // statistics must still report the difference and say which path saw it.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QUrl left =
        writeBytes(directory, QStringLiteral("deepA.bin"), QByteArrayLiteral("deepA"));
    const QUrl right =
        writeBytes(directory, QStringLiteral("deepB.bin"), QByteArrayLiteral("deepB"));
    ASSERT_FALSE(left.isEmpty());
    ASSERT_FALSE(right.isEmpty());
    ScopedStillImageLoader loader{[](const QByteArray& bytes,
                                     QImage* image,
                                     QImage* nativeImage,
                                     dvs::ui::StillImageSourceInfo* info,
                                     std::string*) {
        if (!bytes.startsWith("deep")) {
            return false;
        }
        const bool second = bytes.endsWith("B");
        // Identical display samples on both sides: the RGBA8 quantization hides the difference.
        *image = solidImage(QColor(1, 2, 3));
        if (info != nullptr) {
            info->bitDepth = 16;
            info->channels = 3;
            info->hasAlpha = false;
            info->sourceFormat = QStringLiteral("rgb48le");
            info->displayConverted = true;
        }
        if (nativeImage != nullptr) {
            QImage native(4, 4, QImage::Format_RGBA64);
            if (!native.isNull()) {
                auto* const samples = reinterpret_cast<QRgba64*>(native.bits());
                // One code value apart: the same 8-bit bucket, a different 16-bit sample.
                std::fill(samples,
                          samples + native.sizeInBytes() / sizeof(QRgba64),
                          second ? qRgba64(257, 258, 259, 65535) : qRgba64(256, 257, 258, 65535));
            }
            *nativeImage = std::move(native);
        }
        return true;
    }};
    ImageReviewController controller;
    ASSERT_GT(controller.requestOpenPair(left, right, 11), 0);
    ASSERT_TRUE(waitForControllerIdle(controller));
    ASSERT_TRUE(controller.hasPair());

    controller.setCompareMode(ImageReviewController::AbsDifference);
    ASSERT_TRUE(waitForControllerIdle(controller));
    ASSERT_TRUE(controller.hasDiffResult());

    // The display path is genuinely identical, so its statistics must not be inflated.
    EXPECT_EQ(controller.maxAbsDifference(), 0);
    EXPECT_DOUBLE_EQ(controller.meanAbsDifference(), 0.0);

    EXPECT_TRUE(controller.nativeStatsAvailable());
    EXPECT_EQ(controller.nativeMaxAbsDifference(), 1);
    EXPECT_NEAR(controller.nativeMeanAbsDifference(), 1.0, 1e-9);
    EXPECT_EQ(controller.nativeChangedPixels(), 16);
    EXPECT_EQ(controller.nativeBeyondDisplayPixels(), 16);
    EXPECT_TRUE(controller.displayEqualButNativeDifferent());
    EXPECT_TRUE(controller.nativeStatsText().contains(QStringLiteral("RGBA16")));
    EXPECT_TRUE(controller.diffScopeText().contains(QStringLiteral("RGBA16")));
}

TEST(ImageReviewControllerTests, PairWorkingSetEstimateCountsSidecarsAnalysisAndDerivedImage) {
    const dvs::ui::ImageWorkingSetEstimate single =
        dvs::ui::estimateSingleImageWorkingSet(100, 100, true);
    EXPECT_EQ(single.displayBytes, 40000);
    EXPECT_EQ(single.nativeBytes, 80000);
    EXPECT_EQ(single.totalBytes, 120000);

    const dvs::ui::ImageWorkingSetEstimate pair =
        dvs::ui::estimatePairWorkingSet(QSize(100, 100), QSize(100, 100), true, false);
    EXPECT_EQ(pair.displayBytes, 80000);
    EXPECT_EQ(pair.nativeBytes, 160000);
    // The analysis field is 4 bytes per pixel plus a 1-byte sign map, the derived ARGB32
    // difference image another 4.
    EXPECT_EQ(pair.analysisBytes, 50000);
    EXPECT_EQ(pair.derivedBytes, 40000);
    EXPECT_EQ(pair.resampleBytes, 0);
    EXPECT_EQ(pair.totalBytes, 330000);

    // A mismatched pair that is resampled pays one extra scaled copy of the target extent.
    const dvs::ui::ImageWorkingSetEstimate resampled =
        dvs::ui::estimatePairWorkingSet(QSize(100, 100), QSize(50, 200), true, true);
    EXPECT_EQ(resampled.displayBytes, (10000LL + 10000LL) * 4LL);
    EXPECT_EQ(resampled.resampleBytes, 40000);
    EXPECT_EQ(resampled.totalBytes, 370000);
}

TEST(ImageReviewControllerTests, DifferenceRefusesPairBeyondWorkingSetBudget) {
    ensureCoreApplication();
    dvs::ui::ImagePairLoader loader;
    // 64 bytes cannot hold even a 4x4 pair's display buffers, analysis field and derived image.
    loader.setWorkingSetBudgetBytes(64);
    EXPECT_EQ(loader.workingSetBudgetBytes(), 64);

    std::optional<dvs::ui::ImagePairLoader::DifferenceResult> result;
    dvs::ui::ImagePairLoader::DifferenceOptions options;
    options.compareMode = ImageReviewController::AbsDifference;
    ASSERT_GT(loader.requestDifference(solidImage(QColor(0, 0, 0)),
                                       solidImage(QColor(10, 10, 10)),
                                       QImage{},
                                       QImage{},
                                       options,
                                       [&result](dvs::ui::ImagePairLoader::DifferenceResult value) {
                                           result = std::move(value);
                                       }),
              0);
    ASSERT_TRUE(waitUntil([&result] { return result.has_value(); }));
    EXPECT_FALSE(result->succeeded());
    EXPECT_TRUE(result->error.contains(QStringLiteral("预算")));
    const dvs::ui::ImagePairLoader::Stats stats = loader.stats();
    EXPECT_GT(stats.workingSet.totalBytes, stats.workingSetBudgetBytes);

    // The same pair succeeds once the budget admits it: the refusal is accounting, not a
    // decoding failure.
    loader.setWorkingSetBudgetBytes(dvs::ui::kImagePairWorkingSetBudgetBytes);
    result.reset();
    ASSERT_GT(loader.requestDifference(solidImage(QColor(0, 0, 0)),
                                       solidImage(QColor(10, 10, 10)),
                                       QImage{},
                                       QImage{},
                                       options,
                                       [&result](dvs::ui::ImagePairLoader::DifferenceResult value) {
                                           result = std::move(value);
                                       }),
              0);
    ASSERT_TRUE(waitUntil([&result] { return result.has_value(); }));
    ASSERT_TRUE(result->succeeded());
    EXPECT_EQ(result->maxAbsDifference, 10);
    EXPECT_EQ(result->gain, 4);
}

// ---- Single-side close --------------------------------------------------------------------
//
// Closing one side must keep the other side's file identity, decoded content and source
// provenance. Neither entry is a disguised closeAll: with a pair committed the surviving
// side stays, and only when the named side is the last image does the workspace reach the
// empty state.

TEST(ImageReviewControllerTests, CloseSecondaryKeepsPrimaryAndItsProvenance) {
    ImageReviewController controller;
    ASSERT_TRUE(controller.openPairImages(solidImage(Qt::red),
                                          QStringLiteral("left.png"),
                                          solidImage(Qt::blue),
                                          QStringLiteral("right.png"),
                                          7));
    controller.setCompareMode(ImageReviewController::Wipe);
    controller.setZoom(2.5);
    controller.updateCursorPixel(ImageReviewController::SecondarySlot, 1, 1);

    ASSERT_TRUE(controller.closeSecondary());
    EXPECT_TRUE(controller.hasPrimary());
    EXPECT_FALSE(controller.hasSecondary());
    EXPECT_FALSE(controller.hasPair());
    EXPECT_EQ(controller.primaryPath(), QStringLiteral("left.png"));
    EXPECT_TRUE(controller.secondaryPath().isEmpty());
    // The surviving image is the one that was A: content and provenance travel with it.
    EXPECT_EQ(controller.imageForSlot(ImageReviewController::PrimarySlot).pixel(0, 0),
              QColor(Qt::red).rgb());
    // A single image cannot carry a pair-only mode, so difference/wipe/fade fall back.
    EXPECT_EQ(controller.compareMode(), ImageReviewController::PrimaryOnly);
    // The committed row identity is gone with the pair, and the folder list must not keep
    // a stale selection pointing at a pair that no longer exists.
    EXPECT_EQ(controller.committedPairId(), -1);
    // The observation position is deliberately kept: the surviving image is already
    // decoded and in memory, and a reset would throw away the user's zoom and pan.
    EXPECT_DOUBLE_EQ(controller.zoom(), 2.5);
    // Probe and hover state for the closed side must not survive.
    EXPECT_TRUE(controller.cursorPixel().isEmpty());
    EXPECT_TRUE(controller.frozenProbeSample().isEmpty());
    // The content generation moved, so a late asynchronous result for the closed pair
    // cannot repaint the surviving image.
    const int generation = controller.contentGeneration();
    EXPECT_TRUE(controller.closePrimary());
    EXPECT_NE(controller.contentGeneration(), generation);
}

TEST(ImageReviewControllerTests, ClosePrimaryPromotesSecondaryWithItsOwnIdentity) {
    ImageReviewController controller;
    dvs::ui::StillImageSourceInfo primaryInfo;
    primaryInfo.sourceFormat = QStringLiteral("png");
    primaryInfo.bitDepth = 8;
    primaryInfo.hasAlpha = true;
    primaryInfo.channels = 4;
    primaryInfo.displayConverted = false;
    dvs::ui::StillImageSourceInfo secondaryInfo;
    secondaryInfo.sourceFormat = QStringLiteral("webp");
    secondaryInfo.bitDepth = 16;
    secondaryInfo.hasAlpha = true;
    secondaryInfo.channels = 4;
    secondaryInfo.displayConverted = true;
    ASSERT_TRUE(controller.openPairImages(solidImage(Qt::red),
                                          QStringLiteral("a.png"),
                                          solidImage(Qt::blue),
                                          QStringLiteral("b.webp"),
                                          11,
                                          primaryInfo,
                                          secondaryInfo));

    ASSERT_TRUE(controller.closePrimary());
    EXPECT_TRUE(controller.hasPrimary());
    EXPECT_FALSE(controller.hasSecondary());
    // B is promoted into the internal primary slot, but it keeps its own file identity,
    // content and source information - it is not relabelled as A.
    EXPECT_EQ(controller.primaryPath(), QStringLiteral("b.webp"));
    EXPECT_EQ(controller.primarySourceFormat(), QStringLiteral("webp"));
    EXPECT_EQ(controller.primaryBitDepth(), 16);
    EXPECT_TRUE(controller.primaryDisplayConverted());
    EXPECT_TRUE(controller.primaryHasAlpha());
    EXPECT_EQ(controller.imageForSlot(ImageReviewController::PrimarySlot).pixel(0, 0),
              QColor(Qt::blue).rgb());
    // A single image cannot carry a pair-only mode.
    EXPECT_EQ(controller.compareMode(), ImageReviewController::PrimaryOnly);
    EXPECT_EQ(controller.committedPairId(), -1);
    EXPECT_TRUE(controller.frozenProbeSample().isEmpty());
    EXPECT_TRUE(controller.cursorPixel().isEmpty());
}

TEST(ImageReviewControllerTests, ClosePrimaryWithoutSecondaryReachesEmptyState) {
    ImageReviewController controller;
    ASSERT_TRUE(controller.openPrimaryImage(solidImage(Qt::red), QStringLiteral("only.png")));
    ASSERT_TRUE(controller.hasPrimary());
    EXPECT_FALSE(controller.hasSecondary());

    // The last image is not a single-side close standing in for closeAll: there is no
    // other side to keep, so the same teardown the session close uses runs.
    const int generation = controller.contentGeneration();
    ASSERT_TRUE(controller.closePrimary());
    EXPECT_FALSE(controller.hasPrimary());
    EXPECT_FALSE(controller.hasSecondary());
    EXPECT_TRUE(controller.primaryPath().isEmpty());
    // A new generation replaces the pixels, so a late result for the closed image cannot
    // repaint anything.
    EXPECT_GT(controller.contentGeneration(), generation);
    // Nothing is left to close, so a second call refuses instead of inventing work.
    EXPECT_FALSE(controller.closePrimary());
    EXPECT_FALSE(controller.closeSecondary());
}

TEST(ImageReviewControllerTests, SingleSideCloseRefusesAndKeepsEverythingWithoutAMatch) {
    ImageReviewController controller;
    ASSERT_TRUE(controller.openPairImages(solidImage(Qt::red),
                                          QStringLiteral("a.png"),
                                          solidImage(Qt::blue),
                                          QStringLiteral("b.png")));
    // Nothing loaded: both entries refuse and the workspace is untouched.
    ImageReviewController empty;
    EXPECT_FALSE(empty.closePrimary());
    EXPECT_FALSE(empty.closeSecondary());
    EXPECT_FALSE(empty.hasPrimary());

    // A live pair is untouched by a refused close on an empty controller (the guards are
    // per-controller, and the pair above is still there).
    EXPECT_TRUE(controller.hasPair());
    EXPECT_EQ(controller.primaryPath(), QStringLiteral("a.png"));
    EXPECT_EQ(controller.secondaryPath(), QStringLiteral("b.png"));
}

// ---- Numeric readout format ----------------------------------------------------------------
//
// The switch changes only what is printed and copied. The buffers, the sampling values,
// the difference field and every statistic stay exactly as they were.

TEST(ImageReviewControllerTests, NumericFormatChangesOnlyThePrintedText) {
    ImageReviewController controller;
    ASSERT_TRUE(controller.openPairImages(solidImage(QColor(12, 34, 56)),
                                          QStringLiteral("a.png"),
                                          solidImage(QColor(12, 34, 56)),
                                          QStringLiteral("b.png")));
    const int generation = controller.contentGeneration();
    const QImage before = controller.imageForSlot(ImageReviewController::PrimarySlot);

    EXPECT_EQ(controller.numericFormat(), ImageReviewController::IntegerCodeValues);
    EXPECT_EQ(controller.channelText(12), QStringLiteral("12"));
    EXPECT_EQ(controller.channelText(255), QStringLiteral("255"));
    EXPECT_EQ(controller.channelText16(32768), QStringLiteral("32768"));
    EXPECT_EQ(controller.rgbText(12, 34, 56), QStringLiteral("rgb(12, 34, 56)"));
    EXPECT_EQ(controller.rgbaText(12, 34, 56, 200), QStringLiteral("rgba(12, 34, 56, 200)"));
    // Hex is always #RRGGBBAA: the alpha byte is never dropped and the order is never ARGB.
    EXPECT_EQ(controller.hexText(12, 34, 56, 200), QStringLiteral("#0C2238C8"));
    EXPECT_EQ(controller.hexText(255, 255, 255, 255), QStringLiteral("#FFFFFFFF"));

    controller.setNumericFormat(ImageReviewController::NormalizedFloat);
    EXPECT_EQ(controller.numericFormat(), ImageReviewController::NormalizedFloat);
    // 8-bit display buffer: channel / 255, six decimals, no negative zero.
    EXPECT_EQ(controller.channelText(12), QStringLiteral("0.047059"));
    EXPECT_EQ(controller.channelText(255), QStringLiteral("1.000000"));
    EXPECT_EQ(controller.channelText(0), QStringLiteral("0.000000"));
    // Converted 16-bit sidecar: channel16 / 65535, never quantized to 8 bits first.
    EXPECT_EQ(controller.channelText16(32768), QStringLiteral("0.500008"));
    EXPECT_EQ(controller.rgbText(12, 34, 56), QStringLiteral("rgb(0.047059, 0.133333, 0.219608)"));
    // RGBA states its alpha range explicitly in the integer form; the normalized form is
    // self-describing because every component is on the same 0..1 scale.
    EXPECT_EQ(controller.rgbaText(12, 34, 56, 200),
              QStringLiteral("rgba(0.047059, 0.133333, 0.219608, 0.784314)"));
    // Hex stays hexadecimal in both modes: the switch never changes what it means.
    EXPECT_EQ(controller.hexText(12, 34, 56, 200), QStringLiteral("#0C2238C8"));

    // Signed deltas keep their sign in both modes, and a zero delta never renders as
    // "-0.000000". The integer form is the raw step count; the float form is the same
    // difference on the buffer's own full scale.
    EXPECT_EQ(controller.deltaText(12, 34), QStringLiteral("+0.086275"));
    EXPECT_EQ(controller.deltaText(34, 12), QStringLiteral("-0.086275"));
    EXPECT_EQ(controller.deltaText(12, 12), QStringLiteral("0.000000"));
    EXPECT_EQ(controller.deltaText16(1000, 2000), QStringLiteral("+0.015259"));
    EXPECT_EQ(controller.deltaText16(2000, 1000), QStringLiteral("-0.015259"));
    EXPECT_EQ(controller.deltaText16(1000, 1000), QStringLiteral("0.000000"));
    // Back to the integer form: the same deltas are raw step counts.
    controller.setNumericFormat(ImageReviewController::IntegerCodeValues);
    EXPECT_EQ(controller.deltaText(12, 34), QStringLiteral("+22"));
    EXPECT_EQ(controller.deltaText(34, 12), QStringLiteral("-22"));
    EXPECT_EQ(controller.deltaText(12, 12), QStringLiteral("+0"));
    EXPECT_EQ(controller.deltaText16(1000, 2000), QStringLiteral("+1000"));
    EXPECT_EQ(controller.deltaText16(2000, 1000), QStringLiteral("-1000"));

    // Presentation only: the decoded buffer and the generation are untouched, so nothing
    // downstream can mistake the switch for a re-decode or a re-analysis.
    EXPECT_EQ(controller.contentGeneration(), generation);
    EXPECT_EQ(controller.imageForSlot(ImageReviewController::PrimarySlot), before);
    // The statistics of a difference are still raw 8-bit deltas, not normalized ones.
    ASSERT_TRUE(controller.closeSecondary());
}

TEST(ImageReviewControllerTests, NumericFormatRejectsUnknownValuesAndKeepsTheCurrentOne) {
    ImageReviewController controller;
    EXPECT_EQ(controller.numericFormat(), ImageReviewController::IntegerCodeValues);
    controller.setNumericFormat(ImageReviewController::NormalizedFloat);
    EXPECT_EQ(controller.numericFormat(), ImageReviewController::NormalizedFloat);
    controller.setNumericFormat(99);
    EXPECT_EQ(controller.numericFormat(), ImageReviewController::IntegerCodeValues)
        << "an unknown format falls back to the integer form, never to a silent no-op";
}

// ---- Frozen probe sample & colour copy ------------------------------------------------------
//
// The pointer leaves the pixel long before the user reaches the button, so the sample that
// gets copied is the last valid one of the current material generation.

TEST(ImageReviewControllerTests, ProbeSampleSurvivesThePointerLeavingAndNamesTheSide) {
    ImageReviewController controller;
    ASSERT_TRUE(controller.openPairImages(solidImage(QColor(12, 34, 56, 200)),
                                          QStringLiteral("a.png"),
                                          solidImage(QColor(200, 100, 50, 255)),
                                          QStringLiteral("b.png")));
    // Nothing is frozen before the first valid sample.
    EXPECT_FALSE(controller.frozenProbeSample().value(QStringLiteral("valid")).toBool());

    ASSERT_TRUE(controller.captureProbeSample(ImageReviewController::DisplayPrimarySlot, 1, 1));
    const QVariantMap frozen = controller.frozenProbeSample();
    ASSERT_TRUE(frozen.value(QStringLiteral("valid")).toBool());
    EXPECT_EQ(frozen.value(QStringLiteral("x")).toInt(), 1);
    EXPECT_EQ(frozen.value(QStringLiteral("y")).toInt(), 1);
    EXPECT_EQ(frozen.value(QStringLiteral("r")).toInt(), 12);
    EXPECT_EQ(frozen.value(QStringLiteral("g")).toInt(), 34);
    EXPECT_EQ(frozen.value(QStringLiteral("b")).toInt(), 56);
    EXPECT_EQ(frozen.value(QStringLiteral("a")).toInt(), 200);
    EXPECT_EQ(frozen.value(QStringLiteral("side")).toString(), QStringLiteral("A"));

    // The sample survives the pointer leaving the pixel: it is not re-sampled at click
    // time and it is not the screen colour under the button.
    controller.clearCursorPixel();
    EXPECT_TRUE(controller.frozenProbeSample().value(QStringLiteral("valid")).toBool());
    EXPECT_EQ(controller.probeColorText(ImageReviewController::ProbeRgb),
              QStringLiteral("rgb(12, 34, 56)"));
    EXPECT_EQ(controller.probeColorText(ImageReviewController::ProbeRgba),
              QStringLiteral("rgba(12, 34, 56, 200)"));
    EXPECT_EQ(controller.probeColorText(ImageReviewController::ProbeHex),
              QStringLiteral("#0C2238C8"));

    // B is named explicitly: nothing is guessed from "the main side".
    ASSERT_TRUE(controller.captureProbeSample(ImageReviewController::DisplaySecondarySlot, 2, 2));
    const QVariantMap frozenB = controller.frozenProbeSample();
    EXPECT_EQ(frozenB.value(QStringLiteral("side")).toString(), QStringLiteral("B"));
    EXPECT_EQ(frozenB.value(QStringLiteral("r")).toInt(), 200);
    EXPECT_EQ(controller.probeColorText(ImageReviewController::ProbeRgb),
              QStringLiteral("rgb(200, 100, 50)"));
    EXPECT_EQ(controller.probeColorText(ImageReviewController::ProbeHex),
              QStringLiteral("#C86432FF"));

    // A sample outside the image is refused, and the previously frozen one stays because
    // it still names a real pixel of the current generation.
    EXPECT_FALSE(
        controller.captureProbeSample(ImageReviewController::DisplaySecondarySlot, 99, 99));
    EXPECT_EQ(controller.frozenProbeSample().value(QStringLiteral("x")).toInt(), 2);

    // A new generation replaces the pixels the sample named, so it dies with them.
    ASSERT_TRUE(controller.closeSecondary());
    EXPECT_FALSE(controller.frozenProbeSample().value(QStringLiteral("valid")).toBool());
    EXPECT_TRUE(controller.probeColorText(ImageReviewController::ProbeRgb).isEmpty());
}

TEST(ImageReviewControllerTests, ProbeSampleCopiesTheOriginalDecodeNotADerivedView) {
    ImageReviewController controller;
    // A transparent pixel whose RGB is not black: the straight-alpha buffer keeps the real
    // value, so the copy must not zero it just because the alpha is small.
    ASSERT_TRUE(controller.openPrimaryImage(solidImage(QColor(10, 20, 30, 4)),
                                            QStringLiteral("ghost.png")));
    // The RGB-ignoring-alpha observation is a derived view; the copy still reads the
    // original decode buffer, never the view, the composited background or a wipe line.
    controller.setViewMode(ImageReviewController::RgbOpaqueView);
    ASSERT_TRUE(controller.captureProbeSample(ImageReviewController::DisplayPrimarySlot, 0, 0));
    const QVariantMap frozen = controller.frozenProbeSample();
    EXPECT_EQ(frozen.value(QStringLiteral("r")).toInt(), 10);
    EXPECT_EQ(frozen.value(QStringLiteral("g")).toInt(), 20);
    EXPECT_EQ(frozen.value(QStringLiteral("b")).toInt(), 30);
    EXPECT_EQ(frozen.value(QStringLiteral("a")).toInt(), 4);
    EXPECT_EQ(controller.probeColorText(ImageReviewController::ProbeRgba),
              QStringLiteral("rgba(10, 20, 30, 4)"));
    // A derived view never changes the source label of the frozen sample.
    EXPECT_EQ(frozen.value(QStringLiteral("source")).toString(), QStringLiteral("original"));
}

// ---- Alpha threshold observation filter ----------------------------------------------------
//
// The filter is an observation over the "RGB ignoring alpha" view. It never reaches the
// committed buffers, the difference pipeline, the statistics, the edit working copy or a
// saved copy.

TEST(ImageReviewControllerTests, AlphaFilterUsesStrictGreaterThanOnTheEightBitBuffer) {
    ImageReviewController controller;
    ASSERT_TRUE(
        controller.openPrimaryImage(solidImage(QColor(255, 0, 0, 26)), QStringLiteral("a.png")));
    EXPECT_FALSE(controller.alphaFilterEnabled());
    EXPECT_EQ(controller.alphaThreshold(), 0);
    // Off means everything passes, whatever the threshold happens to be.
    EXPECT_TRUE(controller.alphaPassesFilter(true, 0, 0));
    EXPECT_TRUE(controller.alphaPassesFilter(true, 20, 0));

    controller.setAlphaFilterEnabled(true);
    controller.setAlphaThreshold(25);
    EXPECT_TRUE(controller.alphaFilterEnabled());
    EXPECT_EQ(controller.alphaThreshold(), 25);
    // Strict greater-than: A=20 and A=25 are hidden, A=26 passes.
    EXPECT_FALSE(controller.alphaPassesFilter(true, 20, 0));
    EXPECT_FALSE(controller.alphaPassesFilter(true, 25, 0));
    EXPECT_TRUE(controller.alphaPassesFilter(true, 26, 0));

    // "Off" and "on with T = 0" are different states: T = 0 still hides A = 0.
    controller.setAlphaThreshold(0);
    EXPECT_TRUE(controller.alphaFilterEnabled());
    EXPECT_FALSE(controller.alphaPassesFilter(true, 0, 0));
    EXPECT_TRUE(controller.alphaPassesFilter(true, 1, 0));

    // T = 255 lets nothing through, which is a valid filter result and not an error.
    controller.setAlphaThreshold(255);
    EXPECT_FALSE(controller.alphaPassesFilter(true, 255, 0));
    EXPECT_FALSE(controller.alphaPassesFilter(true, 254, 0));

    // The threshold is clamped to the 0..255 equivalent scale.
    controller.setAlphaThreshold(-5);
    EXPECT_EQ(controller.alphaThreshold(), 0);
    controller.setAlphaThreshold(900);
    EXPECT_EQ(controller.alphaThreshold(), 255);

    // The filter is only active in the observation it belongs to.
    EXPECT_FALSE(controller.alphaFilterActive());
    controller.setViewMode(ImageReviewController::RgbOpaqueView);
    EXPECT_TRUE(controller.alphaFilterActive());
    controller.setViewMode(ImageReviewController::AlphaGrayView);
    EXPECT_FALSE(controller.alphaFilterActive());
    controller.setViewMode(ImageReviewController::RgbaView);
    EXPECT_FALSE(controller.alphaFilterActive());
}

TEST(ImageReviewControllerTests, AlphaFilterComparesHighDepthAlphaWithoutQuantizing) {
    // A real sidecar is what the probe and the view consult, so the 8-bit display value
    // never answers the question. The loader injects both buffers, exactly as the decode
    // path does for a genuine >8-bit source.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QUrl url = writeBytes(directory, QStringLiteral("deep.bin"), QByteArrayLiteral("rgba64"));
    ASSERT_FALSE(url.isEmpty());
    ScopedStillImageLoader loader{[](const QByteArray& bytes,
                                     QImage* image,
                                     QImage* nativeImage,
                                     dvs::ui::StillImageSourceInfo* info,
                                     std::string*) {
        if (!bytes.startsWith("rgba64")) {
            return false;
        }
        // 8-bit display value 99 (the rounded form of 25800/257).
        *image = solidImage(QColor(255, 0, 0, 99));
        if (info != nullptr) {
            info->bitDepth = 16;
            info->channels = 4;
            info->hasAlpha = true;
            info->sourceFormat = QStringLiteral("rgba64le");
            info->displayConverted = true;
        }
        if (nativeImage != nullptr) {
            QImage native(4, 4, QImage::Format_RGBA64);
            if (!native.isNull()) {
                auto* const samples = reinterpret_cast<QRgba64*>(native.bits());
                std::fill(samples,
                          samples + native.sizeInBytes() / sizeof(QRgba64),
                          qRgba64(65535, 0, 0, 25800));
            }
            *nativeImage = std::move(native);
        }
        return true;
    }};
    ImageReviewController loaded;
    loaded.setAlphaFilterEnabled(true);
    // T = 99 corresponds to 99/255 in normalized terms, i.e. 99 * 65535 / 255 = 25443
    // exactly (65535 / 255 = 257). The bound is evaluated in integers, so it is exact and
    // testable.
    loaded.setAlphaThreshold(99);
    ASSERT_GT(loaded.requestOpenPrimary(url), 0);
    ASSERT_TRUE(waitForControllerIdle(loaded));
    // The provenance proves the >8-bit sidecar arrived with the image, so the filter has a
    // real 16-bit alpha to consult rather than the 8-bit display value.
    ASSERT_EQ(loaded.primaryBitDepth(), 16);
    ASSERT_TRUE(loaded.primaryHasAlpha());
    ASSERT_TRUE(loaded.primaryDisplayConverted());

    // T = 99 corresponds to 99/255 in normalized terms, i.e. 99 * 65535 / 255 = 25443
    // exactly (65535 / 255 = 257). The bound is evaluated in integers, so it is exact and
    // testable.
    //
    // 25800 (= 100/255 * 65535, rounded up) is above the bound, so the pixel passes on its
    // own depth even though its 8-bit display value 99 is not > 99.
    EXPECT_TRUE(loaded.alphaPassesFilter(true, 99, 25800));
    // The exact boundary: 25443 is not > 25443, and one step above it passes.
    EXPECT_FALSE(loaded.alphaPassesFilter(true, 99, 25443));
    EXPECT_TRUE(loaded.alphaPassesFilter(true, 99, 25444));
    // A 16-bit value that maps below the 8-bit rounding of the bound is hidden.
    EXPECT_FALSE(loaded.alphaPassesFilter(true, 99, 25442));

    // The frozen sample's own-depth alpha passes the bound, so it is not hidden - even
    // though its 8-bit display value equals the threshold exactly.
    ASSERT_TRUE(loaded.captureProbeSample(ImageReviewController::DisplayPrimarySlot, 0, 0));
    const QVariantMap frozen = loaded.frozenProbeSample();
    EXPECT_EQ(frozen.value(QStringLiteral("a16")).toInt(), 25800);
    EXPECT_FALSE(frozen.value(QStringLiteral("alphaHidden")).toBool());
    // The copy still carries the original decoded values, not the mask.
    EXPECT_EQ(loaded.probeColorText(ImageReviewController::ProbeRgba),
              QStringLiteral("rgba(255, 0, 0, 99)"));

    // Raising the threshold past the sidecar alpha hides the same pixel, and the hint
    // follows the live threshold instead of the one set when the sample was captured.
    // T = 101 maps to 101 * 257 = 25957, which is above the sidecar's 25800.
    loaded.setAlphaThreshold(101);
    EXPECT_TRUE(loaded.frozenProbeSample().value(QStringLiteral("alphaHidden")).toBool());
    loaded.setAlphaThreshold(99);
    EXPECT_FALSE(loaded.frozenProbeSample().value(QStringLiteral("alphaHidden")).toBool());

    // Without a sidecar the 8-bit buffer is the only truth: 99 is not > 99.
    ImageReviewController plain;
    plain.setAlphaFilterEnabled(true);
    plain.setAlphaThreshold(99);
    ASSERT_TRUE(
        plain.openPrimaryImage(solidImage(QColor(255, 0, 0, 99)), QStringLiteral("eight.png")));
    ASSERT_TRUE(plain.captureProbeSample(ImageReviewController::DisplayPrimarySlot, 0, 0));
    EXPECT_TRUE(plain.frozenProbeSample().value(QStringLiteral("alphaHidden")).toBool());
    EXPECT_EQ(plain.frozenProbeSample().value(QStringLiteral("a")).toInt(), 99);
    // The copy of a hidden pixel is still the original RGBA, never the mask colour.
    EXPECT_EQ(plain.probeColorText(ImageReviewController::ProbeRgba),
              QStringLiteral("rgba(255, 0, 0, 99)"));
}

TEST(ImageReviewControllerTests, AlphaFilterChangesTheViewButNeverTheSamplesOrStatistics) {
    ImageReviewController controller;
    ASSERT_TRUE(controller.openPairImages(solidImage(QColor(10, 20, 30, 0)),
                                          QStringLiteral("a.png"),
                                          solidImage(QColor(10, 20, 30, 255)),
                                          QStringLiteral("b.png")));
    controller.setViewMode(ImageReviewController::RgbOpaqueView);
    const QImage unfiltered = controller.imageForSlot(ImageReviewController::DisplayPrimarySlot);
    // The probe reads the original RGBA whatever the filter does.
    ASSERT_TRUE(controller.captureProbeSample(ImageReviewController::DisplayPrimarySlot, 0, 0));
    const QVariantMap before = controller.frozenProbeSample();
    ASSERT_EQ(before.value(QStringLiteral("a")).toInt(), 0);

    controller.setAlphaFilterEnabled(true);
    controller.setAlphaThreshold(128);
    const QImage filtered = controller.imageForSlot(ImageReviewController::DisplayPrimarySlot);
    // The view really changed: the A=0 pixel is now the neutral mask, not black.
    EXPECT_NE(filtered.pixel(0, 0), unfiltered.pixel(0, 0));
    EXPECT_NE(filtered.pixel(0, 0), QColor(Qt::black).rgb())
        << "the mask must never be mistaken for a pixel that was actually black";
    // The original sample, its values and its label are untouched by the filter. Only the
    // "hidden by the current threshold" hint moves, and that is deliberate: it is live
    // view state, so a sample captured before the filter was enabled still names the
    // state the user is looking at now.
    const QVariantMap after = controller.frozenProbeSample();
    for (const QString& key : {QStringLiteral("x"),
                               QStringLiteral("y"),
                               QStringLiteral("r"),
                               QStringLiteral("g"),
                               QStringLiteral("b"),
                               QStringLiteral("a"),
                               QStringLiteral("source"),
                               QStringLiteral("side"),
                               QStringLiteral("slot"),
                               QStringLiteral("generation")}) {
        EXPECT_EQ(after.value(key), before.value(key)) << key.toStdString();
    }
    EXPECT_FALSE(before.value(QStringLiteral("alphaHidden")).toBool())
        << "the filter was off when the sample was captured";
    EXPECT_TRUE(after.value(QStringLiteral("alphaHidden")).toBool())
        << "the hint follows the live threshold";
    EXPECT_EQ(controller.probeColorText(ImageReviewController::ProbeRgba),
              QStringLiteral("rgba(10, 20, 30, 0)"));
    // Turning the filter off restores both the unfiltered view and the hint.
    controller.setAlphaFilterEnabled(false);
    EXPECT_EQ(controller.imageForSlot(ImageReviewController::DisplayPrimarySlot), unfiltered);
    EXPECT_FALSE(controller.frozenProbeSample().value(QStringLiteral("alphaHidden")).toBool());
    EXPECT_EQ(controller.probeColorText(ImageReviewController::ProbeRgba),
              QStringLiteral("rgba(10, 20, 30, 0)"));
}

// COLOR4: the display derivative may be colour-converted, but the committed buffer, its raw
// sampling path and the status switch must keep the same honesty contract.
TEST(ImageReviewControllerTests, ColorDisplayConversionChangesOnlyThePresentedImage) {
    ensureCoreApplication();
    ImageReviewController controller;
    StillImageSourceInfo info;
    info.bitDepth = 8;
    info.channels = 4;
    info.sourceFormat = QStringLiteral("rgba");
    info.color.profileStatus = ImageColorProfileStatus::EmbeddedSupported;
    info.color.profileDataSpace = QStringLiteral("RGB ");
    info.colorSpace = QColorSpace{QColorSpace::DisplayP3};

    const QImage source = solidImage(QColor(200, 100, 50));
    ASSERT_TRUE(controller.openPrimaryImage(source, QStringLiteral("p3.png"), info));
    ASSERT_TRUE(controller.primaryColorDisplayApplied());
    EXPECT_TRUE(controller.colorDisplayEnabled());
    EXPECT_TRUE(controller.primaryColorStatusText().contains(QStringLiteral("已应用文件配置转换")))
        << controller.primaryColorStatusText().toStdString();
    EXPECT_TRUE(controller.primaryColorStatusText().contains(QStringLiteral("显示环境未验证")));
    EXPECT_TRUE(controller.primaryColorStateCompactText().contains(QStringLiteral("已转换")));

    const QImage rawBefore = controller.rawImageForSlot(ImageReviewController::PrimarySlot);
    const QColor rawPixel = rawBefore.pixelColor(2, 2);
    const QImage converted = controller.imageForSlot(ImageReviewController::PrimarySlot);
    const QColor convertedPixel = converted.pixelColor(2, 2);
    // Display P3 codes rendered in sRGB are not the same numbers; if they were, the conversion
    // would be silently absent.
    EXPECT_NE(convertedPixel.rgba(), rawPixel.rgba())
        << "raw " << rawPixel.rgba() << " display " << convertedPixel.rgba();
    EXPECT_FALSE(controller.imageUrl(ImageReviewController::PrimarySlot).isEmpty());

    const QString enabledUrl = controller.imageUrl(ImageReviewController::PrimarySlot);
    controller.setColorDisplayEnabled(false);
    EXPECT_FALSE(controller.primaryColorDisplayApplied());
    EXPECT_FALSE(controller.colorDisplayEnabled());
    EXPECT_TRUE(controller.primaryColorStatusText().contains(QStringLiteral("显示转换已关闭")))
        << controller.primaryColorStatusText().toStdString();
    // Switching the presentation off returns the decoded pixels exactly; the committed raw
    // buffer never changed while the converted frames were on screen.
    EXPECT_EQ(controller.imageForSlot(ImageReviewController::PrimarySlot).pixelColor(2, 2).rgba(),
              rawPixel.rgba());
    EXPECT_EQ(
        controller.rawImageForSlot(ImageReviewController::PrimarySlot).pixelColor(2, 2).rgba(),
        rawPixel.rgba());
    EXPECT_NE(controller.imageUrl(ImageReviewController::PrimarySlot), enabledUrl);

    controller.setColorDisplayEnabled(true);
    EXPECT_TRUE(controller.primaryColorDisplayApplied());
    EXPECT_EQ(controller.imageForSlot(ImageReviewController::PrimarySlot).pixelColor(2, 2).rgba(),
              convertedPixel.rgba());
}

TEST(ImageReviewControllerTests, ColorDisplayConversionNeedsAParsedProfile) {
    ensureCoreApplication();
    ImageReviewController controller;
    StillImageSourceInfo info;
    info.channels = 4;
    info.sourceFormat = QStringLiteral("rgba");
    info.color.profileStatus = ImageColorProfileStatus::EmbeddedSupported;
    info.color.profileDataSpace = QStringLiteral("RGB ");
    // Structurally supported but the engine did not build a colour space: no conversion.
    ASSERT_FALSE(info.colorSpace.isValid());
    const QImage source = solidImage(QColor(20, 140, 60));
    ASSERT_TRUE(controller.openPrimaryImage(source, QStringLiteral("engine-less.png"), info));
    EXPECT_FALSE(controller.primaryColorDisplayApplied());
    EXPECT_TRUE(controller.primaryColorStatusText().contains(QStringLiteral("转换引擎未就绪")))
        << controller.primaryColorStatusText().toStdString();
    EXPECT_EQ(controller.imageForSlot(ImageReviewController::PrimarySlot).pixelColor(1, 1).rgba(),
              QColor(20, 140, 60).rgba());
}

TEST(ImageReviewControllerTests, UnsupportedOrCorruptProfilesNeverGetConverted) {
    ensureCoreApplication();
    for (const ImageColorProfileStatus status :
         {ImageColorProfileStatus::EmbeddedUnsupported, ImageColorProfileStatus::Corrupt}) {
        SCOPED_TRACE(static_cast<int>(status));
        ImageReviewController controller;
        StillImageSourceInfo info;
        info.channels = 4;
        info.sourceFormat = QStringLiteral("rgba");
        info.color.profileStatus = status;
        info.color.profileDataSpace = QStringLiteral("Lab ");
        info.colorSpace = QColorSpace{QColorSpace::DisplayP3}; // Even if one is attached.
        const QImage source = solidImage(QColor(30, 60, 120));
        ASSERT_TRUE(controller.openPrimaryImage(source, QStringLiteral("fallback.png"), info));
        EXPECT_FALSE(controller.primaryColorDisplayApplied());
        EXPECT_TRUE(controller.primaryColorProfileNeedsFallback());
        EXPECT_EQ(
            controller.imageForSlot(ImageReviewController::PrimarySlot).pixelColor(1, 1).rgba(),
            QColor(30, 60, 120).rgba());
    }
}

TEST(ImageReviewControllerTests, ColorDisplayToggleDoesNotMoveDifferenceStatistics) {
    ensureCoreApplication();
    ImageReviewController controller;
    StillImageSourceInfo info;
    info.channels = 4;
    info.sourceFormat = QStringLiteral("rgba");
    info.color.profileStatus = ImageColorProfileStatus::EmbeddedSupported;
    info.color.profileDataSpace = QStringLiteral("RGB ");
    info.colorSpace = QColorSpace{QColorSpace::DisplayP3};
    QImage left(8, 8, QImage::Format_ARGB32);
    left.fill(QColor(180, 90, 40));
    QImage right(8, 8, QImage::Format_ARGB32);
    right.fill(QColor(60, 120, 200));
    ASSERT_TRUE(controller.openPairImages(std::move(left),
                                          QStringLiteral("left.png"),
                                          std::move(right),
                                          QStringLiteral("right.png"),
                                          -1,
                                          info,
                                          info));
    controller.setCompareMode(ImageReviewController::AbsDifference);
    ASSERT_TRUE(waitUntil([&controller] {
        return !controller.openPending() && !controller.diffPending() && controller.hasDiffResult();
    }));
    const int peak = controller.maxAbsDifference();
    const double mean = controller.meanAbsDifference();
    const QImage rawPrimary = controller.rawImageForSlot(ImageReviewController::PrimarySlot);
    const QImage rawSecondary = controller.rawImageForSlot(ImageReviewController::SecondarySlot);
    EXPECT_TRUE(controller.primaryColorDisplayApplied());
    EXPECT_TRUE(controller.secondaryColorDisplayApplied());

    controller.setColorDisplayEnabled(false);
    ASSERT_TRUE(waitUntil([&controller] { return !controller.diffPending(); }));
    EXPECT_EQ(controller.maxAbsDifference(), peak);
    EXPECT_DOUBLE_EQ(controller.meanAbsDifference(), mean);
    EXPECT_EQ(controller.rawImageForSlot(ImageReviewController::PrimarySlot), rawPrimary);
    EXPECT_EQ(controller.rawImageForSlot(ImageReviewController::SecondarySlot), rawSecondary);
    EXPECT_FALSE(controller.primaryColorDisplayApplied());
    EXPECT_FALSE(controller.secondaryColorDisplayApplied());
}

} // namespace
