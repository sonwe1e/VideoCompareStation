#include "dvs/application/PlaybackTrace.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace dvs::application {

void PlaybackTraceBuffer::setSink(ITraceSink* sink) noexcept {
    std::lock_guard lock(sinkMutex_);
    sink_ = sink;
}

bool PlaybackTraceBuffer::record(const TraceEvent event) noexcept {
    // Blocking lock: the critical section is a single-event copy (nanoseconds), and producers
    // never touch the sink or do I/O here, so a brief serialization cannot stall playback.
    // try-lock previously dropped events under producer-producer contention, which made
    // multi-source traces (several decode workers + render + UI threads) fail the gate with
    // sub-percent losses even when the ring was far from full.
    std::lock_guard lock(mutex_);
    // Monotonic positions distinguish empty from full without sacrificing a queue slot.
    if (head_ - tail_ >= kCapacity) {
        overflow_.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    buffer_[indexFor(head_)] = event;
    ++head_;
    return true;
}

std::size_t PlaybackTraceBuffer::drain(TraceEvent* out, const std::size_t max) noexcept {
    std::lock_guard lock(mutex_);
    const std::uint64_t available = head_ - tail_;
    const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(available, max));
    for (std::size_t i = 0U; i < count; ++i) {
        out[i] = buffer_[indexFor(tail_ + static_cast<std::uint64_t>(i))];
    }
    tail_ += static_cast<std::uint64_t>(count);
    return count;
}

std::size_t PlaybackTraceBuffer::drainToSink() noexcept {
    // Copy bounded batches while holding the queue lock, then perform sink I/O after releasing it.
    // A slow filesystem must never extend the producer critical section. Keep the independent
    // sink lifecycle lock until all I/O completes so setSink(nullptr) is a safe ownership fence.
    constexpr std::size_t kDrainBatchSize = 256U;
    std::array<TraceEvent, kDrainBatchSize> batch;
    std::unique_lock sinkLock(sinkMutex_);
    ITraceSink* destination = sink_;
    std::size_t total = 0U;
    if (destination != nullptr) {
        for (;;) {
            const std::size_t count = drain(batch.data(), batch.size());
            if (count == 0U) {
                break;
            }
            total += count;
            for (std::size_t index = 0U; index < count; ++index) {
                if (!destination->append(batch[index])) {
                    // Count this event and the unattempted suffix as lost. Any earlier prefix was
                    // accepted and must not be double-counted.
                    overflow_.fetch_add(count - index, std::memory_order_relaxed);
                    break;
                }
            }
        }
    } else {
        for (;;) {
            const std::size_t count = drain(batch.data(), batch.size());
            total += count;
            if (count == 0U) {
                break;
            }
        }
    }

    std::uint64_t lost = 0U;
    {
        std::lock_guard lock(mutex_);
        lost = overflow_.load(std::memory_order_relaxed) - reportedOverflow_;
    }
    if (destination != nullptr && lost > 0U) {
        if (destination->recordOverflow(lost)) {
            std::lock_guard lock(mutex_);
            reportedOverflow_ += lost;
        }
    }
    return total;
}

std::uint64_t PlaybackTraceBuffer::overflowCount() const noexcept {
    return overflow_.load(std::memory_order_relaxed);
}

void PlaybackTraceBuffer::reset() noexcept {
    // Match drainToSink's sink-before-queue lock order.
    std::lock_guard sinkLock(sinkMutex_);
    std::lock_guard queueLock(mutex_);
    head_ = 0U;
    tail_ = 0U;
    overflow_.store(0U, std::memory_order_relaxed);
    reportedOverflow_ = 0U;
    sink_ = nullptr;
}

PlaybackTrace& PlaybackTrace::instance() noexcept {
    // ReviewRuntime may finish bounded teardown on a detached control thread after the caller has
    // returned. Keep this process-global diagnostic alive through CRT shutdown so that late
    // teardown cannot race destruction of its mutex/atomics.
    // This intentional process-lifetime allocation is not registered for static destruction.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    static PlaybackTrace* const trace = new PlaybackTrace;
    return *trace;
}

void PlaybackTrace::installSink(ITraceSink* sink) noexcept {
    buffer_.setSink(sink);
}

bool PlaybackTrace::enabled() const noexcept {
    return now_.load(std::memory_order_acquire) != nullptr;
}

void PlaybackTrace::enable(const Clock nowMicroseconds) noexcept {
    now_.store(nowMicroseconds, std::memory_order_release);
}

void PlaybackTrace::disable() noexcept {
    now_.store(nullptr, std::memory_order_release);
}

void PlaybackTrace::record(const TraceEventKind kind,
                           const TraceIdentity& identity,
                           const std::uint64_t payload,
                           std::optional<TraceIncomingIdentity> incoming) noexcept {
    const Clock clock = now_.load(std::memory_order_acquire);
    if (clock == nullptr) {
        return;
    }
    TraceEvent event{
        .identity = identity,
        .kind = kind,
        .timestampMicroseconds = clock(),
        .payload = payload,
        .incoming = incoming,
    };
    static_cast<void>(buffer_.record(event));
}

std::size_t PlaybackTrace::drain(TraceEvent* out, const std::size_t max) noexcept {
    return buffer_.drain(out, max);
}

std::size_t PlaybackTrace::drainToSink() noexcept {
    return buffer_.drainToSink();
}

std::uint64_t PlaybackTrace::overflowCount() const noexcept {
    return buffer_.overflowCount();
}

void PlaybackTrace::reset() noexcept {
    disable();
    buffer_.reset();
}

std::uint64_t traceNowMicroseconds() noexcept {
    // steady_clock is monotonic and not subject to system-time adjustments, so deltas between
    // trace records measure true elapsed wall-clock on the worker. The epoch is arbitrary.
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

} // namespace dvs::application
