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
#include <limits>
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

[[nodiscard]] application::PairMetricsSample thresholdSample(const std::int64_t frame = 3) {
    const std::vector<std::uint8_t> first{0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255};
    const std::vector<std::uint8_t> second{10, 0, 0, 0, 20, 20, 20, 255, 0, 0, 0, 255};
    const auto analysis =
        domain::computeRgbAbsoluteAnalysis({first.data(), 3, 1, 12}, {second.data(), 3, 1, 12});
    EXPECT_TRUE(analysis.has_value());
    return application::PairMetricsSample{.canonicalFrameId = domain::FrameId{frame},
                                          .comparable = true,
                                          .analysis = analysis.value()};
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
    EXPECT_EQ(service_.requests.front().priorityFrame, domain::FrameId{3});
}

TEST_F(PairMetricsControllerTests, ClippedWindowPrioritizesActualPlayhead) {
    installTwoSourceSession(3, 1000U);
    controller_->setLaneEnabled(true);
    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);
    ASSERT_EQ(service_.requests.size(), 1U);
    const auto& request = service_.requests.front();
    EXPECT_EQ(request.firstFrame, domain::FrameId{0});
    EXPECT_EQ(request.lastFrame, domain::FrameId{153});
    // The clipped window's midpoint is 76, not the frame actually on screen.
    EXPECT_EQ(request.priorityFrame, domain::FrameId{3});
}

TEST_F(PairMetricsControllerTests, CachedPlayheadPrioritizesNearestMissingPosition) {
    installTwoSourceSession(3, 12U);
    controller_->setLaneEnabled(true);
    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);
    ASSERT_EQ(service_.requests.size(), 1U);
    service_.deliver(
        service_.makeBatch(service_.requests.front(),
                           {application::PairMetricsSample{.canonicalFrameId = domain::FrameId{3},
                                                           .comparable = true}}));
    controller_->refresh();
    processUntil([&] { return service_.requests.size() >= 2U; }, 1000);
    ASSERT_EQ(service_.requests.size(), 2U);
    const auto& request = service_.requests.back();
    EXPECT_EQ(request.firstFrame, domain::FrameId{0});
    EXPECT_EQ(request.lastFrame, domain::FrameId{2});
    EXPECT_EQ(request.priorityFrame, domain::FrameId{2});
}

TEST_F(PairMetricsControllerTests, ValidBatchUpdatesReadoutAndClearsSampling) {
    installTwoSourceSession(3, 12U);
    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);
    ASSERT_EQ(service_.requests.size(), 1U);

    application::PairMetricsSample sample;
    sample.canonicalFrameId = domain::FrameId{3};
    sample.comparable = true;
    sample.analysis.metrics.mae = 1.5;
    sample.analysis.metrics.mse = 4.0;
    sample.analysis.metrics.psnrDb = 42.0;
    sample.analysis.metrics.maxAbsError = 12U;
    sample.analysis.mismatchCounts[1][0] = 14400U;
    sample.analysis.metrics.pixelCount = 57600U;
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
    sample.analysis.metrics.mae = 9.0;
    service_.deliver(service_.makeBatch(staleRequest, {sample}));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    EXPECT_EQ(controller_->sampleCount(), 0);
    EXPECT_FALSE(controller_->hasCurrentSample());
}

TEST_F(PairMetricsControllerTests, ThresholdChangeReusesAnalysisWithoutResampling) {
    installTwoSourceSession();
    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);
    ASSERT_EQ(service_.requests.size(), 1U);
    const auto sample = thresholdSample();
    service_.deliver(service_.makeBatch(service_.requests.front(), {sample}));
    ASSERT_EQ(controller_->currentMismatchPixels(), 2U);
    int stateNotifications = 0;
    QObject::connect(
        controller_.get(), &PairMetricsController::stateChanged, [&] { ++stateNotifications; });
    controller_->setThreshold(12);
    EXPECT_EQ(controller_->currentMismatchPixels(), 1U);
    EXPECT_DOUBLE_EQ(controller_->currentMismatchRatio(), 1.0 / 3.0);
    EXPECT_DOUBLE_EQ(controller_->currentMae(), sample.analysis.metrics.mae);
    EXPECT_GT(stateNotifications, 0);
    controller_->setThreshold(999);
    EXPECT_EQ(controller_->threshold(), 255);
    EXPECT_EQ(controller_->currentMismatchPixels(), 0U);
    controller_->setThreshold(-1);
    EXPECT_EQ(controller_->threshold(), 0);
    EXPECT_EQ(controller_->currentMismatchPixels(), 2U);
    processUntil([&] { return service_.requests.size() > 1U; }, 250);
    EXPECT_EQ(controller_->sampleCount(), 1);
    EXPECT_EQ(service_.requests.size(), 1U);
    EXPECT_EQ(service_.cancelCount, 0);
}

TEST_F(PairMetricsControllerTests, ThresholdPolicyChangeReusesAnalysisWithoutResampling) {
    installTwoSourceSession();
    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);
    ASSERT_EQ(service_.requests.size(), 1U);
    service_.deliver(service_.makeBatch(service_.requests.front(), {thresholdSample()}));
    controller_->setThresholdPolicy(7);
    EXPECT_EQ(controller_->thresholdPolicy(), 1);
    EXPECT_EQ(controller_->currentMismatchPixels(), 2U);
    controller_->setThresholdPolicy(2);
    EXPECT_EQ(controller_->currentMismatchPixels(), 1U);
    EXPECT_EQ(controller_->thresholdPolicy(), 2);
    controller_->setThresholdPolicy(0);
    controller_->setThreshold(3);
    EXPECT_EQ(controller_->currentMismatchPixels(), 1U);
    processUntil([&] { return service_.requests.size() > 1U; }, 250);
    EXPECT_EQ(controller_->sampleCount(), 1);
    EXPECT_EQ(service_.requests.size(), 1U);
    EXPECT_EQ(service_.cancelCount, 0);
}

TEST_F(PairMetricsControllerTests, PredicateChangeAcceptsInflightAnalysisUnderNewPredicate) {
    installTwoSourceSession();
    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);
    ASSERT_EQ(service_.requests.size(), 1U);
    const auto request = service_.requests.front();
    controller_->setThresholdPolicy(0);
    controller_->setThreshold(3);
    EXPECT_TRUE(controller_->sampling());
    service_.deliver(service_.makeBatch(request, {thresholdSample()}));
    EXPECT_TRUE(controller_->hasCurrentSample());
    EXPECT_EQ(controller_->currentMismatchPixels(), 1U);
    EXPECT_FALSE(controller_->sampling());
    processUntil([&] { return service_.requests.size() > 1U; }, 250);
    EXPECT_EQ(service_.requests.size(), 1U);
    EXPECT_EQ(service_.cancelCount, 0);
}

TEST_F(PairMetricsControllerTests, AnalysisCacheEvictsDistantFramesAndKeepsCurrentReadout) {
    installTwoSourceSession();
    controller_ = std::make_unique<PairMetricsController>(PairMetricsController::Dependencies{
        .snapshot = [this] { return snapshot_; }, .service = &service_, .maximumCachedSamples = 3});
    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);
    ASSERT_EQ(service_.requests.size(), 1U);
    std::vector<application::PairMetricsSample> samples;
    for (const std::int64_t frame : {0, 2, 10}) {
        auto sample = thresholdSample(frame);
        sample.analysis.metrics.mae = frame == 10 ? 100.0 : static_cast<double>(frame + 1);
        samples.push_back(sample);
    }
    auto partial = service_.makeBatch(service_.requests.front(), samples);
    partial.finalBatch = false;
    service_.deliver(partial);
    EXPECT_DOUBLE_EQ(controller_->sampleMaxMae(), 100.0);
    auto current = thresholdSample(3);
    current.analysis.metrics.mae = 4.0;
    service_.deliver(service_.makeBatch(service_.requests.front(), {current}));
    EXPECT_EQ(controller_->sampleCount(), 3);
    EXPECT_TRUE(controller_->hasCurrentSample());
    EXPECT_DOUBLE_EQ(controller_->currentMae(), 4.0);
    EXPECT_DOUBLE_EQ(controller_->maeAt(10), -1.0);
    EXPECT_EQ(controller_->sampleFirstFrame(), 0);
    EXPECT_EQ(controller_->sampleLastFrame(), 3);
    EXPECT_DOUBLE_EQ(controller_->sampleMaxMae(), 4.0);

    snapshot_->displayedFrame = domain::FrameId{10};
    controller_->refresh();
    processUntil([&] { return service_.requests.size() >= 2U; }, 1000);
    ASSERT_EQ(service_.requests.size(), 2U);
    auto sample = thresholdSample(10);
    sample.analysis.metrics.mae = 11.0;
    service_.deliver(service_.makeBatch(service_.requests.back(), {sample}));
    EXPECT_EQ(controller_->sampleCount(), 3);
    EXPECT_EQ(controller_->sampleFirstFrame(), 2);
    EXPECT_EQ(controller_->sampleLastFrame(), 10);
    EXPECT_DOUBLE_EQ(controller_->sampleMaxMae(), 11.0);
    EXPECT_DOUBLE_EQ(controller_->currentMae(), 11.0);
}

TEST_F(PairMetricsControllerTests, ZeroCacheLimitStillRetainsOneCurrentAnalysis) {
    installTwoSourceSession();
    controller_ = std::make_unique<PairMetricsController>(PairMetricsController::Dependencies{
        .snapshot = [this] { return snapshot_; }, .service = &service_, .maximumCachedSamples = 0});
    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);
    ASSERT_EQ(service_.requests.size(), 1U);
    service_.deliver(service_.makeBatch(
        service_.requests.front(), {thresholdSample(2), thresholdSample(3), thresholdSample(4)}));
    EXPECT_EQ(controller_->sampleCount(), 1);
    EXPECT_EQ(controller_->sampleFirstFrame(), 3);
    EXPECT_TRUE(controller_->hasCurrentSample());
}

TEST_F(PairMetricsControllerTests, RequestedCacheLimitCannotExceedHardMemoryBudget) {
    installTwoSourceSession(3, 8192);
    controller_ = std::make_unique<PairMetricsController>(PairMetricsController::Dependencies{
        .snapshot = [this] { return snapshot_; },
        .service = &service_,
        .maximumCachedSamples = std::numeric_limits<std::size_t>::max()});
    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);
    ASSERT_EQ(service_.requests.size(), 1U);
    std::vector<application::PairMetricsSample> samples;
    for (std::int64_t frame = 0; frame <= 4096; ++frame) {
        samples.push_back(thresholdSample(frame));
    }
    service_.deliver(service_.makeBatch(service_.requests.front(), samples));
    EXPECT_EQ(controller_->sampleCount(), 4096);
    EXPECT_EQ(controller_->sampleFirstFrame(), 0);
    EXPECT_EQ(controller_->sampleLastFrame(), 4095);
    EXPECT_TRUE(controller_->hasCurrentSample());
}

TEST_F(PairMetricsControllerTests, InputChangesInvalidateAnalysisButPredicateChangesDoNot) {
    installTwoSourceSession();
    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);
    ASSERT_EQ(service_.requests.size(), 1U);
    service_.deliver(service_.makeBatch(service_.requests.front(), {thresholdSample()}));
    snapshot_->alignmentRevision = 5U;
    controller_->refresh();
    EXPECT_EQ(controller_->sampleCount(), 0);
    processUntil([&] { return service_.requests.size() >= 2U; }, 1000);
    ASSERT_EQ(service_.requests.size(), 2U);
    service_.deliver(service_.makeBatch(service_.requests.back(), {thresholdSample()}));
    snapshot_->activeComparisonPair = domain::ComparisonPair{2, 1};
    controller_->refresh();
    EXPECT_EQ(controller_->sampleCount(), 0);
    processUntil([&] { return service_.requests.size() >= 3U; }, 1000);
    ASSERT_EQ(service_.requests.size(), 3U);
    EXPECT_EQ(service_.requests.back().sources.front().id, 2U);
}

TEST_F(PairMetricsControllerTests, MaterialAndMappingHandlesArePartOfAnalysisIdentity) {
    installTwoSourceSession();
    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);
    ASSERT_EQ(service_.requests.size(), 1U);
    const std::vector<std::function<void()>> changes{
        [this] {
            auto first = makeSource(1U, 320, 180, 12);
            first.descriptor.normalizedPath = "new-a.mp4";
            auto comparison =
                domain::ComparisonValidator::validate({first, makeSource(2U, 320, 180, 12)});
            ASSERT_TRUE(comparison.hasValue());
            snapshot_->validatedComparison = std::make_shared<const domain::ValidatedComparisonSet>(
                std::move(comparison.value().set));
        },
        [this] {
            std::vector<domain::MediaTime> times;
            for (int frame = 0; frame < 12; ++frame) {
                times.emplace_back(frame * 33333);
            }
            auto timeline = domain::FrameTimeline::create(std::move(times));
            ASSERT_TRUE(timeline.hasValue());
            snapshot_->sourceTimelines = {
                {2, std::make_shared<const domain::FrameTimeline>(std::move(timeline.value()))}};
        },
        [this] { snapshot_->alignmentOffsets.back().frames = 1; },
        [this] {
            auto rate = domain::RationalRate::create(24, 1);
            ASSERT_TRUE(rate.hasValue());
            snapshot_->canonicalTimeline = rate.value();
        },
        [this] { snapshot_->canonicalFrameCount = 11; },
        [this] {
            snapshot_->sequenceAlignmentMaps =
                std::make_shared<const std::vector<application::SequenceAlignmentResult>>();
        }};
    for (const auto& change : changes) {
        const auto previousRequests = service_.requests.size();
        service_.deliver(service_.makeBatch(service_.requests.back(), {thresholdSample()}));
        ASSERT_EQ(controller_->sampleCount(), 1);
        change();
        controller_->refresh();
        EXPECT_EQ(controller_->sampleCount(), 0);
        processUntil([&] { return service_.requests.size() > previousRequests; }, 1000);
        ASSERT_EQ(service_.requests.size(), previousRequests + 1U);
    }
    service_.deliver(service_.makeBatch(service_.requests.back(), {thresholdSample()}));
    snapshot_->alignmentMode = application::AlignmentMode::Timestamp;
    controller_->refresh();
    EXPECT_EQ(controller_->sampleCount(), 0);
}

TEST_F(PairMetricsControllerTests, ForeignRequestResultsCannotFinishOrPoisonCurrentWork) {
    installTwoSourceSession();
    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);
    ASSERT_EQ(service_.requests.size(), 1U);
    const auto request = service_.requests.front();
    for (const bool changeGeneration : {false, true}) {
        auto foreign = request;
        if (changeGeneration) {
            foreign.context.playbackGeneration = domain::PlaybackGeneration{2};
        } else {
            foreign.context.request.requestId = domain::RequestId{999};
        }
        service_.deliver(service_.makeBatch(foreign, {thresholdSample()}));
        application::PairMetricsFailure failure{.context = foreign.context};
        failure.error.userMessageKey = "foreignFailure";
        service_.sinks.back()->onPairMetricsFailure(failure);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        EXPECT_EQ(controller_->sampleCount(), 0);
        EXPECT_TRUE(controller_->sampling());
        EXPECT_TRUE(controller_->errorKey().isEmpty());
    }
    service_.deliver(service_.makeBatch(request, {thresholdSample()}));
    EXPECT_TRUE(controller_->hasCurrentSample());
    EXPECT_FALSE(controller_->sampling());
    EXPECT_TRUE(controller_->errorKey().isEmpty());
}

TEST_F(PairMetricsControllerTests, CurrentFailureIsConsumedOnlyOnceAcrossQueuedWakes) {
    installTwoSourceSession();
    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);
    ASSERT_EQ(service_.requests.size(), 1U);
    application::PairMetricsFailure failure{.context = service_.requests.front().context};
    failure.error.userMessageKey = "currentFailure";
    service_.sinks.back()->onPairMetricsFailure(failure);
    service_.sinks.back()->onPairMetricsFailure(failure);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    EXPECT_EQ(controller_->errorKey(), QStringLiteral("currentFailure"));
    EXPECT_FALSE(controller_->sampling());
}

TEST_F(PairMetricsControllerTests, MissingAndMalformedPositionsDoNotBecomeComparable) {
    installTwoSourceSession();
    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);
    ASSERT_EQ(service_.requests.size(), 1U);
    service_.deliver(service_.makeBatch(
        service_.requests.front(),
        {application::PairMetricsSample{.canonicalFrameId = domain::FrameId{3}},
         application::PairMetricsSample{.canonicalFrameId = domain::FrameId{-1}}}));
    controller_->setThreshold(255);
    controller_->setThresholdPolicy(2);
    EXPECT_EQ(controller_->sampleCount(), 1);
    EXPECT_TRUE(controller_->hasCurrentSample());
    EXPECT_FALSE(controller_->currentComparable());
    const auto points = controller_->samplePoints(1);
    ASSERT_FALSE(points.isEmpty());
    EXPECT_FALSE(points.front().toMap().value("comparable").toBool());
    EXPECT_EQ(service_.cancelCount, 0);
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
        sample.analysis.metrics.mae = frame == 6 ? 8.0 : 1.0;
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

TEST_F(PairMetricsControllerTests, TimestampModePrefersSourceTimelineAndAppliesUserOffset) {
    // Canonical source 1 is CFR 30 fps. Source 2 declares a 30 fps average rate but carries a
    // probed VFR timeline ({0, 10000, 20000, 30000, 40000} us); the timeline must win over the
    // average-rate guess, and the active user offset must be applied on top.
    auto timeline = domain::FrameTimeline::create({domain::MediaTime{0},
                                                   domain::MediaTime{10'000},
                                                   domain::MediaTime{20'000},
                                                   domain::MediaTime{30'000},
                                                   domain::MediaTime{40'000}});
    ASSERT_TRUE(timeline.hasValue());

    const auto validation =
        domain::ComparisonValidator::validate({makeSource(domain::SourceId{1}, 320, 180, 12),
                                               makeSource(domain::SourceId{2}, 320, 180, 12)});
    ASSERT_TRUE(validation.hasValue());
    snapshot_ = std::make_shared<application::SessionSnapshot>();
    snapshot_->sessionId = domain::SessionId{1};
    snapshot_->sessionEpoch = domain::SessionEpoch{1};
    snapshot_->playbackGeneration = domain::PlaybackGeneration{1};
    snapshot_->displayedFrame = domain::FrameId{1};
    snapshot_->canonicalFrameCount = 12U;
    snapshot_->alignmentRevision = 4U;
    snapshot_->alignmentMode = application::AlignmentMode::Timestamp;
    auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate.hasValue());
    snapshot_->canonicalTimeline = std::move(rate).value();
    snapshot_->sourceTimelines = {application::SourceTimelineView{
        domain::SourceId{2},
        std::make_shared<const domain::FrameTimeline>(std::move(timeline).value()),
    }};
    snapshot_->alignmentOffsets = {application::SourceFrameOffset{domain::SourceId{1}, 0},
                                   application::SourceFrameOffset{domain::SourceId{2}, 1}};
    snapshot_->activeComparisonPair =
        domain::ComparisonPair{domain::SourceId{1}, domain::SourceId{2}};
    snapshot_->validatedComparison =
        std::make_shared<const domain::ValidatedComparisonSet>(std::move(validation).value().set);

    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);
    ASSERT_EQ(service_.requests.size(), 1U);
    const application::PairMetricsRequest& request = service_.requests.front();
    ASSERT_EQ(request.firstFrame, domain::FrameId{1});
    ASSERT_EQ(request.lastFrame, domain::FrameId{1});
    ASSERT_EQ(request.mappedSourceFrames.size(), 2U);
    // The canonical source maps frame-for-frame (no time round-trip).
    EXPECT_EQ(request.mappedSourceFrames[0], 1);
    // Canonical time 33333 us -> timeline frame 3 (the 30 fps guess would say frame 1),
    // plus the +1 user offset -> frame 4.
    EXPECT_EQ(request.mappedSourceFrames[1], 4);

    // A canonical time past the timeline's estimated end (40000 + 10000 us) is Missing, so
    // metrics mark the sample non-comparable instead of measuring the held last frame.
    snapshot_->displayedFrame = domain::FrameId{4};
    controller_->refresh();
    processUntil([&] { return service_.requests.size() >= 2U; }, 1000);
    ASSERT_GE(service_.requests.size(), 2U);
    const application::PairMetricsRequest& beyond = service_.requests.back();
    ASSERT_EQ(beyond.firstFrame, domain::FrameId{4});
    ASSERT_EQ(beyond.mappedSourceFrames.size(), 2U);
    EXPECT_EQ(beyond.mappedSourceFrames[0], 4);
    EXPECT_EQ(beyond.mappedSourceFrames[1], -1);
}

TEST_F(PairMetricsControllerTests, SequenceMappingWindowPreservesOffsetsGapsAndReviewSegments) {
    installTwoSourceSession(3, 12U);
    application::SequenceAlignmentResult sequence;
    sequence.sourceId = domain::SourceId{2};
    for (std::int64_t frame = 0; frame < 12; ++frame) {
        sequence.entries.push_back(application::SequenceAlignmentEntry{
            .canonicalFrameId = domain::FrameId{frame},
            .sourceFrameId = domain::FrameId{frame + 1},
            .matchKind = application::FrameMatchKind::AutoAligned,
            .confidence = 1.0F});
    }
    sequence.entries[4].sourceFrameId.reset();
    sequence.segments = {application::SequenceAlignmentSegment{
                             .firstCanonicalFrame = domain::FrameId{0},
                             .lastCanonicalFrame = domain::FrameId{5},
                             .state = application::AlignmentSegmentState::Accepted},
                         application::SequenceAlignmentSegment{
                             .firstCanonicalFrame = domain::FrameId{6},
                             .lastCanonicalFrame = domain::FrameId{11},
                             .state = application::AlignmentSegmentState::ReviewRequired}};
    snapshot_->sequenceAlignmentMaps =
        std::make_shared<const std::vector<application::SequenceAlignmentResult>>(
            std::vector<application::SequenceAlignmentResult>{sequence});
    controller_->setLaneEnabled(true);
    controller_->refresh();
    processUntil([&] { return !service_.requests.empty(); }, 1000);
    ASSERT_EQ(service_.requests.size(), 1U);
    const auto& mapped = service_.requests.front().mappedSourceFrames;
    ASSERT_EQ(mapped.size(), 24U);
    EXPECT_EQ(mapped[6], 3);
    EXPECT_EQ(mapped[7], 4);
    EXPECT_EQ(mapped[9], -1);
    EXPECT_EQ(mapped[13], 6);
}

} // namespace
} // namespace dvs::ui
