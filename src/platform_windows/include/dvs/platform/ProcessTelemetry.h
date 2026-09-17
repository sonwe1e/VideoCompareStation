#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>

namespace dvs::platform {

struct ProcessTelemetry final {
    std::size_t threadCount = 0U;
    std::size_t workingSetBytes = 0U;
};

[[nodiscard]] ProcessTelemetry sampleCurrentProcessTelemetry() noexcept;

// One worker, no sample queue. The worker exclusively owns the peaks until it is joined.
// Construct before the measured event loop; stopAndTakePeaks() is control-thread-only and
// must be called after that loop exits because it waits for any in-flight OS sample.
class ProcessTelemetrySampler final {
public:
    using Sample = std::function<ProcessTelemetry()>;

    explicit ProcessTelemetrySampler(Sample sample = sampleCurrentProcessTelemetry,
                                     std::chrono::milliseconds interval = std::chrono::milliseconds{
                                         250});
    ~ProcessTelemetrySampler();

    ProcessTelemetrySampler(const ProcessTelemetrySampler&) = delete;
    ProcessTelemetrySampler& operator=(const ProcessTelemetrySampler&) = delete;

    [[nodiscard]] ProcessTelemetry stopAndTakePeaks() noexcept;

private:
    void run() noexcept;

    Sample sample_;
    std::chrono::milliseconds interval_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stopping_ = false;
    ProcessTelemetry peaks_;
    std::thread worker_;
};

} // namespace dvs::platform
