#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace dvs::domain {

// FrameId is the zero-based display-order ordinal on the canonical timeline. It is independent of
// source timestamps. INT64_MAX is reserved as the exclusive end boundary for a final valid frame,
// so it is never itself a valid frame ID.
class FrameId final {
public:
    constexpr explicit FrameId(const std::int64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::int64_t value() const noexcept {
        return value_;
    }

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return value_ >= 0 && value_ < std::numeric_limits<std::int64_t>::max();
    }

    [[nodiscard]] constexpr auto operator<=>(const FrameId&) const noexcept = default;

private:
    std::int64_t value_;
};

// MediaTime is a signed microsecond value. Negative values remain valid for adapter PTS data;
// timeline conversion explicitly rejects them where a FrameId is required.
class MediaTime final {
public:
    constexpr explicit MediaTime(const std::int64_t microseconds) noexcept
        : microseconds_(microseconds) {}

    [[nodiscard]] constexpr std::int64_t microseconds() const noexcept {
        return microseconds_;
    }

    [[nodiscard]] constexpr auto operator<=>(const MediaTime&) const noexcept = default;

private:
    std::int64_t microseconds_;
};

template <typename Tag> class CounterId final {
public:
    constexpr explicit CounterId(const std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }

    [[nodiscard]] constexpr auto operator<=>(const CounterId&) const noexcept = default;

private:
    std::uint64_t value_;
};

struct SessionIdTag;
struct SessionEpochTag;
struct TopologyRevisionTag;
struct TimelineRevisionTag;
struct AlignmentRevisionTag;
struct PlaybackGenerationTag;
struct DeviceGenerationTag;
struct RequestIdTag;
struct CommandIdTag;

using SessionId = CounterId<SessionIdTag>;
using SessionEpoch = CounterId<SessionEpochTag>;
using TopologyRevision = CounterId<TopologyRevisionTag>;
using TimelineRevision = CounterId<TimelineRevisionTag>;
using AlignmentRevision = CounterId<AlignmentRevisionTag>;
using PlaybackGeneration = CounterId<PlaybackGenerationTag>;
using DeviceGeneration = CounterId<DeviceGenerationTag>;
using RequestId = CounterId<RequestIdTag>;
using CommandId = CounterId<CommandIdTag>;

// SourceId identifies one loaded input within a comparison session. It lives with the other
// identifiers (not in ComparisonSource.h) so error and event types can name a source without
// pulling the media descriptor graph into every include.
using SourceId = std::uint32_t;

} // namespace dvs::domain
