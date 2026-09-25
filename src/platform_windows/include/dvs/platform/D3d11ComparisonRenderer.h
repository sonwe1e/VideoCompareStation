#pragma once

#include "dvs/domain/MediaDescriptor.h"
#include "dvs/presentation/ComparisonContract.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace dvs::platform {

class FrameMailbox;
class GraphicsDeviceBroker;
class IRenderActivitySink;
class PresentationAckMailbox;

struct SurfaceRect final {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return width > 0.0F && height > 0.0F;
    }

    [[nodiscard]] constexpr bool operator==(const SurfaceRect&) const = default;
};

struct SurfaceScissorRect final {
    std::int32_t left = 0;
    std::int32_t bottom = 0;
    std::int32_t right = 0;
    std::int32_t top = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return left < right && bottom < top;
    }

    [[nodiscard]] constexpr bool operator==(const SurfaceScissorRect&) const = default;
};

struct SurfaceViewport final {
    float topLeftX = 0.0F;
    float topLeftY = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
};

struct D3dScissorRect final {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return left < right && top < bottom;
    }

    [[nodiscard]] constexpr bool operator==(const D3dScissorRect&) const = default;
};

using SurfaceViewMode = presentation::ViewMode;
using SurfaceDifferenceMetric = presentation::DifferenceMetric;
using SurfaceDifferenceGain = presentation::DifferenceGain;
using SurfaceDifferenceEdge = presentation::DifferenceEdge;
using SurfaceDifferenceFilter = presentation::DifferenceFilter;
using SurfaceThresholdPolicy = presentation::ThresholdPolicy;

struct SurfaceViewTransform final {
    float centerX = 0.5F;
    float centerY = 0.5F;
    float scale = 1.0F;

    [[nodiscard]] bool isValid() const noexcept;
};

struct SurfaceNormalizedRect final {
    float left = 0.0F;
    float top = 0.0F;
    float right = 1.0F;
    float bottom = 1.0F;

    [[nodiscard]] bool isValid() const noexcept;
};

[[nodiscard]] SurfaceNormalizedRect
effectiveSurfaceSampleRect(const SurfaceViewTransform& transform,
                           bool roiEnabled,
                           const SurfaceNormalizedRect& roi) noexcept;

// Qt-facing code snapshots its public scene-graph state into this native-type-free value. The
// matrix is row-major and maps item-local logical coordinates directly to clip space.
struct SurfaceRenderState final {
    std::array<float, 16U> clipFromItem{};
    float logicalWidth = 0.0F;
    float logicalHeight = 0.0F;
    std::uint32_t pixelWidth = 0U;
    std::uint32_t pixelHeight = 0U;
    float opacity = 1.0F;
    bool scissorEnabled = false;
    SurfaceScissorRect scissor;
    bool stencilEnabled = false;
    std::uint32_t stencilReference = 0U;
    SurfaceViewMode viewMode = SurfaceViewMode::SideBySide;
    SurfaceDifferenceMetric differenceMetric = SurfaceDifferenceMetric::RgbAbsolute;
    SurfaceDifferenceGain differenceGain = SurfaceDifferenceGain::Gain1x;
    SurfaceDifferenceEdge differenceEdge = SurfaceDifferenceEdge::Between0And1;
    SurfaceDifferenceFilter differenceFilter = SurfaceDifferenceFilter::Bilinear;
    float wipePosition = 0.5F;
    bool exactPlaneAvailable = false;
    bool thresholdEnabled = false;
    float threshold = 0.0F;
    SurfaceThresholdPolicy thresholdPolicy = SurfaceThresholdPolicy::AnyChannel;
    // Hold-to-peek ("按住看原图"): while true the difference pass is replaced by the raw
    // first source of the active pair, aspect-fit into the same rect. Transient UI state;
    // never persisted.
    bool differenceSuppressed = false;
    SurfaceViewTransform viewTransform;
    bool roiEnabled = false;
    SurfaceNormalizedRect roi;
    std::uint8_t referenceSlot = 0U;

    [[nodiscard]] bool isValid() const noexcept;
};

struct SurfaceSplitLayout final {
    std::uint32_t leftPixelWidth = 0U;
    std::uint32_t rightPixelWidth = 0U;
    SurfaceRect left;
    SurfaceRect right;
};

struct SurfacePanelLayout final {
    std::array<SurfaceRect, 3U> sourceRects{};
    std::array<std::uint8_t, 3U> sourceSlots{0U, 1U, 2U};
    std::size_t sourceCount = 0U;
    std::optional<SurfaceRect> differenceRect;
};

struct SurfaceDisplayExtent final {
    float width = 0.0F;
    float height = 0.0F;

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return width > 0.0F && height > 0.0F;
    }
};

struct SurfacePresentationGeometry final {
    SurfacePanelLayout panels;
    std::array<SurfaceRect, 3U> sourceContentRects{};
    std::optional<SurfaceRect> differenceContentRect;
    std::optional<SurfaceRect> wipeContentRect;
};

struct Nv12ColorTransform final {
    // Row-major float3x4 mapping normalized {Y, U, V, 1} samples to RGB.
    std::array<float, 12U> yuvToRgb{};

    [[nodiscard]] constexpr bool operator==(const Nv12ColorTransform&) const = default;
};

[[nodiscard]] SurfaceSplitLayout computeSurfaceSplit(float logicalWidth,
                                                     float logicalHeight,
                                                     std::uint32_t pixelWidth,
                                                     std::uint32_t pixelHeight) noexcept;

[[nodiscard]] SurfacePanelLayout computeSurfacePanelLayout(SurfaceViewMode viewMode,
                                                           float logicalWidth,
                                                           float logicalHeight,
                                                           std::uint32_t pixelWidth,
                                                           std::uint32_t pixelHeight,
                                                           std::uint8_t referenceSlot,
                                                           SurfaceDifferenceEdge differenceEdge,
                                                           float wipePosition) noexcept;

[[nodiscard]] SurfaceRect aspectFitRect(const SurfaceRect& bounds,
                                        std::uint32_t sourceWidth,
                                        std::uint32_t sourceHeight) noexcept;

[[nodiscard]] SurfacePresentationGeometry computeSurfacePresentationGeometry(
    SurfaceViewMode viewMode,
    float logicalWidth,
    float logicalHeight,
    std::uint32_t pixelWidth,
    std::uint32_t pixelHeight,
    std::uint8_t referenceSlot,
    SurfaceDifferenceEdge differenceEdge,
    float wipePosition,
    const std::array<SurfaceDisplayExtent, 3U>& sourceDisplayExtents) noexcept;

// Qt scene-graph scissor coordinates use the render target's bottom-left origin. D3D11 RECT uses
// its top-left origin; conversion flips by target height, then intersects the active viewport.
[[nodiscard]] std::optional<D3dScissorRect>
d3dScissorFromBottomLeft(const SurfaceScissorRect& scissor,
                         const SurfaceViewport& viewport,
                         std::uint32_t renderTargetHeight) noexcept;

[[nodiscard]] Nv12ColorTransform nv12ColorTransform(const domain::ColorMetadata& metadata,
                                                    std::uint8_t bitDepth = 8U) noexcept;

enum class ComparisonRenderResult {
    Presented,
    PresentedAckPending,
    BackgroundOnly,
    Contended,
    DeviceUnavailable,
    InvalidState,
    ResourceFailure,
    Closed,
};

// Render-thread-only D3D11 compositor. It borrows the current Qt render pass: render() never
// changes render targets or viewports and never begins an external-command section. The renderer
// receives a GpuFrameSet; side-by-side view draws the first two slots' textures left/right, and
// difference view computes the diff between the first two slots.
class D3d11ComparisonRenderer final {
public:
    D3d11ComparisonRenderer(std::shared_ptr<GraphicsDeviceBroker> deviceBroker,
                            std::shared_ptr<FrameMailbox> frameMailbox,
                            std::shared_ptr<PresentationAckMailbox> acknowledgementMailbox,
                            std::weak_ptr<IRenderActivitySink> activitySink);
    ~D3d11ComparisonRenderer();

    D3d11ComparisonRenderer(const D3d11ComparisonRenderer&) = delete;
    D3d11ComparisonRenderer& operator=(const D3d11ComparisonRenderer&) = delete;
    D3d11ComparisonRenderer(D3d11ComparisonRenderer&&) = delete;
    D3d11ComparisonRenderer& operator=(D3d11ComparisonRenderer&&) = delete;

    [[nodiscard]] ComparisonRenderResult render(const SurfaceRenderState& state) noexcept;
    void releaseResources() noexcept;

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace dvs::platform
