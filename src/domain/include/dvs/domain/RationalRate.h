#pragma once

#include "dvs/domain/Identifiers.h"
#include "dvs/domain/Result.h"

#include <cstdint>

namespace dvs::domain {

class RationalRate final {
public:
    [[nodiscard]] static Result<RationalRate> create(std::int64_t numerator,
                                                     std::int64_t denominator);

    [[nodiscard]] constexpr std::int64_t numerator() const noexcept {
        return numerator_;
    }

    [[nodiscard]] constexpr std::int64_t denominator() const noexcept {
        return denominator_;
    }

    [[nodiscard]] double displayFps() const noexcept;
    // Returns the first integer microsecond at or after the exact rational frame boundary.
    // Paired with frameAtOrBefore(), this makes each returned start time map back to its FrameId.
    [[nodiscard]] Result<MediaTime> frameStartTime(FrameId frameId) const;
    // Nearest whole microsecond, with exact halves rounded up, for matching packet presentation
    // times. This is not a canonical lower bound; frameStartTime() keeps its ceiling contract.
    [[nodiscard]] Result<MediaTime> frameStartTimeRounded(FrameId frameId) const;
    [[nodiscard]] Result<FrameId> frameAtOrBefore(MediaTime time) const;
    [[nodiscard]] Result<MediaTime> frameIntervalCeiling() const;

    [[nodiscard]] constexpr bool operator==(const RationalRate&) const noexcept = default;

private:
    constexpr RationalRate(const std::int64_t numerator, const std::int64_t denominator) noexcept
        : numerator_(numerator), denominator_(denominator) {}

    std::int64_t numerator_;
    std::int64_t denominator_;
};

} // namespace dvs::domain
