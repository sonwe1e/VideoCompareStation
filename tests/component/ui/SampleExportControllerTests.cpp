#include "dvs/application/SampleExport.h"
#include "dvs/application/SampleExportWriter.h"
#include "dvs/application/SessionSnapshot.h"
#include "dvs/domain/ComparisonValidator.h"
#include "dvs/domain/InterpolationProtocol.h"
#include "dvs/ui/SampleExportController.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <stdexcept>
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
    static int argc = 1;
    static char appName[] = "SampleExportControllerTests";
    static char* argv[] = {appName, nullptr};
    static QCoreApplication application{argc, argv};
    static_cast<void>(application);
}

[[nodiscard]] std::shared_ptr<application::SessionSnapshot> createTestSnapshot() {
    auto snapshot = std::make_shared<application::SessionSnapshot>();
    snapshot->sessionId = domain::SessionId{11U};
    snapshot->sessionEpoch = domain::SessionEpoch{5U};
    snapshot->canonicalFrameCount = 3U;

    const auto rate = domain::RationalRate::create(25, 1).value();
    domain::MediaDescriptor gtDesc{.duration = domain::MediaTime{120'000}};
    gtDesc.normalizedPath = L"C:/media/gt.mp4";
    gtDesc.extent = {1920U, 1080U};
    gtDesc.frameRate = rate;
    gtDesc.frameCount = {3, domain::FrameCountOrigin::kIndexed};
    gtDesc.duration = domain::MediaTime{120'000};
    gtDesc.codecId = "h264";
    gtDesc.pixelFormatId = "yuv420p";
    gtDesc.bitDepth = 8;
    gtDesc.timingConfidence = domain::TimingConfidence::kVerifiedCfr;

    auto predDesc = gtDesc;
    predDesc.normalizedPath = L"C:/media/candidate.mp4";

    auto validated = domain::ComparisonValidator::validate(
        {{1U, domain::ComparisonRole::kReference, gtDesc, "GT Reference"},
         {2U, domain::ComparisonRole::kPrediction, predDesc, "Model Candidate"}});
    if (!validated) {
        throw std::runtime_error("Failed to build test validated comparison set");
    }
    snapshot->validatedComparison =
        std::make_shared<const domain::ValidatedComparisonSet>(std::move(validated.value().set));
    snapshot->sources = {
        application::SessionSourceView{
            .sourceId = 1U,
            .role = domain::ComparisonRole::kReference,
            .displayName = "GT Reference",
        },
        application::SessionSourceView{
            .sourceId = 2U,
            .role = domain::ComparisonRole::kPrediction,
            .displayName = "Model Candidate",
        },
    };
    snapshot->displayedFrame = domain::FrameId{1};
    return snapshot;
}

[[nodiscard]] domain::InterpolationProtocol createTestProtocol() {
    std::vector<domain::OutputFrameOrigin> origins{
        domain::RetainedFrame{domain::GtFrameIndex{0}},
        domain::GeneratedFrame{domain::GtFrameIndex{0}, domain::GtFrameIndex{2}, 1, 2},
        domain::RetainedFrame{domain::GtFrameIndex{2}},
    };
    auto protocolRes =
        domain::InterpolationProtocol::create(3, domain::MappingOrigin::kImportedManifest, origins);
    if (!protocolRes) {
        throw std::runtime_error("Failed to build test interpolation protocol");
    }
    return protocolRes.value();
}

class RecordingSampleExporter final : public application::ISampleExporter {
public:
    application::SampleExportReport perform(const application::SampleExportJob& job,
                                            const std::atomic_bool& cancelRequested) override {
        ++performCalls;
        capturedJob = job;
        if (cancelRequested.load()) {
            return {.requestId = job.requestId,
                    .outcome = application::SampleExportOutcome::kCanceled,
                    .technicalDetail = "canceled"};
        }
        if (outcome == application::SampleExportOutcome::kCompleted) {
            return {.requestId = job.requestId,
                    .outcome = application::SampleExportOutcome::kCompleted,
                    .publishedMembers = {job.outputDirectory / "input0.png",
                                         job.outputDirectory / "gt.png",
                                         job.outputDirectory / "input1.png",
                                         job.outputDirectory / "prediction.png"},
                    .manifestPath = job.outputDirectory / "manifest.json"};
        }
        return {.requestId = job.requestId, .outcome = outcome, .technicalDetail = failureDetail};
    }

    application::SampleExportOutcome outcome = application::SampleExportOutcome::kCompleted;
    std::string failureDetail;
    std::atomic_int performCalls{0};
    std::optional<application::SampleExportJob> capturedJob;
};

class SampleExportControllerTests : public testing::Test {
protected:
    void SetUp() override {
        ensureCoreApplication();
        snapshot = createTestSnapshot();
        protocol = createTestProtocol();
        exporter = std::make_shared<RecordingSampleExporter>();
    }

    [[nodiscard]] SampleExportController::Dependencies dependencies() {
        return {
            .snapshot = [this] { return snapshot; },
            .exporter = exporter,
            .protocol = [this]() -> const std::optional<domain::InterpolationProtocol>& {
                return protocol;
            },
            .activeCropRect = [this] { return activeCrop; },
        };
    }

    [[nodiscard]] bool waitForExport(SampleExportController& controller) {
        QElapsedTimer timer;
        timer.start();
        while (controller.busy() && timer.elapsed() < 2000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(5);
        }
        return !controller.busy();
    }

    std::shared_ptr<application::SessionSnapshot> snapshot;
    std::optional<domain::InterpolationProtocol> protocol;
    std::optional<application::SampleCropRect> activeCrop;
    std::shared_ptr<RecordingSampleExporter> exporter;
};

TEST_F(SampleExportControllerTests, RejectsExportWhenProtocolOrSnapshotMissing) {
    protocol.reset();
    SampleExportController controller{dependencies()};

    EXPECT_FALSE(controller.canExport());
    EXPECT_FALSE(controller.canExportAtFrame(1));
    const auto summary = controller.planSummaryAtFrame(1);
    EXPECT_TRUE(summary.isEmpty());
    EXPECT_FALSE(controller.exportTriplet(
        QUrl::fromLocalFile(QDir::tempPath()), QStringLiteral("sample_001"), false, 1));
    EXPECT_EQ(exporter->performCalls.load(), 0);
}

TEST_F(SampleExportControllerTests, IdentifiesGeneratedFramesAndRejectsRetainedFrames) {
    SampleExportController controller{dependencies()};

    // Frame 0 is retained -> not exportable as interpolated prediction triplet
    EXPECT_FALSE(controller.canExportAtFrame(0));

    // Frame 1 is generated between GT 0 and GT 2 -> exportable
    EXPECT_TRUE(controller.canExportAtFrame(1));
    controller.setTargetFrame(1);
    EXPECT_TRUE(controller.canExport());
    EXPECT_EQ(controller.targetFrame(), 1);

    // Frame 2 is retained -> not exportable
    EXPECT_FALSE(controller.canExportAtFrame(2));

    // Out of range -> not exportable
    EXPECT_FALSE(controller.canExportAtFrame(10));
}

TEST_F(SampleExportControllerTests, ComputesPlanSummaryAndSuggestedSampleId) {
    SampleExportController controller{dependencies()};

    const auto summary = controller.planSummaryAtFrame(1);
    EXPECT_FALSE(summary.isEmpty());
    EXPECT_EQ(summary.value(QStringLiteral("input0Frame")).toLongLong(), 0);
    EXPECT_EQ(summary.value(QStringLiteral("input0Ordinal")).toLongLong(), 0);
    EXPECT_EQ(summary.value(QStringLiteral("input1Frame")).toLongLong(), 2);
    EXPECT_EQ(summary.value(QStringLiteral("input1Ordinal")).toLongLong(), 2);
    EXPECT_EQ(summary.value(QStringLiteral("outputFrame")).toLongLong(), 1);
    EXPECT_DOUBLE_EQ(summary.value(QStringLiteral("outputRatio")).toDouble(), 0.5);
    EXPECT_TRUE(summary.value(QStringLiteral("hasGt")).toBool());
    EXPECT_FALSE(summary.value(QStringLiteral("hasCrop")).toBool());

    const QString sampleId = controller.suggestedSampleId(1);
    EXPECT_EQ(sampleId, QStringLiteral("sample_2_0.500"));
}

TEST_F(SampleExportControllerTests, PerformsAsyncExportAndDeliversReport) {
    SampleExportController controller{dependencies()};

    const QUrl targetDir = QUrl::fromLocalFile(QDir::tempPath());
    const QString sampleId = QStringLiteral("triplet_001");

    bool finishedSignalReceived = false;
    bool finishedSuccess = false;
    QString finishedMessage;

    QObject::connect(&controller,
                     &SampleExportController::exportFinished,
                     &controller,
                     [&](const bool succeeded, const QString& message) {
                         finishedSignalReceived = true;
                         finishedSuccess = succeeded;
                         finishedMessage = message;
                     });

    ASSERT_TRUE(controller.exportTriplet(targetDir, sampleId, false, 1));
    EXPECT_TRUE(controller.busy());

    EXPECT_TRUE(waitForExport(controller));
    EXPECT_FALSE(controller.busy());
    EXPECT_TRUE(finishedSignalReceived);
    EXPECT_TRUE(finishedSuccess);
    EXPECT_EQ(exporter->performCalls.load(), 1);

    ASSERT_TRUE(exporter->capturedJob.has_value());
    const auto& job = exporter->capturedJob.value();
    EXPECT_EQ(job.sampleId, "triplet_001");
    EXPECT_EQ(job.plan.input0GtFrame.value(), 0);
    EXPECT_EQ(job.plan.input1GtFrame.value(), 2);
    EXPECT_EQ(job.plan.targetGtFrame.value(), 1);
    EXPECT_FALSE(job.plan.cropRect.has_value());
    EXPECT_DOUBLE_EQ(controller.progress(), 1.0);
}

TEST_F(SampleExportControllerTests, HandlesRoiCropSetting) {
    activeCrop = application::SampleCropRect{100, 200, 800, 600};

    SampleExportController controller{dependencies()};

    const auto summary = controller.planSummaryAtFrame(1);
    EXPECT_TRUE(summary.value(QStringLiteral("hasCrop")).toBool());
    EXPECT_GT(summary.value(QStringLiteral("cropWidth")).toInt(), 0);
    EXPECT_GT(summary.value(QStringLiteral("cropHeight")).toInt(), 0);

    const QUrl targetDir = QUrl::fromLocalFile(QDir::tempPath());
    ASSERT_TRUE(controller.exportTriplet(targetDir, QStringLiteral("sample_cropped"), true, 1));
    EXPECT_TRUE(waitForExport(controller));

    ASSERT_TRUE(exporter->capturedJob.has_value());
    const auto& job = exporter->capturedJob.value();
    ASSERT_TRUE(job.plan.cropRect.has_value());
    EXPECT_GT(job.plan.cropRect->width, 0);
    EXPECT_GT(job.plan.cropRect->height, 0);
}

TEST_F(SampleExportControllerTests, CancellationStopsExport) {
    SampleExportController controller{dependencies()};

    const QUrl targetDir = QUrl::fromLocalFile(QDir::tempPath());
    ASSERT_TRUE(controller.exportTriplet(targetDir, QStringLiteral("sample_cancel"), false, 1));
    controller.cancelExport();

    EXPECT_TRUE(waitForExport(controller));
    EXPECT_FALSE(controller.busy());
}

} // namespace
} // namespace dvs::ui
