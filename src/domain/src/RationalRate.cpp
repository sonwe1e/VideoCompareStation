#include "dvs/domain/RationalRate.h"

#include <array>
#include <limits>
#include <numeric>
#include <string>
#include <utility>

namespace dvs::domain {
namespace {

constexpr std::int64_t kMicrosecondsPerSecond = 1'000'000;

struct DivisionResult final {
    std::int64_t quotient = 0;
    std::int64_t remainder = 0;
};

[[nodiscard]] bool
addChecked(const std::int64_t left, const std::int64_t right, std::int64_t* const result) noexcept {
    if (left > std::numeric_limits<std::int64_t>::max() - right) {
        return false;
    }
    *result = left + right;
    return true;
}

[[nodiscard]] bool multiplyChecked(const std::int64_t left,
                                   const std::int64_t right,
                                   std::int64_t* const result) noexcept {
    if (left == 0 || right == 0) {
        *result = 0;
        return true;
    }
    if (left > std::numeric_limits<std::int64_t>::max() / right) {
        return false;
    }
    *result = left * right;
    return true;
}

// Computes floor(multiplicand * multiplier / divisor) and the remainder without constructing the
// potentially overflowing product. Inputs are non-negative and divisor is positive.
[[nodiscard]] bool multiplyAndDivide(const std::int64_t multiplicand,
                                     const std::int64_t multiplier,
                                     const std::int64_t divisor,
                                     DivisionResult* const result) noexcept {
    if (multiplicand < 0 || multiplier < 0 || divisor <= 0) {
        return false;
    }

    const std::int64_t whole = multiplicand / divisor;
    const std::int64_t fractional = multiplicand % divisor;
    std::int64_t quotient = 0;
    std::int64_t remainder = 0;
    const auto unsignedMultiplier = static_cast<std::uint64_t>(multiplier);

    for (int bit = 62; bit >= 0; --bit) {
        if (quotient > std::numeric_limits<std::int64_t>::max() / 2) {
            return false;
        }
        quotient *= 2;

        std::int64_t carry = 0;
        if (remainder >= divisor - remainder) {
            remainder -= divisor - remainder;
            carry = 1;
        } else {
            remainder += remainder;
        }
        if (!addChecked(quotient, carry, &quotient)) {
            return false;
        }

        const auto bitMask = std::uint64_t{1} << bit;
        if ((unsignedMultiplier & bitMask) == 0U) {
            continue;
        }

        if (!addChecked(quotient, whole, &quotient)) {
            return false;
        }

        carry = 0;
        if (remainder >= divisor - fractional) {
            remainder -= divisor - fractional;
            carry = 1;
        } else {
            remainder += fractional;
        }
        if (!addChecked(quotient, carry, &quotient)) {
            return false;
        }
    }

    *result = DivisionResult{.quotient = quotient, .remainder = remainder};
    return true;
}

[[nodiscard]] bool multiplyFactorsAndDivide(const std::array<std::int64_t, 3>& factors,
                                            const std::int64_t divisor,
                                            std::int64_t* const result,
                                            std::int64_t* const remainderResult) noexcept {
    if (divisor <= 0) {
        return false;
    }

    std::int64_t quotient = 1 / divisor;
    std::int64_t remainder = 1 % divisor;
    for (const std::int64_t factor : factors) {
        if (factor < 0) {
            return false;
        }

        DivisionResult partial;
        if (!multiplyAndDivide(remainder, factor, divisor, &partial)) {
            return false;
        }

        std::int64_t scaledQuotient = 0;
        if (!multiplyChecked(quotient, factor, &scaledQuotient) ||
            !addChecked(scaledQuotient, partial.quotient, &quotient)) {
            return false;
        }
        remainder = partial.remainder;
    }

    *result = quotient;
    *remainderResult = remainder;
    return true;
}

[[nodiscard]] MediaError rationalError(const MediaErrorCode code, std::string technicalDetail) {
    return makeMediaError(
        code, MediaOperation::kRationalConversion, std::nullopt, false, std::move(technicalDetail));
}

} // namespace

Result<RationalRate> RationalRate::create(const std::int64_t numerator,
                                          const std::int64_t denominator) {
    if (numerator <= 0 || denominator <= 0) {
        return Result<RationalRate>::failure(
            rationalError(MediaErrorCode::kInvalidRate,
                          "Frame-rate numerator and denominator must be positive."));
    }

    const std::int64_t divisor = std::gcd(numerator, denominator);
    return Result<RationalRate>::success(RationalRate{numerator / divisor, denominator / divisor});
}

double RationalRate::displayFps() const noexcept {
    return static_cast<double>(numerator_) / static_cast<double>(denominator_);
}

Result<MediaTime> RationalRate::frameStartTime(const FrameId frameId) const {
    if (!frameId.isValid()) {
        return Result<MediaTime>::failure(rationalError(MediaErrorCode::kInvalidFrameId,
                                                        "Frame ID must be zero-based and finite."));
    }

    std::int64_t microseconds = 0;
    std::int64_t remainder = 0;
    if (!multiplyFactorsAndDivide({frameId.value(), denominator_, kMicrosecondsPerSecond},
                                  numerator_,
                                  &microseconds,
                                  &remainder)) {
        return Result<MediaTime>::failure(rationalError(
            MediaErrorCode::kArithmeticOverflow, "Frame-to-microsecond conversion overflowed."));
    }
    if (remainder != 0 && !addChecked(microseconds, 1, &microseconds)) {
        return Result<MediaTime>::failure(rationalError(
            MediaErrorCode::kArithmeticOverflow, "Frame-to-microsecond ceiling overflowed."));
    }
    return Result<MediaTime>::success(MediaTime{microseconds});
}

Result<MediaTime> RationalRate::frameStartTimeRounded(const FrameId frameId) const {
    if (!frameId.isValid()) {
        return Result<MediaTime>::failure(rationalError(MediaErrorCode::kInvalidFrameId,
                                                        "Frame ID must be zero-based and finite."));
    }

    std::int64_t microseconds = 0;
    std::int64_t remainder = 0;
    if (!multiplyFactorsAndDivide({frameId.value(), denominator_, kMicrosecondsPerSecond},
                                  numerator_,
                                  &microseconds,
                                  &remainder)) {
        return Result<MediaTime>::failure(rationalError(
            MediaErrorCode::kArithmeticOverflow, "Frame-to-microsecond conversion overflowed."));
    }
    // Compare without doubling the remainder or adding one to a possibly maximal numerator.
    if (remainder >= numerator_ - remainder && !addChecked(microseconds, 1, &microseconds)) {
        return Result<MediaTime>::failure(rationalError(
            MediaErrorCode::kArithmeticOverflow, "Frame-to-microsecond rounding overflowed."));
    }
    return Result<MediaTime>::success(MediaTime{microseconds});
}

Result<FrameId> RationalRate::frameAtOrBefore(const MediaTime time) const {
    if (time.microseconds() < 0) {
        return Result<FrameId>::failure(
            rationalError(MediaErrorCode::kInvalidArgument,
                          "A canonical frame cannot be derived from negative time."));
    }

    DivisionResult scaled;
    if (!multiplyAndDivide(time.microseconds(), numerator_, denominator_, &scaled)) {
        return Result<FrameId>::failure(rationalError(
            MediaErrorCode::kArithmeticOverflow, "Microsecond-to-frame conversion overflowed."));
    }

    const std::int64_t frameValue = scaled.quotient / kMicrosecondsPerSecond;
    const FrameId frameId{frameValue};
    if (!frameId.isValid()) {
        return Result<FrameId>::failure(
            rationalError(MediaErrorCode::kArithmeticOverflow,
                          "Converted frame ID is outside the canonical range."));
    }
    return Result<FrameId>::success(frameId);
}

Result<MediaTime> RationalRate::frameIntervalCeiling() const {
    DivisionResult interval;
    if (!multiplyAndDivide(denominator_, kMicrosecondsPerSecond, numerator_, &interval)) {
        return Result<MediaTime>::failure(rationalError(MediaErrorCode::kArithmeticOverflow,
                                                        "Frame interval conversion overflowed."));
    }

    std::int64_t microseconds = interval.quotient;
    if (interval.remainder != 0) {
        if (!addChecked(microseconds, 1, &microseconds)) {
            return Result<MediaTime>::failure(rationalError(MediaErrorCode::kArithmeticOverflow,
                                                            "Frame interval ceiling overflowed."));
        }
    }
    return Result<MediaTime>::success(MediaTime{microseconds});
}

} // namespace dvs::domain
