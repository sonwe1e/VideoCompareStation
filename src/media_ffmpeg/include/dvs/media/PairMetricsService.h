#pragma once

#include "dvs/application/PairMetrics.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace dvs::media {

// Runs pair-metrics jobs on dedicated RGBA decode sessions. The service owns one worker and a
// single pending slot: submitting a new request supersedes any queued request and cooperatively
// cancels the active job, which then stops silently at the next frame boundary. Sessions are
// reused across jobs while the request keeps addressing the same media, so paused frame stepping
// does not reopen demuxers.
class PairMetricsService final : public application::IPairMetricsService {
public:
    explicit PairMetricsService(std::size_t queueCapacity = 1U);
    ~PairMetricsService() override;

    PairMetricsService(const PairMetricsService&) = delete;
    PairMetricsService& operator=(const PairMetricsService&) = delete;
    PairMetricsService(PairMetricsService&&) = delete;
    PairMetricsService& operator=(PairMetricsService&&) = delete;

    [[nodiscard]] application::PortSubmitResult
    submit(const application::PairMetricsRequest& request,
           std::shared_ptr<application::IPairMetricsSink> sink) override;
    void cancel(const application::PlaybackRequestContext& context) noexcept override;

    [[nodiscard]] std::uint64_t decodedFrameCountForTesting() const noexcept;
    [[nodiscard]] std::uint64_t publishedBatchCountForTesting() const noexcept;

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace dvs::media
