#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "dvs/application/ComparisonMetrics.h"
#include "dvs/domain/ComparisonValidator.h"
#include "dvs/media/MediaProbe.h"
#include "dvs/media/PairMetricsService.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace dvs::media {
namespace {

[[nodiscard]] std::filesystem::path fixture(const char* const name) {
    return std::filesystem::path{DVS_MEDIA_FIXTURE_DIR} / name;
}

[[nodiscard]] domain::ComparisonSource probeSource(const std::filesystem::path& path,
                                                   const domain::SourceId sourceId) {
    const auto descriptor = MediaProbe::inspect(path, sourceId);
    EXPECT_TRUE(descriptor.hasValue());
    return domain::ComparisonSource{.id = sourceId,
                                    .role = domain::ComparisonRole::kPrediction,
                                    .descriptor = descriptor.value(),
                                    .displayName = "Source " + std::to_string(sourceId)};
}

[[nodiscard]] application::PlaybackRequestContext makeContext(const std::uint64_t requestId) {
    return application::PlaybackRequestContext{
        application::RequestContext{
            domain::SessionId{1}, domain::SessionEpoch{1}, domain::RequestId{requestId}},
        domain::PlaybackGeneration{1}};
}

// Collects everything the service publishes for one sink instance. The sink is invoked on the
// service worker, so the collection is mutex-guarded and completion-driven.
class CollectingSink final : public application::IPairMetricsSink {
public:
    void onPairMetricsBatch(application::PairMetricsBatch batch) override {
        std::scoped_lock lock(mutex_);
        batches_.push_back(std::move(batch));
        if (batches_.back().finalBatch) {
            completed_ = true;
        }
        condition_.notify_all();
    }

    void onPairMetricsFailure(application::PairMetricsFailure failure) override {
        std::scoped_lock lock(mutex_);
        failure_ = std::move(failure);
        completed_ = true;
        condition_.notify_all();
    }

    [[nodiscard]] bool waitForCompletion(const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return completed_; });
    }

    [[nodiscard]] std::optional<application::PairMetricsFailure> failure() {
        std::scoped_lock lock(mutex_);
        return failure_;
    }

    [[nodiscard]] std::vector<application::PairMetricsBatch> batches() {
        std::scoped_lock lock(mutex_);
        return batches_;
    }

    [[nodiscard]] std::size_t totalSampleCount() {
        std::scoped_lock lock(mutex_);
        std::size_t total = 0U;
        for (const application::PairMetricsBatch& batch : batches_) {
            total += batch.samples.size();
        }
        return total;
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool completed_ = false;
    std::vector<application::PairMetricsBatch> batches_;
    std::optional<application::PairMetricsFailure> failure_;
};

class PairMetricsServiceTests : public ::testing::Test {
protected:
    PairMetricsService service{1U};

    [[nodiscard]] static application::PairMetricsRequest
    makeRequest(const domain::ComparisonSource& first,
                const domain::ComparisonSource& second,
                const std::int64_t firstFrame,
                const std::int64_t lastFrame,
                const std::int64_t secondOffset = 0,
                const std::uint64_t requestId = 1U) {
        application::PairMetricsRequest request{
            .context = makeContext(requestId),
            .sources = {first, second},
            .offsets = {application::SourceFrameOffset{first.id, 0},
                        application::SourceFrameOffset{second.id, secondOffset}},
            .alignmentRevision = 1U,
            .firstFrame = domain::FrameId{firstFrame},
            .lastFrame = domain::FrameId{lastFrame},
            .mismatchThreshold = 4U,
        };
        return request;
    }
};

TEST_F(PairMetricsServiceTests, IdenticalSourcesReportZeroError) {
    // The same file probed as two distinct sources: a valid pair whose decoded pixels are
    // byte-identical, so every metric must collapse to zero / infinite PSNR.
    const domain::ComparisonSource first =
        probeSource(fixture("h264_a_320x180_30fps_12.mp4"), domain::SourceId{1});
    const domain::ComparisonSource second =
        probeSource(fixture("h264_a_320x180_30fps_12.mp4"), domain::SourceId{2});
    const auto sink = std::make_shared<CollectingSink>();
    const application::PairMetricsRequest request = makeRequest(first, second, 0, 11, 0, 1U);
    ASSERT_EQ(service.submit(request, sink), application::PortSubmitResult::Accepted);
    ASSERT_TRUE(sink->waitForCompletion(std::chrono::seconds{5}));
    EXPECT_FALSE(sink->failure().has_value());
    EXPECT_EQ(sink->totalSampleCount(), 12U);

    const std::vector<application::PairMetricsBatch> batches = sink->batches();
    ASSERT_FALSE(batches.empty());
    EXPECT_EQ(batches.front().metricId, std::string{application::kRgbAbsoluteMetricId});
    for (const application::PairMetricsBatch& batch : batches) {
        for (const application::PairMetricsSample& sample : batch.samples) {
            EXPECT_TRUE(sample.comparable);
            EXPECT_DOUBLE_EQ(sample.metrics.mae, 0.0);
            EXPECT_DOUBLE_EQ(sample.metrics.mse, 0.0);
            EXPECT_DOUBLE_EQ(sample.metrics.psnrDb, domain::kInfinitePsnrDb);
            EXPECT_EQ(sample.metrics.maxAbsError, 0U);
            EXPECT_EQ(sample.metrics.mismatchPixels, 0U);
            EXPECT_EQ(sample.metrics.pixelCount,
                      static_cast<std::uint64_t>(first.descriptor.extent.width) *
                          first.descriptor.extent.height);
        }
    }
    bool hasFinalBatch = false;
    for (const application::PairMetricsBatch& batch : batches) {
        hasFinalBatch = hasFinalBatch || batch.finalBatch;
    }
    EXPECT_TRUE(hasFinalBatch);
}

TEST_F(PairMetricsServiceTests, DifferentSourcesReportComparableSamples) {
    const domain::ComparisonSource first =
        probeSource(fixture("h264_a_320x180_30fps_12.mp4"), domain::SourceId{1});
    const domain::ComparisonSource second =
        probeSource(fixture("h265_a_320x180_30fps_12.mp4"), domain::SourceId{2});
    const auto sink = std::make_shared<CollectingSink>();
    const application::PairMetricsRequest request = makeRequest(first, second, 0, 5, 0, 1U);
    ASSERT_EQ(service.submit(request, sink), application::PortSubmitResult::Accepted);
    ASSERT_TRUE(sink->waitForCompletion(std::chrono::seconds{5}));
    EXPECT_FALSE(sink->failure().has_value());
    EXPECT_EQ(sink->totalSampleCount(), 6U);

    std::size_t comparableCount = 0U;
    for (const application::PairMetricsBatch& batch : sink->batches()) {
        for (const application::PairMetricsSample& sample : batch.samples) {
            if (sample.comparable) {
                ++comparableCount;
                EXPECT_GE(sample.metrics.mae, 0.0);
                EXPECT_GT(sample.metrics.pixelCount, 0U);
            }
        }
    }
    EXPECT_EQ(comparableCount, 6U);
}

TEST_F(PairMetricsServiceTests, OutOfRangeMappingIsNotComparable) {
    const domain::ComparisonSource first =
        probeSource(fixture("h264_a_320x180_30fps_12.mp4"), domain::SourceId{1});
    const domain::ComparisonSource second =
        probeSource(fixture("h265_a_320x180_30fps_12.mp4"), domain::SourceId{2});
    // +100 shifts every mapped frame past the 12-frame source timeline.
    const auto sink = std::make_shared<CollectingSink>();
    const application::PairMetricsRequest request = makeRequest(first, second, 0, 3, 100, 1U);
    ASSERT_EQ(service.submit(request, sink), application::PortSubmitResult::Accepted);
    ASSERT_TRUE(sink->waitForCompletion(std::chrono::seconds{5}));
    EXPECT_FALSE(sink->failure().has_value());
    EXPECT_EQ(sink->totalSampleCount(), 4U);
    for (const application::PairMetricsBatch& batch : sink->batches()) {
        for (const application::PairMetricsSample& sample : batch.samples) {
            EXPECT_FALSE(sample.comparable);
        }
    }
}

TEST_F(PairMetricsServiceTests, GeometryMismatchIsNotComparable) {
    const domain::ComparisonSource first =
        probeSource(fixture("h264_a_320x180_30fps_12.mp4"), domain::SourceId{1});
    const domain::ComparisonSource second =
        probeSource(fixture("h264_b_160x90_30fps_12.mp4"), domain::SourceId{2});
    const auto sink = std::make_shared<CollectingSink>();
    const application::PairMetricsRequest request = makeRequest(first, second, 0, 2, 0, 1U);
    ASSERT_EQ(service.submit(request, sink), application::PortSubmitResult::Accepted);
    ASSERT_TRUE(sink->waitForCompletion(std::chrono::seconds{5}));
    EXPECT_FALSE(sink->failure().has_value());
    EXPECT_GT(sink->totalSampleCount(), 0U);
    for (const application::PairMetricsBatch& batch : sink->batches()) {
        for (const application::PairMetricsSample& sample : batch.samples) {
            EXPECT_FALSE(sample.comparable);
        }
    }
}

TEST_F(PairMetricsServiceTests, InvalidRequestsAreRejected) {
    const domain::ComparisonSource first =
        probeSource(fixture("h264_a_320x180_30fps_12.mp4"), domain::SourceId{1});
    const domain::ComparisonSource second =
        probeSource(fixture("h265_a_320x180_30fps_12.mp4"), domain::SourceId{2});
    const auto sink = std::make_shared<CollectingSink>();

    application::PairMetricsRequest request = makeRequest(first, second, 0, 2, 0, 1U);
    request.sources = {first};
    EXPECT_EQ(service.submit(request, sink), application::PortSubmitResult::Closed);

    request = makeRequest(first, second, 0, 2, 0, 1U);
    request.firstFrame = domain::FrameId{5};
    request.lastFrame = domain::FrameId{2};
    EXPECT_EQ(service.submit(request, sink), application::PortSubmitResult::Closed);

    request = makeRequest(first, second, 0, 2, 0, 1U);
    request.offsets = {application::SourceFrameOffset{first.id, 0}};
    EXPECT_EQ(service.submit(request, sink), application::PortSubmitResult::Closed);

    EXPECT_EQ(service.submit(makeRequest(first, second, 0, 2, 0, 1U), nullptr),
              application::PortSubmitResult::Closed);
}

TEST_F(PairMetricsServiceTests, NewRequestSupersedesActiveJob) {
    const domain::ComparisonSource first =
        probeSource(fixture("h264_a_320x180_30fps_12.mp4"), domain::SourceId{1});
    const domain::ComparisonSource second =
        probeSource(fixture("h265_a_320x180_30fps_12.mp4"), domain::SourceId{2});

    const auto longSink = std::make_shared<CollectingSink>();
    const auto shortSink = std::make_shared<CollectingSink>();
    ASSERT_EQ(service.submit(makeRequest(first, second, 0, 11, 0, 1U), longSink),
              application::PortSubmitResult::Accepted);
    // The short request supersedes the active window job; only it may complete.
    ASSERT_EQ(service.submit(makeRequest(first, second, 0, 0, 0, 2U), shortSink),
              application::PortSubmitResult::Accepted);
    ASSERT_TRUE(shortSink->waitForCompletion(std::chrono::seconds{5}));
    EXPECT_FALSE(shortSink->failure().has_value());
    EXPECT_FALSE(longSink->waitForCompletion(std::chrono::milliseconds{200}));
}

TEST_F(PairMetricsServiceTests, CancelDropsWorkSilently) {
    const domain::ComparisonSource first =
        probeSource(fixture("h264_a_320x180_30fps_12.mp4"), domain::SourceId{1});
    const domain::ComparisonSource second =
        probeSource(fixture("h265_a_320x180_30fps_12.mp4"), domain::SourceId{2});
    const auto sink = std::make_shared<CollectingSink>();
    const application::PairMetricsRequest request = makeRequest(first, second, 0, 11, 0, 7U);
    ASSERT_EQ(service.submit(request, sink), application::PortSubmitResult::Accepted);
    service.cancel(request.context);
    EXPECT_FALSE(sink->waitForCompletion(std::chrono::milliseconds{300}));
}

TEST_F(PairMetricsServiceTests, SessionReuseSkipsReopenBetweenJobs) {
    const domain::ComparisonSource first =
        probeSource(fixture("h264_a_320x180_30fps_12.mp4"), domain::SourceId{1});
    const domain::ComparisonSource second =
        probeSource(fixture("h265_a_320x180_30fps_12.mp4"), domain::SourceId{2});
    const auto firstSink = std::make_shared<CollectingSink>();
    ASSERT_EQ(service.submit(makeRequest(first, second, 0, 0, 0, 1U), firstSink),
              application::PortSubmitResult::Accepted);
    ASSERT_TRUE(firstSink->waitForCompletion(std::chrono::seconds{5}));
    const std::uint64_t decodedAfterFirstJob = service.decodedFrameCountForTesting();

    const auto secondSink = std::make_shared<CollectingSink>();
    ASSERT_EQ(service.submit(makeRequest(first, second, 1, 1, 0, 2U), secondSink),
              application::PortSubmitResult::Accepted);
    ASSERT_TRUE(secondSink->waitForCompletion(std::chrono::seconds{5}));
    EXPECT_GT(service.decodedFrameCountForTesting(), decodedAfterFirstJob);
    EXPECT_GT(secondSink->totalSampleCount(), 0U);
}

} // namespace
} // namespace dvs::media
