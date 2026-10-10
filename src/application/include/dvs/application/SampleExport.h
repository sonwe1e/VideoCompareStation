#pragma once

#include "dvs/domain/Identifiers.h"
#include "dvs/domain/InterpolationProtocol.h"
#include "dvs/domain/Result.h"

#include <cstdint>
#include <optional>

namespace dvs::application {

// Fixed crop in source pixel coordinates for a "fixed-crop triplet": the same rectangle is
// applied identically to every member (input0, target GT, input1) so the training
// correspondence between the members survives the crop. Coordinates are zero-based with the
// origin at the top-left pixel of the source frame; the rectangle is half-open on the right
// and bottom edges, matching every other pixel rect in the pipeline.
struct SampleCropRect final {
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t width = 0;
    std::int64_t height = 0;

    [[nodiscard]] constexpr bool operator==(const SampleCropRect&) const noexcept = default;
};

// Known pixel dimensions of one source frame, used only to bound-check a SampleCropRect. A
// known size must be positive. When the size is unknown the caller passes std::nullopt and the
// crop only gets the local sanity checks (signs, overflow) instead of a bounds check.
struct SampleFrameSize final {
    std::int64_t width = 0;
    std::int64_t height = 0;

    [[nodiscard]] constexpr bool operator==(const SampleFrameSize&) const noexcept = default;
};

// Pure export plan for one training triplet around a single interpolation-generated frame.
// All frame indices are zero-based internal ordinals; any display index base (1-based listing,
// file naming) is an exporter concern, not a plan concern. The plan is data only: it names
// what to extract, never how or where to write it.
struct SampleTripletPlan final {
    // The two GT frames bracketing the generated instant: the model inputs.
    domain::GtFrameIndex input0GtFrame{0};
    domain::GtFrameIndex input1GtFrame{0};
    // GT frame the generated instant lands on. Planning requires the implied position
    // input0 * den + (input1 - input0) * num to divide evenly by den, so this is always an
    // exact GT frame, never a rounded one.
    domain::GtFrameIndex targetGtFrame{0};
    // GT-relative target position in lowest terms, copied verbatim from the protocol.
    std::int64_t targetRatioNumerator = 1;
    std::int64_t targetRatioDenominator = 2;
    // Position of the generated frame inside the candidate asset itself.
    domain::FrameId candidateOutputFrame{0};
    // How the protocol's mapping came to exist, carried through for traceability.
    domain::MappingOrigin mappingOrigin = domain::MappingOrigin::kManual;
    // Fixed crop applied identically to all members; empty means whole frames.
    std::optional<SampleCropRect> cropRect;

    [[nodiscard]] bool operator==(const SampleTripletPlan&) const noexcept = default;
};

// Turns one interpolation-generated output frame into a verifiable triplet export plan. Pure
// planning: nothing is decoded, encoded or written here.
//
// Rejections, all without side effects:
// - An outputFrame outside the protocol forwards the domain's kInvalidFrameId rejection from
//   originAt.
// - RetainedFrame / UnknownFrame / MissingFrame origins are rejected with
//   kInterpolationProtocolInvalid and a technical detail naming the origin kind, because none
//   of them is a synthesized frame with a bracketing input pair.
// - A generated instant whose implied position does not land on an integer GT frame (VFR /
//   59.94-style time bases) is rejected, never rounded to a neighbour.
// - A target that is not strictly between its inputs is rejected defensively, even though the
//   protocol constructor already guarantees it.
// - Crop parameters that are negative, empty, overflowing, or out of bounds against a known
//   frame size are rejected with kInvalidArgument.
[[nodiscard]] domain::Result<SampleTripletPlan>
planSampleTriplet(const domain::InterpolationProtocol& protocol,
                  domain::FrameId outputFrame,
                  std::optional<SampleCropRect> cropRect = std::nullopt,
                  std::optional<SampleFrameSize> frameSize = std::nullopt);

} // namespace dvs::application
