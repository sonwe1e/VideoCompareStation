#include "dvs/ui/VideoFolderModel.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dvs::ui {
namespace {

void ensureFolderApplication() {
    if (QCoreApplication::instance()) {
        return;
    }
    static int argc = 1;
    static char name[] = "VideoFolderModelTests";
    static char* argv[] = {name, nullptr};
    static QCoreApplication application{argc, argv};
    static_cast<void>(application);
}

void waitForScan(VideoFolderModel& model) {
    QElapsedTimer timer;
    timer.start();
    while (model.scanning() && timer.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    ASSERT_FALSE(model.scanning());
}

class VideoFolderModelTests : public testing::Test {
protected:
    void SetUp() override {
        ensureFolderApplication();
        ASSERT_TRUE(directory.isValid());
        model = std::make_unique<VideoFolderModel>(dependencies());
    }
    VideoFolderModel::Dependencies dependencies(const std::size_t limit = 100'000) {
        return {.schedule = [this](auto task) { tasks.push_back(std::move(task)); },
                .openVideo =
                    [this](const QUrl& url) {
                        opened.push_back(url);
                        return rejectOpen ? qulonglong{0} : ++nextIntent;
                    },
                .cancelOpen = [this](const qulonglong id) { canceled.push_back(id); },
                .play =
                    [this] {
                        ++plays;
                        return allowPlay;
                    },
                .maximumFiles = limit};
    }
    QUrl write(const QString& name) {
        const QString path = directory.filePath(name);
        QFile file{path};
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        EXPECT_EQ(file.write("not a decoded video"), 19);
        return QUrl::fromLocalFile(path);
    }
    void load() {
        ASSERT_TRUE(model->loadFolder(QUrl::fromLocalFile(directory.path())));
        ASSERT_FALSE(tasks.empty());
        tasks.back()();
        waitForScan(*model);
    }
    void threeFiles() {
        write(QStringLiteral("clip1.mp4"));
        write(QStringLiteral("clip2.mp4"));
        write(QStringLiteral("clip10.mp4"));
        load();
        ASSERT_EQ(model->fileCount(), 3);
    }
    QUrl at(const int row) {
        return model->data(model->index(row), VideoFolderModel::FileUrlRole).toUrl();
    }
    void commit(const int row, const qulonglong id) {
        model->synchronizeSources({at(row)});
        model->completeOpen(id, true, {});
    }

    QTemporaryDir directory;
    std::unique_ptr<VideoFolderModel> model;
    std::vector<VideoFolderModel::Dependencies::Task> tasks;
    std::vector<QUrl> opened;
    std::vector<qulonglong> canceled;
    qulonglong nextIntent = 0;
    int plays = 0;
    bool rejectOpen = false;
    bool allowPlay = true;
};

TEST_F(VideoFolderModelTests, ScansAsynchronouslyFiltersAndNaturallySortsWithoutDecoding) {
    const auto unicode = write(QString::fromUtf8("片段.MP4"));
    write(QString::fromUtf8("忽略.文本"));
    for (const auto& name : {"clip10.MKV",
                             "clip2.mov",
                             "Clip1.avi",
                             "clip99999999999999999999999.m4v",
                             "clip100000000000000000000000.mp4",
                             "ignore.png",
                             "ignore.txt"}) {
        write(QString::fromLatin1(name));
    }
    ASSERT_TRUE(QDir{directory.path()}.mkdir(QStringLiteral("nested.mp4")));
    QFile nested{directory.filePath(QStringLiteral("nested.mp4/hidden.mp4"))};
    ASSERT_TRUE(nested.open(QIODevice::WriteOnly));
    ASSERT_TRUE(model->loadFolder(QUrl::fromLocalFile(directory.path())));
    EXPECT_TRUE(model->scanning());
    EXPECT_EQ(model->fileCount(), 0);
    ASSERT_EQ(tasks.size(), 1U);
    tasks.front()();
    waitForScan(*model);
    ASSERT_EQ(model->fileCount(), 6);
    EXPECT_EQ(model->data(model->index(0), VideoFolderModel::FileNameRole).toString(), "Clip1.avi");
    EXPECT_EQ(model->data(model->index(1), VideoFolderModel::FileNameRole).toString(), "clip2.mov");
    EXPECT_EQ(model->data(model->index(2), VideoFolderModel::FileNameRole).toString(),
              "clip10.MKV");
    EXPECT_EQ(at(5), unicode);
    EXPECT_TRUE(model->errorText().isEmpty());
    EXPECT_TRUE(opened.empty());
    EXPECT_EQ(model->rowCount(model->index(0)), 0);
    EXPECT_FALSE(model->data(QModelIndex{}, VideoFolderModel::FileUrlRole).isValid());
    EXPECT_EQ(model->roleNames().value(VideoFolderModel::FileNameRole), "fileName");
}

TEST_F(VideoFolderModelTests, OlderScanCannotOverwriteNewFolder) {
    threeFiles();
    QTemporaryDir second;
    ASSERT_TRUE(second.isValid());
    QFile other{second.filePath(QStringLiteral("other.mp4"))};
    ASSERT_TRUE(other.open(QIODevice::WriteOnly));
    ASSERT_TRUE(model->loadFolder(QUrl::fromLocalFile(directory.path())));
    const auto old = tasks.back();
    ASSERT_TRUE(model->loadFolder(QUrl::fromLocalFile(second.path())));
    tasks.back()();
    old();
    waitForScan(*model);
    EXPECT_EQ(model->folderUrl(), QUrl::fromLocalFile(second.path()));
    ASSERT_EQ(model->fileCount(), 1);
    EXPECT_EQ(model->data(model->index(0), VideoFolderModel::FileNameRole).toString(), "other.mp4");
}

TEST_F(VideoFolderModelTests, CancellationAndClearPreventLatePublication) {
    threeFiles();
    ASSERT_TRUE(model->loadFolder(QUrl::fromLocalFile(directory.path())));
    model->cancelScan();
    tasks.back()();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    EXPECT_FALSE(model->scanning());
    EXPECT_EQ(model->fileCount(), 3);
    ASSERT_TRUE(model->loadFolder(QUrl::fromLocalFile(directory.path())));
    model->clear();
    tasks.back()();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    EXPECT_EQ(model->fileCount(), 0);
    EXPECT_TRUE(model->folderUrl().isEmpty());
    EXPECT_TRUE(model->folderName().isEmpty());
}

TEST_F(VideoFolderModelTests, InvalidAndUnreadableFolderKeepPriorList) {
    threeFiles();
    const auto previous = model->folderUrl();
    for (const auto& invalid :
         {QUrl{}, QUrl{"https://example.invalid/videos"}, QUrl{"file://server/share"}}) {
        EXPECT_FALSE(model->loadFolder(invalid));
        EXPECT_EQ(model->folderUrl(), previous);
    }
    ASSERT_TRUE(model->loadFolder(QUrl::fromLocalFile(directory.filePath("not-a-folder"))));
    tasks.back()();
    waitForScan(*model);
    EXPECT_EQ(model->fileCount(), 3);
    EXPECT_EQ(model->folderUrl(), previous);
    EXPECT_FALSE(model->errorText().isEmpty());
}

TEST_F(VideoFolderModelTests, RefreshPreservesActualVideoByUrlNotOldRow) {
    threeFiles();
    const auto video = at(1);
    model->synchronizeSources({video});
    ASSERT_EQ(model->currentRow(), 1);
    write(QStringLiteral("clip0.mp4"));
    ASSERT_TRUE(model->refreshFolder());
    tasks.back()();
    waitForScan(*model);
    EXPECT_EQ(model->currentRow(), 2);
    EXPECT_EQ(at(model->currentRow()), video);
    model->synchronizeSources({video, at(0)});
    EXPECT_EQ(model->currentRow(), -1);
}

TEST_F(VideoFolderModelTests, StartsPlaybackOnlyAfterMatchingCommittedOpen) {
    threeFiles();
    ASSERT_TRUE(model->openAt(1));
    EXPECT_EQ(model->currentRow(), -1);
    EXPECT_EQ(model->pendingRow(), 1);
    EXPECT_EQ(plays, 0);
    model->completeOpen(999, true, {});
    EXPECT_TRUE(model->openPending());
    EXPECT_EQ(plays, 0);
    commit(1, 1);
    EXPECT_EQ(model->currentRow(), 1);
    EXPECT_EQ(plays, 1);
    EXPECT_FALSE(model->openPending());
    model->completeOpen(1, true, {});
    EXPECT_EQ(plays, 1);
}

TEST_F(VideoFolderModelTests, FailedOrRejectedOpenDoesNotChangeCurrentVideo) {
    threeFiles();
    model->synchronizeSources({at(0)});
    ASSERT_TRUE(model->openAt(2));
    model->completeOpen(1, false, QStringLiteral("corrupt video"));
    EXPECT_EQ(model->currentRow(), 0);
    EXPECT_EQ(plays, 0);
    EXPECT_FALSE(model->openPending());
    EXPECT_EQ(model->errorText(), "corrupt video");
    rejectOpen = true;
    EXPECT_FALSE(model->openAt(1));
    EXPECT_EQ(model->currentRow(), 0);
    EXPECT_FALSE(model->errorText().isEmpty());
}

TEST_F(VideoFolderModelTests, FailedReopenOfCurrentFileDoesNotResumePlayback) {
    threeFiles();
    model->synchronizeSources({at(1)});
    ASSERT_TRUE(model->openAt(1));
    model->completeOpen(1, false, QStringLiteral("changed or corrupt"));
    EXPECT_EQ(model->currentRow(), 1);
    EXPECT_EQ(plays, 0);
    EXPECT_EQ(model->errorText(), "changed or corrupt");
}

TEST_F(VideoFolderModelTests, DifferentCommittedSourceCannotTriggerAutoplay) {
    threeFiles();
    ASSERT_TRUE(model->openAt(1));
    model->synchronizeSources({at(0)});
    model->completeOpen(1, true, {});
    EXPECT_EQ(plays, 0);
    EXPECT_EQ(model->currentRow(), 0);
    EXPECT_FALSE(model->errorText().isEmpty());
}

TEST_F(VideoFolderModelTests, RapidSelectionCancelsQueuedWorkAndIgnoresOldTerminal) {
    threeFiles();
    ASSERT_TRUE(model->openAt(0));
    ASSERT_TRUE(model->openAt(1));
    ASSERT_EQ(canceled.size(), 1U);
    EXPECT_EQ(canceled.front(), 1U);
    commit(0, 1);
    EXPECT_EQ(plays, 0);
    EXPECT_TRUE(model->openPending());
    commit(1, 2);
    EXPECT_EQ(plays, 1);
    EXPECT_EQ(model->currentRow(), 1);
}

TEST_F(VideoFolderModelTests, FolderChangeInvalidatesPendingAutoplay) {
    threeFiles();
    ASSERT_TRUE(model->openAt(0));
    ASSERT_TRUE(model->refreshFolder());
    commit(0, 1);
    EXPECT_EQ(plays, 0);
    EXPECT_FALSE(model->openPending());
    EXPECT_FALSE(model->openAt(1));
    tasks.back()();
    waitForScan(*model);
    EXPECT_EQ(model->currentRow(), 0);
}

TEST_F(VideoFolderModelTests, NavigationUsesPendingSelectionAndNeverWraps) {
    EXPECT_FALSE(model->step(1));
    threeFiles();
    EXPECT_FALSE(model->step(0));
    EXPECT_FALSE(model->openAt(-1));
    EXPECT_FALSE(model->openAt(3));
    ASSERT_TRUE(model->step(1));
    EXPECT_EQ(opened.back(), at(0));
    ASSERT_TRUE(model->step(1));
    EXPECT_EQ(opened.back(), at(1));
    ASSERT_TRUE(model->step(1));
    EXPECT_EQ(opened.back(), at(2));
    EXPECT_FALSE(model->step(1));
    commit(2, 3);
    ASSERT_TRUE(model->step(-1));
    EXPECT_EQ(opened.back(), at(1));
    EXPECT_EQ(plays, 1);
}

TEST_F(VideoFolderModelTests, CapacityFailureDoesNotPublishTruncatedFolder) {
    model = std::make_unique<VideoFolderModel>(dependencies(2));
    write(QStringLiteral("one.mp4"));
    load();
    ASSERT_EQ(model->fileCount(), 1);
    write(QStringLiteral("two.mp4"));
    write(QStringLiteral("three.mp4"));
    load();
    EXPECT_EQ(model->fileCount(), 1);
    EXPECT_FALSE(model->errorText().isEmpty());
}

TEST_F(VideoFolderModelTests, SchedulerAndPlayRejectionAreVisible) {
    threeFiles();
    allowPlay = false;
    ASSERT_TRUE(model->openAt(1));
    commit(1, 1);
    EXPECT_FALSE(model->errorText().isEmpty());
    EXPECT_EQ(model->currentRow(), 1);
    auto deps = dependencies();
    deps.schedule = [](auto) { throw std::runtime_error("worker unavailable"); };
    model = std::make_unique<VideoFolderModel>(std::move(deps));
    EXPECT_FALSE(model->loadFolder(QUrl::fromLocalFile(directory.path())));
    EXPECT_FALSE(model->scanning());
    EXPECT_FALSE(model->errorText().isEmpty());
}

TEST_F(VideoFolderModelTests, EmptyFolderCompletesAndRealBackgroundWorkerIsUsable) {
    model = std::make_unique<VideoFolderModel>();
    ASSERT_TRUE(model->loadFolder(QUrl::fromLocalFile(directory.path())));
    waitForScan(*model);
    EXPECT_EQ(model->fileCount(), 0);
    EXPECT_FALSE(model->folderName().isEmpty());
    EXPECT_EQ(model->folderPath(), directory.path());
    EXPECT_TRUE(model->errorText().isEmpty());
    const auto task = [this] {
        auto ephemeral = std::make_unique<VideoFolderModel>(dependencies());
        ephemeral->loadFolder(QUrl::fromLocalFile(directory.path()));
    };
    task();
    tasks.back()(); // The queued closure survives its owner without touching any QObject.
}

} // namespace
} // namespace dvs::ui
