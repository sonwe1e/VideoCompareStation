#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "dvs/application/ComparisonMetrics.h"
#include "dvs/domain/ComparisonValidator.h"
#include "dvs/media/MediaProbe.h"
#include "dvs/media/PairMetricsService.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
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
    explicit CollectingSink(std::function<void()> onBatch = {}) : onBatch_(std::move(onBatch)) {}

    void onPairMetricsBatch(application::PairMetricsBatch batch) override {
        if (onBatch_) {
            onBatch_();
        }
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

    [[nodiscard]] bool waitForBatch(const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return !batches_.empty(); });
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
    std::function<void()> onBatch_;
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
            .mappedSourceFrames = {},
            .alignmentRevision = 1U,
            .firstFrame = domain::FrameId{firstFrame},
            .lastFrame = domain::FrameId{lastFrame},
        };
        return request;
    }

    // Submits one request on the reused service instance and returns the summed MAE of its
    // samples, mirroring how the GUI switches the active pair without recreating the service.
    [[nodiscard]] double submitAndCollectMae(const domain::ComparisonSource& first,
                                             const domain::ComparisonSource& second,
                                             const std::uint64_t requestId) {
        const auto sink = std::make_shared<CollectingSink>();
        EXPECT_EQ(service.submit(makeRequest(first, second, 0, 3, 0, requestId), sink),
                  application::PortSubmitResult::Accepted);
        EXPECT_TRUE(sink->waitForCompletion(std::chrono::seconds{5}));
        EXPECT_FALSE(sink->failure().has_value());
        EXPECT_EQ(sink->totalSampleCount(), 4U);
        double maeSum = 0.0;
        for (const application::PairMetricsBatch& batch : sink->batches()) {
            for (const application::PairMetricsSample& sample : batch.samples) {
                EXPECT_TRUE(sample.comparable);
                maeSum += sample.analysis.metrics.mae;
            }
        }
        return maeSum;
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
            EXPECT_DOUBLE_EQ(sample.analysis.metrics.mae, 0.0);
            EXPECT_DOUBLE_EQ(sample.analysis.metrics.mse, 0.0);
            EXPECT_DOUBLE_EQ(sample.analysis.metrics.psnrDb, domain::kInfinitePsnrDb);
            EXPECT_EQ(sample.analysis.metrics.maxAbsError, 0U);
            EXPECT_EQ(sample.analysis.metricsAt(0).mismatchPixels, 0U);
            EXPECT_EQ(sample.analysis.metrics.pixelCount,
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

TEST_F(PairMetricsServiceTests, UnprioritizedWindowUsesOneForwardRunPerSource) {
    const auto first = probeSource(fixture("h264_a_320x180_30fps_12.mp4"), 1);
    const auto second = probeSource(fixture("h265_a_320x180_30fps_12.mp4"), 2);
    const auto sink = std::make_shared<CollectingSink>();
    ASSERT_EQ(service.submit(makeRequest(first, second, 0, 11), sink),
              application::PortSubmitResult::Accepted);
    ASSERT_TRUE(sink->waitForCompletion(std::chrono::seconds{5}));
    ASSERT_FALSE(sink->failure().has_value());
    const auto batches = sink->batches();
    ASSERT_EQ(batches.size(), 1U);
    std::vector<domain::FrameId> actual;
    std::vector<domain::FrameId> expected;
    for (const auto& sample : batches.front().samples) {
        actual.push_back(sample.canonicalFrameId);
    }
    for (std::int64_t frame = 0; frame < 12; ++frame) {
        expected.emplace_back(frame);
    }
    EXPECT_EQ(actual, expected);
    const auto work = service.workStats();
    EXPECT_EQ(work.seekCount, 2U);
    EXPECT_EQ(work.decodedFrames, 24U);
    EXPECT_EQ(work.sampledFrames, 24U);
    EXPECT_EQ(work.publishedBatches, 1U);
}

TEST_F(PairMetricsServiceTests, PriorityPublishesImmediatelyAndPreservesEveryMappedSample) {
    const auto first = probeSource(fixture("h264_a_320x180_30fps_12.mp4"), 1);
    const auto second = probeSource(fixture("h265_a_320x180_30fps_12.mp4"), 2);
    const auto referenceSink = std::make_shared<CollectingSink>();
    ASSERT_EQ(service.submit(makeRequest(first, second, 0, 10, 1), referenceSink),
              application::PortSubmitResult::Accepted);
    ASSERT_TRUE(referenceSink->waitForCompletion(std::chrono::seconds{5}));
    ASSERT_FALSE(referenceSink->failure().has_value());
    std::vector<application::PairMetricsSample> expectedSamples;
    for (const auto& batch : referenceSink->batches()) {
        expectedSamples.insert(expectedSamples.end(), batch.samples.begin(), batch.samples.end());
    }
    const auto byFrame = [](const auto& left, const auto& right) {
        return left.canonicalFrameId < right.canonicalFrameId;
    };
    std::sort(expectedSamples.begin(), expectedSamples.end(), byFrame);
    // A mapping gap must stay unavailable; the reordered job must not hide it or fill it with
    // another frame. All other dense mappings must reproduce the offset-based reference.
    ASSERT_EQ(expectedSamples.size(), 11U);
    expectedSamples[5] = application::PairMetricsSample{.canonicalFrameId = domain::FrameId{5}};
    for (const std::int64_t priority : {0, 3, 10}) {
        SCOPED_TRACE(priority);
        PairMetricsService focusedService;
        auto request = makeRequest(first, second, 0, 10, 100, 2);
        request.priorityFrame = domain::FrameId{priority};
        for (std::int64_t frame = 0; frame <= 10; ++frame) {
            request.mappedSourceFrames.push_back(frame);
            request.mappedSourceFrames.push_back(frame == 5 ? -1 : frame + 1);
        }
        const auto sink = std::make_shared<CollectingSink>();
        ASSERT_EQ(focusedService.submit(request, sink), application::PortSubmitResult::Accepted);
        ASSERT_TRUE(sink->waitForCompletion(std::chrono::seconds{5}));
        ASSERT_FALSE(sink->failure().has_value());
        const auto batches = sink->batches();
        ASSERT_GE(batches.size(), 2U);
        ASSERT_EQ(batches.front().samples.size(), 1U);
        EXPECT_EQ(batches.front().samples.front().canonicalFrameId, domain::FrameId{priority});
        EXPECT_FALSE(batches.front().finalBatch);
        EXPECT_EQ(batches.front().context, request.context);
        std::vector<application::PairMetricsSample> samples;
        std::vector<domain::FrameId> order;
        for (const auto& batch : batches) {
            samples.insert(samples.end(), batch.samples.begin(), batch.samples.end());
            for (const auto& sample : batch.samples) {
                order.push_back(sample.canonicalFrameId);
            }
        }
        std::vector<domain::FrameId> expectedOrder{domain::FrameId{priority}};
        for (std::int64_t frame = priority + 1; frame <= 10; ++frame) {
            expectedOrder.emplace_back(frame);
        }
        for (std::int64_t frame = 0; frame < priority; ++frame) {
            expectedOrder.emplace_back(frame);
        }
        EXPECT_EQ(order, expectedOrder);
        std::sort(samples.begin(), samples.end(), byFrame);
        EXPECT_EQ(samples, expectedSamples);
    }
}

TEST_F(PairMetricsServiceTests, CancelAfterPriorityBatchStopsBeforeWindowDecode) {
    const auto first = probeSource(fixture("h264_a_320x180_30fps_12.mp4"), 1);
    const auto second = probeSource(fixture("h265_a_320x180_30fps_12.mp4"), 2);
    auto request = makeRequest(first, second, 0, 11);
    request.priorityFrame = domain::FrameId{3};
    // Cancellation from the worker callback is deterministic: the rest of the window must
    // neither decode nor publish, rather than relying on a race against the calling thread.
    const auto sink = std::make_shared<CollectingSink>([&] { service.cancel(request.context); });
    ASSERT_EQ(service.submit(request, sink), application::PortSubmitResult::Accepted);
    ASSERT_TRUE(sink->waitForBatch(std::chrono::seconds{5}));
    EXPECT_FALSE(sink->waitForCompletion(std::chrono::milliseconds{200}));
    EXPECT_EQ(sink->totalSampleCount(), 1U);
    EXPECT_EQ(service.workStats().sampledFrames, 2U);
    EXPECT_EQ(service.workStats().publishedBatches, 1U);
}

TEST_F(PairMetricsServiceTests, SinglePriorityFrameCompletesInItsFirstBatch) {
    const auto first = probeSource(fixture("h264_a_320x180_30fps_12.mp4"), 1);
    const auto second = probeSource(fixture("h265_a_320x180_30fps_12.mp4"), 2);
    auto request = makeRequest(first, second, 3, 3);
    request.priorityFrame = domain::FrameId{3};
    const auto sink = std::make_shared<CollectingSink>();
    ASSERT_EQ(service.submit(request, sink), application::PortSubmitResult::Accepted);
    ASSERT_TRUE(sink->waitForCompletion(std::chrono::seconds{5}));
    ASSERT_FALSE(sink->failure().has_value());
    const auto batches = sink->batches();
    ASSERT_EQ(batches.size(), 1U);
    ASSERT_EQ(batches.front().samples.size(), 1U);
    EXPECT_EQ(batches.front().samples.front().canonicalFrameId, domain::FrameId{3});
    EXPECT_TRUE(batches.front().finalBatch);
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
                EXPECT_GE(sample.analysis.metrics.mae, 0.0);
                EXPECT_GT(sample.analysis.metrics.pixelCount, 0U);
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

    for (const std::int64_t invalid : {-1LL, (std::numeric_limits<std::int64_t>::max)()}) {
        request = makeRequest(first, second, invalid, invalid);
        ASSERT_FALSE(request.isValid());
        EXPECT_EQ(service.submit(request, sink), application::PortSubmitResult::Closed);
    }
    for (const std::int64_t priority : {-1, 3}) {
        request = makeRequest(first, second, 0, 2);
        request.priorityFrame = domain::FrameId{priority};
        ASSERT_FALSE(request.isValid());
        EXPECT_EQ(service.submit(request, sink), application::PortSubmitResult::Closed);
    }

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
    const std::uint64_t decodedAfterFirstJob = service.workStats().sampledFrames;

    const auto secondSink = std::make_shared<CollectingSink>();
    ASSERT_EQ(service.submit(makeRequest(first, second, 1, 1, 0, 2U), secondSink),
              application::PortSubmitResult::Accepted);
    ASSERT_TRUE(secondSink->waitForCompletion(std::chrono::seconds{5}));
    EXPECT_GT(service.workStats().sampledFrames, decodedAfterFirstJob);
    EXPECT_GT(secondSink->totalSampleCount(), 0U);
}

TEST_F(PairMetricsServiceTests, SwitchingComparisonPairRebindsDecodeSessions) {
    // Regression: a decode slot that served another source used to reopen the descriptor
    // captured at its construction, so after A/B -> A/C the second slot silently decoded B
    // while the service published the numbers as the A/C pair. Switching pairs on one reused
    // service instance must always measure the requested sources.
    const domain::ComparisonSource a =
        probeSource(fixture("h264_a_320x180_30fps_12.mp4"), domain::SourceId{1});
    const domain::ComparisonSource b =
        probeSource(fixture("h265_a_320x180_30fps_12.mp4"), domain::SourceId{2});
    // Same geometry and frame count, clearly different pixel content (a different clip).
    const domain::ComparisonSource c =
        probeSource(fixture("h264_rate_mismatch_320x180_24fps_12.mp4"), domain::SourceId{3});

    const double firstAbMae = submitAndCollectMae(a, b, 1U);
    const double acMae = submitAndCollectMae(a, c, 2U);
    // Discriminating precondition: the pairs must measure differently, otherwise a
    // wrong-source decode could hide behind identical numbers.
    ASSERT_NE(acMae, firstAbMae);
    const double bcMae = submitAndCollectMae(b, c, 3U);
    ASSERT_NE(bcMae, firstAbMae);
    ASSERT_NE(bcMae, acMae);

    // Switching back to the first pair must reproduce its measurement exactly: the slot that
    // served C has to rebind to B instead of replaying C's (or A's) file.
    const double secondAbMae = submitAndCollectMae(a, b, 4U);
    EXPECT_DOUBLE_EQ(secondAbMae, firstAbMae);
}

TEST_F(PairMetricsServiceTests, IdenticalSourcesStayZeroErrorUnderEveryMismatchPolicy) {
    const domain::ComparisonSource first =
        probeSource(fixture("h264_a_320x180_30fps_12.mp4"), domain::SourceId{1});
    const domain::ComparisonSource second =
        probeSource(fixture("h264_a_320x180_30fps_12.mp4"), domain::SourceId{2});
    const auto sink = std::make_shared<CollectingSink>();
    ASSERT_EQ(service.submit(makeRequest(first, second, 0, 2), sink),
              application::PortSubmitResult::Accepted);
    ASSERT_TRUE(sink->waitForCompletion(std::chrono::seconds{5}));
    ASSERT_FALSE(sink->failure().has_value());
    for (const auto& batch : sink->batches()) {
        for (const auto& sample : batch.samples) {
            ASSERT_TRUE(sample.comparable);
            for (const auto policy : {domain::MismatchPolicy::LumaOnly,
                                      domain::MismatchPolicy::AnyChannel,
                                      domain::MismatchPolicy::AllChannels}) {
                EXPECT_EQ(sample.analysis.metricsAt(0, policy).mismatchPixels, 0U);
                EXPECT_DOUBLE_EQ(sample.analysis.metrics.mae, 0.0);
            }
        }
    }
}

TEST_F(PairMetricsServiceTests, MismatchPolicyChangesBadPixelCountOnDifferentSources) {
    const domain::ComparisonSource first =
        probeSource(fixture("h264_a_320x180_30fps_12.mp4"), domain::SourceId{1});
    const domain::ComparisonSource second =
        probeSource(fixture("h264_rate_mismatch_320x180_24fps_12.mp4"), domain::SourceId{2});
    std::uint64_t anyChannelPixels = 0U;
    std::uint64_t lumaOnlyPixels = 0U;
    std::uint64_t allChannelsPixels = 0U;
    const std::array<std::pair<domain::MismatchPolicy, std::uint64_t*>, 3U> cases = {
        {{domain::MismatchPolicy::AnyChannel, &anyChannelPixels},
         {domain::MismatchPolicy::LumaOnly, &lumaOnlyPixels},
         {domain::MismatchPolicy::AllChannels, &allChannelsPixels}}};
    const auto sink = std::make_shared<CollectingSink>();
    ASSERT_EQ(service.submit(makeRequest(first, second, 0, 0), sink),
              application::PortSubmitResult::Accepted);
    ASSERT_TRUE(sink->waitForCompletion(std::chrono::seconds{5}));
    ASSERT_FALSE(sink->failure().has_value());
    const auto batches = sink->batches();
    ASSERT_EQ(batches.size(), 1U);
    ASSERT_EQ(batches.front().samples.size(), 1U);
    ASSERT_TRUE(batches.front().samples.front().comparable);
    for (const auto& [policy, pixelCount] : cases) {
        *pixelCount = batches.front().samples.front().analysis.metricsAt(30, policy).mismatchPixels;
    }
    // Both narrower policies are subsets of AnyChannel at the same threshold: the luma sample
    // never exceeds the max channel delta, and AllChannels requires every channel to pass.
    EXPECT_LE(lumaOnlyPixels, anyChannelPixels);
    EXPECT_LE(allChannelsPixels, anyChannelPixels);
    EXPECT_GT(anyChannelPixels, 0U);
}

} // namespace
} // namespace dvs::media
