#include "dvs/platform/ProcessTelemetry.h"

#include "WindowsApi.h"

#include <Psapi.h>
#include <TlHelp32.h>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace dvs::platform {
namespace {

[[nodiscard]] std::size_t currentProcessThreadCount() noexcept {
    const DWORD processId = GetCurrentProcessId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0U);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0U;
    }

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(THREADENTRY32);
    std::size_t count = 0U;
    if (Thread32First(snapshot, &entry) != FALSE) {
        do {
            if (entry.th32OwnerProcessID == processId) {
                ++count;
            }
        } while (Thread32Next(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);
    return count;
}

[[nodiscard]] std::size_t currentProcessWorkingSetBytes() noexcept {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(PROCESS_MEMORY_COUNTERS);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == FALSE) {
        return 0U;
    }
    return counters.WorkingSetSize;
}

} // namespace

ProcessTelemetry sampleCurrentProcessTelemetry() noexcept {
    return ProcessTelemetry{
        .threadCount = currentProcessThreadCount(),
        .workingSetBytes = currentProcessWorkingSetBytes(),
    };
}

ProcessTelemetrySampler::ProcessTelemetrySampler(Sample sample,
                                                 const std::chrono::milliseconds interval)
    : sample_(std::move(sample)), interval_(interval) {
    if (!sample_ || interval_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument{"Telemetry sampling requires a probe and positive interval."};
    }
    worker_ = std::thread{[this] { run(); }};
}

ProcessTelemetrySampler::~ProcessTelemetrySampler() {
    static_cast<void>(stopAndTakePeaks());
}

ProcessTelemetry ProcessTelemetrySampler::stopAndTakePeaks() noexcept {
    {
        const std::lock_guard lock{mutex_};
        stopping_ = true;
    }
    condition_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
    return peaks_;
}

void ProcessTelemetrySampler::run() noexcept {
    auto deadline = std::chrono::steady_clock::now() + interval_;
    std::unique_lock lock{mutex_};
    while (!condition_.wait_until(lock, deadline, [this] { return stopping_; })) {
        lock.unlock();
        try {
            const ProcessTelemetry value = sample_();
            peaks_.threadCount = (std::max)(peaks_.threadCount, value.threadCount);
            peaks_.workingSetBytes = (std::max)(peaks_.workingSetBytes, value.workingSetBytes);
        } catch (...) {
            // Telemetry is observational; a failed sample must not terminate playback.
        }
        // Preserve the cadence without queueing catch-up work after a slow enumeration.
        deadline += interval_;
        const auto now = std::chrono::steady_clock::now();
        if (deadline <= now) {
            deadline = now + interval_;
        }
        lock.lock();
    }
}

} // namespace dvs::platform
