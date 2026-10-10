#include "dvs/domain/InterpolationProtocol.h"

#include "dvs/domain/MediaError.h"

#include <algorithm>
#include <compare>
#include <numeric>
#include <utility>

namespace dvs::domain {
namespace {

// Upper bound for sequence lengths, GT frame indices and ratio denominators. Every product used
// by the ordering validation stays well inside int64 with this limit.
constexpr std::int64_t kMaxSequenceLength = 1'000'000'000;

MediaError protocolError(std::string technicalDetail) {
    return makeMediaError(MediaErrorCode::kInterpolationProtocolInvalid,
                          MediaOperation::kInterpolationProtocol,
                          std::nullopt,
                          false,
                          std::move(technicalDetail));
}

MediaError invalidArgument(std::string technicalDetail) {
    return makeMediaError(MediaErrorCode::kInvalidArgument,
                          MediaOperation::kInterpolationProtocol,
                          std::nullopt,
                          false,
                          std::move(technicalDetail));
}

// Compares two non-negative rationals with positive denominators without computing products that
// could overflow: whole parts first, then remainders whose cross products stay bounded.
std::weak_ordering compareNonNegativeRational(const std::int64_t leftNumerator,
                                              const std::int64_t leftDenominator,
                                              const std::int64_t rightNumerator,
                                              const std::int64_t rightDenominator) noexcept {
    const std::int64_t leftWhole = leftNumerator / leftDenominator;
    const std::int64_t rightWhole = rightNumerator / rightDenominator;
    if (leftWhole != rightWhole) {
        return leftWhole <=> rightWhole;
    }
    const std::int64_t leftRemainder = leftNumerator % leftDenominator;
    const std::int64_t rightRemainder = rightNumerator % rightDenominator;
    return leftRemainder * rightDenominator <=> rightRemainder * leftDenominator;
}

struct ImpliedPosition final {
    std::int64_t numerator;
    std::int64_t denominator;
};

ImpliedPosition impliedGtPosition(const RetainedFrame& frame) noexcept {
    return ImpliedPosition{.numerator = frame.gtFrame.value(), .denominator = 1};
}

ImpliedPosition impliedGtPosition(const GeneratedFrame& frame) noexcept {
    const std::int64_t inputSpan = frame.input1GtFrame.value() - frame.input0GtFrame.value();
    return ImpliedPosition{.numerator = frame.input0GtFrame.value() * frame.targetRatioDenominator +
                                        inputSpan * frame.targetRatioNumerator,
                           .denominator = frame.targetRatioDenominator};
}

Status
validateGtIndex(const GtFrameIndex index, const std::int64_t gtFrameCount, const char* role) {
    if (!index.isValid() || index.value() >= kMaxSequenceLength) {
        return Status::failure(protocolError(std::string{role} + " is not a valid GT frame index"));
    }
    if (gtFrameCount > 0 && index.value() >= gtFrameCount) {
        return Status::failure(
            protocolError(std::string{role} + " exceeds the known GT frame count"));
    }
    return Status::success();
}

} // namespace

Result<InterpolationProtocol>
InterpolationProtocol::create(const std::int64_t gtFrameCount,
                              const MappingOrigin mappingOrigin,
                              std::vector<OutputFrameOrigin> origins) {
    if (gtFrameCount < 0 || gtFrameCount > kMaxSequenceLength) {
        return Result<InterpolationProtocol>::failure(
            protocolError("GT frame count is out of range"));
    }
    if (origins.empty()) {
        return Result<InterpolationProtocol>::failure(
            protocolError("protocol must describe at least one output position"));
    }
    if (static_cast<std::int64_t>(origins.size()) > kMaxSequenceLength) {
        return Result<InterpolationProtocol>::failure(
            protocolError("output position count is out of range"));
    }

    std::optional<ImpliedPosition> previousPosition;
    for (auto& origin : origins) {
        if (auto* retained = std::get_if<RetainedFrame>(&origin)) {
            if (const auto check = validateGtIndex(retained->gtFrame, gtFrameCount, "retained");
                !check.hasValue()) {
                return Result<InterpolationProtocol>::failure(check.error());
            }
        } else if (auto* generated = std::get_if<GeneratedFrame>(&origin)) {
            if (const auto check =
                    validateGtIndex(generated->input0GtFrame, gtFrameCount, "generated input0");
                !check.hasValue()) {
                return Result<InterpolationProtocol>::failure(check.error());
            }
            if (const auto check =
                    validateGtIndex(generated->input1GtFrame, gtFrameCount, "generated input1");
                !check.hasValue()) {
                return Result<InterpolationProtocol>::failure(check.error());
            }
            if (generated->input0GtFrame >= generated->input1GtFrame) {
                return Result<InterpolationProtocol>::failure(
                    protocolError("generated inputs are not strictly ordered"));
            }
            if (generated->targetRatioDenominator <= 0 ||
                generated->targetRatioDenominator > kMaxSequenceLength ||
                generated->targetRatioNumerator <= 0 ||
                generated->targetRatioNumerator >= generated->targetRatioDenominator) {
                return Result<InterpolationProtocol>::failure(
                    protocolError("target time ratio is not strictly between the inputs"));
            }
            const auto divisor =
                std::gcd(generated->targetRatioNumerator, generated->targetRatioDenominator);
            generated->targetRatioNumerator /= divisor;
            generated->targetRatioDenominator /= divisor;
        } else if (const auto* missing = std::get_if<MissingFrame>(&origin)) {
            if (missing->expectedGtFrame.has_value()) {
                if (const auto check = validateGtIndex(
                        *missing->expectedGtFrame, gtFrameCount, "missing expectation");
                    !check.hasValue()) {
                    return Result<InterpolationProtocol>::failure(check.error());
                }
            }
        }

        const auto* retained = std::get_if<RetainedFrame>(&origin);
        const auto* generated = std::get_if<GeneratedFrame>(&origin);
        if (retained == nullptr && generated == nullptr) {
            continue;
        }
        const ImpliedPosition position =
            retained != nullptr ? impliedGtPosition(*retained) : impliedGtPosition(*generated);
        if (previousPosition.has_value() &&
            compareNonNegativeRational(position.numerator,
                                       position.denominator,
                                       previousPosition->numerator,
                                       previousPosition->denominator) <= 0) {
            return Result<InterpolationProtocol>::failure(
                protocolError("output positions do not map to strictly increasing GT positions"));
        }
        previousPosition = position;
    }

    return Result<InterpolationProtocol>::success(
        InterpolationProtocol{gtFrameCount, mappingOrigin, std::move(origins)});
}

InterpolationProtocol::InterpolationProtocol(const std::int64_t gtFrameCount,
                                             const MappingOrigin mappingOrigin,
                                             std::vector<OutputFrameOrigin> origins) noexcept
    : gtFrameCount_(gtFrameCount), mappingOrigin_(mappingOrigin), origins_(std::move(origins)) {}

std::int64_t InterpolationProtocol::frameCount() const noexcept {
    return static_cast<std::int64_t>(origins_.size());
}

std::int64_t InterpolationProtocol::gtFrameCount() const noexcept {
    return gtFrameCount_;
}

MappingOrigin InterpolationProtocol::mappingOrigin() const noexcept {
    return mappingOrigin_;
}

Result<OutputFrameOrigin> InterpolationProtocol::originAt(const FrameId outputFrame) const {
    if (!outputFrame.isValid() || outputFrame.value() >= frameCount()) {
        return Result<OutputFrameOrigin>::failure(
            makeMediaError(MediaErrorCode::kInvalidFrameId,
                           MediaOperation::kInterpolationProtocol,
                           std::nullopt,
                           false,
                           "output frame is outside the protocol"));
    }
    return Result<OutputFrameOrigin>::success(
        origins_[static_cast<std::size_t>(outputFrame.value())]);
}

std::optional<FrameId> InterpolationProtocol::nextGeneratedFrame(const FrameId after) const {
    const std::int64_t count = frameCount();
    if (after.value() >= count) {
        return std::nullopt;
    }
    const std::int64_t start = after.value() < 0 ? std::int64_t{0} : after.value() + 1;
    for (std::int64_t index = start; index < count; ++index) {
        if (std::holds_alternative<GeneratedFrame>(origins_[static_cast<std::size_t>(index)])) {
            return FrameId{index};
        }
    }
    return std::nullopt;
}

std::optional<FrameId> InterpolationProtocol::previousGeneratedFrame(const FrameId before) const {
    const std::int64_t count = frameCount();
    if (before.value() < 0) {
        return std::nullopt;
    }
    const std::int64_t end = std::min(before.value(), count);
    for (std::int64_t index = end - 1; index >= 0; --index) {
        if (std::holds_alternative<GeneratedFrame>(origins_[static_cast<std::size_t>(index)])) {
            return FrameId{index};
        }
    }
    return std::nullopt;
}

Result<InterpolationProtocol> buildDecimatedProtocol(const DecimationPlan& plan,
                                                     const MappingOrigin mappingOrigin,
                                                     const TailPolicy tailPolicy) {
    if (plan.gtFrameCount <= 0 || plan.gtFrameCount > kMaxSequenceLength) {
        return Result<InterpolationProtocol>::failure(
            invalidArgument("decimation needs a positive GT frame count"));
    }
    if (plan.stride < 2 || plan.stride > kMaxSequenceLength) {
        return Result<InterpolationProtocol>::failure(
            invalidArgument("decimation stride must be at least 2"));
    }
    if (plan.phase < 0 || plan.phase >= plan.stride) {
        return Result<InterpolationProtocol>::failure(
            invalidArgument("decimation phase must be within [0, stride)"));
    }
    if (plan.phase > plan.gtFrameCount - 1) {
        return Result<InterpolationProtocol>::failure(
            invalidArgument("GT sequence ends before the first retained frame"));
    }

    std::vector<OutputFrameOrigin> origins;
    for (std::int64_t retainedGtFrame = plan.phase; retainedGtFrame < plan.gtFrameCount;
         retainedGtFrame += plan.stride) {
        origins.push_back(RetainedFrame{GtFrameIndex{retainedGtFrame}});
        const std::int64_t nextRetainedGtFrame = retainedGtFrame + plan.stride;
        if (nextRetainedGtFrame >= plan.gtFrameCount) {
            if (tailPolicy == TailPolicy::kMarkPartialIntervalMissing &&
                plan.gtFrameCount - 1 > retainedGtFrame) {
                for (std::int64_t expected = retainedGtFrame + 1; expected < plan.gtFrameCount;
                     ++expected) {
                    origins.push_back(MissingFrame{GtFrameIndex{expected}});
                }
            }
            break;
        }
        for (std::int64_t step = 1; step < plan.stride; ++step) {
            origins.push_back(GeneratedFrame{GtFrameIndex{retainedGtFrame},
                                             GtFrameIndex{nextRetainedGtFrame},
                                             step,
                                             plan.stride});
        }
    }
    return InterpolationProtocol::create(plan.gtFrameCount, mappingOrigin, std::move(origins));
}

} // namespace dvs::domain
