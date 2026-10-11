#pragma once

#include "dvs/application/Ports.h"
#include "dvs/media/DecoderBackend.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace dvs::platform {
class FrameBudget;
class GraphicsDeviceBroker;
} // namespace dvs::platform

namespace dvs::media {

struct FrameProviderStatistics final {
    std::uint64_t assembledFrameSets = 0U;
    std::uint64_t totalAssemblyMicroseconds = 0U;
    std::uint64_t maximumAssemblyMicroseconds = 0U;
    std::uint64_t frameSetCacheHits = 0U;
    // E05 complete-FrameSet cache evidence: misses pair with hits, evictions with occupancy.
    // Entry/byte occupancy is the real retained amount, not a capacity claim.
    std::uint64_t frameSetCacheMisses = 0U;
    std::uint64_t frameSetCacheEvictions = 0U;
    std::size_t frameSetCacheEntries = 0U;
    std::size_t frameSetCacheRetainedBytes = 0U;
    // Held-forward step gate (plan 1.6 M1.1): counts captured across the
    // HeldStepping window and reported as deltas.
    std::uint64_t sequentialRequestCount = 0U;
    std::uint64_t cancelCount = 0U;
    std::uint64_t decoderReopenCount = 0U;
    std::uint64_t generationDeltaCount = 0U;
};

// Multi-source review adapter backed by independent software decoder contexts. It accepts bounded
// exact, sequential, and prefetch work, then emits only complete frame sets with one entry per
// loaded source.
class MultiSourceFrameProvider final : public application::IFrameProvider {
public:
    explicit MultiSourceFrameProvider(
        platform::FrameBudget& frameBudget,
        std::size_t requestCapacity = 16U,
        bool lowPriority = false,
        std::shared_ptr<platform::GraphicsDeviceBroker> deviceBroker = {});
    ~MultiSourceFrameProvider() override;

    MultiSourceFrameProvider(const MultiSourceFrameProvider&) = delete;
    MultiSourceFrameProvider& operator=(const MultiSourceFrameProvider&) = delete;
    MultiSourceFrameProvider(MultiSourceFrameProvider&&) = delete;
    MultiSourceFrameProvider& operator=(MultiSourceFrameProvider&&) = delete;

    [[nodiscard]] application::PortSubmitResult
    submit(const application::FrameProviderOpenRequest& request,
           std::shared_ptr<application::IApplicationEventSink> events) override;
    [[nodiscard]] application::PortSubmitResult
    submit(const application::FrameRequest& request,
           std::shared_ptr<application::IApplicationEventSink> events) override;
    [[nodiscard]] application::PortSubmitResult
    submit(const application::FrameProviderCloseRequest& request,
           std::shared_ptr<application::IApplicationEventSink> events) override;
    void cancel(const application::PlaybackRequestContext& context) noexcept override;

    // Component-test diagnostic for the persistent per-source worker invariant.
    [[nodiscard]] std::vector<std::thread::id> decodeWorkerIdsForTesting() const;
    [[nodiscard]] std::vector<std::uint64_t> decodeCountsForTesting() const;
    [[nodiscard]] std::uint64_t frameSetCacheHitCountForTesting() const noexcept;
    [[nodiscard]] std::vector<DecoderBackendStatus> decoderBackendStatuses() const;
    [[nodiscard]] FrameProviderStatistics statistics() const noexcept;

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace dvs::media
