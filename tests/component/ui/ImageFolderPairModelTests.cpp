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

} // namespace
