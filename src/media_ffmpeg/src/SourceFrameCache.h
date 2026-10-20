#pragma once

#include "dvs/application/FrameHandle.h"
#include "dvs/application/NormalizedFrameFormat.h"
#include "dvs/domain/FrameTimeline.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>

namespace dvs::media::internal {

struct NormalizationProfile final {
    application::NormalizedFrameFormat format = application::NormalizedFrameFormat::Nv12_8;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;

    [[nodiscard]] constexpr bool operator==(const NormalizationProfile&) const noexcept = default;
};

struct SourceFrameCacheKey final {
    std::string sourceFingerprint;
    domain::FrameId sourceFrame;
    NormalizationProfile profile;

    [[nodiscard]] bool operator==(const SourceFrameCacheKey&) const noexcept = default;
};

struct SourceFrameCacheKeyHash final {
    [[nodiscard]] std::size_t operator()(const SourceFrameCacheKey& key) const noexcept;
};

struct CachedSourceFrame final {
    application::FrameHandle handle;
    domain::MediaTime presentationTime;
};

// Per-source capacity planning. The previous flat byte cap was sized for 1080p NV12 only: it held
// four NV12 frames, two P010 frames, and a single 4K frame, so held-backward had no window to
// consume and 4K fell through to one exact seek per step. Sizing from the descriptor's pixel
// layout expresses the same global budget in frames, which is the unit the reverse window and
// read-ahead actually need.
//
// The frame caps bound a single source so one large source cannot hold the whole budget, and the
// 48 MiB cap keeps two sources at 96 MiB, i.e. inside the half-budget share they are given.
inline constexpr std::size_t kMaximumSourceCacheFrames = 24U;
inline constexpr std::size_t kMaximumSourceCacheBytesPerSource = std::size_t{48U} * 1024U * 1024U;

// Bytes one normalized frame occupies for the given storage format. NV12 is 1.5 bytes/pixel and
// P010 is 3 bytes/pixel; unknown or packed formats use 4 bytes/pixel so the estimate never
// understates a frame the cache will really retain (understating would overfill the budget).
[[nodiscard]] std::size_t estimatedFrameBytes(std::uint32_t width,
                                              std::uint32_t height,
                                              application::NormalizedFrameFormat format) noexcept;

// Capacity for one source cache: the smaller of the source's budget share and the
// byte/frame-count caps. Never larger than `budgetShareBytes`, so the actor caches across all
// sources stay inside the share the provider partitioned for them; a source whose share cannot
// hold two frames is left that small on purpose, and the actor's `< 2` window gate sends it to
// per-step Exact instead of the planner widening the global budget for it.
[[nodiscard]] std::size_t sourceCacheCapacityBytes(std::size_t budgetShareBytes,
                                                   std::size_t frameBytes) noexcept;

// Actor-owned byte-bounded LRU. It is intentionally not synchronized: the owning decode actor
// is the only thread that reads or mutates it.
class SourceFrameCache final {
public:
    explicit SourceFrameCache(std::size_t capacityBytes) noexcept;

    [[nodiscard]] std::optional<CachedSourceFrame> find(const SourceFrameCacheKey& key);
    void insert(SourceFrameCacheKey key, CachedSourceFrame frame);
    // Drops all cached content. Callers use this to invalidate frames (decoder close, read-ahead
    // with a one-frame cache); the run's eviction evidence survives, because a content
    // invalidation is not a statistics reset.
    void clear() noexcept;
    // Explicit per-open statistics reset: zeroes the eviction counter together with the actor's
    // other per-session counters.
    void resetStatistics() noexcept;

    [[nodiscard]] std::size_t capacityBytes() const noexcept;
    [[nodiscard]] std::size_t retainedBytes() const noexcept;
    [[nodiscard]] std::size_t entryCount() const noexcept;
    // Entries dropped by evictToFit since the last explicit statistics reset (open). E05 cache
    // evidence: eviction pressure, read next to hits, tells a warm cache from one that keeps
    // discarding what reviews revisit.
    [[nodiscard]] std::uint64_t evictionCount() const noexcept;

private:
    struct Entry final {
        SourceFrameCacheKey key;
        CachedSourceFrame frame;
        std::size_t bytes = 0U;
    };

    using EntryList = std::list<Entry>;
    using EntryIndex =
        std::unordered_map<SourceFrameCacheKey, EntryList::iterator, SourceFrameCacheKeyHash>;

    void evictToFit(std::size_t incomingBytes) noexcept;

    std::size_t capacityBytes_ = 0U;
    std::size_t retainedBytes_ = 0U;
    std::uint64_t evictionCount_ = 0U;
    EntryList entries_;
    EntryIndex index_;
};

} // namespace dvs::media::internal
