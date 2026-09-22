#include "dvs/domain/ComparisonValidator.h"
#include "dvs/domain/FrameTimeline.h"
#include "dvs/persistence/IssueRecordRepository.h"
#include "dvs/ui/ImageFolderPairModel.h"
#include "dvs/ui/ImageReviewController.h"
#include "dvs/ui/IssueLogController.h"
#include "dvs/ui/ReviewController.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUrl>

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

namespace dvs::ui {
namespace {

void ensureCoreApplication() {
    if (QCoreApplication::instance() != nullptr) {
        return;
    }
    static int argumentCount = 1;
    static char applicationName[] = "IssueLogControllerTests";
    static char* arguments[] = {applicationName, nullptr};
    static QCoreApplication application{argumentCount, arguments};
    static_cast<void>(application);
}

[[nodiscard]] application::SessionSnapshot makeReadySnapshot(const std::vector<QString>& paths,
                                                             const std::size_t referenceIndex) {
    application::SessionSnapshot snapshot;
    snapshot.sessionId = domain::SessionId{17U};
    snapshot.sessionEpoch = domain::SessionEpoch{3U};
    snapshot.playbackGeneration = domain::PlaybackGeneration{5U};
    snapshot.deviceGeneration = domain::DeviceGeneration{2U};
    snapshot.graphicsReady = true;
    snapshot.sessionState = domain::SessionState::kReady;
    snapshot.playbackState = domain::PlaybackState::kPaused;

    const auto rate = domain::RationalRate::create(30, 1);
    EXPECT_TRUE(rate);

    std::vector<domain::ComparisonSource> sources;
    sources.reserve(paths.size());
    for (std::size_t index = 0U; index < paths.size(); ++index) {
        const QFileInfo info{paths[index]};
        sources.push_back(domain::ComparisonSource{
            .id = static_cast<domain::SourceId>(index),
            .role = index == referenceIndex ? domain::ComparisonRole::kReference
                                            : domain::ComparisonRole::kPrediction,
            .descriptor =
                domain::MediaDescriptor{
                    .normalizedPath = std::filesystem::path{paths[index].toStdWString()},
                    .extent = domain::MediaExtent{.width = 1'920U, .height = 1'080U},
                    .frameRate = rate.value(),
                    .frameCount =
                        domain::FrameCountInfo{
                            .value = 12U,
                            .origin = domain::FrameCountOrigin::kReported,
                        },
                    .duration = domain::MediaTime{400'000},
                    .codecId = "h264",
                    .pixelFormatId = "nv12",
                    .bitDepth = 8U,
                    .decodeCapabilities =
                        domain::DecodeCapabilities{
                            .softwareDecode = true,
                            .d3d11VaDecode = true,
                        },
                    .timingConfidence = domain::TimingConfidence::kVerifiedCfr,
                    .sourceIdentity =
                        domain::SourceFileIdentity{
                            .byteSize = static_cast<std::uint64_t>(info.size()),
                            .modifiedUtcMilliseconds = info.lastModified().toMSecsSinceEpoch(),
                            .fingerprintSha256 = std::string(64U, '0'),
                        },
                },
            .displayName = std::string{"Source "} + static_cast<char>('A' + index),
        });
    }
    auto validated = domain::ComparisonValidator::validate(std::move(sources));
    EXPECT_TRUE(validated);
    snapshot.validatedComparison =
        std::make_shared<const domain::ValidatedComparisonSet>(std::move(validated).value().set);
    for (const auto& source : snapshot.validatedComparison->sources()) {
        snapshot.sources.push_back(application::SessionSourceView{
            .sourceId = source.id,
            .role = source.role,
            .displayName = source.displayName,
        });
    }
    snapshot.canonicalFrameCount =
        static_cast<std::uint64_t>(snapshot.validatedComparison->canonicalFrameCount());
    return snapshot;
}

[[nodiscard]] bool writeFile(const QString& path) {
    QFile file{path};
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return file.write("fixture") == 7;
}

class FakeBackend final {
public:
    application::SessionSnapshot currentSnapshot;
};

[[nodiscard]] ReviewController::Dependencies
dependenciesFor(const std::shared_ptr<FakeBackend>& backend) {
    return ReviewController::Dependencies{
        .submit =
            [](application::PlaybackCommand) { return application::PortSubmitResult::Accepted; },
        .snapshot = [backend]() -> std::shared_ptr<const application::SessionSnapshot> {
            return std::make_shared<const application::SessionSnapshot>(backend->currentSnapshot);
        },
        .takeCompletedCommands = [] { return std::vector<application::CommandTerminal>{}; },
        .decoderBackendStates = [] { return std::vector<ReviewController::DecoderBackendState>{}; },
        .eventDriven = true,
    };
}

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

TEST_F(IssueLogControllerTests, CapturesPerSideFramePtsAndMappingFromCommittedSnapshot) {
    ensureCoreApplication();
    const QString pathA = tempDir_.path() + QStringLiteral("/a.mp4");
    const QString pathB = tempDir_.path() + QStringLiteral("/b.mp4");
    ASSERT_TRUE(writeFile(pathA));
    ASSERT_TRUE(writeFile(pathB));

    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot = makeReadySnapshot({pathA, pathB}, 0U);
    backend->currentSnapshot.displayedFrame = domain::FrameId{7};
    backend->currentSnapshot.presentedSources = {
        application::PresentedSourceState{
            .sourceId = 0U,
            .sourceFrameId = domain::FrameId{7},
            .matchKind = application::FrameMatchKind::ExactIndex,
            .alignmentConfidence = 1.0F,
            .presentationTime = domain::MediaTime{233'000},
        },
        application::PresentedSourceState{
            .sourceId = 1U,
            .sourceFrameId = domain::FrameId{9},
            .matchKind = application::FrameMatchKind::AutoAligned,
            .alignmentConfidence = 0.95F,
            .presentationTime = domain::MediaTime{301'000},
        },
    };
    backend->currentSnapshot.alignmentRevision = 5U;
    ReviewController review{dependenciesFor(backend)};
    controller_->setReviewController(&review);
    controller_->setWorkspaceMode(QStringLiteral("video"));
    ASSERT_TRUE(controller_->captureCurrentIssue(QStringLiteral("offset-note")));

    const std::string path = (tempDir_.path() + QStringLiteral("/issues.json")).toStdString();
    ASSERT_TRUE(controller_->saveIssues(QUrl::fromLocalFile(QString::fromStdString(path))));
    const application::IssueRecordIoResult loaded = repository_->load(path);
    ASSERT_TRUE(loaded.ok) << loaded.error;
    ASSERT_EQ(loaded.records.size(), 1U);
    const application::IssueRecord& record = loaded.records.front();
    EXPECT_TRUE(record.hasValidPresentation);
    EXPECT_EQ(record.alignmentRevision, 5U);
    ASSERT_EQ(record.sources.size(), 2U);

    const application::IssueSourceRef& sourceA = record.sources[0];
    EXPECT_TRUE(sourceA.hasPresentation);
    EXPECT_EQ(sourceA.displayIndex, 7);
    EXPECT_EQ(sourceA.presentationTimestampTicks, 233'000);
    EXPECT_EQ(sourceA.timeBaseNumerator, 1);
    EXPECT_EQ(sourceA.timeBaseDenominator, 1'000'000);
    EXPECT_EQ(sourceA.presentationMatchKind, 0); // ExactIndex

    const application::IssueSourceRef& sourceB = record.sources[1];
    EXPECT_TRUE(sourceB.hasPresentation);
    EXPECT_EQ(sourceB.displayIndex, 9);
    EXPECT_EQ(sourceB.presentationTimestampTicks, 301'000);
    EXPECT_EQ(sourceB.presentationMatchKind, 2); // AutoAligned

    // Restore targets the canonical source's actual frame, never an arbitrary side.
    controller_->clearIssues();
    ASSERT_TRUE(controller_->loadIssues(QUrl::fromLocalFile(QString::fromStdString(path))));
    const QVariantMap restore = controller_->restoreIssue(0);
    EXPECT_EQ(restore.value(QStringLiteral("decision")).toString(), QStringLiteral("ready"));
    EXPECT_EQ(restore.value(QStringLiteral("frame")).toLongLong(), 7);
}

TEST_F(IssueLogControllerTests, CapturesMissingSideWithoutClaimingAFrame) {
    ensureCoreApplication();
    const QString pathA = tempDir_.path() + QStringLiteral("/a.mp4");
    const QString pathB = tempDir_.path() + QStringLiteral("/b.mp4");
    const QString pathC = tempDir_.path() + QStringLiteral("/c.mp4");
    ASSERT_TRUE(writeFile(pathA));
    ASSERT_TRUE(writeFile(pathB));
    ASSERT_TRUE(writeFile(pathC));

    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot = makeReadySnapshot({pathA, pathB, pathC}, 0U);
    backend->currentSnapshot.displayedFrame = domain::FrameId{7};
    backend->currentSnapshot.presentedSources = {
        application::PresentedSourceState{
            .sourceId = 0U,
            .sourceFrameId = domain::FrameId{7},
            .matchKind = application::FrameMatchKind::ExactIndex,
            .alignmentConfidence = 1.0F,
            .presentationTime = domain::MediaTime{270'000},
        },
        application::PresentedSourceState{
            .sourceId = 1U,
            .sourceFrameId = std::nullopt,
            .matchKind = application::FrameMatchKind::Missing,
            .alignmentConfidence = 0.0F,
            .missingReason = application::MissingReason::AlignmentGap,
        },
        application::PresentedSourceState{
            .sourceId = 2U,
            .sourceFrameId = domain::FrameId{8},
            .matchKind = application::FrameMatchKind::GlobalOffset,
            .alignmentConfidence = 0.9F,
        },
    };
    backend->currentSnapshot.alignmentRevision = 9U;

    ReviewController review{dependenciesFor(backend)};
    controller_->setReviewController(&review);
    controller_->setWorkspaceMode(QStringLiteral("video"));
    ASSERT_TRUE(controller_->captureCurrentIssue(QStringLiteral("missing-note")));

    const std::string path = (tempDir_.path() + QStringLiteral("/issues.json")).toStdString();
    ASSERT_TRUE(controller_->saveIssues(QUrl::fromLocalFile(QString::fromStdString(path))));
    const application::IssueRecordIoResult loaded = repository_->load(path);
    ASSERT_TRUE(loaded.ok) << loaded.error;
    ASSERT_EQ(loaded.records.size(), 1U);
    const application::IssueRecord& record = loaded.records.front();
    EXPECT_TRUE(record.hasValidPresentation);
    EXPECT_EQ(record.alignmentRevision, 9U);
    ASSERT_EQ(record.sources.size(), 3U);

    EXPECT_TRUE(record.sources[0].hasPresentation);
    EXPECT_EQ(record.sources[0].displayIndex, 7);

    const application::IssueSourceRef& missing = record.sources[1];
    EXPECT_FALSE(missing.hasPresentation);
    EXPECT_EQ(missing.displayIndex, -1);
    EXPECT_EQ(missing.presentationTimestampTicks, -1);
    EXPECT_EQ(missing.presentationMatchKind, 4);     // Missing
    EXPECT_EQ(missing.presentationMissingReason, 1); // AlignmentGap

    EXPECT_TRUE(record.sources[2].hasPresentation);
    EXPECT_EQ(record.sources[2].displayIndex, 8);
    EXPECT_EQ(record.sources[2].presentationMatchKind, 1); // GlobalOffset
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
        file.write(R"({"schemaVersion":0,"kind":"comparestation-issue-log","records":[]})");
    }
    EXPECT_FALSE(controller_->loadIssues(QUrl::fromLocalFile(path)));
    EXPECT_NE(
        controller_->lastError().indexOf(QStringLiteral("unsupported issue-record schemaVersion")),
        -1);
}

} // namespace
} // namespace dvs::ui
