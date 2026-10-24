#include "dvs/ui/VideoTaskHost.h"

#include <QFileInfo>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <gtest/gtest.h>
#include <vector>

using dvs::ui::VideoTaskHost;

namespace {

// The host never touches the disk, so a fixed fictitious absolute root keeps the tests
// hermetic: QFileInfo normalization works on paths that do not exist.
const QString kRoot = QStringLiteral("G:/dvs-video-task-host/");

[[nodiscard]] QUrl videoUrl(const QString& name) {
    return QUrl::fromLocalFile(kRoot + name);
}

[[nodiscard]] QString identityFor(const QUrl& url) {
    const QString local = url.toLocalFile();
    if (!local.isEmpty()) {
        return QFileInfo{local}.absoluteFilePath();
    }
    return url.toString();
}

[[nodiscard]] QVariantList variantUrls(const std::vector<QUrl>& urls) {
    QVariantList list;
    for (const QUrl& url : urls) {
        list.push_back(QVariant::fromValue(url));
    }
    return list;
}

[[nodiscard]] std::vector<QUrl>
group(const QString& first, const QString& second = {}, const QString& third = {}) {
    std::vector<QUrl> urls{videoUrl(first)};
    if (!second.isEmpty()) {
        urls.push_back(videoUrl(second));
    }
    if (!third.isEmpty()) {
        urls.push_back(videoUrl(third));
    }
    return urls;
}

struct HostSignals {
    int openRequested = 0;
    int taskOpened = 0;
    int allTasksClosed = 0;
    QString failure;
    QVariantList lastRequestedUrls;
    QString lastRequestedPurpose;
    int lastRequestedReference = -1;

    void attach(VideoTaskHost* host) {
        QObject::connect(
            host,
            &VideoTaskHost::openRequested,
            [this](
                int index, const QVariantList& urls, int referenceIndex, const QString& purpose) {
                ++openRequested;
                lastRequestedUrls = urls;
                lastRequestedReference = referenceIndex;
                lastRequestedPurpose = purpose;
                static_cast<void>(index);
            });
        QObject::connect(host, &VideoTaskHost::taskOpened, [this] { ++taskOpened; });
        QObject::connect(host, &VideoTaskHost::allTasksClosed, [this] { ++allTasksClosed; });
        QObject::connect(
            host, &VideoTaskHost::openFailed, [this](const QString& error) { failure = error; });
    }
};

// Opens and commits in one step; a committed task is loaded and active (design §2.2 step 3).
[[nodiscard]] int
openAndCommit(VideoTaskHost& host, const std::vector<QUrl>& urls, const int referenceIndex) {
    const int index = host.openTask(variantUrls(urls), referenceIndex, QStringLiteral("Review"));
    if (index < 0) {
        return index;
    }
    QStringList frozen;
    for (const QUrl& url : urls) {
        frozen.push_back(identityFor(url) + QStringLiteral("|1|1"));
    }
    host.commitOpen(index, frozen);
    return index;
}

[[nodiscard]] QVariantMap tabAt(const VideoTaskHost& host, const int index) {
    return host.tabs().at(index).toMap();
}

} // namespace

TEST(VideoTaskHostTests, OpenSingleVideoCreatesALoadingRecordThatCommitCompletes) {
    VideoTaskHost host;
    HostSignals sink;
    sink.attach(&host);

    const int index =
        host.openTask(variantUrls(group(QStringLiteral("a.mp4"))), 0, QStringLiteral("Review"));
    ASSERT_EQ(index, 0);
    EXPECT_EQ(host.count(), 1);
    EXPECT_EQ(host.activeIndex(), -1);
    ASSERT_EQ(sink.openRequested, 1);
    EXPECT_EQ(sink.lastRequestedUrls.size(), 1);
    EXPECT_EQ(sink.lastRequestedReference, 0);
    EXPECT_EQ(sink.lastRequestedPurpose, QStringLiteral("Review"));
    EXPECT_EQ(sink.taskOpened, 0);
    QVariantMap tab = tabAt(host, 0);
    EXPECT_EQ(tab.value(QStringLiteral("title")).toString(), QStringLiteral("正在打开…"));
    EXPECT_TRUE(tab.value(QStringLiteral("loading")).toBool());
    EXPECT_FALSE(tab.value(QStringLiteral("active")).toBool());
    EXPECT_EQ(tab.value(QStringLiteral("sourceCount")).toInt(), 1);
    EXPECT_FALSE(tab.value(QStringLiteral("comparison")).toBool());

    ASSERT_TRUE(host.commitOpen(0, {QStringLiteral("frozen-a")}));
    EXPECT_EQ(sink.taskOpened, 1);
    EXPECT_EQ(host.activeIndex(), 0);
    tab = tabAt(host, 0);
    EXPECT_EQ(tab.value(QStringLiteral("title")).toString(), QStringLiteral("a.mp4"));
    EXPECT_FALSE(tab.value(QStringLiteral("loading")).toBool());
    EXPECT_TRUE(tab.value(QStringLiteral("active")).toBool());
    EXPECT_EQ(tab.value(QStringLiteral("detail")).toString(),
              identityFor(videoUrl(QStringLiteral("a.mp4"))));
}

TEST(VideoTaskHostTests, IdentityVariantsOfTheSameSingleVideoFocusTheExistingTab) {
    VideoTaskHost host;
    HostSignals sink;
    sink.attach(&host);
    ASSERT_EQ(openAndCommit(host, group(QStringLiteral("a.mp4")), 0), 0);
    host.updateTaskState(0, QVariantMap{{QStringLiteral("frame"), 42}});
    const int requestsBefore = sink.openRequested;

    // Case differences, a redundant "." segment and file:/// spellings normalize to the same
    // identity (design §2.1); every variant focuses the existing record without a new open.
    const QUrl upperA = QUrl::fromLocalFile(kRoot.toUpper() + QStringLiteral("A.MP4"));
    const QUrl dottedA = QUrl::fromLocalFile(kRoot + QStringLiteral("./a.mp4"));
    const QVariantList variants = {
        QVariant::fromValue(upperA),
        QVariant::fromValue(dottedA),
        QVariant(QStringLiteral("file:///G:/dvs-video-task-host/a.mp4")),
        QVariant(QStringLiteral("G:/dvs-video-task-host/a.mp4")),
    };
    for (const QVariant& variant : variants) {
        SCOPED_TRACE(variant.toString().toStdString());
        EXPECT_EQ(host.openTask({variant}, 0, QStringLiteral("Review")), 0);
        EXPECT_EQ(host.count(), 1);
    }
    EXPECT_EQ(sink.openRequested, requestsBefore);
    EXPECT_EQ(host.activeIndex(), 0);
    EXPECT_EQ(host.taskState(0).value(QStringLiteral("frame")).toInt(), 42);
}

TEST(VideoTaskHostTests, ComparisonAbAndComparisonAcStaySeparateTasks) {
    VideoTaskHost host;
    HostSignals sink;
    sink.attach(&host);
    ASSERT_EQ(openAndCommit(host, group(QStringLiteral("a.mp4"), QStringLiteral("b.mp4")), 0), 0);

    const std::vector<QUrl> ac = group(QStringLiteral("a.mp4"), QStringLiteral("c.mp4"));
    EXPECT_EQ(openAndCommit(host, ac, 0), 1);
    EXPECT_EQ(host.count(), 2);
    EXPECT_EQ(sink.openRequested, 2);

    // Re-opening AB focuses tab 0; AC keeps its own tab.
    const std::vector<QUrl> ab = group(QStringLiteral("a.mp4"), QStringLiteral("b.mp4"));
    EXPECT_EQ(openAndCommit(host, ab, 0), 0);
    EXPECT_EQ(host.count(), 2);
    EXPECT_EQ(sink.openRequested, 2);
    const QVariantMap second = tabAt(host, 1);
    EXPECT_EQ(second.value(QStringLiteral("sourceCount")).toInt(), 2);
    EXPECT_TRUE(second.value(QStringLiteral("comparison")).toBool());
}

TEST(VideoTaskHostTests, SingleVideoAndPairOfTheSameSourceStaySeparateTasks) {
    VideoTaskHost host;
    ASSERT_EQ(openAndCommit(host, group(QStringLiteral("a.mp4")), 0), 0);
    const std::vector<QUrl> ab = group(QStringLiteral("a.mp4"), QStringLiteral("b.mp4"));
    ASSERT_EQ(openAndCommit(host, ab, 0), 1);
    EXPECT_EQ(host.count(), 2);
    EXPECT_EQ(
        host.openTask(variantUrls(group(QStringLiteral("a.mp4"))), 0, QStringLiteral("Review")), 0);
    EXPECT_EQ(host.count(), 2);
    EXPECT_EQ(host.activeIndex(), 0);
}

TEST(VideoTaskHostTests, OrderIndependentMatchFocusesTheRecordedSlotOrder) {
    VideoTaskHost host;
    HostSignals sink;
    sink.attach(&host);
    ASSERT_EQ(openAndCommit(host, group(QStringLiteral("a.mp4"), QStringLiteral("b.mp4")), 0), 0);
    host.updateTaskState(0, QVariantMap{{QStringLiteral("frame"), 77}});
    const QVariantMap tabBefore = tabAt(host, 0);

    // B↔A with the same reference source is the same task (decision e); the recorded slot
    // order stays A↔B and the snapshot survives the focus.
    const std::vector<QUrl> swapped = group(QStringLiteral("b.mp4"), QStringLiteral("a.mp4"));
    const int index = host.openTask(variantUrls(swapped), 1, QStringLiteral("Review"));
    EXPECT_EQ(index, 0);
    EXPECT_EQ(host.count(), 1);
    EXPECT_EQ(sink.openRequested, 1);
    EXPECT_EQ(tabAt(host, 0), tabBefore);
    EXPECT_EQ(host.taskState(0).value(QStringLiteral("frame")).toInt(), 77);
}

TEST(VideoTaskHostTests, SameGroupWithADifferentReferenceIsADifferentTask) {
    VideoTaskHost host;
    HostSignals sink;
    sink.attach(&host);
    const std::vector<QUrl> ab = group(QStringLiteral("a.mp4"), QStringLiteral("b.mp4"));
    ASSERT_EQ(openAndCommit(host, ab, 0), 0);
    EXPECT_EQ(openAndCommit(host, ab, 1), 1);
    EXPECT_EQ(host.count(), 2);
    EXPECT_EQ(sink.openRequested, 2);
}

TEST(VideoTaskHostTests, ThirteenthTabIsRejectedUntilATabCloses) {
    VideoTaskHost host;
    HostSignals sink;
    sink.attach(&host);
    for (int index = 0; index < 12; ++index) {
        const QUrl url = videoUrl(QStringLiteral("video%1.mp4").arg(index));
        ASSERT_EQ(openAndCommit(host, {url}, 0), index);
    }
    EXPECT_EQ(host.count(), 12);
    EXPECT_TRUE(sink.failure.isEmpty());

    const QVariantList thirteenth = variantUrls(group(QStringLiteral("13th.mp4")));
    EXPECT_EQ(host.openTask(thirteenth, 0, QStringLiteral("Review")), -1);
    EXPECT_EQ(host.count(), 12);
    ASSERT_FALSE(sink.failure.isEmpty());
    EXPECT_TRUE(sink.failure.contains(QStringLiteral("12")));

    ASSERT_TRUE(host.closeAt(5));
    EXPECT_EQ(host.openTask(thirteenth, 0, QStringLiteral("Review")), 11);
    EXPECT_EQ(host.count(), 12);
}

TEST(VideoTaskHostTests, SecondOpenWhileLoadingDefersActivationToTheCommit) {
    VideoTaskHost host;
    HostSignals sink;
    sink.attach(&host);
    ASSERT_EQ(
        host.openTask(variantUrls(group(QStringLiteral("a.mp4"))), 0, QStringLiteral("Review")), 0);
    EXPECT_EQ(host.activeIndex(), -1);

    const QVariantList upperA = {
        QVariant::fromValue(QUrl::fromLocalFile(kRoot.toUpper() + QStringLiteral("A.MP4"))),
    };
    const int again = host.openTask(upperA, 0, QStringLiteral("Review"));
    EXPECT_EQ(again, 0);
    EXPECT_EQ(host.count(), 1);
    EXPECT_EQ(sink.openRequested, 1);
    EXPECT_EQ(host.activeIndex(), -1);

    ASSERT_TRUE(host.commitOpen(0, {QStringLiteral("frozen-a")}));
    EXPECT_EQ(host.activeIndex(), 0);
    EXPECT_EQ(sink.taskOpened, 1);
}

TEST(VideoTaskHostTests, AppendSourceEvolvesTheActiveTaskInPlace) {
    VideoTaskHost host;
    HostSignals sink;
    sink.attach(&host);
    ASSERT_EQ(openAndCommit(host, group(QStringLiteral("a.mp4"), QStringLiteral("b.mp4")), 0), 0);
    host.updateTaskState(0, QVariantMap{{QStringLiteral("frame"), 5}});

    EXPECT_TRUE(host.appendSourceToActiveTask(videoUrl(QStringLiteral("c.mp4"))));
    EXPECT_EQ(host.count(), 1);
    EXPECT_EQ(sink.openRequested, 1);
    QVariantMap tab = tabAt(host, 0);
    EXPECT_EQ(tab.value(QStringLiteral("sourceCount")).toInt(), 3);
    EXPECT_TRUE(tab.value(QStringLiteral("comparison")).toBool());
    EXPECT_EQ(tab.value(QStringLiteral("title")).toString(), QStringLiteral("a.mp4 + 2"));
    EXPECT_EQ(host.taskState(0).value(QStringLiteral("frame")).toInt(), 5);

    // The evolved group matches in any slot order with the same reference (decisions e + f).
    const std::vector<QUrl> rotated =
        group(QStringLiteral("c.mp4"), QStringLiteral("b.mp4"), QStringLiteral("a.mp4"));
    EXPECT_EQ(host.openTask(variantUrls(rotated), 2, QStringLiteral("Review")), 0);
    EXPECT_EQ(host.count(), 1);

    // A fourth source is refused.
    EXPECT_FALSE(host.appendSourceToActiveTask(videoUrl(QStringLiteral("d.mp4"))));
    EXPECT_EQ(tabAt(host, 0).value(QStringLiteral("sourceCount")).toInt(), 3);
}

TEST(VideoTaskHostTests, RemoveSourceShrinksTheTaskAndMovesTheReferenceWhenNeeded) {
    VideoTaskHost host;
    HostSignals sink;
    sink.attach(&host);
    const QStringList frozen = {
        QStringLiteral("frozen-a"),
        QStringLiteral("frozen-b"),
        QStringLiteral("frozen-c"),
    };
    const std::vector<QUrl> abc =
        group(QStringLiteral("a.mp4"), QStringLiteral("b.mp4"), QStringLiteral("c.mp4"));
    ASSERT_EQ(host.openTask(variantUrls(abc), 1, QStringLiteral("Review")), 0);
    ASSERT_TRUE(host.commitOpen(0, frozen));
    host.updateTaskState(0, QVariantMap{{QStringLiteral("frame"), 9}});

    // Removing by frozen identity removes the reference slot; the reference moves to slot 0.
    EXPECT_TRUE(host.removeSourceFromActiveTask(QStringLiteral("frozen-b")));
    QVariantMap tab = tabAt(host, 0);
    EXPECT_EQ(tab.value(QStringLiteral("sourceCount")).toInt(), 2);
    EXPECT_EQ(tab.value(QStringLiteral("title")).toString(), QStringLiteral("a.mp4  ↔  c.mp4"));
    EXPECT_EQ(host.taskState(0).value(QStringLiteral("frame")).toInt(), 9);

    // The shrunken group still matches in any order with the moved reference (slot 0 = a).
    const std::vector<QUrl> ca = group(QStringLiteral("c.mp4"), QStringLiteral("a.mp4"));
    EXPECT_EQ(host.openTask(variantUrls(ca), 1, QStringLiteral("Review")), 0);
    EXPECT_EQ(host.count(), 1);

    // Removing by URL identity shrinks to the single-video shape on the same tab.
    const QString identityC = identityFor(videoUrl(QStringLiteral("c.mp4")));
    EXPECT_TRUE(host.removeSourceFromActiveTask(identityC));
    tab = tabAt(host, 0);
    EXPECT_EQ(tab.value(QStringLiteral("sourceCount")).toInt(), 1);
    EXPECT_FALSE(tab.value(QStringLiteral("comparison")).toBool());
    EXPECT_EQ(tab.value(QStringLiteral("title")).toString(), QStringLiteral("a.mp4"));
    EXPECT_EQ(host.count(), 1);
    EXPECT_EQ(host.taskState(0).value(QStringLiteral("frame")).toInt(), 9);

    // The last source is closed with closeAt/closeActive, not by removal.
    const QString identityA = identityFor(videoUrl(QStringLiteral("a.mp4")));
    EXPECT_FALSE(host.removeSourceFromActiveTask(identityA));
    EXPECT_EQ(host.count(), 1);
}

TEST(VideoTaskHostTests, ClosingTheActiveTabPrefersTheRightNeighbourThenLeftAtTheEnd) {
    VideoTaskHost host;
    HostSignals sink;
    sink.attach(&host);
    const QStringList names = {
        QStringLiteral("a.mp4"),
        QStringLiteral("b.mp4"),
        QStringLiteral("c.mp4"),
        QStringLiteral("d.mp4"),
    };
    for (const QString& name : names) {
        ASSERT_GE(openAndCommit(host, {videoUrl(name)}, 0), 0);
    }
    ASSERT_EQ(host.count(), 4);

    ASSERT_TRUE(host.activate(0));
    EXPECT_TRUE(host.closeActive());
    EXPECT_EQ(host.count(), 3);
    EXPECT_EQ(host.activeIndex(), 0);

    ASSERT_TRUE(host.activate(2));
    EXPECT_TRUE(host.closeActive());
    EXPECT_EQ(host.count(), 2);
    EXPECT_EQ(host.activeIndex(), 1);

    ASSERT_TRUE(host.activate(0));
    EXPECT_TRUE(host.closeActive());
    EXPECT_EQ(host.count(), 1);
    EXPECT_EQ(host.activeIndex(), 0);

    EXPECT_TRUE(host.closeActive());
    EXPECT_EQ(host.count(), 0);
    EXPECT_EQ(host.activeIndex(), -1);
    EXPECT_EQ(sink.allTasksClosed, 1);
    EXPECT_FALSE(host.closeActive());
}

TEST(VideoTaskHostTests, CloseAndSwitchAreRefusedWhileALoadingTabExists) {
    VideoTaskHost host;
    HostSignals sink;
    sink.attach(&host);
    ASSERT_EQ(openAndCommit(host, group(QStringLiteral("a.mp4")), 0), 0);
    ASSERT_EQ(
        host.openTask(variantUrls(group(QStringLiteral("b.mp4"))), 0, QStringLiteral("Review")), 1);
    EXPECT_TRUE(tabAt(host, 1).value(QStringLiteral("loading")).toBool());

    EXPECT_FALSE(host.closeAt(1));
    EXPECT_EQ(host.count(), 2);
    EXPECT_FALSE(host.activate(1));
    EXPECT_EQ(host.activeIndex(), 0);
    host.cycle(1);
    EXPECT_EQ(host.activeIndex(), 0);

    ASSERT_TRUE(host.commitOpen(1, {QStringLiteral("frozen-b")}));
    EXPECT_EQ(host.activeIndex(), 1);
    EXPECT_TRUE(host.closeAt(1));
    EXPECT_EQ(host.count(), 1);
}

TEST(VideoTaskHostTests, CycleSkipsLoadingAndRestoringTabs) {
    VideoTaskHost host;
    ASSERT_EQ(openAndCommit(host, group(QStringLiteral("a.mp4")), 0), 0);
    ASSERT_EQ(openAndCommit(host, group(QStringLiteral("b.mp4")), 0), 1);
    ASSERT_EQ(openAndCommit(host, group(QStringLiteral("c.mp4")), 0), 2);

    ASSERT_TRUE(host.setTaskRestoring(1, true));
    EXPECT_EQ(tabAt(host, 1).value(QStringLiteral("title")).toString(),
              QStringLiteral("正在恢复…"));

    ASSERT_TRUE(host.activate(0));
    host.cycle(1);
    EXPECT_EQ(host.activeIndex(), 2);
    host.cycle(1);
    EXPECT_EQ(host.activeIndex(), 0);
    host.cycle(-1);
    EXPECT_EQ(host.activeIndex(), 2);

    ASSERT_TRUE(host.setTaskRestoring(1, false));
    EXPECT_EQ(tabAt(host, 1).value(QStringLiteral("title")).toString(), QStringLiteral("b.mp4"));
}

TEST(VideoTaskHostTests, StateSnapshotRoundTripsAndSurvivesFocusAndEvolution) {
    VideoTaskHost host;
    ASSERT_EQ(openAndCommit(host, group(QStringLiteral("a.mp4")), 0), 0);
    const QVariantMap snapshot = {
        {QStringLiteral("frame"), 321},
        {QStringLiteral("zoom"), 2.5},
        {QStringLiteral("viewMode"), QStringLiteral("difference")},
        {QStringLiteral("roiEnabled"), true},
    };
    host.updateTaskState(0, snapshot);
    EXPECT_EQ(host.taskState(0), snapshot);

    EXPECT_EQ(
        host.openTask(variantUrls(group(QStringLiteral("a.mp4"))), 0, QStringLiteral("Review")), 0);
    EXPECT_TRUE(host.appendSourceToActiveTask(videoUrl(QStringLiteral("b.mp4"))));
    EXPECT_EQ(host.taskState(0), snapshot);

    const QVariantMap replacement = {{QStringLiteral("frame"), 1}};
    host.updateTaskState(0, replacement);
    EXPECT_EQ(host.taskState(0), replacement);

    host.updateTaskState(7, snapshot);
    EXPECT_EQ(host.taskState(7), QVariantMap{});
    EXPECT_EQ(host.count(), 1);
}

// The QML switch skeleton decides "session still shows this task" from the tabs model's
// session facts (slot-ordered urls plus the reference slot, design §4.3), so they must
// project the record exactly and follow it through group evolution.
TEST(VideoTaskHostTests, TabsExposeSessionFactsForTheSwitchSkeleton) {
    VideoTaskHost host;
    ASSERT_EQ(openAndCommit(host, group(QStringLiteral("a.mp4")), 0), 0);
    ASSERT_EQ(openAndCommit(host, group(QStringLiteral("b.mp4"), QStringLiteral("c.mp4")), 1), 1);

    const QVariantMap single = tabAt(host, 0);
    EXPECT_EQ(single.value(QStringLiteral("urls")).toStringList(),
              QStringList{videoUrl(QStringLiteral("a.mp4")).toString()});
    EXPECT_EQ(single.value(QStringLiteral("referenceIndex")).toInt(), 0);

    QVariantMap comparison = tabAt(host, 1);
    QStringList comparisonUrls = comparison.value(QStringLiteral("urls")).toStringList();
    ASSERT_EQ(comparisonUrls.size(), 2);
    EXPECT_EQ(comparisonUrls.front(), videoUrl(QStringLiteral("b.mp4")).toString());
    EXPECT_EQ(comparisonUrls.back(), videoUrl(QStringLiteral("c.mp4")).toString());
    EXPECT_EQ(comparison.value(QStringLiteral("referenceIndex")).toInt(), 1);

    // Group evolution follows the record (decision f): the appended slot extends the list.
    ASSERT_TRUE(host.appendSourceToActiveTask(videoUrl(QStringLiteral("d.mp4"))));
    comparisonUrls = tabAt(host, 1).value(QStringLiteral("urls")).toStringList();
    ASSERT_EQ(comparisonUrls.size(), 3);
    EXPECT_EQ(comparisonUrls.back(), videoUrl(QStringLiteral("d.mp4")).toString());
}

// Slice-4 evolution API: an in-place disc switch rewrites the active record's group and
// resets the state snapshot (the new material has nothing to restore), while a group
// evolution keeps it (decision f). The tab survives both.
TEST(VideoTaskHostTests, ReplaceActiveTaskGroupResetsOrKeepsTheSnapshotByIntent) {
    VideoTaskHost host;
    ASSERT_EQ(openAndCommit(host, group(QStringLiteral("a.mp4")), 0), 0);
    host.updateTaskState(0, QVariantMap{{QStringLiteral("frame"), 90}});

    // In-place disc switch: fresh group, snapshot dropped, same tab.
    EXPECT_TRUE(host.replaceActiveTaskGroup(variantUrls(group(QStringLiteral("b.mp4"))), 0, true));
    EXPECT_EQ(host.count(), 1);
    EXPECT_EQ(host.activeIndex(), 0);
    const QVariantMap switched = tabAt(host, 0);
    EXPECT_EQ(switched.value(QStringLiteral("urls")).toStringList(),
              QStringList{videoUrl(QStringLiteral("b.mp4")).toString()});
    EXPECT_FALSE(switched.value(QStringLiteral("comparison")).toBool());
    EXPECT_TRUE(host.taskState(0).isEmpty()) << "a disc switch has no positions to restore";

    // Group evolution (a source removed): the snapshot must survive.
    host.updateTaskState(0, QVariantMap{{QStringLiteral("frame"), 12}});
    EXPECT_TRUE(host.replaceActiveTaskGroup(
        variantUrls(group(QStringLiteral("b.mp4"), QStringLiteral("c.mp4"))), 1, false));
    const QVariantMap evolved = tabAt(host, 0);
    EXPECT_TRUE(evolved.value(QStringLiteral("comparison")).toBool());
    EXPECT_EQ(evolved.value(QStringLiteral("referenceIndex")).toInt(), 1);
    EXPECT_EQ(host.taskState(0).value(QStringLiteral("frame")).toInt(), 12);

    // Guards: bad reference index / oversized group refuse and change nothing.
    EXPECT_FALSE(host.replaceActiveTaskGroup(variantUrls(group(QStringLiteral("z.mp4"))), 1, true));
    const std::vector<QUrl> four = {
        videoUrl(QStringLiteral("a.mp4")),
        videoUrl(QStringLiteral("b.mp4")),
        videoUrl(QStringLiteral("c.mp4")),
        videoUrl(QStringLiteral("d.mp4")),
    };
    EXPECT_FALSE(host.replaceActiveTaskGroup(variantUrls(four), 0, true));
    EXPECT_EQ(tabAt(host, 0).value(QStringLiteral("sourceCount")).toInt(), 2);
}

// setTaskReference updates the reference identity in place, so a later openTask with the
// new reference matches this task instead of creating a duplicate.
TEST(VideoTaskHostTests, SetTaskReferenceUpdatesMatchingForLaterOpens) {
    VideoTaskHost host;
    ASSERT_EQ(openAndCommit(host, group(QStringLiteral("a.mp4"), QStringLiteral("b.mp4")), 0), 0);
    EXPECT_TRUE(host.setTaskReference(0, 1));
    EXPECT_EQ(tabAt(host, 0).value(QStringLiteral("referenceIndex")).toInt(), 1);
    // Same multiset with the new reference slot -> the same task, not a second tab.
    EXPECT_EQ(host.openTask(variantUrls(group(QStringLiteral("b.mp4"), QStringLiteral("a.mp4"))),
                            0,
                            QStringLiteral("Review")),
              0);
    EXPECT_EQ(host.count(), 1);
    // Same multiset with the OLD reference slot is now a different task.
    EXPECT_EQ(host.openTask(variantUrls(group(QStringLiteral("a.mp4"), QStringLiteral("b.mp4"))),
                            0,
                            QStringLiteral("Review")),
              1);
    EXPECT_EQ(host.count(), 2);
    EXPECT_FALSE(host.setTaskReference(0, 9));
}

// indexOfTask is the adoption query: it reports the matching record without creating or
// activating anything, and honours the reference-identity rule.
TEST(VideoTaskHostTests, IndexOfTaskQueriesWithoutCreatingOrActivating) {
    VideoTaskHost host;
    ASSERT_EQ(openAndCommit(host, group(QStringLiteral("a.mp4")), 0), 0);
    ASSERT_EQ(openAndCommit(host, group(QStringLiteral("b.mp4"), QStringLiteral("c.mp4")), 1), 1);
    EXPECT_EQ(host.activeIndex(), 1);

    // Order-independent multiset match; the reference identity rides the SLOT, so the
    // reversed order matches with its reference at slot 0 (identity c either way).
    EXPECT_EQ(
        host.indexOfTask(variantUrls(group(QStringLiteral("c.mp4"), QStringLiteral("b.mp4"))), 0),
        1);
    EXPECT_EQ(
        host.indexOfTask(variantUrls(group(QStringLiteral("b.mp4"), QStringLiteral("c.mp4"))), 1),
        1);
    EXPECT_EQ(host.indexOfTask(variantUrls(group(QStringLiteral("a.mp4"))), 0), 0);
    // Same multiset with a DIFFERENT reference identity: no match (own task).
    EXPECT_EQ(
        host.indexOfTask(variantUrls(group(QStringLiteral("b.mp4"), QStringLiteral("c.mp4"))), 0),
        -1);
    // Unknown material and invalid arguments never match.
    EXPECT_EQ(host.indexOfTask(variantUrls(group(QStringLiteral("z.mp4"))), 0), -1);
    EXPECT_EQ(host.indexOfTask({}, 0), -1);
    EXPECT_EQ(host.activeIndex(), 1) << "the query must not activate anything";
    EXPECT_EQ(host.count(), 2) << "the query must not create anything";
}

// urlsMatch is the deterministic identity comparison the QML gates use; it accepts
// QUrl and QString variants interchangeably.
TEST(VideoTaskHostTests, UrlsMatchComparesUrlAndStringVariantsDeterministically) {
    VideoTaskHost host;
    const QVariantList left{QVariant::fromValue(videoUrl(QStringLiteral("a.mp4"))),
                            QVariant::fromValue(videoUrl(QStringLiteral("b.mp4")))};
    const QVariantList sameStrings{videoUrl(QStringLiteral("a.mp4")).toString(),
                                   videoUrl(QStringLiteral("b.mp4")).toString()};
    const QVariantList other{QVariant::fromValue(videoUrl(QStringLiteral("a.mp4"))),
                             QVariant::fromValue(videoUrl(QStringLiteral("c.mp4")))};
    EXPECT_TRUE(host.urlsMatch(left, sameStrings));
    EXPECT_FALSE(host.urlsMatch(left, other));
    EXPECT_FALSE(host.urlsMatch(left, QVariantList{}));
    EXPECT_TRUE(host.urlsMatch(QVariantList{}, QVariantList{}));
}

TEST(VideoTaskHostTests, FailOpenRemovesALoadingRecordButKeepsALoadedOne) {
    VideoTaskHost host;
    HostSignals sink;
    sink.attach(&host);
    ASSERT_EQ(openAndCommit(host, group(QStringLiteral("a.mp4")), 0), 0);
    host.updateTaskState(0, QVariantMap{{QStringLiteral("frame"), 11}});

    ASSERT_EQ(
        host.openTask(variantUrls(group(QStringLiteral("x.mp4"))), 0, QStringLiteral("Review")), 1);
    EXPECT_EQ(host.count(), 2);
    EXPECT_TRUE(host.failOpen(1, QStringLiteral("boom")));
    EXPECT_EQ(host.count(), 1);
    EXPECT_EQ(sink.failure, QStringLiteral("boom"));
    EXPECT_EQ(sink.allTasksClosed, 0);

    // A loaded task whose reopen failed keeps its record and snapshot (design §2.2 step 4).
    EXPECT_EQ(
        host.openTask(variantUrls(group(QStringLiteral("a.mp4"))), 0, QStringLiteral("Review")), 0);
    EXPECT_TRUE(host.failOpen(0, QStringLiteral("reopen failed")));
    EXPECT_EQ(host.count(), 1);
    EXPECT_EQ(tabAt(host, 0).value(QStringLiteral("title")).toString(), QStringLiteral("a.mp4"));
    EXPECT_EQ(host.taskState(0).value(QStringLiteral("frame")).toInt(), 11);
    EXPECT_EQ(sink.failure, QStringLiteral("reopen failed"));

    EXPECT_FALSE(host.failOpen(-1, QStringLiteral("ignored")));
}

TEST(VideoTaskHostTests, InvalidOpenRequestsAreRejected) {
    VideoTaskHost host;
    HostSignals sink;
    sink.attach(&host);
    EXPECT_EQ(host.openTask({}, 0, QStringLiteral("Review")), -1);
    EXPECT_FALSE(sink.failure.isEmpty());

    sink.failure.clear();
    const std::vector<QUrl> four = {
        videoUrl(QStringLiteral("a.mp4")),
        videoUrl(QStringLiteral("b.mp4")),
        videoUrl(QStringLiteral("c.mp4")),
        videoUrl(QStringLiteral("d.mp4")),
    };
    EXPECT_EQ(host.openTask(variantUrls(four), 0, QStringLiteral("Review")), -1);
    EXPECT_FALSE(sink.failure.isEmpty());

    sink.failure.clear();
    const QVariantList single = variantUrls(group(QStringLiteral("a.mp4")));
    EXPECT_EQ(host.openTask(single, -1, QStringLiteral("Review")), -1);
    EXPECT_EQ(host.openTask(single, 1, QStringLiteral("Review")), -1);
    EXPECT_FALSE(sink.failure.isEmpty());
    EXPECT_EQ(host.count(), 0);
    EXPECT_EQ(sink.openRequested, 0);
}
