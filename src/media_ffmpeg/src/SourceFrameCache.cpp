#include "SourceFrameCache.h"

#include <algorithm>
#include <functional>
#include <type_traits>
#include <utility>

namespace dvs::media::internal {
namespace {

void combineHash(std::size_t& seed, const std::size_t value) noexcept {
    seed ^= value + 0x9E3779B9U + (seed << 6U) + (seed >> 2U);
}

} // namespace

std::size_t estimatedFrameBytes(const std::uint32_t width,
                                const std::uint32_t height,
                                const application::NormalizedFrameFormat format) noexcept {
    if (width == 0U || height == 0U) {
        return 0U;
    }
    // Twice the bytes-per-pixel so NV12's 1.5 stays integral: 3 = NV12, 6 = P010, 8 = BGRA.
    std::uint64_t twiceBytesPerPixel = 8U;
    switch (format) {
    case application::NormalizedFrameFormat::Nv12_8:
        twiceBytesPerPixel = 3U;
        break;
    case application::NormalizedFrameFormat::P010_10:
        twiceBytesPerPixel = 6U;
        break;
    case application::NormalizedFrameFormat::Bgra8:
        twiceBytesPerPixel = 8U;
        break;
    }
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    // Round up: a frame estimate below the stored size would let the cache overrun its share.
    return static_cast<std::size_t>(((pixels * twiceBytesPerPixel) + 1U) / 2U);
}

std::size_t sourceCacheCapacityBytes(const std::size_t budgetShareBytes,
                                     const std::size_t frameBytes) noexcept {
    if (budgetShareBytes == 0U || frameBytes == 0U) {
        return 0U;
    }
    const std::uint64_t framesCap =
        static_cast<std::uint64_t>(frameBytes) * kMaximumSourceCacheFrames;
    return static_cast<std::size_t>(
        (std::min)(static_cast<std::uint64_t>(budgetShareBytes), framesCap));
}

std::size_t SourceFrameCacheKeyHash::operator()(const SourceFrameCacheKey& key) const noexcept {
    std::size_t result = std::hash<std::string>{}(key.sourceFingerprint);
    combineHash(result, std::hash<std::int64_t>{}(key.sourceFrame.value()));
    combineHash(result,
                std::hash<std::underlying_type_t<application::NormalizedFrameFormat>>{}(
                    static_cast<std::underlying_type_t<application::NormalizedFrameFormat>>(
                        key.profile.format)));
    combineHash(result, std::hash<std::uint32_t>{}(key.profile.width));
    combineHash(result, std::hash<std::uint32_t>{}(key.profile.height));
    return result;
}

SourceFrameCache::SourceFrameCache( // NOLINT(bugprone-exception-escape)
    const std::size_t capacityBytes) noexcept
    : capacityBytes_(capacityBytes) {}

std::optional<CachedSourceFrame> SourceFrameCache::find(const SourceFrameCacheKey& key) {
    const EntryIndex::iterator indexed = index_.find(key);
    if (indexed == index_.end()) {
        return std::nullopt;
    }
    const EntryList::iterator entry = indexed->second;
    entries_.splice(entries_.begin(), entries_, entry);
    return entry->frame;
}

void SourceFrameCache::insert(SourceFrameCacheKey key, CachedSourceFrame frame) {
    const std::size_t bytes = frame.handle.accountedBytes();
    if (capacityBytes_ == 0U || bytes == 0U || bytes > capacityBytes_) {
        return;
    }

    const EntryIndex::iterator existing = index_.find(key);
    if (existing != index_.end()) {
        retainedBytes_ -= existing->second->bytes;
        entries_.erase(existing->second);
        index_.erase(existing);
    }
    evictToFit(bytes);
    entries_.push_front(Entry{
        .key = std::move(key),
        .frame = std::move(frame),
        .bytes = bytes,
    });
    try {
        const bool inserted = index_.emplace(entries_.front().key, entries_.begin()).second;
        if (!inserted) {
            entries_.pop_front();
            return;
        }
    } catch (...) {
        entries_.pop_front();
        throw;
    }
    retainedBytes_ += bytes;
}

void SourceFrameCache::clear() noexcept {
    index_.clear();
    entries_.clear();
    retainedBytes_ = 0U;
}

void SourceFrameCache::resetStatistics() noexcept {
    evictionCount_ = 0U;
}

std::size_t SourceFrameCache::capacityBytes() const noexcept {
    return capacityBytes_;
}

std::size_t SourceFrameCache::retainedBytes() const noexcept {
    return retainedBytes_;
}

std::size_t SourceFrameCache::entryCount() const noexcept {
    return entries_.size();
}

std::uint64_t SourceFrameCache::evictionCount() const noexcept {
    return evictionCount_;
}

void SourceFrameCache::evictToFit(const std::size_t incomingBytes) noexcept {
    while (!entries_.empty() && retainedBytes_ > capacityBytes_ - incomingBytes) {
        retainedBytes_ -= entries_.back().bytes;
        index_.erase(entries_.back().key);
        entries_.pop_back();
        ++evictionCount_;
    }
}

} // namespace dvs::media::internal
