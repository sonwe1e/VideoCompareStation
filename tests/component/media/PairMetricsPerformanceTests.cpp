#include "dvs/application/PairMetrics.h"
#include "dvs/media/MediaProbe.h"
#include "dvs/media/PairMetricsService.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dvs::media {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::optional<std::string> configuredPath(const char* const name) {
    char* buffer = nullptr;
    std::size_t size = 0;
    const auto status = _dupenv_s(&buffer, &size, name);
    const std::unique_ptr<char, decltype(&std::free)> value{buffer, &std::free};
    if (status != 0 || value == nullptr || size <= 1U) {
        return std::nullopt;
    }
    return std::string{value.get()};
}

// This opt-in CPU benchmark belongs to the media suite, not the hardware playback gate. It
// measures a complete 301-position window (or the entire shorter source), including media open,
// source-identity verification, timeline indexing, decode, conversion and scalar scoring.
class BenchmarkSink final : public application::IPairMetricsSink {
public:
    explicit BenchmarkSink(const Clock::time_point started) : started_(started) {}

    void onPairMetricsBatch(application::PairMetricsBatch batch) override {
        std::scoped_lock lock(mutex_);
        if (samples_.empty() && !batch.samples.empty()) {
            firstBatchMilliseconds_ = millisecondsSinceStart();
            firstBatchSamples_ = batch.samples.size();
        }
        samples_.insert(samples_.end(), batch.samples.begin(), batch.samples.end());
        if (batch.finalBatch) {
            totalMilliseconds_ = millisecondsSinceStart();
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

    [[nodiscard]] bool wait() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds{90}, [this] { return completed_; });
    }

    struct Result final {
        std::vector<application::PairMetricsSample> samples;
        std::optional<application::PairMetricsFailure> failure;
        double firstBatchMilliseconds = 0.0;
        double totalMilliseconds = 0.0;
        std::size_t firstBatchSamples = 0;
    };

    [[nodiscard]] Result result() {
        std::scoped_lock lock(mutex_);
        return {
            samples_, failure_, firstBatchMilliseconds_, totalMilliseconds_, firstBatchSamples_};
    }

private:
    [[nodiscard]] double millisecondsSinceStart() const {
        return std::chrono::duration<double, std::milli>(Clock::now() - started_).count();
    }

    Clock::time_point started_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<application::PairMetricsSample> samples_;
    std::optional<application::PairMetricsFailure> failure_;
    bool completed_ = false;
    double firstBatchMilliseconds_ = 0.0;
    double totalMilliseconds_ = 0.0;
    std::size_t firstBatchSamples_ = 0;
};

TEST(PairMetricsPerformanceTests, ReportsConfiguredWindowWork) {
    const auto firstPath = configuredPath("DVS_METRICS_BENCHMARK_A");
    const auto secondPath = configuredPath("DVS_METRICS_BENCHMARK_B");
    if (!firstPath.has_value() || !secondPath.has_value()) {
        GTEST_SKIP() << "Set DVS_METRICS_BENCHMARK_A/B to local video files.";
    }
    const auto first = MediaProbe::inspect(std::filesystem::path{*firstPath}, domain::SourceId{1});
    const auto second =
        MediaProbe::inspect(std::filesystem::path{*secondPath}, domain::SourceId{2});
    ASSERT_TRUE(first.hasValue());
    ASSERT_TRUE(second.hasValue());
    const std::int64_t count = std::min<std::int64_t>(
        301, std::min(first.value().frameCount.value, second.value().frameCount.value));
    ASSERT_GT(count, 0);
    const domain::ComparisonSource a{.id = 1,
                                     .role = domain::ComparisonRole::kPrediction,
                                     .descriptor = first.value(),
                                     .displayName = "benchmark A"};
    const domain::ComparisonSource b{.id = 2,
                                     .role = domain::ComparisonRole::kPrediction,
                                     .descriptor = second.value(),
                                     .displayName = "benchmark B"};
    std::vector<application::PairMetricsSample> reference;
    for (std::uint64_t round = 0; round < 6U; ++round) {
        PairMetricsService service;
        const auto started = Clock::now();
        const auto sink = std::make_shared<BenchmarkSink>(started);
        const application::PairMetricsRequest request{
            .context = {application::RequestContext{domain::SessionId{1},
                                                    domain::SessionEpoch{1},
                                                    domain::RequestId{round + 1}},
                        domain::PlaybackGeneration{1}},
            .sources = {a, b},
            .offsets = {{a.id, 0}, {b.id, 0}},
            .firstFrame = domain::FrameId{0},
            .lastFrame = domain::FrameId{count - 1},
            .priorityFrame = domain::FrameId{(count - 1) / 2},
        };
        ASSERT_EQ(service.submit(request, sink), application::PortSubmitResult::Accepted);
        ASSERT_TRUE(sink->wait());
        auto result = sink->result();
        ASSERT_FALSE(result.failure.has_value());
        ASSERT_EQ(result.samples.size(), static_cast<std::size_t>(count));
        std::sort(
            result.samples.begin(), result.samples.end(), [](const auto& left, const auto& right) {
                return left.canonicalFrameId < right.canonicalFrameId;
            });
        for (std::int64_t frame = 0; frame < count; ++frame) {
            EXPECT_EQ(result.samples[static_cast<std::size_t>(frame)].canonicalFrameId,
                      domain::FrameId{frame});
            EXPECT_TRUE(result.samples[static_cast<std::size_t>(frame)].comparable);
        }
        if (round == 0U) {
            reference = result.samples;
        } else {
            EXPECT_EQ(result.samples, reference);
        }
        const auto work = service.workStats();
        const auto queriesStarted = Clock::now();
        std::uint64_t mismatchChecksum = 0U;
        for (const auto policy : {domain::MismatchPolicy::LumaOnly,
                                  domain::MismatchPolicy::AnyChannel,
                                  domain::MismatchPolicy::AllChannels}) {
            for (int threshold = 0; threshold <= 255; ++threshold) {
                for (const auto& sample : result.samples) {
                    mismatchChecksum +=
                        sample.analysis.metricsAt(static_cast<std::uint8_t>(threshold), policy)
                            .mismatchPixels;
                }
            }
        }
        const double queryMilliseconds =
            std::chrono::duration<double, std::milli>(Clock::now() - queriesStarted).count();
        const auto afterQueries = service.workStats();
        EXPECT_EQ(afterQueries.seekCount, work.seekCount);
        EXPECT_EQ(afterQueries.decodedFrames, work.decodedFrames);
        EXPECT_EQ(afterQueries.sampledFrames, work.sampledFrames);
        double maeSum = 0.0;
        for (const auto& sample : result.samples) {
            maeSum += sample.analysis.metrics.mae;
        }
        std::cout << std::setprecision(17) << "PAIR_METRICS_BENCHMARK {\"round\":" << round
                  << ",\"warmup\":" << (round == 0U ? "true" : "false")
                  << ",\"positions\":" << count
                  << ",\"firstBatchMs\":" << result.firstBatchMilliseconds
                  << ",\"totalMs\":" << result.totalMilliseconds
                  << ",\"firstBatchSamples\":" << result.firstBatchSamples
                  << ",\"seeks\":" << work.seekCount << ",\"decodedFrames\":" << work.decodedFrames
                  << ",\"sampledFrames\":" << work.sampledFrames
                  << ",\"batches\":" << work.publishedBatches << ",\"maeSum\":" << maeSum
                  << ",\"predicateQueryMs\":" << queryMilliseconds
                  << ",\"predicateQueries\":" << count * 768
                  << ",\"mismatchChecksum\":" << mismatchChecksum << "}\n";
    }
}

} // namespace
} // namespace dvs::media
