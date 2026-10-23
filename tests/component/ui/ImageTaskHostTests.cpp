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
