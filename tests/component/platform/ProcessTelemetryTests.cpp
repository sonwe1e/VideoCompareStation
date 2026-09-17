#include "dvs/platform/ProcessTelemetry.h"

#include <atomic>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>

namespace dvs::platform {
namespace {

using namespace std::chrono_literals;

TEST(ProcessTelemetryTests, SlowProbeRunsOffCallerAndDoesNotQueueConcurrentSamples) {
    const std::thread::id caller = std::this_thread::get_id();
    std::promise<std::thread::id> entered;
    auto enteredFuture = entered.get_future();
    std::promise<void> release;
    const auto released = release.get_future().share();
    std::atomic<unsigned> calls{0U};
    ProcessTelemetrySampler sampler{[&] {
                                        if (calls.fetch_add(1U) == 0U) {
                                            entered.set_value(std::this_thread::get_id());
                                        }
                                        released.wait();
                                        return ProcessTelemetry{.threadCount = 7U,
                                                                .workingSetBytes = 4096U};
                                    },
                                    5ms};

    const auto status = enteredFuture.wait_for(2s);
    if (status == std::future_status::ready) {
        EXPECT_NE(enteredFuture.get(), caller);
        // The caller remains free while one sample spans many sampling deadlines.
        std::this_thread::sleep_for(30ms);
        EXPECT_EQ(calls.load(), 1U);
    }
    release.set_value();
    const ProcessTelemetry peaks = sampler.stopAndTakePeaks();
    ASSERT_EQ(status, std::future_status::ready);
    EXPECT_EQ(peaks.threadCount, 7U);
    EXPECT_EQ(peaks.workingSetBytes, 4096U);
    const unsigned stoppedCalls = calls.load();
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(calls.load(), stoppedCalls);
    EXPECT_EQ(sampler.stopAndTakePeaks().threadCount, 7U);
}

TEST(ProcessTelemetryTests, KeepsIndependentPeaksAndSurvivesFailedSample) {
    std::atomic<unsigned> calls{0U};
    std::promise<void> sampled;
    auto sampledFuture = sampled.get_future();
    ProcessTelemetrySampler sampler{
        [&]() -> ProcessTelemetry {
            switch (calls.fetch_add(1U)) {
            case 0U:
                return ProcessTelemetry{.threadCount = 12U, .workingSetBytes = 200U};
            case 1U:
                throw std::runtime_error{"unavailable sample"};
            case 2U:
                sampled.set_value();
                return ProcessTelemetry{.threadCount = 3U, .workingSetBytes = 900U};
            default:
                return {};
            }
        },
        5ms};
    const auto status = sampledFuture.wait_for(2s);
    const ProcessTelemetry peaks = sampler.stopAndTakePeaks();
    ASSERT_EQ(status, std::future_status::ready);
    EXPECT_EQ(peaks.threadCount, 12U);
    EXPECT_EQ(peaks.workingSetBytes, 900U);
}

TEST(ProcessTelemetryTests, StopWakesIdleWorkerWithoutWaitingForSamplingDeadline) {
    std::atomic<unsigned> calls{0U};
    ProcessTelemetrySampler sampler{[&] {
                                        ++calls;
                                        return ProcessTelemetry{};
                                    },
                                    10s};
    const auto started = std::chrono::steady_clock::now();
    const ProcessTelemetry peaks = sampler.stopAndTakePeaks();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
    EXPECT_EQ(calls.load(), 0U);
    EXPECT_EQ(peaks.threadCount, 0U);
    EXPECT_EQ(peaks.workingSetBytes, 0U);
}

TEST(ProcessTelemetryTests, RejectsMissingProbeAndNonpositiveInterval) {
    EXPECT_THROW(ProcessTelemetrySampler(ProcessTelemetrySampler::Sample{}), std::invalid_argument);
    EXPECT_THROW(ProcessTelemetrySampler(sampleCurrentProcessTelemetry, 0ms),
                 std::invalid_argument);
}

} // namespace
} // namespace dvs::platform
