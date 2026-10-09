#include "dvs/ui/ReviewPreferencesController.h"
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

TEST_F(VideoFolderModelTests, ContextOpenUsesCapturedFileRatherThanAReorderedRow) {
    threeFiles();
    const QUrl selected = at(1);
    write(QStringLiteral("clip0.mp4"));
    load();
    ASSERT_NE(at(1), selected);
    ASSERT_TRUE(model->openFile(selected));
    ASSERT_EQ(opened.size(), 1U);
    EXPECT_EQ(opened.front(), selected);
    EXPECT_EQ(model->pendingRow(), 2);
    EXPECT_EQ(plays, 0);
    EXPECT_EQ(model->currentRow(), -1);
    model->completeOpen(nextIntent, false, QStringLiteral("media-open-failed"));
    EXPECT_EQ(model->errorText(), QStringLiteral("media-open-failed"));
    EXPECT_FALSE(model->openPending());
    EXPECT_EQ(plays, 0);
}

TEST_F(VideoFolderModelTests, ContextOpenRejectsInvalidUrlsAndKeepsCommittedSelection) {
    threeFiles();
    model->synchronizeSources({at(1)});
    for (const QUrl& url : {QUrl{},
                            QUrl{QStringLiteral("https://example.com/a.mp4")},
                            QUrl::fromLocalFile(directory.filePath("a.xyz"))}) {
        EXPECT_FALSE(model->openFile(url));
    }
    EXPECT_TRUE(opened.empty());
    EXPECT_EQ(model->currentRow(), 1);
    EXPECT_EQ(plays, 0);
}

TEST_F(VideoFolderModelTests, ScansAsynchronouslyFiltersAndNaturallySortsWithoutDecoding) {
    const auto unicode = write(QString::fromUtf8("片段.MP4"));
    const auto image = write(QString::fromUtf8("ignore.png"));
    write(QString::fromUtf8("忽略.文本"));
    for (const auto& name : {"clip10.MKV",
                             "clip2.mov",
                             "Clip1.avi",
                             "clip99999999999999999999999.m4v",
                             "clip100000000000000000000000.mp4",
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
    ASSERT_EQ(model->fileCount(), 7);
    EXPECT_EQ(model->data(model->index(0), VideoFolderModel::FileNameRole).toString(), "Clip1.avi");
    EXPECT_EQ(model->data(model->index(1), VideoFolderModel::FileNameRole).toString(), "clip2.mov");
    EXPECT_EQ(model->data(model->index(2), VideoFolderModel::FileNameRole).toString(),
              "clip10.MKV");
    // Still images join the listing, marked by kind for the sidebar's image dispatch.
    EXPECT_EQ(at(5), image);
    EXPECT_TRUE(model->data(model->index(5), VideoFolderModel::FileIsImageRole).toBool());
    EXPECT_FALSE(model->data(model->index(0), VideoFolderModel::FileIsImageRole).toBool());
    EXPECT_EQ(at(6), unicode);
    EXPECT_TRUE(model->errorText().isEmpty());
    EXPECT_TRUE(opened.empty());
    EXPECT_EQ(model->rowCount(model->index(0)), 0);
    EXPECT_FALSE(model->data(QModelIndex{}, VideoFolderModel::FileUrlRole).isValid());
    EXPECT_EQ(model->roleNames().value(VideoFolderModel::FileNameRole), "fileName");
}

// Mixed-media folders open images without a video intent: the model stages the pending row,
// the host drives the decode through the image workspace, and only a matching URL finishes.
TEST_F(VideoFolderModelTests, ImageRowsStageWithoutVideoIntentAndFinishByUrl) {
    write(QStringLiteral("clip1.mp4"));
    const QUrl image = write(QStringLiteral("shot.png"));
    load();
    ASSERT_EQ(model->fileCount(), 2);
    EXPECT_TRUE(model->isImageUrl(image));
    EXPECT_EQ(model->urlForRow(1), image);

    ASSERT_TRUE(model->openAt(1));
    EXPECT_TRUE(model->openPending());
    EXPECT_EQ(model->pendingRow(), 1);
    EXPECT_TRUE(opened.empty());
    EXPECT_EQ(plays, 0);

    // A terminal for a foreign file must not retire the staged image.
    EXPECT_FALSE(model->finishPendingImageOpen(true, {}, at(0).toLocalFile()));
    EXPECT_TRUE(model->openPending());
    ASSERT_TRUE(
        model->finishPendingImageOpen(false, QStringLiteral("decode-failed"), image.toLocalFile()));
    EXPECT_FALSE(model->openPending());
    EXPECT_EQ(model->errorText(), QStringLiteral("decode-failed"));

    ASSERT_TRUE(model->openAt(1));
    ASSERT_TRUE(model->finishPendingImageOpen(true, {}, image.toLocalFile()));
    EXPECT_FALSE(model->openPending());
    EXPECT_TRUE(model->errorText().isEmpty());
    EXPECT_TRUE(opened.empty());
    EXPECT_EQ(plays, 0);

    ASSERT_TRUE(model->openAt(1));
    model->cancelPendingImageOpen();
    EXPECT_FALSE(model->openPending());
    EXPECT_TRUE(opened.empty());
}

// Recent entries classify by kind so the sidebar can route images to the image workspace.
// The list arrives through the preferences controller exactly as in the composed app.
TEST_F(VideoFolderModelTests, RecentEntriesCarryImageKind) {
    class ClosedSettingsRepository final : public application::ISettingsRepository {
    public:
        [[nodiscard]] application::PortSubmitResult
        submit(const application::SettingsLoadRequest&,
               std::shared_ptr<application::IApplicationEventSink>) override {
            return application::PortSubmitResult::Closed;
        }
        [[nodiscard]] application::PortSubmitResult
        submit(const application::SettingsSaveRequest&,
               std::shared_ptr<application::IApplicationEventSink>) override {
            return application::PortSubmitResult::Closed;
        }
        void cancel(const application::RequestContext&) noexcept override {}
    };
    const QUrl video = write(QStringLiteral("clip.mp4"));
    const QUrl image = write(QStringLiteral("shot.png"));
    load();
    ASSERT_EQ(model->fileCount(), 2);
    ReviewPreferencesController preferences{std::make_shared<ClosedSettingsRepository>()};
    model->attachPreferences(preferences);
    preferences.rememberMediaFile(image);
    preferences.rememberMediaFile(video);
    ASSERT_EQ(model->recentFiles().size(), 2);
    EXPECT_FALSE(model->recentFiles().front().toMap().value(QStringLiteral("isImage")).toBool());
    EXPECT_TRUE(model->recentFiles().back().toMap().value(QStringLiteral("isImage")).toBool());
    EXPECT_FALSE(model->isRecentImage(0));
    EXPECT_TRUE(model->isRecentImage(1));
    preferences.stop();
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

TEST_F(VideoFolderModelTests, CommittedSingleVideoAutomaticallyFollowsParentWithoutOpeningIt) {
    const auto video = write(QString::fromUtf8("片段2.MP4"));
    write(QString::fromUtf8("片段10.mp4"));
    write(QStringLiteral("ignored.txt"));
    model->synchronizeSources({video});
    EXPECT_TRUE(model->scanning());
    ASSERT_EQ(tasks.size(), 1U);
    EXPECT_TRUE(opened.empty());
    tasks.back()();
    waitForScan(*model);
    EXPECT_EQ(model->folderUrl(), QUrl::fromLocalFile(directory.path()));
    ASSERT_EQ(model->fileCount(), 2);
    EXPECT_EQ(model->currentRow(), 0);
    EXPECT_EQ(model->currentUrl(), video);
    model->synchronizeSources({video});
    EXPECT_EQ(tasks.size(), 1U);
    EXPECT_TRUE(model->recentFiles().isEmpty()); // Projections are not successful open terminals.
    model->recordCommittedVideo();
    ASSERT_EQ(model->recentFiles().size(), 1);
    EXPECT_EQ(model->recentFiles().front().toMap().value("fileUrl").toUrl(), video);
    EXPECT_EQ(plays, 0);
}

TEST_F(VideoFolderModelTests, AutoFollowCannotCancelNewerPendingRecentOpen) {
    threeFiles();
    const auto first = at(0);
    const auto second = at(1);
    model->synchronizeSources({first});
    model->recordCommittedVideo();
    model->synchronizeSources({second});
    model->recordCommittedVideo();
    ASSERT_TRUE(model->openRecent(1));
    EXPECT_EQ(opened.back(), first);
    const auto firstIntent = nextIntent;
    ASSERT_TRUE(model->openRecent(0));
    const auto secondIntent = nextIntent;
    const auto canceledBefore = canceled.size();
    QTemporaryDir other;
    ASSERT_TRUE(other.isValid());
    // An older active open may commit while the latest request remains queued.
    model->synchronizeSources({QUrl::fromLocalFile(other.filePath("old.mp4"))});
    ASSERT_TRUE(model->scanning());
    EXPECT_TRUE(model->openPending());
    EXPECT_EQ(canceled.size(), canceledBefore);
    model->completeOpen(firstIntent, true, {});
    EXPECT_EQ(plays, 0);
    model->synchronizeSources({second});
    model->completeOpen(secondIntent, true, {});
    EXPECT_EQ(plays, 1);
    EXPECT_FALSE(model->openPending());
    tasks.back()();
    waitForScan(*model);
    EXPECT_EQ(at(model->currentRow()), second);
}

TEST_F(VideoFolderModelTests, RecentHistoryDeduplicatesCapsAndRetainsMissingFiles) {
    for (int index = 0; index < 55; ++index) {
        const auto url = QUrl::fromLocalFile(directory.filePath(QString("clip%1.mp4").arg(index)));
        model->synchronizeSources({url});
        model->recordCommittedVideo();
    }
    ASSERT_EQ(model->recentFiles().size(), 50);
    const auto oldest = model->recentFiles().back().toMap().value("fileUrl").toUrl();
    EXPECT_TRUE(oldest.toLocalFile().endsWith("clip5.mp4"));
    model->synchronizeSources({oldest});
    model->recordCommittedVideo();
    ASSERT_EQ(model->recentFiles().size(), 50);
    EXPECT_EQ(model->recentFiles().front().toMap().value("fileUrl").toUrl(), oldest);
    const auto history = model->recentFiles();
    ASSERT_TRUE(model->openRecent(1)); // No GUI-thread file I/O; normal open reports missing files.
    model->completeOpen(nextIntent, false, QStringLiteral("media-open-failed"));
    EXPECT_EQ(model->recentFiles(), history);
    EXPECT_EQ(model->currentUrl(), oldest);
    EXPECT_EQ(plays, 0);
    EXPECT_EQ(model->errorText(), "media-open-failed");
    EXPECT_FALSE(model->openRecent(-1));
    EXPECT_FALSE(model->openRecent(50));
    model->clear();
    EXPECT_EQ(model->recentFiles(), history);
}

// Comparison sessions and non-local sources never create single-video history. A still
// image no longer appears here: images are first-class media and reach the recent list only
// through their own commit terminal, never through the video shell's source projection.
TEST_F(VideoFolderModelTests, ComparisonAndNonLocalSourcesDoNotCreateSingleVideoHistory) {
    const auto first = write(QStringLiteral("clip1.mp4"));
    const auto second = write(QStringLiteral("clip2.mp4"));
    for (const QVariantList& sources : {QVariantList{first, second},
                                        QVariantList{QUrl{"https://example.invalid/a.mp4"}},
                                        QVariantList{QUrl{"file://server/share/a.mp4"}}}) {
        model->synchronizeSources(sources);
        model->recordCommittedVideo();
        EXPECT_TRUE(model->currentUrl().isEmpty());
        EXPECT_TRUE(model->recentFiles().isEmpty());
        EXPECT_TRUE(tasks.empty());
    }
    EXPECT_TRUE(opened.empty());
}

TEST_F(VideoFolderModelTests, SameFolderMissingRowIsRefreshedButManualBrowsingIsNotOverridden) {
    threeFiles();
    const auto original = at(0);
    model->synchronizeSources({original});
    const auto added = write(QStringLiteral("clip3.mp4"));
    model->synchronizeSources({added});
    EXPECT_TRUE(model->scanning());
    tasks.back()();
    waitForScan(*model);
    EXPECT_EQ(at(model->currentRow()), added);
    QTemporaryDir chosen;
    ASSERT_TRUE(chosen.isValid());
    ASSERT_TRUE(model->loadFolder(QUrl::fromLocalFile(chosen.path())));
    tasks.back()();
    waitForScan(*model);
    const auto scans = tasks.size();
    model->synchronizeSources({added});
    EXPECT_EQ(tasks.size(), scans);
    EXPECT_EQ(model->folderUrl(), QUrl::fromLocalFile(chosen.path()));
    EXPECT_EQ(model->currentRow(), -1);
}

TEST_F(VideoFolderModelTests, SuccessfulRecentReopenReturnsFromManuallyBrowsedFolder) {
    threeFiles();
    const auto video = at(1);
    model->synchronizeSources({video});
    model->recordCommittedVideo();
    QTemporaryDir chosen;
    ASSERT_TRUE(chosen.isValid());
    ASSERT_TRUE(model->loadFolder(QUrl::fromLocalFile(chosen.path())));
    tasks.back()();
    waitForScan(*model);
    ASSERT_TRUE(model->openRecent(0));
    model->synchronizeSources({video}); // Same source URL, still a new successful open intent.
    model->completeOpen(nextIntent, true, {});
    model->recordCommittedVideo();
    EXPECT_TRUE(model->scanning());
    tasks.back()();
    waitForScan(*model);
    EXPECT_EQ(model->folderUrl(), QUrl::fromLocalFile(directory.path()));
    EXPECT_EQ(model->currentRow(), 1);
    EXPECT_EQ(model->recentCurrentRow(), 0);
    EXPECT_EQ(plays, 1);
}

TEST_F(VideoFolderModelTests, FolderScanCompletionDoesNotEraseRecentOpenFailure) {
    threeFiles();
    const auto video = at(1);
    model->synchronizeSources({video});
    model->recordCommittedVideo();
    ASSERT_TRUE(model->refreshFolder());
    ASSERT_TRUE(model->openRecent(0));
    model->completeOpen(nextIntent, false, QStringLiteral("missing video"));
    tasks.back()();
    waitForScan(*model);
    EXPECT_EQ(model->errorText(), "missing video");
    EXPECT_EQ(model->currentUrl(), video);
    EXPECT_EQ(plays, 0);
}

} // namespace
} // namespace dvs::ui
