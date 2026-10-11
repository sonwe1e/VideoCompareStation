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

// Actor-owned byte-bounded LRU. It is intentionally not synchronized: the owning decode actor
// is the only thread that reads or mutates it.
class SourceFrameCache final {
public:
    explicit SourceFrameCache(std::size_t capacityBytes) noexcept;

    [[nodiscard]] std::optional<CachedSourceFrame> find(const SourceFrameCacheKey& key);
    void insert(SourceFrameCacheKey key, CachedSourceFrame frame);
    void clear() noexcept;

    [[nodiscard]] std::size_t capacityBytes() const noexcept;
    [[nodiscard]] std::size_t retainedBytes() const noexcept;
    [[nodiscard]] std::size_t entryCount() const noexcept;
    // Entries dropped by evictToFit since construction. E05 cache evidence: eviction pressure,
    // read next to hits, tells a warm cache from one that keeps discarding what reviews revisit.
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
