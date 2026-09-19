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
#include <gtest/gtest.h>
#include <string>

namespace {

using dvs::ui::ImageReviewController;

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
    ScopedStillImageLoader loader{
        [&loaderCalls](const QByteArray& bytes, QImage* image, std::string*) {
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

    ScopedStillImageLoader loader{[](const QByteArray& bytes, QImage* image, std::string* error) {
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
    ScopedStillImageLoader loader{[&started](const QByteArray&, QImage* image, std::string*) {
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
    ScopedStillImageLoader loader{
        [&decodeCalls](const QByteArray& bytes, QImage* image, std::string*) {
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
    ScopedStillImageLoader decoder{[](const QByteArray&, QImage* image, std::string*) {
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
    std::atomic<int> visitedRows{0};
    worker->setDifferenceRowObserverForTesting([&](const int row) {
        ++visitedRows;
        if (row == 0) {
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
    EXPECT_FALSE(controller.diffPending());
    EXPECT_FALSE(controller.hasDiffResult());
    release.release();
    EXPECT_TRUE(waitUntil([&] { return worker->stats().activeRequests == 0; }));
    EXPECT_EQ(visitedRows.load(), 1);
    EXPECT_FALSE(controller.hasDiffResult());
    EXPECT_EQ(controller.asyncStats().value("diff_cache_entries").toInt(), 0);
    worker->setDifferenceRowObserverForTesting({});
    controller.setCompareMode(ImageReviewController::AbsDifference);
    ASSERT_TRUE(waitForControllerIdle(controller));
    EXPECT_TRUE(controller.hasDiffResult());
    EXPECT_EQ(controller.maxAbsDifference(), 0);
    controller.setCompareMode(ImageReviewController::SideBySide);
    controller.setCompareMode(ImageReviewController::AbsDifference);
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
    policy.loader = [&](const QByteArray& bytes, QImage* image, std::string*) {
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
    ScopedStillImageLoader loader{[&decoderCalls](const QByteArray&, QImage* image, std::string*) {
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
} // namespace
