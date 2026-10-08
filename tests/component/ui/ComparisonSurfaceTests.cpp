#include "dvs/application/FrameSet.h"
#include "dvs/platform/D3d11ComparisonRenderer.h"
#include "dvs/platform/FrameBudget.h"
#include "dvs/platform/FrameMailbox.h"
#include "dvs/platform/FrameResourceFactory.h"
#include "dvs/platform/GpuTransferActor.h"
#include "dvs/platform/GraphicsDeviceBroker.h"
#include "dvs/platform/PresentationAckMailbox.h"
#include "dvs/platform/RenderActivitySink.h"
#include "dvs/ui/ComparisonSurface.h"

#include "RenderRetry.h"

#include <QColor>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickGraphicsConfiguration>
#include <QQuickWindow>
#include <QScreen>
#include <QThread>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <d3d11.h>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>
#include <wrl/client.h>

namespace dvs::ui {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] std::array<float, 3U> applyColorTransform(
    const platform::Nv12ColorTransform& transform, const float y, const float u, const float v) {
    const std::array<float, 4U> sample{y, u, v, 1.0F};
    std::array<float, 3U> rgb{};
    for (std::size_t row = 0U; row < rgb.size(); ++row) {
        for (std::size_t column = 0U; column < sample.size(); ++column) {
            rgb[row] += transform.yuvToRgb[(row * sample.size()) + column] * sample[column];
        }
    }
    return rgb;
}

[[nodiscard]] QColor expectedNv12Color(const domain::ColorMetadata& metadata,
                                       const std::uint8_t y,
                                       const std::uint8_t u,
                                       const std::uint8_t v) {
    const std::array<float, 3U> rgb = applyColorTransform(platform::nv12ColorTransform(metadata),
                                                          static_cast<float>(y) / 255.0F,
                                                          static_cast<float>(u) / 255.0F,
                                                          static_cast<float>(v) / 255.0F);
    const auto channel = [](const float value) {
        return static_cast<int>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    };
    return QColor{channel(rgb[0U]), channel(rgb[1U]), channel(rgb[2U])};
}

[[nodiscard]] QColor expectedP010Color(const domain::ColorMetadata& metadata,
                                       const std::uint16_t y,
                                       const std::uint16_t u,
                                       const std::uint16_t v) {
    constexpr float kStorageMaximum = 65535.0F;
    const auto normalized = [](const std::uint16_t code) {
        return static_cast<float>(static_cast<std::uint16_t>(code << 6U)) / kStorageMaximum;
    };
    const std::array<float, 3U> rgb = applyColorTransform(
        platform::nv12ColorTransform(metadata, 10U), normalized(y), normalized(u), normalized(v));
    const auto channel = [](const float value) {
        return static_cast<int>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    };
    return QColor{channel(rgb[0U]), channel(rgb[1U]), channel(rgb[2U])};
}

[[nodiscard]] float differenceGainValue(const ComparisonSurface::DifferenceGain gain) {
    switch (gain) {
    case ComparisonSurface::Gain1x:
        return 1.0F;
    case ComparisonSurface::Gain2x:
        return 2.0F;
    case ComparisonSurface::Gain4x:
        return 4.0F;
    case ComparisonSurface::Gain8x:
        return 8.0F;
    case ComparisonSurface::Gain16x:
        return 16.0F;
    }
    return 1.0F;
}

[[nodiscard]] QColor expectedDifferenceColor(const domain::ColorMetadata& metadataA,
                                             const std::array<std::uint8_t, 3U>& sampleA,
                                             const domain::ColorMetadata& metadataB,
                                             const std::array<std::uint8_t, 3U>& sampleB,
                                             const ComparisonSurface::DifferenceMetric metric,
                                             const ComparisonSurface::DifferenceGain gain) {
    const auto rgb = [](const domain::ColorMetadata& metadata,
                        const std::array<std::uint8_t, 3U>& sample) {
        return applyColorTransform(platform::nv12ColorTransform(metadata),
                                   static_cast<float>(sample[0U]) / 255.0F,
                                   static_cast<float>(sample[1U]) / 255.0F,
                                   static_cast<float>(sample[2U]) / 255.0F);
    };
    const auto luma = [](const std::array<float, 3U>& value) {
        return (0.2126F * value[0U]) + (0.7152F * value[1U]) + (0.0722F * value[2U]);
    };
    const std::array<float, 3U> rgbA = rgb(metadataA, sampleA);
    const std::array<float, 3U> rgbB = rgb(metadataB, sampleB);
    const float multiplier = differenceGainValue(gain);
    std::array<float, 3U> result{};
    if (metric == ComparisonSurface::Luma) {
        const float difference = multiplier * std::abs(luma(rgbA) - luma(rgbB));
        result = {difference, difference, difference};
    } else if (metric == ComparisonSurface::Chroma) {
        const float lumaA = luma(rgbA);
        const float lumaB = luma(rgbB);
        const float cbA = (rgbA[2U] - lumaA) / 1.8556F;
        const float cbB = (rgbB[2U] - lumaB) / 1.8556F;
        const float crA = (rgbA[0U] - lumaA) / 1.5748F;
        const float crB = (rgbB[0U] - lumaB) / 1.5748F;
        result = {multiplier * std::abs(crA - crB), 0.0F, multiplier * std::abs(cbA - cbB)};
    } else {
        for (std::size_t index = 0U; index < result.size(); ++index) {
            result[index] = multiplier * std::abs(rgbA[index] - rgbB[index]);
        }
        if (metric == ComparisonSurface::Heatmap) {
            const float value =
                std::clamp(std::max({result[0U], result[1U], result[2U]}), 0.0F, 1.0F);
            result = {std::clamp(value * 3.0F, 0.0F, 1.0F),
                      std::clamp((value * 3.0F) - 1.0F, 0.0F, 1.0F),
                      std::clamp((value * 3.0F) - 2.0F, 0.0F, 1.0F)};
        }
    }
    const auto channel = [](const float value) {
        return static_cast<int>(
            std::lround(std::clamp(value, 0.0F, 1.0F) * static_cast<float>(255)));
    };
    return QColor{channel(result[0U]), channel(result[1U]), channel(result[2U])};
}

void expectColorNear(const QColor& actual, const QColor& expected, const int tolerance = 3) {
    EXPECT_NEAR(actual.red(), expected.red(), tolerance);
    EXPECT_NEAR(actual.green(), expected.green(), tolerance);
    EXPECT_NEAR(actual.blue(), expected.blue(), tolerance);
}

[[nodiscard]] int maximumRgbDifference(const QImage& left, const QImage& right) {
    if (left.size() != right.size() || left.isNull() || right.isNull()) {
        return 255;
    }
    int maximum = 0;
    for (int y = 0; y < left.height(); ++y) {
        for (int x = 0; x < left.width(); ++x) {
            const QColor leftPixel = left.pixelColor(x, y);
            const QColor rightPixel = right.pixelColor(x, y);
            maximum = std::max({maximum,
                                std::abs(leftPixel.red() - rightPixel.red()),
                                std::abs(leftPixel.green() - rightPixel.green()),
                                std::abs(leftPixel.blue() - rightPixel.blue())});
        }
    }
    return maximum;
}

TEST(ComparisonSurfaceGeometryTests, SplitsEveryOddPhysicalPixelWithoutAGap) {
    const platform::SurfaceSplitLayout split =
        platform::computeSurfaceSplit(101.0F, 60.0F, 101U, 60U);

    EXPECT_EQ(split.leftPixelWidth, 50U);
    EXPECT_EQ(split.rightPixelWidth, 51U);
    EXPECT_FLOAT_EQ(split.left.x + split.left.width, split.right.x);
    EXPECT_FLOAT_EQ(split.right.x + split.right.width, 101.0F);
    EXPECT_FLOAT_EQ(split.left.height, 60.0F);
    EXPECT_FLOAT_EQ(split.right.height, 60.0F);
}

TEST(ComparisonSurfaceGeometryTests, AspectFitsUnequalSourcesWithinTheirOwnHalves) {
    const platform::SurfaceRect bounds{
        .x = 50.0F,
        .y = 0.0F,
        .width = 50.0F,
        .height = 60.0F,
    };
    const platform::SurfaceRect landscape = platform::aspectFitRect(bounds, 16U, 9U);
    const platform::SurfaceRect portrait = platform::aspectFitRect(bounds, 9U, 16U);

    EXPECT_FLOAT_EQ(landscape.x, 50.0F);
    EXPECT_NEAR(landscape.height, 28.125F, 0.001F);
    EXPECT_NEAR(landscape.y, 15.9375F, 0.001F);
    EXPECT_FLOAT_EQ(portrait.height, 60.0F);
    EXPECT_NEAR(portrait.width, 33.75F, 0.001F);
    EXPECT_NEAR(portrait.x, 58.125F, 0.001F);
}

TEST(ComparisonSurfaceGeometryTests, ConvertsAndClampsBottomLeftScissorToD3dViewport) {
    const std::optional<platform::D3dScissorRect> converted = platform::d3dScissorFromBottomLeft(
        platform::SurfaceScissorRect{
            .left = -5,
            .bottom = 20,
            .right = 120,
            .top = 50,
        },
        platform::SurfaceViewport{
            .topLeftX = 10.0F,
            .topLeftY = 10.0F,
            .width = 100.0F,
            .height = 100.0F,
        },
        120U);

    ASSERT_TRUE(converted.has_value());
    EXPECT_EQ(*converted,
              (platform::D3dScissorRect{.left = 10, .top = 70, .right = 110, .bottom = 100}));
    EXPECT_FALSE(platform::d3dScissorFromBottomLeft(
                     platform::SurfaceScissorRect{
                         .left = 0,
                         .bottom = 20,
                         .right = 5,
                         .top = 50,
                     },
                     platform::SurfaceViewport{
                         .topLeftX = 10.0F,
                         .topLeftY = 10.0F,
                         .width = 100.0F,
                         .height = 100.0F,
                     },
                     120U)
                     .has_value());
}

TEST(ComparisonSurfaceGeometryTests, RejectsUnknownPresentationOptions) {
    platform::SurfaceRenderState state{
        .logicalWidth = 100.0F,
        .logicalHeight = 60.0F,
        .pixelWidth = 100U,
        .pixelHeight = 60U,
    };
    EXPECT_TRUE(state.isValid());

    state.viewMode = static_cast<platform::SurfaceViewMode>(255U);
    EXPECT_FALSE(state.isValid());
    state.viewMode = platform::SurfaceViewMode::SideBySide;
    state.differenceMetric = static_cast<platform::SurfaceDifferenceMetric>(255U);
    EXPECT_FALSE(state.isValid());
    state.differenceMetric = platform::SurfaceDifferenceMetric::RgbAbsolute;
    state.differenceGain = static_cast<platform::SurfaceDifferenceGain>(255U);
    EXPECT_FALSE(state.isValid());
    state.differenceGain = platform::SurfaceDifferenceGain::Gain1x;
    state.differenceEdge = static_cast<platform::SurfaceDifferenceEdge>(255U);
    EXPECT_FALSE(state.isValid());
    state.differenceEdge = platform::SurfaceDifferenceEdge::Between0And1;
    state.differenceFilter = static_cast<platform::SurfaceDifferenceFilter>(255U);
    EXPECT_FALSE(state.isValid());
    state.differenceFilter = platform::SurfaceDifferenceFilter::Bilinear;
    state.thresholdPolicy = static_cast<platform::SurfaceThresholdPolicy>(255U);
    EXPECT_FALSE(state.isValid());
    state.thresholdPolicy = platform::SurfaceThresholdPolicy::AnyChannel;
    state.threshold = 1.1F;
    EXPECT_FALSE(state.isValid());
    state.threshold = 0.0F;
    state.viewTransform.scale = 0.0F;
    EXPECT_FALSE(state.isValid());
    state.viewTransform.scale = 1.0F;
    state.roiEnabled = true;
    state.roi.right = 0.0F;
    EXPECT_FALSE(state.isValid());
    state.roi = {};
    state.roiEnabled = false;
    state.referenceSlot = 3U;
    EXPECT_FALSE(state.isValid());
}

TEST(ComparisonSurfaceGeometryTests, ComposesOneSynchronizedViewportInsideTheRoi) {
    const platform::SurfaceNormalizedRect sample = platform::effectiveSurfaceSampleRect(
        platform::SurfaceViewTransform{
            .centerX = 0.5F,
            .centerY = 0.25F,
            .scale = 2.0F,
        },
        true,
        platform::SurfaceNormalizedRect{
            .left = 0.2F,
            .top = 0.1F,
            .right = 0.8F,
            .bottom = 0.9F,
        });

    EXPECT_FLOAT_EQ(sample.left, 0.35F);
    EXPECT_FLOAT_EQ(sample.right, 0.65F);
    EXPECT_FLOAT_EQ(sample.top, 0.1F);
    EXPECT_FLOAT_EQ(sample.bottom, 0.5F);
}

TEST(ComparisonSurfaceGeometryTests, BelowFitShrinksTheDestinationAndRetainsTheWholeRoi) {
    const platform::SurfaceRect bounds{0.0F, 0.0F, 800.0F, 600.0F};
    const platform::SurfaceRect fitted{0.0F, 75.0F, 800.0F, 450.0F};
    const platform::SurfaceNormalizedRect roi{0.25F, 0.125F, 0.75F, 0.875F};
    const platform::SurfaceViewTransform belowFit{0.5F, 0.5F, 0.25F};
    const platform::SurfaceNormalizedRect sample =
        platform::effectiveSurfaceSampleRect(belowFit, true, roi);
    EXPECT_FLOAT_EQ(sample.left, roi.left);
    EXPECT_FLOAT_EQ(sample.top, roi.top);
    EXPECT_FLOAT_EQ(sample.right, roi.right);
    EXPECT_FLOAT_EQ(sample.bottom, roi.bottom);
    EXPECT_EQ(platform::transformedSurfaceContentRect(
                  bounds, fitted, belowFit, {1.0F, 1.0F, 0.0F, 0.0F}, {}),
              (platform::SurfaceRect{300.0F, 243.75F, 200.0F, 112.5F}));

    // Fit and magnification keep the established destination, even if it is off-grid.
    const platform::SurfacePixelGrid grid{1.25F, 1.25F, 0.375F, 0.625F};
    for (const float scale : {1.0F, 2.0F, 64.0F}) {
        SCOPED_TRACE(scale);
        EXPECT_EQ(platform::transformedSurfaceContentRect(
                      bounds, fitted, {0.5F, 0.5F, scale}, grid, {0.0F, 0.0F, 200.0F, 112.5F}),
                  fitted);
    }
    const platform::SurfaceNormalizedRect zoomed =
        platform::effectiveSurfaceSampleRect({0.5F, 0.5F, 2.0F}, true, roi);
    EXPECT_FLOAT_EQ(zoomed.left, 0.375F);
    EXPECT_FLOAT_EQ(zoomed.top, 0.3125F);
    EXPECT_FLOAT_EQ(zoomed.right, 0.625F);
    EXPECT_FLOAT_EQ(zoomed.bottom, 0.6875F);
}

TEST(ComparisonSurfaceGeometryTests, DerivesPhysicalPixelGridFromClipAndOffsetViewport) {
    const platform::SurfaceViewport viewport{17.0F, 29.0F, 1024.0F, 512.0F};
    const std::array<float, 16U> clip{
        2.5F / 1024.0F,
        0.0F,
        0.0F,
        -1.0F + 3.75F / 1024.0F,
        0.0F,
        -3.0F / 512.0F,
        0.0F,
        1.0F - 4.5F / 512.0F,
        0.0F,
        0.0F,
        1.0F,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        1.0F,
    };
    const platform::SurfacePixelGrid grid = platform::surfacePixelGridFromClip(clip, viewport);
    ASSERT_TRUE(grid.isValid());
    EXPECT_FLOAT_EQ(grid.scaleX, 1.25F);
    EXPECT_FLOAT_EQ(grid.scaleY, 1.5F);
    EXPECT_FLOAT_EQ(grid.originX, 18.875F);
    EXPECT_FLOAT_EQ(grid.originY, 31.25F);

    // Axis-aligned homogeneous scaling is equivalent; shear/perspective is unsupported.
    std::array<float, 16U> homogeneous = clip;
    for (float& value : homogeneous) {
        value *= 2.0F;
    }
    const platform::SurfacePixelGrid equivalent =
        platform::surfacePixelGridFromClip(homogeneous, viewport);
    EXPECT_FLOAT_EQ(equivalent.scaleX, grid.scaleX);
    EXPECT_FLOAT_EQ(equivalent.scaleY, grid.scaleY);
    EXPECT_FLOAT_EQ(equivalent.originX, grid.originX);
    EXPECT_FLOAT_EQ(equivalent.originY, grid.originY);
    std::array<float, 16U> unsupported = clip;
    unsupported[1U] = 0.0000001F;
    EXPECT_FALSE(platform::surfacePixelGridFromClip(unsupported, viewport).isValid());
    unsupported = clip;
    unsupported[12U] = 0.0000001F;
    EXPECT_FALSE(platform::surfacePixelGridFromClip(unsupported, viewport).isValid());
}

TEST(ComparisonSurfaceGeometryTests, DistinguishesNominalNativeIntentFromRoundedWindowProjection) {
    const auto grid = platform::surfacePixelGridForWindow(801.0F, 601.0F, 1.5F);
    EXPECT_FLOAT_EQ(grid.scaleX, 1202.0F / 801.0F);
    EXPECT_FLOAT_EQ(grid.scaleY, 902.0F / 601.0F);
    // Keep qreal/double precision until Qt's integer rounding, including custom DPR.
    // Narrowing 1.3 to float first rounds 805*DPR down by one entire target pixel.
    const auto customGrid = platform::surfacePixelGridForWindow(805.0, 605.0, 1.3);
    EXPECT_FLOAT_EQ(customGrid.scaleX, 1047.0F / 805.0F);
    EXPECT_FLOAT_EQ(customGrid.scaleY, 787.0F / 605.0F);
    const platform::SurfaceRect bounds{0.0F, 0.0F, 801.0F, 601.0F};
    const auto fitted = platform::aspectFitRect(bounds, 160U, 90U);
    const platform::SurfaceRect pixels{0.0F, 0.0F, 160.0F, 90.0F};
    const float nativeScale = 160.0F / (801.0F * 1.5F);
    const auto native = platform::transformedSurfaceContentRect(
        bounds, fitted, {0.5F, 0.5F, nativeScale}, grid, pixels, 1.5F);
    ASSERT_TRUE(native.isValid());
    EXPECT_NEAR(native.width * grid.scaleX, 160.0F, 0.0001F);
    EXPECT_NEAR(native.height * grid.scaleY, 90.0F, 0.0001F);
    EXPECT_NEAR(native.x * grid.scaleX, std::round(native.x * grid.scaleX), 0.0001F);
    EXPECT_NEAR(native.y * grid.scaleY, std::round(native.y * grid.scaleY), 0.0001F);
    const auto fractional = platform::transformedSurfaceContentRect(
        bounds, fitted, {0.5F, 0.5F, nativeScale * 0.998F}, grid, pixels, 1.5F);
    EXPECT_FLOAT_EQ(fractional.width, fitted.width * nativeScale * 0.998F);
    EXPECT_LT(fractional.width * grid.scaleX, 160.0F);
    const platform::SurfaceRect tightBounds{0.0F, 0.0F, 160.02F, 90.02F};
    const auto tightFit = platform::aspectFitRect(tightBounds, 160U, 90U);
    EXPECT_FALSE(platform::transformedSurfaceContentRect(tightBounds,
                                                         tightFit,
                                                         {0.5F, 0.5F, 160.0F / tightFit.width},
                                                         {0.9998F, 1.0F, 0.0F, 0.0F},
                                                         pixels,
                                                         1.0F)
                     .isValid());

    QQuickWindow window;
    window.resize(801, 601);
    ComparisonSurface surface{window.contentItem()};
    surface.setSize(QSizeF{801.0, 601.0});
    surface.setSourceDisplayInfo(
        {QVariantMap{{QStringLiteral("width"), 160}, {QStringLiteral("height"), 90}}});
    surface.setViewMode(ComparisonSurface::Single);
    const qreal dpr = window.effectiveDevicePixelRatio();
    surface.zoomAt(0.5, 0.5, 160.0 / (801.0 * dpr));
    const auto actualGrid =
        platform::surfacePixelGridForWindow(801.0F, 601.0F, static_cast<float>(dpr));
    const auto content = platform::transformedSurfaceContentRect(
        bounds,
        fitted,
        {0.5F, 0.5F, static_cast<float>(surface.viewScale())},
        actualGrid,
        pixels,
        static_cast<float>(dpr));
    ASSERT_TRUE(content.isValid());
    const auto center =
        surface.mapSurfacePoint(content.x + content.width * 0.5, content.y + content.height * 0.5);
    EXPECT_TRUE(center.value(QStringLiteral("insideContent")).toBool());
    EXPECT_NEAR(center.value(QStringLiteral("sourceX")).toDouble(), 0.5, 0.000001);
    EXPECT_NEAR(center.value(QStringLiteral("sourceY")).toDouble(), 0.5, 0.000001);
    EXPECT_FALSE(surface.mapSurfacePoint(content.x - 0.01, content.y + content.height * 0.5)
                     .value(QStringLiteral("insideContent"))
                     .toBool());

    // Exactly one source pixel remains reachable even when Qt rounds the physical grid
    // below the nominal DPR. Validate the corrected result before applying a generic floor.
    surface.restoreViewport(0.5, 0.5, 1.0, true, 0.0, 0.0, 1.0 / 160.0, 1.0 / 90.0);
    const qreal onePixelScale = 1.0 / (601.0 * dpr);
    EXPECT_TRUE(surface.canDisplayViewScale(onePixelScale));
    surface.zoomAt(0.5, 0.5, onePixelScale);
    EXPECT_NEAR(surface.viewScale(), onePixelScale, 0.000000001);
    EXPECT_TRUE(surface.roiEnabled());
    surface.setSize(QSizeF{802.0, 601.0});
    EXPECT_NEAR(surface.viewScale(), onePixelScale, 0.000000001);
    const auto onePixel = platform::transformedSurfaceContentRect(
        {0.0F, 0.0F, 801.0F, 601.0F},
        {100.0F, 0.0F, 601.0F, 601.0F},
        {0.5F, 0.5F, 1.0F / (601.0F * 1.25F)},
        platform::surfacePixelGridForWindow(801.0F, 601.0F, 1.25F),
        {0.0F, 0.0F, 1.0F, 1.0F},
        1.25F);
    EXPECT_TRUE(onePixel.isValid());
}

TEST(ComparisonSurfaceGeometryTests, NativeTexelCentersAlignInAnOddInsetViewportAtEveryDpr) {
    const platform::SurfaceRect bounds{0.0F, 0.0F, 801.0F, 601.0F};
    const platform::SurfaceRect fitted = platform::aspectFitRect(bounds, 160U, 90U);
    const platform::SurfaceRect source{0.0F, 0.0F, 160.0F, 90.0F};
    constexpr std::array<std::array<float, 2U>, 5U> kTexels{
        {{0.0F, 0.0F}, {159.0F, 0.0F}, {0.0F, 89.0F}, {159.0F, 89.0F}, {79.0F, 44.0F}}};
    for (const float dpr : {1.0F, 1.25F, 1.5F, 2.0F}) {
        SCOPED_TRACE(dpr);
        // The comparison item starts one DIP inside its containing window.
        const platform::SurfacePixelGrid grid{dpr, dpr, dpr, dpr};
        const platform::SurfaceRect content = platform::transformedSurfaceContentRect(
            bounds, fitted, {0.5F, 0.5F, 160.0F / (801.0F * dpr)}, grid, source);
        ASSERT_TRUE(content.isValid());
        EXPECT_NEAR(content.width * dpr, 160.0F, 0.0001F);
        EXPECT_NEAR(content.height * dpr, 90.0F, 0.0001F);
        const float physicalLeft = content.x * dpr + grid.originX;
        const float physicalTop = content.y * dpr + grid.originY;
        EXPECT_NEAR(physicalLeft, std::round(physicalLeft), 0.0001F);
        EXPECT_NEAR(physicalTop, std::round(physicalTop), 0.0001F);
        EXPECT_GE(content.x, bounds.x);
        EXPECT_GE(content.y, bounds.y);
        EXPECT_LE(content.x + content.width, bounds.width);
        EXPECT_LE(content.y + content.height, bounds.height);
        EXPECT_LE(std::abs(content.x + content.width * 0.5F - bounds.width * 0.5F) * dpr, 0.5001F);
        EXPECT_LE(std::abs(content.y + content.height * 0.5F - bounds.height * 0.5F) * dpr,
                  0.5001F);
        // A one-pixel checkerboard sampled at these device centers must land at texel
        // centers, not between alternating black/white texels under bilinear filtering.
        for (const auto& texel : kTexels) {
            const float physicalX = std::round(physicalLeft) + texel[0U] + 0.5F;
            const float physicalY = std::round(physicalTop) + texel[1U] + 0.5F;
            const float sourceX = (physicalX - physicalLeft) / (content.width * dpr) * 160.0F;
            const float sourceY = (physicalY - physicalTop) / (content.height * dpr) * 90.0F;
            EXPECT_NEAR(sourceX, texel[0U] + 0.5F, 0.0001F);
            EXPECT_NEAR(sourceY, texel[1U] + 0.5F, 0.0001F);
        }
    }
}

TEST(ComparisonSurfaceGeometryTests, NativeRotatedRoiKeepsFractionalSourcePixelPhase) {
    const platform::SurfaceNormalizedRect roi{
        8.25F / 64.0F, 4.5F / 32.0F, 55.5F / 64.0F, 27.75F / 32.0F};
    const std::array<platform::SurfaceRect, 4U> expected{
        platform::SurfaceRect{8.25F, 4.5F, 47.25F, 23.25F},
        platform::SurfaceRect{4.5F, 8.5F, 23.25F, 47.25F},
        platform::SurfaceRect{8.5F, 4.25F, 47.25F, 23.25F},
        platform::SurfaceRect{4.25F, 8.25F, 23.25F, 47.25F},
    };
    const platform::SurfaceRect bounds{0.0F, 0.0F, 801.0F, 601.0F};
    // Include a fractional scene translation as well as the one-DIP inset.
    const platform::SurfacePixelGrid grid{1.25F, 1.25F, 1.875F, 2.1875F};
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        SCOPED_TRACE(index * 90U);
        const platform::SurfaceRect source = platform::orientedSurfaceSamplePixels(
            64.0F, 32.0F, static_cast<std::uint16_t>(index * 90U), roi);
        ASSERT_EQ(source, expected[index]);
        const float fitScale = std::min(bounds.width / source.width, bounds.height / source.height);
        const platform::SurfaceRect fitted{
            (bounds.width - source.width * fitScale) * 0.5F,
            (bounds.height - source.height * fitScale) * 0.5F,
            source.width * fitScale,
            source.height * fitScale,
        };
        const platform::SurfaceRect content = platform::transformedSurfaceContentRect(
            bounds, fitted, {0.5F, 0.5F, 1.0F / (fitScale * grid.scaleX)}, grid, source);
        ASSERT_TRUE(content.isValid());
        EXPECT_NEAR(content.width * grid.scaleX, source.width, 0.0001F);
        EXPECT_NEAR(content.height * grid.scaleY, source.height, 0.0001F);
        const float phaseX = content.x * grid.scaleX + grid.originX - source.x;
        const float phaseY = content.y * grid.scaleY + grid.originY - source.y;
        EXPECT_NEAR(phaseX, std::round(phaseX), 0.0001F);
        EXPECT_NEAR(phaseY, std::round(phaseY), 0.0001F);
        EXPECT_GE(content.x, bounds.x);
        EXPECT_GE(content.y, bounds.y);
        EXPECT_LE(content.x + content.width, bounds.width);
        EXPECT_LE(content.y + content.height, bounds.height);
    }

    // When no phase-aligned placement fits, retain the full ROI rather than crossing
    // into the adjacent panel or changing its sample. The unavoidable phase stays visible.
    const platform::SurfaceRect tightBounds{0.0F, 0.0F, 32.5F, 32.5F};
    const platform::SurfaceRect tight =
        platform::transformedSurfaceContentRect(tightBounds,
                                                tightBounds,
                                                {0.5F, 0.5F, 32.0F / 32.5F},
                                                {1.0F, 1.0F, 0.0F, 0.0F},
                                                {0.75F, 0.75F, 32.0F, 32.0F});
    EXPECT_FLOAT_EQ(tight.width, 32.0F);
    EXPECT_FLOAT_EQ(tight.height, 32.0F);
    EXPECT_FLOAT_EQ(tight.x, 0.25F);
    EXPECT_FLOAT_EQ(tight.y, 0.25F);
}

TEST(ComparisonSurfaceGeometryTests, RejectsInvalidScalesAndSubpixelDestinationExtents) {
    platform::SurfaceViewTransform transform{0.5F, 0.5F, presentation::kMinimumViewportScale};
    EXPECT_TRUE(transform.isValid());
    transform.scale = 0.25F;
    EXPECT_TRUE(transform.isValid());
    transform.centerX = 0.25F;
    EXPECT_FALSE(transform.isValid());
    transform.centerX = 0.5F;
    for (const float scale : {0.0F,
                              -1.0F,
                              std::numeric_limits<float>::denorm_min(),
                              std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::quiet_NaN(),
                              65.0F}) {
        SCOPED_TRACE(scale);
        transform.scale = scale;
        EXPECT_FALSE(transform.isValid());
    }
    const platform::SurfaceRect bounds{0.0F, 0.0F, 100.0F, 100.0F};
    const platform::SurfacePixelGrid grid{2.0F, 1.0F, 0.25F, 0.75F};
    EXPECT_TRUE(platform::transformedSurfaceContentRect(
                    bounds, {0.0F, 0.0F, 32.0F, 64.0F}, {0.5F, 0.5F, 0.015625F}, grid, {})
                    .isValid());
    EXPECT_FALSE(platform::transformedSurfaceContentRect(
                     bounds, {0.0F, 0.0F, 31.0F, 64.0F}, {0.5F, 0.5F, 0.015625F}, grid, {})
                     .isValid());
    EXPECT_FALSE(platform::transformedSurfaceContentRect(
                     bounds, {0.0F, 0.0F, 32.0F, 63.0F}, {0.5F, 0.5F, 0.015625F}, grid, {})
                     .isValid());
    EXPECT_FALSE(platform::transformedSurfaceContentRect(
                     bounds, bounds, {0.5F, 0.5F, presentation::kMinimumViewportScale}, grid, {})
                     .isValid());
    EXPECT_FALSE(platform::transformedSurfaceContentRect(bounds, bounds, {0.5F, 0.5F, 0.5F}, {}, {})
                     .isValid());
    transform = {0.5F, 0.5F, 0.5F};
    EXPECT_FALSE(platform::transformedSurfaceContentRect(
                     bounds,
                     {0.0F, 0.0F, std::numeric_limits<float>::infinity(), 100.0F},
                     transform,
                     grid,
                     {})
                     .isValid());
}

TEST(ComparisonSurfaceGeometryTests, MapsShrunkenContentCornersAndRejectsItsNewBackground) {
    ComparisonSurface surface;
    surface.setSize(QSizeF{800.0, 600.0});
    surface.setPosition(QPointF{1.0, 1.0});
    surface.setSourceDisplayInfo(
        {QVariantMap{{QStringLiteral("width"), 160}, {QStringLiteral("height"), 90}}});
    surface.setViewMode(ComparisonSurface::Single);
    surface.zoomAt(0.25, 0.75, 0.2);
    ASSERT_DOUBLE_EQ(surface.viewScale(), 0.2);
    surface.panBy(0.2, -0.2);
    EXPECT_DOUBLE_EQ(surface.viewCenterX(), 0.5);
    EXPECT_DOUBLE_EQ(surface.viewCenterY(), 0.5);

    const auto expectContent = [&](const qreal left,
                                   const qreal top,
                                   const qreal width,
                                   const qreal height,
                                   const qreal roiLeft,
                                   const qreal roiTop,
                                   const qreal roiExtent) {
        constexpr std::array<std::array<qreal, 2U>, 5U> kPoints{
            {{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}, {1.0, 1.0}, {0.5, 0.5}}};
        for (const auto& point : kPoints) {
            const QVariantMap hit =
                surface.mapSurfacePoint(left + point[0U] * width, top + point[1U] * height);
            EXPECT_TRUE(hit.value(QStringLiteral("insideContent")).toBool());
            EXPECT_NEAR(hit.value(QStringLiteral("displayX")).toDouble(), point[0U], 0.0001);
            EXPECT_NEAR(hit.value(QStringLiteral("displayY")).toDouble(), point[1U], 0.0001);
            EXPECT_NEAR(hit.value(QStringLiteral("sourceX")).toDouble(),
                        roiLeft + point[0U] * roiExtent,
                        0.0001);
            EXPECT_NEAR(hit.value(QStringLiteral("sourceY")).toDouble(),
                        roiTop + point[1U] * roiExtent,
                        0.0001);
        }
        const std::array<QPointF, 4U> background{
            QPointF{left - 0.5, top + height * 0.5},
            QPointF{left + width + 0.5, top + height * 0.5},
            QPointF{left + width * 0.5, top - 0.5},
            QPointF{left + width * 0.5, top + height + 0.5},
        };
        for (const QPointF& point : background) {
            const QVariantMap hit = surface.mapSurfacePoint(point.x(), point.y());
            EXPECT_TRUE(hit.value(QStringLiteral("insidePanel")).toBool());
            EXPECT_FALSE(hit.value(QStringLiteral("insideContent")).toBool());
            EXPECT_FALSE(hit.contains(QStringLiteral("sourceX")));
            EXPECT_FALSE(hit.contains(QStringLiteral("sourceY")));
        }
    };
    expectContent(320.0, 255.0, 160.0, 90.0, 0.0, 0.0, 1.0);

    // Restoring a smaller ROI must keep the ROI's fractional pixel phase. The top
    // source edge is 22.5 pixels, so the 45-pixel destination starts at y=277.5.
    surface.restoreViewport(0.5, 0.5, 0.1, true, 0.25, 0.25, 0.75, 0.75);
    ASSERT_DOUBLE_EQ(surface.viewScale(), 0.1);
    expectContent(360.0, 277.5, 80.0, 45.0, 0.25, 0.25, 0.5);
}

TEST(ComparisonSurfaceGeometryTests, KeepsBelowFitInsideThePhysicalFootprintAfterResize) {
    ComparisonSurface surface;
    surface.setSize(QSizeF{800.0, 600.0});
    surface.setSourceDisplayInfo(
        {QVariantMap{{QStringLiteral("width"), 160}, {QStringLiteral("height"), 90}}});
    surface.setViewMode(ComparisonSurface::Single);
    surface.zoomAt(0.5, 0.5, 0.01);
    ASSERT_DOUBLE_EQ(surface.viewScale(), 0.01);
    surface.setSize(QSizeF{32.0, 32.0});
    EXPECT_NEAR(surface.minimumViewScale(), 1.0 / 18.0, 0.000001);
    EXPECT_DOUBLE_EQ(surface.viewScale(), surface.minimumViewScale());
    const qreal before = surface.viewScale();
    surface.restoreViewport(0.5, 0.5, 0.001, true, 0.25, 0.25, 0.75, 0.75);
    EXPECT_DOUBLE_EQ(surface.viewScale(), before);
    EXPECT_FALSE(surface.roiEnabled());
    surface.zoomAt(0.5, 0.5, 0.001);
    EXPECT_DOUBLE_EQ(surface.viewScale(), surface.minimumViewScale());
    surface.setSize(QSizeF{800.0, 600.0});
    EXPECT_DOUBLE_EQ(surface.viewScale(), before);
}

TEST(ComparisonSurfaceGeometryTests, SharesThreeSourcePanelGeometryWithLabels) {
    const platform::SurfacePanelLayout threeUp =
        platform::computeSurfacePanelLayout(platform::SurfaceViewMode::ThreeUp,
                                            901.0F,
                                            600.0F,
                                            901U,
                                            600U,
                                            0U,
                                            platform::SurfaceDifferenceEdge::Between0And1,
                                            0.5F);
    ASSERT_EQ(threeUp.sourceCount, 3U);
    EXPECT_EQ(threeUp.sourceSlots, (std::array<std::uint8_t, 3U>{0U, 1U, 2U}));
    EXPECT_FLOAT_EQ(threeUp.sourceRects[0U].width + threeUp.sourceRects[1U].width +
                        threeUp.sourceRects[2U].width,
                    901.0F);

    const platform::SurfacePanelLayout focus =
        platform::computeSurfacePanelLayout(platform::SurfaceViewMode::ReferenceFocus,
                                            900.0F,
                                            601.0F,
                                            900U,
                                            601U,
                                            2U,
                                            platform::SurfaceDifferenceEdge::Between0And1,
                                            0.5F);
    ASSERT_EQ(focus.sourceCount, 3U);
    EXPECT_EQ(focus.sourceSlots, (std::array<std::uint8_t, 3U>{2U, 0U, 1U}));
    EXPECT_FLOAT_EQ(focus.sourceRects[0U].width, 600.0F);
    EXPECT_FLOAT_EQ(focus.sourceRects[1U].x, 600.0F);

    const platform::SurfacePanelLayout grid =
        platform::computeSurfacePanelLayout(platform::SurfaceViewMode::AnalysisGrid,
                                            901.0F,
                                            601.0F,
                                            901U,
                                            601U,
                                            0U,
                                            platform::SurfaceDifferenceEdge::Between0And2,
                                            0.5F);
    ASSERT_EQ(grid.sourceCount, 3U);
    ASSERT_TRUE(grid.differenceRect.has_value());
    EXPECT_FLOAT_EQ(grid.sourceRects[2U].y, grid.differenceRect->y);
    EXPECT_FLOAT_EQ(grid.sourceRects[1U].x, grid.differenceRect->x);
}

TEST(ComparisonSurfaceGeometryTests, MapsWipeLabelsToSelectedPairAndSplit) {
    const platform::SurfacePanelLayout wipe =
        platform::computeSurfacePanelLayout(platform::SurfaceViewMode::Wipe,
                                            800.0F,
                                            450.0F,
                                            800U,
                                            450U,
                                            0U,
                                            platform::SurfaceDifferenceEdge::Between1And2,
                                            0.25F);
    ASSERT_EQ(wipe.sourceCount, 2U);
    EXPECT_EQ(wipe.sourceSlots[0U], 1U);
    EXPECT_EQ(wipe.sourceSlots[1U], 2U);
    EXPECT_FLOAT_EQ(wipe.sourceRects[0U].width, 200.0F);
    EXPECT_FLOAT_EQ(wipe.sourceRects[1U].x, 200.0F);
}

TEST(ComparisonSurfaceGeometryTests, MapsPointerInputThroughTheRendererPanelLayout) {
    ComparisonSurface surface;
    surface.setWidth(800.0);
    surface.setHeight(450.0);
    surface.setSourceDisplayInfo(
        {QVariantMap{{QStringLiteral("width"), 1920}, {QStringLiteral("height"), 1080}},
         QVariantMap{{QStringLiteral("width"), 1920}, {QStringLiteral("height"), 1080}},
         QVariantMap{{QStringLiteral("width"), 1920}, {QStringLiteral("height"), 1080}}});
    surface.setViewMode(ComparisonSurface::Wipe);
    surface.setDifferenceEdge(ComparisonSurface::Edge1And2);
    surface.setWipePosition(0.25);

    const QVariantMap left = surface.mapSurfacePoint(100.0, 225.0);
    EXPECT_EQ(left.value(QStringLiteral("panelIndex")).toInt(), 0);
    EXPECT_EQ(left.value(QStringLiteral("sourceSlot")).toInt(), 1);
    EXPECT_EQ(left.value(QStringLiteral("region")).toInt(), ComparisonSurface::WipeCompositeRegion);
    EXPECT_TRUE(left.value(QStringLiteral("insideContent")).toBool());
    EXPECT_DOUBLE_EQ(left.value(QStringLiteral("normalizedX")).toDouble(), 0.125);
    EXPECT_DOUBLE_EQ(left.value(QStringLiteral("normalizedY")).toDouble(), 0.5);

    const QVariantMap right = surface.mapSurfacePoint(500.0, 225.0);
    EXPECT_EQ(right.value(QStringLiteral("panelIndex")).toInt(), 1);
    EXPECT_EQ(right.value(QStringLiteral("sourceSlot")).toInt(), 2);
    EXPECT_TRUE(right.value(QStringLiteral("insideContent")).toBool());
    EXPECT_DOUBLE_EQ(right.value(QStringLiteral("normalizedX")).toDouble(), 0.625);
    EXPECT_DOUBLE_EQ(right.value(QStringLiteral("normalizedY")).toDouble(), 0.5);
}

TEST(ComparisonSurfaceGeometryTests, RejectsLetterboxAndMapsAnalysisDifferenceContent) {
    ComparisonSurface surface;
    surface.setWidth(800.0);
    surface.setHeight(800.0);
    surface.setSourceDisplayInfo(
        {QVariantMap{{QStringLiteral("width"), 1920}, {QStringLiteral("height"), 1080}},
         QVariantMap{{QStringLiteral("width"), 1080}, {QStringLiteral("height"), 1920}},
         QVariantMap{{QStringLiteral("width"), 1920}, {QStringLiteral("height"), 1080}}});
    surface.setViewMode(ComparisonSurface::Single);

    const QVariantMap letterbox = surface.mapSurfacePoint(400.0, 50.0);
    EXPECT_TRUE(letterbox.value(QStringLiteral("insidePanel")).toBool());
    EXPECT_FALSE(letterbox.value(QStringLiteral("insideContent")).toBool());

    surface.setViewMode(ComparisonSurface::AnalysisGrid);
    const QVariantMap difference = surface.mapSurfacePoint(600.0, 600.0);
    EXPECT_EQ(difference.value(QStringLiteral("region")).toInt(),
              ComparisonSurface::DifferenceRegion);
    EXPECT_EQ(difference.value(QStringLiteral("panelIndex")).toInt(), 3);
    EXPECT_TRUE(difference.value(QStringLiteral("insideContent")).toBool());
    EXPECT_DOUBLE_EQ(difference.value(QStringLiteral("normalizedX")).toDouble(), 0.5);
    EXPECT_DOUBLE_EQ(difference.value(QStringLiteral("normalizedY")).toDouble(), 0.5);
}

TEST(ComparisonSurfaceGeometryTests, MapsRotatedRoiAndZoomPointsBackToSourceCoordinates) {
    ComparisonSurface surface;
    surface.setWidth(800.0);
    surface.setHeight(800.0);
    surface.setSourceDisplayInfo({QVariantMap{{QStringLiteral("width"), 1920},
                                              {QStringLiteral("height"), 1080},
                                              {QStringLiteral("rotationDegrees"), 90}}});
    surface.setViewMode(ComparisonSurface::Single);
    surface.restoreViewport(0.5, 0.5, 2.0, true, 0.2, 0.1, 0.8, 0.9);

    // The displayed ROI is 600 x 800 with a 90-degree source rotation. Its top-left
    // logical point is therefore the source's upper-right point in the zoomed ROI.
    const QVariantMap point = surface.mapSurfacePoint(100.0, 0.0);
    EXPECT_TRUE(point.value(QStringLiteral("insideContent")).toBool());
    EXPECT_NEAR(point.value(QStringLiteral("displayX")).toDouble(), 0.0, 0.0001);
    EXPECT_NEAR(point.value(QStringLiteral("displayY")).toDouble(), 0.0, 0.0001);
    EXPECT_NEAR(point.value(QStringLiteral("normalizedX")).toDouble(), 1.0, 0.0001);
    EXPECT_NEAR(point.value(QStringLiteral("normalizedY")).toDouble(), 0.0, 0.0001);
    EXPECT_NEAR(point.value(QStringLiteral("sourceX")).toDouble(), 0.65, 0.0001);
    EXPECT_NEAR(point.value(QStringLiteral("sourceY")).toDouble(), 0.3, 0.0001);
}

TEST(ComparisonSurfaceGeometryTests, KeepsWipeHandleAlignedWithSurfaceCoordinates) {
    ComparisonSurface surface;
    surface.setWidth(800.0);
    surface.setHeight(800.0);
    surface.setSourceDisplayInfo(
        {QVariantMap{{QStringLiteral("width"), 1080}, {QStringLiteral("height"), 1920}},
         QVariantMap{{QStringLiteral("width"), 1080}, {QStringLiteral("height"), 1920}}});
    surface.setViewMode(ComparisonSurface::Wipe);
    surface.setWipePosition(0.25);

    // The divider remains in surface coordinates even when portrait content is letterboxed.
    EXPECT_NEAR(surface.wipeSplitLogicalX(), 200.0, 0.0001);
    EXPECT_NEAR(surface.wipePositionForLogicalX(200.0), 0.25, 0.0001);
    EXPECT_NEAR(surface.wipePositionForLogicalX(0.0), 0.0, 0.0001);
    EXPECT_NEAR(surface.wipePositionForLogicalX(800.0), 1.0, 0.0001);
}

TEST(ComparisonSurfaceColorTests, ConvertsFullRangeBt601AndBt709WithDifferentMatrices) {
    const domain::ColorMetadata bt601{
        .matrix = domain::ColorMatrix::kBt601,
        .range = domain::ColorRange::kFull,
        .matrixInferred = false,
    };
    const domain::ColorMetadata bt709{
        .matrix = domain::ColorMatrix::kBt709,
        .range = domain::ColorRange::kFull,
        .matrixInferred = false,
    };

    const std::array<float, 3U> rgb601 =
        applyColorTransform(platform::nv12ColorTransform(bt601), 0.5F, 0.25F, 0.75F);
    const std::array<float, 3U> rgb709 =
        applyColorTransform(platform::nv12ColorTransform(bt709), 0.5F, 0.25F, 0.75F);

    // Full-range digital chroma remains centred on code 128, not the mathematical
    // midpoint 127.5. These values therefore include the 128 / 255 offset.
    EXPECT_NEAR(rgb601[0U], 0.847751F, 0.0002F);
    EXPECT_NEAR(rgb601[1U], 0.409575F, 0.0003F);
    EXPECT_NEAR(rgb601[2U], 0.053525F, 0.0002F);
    EXPECT_NEAR(rgb709[0U], 0.890612F, 0.0002F);
    EXPECT_NEAR(rgb709[1U], 0.431085F, 0.0003F);
    EXPECT_NEAR(rgb709[2U], 0.032461F, 0.0002F);
}

TEST(ComparisonSurfaceColorTests, NormalizesLimitedRangeBlackAndWhiteForBothMatrices) {
    for (const domain::ColorMatrix matrix :
         {domain::ColorMatrix::kBt601, domain::ColorMatrix::kBt709}) {
        const domain::ColorMetadata metadata{
            .matrix = matrix,
            .range = domain::ColorRange::kLimited,
            .matrixInferred = false,
        };
        const platform::Nv12ColorTransform transform = platform::nv12ColorTransform(metadata);
        const std::array<float, 3U> black =
            applyColorTransform(transform, 16.0F / 255.0F, 128.0F / 255.0F, 128.0F / 255.0F);
        const std::array<float, 3U> white =
            applyColorTransform(transform, 235.0F / 255.0F, 128.0F / 255.0F, 128.0F / 255.0F);
        for (const float channel : black) {
            EXPECT_NEAR(channel, 0.0F, 0.0002F);
        }
        for (const float channel : white) {
            EXPECT_NEAR(channel, 1.0F, 0.0002F);
        }
    }
}

TEST(ComparisonSurfacePropertyTests, ExposesTypedDifferenceDefaultsAndNotifiesOnlyOnChange) {
    ComparisonSurface surface;

    EXPECT_EQ(surface.viewMode(), ComparisonSurface::SideBySide);
    EXPECT_EQ(surface.differenceMetric(), ComparisonSurface::RgbAbsolute);
    EXPECT_EQ(surface.differenceGain(), ComparisonSurface::Gain1x);
    EXPECT_EQ(surface.differenceEdge(), ComparisonSurface::Edge0And1);
    EXPECT_EQ(surface.differenceFilter(), ComparisonSurface::Bilinear);
    EXPECT_EQ(surface.referenceSlot(), 0);

    int viewModeChanges = 0;
    int metricChanges = 0;
    int gainChanges = 0;
    int edgeChanges = 0;
    int filterChanges = 0;
    int referenceSlotChanges = 0;
    QObject::connect(&surface, &ComparisonSurface::viewModeChanged, [&] { ++viewModeChanges; });
    QObject::connect(
        &surface, &ComparisonSurface::differenceMetricChanged, [&] { ++metricChanges; });
    QObject::connect(&surface, &ComparisonSurface::differenceGainChanged, [&] { ++gainChanges; });
    QObject::connect(&surface, &ComparisonSurface::differenceEdgeChanged, [&] { ++edgeChanges; });
    QObject::connect(
        &surface, &ComparisonSurface::differenceFilterChanged, [&] { ++filterChanges; });
    QObject::connect(
        &surface, &ComparisonSurface::referenceSlotChanged, [&] { ++referenceSlotChanges; });

    surface.setViewMode(ComparisonSurface::Difference);
    surface.setDifferenceMetric(ComparisonSurface::Heatmap);
    surface.setDifferenceGain(ComparisonSurface::Gain16x);
    surface.setDifferenceEdge(ComparisonSurface::Edge1And2);
    surface.setDifferenceFilter(ComparisonSurface::Bicubic);
    surface.setReferenceSlot(2);

    EXPECT_EQ(surface.viewMode(), ComparisonSurface::Difference);
    EXPECT_EQ(surface.differenceMetric(), ComparisonSurface::Heatmap);
    EXPECT_EQ(surface.differenceGain(), ComparisonSurface::Gain16x);
    EXPECT_EQ(surface.differenceEdge(), ComparisonSurface::Edge1And2);
    EXPECT_EQ(surface.differenceFilter(), ComparisonSurface::Bicubic);
    EXPECT_EQ(surface.referenceSlot(), 2);
    EXPECT_EQ(viewModeChanges, 1);
    EXPECT_EQ(metricChanges, 1);
    EXPECT_EQ(gainChanges, 1);
    EXPECT_EQ(edgeChanges, 1);
    EXPECT_EQ(filterChanges, 1);
    EXPECT_EQ(referenceSlotChanges, 1);

    surface.setViewMode(ComparisonSurface::Difference);
    surface.setDifferenceMetric(ComparisonSurface::Heatmap);
    surface.setDifferenceGain(ComparisonSurface::Gain16x);
    surface.setDifferenceEdge(ComparisonSurface::Edge1And2);
    surface.setDifferenceFilter(ComparisonSurface::Bicubic);
    surface.setReferenceSlot(2);
    EXPECT_EQ(viewModeChanges, 1);
    EXPECT_EQ(metricChanges, 1);
    EXPECT_EQ(gainChanges, 1);
    EXPECT_EQ(edgeChanges, 1);
    EXPECT_EQ(filterChanges, 1);
    EXPECT_EQ(referenceSlotChanges, 1);
}

// The video-slot filter is what makes high magnification trustworthy: bilinear above 200%
// averages neighbouring texels and hides block artifacts, so Auto flips to nearest there.
// The explicit Smooth/Pixel modes override the automatic switch, and every change notifies.
TEST(ComparisonSurfacePropertyTests, VideoFilterModeDefaultsToAutoAndSwitchesAtTwiceZoom) {
    ComparisonSurface surface;

    EXPECT_EQ(surface.videoFilterMode(), ComparisonSurface::VideoFilterAuto);
    // Fit zoom (1.0) samples smoothly; the zoomed-in case must not.
    EXPECT_EQ(surface.effectiveVideoFilter(), ComparisonSurface::Bilinear);
    surface.zoomAt(0.5, 0.5, 2.5);
    EXPECT_NEAR(surface.viewScale(), 2.5, 0.000001);
    EXPECT_EQ(surface.effectiveVideoFilter(), ComparisonSurface::Nearest);
    // The threshold itself: exactly 200% already shows individual texels.
    surface.zoomAt(0.5, 0.5, 2.0 / 2.5);
    EXPECT_NEAR(surface.viewScale(), 2.0, 0.000001);
    EXPECT_EQ(surface.effectiveVideoFilter(), ComparisonSurface::Nearest);
    surface.zoomAt(0.5, 0.5, 0.5);
    EXPECT_NEAR(surface.viewScale(), 1.0, 0.000001);
    EXPECT_EQ(surface.effectiveVideoFilter(), ComparisonSurface::Bilinear);

    int modeChanges = 0;
    QObject::connect(&surface, &ComparisonSurface::videoFilterModeChanged, [&] { ++modeChanges; });

    surface.setVideoFilterMode(ComparisonSurface::VideoFilterPixel);
    EXPECT_EQ(surface.videoFilterMode(), ComparisonSurface::VideoFilterPixel);
    EXPECT_EQ(surface.effectiveVideoFilter(), ComparisonSurface::Nearest);
    // Pinned smooth keeps bilinear even at high zoom.
    surface.setVideoFilterMode(ComparisonSurface::VideoFilterSmooth);
    EXPECT_EQ(surface.effectiveVideoFilter(), ComparisonSurface::Bilinear);
    surface.zoomAt(0.5, 0.5, 4.0);
    EXPECT_NEAR(surface.viewScale(), 4.0, 0.000001);
    EXPECT_EQ(surface.effectiveVideoFilter(), ComparisonSurface::Bilinear);
    EXPECT_EQ(modeChanges, 2);

    // Same-value writes do not notify, and unknown values are rejected.
    surface.setVideoFilterMode(ComparisonSurface::VideoFilterSmooth);
    surface.setVideoFilterMode(static_cast<ComparisonSurface::VideoFilterMode>(99));
    EXPECT_EQ(modeChanges, 2);

    // Back to Auto: the zoom decides again.
    surface.setVideoFilterMode(ComparisonSurface::VideoFilterAuto);
    EXPECT_EQ(modeChanges, 3);
    EXPECT_EQ(surface.effectiveVideoFilter(), ComparisonSurface::Nearest);
}

TEST(ComparisonSurfacePropertyTests, DifferenceSuppressedDefaultsFalseAndNotifiesOnlyOnChange) {
    ComparisonSurface surface;

    EXPECT_FALSE(surface.differenceSuppressed());
    int suppressedChanges = 0;
    int geometryChanges = 0;
    QObject::connect(
        &surface, &ComparisonSurface::differenceSuppressedChanged, [&] { ++suppressedChanges; });
    QObject::connect(
        &surface, &ComparisonSurface::presentationGeometryChanged, [&] { ++geometryChanges; });

    surface.setDifferenceSuppressed(true);
    EXPECT_TRUE(surface.differenceSuppressed());
    EXPECT_EQ(suppressedChanges, 1);
    // Hold-to-peek is transient presentation state: the panel layout must not move while
    // the button is held, so no geometry invalidation may fire.
    EXPECT_EQ(geometryChanges, 0);

    surface.setDifferenceSuppressed(true);
    EXPECT_EQ(suppressedChanges, 1);

    surface.setDifferenceSuppressed(false);
    EXPECT_FALSE(surface.differenceSuppressed());
    EXPECT_EQ(suppressedChanges, 2);
    EXPECT_EQ(geometryChanges, 0);
}

TEST(ComparisonSurfacePropertyTests, ClampsWipePositionAndNotifiesOnlyOnChange) {
    ComparisonSurface surface;
    int changes = 0;
    int geometryChanges = 0;
    QObject::connect(&surface, &ComparisonSurface::wipePositionChanged, [&] { ++changes; });
    QObject::connect(
        &surface, &ComparisonSurface::presentationGeometryChanged, [&] { ++geometryChanges; });

    surface.setWidth(640.0);
    EXPECT_DOUBLE_EQ(surface.wipePosition(), 0.5);
    EXPECT_DOUBLE_EQ(surface.wipeSplitLogicalX(), 320.0);
    surface.setViewMode(ComparisonSurface::Wipe);
    surface.setWipePosition(0.25);
    EXPECT_DOUBLE_EQ(surface.wipePosition(), 0.25);
    EXPECT_DOUBLE_EQ(surface.wipeSplitLogicalX(), 160.0);
    EXPECT_EQ(changes, 1);
    EXPECT_GE(geometryChanges, 2);

    surface.setWipePosition(0.25);
    EXPECT_EQ(changes, 1);
    surface.setWipePosition(-1.0);
    EXPECT_DOUBLE_EQ(surface.wipePosition(), 0.0);
    surface.setWipePosition(2.0);
    EXPECT_DOUBLE_EQ(surface.wipePosition(), 1.0);
    EXPECT_EQ(changes, 3);
}

TEST(ComparisonSurfacePropertyTests, AcceptsExplicitSingleViewModeValue) {
    ComparisonSurface surface;
    surface.setViewMode(ComparisonSurface::Single);
    EXPECT_EQ(surface.viewMode(), ComparisonSurface::Single);
}

TEST(ComparisonSurfacePropertyTests, ValidatesThresholdAndSynchronizedViewportMutations) {
    ComparisonSurface surface;
    int thresholdChanges = 0;
    int viewportChanges = 0;
    QObject::connect(&surface, &ComparisonSurface::thresholdChanged, [&] { ++thresholdChanges; });
    QObject::connect(&surface, &ComparisonSurface::viewportChanged, [&] { ++viewportChanges; });

    surface.setThresholdEnabled(true);
    surface.setThreshold(64.0 / 255.0);
    surface.setThresholdPolicy(ComparisonSurface::ThresholdAllChannels);
    EXPECT_TRUE(surface.thresholdEnabled());
    EXPECT_NEAR(surface.threshold(), 64.0 / 255.0, 0.000001);
    EXPECT_EQ(surface.thresholdPolicy(), ComparisonSurface::ThresholdAllChannels);
    EXPECT_EQ(thresholdChanges, 3);

    surface.zoomAt(0.25, 0.75, 4.0);
    EXPECT_DOUBLE_EQ(surface.viewScale(), 4.0);
    const qreal zoomedCenterX = surface.viewCenterX();
    surface.panBy(0.1, -0.1);
    EXPECT_NE(surface.viewCenterX(), zoomedCenterX);
    surface.setRoiNormalized(0.8, 0.7, 0.2, 0.1);
    EXPECT_TRUE(surface.roiEnabled());
    EXPECT_DOUBLE_EQ(surface.roiLeft(), 0.2);
    EXPECT_DOUBLE_EQ(surface.roiTop(), 0.1);
    EXPECT_DOUBLE_EQ(surface.roiRight(), 0.8);
    EXPECT_DOUBLE_EQ(surface.roiBottom(), 0.7);
    EXPECT_DOUBLE_EQ(surface.viewScale(), 1.0);
    surface.clearRoi();
    EXPECT_FALSE(surface.roiEnabled());
    EXPECT_GE(viewportChanges, 4);
}

TEST(ComparisonSurfacePropertyTests, ZoomToNormalizedRectFitsSelectionAndCentersView) {
    ComparisonSurface surface;
    surface.zoomToNormalizedRect(0.2, 0.3, 0.6, 0.7);
    // Box is 0.4 x 0.4 → scale 1/0.4 = 2.5, centered at (0.4, 0.5).
    EXPECT_DOUBLE_EQ(surface.viewScale(), 2.5);
    EXPECT_NEAR(surface.viewCenterX(), 0.4, 1e-6);
    EXPECT_NEAR(surface.viewCenterY(), 0.5, 1e-6);

    const qreal scaleBefore = surface.viewScale();
    surface.zoomToNormalizedRect(0.4, 0.4, 0.401, 0.401);
    EXPECT_DOUBLE_EQ(surface.viewScale(), scaleBefore);
}

class CountingActivitySink final : public platform::IRenderActivitySink {
public:
    void notifyFramePublished() noexcept override {
        frameNotifications.fetch_add(1U, std::memory_order_relaxed);
    }

    void notifyFrameRenderStarted() noexcept override {
        frameRenderStartedNotifications.fetch_add(1U, std::memory_order_relaxed);
    }

    void notifyAckPublished() noexcept override {
        acknowledgementNotifications.fetch_add(1U, std::memory_order_relaxed);
    }

    void notifyAckBackpressured() noexcept override {
        acknowledgementBackpressureNotifications.fetch_add(1U, std::memory_order_relaxed);
    }

    std::atomic<std::uint64_t> frameNotifications{0U};
    std::atomic<std::uint64_t> frameRenderStartedNotifications{0U};
    std::atomic<std::uint64_t> acknowledgementNotifications{0U};
    std::atomic<std::uint64_t> acknowledgementBackpressureNotifications{0U};
};

[[nodiscard]] application::FrameRequestContext
makeContext(const std::uint64_t requestId = 1U, const std::uint64_t playbackGeneration = 1U) {
    return application::FrameRequestContext{
        .playback =
            application::PlaybackRequestContext{
                .request =
                    application::RequestContext{
                        .sessionId = domain::SessionId{41U},
                        .sessionEpoch = domain::SessionEpoch{3U},
                        .requestId = domain::RequestId{requestId},
                    },
                .playbackGeneration = domain::PlaybackGeneration{playbackGeneration},
            },
        .deviceGeneration = domain::DeviceGeneration{1U},
    };
}

[[nodiscard]] std::optional<application::FrameHandle>
makeSolidFrame(platform::FrameResourceFactory& factory,
               const std::uint32_t width,
               const std::uint32_t height,
               const std::uint8_t y,
               const std::uint8_t u,
               const std::uint8_t v,
               const domain::ColorMetadata color) {
    const std::uint32_t uvRowBytes = ((width + 1U) / 2U) * 2U;
    const platform::Nv12FrameLayout layout{
        .width = width,
        .height = height,
        .yStride = width,
        .uvStride = uvRowBytes,
    };
    std::vector<std::uint8_t> yPlane(static_cast<std::size_t>(width) * height, y);
    std::vector<std::uint8_t> uvPlane(static_cast<std::size_t>(uvRowBytes) * ((height + 1U) / 2U));
    for (std::size_t index = 0U; index < uvPlane.size(); index += 2U) {
        uvPlane[index] = u;
        uvPlane[index + 1U] = v;
    }
    return factory.createCpuNv12(layout, color, std::span{yPlane}, std::span{uvPlane});
}

[[nodiscard]] std::optional<application::FrameHandle>
makeSolidP010Frame(platform::FrameResourceFactory& factory,
                   const std::uint32_t width,
                   const std::uint32_t height,
                   const std::uint16_t y,
                   const std::uint16_t u,
                   const std::uint16_t v,
                   const domain::ColorMetadata color) {
    const std::uint32_t yRowBytes = width * 2U;
    const std::uint32_t uvRowBytes = ((width + 1U) / 2U) * 4U;
    const platform::P010FrameLayout layout{
        .width = width,
        .height = height,
        .yStride = yRowBytes,
        .uvStride = uvRowBytes,
    };
    std::vector<std::uint8_t> yPlane(static_cast<std::size_t>(yRowBytes) * height);
    std::vector<std::uint8_t> uvPlane(static_cast<std::size_t>(uvRowBytes) * ((height + 1U) / 2U));
    const auto store =
        [](std::span<std::uint8_t> bytes, const std::size_t offset, const std::uint16_t code) {
            const std::uint16_t packed = static_cast<std::uint16_t>(code << 6U);
            bytes[offset] = static_cast<std::uint8_t>(packed & 0x00FFU);
            bytes[offset + 1U] = static_cast<std::uint8_t>(packed >> 8U);
        };
    for (std::size_t offset = 0U; offset < yPlane.size(); offset += 2U) {
        store(yPlane, offset, y);
    }
    for (std::size_t offset = 0U; offset < uvPlane.size(); offset += 4U) {
        store(uvPlane, offset, u);
        store(uvPlane, offset + 2U, v);
    }
    return factory.createCpuP010(layout, color, yPlane, uvPlane);
}

[[nodiscard]] std::optional<application::FrameSet>
makeSolidP010Set(platform::FrameBudget& budget,
                 const domain::FrameId frameId,
                 const std::uint16_t leftY,
                 const std::uint16_t rightY,
                 const domain::ColorMetadata metadata) {
    platform::FrameResourceFactory factory{budget};
    const std::optional<application::FrameHandle> left =
        makeSolidP010Frame(factory, 16U, 10U, leftY, 512U, 512U, metadata);
    const std::optional<application::FrameHandle> right =
        makeSolidP010Frame(factory, 16U, 10U, rightY, 512U, 512U, metadata);
    if (!left || !right) {
        return std::nullopt;
    }
    return application::FrameSet::create(
        frameId,
        domain::MediaTime{0},
        {
            application::MappedSourceFrame{
                .sourceId = 0U,
                .sourceFrameId = frameId,
                .frame = *left,
                .presentationTime = domain::MediaTime{0},
                .matchKind = application::FrameMatchKind::ExactIndex,
            },
            application::MappedSourceFrame{
                .sourceId = 1U,
                .sourceFrameId = frameId,
                .frame = *right,
                .presentationTime = domain::MediaTime{0},
                .matchKind = application::FrameMatchKind::ExactIndex,
            },
        });
}

[[nodiscard]] std::optional<application::FrameHandle>
makeHorizontalLumaFrame(platform::FrameResourceFactory& factory,
                        const std::span<const std::uint8_t> columns,
                        const std::uint32_t height,
                        const domain::ColorMetadata color) {
    if (columns.empty() || height == 0U ||
        columns.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::nullopt;
    }
    const std::uint32_t width = static_cast<std::uint32_t>(columns.size());
    const std::uint32_t uvRowBytes = ((width + 1U) / 2U) * 2U;
    const platform::Nv12FrameLayout layout{
        .width = width,
        .height = height,
        .yStride = width,
        .uvStride = uvRowBytes,
    };
    std::vector<std::uint8_t> yPlane(static_cast<std::size_t>(width) * height);
    for (std::uint32_t row = 0U; row < height; ++row) {
        std::copy(columns.begin(),
                  columns.end(),
                  yPlane.begin() + static_cast<std::ptrdiff_t>(row * width));
    }
    std::vector<std::uint8_t> uvPlane(static_cast<std::size_t>(uvRowBytes) * ((height + 1U) / 2U),
                                      128U);
    return factory.createCpuNv12(layout, color, std::span{yPlane}, std::span{uvPlane});
}

[[nodiscard]] std::optional<application::FrameSet>
makeHorizontalLumaSet(platform::FrameBudget& budget,
                      const domain::FrameId frameId,
                      const std::span<const std::uint8_t> columnsA,
                      const std::span<const std::uint8_t> columnsB,
                      const application::FramePresentation presentation = {}) {
    if (columnsA.size() != columnsB.size()) {
        return std::nullopt;
    }
    const domain::ColorMetadata metadata{
        .matrix = domain::ColorMatrix::kBt709,
        .range = domain::ColorRange::kFull,
        .matrixInferred = false,
    };
    platform::FrameResourceFactory factory{budget, presentation};
    const std::optional<application::FrameHandle> frameA =
        makeHorizontalLumaFrame(factory, columnsA, 8U, metadata);
    const std::optional<application::FrameHandle> frameB =
        makeHorizontalLumaFrame(factory, columnsB, 8U, metadata);
    if (!frameA.has_value() || !frameB.has_value()) {
        return std::nullopt;
    }
    return application::FrameSet::create(
        frameId,
        domain::MediaTime{0},
        {
            application::MappedSourceFrame{
                .sourceId = 0U,
                .sourceFrameId = domain::FrameId{frameId.value()},
                .frame = *frameA,
                .presentationTime = domain::MediaTime{0},
                .matchKind = application::FrameMatchKind::ExactIndex,
            },
            application::MappedSourceFrame{
                .sourceId = 1U,
                .sourceFrameId = domain::FrameId{frameId.value()},
                .frame = *frameB,
                .presentationTime = domain::MediaTime{0},
                .matchKind = application::FrameMatchKind::ExactIndex,
            },
        });
}

[[nodiscard]] std::optional<application::FrameSet>
makeSolidSetWithMetadata(platform::FrameBudget& budget,
                         const domain::FrameId frameId,
                         const std::uint8_t leftY,
                         const std::uint8_t leftU,
                         const std::uint8_t leftV,
                         const domain::ColorMetadata& leftColor,
                         const std::uint8_t rightY,
                         const std::uint8_t rightU,
                         const std::uint8_t rightV,
                         const domain::ColorMetadata& rightColor,
                         const application::FramePresentation presentation = {}) {
    platform::FrameResourceFactory factory{budget, presentation};
    const std::optional<application::FrameHandle> left =
        makeSolidFrame(factory, 16U, 9U, leftY, leftU, leftV, leftColor);
    const std::optional<application::FrameHandle> right =
        makeSolidFrame(factory, 12U, 9U, rightY, rightU, rightV, rightColor);
    if (!left.has_value() || !right.has_value()) {
        return std::nullopt;
    }
    return application::FrameSet::create(
        frameId,
        domain::MediaTime{0},
        {
            application::MappedSourceFrame{
                .sourceId = 0U,
                .sourceFrameId = domain::FrameId{frameId.value()},
                .frame = *left,
                .presentationTime = domain::MediaTime{0},
                .matchKind = application::FrameMatchKind::ExactIndex,
            },
            application::MappedSourceFrame{
                .sourceId = 1U,
                .sourceFrameId = domain::FrameId{frameId.value()},
                .frame = *right,
                .presentationTime = domain::MediaTime{0},
                .matchKind = application::FrameMatchKind::ExactIndex,
            },
        });
}

[[nodiscard]] std::optional<application::FrameSet> makeSolidSet(platform::FrameBudget& budget,
                                                                const domain::FrameId frameId,
                                                                const std::uint8_t leftY,
                                                                const std::uint8_t rightY) {
    return makeSolidSetWithMetadata(budget,
                                    frameId,
                                    leftY,
                                    128U,
                                    128U,
                                    domain::ColorMetadata{
                                        .matrix = domain::ColorMatrix::kBt601,
                                        .range = domain::ColorRange::kLimited,
                                        .matrixInferred = false,
                                    },
                                    rightY,
                                    128U,
                                    128U,
                                    domain::ColorMetadata{
                                        .matrix = domain::ColorMatrix::kBt709,
                                        .range = domain::ColorRange::kFull,
                                        .matrixInferred = false,
                                    });
}

[[nodiscard]] std::optional<application::FrameSet> makeSingleSolidSet(platform::FrameBudget& budget,
                                                                      const domain::FrameId frameId,
                                                                      const std::uint8_t luma) {
    const domain::ColorMetadata metadata{
        .matrix = domain::ColorMatrix::kBt709,
        .range = domain::ColorRange::kFull,
        .matrixInferred = false,
    };
    platform::FrameResourceFactory factory{budget};
    const std::optional<application::FrameHandle> frame =
        makeSolidFrame(factory, 16U, 9U, luma, 128U, 128U, metadata);
    if (!frame.has_value()) {
        return std::nullopt;
    }
    return application::FrameSet::create(frameId,
                                         domain::MediaTime{0},
                                         {application::MappedSourceFrame{
                                             .sourceId = 0U,
                                             .sourceFrameId = frameId,
                                             .frame = *frame,
                                             .presentationTime = domain::MediaTime{0},
                                             .matchKind = application::FrameMatchKind::ExactIndex,
                                         }});
}

[[nodiscard]] std::optional<application::FrameSet>
makeThreeSolidSet(platform::FrameBudget& budget,
                  const domain::FrameId frameId,
                  const std::array<std::uint8_t, 3U> lumaValues) {
    const domain::ColorMetadata metadata{
        .matrix = domain::ColorMatrix::kBt709,
        .range = domain::ColorRange::kFull,
        .matrixInferred = false,
    };
    platform::FrameResourceFactory factory{budget};
    std::array<std::optional<application::FrameHandle>, 3U> frames;
    for (std::size_t index = 0U; index < frames.size(); ++index) {
        frames[index] = makeSolidFrame(factory, 16U, 9U, lumaValues[index], 128U, 128U, metadata);
        if (!frames[index].has_value()) {
            return std::nullopt;
        }
    }
    std::vector<application::MappedSourceFrame> entries;
    entries.reserve(frames.size());
    for (std::size_t index = 0U; index < frames.size(); ++index) {
        entries.push_back(application::MappedSourceFrame{
            .sourceId = static_cast<domain::SourceId>(index),
            .sourceFrameId = frameId,
            .frame = *frames[index],
            .presentationTime = domain::MediaTime{0},
            .matchKind = application::FrameMatchKind::ExactIndex,
        });
    }
    return application::FrameSet::create(frameId, domain::MediaTime{0}, std::move(entries));
}

[[nodiscard]] std::optional<application::FrameSet>
makeThreeSolidSetWithMissingMiddle(platform::FrameBudget& budget, const domain::FrameId frameId) {
    const domain::ColorMetadata metadata{
        .matrix = domain::ColorMatrix::kBt709,
        .range = domain::ColorRange::kFull,
        .matrixInferred = false,
    };
    platform::FrameResourceFactory factory{budget};
    std::optional<application::FrameHandle> first =
        makeSolidFrame(factory, 16U, 9U, 64U, 128U, 128U, metadata);
    std::optional<application::FrameHandle> third =
        makeSolidFrame(factory, 16U, 9U, 224U, 128U, 128U, metadata);
    if (!first.has_value() || !third.has_value()) {
        return std::nullopt;
    }
    return application::FrameSet::create(
        frameId,
        domain::MediaTime{0},
        {
            application::MappedSourceFrame{
                .sourceId = 0U,
                .sourceFrameId = frameId,
                .frame = *first,
                .presentationTime = domain::MediaTime{0},
                .matchKind = application::FrameMatchKind::ExactIndex,
            },
            application::MappedSourceFrame{
                .sourceId = 1U,
                .sourceFrameId = std::nullopt,
                .frame = std::nullopt,
                .presentationTime = domain::MediaTime{0},
                .matchKind = application::FrameMatchKind::Missing,
                .missingReason = application::MissingReason::AlignmentGap,
            },
            application::MappedSourceFrame{
                .sourceId = 2U,
                .sourceFrameId = frameId,
                .frame = *third,
                .presentationTime = domain::MediaTime{0},
                .matchKind = application::FrameMatchKind::ExactIndex,
            },
        });
}

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate predicate, const std::chrono::milliseconds timeout) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout.count()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::yieldCurrentThread();
    }
    return predicate();
}

class SurfaceWarpHarness final {
public:
    SurfaceWarpHarness()
        : mailbox(std::make_shared<platform::FrameMailbox>(domain::DeviceGeneration{1U})),
          acknowledgementMailbox(std::make_shared<platform::PresentationAckMailbox>()),
          activitySink(std::make_shared<CountingActivitySink>()), surface(window.contentItem()) {
        QQuickGraphicsConfiguration configuration;
        configuration.setPreferSoftwareDevice(true);
        configuration.setDepthBufferFor2D(true);
        window.setGraphicsConfiguration(configuration);
        window.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        window.setColor(Qt::magenta);
        window.setGeometry(100, 100, 101, 64);
        surface.setSize(QSizeF{101.0, 64.0});
        attached =
            surface.attachRendererServices(broker, mailbox, acknowledgementMailbox, activitySink);
    }

    ~SurfaceWarpHarness() {
        releaseRenderer();
        window.hide();
    }

    [[nodiscard]] bool start() {
        if (!attached) {
            return false;
        }
        window.show();
        if (!waitUntil([this] { return window.isExposed(); }, 10s)) {
            return false;
        }
        if (QScreen* const screen = QGuiApplication::primaryScreen()) {
            const QRect available = screen->availableGeometry();
            window.setPosition(available.right() - window.width() + 1,
                               available.bottom() - window.height() + 1);
        }
        surface.update();
        window.requestUpdate();
        const bool ready = waitUntil(
            [this] { return broker->currentGeneration() == domain::DeviceGeneration{1U}; }, 10s);
        if (ready) {
            static_cast<void>(broker->tryConsumeNotification());
        }
        return ready;
    }

    void requestRender() {
        surface.update();
        window.requestUpdate();
    }

    [[nodiscard]] QImage grab() {
        requestRender();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        // A QQuickRenderNode synchronizes GUI-thread properties on the next scene-graph frame.
        // Consume that frame before capturing so a setter immediately followed by grab() cannot
        // observe the previous Wipe/ROI state.
        static_cast<void>(window.grabWindow());
        requestRender();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        return window.grabWindow();
    }

    [[nodiscard]] bool ensureOddPhysicalWidth() {
        constexpr int firstLogicalWidth = 101;
        constexpr int lastLogicalWidth = 109;
        for (int logicalWidth = firstLogicalWidth; logicalWidth <= lastLogicalWidth;
             ++logicalWidth) {
            window.resize(logicalWidth, window.height());
            surface.setWidth(static_cast<qreal>(logicalWidth));
            const QImage image = grab();
            if (!image.isNull() && (image.width() % 2) == 1) {
                return true;
            }
        }
        return false;
    }

    void releaseRenderer() {
        if (!attached) {
            return;
        }
        surface.detachRendererServices();
        attached = false;
        surface.update();
        window.requestUpdate();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (window.isExposed()) {
            // grabWindow() is a blocking scene-graph sync/render barrier. With services detached,
            // updatePaintNode() deletes the render node and releases its pinned front pair.
            static_cast<void>(window.grabWindow());
        }
        window.releaseResources();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }

    std::shared_ptr<platform::GraphicsDeviceBroker> broker =
        std::make_shared<platform::GraphicsDeviceBroker>();
    std::shared_ptr<platform::FrameMailbox> mailbox;
    std::shared_ptr<platform::PresentationAckMailbox> acknowledgementMailbox;
    std::shared_ptr<CountingActivitySink> activitySink;
    QQuickWindow window;
    ComparisonSurface surface;
    bool attached = false;
};

[[nodiscard]] application::FrameSetPresented
makeDummyAcknowledgement(const std::uint64_t requestId) {
    return application::FrameSetPresented{
        .context = makeContext(requestId),
        .frameId = domain::FrameId{static_cast<std::int64_t>(requestId)},
    };
}

TEST(ComparisonSurfaceWarpTests, RetriesContendedRenderWithoutAnotherPublication) {
    SurfaceWarpHarness harness;
    ASSERT_TRUE(harness.start());
    static_cast<void>(harness.window.grabWindow());

    std::atomic<int> attempts{0};
    QObject::connect(
        &harness.window,
        &QQuickWindow::afterRendering,
        &harness.window,
        [&] {
            if (attempts.fetch_add(1, std::memory_order_relaxed) == 0) {
                detail::RenderRetry retry;
                retry.retryContendedRender(harness.window,
                                           platform::ComparisonRenderResult::Contended);
            }
        },
        Qt::DirectConnection);
    harness.requestRender();
    EXPECT_TRUE(waitUntil([&] { return attempts.load(std::memory_order_relaxed) >= 2; }, 2s));
    QObject::disconnect(&harness.window, nullptr, &harness.window, nullptr);
}

TEST(ComparisonSurfaceWarpTests, RendersUnequalAspectNv12AcrossAnOddSplitWithoutABlackSeam) {
    SurfaceWarpHarness harness;
    ASSERT_TRUE(harness.start());
    if (!harness.ensureOddPhysicalWidth()) {
        GTEST_SKIP() << "The active screen scale cannot produce an odd physical window width.";
    }

    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    const application::FrameRequestContext context = makeContext();
    std::optional<application::FrameSet> pair =
        makeSolidSet(*budget, domain::FrameId{7}, 235U, 128U);
    ASSERT_TRUE(pair.has_value());
    ASSERT_EQ(actor.submit(context, std::move(*pair)), platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const QImage image = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(image.isNull());
    ASSERT_GE(image.width(), 3);
    ASSERT_GE(image.height(), 3);
    ASSERT_EQ(image.width() % 2, 1);
    const int split = image.width() / 2;
    const QColor left = image.pixelColor(split - 1, image.height() / 2);
    const QColor right = image.pixelColor(split, image.height() / 2);
    EXPECT_GT(left.red(), 240);
    EXPECT_GT(left.green(), 240);
    EXPECT_GT(left.blue(), 240);
    EXPECT_GT(right.red(), 115);
    EXPECT_LT(right.red(), 140);
    EXPECT_GT(right.green(), 115);
    EXPECT_LT(right.green(), 140);
    EXPECT_GT(right.blue(), 115);
    EXPECT_LT(right.blue(), 140);

    const std::optional<application::FrameSetPresented> acknowledgement =
        harness.acknowledgementMailbox->tryPop();
    ASSERT_TRUE(acknowledgement.has_value());
    EXPECT_EQ(acknowledgement->frameId, domain::FrameId{7});
    EXPECT_EQ(harness.activitySink->acknowledgementNotifications.load(std::memory_order_relaxed),
              1U);

    ASSERT_TRUE(harness.mailbox->clear(context.playback));
    const QImage afterClear = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(afterClear.isNull());
    expectColorNear(afterClear.pixelColor(split - 1, afterClear.height() / 2), left, 1);
    expectColorNear(afterClear.pixelColor(split, afterClear.height() / 2), right, 1);
    EXPECT_FALSE(harness.acknowledgementMailbox->tryPop().has_value());

    harness.surface.setOpacity(0.5);
    const QImage translucent = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(translucent.isNull());
    // Video and its non-overlapping letterbox bars each blend exactly once over magenta.
    expectColorNear(translucent.pixelColor(translucent.width() / 4, translucent.height() / 2),
                    QColor{255, 128, 255},
                    2);
    expectColorNear(translucent.pixelColor(translucent.width() / 4, 1), QColor{128, 0, 128}, 2);

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

// The pure-QML popup suite cannot catch a native video draw overwriting opaque QML batches.
// Keep the production dialog above a real ComparisonSurface and sample RGB, not window alpha:
// both the white video and the popup have alpha 255 even when the chrome is missing.
TEST(ComparisonSurfaceWarpTests, DropConfirmationChromeStaysOpaqueOverVideo) {
    ASSERT_TRUE(qgetenv("QSG_NO_DEPTH_BUFFER").isEmpty())
        << "This regression requires Qt's depth-buffer batching; unset QSG_NO_DEPTH_BUFFER.";
    SurfaceWarpHarness harness;
    harness.window.resize(720, 520);
    harness.surface.setSize(QSizeF{720.0, 520.0});
    harness.surface.setViewMode(ComparisonSurface::Single);
    struct DepthAttachmentProbe final {
        std::atomic<int> bound{0};
        std::atomic<int> missing{0};
        std::atomic<int> noLease{0};
    };
    const auto depthProbe = std::make_shared<DepthAttachmentProbe>();
    QObject probeContext;
    // In this one-pass fixture the raw ComparisonRenderNode has flushed beginExternal before
    // this signal. Read its bound DSV before endPass; do not assert from the render thread.
    // The context disconnects on every exit, while shared captures outlive any in-flight call.
    QObject::connect(
        &harness.window,
        &QQuickWindow::afterRenderPassRecording,
        &probeContext,
        [broker = harness.broker, depthProbe] {
            const platform::GraphicsDeviceLeaseResult result = broker->tryLease();
            if (!result.lease.has_value()) {
                depthProbe->noLease.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthView;
            result.lease->immediateContext->OMGetRenderTargets(
                0U, nullptr, depthView.GetAddressOf());
            if (depthView) {
                depthProbe->bound.fetch_add(1, std::memory_order_relaxed);
            } else {
                depthProbe->missing.fetch_add(1, std::memory_order_relaxed);
            }
        },
        Qt::DirectConnection);
    ASSERT_TRUE(harness.start());

    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> pair =
        makeSolidSet(*budget, domain::FrameId{37}, 235U, 235U);
    ASSERT_TRUE(pair.has_value());
    ASSERT_EQ(actor.submit(makeContext(37U), std::move(*pair)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));
    const QImage video = harness.grab();
    ASSERT_FALSE(video.isNull());
    ASSERT_GT(depthProbe->bound.load(), 0) << "No observed native depth attachment; unavailable "
                                              "leases: "
                                           << depthProbe->noLease.load();
    ASSERT_EQ(depthProbe->missing.load(), 0);
    RecordProperty("device_pixel_ratio",
                   QString::number(harness.window.devicePixelRatio()).toStdString());
    RecordProperty("qt_scale_factor", qgetenv("QT_SCALE_FACTOR").toStdString());
    RecordProperty("grab_width", video.width());
    RecordProperty("grab_height", video.height());
    qInfo().nospace() << "DropConfirmation native capture: effectiveDpr="
                      << harness.window.devicePixelRatio()
                      << " qtScaleFactor=" << qgetenv("QT_SCALE_FACTOR")
                      << " logical=" << harness.window.size() << " pixels=" << video.size();
    expectColorNear(video.pixelColor(video.width() / 4, video.height() / 2), Qt::white, 3);

    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    QQmlComponent component{&engine};
    component.setData(R"QML(
import QtQuick
import "."
Item {
    DropConfirmationDialog {
        objectName: "nativeDropDialog"
        pendingVideos: ["file:///C:/one.mp4", "file:///C:/two.mp4"]
        fileNameFunction: function(url) { return url.toString().split("/").pop(); }
        pathNameFunction: function(url) { return "Videos"; }
    }
}
)QML",
                      QUrl::fromLocalFile(QDir{QStringLiteral(DVS_QML_SOURCE_DIR)}.filePath(
                          QStringLiteral("NativeDropDialogProbe.qml"))));
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << component.errorString().toStdString();
    std::unique_ptr<QObject> holder{component.beginCreate(engine.rootContext())};
    auto* const host = qobject_cast<QQuickItem*>(holder.get());
    ASSERT_NE(host, nullptr);
    host->setParentItem(harness.window.contentItem());
    host->setSize(QSizeF{720.0, 520.0});
    component.completeCreate();
    ASSERT_FALSE(component.isError()) << component.errorString().toStdString();
    QObject* const dialog = host->findChild<QObject*>(QStringLiteral("nativeDropDialog"));
    ASSERT_NE(dialog, nullptr);

    // Exercise both source counts and repeated close/reopen with an already presented video.
    std::size_t checkedSamples = 0U;
    for (int sourceCount : {2, 3, 2}) {
        QVariantList sources{QStringLiteral("file:///C:/one.mp4"),
                             QStringLiteral("file:///C:/two.mp4")};
        if (sourceCount == 3) {
            sources.push_back(QStringLiteral("file:///C:/three.mp4"));
        }
        ASSERT_TRUE(dialog->setProperty("pendingVideos", sources));
        ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "open"));
        ASSERT_TRUE(waitUntil([&] { return dialog->property("opened").toBool(); }, 2s));
        const int previousDepthProbes = depthProbe->bound.load();
        const QImage opened = harness.grab().convertToFormat(QImage::Format_RGBA8888);
        ASSERT_FALSE(opened.isNull());
        ASSERT_GT(depthProbe->bound.load(), previousDepthProbes)
            << "The popup capture did not observe a bound native depth attachment.";
        ASSERT_EQ(depthProbe->missing.load(), 0);
        ASSERT_EQ(opened.size(), video.size());
        auto* const background =
            dialog->findChild<QQuickItem*>(QStringLiteral("dropDialogBackground"));
        auto* const header = dialog->findChild<QQuickItem*>(QStringLiteral("dropDialogHeader"));
        auto* const footer = dialog->findChild<QQuickItem*>(QStringLiteral("dropDialogFooter"));
        ASSERT_NE(background, nullptr);
        ASSERT_NE(header, nullptr);
        ASSERT_NE(footer, nullptr);
        ASSERT_GT(background->height(), 200.0);
        const QColor bodyColor = background->property("color").value<QColor>();
        const QColor headerColor = header->property("color").value<QColor>();
        const QColor footerColor = footer->property("color").value<QColor>();
        ASSERT_EQ(bodyColor.alpha(), 255);
        ASSERT_EQ(headerColor.alpha(), 255);
        ASSERT_EQ(footerColor.alpha(), 255);
        const qreal scaleX = static_cast<qreal>(opened.width()) / harness.window.width();
        const qreal scaleY = static_cast<qreal>(opened.height()) / harness.window.height();
        ASSERT_NEAR(scaleX, harness.window.devicePixelRatio(), 1.0 / harness.window.width());
        ASSERT_NEAR(scaleY, harness.window.devicePixelRatio(), 1.0 / harness.window.height());
        const auto sampleChrome =
            [&](QQuickItem* item, const qreal x, const qreal y, const QColor& expected) {
                const QPointF point = item->mapToScene(QPointF{x, y});
                const QPoint pixel{static_cast<int>(std::floor(point.x() * scaleX)),
                                   static_cast<int>(std::floor(point.y() * scaleY))};
                ASSERT_TRUE(opened.rect().contains(pixel));
                // Prove each sampled location had a decoded frame, not a black letterbox.
                expectColorNear(video.pixelColor(pixel), Qt::white, 3);
                expectColorNear(opened.pixelColor(pixel), expected, 3);
                ++checkedSamples;
            };
        for (const qreal x : {24.0, header->width() / 2.0, header->width() - 24.0}) {
            for (const int delta : {-6, -2, 2, 6}) {
                SCOPED_TRACE(testing::Message()
                             << "sources=" << sourceCount << " x=" << x << " delta=" << delta);
                sampleChrome(
                    header, x, header->height() + delta, delta < 0 ? headerColor : bodyColor);
                sampleChrome(footer, x, delta, delta < 0 ? bodyColor : footerColor);
            }
        }
        ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "close"));
        ASSERT_TRUE(waitUntil([&] { return !dialog->property("visible").toBool(); }, 2s));
        const QImage dismissed = harness.grab();
        ASSERT_FALSE(dismissed.isNull());
        expectColorNear(
            dismissed.pixelColor(dismissed.width() / 4, dismissed.height() / 2), Qt::white, 3);
    }
    EXPECT_EQ(checkedSamples, 72U);
    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, CoversTheEntireBackgroundWithBlack) {
    SurfaceWarpHarness harness;
    ASSERT_TRUE(harness.start());

    const QImage image = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(image.isNull());
    const std::array<QPoint, 5U> samples{
        QPoint{1, 1},
        QPoint{image.width() - 2, 1},
        QPoint{1, image.height() - 2},
        QPoint{image.width() - 2, image.height() - 2},
        QPoint{image.width() / 2, image.height() / 2},
    };
    for (const QPoint& sample : samples) {
        expectColorNear(image.pixelColor(sample), QColor{0, 0, 0}, 1);
    }
    harness.releaseRenderer();
}

TEST(ComparisonSurfaceWarpTests, UploadsAndRendersP010WithoutReducingItToEightBit) {
    SurfaceWarpHarness harness;
    ASSERT_TRUE(harness.start());
    const domain::ColorMetadata metadata{
        .matrix = domain::ColorMatrix::kBt709,
        .range = domain::ColorRange::kFull,
        .matrixInferred = false,
    };
    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> pair =
        makeSolidP010Set(*budget, domain::FrameId{8}, 257U, 769U, metadata);
    ASSERT_TRUE(pair.has_value());
    ASSERT_EQ(actor.submit(makeContext(8U), std::move(*pair)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const QImage image = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(image.isNull());
    expectColorNear(image.pixelColor(image.width() / 4, image.height() / 2),
                    expectedP010Color(metadata, 257U, 512U, 512U),
                    2);
    expectColorNear(image.pixelColor(image.width() * 3 / 4, image.height() / 2),
                    expectedP010Color(metadata, 769U, 512U, 512U),
                    2);

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, AppliesRotationBeforeAspectFitAndSampling) {
    SurfaceWarpHarness harness;
    ASSERT_TRUE(harness.start());
    constexpr std::array<std::uint8_t, 16U> kColumns{
        16U,
        16U,
        16U,
        16U,
        16U,
        16U,
        16U,
        16U,
        235U,
        235U,
        235U,
        235U,
        235U,
        235U,
        235U,
        235U,
    };
    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> pair =
        makeHorizontalLumaSet(*budget,
                              domain::FrameId{9},
                              kColumns,
                              kColumns,
                              application::FramePresentation{.rotationDegrees = 90U});
    ASSERT_TRUE(pair.has_value());
    ASSERT_EQ(actor.submit(makeContext(9U), std::move(*pair)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const QImage image = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(image.isNull());
    const int leftCenter = image.width() / 4;
    const QColor upper = image.pixelColor(leftCenter, image.height() / 4);
    const QColor lower = image.pixelColor(leftCenter, image.height() * 3 / 4);
    EXPECT_GT(upper.red(), 225);
    EXPECT_LT(lower.red(), 25);
    // 16x8 rotated content is portrait and therefore leaves horizontal letterbox bars in the
    // wider half-panel.
    expectColorNear(image.pixelColor(2, image.height() / 2), QColor{0, 0, 0}, 2);

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, UsesSampleAspectRatioForDisplayGeometry) {
    SurfaceWarpHarness harness;
    ASSERT_TRUE(harness.start());
    const domain::ColorMetadata metadata{
        .matrix = domain::ColorMatrix::kBt709,
        .range = domain::ColorRange::kFull,
        .matrixInferred = false,
    };
    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> pair =
        makeSolidSetWithMetadata(*budget,
                                 domain::FrameId{10},
                                 235U,
                                 128U,
                                 128U,
                                 metadata,
                                 235U,
                                 128U,
                                 128U,
                                 metadata,
                                 application::FramePresentation{
                                     .sampleAspectNumerator = 2U,
                                     .sampleAspectDenominator = 1U,
                                 });
    ASSERT_TRUE(pair.has_value());
    ASSERT_EQ(actor.submit(makeContext(10U), std::move(*pair)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const QImage image = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(image.isNull());
    const int centerX = image.width() / 4;
    const int centerY = image.height() / 2;
    EXPECT_GT(image.pixelColor(centerX, centerY).red(), 225);
    expectColorNear(image.pixelColor(centerX, centerY - (image.height() / 4)), QColor{0, 0, 0}, 2);

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, AppliesEveryMatrixAndRangeCombinationInThePixelShader) {
    SurfaceWarpHarness harness;
    ASSERT_TRUE(harness.start());

    constexpr std::uint8_t y = 128U;
    constexpr std::uint8_t u = 96U;
    constexpr std::uint8_t v = 160U;
    const domain::ColorMetadata bt601Full{
        .matrix = domain::ColorMatrix::kBt601,
        .range = domain::ColorRange::kFull,
        .matrixInferred = false,
    };
    const domain::ColorMetadata bt709Limited{
        .matrix = domain::ColorMatrix::kBt709,
        .range = domain::ColorRange::kLimited,
        .matrixInferred = false,
    };
    const domain::ColorMetadata bt601Limited{
        .matrix = domain::ColorMatrix::kBt601,
        .range = domain::ColorRange::kLimited,
        .matrixInferred = false,
    };
    const domain::ColorMetadata bt709Full{
        .matrix = domain::ColorMatrix::kBt709,
        .range = domain::ColorRange::kFull,
        .matrixInferred = false,
    };

    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    const auto verifyPair = [&](const std::uint64_t requestId,
                                const domain::FrameId frameId,
                                const domain::ColorMetadata& leftMetadata,
                                const domain::ColorMetadata& rightMetadata) {
        std::optional<application::FrameSet> pair = makeSolidSetWithMetadata(
            *budget, frameId, y, u, v, leftMetadata, y, u, v, rightMetadata);
        ASSERT_TRUE(pair.has_value());
        ASSERT_EQ(actor.submit(makeContext(requestId), std::move(*pair)),
                  platform::GpuTransferSubmitResult::Accepted);
        ASSERT_TRUE(actor.waitUntilIdle(5s));

        const QImage image = harness.grab().convertToFormat(QImage::Format_RGBA8888);
        ASSERT_FALSE(image.isNull());
        expectColorNear(image.pixelColor(image.width() / 4, image.height() / 2),
                        expectedNv12Color(leftMetadata, y, u, v));
        expectColorNear(image.pixelColor(image.width() * 3 / 4, image.height() / 2),
                        expectedNv12Color(rightMetadata, y, u, v));
        const std::optional<application::FrameSetPresented> acknowledgement =
            harness.acknowledgementMailbox->tryPop();
        ASSERT_TRUE(acknowledgement.has_value());
        EXPECT_EQ(acknowledgement->frameId, frameId);
    };

    verifyPair(20U, domain::FrameId{20}, bt601Full, bt709Limited);
    verifyPair(21U, domain::FrameId{21}, bt601Limited, bt709Full);
    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests,
     RendersEveryDifferenceMetricGainAndFilterWithoutDuplicateAcknowledgement) {
    SurfaceWarpHarness harness;
    harness.surface.setViewMode(ComparisonSurface::Difference);
    ASSERT_TRUE(harness.start());

    constexpr std::array<std::uint8_t, 3U> sampleA{128U, 64U, 192U};
    constexpr std::array<std::uint8_t, 3U> sampleB{96U, 160U, 80U};
    const domain::ColorMetadata metadataA{
        .matrix = domain::ColorMatrix::kBt601,
        .range = domain::ColorRange::kFull,
        .matrixInferred = false,
    };
    const domain::ColorMetadata metadataB{
        .matrix = domain::ColorMatrix::kBt709,
        .range = domain::ColorRange::kLimited,
        .matrixInferred = false,
    };

    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> pair = makeSolidSetWithMetadata(*budget,
                                                                         domain::FrameId{30},
                                                                         sampleA[0U],
                                                                         sampleA[1U],
                                                                         sampleA[2U],
                                                                         metadataA,
                                                                         sampleB[0U],
                                                                         sampleB[1U],
                                                                         sampleB[2U],
                                                                         metadataB);
    ASSERT_TRUE(pair.has_value());
    ASSERT_EQ(actor.submit(makeContext(30U), std::move(*pair)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const QImage first = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(first.isNull());
    const QPoint samplePoint{first.width() / 2, first.height() / 2};
    expectColorNear(first.pixelColor(samplePoint),
                    expectedDifferenceColor(metadataA,
                                            sampleA,
                                            metadataB,
                                            sampleB,
                                            ComparisonSurface::RgbAbsolute,
                                            ComparisonSurface::Gain1x),
                    4);
    const std::optional<application::FrameSetPresented> acknowledgement =
        harness.acknowledgementMailbox->tryPop();
    ASSERT_TRUE(acknowledgement.has_value());
    EXPECT_EQ(acknowledgement->frameId, domain::FrameId{30});

    constexpr std::array metrics{
        ComparisonSurface::RgbAbsolute,
        ComparisonSurface::Luma,
        ComparisonSurface::Chroma,
        ComparisonSurface::Heatmap,
    };
    constexpr std::array gains{
        ComparisonSurface::Gain1x,
        ComparisonSurface::Gain2x,
        ComparisonSurface::Gain4x,
        ComparisonSurface::Gain8x,
        ComparisonSurface::Gain16x,
    };
    for (const ComparisonSurface::DifferenceMetric metric : metrics) {
        harness.surface.setDifferenceMetric(metric);
        for (const ComparisonSurface::DifferenceGain gain : gains) {
            harness.surface.setDifferenceGain(gain);
            const QImage image = harness.grab().convertToFormat(QImage::Format_RGBA8888);
            ASSERT_FALSE(image.isNull());
            expectColorNear(
                image.pixelColor(image.width() / 2, image.height() / 2),
                expectedDifferenceColor(metadataA, sampleA, metadataB, sampleB, metric, gain),
                5);
        }
    }

    harness.surface.setDifferenceMetric(ComparisonSurface::RgbAbsolute);
    harness.surface.setDifferenceGain(ComparisonSurface::Gain1x);
    constexpr std::array filters{
        ComparisonSurface::Nearest,
        ComparisonSurface::Bilinear,
        ComparisonSurface::Bicubic,
    };
    for (const ComparisonSurface::DifferenceFilter filter : filters) {
        harness.surface.setDifferenceFilter(filter);
        const QImage image = harness.grab().convertToFormat(QImage::Format_RGBA8888);
        ASSERT_FALSE(image.isNull());
        expectColorNear(image.pixelColor(image.width() / 2, image.height() / 2),
                        expectedDifferenceColor(metadataA,
                                                sampleA,
                                                metadataB,
                                                sampleB,
                                                ComparisonSurface::RgbAbsolute,
                                                ComparisonSurface::Gain1x),
                        5);
    }

    const QColor opaqueDifference = expectedDifferenceColor(metadataA,
                                                            sampleA,
                                                            metadataB,
                                                            sampleB,
                                                            ComparisonSurface::RgbAbsolute,
                                                            ComparisonSurface::Gain1x);
    harness.surface.setOpacity(0.5);
    const QImage translucent = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(translucent.isNull());
    expectColorNear(translucent.pixelColor(translucent.width() / 2, translucent.height() / 2),
                    QColor{(opaqueDifference.red() + 255) / 2,
                           opaqueDifference.green() / 2,
                           (opaqueDifference.blue() + 255) / 2},
                    4);

    EXPECT_FALSE(harness.acknowledgementMailbox->tryPop().has_value());
    EXPECT_EQ(harness.activitySink->acknowledgementNotifications.load(std::memory_order_relaxed),
              1U);
    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, UsesTheFirstEdgeSourceAsCanvasForUnequalExtents) {
    SurfaceWarpHarness harness;
    harness.surface.setViewMode(ComparisonSurface::Difference);
    harness.surface.setDifferenceGain(ComparisonSurface::Gain4x);
    ASSERT_TRUE(harness.start());

    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> pair =
        makeSolidSet(*budget, domain::FrameId{31}, 235U, 64U);
    ASSERT_TRUE(pair.has_value());
    ASSERT_EQ(actor.submit(makeContext(31U), std::move(*pair)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const QImage referenceA = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(referenceA.isNull());
    const QColor topA = referenceA.pixelColor(referenceA.width() / 2, 1);
    const QColor leftA = referenceA.pixelColor(1, referenceA.height() / 2);
    expectColorNear(topA, QColor{0, 0, 0}, 2);
    EXPECT_GT(leftA.red() + leftA.green() + leftA.blue(), 20);
    ASSERT_TRUE(harness.acknowledgementMailbox->tryPop().has_value());

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, RendersThreeSourcesAndSelectedPairwiseDifferenceEdges) {
    SurfaceWarpHarness harness;
    harness.surface.setViewMode(ComparisonSurface::ThreeUp);
    ASSERT_TRUE(harness.start());

    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> set =
        makeThreeSolidSet(*budget, domain::FrameId{40}, {32U, 64U, 224U});
    ASSERT_TRUE(set.has_value());
    ASSERT_EQ(actor.submit(makeContext(40U), std::move(*set)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const QImage threeUp = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(threeUp.isNull());
    const int middleY = threeUp.height() / 2;
    const QColor first = threeUp.pixelColor(threeUp.width() / 6, middleY);
    const QColor second = threeUp.pixelColor(threeUp.width() / 2, middleY);
    const QColor third = threeUp.pixelColor(threeUp.width() * 5 / 6, middleY);
    EXPECT_LT(first.red(), second.red())
        << first.name().toStdString() << " vs " << second.name().toStdString();
    EXPECT_LT(second.red(), third.red())
        << second.name().toStdString() << " vs " << third.name().toStdString() << ", image width "
        << threeUp.width();
    ASSERT_TRUE(harness.acknowledgementMailbox->tryPop().has_value());

    harness.surface.setViewMode(ComparisonSurface::Difference);
    harness.surface.setDifferenceEdge(ComparisonSurface::Edge0And1);
    const QImage smallDifference = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(smallDifference.isNull());
    harness.surface.setDifferenceEdge(ComparisonSurface::Edge0And2);
    const QImage largeDifference = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(largeDifference.isNull());
    EXPECT_LT(
        smallDifference.pixelColor(smallDifference.width() / 2, smallDifference.height() / 2).red(),
        largeDifference.pixelColor(largeDifference.width() / 2, largeDifference.height() / 2)
            .red());
    EXPECT_FALSE(harness.acknowledgementMailbox->tryPop().has_value());

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, WipeUsesTheSelectedPairAndDraggableSplitPosition) {
    SurfaceWarpHarness harness;
    harness.surface.setViewMode(ComparisonSurface::Wipe);
    harness.surface.setDifferenceEdge(ComparisonSurface::Edge0And2);
    harness.surface.setWipePosition(0.25);
    ASSERT_TRUE(harness.start());

    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> set =
        makeThreeSolidSet(*budget, domain::FrameId{41}, {32U, 96U, 224U});
    ASSERT_TRUE(set.has_value());
    ASSERT_EQ(actor.submit(makeContext(41U), std::move(*set)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const QImage image = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(image.isNull());
    const int middleY = image.height() / 2;
    const QColor left = image.pixelColor(image.width() / 8, middleY);
    const QColor right = image.pixelColor(image.width() * 5 / 8, middleY);
    EXPECT_LT(left.red(), right.red());
    EXPECT_LT(left.green(), right.green());
    EXPECT_LT(left.blue(), right.blue());
    EXPECT_TRUE(harness.acknowledgementMailbox->tryPop().has_value());

    for (const ComparisonSurface::DifferenceEdge edge : {ComparisonSurface::Edge0And1,
                                                         ComparisonSurface::Edge0And2,
                                                         ComparisonSurface::Edge1And2}) {
        harness.surface.setDifferenceEdge(edge);
        harness.surface.setWipePosition(0.5);
        const QImage pairImage = harness.grab().convertToFormat(QImage::Format_RGBA8888);
        ASSERT_FALSE(pairImage.isNull());
        const QColor pairLeft = pairImage.pixelColor(pairImage.width() / 4, pairImage.height() / 2);
        const QColor pairRight =
            pairImage.pixelColor(pairImage.width() * 3 / 4, pairImage.height() / 2);
        EXPECT_LT(pairLeft.red(), pairRight.red());
        EXPECT_LT(pairLeft.green(), pairRight.green());
        EXPECT_LT(pairLeft.blue(), pairRight.blue());
    }

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, WipeLeavesOnlyTheUnavailableSideBlack) {
    SurfaceWarpHarness harness;
    harness.surface.setViewMode(ComparisonSurface::Wipe);
    harness.surface.setDifferenceEdge(ComparisonSurface::Edge0And1);
    harness.surface.setWipePosition(0.5);
    ASSERT_TRUE(harness.start());

    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> set =
        makeThreeSolidSetWithMissingMiddle(*budget, domain::FrameId{43});
    ASSERT_TRUE(set.has_value());
    ASSERT_EQ(actor.submit(makeContext(43U), std::move(*set)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const QImage image = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(image.isNull());
    const int middleY = image.height() / 2;
    const QColor available = image.pixelColor(image.width() / 4, middleY);
    const QColor unavailable = image.pixelColor(image.width() * 3 / 4, middleY);
    EXPECT_GT(available.red() + available.green() + available.blue(), 20);
    expectColorNear(unavailable, QColor{0, 0, 0}, 2);
    EXPECT_TRUE(harness.acknowledgementMailbox->tryPop().has_value());

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, WipeSplitUsesSurfaceCoordinatesAcrossLetterboxing) {
    SurfaceWarpHarness harness;
    harness.surface.setViewMode(ComparisonSurface::Wipe);
    harness.surface.setDifferenceEdge(ComparisonSurface::Edge0And1);
    ASSERT_TRUE(harness.start());

    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    const domain::ColorMetadata metadata{
        .matrix = domain::ColorMatrix::kBt709,
        .range = domain::ColorRange::kFull,
        .matrixInferred = false,
    };
    std::optional<application::FrameSet> set =
        makeSolidSetWithMetadata(*budget,
                                 domain::FrameId{42},
                                 32U,
                                 128U,
                                 128U,
                                 metadata,
                                 224U,
                                 128U,
                                 128U,
                                 metadata,
                                 application::FramePresentation{.rotationDegrees = 90});
    ASSERT_TRUE(set.has_value());
    ASSERT_EQ(actor.submit(makeContext(42U), std::move(*set)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    harness.surface.setWipePosition(0.25);
    const QImage rightImage = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(rightImage.isNull());
    harness.surface.setWipePosition(0.75);
    const QImage leftImage = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(leftImage.isNull());

    const int sampleX = leftImage.width() * 13 / 20;
    const int sampleY = leftImage.height() / 2;
    const QColor right = rightImage.pixelColor(sampleX, sampleY);
    const QColor left = leftImage.pixelColor(sampleX, sampleY);
    EXPECT_LT(left.red(), right.red());
    EXPECT_LT(left.green(), right.green());
    EXPECT_LT(left.blue(), right.blue());

    harness.surface.zoomAt(0.5, 0.5, 2.0);
    harness.surface.setRoiNormalized(0.1, 0.1, 0.9, 0.9);
    harness.surface.setWipePosition(0.25);
    const QImage transformedRight = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    harness.surface.setWipePosition(0.75);
    const QImage transformedLeft = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(transformedRight.isNull());
    ASSERT_FALSE(transformedLeft.isNull());
    const QColor roiRight = transformedRight.pixelColor(sampleX, sampleY);
    const QColor roiLeft = transformedLeft.pixelColor(sampleX, sampleY);
    EXPECT_LT(roiLeft.red(), roiRight.red());

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, WipePreservesDirectionalSamplingAtQuarterTurnRotations) {
    SurfaceWarpHarness harness;
    harness.surface.setViewMode(ComparisonSurface::Wipe);
    harness.surface.setDifferenceEdge(ComparisonSurface::Edge0And1);
    harness.surface.setWipePosition(0.5);
    ASSERT_TRUE(harness.start());

    constexpr std::array<std::uint8_t, 16U> kDarkToLight{
        16U,
        16U,
        16U,
        16U,
        16U,
        16U,
        16U,
        16U,
        235U,
        235U,
        235U,
        235U,
        235U,
        235U,
        235U,
        235U,
    };
    constexpr std::array<std::uint8_t, 16U> kLightToDark{
        235U,
        235U,
        235U,
        235U,
        235U,
        235U,
        235U,
        235U,
        16U,
        16U,
        16U,
        16U,
        16U,
        16U,
        16U,
        16U,
    };
    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};

    const auto renderRotation = [&](const std::uint16_t rotation, const std::uint64_t requestId) {
        std::optional<application::FrameSet> set =
            makeHorizontalLumaSet(*budget,
                                  domain::FrameId{static_cast<std::int64_t>(requestId)},
                                  kDarkToLight,
                                  kLightToDark,
                                  application::FramePresentation{.rotationDegrees = rotation});
        EXPECT_TRUE(set.has_value());
        if (!set.has_value()) {
            return QImage{};
        }
        EXPECT_EQ(actor.submit(makeContext(requestId), std::move(*set)),
                  platform::GpuTransferSubmitResult::Accepted);
        EXPECT_TRUE(actor.waitUntilIdle(5s));
        return harness.grab().convertToFormat(QImage::Format_RGBA8888);
    };

    const QImage clockwise = renderRotation(90U, 43U);
    ASSERT_FALSE(clockwise.isNull());
    const int leftX = clockwise.width() / 2 - clockwise.width() / 12;
    const int rightX = clockwise.width() / 2 + clockwise.width() / 12;
    const int upperY = clockwise.height() / 4;
    const int lowerY = clockwise.height() * 3 / 4;
    EXPECT_GT(clockwise.pixelColor(leftX, upperY).red(), clockwise.pixelColor(leftX, lowerY).red());
    EXPECT_LT(clockwise.pixelColor(rightX, upperY).red(),
              clockwise.pixelColor(rightX, lowerY).red());

    const QImage counterClockwise = renderRotation(270U, 44U);
    ASSERT_FALSE(counterClockwise.isNull());
    EXPECT_LT(counterClockwise.pixelColor(leftX, upperY).red(),
              counterClockwise.pixelColor(leftX, lowerY).red());
    EXPECT_GT(counterClockwise.pixelColor(rightX, upperY).red(),
              counterClockwise.pixelColor(rightX, lowerY).red());

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, RendersThreeSourcesAndSelectedDiffInOneAnalysisGrid) {
    SurfaceWarpHarness harness;
    harness.surface.setViewMode(ComparisonSurface::AnalysisGrid);
    harness.surface.setDifferenceEdge(ComparisonSurface::Edge0And2);
    ASSERT_TRUE(harness.start());

    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> set =
        makeThreeSolidSet(*budget, domain::FrameId{43}, {32U, 96U, 224U});
    ASSERT_TRUE(set.has_value());
    ASSERT_EQ(actor.submit(makeContext(43U), std::move(*set)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const QImage grid = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(grid.isNull());
    const QColor first = grid.pixelColor(grid.width() / 4, grid.height() / 4);
    const QColor second = grid.pixelColor(grid.width() * 3 / 4, grid.height() / 4);
    const QColor third = grid.pixelColor(grid.width() / 4, grid.height() * 3 / 4);
    const QColor difference = grid.pixelColor(grid.width() * 3 / 4, grid.height() * 3 / 4);
    EXPECT_LT(first.red(), second.red());
    EXPECT_LT(second.red(), third.red());
    EXPECT_GT(difference.red() + difference.green() + difference.blue(), 20);
    ASSERT_TRUE(harness.acknowledgementMailbox->tryPop().has_value());
    EXPECT_FALSE(harness.acknowledgementMailbox->tryPop().has_value());

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, SingleViewUsesTheWholeSurfaceForItsOnlySource) {
    SurfaceWarpHarness harness;
    harness.surface.setViewMode(ComparisonSurface::Single);
    ASSERT_TRUE(harness.start());

    auto budget = std::make_shared<platform::FrameBudget>(4U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> set =
        makeSingleSolidSet(*budget, domain::FrameId{44}, 192U);
    ASSERT_TRUE(set.has_value());
    ASSERT_EQ(actor.submit(makeContext(44U), std::move(*set)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const QImage image = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(image.isNull());
    const QColor upper = image.pixelColor(image.width() / 2, image.height() / 4);
    const QColor lower = image.pixelColor(image.width() / 2, image.height() * 3 / 4);
    EXPECT_GT(upper.red() + upper.green() + upper.blue(), 100);
    expectColorNear(lower, upper, 2);

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, ThresholdMaskBlacksDifferencesBelowTheSelectedPolicy) {
    SurfaceWarpHarness harness;
    harness.surface.setViewMode(ComparisonSurface::Difference);
    harness.surface.setThresholdEnabled(true);
    harness.surface.setThresholdPolicy(ComparisonSurface::ThresholdAnyChannel);
    harness.surface.setThreshold(1.0);
    ASSERT_TRUE(harness.start());

    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> set =
        makeThreeSolidSet(*budget, domain::FrameId{44}, {32U, 96U, 224U});
    ASSERT_TRUE(set.has_value());
    ASSERT_EQ(actor.submit(makeContext(44U), std::move(*set)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const QImage masked = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(masked.isNull());
    expectColorNear(masked.pixelColor(masked.width() / 2, masked.height() / 2), QColor{0, 0, 0}, 2);
    harness.surface.setThreshold(0.0);
    const QImage visible = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(visible.isNull());
    const QColor visibleDifference = visible.pixelColor(visible.width() / 2, visible.height() / 2);
    EXPECT_GT(visibleDifference.red() + visibleDifference.green() + visibleDifference.blue(), 20);
    ASSERT_TRUE(harness.acknowledgementMailbox->tryPop().has_value());
    EXPECT_FALSE(harness.acknowledgementMailbox->tryPop().has_value());

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, HighlightThresholdPreservesFirstSourceAndOpacity) {
    SurfaceWarpHarness harness;
    harness.surface.setViewMode(ComparisonSurface::Difference);
    harness.surface.setDifferenceMetric(ComparisonSurface::Highlight);
    harness.surface.setDifferenceGain(ComparisonSurface::Gain4x);
    harness.surface.setThresholdEnabled(true);
    harness.surface.setThreshold(1.0);
    ASSERT_TRUE(harness.start());

    const domain::ColorMetadata metadata{
        .matrix = domain::ColorMatrix::kBt709,
        .range = domain::ColorRange::kFull,
        .matrixInferred = false,
    };
    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> pair = makeSolidSetWithMetadata(
        *budget, domain::FrameId{46}, 96U, 91U, 173U, metadata, 112U, 91U, 173U, metadata);
    ASSERT_TRUE(pair.has_value());
    ASSERT_EQ(actor.submit(makeContext(46U), std::move(*pair)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const QColor sourceA = expectedNv12Color(metadata, 96U, 91U, 173U);
    // Only Y changes by 16 full-range code values. Gain 4 gives tint strength 64/255.
    const float strength = 64.0F / 255.0F;
    const auto tintedChannel = [strength](const int original, const float tint) {
        return static_cast<int>(
            std::lround(static_cast<float>(original) * (1.0F - strength) + tint * strength));
    };
    const QColor highlighted{tintedChannel(sourceA.red(), 255.0F),
                             tintedChannel(sourceA.green(), 0.15F * 255.0F),
                             tintedChannel(sourceA.blue(), 0.35F * 255.0F)};
    const auto expectCenter = [&harness](const QColor& expected) {
        const QImage image = harness.grab().convertToFormat(QImage::Format_RGBA8888);
        ASSERT_FALSE(image.isNull());
        expectColorNear(image.pixelColor(image.width() / 2, image.height() / 2), expected, 3);
    };
    constexpr std::array policies{ComparisonSurface::ThresholdLumaOnly,
                                  ComparisonSurface::ThresholdAnyChannel,
                                  ComparisonSurface::ThresholdAllChannels};
    for (const ComparisonSurface::ThresholdPolicy policy : policies) {
        harness.surface.setThresholdPolicy(policy);
        for (const qreal opacity : {1.0, 0.5}) {
            harness.surface.setOpacity(opacity);
            const auto overWindow = [opacity](const QColor& foreground) {
                return QColor{static_cast<int>(std::lround(foreground.red() * opacity +
                                                           255.0 * (1.0 - opacity))),
                              static_cast<int>(std::lround(foreground.green() * opacity)),
                              static_cast<int>(std::lround(foreground.blue() * opacity +
                                                           255.0 * (1.0 - opacity)))};
            };
            harness.surface.setThreshold(1.0);
            expectCenter(overWindow(sourceA));
            harness.surface.setThreshold(0.0);
            expectCenter(overWindow(highlighted));
            harness.surface.setThreshold(1.0);
            harness.surface.setThresholdEnabled(false);
            expectCenter(overWindow(highlighted));
            harness.surface.setThresholdEnabled(true);
            expectCenter(overWindow(sourceA));
        }
    }

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, SignedSubtractThresholdPreservesNeutralGrayAndOpacity) {
    SurfaceWarpHarness harness;
    harness.surface.setViewMode(ComparisonSurface::Difference);
    harness.surface.setDifferenceMetric(ComparisonSurface::SignedSubtract);
    harness.surface.setDifferenceGain(ComparisonSurface::Gain4x);
    harness.surface.setThresholdEnabled(true);
    ASSERT_TRUE(harness.start());

    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> set =
        makeThreeSolidSet(*budget, domain::FrameId{47}, {96U, 112U, 80U});
    ASSERT_TRUE(set.has_value());
    ASSERT_EQ(actor.submit(makeContext(47U), std::move(*set)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    constexpr std::array edges{ComparisonSurface::Edge0And1, ComparisonSurface::Edge0And2};
    constexpr std::array policies{ComparisonSurface::ThresholdLumaOnly,
                                  ComparisonSurface::ThresholdAnyChannel,
                                  ComparisonSurface::ThresholdAllChannels};
    for (const ComparisonSurface::DifferenceEdge edge : edges) {
        harness.surface.setDifferenceEdge(edge);
        // Full-range Y differs by -/+16; signed subtract at gain 4 is 127.5 -/+64.
        const int signedGray = edge == ComparisonSurface::Edge0And1 ? 64 : 192;
        for (const ComparisonSurface::ThresholdPolicy policy : policies) {
            harness.surface.setThresholdPolicy(policy);
            for (const qreal opacity : {1.0, 0.5}) {
                harness.surface.setOpacity(opacity);
                const auto expectGray = [&harness, opacity](const int gray) {
                    const QImage image = harness.grab().convertToFormat(QImage::Format_RGBA8888);
                    ASSERT_FALSE(image.isNull());
                    const int redBlue =
                        static_cast<int>(std::lround(gray * opacity + 255.0 * (1.0 - opacity)));
                    const int green = static_cast<int>(std::lround(gray * opacity));
                    expectColorNear(image.pixelColor(image.width() / 2, image.height() / 2),
                                    QColor{redBlue, green, redBlue},
                                    3);
                };
                harness.surface.setThreshold(1.0);
                expectGray(128);
                harness.surface.setThreshold(0.0);
                expectGray(signedGray);
                harness.surface.setThreshold(1.0);
                harness.surface.setThresholdEnabled(false);
                expectGray(signedGray);
                harness.surface.setThresholdEnabled(true);
            }
        }
    }

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

// Threshold filtering paints each mode's zero-difference value. Highlight (the first source)
// and SignedSubtract (mid-gray) have dedicated tests above; this table locks the remaining
// metrics, whose zero difference reads black, so a shader edit cannot re-tint filtered
// regions for any policy. Exact planes stay available throughout so its black comes from the
// filter, never from the fail-closed path.
TEST(ComparisonSurfaceWarpTests, ThresholdFilterKeepsBlackNeutralDifferenceMetricsBlack) {
    SurfaceWarpHarness harness;
    harness.surface.setViewMode(ComparisonSurface::Difference);
    harness.surface.setDifferenceMetric(ComparisonSurface::RgbAbsolute);
    harness.surface.setExactPlaneAvailable(true);
    harness.surface.setThresholdEnabled(true);
    ASSERT_TRUE(harness.start());

    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> grays =
        makeThreeSolidSet(*budget, domain::FrameId{48}, {80U, 140U, 224U});
    ASSERT_TRUE(grays.has_value());
    ASSERT_EQ(actor.submit(makeContext(48U), std::move(*grays)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    constexpr std::array metrics{ComparisonSurface::RgbAbsolute,
                                 ComparisonSurface::Luma,
                                 ComparisonSurface::Chroma,
                                 ComparisonSurface::Heatmap,
                                 ComparisonSurface::ExactPlanes};
    constexpr std::array policies{ComparisonSurface::ThresholdLumaOnly,
                                  ComparisonSurface::ThresholdAnyChannel,
                                  ComparisonSurface::ThresholdAllChannels};
    harness.surface.setThreshold(1.0);
    for (const ComparisonSurface::DifferenceMetric metric : metrics) {
        harness.surface.setDifferenceMetric(metric);
        for (const ComparisonSurface::ThresholdPolicy policy : policies) {
            harness.surface.setThresholdPolicy(policy);
            const QImage filtered = harness.grab().convertToFormat(QImage::Format_RGBA8888);
            ASSERT_FALSE(filtered.isNull());
            expectColorNear(filtered.pixelColor(filtered.width() / 2, filtered.height() / 2),
                            QColor{0, 0, 0},
                            2);
        }
    }

    // The same shader must still paint a reading once the threshold stops rejecting it, so the
    // black above is the filter's neutral and not a pass-wide blackout. Chroma needs colored
    // sources; exact planes keep their own non-black coverage in the ExactPlaneDiff tests.
    const domain::ColorMetadata metadata{
        .matrix = domain::ColorMatrix::kBt709,
        .range = domain::ColorRange::kFull,
        .matrixInferred = false,
    };
    std::optional<application::FrameSet> colored = makeSolidSetWithMetadata(*budget,
                                                                            domain::FrameId{49},
                                                                            80U,
                                                                            100U,
                                                                            180U,
                                                                            metadata, //
                                                                            140U,
                                                                            180U,
                                                                            100U,
                                                                            metadata);
    ASSERT_TRUE(colored.has_value());
    ASSERT_EQ(actor.submit(makeContext(49U), std::move(*colored)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));
    harness.surface.setThreshold(0.0);
    for (const ComparisonSurface::DifferenceMetric metric : {ComparisonSurface::RgbAbsolute,
                                                             ComparisonSurface::Luma,
                                                             ComparisonSurface::Chroma,
                                                             ComparisonSurface::Heatmap}) {
        harness.surface.setDifferenceMetric(metric);
        const QImage visible = harness.grab().convertToFormat(QImage::Format_RGBA8888);
        ASSERT_FALSE(visible.isNull());
        const QColor reading = visible.pixelColor(visible.width() / 2, visible.height() / 2);
        EXPECT_GT(reading.red() + reading.green() + reading.blue(), 20);
    }

    EXPECT_TRUE(harness.acknowledgementMailbox->tryPop().has_value());
    EXPECT_TRUE(harness.acknowledgementMailbox->tryPop().has_value());
    EXPECT_FALSE(harness.acknowledgementMailbox->tryPop().has_value());

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

// The Fade view reuses the difference pass with the Crossfade metric and the wipe position as
// the blend amount. A threshold left enabled from a difference mode must not black out the
// blend: crossfade reads no difference, so there is nothing for the filter to reject.
TEST(ComparisonSurfaceWarpTests, FadeViewIsNotFilteredByTheDifferenceThreshold) {
    SurfaceWarpHarness harness;
    harness.surface.setViewMode(ComparisonSurface::Fade);
    harness.surface.setWipePosition(0.5);
    harness.surface.setThresholdEnabled(true);
    harness.surface.setThreshold(1.0);
    ASSERT_TRUE(harness.start());

    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> set =
        makeThreeSolidSet(*budget, domain::FrameId{50}, {96U, 112U, 224U});
    ASSERT_TRUE(set.has_value());
    ASSERT_EQ(actor.submit(makeContext(50U), std::move(*set)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const auto expectBlend = [&harness]() {
        const QImage image = harness.grab().convertToFormat(QImage::Format_RGBA8888);
        ASSERT_FALSE(image.isNull());
        expectColorNear(
            image.pixelColor(image.width() / 2, image.height() / 2), QColor{104, 104, 104}, 3);
    };
    expectBlend();
    harness.surface.setThresholdEnabled(false);
    expectBlend();

    ASSERT_TRUE(harness.acknowledgementMailbox->tryPop().has_value());
    EXPECT_FALSE(harness.acknowledgementMailbox->tryPop().has_value());

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, ExactPlaneDiffFailsClosedAndComparesRawNv12Codes) {
    SurfaceWarpHarness harness;
    harness.surface.setViewMode(ComparisonSurface::Difference);
    harness.surface.setDifferenceMetric(ComparisonSurface::ExactPlanes);
    harness.surface.setDifferenceEdge(ComparisonSurface::Edge0And2);
    ASSERT_TRUE(harness.start());

    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> set =
        makeThreeSolidSet(*budget, domain::FrameId{45}, {32U, 96U, 224U});
    ASSERT_TRUE(set.has_value());
    ASSERT_EQ(actor.submit(makeContext(45U), std::move(*set)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const QImage unavailable = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(unavailable.isNull());
    expectColorNear(unavailable.pixelColor(unavailable.width() / 2, unavailable.height() / 2),
                    QColor{0, 0, 0},
                    2);
    harness.surface.setExactPlaneAvailable(true);
    const QImage exact = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(exact.isNull());
    expectColorNear(exact.pixelColor(exact.width() / 2, exact.height() / 2), QColor{192, 0, 0}, 3);
    ASSERT_TRUE(harness.acknowledgementMailbox->tryPop().has_value());
    EXPECT_FALSE(harness.acknowledgementMailbox->tryPop().has_value());

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, ExactPlaneDiffRetainsOneCodeP010Differences) {
    SurfaceWarpHarness harness;
    harness.surface.setViewMode(ComparisonSurface::Difference);
    harness.surface.setDifferenceMetric(ComparisonSurface::ExactPlanes);
    harness.surface.setDifferenceGain(ComparisonSurface::Gain16x);
    harness.surface.setExactPlaneAvailable(true);
    ASSERT_TRUE(harness.start());

    const domain::ColorMetadata metadata{
        .matrix = domain::ColorMatrix::kBt709,
        .range = domain::ColorRange::kFull,
        .matrixInferred = false,
    };
    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> set =
        makeSolidP010Set(*budget, domain::FrameId{46}, 512U, 513U, metadata);
    ASSERT_TRUE(set.has_value());
    ASSERT_EQ(actor.submit(makeContext(46U), std::move(*set)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const QImage exact = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(exact.isNull());
    expectColorNear(exact.pixelColor(exact.width() / 2, exact.height() / 2), QColor{4, 0, 0}, 1);

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, LeavesMissingSourceSlotUndrawnWithoutShiftingLaterSources) {
    SurfaceWarpHarness harness;
    harness.surface.setViewMode(ComparisonSurface::ThreeUp);
    ASSERT_TRUE(harness.start());

    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> set =
        makeThreeSolidSetWithMissingMiddle(*budget, domain::FrameId{41});
    ASSERT_TRUE(set.has_value());
    ASSERT_EQ(actor.submit(makeContext(41U), std::move(*set)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const QImage threeUp = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(threeUp.isNull());
    const int middleY = threeUp.height() / 2;
    const QColor first = threeUp.pixelColor(threeUp.width() / 6, middleY);
    const QColor missing = threeUp.pixelColor(threeUp.width() / 2, middleY);
    const QColor third = threeUp.pixelColor(threeUp.width() * 5 / 6, middleY);
    EXPECT_GT(first.red() + first.green() + first.blue(), 20);
    expectColorNear(missing, QColor{255, 0, 255}, 2);
    EXPECT_GT(third.red(), first.red());
    EXPECT_TRUE(harness.acknowledgementMailbox->tryPop().has_value());

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, DifferenceWithMissingEdgeStaysBlackInsteadOfShowingOneSource) {
    SurfaceWarpHarness harness;
    harness.surface.setViewMode(ComparisonSurface::Difference);
    harness.surface.setDifferenceEdge(ComparisonSurface::Edge0And1);
    ASSERT_TRUE(harness.start());

    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> set =
        makeThreeSolidSetWithMissingMiddle(*budget, domain::FrameId{42});
    ASSERT_TRUE(set.has_value());
    ASSERT_EQ(actor.submit(makeContext(42U), std::move(*set)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const QImage unavailable = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(unavailable.isNull());
    expectColorNear(unavailable.pixelColor(unavailable.width() / 2, unavailable.height() / 2),
                    QColor{0, 0, 0},
                    2);
    EXPECT_TRUE(harness.acknowledgementMailbox->tryPop().has_value());

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, RendersIdenticalFramesBlackForEveryDifferenceMetric) {
    SurfaceWarpHarness harness;
    harness.surface.setViewMode(ComparisonSurface::Difference);
    harness.surface.setDifferenceGain(ComparisonSurface::Gain16x);
    ASSERT_TRUE(harness.start());

    const domain::ColorMetadata metadata{
        .matrix = domain::ColorMatrix::kBt709,
        .range = domain::ColorRange::kLimited,
        .matrixInferred = false,
    };
    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> pair = makeSolidSetWithMetadata(
        *budget, domain::FrameId{32}, 128U, 91U, 173U, metadata, 128U, 91U, 173U, metadata);
    ASSERT_TRUE(pair.has_value());
    ASSERT_EQ(actor.submit(makeContext(32U), std::move(*pair)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    constexpr std::array metrics{
        ComparisonSurface::RgbAbsolute,
        ComparisonSurface::Luma,
        ComparisonSurface::Chroma,
        ComparisonSurface::Heatmap,
    };
    for (const ComparisonSurface::DifferenceMetric metric : metrics) {
        harness.surface.setDifferenceMetric(metric);
        const QImage image = harness.grab().convertToFormat(QImage::Format_RGBA8888);
        ASSERT_FALSE(image.isNull());
        expectColorNear(
            image.pixelColor(image.width() / 2, image.height() / 2), QColor{0, 0, 0}, 2);
    }
    const std::optional<application::FrameSetPresented> acknowledgement =
        harness.acknowledgementMailbox->tryPop();
    ASSERT_TRUE(acknowledgement.has_value());
    EXPECT_EQ(acknowledgement->frameId, domain::FrameId{32});
    EXPECT_FALSE(harness.acknowledgementMailbox->tryPop().has_value());
    EXPECT_EQ(harness.activitySink->acknowledgementNotifications.load(std::memory_order_relaxed),
              1U);

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

// Step-2 hold-to-peek: while suppressed the difference pass must be replaced by the raw
// first source of the active pair — same rect, same fit — and releasing restores the
// difference. A (32, 224) luma pair makes the two outcomes clearly distinguishable:
// the absolute difference is a bright ~205 gray, the raw first source a dark ~19 gray.
TEST(ComparisonSurfaceWarpTests, DifferencePeekReplacesThePassWithTheRawFirstSource) {
    SurfaceWarpHarness harness;
    harness.surface.setViewMode(ComparisonSurface::Difference);
    ASSERT_TRUE(harness.start());

    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> pair =
        makeSolidSet(*budget, domain::FrameId{36}, 32U, 224U);
    ASSERT_TRUE(pair.has_value());
    ASSERT_EQ(actor.submit(makeContext(36U), std::move(*pair)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const QImage difference = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(difference.isNull());
    const QColor differenceCenter =
        difference.pixelColor(difference.width() / 2, difference.height() / 2);
    expectColorNear(differenceCenter, QColor{205, 205, 205}, 4);

    // Holding the peek button: the raw first source (BT.601 limited, luma 32 → ~19 gray)
    // replaces the difference in the same rect; the viewport/fit must not move.
    harness.surface.setDifferenceSuppressed(true);
    const QImage peek = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(peek.isNull());
    expectColorNear(peek.pixelColor(peek.width() / 2, peek.height() / 2), QColor{19, 19, 19}, 4);

    // Releasing restores the exact difference image.
    harness.surface.setDifferenceSuppressed(false);
    const QImage restored = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(restored.isNull());
    expectColorNear(
        restored.pixelColor(restored.width() / 2, restored.height() / 2), differenceCenter, 2);
    EXPECT_TRUE(harness.acknowledgementMailbox->tryPop().has_value());

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, NativeBelowFitRetainsHighContrastTexelsAndBlackBackground) {
    SurfaceWarpHarness harness;
    // Odd bounds and a fractional inset require phase alignment even at 100% DPI.
    // Avoid an exact half-pixel tie, whose equal placements can differ by float roundoff.
    harness.window.resize(101, 65);
    harness.surface.setPosition(QPointF{1.125, 1.125});
    harness.surface.setSize(QSizeF{99.0, 63.0});
    harness.surface.setViewMode(ComparisonSurface::Single);
    harness.surface.setDifferenceFilter(ComparisonSurface::Bilinear);
    harness.surface.setSourceDisplayInfo(
        {QVariantMap{{QStringLiteral("width"), 8}, {QStringLiteral("height"), 8}}});
    ASSERT_TRUE(harness.start());
    const qreal dpr = harness.window.effectiveDevicePixelRatio();
    harness.surface.zoomAt(0.5, 0.5, 8.0 / (63.0 * dpr));
    ASSERT_LT(harness.surface.viewScale(), 1.0);

    constexpr std::array<std::uint8_t, 8U> kPattern{16U, 235U, 16U, 235U, 16U, 235U, 16U, 235U};
    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> pair =
        makeHorizontalLumaSet(*budget, domain::FrameId{57}, kPattern, kPattern);
    ASSERT_TRUE(pair.has_value());
    ASSERT_EQ(actor.submit(makeContext(57U), std::move(*pair)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));
    const QImage image = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(image.isNull());
    const qreal rasterScaleX = image.width() / static_cast<qreal>(qRound(image.width() / dpr));
    const qreal rasterScaleY = image.height() / static_cast<qreal>(qRound(image.height() / dpr));
    const int left = static_cast<int>(
        std::lround((harness.surface.x() + harness.surface.width() * 0.5) * rasterScaleX - 4.0));
    const int top = static_cast<int>(
        std::lround((harness.surface.y() + harness.surface.height() * 0.5) * rasterScaleY - 4.0));
    ASSERT_GT(left, 0);
    ASSERT_GT(top, 0);
    ASSERT_LT(left + 8, image.width());
    ASSERT_LT(top + 8, image.height());
    // Full-range neutral NV12 has the same RGB code. Any half-texel phase error
    // blends these alternating columns and fails even if Nearest would appear sharp.
    for (int row = 0; row < 8; ++row) {
        for (std::size_t column = 0U; column < kPattern.size(); ++column) {
            const int luma = kPattern[column];
            expectColorNear(image.pixelColor(left + static_cast<int>(column), top + row),
                            QColor{luma, luma, luma},
                            3);
        }
    }
    for (int offset = 0; offset < 8; ++offset) {
        expectColorNear(image.pixelColor(left - 1, top + offset), QColor{0, 0, 0}, 1);
        expectColorNear(image.pixelColor(left + 8, top + offset), QColor{0, 0, 0}, 1);
        expectColorNear(image.pixelColor(left + offset, top - 1), QColor{0, 0, 0}, 1);
        expectColorNear(image.pixelColor(left + offset, top + 8), QColor{0, 0, 0}, 1);
    }
    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, AppliesDistinctDeterministicSpatialFilters) {
    SurfaceWarpHarness harness;
    harness.surface.setViewMode(ComparisonSurface::Difference);
    ASSERT_TRUE(harness.start());

    constexpr std::array<std::uint8_t, 8U> patternA{0U, 255U, 0U, 255U, 0U, 255U, 0U, 255U};
    constexpr std::array<std::uint8_t, 8U> patternB{};
    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> pair = makeHorizontalLumaSet(
        *budget, domain::FrameId{33}, std::span{patternA}, std::span{patternB});
    ASSERT_TRUE(pair.has_value());
    ASSERT_EQ(actor.submit(makeContext(33U), std::move(*pair)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    harness.surface.setDifferenceFilter(ComparisonSurface::Nearest);
    const QImage nearest = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    harness.surface.setDifferenceFilter(ComparisonSurface::Bilinear);
    const QImage bilinear = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    harness.surface.setDifferenceFilter(ComparisonSurface::Bicubic);
    const QImage bicubic = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    const QImage bicubicAgain = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(nearest.isNull());
    ASSERT_FALSE(bilinear.isNull());
    ASSERT_FALSE(bicubic.isNull());
    ASSERT_FALSE(bicubicAgain.isNull());
    EXPECT_GT(maximumRgbDifference(nearest, bilinear), 20);
    EXPECT_GT(maximumRgbDifference(bilinear, bicubic), 2);
    EXPECT_LE(maximumRgbDifference(bicubic, bicubicAgain), 1);

    const std::optional<application::FrameSetPresented> acknowledgement =
        harness.acknowledgementMailbox->tryPop();
    ASSERT_TRUE(acknowledgement.has_value());
    EXPECT_EQ(acknowledgement->frameId, domain::FrameId{33});
    EXPECT_FALSE(harness.acknowledgementMailbox->tryPop().has_value());
    EXPECT_EQ(harness.activitySink->acknowledgementNotifications.load(std::memory_order_relaxed),
              1U);

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, HonorsAncestorScissorInDifferenceMode) {
    SurfaceWarpHarness harness;
    QQuickItem clipper{harness.window.contentItem()};
    clipper.setSize(QSizeF{51.0, 64.0});
    clipper.setClip(true);
    harness.surface.setParentItem(&clipper);
    harness.surface.setViewMode(ComparisonSurface::Difference);
    harness.surface.setDifferenceGain(ComparisonSurface::Gain4x);
    ASSERT_TRUE(harness.start());

    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> pair =
        makeSolidSet(*budget, domain::FrameId{34}, 235U, 64U);
    ASSERT_TRUE(pair.has_value());
    ASSERT_EQ(actor.submit(makeContext(34U), std::move(*pair)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const QImage image = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(image.isNull());
    const QColor inside = image.pixelColor(image.width() / 4, image.height() / 2);
    const QColor outside = image.pixelColor(image.width() * 3 / 4, image.height() / 2);
    EXPECT_GT(inside.red() + inside.green() + inside.blue(), 20);
    expectColorNear(outside, QColor{255, 0, 255}, 2);
    ASSERT_TRUE(harness.acknowledgementMailbox->tryPop().has_value());
    EXPECT_FALSE(harness.acknowledgementMailbox->tryPop().has_value());

    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(ComparisonSurfaceWarpTests, AcknowledgesOnlyTheLatestReplacementAndRetriesAFullQueueOnce) {
    SurfaceWarpHarness harness;
    ASSERT_TRUE(harness.start());

    ASSERT_EQ(harness.acknowledgementMailbox->tryPush(makeDummyAcknowledgement(91U)),
              platform::PresentationAckPushResult::Accepted);
    ASSERT_EQ(harness.acknowledgementMailbox->tryPush(makeDummyAcknowledgement(92U)),
              platform::PresentationAckPushResult::Accepted);
    ASSERT_EQ(harness.acknowledgementMailbox->tryPush(makeDummyAcknowledgement(93U)),
              platform::PresentationAckPushResult::Accepted);
    ASSERT_EQ(harness.acknowledgementMailbox->tryPush(makeDummyAcknowledgement(94U)),
              platform::PresentationAckPushResult::Accepted);

    auto budget = std::make_shared<platform::FrameBudget>(16U * 1024U * 1024U);
    platform::GpuTransferActor actor{budget, harness.broker, harness.mailbox, harness.activitySink};
    std::optional<application::FrameSet> first =
        makeSolidSet(*budget, domain::FrameId{10}, 64U, 64U);
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(actor.submit(makeContext(10U), std::move(*first)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const QImage firstImage = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(firstImage.isNull());
    EXPECT_LT(firstImage.pixelColor(firstImage.width() / 4, firstImage.height() / 2).red(), 90);
    EXPECT_EQ(harness.activitySink->acknowledgementNotifications.load(std::memory_order_relaxed),
              0U);

    std::optional<application::FrameSet> second =
        makeSolidSet(*budget, domain::FrameId{11}, 128U, 128U);
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(actor.submit(makeContext(11U), std::move(*second)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));
    std::optional<application::FrameSet> third =
        makeSolidSet(*budget, domain::FrameId{12}, 192U, 192U);
    ASSERT_TRUE(third.has_value());
    ASSERT_EQ(actor.submit(makeContext(12U), std::move(*third)),
              platform::GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(5s));

    const QImage backpressured = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(backpressured.isNull());
    EXPECT_LT(backpressured.pixelColor(backpressured.width() / 4, backpressured.height() / 2).red(),
              90);
    EXPECT_EQ(harness.activitySink->acknowledgementNotifications.load(std::memory_order_relaxed),
              0U);
    const std::optional<application::FrameSetPresented> dummyA =
        harness.acknowledgementMailbox->tryPop();
    ASSERT_TRUE(dummyA.has_value());
    EXPECT_EQ(dummyA->frameId, domain::FrameId{91});

    const QImage latest = harness.grab().convertToFormat(QImage::Format_RGBA8888);
    ASSERT_FALSE(latest.isNull());
    EXPECT_GT(latest.pixelColor(latest.width() / 4, latest.height() / 2).red(), 180);
    EXPECT_EQ(harness.activitySink->acknowledgementNotifications.load(std::memory_order_relaxed),
              1U);
    // Drain the remaining pre-filled dummies before the retried frame-10 acknowledgement.
    const std::optional<application::FrameSetPresented> dummyB =
        harness.acknowledgementMailbox->tryPop();
    ASSERT_TRUE(dummyB.has_value());
    EXPECT_EQ(dummyB->frameId, domain::FrameId{92});
    const std::optional<application::FrameSetPresented> dummyC =
        harness.acknowledgementMailbox->tryPop();
    ASSERT_TRUE(dummyC.has_value());
    EXPECT_EQ(dummyC->frameId, domain::FrameId{93});
    const std::optional<application::FrameSetPresented> dummyD =
        harness.acknowledgementMailbox->tryPop();
    ASSERT_TRUE(dummyD.has_value());
    EXPECT_EQ(dummyD->frameId, domain::FrameId{94});
    const std::optional<application::FrameSetPresented> presented =
        harness.acknowledgementMailbox->tryPop();
    ASSERT_TRUE(presented.has_value());
    EXPECT_EQ(presented->frameId, domain::FrameId{10});

    ASSERT_FALSE(harness.grab().isNull());
    const std::optional<application::FrameSetPresented> latestPresented =
        harness.acknowledgementMailbox->tryPop();
    ASSERT_TRUE(latestPresented.has_value());
    EXPECT_EQ(latestPresented->frameId, domain::FrameId{12});
    EXPECT_FALSE(harness.acknowledgementMailbox->tryPop().has_value());
    EXPECT_EQ(harness.activitySink->acknowledgementNotifications.load(std::memory_order_relaxed),
              2U);
    harness.releaseRenderer();
    EXPECT_TRUE(actor.shutdown(2s));
}

} // namespace
} // namespace dvs::ui
