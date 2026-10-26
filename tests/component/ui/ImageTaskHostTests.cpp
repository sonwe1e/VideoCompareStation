#include "dvs/ui/ImageEditController.h"
#include "dvs/ui/ImageReviewController.h"
#include "dvs/ui/ImageTaskHost.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QVariantList>

#include <chrono>
#include <functional>
#include <gtest/gtest.h>
#include <string>
#include <utility>

using dvs::ui::ImageReviewController;

namespace {

void ensureCoreApplication() {
    if (QCoreApplication::instance() == nullptr) {
        static int argc = 1;
        static char name[] = "dvs_image_task_host_tests";
        static char* argv[] = {name, nullptr};
        static QCoreApplication application{argc, argv};
        static_cast<void>(application);
    }
}

[[nodiscard]] bool waitUntil(const std::function<bool()>& predicate, const int timeoutMs = 5000) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeoutMs);
    while (clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(2U);
    }
    return predicate();
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
    const QString path = QDir{directory.path()}.filePath(name);
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

[[nodiscard]] ImageReviewController::StillImageLoader solidLoader(int* decodeCalls = nullptr) {
    return [decodeCalls](const QByteArray&,
                         QImage* image,
                         QImage*,
                         dvs::ui::StillImageSourceInfo* info,
                         std::string*) {
        if (decodeCalls != nullptr) {
            ++(*decodeCalls);
        }
        QImage decoded{16, 16, QImage::Format_RGBA8888};
        if (decoded.isNull()) {
            return false;
        }
        decoded.fill(QColor(40, 80, 120, 255));
        *image = decoded;
        if (info != nullptr) {
            info->bitDepth = 8;
            info->channels = 4;
            info->hasAlpha = true;
            info->sourceFormat = QStringLiteral("rgba");
        }
        return true;
    };
}

} // namespace

TEST(ImageTaskHostTests, EditComparisonKeepsAnImmutableVersionAndIndependentTaskIdentity) {
    ensureCoreApplication();
    ImageReviewController initial;
    dvs::ui::ImageTaskHost host{&initial};
    ASSERT_EQ(host.ensureActiveController(), &initial);
    QImage source(32, 24, QImage::Format_ARGB32);
    source.fill(Qt::blue);
    ASSERT_TRUE(initial.openPrimaryImage(source, QStringLiteral("source.png")));
    initial.restoreObservationPosition(2.0, 0.1, 0.2);
    dvs::ui::ImageEditController edit;
    edit.setSourceImageProvider([source](int) { return source; });
    ASSERT_TRUE(edit.beginSession(2, QStringLiteral("source.png"), {}));
    ASSERT_TRUE(edit.fillImageRect(2, 3, 8, 6, Qt::red));
    const QImage firstRevision = edit.editedImage().copy();
    const qint64 beforeBytes = host.workingSetBytes();
    int recentOpenCount = 0;
    QObject::connect(
        &host, &dvs::ui::ImageTaskHost::taskOpened, [&recentOpenCount](int) { ++recentOpenCount; });

    ASSERT_EQ(host.openEditComparison(&edit), 1);
    auto* const snapshot = host.activeController();
    ASSERT_NE(snapshot, nullptr);
    EXPECT_NE(snapshot, &initial);
    EXPECT_NE(snapshot->imageToken(), initial.imageToken());
    EXPECT_EQ(host.controllerForToken(snapshot->imageToken()), snapshot);
    EXPECT_EQ(snapshot->rawImageForSlot(ImageReviewController::PrimarySlot), source);
    EXPECT_EQ(snapshot->rawImageForSlot(ImageReviewController::SecondarySlot), firstRevision);
    EXPECT_EQ(snapshot->compareMode(), ImageReviewController::SideBySide);
    EXPECT_DOUBLE_EQ(snapshot->zoom(), initial.zoom());
    EXPECT_FALSE(snapshot->colorDisplayEnabled());
    EXPECT_GT(host.workingSetBytes(), beforeBytes);
    EXPECT_EQ(recentOpenCount, 0);
    EXPECT_TRUE(host.tabs().at(1).toMap().value(QStringLiteral("editComparison")).toBool());
    ASSERT_EQ(host.openEditComparison(&edit), 1);
    EXPECT_EQ(host.count(), 2);
    snapshot->setCompareMode(ImageReviewController::AbsDifference);
    ASSERT_TRUE(
        waitUntil([snapshot] { return !snapshot->diffPending() && snapshot->hasDiffResult(); }));
    EXPECT_EQ(snapshot->maxAbsDifference(), 255);
    EXPECT_GT(snapshot->meanAbsDifference(), 0.0);

    ASSERT_TRUE(edit.undo());
    EXPECT_EQ(snapshot->rawImageForSlot(ImageReviewController::SecondarySlot), firstRevision);
    ASSERT_EQ(host.openEditComparison(&edit), 2);
    EXPECT_EQ(host.count(), 3);
    EXPECT_EQ(host.activeController()->rawImageForSlot(ImageReviewController::SecondarySlot),
              source);
    auto* const reverted = host.activeController();
    reverted->setCompareMode(ImageReviewController::AbsDifference);
    ASSERT_TRUE(
        waitUntil([reverted] { return !reverted->diffPending() && reverted->hasDiffResult(); }));
    EXPECT_EQ(reverted->maxAbsDifference(), 0);
    EXPECT_EQ(snapshot->maxAbsDifference(), 255);
    EXPECT_EQ(initial.rawImageForSlot(ImageReviewController::PrimarySlot), source);
    edit.endSession();
    ASSERT_TRUE(host.activate(1));
    EXPECT_EQ(snapshot->rawImageForSlot(ImageReviewController::SecondarySlot), firstRevision);
}

TEST(ImageTaskHostTests, EditComparisonRejectsFolderReplacementWithoutChangingItsPixels) {
    ensureCoreApplication();
    ImageReviewController initial;
    dvs::ui::ImageTaskHost host{&initial};
    ASSERT_EQ(host.ensureActiveController(), &initial);
    QImage source(16, 16, QImage::Format_ARGB32);
    source.fill(Qt::blue);
    ASSERT_TRUE(initial.openPrimaryImage(source, QStringLiteral("source.png")));
    dvs::ui::ImageEditController edit;
    edit.setSourceImageProvider([source](int) { return source; });
    ASSERT_TRUE(edit.beginSession(2, QStringLiteral("source.png"), {}));
    ASSERT_EQ(host.openEditComparison(&edit), 1);
    auto* const snapshot = host.activeController();
    QString error;
    EXPECT_EQ(host.requestOpenPrimaryInActiveTask(QUrl{"file:///replacement.png"}, 0, &error), -1);
    EXPECT_TRUE(error.contains(QStringLiteral("固定快照")));
    error.clear();
    EXPECT_EQ(
        host.requestOpenPairInActiveTask(QUrl{"file:///a.png"}, QUrl{"file:///b.png"}, 0, &error),
        -1);
    EXPECT_TRUE(error.contains(QStringLiteral("固定快照")));
    EXPECT_FALSE(snapshot->openPending());
    EXPECT_EQ(snapshot->rawImageForSlot(ImageReviewController::PrimarySlot), source);
    EXPECT_EQ(host.count(), 2);
}

TEST(ImageTaskHostTests, EditComparisonBudgetFailurePreservesTheOriginalTaskAndWorkingCopy) {
    ensureCoreApplication();
    ImageReviewController initial;
    dvs::ui::ImageTaskHost host{&initial, 1};
    ASSERT_EQ(host.ensureActiveController(), &initial);
    QImage source(16, 16, QImage::Format_ARGB32);
    source.fill(Qt::blue);
    ASSERT_TRUE(initial.openPrimaryImage(source, QStringLiteral("source.png")));
    dvs::ui::ImageEditController edit;
    edit.setSourceImageProvider([source](int) { return source; });
    ASSERT_TRUE(edit.beginSession(2, QStringLiteral("source.png"), {}));
    ASSERT_TRUE(edit.fillImageRect(0, 0, 4, 4, Qt::red));
    const QImage edited = edit.editedImage().copy();
    EXPECT_EQ(host.openEditComparison(&edit), -1);
    EXPECT_EQ(host.count(), 1);
    EXPECT_EQ(host.activeController(), &initial);
    EXPECT_TRUE(edit.active());
    EXPECT_TRUE(edit.dirty());
    EXPECT_EQ(edit.editedImage(), edited);
}

TEST(ImageTaskHostTests, OpensFirstImageAsTheInitialTab) {
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QUrl first = writeBytes(directory, QStringLiteral("first.png"), QByteArrayLiteral("one"));
    ASSERT_FALSE(first.isEmpty());
    ScopedStillImageLoader loader{solidLoader()};

    ImageReviewController initial;
    dvs::ui::ImageTaskHost host{&initial};
    int openedCount = 0;
    QObject::connect(
        &host, &dvs::ui::ImageTaskHost::taskOpened, [&openedCount](int) { ++openedCount; });

    ASSERT_EQ(host.openImages({QVariant::fromValue(first)}), 0);
    ASSERT_TRUE(waitUntil([&host] {
        return host.count() == 1 && host.activeIndex() == 0 && host.activeController() != nullptr &&
               host.activeController()->hasPrimary();
    }));
    EXPECT_EQ(host.activeController(), &initial);
    EXPECT_EQ(openedCount, 1);
    ASSERT_EQ(host.tabs().size(), 1);
    const QVariantMap tab = host.tabs().constFirst().toMap();
    EXPECT_EQ(tab.value(QStringLiteral("title")).toString(), QStringLiteral("first.png"));
    EXPECT_FALSE(tab.value(QStringLiteral("loading")).toBool());
}

TEST(ImageTaskHostTests, SecondOpenKeepsTheFirstTaskAliveAndActivatesTheNewTab) {
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QUrl first = writeBytes(directory, QStringLiteral("first.png"), QByteArrayLiteral("one"));
    const QUrl second =
        writeBytes(directory, QStringLiteral("second.png"), QByteArrayLiteral("two"));
    ASSERT_FALSE(first.isEmpty() || second.isEmpty());
    ScopedStillImageLoader loader{solidLoader()};

    ImageReviewController initial;
    dvs::ui::ImageTaskHost host{&initial};
    ASSERT_EQ(host.openImages({QVariant::fromValue(first)}), 0);
    ASSERT_TRUE(waitUntil([&host] {
        const auto* active = host.activeController();
        return active != nullptr && active->hasPrimary();
    }));
    ImageReviewController* const firstController = host.activeController();

    ASSERT_EQ(host.openImages({QVariant::fromValue(second)}), 1);
    ASSERT_TRUE(waitUntil([&host] {
        return host.count() == 2 && host.activeIndex() == 1 &&
               host.activeController()->hasPrimary();
    }));
    EXPECT_NE(host.activeController(), firstController);
    EXPECT_TRUE(host.activeController()->primaryPath().endsWith(QStringLiteral("second.png")));
    EXPECT_TRUE(firstController->hasPrimary());
    EXPECT_TRUE(firstController->primaryPath().endsWith(QStringLiteral("first.png")));
}

TEST(ImageTaskHostTests, DuplicateOpenActivatesTheExistingTabWithoutReloading) {
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QUrl first = writeBytes(directory, QStringLiteral("first.png"), QByteArrayLiteral("one"));
    const QUrl second =
        writeBytes(directory, QStringLiteral("second.png"), QByteArrayLiteral("two"));
    ASSERT_FALSE(first.isEmpty() || second.isEmpty());
    int decodeCalls = 0;
    ScopedStillImageLoader loader{solidLoader(&decodeCalls)};

    ImageReviewController initial;
    dvs::ui::ImageTaskHost host{&initial};
    ASSERT_EQ(host.openImages({QVariant::fromValue(first)}), 0);
    ASSERT_TRUE(waitUntil([&host] {
        const auto* active = host.activeController();
        return active != nullptr && active->hasPrimary();
    }));
    ASSERT_EQ(host.openImages({QVariant::fromValue(second)}), 1);
    ASSERT_TRUE(waitUntil([&host] { return host.count() == 2 && host.activeIndex() == 1; }));
    const int callsBeforeDuplicate = decodeCalls;

    ASSERT_EQ(host.openImages({QVariant::fromValue(first)}), 0);
    EXPECT_EQ(host.count(), 2);
    EXPECT_EQ(host.activeIndex(), 0);
    EXPECT_EQ(decodeCalls, callsBeforeDuplicate);
}

TEST(ImageTaskHostTests, ClosingTheActiveTabSelectsItsNeighbour) {
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QUrl first = writeBytes(directory, QStringLiteral("a.png"), QByteArrayLiteral("a"));
    const QUrl second = writeBytes(directory, QStringLiteral("b.png"), QByteArrayLiteral("b"));
    const QUrl third = writeBytes(directory, QStringLiteral("c.png"), QByteArrayLiteral("c"));
    ASSERT_FALSE(first.isEmpty() || second.isEmpty() || third.isEmpty());
    ScopedStillImageLoader loader{solidLoader()};

    ImageReviewController initial;
    dvs::ui::ImageTaskHost host{&initial};
    int opened = 0;
    for (const QUrl& url : {first, second, third}) {
        ASSERT_GE(host.openImages({QVariant::fromValue(url)}), 0);
        ++opened;
        ASSERT_TRUE(waitUntil([&host, opened] {
            const auto* const active = host.activeController();
            return host.count() == opened && host.activeIndex() == opened - 1 &&
                   active != nullptr && active->hasPrimary();
        }));
    }
    ASSERT_TRUE(waitUntil([&host] { return host.count() == 3; }));
    ASSERT_TRUE(host.activate(1));
    EXPECT_EQ(host.activeIndex(), 1);

    ASSERT_TRUE(host.closeActive());
    EXPECT_EQ(host.count(), 2);
    EXPECT_EQ(host.activeIndex(), 1);
    EXPECT_TRUE(host.activeController()->primaryPath().endsWith(QStringLiteral("c.png")));
}

TEST(ImageTaskHostTests, WorkingSetBudgetRejectsTheTabThatWouldExceedIt) {
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QUrl first = writeBytes(directory, QStringLiteral("first.png"), QByteArrayLiteral("one"));
    const QUrl second =
        writeBytes(directory, QStringLiteral("second.png"), QByteArrayLiteral("two"));
    ASSERT_FALSE(first.isEmpty() || second.isEmpty());
    ScopedStillImageLoader loader{solidLoader()};

    ImageReviewController initial;
    dvs::ui::ImageTaskHost host{&initial, 1}; // one byte: the second 16x16 pair cannot fit.
    QString failure;
    QObject::connect(&host, &dvs::ui::ImageTaskHost::openFailed, [&failure](const QString& error) {
        failure = error;
    });

    ASSERT_EQ(host.openImages({QVariant::fromValue(first)}), 0);
    ASSERT_TRUE(waitUntil([&host] {
        const auto* active = host.activeController();
        return active != nullptr && active->hasPrimary();
    }));
    ASSERT_EQ(host.openImages({QVariant::fromValue(second)}), 1);
    const bool rejected =
        waitUntil([&host, &failure] { return host.count() == 1 && !failure.isEmpty(); });
    ASSERT_TRUE(rejected);
    EXPECT_EQ(host.activeIndex(), 0);
    EXPECT_TRUE(host.activeController()->primaryPath().endsWith(QStringLiteral("first.png")));
}

TEST(ImageTaskHostTests, ImageUrlCarriesTheTaskTokenForTheProviderRouter) {
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QUrl first = writeBytes(directory, QStringLiteral("first.png"), QByteArrayLiteral("one"));
    ASSERT_FALSE(first.isEmpty());
    ScopedStillImageLoader loader{solidLoader()};

    ImageReviewController initial;
    dvs::ui::ImageTaskHost host{&initial};
    ASSERT_EQ(host.openImages({QVariant::fromValue(first)}), 0);
    ASSERT_TRUE(waitUntil([&host] {
        const auto* active = host.activeController();
        return active != nullptr && active->hasPrimary();
    }));
    const QString token = host.activeController()->imageToken();
    ASSERT_FALSE(token.isEmpty());
    EXPECT_EQ(host.controllerForToken(token), host.activeController());
    EXPECT_TRUE(host.activeController()->imageUrl(2).contains(QStringLiteral("?w=") + token));
}
