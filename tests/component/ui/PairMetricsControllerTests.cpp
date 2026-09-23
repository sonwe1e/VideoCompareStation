#include "dvs/application/ComparisonMetrics.h"
#include "dvs/domain/ComparisonValidator.h"
#include "dvs/ui/PairMetricsController.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
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
    static char applicationName[] = "PairMetricsControllerTests";
    static char* arguments[] = {applicationName, nullptr};
    static QCoreApplication application{argumentCount, arguments};
    static_cast<void>(application);
}

void processUntil(const std::function<bool()>& predicate, const int timeoutMilliseconds) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (elapsed.elapsed() > timeoutMilliseconds) {
            break;
        }
    }
}

[[nodiscard]] domain::ComparisonSource makeSource(const domain::SourceId id,
                                                  const std::uint32_t width,
                                                  const std::uint32_t height,
                                                  const std::int64_t frameCount) {
    auto rate = domain::RationalRate::create(30, 1);
    EXPECT_TRUE(rate.hasValue());
    domain::MediaDescriptor descriptor{
        .normalizedPath = "source-" + std::to_string(id) + ".mp4",
        .extent = domain::MediaExtent{.width = width, .height = height},
        .frameRate = std::move(rate).value(),
        .frameCount = domain::FrameCountInfo{.value = frameCount,
                                             .origin = domain::FrameCountOrigin::kReported},
        .duration = domain::MediaTime{frameCount * 1'000'000 / 30},
        .codecId = "h264",
        .pixelFormatId = "yuv420p",
        .bitDepth = 8,
        .decodeCapabilities =
            domain::DecodeCapabilities{.softwareDecode = true, .d3d11VaDecode = false},
        .timingConfidence = domain::TimingConfidence::kDeclaredCfr,
    };
    return domain::ComparisonSource{.id = id,
                                    .role = domain::ComparisonRole::kPrediction,
                                    .descriptor = std::move(descriptor),
                                    .displayName = "source-" + std::to_string(id)};
}

class FakePairMetricsService final : public application::IPairMetricsService {
public:
    application::PortSubmitResult
    submit(const application::PairMetricsRequest& request,
           std::shared_ptr<application::IPairMetricsSink> sink) override {
        requests.push_back(request);
        sinks.push_back(std::move(sink));
        return application::PortSubmitResult::Accepted;
    }

    void cancel(const application::PlaybackRequestContext& context) noexcept override {
        static_cast<void>(context);
        ++cancelCount;
    }

    [[nodiscard]] application::PairMetricsBatch
    makeBatch(const application::PairMetricsRequest& request,
              std::vector<application::PairMetricsSample> samples,
              const bool finalBatch = true) const {
        application::PairMetricsBatch batch{
            .context = request.context,
            .sources = request.sources,
            .alignmentRevision = request.alignmentRevision,
            .mismatchThreshold = request.mismatchThreshold,
            .metricId = std::string{application::kRgbAbsoluteMetricId},
            .samples = std::move(samples),
            .finalBatch = finalBatch,
        };
        return batch;
    }

    void deliver(const application::PairMetricsBatch& batch) {
        ASSERT_FALSE(sinks.empty());
        sinks.back()->onPairMetricsBatch(batch);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    }

    std::vector<application::PairMetricsRequest> requests;
    std::vector<std::shared_ptr<application::IPairMetricsSink>> sinks;
    int cancelCount = 0;
};

class PairMetricsControllerTests : public ::testing::Test {
protected:
    void SetUp() override {
        ensureCoreApplication();
        snapshot_ = std::make_shared<application::SessionSnapshot>();
        controller_ = std::make_unique<PairMetricsController>(PairMetricsController::Dependencies{
            .snapshot = [this] { return snapshot_; },
            .service = &service_,
        });
    }

    void installTwoSourceSession(const std::int64_t displayedFrame = 3,
                                 const std::uint64_t frameCount = 12U) {
        const auto validation = domain::ComparisonValidator::validate(
            {makeSource(domain::SourceId{1}, 320, 180, frameCount),
             makeSource(domain::SourceId{2}, 320, 180, frameCount)});
        ASSERT_TRUE(validation.hasValue());
        snapshot_ = std::make_shared<application::SessionSnapshot>();
        snapshot_->sessionId = domain::SessionId{1};
        snapshot_->sessionEpoch = domain::SessionEpoch{1};
        snapshot_->playbackGeneration = domain::PlaybackGeneration{1};
        snapshot_->displayedFrame = domain::FrameId{displayedFrame};
        snapshot_->canonicalFrameCount = frameCount;
        snapshot_->alignmentRevision = 4U;
        snapshot_->alignmentOffsets = {application::SourceFrameOffset{domain::SourceId{1}, 0},
                                       application::SourceFrameOffset{domain::SourceId{2}, 0}};
        snapshot_->activeComparisonPair =
            domain::ComparisonPair{domain::SourceId{1}, domain::SourceId{2}};
        snapshot_->validatedComparison = std::make_shared<const domain::ValidatedComparisonSet>(
            std::move(validation).value().set);
    }

    FakePairMetricsService service_;
    std::shared_ptr<application::SessionSnapshot> snapshot_;
    std::unique_ptr<PairMetricsController> controller_;
};

TEST_F(PairMetricsControllerTests, UnavailableWithoutActivePair) {
    controller_->refresh();
    processUntil([&] { return false; }, 30);
    EXPECT_FALSE(controller_->available());
    EXPECT_TRUE(service_.requests.empty());
}

TEST_F(PairMetricsControllerTests, SubmitsCurrentFrameRequestWhenLaneDisabled) {
    installTwoSourceSession(3, 12U);
    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);
    ASSERT_EQ(service_.requests.size(), 1U);
    const application::PairMetricsRequest& request = service_.requests.front();
    ASSERT_EQ(request.sources.size(), 2U);
    EXPECT_EQ(request.sources[0].id, domain::SourceId{1});
    EXPECT_EQ(request.sources[1].id, domain::SourceId{2});
    EXPECT_EQ(request.firstFrame, domain::FrameId{3});
    EXPECT_EQ(request.lastFrame, domain::FrameId{3});
    EXPECT_EQ(request.alignmentRevision, 4U);
    EXPECT_TRUE(controller_->sampling());
}

TEST_F(PairMetricsControllerTests, LaneEnabledWidensSamplingWindow) {
    installTwoSourceSession(3, 12U);
    controller_->setLaneEnabled(true);
    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);
    ASSERT_EQ(service_.requests.size(), 1U);
    EXPECT_EQ(service_.requests.front().firstFrame, domain::FrameId{0});
    EXPECT_EQ(service_.requests.front().lastFrame, domain::FrameId{11});
}

TEST_F(PairMetricsControllerTests, ValidBatchUpdatesReadoutAndClearsSampling) {
    installTwoSourceSession(3, 12U);
    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);
    ASSERT_EQ(service_.requests.size(), 1U);

    application::PairMetricsSample sample;
    sample.canonicalFrameId = domain::FrameId{3};
    sample.comparable = true;
    sample.metrics.mae = 1.5;
    sample.metrics.mse = 4.0;
    sample.metrics.psnrDb = 42.0;
    sample.metrics.maxAbsError = 12U;
    sample.metrics.mismatchRatio = 0.25;
    sample.metrics.mismatchPixels = 14400U;
    sample.metrics.pixelCount = 57600U;
    service_.deliver(service_.makeBatch(service_.requests.front(), {sample}));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    EXPECT_TRUE(controller_->available());
    EXPECT_FALSE(controller_->sampling());
    EXPECT_TRUE(controller_->hasCurrentSample());
    EXPECT_TRUE(controller_->currentComparable());
    EXPECT_DOUBLE_EQ(controller_->currentMae(), 1.5);
    EXPECT_DOUBLE_EQ(controller_->currentPsnrDb(), 42.0);
    EXPECT_DOUBLE_EQ(controller_->currentMaxAbsError(), 12.0);
    EXPECT_DOUBLE_EQ(controller_->currentMismatchRatio(), 0.25);
    EXPECT_EQ(controller_->currentMismatchPixels(), 14400U);
    EXPECT_EQ(controller_->currentPixelCount(), 57600U);
    EXPECT_EQ(controller_->metricId().toStdString(),
              std::string{application::kRgbAbsoluteMetricId});
    EXPECT_EQ(controller_->sampleCount(), 1);
    EXPECT_EQ(controller_->sampleFirstFrame(), 3);
    EXPECT_EQ(controller_->sampleLastFrame(), 3);
}

TEST_F(PairMetricsControllerTests, StaleBatchIsDroppedAfterEpochChange) {
    installTwoSourceSession(3, 12U);
    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);
    ASSERT_EQ(service_.requests.size(), 1U);
    const application::PairMetricsRequest staleRequest = service_.requests.front();

    // A new session epoch supersedes the in-flight request; its late batch must be dropped.
    installTwoSourceSession(4, 12U);
    snapshot_->sessionEpoch = domain::SessionEpoch{2};
    controller_->refresh();
    processUntil([&] { return service_.requests.size() >= 2U; }, 1000);

    application::PairMetricsSample sample;
    sample.canonicalFrameId = domain::FrameId{3};
    sample.comparable = true;
    sample.metrics.mae = 9.0;
    service_.deliver(service_.makeBatch(staleRequest, {sample}));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    EXPECT_EQ(controller_->sampleCount(), 0);
    EXPECT_FALSE(controller_->hasCurrentSample());
}

TEST_F(PairMetricsControllerTests, ThresholdChangeClearsCacheAndResamples) {
    installTwoSourceSession(3, 12U);
    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);
    application::PairMetricsSample sample;
    sample.canonicalFrameId = domain::FrameId{3};
    sample.comparable = true;
    sample.metrics.mae = 2.0;
    service_.deliver(service_.makeBatch(service_.requests.front(), {sample}));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    ASSERT_EQ(controller_->sampleCount(), 1);

    controller_->setThreshold(12);
    processUntil([&] { return service_.requests.size() >= 2U; }, 1000);

    EXPECT_EQ(controller_->sampleCount(), 0);
    ASSERT_GE(service_.requests.size(), 2U);
    EXPECT_EQ(service_.requests.back().mismatchThreshold, 12U);
}

TEST_F(PairMetricsControllerTests, PeakFramesAndSamplePointsProjectCache) {
    installTwoSourceSession(0, 12U);
    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);

    std::vector<application::PairMetricsSample> samples;
    for (std::int64_t frame = 0; frame < 12; ++frame) {
        application::PairMetricsSample sample;
        sample.canonicalFrameId = domain::FrameId{frame};
        sample.comparable = true;
        sample.metrics.mae = frame == 6 ? 8.0 : 1.0;
        samples.push_back(sample);
    }
    service_.deliver(service_.makeBatch(service_.requests.front(), std::move(samples)));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    EXPECT_EQ(controller_->sampleCount(), 12);
    EXPECT_DOUBLE_EQ(controller_->maeAt(6), 8.0);
    EXPECT_TRUE(controller_->comparableAt(6));
    EXPECT_DOUBLE_EQ(controller_->maeAt(999), -1.0);
    EXPECT_FALSE(controller_->comparableAt(999));
    EXPECT_DOUBLE_EQ(controller_->sampleMaxMae(), 8.0);

    const QVariantList peaks = controller_->peakFrames(3, 2.0);
    ASSERT_EQ(peaks.size(), 1);
    EXPECT_EQ(peaks.first().toMap().value("frame").toLongLong(), 6);

    const QVariantList points = controller_->samplePoints(4);
    EXPECT_EQ(points.size(), 4);
    // The strongest sample inside each bucket must survive the reduction.
    bool strongestProjected = false;
    for (const QVariant& point : points) {
        if (point.toMap().value("frame").toLongLong() == 6) {
            strongestProjected = true;
            EXPECT_DOUBLE_EQ(point.toMap().value("mae").toDouble(), 8.0);
        }
    }
    EXPECT_TRUE(strongestProjected);
}

} // namespace
} // namespace dvs::ui
