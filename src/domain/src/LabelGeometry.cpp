#include "dvs/domain/LabelGeometry.h"

#include "dvs/domain/MediaError.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dvs::domain {
namespace {

// Upper bound for any grid side this module accepts or produces. It keeps every index product
// inside int64 and rejects absurd label sizes before any allocation is attempted.
constexpr std::int64_t kMaxGridSide = 1'000'000;

// MediaError.h is frozen for this ticket, so no dedicated label-geometry operation exists; the
// interpolation-protocol operation is the nearest member of the same sample-production pipeline.
// Every failure below carries kInvalidArgument with an explicit technicalDetail.
MediaError invalidArgument(std::string technicalDetail) {
    return makeMediaError(MediaErrorCode::kInvalidArgument,
                          MediaOperation::kInterpolationProtocol,
                          std::nullopt,
                          false,
                          std::move(technicalDetail));
}

struct Point2 final {
    double x = 0.0;
    double y = 0.0;
};

[[nodiscard]] Point2
applyAffine(const Affine2& transform, const double x, const double y) noexcept {
    return Point2{.x = x * transform.a11 + y * transform.a21 + transform.tx,
                  .y = x * transform.a12 + y * transform.a22 + transform.ty};
}

// Half-open containment with the boundary counting as outside. Comparisons against NaN are
// false, so non-finite vectors degrade to invalid samples instead of errors.
[[nodiscard]] bool insideRect(const Point2& point, const GridRect& rect) noexcept {
    return point.x >= static_cast<double>(rect.x) &&
           point.x < static_cast<double>(rect.x + rect.width) &&
           point.y >= static_cast<double>(rect.y) &&
           point.y < static_cast<double>(rect.y + rect.height);
}

[[nodiscard]] bool
insideGrid(const Point2& point, const std::int64_t width, const std::int64_t height) noexcept {
    return insideRect(point, GridRect{.x = 0, .y = 0, .width = width, .height = height});
}

Status
validateGridDimensions(const std::int64_t width, const std::int64_t height, const char* role) {
    if (width <= 0 || height <= 0) {
        return Status::failure(invalidArgument(std::string{role} + " needs positive dimensions"));
    }
    if (width > kMaxGridSide || height > kMaxGridSide) {
        return Status::failure(
            invalidArgument(std::string{role} + " exceeds the maximum grid side"));
    }
    return Status::success();
}

Status validateSampleStorage(const std::vector<double>& samples,
                             const std::int64_t width,
                             const std::int64_t height,
                             const char* role) {
    const auto expected = static_cast<std::int64_t>(samples.size());
    if (expected != width * height) {
        return Status::failure(
            invalidArgument(std::string{role} + " storage does not match the grid size"));
    }
    return Status::success();
}

Status validateFlowGrid(const FlowGrid& flow) {
    if (const auto check = validateGridDimensions(flow.width, flow.height, "flow grid");
        !check.hasValue()) {
        return check;
    }
    if (const auto check = validateSampleStorage(flow.dx, flow.width, flow.height, "flow dx");
        !check.hasValue()) {
        return check;
    }
    return validateSampleStorage(flow.dy, flow.width, flow.height, "flow dy");
}

Status validateMaskGrid(const MaskGrid& mask) {
    if (const auto check = validateGridDimensions(mask.width, mask.height, "mask grid");
        !check.hasValue()) {
        return check;
    }
    return validateSampleStorage(mask.values, mask.width, mask.height, "mask values");
}

Status validateValidMask(const ValidMask& mask, const char* role) {
    if (const auto check = validateGridDimensions(mask.width, mask.height, role);
        !check.hasValue()) {
        return check;
    }
    const auto expected = static_cast<std::int64_t>(mask.valid.size());
    if (expected != mask.width * mask.height) {
        return Status::failure(
            invalidArgument(std::string{role} + " storage does not match the grid size"));
    }
    for (const std::uint8_t entry : mask.valid) {
        if (entry > 1) {
            return Status::failure(invalidArgument(std::string{role} + " entries must be 0 or 1"));
        }
    }
    return Status::success();
}

// inputValid may be null, which means every input sample is valid.
Status validateOptionalValidMask(const ValidMask* inputValid,
                                 const std::int64_t width,
                                 const std::int64_t height) {
    if (inputValid == nullptr) {
        return Status::success();
    }
    if (inputValid->width != width || inputValid->height != height) {
        return Status::failure(
            invalidArgument("validity mask dimensions do not match the label grid"));
    }
    return validateValidMask(*inputValid, "validity mask");
}

Status validateCropRect(const GridRect& rect,
                        const std::int64_t gridWidth,
                        const std::int64_t gridHeight) {
    if (rect.width <= 0 || rect.height <= 0) {
        return Status::failure(invalidArgument("crop rectangle needs positive dimensions"));
    }
    if (rect.x < 0 || rect.y < 0) {
        return Status::failure(invalidArgument("crop rectangle must start inside the grid"));
    }
    // Subtraction form keeps the arithmetic overflow-free: gridWidth/gridHeight are bounded by
    // kMaxGridSide and rect.width/rect.height are positive at this point.
    if (rect.x > gridWidth - rect.width || rect.y > gridHeight - rect.height) {
        return Status::failure(invalidArgument("crop rectangle must stay inside the grid"));
    }
    return Status::success();
}

Status validatePads(const GridPads& pads, const std::int64_t width, const std::int64_t height) {
    if (pads.left < 0 || pads.right < 0 || pads.top < 0 || pads.bottom < 0) {
        return Status::failure(invalidArgument("padding amounts must be non-negative"));
    }
    if (pads.left > kMaxGridSide || pads.right > kMaxGridSide || pads.top > kMaxGridSide ||
        pads.bottom > kMaxGridSide) {
        return Status::failure(invalidArgument("padding exceeds the maximum grid side"));
    }
    if (width + pads.left + pads.right > kMaxGridSide ||
        height + pads.top + pads.bottom > kMaxGridSide) {
        return Status::failure(invalidArgument("padded grid exceeds the maximum grid side"));
    }
    return Status::success();
}

[[nodiscard]] bool markedValidAt(const ValidMask* inputValid,
                                 const std::size_t sampleIndex) noexcept {
    return inputValid == nullptr || inputValid->valid[sampleIndex] != 0;
}

} // namespace

Result<FlowTransformResult>
cropFlowGrid(const FlowGrid& flow, const ValidMask* inputValid, const GridRect rect) {
    if (const auto check = validateFlowGrid(flow); !check.hasValue()) {
        return Result<FlowTransformResult>::failure(check.error());
    }
    if (const auto check = validateOptionalValidMask(inputValid, flow.width, flow.height);
        !check.hasValue()) {
        return Result<FlowTransformResult>::failure(check.error());
    }
    if (const auto check = validateCropRect(rect, flow.width, flow.height); !check.hasValue()) {
        return Result<FlowTransformResult>::failure(check.error());
    }

    FlowGrid cropped;
    cropped.width = rect.width;
    cropped.height = rect.height;
    cropped.direction = flow.direction;
    const auto cropSampleCount = static_cast<std::size_t>(rect.width * rect.height);
    cropped.dx.resize(cropSampleCount);
    cropped.dy.resize(cropSampleCount);

    ValidMask validity;
    validity.width = rect.width;
    validity.height = rect.height;
    validity.valid.assign(cropSampleCount, 1);

    for (std::int64_t row = 0; row < rect.height; ++row) {
        for (std::int64_t column = 0; column < rect.width; ++column) {
            const std::int64_t sourceColumn = rect.x + column;
            const std::int64_t sourceRow = rect.y + row;
            const auto sourceIndex =
                static_cast<std::size_t>(sourceRow * flow.width + sourceColumn);
            const auto targetIndex = static_cast<std::size_t>(row * rect.width + column);
            cropped.dx[targetIndex] = flow.dx[sourceIndex];
            cropped.dy[targetIndex] = flow.dy[sourceIndex];
            // The endpoint is checked in input coordinates against the crop rectangle; the
            // boundary itself counts as outside.
            const Point2 endpoint{static_cast<double>(sourceColumn) + flow.dx[sourceIndex],
                                  static_cast<double>(sourceRow) + flow.dy[sourceIndex]};
            validity.valid[targetIndex] =
                markedValidAt(inputValid, sourceIndex) && insideRect(endpoint, rect) ? 1 : 0;
        }
    }
    return Result<FlowTransformResult>::success(
        FlowTransformResult{.flow = std::move(cropped), .valid = std::move(validity)});
}

Result<FlowTransformResult> flipFlowGridHorizontal(const FlowGrid& flow,
                                                   const ValidMask* inputValid) {
    if (const auto check = validateFlowGrid(flow); !check.hasValue()) {
        return Result<FlowTransformResult>::failure(check.error());
    }
    if (const auto check = validateOptionalValidMask(inputValid, flow.width, flow.height);
        !check.hasValue()) {
        return Result<FlowTransformResult>::failure(check.error());
    }

    FlowGrid flipped;
    flipped.width = flow.width;
    flipped.height = flow.height;
    flipped.direction = flow.direction;
    const auto flipSampleCount = static_cast<std::size_t>(flow.width * flow.height);
    flipped.dx.resize(flipSampleCount);
    flipped.dy.resize(flipSampleCount);

    ValidMask validity;
    validity.width = flow.width;
    validity.height = flow.height;
    validity.valid.assign(flipSampleCount, 1);

    for (std::int64_t row = 0; row < flow.height; ++row) {
        for (std::int64_t column = 0; column < flow.width; ++column) {
            const auto sourceIndex = static_cast<std::size_t>(row * flow.width + column);
            const auto targetIndex =
                static_cast<std::size_t>(row * flow.width + (flow.width - 1 - column));
            flipped.dx[targetIndex] = -flow.dx[sourceIndex];
            flipped.dy[targetIndex] = flow.dy[sourceIndex];
            // Flip mirrors endpoints instead of re-checking them; see the header contract.
            validity.valid[targetIndex] = markedValidAt(inputValid, sourceIndex) ? 1 : 0;
        }
    }
    return Result<FlowTransformResult>::success(
        FlowTransformResult{.flow = std::move(flipped), .valid = std::move(validity)});
}

Result<FlowTransformResult> transformFlowEndpoints(const FlowGrid& flow,
                                                   const ValidMask* inputValid,
                                                   const Affine2& sourceTransform,
                                                   const Affine2& targetTransform,
                                                   const std::int64_t targetWidth,
                                                   const std::int64_t targetHeight) {
    if (const auto check = validateFlowGrid(flow); !check.hasValue()) {
        return Result<FlowTransformResult>::failure(check.error());
    }
    if (const auto check = validateOptionalValidMask(inputValid, flow.width, flow.height);
        !check.hasValue()) {
        return Result<FlowTransformResult>::failure(check.error());
    }
    if (const auto check = validateGridDimensions(targetWidth, targetHeight, "target grid");
        !check.hasValue()) {
        return Result<FlowTransformResult>::failure(check.error());
    }

    FlowGrid transformed;
    transformed.width = targetWidth;
    transformed.height = targetHeight;
    transformed.direction = flow.direction;
    const auto transformSampleCount = static_cast<std::size_t>(targetWidth * targetHeight);
    transformed.dx.assign(transformSampleCount, 0.0);
    transformed.dy.assign(transformSampleCount, 0.0);

    ValidMask validity;
    validity.width = targetWidth;
    validity.height = targetHeight;
    validity.valid.assign(transformSampleCount, 0);

    std::vector<std::uint8_t> claimed(transformSampleCount, 0);
    for (std::int64_t row = 0; row < flow.height; ++row) {
        for (std::int64_t column = 0; column < flow.width; ++column) {
            const auto sourceIndex = static_cast<std::size_t>(row * flow.width + column);
            const Point2 origin =
                applyAffine(sourceTransform, static_cast<double>(column), static_cast<double>(row));
            if (!insideGrid(origin, targetWidth, targetHeight)) {
                // The transformed sample leaves the output grid: dropped, not an error.
                continue;
            }
            if (origin.x != std::floor(origin.x) || origin.y != std::floor(origin.y)) {
                // The sample lands between pixel centres; this module does not resample.
                continue;
            }
            const auto targetColumn = static_cast<std::int64_t>(origin.x);
            const auto targetRow = static_cast<std::int64_t>(origin.y);
            const auto targetIndex =
                static_cast<std::size_t>(targetRow * targetWidth + targetColumn);
            if (claimed[targetIndex] != 0) {
                return Result<FlowTransformResult>::failure(invalidArgument(
                    "source transform maps several samples onto one output cell; refusing to "
                    "resample"));
            }
            claimed[targetIndex] = 1;
            const Point2 endpoint = applyAffine(targetTransform,
                                                static_cast<double>(column) + flow.dx[sourceIndex],
                                                static_cast<double>(row) + flow.dy[sourceIndex]);
            transformed.dx[targetIndex] = endpoint.x - origin.x;
            transformed.dy[targetIndex] = endpoint.y - origin.y;
            validity.valid[targetIndex] = markedValidAt(inputValid, sourceIndex) &&
                                                  insideGrid(endpoint, targetWidth, targetHeight)
                                              ? 1
                                              : 0;
        }
    }
    return Result<FlowTransformResult>::success(
        FlowTransformResult{.flow = std::move(transformed), .valid = std::move(validity)});
}

Result<FlowTransformResult> transformFlowVectorsForCommonAffine(const FlowGrid& flow,
                                                                const ValidMask* inputValid,
                                                                const Affine2& commonTransform,
                                                                const std::int64_t targetWidth,
                                                                const std::int64_t targetHeight) {
    if (const auto check = validateFlowGrid(flow); !check.hasValue()) {
        return Result<FlowTransformResult>::failure(check.error());
    }
    if (const auto check = validateOptionalValidMask(inputValid, flow.width, flow.height);
        !check.hasValue()) {
        return Result<FlowTransformResult>::failure(check.error());
    }
    if (const auto check = validateGridDimensions(targetWidth, targetHeight, "target grid");
        !check.hasValue()) {
        return Result<FlowTransformResult>::failure(check.error());
    }

    FlowGrid transformed;
    transformed.width = targetWidth;
    transformed.height = targetHeight;
    transformed.direction = flow.direction;
    const auto shortcutSampleCount = static_cast<std::size_t>(targetWidth * targetHeight);
    transformed.dx.assign(shortcutSampleCount, 0.0);
    transformed.dy.assign(shortcutSampleCount, 0.0);

    ValidMask validity;
    validity.width = targetWidth;
    validity.height = targetHeight;
    validity.valid.assign(shortcutSampleCount, 0);

    std::vector<std::uint8_t> claimed(shortcutSampleCount, 0);
    for (std::int64_t row = 0; row < flow.height; ++row) {
        for (std::int64_t column = 0; column < flow.width; ++column) {
            const auto sourceIndex = static_cast<std::size_t>(row * flow.width + column);
            const Point2 origin =
                applyAffine(commonTransform, static_cast<double>(column), static_cast<double>(row));
            if (!insideGrid(origin, targetWidth, targetHeight)) {
                continue;
            }
            if (origin.x != std::floor(origin.x) || origin.y != std::floor(origin.y)) {
                continue;
            }
            const auto targetColumn = static_cast<std::int64_t>(origin.x);
            const auto targetRow = static_cast<std::int64_t>(origin.y);
            const auto targetIndex =
                static_cast<std::size_t>(targetRow * targetWidth + targetColumn);
            if (claimed[targetIndex] != 0) {
                return Result<FlowTransformResult>::failure(invalidArgument(
                    "source transform maps several samples onto one output cell; refusing to "
                    "resample"));
            }
            claimed[targetIndex] = 1;
            const double vectorX = flow.dx[sourceIndex] * commonTransform.a11 +
                                   flow.dy[sourceIndex] * commonTransform.a21;
            const double vectorY = flow.dx[sourceIndex] * commonTransform.a12 +
                                   flow.dy[sourceIndex] * commonTransform.a22;
            transformed.dx[targetIndex] = vectorX;
            transformed.dy[targetIndex] = vectorY;
            const Point2 endpoint{origin.x + vectorX, origin.y + vectorY};
            validity.valid[targetIndex] = markedValidAt(inputValid, sourceIndex) &&
                                                  insideGrid(endpoint, targetWidth, targetHeight)
                                              ? 1
                                              : 0;
        }
    }
    return Result<FlowTransformResult>::success(
        FlowTransformResult{.flow = std::move(transformed), .valid = std::move(validity)});
}

Result<FlowTransformResult>
padFlowGrid(const FlowGrid& flow, const ValidMask* inputValid, const GridPads pads) {
    if (const auto check = validateFlowGrid(flow); !check.hasValue()) {
        return Result<FlowTransformResult>::failure(check.error());
    }
    if (const auto check = validateOptionalValidMask(inputValid, flow.width, flow.height);
        !check.hasValue()) {
        return Result<FlowTransformResult>::failure(check.error());
    }
    if (const auto check = validatePads(pads, flow.width, flow.height); !check.hasValue()) {
        return Result<FlowTransformResult>::failure(check.error());
    }

    const std::int64_t paddedWidth = flow.width + pads.left + pads.right;
    const std::int64_t paddedHeight = flow.height + pads.top + pads.bottom;

    FlowGrid padded;
    padded.width = paddedWidth;
    padded.height = paddedHeight;
    padded.direction = flow.direction;
    const auto paddedSampleCount = static_cast<std::size_t>(paddedWidth * paddedHeight);
    padded.dx.assign(paddedSampleCount, 0.0);
    padded.dy.assign(paddedSampleCount, 0.0);

    ValidMask paddedValidity;
    paddedValidity.width = paddedWidth;
    paddedValidity.height = paddedHeight;
    paddedValidity.valid.assign(paddedSampleCount, 0);

    for (std::int64_t row = 0; row < flow.height; ++row) {
        for (std::int64_t column = 0; column < flow.width; ++column) {
            const auto sourceIndex = static_cast<std::size_t>(row * flow.width + column);
            const auto targetIndex =
                static_cast<std::size_t>((row + pads.top) * paddedWidth + (column + pads.left));
            padded.dx[targetIndex] = flow.dx[sourceIndex];
            padded.dy[targetIndex] = flow.dy[sourceIndex];
            // Input validity is mirrored as-is, never upgraded by the larger canvas.
            paddedValidity.valid[targetIndex] = markedValidAt(inputValid, sourceIndex) ? 1 : 0;
        }
    }
    return Result<FlowTransformResult>::success(
        FlowTransformResult{.flow = std::move(padded), .valid = std::move(paddedValidity)});
}

Result<MaskGrid> flipMaskHorizontal(const MaskGrid& mask) {
    if (const auto check = validateMaskGrid(mask); !check.hasValue()) {
        return Result<MaskGrid>::failure(check.error());
    }

    MaskGrid flipped;
    flipped.width = mask.width;
    flipped.height = mask.height;
    flipped.values.resize(static_cast<std::size_t>(mask.width * mask.height));
    for (std::int64_t row = 0; row < mask.height; ++row) {
        for (std::int64_t column = 0; column < mask.width; ++column) {
            const auto sourceIndex = static_cast<std::size_t>(row * mask.width + column);
            const auto targetIndex =
                static_cast<std::size_t>(row * mask.width + (mask.width - 1 - column));
            flipped.values[targetIndex] = mask.values[sourceIndex];
        }
    }
    return Result<MaskGrid>::success(std::move(flipped));
}

Result<MaskGrid> cropMask(const MaskGrid& mask, const GridRect rect) {
    if (const auto check = validateMaskGrid(mask); !check.hasValue()) {
        return Result<MaskGrid>::failure(check.error());
    }
    if (const auto check = validateCropRect(rect, mask.width, mask.height); !check.hasValue()) {
        return Result<MaskGrid>::failure(check.error());
    }

    MaskGrid cropped;
    cropped.width = rect.width;
    cropped.height = rect.height;
    cropped.values.resize(static_cast<std::size_t>(rect.width * rect.height));
    for (std::int64_t row = 0; row < rect.height; ++row) {
        for (std::int64_t column = 0; column < rect.width; ++column) {
            const auto sourceIndex =
                static_cast<std::size_t>((rect.y + row) * mask.width + (rect.x + column));
            const auto targetIndex = static_cast<std::size_t>(row * rect.width + column);
            cropped.values[targetIndex] = mask.values[sourceIndex];
        }
    }
    return Result<MaskGrid>::success(std::move(cropped));
}

Result<MaskPadResult> padMask(const MaskGrid& mask, const GridPads pads, const double fillValue) {
    if (const auto check = validateMaskGrid(mask); !check.hasValue()) {
        return Result<MaskPadResult>::failure(check.error());
    }
    if (const auto check = validatePads(pads, mask.width, mask.height); !check.hasValue()) {
        return Result<MaskPadResult>::failure(check.error());
    }

    const std::int64_t paddedWidth = mask.width + pads.left + pads.right;
    const std::int64_t paddedHeight = mask.height + pads.top + pads.bottom;
    const auto maskSampleCount = static_cast<std::size_t>(paddedWidth * paddedHeight);

    MaskGrid padded;
    padded.width = paddedWidth;
    padded.height = paddedHeight;
    padded.values.assign(maskSampleCount, fillValue);

    ValidMask validity;
    validity.width = paddedWidth;
    validity.height = paddedHeight;
    validity.valid.assign(maskSampleCount, 0);

    for (std::int64_t row = 0; row < mask.height; ++row) {
        for (std::int64_t column = 0; column < mask.width; ++column) {
            const auto sourceIndex = static_cast<std::size_t>(row * mask.width + column);
            const auto targetIndex =
                static_cast<std::size_t>((row + pads.top) * paddedWidth + (column + pads.left));
            padded.values[targetIndex] = mask.values[sourceIndex];
            validity.valid[targetIndex] = 1;
        }
    }
    return Result<MaskPadResult>::success(
        MaskPadResult{.mask = std::move(padded), .valid = std::move(validity)});
}

} // namespace dvs::domain
