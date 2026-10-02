#include "dvs/application/ComparisonMetrics.h"
#include "dvs/domain/ComparisonValidator.h"
#include "dvs/media/MediaProbe.h"
#include "dvs/media/PairMetricsService.h"
#include "dvs/ui/PairMetricsController.h"

#include "PairMetricsDecodeSession.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <utility>

namespace dvs::ui {
namespace {

void ensureApplication() {
    if (QCoreApplication::instance() != nullptr) {
        return;
    }
    static int argumentCount = 1;
    static char name[] = "PairMetricsThresholdReuseTests";
    static char* arguments[] = {name, nullptr};
    static QCoreApplication application{argumentCount, arguments};
    static_cast<void>(application);
}

void waitUntil(const std::function<bool()>& predicate, const int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

class CountingService final : public application::IPairMetricsService {
public:
    application::PortSubmitResult
    submit(const application::PairMetricsRequest& request,
           std::shared_ptr<application::IPairMetricsSink> sink) override {
        ++submissions;
        return backend.submit(request, std::move(sink));
    }
    void cancel(const application::PlaybackRequestContext& context) noexcept override {
        ++cancellations;
        backend.cancel(context);
    }

    media::PairMetricsService backend;
    int submissions = 0;
    int cancellations = 0;
};

TEST(PairMetricsThresholdReuseTests, RealDecodeWindowReusesAnalysisForEveryPredicate) {
    ensureApplication();
    const auto root = std::filesystem::path{DVS_MEDIA_FIXTURE_DIR};
    const auto a = media::MediaProbe::inspect(root / "h264_a_320x180_30fps_12.mp4", 1);
    const auto b = media::MediaProbe::inspect(root / "h265_a_320x180_30fps_12.mp4", 2);
    ASSERT_TRUE(a.hasValue());
    ASSERT_TRUE(b.hasValue());
    const auto validated =
        domain::ComparisonValidator::validate({{.id = 1,
                                                .role = domain::ComparisonRole::kPrediction,
                                                .descriptor = a.value(),
                                                .displayName = "A"},
                                               {.id = 2,
                                                .role = domain::ComparisonRole::kPrediction,
                                                .descriptor = b.value(),
                                                .displayName = "B"}});
    ASSERT_TRUE(validated.hasValue());
    auto snapshot = std::make_shared<application::SessionSnapshot>();
    snapshot->sessionId = domain::SessionId{1};
    snapshot->sessionEpoch = domain::SessionEpoch{1};
    snapshot->playbackGeneration = domain::PlaybackGeneration{1};
    snapshot->displayedFrame = domain::FrameId{3};
    snapshot->canonicalFrameCount = 12;
    snapshot->activeComparisonPair = domain::ComparisonPair{1, 2};
    snapshot->alignmentOffsets = {{1, 0}, {2, 0}};
    snapshot->validatedComparison =
        std::make_shared<const domain::ValidatedComparisonSet>(validated.value().set);
    CountingService service;
    PairMetricsController controller{{.snapshot = [&] { return snapshot; }, .service = &service}};
    controller.setLaneEnabled(true);
    controller.refresh();
    waitUntil([&] { return controller.sampleCount() == 12 && !controller.sampling(); }, 5000);
    ASSERT_EQ(controller.sampleCount(), 12);
    ASSERT_TRUE(controller.currentComparable());
    ASSERT_GT(controller.currentMae(), 0.0);
    const auto before = service.backend.workStats();

    // Decode independently for the unchanged scalar reference. These sessions do not contribute
    // to the production service's telemetry; they verify the histogram/GUI projection, not just
    // the fact that a request counter stayed at one.
    std::atomic<bool> canceled = false;
    media::internal::PairMetricsDecodeSession firstDecoder{1, a.value()};
    media::internal::PairMetricsDecodeSession secondDecoder{2, b.value()};
    ASSERT_TRUE(firstDecoder.open(canceled).hasValue());
    ASSERT_TRUE(secondDecoder.open(canceled).hasValue());
    const auto first = firstDecoder.decodeRgba(domain::FrameId{3}, canceled);
    const auto second = secondDecoder.decodeRgba(domain::FrameId{3}, canceled);
    ASSERT_TRUE(first.hasValue());
    ASSERT_TRUE(second.hasValue());
    const domain::Rgba8View firstView{first.value().pixels.data(),
                                      first.value().width,
                                      first.value().height,
                                      first.value().width * 4U};
    const domain::Rgba8View secondView{second.value().pixels.data(),
                                       second.value().width,
                                       second.value().height,
                                       second.value().width * 4U};
    for (const auto policy : {domain::MismatchPolicy::LumaOnly,
                              domain::MismatchPolicy::AnyChannel,
                              domain::MismatchPolicy::AllChannels}) {
        controller.setThresholdPolicy(static_cast<int>(policy));
        for (const int threshold : {0, 1, 10, 30, 128, 255}) {
            controller.setThreshold(threshold);
            const auto reference = domain::computeRgbAbsoluteMetrics(
                firstView, secondView, static_cast<std::uint8_t>(threshold), policy);
            ASSERT_TRUE(reference.has_value());
            EXPECT_EQ(controller.currentMismatchPixels(), reference->mismatchPixels);
            EXPECT_DOUBLE_EQ(controller.currentMismatchRatio(), reference->mismatchRatio);
            EXPECT_DOUBLE_EQ(controller.currentMae(), reference->mae);
            EXPECT_DOUBLE_EQ(controller.currentMse(), reference->mse);
            EXPECT_DOUBLE_EQ(controller.currentPsnrDb(), reference->psnrDb);
            EXPECT_DOUBLE_EQ(controller.currentMaxAbsError(), reference->maxAbsError);
        }
    }
    QElapsedTimer queryTimer;
    queryTimer.start();
    std::uint64_t checksum = 0;
    for (int policy = 0; policy < 3; ++policy) {
        controller.setThresholdPolicy(policy);
        for (int threshold = 0; threshold <= 255; ++threshold) {
            controller.setThreshold(threshold);
            checksum += controller.currentMismatchPixels();
        }
    }
    const double queryMilliseconds = static_cast<double>(queryTimer.nsecsElapsed()) / 1'000'000.0;
    // Allow any accidental debounce/re-submit to actually fire before comparing work totals.
    waitUntil([&] { return service.submissions > 1; }, 250);
    const auto after = service.backend.workStats();
    EXPECT_GT(checksum, 0U);
    EXPECT_EQ(service.submissions, 1);
    EXPECT_EQ(service.cancellations, 0);
    EXPECT_EQ(after.seekCount, before.seekCount);
    EXPECT_EQ(after.decodedFrames, before.decodedFrames);
    EXPECT_EQ(after.sampledFrames, before.sampledFrames);
    EXPECT_EQ(after.publishedBatches, before.publishedBatches);
    EXPECT_EQ(controller.sampleCount(), 12);
    std::cout << "PAIR_METRICS_THRESHOLD_REUSE {\"positions\":12,\"predicateQueries\":768,"
              << "\"queryMs\":" << queryMilliseconds << ",\"submissions\":" << service.submissions
              << ",\"seeksBefore\":" << before.seekCount << ",\"seeksAfter\":" << after.seekCount
              << ",\"decodedBefore\":" << before.decodedFrames
              << ",\"decodedAfter\":" << after.decodedFrames << "}\n";
}

} // namespace
} // namespace dvs::ui
