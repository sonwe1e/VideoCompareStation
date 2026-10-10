#include "dvs/domain/LabelGeometry.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <utility>
#include <vector>

namespace dvs::domain {
namespace {

FlowGrid flowGrid(const std::int64_t width,
                  const std::int64_t height,
                  std::vector<double> dx,
                  std::vector<double> dy,
                  const FlowDirection direction = FlowDirection::kForward) {
    return FlowGrid{.width = width,
                    .height = height,
                    .dx = std::move(dx),
                    .dy = std::move(dy),
                    .direction = direction};
}

void expectInvalidArgument(const MediaError& error) {
    EXPECT_EQ(error.code, MediaErrorCode::kInvalidArgument);
    EXPECT_FALSE(error.technicalDetail.empty());
}

TEST(LabelGeometryTests, TranslatesAnalyticFlowThroughTheGeneralEndpointRule) {
    // 2x2 grid, every sample carries f = (3, -1).
    const FlowGrid flow = flowGrid(2, 2, {3, 3, 3, 3}, {-1, -1, -1, -1});

    // Shared translation T0 = T1 = translate(2, 1): a common translation leaves vectors
    // unchanged, T(x + f) - T(x) = f, and the samples land on columns 2..3, rows 1..2.
    const Affine2 sharedTranslation{
        .a11 = 1.0, .a12 = 0.0, .a21 = 0.0, .a22 = 1.0, .tx = 2.0, .ty = 1.0};
    const auto shared =
        transformFlowEndpoints(flow, nullptr, sharedTranslation, sharedTranslation, 8, 4);
    ASSERT_TRUE(shared);
    EXPECT_EQ(shared.value().flow.width, 8);
    EXPECT_EQ(shared.value().flow.height, 4);
    EXPECT_EQ(shared.value().flow.direction, FlowDirection::kForward);
    for (std::int64_t row = 1; row <= 2; ++row) {
        for (std::int64_t column = 2; column <= 3; ++column) {
            const auto cell = static_cast<std::size_t>(row * 8 + column);
            EXPECT_DOUBLE_EQ(shared.value().flow.dx[cell], 3.0);
            EXPECT_DOUBLE_EQ(shared.value().flow.dy[cell], -1.0);
            EXPECT_EQ(shared.value().valid.valid[cell], 1);
        }
    }
    // Cells no source sample covers keep zero vectors and valid = 0.
    EXPECT_DOUBLE_EQ(shared.value().flow.dx[0], 0.0);
    EXPECT_DOUBLE_EQ(shared.value().flow.dy[0], 0.0);
    EXPECT_EQ(shared.value().valid.valid[0], 0);

    // Diverging translations T0 = identity, T1 = translate(5, 0) with f = (1, 0): the general
    // rule must give f' = f + (5, 0) = (6, 0). The vector shortcut does not apply here.
    const Affine2 identity{};
    const Affine2 targetShift{.a11 = 1.0, .a12 = 0.0, .a21 = 0.0, .a22 = 1.0, .tx = 5.0, .ty = 0.0};
    const FlowGrid gentle = flowGrid(2, 2, {1, 1, 1, 1}, {0, 0, 0, 0});
    const auto diverged = transformFlowEndpoints(gentle, nullptr, identity, targetShift, 8, 2);
    ASSERT_TRUE(diverged);
    // The identity source transform keeps the samples at columns 0..1 of rows 0..1, i.e. row
    // major indices 0, 1, 8 and 9 of the 8-wide target.
    for (const std::size_t cell :
         {std::size_t{0}, std::size_t{1}, std::size_t{8}, std::size_t{9}}) {
        EXPECT_DOUBLE_EQ(diverged.value().flow.dx[cell], 6.0);
        EXPECT_DOUBLE_EQ(diverged.value().flow.dy[cell], 0.0);
        EXPECT_EQ(diverged.value().valid.valid[cell], 1);
    }
    EXPECT_EQ(diverged.value().valid.valid[4], 0);
}

TEST(LabelGeometryTests, RotatesAnalyticFlowThroughVectorAndEndpointRules) {
    // 90-degree rotation A = (0 1; -1 0) combined with translate(2, 0): pixel (x, y) maps to
    // (2 - y, x). Every sample carries f = (0, 1); the vector rule gives f' = f * A = (-1, 0).
    const FlowGrid flow = flowGrid(2, 2, {0, 0, 0, 0}, {1, 1, 1, 1});
    const Affine2 rotation{.a11 = 0.0, .a12 = 1.0, .a21 = -1.0, .a22 = 0.0, .tx = 2.0, .ty = 0.0};

    const auto byEndpoints = transformFlowEndpoints(flow, nullptr, rotation, rotation, 3, 2);
    ASSERT_TRUE(byEndpoints);
    // Covered target cells: (0,0)->(2,0), (1,0)->(2,1), (0,1)->(1,0), (1,1)->(1,1), i.e. row
    // major indices 2, 5, 1, 4 in the 3x2 target.
    for (const std::size_t cell :
         {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{5}}) {
        EXPECT_DOUBLE_EQ(byEndpoints.value().flow.dx[cell], -1.0);
        EXPECT_DOUBLE_EQ(byEndpoints.value().flow.dy[cell], 0.0);
        EXPECT_EQ(byEndpoints.value().valid.valid[cell], 1);
    }
    // Target (0, 0) stays uncovered and target (0, 1) mirrors it.
    EXPECT_DOUBLE_EQ(byEndpoints.value().flow.dx[0], 0.0);
    EXPECT_EQ(byEndpoints.value().valid.valid[0], 0);
    EXPECT_EQ(byEndpoints.value().valid.valid[3], 0);

    // The shared-affine vector shortcut must agree bit for bit on this analytic transform.
    const auto byVectors = transformFlowVectorsForCommonAffine(flow, nullptr, rotation, 3, 2);
    ASSERT_TRUE(byVectors);
    EXPECT_EQ(byEndpoints.value(), byVectors.value());
}

TEST(LabelGeometryTests, ScalesAnalyticFlowThroughVectorAndEndpointRules) {
    // Shared 2x scale: vectors double under the linear part, f = (0.5, 0) becomes (1, 0), and
    // the 2x2 source covers the four even target cells of a 4x4 grid.
    const FlowGrid flow = flowGrid(2, 2, {0.5, 0.5, 0.5, 0.5}, {0, 0, 0, 0});
    const Affine2 scale{.a11 = 2.0, .a12 = 0.0, .a21 = 0.0, .a22 = 2.0, .tx = 0.0, .ty = 0.0};

    const auto byEndpoints = transformFlowEndpoints(flow, nullptr, scale, scale, 4, 4);
    ASSERT_TRUE(byEndpoints);
    for (const std::size_t cell :
         {std::size_t{0}, std::size_t{2}, std::size_t{8}, std::size_t{10}}) {
        EXPECT_DOUBLE_EQ(byEndpoints.value().flow.dx[cell], 1.0);
        EXPECT_DOUBLE_EQ(byEndpoints.value().flow.dy[cell], 0.0);
        EXPECT_EQ(byEndpoints.value().valid.valid[cell], 1);
    }
    EXPECT_EQ(byEndpoints.value().valid.valid[5], 0);

    const auto byVectors = transformFlowVectorsForCommonAffine(flow, nullptr, scale, 4, 4);
    ASSERT_TRUE(byVectors);
    EXPECT_EQ(byEndpoints.value(), byVectors.value());

    // Non-common scaling T0 = 2x, T1(x) = 2x + (1, 0): only the general rule applies and gives
    // f' = 2*f + (1, 0) = (2, 0); endpoints land on x = 2 and 4 of a 5-wide target.
    const Affine2 shiftedScale{
        .a11 = 2.0, .a12 = 0.0, .a21 = 0.0, .a22 = 2.0, .tx = 1.0, .ty = 0.0};
    const auto diverged = transformFlowEndpoints(flow, nullptr, scale, shiftedScale, 5, 4);
    ASSERT_TRUE(diverged);
    // Covered cells of the 5-wide target: (0,0), (2,0), (0,2), (2,2) -> indices 0, 2, 10, 12.
    for (const std::size_t cell :
         {std::size_t{0}, std::size_t{2}, std::size_t{10}, std::size_t{12}}) {
        EXPECT_DOUBLE_EQ(diverged.value().flow.dx[cell], 2.0);
        EXPECT_DOUBLE_EQ(diverged.value().flow.dy[cell], 0.0);
        EXPECT_EQ(diverged.value().valid.valid[cell], 1);
    }
}

TEST(LabelGeometryTests, CropsAsymmetricRectanglesByGridPosition) {
    // 5x3 grid with distinct values dx = 0.01 * i, dy = 0.02 * i (i = row * 5 + column); the
    // crop (1, 1, 3, 2) is asymmetric: one column and one row in, two columns to the right edge.
    std::vector<double> dx(15);
    std::vector<double> dy(15);
    for (std::int64_t row = 0; row < 3; ++row) {
        for (std::int64_t column = 0; column < 5; ++column) {
            const auto i = static_cast<std::size_t>(row * 5 + column);
            dx[i] = 0.01 * static_cast<double>(i);
            dy[i] = 0.02 * static_cast<double>(i);
        }
    }
    const FlowGrid flow = flowGrid(5, 3, std::move(dx), std::move(dy), FlowDirection::kBackward);

    const auto cropped =
        cropFlowGrid(flow, nullptr, GridRect{.x = 1, .y = 1, .width = 3, .height = 2});
    ASSERT_TRUE(cropped);
    EXPECT_EQ(cropped.value().flow.width, 3);
    EXPECT_EQ(cropped.value().flow.height, 2);
    EXPECT_EQ(cropped.value().flow.direction, FlowDirection::kBackward);
    EXPECT_EQ(cropped.value().valid.width, 3);
    EXPECT_EQ(cropped.value().valid.height, 2);

    // out(x, y) = in(x + 1, y + 1); every endpoint stays inside the crop rectangle.
    const std::vector<double> expectedDx{0.06, 0.07, 0.08, 0.11, 0.12, 0.13};
    const std::vector<double> expectedDy{0.12, 0.14, 0.16, 0.22, 0.24, 0.26};
    EXPECT_EQ(cropped.value().flow.dx, expectedDx);
    EXPECT_EQ(cropped.value().flow.dy, expectedDy);
    EXPECT_EQ(cropped.value().valid.valid, std::vector<std::uint8_t>(6, 1));
}

TEST(LabelGeometryTests, CropMarksOutboundEndpointsInvalid) {
    // Same 5x3 / (1, 1, 3, 2) crop; vectors crafted so endpoints sit inside, outside, and exactly
    // on each boundary of the crop rectangle.
    std::vector<double> dx(15, 0.0);
    std::vector<double> dy(15, 0.0);
    const auto set =
        [&dx,
         &dy](const std::int64_t column, const std::int64_t row, const double vx, const double vy) {
            const auto i = static_cast<std::size_t>(row * 5 + column);
            dx[i] = vx;
            dy[i] = vy;
        };
    set(2, 1, 0.5, 0.0);   // endpoint (2.5, 1): inside
    set(3, 1, 0.5, 0.0);   // endpoint (3.5, 1): inside
    set(3, 2, 1.0, 0.0);   // endpoint (4.0, 2): x exactly on the right boundary -> outside
    set(1, 2, -0.25, 0.0); // endpoint (0.75, 2): left of the rectangle -> outside
    set(2, 2, 0.0, 1.0);   // endpoint (2.0, 3.0): y exactly on the bottom boundary -> outside
    set(1, 1, 0.0, -0.5);  // endpoint (1.0, 0.5): above the rectangle -> outside
    const FlowGrid flow = flowGrid(5, 3, std::move(dx), std::move(dy));

    const auto cropped =
        cropFlowGrid(flow, nullptr, GridRect{.x = 1, .y = 1, .width = 3, .height = 2});
    ASSERT_TRUE(cropped);
    EXPECT_EQ(cropped.value().valid.valid, std::vector<std::uint8_t>({0, 1, 1, 0, 0, 0}));
    // Invalid samples keep their copied vectors; validity is the flag that marks them unusable.
    EXPECT_DOUBLE_EQ(cropped.value().flow.dx[5], 1.0);
    EXPECT_DOUBLE_EQ(cropped.value().flow.dx[3], -0.25);

    // Input validity gates even the in-bounds samples: zeroing in(2, 1) flips out(1, 0).
    const ValidMask input{
        .width = 5, .height = 3, .valid = {1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1}};
    const auto gated =
        cropFlowGrid(flow, &input, GridRect{.x = 1, .y = 1, .width = 3, .height = 2});
    ASSERT_TRUE(gated);
    EXPECT_EQ(gated.value().valid.valid, std::vector<std::uint8_t>({0, 0, 1, 0, 0, 0}));
}

TEST(LabelGeometryTests, FlipsMirrorColumnsNegateDxAndMirrorValidity) {
    const FlowGrid flow = flowGrid(3,
                                   2,
                                   {0.0, 1.0, 2.0, 2.9, 4.0, 5.0},
                                   {0.5, -1.0, 2.5, -2.0, 0.0, 1.0},
                                   FlowDirection::kBackward);
    const ValidMask input{.width = 3, .height = 2, .valid = {0, 1, 1, 1, 0, 1}};

    const auto flipped = flipFlowGridHorizontal(flow, &input);
    ASSERT_TRUE(flipped);
    EXPECT_EQ(flipped.value().flow.width, 3);
    EXPECT_EQ(flipped.value().flow.height, 2);
    EXPECT_EQ(flipped.value().flow.direction, FlowDirection::kBackward);
    EXPECT_EQ(flipped.value().flow.dx, std::vector<double>({-2.0, -1.0, 0.0, -5.0, -4.0, -2.9}));
    EXPECT_EQ(flipped.value().flow.dy, std::vector<double>({2.5, -1.0, 0.5, 1.0, 0.0, -2.0}));
    // Validity mirrors with the samples: the flipped valid set is the mirror of the input set.
    EXPECT_EQ(flipped.value().valid.valid, std::vector<std::uint8_t>({1, 1, 0, 1, 0, 1}));
    // in(0, 1) has endpoint (2.9, 1), inside [0, 3). After the flip the sample sits at (2, 1)
    // with endpoint -0.9, outside the frame; flip deliberately keeps the mirrored validity bit
    // because the mirror is an exact involution and re-checking would only re-apply the
    // boundary convention, not recover supervision.
    EXPECT_EQ(flipped.value().valid.valid[5], 1);
}

TEST(LabelGeometryTests, PadsFlowWithZeroVectorsAndInvalidBorder) {
    const FlowGrid flow = flowGrid(2, 2, {1, 1, 1, 1}, {1, 1, 1, 1});

    const auto padded =
        padFlowGrid(flow, nullptr, GridPads{.left = 1, .right = 2, .top = 0, .bottom = 1});
    ASSERT_TRUE(padded);
    EXPECT_EQ(padded.value().flow.width, 5);
    EXPECT_EQ(padded.value().flow.height, 3);
    EXPECT_EQ(padded.value().flow.direction, FlowDirection::kForward);
    // Interior keeps the vectors; every padded cell is a zero vector with valid = 0.
    const std::vector<double> expectedInterior{0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(padded.value().flow.dx, expectedInterior);
    EXPECT_EQ(padded.value().flow.dy, expectedInterior);
    EXPECT_EQ(padded.value().valid.valid,
              std::vector<std::uint8_t>({0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0}));

    // Input validity is mirrored as-is into the interior, never upgraded.
    const ValidMask input{.width = 2, .height = 2, .valid = {0, 1, 1, 1}};
    const auto gated =
        padFlowGrid(flow, &input, GridPads{.left = 1, .right = 2, .top = 0, .bottom = 1});
    ASSERT_TRUE(gated);
    EXPECT_EQ(gated.value().valid.valid,
              std::vector<std::uint8_t>({0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0}));
}

TEST(LabelGeometryTests, RearrangesMasksByExactIndexPermutation) {
    // Horizontal flip of a class mask: pure column reversal, no interpolation ever.
    const MaskGrid mask{.width = 3, .height = 2, .values = {1, 2, 3, 4, 5, 6}};
    const auto flipped = flipMaskHorizontal(mask);
    ASSERT_TRUE(flipped);
    EXPECT_EQ(flipped.value().width, 3);
    EXPECT_EQ(flipped.value().height, 2);
    EXPECT_EQ(flipped.value().values, std::vector<double>({3, 2, 1, 6, 5, 4}));

    // Asymmetric crop of a continuous mask with fractional values proves no resampling happens.
    std::vector<double> values(15);
    for (std::int64_t row = 0; row < 3; ++row) {
        for (std::int64_t column = 0; column < 5; ++column) {
            values[static_cast<std::size_t>(row * 5 + column)] =
                static_cast<double>(row * 5 + column) + 0.25;
        }
    }
    const MaskGrid continuous{.width = 5, .height = 3, .values = std::move(values)};
    const auto cropped = cropMask(continuous, GridRect{.x = 1, .y = 1, .width = 3, .height = 2});
    ASSERT_TRUE(cropped);
    EXPECT_EQ(cropped.value().width, 3);
    EXPECT_EQ(cropped.value().height, 2);
    EXPECT_EQ(cropped.value().values, std::vector<double>({6.25, 7.25, 8.25, 11.25, 12.25, 13.25}));
}

TEST(LabelGeometryTests, PadsMasksWithFillValueAndInvalidBorder) {
    const MaskGrid mask{.width = 2, .height = 2, .values = {1.5, 2.5, 3.5, 4.5}};

    const auto padded = padMask(mask, GridPads{.left = 1, .right = 1, .top = 1, .bottom = 1}, 9.0);
    ASSERT_TRUE(padded);
    EXPECT_EQ(padded.value().mask.width, 4);
    EXPECT_EQ(padded.value().mask.height, 4);
    EXPECT_EQ(padded.value().mask.values,
              std::vector<double>({9, 9, 9, 9, 9, 1.5, 2.5, 9, 9, 3.5, 4.5, 9, 9, 9, 9, 9}));
    EXPECT_EQ(padded.value().valid.valid,
              std::vector<std::uint8_t>({0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0}));
}

TEST(LabelGeometryTests, HandlesSinglePixelGridsAndTargets) {
    const FlowGrid flow = flowGrid(1, 1, {0.25}, {0.5});

    const auto cropped =
        cropFlowGrid(flow, nullptr, GridRect{.x = 0, .y = 0, .width = 1, .height = 1});
    ASSERT_TRUE(cropped);
    EXPECT_EQ(cropped.value().flow.dx, std::vector<double>({0.25}));
    EXPECT_EQ(cropped.value().flow.dy, std::vector<double>({0.5}));
    EXPECT_EQ(cropped.value().valid.valid, std::vector<std::uint8_t>({1}));

    const auto flipped = flipFlowGridHorizontal(flow, nullptr);
    ASSERT_TRUE(flipped);
    EXPECT_EQ(flipped.value().flow.dx, std::vector<double>({-0.25}));
    EXPECT_EQ(flipped.value().flow.dy, std::vector<double>({0.5}));
    EXPECT_EQ(flipped.value().valid.valid, std::vector<std::uint8_t>({1}));

    const Affine2 identity{};
    const auto transformed = transformFlowEndpoints(flow, nullptr, identity, identity, 1, 1);
    ASSERT_TRUE(transformed);
    EXPECT_EQ(transformed.value().flow.dx, std::vector<double>({0.25}));
    EXPECT_EQ(transformed.value().flow.dy, std::vector<double>({0.5}));
    EXPECT_EQ(transformed.value().valid.valid, std::vector<std::uint8_t>({1}));

    const auto shortcut = transformFlowVectorsForCommonAffine(flow, nullptr, identity, 1, 1);
    ASSERT_TRUE(shortcut);
    EXPECT_EQ(transformed.value(), shortcut.value());

    const auto padded =
        padFlowGrid(flow, nullptr, GridPads{.left = 1, .right = 1, .top = 1, .bottom = 1});
    ASSERT_TRUE(padded);
    EXPECT_EQ(padded.value().flow.width, 3);
    EXPECT_EQ(padded.value().flow.height, 3);
    EXPECT_EQ(padded.value().flow.dx, std::vector<double>({0, 0, 0, 0, 0.25, 0, 0, 0, 0}));
    EXPECT_EQ(padded.value().valid.valid, std::vector<std::uint8_t>({0, 0, 0, 0, 1, 0, 0, 0, 0}));

    const MaskGrid single{.width = 1, .height = 1, .values = {7}};
    const auto flippedMask = flipMaskHorizontal(single);
    ASSERT_TRUE(flippedMask);
    EXPECT_EQ(flippedMask.value().values, std::vector<double>({7}));
    const auto croppedMask = cropMask(single, GridRect{.x = 0, .y = 0, .width = 1, .height = 1});
    ASSERT_TRUE(croppedMask);
    EXPECT_EQ(croppedMask.value().values, std::vector<double>({7}));
    const auto paddedMask =
        padMask(single, GridPads{.left = 1, .right = 0, .top = 0, .bottom = 0}, 0.0);
    ASSERT_TRUE(paddedMask);
    EXPECT_EQ(paddedMask.value().mask.values, std::vector<double>({0, 7}));
    EXPECT_EQ(paddedMask.value().valid.valid, std::vector<std::uint8_t>({0, 1}));
}

TEST(LabelGeometryTests, RoundTripsTranslationInverseAndDoubleFlip) {
    // Translation is theoretically invertible: applying T then T^-1 recovers the original flow
    // with every sample valid (endpoints stay inside both frames by construction).
    const FlowGrid original = flowGrid(2, 2, {0.5, 0.5, 0.5, 0.5}, {0.5, 0.5, 0.5, 0.5});
    const Affine2 forward{.a11 = 1.0, .a12 = 0.0, .a21 = 0.0, .a22 = 1.0, .tx = 2.0, .ty = 1.0};
    const Affine2 backward{.a11 = 1.0, .a12 = 0.0, .a21 = 0.0, .a22 = 1.0, .tx = -2.0, .ty = -1.0};

    const auto moved = transformFlowEndpoints(original, nullptr, forward, forward, 5, 4);
    ASSERT_TRUE(moved);
    // The samples cover columns 2..3 of rows 1..2 in the 5x4 target and all stay valid.
    EXPECT_EQ(moved.value().valid.valid, std::vector<std::uint8_t>({0, 0, 0, 0, 0, 0, 0, 1, 1, 0,
                                                                    0, 0, 1, 1, 0, 0, 0, 0, 0, 0}));
    const auto restored =
        transformFlowEndpoints(moved.value().flow, &moved.value().valid, backward, backward, 2, 2);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored.value().flow, original);
    EXPECT_EQ(restored.value().valid.valid, std::vector<std::uint8_t>({1, 1, 1, 1}));

    // Flip is an involution: flipping twice restores the flow grid and the validity mask.
    const FlowGrid asymmetric =
        flowGrid(3, 2, {0.0, 1.0, 2.0, 2.9, 4.0, 5.0}, {0.5, -1.0, 2.5, -2.0, 0.0, 1.0});
    const ValidMask asymmetricValid{.width = 3, .height = 2, .valid = {0, 1, 1, 1, 0, 1}};
    const auto flippedOnce = flipFlowGridHorizontal(asymmetric, &asymmetricValid);
    ASSERT_TRUE(flippedOnce);
    const auto flippedTwice =
        flipFlowGridHorizontal(flippedOnce.value().flow, &flippedOnce.value().valid);
    ASSERT_TRUE(flippedTwice);
    EXPECT_EQ(flippedTwice.value().flow, asymmetric);
    EXPECT_EQ(flippedTwice.value().valid, asymmetricValid);
}

TEST(LabelGeometryTests, PreservesFlowDirectionAcrossTransforms) {
    // Backward labels must survive every transform unchanged in direction. Time reversal is
    // intentionally absent from the API: the backward field of a swapped pair is the inverse
    // mapping, not the per-sample negation of the forward field (see the header contract).
    const FlowGrid backward = flowGrid(
        2, 2, {0.25, 0.25, 0.25, 0.25}, {0.25, 0.25, 0.25, 0.25}, FlowDirection::kBackward);
    const Affine2 identity{};
    const GridRect whole{.x = 0, .y = 0, .width = 2, .height = 2};
    const GridPads one{.left = 1, .right = 1, .top = 1, .bottom = 1};

    const auto cropped = cropFlowGrid(backward, nullptr, whole);
    ASSERT_TRUE(cropped);
    EXPECT_EQ(cropped.value().flow.direction, FlowDirection::kBackward);

    const auto flipped = flipFlowGridHorizontal(backward, nullptr);
    ASSERT_TRUE(flipped);
    EXPECT_EQ(flipped.value().flow.direction, FlowDirection::kBackward);

    const auto transformed = transformFlowEndpoints(backward, nullptr, identity, identity, 2, 2);
    ASSERT_TRUE(transformed);
    EXPECT_EQ(transformed.value().flow.direction, FlowDirection::kBackward);

    const auto shortcut = transformFlowVectorsForCommonAffine(backward, nullptr, identity, 2, 2);
    ASSERT_TRUE(shortcut);
    EXPECT_EQ(shortcut.value().flow.direction, FlowDirection::kBackward);

    const auto padded = padFlowGrid(backward, nullptr, one);
    ASSERT_TRUE(padded);
    EXPECT_EQ(padded.value().flow.direction, FlowDirection::kBackward);
}

TEST(LabelGeometryTests, DropsSamplesThatLandBetweenPixelCentresOrOutsideTheTarget) {
    // Halved x coordinates: source column 1 maps onto x = 0.5, between pixel centres, and is
    // dropped rather than resampled.
    const FlowGrid flow = flowGrid(3, 1, {2.0, 2.0, 2.0}, {0.0, 0.0, 0.0});
    const Affine2 half{.a11 = 0.5, .a12 = 0.0, .a21 = 0.0, .a22 = 1.0, .tx = 0.0, .ty = 0.0};
    const auto shrunk = transformFlowEndpoints(flow, nullptr, half, half, 2, 1);
    ASSERT_TRUE(shrunk);
    EXPECT_EQ(shrunk.value().flow.width, 2);
    // Covered cells: (0,0)->(0,0) and (2,0)->(1,0); f' = f * A = (1, 0).
    EXPECT_DOUBLE_EQ(shrunk.value().flow.dx[0], 1.0);
    EXPECT_DOUBLE_EQ(shrunk.value().flow.dx[1], 1.0);
    EXPECT_EQ(shrunk.value().valid.valid[0], 1);
    // The second covered sample's endpoint lands exactly on x = 2, the boundary: invalid.
    EXPECT_EQ(shrunk.value().valid.valid[1], 0);

    // Samples whose image leaves the target grid entirely are dropped without an error.
    const Affine2 away{.a11 = 1.0, .a12 = 0.0, .a21 = 0.0, .a22 = 1.0, .tx = -1.0, .ty = -1.0};
    const auto emptied = transformFlowEndpoints(flow, nullptr, away, away, 2, 2);
    ASSERT_TRUE(emptied);
    EXPECT_EQ(emptied.value().flow.dx, std::vector<double>(4, 0.0));
    EXPECT_EQ(emptied.value().valid.valid, std::vector<std::uint8_t>(4, 0));
}

TEST(LabelGeometryTests, RejectsSourceTransformsThatCollapseSamples) {
    const FlowGrid flow = flowGrid(2, 1, {0.0, 0.0}, {0.0, 0.0});
    // T0 projects both columns onto column 0; keeping one survivor would resample.
    const Affine2 collapse{.a11 = 0.0, .a12 = 0.0, .a21 = 0.0, .a22 = 1.0, .tx = 0.0, .ty = 0.0};

    const auto rejected = transformFlowEndpoints(flow, nullptr, collapse, collapse, 2, 1);
    ASSERT_FALSE(rejected);
    expectInvalidArgument(rejected.error());

    const auto shortcutRejected =
        transformFlowVectorsForCommonAffine(flow, nullptr, collapse, 2, 1);
    ASSERT_FALSE(shortcutRejected);
    expectInvalidArgument(shortcutRejected.error());
}

TEST(LabelGeometryTests, RejectsInvalidArgumentsAcrossEveryOperation) {
    const FlowGrid valid = flowGrid(2, 2, {1, 1, 1, 1}, {0, 0, 0, 0});
    const MaskGrid mask{.width = 2, .height = 2, .values = {1, 2, 3, 4}};
    const Affine2 identity{};
    const GridRect unit{.x = 0, .y = 0, .width = 1, .height = 1};

    // Crop rectangles: negative origin, non-positive size, extent past the grid.
    auto rejected =
        cropFlowGrid(valid, nullptr, GridRect{.x = -1, .y = 0, .width = 1, .height = 1});
    ASSERT_FALSE(rejected);
    expectInvalidArgument(rejected.error());
    rejected = cropFlowGrid(valid, nullptr, GridRect{.x = 0, .y = 0, .width = 0, .height = 1});
    ASSERT_FALSE(rejected);
    expectInvalidArgument(rejected.error());
    rejected = cropFlowGrid(valid, nullptr, GridRect{.x = 0, .y = 0, .width = 1, .height = -1});
    ASSERT_FALSE(rejected);
    expectInvalidArgument(rejected.error());
    rejected = cropFlowGrid(valid, nullptr, GridRect{.x = 1, .y = 0, .width = 2, .height = 1});
    ASSERT_FALSE(rejected);
    expectInvalidArgument(rejected.error());
    auto rejectedMask = cropMask(mask, GridRect{.x = 0, .y = 1, .width = 1, .height = 2});
    ASSERT_FALSE(rejectedMask);
    expectInvalidArgument(rejectedMask.error());

    // Storage that does not match the declared dimensions.
    const FlowGrid ragged = flowGrid(2, 2, {1, 1, 1}, {0, 0, 0, 0});
    rejected = cropFlowGrid(ragged, nullptr, unit);
    ASSERT_FALSE(rejected);
    expectInvalidArgument(rejected.error());
    rejected = flipFlowGridHorizontal(ragged, nullptr);
    ASSERT_FALSE(rejected);
    expectInvalidArgument(rejected.error());
    rejected = padFlowGrid(ragged, nullptr, GridPads{});
    ASSERT_FALSE(rejected);
    expectInvalidArgument(rejected.error());
    rejected = transformFlowEndpoints(ragged, nullptr, identity, identity, 2, 2);
    ASSERT_FALSE(rejected);
    expectInvalidArgument(rejected.error());
    const MaskGrid raggedMask{.width = 2, .height = 2, .values = {1, 2, 3}};
    rejectedMask = flipMaskHorizontal(raggedMask);
    ASSERT_FALSE(rejectedMask);
    expectInvalidArgument(rejectedMask.error());
    rejectedMask = cropMask(raggedMask, unit);
    ASSERT_FALSE(rejectedMask);
    expectInvalidArgument(rejectedMask.error());
    auto rejectedPad = padMask(raggedMask, GridPads{}, 0.0);
    ASSERT_FALSE(rejectedPad);
    expectInvalidArgument(rejectedPad.error());

    // Dimensions outside the accepted range (side bound is 1'000'000).
    const FlowGrid oversized{.width = 1'000'001, .height = 1, .dx = {}, .dy = {}};
    rejected = flipFlowGridHorizontal(oversized, nullptr);
    ASSERT_FALSE(rejected);
    expectInvalidArgument(rejected.error());
    rejected = transformFlowEndpoints(valid, nullptr, identity, identity, 0, 2);
    ASSERT_FALSE(rejected);
    expectInvalidArgument(rejected.error());
    rejected = transformFlowEndpoints(valid, nullptr, identity, identity, 2, 1'000'001);
    ASSERT_FALSE(rejected);
    expectInvalidArgument(rejected.error());

    // Validity-mask contract violations: wrong size, non-binary entries, wrong dimensions.
    const ValidMask wrongSize{.width = 2, .height = 2, .valid = {1, 0, 1}};
    rejected = cropFlowGrid(valid, &wrongSize, unit);
    ASSERT_FALSE(rejected);
    expectInvalidArgument(rejected.error());
    const ValidMask nonBinary{.width = 2, .height = 2, .valid = {1, 0, 2, 0}};
    rejected = flipFlowGridHorizontal(valid, &nonBinary);
    ASSERT_FALSE(rejected);
    expectInvalidArgument(rejected.error());
    const ValidMask wrongDims{.width = 1, .height = 4, .valid = {1, 0, 1, 0}};
    rejected = padFlowGrid(valid, &wrongDims, GridPads{});
    ASSERT_FALSE(rejected);
    expectInvalidArgument(rejected.error());
    rejected = transformFlowEndpoints(valid, &wrongDims, identity, identity, 2, 2);
    ASSERT_FALSE(rejected);
    expectInvalidArgument(rejected.error());

    // Padding: negative amounts and totals beyond the grid-side bound.
    rejected = padFlowGrid(valid, nullptr, GridPads{.left = -1, .right = 0, .top = 0, .bottom = 0});
    ASSERT_FALSE(rejected);
    expectInvalidArgument(rejected.error());
    rejected = padFlowGrid(
        valid, nullptr, GridPads{.left = 1'000'000, .right = 1'000'000, .top = 0, .bottom = 0});
    ASSERT_FALSE(rejected);
    expectInvalidArgument(rejected.error());
    rejectedPad = padMask(mask, GridPads{.top = -1}, 0.0);
    ASSERT_FALSE(rejectedPad);
    expectInvalidArgument(rejectedPad.error());

    // A grid with non-positive dimensions is rejected before anything else.
    const FlowGrid empty = flowGrid(0, 0, {}, {});
    rejected = flipFlowGridHorizontal(empty, nullptr);
    ASSERT_FALSE(rejected);
    expectInvalidArgument(rejected.error());
}

} // namespace
} // namespace dvs::domain
