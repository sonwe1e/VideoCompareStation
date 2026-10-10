#include "dvs/platform/D3d11ComparisonRenderer.h"

#include "dvs/application/PlaybackTrace.h"
#include "dvs/platform/FrameMailbox.h"
#include "dvs/platform/GraphicsDeviceBroker.h"
#include "dvs/platform/PresentationAckMailbox.h"
#include "dvs/platform/RenderActivitySink.h"

#include "D3d11GpuFrameBacking.h"
#include "GpuFrameResource.h"
#include "GpuFrameSet.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <d3d11.h>
#include <dvs/platform/shaders/DvsNv12Shaders.generated.h>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <wrl/client.h>

namespace dvs::platform {
namespace {

using Microsoft::WRL::ComPtr;

struct alignas(16) ComposeConstants final {
    std::array<float, 16U> clipFromItem{};
    std::array<float, 4U> destinationRect{};
    // Texture coordinates and display coordinates deliberately stay separate. Wipe clips the
    // latter, then rotation is applied in the normalized display space before the result is
    // projected into the decoder-owned texture region.
    std::array<float, 4U> textureRegion{};
    std::array<float, 4U> displayUvRect{0.0F, 0.0F, 1.0F, 1.0F};
    float opacity = 1.0F;
    std::array<float, 3U> padding{};
    std::uint32_t sourceRotation = 0U;
    std::array<float, 3U> rotationPadding{};
};

struct alignas(16) ColorConstants final {
    std::array<float, 12U> yuvToRgb{};
};

struct alignas(16) DifferenceConstants final {
    std::array<float, 4U> sourceUvRectA{};
    std::array<float, 4U> sourceUvRectB{};
    std::array<float, 4U> planeDimensionsA{};
    std::array<float, 4U> planeDimensionsB{};
    std::uint32_t metric = 0U;
    float gain = 1.0F;
    std::uint32_t filter = 0U;
    float padding = 0.0F;
    std::uint32_t thresholdEnabled = 0U;
    float threshold = 0.0F;
    std::uint32_t thresholdPolicy = 0U;
    float thresholdPadding = 0.0F;
    std::uint32_t sourceRotationA = 0U;
    std::uint32_t sourceRotationB = 0U;
    float fadeAmount = 0.5F;
    float rotationPadding = 0.0F;
};

static_assert(sizeof(ComposeConstants) == 144U);
static_assert(sizeof(ColorConstants) == 48U);
static_assert(sizeof(DifferenceConstants) == 112U);

struct VideoDraw final {
    ComposeConstants compose;
    ColorConstants color;
    ID3D11ShaderResourceView* yView = nullptr;
    ID3D11ShaderResourceView* uvView = nullptr;
};

struct DifferenceDraw final {
    ComposeConstants compose;
    ColorConstants colorA;
    ColorConstants colorB;
    DifferenceConstants options;
    ID3D11ShaderResourceView* yViewA = nullptr;
    ID3D11ShaderResourceView* uvViewA = nullptr;
    ID3D11ShaderResourceView* yViewB = nullptr;
    ID3D11ShaderResourceView* uvViewB = nullptr;
    SurfaceDifferenceFilter filter = SurfaceDifferenceFilter::Bilinear;
};

struct PreparedSetDraw final {
    FrameMailboxPublication publication;
    bool hasDifference = false;
    // Filter for the regular video-slot pass; mirrored from SurfaceRenderState::videoFilter
    // so drawVideoSlots can pick its sampler without threading the whole state through.
    SurfaceDifferenceFilter videoFilter = SurfaceDifferenceFilter::Bilinear;
    // Up to three slot draws (two-up, three-up, reference-focus layouts). Each draw carries its
    // own constants and owns one constant-buffer set until the immediate-context draw is issued.
    std::array<VideoDraw, 3U> videoDraws{};
    std::size_t videoDrawCount = 0U;
    DifferenceDraw differenceDraw;
    std::array<ComposeConstants, 16U> letterboxBars{};
    std::size_t letterboxBarCount = 0U;
};

[[nodiscard]] bool allFinite(const std::array<float, 16U>& values) noexcept {
    return std::all_of(
        values.cbegin(), values.cend(), [](const float value) { return std::isfinite(value); });
}

[[nodiscard]] bool isValid(const SurfaceViewMode value) noexcept {
    return value == SurfaceViewMode::SideBySide || value == SurfaceViewMode::ThreeUp ||
           value == SurfaceViewMode::ReferenceFocus || value == SurfaceViewMode::Difference ||
           value == SurfaceViewMode::AnalysisGrid || value == SurfaceViewMode::Wipe ||
           value == SurfaceViewMode::Single || value == SurfaceViewMode::Fade;
}

[[nodiscard]] bool isValid(const SurfaceDifferenceMetric value) noexcept {
    return value == SurfaceDifferenceMetric::RgbAbsolute ||
           value == SurfaceDifferenceMetric::Luma || value == SurfaceDifferenceMetric::Chroma ||
           value == SurfaceDifferenceMetric::Heatmap ||
           value == SurfaceDifferenceMetric::ExactPlanes ||
           value == SurfaceDifferenceMetric::SignedSubtract ||
           value == SurfaceDifferenceMetric::Highlight ||
           value == SurfaceDifferenceMetric::Crossfade;
}

[[nodiscard]] bool isValid(const SurfaceDifferenceGain value) noexcept {
    return value == SurfaceDifferenceGain::Gain1x || value == SurfaceDifferenceGain::Gain2x ||
           value == SurfaceDifferenceGain::Gain4x || value == SurfaceDifferenceGain::Gain8x ||
           value == SurfaceDifferenceGain::Gain16x;
}

[[nodiscard]] bool isValid(const SurfaceDifferenceEdge value) noexcept {
    return value == SurfaceDifferenceEdge::Between0And1 ||
           value == SurfaceDifferenceEdge::Between0And2 ||
           value == SurfaceDifferenceEdge::Between1And2;
}

[[nodiscard]] bool isValid(const SurfaceDifferenceFilter value) noexcept {
    return value == SurfaceDifferenceFilter::Nearest ||
           value == SurfaceDifferenceFilter::Bilinear || value == SurfaceDifferenceFilter::Bicubic;
}

[[nodiscard]] bool isValid(const SurfaceThresholdPolicy value) noexcept {
    return value == SurfaceThresholdPolicy::LumaOnly ||
           value == SurfaceThresholdPolicy::AnyChannel ||
           value == SurfaceThresholdPolicy::AllChannels;
}

[[nodiscard]] float differenceGain(const SurfaceDifferenceGain value) noexcept {
    switch (value) {
    case SurfaceDifferenceGain::Gain1x:
        return 1.0F;
    case SurfaceDifferenceGain::Gain2x:
        return 2.0F;
    case SurfaceDifferenceGain::Gain4x:
        return 4.0F;
    case SurfaceDifferenceGain::Gain8x:
        return 8.0F;
    case SurfaceDifferenceGain::Gain16x:
        return 16.0F;
    }
    return 1.0F;
}

[[nodiscard]] std::array<float, 4U>
textureRegionValues(const application::TextureRegion& region) noexcept {
    return {
        region.left,
        region.top,
        region.right - region.left,
        region.bottom - region.top,
    };
}

[[nodiscard]] application::TextureRegion
transformedTextureRegion(const application::TextureRegion& source,
                         const SurfaceViewTransform& transform,
                         const bool roiEnabled,
                         const SurfaceNormalizedRect& roi) noexcept {
    const SurfaceNormalizedRect sample = effectiveSurfaceSampleRect(transform, roiEnabled, roi);
    const float sourceWidth = source.right - source.left;
    const float sourceHeight = source.bottom - source.top;
    return application::TextureRegion{
        .left = source.left + (sample.left * sourceWidth),
        .top = source.top + (sample.top * sourceHeight),
        .right = source.left + (sample.right * sourceWidth),
        .bottom = source.top + (sample.bottom * sourceHeight),
    };
}

[[nodiscard]] std::pair<float, float> transformedExtent(const application::FrameGeometry& geometry,
                                                        const bool roiEnabled,
                                                        const SurfaceNormalizedRect& roi) noexcept {
    const float widthScale = roiEnabled ? roi.right - roi.left : 1.0F;
    const float heightScale = roiEnabled ? roi.bottom - roi.top : 1.0F;
    const float sampleWidth = static_cast<float>(geometry.width) * widthScale *
                              static_cast<float>(geometry.presentation.sampleAspectNumerator) /
                              static_cast<float>(geometry.presentation.sampleAspectDenominator);
    const float sampleHeight = static_cast<float>(geometry.height) * heightScale;
    if (geometry.presentation.rotationDegrees == 90U ||
        geometry.presentation.rotationDegrees == 270U) {
        return {sampleHeight, sampleWidth};
    }
    return {sampleWidth, sampleHeight};
}

[[nodiscard]] std::array<float, 4U>
sourcePlaneDimensions(const application::FrameGeometry& geometry) noexcept {
    const std::uint32_t uvWidth = (geometry.width / 2U) + (geometry.width % 2U);
    const std::uint32_t uvHeight = (geometry.height / 2U) + (geometry.height % 2U);
    return {
        static_cast<float>(geometry.width),
        static_cast<float>(geometry.height),
        static_cast<float>(uvWidth),
        static_cast<float>(uvHeight),
    };
}

[[nodiscard]] ComposeConstants makeComposeConstants(const SurfaceRenderState& state,
                                                    const SurfaceRect& destination,
                                                    const application::TextureRegion& region,
                                                    const std::uint16_t rotationDegrees = 0U,
                                                    const std::array<float, 4U>& displayUvRect = {
                                                        0.0F, 0.0F, 1.0F, 1.0F}) {
    return ComposeConstants{
        .clipFromItem = state.clipFromItem,
        .destinationRect = {destination.x, destination.y, destination.width, destination.height},
        .textureRegion =
            {
                region.left,
                region.top,
                region.right - region.left,
                region.bottom - region.top,
            },
        .displayUvRect = displayUvRect,
        .opacity = std::clamp(state.opacity, 0.0F, 1.0F),
        .sourceRotation = rotationDegrees / 90U,
    };
}

[[nodiscard]] ComposeConstants makeBlackConstants(const SurfaceRenderState& state) {
    return makeComposeConstants(state,
                                SurfaceRect{
                                    .x = 0.0F,
                                    .y = 0.0F,
                                    .width = state.logicalWidth,
                                    .height = state.logicalHeight,
                                },
                                application::TextureRegion{});
}

[[nodiscard]] D3D11_BUFFER_DESC dynamicConstantBufferDescription(const std::size_t byteWidth) {
    return D3D11_BUFFER_DESC{
        .ByteWidth = static_cast<UINT>(byteWidth),
        .Usage = D3D11_USAGE_DYNAMIC,
        .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
        .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
        .MiscFlags = 0U,
        .StructureByteStride = 0U,
    };
}

[[nodiscard]] HRESULT createDynamicConstantBuffer(ID3D11Device& device,
                                                  const std::size_t byteWidth,
                                                  ComPtr<ID3D11Buffer>& result) noexcept {
    const D3D11_BUFFER_DESC description = dynamicConstantBufferDescription(byteWidth);
    return device.CreateBuffer(&description, nullptr, result.ReleaseAndGetAddressOf());
}

template <typename Value>
[[nodiscard]] HRESULT writeConstantBuffer(ID3D11DeviceContext& context,
                                          ID3D11Buffer& buffer,
                                          const Value& value) noexcept {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT result = context.Map(&buffer, 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped);
    if (FAILED(result)) {
        return result;
    }
    std::memcpy(mapped.pData, &value, sizeof(value));
    context.Unmap(&buffer, 0U);
    return S_OK;
}

[[nodiscard]] const D3d11GpuFrameBacking*
d3dBacking(const std::shared_ptr<const GpuFrameResource>& frame) noexcept {
    if (!frame) {
        return nullptr;
    }
    return dynamic_cast<const D3d11GpuFrameBacking*>(&frame->backing());
}

[[nodiscard]] std::uint8_t frameBitDepth(const GpuFrameResource& frame) noexcept {
    return frame.format() == application::NormalizedFrameFormat::P010_10 ? 10U : 8U;
}

[[nodiscard]] VideoDraw makeVideoDraw(const SurfaceRenderState& state,
                                      const SurfaceRect& destination,
                                      const GpuFrameResource& frame,
                                      const D3d11GpuFrameBacking& backing) {
    const application::TextureRegion sourceRegion = transformedTextureRegion(
        frame.geometry().textureRegion, state.viewTransform, state.roiEnabled, state.roi);
    return VideoDraw{
        .compose = makeComposeConstants(
            state, destination, sourceRegion, frame.geometry().presentation.rotationDegrees),
        .color =
            ColorConstants{
                .yuvToRgb =
                    nv12ColorTransform(frame.colorMetadata(), frameBitDepth(frame)).yuvToRgb,
            },
        .yView = backing.yView(),
        .uvView = backing.uvView(),
    };
}

[[nodiscard]] DifferenceDraw makeDifferenceDraw(const SurfaceRenderState& state,
                                                const SurfaceRect& destination,
                                                const GpuFrameResource& frameA,
                                                const D3d11GpuFrameBacking& backingA,
                                                const GpuFrameResource& frameB,
                                                const D3d11GpuFrameBacking& backingB) {
    const application::TextureRegion regionA = transformedTextureRegion(
        frameA.geometry().textureRegion, state.viewTransform, state.roiEnabled, state.roi);
    const application::TextureRegion regionB = transformedTextureRegion(
        frameB.geometry().textureRegion, state.viewTransform, state.roiEnabled, state.roi);
    return DifferenceDraw{
        .compose = makeComposeConstants(
            state,
            destination,
            application::TextureRegion{.left = 0.0F, .top = 0.0F, .right = 1.0F, .bottom = 1.0F}),
        .colorA =
            ColorConstants{
                .yuvToRgb =
                    nv12ColorTransform(frameA.colorMetadata(), frameBitDepth(frameA)).yuvToRgb,
            },
        .colorB =
            ColorConstants{
                .yuvToRgb =
                    nv12ColorTransform(frameB.colorMetadata(), frameBitDepth(frameB)).yuvToRgb,
            },
        .options =
            DifferenceConstants{
                .sourceUvRectA = textureRegionValues(regionA),
                .sourceUvRectB = textureRegionValues(regionB),
                .planeDimensionsA = sourcePlaneDimensions(frameA.geometry()),
                .planeDimensionsB = sourcePlaneDimensions(frameB.geometry()),
                .metric = static_cast<std::uint32_t>(state.viewMode == SurfaceViewMode::Fade
                                                         ? SurfaceDifferenceMetric::Crossfade
                                                         : state.differenceMetric),
                .gain = differenceGain(state.differenceGain),
                .filter = static_cast<std::uint32_t>(state.differenceMetric ==
                                                             SurfaceDifferenceMetric::ExactPlanes
                                                         ? SurfaceDifferenceFilter::Nearest
                                                         : state.differenceFilter),
                .thresholdEnabled = state.thresholdEnabled ? 1U : 0U,
                .threshold = state.threshold,
                .thresholdPolicy = static_cast<std::uint32_t>(state.thresholdPolicy),
                .sourceRotationA = frameA.geometry().presentation.rotationDegrees / 90U,
                .sourceRotationB = frameB.geometry().presentation.rotationDegrees / 90U,
                .fadeAmount = state.wipePosition,
            },
        .yViewA = backingA.yView(),
        .uvViewA = backingA.uvView(),
        .yViewB = backingB.yView(),
        .uvViewB = backingB.uvView(),
        .filter = state.differenceMetric == SurfaceDifferenceMetric::ExactPlanes
                      ? SurfaceDifferenceFilter::Nearest
                      : state.differenceFilter,
    };
}

void appendLetterboxBars(const SurfaceRenderState& state,
                         const SurfaceRect& bounds,
                         const SurfaceRect& content,
                         PreparedSetDraw& prepared) {
    const auto append = [&](const SurfaceRect& bar) {
        if (bar.isValid() && prepared.letterboxBarCount < prepared.letterboxBars.size()) {
            prepared.letterboxBars[prepared.letterboxBarCount] =
                makeComposeConstants(state, bar, application::TextureRegion{});
            ++prepared.letterboxBarCount;
        }
    };

    append(SurfaceRect{
        .x = bounds.x, .y = bounds.y, .width = bounds.width, .height = content.y - bounds.y});
    append(SurfaceRect{.x = bounds.x,
                       .y = content.y + content.height,
                       .width = bounds.width,
                       .height = (bounds.y + bounds.height) - (content.y + content.height)});
    append(SurfaceRect{
        .x = bounds.x, .y = content.y, .width = content.x - bounds.x, .height = content.height});
    append(SurfaceRect{.x = content.x + content.width,
                       .y = content.y,
                       .width = (bounds.x + bounds.width) - (content.x + content.width),
                       .height = content.height});
}

[[nodiscard]] bool isRemovedResult(const HRESULT result) noexcept {
    return result == DXGI_ERROR_DEVICE_HUNG || result == DXGI_ERROR_DEVICE_REMOVED ||
           result == DXGI_ERROR_DEVICE_RESET || result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

} // namespace

bool SurfaceViewTransform::isValid() const noexcept {
    return std::isfinite(centerX) && std::isfinite(centerY) && std::isfinite(scale) &&
           centerX >= 0.0F && centerX <= 1.0F && centerY >= 0.0F && centerY <= 1.0F &&
           scale >= presentation::kMinimumViewportScale &&
           scale <= presentation::kMaximumViewportScale &&
           (scale >= 1.0F || (centerX == 0.5F && centerY == 0.5F));
}

bool SurfaceNormalizedRect::isValid() const noexcept {
    return std::isfinite(left) && std::isfinite(top) && std::isfinite(right) &&
           std::isfinite(bottom) && left >= 0.0F && top >= 0.0F && right <= 1.0F &&
           bottom <= 1.0F && left < right && top < bottom;
}

SurfaceNormalizedRect effectiveSurfaceSampleRect(const SurfaceViewTransform& transform,
                                                 const bool roiEnabled,
                                                 const SurfaceNormalizedRect& roi) noexcept {
    if (!transform.isValid() || (roiEnabled && !roi.isValid())) {
        return {};
    }
    const float roiLeft = roiEnabled ? roi.left : 0.0F;
    const float roiTop = roiEnabled ? roi.top : 0.0F;
    const float roiWidth = roiEnabled ? roi.right - roi.left : 1.0F;
    const float roiHeight = roiEnabled ? roi.bottom - roi.top : 1.0F;
    const float visible = 1.0F / (std::max)(transform.scale, 1.0F);
    const float viewportLeft =
        std::clamp(transform.centerX - (visible * 0.5F), 0.0F, 1.0F - visible);
    const float viewportTop =
        std::clamp(transform.centerY - (visible * 0.5F), 0.0F, 1.0F - visible);
    return SurfaceNormalizedRect{
        .left = roiLeft + (viewportLeft * roiWidth),
        .top = roiTop + (viewportTop * roiHeight),
        .right = roiLeft + ((viewportLeft + visible) * roiWidth),
        .bottom = roiTop + ((viewportTop + visible) * roiHeight),
    };
}

bool SurfacePixelGrid::isValid() const noexcept {
    return std::isfinite(scaleX) && std::isfinite(scaleY) && std::isfinite(originX) &&
           std::isfinite(originY) && scaleX > 0.0F && scaleY > 0.0F;
}

SurfacePixelGrid surfacePixelGridFromClip(const std::array<float, 16U>& matrix,
                                          const SurfaceViewport& viewport) noexcept {
    if (!allFinite(matrix) || !std::isfinite(viewport.topLeftX) ||
        !std::isfinite(viewport.topLeftY) || !std::isfinite(viewport.width) ||
        !std::isfinite(viewport.height) || viewport.width <= 0.0F || viewport.height <= 0.0F ||
        matrix[15U] <= 0.0F || matrix[1U] != 0.0F || matrix[4U] != 0.0F || matrix[12U] != 0.0F ||
        matrix[13U] != 0.0F) {
        return {};
    }
    const float inverseW = 1.0F / matrix[15U];
    const SurfacePixelGrid result{
        .scaleX = matrix[0U] * inverseW * viewport.width * 0.5F,
        .scaleY = -matrix[5U] * inverseW * viewport.height * 0.5F,
        .originX = viewport.topLeftX + (matrix[3U] * inverseW + 1.0F) * viewport.width * 0.5F,
        .originY = viewport.topLeftY + (1.0F - matrix[7U] * inverseW) * viewport.height * 0.5F,
    };
    return result.isValid() ? result : SurfacePixelGrid{};
}

SurfacePixelGrid surfacePixelGridForWindow(const double logicalWidth,
                                           const double logicalHeight,
                                           const double nominalDevicePixelRatio) noexcept {
    if (!std::isfinite(logicalWidth) || !std::isfinite(logicalHeight) ||
        !std::isfinite(nominalDevicePixelRatio) || logicalWidth <= 0.0F || logicalHeight <= 0.0F ||
        nominalDevicePixelRatio <= 0.0F) {
        return {};
    }
    const auto axisScale = [nominalDevicePixelRatio](const double logical) {
        const double pixels = std::round(logical * nominalDevicePixelRatio);
        const double projection = std::round(pixels / nominalDevicePixelRatio);
        return pixels >= 1.0 && projection >= 1.0 ? static_cast<float>(pixels / projection) : 0.0F;
    };
    const SurfacePixelGrid result{axisScale(logicalWidth), axisScale(logicalHeight), 0.0F, 0.0F};
    return result.isValid() ? result : SurfacePixelGrid{};
}

SurfaceRect orientedSurfaceSamplePixels(const float sourceWidth,
                                        const float sourceHeight,
                                        const std::uint16_t rotationDegrees,
                                        const SurfaceNormalizedRect& sample) noexcept {
    if (!std::isfinite(sourceWidth) || !std::isfinite(sourceHeight) || sourceWidth <= 0.0F ||
        sourceHeight <= 0.0F || !sample.isValid()) {
        return {};
    }
    const float left = sample.left * sourceWidth;
    const float top = sample.top * sourceHeight;
    const float right = sample.right * sourceWidth;
    const float bottom = sample.bottom * sourceHeight;
    switch (rotationDegrees) {
    case 0U:
        return {left, top, right - left, bottom - top};
    case 90U:
        return {top, sourceWidth - right, bottom - top, right - left};
    case 180U:
        return {sourceWidth - right, sourceHeight - bottom, right - left, bottom - top};
    case 270U:
        return {sourceHeight - bottom, left, bottom - top, right - left};
    default:
        return {};
    }
}

SurfaceRect transformedSurfaceContentRect(const SurfaceRect& bounds,
                                          const SurfaceRect& fittedContent,
                                          const SurfaceViewTransform& transform,
                                          const SurfacePixelGrid& pixelGrid,
                                          const SurfaceRect& sourcePixels,
                                          const float nominalPixelsPerUnit) noexcept {
    const auto finiteRect = [](const SurfaceRect& rect) {
        return rect.isValid() && std::isfinite(rect.x) && std::isfinite(rect.y) &&
               std::isfinite(rect.width) && std::isfinite(rect.height);
    };
    if (!transform.isValid() || !finiteRect(bounds) || !finiteRect(fittedContent)) {
        return {};
    }
    if (transform.scale >= 1.0F) {
        return fittedContent;
    }
    // A numeric-positive scale can still produce no meaningful raster footprint.
    // Fail closed before float conversion, division or phase rounding; the GUI uses
    // the matching physical-pixel floor and therefore cannot request this state.
    if (!pixelGrid.isValid() || !std::isfinite(nominalPixelsPerUnit) ||
        nominalPixelsPerUnit < 0.0F) {
        return {};
    }
    double width = static_cast<double>(fittedContent.width) * transform.scale;
    double height = static_cast<double>(fittedContent.height) * transform.scale;
    constexpr double pixelTolerance = 0.00001;
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0 || height <= 0.0) {
        return {};
    }
    double x = fittedContent.x + (static_cast<double>(fittedContent.width) - width) * 0.5;
    double y = fittedContent.y + (static_cast<double>(fittedContent.height) - height) * 0.5;
    if (pixelGrid.isValid() && finiteRect(sourcePixels)) {
        // At native scale, preserve the source sample's fractional phase on the physical
        // pixel grid. Never enlarge/crop into an adjacent panel to obtain alignment.
        const auto alignAxis = [](double& position,
                                  double& extent,
                                  const double boundStart,
                                  const double boundExtent,
                                  const double pixelsPerUnit,
                                  const double nominalPixelsPerUnit,
                                  const double physicalOrigin,
                                  const double sampleOrigin,
                                  const double sampleExtent) {
            constexpr double nativeTolerance = 0.00001;
            if (std::abs(extent * nominalPixelsPerUnit / sampleExtent - 1.0) > nativeTolerance) {
                return true;
            }
            const double nativeExtent = sampleExtent / pixelsPerUnit;
            if (!std::isfinite(nativeExtent) || nativeExtent <= 0.0 || nativeExtent > boundExtent) {
                return false;
            }
            position += (extent - nativeExtent) * 0.5;
            extent = nativeExtent;
            const double minimumPhase =
                std::ceil(boundStart * pixelsPerUnit + physicalOrigin - sampleOrigin);
            const double maximumPhase =
                std::floor((boundStart + boundExtent - extent) * pixelsPerUnit + physicalOrigin -
                           sampleOrigin);
            if (minimumPhase > maximumPhase) {
                // A tightly fitting fractional ROI may have no phase-aligned placement
                // inside this panel. Keep full content and correct size instead of cropping.
                return true;
            }
            const double nearestPhase =
                std::round(position * pixelsPerUnit + physicalOrigin - sampleOrigin);
            position = (std::clamp(nearestPhase, minimumPhase, maximumPhase) + sampleOrigin -
                        physicalOrigin) /
                       pixelsPerUnit;
            return true;
        };
        const bool xValid =
            alignAxis(x,
                      width,
                      bounds.x,
                      bounds.width,
                      pixelGrid.scaleX,
                      nominalPixelsPerUnit > 0.0F ? nominalPixelsPerUnit : pixelGrid.scaleX,
                      pixelGrid.originX,
                      sourcePixels.x,
                      sourcePixels.width);
        const bool yValid =
            alignAxis(y,
                      height,
                      bounds.y,
                      bounds.height,
                      pixelGrid.scaleY,
                      nominalPixelsPerUnit > 0.0F ? nominalPixelsPerUnit : pixelGrid.scaleY,
                      pixelGrid.originY,
                      sourcePixels.y,
                      sourcePixels.height);
        if (!xValid || !yValid) {
            return {};
        }
    }
    if (width * pixelGrid.scaleX < 1.0 - pixelTolerance ||
        height * pixelGrid.scaleY < 1.0 - pixelTolerance) {
        return {};
    }
    const SurfaceRect result{static_cast<float>(x),
                             static_cast<float>(y),
                             static_cast<float>(width),
                             static_cast<float>(height)};
    return finiteRect(result) ? result : SurfaceRect{};
}

bool SurfaceRenderState::isValid() const noexcept {
    if (!allFinite(clipFromItem) || !std::isfinite(logicalWidth) || !std::isfinite(logicalHeight) ||
        !std::isfinite(opacity) || !std::isfinite(nominalDevicePixelRatio) ||
        nominalDevicePixelRatio <= 0.0F || logicalWidth <= 0.0F || logicalHeight <= 0.0F ||
        pixelWidth == 0U || pixelHeight == 0U) {
        return false;
    }
    return (!scissorEnabled || scissor.isValid()) && dvs::platform::isValid(viewMode) &&
           dvs::platform::isValid(differenceMetric) && dvs::platform::isValid(differenceGain) &&
           dvs::platform::isValid(differenceEdge) && dvs::platform::isValid(differenceFilter) &&
           dvs::platform::isValid(videoFilter) && dvs::platform::isValid(thresholdPolicy) &&
           std::isfinite(threshold) && threshold >= 0.0F && threshold <= 1.0F &&
           viewTransform.isValid() && (!roiEnabled || roi.isValid()) &&
           std::isfinite(wipePosition) && wipePosition >= 0.0F && wipePosition <= 1.0F &&
           referenceSlot < 3U;
}

SurfaceSplitLayout computeSurfaceSplit(const float logicalWidth,
                                       const float logicalHeight,
                                       const std::uint32_t pixelWidth,
                                       const std::uint32_t pixelHeight) noexcept {
    if (!std::isfinite(logicalWidth) || !std::isfinite(logicalHeight) || logicalWidth <= 0.0F ||
        logicalHeight <= 0.0F || pixelWidth < 2U || pixelHeight == 0U) {
        return {};
    }

    const std::uint32_t leftPixels = pixelWidth / 2U;
    const std::uint32_t rightPixels = pixelWidth - leftPixels;
    const float split =
        logicalWidth * static_cast<float>(leftPixels) / static_cast<float>(pixelWidth);
    return SurfaceSplitLayout{
        .leftPixelWidth = leftPixels,
        .rightPixelWidth = rightPixels,
        .left = SurfaceRect{.x = 0.0F, .y = 0.0F, .width = split, .height = logicalHeight},
        .right =
            SurfaceRect{
                .x = split,
                .y = 0.0F,
                .width = logicalWidth - split,
                .height = logicalHeight,
            },
    };
}

// Equal-width column regions for the two-up and three-up layouts. Region boundaries snap to
// pixel columns so adjacent panels never overlap or drop a pixel column.
struct SurfaceColumnLayout final {
    std::array<SurfaceRect, 3U> columns{};
    std::size_t count = 0U;
};

[[nodiscard]] SurfaceColumnLayout computeSurfaceColumns(const float logicalWidth,
                                                        const float logicalHeight,
                                                        const std::uint32_t pixelWidth,
                                                        const std::uint32_t pixelHeight,
                                                        const std::size_t count) noexcept {
    SurfaceColumnLayout layout{};
    if (!std::isfinite(logicalWidth) || !std::isfinite(logicalHeight) || logicalWidth <= 0.0F ||
        logicalHeight <= 0.0F || pixelWidth == 0U || pixelHeight == 0U || count == 0U ||
        count > layout.columns.size()) {
        return layout;
    }
    for (std::size_t index = 0U; index < count; ++index) {
        const std::uint32_t startPixels = static_cast<std::uint32_t>(pixelWidth * index / count);
        const std::uint32_t endPixels =
            static_cast<std::uint32_t>(pixelWidth * (index + 1U) / count);
        const float x =
            logicalWidth * static_cast<float>(startPixels) / static_cast<float>(pixelWidth);
        const float width = logicalWidth * static_cast<float>(endPixels - startPixels) /
                            static_cast<float>(pixelWidth);
        layout.columns[index] =
            SurfaceRect{.x = x, .y = 0.0F, .width = width, .height = logicalHeight};
    }
    layout.count = count;
    return layout;
}

// Reference-focus layout (USERPLAN 6.3): the reference source (slot 0) takes the left two
// thirds; the two predictions stack in the right third.
struct SurfaceFocusLayout final {
    SurfaceRect main;
    SurfaceRect topRight;
    SurfaceRect bottomRight;
};

[[nodiscard]] SurfaceFocusLayout
computeReferenceFocusLayout(const float logicalWidth,
                            const float logicalHeight,
                            const std::uint32_t pixelWidth,
                            const std::uint32_t pixelHeight) noexcept {
    if (!std::isfinite(logicalWidth) || !std::isfinite(logicalHeight) || logicalWidth <= 0.0F ||
        logicalHeight <= 0.0F || pixelWidth < 3U || pixelHeight < 2U) {
        return {};
    }
    const std::uint32_t mainPixels = pixelWidth * 2U / 3U;
    const float mainWidth =
        logicalWidth * static_cast<float>(mainPixels) / static_cast<float>(pixelWidth);
    const std::uint32_t topPixels = pixelHeight / 2U;
    const float topHeight =
        logicalHeight * static_cast<float>(topPixels) / static_cast<float>(pixelHeight);
    return SurfaceFocusLayout{
        .main = SurfaceRect{.x = 0.0F, .y = 0.0F, .width = mainWidth, .height = logicalHeight},
        .topRight =
            SurfaceRect{
                .x = mainWidth, .y = 0.0F, .width = logicalWidth - mainWidth, .height = topHeight},
        .bottomRight = SurfaceRect{.x = mainWidth,
                                   .y = topHeight,
                                   .width = logicalWidth - mainWidth,
                                   .height = logicalHeight - topHeight},
    };
}

SurfacePanelLayout computeSurfacePanelLayout(const SurfaceViewMode viewMode,
                                             const float logicalWidth,
                                             const float logicalHeight,
                                             const std::uint32_t pixelWidth,
                                             const std::uint32_t pixelHeight,
                                             const std::uint8_t referenceSlot,
                                             const SurfaceDifferenceEdge differenceEdge,
                                             const float wipePosition) noexcept {
    SurfacePanelLayout result;
    if (!isValid(viewMode) || !std::isfinite(logicalWidth) || !std::isfinite(logicalHeight) ||
        logicalWidth <= 0.0F || logicalHeight <= 0.0F || pixelWidth == 0U || pixelHeight == 0U ||
        referenceSlot > 2U || !isValid(differenceEdge) || !std::isfinite(wipePosition) ||
        wipePosition < 0.0F || wipePosition > 1.0F) {
        return result;
    }
    const SurfaceRect full{
        .x = 0.0F,
        .y = 0.0F,
        .width = logicalWidth,
        .height = logicalHeight,
    };
    if (viewMode == SurfaceViewMode::Single) {
        result.sourceRects[0U] = full;
        result.sourceCount = 1U;
        return result;
    }
    if (viewMode == SurfaceViewMode::Difference || viewMode == SurfaceViewMode::Fade) {
        result.differenceRect = full;
        return result;
    }
    if (viewMode == SurfaceViewMode::Wipe) {
        const float split = logicalWidth * wipePosition;
        result.sourceRects[0U] =
            SurfaceRect{.x = 0.0F, .y = 0.0F, .width = split, .height = logicalHeight};
        result.sourceRects[1U] = SurfaceRect{
            .x = split,
            .y = 0.0F,
            .width = logicalWidth - split,
            .height = logicalHeight,
        };
        if (differenceEdge == SurfaceDifferenceEdge::Between0And2) {
            result.sourceSlots = {0U, 2U, 1U};
        } else if (differenceEdge == SurfaceDifferenceEdge::Between1And2) {
            result.sourceSlots = {1U, 2U, 0U};
        }
        result.sourceCount = 2U;
        return result;
    }
    if (viewMode == SurfaceViewMode::AnalysisGrid) {
        const std::uint32_t leftPixels = pixelWidth / 2U;
        const std::uint32_t topPixels = pixelHeight / 2U;
        const float leftWidth =
            logicalWidth * static_cast<float>(leftPixels) / static_cast<float>(pixelWidth);
        const float topHeight =
            logicalHeight * static_cast<float>(topPixels) / static_cast<float>(pixelHeight);
        const float rightWidth = logicalWidth - leftWidth;
        const float bottomHeight = logicalHeight - topHeight;
        result.sourceRects = {
            SurfaceRect{.x = 0.0F, .y = 0.0F, .width = leftWidth, .height = topHeight},
            SurfaceRect{.x = leftWidth, .y = 0.0F, .width = rightWidth, .height = topHeight},
            SurfaceRect{.x = 0.0F, .y = topHeight, .width = leftWidth, .height = bottomHeight},
        };
        result.sourceCount = 3U;
        result.differenceRect = SurfaceRect{
            .x = leftWidth,
            .y = topHeight,
            .width = rightWidth,
            .height = bottomHeight,
        };
        return result;
    }
    if (viewMode == SurfaceViewMode::ThreeUp) {
        const SurfaceColumnLayout columns =
            computeSurfaceColumns(logicalWidth, logicalHeight, pixelWidth, pixelHeight, 3U);
        result.sourceRects = columns.columns;
        result.sourceCount = columns.count;
        return result;
    }
    if (viewMode == SurfaceViewMode::ReferenceFocus) {
        const SurfaceFocusLayout focus =
            computeReferenceFocusLayout(logicalWidth, logicalHeight, pixelWidth, pixelHeight);
        if (!focus.main.isValid() || !focus.topRight.isValid() || !focus.bottomRight.isValid()) {
            return result;
        }
        result.sourceRects = {focus.main, focus.topRight, focus.bottomRight};
        result.sourceSlots[0U] = referenceSlot;
        std::size_t destination = 1U;
        for (std::uint8_t slot = 0U; slot < 3U; ++slot) {
            if (slot != referenceSlot) {
                result.sourceSlots[destination++] = slot;
            }
        }
        result.sourceCount = 3U;
        return result;
    }
    const SurfaceColumnLayout columns =
        computeSurfaceColumns(logicalWidth, logicalHeight, pixelWidth, pixelHeight, 2U);
    result.sourceRects = columns.columns;
    result.sourceCount = columns.count;
    return result;
}

namespace {

[[nodiscard]] SurfaceRect aspectFitRectFloat(const SurfaceRect& bounds,
                                             const float sourceWidth,
                                             const float sourceHeight) noexcept {
    if (!bounds.isValid() || !std::isfinite(bounds.x) || !std::isfinite(bounds.y) ||
        !std::isfinite(bounds.width) || !std::isfinite(bounds.height) || sourceWidth == 0U ||
        sourceHeight == 0U) {
        return {};
    }

    const float sourceAspect = sourceWidth / sourceHeight;
    const float boundsAspect = bounds.width / bounds.height;
    float fittedWidth = bounds.width;
    float fittedHeight = bounds.height;
    if (sourceAspect > boundsAspect) {
        fittedHeight = fittedWidth / sourceAspect;
    } else {
        fittedWidth = fittedHeight * sourceAspect;
    }

    return SurfaceRect{
        .x = bounds.x + ((bounds.width - fittedWidth) * 0.5F),
        .y = bounds.y + ((bounds.height - fittedHeight) * 0.5F),
        .width = fittedWidth,
        .height = fittedHeight,
    };
}

} // namespace

SurfaceRect aspectFitRect(const SurfaceRect& bounds,
                          const std::uint32_t sourceWidth,
                          const std::uint32_t sourceHeight) noexcept {
    return aspectFitRectFloat(
        bounds, static_cast<float>(sourceWidth), static_cast<float>(sourceHeight));
}

SurfacePresentationGeometry
computeSurfacePresentationGeometry(const SurfaceViewMode viewMode,
                                   const float logicalWidth,
                                   const float logicalHeight,
                                   const std::uint32_t pixelWidth,
                                   const std::uint32_t pixelHeight,
                                   const std::uint8_t referenceSlot,
                                   const SurfaceDifferenceEdge differenceEdge,
                                   const float wipePosition,
                                   const std::array<SurfaceDisplayExtent, 3U>& sourceDisplayExtents,
                                   const bool differenceSuppressed) noexcept {
    SurfacePresentationGeometry result;
    result.panels = computeSurfacePanelLayout(viewMode,
                                              logicalWidth,
                                              logicalHeight,
                                              pixelWidth,
                                              pixelHeight,
                                              referenceSlot,
                                              differenceEdge,
                                              wipePosition);
    const auto fitted = [&sourceDisplayExtents](const SurfaceRect& bounds,
                                                const std::uint8_t slot) {
        if (slot >= sourceDisplayExtents.size() || !sourceDisplayExtents[slot].isValid()) {
            return SurfaceRect{};
        }
        return aspectFitRectFloat(
            bounds, sourceDisplayExtents[slot].width, sourceDisplayExtents[slot].height);
    };
    for (std::size_t index = 0U; index < result.panels.sourceCount; ++index) {
        result.sourceContentRects[index] =
            fitted(result.panels.sourceRects[index], result.panels.sourceSlots[index]);
    }

    std::uint8_t firstDifferenceSlot = 0U;
    if (differenceEdge == SurfaceDifferenceEdge::Between1And2) {
        firstDifferenceSlot = 1U;
    }
    if (viewMode == SurfaceViewMode::Wipe) {
        const SurfaceRect full{
            .x = 0.0F, .y = 0.0F, .width = logicalWidth, .height = logicalHeight};
        result.wipeContentRect = fitted(full, firstDifferenceSlot);
        if (result.wipeContentRect->isValid()) {
            result.sourceContentRects[0U] = *result.wipeContentRect;
            result.sourceContentRects[1U] = *result.wipeContentRect;
        }
    } else if (result.panels.differenceRect.has_value()) {
        const std::uint8_t displayedSlot =
            differenceSuppressed ? referenceSlot : firstDifferenceSlot;
        result.differenceContentRect = fitted(*result.panels.differenceRect, displayedSlot);
    }
    return result;
}

std::optional<D3dScissorRect>
d3dScissorFromBottomLeft(const SurfaceScissorRect& scissor,
                         const SurfaceViewport& viewport,
                         const std::uint32_t renderTargetHeight) noexcept {
    if (!scissor.isValid() || !std::isfinite(viewport.topLeftX) ||
        !std::isfinite(viewport.topLeftY) || !std::isfinite(viewport.width) ||
        !std::isfinite(viewport.height) || viewport.width <= 0.0F || viewport.height <= 0.0F ||
        renderTargetHeight == 0U) {
        return std::nullopt;
    }

    const double viewportLeft = std::floor(static_cast<double>(viewport.topLeftX));
    const double viewportTop = std::floor(static_cast<double>(viewport.topLeftY));
    const double viewportRight = std::ceil(static_cast<double>(viewport.topLeftX) + viewport.width);
    const double viewportBottom =
        std::ceil(static_cast<double>(viewport.topLeftY) + viewport.height);
    if (viewportLeft >= viewportRight || viewportTop >= viewportBottom) {
        return std::nullopt;
    }

    const double left = std::clamp(static_cast<double>(scissor.left), viewportLeft, viewportRight);
    const double right =
        std::clamp(static_cast<double>(scissor.right), viewportLeft, viewportRight);
    const double flippedTop = std::clamp(
        static_cast<double>(renderTargetHeight) - scissor.top, viewportTop, viewportBottom);
    const double flippedBottom = std::clamp(
        static_cast<double>(renderTargetHeight) - scissor.bottom, viewportTop, viewportBottom);
    const auto toLong = [](const double value) {
        constexpr double minimum = static_cast<double>((std::numeric_limits<std::int32_t>::min)());
        constexpr double maximum = static_cast<double>((std::numeric_limits<std::int32_t>::max)());
        return static_cast<std::int32_t>(std::clamp(value, minimum, maximum));
    };

    const D3dScissorRect result{
        .left = toLong(std::floor(left)),
        .top = toLong(std::floor(flippedTop)),
        .right = toLong(std::ceil(right)),
        .bottom = toLong(std::ceil(flippedBottom)),
    };
    if (!result.isValid()) {
        return std::nullopt;
    }
    return result;
}

Nv12ColorTransform nv12ColorTransform(const domain::ColorMetadata& metadata,
                                      const std::uint8_t bitDepth) noexcept {
    const bool bt709 = metadata.matrix == domain::ColorMatrix::kBt709;
    const float redLuma = bt709 ? 0.2126F : 0.299F;
    const float blueLuma = bt709 ? 0.0722F : 0.114F;
    const float greenLuma = 1.0F - redLuma - blueLuma;

    const float redFromCr = 2.0F * (1.0F - redLuma);
    const float blueFromCb = 2.0F * (1.0F - blueLuma);
    const float greenFromCb = -2.0F * blueLuma * (1.0F - blueLuma) / greenLuma;
    const float greenFromCr = -2.0F * redLuma * (1.0F - redLuma) / greenLuma;

    const bool p010 = bitDepth == 10U;
    const bool limited = metadata.range == domain::ColorRange::kLimited;
    const float storageMaximum = p010 ? 65535.0F : 255.0F;
    const float codeScale = p010 ? 64.0F : 1.0F;
    const float codeMaximum = (p010 ? 1023.0F : 255.0F) * codeScale;
    const float yMinimum = (p010 ? 64.0F : 16.0F) * codeScale;
    const float yRange = (p010 ? 876.0F : 219.0F) * codeScale;
    const float chromaMidpoint = (p010 ? 512.0F : 128.0F) * codeScale;
    const float chromaRange = (p010 ? 896.0F : 224.0F) * codeScale;
    const float yScale = limited ? (storageMaximum / yRange) : (storageMaximum / codeMaximum);
    const float yOffset = limited ? (-yMinimum / yRange) : 0.0F;
    const float chromaScale =
        limited ? (storageMaximum / chromaRange) : (storageMaximum / codeMaximum);
    const float chromaOffset =
        limited ? (-chromaMidpoint / chromaRange) : (-chromaMidpoint / codeMaximum);

    return Nv12ColorTransform{
        .yuvToRgb =
            {
                yScale,
                0.0F,
                redFromCr * chromaScale,
                yOffset + (redFromCr * chromaOffset),
                yScale,
                greenFromCb * chromaScale,
                greenFromCr * chromaScale,
                yOffset + ((greenFromCb + greenFromCr) * chromaOffset),
                yScale,
                blueFromCb * chromaScale,
                0.0F,
                yOffset + (blueFromCb * chromaOffset),
            },
    };
}

class D3d11ComparisonRenderer::Impl final {
public:
    Impl(std::shared_ptr<GraphicsDeviceBroker> deviceBroker,
         std::shared_ptr<FrameMailbox> frameMailbox,
         std::shared_ptr<PresentationAckMailbox> acknowledgementMailbox,
         std::weak_ptr<IRenderActivitySink> activitySink) noexcept
        : deviceBroker_(std::move(deviceBroker)), frameMailbox_(std::move(frameMailbox)),
          acknowledgementMailbox_(std::move(acknowledgementMailbox)),
          activitySink_(std::move(activitySink)) {}

    [[nodiscard]] ComparisonRenderResult render(const SurfaceRenderState& state) noexcept {
        if (!state.isValid() || !deviceBroker_ || !frameMailbox_ || !acknowledgementMailbox_) {
            return ComparisonRenderResult::InvalidState;
        }

        retryPendingAcknowledgement();
        if (acknowledgementClosed_) {
            return ComparisonRenderResult::Closed;
        }

        const GraphicsDeviceLeaseResult leaseResult = deviceBroker_->tryLease();
        if (leaseResult.status == GraphicsDeviceLeaseStatus::Busy) {
            return ComparisonRenderResult::Contended;
        }
        if (leaseResult.status == GraphicsDeviceLeaseStatus::Closed) {
            return ComparisonRenderResult::Closed;
        }
        if (leaseResult.status != GraphicsDeviceLeaseStatus::Available ||
            !leaseResult.lease.has_value()) {
            return ComparisonRenderResult::DeviceUnavailable;
        }
        const GraphicsDeviceLease& lease = *leaseResult.lease;
        if (!ensureDeviceResources(lease)) {
            return ComparisonRenderResult::ResourceFailure;
        }
        if (!applyRenderState(state, *lease.immediateContext.Get())) {
            return ComparisonRenderResult::InvalidState;
        }

        // A full lossless ACK queue is presentation backpressure. Keep repainting the retained
        // front pair, but do not expose a later publication until the pending event is admitted.
        if (pendingAcknowledgement_.has_value()) {
            return drawFrontOrBackground(state, lease) ? ComparisonRenderResult::PresentedAckPending
                                                       : ComparisonRenderResult::ResourceFailure;
        }

        const FrameMailboxReadResult read = frameMailbox_->tryLatest(lease.deviceGeneration);
        switch (read.status) {
        case FrameMailboxReadStatus::Contended:
            return drawFrontOrBackground(state, lease) ? ComparisonRenderResult::Contended
                                                       : ComparisonRenderResult::ResourceFailure;
        case FrameMailboxReadStatus::Closed:
            return ComparisonRenderResult::Closed;
        case FrameMailboxReadStatus::DeviceGenerationMismatch:
            frontPublication_.reset();
            return ComparisonRenderResult::DeviceUnavailable;
        case FrameMailboxReadStatus::Empty:
            return drawFrontOrBackground(state, lease) ? ComparisonRenderResult::BackgroundOnly
                                                       : ComparisonRenderResult::ResourceFailure;
        case FrameMailboxReadStatus::Available:
            break;
        }
        if (!read.publication.has_value() || !read.publication->set) {
            return drawFrontOrBackground(state, lease) ? ComparisonRenderResult::BackgroundOnly
                                                       : ComparisonRenderResult::ResourceFailure;
        }

        const FrameMailboxPublication publication = *read.publication;
        if (frontPublication_.has_value() &&
            frontPublication_->publicationSerial == publication.publicationSerial &&
            frontPublication_->set == publication.set) {
            return drawFrontOrBackground(state, lease) ? ComparisonRenderResult::Presented
                                                       : ComparisonRenderResult::ResourceFailure;
        }

        if (const std::shared_ptr<IRenderActivitySink> sink = activitySink_.lock()) {
            sink->notifyFrameRenderStarted();
        }
        emitStagingTrace(application::TraceEventKind::RenderDrawStarted,
                         publication.set->frameId());
        PreparedSetDraw prepared;
        if (!prepareSetDraw(state, publication, lease, prepared)) {
            return frontPublication_.has_value() && drawFrontOrBackground(state, lease)
                       ? ComparisonRenderResult::BackgroundOnly
                       : ComparisonRenderResult::ResourceFailure;
        }

        // Replacement is checked after every fallible preparation and immediately before the
        // first command. The new pair replaces the pinned front only after both sides are issued.
        const FrameMailboxDrawStatus drawStatus =
            frameMailbox_->validateForDraw(publication, lease.deviceGeneration);
        if (drawStatus == FrameMailboxDrawStatus::Contended) {
            return drawFrontOrBackground(state, lease) ? ComparisonRenderResult::Contended
                                                       : ComparisonRenderResult::ResourceFailure;
        }
        if (drawStatus == FrameMailboxDrawStatus::Closed) {
            return ComparisonRenderResult::Closed;
        }
        if (drawStatus == FrameMailboxDrawStatus::DeviceGenerationMismatch) {
            frontPublication_.reset();
            return ComparisonRenderResult::DeviceUnavailable;
        }
        if (drawStatus == FrameMailboxDrawStatus::Superseded) {
            return drawFrontOrBackground(state, lease) ? ComparisonRenderResult::BackgroundOnly
                                                       : ComparisonRenderResult::ResourceFailure;
        }

        if (!drawPreparedSet(prepared, lease)) {
            return ComparisonRenderResult::ResourceFailure;
        }
        frontPublication_ = publication;
        return acknowledge(publication, *publication.set);
    }

    // The frame set that is actually on screen: the retained front publication, which render()
    // keeps repainting while a mailbox clear or presentation-acknowledgement backpressure holds a
    // newer publication back. Nothing else qualifies - a publication the display path has not
    // admitted is not what the user is looking at, and resolving through the mailbox would also
    // cost a device lease on every frame of an empty session.
    [[nodiscard]] std::shared_ptr<const GpuFrameSet> presentedSet() const {
        if (frontPublication_.has_value() && frontPublication_->set != nullptr) {
            return frontPublication_->set;
        }
        return nullptr;
    }

    [[nodiscard]] PresentedFrameIdentity presentedFrameIdentity() const noexcept {
        PresentedFrameIdentity identity;
        const std::shared_ptr<const GpuFrameSet> set = presentedSet();
        if (set == nullptr) {
            return identity;
        }
        identity.valid = true;
        identity.frameId = set->frameId().value();
        identity.sessionEpoch = set->context().playback.request.sessionEpoch.value();
        identity.playbackGeneration = set->context().playback.playbackGeneration.value();
        return identity;
    }

    [[nodiscard]] std::vector<SourcePixelProbe>
    probeSourcePixels(const std::vector<SourcePixelProbeRequest>& requests) noexcept {
        std::vector<SourcePixelProbe> results;
        results.reserve(requests.size());
        if (requests.empty()) {
            return results;
        }
        // Resolve the set once per batch so A/B/C of one cursor position can never describe two
        // different frames, then lease once for the whole batch.
        // A retry may contain only the busy slots. Keep the original set until the caller
        // completes/cancels the batch; drawing a newer set must not change its answers.
        const bool continuesBatch =
            pixelProbeBatchSet_ &&
            std::all_of(requests.begin(), requests.end(), [this](const auto& request) {
                return std::any_of(pixelProbeBatchRequests_.begin(),
                                   pixelProbeBatchRequests_.end(),
                                   [&request](const auto& original) {
                                       return request.slot == original.slot &&
                                              request.requestId == original.requestId &&
                                              request.normalizedX == original.normalizedX &&
                                              request.normalizedY == original.normalizedY;
                                   });
            });
        if (!continuesBatch) {
            cancelSourcePixelProbes();
            pixelProbeBatchSet_ = presentedSet();
            pixelProbeBatchRequests_ = requests;
        }
        const std::shared_ptr<const GpuFrameSet> set = pixelProbeBatchSet_;
        if (set == nullptr) {
            for (const SourcePixelProbeRequest& request : requests) {
                SourcePixelProbe result;
                result.slot = request.slot;
                result.requestId = request.requestId;
                result.reason = SourcePixelProbe::Reason::NoFrame;
                results.push_back(result);
            }
            return results;
        }
        const GraphicsDeviceLeaseResult leaseResult = deviceBroker_->tryLease();
        if (leaseResult.status != GraphicsDeviceLeaseStatus::Available ||
            !leaseResult.lease.has_value()) {
            for (const SourcePixelProbeRequest& request : requests) {
                SourcePixelProbe result;
                result.slot = request.slot;
                result.requestId = request.requestId;
                result.identityValid = true;
                result.frameId = set->frameId().value();
                result.sessionEpoch = set->context().playback.request.sessionEpoch.value();
                result.playbackGeneration = set->context().playback.playbackGeneration.value();
                result.reason = SourcePixelProbe::Reason::DeviceBusy;
                results.push_back(result);
            }
            return results;
        }
        const GraphicsDeviceLease& lease = *leaseResult.lease;
        probeCopiesIssued_ = false;
        for (const SourcePixelProbeRequest& request : requests) {
            results.push_back(
                probeSlot(set, request, lease.device.Get(), lease.immediateContext.Get()));
        }
        if (probeCopiesIssued_) {
            // Submit the tiny copy batch even when Qt parks a paused window. Flush submits GPU
            // work asynchronously; completion is checked on a later pass with DO_NOT_WAIT.
            lease.immediateContext->Flush();
        }
        return results;
    }

    [[nodiscard]] SourcePixelProbe probeSourcePixel(const std::uint8_t slot,
                                                    const double normalizedX,
                                                    const double normalizedY) noexcept {
        const std::vector<SourcePixelProbe> probed = probeSourcePixels({SourcePixelProbeRequest{
            .slot = slot, .normalizedX = normalizedX, .normalizedY = normalizedY}});
        return probed.empty() ? SourcePixelProbe{} : probed.front();
    }

    void cancelSourcePixelProbes() noexcept {
        pixelProbeBatchSet_.reset();
        pixelProbeBatchRequests_.clear();
        for (auto& readback : pixelReadbacks_) {
            readback.set.reset();
            readback.luma.values.reset();
            readback.chroma.values.reset();
        }
    }

    void releaseResources() noexcept {
        // Drop the front publication before the device objects so its deferred-retirement
        // deleters can observe a still-valid broker/device generation during teardown.
        frontPublication_.reset();
        cancelSourcePixelProbes();
        pixelReadbacks_ = {};
        differenceColorBufferB_.Reset();
        differenceColorBufferA_.Reset();
        colorBufferC_.Reset();
        colorBufferB_.Reset();
        colorBufferA_.Reset();
        differenceBuffer_.Reset();
        differenceComposeBuffer_.Reset();
        composeBufferC_.Reset();
        composeBufferB_.Reset();
        composeBufferA_.Reset();
        for (ComPtr<ID3D11Buffer>& buffer : blackComposeBuffers_) {
            buffer.Reset();
        }
        linearSampler_.Reset();
        nearestSampler_.Reset();
        scissorRasterizerState_.Reset();
        rasterizerState_.Reset();
        stencilState_.Reset();
        depthState_.Reset();
        blendState_.Reset();
        differencePixelShader_.Reset();
        nv12PixelShader_.Reset();
        blackPixelShader_.Reset();
        vertexShader_.Reset();
        device_.Reset();
        deviceGeneration_ = domain::DeviceGeneration{0U};
    }

private:
    // One slot of an already resolved batch: reads the texel and stamps the identity of the set
    // the whole batch was resolved against.
    [[nodiscard]] SourcePixelProbe probeSlot(const std::shared_ptr<const GpuFrameSet>& set,
                                             const SourcePixelProbeRequest& request,
                                             ID3D11Device* const device,
                                             ID3D11DeviceContext* const context) noexcept {
        SourcePixelProbe result;
        result.slot = request.slot;
        result.requestId = request.requestId;
        result.identityValid = true;
        result.frameId = set->frameId().value();
        result.sessionEpoch = set->context().playback.request.sessionEpoch.value();
        result.playbackGeneration = set->context().playback.playbackGeneration.value();
        if (request.slot >= pixelReadbacks_.size()) {
            result.reason = SourcePixelProbe::Reason::MissingSlot;
            return result;
        }
        if (!std::isfinite(request.normalizedX) || !std::isfinite(request.normalizedY) ||
            request.normalizedX < 0.0 || request.normalizedX >= 1.0 || request.normalizedY < 0.0 ||
            request.normalizedY >= 1.0) {
            result.reason = SourcePixelProbe::Reason::OutOfRange;
            return result;
        }
        const GpuFrameSlot* const slotEntry = set->find(domain::SourceId{request.slot});
        if (slotEntry == nullptr || slotEntry->frame == nullptr) {
            result.reason = SourcePixelProbe::Reason::MissingSlot;
            return result;
        }
        const auto* const backing =
            dynamic_cast<const D3d11GpuFrameBacking*>(&slotEntry->frame->backing());
        if (backing == nullptr || backing->yTexture() == nullptr ||
            backing->uvTexture() == nullptr) {
            result.reason = SourcePixelProbe::Reason::OutOfRange;
            return result;
        }
        const D3d11PlaneDimensions& dimensions = backing->yDimensions();
        if (dimensions.width == 0U || dimensions.height == 0U) {
            result.reason = SourcePixelProbe::Reason::OutOfRange;
            return result;
        }
        const auto pixelX = static_cast<std::uint32_t>(
            std::clamp(std::floor(request.normalizedX * static_cast<double>(dimensions.width)),
                       0.0,
                       static_cast<double>(dimensions.width - 1U)));
        const auto pixelY = static_cast<std::uint32_t>(
            std::clamp(std::floor(request.normalizedY * static_cast<double>(dimensions.height)),
                       0.0,
                       static_cast<double>(dimensions.height - 1U)));
        result.x = pixelX;
        result.y = pixelY;
        PixelReadback& readback = pixelReadbacks_[request.slot];
        if (!readback.set || readback.set != set || readback.result.requestId != result.requestId ||
            readback.result.x != pixelX || readback.result.y != pixelY) {
            readback.set = set;
            readback.result = result;
            readback.luma.values.reset();
            readback.chroma.values.reset();
            if (!copyProbePlane(
                    device, context, backing->yTexture(), pixelX, pixelY, readback.luma) ||
                !copyProbePlane(device,
                                context,
                                backing->uvTexture(),
                                pixelX >> 1U,
                                pixelY >> 1U,
                                readback.chroma)) {
                readback.set.reset();
                result.reason = SourcePixelProbe::Reason::ReadbackFailed;
                return result;
            }
            probeCopiesIssued_ = true;
        }
        const HRESULT lumaStatus = pollProbePlane(context, readback.luma);
        const HRESULT chromaStatus = pollProbePlane(context, readback.chroma);
        if (lumaStatus == DXGI_ERROR_WAS_STILL_DRAWING ||
            chromaStatus == DXGI_ERROR_WAS_STILL_DRAWING) {
            result.reason = SourcePixelProbe::Reason::DeviceBusy;
            return result;
        }
        if (FAILED(lumaStatus) || FAILED(chromaStatus)) {
            readback.set.reset();
            result.reason = SourcePixelProbe::Reason::ReadbackFailed;
            return result;
        }
        result.luma = (*readback.luma.values)[0];
        result.cb = (*readback.chroma.values)[0];
        result.cr = (*readback.chroma.values)[1];
        result.bitDepth = readback.luma.format == DXGI_FORMAT_R8_UNORM ? 8U : 10U;
        result.available = true;
        readback.set.reset();
        return result;
    }

    struct ProbePlane final {
        ComPtr<ID3D11Texture2D> staging;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        std::optional<std::array<std::uint32_t, 2U>> values;
    };

    struct PixelReadback final {
        ProbePlane luma;
        ProbePlane chroma;
        std::shared_ptr<const GpuFrameSet> set;
        SourcePixelProbe result;
    };

    // Six 1x1 staging resources at most, reused across cursor positions. Copies are issued once
    // per request; subsequent render passes only poll with DO_NOT_WAIT and never stall the GPU.
    [[nodiscard]] static bool copyProbePlane(ID3D11Device* device,
                                             ID3D11DeviceContext* context,
                                             ID3D11Texture2D* source,
                                             std::uint32_t x,
                                             std::uint32_t y,
                                             ProbePlane& plane) {
        D3D11_TEXTURE2D_DESC sourceDescription{};
        source->GetDesc(&sourceDescription);
        const auto format = sourceDescription.Format;
        if (x >= sourceDescription.Width || y >= sourceDescription.Height ||
            (format != DXGI_FORMAT_R8_UNORM && format != DXGI_FORMAT_R8G8_UNORM &&
             format != DXGI_FORMAT_R16_UNORM && format != DXGI_FORMAT_R16G16_UNORM)) {
            return false;
        }
        if (!plane.staging || plane.format != format) {
            plane.staging.Reset();
            D3D11_TEXTURE2D_DESC description{};
            description.Width = 1U;
            description.Height = 1U;
            description.MipLevels = 1U;
            description.ArraySize = 1U;
            description.Format = format;
            description.SampleDesc.Count = 1U;
            description.Usage = D3D11_USAGE_STAGING;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(
                    device->CreateTexture2D(&description, nullptr, plane.staging.GetAddressOf()))) {
                return false;
            }
            plane.format = format;
        }
        const D3D11_BOX region{x, y, 0U, x + 1U, y + 1U, 1U};
        context->CopySubresourceRegion(plane.staging.Get(), 0U, 0U, 0U, 0U, source, 0U, &region);
        return true;
    }

    [[nodiscard]] static HRESULT pollProbePlane(ID3D11DeviceContext* context, ProbePlane& plane) {
        if (plane.values.has_value()) {
            return S_OK;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT status = context->Map(
            plane.staging.Get(), 0U, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (FAILED(status)) {
            return status;
        }
        std::array<std::uint32_t, 2U> values{};
        if (plane.format == DXGI_FORMAT_R8_UNORM || plane.format == DXGI_FORMAT_R8G8_UNORM) {
            const auto* const bytes = static_cast<const std::uint8_t*>(mapped.pData);
            values = {bytes[0], plane.format == DXGI_FORMAT_R8G8_UNORM ? bytes[1] : bytes[0]};
        } else {
            const auto* const words = static_cast<const std::uint16_t*>(mapped.pData);
            values = {static_cast<std::uint32_t>(words[0] >> 6),
                      static_cast<std::uint32_t>(
                          (plane.format == DXGI_FORMAT_R16G16_UNORM ? words[1] : words[0]) >> 6)};
        }
        context->Unmap(plane.staging.Get(), 0U);
        plane.values = values;
        return S_OK;
    }

    std::array<PixelReadback, 3U> pixelReadbacks_;
    std::shared_ptr<const GpuFrameSet> pixelProbeBatchSet_;
    std::vector<SourcePixelProbeRequest> pixelProbeBatchRequests_;
    bool probeCopiesIssued_ = false;

    [[nodiscard]] bool ensureDeviceResources(const GraphicsDeviceLease& lease) noexcept {
        if (device_ && device_.Get() == lease.device.Get() &&
            deviceGeneration_ == lease.deviceGeneration) {
            return true;
        }
        releaseResources();
        if (!lease.device || !lease.immediateContext) {
            return false;
        }

        device_ = lease.device;
        deviceGeneration_ = lease.deviceGeneration;
        HRESULT result = device_->CreateVertexShader(shaders::kComposeVertexShader.data(),
                                                     shaders::kComposeVertexShader.size(),
                                                     nullptr,
                                                     vertexShader_.GetAddressOf());
        if (FAILED(result)) {
            return failDevice(result, lease);
        }
        result = device_->CreatePixelShader(shaders::kBlackPixelShader.data(),
                                            shaders::kBlackPixelShader.size(),
                                            nullptr,
                                            blackPixelShader_.GetAddressOf());
        if (FAILED(result)) {
            return failDevice(result, lease);
        }
        result = device_->CreatePixelShader(shaders::kNv12PixelShader.data(),
                                            shaders::kNv12PixelShader.size(),
                                            nullptr,
                                            nv12PixelShader_.GetAddressOf());
        if (FAILED(result)) {
            return failDevice(result, lease);
        }
        result = device_->CreatePixelShader(shaders::kDifferencePixelShader.data(),
                                            shaders::kDifferencePixelShader.size(),
                                            nullptr,
                                            differencePixelShader_.GetAddressOf());
        if (FAILED(result)) {
            return failDevice(result, lease);
        }

        for (ComPtr<ID3D11Buffer>& buffer : blackComposeBuffers_) {
            result = createDynamicConstantBuffer(*device_.Get(), sizeof(ComposeConstants), buffer);
            if (FAILED(result)) {
                return failDevice(result, lease);
            }
        }
        if (FAILED(result = createDynamicConstantBuffer(
                       *device_.Get(), sizeof(ComposeConstants), composeBufferA_)) ||
            FAILED(result = createDynamicConstantBuffer(
                       *device_.Get(), sizeof(ComposeConstants), composeBufferB_)) ||
            FAILED(result = createDynamicConstantBuffer(
                       *device_.Get(), sizeof(ComposeConstants), composeBufferC_)) ||
            FAILED(result = createDynamicConstantBuffer(
                       *device_.Get(), sizeof(ComposeConstants), differenceComposeBuffer_)) ||
            FAILED(result = createDynamicConstantBuffer(
                       *device_.Get(), sizeof(ColorConstants), colorBufferA_)) ||
            FAILED(result = createDynamicConstantBuffer(
                       *device_.Get(), sizeof(ColorConstants), colorBufferB_)) ||
            FAILED(result = createDynamicConstantBuffer(
                       *device_.Get(), sizeof(ColorConstants), colorBufferC_)) ||
            FAILED(result = createDynamicConstantBuffer(
                       *device_.Get(), sizeof(ColorConstants), differenceColorBufferA_)) ||
            FAILED(result = createDynamicConstantBuffer(
                       *device_.Get(), sizeof(ColorConstants), differenceColorBufferB_)) ||
            FAILED(result = createDynamicConstantBuffer(
                       *device_.Get(), sizeof(DifferenceConstants), differenceBuffer_))) {
            return failDevice(result, lease);
        }

        D3D11_SAMPLER_DESC samplerDescription{};
        samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samplerDescription.MinLOD = 0.0F;
        samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
        samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        result = device_->CreateSamplerState(&samplerDescription, nearestSampler_.GetAddressOf());
        if (FAILED(result)) {
            return failDevice(result, lease);
        }
        samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        result = device_->CreateSamplerState(&samplerDescription, linearSampler_.GetAddressOf());
        if (FAILED(result)) {
            return failDevice(result, lease);
        }

        D3D11_BLEND_DESC blendDescription{};
        D3D11_RENDER_TARGET_BLEND_DESC& renderTarget = blendDescription.RenderTarget[0U];
        renderTarget.BlendEnable = TRUE;
        renderTarget.SrcBlend = D3D11_BLEND_ONE;
        renderTarget.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        renderTarget.BlendOp = D3D11_BLEND_OP_ADD;
        renderTarget.SrcBlendAlpha = D3D11_BLEND_ONE;
        renderTarget.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        renderTarget.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        renderTarget.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        result = device_->CreateBlendState(&blendDescription, blendState_.GetAddressOf());
        if (FAILED(result)) {
            return failDevice(result, lease);
        }

        D3D11_DEPTH_STENCIL_DESC depthDescription{};
        // Qt draws opaque UI before the alpha pass containing our DepthAwareRendering node.
        // Respect that depth buffer or the video overwrites opaque chrome above it. Keep writes
        // disabled: the scene graph owns stacking, and this node must not occlude later draws.
        depthDescription.DepthEnable = TRUE;
        depthDescription.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depthDescription.DepthFunc = D3D11_COMPARISON_LESS;
        result = device_->CreateDepthStencilState(&depthDescription, depthState_.GetAddressOf());
        if (FAILED(result)) {
            return failDevice(result, lease);
        }
        depthDescription.StencilEnable = TRUE;
        depthDescription.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
        depthDescription.StencilWriteMask = 0U;
        depthDescription.FrontFace = D3D11_DEPTH_STENCILOP_DESC{
            .StencilFailOp = D3D11_STENCIL_OP_KEEP,
            .StencilDepthFailOp = D3D11_STENCIL_OP_KEEP,
            .StencilPassOp = D3D11_STENCIL_OP_KEEP,
            .StencilFunc = D3D11_COMPARISON_EQUAL,
        };
        depthDescription.BackFace = depthDescription.FrontFace;
        result = device_->CreateDepthStencilState(&depthDescription, stencilState_.GetAddressOf());
        if (FAILED(result)) {
            return failDevice(result, lease);
        }

        D3D11_RASTERIZER_DESC rasterizerDescription{};
        rasterizerDescription.FillMode = D3D11_FILL_SOLID;
        rasterizerDescription.CullMode = D3D11_CULL_NONE;
        rasterizerDescription.DepthClipEnable = TRUE;
        result =
            device_->CreateRasterizerState(&rasterizerDescription, rasterizerState_.GetAddressOf());
        if (FAILED(result)) {
            return failDevice(result, lease);
        }
        rasterizerDescription.ScissorEnable = TRUE;
        result = device_->CreateRasterizerState(&rasterizerDescription,
                                                scissorRasterizerState_.GetAddressOf());
        if (FAILED(result)) {
            return failDevice(result, lease);
        }
        return true;
    }

    [[nodiscard]] bool failDevice(const HRESULT result, const GraphicsDeviceLease& lease) noexcept {
        HRESULT reason = lease.device ? lease.device->GetDeviceRemovedReason() : result;
        if (SUCCEEDED(reason) && isRemovedResult(result)) {
            reason = result;
        }
        if (FAILED(reason)) {
            static_cast<void>(deviceBroker_->reportDeviceLost(lease.deviceGeneration, reason));
        }
        releaseResources();
        return false;
    }

    [[nodiscard]] bool applyRenderState(const SurfaceRenderState& state,
                                        ID3D11DeviceContext& context) const noexcept {
        std::optional<D3dScissorRect> convertedScissor;
        if (state.scissorEnabled) {
            UINT viewportCount = 1U;
            D3D11_VIEWPORT viewport{};
            context.RSGetViewports(&viewportCount, &viewport);
            if (viewportCount == 0U) {
                return false;
            }

            ComPtr<ID3D11RenderTargetView> renderTarget;
            context.OMGetRenderTargets(1U, renderTarget.GetAddressOf(), nullptr);
            ComPtr<ID3D11Resource> renderTargetResource;
            if (renderTarget) {
                renderTarget->GetResource(renderTargetResource.GetAddressOf());
            }
            ComPtr<ID3D11Texture2D> renderTargetTexture;
            if (!renderTargetResource || FAILED(renderTargetResource.As(&renderTargetTexture)) ||
                !renderTargetTexture) {
                return false;
            }
            D3D11_TEXTURE2D_DESC renderTargetDescription{};
            renderTargetTexture->GetDesc(&renderTargetDescription);
            convertedScissor = d3dScissorFromBottomLeft(state.scissor,
                                                        SurfaceViewport{
                                                            .topLeftX = viewport.TopLeftX,
                                                            .topLeftY = viewport.TopLeftY,
                                                            .width = viewport.Width,
                                                            .height = viewport.Height,
                                                        },
                                                        renderTargetDescription.Height);
            if (!convertedScissor.has_value()) {
                return false;
            }
        }

        const float blendFactor[4U] = {0.0F, 0.0F, 0.0F, 0.0F};
        context.OMSetBlendState(blendState_.Get(), blendFactor, 0xFFFFFFFFU);
        context.OMSetDepthStencilState(
            state.stencilEnabled ? stencilState_.Get() : depthState_.Get(), state.stencilReference);
        context.RSSetState(state.scissorEnabled ? scissorRasterizerState_.Get()
                                                : rasterizerState_.Get());
        if (convertedScissor.has_value()) {
            const D3D11_RECT rectangle{
                .left = convertedScissor->left,
                .top = convertedScissor->top,
                .right = convertedScissor->right,
                .bottom = convertedScissor->bottom,
            };
            context.RSSetScissorRects(1U, &rectangle);
        }
        context.IASetInputLayout(nullptr);
        context.IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context.VSSetShader(vertexShader_.Get(), nullptr, 0U);
        context.HSSetShader(nullptr, nullptr, 0U);
        context.DSSetShader(nullptr, nullptr, 0U);
        context.GSSetShader(nullptr, nullptr, 0U);
        return true;
    }

    [[nodiscard]] bool drawBackground(const SurfaceRenderState& state,
                                      const GraphicsDeviceLease& lease) noexcept {
        const ComposeConstants constants = makeBlackConstants(state);
        const HRESULT result = writeConstantBuffer(
            *lease.immediateContext.Get(), *blackComposeBuffers_[0U].Get(), constants);
        if (FAILED(result)) {
            return failDevice(result, lease);
        }
        ID3D11Buffer* const composeBuffer = blackComposeBuffers_[0U].Get();
        lease.immediateContext->VSSetConstantBuffers(0U, 1U, &composeBuffer);
        lease.immediateContext->PSSetShader(blackPixelShader_.Get(), nullptr, 0U);
        lease.immediateContext->Draw(4U, 0U);
        return true;
    }

    [[nodiscard]] bool prepareSetDraw(const SurfaceRenderState& state,
                                      const FrameMailboxPublication& publication,
                                      const GraphicsDeviceLease& lease,
                                      PreparedSetDraw& prepared) noexcept {
        if (!publication.set) {
            return false;
        }
        const GpuFrameSet& set = *publication.set;
        prepared.videoFilter = state.videoFilter;

        // Extract up to three slots; slots without a frame stay black (the render target is
        // cleared to black before the set is drawn).
        // GPU transfer omits Missing entries, so vector position is not the source slot. Resolve
        // by the stable source IDs assigned at open time or a missing source would shift every
        // later panel to the left and make the explicit black gap disappear.
        const GpuFrameSlot* const slotA = set.find(0U);
        const GpuFrameSlot* const slotB = set.find(1U);
        const GpuFrameSlot* const slotC = set.find(2U);
        const std::shared_ptr<const GpuFrameResource> frameA = slotA ? slotA->frame : nullptr;
        const std::shared_ptr<const GpuFrameResource> frameB = slotB ? slotB->frame : nullptr;
        const std::shared_ptr<const GpuFrameResource> frameC = slotC ? slotC->frame : nullptr;
        const D3d11GpuFrameBacking* const backingA = d3dBacking(frameA);
        const D3d11GpuFrameBacking* const backingB = d3dBacking(frameB);
        const D3d11GpuFrameBacking* const backingC = d3dBacking(frameC);

        // At least one frame must be present for rendering.
        if (!frameA && !frameB && !frameC) {
            return false;
        }

        const auto backingUsable = [](const GpuFrameResource* const frame,
                                      const D3d11GpuFrameBacking* const backing) {
            return frame != nullptr && backing != nullptr && backing->yView() != nullptr &&
                   backing->uvView() != nullptr;
        };
        SurfacePixelGrid pixelGrid;
        if (state.viewTransform.scale < 1.0F) {
            UINT viewportCount = 1U;
            D3D11_VIEWPORT viewport{};
            lease.immediateContext->RSGetViewports(&viewportCount, &viewport);
            if (viewportCount == 1U) {
                pixelGrid = surfacePixelGridFromClip(
                    state.clipFromItem,
                    SurfaceViewport{
                        viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height});
            }
        }
        const auto contentDestination = [&](const SurfaceRect& bounds,
                                            const GpuFrameResource& frame) {
            const auto [contentWidth, contentHeight] =
                transformedExtent(frame.geometry(), state.roiEnabled, state.roi);
            const SurfaceRect fitted = aspectFitRectFloat(bounds, contentWidth, contentHeight);
            const SurfaceNormalizedRect sample =
                effectiveSurfaceSampleRect(state.viewTransform, state.roiEnabled, state.roi);
            return transformedSurfaceContentRect(
                bounds,
                fitted,
                state.viewTransform,
                pixelGrid,
                orientedSurfaceSamplePixels(static_cast<float>(frame.geometry().width),
                                            static_cast<float>(frame.geometry().height),
                                            frame.geometry().presentation.rotationDegrees,
                                            sample),
                state.nominalDevicePixelRatio);
        };
        // Appends one aspect-fit draw into the given bounds; returns false on a degenerate fit.
        const auto appendRegionDraw = [&](const SurfaceRect& bounds,
                                          const GpuFrameResource& frame,
                                          const D3d11GpuFrameBacking& backing) {
            const SurfaceRect destination = contentDestination(bounds, frame);
            if (!destination.isValid() || prepared.videoDrawCount >= prepared.videoDraws.size()) {
                return false;
            }
            prepared.videoDraws[prepared.videoDrawCount] =
                makeVideoDraw(state, destination, frame, backing);
            ++prepared.videoDrawCount;
            appendLetterboxBars(state, bounds, destination, prepared);
            return true;
        };
        ID3D11DeviceContext& context = *lease.immediateContext.Get();
        HRESULT result = S_OK;
        const auto appendDifference = [&](const SurfaceRect& bounds) {
            if (state.differenceMetric == SurfaceDifferenceMetric::ExactPlanes &&
                !state.exactPlaneAvailable) {
                if (prepared.letterboxBarCount < prepared.letterboxBars.size()) {
                    prepared.letterboxBars[prepared.letterboxBarCount] =
                        makeComposeConstants(state, bounds, application::TextureRegion{});
                    ++prepared.letterboxBarCount;
                }
                return true;
            }
            const GpuFrameResource* edgeFirst = frameA.get();
            const D3d11GpuFrameBacking* edgeFirstBacking = backingA;
            const GpuFrameResource* edgeSecond = frameB.get();
            const D3d11GpuFrameBacking* edgeSecondBacking = backingB;
            if (state.differenceEdge == SurfaceDifferenceEdge::Between0And2) {
                edgeSecond = frameC.get();
                edgeSecondBacking = backingC;
            } else if (state.differenceEdge == SurfaceDifferenceEdge::Between1And2) {
                edgeFirst = frameB.get();
                edgeFirstBacking = backingB;
                edgeSecond = frameC.get();
                edgeSecondBacking = backingC;
            }
            const bool edgeFirstUsable = backingUsable(edgeFirst, edgeFirstBacking);
            const bool edgeSecondUsable = backingUsable(edgeSecond, edgeSecondBacking);
            if (!edgeFirstUsable || !edgeSecondUsable) {
                if (prepared.letterboxBarCount < prepared.letterboxBars.size()) {
                    prepared.letterboxBars[prepared.letterboxBarCount] =
                        makeComposeConstants(state, bounds, application::TextureRegion{});
                    ++prepared.letterboxBarCount;
                }
                return true;
            }
            // Hold-to-peek ("查看参考原图"): the difference pass is replaced by the reference
            // (GT) source of the session - the same slot the ReferenceFocus layout puts in the
            // main panel - drawn into the difference canvas with its own aspect fit and
            // letterboxing. Falling back to the pair's first source would show the candidate
            // under a label that promises the reference, so an unavailable reference slot draws
            // nothing instead of the wrong frame.
            if (state.differenceSuppressed) {
                const GpuFrameResource* peekFrame = nullptr;
                const D3d11GpuFrameBacking* peekBacking = nullptr;
                switch (state.referenceSlot) {
                case 0U:
                    peekFrame = frameA.get();
                    peekBacking = backingA;
                    break;
                case 1U:
                    peekFrame = frameB.get();
                    peekBacking = backingB;
                    break;
                case 2U:
                    peekFrame = frameC.get();
                    peekBacking = backingC;
                    break;
                default:
                    break;
                }
                if (peekFrame != nullptr && peekBacking != nullptr &&
                    backingUsable(peekFrame, peekBacking)) {
                    return appendRegionDraw(bounds, *peekFrame, *peekBacking);
                }
                if (prepared.letterboxBarCount < prepared.letterboxBars.size()) {
                    prepared.letterboxBars[prepared.letterboxBarCount] =
                        makeComposeConstants(state, bounds, application::TextureRegion{});
                    ++prepared.letterboxBarCount;
                }
                return true;
            }
            const SurfaceRect destination = contentDestination(bounds, *edgeFirst);
            if (!destination.isValid()) {
                return false;
            }
            prepared.hasDifference = true;
            prepared.differenceDraw = makeDifferenceDraw(
                state, destination, *edgeFirst, *edgeFirstBacking, *edgeSecond, *edgeSecondBacking);
            appendLetterboxBars(state, bounds, destination, prepared);
            return true;
        };
        const auto appendWipe = [&](const SurfaceRect& bounds) {
            const auto appendUnavailableBlack = [&](const SurfaceRect& rectangle) {
                if (!rectangle.isValid() ||
                    prepared.letterboxBarCount >= prepared.letterboxBars.size()) {
                    return false;
                }
                prepared.letterboxBars[prepared.letterboxBarCount] =
                    makeComposeConstants(state, rectangle, application::TextureRegion{});
                ++prepared.letterboxBarCount;
                return true;
            };
            const GpuFrameResource* leftFrame = frameA.get();
            const D3d11GpuFrameBacking* leftBacking = backingA;
            const GpuFrameResource* rightFrame = frameB.get();
            const D3d11GpuFrameBacking* rightBacking = backingB;
            if (state.differenceEdge == SurfaceDifferenceEdge::Between0And2) {
                rightFrame = frameC.get();
                rightBacking = backingC;
            } else if (state.differenceEdge == SurfaceDifferenceEdge::Between1And2) {
                leftFrame = frameB.get();
                leftBacking = backingB;
                rightFrame = frameC.get();
                rightBacking = backingC;
            }
            const bool leftUsable = backingUsable(leftFrame, leftBacking);
            const bool rightUsable = backingUsable(rightFrame, rightBacking);
            if (!leftUsable && !rightUsable) {
                if (prepared.letterboxBarCount < prepared.letterboxBars.size()) {
                    prepared.letterboxBars[prepared.letterboxBarCount] =
                        makeComposeConstants(state, bounds, application::TextureRegion{});
                    ++prepared.letterboxBarCount;
                }
                return true;
            }

            const SurfaceRect destination =
                contentDestination(bounds, *(leftUsable ? leftFrame : rightFrame));
            if (!destination.isValid()) {
                return false;
            }
            appendLetterboxBars(state, bounds, destination, prepared);

            const float position = std::clamp(state.wipePosition, 0.0F, 1.0F);
            // Wipe is positioned in the full presentation surface, not in the aspect-fitted
            // content rect. This keeps the renderer, handle, and hit mapping on one logical
            // coordinate system even while the video is letterboxed.
            const float splitX = bounds.x + bounds.width * position;
            const float leftWidth = std::clamp(splitX - destination.x, 0.0F, destination.width);
            const float contentPosition = leftWidth / destination.width;
            if (!leftUsable && leftWidth > 0.0F &&
                !appendUnavailableBlack(SurfaceRect{
                    .x = destination.x,
                    .y = destination.y,
                    .width = leftWidth,
                    .height = destination.height,
                })) {
                return false;
            }
            if (leftUsable && leftWidth > 0.0F) {
                VideoDraw left = makeVideoDraw(state, destination, *leftFrame, *leftBacking);
                left.compose.destinationRect[2U] = leftWidth;
                left.compose.displayUvRect = {0.0F, 0.0F, contentPosition, 1.0F};
                prepared.videoDraws[prepared.videoDrawCount] = std::move(left);
                ++prepared.videoDrawCount;
            }
            const float rightWidth = destination.width - leftWidth;
            if (!rightUsable && rightWidth > 0.0F &&
                !appendUnavailableBlack(SurfaceRect{
                    .x = destination.x + leftWidth,
                    .y = destination.y,
                    .width = rightWidth,
                    .height = destination.height,
                })) {
                return false;
            }
            if (rightUsable && rightWidth > 0.0F) {
                VideoDraw right = makeVideoDraw(state, destination, *rightFrame, *rightBacking);
                right.compose.destinationRect[0U] = destination.x + leftWidth;
                right.compose.destinationRect[2U] = rightWidth;
                right.compose.displayUvRect = {
                    contentPosition,
                    0.0F,
                    1.0F - contentPosition,
                    1.0F,
                };
                prepared.videoDraws[prepared.videoDrawCount] = std::move(right);
                ++prepared.videoDrawCount;
            }
            return true;
        };

        prepared.publication = publication;
        if (state.viewMode == SurfaceViewMode::Difference ||
            state.viewMode == SurfaceViewMode::Fade) {
            // Difference and Fade never degrades to a single-source image. If either selected
            // slot is unavailable, draw an explicit black unavailable canvas; the QML projection
            // names the missing source and canonical frame above it.
            if (!appendDifference(SurfaceRect{
                    .x = 0.0F,
                    .y = 0.0F,
                    .width = state.logicalWidth,
                    .height = state.logicalHeight,
                })) {
                return false;
            }
        } else if (state.viewMode == SurfaceViewMode::Wipe) {
            if (!appendWipe(SurfaceRect{
                    .x = 0.0F,
                    .y = 0.0F,
                    .width = state.logicalWidth,
                    .height = state.logicalHeight,
                })) {
                return false;
            }
        } else {
            // Slot views (USERPLAN 6.3): two-up (two columns), three-up (three columns), or
            // reference focus (slot 0 large on the left, slots 1 and 2 stacked on the right).
            // Slots without a frame stay black via the background clear.
            const SurfacePanelLayout layout = computeSurfacePanelLayout(state.viewMode,
                                                                        state.logicalWidth,
                                                                        state.logicalHeight,
                                                                        state.pixelWidth,
                                                                        state.pixelHeight,
                                                                        state.referenceSlot,
                                                                        state.differenceEdge,
                                                                        state.wipePosition);
            const auto& regions = layout.sourceRects;
            const auto& regionSlots = layout.sourceSlots;
            const std::size_t regionCount = layout.sourceCount;

            const std::array<const GpuFrameResource*, 3U> slotFrames{
                frameA.get(), frameB.get(), frameC.get()};
            const std::array<const D3d11GpuFrameBacking*, 3U> slotBackings{
                backingA, backingB, backingC};
            for (std::size_t region = 0U; region < regionCount; ++region) {
                const std::size_t slot = regionSlots[region];
                if (!backingUsable(slotFrames[slot], slotBackings[slot])) {
                    continue;
                }
                if (!appendRegionDraw(regions[region], *slotFrames[slot], *slotBackings[slot])) {
                    return false;
                }
            }
            if (layout.differenceRect.has_value() && !appendDifference(*layout.differenceRect)) {
                return false;
            }
        }
        for (std::size_t index = 0U; SUCCEEDED(result) && index < prepared.letterboxBarCount;
             ++index) {
            result = writeConstantBuffer(
                context, *blackComposeBuffers_[index].Get(), prepared.letterboxBars[index]);
        }
        return SUCCEEDED(result) || failDevice(result, lease);
    }

    [[nodiscard]] bool drawPreparedSet(const PreparedSetDraw& prepared,
                                       const GraphicsDeviceLease& lease) noexcept {
        ID3D11DeviceContext& context = *lease.immediateContext.Get();
        context.PSSetShader(blackPixelShader_.Get(), nullptr, 0U);
        for (std::size_t index = 0U; index < prepared.letterboxBarCount; ++index) {
            ID3D11Buffer* const composeBuffer = blackComposeBuffers_[index].Get();
            context.VSSetConstantBuffers(0U, 1U, &composeBuffer);
            context.Draw(4U, 0U);
        }

        if (prepared.videoDrawCount != 0U) {
            if (!drawVideoSlots(context, prepared, lease)) {
                const std::array<ID3D11ShaderResourceView*, 4U> nullViews{};
                context.PSSetShaderResources(
                    0U, static_cast<UINT>(nullViews.size()), nullViews.data());
                return false;
            }
        }
        if (prepared.hasDifference) {
            if (!drawDifference(context, prepared.differenceDraw, lease)) {
                const std::array<ID3D11ShaderResourceView*, 4U> nullViews{};
                context.PSSetShaderResources(
                    0U, static_cast<UINT>(nullViews.size()), nullViews.data());
                return false;
            }
        }
        const std::array<ID3D11ShaderResourceView*, 4U> nullViews{};
        context.PSSetShaderResources(0U, static_cast<UINT>(nullViews.size()), nullViews.data());
        return true;
    }

    [[nodiscard]] bool drawVideoSlots(ID3D11DeviceContext& context,
                                      const PreparedSetDraw& prepared,
                                      const GraphicsDeviceLease& lease) noexcept {
        context.PSSetShader(nv12PixelShader_.Get(), nullptr, 0U);
        // Point sampling above 200% zoom is what keeps pixels legible as pixels; bilinear
        // would average neighbouring texels and hide exactly the defects under review.
        ID3D11SamplerState* const sampler =
            prepared.videoFilter == SurfaceDifferenceFilter::Bilinear ? linearSampler_.Get()
                                                                      : nearestSampler_.Get();
        context.PSSetSamplers(0U, 1U, &sampler);

        for (std::size_t index = 0U; index < prepared.videoDrawCount; ++index) {
            const VideoDraw& draw = prepared.videoDraws[index];
            if (draw.yView == nullptr || draw.uvView == nullptr) {
                continue;
            }
            // Each queued draw owns a distinct constant buffer. Reusing A for the third draw
            // would overwrite draw zero before the GPU consumed it on deferred WARP/hardware
            // execution, making two panels show the same source.
            const std::array<ID3D11Buffer*, 3U> composeBuffers{
                composeBufferA_.Get(), composeBufferB_.Get(), composeBufferC_.Get()};
            const std::array<ID3D11Buffer*, 3U> colorBuffers{
                colorBufferA_.Get(), colorBufferB_.Get(), colorBufferC_.Get()};
            ID3D11Buffer* const composeBuffer = composeBuffers[index];
            ID3D11Buffer* const colorBuffer = colorBuffers[index];
            HRESULT result = writeConstantBuffer(context, *composeBuffer, draw.compose);
            if (FAILED(result)) {
                return failDevice(result, lease);
            }
            result = writeConstantBuffer(context, *colorBuffer, draw.color);
            if (FAILED(result)) {
                return failDevice(result, lease);
            }
            const std::array<ID3D11ShaderResourceView*, 2U> views{draw.yView, draw.uvView};
            context.VSSetConstantBuffers(0U, 1U, &composeBuffer);
            context.PSSetConstantBuffers(1U, 1U, &colorBuffer);
            context.PSSetShaderResources(0U, static_cast<UINT>(views.size()), views.data());
            context.Draw(4U, 0U);
        }
        return true;
    }

    [[nodiscard]] bool drawDifference(ID3D11DeviceContext& context,
                                      const DifferenceDraw& draw,
                                      const GraphicsDeviceLease& lease) noexcept {
        HRESULT result =
            writeConstantBuffer(context, *differenceComposeBuffer_.Get(), draw.compose);
        if (FAILED(result)) {
            return failDevice(result, lease);
        }
        result = writeConstantBuffer(context, *differenceColorBufferA_.Get(), draw.colorA);
        if (FAILED(result)) {
            return failDevice(result, lease);
        }
        result = writeConstantBuffer(context, *differenceColorBufferB_.Get(), draw.colorB);
        if (FAILED(result)) {
            return failDevice(result, lease);
        }
        result = writeConstantBuffer(context, *differenceBuffer_.Get(), draw.options);
        if (FAILED(result)) {
            return failDevice(result, lease);
        }
        context.PSSetShader(differencePixelShader_.Get(), nullptr, 0U);
        ID3D11SamplerState* const sampler = draw.filter == SurfaceDifferenceFilter::Bilinear
                                                ? linearSampler_.Get()
                                                : nearestSampler_.Get();
        context.PSSetSamplers(0U, 1U, &sampler);

        ID3D11Buffer* const composeBuffer = differenceComposeBuffer_.Get();
        const std::array<ID3D11Buffer*, 3U> pixelBuffers{
            differenceColorBufferA_.Get(), differenceColorBufferB_.Get(), differenceBuffer_.Get()};
        const std::array<ID3D11ShaderResourceView*, 4U> views{
            draw.yViewA, draw.uvViewA, draw.yViewB, draw.uvViewB};
        context.VSSetConstantBuffers(0U, 1U, &composeBuffer);
        context.PSSetConstantBuffers(
            1U, static_cast<UINT>(pixelBuffers.size()), pixelBuffers.data());
        context.PSSetShaderResources(0U, static_cast<UINT>(views.size()), views.data());
        context.Draw(4U, 0U);
        return true;
    }

    [[nodiscard]] bool drawFrontOrBackground(const SurfaceRenderState& state,
                                             const GraphicsDeviceLease& lease) noexcept {
        if (!frontPublication_.has_value()) {
            return drawBackground(state, lease);
        }

        PreparedSetDraw prepared;
        if (!prepareSetDraw(state, *frontPublication_, lease, prepared)) {
            return false;
        }
        return drawPreparedSet(prepared, lease);
    }

    void retryPendingAcknowledgement() noexcept {
        if (!pendingAcknowledgement_.has_value() || acknowledgementClosed_) {
            return;
        }
        const PresentationAckPushResult result =
            acknowledgementMailbox_->tryPush(pendingAcknowledgement_->event);
        if (result == PresentationAckPushResult::Accepted) {
            emitStagingTrace(application::TraceEventKind::RenderAckPublished,
                             pendingAcknowledgement_->event.frameId);
            pendingAcknowledgement_.reset();
            notifyAcknowledgementPublished();
        } else if (result == PresentationAckPushResult::Closed) {
            pendingAcknowledgement_.reset();
            acknowledgementClosed_ = true;
        }
    }

    // Additive render-thread observation. The renderer carries no playback identity, so the event
    // uses the default scope and the canonical frame id as its payload; an analyzer correlates it
    // with RenderPublished/PresentationAcknowledged by timestamp and frame. Disabled tracing
    // leaves this as one atomic load and a branch.
    static void emitStagingTrace(const application::TraceEventKind kind,
                                 const domain::FrameId frameId) noexcept {
        application::PlaybackTrace& trace = application::PlaybackTrace::instance();
        if (!trace.enabled()) {
            return;
        }
        trace.record(
            kind, application::TraceIdentity{}, static_cast<std::uint64_t>(frameId.value()));
    }

    [[nodiscard]] ComparisonRenderResult acknowledge(const FrameMailboxPublication& publication,
                                                     const GpuFrameSet& set) noexcept {
        if (acknowledgementClosed_) {
            return ComparisonRenderResult::Closed;
        }
        if (publication.publicationSerial <= highestAcknowledgementSerial_) {
            return pendingAcknowledgement_.has_value() ? ComparisonRenderResult::PresentedAckPending
                                                       : ComparisonRenderResult::Presented;
        }
        if (pendingAcknowledgement_.has_value()) {
            return ComparisonRenderResult::PresentedAckPending;
        }

        const application::FrameSetPresented event{
            .context = set.context(),
            .frameId = set.frameId(),
        };
        highestAcknowledgementSerial_ = publication.publicationSerial;
        const PresentationAckPushResult result = acknowledgementMailbox_->tryPush(event);
        if (result == PresentationAckPushResult::Accepted) {
            emitStagingTrace(application::TraceEventKind::RenderAckPublished, set.frameId());
            notifyAcknowledgementPublished();
            return ComparisonRenderResult::Presented;
        }
        if (result == PresentationAckPushResult::Full) {
            pendingAcknowledgement_ = PendingAcknowledgement{
                .publicationSerial = publication.publicationSerial,
                .event = event,
            };
            if (const std::shared_ptr<IRenderActivitySink> sink = activitySink_.lock()) {
                sink->notifyAckBackpressured();
            }
            return ComparisonRenderResult::PresentedAckPending;
        }
        acknowledgementClosed_ = true;
        return ComparisonRenderResult::Closed;
    }

    void notifyAcknowledgementPublished() noexcept {
        if (const std::shared_ptr<IRenderActivitySink> sink = activitySink_.lock()) {
            sink->notifyAckPublished();
        }
    }

    struct PendingAcknowledgement final {
        std::uint64_t publicationSerial = 0U;
        application::FrameSetPresented event;
    };

    std::shared_ptr<GraphicsDeviceBroker> deviceBroker_;
    std::shared_ptr<FrameMailbox> frameMailbox_;
    std::shared_ptr<PresentationAckMailbox> acknowledgementMailbox_;
    std::weak_ptr<IRenderActivitySink> activitySink_;
    std::optional<FrameMailboxPublication> frontPublication_;
    std::optional<PendingAcknowledgement> pendingAcknowledgement_;
    std::uint64_t highestAcknowledgementSerial_ = 0U;
    bool acknowledgementClosed_ = false;

    domain::DeviceGeneration deviceGeneration_{0U};
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> blackPixelShader_;
    ComPtr<ID3D11PixelShader> nv12PixelShader_;
    ComPtr<ID3D11PixelShader> differencePixelShader_;
    ComPtr<ID3D11BlendState> blendState_;
    ComPtr<ID3D11DepthStencilState> depthState_;
    ComPtr<ID3D11DepthStencilState> stencilState_;
    ComPtr<ID3D11RasterizerState> rasterizerState_;
    ComPtr<ID3D11RasterizerState> scissorRasterizerState_;
    ComPtr<ID3D11SamplerState> nearestSampler_;
    ComPtr<ID3D11SamplerState> linearSampler_;
    std::array<ComPtr<ID3D11Buffer>, 16U> blackComposeBuffers_;
    ComPtr<ID3D11Buffer> composeBufferA_;
    ComPtr<ID3D11Buffer> composeBufferB_;
    ComPtr<ID3D11Buffer> composeBufferC_;
    ComPtr<ID3D11Buffer> differenceComposeBuffer_;
    ComPtr<ID3D11Buffer> colorBufferA_;
    ComPtr<ID3D11Buffer> colorBufferB_;
    ComPtr<ID3D11Buffer> colorBufferC_;
    ComPtr<ID3D11Buffer> differenceColorBufferA_;
    ComPtr<ID3D11Buffer> differenceColorBufferB_;
    ComPtr<ID3D11Buffer> differenceBuffer_;
};

D3d11ComparisonRenderer::D3d11ComparisonRenderer(
    std::shared_ptr<GraphicsDeviceBroker> deviceBroker,
    std::shared_ptr<FrameMailbox> frameMailbox,
    std::shared_ptr<PresentationAckMailbox> acknowledgementMailbox,
    std::weak_ptr<IRenderActivitySink> activitySink)
    : impl_(std::make_unique<Impl>(std::move(deviceBroker),
                                   std::move(frameMailbox),
                                   std::move(acknowledgementMailbox),
                                   std::move(activitySink))) {}

D3d11ComparisonRenderer::~D3d11ComparisonRenderer() = default;

ComparisonRenderResult D3d11ComparisonRenderer::render(const SurfaceRenderState& state) noexcept {
    return impl_->render(state);
}

std::vector<SourcePixelProbe> D3d11ComparisonRenderer::probeSourcePixels(
    const std::vector<SourcePixelProbeRequest>& requests) noexcept {
    return impl_->probeSourcePixels(requests);
}

SourcePixelProbe D3d11ComparisonRenderer::probeSourcePixel(const std::uint8_t slot,
                                                           const double normalizedX,
                                                           const double normalizedY) noexcept {
    return impl_->probeSourcePixel(slot, normalizedX, normalizedY);
}

PresentedFrameIdentity D3d11ComparisonRenderer::presentedFrameIdentity() const noexcept {
    return impl_->presentedFrameIdentity();
}

void D3d11ComparisonRenderer::cancelSourcePixelProbes() noexcept {
    impl_->cancelSourcePixelProbes();
}

void D3d11ComparisonRenderer::releaseResources() noexcept {
    impl_->releaseResources();
}

} // namespace dvs::platform
