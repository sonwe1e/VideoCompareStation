#include "dvs/application/SampleExport.h"
#include "dvs/domain/MediaError.h"

#include <limits>
#include <string>
#include <utility>
#include <variant>

namespace dvs::application {
namespace {

// A mapping or provenance rejection: the requested output position cannot be turned into a
// triplet because of what the protocol says about it.
[[nodiscard]] domain::MediaError mappingRejected(std::string technicalDetail) {
    return domain::makeMediaError(domain::MediaErrorCode::kInterpolationProtocolInvalid,
                                  domain::MediaOperation::kInterpolationProtocol,
                                  std::nullopt,
                                  false,
                                  std::move(technicalDetail));
}

// A crop or parameter rejection: the caller handed the planner unusable rectangle data.
[[nodiscard]] domain::MediaError cropRejected(std::string technicalDetail) {
    return domain::makeMediaError(domain::MediaErrorCode::kInvalidArgument,
                                  domain::MediaOperation::kInterpolationProtocol,
                                  std::nullopt,
                                  false,
                                  std::move(technicalDetail));
}

// Overflow checks for the non-negative operands used below. The protocol constructor already
// bounds GT indices and ratio terms well inside int64, so these guards stay defensive: they
// keep the planner honest even if that domain-internal bound ever changes.
[[nodiscard]] bool productOverflows(const std::int64_t left, const std::int64_t right) noexcept {
    return left > 0 && right > 0 && right > std::numeric_limits<std::int64_t>::max() / left;
}

[[nodiscard]] bool sumOverflows(const std::int64_t left, const std::int64_t right) noexcept {
    return left > 0 && right > 0 && left > std::numeric_limits<std::int64_t>::max() - right;
}

[[nodiscard]] std::string positionLabel(const domain::FrameId outputFrame) {
    return "Output position " + std::to_string(outputFrame.value());
}

// Local sanity plus optional bounds validation for the fixed crop. A known frame size must be
// positive; the crop must have a non-negative origin, positive extents, edges that stay inside
// int64, and - when the size is known - a rectangle that fits entirely within the frame. When
// the size is unknown (std::nullopt) only the local checks apply.
[[nodiscard]] domain::Status validateCrop(const std::optional<SampleCropRect>& cropRect,
                                          const std::optional<SampleFrameSize>& frameSize) {
    if (frameSize.has_value() && (frameSize->width <= 0 || frameSize->height <= 0)) {
        return domain::Status::failure(
            cropRejected("A known frame size must have positive width and height."));
    }
    if (!cropRect.has_value()) {
        return domain::Status::success();
    }
    const SampleCropRect& crop = *cropRect;
    if (crop.x < 0 || crop.y < 0) {
        return domain::Status::failure(
            cropRejected("The crop origin must be non-negative source pixel coordinates."));
    }
    if (crop.width <= 0 || crop.height <= 0) {
        return domain::Status::failure(
            cropRejected("The crop must have positive width and height."));
    }
    if (sumOverflows(crop.x, crop.width) || sumOverflows(crop.y, crop.height)) {
        return domain::Status::failure(cropRejected("The crop rectangle edges overflow int64."));
    }
    if (frameSize.has_value() &&
        (crop.x + crop.width > frameSize->width || crop.y + crop.height > frameSize->height)) {
        return domain::Status::failure(
            cropRejected("The crop rectangle does not fit inside the known frame size."));
    }
    return domain::Status::success();
}

} // namespace

domain::Result<SampleTripletPlan>
planSampleTriplet(const domain::InterpolationProtocol& protocol,
                  const domain::FrameId outputFrame,
                  const std::optional<SampleCropRect> cropRect,
                  const std::optional<SampleFrameSize> frameSize) {
    // Range rejection stays with the domain: originAt is the single authority on which output
    // positions exist, so its kInvalidFrameId error is forwarded unchanged.
    auto origin = protocol.originAt(outputFrame);
    if (!origin.hasValue()) {
        return domain::Result<SampleTripletPlan>::failure(origin.error());
    }

    const auto* const generated = std::get_if<domain::GeneratedFrame>(&origin.value());
    if (generated == nullptr) {
        if (const auto* const retained = std::get_if<domain::RetainedFrame>(&origin.value())) {
            return domain::Result<SampleTripletPlan>::failure(mappingRejected(
                positionLabel(outputFrame) + " is a retained frame copied from GT frame " +
                std::to_string(retained->gtFrame.value()) +
                "; a passthrough copy has no bracketing input pair, so no triplet exists."));
        }
        if (std::holds_alternative<domain::UnknownFrame>(origin.value())) {
            return domain::Result<SampleTripletPlan>::failure(mappingRejected(
                positionLabel(outputFrame) +
                " has unknown provenance; without a committed mapping the GT inputs and "
                "target cannot be named, so no triplet can be planned."));
        }
        // Only MissingFrame remains of the four origin kinds.
        const auto& missing = std::get<domain::MissingFrame>(origin.value());
        return domain::Result<SampleTripletPlan>::failure(mappingRejected(
            positionLabel(outputFrame) + " is missing from the candidate asset" +
            (missing.expectedGtFrame.has_value()
                 ? " although GT frame " + std::to_string(missing.expectedGtFrame->value()) +
                       " was expected"
                 : std::string{}) +
            "; there is no candidate frame to export."));
    }

    // Implied GT position of the generated instant:
    // (input0 * den + (input1 - input0) * num) / den. The protocol constructor holds
    // input0 < input1, den > 0 and 0 < num < den, so every operand here is non-negative and
    // only the positive corner can overflow.
    const std::int64_t input0 = generated->input0GtFrame.value();
    const std::int64_t input1 = generated->input1GtFrame.value();
    const std::int64_t denominator = generated->targetRatioDenominator;
    const std::int64_t span = input1 - input0;
    if (productOverflows(input0, denominator) ||
        productOverflows(span, generated->targetRatioNumerator)) {
        return domain::Result<SampleTripletPlan>::failure(mappingRejected(
            positionLabel(outputFrame) + " has an implied GT position that overflows int64."));
    }
    const std::int64_t scaledBase = input0 * denominator;
    const std::int64_t scaledOffset = span * generated->targetRatioNumerator;
    if (sumOverflows(scaledBase, scaledOffset)) {
        return domain::Result<SampleTripletPlan>::failure(mappingRejected(
            positionLabel(outputFrame) + " has an implied GT position that overflows int64."));
    }
    const std::int64_t numerator = scaledBase + scaledOffset;
    // The generated instant must land exactly on a GT frame. A position like 3/2 (VFR or a
    // 59.94-style time base) has no GT frame to use as the triplet target, and planning
    // refuses instead of rounding to a neighbour.
    if (numerator % denominator != 0) {
        return domain::Result<SampleTripletPlan>::failure(mappingRejected(
            positionLabel(outputFrame) + " implies GT position " + std::to_string(numerator) + "/" +
            std::to_string(denominator) +
            ", which is not an integer frame; refusing to round to a neighbouring frame."));
    }
    const std::int64_t target = numerator / denominator;
    // Defensive: with the protocol invariants above, an exact target is always strictly between
    // the inputs. A protocol that somehow lost that invariant must not yield a degenerate
    // triplet, so the strict bounds are re-asserted here.
    if (target <= input0 || target >= input1) {
        return domain::Result<SampleTripletPlan>::failure(
            mappingRejected(positionLabel(outputFrame) +
                            " has an implied target that is not strictly between its inputs."));
    }

    if (const auto crop = validateCrop(cropRect, frameSize); !crop.hasValue()) {
        return domain::Result<SampleTripletPlan>::failure(crop.error());
    }

    SampleTripletPlan plan;
    plan.input0GtFrame = generated->input0GtFrame;
    plan.input1GtFrame = generated->input1GtFrame;
    plan.targetGtFrame = domain::GtFrameIndex{target};
    plan.targetRatioNumerator = generated->targetRatioNumerator;
    plan.targetRatioDenominator = generated->targetRatioDenominator;
    plan.candidateOutputFrame = outputFrame;
    plan.mappingOrigin = protocol.mappingOrigin();
    plan.cropRect = cropRect;
    return domain::Result<SampleTripletPlan>::success(plan);
}

} // namespace dvs::application
