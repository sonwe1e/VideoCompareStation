#pragma once

#include "dvs/application/PlaybackEvidence.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dvs::application {

// Sorted-ascending percentile reservoir behind PlaybackLatencyStats. One instance owns the
// bounded sample set for one measured stage; the caller owns the published aggregate and pulls
// the percentiles out through finalizeStats(). Shared by the trace-side evidence collector
// (PlaybackEvidence.cpp), the coordinator's planned-to-published frame-set accounting (E03), and
// the media-layer decode-latency percentiles, so every percentile in the product uses the same
// nearest-rank definition and the same truncation behavior.
//
// Truncation keeps the largest samples once the reservoir is full: count and max stay exact for
// every sample, while the percentiles are exact below the cap and biased upward beyond it (the
// conservative direction for stall diagnosis, documented on PlaybackLatencyStats).
class PlaybackLatencyReservoir final {
public:
    explicit PlaybackLatencyReservoir(const std::size_t capacity) : capacity_(capacity) {}

    void add(const std::uint64_t sample, PlaybackLatencyStats& stats) {
        stats.count += 1U;
        stats.maxMicroseconds = std::max(stats.maxMicroseconds, sample);
        if (capacity_ == 0U) {
            // A zero-capacity reservoir keeps the exact count and maximum but retains nothing;
            // without this branch the full-eviction path below would call front() on an empty
            // vector (UB) because size() < 0 is never true.
            stats.truncatedSampleCount += 1U;
            return;
        }
        if (samples_.size() < capacity_) {
            samples_.insert(std::lower_bound(samples_.begin(), samples_.end(), sample), sample);
            return;
        }
        // The reservoir is full: exactly one sample stays outside it. Evicting the smallest
        // keeps the percentile biased upward rather than downward under truncation.
        stats.truncatedSampleCount += 1U;
        if (sample <= samples_.front()) {
            return;
        }
        samples_.erase(samples_.begin());
        samples_.insert(std::lower_bound(samples_.begin(), samples_.end(), sample), sample);
    }

    // Writes the nearest-rank percentiles into `stats`. Idempotent; call whenever the aggregate
    // is published. An empty reservoir leaves all percentiles at zero (nothing measured), never
    // a fabricated zero-time sample.
    void finalizeStats(PlaybackLatencyStats& stats) const {
        stats.p50Microseconds = percentile(50U);
        stats.p95Microseconds = percentile(95U);
        stats.p99Microseconds = percentile(99U);
    }

    void reset() {
        samples_.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return samples_.size();
    }

private:
    // Nearest-rank percentile: the ceil(p * n)-th smallest sample.
    [[nodiscard]] std::uint64_t percentile(const std::uint64_t percentage) const {
        if (samples_.empty()) {
            return 0U;
        }
        const std::size_t rank =
            static_cast<std::size_t>((percentage * samples_.size() + (100U - 1U)) / 100U);
        const std::size_t index = std::min(rank, samples_.size()) - 1U;
        return samples_[index];
    }

    std::size_t capacity_;
    std::vector<std::uint64_t> samples_;
};

} // namespace dvs::application
