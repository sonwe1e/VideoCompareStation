#include "dvs/platform/SteadyDeadlineScheduler.h"

#include "WindowsApi.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace dvs::platform {
namespace {

#ifndef CREATE_WAITABLE_TIMER_MANUAL_RESET
#define CREATE_WAITABLE_TIMER_MANUAL_RESET 0x00000002
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000004
#endif

struct DeadlineKey final {
    std::chrono::steady_clock::time_point due;
    std::uint64_t sequence = 0;

    [[nodiscard]] bool operator<(const DeadlineKey& other) const noexcept {
        if (due < other.due) {
            return true;
        }
        if (other.due < due) {
            return false;
        }
        return sequence < other.sequence;
    }
};

struct ScheduledDeadline final {
    application::DeadlineRequest request;
    std::weak_ptr<application::IApplicationEventSink> events;
};

struct HandleCloser final {
    void operator()(HANDLE handle) const noexcept {
        if (handle != nullptr) {
            CloseHandle(handle);
        }
    }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

} // namespace

std::chrono::steady_clock::time_point SystemSteadyClock::now() const noexcept {
    return std::chrono::steady_clock::now();
}

// The deadline wait uses a high-resolution waitable timer instead of a condition variable.
// SleepConditionVariableSRW wakes on the default system timer quantum (about 15.6 ms), which
// exceeds the playback preparation lead (14 ms at 60 fps, half a frame at 120 fps) and made
// cadence deadlines systematically late. A waitable timer armed with
// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION wakes within sub-millisecond precision on Windows 10
// 1803+; on older systems it falls back to the plain timer with the previous precision.
class SteadyDeadlineScheduler::Impl final {
public:
    Impl() {
        wake_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        timer_.reset(CreateWaitableTimerExW(nullptr,
                                            nullptr,
                                            CREATE_WAITABLE_TIMER_MANUAL_RESET |
                                                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                            TIMER_ALL_ACCESS));
        if (!timer_) {
            timer_.reset(CreateWaitableTimerExW(
                nullptr, nullptr, CREATE_WAITABLE_TIMER_MANUAL_RESET, TIMER_ALL_ACCESS));
        }
        if (!wake_ || !timer_) {
            throw std::runtime_error("SteadyDeadlineScheduler: failed to create wait objects");
        }
        worker_ = std::thread([this] { run(); });
    }

    ~Impl() {
        shutdown();
    }

    [[nodiscard]] application::PortSubmitResult
    schedule(const application::DeadlineRequest& request,
             const std::shared_ptr<application::IApplicationEventSink>& events) {
        if (!events) {
            return application::PortSubmitResult::Closed;
        }

        try {
            {
                std::lock_guard lock(mutex_);
                if (stopping_) {
                    return application::PortSubmitResult::Closed;
                }
                if (dispatchingTimerId_.has_value() && *dispatchingTimerId_ == request.timerId) {
                    return application::PortSubmitResult::Busy;
                }
                if (nextSequence_ == std::numeric_limits<std::uint64_t>::max()) {
                    return application::PortSubmitResult::Busy;
                }

                const DeadlineKey key{
                    .due = request.due,
                    .sequence = nextSequence_++,
                };
                const auto [scheduled, inserted] = deadlines_.emplace(
                    key,
                    ScheduledDeadline{
                        .request = request,
                        .events = std::weak_ptr<application::IApplicationEventSink>{events},
                    });
                if (!inserted) {
                    return application::PortSubmitResult::Busy;
                }

                const auto existing = deadlinesByTimerId_.find(request.timerId);
                if (existing == deadlinesByTimerId_.end()) {
                    try {
                        static_cast<void>(deadlinesByTimerId_.emplace(request.timerId, scheduled));
                    } catch (...) {
                        deadlines_.erase(scheduled);
                        throw;
                    }
                } else {
                    const DeadlineIterator previous = existing->second;
                    existing->second = scheduled;
                    deadlines_.erase(previous);
                }
            }
            wakeWorker();
            return application::PortSubmitResult::Accepted;
        } catch (...) {
            return application::PortSubmitResult::Busy;
        }
    }

    [[nodiscard]] bool cancel(const std::uint64_t timerId) noexcept {
        {
            std::lock_guard lock(mutex_);
            const auto scheduled = deadlinesByTimerId_.find(timerId);
            if (scheduled == deadlinesByTimerId_.end()) {
                return false;
            }
            deadlines_.erase(scheduled->second);
            deadlinesByTimerId_.erase(scheduled);
        }
        wakeWorker();
        return true;
    }

private:
    using DeadlineMap = std::map<DeadlineKey, ScheduledDeadline>;
    using DeadlineIterator = DeadlineMap::iterator;

    static void postDeadline(const ScheduledDeadline& scheduled) noexcept {
        const std::shared_ptr<application::IApplicationEventSink> events = scheduled.events.lock();
        if (!events) {
            return;
        }

        static_cast<void>(events->postCritical(application::ApplicationEvent{
            application::DeadlineElapsed{
                .context = scheduled.request.context,
                .timerId = scheduled.request.timerId,
            },
        }));
    }

    void wakeWorker() const noexcept {
        static_cast<void>(SetEvent(wake_.get()));
    }

    // Arms the timer with the remaining time to due. The period is floored to whole 100 ns
    // units, so the timer may wake one unit early; the worker loop then re-arms with the
    // remainder instead of dispatching early.
    void armTimer(const std::chrono::steady_clock::time_point due) const noexcept {
        const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
            due - std::chrono::steady_clock::now());
        auto periodUnits = remaining.count() / 100;
        if (periodUnits < 1) {
            periodUnits = 1;
        }
        LARGE_INTEGER relativeDue{};
        relativeDue.QuadPart = -periodUnits;
        static_cast<void>(CancelWaitableTimer(timer_.get()));
        static_cast<void>(SetWaitableTimer(timer_.get(), &relativeDue, 0, nullptr, nullptr, FALSE));
    }

    void run() noexcept {
        for (;;) {
            std::chrono::steady_clock::time_point due{};
            bool hasDeadline = false;
            {
                std::lock_guard lock(mutex_);
                if (stopping_) {
                    return;
                }
                if (!deadlines_.empty()) {
                    due = deadlines_.begin()->first.due;
                    hasDeadline = true;
                }
            }

            if (!hasDeadline) {
                static_cast<void>(WaitForSingleObject(wake_.get(), INFINITE));
                continue;
            }

            if (std::chrono::steady_clock::now() < due) {
                armTimer(due);
                const HANDLE waitHandles[] = {wake_.get(), timer_.get()};
                static_cast<void>(WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE));
                // Re-read the queue head: a replaced earlier deadline or a wake must win over
                // the stale armed due before anything is dispatched.
                continue;
            }

            {
                std::unique_lock lock(mutex_);
                if (stopping_) {
                    return;
                }
                if (deadlines_.empty()) {
                    continue;
                }
                const DeadlineIterator scheduled = deadlines_.begin();
                if (scheduled->first.due > std::chrono::steady_clock::now()) {
                    continue;
                }
                const std::uint64_t timerId = scheduled->second.request.timerId;
                const auto active = deadlinesByTimerId_.find(timerId);
                if (active == deadlinesByTimerId_.end() || active->second != scheduled) {
                    deadlines_.erase(scheduled);
                    continue;
                }

                ScheduledDeadline deadline = std::move(scheduled->second);
                deadlinesByTimerId_.erase(active);
                deadlines_.erase(scheduled);
                dispatchingTimerId_ = timerId;

                lock.unlock();
                postDeadline(deadline);
                lock.lock();

                if (dispatchingTimerId_.has_value() && *dispatchingTimerId_ == timerId) {
                    dispatchingTimerId_.reset();
                }
            }
        }
    }

    void shutdown() noexcept {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            deadlinesByTimerId_.clear();
            deadlines_.clear();
        }
        wakeWorker();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    std::mutex mutex_;
    UniqueHandle wake_;
    UniqueHandle timer_;
    DeadlineMap deadlines_;
    std::unordered_map<std::uint64_t, DeadlineIterator> deadlinesByTimerId_;
    std::optional<std::uint64_t> dispatchingTimerId_;
    std::uint64_t nextSequence_ = 0;
    bool stopping_ = false;
    std::thread worker_;
};

SteadyDeadlineScheduler::SteadyDeadlineScheduler() : impl_(std::make_unique<Impl>()) {}

SteadyDeadlineScheduler::~SteadyDeadlineScheduler() = default;

application::PortSubmitResult
SteadyDeadlineScheduler::schedule(const application::DeadlineRequest& request,
                                  std::shared_ptr<application::IApplicationEventSink> events) {
    return impl_->schedule(request, events);
}

bool SteadyDeadlineScheduler::cancel(const std::uint64_t timerId) noexcept {
    return impl_->cancel(timerId);
}

} // namespace dvs::platform
