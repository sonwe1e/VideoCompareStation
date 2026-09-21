#include "dvs/persistence/IssueRecordRepository.h"
#include "dvs/ui/ImageFolderPairModel.h"
#include "dvs/ui/ImageReviewController.h"
#include "dvs/ui/IssueLogController.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUrl>

#include <gtest/gtest.h>

namespace dvs::ui {
namespace {

class IssueLogControllerTests : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(tempDir_.isValid());
        repository_ = std::make_unique<persistence::IssueRecordRepository>(
            (tempDir_.path() + QStringLiteral("/issues.json")).toStdString());
        controller_ = std::make_unique<IssueLogController>(repository_.get());
        controller_->setDefaultDocumentPath(
            QUrl::fromLocalFile(tempDir_.path() + QStringLiteral("/issues.json")));
    }

    QTemporaryDir tempDir_;
    std::unique_ptr<persistence::IssueRecordRepository> repository_;
    std::unique_ptr<IssueLogController> controller_;
};

TEST_F(IssueLogControllerTests, SavesAndReloadsManualIssueLog) {
    ImageReviewController image;
    controller_->setImageController(&image);
    controller_->setWorkspaceMode(QStringLiteral("image"));
    ASSERT_TRUE(controller_->captureCurrentIssue(QStringLiteral("note-a")));
    ASSERT_TRUE(controller_->saveIssues(
        QUrl::fromLocalFile(tempDir_.path() + QStringLiteral("/issues.json"))));
    controller_->clearIssues();
    EXPECT_EQ(controller_->count(), 0);
    ASSERT_TRUE(controller_->loadIssues(
        QUrl::fromLocalFile(tempDir_.path() + QStringLiteral("/issues.json"))));
    EXPECT_EQ(controller_->count(), 1);
    const QVariantMap issue = controller_->issueAt(0);
    EXPECT_EQ(issue.value(QStringLiteral("note")).toString(), QStringLiteral("note-a"));
    EXPECT_FALSE(issue.value(QStringLiteral("hasValidPresentation")).toBool());
}

TEST_F(IssueLogControllerTests, RestoreRequiresRelocationWhenSourceMoved) {
    const QString sourcePath = tempDir_.path() + QStringLiteral("/clip.bin");
    {
        QFile file{sourcePath};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("aaaa");
    }

    application::IssueRecord record;
    record.schemaVersion = application::kIssueRecordSchemaVersion;
    record.kind = application::IssueRecordKind::Video;
    record.hasValidPresentation = true;
    application::IssueSourceRef source;
    source.path = sourcePath.toStdString();
    {
        QFile file{sourcePath};
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        source.byteSize = file.size();
        source.modifiedUtcMilliseconds = QFileInfo{sourcePath}.lastModified().toMSecsSinceEpoch();
    }
    source.hasPresentation = true;
    source.displayIndex = 7;
    record.sources.push_back(source);

    const std::string path = (tempDir_.path() + QStringLiteral("/moved.json")).toStdString();
    std::string error;
    ASSERT_TRUE(repository_->save(path, {record}, &error)) << error;
    ASSERT_TRUE(controller_->loadIssues(QUrl::fromLocalFile(QString::fromStdString(path))));
    ASSERT_EQ(controller_->count(), 1);

    QVariantMap ready = controller_->restoreIssue(0);
    EXPECT_EQ(ready.value(QStringLiteral("decision")).toString(), QStringLiteral("ready"));
    EXPECT_TRUE(ready.value(QStringLiteral("ok")).toBool());

    {
        QFile file{sourcePath};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("bbbbbbbb");
    }
    QVariantMap relocated = controller_->restoreIssue(0);
    EXPECT_EQ(relocated.value(QStringLiteral("decision")).toString(),
              QStringLiteral("relocation-required"));
    EXPECT_FALSE(relocated.value(QStringLiteral("ok")).toBool());
}

TEST_F(IssueLogControllerTests, OldSchemaLoadFailsWithExplanation) {
    const QString path = tempDir_.path() + QStringLiteral("/old.json");
    {
        QFile file{path};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(R"({"schemaVersion":0,"kind":"vcstation-issue-log","records":[]})");
    }
    EXPECT_FALSE(controller_->loadIssues(QUrl::fromLocalFile(path)));
    EXPECT_NE(
        controller_->lastError().indexOf(QStringLiteral("unsupported issue-record schemaVersion")),
        -1);
}

} // namespace
} // namespace dvs::ui
