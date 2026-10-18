#include "dvs/ui/ImageFolderCursor.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QUrl>

#include <gtest/gtest.h>

namespace {

using dvs::ui::ImageFolderCursor;

[[nodiscard]] QString writeFile(const QTemporaryDir& dir, const QString& name) {
    const QString path = QDir(dir.path()).filePath(name);
    QFile file{path};
    if (!file.open(QIODevice::WriteOnly)) {
        return {};
    }
    file.write("x");
    file.close();
    return path;
}

class ImageFolderCursorTests : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(folder_.isValid());
    }

    [[nodiscard]] QString pathOf(const QString& name) const {
        return QDir(folder_.path()).filePath(name);
    }

    QTemporaryDir folder_;
};

TEST_F(ImageFolderCursorTests, OrdersNeighboursNaturallyInsteadOfLexically) {
    static_cast<void>(writeFile(folder_, "shot10.png"));
    static_cast<void>(writeFile(folder_, "shot2.png"));
    static_cast<void>(writeFile(folder_, "shot1.png"));
    static_cast<void>(writeFile(folder_, "shot20.png"));

    ImageFolderCursor cursor;
    cursor.setCurrentFile(pathOf("shot1.png"));

    ASSERT_EQ(cursor.count(), 4);
    EXPECT_EQ(cursor.currentIndex(), 1);
    EXPECT_EQ(cursor.nextUrl().fileName(), QStringLiteral("shot2.png"));
    EXPECT_EQ(cursor.lastUrl().fileName(), QStringLiteral("shot20.png"));

    cursor.setCurrentFile(pathOf("shot2.png"));
    EXPECT_EQ(cursor.nextUrl().fileName(), QStringLiteral("shot10.png"));
}

TEST_F(ImageFolderCursorTests, StopsAtBothEdgesInsteadOfWrapping) {
    static_cast<void>(writeFile(folder_, "a.png"));
    static_cast<void>(writeFile(folder_, "b.png"));
    static_cast<void>(writeFile(folder_, "c.png"));

    ImageFolderCursor cursor;
    cursor.setCurrentFile(pathOf("a.png"));
    EXPECT_FALSE(cursor.canGoPrevious());
    EXPECT_TRUE(cursor.canGoNext());
    EXPECT_TRUE(cursor.previousUrl().isEmpty());

    cursor.setCurrentFile(pathOf("c.png"));
    EXPECT_TRUE(cursor.canGoPrevious());
    EXPECT_FALSE(cursor.canGoNext());
    EXPECT_TRUE(cursor.nextUrl().isEmpty());
    EXPECT_EQ(cursor.currentIndex(), cursor.count());
}

TEST_F(ImageFolderCursorTests, ListsOnlyThisDirectoryAndOnlyStillImages) {
    static_cast<void>(writeFile(folder_, "first.png"));
    static_cast<void>(writeFile(folder_, "second.jpg"));
    static_cast<void>(writeFile(folder_, "notes.txt"));
    static_cast<void>(writeFile(folder_, "clip.mp4"));
    ASSERT_TRUE(QDir(folder_.path()).mkpath("nested"));
    static_cast<void>(writeFile(folder_, "nested/third.png"));

    ImageFolderCursor cursor;
    cursor.setCurrentFile(pathOf("first.png"));

    // A recursive scan would offer nested/third.png as a neighbour the flat list never showed.
    EXPECT_EQ(cursor.count(), 2);
    EXPECT_EQ(cursor.nextUrl().fileName(), QStringLiteral("second.jpg"));
}

TEST_F(ImageFolderCursorTests, ClearsForANonImageEvenWhenItExists) {
    static_cast<void>(writeFile(folder_, "a.png"));
    static_cast<void>(writeFile(folder_, "notes.txt"));

    ImageFolderCursor cursor;
    cursor.setCurrentFile(pathOf("a.png"));
    ASSERT_EQ(cursor.count(), 1);

    cursor.setCurrentFile(pathOf("notes.txt"));
    EXPECT_EQ(cursor.count(), 0);
    EXPECT_EQ(cursor.currentIndex(), 0);
    EXPECT_TRUE(cursor.folderPath().isEmpty());
    EXPECT_TRUE(cursor.nextUrl().isEmpty());
}

TEST_F(ImageFolderCursorTests, ClearsWhenTheOpenedFileNoLongerExists) {
    const QString path = writeFile(folder_, "gone.png");
    static_cast<void>(writeFile(folder_, "other.png"));
    ASSERT_FALSE(path.isEmpty());
    ASSERT_TRUE(QFile::remove(path));

    ImageFolderCursor cursor;
    cursor.setCurrentFile(path);
    EXPECT_EQ(cursor.count(), 0);
    EXPECT_TRUE(cursor.currentFileName().isEmpty());
}

TEST_F(ImageFolderCursorTests, RescansSoADeletedNeighbourDisappears) {
    static_cast<void>(writeFile(folder_, "1.png"));
    const QString second = writeFile(folder_, "2.png");
    static_cast<void>(writeFile(folder_, "3.png"));

    ImageFolderCursor cursor;
    cursor.setCurrentFile(pathOf("3.png"));
    ASSERT_EQ(cursor.previousUrl().fileName(), QStringLiteral("2.png"));

    ASSERT_TRUE(QFile::remove(second));
    cursor.setCurrentFile(pathOf("3.png"));

    EXPECT_EQ(cursor.count(), 2);
    EXPECT_EQ(cursor.previousUrl().fileName(), QStringLiteral("1.png"));
}

TEST_F(ImageFolderCursorTests, SingleImageDirectoryOffersNoNeighbours) {
    static_cast<void>(writeFile(folder_, "only.png"));

    ImageFolderCursor cursor;
    cursor.setCurrentFile(pathOf("only.png"));

    EXPECT_EQ(cursor.count(), 1);
    EXPECT_EQ(cursor.currentIndex(), 1);
    EXPECT_FALSE(cursor.canGoNext());
    EXPECT_FALSE(cursor.canGoPrevious());
}

TEST_F(ImageFolderCursorTests, ClearResetsAReadyCursor) {
    static_cast<void>(writeFile(folder_, "a.png"));
    static_cast<void>(writeFile(folder_, "b.png"));

    ImageFolderCursor cursor;
    cursor.setCurrentFile(pathOf("a.png"));
    ASSERT_EQ(cursor.count(), 2);

    cursor.clear();
    EXPECT_EQ(cursor.count(), 0);
    EXPECT_EQ(cursor.currentIndex(), 0);
    EXPECT_TRUE(cursor.previousUrl().isEmpty());
    EXPECT_TRUE(cursor.nextUrl().isEmpty());
}

} // namespace
