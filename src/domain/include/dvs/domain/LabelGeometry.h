#pragma once

#include "dvs/domain/Result.h"

#include <cstdint>
#include <vector>

namespace dvs::domain {

// Pure geometry for sample labels: coordinate and vector transforms for optical-flow and mask
// label grids. This module never touches image pixels, performs no I/O, and keeps no state; it
// only rearranges label samples and recomputes their validity under a known frame transform.
//
// Sampling contract shared by every grid below: samples sit on pixel centres, one per integer
// position; column x increases to the right and row y increases downwards; the sample at column
// x / row y is stored at index y * width + x (row major).

// Direction a flow label is expressed in. kForward: samples sit in the earlier frame and point
// into the later frame (sample x maps to x + f(x) there). kBackward: samples sit in the later
// frame and point into the earlier frame. The direction is part of the label's meaning and every
// transform in this module preserves it.
//
// Time reversal (turning a forward label into the backward label of the swapped pair) is
// deliberately not offered. The backward field of a reversed pair is the inverse of the forward
// mapping and cannot be produced by negating vectors per sample: a negated vector is anchored at
// the wrong pixel unless the forward endpoint happened to land exactly on a pixel centre.
// Solving for the true inverse mapping is a resampling decision, and this module refuses to
// manufacture wrong labels silently, so no such function exists.
enum class FlowDirection {
    kForward,
    kBackward,
};

// Dense optical-flow label on a pixel-centre grid. Vectors are in pixels; dx is positive to the
// right and dy is positive downwards. A vector points from its sample position x to the endpoint
// x + f(x), expressed in the coordinates of the frame the flow points into.
struct FlowGrid final {
    std::int64_t width = 0;
    std::int64_t height = 0;
    std::vector<double> dx;
    std::vector<double> dy;
    FlowDirection direction = FlowDirection::kForward;

    [[nodiscard]] bool operator==(const FlowGrid&) const noexcept = default;
};

// Per-sample validity of a label grid on the same row-major layout as FlowGrid: 1 where the
// label is real supervision, 0 where it must not be trusted (out-of-frame endpoints, padded
// canvas, transform targets no source sample covers). Every entry must be 0 or 1.
struct ValidMask final {
    std::int64_t width = 0;
    std::int64_t height = 0;
    std::vector<std::uint8_t> valid;

    [[nodiscard]] bool operator==(const ValidMask&) const noexcept = default;
};

// Mask label values (class indices, alpha, probabilities) on a pixel-centre grid, row major,
// same layout as FlowGrid. Values are double so integer class ids and continuous values share
// one representation; every mask operation below is an exact index rearrangement, so class masks
// need no interpolation rule here and continuous masks are never rounded.
//
// Affine transforms of masks are deliberately absent from this API: they would require a
// resampling decision and would leave uncovered holes exactly where flow endpoints go out of
// frame. This module performs exact rearrangements only (flip, crop, pad); anything finer is
// refused by absence rather than silently approximated.
struct MaskGrid final {
    std::int64_t width = 0;
    std::int64_t height = 0;
    std::vector<double> values;

    [[nodiscard]] bool operator==(const MaskGrid&) const noexcept = default;
};

// Affine map in row-vector convention: y = x * A + t, that is
// y.x = x.x * a11 + x.y * a21 + tx and y.y = x.x * a12 + x.y * a22 + ty.
struct Affine2 final {
    double a11 = 1.0;
    double a12 = 0.0;
    double a21 = 0.0;
    double a22 = 1.0;
    double tx = 0.0;
    double ty = 0.0;

    [[nodiscard]] constexpr bool operator==(const Affine2&) const noexcept = default;
};

// Half-open sub-rectangle of a grid: columns [x, x + width), rows [y, y + height).
struct GridRect final {
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t width = 0;
    std::int64_t height = 0;

    [[nodiscard]] constexpr bool operator==(const GridRect&) const noexcept = default;
};

// Padding added around a grid; all four amounts must be non-negative.
struct GridPads final {
    std::int64_t left = 0;
    std::int64_t right = 0;
    std::int64_t top = 0;
    std::int64_t bottom = 0;

    [[nodiscard]] constexpr bool operator==(const GridPads&) const noexcept = default;
};

// A flow grid plus the validity of each of its samples. Cells whose endpoint left the frame
// still carry their computed vectors; the validity flag is what marks them unusable.
struct FlowTransformResult final {
    FlowGrid flow;
    ValidMask valid;

    [[nodiscard]] bool operator==(const FlowTransformResult&) const noexcept = default;
};

// A padded mask plus the validity created by padding: interior cells keep their supervision,
// padded cells carry the fill value with valid = 0 because padding cannot manufacture truth.
struct MaskPadResult final {
    MaskGrid mask;
    ValidMask valid;

    [[nodiscard]] bool operator==(const MaskPadResult&) const noexcept = default;
};

// Crops a flow grid by grid position: the sample at output (x, y) is the input sample at
// (rect.x + x, rect.y + y); vectors are unchanged because the same-region crop translates the
// source frame and the target frame by the same origin. A sample stays valid only when its
// input validity says so AND its endpoint (sample position + vector, in input coordinates) lies
// inside the crop rectangle, half-open: an endpoint exactly on the rectangle boundary counts as
// outside. inputValid may be null, meaning every input sample is valid.
[[nodiscard]] Result<FlowTransformResult>
cropFlowGrid(const FlowGrid& flow, const ValidMask* inputValid, GridRect rect);

// Mirrors a flow grid horizontally: columns reverse, dx negates (the motion now points the
// other way in x), dy is unchanged, and any input validity is mirrored with its sample.
// Flip is an exact involution, an isometry on pixel-centre coordinates: it maps the endpoint of
// every sample onto the mirror image of the original endpoint, so the endpoint multiset is
// mirrored as a whole and flip neither creates nor destroys endpoint information. It therefore
// does not re-check endpoints the way cropFlowGrid and transformFlowEndpoints do; re-checking
// against the half-open frame would discard samples only because the boundary convention is
// not mirror-symmetric, not because supervision was lost. inputValid may be null (all valid).
[[nodiscard]] Result<FlowTransformResult> flipFlowGridHorizontal(const FlowGrid& flow,
                                                                 const ValidMask* inputValid);

// General endpoint rule: for a flow whose sample x points at x + f(x), with the source frame
// transformed by sourceTransform (T0) and the target frame by targetTransform (T1), the output
// sample at T0(x) carries the vector T1(x + f(x)) - T0(x). The output grid is
// targetWidth x targetHeight; a source sample lands only when T0(x) falls exactly on an output
// pixel centre (integer position in pixel-centre coordinates) — samples landing between centres
// or outside the output grid are dropped silently, because this module does not resample. When
// two samples land on the same output cell (a collapsing transform) the request is rejected:
// choosing a survivor would be a resampling decision. A landed sample is valid only when its
// input validity says so AND its transformed endpoint lies inside the output grid, half-open,
// boundary counting as outside. Cells no sample covers keep zero vectors and valid = 0.
// inputValid may be null (all valid).
[[nodiscard]] Result<FlowTransformResult> transformFlowEndpoints(const FlowGrid& flow,
                                                                 const ValidMask* inputValid,
                                                                 const Affine2& sourceTransform,
                                                                 const Affine2& targetTransform,
                                                                 std::int64_t targetWidth,
                                                                 std::int64_t targetHeight);

// Shared-affine shortcut: when T0 == T1 == T(x) = x * A + t, the endpoint rule collapses to the
// vector rule f' = f * A (linear part only; the translation cancels). Placement, dropping,
// collision rejection and validity follow transformFlowEndpoints. For the analytic transforms
// covered by the tests (integer translations, 90-degree rotations, power-of-two scales) both
// formulas are exact in double arithmetic and agree bit for bit; for general inputs the two
// formulas may differ in the last ulp, and transformFlowEndpoints remains the contract.
// inputValid may be null (all valid).
[[nodiscard]] Result<FlowTransformResult>
transformFlowVectorsForCommonAffine(const FlowGrid& flow,
                                    const ValidMask* inputValid,
                                    const Affine2& commonTransform,
                                    std::int64_t targetWidth,
                                    std::int64_t targetHeight);

// Pads a flow grid. Copied samples keep their vectors and their input validity — never
// upgraded: an endpoint outside the original frame stays invalid even when the padded canvas
// would now contain it, because the label truth did not change. Padded cells carry zero vectors
// and valid = 0, because padding cannot manufacture flow supervision. inputValid may be null
// (all valid).
[[nodiscard]] Result<FlowTransformResult>
padFlowGrid(const FlowGrid& flow, const ValidMask* inputValid, GridPads pads);

// Mirrors a mask horizontally: an exact column reversal with no interpolation, identical for
// class masks and continuous alpha/probability masks because no value arithmetic happens.
[[nodiscard]] Result<MaskGrid> flipMaskHorizontal(const MaskGrid& mask);

// Crops a mask by grid position: the sample at output (x, y) is the input sample at
// (rect.x + x, rect.y + y). Nearest-neighbour equality — an exact index rearrangement, so class
// labels are never blended and continuous values are never filtered.
[[nodiscard]] Result<MaskGrid> cropMask(const MaskGrid& mask, GridRect rect);

// Pads a mask: interior cells keep their values and are valid; padded cells carry fillValue
// with valid = 0, because padding cannot manufacture label truth.
[[nodiscard]] Result<MaskPadResult> padMask(const MaskGrid& mask, GridPads pads, double fillValue);

} // namespace dvs::domain
