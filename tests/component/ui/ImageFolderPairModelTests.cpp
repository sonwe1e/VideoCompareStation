#include "dvs/ui/ImageFolderPairModel.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QUrl>

#include <gtest/gtest.h>
#include <vector>

namespace {

using dvs::ui::ImageFolderPairModel;

struct RecordedPair {
    QUrl primary;
    QUrl secondary;
};

[[nodiscard]] QUrl
writeFile(const QTemporaryDir& dir, const QString& name, const QByteArray& content) {
    const QString path = QDir(dir.path()).filePath(name);
    QFile file{path};
    if (!file.open(QIODevice::WriteOnly)) {
        return {};
    }
    file.write(content);
    file.close();
    return QUrl::fromLocalFile(path);
}

class ImageFolderPairModelTests : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(left_.isValid());
        ASSERT_TRUE(right_.isValid());
    }

    [[nodiscard]] QUrl folderUrl(const QTemporaryDir& dir) const {
        return QUrl::fromLocalFile(QDir(dir.path()).absolutePath());
    }

    QTemporaryDir left_;
    QTemporaryDir right_;
};

TEST_F(ImageFolderPairModelTests, PairsSameNamedImagesCaseInsensitively) {
    const QUrl leftA = writeFile(left_, "shot.png", "a");
    const QUrl leftOnly = writeFile(left_, "only_left.png", "a");
    static_cast<void>(writeFile(left_, "notes.txt", "ignored"));
    const QUrl rightA = writeFile(right_, "Shot.PNG", "b");
    const QUrl rightOnly = writeFile(right_, "only_right.png", "b");
    ASSERT_TRUE(!leftA.isEmpty() && !leftOnly.isEmpty());
    ASSERT_TRUE(!rightA.isEmpty() && !rightOnly.isEmpty());

    ImageFolderPairModel model;
    ASSERT_TRUE(model.loadFolders(folderUrl(left_), folderUrl(right_)));

    EXPECT_EQ(model.pairCount(), 3);
    EXPECT_EQ(model.leftFolderName(), QDir(left_.path()).dirName());
    EXPECT_EQ(model.rightFolderName(), QDir(right_.path()).dirName());
    EXPECT_TRUE(model.errorText().isEmpty());

    // Rows are sorted by name (only_left < only_right < shot); case-insensitive names pair.
    ASSERT_EQ(model.data(model.index(0, 0), ImageFolderPairModel::FileNameRole).toString(),
              QStringLiteral("only_left.png"));
    EXPECT_FALSE(model.data(model.index(0, 0), ImageFolderPairModel::HasBothRole).toBool());
    ASSERT_EQ(model.data(model.index(1, 0), ImageFolderPairModel::FileNameRole).toString(),
              QStringLiteral("only_right.png"));
    EXPECT_FALSE(model.data(model.index(1, 0), ImageFolderPairModel::HasLeftRole).toBool());
    ASSERT_EQ(model.data(model.index(2, 0), ImageFolderPairModel::FileNameRole).toString(),
              QStringLiteral("shot.png"));
    EXPECT_TRUE(model.data(model.index(2, 0), ImageFolderPairModel::HasBothRole).toBool());
    EXPECT_EQ(model.data(model.index(2, 0), ImageFolderPairModel::LeftPathRole).toUrl(), leftA);
    EXPECT_EQ(model.data(model.index(2, 0), ImageFolderPairModel::RightPathRole).toUrl(), rightA);
    EXPECT_NE(model.data(model.index(2, 0), ImageFolderPairModel::RightPathRole).toUrl(),
              rightOnly);
    EXPECT_NE(model.data(model.index(2, 0), ImageFolderPairModel::LeftPathRole).toUrl(), leftOnly);
}

TEST_F(ImageFolderPairModelTests, OpenPairAtDrivesInjectedOpenerAndSkipsSingles) {
    static_cast<void>(writeFile(left_, "shot.png", "a"));
    static_cast<void>(writeFile(left_, "a_left_only.png", "a"));
    static_cast<void>(writeFile(right_, "shot.png", "b"));

    std::vector<RecordedPair> opened;
    ImageFolderPairModel model;
    model.setPairOpener([&opened](const QUrl& primary, const QUrl& secondary, const int pairId) {
        opened.push_back(RecordedPair{primary, secondary});
        static_cast<void>(pairId);
        return QString{};
    });
    ASSERT_TRUE(model.loadFolders(folderUrl(left_), folderUrl(right_)));

    // Single-sided rows refuse to open (row 0 sorts first).
    ASSERT_EQ(model.data(model.index(0, 0), ImageFolderPairModel::FileNameRole).toString(),
              QStringLiteral("a_left_only.png"));
    EXPECT_FALSE(model.openPairAt(0));
    EXPECT_TRUE(opened.empty());

    // Complete row drives the injected opener with left/right in order and advances the
    // committed selection to the opened row.
    const int completeRow = model.firstCompleteRow();
    ASSERT_GE(completeRow, 0);
    EXPECT_TRUE(model.openPairAt(completeRow));
    ASSERT_EQ(opened.size(), 1U);
    EXPECT_EQ(opened[0].primary,
              model.data(model.index(completeRow, 0), ImageFolderPairModel::LeftPathRole).toUrl());
    EXPECT_EQ(opened[0].secondary,
              model.data(model.index(completeRow, 0), ImageFolderPairModel::RightPathRole).toUrl());
    EXPECT_EQ(model.currentPair(), completeRow);

    // Out-of-range rows are rejected.
    EXPECT_FALSE(model.openPairAt(-1));
    EXPECT_FALSE(model.openPairAt(model.pairCount()));
}

TEST_F(ImageFolderPairModelTests, FailedOpenKeepsSelectionAndSurfacesError) {
    static_cast<void>(writeFile(left_, "shot.png", "a"));
    static_cast<void>(writeFile(left_, "other.png", "a"));
    static_cast<void>(writeFile(right_, "shot.png", "b"));
    static_cast<void>(writeFile(right_, "other.png", "b"));

    ImageFolderPairModel model;
    model.setPairOpener([](const QUrl&, const QUrl&, const int pairId) {
        // Row 0 opens; row 1 fails like a corrupt/oversized B would.
        return pairId == 1 ? QStringLiteral("B 侧损坏") : QString{};
    });
    ASSERT_TRUE(model.loadFolders(folderUrl(left_), folderUrl(right_)));

    ASSERT_TRUE(model.openPairAt(0));
    EXPECT_EQ(model.currentPair(), 0);

    EXPECT_FALSE(model.openPairAt(1));
    // Selection stays locked to the still-displayed pair; the failure source is surfaced.
    EXPECT_EQ(model.currentPair(), 0);
    EXPECT_EQ(model.errorText(), QStringLiteral("B 侧损坏"));

    // A later success clears the error and advances the selection.
    EXPECT_TRUE(model.openPairAt(0));
    EXPECT_TRUE(model.errorText().isEmpty());
    EXPECT_EQ(model.currentPair(), 0);
}

TEST_F(ImageFolderPairModelTests, StepCompleteRowWalksOnlyComparablePairs) {
    // Sorted rows: a_left_only, right_only, shot_a, shot_b. Stepping must skip the two
    // single-sided rows at 0/1.
    static_cast<void>(writeFile(left_, "a_left_only.png", "a"));
    static_cast<void>(writeFile(left_, "shot_a.png", "a"));
    static_cast<void>(writeFile(left_, "shot_b.png", "a"));
    static_cast<void>(writeFile(right_, "shot_a.png", "b"));
    static_cast<void>(writeFile(right_, "shot_b.png", "b"));
    static_cast<void>(writeFile(right_, "right_only.png", "b"));

    ImageFolderPairModel model;
    ASSERT_TRUE(model.loadFolders(folderUrl(left_), folderUrl(right_)));
    ASSERT_EQ(model.pairCount(), 4);

    const int first = model.firstCompleteRow();
    ASSERT_EQ(first, 2);
    model.setCurrentPair(first);

    // stepCompleteRow is a pure query: the QML sidebar commits currentPair after each
    // step, so each call is verified from the committed position.
    EXPECT_EQ(model.stepCompleteRow(1), 3);
    model.setCurrentPair(3);
    EXPECT_EQ(model.stepCompleteRow(1), 2);
    model.setCurrentPair(2);
    EXPECT_EQ(model.stepCompleteRow(-1), 3);
}

TEST_F(ImageFolderPairModelTests, RejectsMissingAndIdenticalFolders) {
    ImageFolderPairModel model;

    EXPECT_FALSE(model.loadFolders(QUrl::fromLocalFile(QStringLiteral("Z:/definitely/missing")),
                                   folderUrl(right_)));
    EXPECT_FALSE(model.errorText().isEmpty());
    EXPECT_EQ(model.pairCount(), 0);

    EXPECT_FALSE(model.loadFolders(folderUrl(left_), folderUrl(left_)));
    EXPECT_FALSE(model.errorText().isEmpty());

    // A file is not a folder either.
    const QUrl fileUrl = writeFile(left_, "shot.png", "a");
    EXPECT_FALSE(model.loadFolders(fileUrl, folderUrl(right_)));
}

TEST_F(ImageFolderPairModelTests, EmptyFoldersReportNoPairs) {
    ImageFolderPairModel model;
    ASSERT_TRUE(model.loadFolders(folderUrl(left_), folderUrl(right_)));
    EXPECT_EQ(model.pairCount(), 0);
    EXPECT_TRUE(model.errorText().isEmpty());
    EXPECT_EQ(model.firstCompleteRow(), -1);
}

TEST_F(ImageFolderPairModelTests, ClearDetachesFolderSessionAndSelection) {
    static_cast<void>(writeFile(left_, "shot.png", "a"));
    static_cast<void>(writeFile(right_, "shot.png", "b"));
    ImageFolderPairModel model;
    model.setPairOpener([](const QUrl&, const QUrl&, const int) { return QString{}; });
    ASSERT_TRUE(model.loadFolders(folderUrl(left_), folderUrl(right_)));
    ASSERT_EQ(model.pairCount(), 1);
    ASSERT_TRUE(model.openPairAt(0));
    ASSERT_EQ(model.currentPair(), 0);
    ASSERT_FALSE(model.leftFolderPath().isEmpty());

    model.clear();
    EXPECT_EQ(model.pairCount(), 0);
    EXPECT_EQ(model.currentPair(), -1);
    EXPECT_TRUE(model.leftFolderPath().isEmpty());
    EXPECT_TRUE(model.rightFolderPath().isEmpty());
    EXPECT_TRUE(model.leftFolderName().isEmpty());
    EXPECT_TRUE(model.rightFolderName().isEmpty());
    EXPECT_TRUE(model.errorText().isEmpty());
    EXPECT_FALSE(model.openPairAt(0));

    // Repeating clear on an already-detached model is a safe no-op.
    model.clear();
    EXPECT_EQ(model.pairCount(), 0);
    EXPECT_EQ(model.currentPair(), -1);
}

TEST_F(ImageFolderPairModelTests, AsyncOpenAdvancesOnlyOnMatchingSuccess) {
    static_cast<void>(writeFile(left_, "shot.png", "a"));
    static_cast<void>(writeFile(right_, "shot.png", "b"));

    ImageFolderPairModel model;
    int nextRequestId = 42;
    int lastPairId = -1;
    std::vector<RecordedPair> invoked;
    model.setAsyncPairOpener(
        [&](const QUrl& primary, const QUrl& secondary, const int pairId, QString* error) {
            invoked.push_back(RecordedPair{primary, secondary});
            lastPairId = pairId;
            if (error != nullptr) {
                error->clear();
            }
            return nextRequestId;
        });
    int cancelledRequest = -1;
    model.setAsyncPairCancel(
        [&cancelledRequest](const int requestId) { cancelledRequest = requestId; });
    ASSERT_TRUE(model.loadFolders(folderUrl(left_), folderUrl(right_)));
    const int completeRow = model.firstCompleteRow();
    ASSERT_GE(completeRow, 0);

    ASSERT_TRUE(model.openPairAt(completeRow));
    EXPECT_TRUE(model.openPending());
    EXPECT_EQ(model.pendingPair(), completeRow);
    EXPECT_EQ(model.currentPair(), -1);
    ASSERT_EQ(invoked.size(), 1U);
    EXPECT_EQ(lastPairId, completeRow);

    // A stale completion cannot advance the list selection.
    model.completePairOpen(41U, true, QString{});
    EXPECT_TRUE(model.openPending());
    EXPECT_EQ(model.currentPair(), -1);

    // A failure reports its source and clears the pending state, still leaving selection.
    model.completePairOpen(42U, false, QStringLiteral("B 侧损坏"));
    EXPECT_FALSE(model.openPending());
    EXPECT_EQ(model.currentPair(), -1);
    EXPECT_EQ(model.errorText(), QStringLiteral("B 侧损坏"));

    // A later success commits exactly once and clears the failure text.
    ASSERT_TRUE(model.openPairAt(completeRow));
    model.completePairOpen(42U, true, QString{});
    EXPECT_FALSE(model.openPending());
    EXPECT_EQ(model.currentPair(), completeRow);
    EXPECT_TRUE(model.errorText().isEmpty());
}

TEST_F(ImageFolderPairModelTests, PendingNavigationUsesNewestDesiredRowAndDoesNotRestartIt) {
    for (const QString& name :
         {QStringLiteral("a.png"), QStringLiteral("b.png"), QStringLiteral("c.png")}) {
        static_cast<void>(writeFile(left_, name, "a"));
        static_cast<void>(writeFile(right_, name, "b"));
    }
    ImageFolderPairModel model;
    ASSERT_TRUE(model.loadFolders(folderUrl(left_), folderUrl(right_)));
    int requests = 0;
    std::vector<int> cancelled;
    model.setAsyncPairOpener([&](const QUrl&, const QUrl&, int, QString*) { return ++requests; });
    model.setAsyncPairCancel([&](const int request) { cancelled.push_back(request); });
    model.setCurrentPair(0);
    ASSERT_TRUE(model.openPairAt(model.stepCompleteRow(1)));
    EXPECT_EQ(model.pendingPair(), 1);
    EXPECT_TRUE(model.openPairAt(1));
    EXPECT_EQ(requests, 1);
    EXPECT_TRUE(cancelled.empty());
    ASSERT_TRUE(model.openPairAt(model.stepCompleteRow(1)));
    EXPECT_EQ(model.pendingPair(), 2);
    EXPECT_EQ(model.currentPair(), 0);
    ASSERT_EQ(cancelled.size(), 1U);
    EXPECT_EQ(cancelled.front(), 1);
    model.completePairOpen(1, true, {});
    EXPECT_EQ(model.pendingPair(), 2);
    EXPECT_EQ(model.stepCompleteRow(1), 0);
    EXPECT_EQ(model.stepCompleteRow(-1), 1);
    model.completePairOpen(2, true, {});
    EXPECT_EQ(model.currentPair(), 2);
    EXPECT_FALSE(model.openPending());
}

TEST_F(ImageFolderPairModelTests, CancelPendingAsyncOpenIgnoresLateCompletion) {
    static_cast<void>(writeFile(left_, "shot.png", "a"));
    static_cast<void>(writeFile(right_, "shot.png", "b"));

    ImageFolderPairModel model;
    model.setAsyncPairOpener([](const QUrl&, const QUrl&, const int, QString*) { return 7; });
    int cancelledRequest = -1;
    model.setAsyncPairCancel(
        [&cancelledRequest](const int requestId) { cancelledRequest = requestId; });
    ASSERT_TRUE(model.loadFolders(folderUrl(left_), folderUrl(right_)));
    const int completeRow = model.firstCompleteRow();
    ASSERT_GE(completeRow, 0);
    ASSERT_TRUE(model.openPairAt(completeRow));

    model.cancelPendingOpen();
    EXPECT_FALSE(model.openPending());
    EXPECT_EQ(cancelledRequest, 7);
    model.completePairOpen(7U, true, QString{});
    EXPECT_EQ(model.currentPair(), -1);
}
TEST_F(ImageFolderPairModelTests, LoadFoldersReportsCountsForCompleteAndSingleRows) {
    static_cast<void>(writeFile(left_, "pair_a.png", "a"));
    static_cast<void>(writeFile(right_, "pair_a.png", "b"));
    static_cast<void>(writeFile(left_, "left_only.png", "a"));
    static_cast<void>(writeFile(right_, "right_only.png", "b"));

    ImageFolderPairModel model;
    ASSERT_TRUE(model.loadFolders(folderUrl(left_), folderUrl(right_)));
    EXPECT_EQ(model.pairCount(), 3);
    EXPECT_EQ(model.completeCount(), 1);
    EXPECT_EQ(model.missingCount(), 2);
    EXPECT_EQ(model.conflictCount(), 0);
}

TEST_F(ImageFolderPairModelTests, MergePairRowsListsCaseConflictsAndPairsCaseInsensitively) {
    using dvs::ui::ImageFolderPairModel;
    // Pure-function coverage for the case-conflict semantics, independent of the
    // filesystem's case sensitivity (T6).
    ImageFolderPairModel::FolderSideScan left;
    left.entries.emplace(QStringLiteral("shot.png"), QUrl::fromLocalFile("C:/a/Shot.PNG"));
    left.names[QStringLiteral("shot.png")] =
        QStringList{QStringLiteral("Shot.PNG"), QStringLiteral("shot.png")};
    left.entries.emplace(QStringLiteral("only_left.png"),
                         QUrl::fromLocalFile("C:/a/only_left.png"));
    left.names[QStringLiteral("only_left.png")] = QStringList{QStringLiteral("only_left.png")};

    ImageFolderPairModel::FolderSideScan right;
    right.entries.emplace(QStringLiteral("shot.png"), QUrl::fromLocalFile("C:/b/shot.png"));
    right.names[QStringLiteral("shot.png")] = QStringList{QStringLiteral("shot.png")};
    right.entries.emplace(QStringLiteral("only_right.png"),
                          QUrl::fromLocalFile("C:/b/only_right.png"));
    right.names[QStringLiteral("only_right.png")] = QStringList{QStringLiteral("only_right.png")};

    const std::vector<ImageFolderPairModel::PairRow> rows =
        ImageFolderPairModel::mergePairRows(left, right);
    ASSERT_EQ(rows.size(), 3U);
    // Sorted by name: only_left, only_right, shot.
    EXPECT_EQ(rows[0].fileName, QStringLiteral("only_left.png"));
    EXPECT_TRUE(rows[0].hasLeft);
    EXPECT_FALSE(rows[0].hasRight);
    EXPECT_FALSE(rows[0].caseConflict);
    EXPECT_EQ(rows[1].fileName, QStringLiteral("only_right.png"));
    EXPECT_TRUE(rows[1].hasRight);
    EXPECT_FALSE(rows[1].hasLeft);
    EXPECT_EQ(rows[2].fileName, QStringLiteral("Shot.PNG"));
    EXPECT_TRUE(rows[2].hasLeft);
    EXPECT_TRUE(rows[2].hasRight);
    // The ambiguous A side is listed explicitly with the file actually used.
    EXPECT_TRUE(rows[2].caseConflict);
    EXPECT_TRUE(rows[2].caseConflictDetail.contains(QStringLiteral("A 侧")));
    EXPECT_TRUE(rows[2].caseConflictDetail.contains(QStringLiteral("Shot.PNG")));
    EXPECT_TRUE(rows[2].caseConflictDetail.contains(QStringLiteral("shot.png")));
    EXPECT_EQ(rows[2].leftUrl, QUrl::fromLocalFile("C:/a/Shot.PNG"));
    EXPECT_EQ(rows[2].rightUrl, QUrl::fromLocalFile("C:/b/shot.png"));
}

TEST_F(ImageFolderPairModelTests, CaseConflictingNamesAreListedExplicitlyNotSilentlyChosen) {
    // "Shot.PNG" and "shot.png" in one folder fold to the same key: the row must say so
    // instead of silently picking one of them (T6).
    static_cast<void>(writeFile(left_, "Shot.PNG", "a"));
    static_cast<void>(writeFile(left_, "shot.png", "a"));
    static_cast<void>(writeFile(right_, "shot.png", "b"));
    const QDir leftDir{left_.path()};
    int shotEntries = 0;
    for (const QString& name : leftDir.entryList(QDir::Files, QDir::Name | QDir::IgnoreCase)) {
        if (name.toCaseFolded() == QStringLiteral("shot.png")) {
            ++shotEntries;
        }
    }
    if (shotEntries < 2) {
        GTEST_SKIP() << "filesystem collapses case variants; cannot create a name conflict";
    }

    ImageFolderPairModel model;
    ASSERT_TRUE(model.loadFolders(folderUrl(left_), folderUrl(right_)));
    ASSERT_EQ(model.pairCount(), 1);
    EXPECT_EQ(model.completeCount(), 1);
    EXPECT_EQ(model.missingCount(), 0);
    EXPECT_EQ(model.conflictCount(), 1);

    const QModelIndex row = model.index(0, 0);
    EXPECT_TRUE(model.data(row, ImageFolderPairModel::CaseConflictRole).toBool());
    const QString detail = model.data(row, ImageFolderPairModel::CaseConflictDetailRole).toString();
    EXPECT_TRUE(detail.contains(QStringLiteral("shot.png")));
    EXPECT_TRUE(detail.contains(QStringLiteral("Shot.PNG")));
    EXPECT_TRUE(detail.contains(QStringLiteral("A")));
    // The chosen URL is one of the two conflicting files and is named in the detail, so
    // the pairing is explicit rather than silent.
    const QUrl chosen = model.data(row, ImageFolderPairModel::LeftPathRole).toUrl();
    EXPECT_TRUE(detail.contains(QFileInfo{chosen.toLocalFile()}.fileName()));

    const QVariantMap urls = model.pairUrlsAt(0);
    EXPECT_EQ(urls.value(QStringLiteral("leftUrl")).toUrl(), chosen);
    EXPECT_EQ(urls.value(QStringLiteral("rightUrl")).toUrl(),
              model.data(row, ImageFolderPairModel::RightPathRole).toUrl());
    EXPECT_TRUE(urls.value(QStringLiteral("hasBoth")).toBool());
    EXPECT_TRUE(urls.value(QStringLiteral("caseConflict")).toBool());
    EXPECT_TRUE(urls.value(QStringLiteral("caseConflictDetail")).toString() == detail);
    EXPECT_TRUE(model.pairUrlsAt(-1).isEmpty());
    EXPECT_TRUE(model.pairUrlsAt(model.pairCount()).isEmpty());
}

TEST_F(ImageFolderPairModelTests, CountsReportCompleteMissingAndConflictingRows) {
    static_cast<void>(writeFile(left_, "pair.png", "a"));
    static_cast<void>(writeFile(right_, "pair.png", "b"));
    static_cast<void>(writeFile(left_, "left_only.png", "a"));
    static_cast<void>(writeFile(right_, "Only_Right.png", "b"));
    static_cast<void>(writeFile(right_, "only_right.png", "b"));
    const QDir rightDir{right_.path()};
    int onlyRightEntries = 0;
    for (const QString& name : rightDir.entryList(QDir::Files, QDir::Name | QDir::IgnoreCase)) {
        if (name.toCaseFolded() == QStringLiteral("only_right.png")) {
            ++onlyRightEntries;
        }
    }
    if (onlyRightEntries < 2) {
        GTEST_SKIP() << "filesystem collapses case variants; cannot create a name conflict";
    }

    ImageFolderPairModel model;
    ASSERT_TRUE(model.loadFolders(folderUrl(left_), folderUrl(right_)));
    // pair.png is complete; left_only is left-only; the two case variants of
    // only_right collapse into one explicit conflict row on the right side.
    EXPECT_EQ(model.pairCount(), 3);
    EXPECT_EQ(model.completeCount(), 1);
    EXPECT_EQ(model.missingCount(), 2);
    EXPECT_EQ(model.conflictCount(), 1);
}

TEST_F(ImageFolderPairModelTests, SingleSideRowOpensExistingSideAndAdvancesSelectionOnSuccess) {
    static_cast<void>(writeFile(left_, "shot.png", "a"));
    static_cast<void>(writeFile(right_, "shot.png", "b"));
    static_cast<void>(writeFile(right_, "only_right.png", "b"));

    ImageFolderPairModel model;
    int nextRequestId = 100;
    std::vector<QUrl> opened;
    int lastRow = -1;
    model.setSingleSideOpener(
        [&opened, &lastRow, &nextRequestId](const QUrl& url, const int row, QString* error) {
            opened.push_back(url);
            lastRow = row;
            if (error != nullptr) {
                error->clear();
            }
            return nextRequestId;
        });
    int cancelledRequest = -1;
    model.setAsyncPairCancel(
        [&cancelledRequest](const int requestId) { cancelledRequest = requestId; });
    ASSERT_TRUE(model.loadFolders(folderUrl(left_), folderUrl(right_)));
    // Rows: only_right (right-only), shot (complete).
    ASSERT_EQ(model.pairCount(), 2);
    const int singleRow = 0;
    EXPECT_FALSE(model.data(model.index(singleRow, 0), ImageFolderPairModel::HasLeftRole).toBool());
    EXPECT_TRUE(model.data(model.index(singleRow, 0), ImageFolderPairModel::HasRightRole).toBool());

    // Complete rows are not single-side opens.
    EXPECT_FALSE(model.openSingleSideAt(1));
    EXPECT_TRUE(opened.empty());
    // Out-of-range rows are rejected.
    EXPECT_FALSE(model.openSingleSideAt(-1));
    EXPECT_FALSE(model.openSingleSideAt(model.pairCount()));

    ASSERT_TRUE(model.openSingleSideAt(singleRow));
    EXPECT_TRUE(model.openPending());
    EXPECT_EQ(model.pendingPair(), singleRow);
    ASSERT_EQ(opened.size(), 1U);
    EXPECT_EQ(lastRow, singleRow);
    EXPECT_EQ(opened.front().toLocalFile(),
              QDir(right_.path()).filePath(QStringLiteral("only_right.png")));
    EXPECT_EQ(model.currentPair(), -1);

    // A stale completion cannot advance the selection.
    model.completeSingleSideOpen(99U, true, QString{});
    EXPECT_EQ(model.currentPair(), -1);

    // Failure surfaces the source and keeps the selection.
    model.completeSingleSideOpen(100U, false, QStringLiteral("文件损坏"));
    EXPECT_FALSE(model.openPending());
    EXPECT_EQ(model.currentPair(), -1);
    EXPECT_EQ(model.errorText(), QStringLiteral("文件损坏"));

    // Success advances the selection exactly once and clears the failure text.
    ASSERT_TRUE(model.openSingleSideAt(singleRow));
    model.completeSingleSideOpen(100U, true, QString{});
    EXPECT_FALSE(model.openPending());
    EXPECT_EQ(model.currentPair(), singleRow);
    EXPECT_TRUE(model.errorText().isEmpty());

    // Cancelling a pending single-side open ignores its late terminal.
    ASSERT_TRUE(model.openSingleSideAt(singleRow));
    model.cancelPendingOpen();
    EXPECT_FALSE(model.openPending());
    EXPECT_EQ(cancelledRequest, 100);
    model.completeSingleSideOpen(100U, true, QString{});
    EXPECT_EQ(model.currentPair(), singleRow);
}
} // namespace
