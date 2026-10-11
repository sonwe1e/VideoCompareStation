#include "dvs/persistence/InterpolationProtocolRepository.h"
#include "dvs/ui/InterpolationProtocolController.h"
#include "dvs/ui/ReviewController.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dvs::ui {
namespace {

using namespace std::chrono_literals;

void ensureCoreApplication() {
    if (QCoreApplication::instance() != nullptr) {
        return;
    }
    static int argumentCount = 1;
    static char applicationName[] = "InterpolationProtocolControllerTests";
    static char* arguments[] = {applicationName, nullptr};
    static QCoreApplication application{argumentCount, arguments};
    static_cast<void>(application);
}

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate predicate, const std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        QThread::msleep(1U);
    }
    return predicate();
}

[[nodiscard]] application::SessionSnapshot emptySnapshot() {
    application::SessionSnapshot snapshot;
    snapshot.sessionId = domain::SessionId{17U};
    snapshot.sessionEpoch = domain::SessionEpoch{3U};
    snapshot.deviceGeneration = domain::DeviceGeneration{2U};
    snapshot.graphicsReady = true;
    return snapshot;
}

[[nodiscard]] application::SessionSnapshot readySnapshot(const std::int64_t frame,
                                                         const std::uint64_t frameCount) {
    application::SessionSnapshot snapshot = emptySnapshot();
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

class FakeBackend final {
public:
    application::SessionSnapshot currentSnapshot = emptySnapshot();
    std::vector<application::PlaybackCommand> submitted;
    std::vector<application::CommandTerminal> terminals;
};

[[nodiscard]] ReviewController::Dependencies dependenciesFor(FakeBackend& backend) {
    return ReviewController::Dependencies{
        .submit =
            [&backend](application::PlaybackCommand command) {
                backend.submitted.push_back(std::move(command));
                return application::PortSubmitResult::Accepted;
            },
        .snapshot = [&backend]() -> std::shared_ptr<const application::SessionSnapshot> {
            return std::make_shared<const application::SessionSnapshot>(backend.currentSnapshot);
        },
        .takeCompletedCommands = [&backend] { return std::exchange(backend.terminals, {}); },
        .decoderBackendStates = [] { return std::vector<ReviewController::DecoderBackendState>{}; },
        .eventDriven = false,
    };
}

// Repository double: lets tests hand back ready-made results or failures, and counts calls so
// stale-delivery tests can wait until every worker has actually finished reading.
class FakeProtocolRepository final : public application::IInterpolationProtocolRepository {
public:
    std::function<application::InterpolationProtocolIoResult(const std::filesystem::path&)>
        behavior;
    mutable std::atomic<int> calls{0};

    [[nodiscard]] application::InterpolationProtocolIoResult
    load(const std::filesystem::path& path) const override {
        ++calls;
        return behavior ? behavior(path) : application::InterpolationProtocolIoResult{};
    }
};

[[nodiscard]] application::InterpolationProtocolIoResult protocolResult(const std::string& json) {
    return persistence::InterpolationProtocolRepository::decodeDocument(json);
}

[[nodiscard]] QString
writeManifest(const QTemporaryDir& dir, const QString& name, const char* const json) {
    QString path = dir.path() + QStringLiteral("/") + name;
    QFile file{path};
    if (!file.open(QIODevice::WriteOnly)) {
        return {};
    }
    file.write(json);
    return path;
}

// 60->20->60 decimation: R0 G1 G2 R3 G4 G5 R6 G7 G8 R9 (GT 10, stride 3).
constexpr char kStrideThreeManifest[] =
    R"({"schema_version":1,"kind":"decimation","gt_frame_count":10,"stride":3,"phase":0,)"
    R"("tail_policy":"trim"})";

// 60->30->60 decimation: R0 G1 R2 G3 R4 G5 R6 (GT 7, stride 2).
constexpr char kStrideTwoManifest[] =
    R"({"schema_version":1,"kind":"decimation","gt_frame_count":7,"stride":2,"phase":0,)"
    R"("tail_policy":"trim"})";

class InterpolationProtocolControllerTests : public ::testing::Test {
protected:
    void SetUp() override {
        ensureCoreApplication();
        ASSERT_TRUE(tempDir_.isValid());
    }

    QTemporaryDir tempDir_;
};

TEST_F(InterpolationProtocolControllerTests, LoadThenNextGeneratedFrameSeeksThroughCoordinator) {
    FakeBackend backend;
    backend.currentSnapshot = readySnapshot(2, 10U);
    ReviewController review{dependenciesFor(backend)};
    FakeProtocolRepository repository;
    repository.behavior = [](const std::filesystem::path&) {
        return protocolResult(kStrideThreeManifest);
    };
    InterpolationProtocolController controller{&repository};
    controller.setReviewController(&review);

    const QString manifestPath = writeManifest(tempDir_, "protocol.json", kStrideThreeManifest);
    ASSERT_FALSE(manifestPath.isEmpty());
    ASSERT_TRUE(controller.loadProtocol(QUrl::fromLocalFile(manifestPath)));
    ASSERT_TRUE(waitUntil([&controller] { return controller.protocolLoaded(); }));
    EXPECT_TRUE(controller.lastError().isEmpty());

    // Frame 2 is the second generated position (GT inputs 0..3, ratio 2/3); displayed as 1-based,
    // i.e. inputs 1 and 4 - the same index base the retained/missing badges use.
    EXPECT_EQ(controller.currentOriginKind(), QStringLiteral("generated"));
    EXPECT_TRUE(controller.currentOriginBadge().contains(QStringLiteral("生成帧")));
    EXPECT_TRUE(controller.currentOriginBadge().contains(QStringLiteral("输入 GT 1/4")));
    EXPECT_TRUE(controller.canGoNextGenerated());
    EXPECT_TRUE(controller.canGoPreviousGenerated());

    ASSERT_TRUE(controller.nextGeneratedFrame());
    ASSERT_EQ(backend.submitted.size(), 1U);
    const auto& command = backend.submitted.back();
    const auto* seek = std::get_if<application::SeekFrameCommand>(&command);
    ASSERT_NE(seek, nullptr);
    EXPECT_EQ(seek->frameId, domain::FrameId{4});
    // Command identity rides the existing chain: a fresh command id and the session epoch.
    EXPECT_GE(seek->context.commandId.value(), 1U);
}

TEST_F(InterpolationProtocolControllerTests, NavigationRunsBothDirectionsAndStopsAtEdges) {
    FakeBackend backend;
    backend.currentSnapshot = readySnapshot(9, 10U);
    ReviewController review{dependenciesFor(backend)};
    FakeProtocolRepository repository;
    repository.behavior = [](const std::filesystem::path&) {
        return protocolResult(kStrideThreeManifest);
    };
    InterpolationProtocolController controller{&repository};
    controller.setReviewController(&review);
    ASSERT_TRUE(controller.loadProtocol(
        QUrl::fromLocalFile(writeManifest(tempDir_, "p.json", kStrideThreeManifest))));
    ASSERT_TRUE(waitUntil([&controller] { return controller.protocolLoaded(); }));

    // R9: retained, no generated frame after it, one before it.
    EXPECT_FALSE(controller.canGoNextGenerated());
    ASSERT_TRUE(controller.canGoPreviousGenerated());
    EXPECT_FALSE(controller.nextGeneratedFrame());
    EXPECT_TRUE(backend.submitted.empty());
    ASSERT_TRUE(controller.previousGeneratedFrame());
    ASSERT_EQ(backend.submitted.size(), 1U);
    const auto* seek = std::get_if<application::SeekFrameCommand>(&backend.submitted.back());
    ASSERT_NE(seek, nullptr);
    EXPECT_EQ(seek->frameId, domain::FrameId{8});

    // Move to frame 1 (first generated): nothing before it, next is 2.
    backend.currentSnapshot = readySnapshot(1, 10U);
    ASSERT_TRUE(waitUntil([&review] { return review.currentFrame() == 1; }));
    EXPECT_FALSE(controller.canGoPreviousGenerated());
    EXPECT_TRUE(controller.canGoNextGenerated());
    EXPECT_FALSE(controller.previousGeneratedFrame());
    ASSERT_TRUE(controller.nextGeneratedFrame());
    seek = std::get_if<application::SeekFrameCommand>(&backend.submitted.back());
    ASSERT_NE(seek, nullptr);
    EXPECT_EQ(seek->frameId, domain::FrameId{2});
}

TEST_F(InterpolationProtocolControllerTests, WithoutProtocolNavigationIsInert) {
    FakeBackend backend;
    backend.currentSnapshot = readySnapshot(2, 10U);
    ReviewController review{dependenciesFor(backend)};
    FakeProtocolRepository repository;
    InterpolationProtocolController controller{&repository};
    controller.setReviewController(&review);

    EXPECT_FALSE(controller.protocolLoaded());
    EXPECT_FALSE(controller.canGoNextGenerated());
    EXPECT_FALSE(controller.canGoPreviousGenerated());
    EXPECT_FALSE(controller.nextGeneratedFrame());
    EXPECT_FALSE(controller.previousGeneratedFrame());
    EXPECT_EQ(controller.currentOriginKind(), QString{});
    EXPECT_EQ(controller.originKindAtCurrentFrame(), QString{});
    EXPECT_TRUE(backend.submitted.empty());
}

TEST_F(InterpolationProtocolControllerTests, ClearProtocolRollsBackToPlainReview) {
    FakeBackend backend;
    backend.currentSnapshot = readySnapshot(2, 10U);
    ReviewController review{dependenciesFor(backend)};
    FakeProtocolRepository repository;
    repository.behavior = [](const std::filesystem::path&) {
        return protocolResult(kStrideThreeManifest);
    };
    InterpolationProtocolController controller{&repository};
    controller.setReviewController(&review);
    ASSERT_TRUE(controller.loadProtocol(
        QUrl::fromLocalFile(writeManifest(tempDir_, "p.json", kStrideThreeManifest))));
    ASSERT_TRUE(waitUntil([&controller] { return controller.protocolLoaded(); }));

    controller.clearProtocol();
    EXPECT_FALSE(controller.protocolLoaded());
    EXPECT_FALSE(controller.canGoNextGenerated());
    EXPECT_FALSE(controller.nextGeneratedFrame());
    EXPECT_EQ(controller.currentOriginKind(), QString{});
    EXPECT_TRUE(controller.statusText().contains(QStringLiteral("无协议")));
    EXPECT_TRUE(backend.submitted.empty());
}

TEST_F(InterpolationProtocolControllerTests, FailedLoadKeepsTheProtocolAlreadyInPlace) {
    FakeBackend backend;
    backend.currentSnapshot = readySnapshot(2, 10U);
    ReviewController review{dependenciesFor(backend)};
    FakeProtocolRepository repository;
    bool failNext = false;
    repository.behavior = [&failNext](const std::filesystem::path&) {
        if (failNext) {
            application::InterpolationProtocolIoResult result;
            result.error = "interpolation-protocol document is not valid UTF-8 JSON.";
            return result;
        }
        return protocolResult(kStrideThreeManifest);
    };
    InterpolationProtocolController controller{&repository};
    controller.setReviewController(&review);
    ASSERT_TRUE(controller.loadProtocol(
        QUrl::fromLocalFile(writeManifest(tempDir_, "p.json", kStrideThreeManifest))));
    ASSERT_TRUE(waitUntil([&controller] { return controller.protocolLoaded(); }));

    failNext = true;
    ASSERT_TRUE(
        controller.loadProtocol(QUrl::fromLocalFile(writeManifest(tempDir_, "p.json", "bad"))));
    ASSERT_TRUE(waitUntil([&controller] { return controller.lastError().length() > 0; }));
    EXPECT_TRUE(controller.lastError().contains(QStringLiteral("not valid UTF-8 JSON")))
        << controller.lastError().toStdString();
    // Transactional: the failed import did not clear the working protocol.
    EXPECT_TRUE(controller.protocolLoaded());
    EXPECT_TRUE(controller.canGoNextGenerated());
}

TEST_F(InterpolationProtocolControllerTests, SupersededLoadResultIsDropped) {
    FakeBackend backend;
    backend.currentSnapshot = readySnapshot(2, 10U);
    ReviewController review{dependenciesFor(backend)};
    FakeProtocolRepository repository;
    repository.behavior = [](const std::filesystem::path& path) {
        const std::string name = path.filename().string();
        return protocolResult(name.find("stride2") != std::string::npos ? kStrideTwoManifest
                                                                        : kStrideThreeManifest);
    };
    InterpolationProtocolController controller{&repository};
    controller.setReviewController(&review);
    const QString first = writeManifest(tempDir_, "stride2.json", kStrideTwoManifest);
    const QString second = writeManifest(tempDir_, "stride3.json", kStrideThreeManifest);
    ASSERT_FALSE(first.isEmpty());
    ASSERT_FALSE(second.isEmpty());

    // Two rapid loads: whichever worker finishes first, only the newest request id may deliver.
    ASSERT_TRUE(controller.loadProtocol(QUrl::fromLocalFile(first)));
    ASSERT_TRUE(controller.loadProtocol(QUrl::fromLocalFile(second)));
    ASSERT_TRUE(waitUntil(
        [&controller] { return controller.statusText().contains(QStringLiteral("10 个输出位")); }));
    // Wait until both workers have finished reading, then drain any trailing stale delivery
    // before the controller goes away.
    ASSERT_TRUE(waitUntil([&repository] { return repository.calls.load() >= 2; }));
    for (int spin = 0; spin < 20; ++spin) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        QThread::msleep(1U);
    }
    EXPECT_TRUE(controller.lastError().isEmpty());
}

TEST_F(InterpolationProtocolControllerTests, ReportsOriginKindsForExplicitManifest) {
    FakeBackend backend;
    ReviewController review{dependenciesFor(backend)};
    FakeProtocolRepository repository;
    repository.behavior = [](const std::filesystem::path&) {
        return protocolResult(R"({
            "schema_version": 1,
            "kind": "explicit",
            "gt_frame_count": 5,
            "origins": [
                {"type": "retained", "gt": 0},
                {"type": "unknown"},
                {"type": "missing", "expected": null},
                {"type": "missing", "expected": 2},
                {"type": "generated", "input0": 2, "input1": 4, "ratio": [1, 2]}
            ]
        })");
    };
    InterpolationProtocolController controller{&repository};
    controller.setReviewController(&review);
    ASSERT_TRUE(controller.loadProtocol(QUrl::fromLocalFile(
        writeManifest(tempDir_, "explicit.json", R"({"schema_version":1,"kind":"explicit"})"))));
    ASSERT_TRUE(waitUntil([&controller] { return controller.protocolLoaded(); }));

    struct Case {
        qint64 frame;
        QString kind;
        QString badgeMarker;
    };
    const Case cases[] = {
        {0, QStringLiteral("retained"), QStringLiteral("保留帧 · GT 1")},
        {1, QStringLiteral("unknown"), QStringLiteral("未知来源")},
        {2, QStringLiteral("missing"), QStringLiteral("缺失帧")},
        {3, QStringLiteral("missing"), QStringLiteral("缺失帧 · 预期 GT 3")},
        {4, QStringLiteral("generated"), QStringLiteral("比例 1/2")},
        // Frame past the protocol's five positions is reported as out of protocol, not guessed.
        {7, QString{}, QStringLiteral("当前帧不在协议内")},
    };
    for (const Case& entry : cases) {
        backend.currentSnapshot = readySnapshot(entry.frame, 10U);
        ASSERT_TRUE(waitUntil([&review, &entry] { return review.currentFrame() == entry.frame; }));
        EXPECT_EQ(controller.currentOriginKind(), entry.kind)
            << "frame " << entry.frame << " badge "
            << controller.currentOriginBadge().toStdString();
        EXPECT_TRUE(controller.currentOriginBadge().contains(entry.badgeMarker))
            << "frame " << entry.frame << " badge "
            << controller.currentOriginBadge().toStdString();
    }
    EXPECT_EQ(controller.originKindAtCurrentFrame(), controller.currentOriginKind());
}

TEST_F(InterpolationProtocolControllerTests, OriginSummariesInRangeProjectEveryProvenanceKind) {
    FakeBackend backend;
    ReviewController review{dependenciesFor(backend)};
    FakeProtocolRepository repository;
    repository.behavior = [](const std::filesystem::path&) {
        return protocolResult(R"({
            "schema_version": 1,
            "kind": "explicit",
            "gt_frame_count": 5,
            "origins": [
                {"type": "retained", "gt": 0},
                {"type": "unknown"},
                {"type": "missing", "expected": null},
                {"type": "missing", "expected": 2},
                {"type": "generated", "input0": 2, "input1": 4, "ratio": [1, 2]}
            ]
        })");
    };
    InterpolationProtocolController controller{&repository};
    controller.setReviewController(&review);
    ASSERT_TRUE(controller.loadProtocol(QUrl::fromLocalFile(
        writeManifest(tempDir_, "explicit.json", R"({"schema_version":1,"kind":"explicit"})"))));
    ASSERT_TRUE(waitUntil([&controller] { return controller.protocolLoaded(); }));

    // Frames 0..4 carry the five origins; 5..7 sit past the protocol's positions and must stay
    // "outside" so the strip can still align its cells with per-side presentation facts.
    const QVariantList summaries = controller.originSummariesInRange(0, 7);
    ASSERT_EQ(summaries.size(), 8);
    const QVariantMap retained = summaries.at(0).toMap();
    EXPECT_EQ(retained.value(QStringLiteral("kind")).toString(), QStringLiteral("retained"));
    EXPECT_EQ(retained.value(QStringLiteral("frame")).toLongLong(), 0);
    EXPECT_EQ(retained.value(QStringLiteral("displayFrame")).toLongLong(), 1);
    EXPECT_EQ(retained.value(QStringLiteral("displayGtFrame")).toLongLong(), 1);
    const QVariantMap unknown = summaries.at(1).toMap();
    EXPECT_EQ(unknown.value(QStringLiteral("kind")).toString(), QStringLiteral("unknown"));
    EXPECT_FALSE(unknown.contains(QStringLiteral("displayGtFrame")));
    const QVariantMap missingWithoutExpected = summaries.at(2).toMap();
    EXPECT_EQ(missingWithoutExpected.value(QStringLiteral("kind")).toString(),
              QStringLiteral("missing"));
    EXPECT_EQ(missingWithoutExpected.value(QStringLiteral("displayExpectedGtFrame")).toLongLong(),
              -1);
    const QVariantMap missingWithExpected = summaries.at(3).toMap();
    EXPECT_EQ(missingWithExpected.value(QStringLiteral("displayExpectedGtFrame")).toLongLong(), 3);
    const QVariantMap generated = summaries.at(4).toMap();
    EXPECT_EQ(generated.value(QStringLiteral("kind")).toString(), QStringLiteral("generated"));
    EXPECT_EQ(generated.value(QStringLiteral("displayInput0Frame")).toLongLong(), 3);
    EXPECT_EQ(generated.value(QStringLiteral("displayInput1Frame")).toLongLong(), 5);
    EXPECT_EQ(generated.value(QStringLiteral("ratioNumerator")).toLongLong(), 1);
    EXPECT_EQ(generated.value(QStringLiteral("ratioDenominator")).toLongLong(), 2);
    for (qsizetype index = 5; index < summaries.size(); ++index) {
        EXPECT_EQ(summaries.at(index).toMap().value(QStringLiteral("kind")).toString(),
                  QStringLiteral("outside"))
            << "frame index " << index;
    }

    // A negative start clamps to the first output position instead of widening the range.
    const QVariantList clamped = controller.originSummariesInRange(-3, 1);
    ASSERT_EQ(clamped.size(), 2);
    EXPECT_EQ(clamped.front().toMap().value(QStringLiteral("frame")).toLongLong(), 0);

    // Without a protocol the strip is not blocked: no badges, empty list.
    controller.clearProtocol();
    EXPECT_TRUE(controller.originSummariesInRange(0, 4).isEmpty());
}

TEST_F(InterpolationProtocolControllerTests, RejectsUnusableLoadRequests) {
    FakeBackend backend;
    ReviewController review{dependenciesFor(backend)};
    FakeProtocolRepository repository;
    InterpolationProtocolController controller{&repository};
    controller.setReviewController(&review);

    EXPECT_FALSE(controller.loadProtocol(QUrl{QStringLiteral("http://example.com/p.json")}));
    EXPECT_TRUE(controller.lastError().contains(QStringLiteral("local file")));
    EXPECT_FALSE(controller.loadProtocol(QUrl{}));
    EXPECT_TRUE(backend.submitted.empty());

    InterpolationProtocolController repositoryless{nullptr};
    EXPECT_FALSE(repositoryless.loadProtocol(
        QUrl::fromLocalFile(writeManifest(tempDir_, "p.json", kStrideThreeManifest))));
    EXPECT_TRUE(repositoryless.lastError().contains(QStringLiteral("adapter missing")));
}

} // namespace
} // namespace dvs::ui
