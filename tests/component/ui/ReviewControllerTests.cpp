#include "dvs/domain/ComparisonValidator.h"
#include "dvs/domain/FrameTimeline.h"
#include "dvs/ui/ReviewController.h"
#include "dvs/ui/ReviewShellController.h"
#include "dvs/ui/SourceIdentity.h"
#include "dvs/ui/SourceListModel.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace dvs::ui {
namespace {

using namespace std::chrono_literals;

void ensureCoreApplication() {
    if (QCoreApplication::instance() != nullptr) {
        return;
    }
    static int argumentCount = 1;
    static char applicationName[] = "ReviewControllerTests";
    static char* arguments[] = {applicationName, nullptr};
    static QCoreApplication application{argumentCount, arguments};
    static_cast<void>(application);
}

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate predicate, const std::chrono::milliseconds timeout = 1s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        QThread::msleep(1U);
    }
    return predicate();
}

[[nodiscard]] application::SessionSnapshot emptySnapshot(const bool graphicsReady = true,
                                                         const std::uint64_t sessionId = 17U,
                                                         const std::uint64_t sessionEpoch = 3U) {
    application::SessionSnapshot snapshot;
    snapshot.sessionId = domain::SessionId{sessionId};
    snapshot.sessionEpoch = domain::SessionEpoch{sessionEpoch};
    snapshot.deviceGeneration = domain::DeviceGeneration{2U};
    snapshot.graphicsReady = graphicsReady;
    return snapshot;
}

[[nodiscard]] application::SessionSnapshot readySnapshot(const std::int64_t frame,
                                                         const std::uint64_t frameCount,
                                                         const std::uint64_t sessionEpoch = 3U) {
    application::SessionSnapshot snapshot = emptySnapshot(true, 17U, sessionEpoch);
    snapshot.playbackGeneration = domain::PlaybackGeneration{5U};
    snapshot.sessionState = domain::SessionState::kReady;
    snapshot.playbackState = domain::PlaybackState::kPaused;
    snapshot.displayedFrame = domain::FrameId{frame};
    snapshot.canonicalFrameCount = frameCount;
    snapshot.sources = {
        application::SessionSourceView{
            .sourceId = 0U,
            .role = domain::ComparisonRole::kReference,
            .displayName = "A",
        },
        application::SessionSourceView{
            .sourceId = 1U,
            .role = domain::ComparisonRole::kPrediction,
            .displayName = "B",
        },
    };
    return snapshot;
}

[[nodiscard]] domain::SourceFileIdentity realFileIdentity(const std::filesystem::path& path) {
    const QFileInfo info{QString::fromStdWString(path.wstring())};
    return domain::SourceFileIdentity{
        .byteSize = info.exists() ? static_cast<std::uint64_t>(info.size()) : 0U,
        .modifiedUtcMilliseconds = info.exists() ? info.lastModified().toMSecsSinceEpoch() : 0,
        .fingerprintSha256 = std::string(64U, '0'),
    };
}

[[nodiscard]] application::SessionSnapshot
readySnapshotWithTiming(const std::vector<std::filesystem::path>& paths,
                        const std::size_t referenceIndex,
                        const std::optional<domain::RationalRate> rate,
                        const domain::TimingConfidence timingConfidence,
                        const std::int64_t frameCount,
                        const bool includeSourceIdentity = true) {
    application::SessionSnapshot snapshot = readySnapshot(2, 12);
    std::vector<domain::ComparisonSource> sources;
    sources.reserve(paths.size());
    for (std::size_t index = 0U; index < paths.size(); ++index) {
        sources.push_back(domain::ComparisonSource{
            .id = static_cast<domain::SourceId>(index),
            .role = index == referenceIndex ? domain::ComparisonRole::kReference
                                            : domain::ComparisonRole::kPrediction,
            .descriptor =
                domain::MediaDescriptor{
                    .normalizedPath = paths[index],
                    .extent = domain::MediaExtent{.width = 1'920U, .height = 1'080U},
                    .frameRate = rate,
                    .frameCount =
                        domain::FrameCountInfo{
                            .value = frameCount,
                            .origin =
                                timingConfidence == domain::TimingConfidence::kVariableFrameRate
                                    ? domain::FrameCountOrigin::kIndexed
                                    : domain::FrameCountOrigin::kReported,
                        },
                    .duration = domain::MediaTime{std::max<std::int64_t>(400'000, frameCount)},
                    .codecId = "h264",
                    .pixelFormatId = "nv12",
                    .bitDepth = 8U,
                    .decodeCapabilities =
                        domain::DecodeCapabilities{
                            .softwareDecode = true,
                            .d3d11VaDecode = true,
                        },
                    .timingConfidence = timingConfidence,
                    .sourceIdentity = includeSourceIdentity
                                          ? std::optional{realFileIdentity(paths[index])}
                                          : std::nullopt,
                },
            .displayName = std::string{"Source "} + static_cast<char>('A' + index),
        });
    }
    auto validated = domain::ComparisonValidator::validate(std::move(sources));
    EXPECT_TRUE(validated);
    snapshot.validatedComparison =
        std::make_shared<const domain::ValidatedComparisonSet>(std::move(validated).value().set);
    snapshot.sources.clear();
    for (const auto& source : snapshot.validatedComparison->sources()) {
        snapshot.sources.push_back(application::SessionSourceView{
            .sourceId = source.id,
            .role = source.role,
            .displayName = source.displayName,
        });
    }
    snapshot.canonicalFrameCount =
        static_cast<std::uint64_t>(snapshot.validatedComparison->canonicalFrameCount());
    if (rate.has_value())
        snapshot.canonicalTimeline = *rate;
    return snapshot;
}

[[nodiscard]] application::SessionSnapshot
readySnapshotWithSources(const std::vector<std::filesystem::path>& paths,
                         const std::size_t referenceIndex = 0U) {
    const auto rate = domain::RationalRate::create(30, 1);
    EXPECT_TRUE(rate);
    return readySnapshotWithTiming(
        paths, referenceIndex, rate.value(), domain::TimingConfidence::kVerifiedCfr, 12);
}

class FakeBackend final {
public:
    application::SessionSnapshot currentSnapshot = emptySnapshot();
    application::PortSubmitResult submitResult = application::PortSubmitResult::Accepted;
    std::vector<application::PlaybackCommand> submitted;
    std::vector<application::CommandTerminal> terminals;
    std::vector<ReviewController::DecoderBackendState> decoderBackendStates;
    std::size_t submitCalls = 0U;
    std::size_t snapshotCalls = 0U;
    std::size_t drainCalls = 0U;
    std::thread::id lastAccessThread;
};

[[nodiscard]] ReviewController::Dependencies
dependenciesFor(const std::weak_ptr<FakeBackend>& weakBackend) {
    return ReviewController::Dependencies{
        .submit =
            [weakBackend](application::PlaybackCommand command) {
                const std::shared_ptr<FakeBackend> backend = weakBackend.lock();
                if (!backend) {
                    return application::PortSubmitResult::Closed;
                }
                ++backend->submitCalls;
                backend->lastAccessThread = std::this_thread::get_id();
                backend->submitted.push_back(std::move(command));
                return backend->submitResult;
            },
        .snapshot = [weakBackend]() -> std::shared_ptr<const application::SessionSnapshot> {
            const std::shared_ptr<FakeBackend> backend = weakBackend.lock();
            if (!backend) {
                return {};
            }
            ++backend->snapshotCalls;
            backend->lastAccessThread = std::this_thread::get_id();
            return std::make_shared<const application::SessionSnapshot>(backend->currentSnapshot);
        },
        .takeCompletedCommands =
            [weakBackend] {
                const std::shared_ptr<FakeBackend> backend = weakBackend.lock();
                if (!backend) {
                    return std::vector<application::CommandTerminal>{};
                }
                ++backend->drainCalls;
                backend->lastAccessThread = std::this_thread::get_id();
                std::vector<application::CommandTerminal> result = std::move(backend->terminals);
                backend->terminals.clear();
                return result;
            },
        .decoderBackendStates =
            [weakBackend] {
                const std::shared_ptr<FakeBackend> backend = weakBackend.lock();
                return backend ? backend->decoderBackendStates
                               : std::vector<ReviewController::DecoderBackendState>{};
            },
    };
}

[[nodiscard]] QString createFile(QTemporaryDir& directory, const QString& filename) {
    const QString path = directory.filePath(filename);
    QFile file{path};
    if (!file.open(QIODevice::WriteOnly) || file.write("fixture") != 7) {
        return {};
    }
    file.close();
    return path;
}

class ManualTaskQueue final {
public:
    void schedule(std::function<void()> task) {
        tasks_.push_back(std::move(task));
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return tasks_.size();
    }

    void runAt(const std::size_t index) {
        std::function<void()> task = std::move(tasks_.at(index));
        tasks_.erase(tasks_.begin() + static_cast<std::ptrdiff_t>(index));
        task();
    }

private:
    std::vector<std::function<void()>> tasks_;
};

void completeLastCommand(
    const std::shared_ptr<FakeBackend>& backend,
    const application::CommandOutcome outcome = application::CommandOutcome::Succeeded) {
    ASSERT_FALSE(backend->submitted.empty());
    backend->terminals.push_back(application::CommandTerminal{
        .context = application::commandContext(backend->submitted.back()),
        .outcome = outcome,
        .error = std::nullopt,
    });
}

class ReviewControllerTests : public testing::Test {
protected:
    void SetUp() override {
        ensureCoreApplication();
    }
};

TEST_F(ReviewControllerTests, ProjectsMediaTimeTimecodeAndDetailedSourceInformation) {
    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot =
        readySnapshotWithSources({"C:/media/reference.mp4", "C:/media/prediction.mp4"});
    backend->decoderBackendStates = {
        ReviewController::DecoderBackendState{
            .sourceId = 0U,
            .d3d11Va = false,
            .fallbackReason = "shared-device-unavailable",
        },
        ReviewController::DecoderBackendState{
            .sourceId = 1U,
            .d3d11Va = true,
        },
    };
    ReviewController controller{dependenciesFor(backend)};

    EXPECT_EQ(controller.currentMediaTime(), 66'667);
    EXPECT_EQ(controller.timecodeForFrame(2), QStringLiteral("00:00:00:02"));
    EXPECT_EQ(controller.mediaTimeForFrame(2), 66'667);
    EXPECT_EQ(controller.frameForMediaTime(66'667), 2);
    EXPECT_EQ(controller.rationalFrameRate(), QStringLiteral("30/1"));
    EXPECT_EQ(controller.timingMode(), QStringLiteral("Verified CFR"));
    EXPECT_FALSE(controller.dropFrameTimecodeAvailable());
    const QVariantList info = controller.sourceMediaInfo();
    ASSERT_EQ(info.size(), 2);
    const QVariantMap first = info.front().toMap();
    EXPECT_EQ(first.value(QStringLiteral("width")).toUInt(), 1'920U);
    EXPECT_EQ(first.value(QStringLiteral("height")).toUInt(), 1'080U);
    EXPECT_EQ(first.value(QStringLiteral("codec")).toString(), QStringLiteral("h264"));
    EXPECT_EQ(first.value(QStringLiteral("decodeBackend")).toString(), QStringLiteral("Software"));
    EXPECT_EQ(first.value(QStringLiteral("decodeFallbackReason")).toString(),
              QStringLiteral("shared-device-unavailable"));
    EXPECT_EQ(info[1].toMap().value(QStringLiteral("decodeBackend")).toString(),
              QStringLiteral("D3D11VA"));
}

TEST_F(ReviewControllerTests, FormatsFractionalDropFrameAndVariableRateMediaTime) {
    const auto ntscRate = domain::RationalRate::create(30'000, 1'001);
    ASSERT_TRUE(ntscRate);
    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot = readySnapshotWithTiming({"C:/media/ntsc.mp4"},
                                                       0U,
                                                       ntscRate.value(),
                                                       domain::TimingConfidence::kVerifiedCfr,
                                                       20'000);
    ReviewController controller{dependenciesFor(backend)};

    EXPECT_TRUE(controller.dropFrameTimecodeAvailable());
    EXPECT_EQ(controller.timecodeForFrame(1'800, false), QStringLiteral("00:01:00:00 NDF"));
    EXPECT_EQ(controller.timecodeForFrame(1'800, true), QStringLiteral("00:01:00;02 DF"));

    auto variable = domain::FrameTimeline::create(
        {domain::MediaTime{0}, domain::MediaTime{41'000}, domain::MediaTime{83'000}});
    ASSERT_TRUE(variable);
    backend->currentSnapshot = readySnapshotWithTiming(
        {"C:/media/vfr.mp4"}, 0U, std::nullopt, domain::TimingConfidence::kVariableFrameRate, 3);
    backend->currentSnapshot.canonicalTimeline =
        std::make_shared<const domain::FrameTimeline>(std::move(variable).value());
    backend->currentSnapshot.displayedFrame = domain::FrameId{2};
    controller.refreshProjection();

    EXPECT_EQ(controller.timingMode(), QStringLiteral("VFR"));
    EXPECT_TRUE(controller.rationalFrameRate().isEmpty());
    EXPECT_EQ(controller.timecodeForFrame(2), QStringLiteral("00:00:00.083 · Frame 3"));
    EXPECT_EQ(controller.mediaTimeForFrame(2), 83'000);
    EXPECT_EQ(controller.frameForMediaTime(82'999), 1);
}

TEST_F(ReviewControllerTests, ReviewsDroppedFilesInCppAndNormalizesUnicodePaths) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourceAPath = createFile(directory, QStringLiteral("source_\u7532.mp4"));
    const QString sourceBPath = createFile(directory, QStringLiteral("source_\u4e59.custom"));
    ASSERT_FALSE(sourceAPath.isEmpty());
    ASSERT_FALSE(sourceBPath.isEmpty());

    auto backend = std::make_shared<FakeBackend>();
    ReviewController controller{dependenciesFor(backend)};
    const QVariantMap result = controller.handleDroppedUrls(
        {QUrl::fromLocalFile(sourceAPath), QUrl::fromLocalFile(sourceBPath)});

    ASSERT_TRUE(result.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(result.value(QStringLiteral("kind")).toString(), QStringLiteral("videos"));
    const QVariantList urls = result.value(QStringLiteral("urls")).toList();
    ASSERT_EQ(urls.size(), 2);
    EXPECT_EQ(urls.front().toUrl().toLocalFile(), QFileInfo{sourceAPath}.canonicalFilePath());
    EXPECT_EQ(urls.back().toUrl().toLocalFile(), QFileInfo{sourceBPath}.canonicalFilePath());
}

TEST_F(ReviewControllerTests, RejectsDuplicateAndMissingDroppedFiles) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = createFile(directory, QStringLiteral("source.mp4"));
    ASSERT_FALSE(sourcePath.isEmpty());

    auto backend = std::make_shared<FakeBackend>();
    ReviewController controller{dependenciesFor(backend)};
    const QUrl sourceUrl = QUrl::fromLocalFile(sourcePath);

    const QVariantMap duplicate = controller.handleDroppedUrls({sourceUrl, sourceUrl});
    EXPECT_FALSE(duplicate.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(duplicate.value(QStringLiteral("errorKey")).toString(),
              QStringLiteral("drop-duplicate"));

    const QVariantMap missing = controller.handleDroppedUrls(
        {QUrl::fromLocalFile(directory.filePath(QStringLiteral("missing.mp4")))});
    EXPECT_FALSE(missing.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(missing.value(QStringLiteral("errorKey")).toString(), QStringLiteral("drop-missing"));
}

TEST_F(ReviewControllerTests, CanonicalizesUnicodeLocalFilesAndDispatchesScopedOpenCommand) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourceAPath = createFile(directory, QStringLiteral("source_\u7532.mp4"));
    const QString sourceBPath = createFile(directory, QStringLiteral("source_\u4e59.mp4"));
    ASSERT_FALSE(sourceAPath.isEmpty());
    ASSERT_FALSE(sourceBPath.isEmpty());

    auto backend = std::make_shared<FakeBackend>();
    ReviewController controller{dependenciesFor(backend)};
    std::thread::id notificationThread;
    QObject::connect(&controller, &ReviewController::stateChanged, &controller, [&] {
        notificationThread = std::this_thread::get_id();
    });

    ASSERT_TRUE(controller.openComparison(QUrl::fromLocalFile(sourceAPath),
                                          QUrl::fromLocalFile(sourceBPath)));
    ASSERT_EQ(backend->submitted.size(), 1U);
    const auto* const command =
        std::get_if<application::OpenComparisonCommand>(&backend->submitted.front());
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->context.sessionId, domain::SessionId{17U});
    EXPECT_EQ(command->context.sessionEpoch, domain::SessionEpoch{3U});
    EXPECT_EQ(command->context.commandId, domain::CommandId{1U});
    ASSERT_EQ(command->sources.size(), 2U);
    EXPECT_TRUE(command->sources[0].path ==
                std::filesystem::path{QFileInfo{sourceAPath}.canonicalFilePath().toStdWString()});
    EXPECT_TRUE(command->sources[1].path ==
                std::filesystem::path{QFileInfo{sourceBPath}.canonicalFilePath().toStdWString()});
    EXPECT_EQ(command->intent, application::OpenReviewIntent::NewReview);
    EXPECT_TRUE(controller.sourceAFilename().isEmpty());
    EXPECT_TRUE(controller.sourceBFilename().isEmpty());
    EXPECT_TRUE(controller.busy());
    EXPECT_FALSE(controller.canOpen());
    EXPECT_EQ(backend->lastAccessThread, std::this_thread::get_id());
    EXPECT_EQ(notificationThread, std::this_thread::get_id());
    controller.stop();
}

TEST_F(ReviewControllerTests, DispatchesThreeSourcesWithTheSelectedReferenceRole) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourceAPath = createFile(directory, QStringLiteral("reference.mp4"));
    const QString sourceBPath = createFile(directory, QStringLiteral("prediction_1.mp4"));
    const QString sourceCPath = createFile(directory, QStringLiteral("prediction_2.mp4"));
    ASSERT_FALSE(sourceAPath.isEmpty());
    ASSERT_FALSE(sourceBPath.isEmpty());
    ASSERT_FALSE(sourceCPath.isEmpty());

    auto backend = std::make_shared<FakeBackend>();
    ReviewController controller{dependenciesFor(backend)};
    ASSERT_TRUE(controller.openComparisonSet(QUrl::fromLocalFile(sourceAPath),
                                             QUrl::fromLocalFile(sourceBPath),
                                             QUrl::fromLocalFile(sourceCPath),
                                             0));

    ASSERT_EQ(backend->submitted.size(), 1U);
    const auto* const command =
        std::get_if<application::OpenComparisonCommand>(&backend->submitted.front());
    ASSERT_NE(command, nullptr);
    ASSERT_EQ(command->sources.size(), 3U);
    EXPECT_EQ(command->sources[0].role, domain::ComparisonRole::kReference);
    EXPECT_EQ(command->sources[1].role, domain::ComparisonRole::kPrediction);
    EXPECT_EQ(command->sources[2].role, domain::ComparisonRole::kPrediction);
    EXPECT_TRUE(controller.sourceCFilename().isEmpty());
    EXPECT_TRUE(controller.sourceCErrorKey().isEmpty());
    controller.stop();
}

TEST_F(ReviewControllerTests, ReopenSourcesRequestsMediaTimePreservingSessionRebuild) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourceAPath = createFile(directory, QStringLiteral("reference.mp4"));
    const QString sourceBPath = createFile(directory, QStringLiteral("prediction.mp4"));
    ASSERT_FALSE(sourceAPath.isEmpty());
    ASSERT_FALSE(sourceBPath.isEmpty());

    auto backend = std::make_shared<FakeBackend>();
    ReviewController controller{dependenciesFor(backend)};
    ASSERT_TRUE(controller.reopenSources(
        {QUrl::fromLocalFile(sourceAPath), QUrl::fromLocalFile(sourceBPath)}, 0));

    ASSERT_EQ(backend->submitted.size(), 1U);
    const auto* const command =
        std::get_if<application::OpenComparisonCommand>(&backend->submitted.front());
    ASSERT_NE(command, nullptr);
    EXPECT_TRUE(command->preserveDisplayedTime);
    EXPECT_EQ(command->intent, application::OpenReviewIntent::ReplaceSources);
    controller.stop();
}

TEST_F(ReviewControllerTests, FailedCandidateOpenLeavesActiveSourcesAndReferenceUnchanged) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourceAPath = createFile(directory, QStringLiteral("active_a.mp4"));
    const QString sourceBPath = createFile(directory, QStringLiteral("active_b.mp4"));
    ASSERT_FALSE(sourceAPath.isEmpty());
    ASSERT_FALSE(sourceBPath.isEmpty());

    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot =
        readySnapshotWithSources({std::filesystem::path{sourceAPath.toStdWString()},
                                  std::filesystem::path{sourceBPath.toStdWString()}},
                                 1U);
    ReviewController controller{dependenciesFor(backend)};

    ASSERT_EQ(controller.sourceCount(), 2);
    ASSERT_EQ(controller.canonicalSourceIndex(), 1);
    const QVariantList activeSources = controller.sourceUrls();
    ASSERT_EQ(activeSources.size(), 2);

    EXPECT_FALSE(controller.openSources(
        {QUrl::fromLocalFile(sourceAPath),
         QUrl::fromLocalFile(directory.filePath(QStringLiteral("missing.mp4")))},
        0));
    EXPECT_EQ(controller.sourceUrls(), activeSources);
    EXPECT_EQ(controller.sourceCount(), 2);
    EXPECT_EQ(controller.canonicalSourceIndex(), 1);
    EXPECT_EQ(controller.sourceBErrorKey(), QStringLiteral("source-missing"));
    EXPECT_TRUE(backend->submitted.empty());
    controller.stop();
}

TEST_F(ReviewControllerTests, ChangeReferenceRebuildsTheCanonicalTimelineFromActiveSources) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourceAPath = createFile(directory, QStringLiteral("active_a.mp4"));
    const QString sourceBPath = createFile(directory, QStringLiteral("active_b.mp4"));
    ASSERT_FALSE(sourceAPath.isEmpty());
    ASSERT_FALSE(sourceBPath.isEmpty());

    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot =
        readySnapshotWithSources({std::filesystem::path{sourceAPath.toStdWString()},
                                  std::filesystem::path{sourceBPath.toStdWString()}});
    ReviewController controller{dependenciesFor(backend)};

    ASSERT_TRUE(controller.changeReference(1));
    ASSERT_EQ(backend->submitted.size(), 1U);
    const auto* command =
        std::get_if<application::OpenComparisonCommand>(&backend->submitted.front());
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->intent, application::OpenReviewIntent::ChangeReference);
    EXPECT_TRUE(command->preserveDisplayedTime);
    ASSERT_EQ(command->sources.size(), 2U);
    EXPECT_EQ(command->sources[0].role, domain::ComparisonRole::kPrediction);
    EXPECT_EQ(command->sources[1].role, domain::ComparisonRole::kReference);
    EXPECT_EQ(controller.canonicalSourceIndex(), 0);
    controller.stop();
}

TEST_F(ReviewControllerTests, ShellKeepsActiveAndStagedSourcesSeparateDuringARebuild) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourceAPath = createFile(directory, QStringLiteral("active_a.mp4"));
    const QString sourceBPath = createFile(directory, QStringLiteral("active_b.mp4"));
    const QString sourceCPath = createFile(directory, QStringLiteral("candidate_c.mp4"));
    ASSERT_FALSE(sourceAPath.isEmpty());
    ASSERT_FALSE(sourceBPath.isEmpty());
    ASSERT_FALSE(sourceCPath.isEmpty());

    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot =
        readySnapshotWithSources({std::filesystem::path{sourceAPath.toStdWString()},
                                  std::filesystem::path{sourceBPath.toStdWString()}});
    ReviewController controller{dependenciesFor(backend)};
    ReviewShellController shell{controller};

    ASSERT_EQ(shell.activeSources().size(), 2);
    ASSERT_EQ(shell.stagedSources(), shell.activeSources());
    const QVariantList staged{
        QUrl::fromLocalFile(sourceAPath),
        QUrl::fromLocalFile(sourceBPath),
        QUrl::fromLocalFile(sourceCPath),
    };
    ASSERT_TRUE(shell.stageSources(staged, 2));
    EXPECT_EQ(shell.activeSources().size(), 2);
    EXPECT_EQ(shell.stagedSources().size(), 3);
    EXPECT_EQ(shell.stagedReferenceIndex(), 2);

    ASSERT_TRUE(shell.openStagedSources(true));
    ASSERT_EQ(backend->submitted.size(), 1U);
    const auto* command =
        std::get_if<application::OpenComparisonCommand>(&backend->submitted.front());
    ASSERT_NE(command, nullptr);

    backend->currentSnapshot = emptySnapshot();
    backend->currentSnapshot.sessionState = domain::SessionState::kLoading;
    controller.refreshProjection();
    EXPECT_TRUE(controller.busy());
    EXPECT_EQ(shell.activeSources().size(), 2);

    backend->currentSnapshot =
        readySnapshotWithSources({std::filesystem::path{sourceAPath.toStdWString()},
                                  std::filesystem::path{sourceBPath.toStdWString()},
                                  std::filesystem::path{sourceCPath.toStdWString()}},
                                 2U);
    backend->terminals.push_back(application::CommandTerminal{
        .context = command->context,
        .outcome = application::CommandOutcome::Succeeded,
    });
    controller.refreshProjection();
    EXPECT_FALSE(controller.busy());
    EXPECT_EQ(shell.activeSources().size(), 3);
    EXPECT_EQ(shell.canonicalSourceIndex(), 2);
    controller.stop();
}

TEST_F(ReviewControllerTests, ShellRejectsPredictionOnlyReferenceIndex) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourceAPath = createFile(directory, QStringLiteral("prediction_a.mp4"));
    const QString sourceBPath = createFile(directory, QStringLiteral("prediction_b.mp4"));
    ASSERT_FALSE(sourceAPath.isEmpty());
    ASSERT_FALSE(sourceBPath.isEmpty());

    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot =
        readySnapshotWithSources({std::filesystem::path{sourceAPath.toStdWString()},
                                  std::filesystem::path{sourceBPath.toStdWString()}});
    ReviewController controller{dependenciesFor(backend)};
    ReviewShellController shell{controller};

    EXPECT_FALSE(shell.stageSources(
        {QUrl::fromLocalFile(sourceAPath), QUrl::fromLocalFile(sourceBPath)}, -1));
    EXPECT_TRUE(backend->submitted.empty());
    controller.stop();
}

TEST_F(ReviewControllerTests, ShellCanRemoveAnyActiveSourceAndPreservesTheReferenceRole) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourceAPath = createFile(directory, QStringLiteral("active_a.mp4"));
    const QString sourceBPath = createFile(directory, QStringLiteral("active_b.mp4"));
    ASSERT_FALSE(sourceAPath.isEmpty());
    ASSERT_FALSE(sourceBPath.isEmpty());

    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot =
        readySnapshotWithSources({std::filesystem::path{sourceAPath.toStdWString()},
                                  std::filesystem::path{sourceBPath.toStdWString()}},
                                 1U);
    ReviewController controller{dependenciesFor(backend)};
    ReviewShellController shell{controller};

    ASSERT_TRUE(shell.removeActiveSource(0));
    ASSERT_EQ(backend->submitted.size(), 1U);
    const auto* command =
        std::get_if<application::OpenComparisonCommand>(&backend->submitted.front());
    ASSERT_NE(command, nullptr);
    ASSERT_EQ(command->sources.size(), 1U);
    EXPECT_EQ(command->sources.front().role, domain::ComparisonRole::kReference);
    EXPECT_EQ(command->intent, application::OpenReviewIntent::ReplaceSources);
    EXPECT_TRUE(command->preserveDisplayedTime);
    EXPECT_EQ(shell.openIntent(), ReviewShellController::ReplaceSources);
    controller.stop();
}

TEST_F(ReviewControllerTests, ControllerClosesTheActiveSessionWithoutAWorkspaceBridge) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = createFile(directory, QStringLiteral("active.mp4"));
    ASSERT_FALSE(sourcePath.isEmpty());

    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot =
        readySnapshotWithSources({std::filesystem::path{sourcePath.toStdWString()}});
    ReviewController controller{dependenciesFor(backend)};

    ASSERT_TRUE(controller.closeSources());
    ASSERT_EQ(backend->submitted.size(), 1U);
    EXPECT_NE(std::get_if<application::CloseSessionCommand>(&backend->submitted.front()), nullptr);
    controller.stop();
}

TEST_F(ReviewControllerTests, ShellRoutesAndSerializesStartupRequestsThroughTheIntentQueue) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString firstPath = createFile(directory, QStringLiteral("startup_a.mp4"));
    const QString secondPath = createFile(directory, QStringLiteral("startup_b.mp4"));
    auto backend = std::make_shared<FakeBackend>();
    ReviewController controller{dependenciesFor(backend)};
    ReviewShellController shell{controller};
    const QVariantList first{QUrl::fromLocalFile(firstPath)};
    const QVariantList second{QUrl::fromLocalFile(secondPath)};

    ASSERT_TRUE(shell.enqueueStartupRequest(2, first));
    EXPECT_EQ(shell.activeIntent().value(QStringLiteral("origin")).toInt(),
              ReviewShellController::StartupOrigin);
    ASSERT_EQ(backend->submitted.size(), 1U);
    const application::CommandContext firstContext =
        application::commandContext(backend->submitted.front());

    ASSERT_TRUE(shell.enqueueStartupRequest(2, second));
    EXPECT_EQ(shell.queuedIntentCount(), 1);
    EXPECT_EQ(shell.queuedIntents().front().toMap().value(QStringLiteral("origin")).toInt(),
              ReviewShellController::StartupOrigin);

    backend->currentSnapshot =
        readySnapshotWithSources({std::filesystem::path{firstPath.toStdWString()}});
    backend->terminals.push_back(application::CommandTerminal{
        .context = firstContext,
        .outcome = application::CommandOutcome::Succeeded,
    });
    controller.refreshProjection();
    ASSERT_TRUE(waitUntil([&backend] { return backend->submitted.size() == 2U; }));
    EXPECT_EQ(shell.activeIntent().value(QStringLiteral("origin")).toInt(),
              ReviewShellController::StartupOrigin);
    EXPECT_EQ(shell.queuedIntentCount(), 0);
    const auto* secondCommand =
        std::get_if<application::OpenComparisonCommand>(&backend->submitted.back());
    ASSERT_NE(secondCommand, nullptr);
    ASSERT_EQ(secondCommand->sources.size(), 1U);
    EXPECT_EQ(secondCommand->sources.front().path,
              std::filesystem::path{secondPath.toStdWString()});
    controller.stop();
}

TEST_F(ReviewControllerTests, ShellPrioritizesCloseAndCancelsOlderQueuedIntents) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourceAPath = createFile(directory, QStringLiteral("queued_a.mp4"));
    const QString sourceBPath = createFile(directory, QStringLiteral("queued_b.mp4"));
    ASSERT_FALSE(sourceAPath.isEmpty());
    ASSERT_FALSE(sourceBPath.isEmpty());

    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot =
        readySnapshotWithSources({std::filesystem::path{sourceAPath.toStdWString()}});
    ReviewController controller{dependenciesFor(backend)};
    ReviewShellController shell{controller};
    ASSERT_TRUE(controller.openSources(
        {QUrl::fromLocalFile(sourceAPath), QUrl::fromLocalFile(sourceBPath)}, 0));
    ASSERT_TRUE(controller.busy());
    ASSERT_EQ(backend->submitted.size(), 1U);
    const application::CommandContext pendingContext =
        application::commandContext(backend->submitted.front());

    ASSERT_TRUE(shell.stageSources(
        {QUrl::fromLocalFile(sourceAPath), QUrl::fromLocalFile(sourceBPath)}, 0));
    EXPECT_TRUE(shell.openStagedSources(false));
    EXPECT_TRUE(shell.closeSources());
    ASSERT_EQ(shell.queuedIntentCount(), 1);
    EXPECT_EQ(shell.queuedIntents().front().toMap().value(QStringLiteral("kind")).toInt(),
              ReviewShellController::CloseSourcesIntent);
    EXPECT_EQ(backend->submitted.size(), 1U);

    backend->currentSnapshot =
        readySnapshotWithSources({std::filesystem::path{sourceAPath.toStdWString()},
                                  std::filesystem::path{sourceBPath.toStdWString()}});
    backend->terminals.push_back(application::CommandTerminal{
        .context = pendingContext,
        .outcome = application::CommandOutcome::Succeeded,
    });
    controller.refreshProjection();
    ASSERT_TRUE(waitUntil([&backend] { return backend->submitted.size() == 2U; }));
    EXPECT_NE(std::get_if<application::CloseSessionCommand>(&backend->submitted.back()), nullptr);
    EXPECT_EQ(shell.queuedIntentCount(), 0);
    controller.stop();
}

TEST_F(ReviewControllerTests, ShellRebasesQueuedAddBySourceIdentityAfterGenerationChange) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourceA = createFile(directory, QStringLiteral("a.mp4"));
    const QString sourceB = createFile(directory, QStringLiteral("b.mp4"));
    const QString sourceC = createFile(directory, QStringLiteral("c.mp4"));
    const QString sourceD = createFile(directory, QStringLiteral("d.mp4"));
    const QString sourceE = createFile(directory, QStringLiteral("e.mp4"));
    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot =
        readySnapshotWithSources({std::filesystem::path{sourceA.toStdWString()},
                                  std::filesystem::path{sourceB.toStdWString()}});
    ReviewController controller{dependenciesFor(backend)};
    ReviewShellController shell{controller};

    ASSERT_TRUE(
        shell.stageSources({QUrl::fromLocalFile(sourceC), QUrl::fromLocalFile(sourceD)}, 0));
    ASSERT_TRUE(shell.openStagedSources(false));
    ASSERT_EQ(backend->submitted.size(), 1U);
    const application::CommandContext openContext =
        application::commandContext(backend->submitted.front());

    ASSERT_TRUE(shell.stageSources(
        {QUrl::fromLocalFile(sourceA), QUrl::fromLocalFile(sourceB), QUrl::fromLocalFile(sourceE)},
        0));
    ASSERT_TRUE(shell.openStagedSources(true));
    ASSERT_EQ(shell.queuedIntentCount(), 1);

    backend->currentSnapshot =
        readySnapshotWithSources({std::filesystem::path{sourceC.toStdWString()},
                                  std::filesystem::path{sourceD.toStdWString()}});
    backend->terminals.push_back(application::CommandTerminal{
        .context = openContext,
        .outcome = application::CommandOutcome::Succeeded,
    });
    controller.refreshProjection();
    ASSERT_TRUE(waitUntil([&backend] { return backend->submitted.size() == 2U; }));
    const auto* rebased =
        std::get_if<application::OpenComparisonCommand>(&backend->submitted.back());
    ASSERT_NE(rebased, nullptr);
    ASSERT_EQ(rebased->sources.size(), 3U);
    EXPECT_EQ(rebased->sources[0U].path, std::filesystem::path{sourceC.toStdWString()});
    EXPECT_EQ(rebased->sources[1U].path, std::filesystem::path{sourceD.toStdWString()});
    EXPECT_EQ(rebased->sources[2U].path, std::filesystem::path{sourceE.toStdWString()});
    EXPECT_EQ(shell.activeGeneration(), 1U);
    controller.stop();
}

TEST_F(ReviewControllerTests, ShellBoundsExposesAndCancelsQueuedIntents) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourceA = createFile(directory, QStringLiteral("a.mp4"));
    const QString sourceB = createFile(directory, QStringLiteral("b.mp4"));
    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot =
        readySnapshotWithSources({std::filesystem::path{sourceA.toStdWString()},
                                  std::filesystem::path{sourceB.toStdWString()}});
    ReviewController controller{dependenciesFor(backend)};
    ReviewShellController shell{controller};

    ASSERT_TRUE(
        controller.openSources({QUrl::fromLocalFile(sourceA), QUrl::fromLocalFile(sourceB)}, 0));
    for (int index = 0; index < 8; ++index) {
        EXPECT_TRUE(shell.removeActiveSource(index % 2));
    }
    EXPECT_FALSE(shell.removeActiveSource(0));
    ASSERT_EQ(shell.queuedIntents().size(), 8);
    const qulonglong firstId =
        shell.queuedIntents().front().toMap().value(QStringLiteral("id")).toULongLong();
    EXPECT_TRUE(shell.cancelQueuedIntent(firstId));
    EXPECT_EQ(shell.queuedIntentCount(), 7);
    shell.cancelAllQueuedIntents();
    EXPECT_EQ(shell.queuedIntentCount(), 0);
    controller.stop();
}

TEST_F(ReviewControllerTests, ShellUsesSourceIdentityForOperationsAndRollsBackFailedRemoval) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourceA = createFile(directory, QStringLiteral("identity_a.mp4"));
    const QString sourceB = createFile(directory, QStringLiteral("identity_b.mp4"));
    ASSERT_FALSE(sourceA.isEmpty());
    ASSERT_FALSE(sourceB.isEmpty());

    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot =
        readySnapshotWithSources({std::filesystem::path{sourceA.toStdWString()},
                                  std::filesystem::path{sourceB.toStdWString()}});
    ReviewController controller{dependenciesFor(backend)};
    ReviewShellController shell{controller};
    const QString sourceBIdentity = canonicalSourceIdentity(QUrl::fromLocalFile(sourceB));

    ASSERT_EQ(shell.activeSourceIdentities().size(), 2);
    EXPECT_EQ(shell.activeSourceIdentities().at(1), sourceBIdentity);
    ASSERT_TRUE(shell.removeActiveSourceByIdentity(sourceBIdentity));
    ASSERT_EQ(backend->submitted.size(), 1U);
    EXPECT_EQ(shell.pendingSourceIdentities(), QStringList{sourceBIdentity});

    // A repeated click is accepted as the existing operation, rather than queueing another
    // topology mutation against the same source revision.
    EXPECT_TRUE(shell.removeActiveSourceByIdentity(sourceBIdentity));
    EXPECT_EQ(backend->submitted.size(), 1U);

    completeLastCommand(backend, application::CommandOutcome::Failed);
    controller.refreshProjection();
    EXPECT_TRUE(shell.pendingSourceIdentities().isEmpty());
    EXPECT_EQ(shell.stagedSources(), shell.activeSources());

    ASSERT_TRUE(shell.changeReferenceByIdentity(sourceBIdentity));
    ASSERT_EQ(backend->submitted.size(), 2U);
    EXPECT_EQ(shell.pendingSourceIdentities(), QStringList{sourceBIdentity});
    EXPECT_TRUE(shell.changeReferenceByIdentity(sourceBIdentity));
    EXPECT_EQ(backend->submitted.size(), 2U);
    controller.stop();
}

TEST_F(ReviewControllerTests, FrozenIdentityStaysStableWhenFileChangesOnDisk) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourceA = createFile(directory, QStringLiteral("frozen_a.mp4"));
    const QString sourceB = createFile(directory, QStringLiteral("frozen_b.mp4"));
    ASSERT_FALSE(sourceA.isEmpty());
    ASSERT_FALSE(sourceB.isEmpty());

    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot =
        readySnapshotWithSources({std::filesystem::path{sourceA.toStdWString()},
                                  std::filesystem::path{sourceB.toStdWString()}});
    ReviewController controller{dependenciesFor(backend)};
    ReviewShellController shell{controller};

    ASSERT_EQ(shell.activeSourceIdentities().size(), 2);
    const QStringList frozenIdentities = shell.activeSourceIdentities();
    const QString frozenA = frozenIdentities.at(0);
    const QString frozenB = frozenIdentities.at(1);
    EXPECT_FALSE(frozenA.isEmpty());
    EXPECT_FALSE(frozenB.isEmpty());

    const auto* model = controller.sources();
    ASSERT_NE(model, nullptr);
    ASSERT_EQ(model->rowCount(), 2);
    EXPECT_EQ(model->data(model->index(0, 0), SourceListModel::ChangedOnDiskRole).toBool(), false);
    EXPECT_EQ(model->data(model->index(1, 0), SourceListModel::ChangedOnDiskRole).toBool(), false);

    QFile fileB{sourceB};
    ASSERT_TRUE(fileB.open(QIODevice::Append));
    ASSERT_EQ(fileB.write("-appended"), 9);
    fileB.close();

    // File metadata checks run asynchronously and are throttled off the projection path.
    ASSERT_TRUE(waitUntil([&controller, model] {
        controller.refreshProjection();
        return model->data(model->index(1, 0), SourceListModel::ChangedOnDiskRole).toBool();
    }));

    EXPECT_EQ(shell.activeSourceIdentities().at(0), frozenA);
    EXPECT_EQ(shell.activeSourceIdentities().at(1), frozenB);
    EXPECT_EQ(model->data(model->index(0, 0), SourceListModel::SourceIdentityRole).toString(),
              frozenA);
    EXPECT_EQ(model->data(model->index(1, 0), SourceListModel::SourceIdentityRole).toString(),
              frozenB);
    EXPECT_EQ(model->data(model->index(0, 0), SourceListModel::ChangedOnDiskRole).toBool(), false);
    EXPECT_EQ(model->data(model->index(1, 0), SourceListModel::ChangedOnDiskRole).toBool(), true);

    controller.stop();
}

TEST_F(ReviewControllerTests, MissingDescriptorIdentityUsesOnePathOnlyFrozenIdentity) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourceA = createFile(directory, QStringLiteral("missing_identity_a.mp4"));
    const QString sourceB = createFile(directory, QStringLiteral("missing_identity_b.mp4"));
    ASSERT_FALSE(sourceA.isEmpty());
    ASSERT_FALSE(sourceB.isEmpty());

    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate);
    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot =
        readySnapshotWithTiming({std::filesystem::path{sourceA.toStdWString()},
                                 std::filesystem::path{sourceB.toStdWString()}},
                                0U,
                                rate.value(),
                                domain::TimingConfidence::kVerifiedCfr,
                                12,
                                false);
    ManualTaskQueue tasks;
    auto dependencies = dependenciesFor(backend);
    dependencies.eventDriven = true;
    dependencies.scheduleBackgroundTask =
        [&tasks](ReviewController::Dependencies::BackgroundTask task) {
            tasks.schedule(std::move(task));
        };
    ReviewController controller{std::move(dependencies)};
    ReviewShellController shell{controller};

    const QString expectedIdentity = QDir::cleanPath(sourceB).toCaseFolded();
    ASSERT_EQ(shell.activeSourceIdentities().size(), 2);
    EXPECT_EQ(shell.activeSourceIdentities().at(1), expectedIdentity);
    const auto* model = controller.sources();
    ASSERT_NE(model, nullptr);
    EXPECT_EQ(model->data(model->index(1, 0), SourceListModel::SourceIdentityRole).toString(),
              expectedIdentity);
    EXPECT_TRUE(shell.removeActiveSourceByIdentity(expectedIdentity));
    EXPECT_EQ(backend->submitted.size(), 1U);
    controller.stop();
}

TEST_F(ReviewControllerTests, LateDiskStatusFromPreviousComparisonCannotOverwriteReplacement) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourceA = createFile(directory, QStringLiteral("generation_a.mp4"));
    const QString sourceB = createFile(directory, QStringLiteral("generation_b.mp4"));
    ASSERT_FALSE(sourceA.isEmpty());
    ASSERT_FALSE(sourceB.isEmpty());
    const std::vector<std::filesystem::path> paths{
        std::filesystem::path{sourceA.toStdWString()},
        std::filesystem::path{sourceB.toStdWString()},
    };

    application::SessionSnapshot replacement = readySnapshotWithSources(paths);
    QFile changedFile{sourceB};
    ASSERT_TRUE(changedFile.open(QIODevice::Append));
    ASSERT_EQ(changedFile.write("-changed"), 8);
    changedFile.close();
    application::SessionSnapshot original = readySnapshotWithSources(paths);

    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot = std::move(original);
    ManualTaskQueue tasks;
    auto dependencies = dependenciesFor(backend);
    dependencies.eventDriven = true;
    dependencies.scheduleBackgroundTask =
        [&tasks](ReviewController::Dependencies::BackgroundTask task) {
            tasks.schedule(std::move(task));
        };
    ReviewController controller{std::move(dependencies)};
    const auto* model = controller.sources();
    ASSERT_NE(model, nullptr);
    ASSERT_EQ(tasks.size(), 1U);

    replacement.sessionEpoch = domain::SessionEpoch{4U};
    backend->currentSnapshot = std::move(replacement);
    controller.refreshProjection();
    ASSERT_EQ(tasks.size(), 2U);

    tasks.runAt(1U);
    ASSERT_TRUE(waitUntil([model] {
        return model->data(model->index(1, 0), SourceListModel::ChangedOnDiskRole).toBool();
    }));

    tasks.runAt(0U);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    EXPECT_TRUE(model->data(model->index(1, 0), SourceListModel::ChangedOnDiskRole).toBool());
    controller.stop();
}

TEST_F(ReviewControllerTests, QueuedDiskStatusTaskMayFinishAfterControllerDestruction) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourceA = createFile(directory, QStringLiteral("destroy_a.mp4"));
    const QString sourceB = createFile(directory, QStringLiteral("destroy_b.mp4"));
    ASSERT_FALSE(sourceA.isEmpty());
    ASSERT_FALSE(sourceB.isEmpty());

    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot =
        readySnapshotWithSources({std::filesystem::path{sourceA.toStdWString()},
                                  std::filesystem::path{sourceB.toStdWString()}});
    ManualTaskQueue tasks;
    auto dependencies = dependenciesFor(backend);
    dependencies.eventDriven = true;
    dependencies.scheduleBackgroundTask =
        [&tasks](ReviewController::Dependencies::BackgroundTask task) {
            tasks.schedule(std::move(task));
        };
    auto controller = std::make_unique<ReviewController>(std::move(dependencies));
    ASSERT_EQ(tasks.size(), 1U);
    EXPECT_FALSE(controller->waitForSourceDiskStatusIdle(0ms));

    QPointer<ReviewController> guarded{controller.get()};
    controller.reset();
    ASSERT_TRUE(guarded.isNull());
    tasks.runAt(0U);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    SUCCEED();
}

TEST_F(ReviewControllerTests, EventDrivenDiskStatusRetriesAndContinuesWhileIdle) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourceA = createFile(directory, QStringLiteral("event_driven_a.mp4"));
    const QString sourceB = createFile(directory, QStringLiteral("event_driven_b.mp4"));
    ASSERT_FALSE(sourceA.isEmpty());
    ASSERT_FALSE(sourceB.isEmpty());

    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot =
        readySnapshotWithSources({std::filesystem::path{sourceA.toStdWString()},
                                  std::filesystem::path{sourceB.toStdWString()}});
    ManualTaskQueue tasks;
    std::size_t probeCalls = 0U;
    auto dependencies = dependenciesFor(backend);
    dependencies.eventDriven = true;
    dependencies.scheduleBackgroundTask =
        [&tasks](ReviewController::Dependencies::BackgroundTask task) {
            tasks.schedule(std::move(task));
        };
    dependencies.sourceFileMetadataProbe =
        [&probeCalls](const QString&) -> ReviewController::SourceFileMetadata {
        ++probeCalls;
        if (probeCalls == 1U) {
            throw std::runtime_error{"injected metadata failure"};
        }
        return ReviewController::SourceFileMetadata{.exists = false};
    };
    ReviewController controller{std::move(dependencies)};
    const auto* model = controller.sources();
    ASSERT_NE(model, nullptr);
    ASSERT_EQ(tasks.size(), 1U);

    tasks.runAt(0U);
    ASSERT_TRUE(waitUntil([&tasks] { return tasks.size() == 1U; }));
    tasks.runAt(0U);
    ASSERT_TRUE(waitUntil([model] {
        return model->data(model->index(0, 0), SourceListModel::ChangedOnDiskRole).toBool();
    }));

    ASSERT_TRUE(waitUntil([&tasks] { return tasks.size() == 1U; }));
    EXPECT_GE(probeCalls, 2U);
    controller.stop();
    EXPECT_FALSE(controller.waitForSourceDiskStatusIdle(0ms));
    tasks.runAt(0U);
    EXPECT_TRUE(controller.waitForSourceDiskStatusIdle(100ms));
}

TEST_F(ReviewControllerTests, ShellOwnsChromeInspectorAndPendingActionState) {
    auto backend = std::make_shared<FakeBackend>();
    ReviewController controller{dependenciesFor(backend)};
    ReviewShellController shell{controller};

    shell.setInspectorVisible(true);
    EXPECT_TRUE(shell.inspectorVisible());
    shell.setChromeVisible(false);
    EXPECT_FALSE(shell.chromeVisible());
    EXPECT_FALSE(shell.inspectorVisible());
    shell.setInspectorVisible(true);
    EXPECT_FALSE(shell.inspectorVisible());
    shell.setChromeVisible(true);

    const QVariantMap action{
        {QStringLiteral("kind"), QStringLiteral("openVideos")},
        {QStringLiteral("urls"),
         QVariantList{QUrl::fromLocalFile(QStringLiteral("C:/media/a.mp4"))}},
    };
    ASSERT_TRUE(shell.beginPendingAction(action));
    EXPECT_TRUE(shell.hasPendingAction());
    EXPECT_EQ(shell.pendingAction(), action);
    EXPECT_EQ(shell.takePendingAction(), action);
    EXPECT_FALSE(shell.hasPendingAction());
    EXPECT_TRUE(shell.takePendingAction().isEmpty());
    controller.stop();
}

TEST_F(ReviewControllerTests, ShellOwnsRangeStateAndPreservesItsOrderingInvariants) {
    auto backend = std::make_shared<FakeBackend>();
    ReviewController controller{dependenciesFor(backend)};
    ReviewShellController shell{controller};

    EXPECT_FALSE(shell.setRangeIn(-1, -1.0));
    ASSERT_TRUE(shell.setRangeOut(8, 8000.0));
    ASSERT_TRUE(shell.setRangeIn(3, 3000.0));
    EXPECT_EQ(shell.inFrame(), 3);
    EXPECT_EQ(shell.outFrame(), 8);
    EXPECT_TRUE(shell.setRangePlaybackState(true, true));
    EXPECT_TRUE(shell.rangePlaybackActive());
    EXPECT_TRUE(shell.rangeStartPending());

    ASSERT_TRUE(shell.setRangeIn(9, 9000.0));
    EXPECT_EQ(shell.inFrame(), 9);
    EXPECT_EQ(shell.outFrame(), -1);
    EXPECT_FALSE(shell.rangePlaybackActive());
    EXPECT_FALSE(shell.rangeStartPending());
    EXPECT_FALSE(shell.setRangePlaybackState(true, false));

    ASSERT_TRUE(shell.setRangeOut(12, 12000.0));
    shell.remapRange(4, 6);
    EXPECT_EQ(shell.inFrame(), 4);
    EXPECT_EQ(shell.outFrame(), 6);
    EXPECT_DOUBLE_EQ(shell.inMediaTime(), 9000.0);
    EXPECT_DOUBLE_EQ(shell.outMediaTime(), 12000.0);

    shell.clearRange();
    EXPECT_EQ(shell.inFrame(), -1);
    EXPECT_EQ(shell.outFrame(), -1);
    EXPECT_DOUBLE_EQ(shell.inMediaTime(), -1.0);
    EXPECT_DOUBLE_EQ(shell.outMediaTime(), -1.0);
    controller.stop();
}

TEST_F(ReviewControllerTests, RejectsAnAbsentOrOutOfRangeReferenceSource) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourceAPath = createFile(directory, QStringLiteral("first.mp4"));
    const QString sourceBPath = createFile(directory, QStringLiteral("second.mp4"));
    ASSERT_FALSE(sourceAPath.isEmpty());
    ASSERT_FALSE(sourceBPath.isEmpty());

    auto backend = std::make_shared<FakeBackend>();
    ReviewController controller{dependenciesFor(backend)};
    EXPECT_FALSE(controller.openComparisonSet(
        QUrl::fromLocalFile(sourceAPath), QUrl::fromLocalFile(sourceBPath), QUrl{}, 2));
    EXPECT_FALSE(controller.openComparisonSet(
        QUrl::fromLocalFile(sourceAPath), QUrl::fromLocalFile(sourceBPath), QUrl{}, -1));
    EXPECT_TRUE(backend->submitted.empty());
    controller.stop();
}

TEST_F(ReviewControllerTests, RejectsNonLocalMissingAndDirectoryUrlsWithoutDispatch) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString validPath = createFile(directory, QStringLiteral("valid.mp4"));
    ASSERT_FALSE(validPath.isEmpty());
    auto backend = std::make_shared<FakeBackend>();
    ReviewController controller{dependenciesFor(backend)};

    EXPECT_FALSE(controller.openComparison(QUrl{QStringLiteral("https://example.invalid/a.mp4")},
                                           QUrl::fromLocalFile(validPath)));
    EXPECT_EQ(controller.sourceAErrorKey(), QStringLiteral("invalid-argument"));
    EXPECT_TRUE(controller.sourceAFilename().isEmpty());
    EXPECT_TRUE(controller.sourceBFilename().isEmpty());
    EXPECT_TRUE(backend->submitted.empty());

    const QString missingPath = directory.filePath(QStringLiteral("missing.mp4"));
    EXPECT_FALSE(controller.openComparison(QUrl::fromLocalFile(validPath),
                                           QUrl::fromLocalFile(missingPath)));
    EXPECT_TRUE(controller.sourceAErrorKey().isEmpty());
    EXPECT_EQ(controller.sourceBErrorKey(), QStringLiteral("source-missing"));
    EXPECT_TRUE(backend->submitted.empty());

    EXPECT_FALSE(controller.openComparison(QUrl::fromLocalFile(directory.path()),
                                           QUrl::fromLocalFile(validPath)));
    EXPECT_EQ(controller.sourceAErrorKey(), QStringLiteral("source-missing"));
    EXPECT_TRUE(backend->submitted.empty());

    // Closing an already-empty review is a successful no-op, but it must still clear errors
    // that came from the rejected candidate rather than an active session.
    ReviewShellController shell{controller};
    EXPECT_TRUE(shell.closeSources());
    EXPECT_TRUE(controller.sourceAErrorKey().isEmpty());
    EXPECT_TRUE(controller.sourceBErrorKey().isEmpty());
    EXPECT_TRUE(controller.sourceCErrorKey().isEmpty());
    controller.stop();
}

TEST_F(ReviewControllerTests, NavigationStaysAvailableAndLatestContextClearsFramePending) {
    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot = readySnapshot(3, 10U);
    ReviewController controller{dependenciesFor(backend)};
    ASSERT_TRUE(controller.next());
    ASSERT_FALSE(controller.busy());
    ASSERT_TRUE(controller.framePending());
    ASSERT_EQ(backend->submitted.size(), 1U);

    EXPECT_TRUE(controller.first());
    EXPECT_TRUE(controller.previous());
    EXPECT_TRUE(controller.next());
    EXPECT_TRUE(controller.last());
    EXPECT_FALSE(controller.openComparison(QUrl{}, QUrl{}));
    ASSERT_EQ(backend->submitted.size(), 5U);
    const application::CommandContext pending =
        application::commandContext(backend->submitted.back());

    backend->terminals.push_back(application::CommandTerminal{
        .context =
            application::CommandContext{
                .sessionId = pending.sessionId,
                .sessionEpoch = pending.sessionEpoch,
                .commandId = domain::CommandId{pending.commandId.value() + 1U},
            },
        .outcome = application::CommandOutcome::Succeeded,
    });
    ASSERT_TRUE(waitUntil([&backend] { return backend->terminals.empty(); }));
    EXPECT_TRUE(controller.framePending());

    backend->terminals.push_back(application::CommandTerminal{
        .context =
            application::CommandContext{
                .sessionId = pending.sessionId,
                .sessionEpoch = domain::SessionEpoch{pending.sessionEpoch.value() + 1U},
                .commandId = pending.commandId,
            },
        .outcome = application::CommandOutcome::Succeeded,
    });
    ASSERT_TRUE(waitUntil([&backend] { return backend->terminals.empty(); }));
    EXPECT_TRUE(controller.framePending());

    backend->terminals.push_back(application::CommandTerminal{
        .context =
            application::CommandContext{
                .sessionId = domain::SessionId{pending.sessionId.value() + 1U},
                .sessionEpoch = pending.sessionEpoch,
                .commandId = pending.commandId,
            },
        .outcome = application::CommandOutcome::Succeeded,
    });
    ASSERT_TRUE(waitUntil([&backend] { return backend->terminals.empty(); }));
    EXPECT_TRUE(controller.framePending());

    backend->terminals.push_back(application::CommandTerminal{
        .context = pending,
        .outcome = application::CommandOutcome::Succeeded,
    });
    EXPECT_TRUE(waitUntil([&controller] { return !controller.framePending(); }));
    controller.stop();
}

TEST_F(ReviewControllerTests, ProjectsDisplayFramesGraphicsAndRoleSpecificErrorKeys) {
    auto backend = std::make_shared<FakeBackend>();
    ReviewController controller{dependenciesFor(backend)};
    EXPECT_EQ(controller.displayState(), ReviewController::ReviewDisplayState::Empty);
    EXPECT_TRUE(controller.graphicsReady());
    EXPECT_EQ(controller.currentFrame(), -1);
    EXPECT_EQ(controller.totalFrames(), 0U);

    backend->currentSnapshot = emptySnapshot();
    backend->currentSnapshot.sessionState = domain::SessionState::kLoading;
    ASSERT_TRUE(waitUntil([&controller] {
        return controller.displayState() == ReviewController::ReviewDisplayState::Loading;
    }));

    backend->currentSnapshot = readySnapshot(2, 5U);
    backend->currentSnapshot.sources.push_back(application::SessionSourceView{
        .sourceId = 2U,
        .role = domain::ComparisonRole::kPrediction,
        .displayName = "C",
    });
    backend->currentSnapshot.presentedSources = {
        application::PresentedSourceState{
            .sourceId = 0U,
            .sourceFrameId = domain::FrameId{2},
            .matchKind = application::FrameMatchKind::ExactIndex,
        },
        application::PresentedSourceState{
            .sourceId = 1U,
            .sourceFrameId = domain::FrameId{3},
            .matchKind = application::FrameMatchKind::AutoAligned,
            .alignmentConfidence = 0.64F,
        },
        application::PresentedSourceState{
            .sourceId = 2U,
            .sourceFrameId = std::nullopt,
            .matchKind = application::FrameMatchKind::Missing,
            .missingReason = application::MissingReason::AfterSourceEnd,
        },
    };
    backend->currentSnapshot.compatibilityFindings = {
        application::CompatibilityFindingView{
            .severity = domain::CompatibilitySeverity::kAlignmentRequired,
            .code = domain::MediaErrorCode::kSourceFrameCountMismatch,
            .sources = {0U, 2U},
        },
    };
    backend->currentSnapshot.alignmentEstimates = {
        application::GlobalOffsetEstimate{
            .sourceId = 1U,
            .bestOffset = 1,
            .bestCost = 0.08F,
            .runnerUpCost = 0.22F,
            .confidence = 0.64F,
            .evidenceCount = 5U,
            .autoApplicable = true,
        },
        application::GlobalOffsetEstimate{
            .sourceId = 2U,
            .bestOffset = -2,
            .bestCost = 0.24F,
            .runnerUpCost = 0.25F,
            .confidence = 0.04F,
            .evidenceCount = 5U,
            .autoApplicable = false,
        },
    };
    backend->currentSnapshot.sequenceAlignments = {
        application::SequenceAlignmentSummary{
            .sourceId = 1U,
            .anomalies =
                {
                    application::SequenceAlignmentAnomaly{
                        .kind = application::SequenceAlignmentAnomalyKind::TargetFrameMissing,
                        .canonicalFrameId = domain::FrameId{3},
                    },
                    application::SequenceAlignmentAnomaly{
                        .kind = application::SequenceAlignmentAnomalyKind::TargetFrameDuplicate,
                        .canonicalFrameId = domain::FrameId{4},
                        .sourceFrameId = domain::FrameId{5},
                    },
                },
            .anomalyCount = 2U,
            .lowConfidenceRuns =
                {
                    application::SequenceAlignmentLowConfidenceRun{
                        .firstCanonicalFrame = domain::FrameId{0},
                        .lastCanonicalFrame = domain::FrameId{1},
                        .minimumConfidence = 0.18F,
                    },
                    application::SequenceAlignmentLowConfidenceRun{
                        .firstCanonicalFrame = domain::FrameId{4},
                        .lastCanonicalFrame = domain::FrameId{4},
                        .minimumConfidence = 0.25F,
                    },
                },
            .totalCost = 0.2F,
            .meanMatchCost = 0.02F,
            .confidence = 0.82F,
            .autoApplicable = true,
        },
    };
    backend->currentSnapshot.manualAlignmentAnchors = {
        application::SourceAlignmentAnchors{
            .sourceId = 1U,
            .anchors =
                {
                    application::ManualAlignmentAnchor{
                        .canonicalFrameId = domain::FrameId{3},
                        .sourceFrameId = domain::FrameId{4},
                    },
                    application::ManualAlignmentAnchor{
                        .canonicalFrameId = domain::FrameId{8},
                        .sourceFrameId = domain::FrameId{10},
                    },
                },
        },
    };
    backend->currentSnapshot.lastError =
        domain::makeMediaError(domain::MediaErrorCode::kMediaProbeFailed,
                               domain::MediaOperation::kMediaProbe,
                               domain::SourceId{0},
                               true,
                               "must never be exposed");
    ASSERT_TRUE(waitUntil([&controller] {
        return controller.displayState() == ReviewController::ReviewDisplayState::Ready &&
               controller.currentFrame() == 2;
    }));
    EXPECT_EQ(controller.totalFrames(), 5U);
    EXPECT_TRUE(controller.frameMappingStatus().contains(QStringLiteral("B: source frame 4")));
    EXPECT_TRUE(controller.frameMappingStatus().contains(QStringLiteral("C: Missing frame")));
    EXPECT_FALSE(controller.sourceAMissing());
    EXPECT_FALSE(controller.sourceBMissing());
    EXPECT_TRUE(controller.sourceCMissing());
    EXPECT_TRUE(controller.frameMappingStatus().contains(QStringLiteral("auto offset +1, 64%")));
    EXPECT_TRUE(controller.alignmentEstimateStatus().contains(QStringLiteral("B: auto +1 (64%)")));
    EXPECT_TRUE(controller.alignmentEstimateStatus().contains(
        QStringLiteral("C: suggested -2 (4%, review manually)")));
    EXPECT_TRUE(controller.autoAlignmentActive());
    EXPECT_TRUE(controller.sequenceAlignmentStatus().contains(
        QStringLiteral("B: sequence mapped, 82% confidence")));
    EXPECT_TRUE(controller.sequenceAlignmentStatus().contains(QStringLiteral("missing @ 4")));
    EXPECT_TRUE(controller.sequenceAlignmentStatus().contains(QStringLiteral("duplicate @ 5")));
    EXPECT_TRUE(controller.manualAnchorActive());
    EXPECT_EQ(controller.manualAnchorStatus(), QStringLiteral("B: anchors 4↔5, 9↔11"));
    const QVariantList timelineMarkers = controller.alignmentTimelineMarkers();
    ASSERT_EQ(timelineMarkers.size(), 6);
    EXPECT_EQ(timelineMarkers[0].toMap().value(QStringLiteral("kind")).toString(),
              QStringLiteral("missing"));
    EXPECT_EQ(timelineMarkers[0].toMap().value(QStringLiteral("frame")).toULongLong(), 3U);
    EXPECT_EQ(timelineMarkers[1].toMap().value(QStringLiteral("kind")).toString(),
              QStringLiteral("duplicate"));
    EXPECT_EQ(timelineMarkers[2].toMap().value(QStringLiteral("kind")).toString(),
              QStringLiteral("anchor"));
    EXPECT_EQ(timelineMarkers[2].toMap().value(QStringLiteral("frame")).toULongLong(), 3U);
    EXPECT_EQ(timelineMarkers[4].toMap().value(QStringLiteral("kind")).toString(),
              QStringLiteral("low-confidence"));
    EXPECT_EQ(timelineMarkers[4].toMap().value(QStringLiteral("frame")).toULongLong(), 0U);
    EXPECT_EQ(timelineMarkers[5].toMap().value(QStringLiteral("frame")).toULongLong(), 4U);
    const QVariantList compatibilityFindings = controller.compatibilityFindings();
    ASSERT_EQ(compatibilityFindings.size(), 1);
    EXPECT_EQ(compatibilityFindings.front().toMap().value(QStringLiteral("code")).toString(),
              QStringLiteral("source-frame-count-mismatch"));
    EXPECT_EQ(compatibilityFindings.front().toMap().value(QStringLiteral("severity")).toInt(),
              static_cast<int>(domain::CompatibilitySeverity::kAlignmentRequired));
    EXPECT_EQ(
        compatibilityFindings.front().toMap().value(QStringLiteral("sources")).toList(),
        QVariantList({QVariant::fromValue<qulonglong>(0U), QVariant::fromValue<qulonglong>(2U)}));
    QAbstractItemModel* const sources = controller.sources();
    ASSERT_NE(sources, nullptr);
    ASSERT_EQ(sources->rowCount(), 3);
    EXPECT_EQ(sources->data(sources->index(0, 0), SourceListModel::FilenameRole).toString(),
              QStringLiteral("A"));
    EXPECT_EQ(
        sources->data(sources->index(1, 0), SourceListModel::CurrentSourceFrameRole).toLongLong(),
        3);
    EXPECT_EQ(sources->data(sources->index(2, 0), SourceListModel::MissingRole).toBool(), true);
    const QVariantList differenceEdges = controller.differenceEdges();
    ASSERT_EQ(differenceEdges.size(), 3);
    EXPECT_EQ(differenceEdges[1].toMap().value(QStringLiteral("label")).toString(),
              QStringLiteral("A ↔ C"));
    EXPECT_EQ(differenceEdges[1].toMap().value(QStringLiteral("preferenceValue")).toInt(), 1);
    EXPECT_EQ(controller.sourceAErrorKey(), QStringLiteral("media-probe-failed"));
    EXPECT_TRUE(controller.sourceBErrorKey().isEmpty());
    EXPECT_TRUE(controller.pairErrorKey().isEmpty());

    backend->currentSnapshot.lastError =
        domain::makeMediaError(domain::MediaErrorCode::kSourceMissing,
                               domain::MediaOperation::kMediaProbe,
                               domain::SourceId{1},
                               true);
    ASSERT_TRUE(waitUntil([&controller] {
        return controller.sourceBErrorKey() == QStringLiteral("source-missing");
    }));
    EXPECT_TRUE(controller.sourceAErrorKey().isEmpty());

    backend->currentSnapshot.lastError =
        domain::makeMediaError(domain::MediaErrorCode::kMediaDecodeFailed,
                               domain::MediaOperation::kMediaDecode,
                               domain::SourceId{2},
                               true);
    ASSERT_TRUE(waitUntil([&controller] {
        return controller.sourceCErrorKey() == QStringLiteral("media-decode-failed");
    }));
    EXPECT_TRUE(controller.sourceAErrorKey().isEmpty());
    EXPECT_TRUE(controller.sourceBErrorKey().isEmpty());

    backend->currentSnapshot.lastError =
        domain::makeMediaError(domain::MediaErrorCode::kSourceFrameCountMismatch,
                               domain::MediaOperation::kSourcePairValidation,
                               std::nullopt,
                               false);
    ASSERT_TRUE(waitUntil([&controller] {
        return controller.pairErrorKey() == QStringLiteral("source-frame-count-mismatch");
    }));
    EXPECT_TRUE(controller.sourceAErrorKey().isEmpty());
    EXPECT_TRUE(controller.sourceBErrorKey().isEmpty());

    backend->currentSnapshot = emptySnapshot(false);
    backend->currentSnapshot.sessionState = domain::SessionState::kInvalid;
    ASSERT_TRUE(waitUntil([&controller] {
        return controller.displayState() == ReviewController::ReviewDisplayState::Invalid &&
               !controller.graphicsReady();
    }));
    backend->currentSnapshot.sessionState = domain::SessionState::kError;
    ASSERT_TRUE(waitUntil([&controller] {
        return controller.displayState() == ReviewController::ReviewDisplayState::Error;
    }));
    controller.stop();
}

TEST_F(ReviewControllerTests, ProjectsContinuousPlaybackWithoutLatchingGeneralBusy) {
    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot = readySnapshot(2, 8U);
    ReviewController controller{dependenciesFor(backend)};

    EXPECT_FALSE(controller.playing());
    EXPECT_TRUE(controller.canPlay());
    EXPECT_FALSE(controller.canPause());
    ASSERT_TRUE(controller.play());
    ASSERT_TRUE(std::holds_alternative<application::PlayCommand>(backend->submitted.back()));
    EXPECT_FALSE(controller.busy());
    EXPECT_FALSE(controller.canPlay());

    backend->currentSnapshot.playbackState = domain::PlaybackState::kPlaying;
    completeLastCommand(backend);
    ASSERT_TRUE(waitUntil([&controller] { return controller.playing() && controller.canPause(); }));
    EXPECT_FALSE(controller.busy());
    EXPECT_FALSE(controller.canOpen());
    // Frame navigation stays enabled during playback: dispatching it pauses first, then seeks.
    EXPECT_TRUE(controller.canFirst());
    EXPECT_TRUE(controller.canPrevious());
    EXPECT_TRUE(controller.canNext());
    EXPECT_TRUE(controller.canLast());

    ASSERT_TRUE(controller.togglePlayback());
    ASSERT_TRUE(std::holds_alternative<application::PauseCommand>(backend->submitted.back()));
    EXPECT_FALSE(controller.busy());
    backend->currentSnapshot.playbackState = domain::PlaybackState::kPaused;
    backend->currentSnapshot.requestedFrame = domain::FrameId{3};
    completeLastCommand(backend);
    ASSERT_TRUE(waitUntil([&controller] {
        return !controller.playing() && !controller.canPause() && !controller.canPlay();
    }));
    // Navigation stays enabled while the pause drains; a new step supersedes the in-flight one.
    EXPECT_TRUE(controller.canFirst());

    backend->currentSnapshot.displayedFrame = domain::FrameId{3};
    backend->currentSnapshot.requestedFrame.reset();
    ASSERT_TRUE(waitUntil([&controller] { return controller.canPlay(); }));
    EXPECT_TRUE(controller.canFirst());
    controller.stop();
}

TEST_F(ReviewControllerTests, OneFrameKeepsFirstAndLastButDisablesMovement) {
    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot = readySnapshot(0, 1U);
    ReviewController controller{dependenciesFor(backend)};
    EXPECT_TRUE(controller.canFirst());
    EXPECT_TRUE(controller.canLast());
    EXPECT_FALSE(controller.canPrevious());
    EXPECT_FALSE(controller.canNext());
    EXPECT_FALSE(controller.canPlay());
    EXPECT_FALSE(controller.canPause());
    EXPECT_FALSE(controller.previous());
    EXPECT_FALSE(controller.next());

    ASSERT_TRUE(controller.first());
    ASSERT_TRUE(std::holds_alternative<application::FirstFrameCommand>(backend->submitted.back()));
    completeLastCommand(backend);
    ASSERT_TRUE(waitUntil([&controller] { return !controller.busy(); }));

    ASSERT_TRUE(controller.last());
    ASSERT_TRUE(std::holds_alternative<application::LastFrameCommand>(backend->submitted.back()));
    EXPECT_EQ(application::commandContext(backend->submitted.front()).commandId,
              domain::CommandId{1U});
    EXPECT_EQ(application::commandContext(backend->submitted.back()).commandId,
              domain::CommandId{2U});
    controller.stop();
}

TEST_F(ReviewControllerTests, NavigationDispatchesExactVariantsAndClampsUiAtBoundaries) {
    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot = readySnapshot(5, 10U);
    ReviewController controller{dependenciesFor(backend)};

    ASSERT_TRUE(controller.previous());
    ASSERT_EQ(std::get<application::StepFramesCommand>(backend->submitted.back()).delta, -1);
    completeLastCommand(backend);
    ASSERT_TRUE(waitUntil([&controller] { return !controller.busy(); }));

    ASSERT_TRUE(controller.next());
    ASSERT_EQ(std::get<application::StepFramesCommand>(backend->submitted.back()).delta, 1);
    completeLastCommand(backend);
    ASSERT_TRUE(waitUntil([&controller] { return !controller.busy(); }));

    ASSERT_TRUE(controller.first());
    EXPECT_TRUE(std::holds_alternative<application::FirstFrameCommand>(backend->submitted.back()));
    completeLastCommand(backend);
    ASSERT_TRUE(waitUntil([&controller] { return !controller.busy(); }));

    ASSERT_TRUE(controller.last());
    EXPECT_TRUE(std::holds_alternative<application::LastFrameCommand>(backend->submitted.back()));
    completeLastCommand(backend);
    ASSERT_TRUE(waitUntil([&controller] { return !controller.busy(); }));

    backend->currentSnapshot.displayedFrame = domain::FrameId{0};
    ASSERT_TRUE(waitUntil([&controller] { return !controller.canPrevious(); }));
    EXPECT_TRUE(controller.canNext());
    backend->currentSnapshot.displayedFrame = domain::FrameId{9};
    ASSERT_TRUE(waitUntil([&controller] { return !controller.canNext(); }));
    EXPECT_TRUE(controller.canPrevious());
    controller.stop();
}

TEST_F(ReviewControllerTests, GenericStepAndSeekDispatchExactCommandsAndRejectInvalidTargets) {
    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot = readySnapshot(5, 10U);
    ReviewController controller{dependenciesFor(backend)};

    ASSERT_TRUE(controller.stepFrames(-5));
    ASSERT_EQ(std::get<application::StepFramesCommand>(backend->submitted.back()).delta, -5);
    completeLastCommand(backend);
    ASSERT_TRUE(waitUntil([&controller] { return !controller.busy(); }));

    ASSERT_TRUE(controller.stepFrames(10));
    ASSERT_EQ(std::get<application::StepFramesCommand>(backend->submitted.back()).delta, 10);
    completeLastCommand(backend);
    ASSERT_TRUE(waitUntil([&controller] { return !controller.busy(); }));

    ASSERT_TRUE(controller.seekFrame(8));
    EXPECT_EQ(std::get<application::SeekFrameCommand>(backend->submitted.back()).frameId,
              domain::FrameId{8});
    completeLastCommand(backend);
    ASSERT_TRUE(waitUntil([&controller] { return !controller.busy(); }));

    const std::size_t submitted = backend->submitted.size();
    EXPECT_FALSE(controller.stepFrames(0));
    EXPECT_FALSE(controller.seekFrame(-1));
    EXPECT_FALSE(controller.seekFrame(10));
    EXPECT_FALSE(controller.seekFrame(5));
    EXPECT_EQ(backend->submitted.size(), submitted);

    backend->currentSnapshot.displayedFrame = domain::FrameId{0};
    ASSERT_TRUE(waitUntil([&controller] { return controller.currentFrame() == 0; }));
    EXPECT_FALSE(controller.stepFrames(-5));
    EXPECT_TRUE(controller.stepFrames(5));
    completeLastCommand(backend);
    ASSERT_TRUE(waitUntil([&controller] { return !controller.busy(); }));

    backend->currentSnapshot.displayedFrame = domain::FrameId{9};
    ASSERT_TRUE(waitUntil([&controller] { return controller.currentFrame() == 9; }));
    EXPECT_FALSE(controller.stepFrames(5));
    EXPECT_TRUE(controller.stepFrames(-5));
    controller.stop();
}

TEST_F(ReviewControllerTests, DispatchesExplicitAlignmentOffsetsAsOneAtomicCommand) {
    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot = readySnapshot(5, 10U);
    ReviewController controller{dependenciesFor(backend)};

    ASSERT_TRUE(controller.applyAlignmentOffsets(0, 2, 7));
    ASSERT_EQ(backend->submitted.size(), 1U);
    const auto* const command =
        std::get_if<application::SetAlignmentOffsetsCommand>(&backend->submitted.back());
    ASSERT_NE(command, nullptr);
    ASSERT_EQ(command->sourceOffsets.size(), 1U);
    EXPECT_EQ(command->sourceOffsets.front(),
              (application::SourceFrameOffset{.sourceId = 1U, .frames = 2}));
    EXPECT_TRUE(controller.busy());
    controller.stop();
}

TEST_F(ReviewControllerTests, DispatchesDynamicSourceOffsetsAndRejectsMalformedRows) {
    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot = readySnapshot(5, 10U);
    ReviewController controller{dependenciesFor(backend)};

    const QVariantList offsets{
        QVariantMap{
            {QStringLiteral("sourceId"), 0U},
            {QStringLiteral("frames"), 0},
        },
        QVariantMap{
            {QStringLiteral("sourceId"), 1U},
            {QStringLiteral("frames"), -3},
        },
    };
    ASSERT_TRUE(controller.applySourceOffsets(offsets));
    ASSERT_EQ(backend->submitted.size(), 1U);
    const auto* const command =
        std::get_if<application::SetAlignmentOffsetsCommand>(&backend->submitted.back());
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->sourceOffsets,
              (std::vector<application::SourceFrameOffset>{
                  application::SourceFrameOffset{.sourceId = 0U, .frames = 0},
                  application::SourceFrameOffset{.sourceId = 1U, .frames = -3},
              }));

    completeLastCommand(backend);
    ASSERT_TRUE(waitUntil([&controller] { return !controller.busy(); }));
    EXPECT_FALSE(controller.applySourceOffsets(QVariantList{
        QVariantMap{
            {QStringLiteral("sourceId"), 1U},
            {QStringLiteral("frames"), 2},
        },
        QVariantMap{
            {QStringLiteral("sourceId"), 1U},
            {QStringLiteral("frames"), 3},
        },
    }));
    EXPECT_FALSE(controller.applySourceOffsets(QVariantList{
        QVariantMap{
            {QStringLiteral("sourceId"), QStringLiteral("bad")},
            {QStringLiteral("frames"), 2},
        },
    }));
    EXPECT_EQ(backend->submitted.size(), 1U);
    controller.stop();
}

TEST_F(ReviewControllerTests, DispatchesAutomaticAlignmentWithoutBlockingNavigation) {
    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot = readySnapshot(5, 10U);
    ReviewController controller{dependenciesFor(backend)};

    ASSERT_TRUE(controller.estimateAlignment());
    ASSERT_EQ(backend->submitted.size(), 1U);
    EXPECT_NE(std::get_if<application::EstimateAlignmentCommand>(&backend->submitted.back()),
              nullptr);
    EXPECT_FALSE(controller.busy());
    EXPECT_TRUE(controller.next());
    EXPECT_FALSE(controller.busy());
    EXPECT_TRUE(controller.framePending());
    controller.stop();
}

TEST_F(ReviewControllerTests, DispatchesSequenceAnalysisWithoutBlockingNavigation) {
    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot = readySnapshot(5, 10U);
    ReviewController controller{dependenciesFor(backend)};

    ASSERT_TRUE(controller.analyzeSequenceAlignment());
    ASSERT_EQ(backend->submitted.size(), 1U);
    EXPECT_NE(std::get_if<application::AnalyzeSequenceAlignmentCommand>(&backend->submitted.back()),
              nullptr);
    EXPECT_FALSE(controller.busy());
    EXPECT_TRUE(controller.previous());
    EXPECT_FALSE(controller.busy());
    EXPECT_TRUE(controller.framePending());
    controller.stop();
}

TEST_F(ReviewControllerTests, ProjectsAnalysisProgressAndDispatchesCancellation) {
    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot = readySnapshot(5, 10U);
    backend->currentSnapshot.alignmentAnalysisJobId = application::AlignmentAnalysisJobId{7U};
    backend->currentSnapshot.alignmentAnalysisKind = application::AlignmentAnalysisKind::Sequence;
    backend->currentSnapshot.alignmentAnalysisPhase =
        application::AlignmentAnalysisPhase::ComputingAlignment;
    backend->currentSnapshot.alignmentAnalysisCompletedUnits = 25U;
    backend->currentSnapshot.alignmentAnalysisWork =
        application::AlignmentWorkEstimate{.totalUnits = 100U, .unitName = "work units"};
    ReviewController controller{dependenciesFor(backend)};

    EXPECT_TRUE(controller.alignmentAnalysisRunning());
    EXPECT_DOUBLE_EQ(controller.alignmentAnalysisProgress(), 0.25);
    EXPECT_FALSE(controller.alignmentAnalysisStatus().isEmpty());
    ASSERT_TRUE(controller.cancelAlignmentAnalysis());
    ASSERT_EQ(backend->submitted.size(), 1U);
    EXPECT_NE(std::get_if<application::CancelAlignmentAnalysisCommand>(&backend->submitted.back()),
              nullptr);
    EXPECT_FALSE(controller.busy());
    controller.stop();
}

TEST_F(ReviewControllerTests, ProjectsAndDispatchesAutomaticAlignmentConfirmationAndUndo) {
    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot = readySnapshot(5, 10U);
    backend->currentSnapshot.alignmentRequired = true;
    backend->currentSnapshot.automaticAlignmentPending = true;
    backend->currentSnapshot.canConfirmAutomaticAlignment = true;
    ReviewController controller{dependenciesFor(backend)};

    EXPECT_TRUE(controller.alignmentRequired());
    EXPECT_TRUE(controller.automaticAlignmentPending());
    EXPECT_TRUE(controller.canConfirmAutomaticAlignment());
    EXPECT_FALSE(controller.canUndoAutomaticAlignment());
    ASSERT_TRUE(controller.confirmAutomaticAlignment());
    ASSERT_EQ(backend->submitted.size(), 1U);
    EXPECT_NE(
        std::get_if<application::ConfirmAutomaticAlignmentCommand>(&backend->submitted.back()),
        nullptr);

    completeLastCommand(backend);
    backend->currentSnapshot.alignmentRequired = false;
    backend->currentSnapshot.automaticAlignmentPending = false;
    backend->currentSnapshot.canConfirmAutomaticAlignment = false;
    backend->currentSnapshot.canUndoAutomaticAlignment = true;
    ASSERT_TRUE(waitUntil(
        [&controller] { return !controller.busy() && controller.canUndoAutomaticAlignment(); }));
    ASSERT_TRUE(controller.undoAutomaticAlignment());
    ASSERT_EQ(backend->submitted.size(), 2U);
    EXPECT_NE(std::get_if<application::UndoAutomaticAlignmentCommand>(&backend->submitted.back()),
              nullptr);
    controller.stop();
}

TEST_F(ReviewControllerTests, DispatchesManualAnchorUpsertAndClearCommands) {
    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot = readySnapshot(5, 10U);
    ReviewController controller{dependenciesFor(backend)};

    EXPECT_FALSE(controller.setManualAlignmentAnchor(-1, 5, 6));
    EXPECT_FALSE(controller.setManualAlignmentAnchor(1, -1, 6));
    ASSERT_TRUE(controller.setManualAlignmentAnchor(1, 5, 6));
    ASSERT_EQ(backend->submitted.size(), 1U);
    const auto* const anchor =
        std::get_if<application::SetManualAlignmentAnchorCommand>(&backend->submitted.back());
    ASSERT_NE(anchor, nullptr);
    EXPECT_EQ(anchor->sourceId, 1U);
    EXPECT_EQ(anchor->anchor.canonicalFrameId, domain::FrameId{5});
    EXPECT_EQ(anchor->anchor.sourceFrameId, domain::FrameId{6});

    completeLastCommand(backend);
    ASSERT_TRUE(waitUntil([&controller] { return !controller.busy(); }));
    ASSERT_TRUE(controller.clearManualAlignmentAnchors());
    EXPECT_NE(
        std::get_if<application::ClearManualAlignmentAnchorsCommand>(&backend->submitted.back()),
        nullptr);
    controller.stop();
}

TEST_F(ReviewControllerTests, BusyAndClosedSubmissionsDoNotLatchAndIdsUseLatestEpoch) {
    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot = readySnapshot(5, 10U, 3U);
    backend->submitResult = application::PortSubmitResult::Busy;
    ReviewController controller{dependenciesFor(backend)};

    EXPECT_FALSE(controller.next());
    EXPECT_FALSE(controller.busy());
    ASSERT_EQ(backend->submitted.size(), 1U);
    EXPECT_EQ(application::commandContext(backend->submitted.back()).commandId,
              domain::CommandId{1U});

    const std::size_t snapshotCallsBeforeEpochChange = backend->snapshotCalls;
    backend->currentSnapshot.sessionEpoch = domain::SessionEpoch{4U};
    ASSERT_TRUE(waitUntil([&backend, snapshotCallsBeforeEpochChange] {
        return backend->snapshotCalls > snapshotCallsBeforeEpochChange;
    }));
    backend->submitResult = application::PortSubmitResult::Closed;
    EXPECT_FALSE(controller.previous());
    EXPECT_FALSE(controller.busy());
    ASSERT_EQ(backend->submitted.size(), 2U);
    EXPECT_EQ(application::commandContext(backend->submitted.back()).sessionEpoch,
              domain::SessionEpoch{4U});
    EXPECT_EQ(application::commandContext(backend->submitted.back()).commandId,
              domain::CommandId{2U});

    backend->submitResult = application::PortSubmitResult::Accepted;
    ASSERT_TRUE(controller.first());
    EXPECT_EQ(application::commandContext(backend->submitted.back()).commandId,
              domain::CommandId{3U});
    controller.stop();
}

TEST_F(ReviewControllerTests, StopAndExpiredBackendFailClosedWithoutFurtherAccess) {
    auto backend = std::make_shared<FakeBackend>();
    backend->currentSnapshot = readySnapshot(4, 10U);
    ReviewController controller{dependenciesFor(backend)};
    ASSERT_TRUE(controller.next());
    ASSERT_TRUE(controller.framePending());

    std::thread::id notificationThread;
    QObject::connect(&controller, &ReviewController::stateChanged, &controller, [&] {
        notificationThread = std::this_thread::get_id();
    });
    std::thread stopper{[&controller] { controller.stop(); }};
    stopper.join();
    ASSERT_TRUE(waitUntil([&controller] { return !controller.graphicsReady(); }));
    EXPECT_EQ(notificationThread, std::this_thread::get_id());
    const std::size_t accesses =
        backend->submitCalls + backend->snapshotCalls + backend->drainCalls;
    EXPECT_FALSE(controller.busy());
    EXPECT_FALSE(controller.graphicsReady());
    EXPECT_FALSE(controller.canOpen());
    EXPECT_FALSE(controller.canFirst());
    EXPECT_FALSE(controller.canPrevious());
    EXPECT_FALSE(controller.canNext());
    EXPECT_FALSE(controller.canLast());
    EXPECT_FALSE(controller.canPlay());
    EXPECT_FALSE(controller.canPause());
    EXPECT_FALSE(controller.playing());
    EXPECT_FALSE(controller.first());
    EXPECT_FALSE(controller.play());
    EXPECT_FALSE(controller.pause());
    EXPECT_FALSE(controller.togglePlayback());
    EXPECT_FALSE(controller.openComparison(QUrl{}, QUrl{}));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    EXPECT_EQ(backend->submitCalls + backend->snapshotCalls + backend->drainCalls, accesses);

    auto expiringBackend = std::make_shared<FakeBackend>();
    expiringBackend->currentSnapshot = readySnapshot(4, 10U);
    ReviewController expiringController{dependenciesFor(expiringBackend)};
    ASSERT_TRUE(expiringController.graphicsReady());
    expiringBackend.reset();
    ASSERT_TRUE(waitUntil([&expiringController] {
        return !expiringController.graphicsReady() && !expiringController.canOpen();
    }));
    EXPECT_FALSE(expiringController.next());
}

} // namespace
} // namespace dvs::ui
