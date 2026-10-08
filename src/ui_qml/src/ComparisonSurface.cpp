#include "dvs/ui/ComparisonSurface.h"

#include "dvs/platform/D3d11ComparisonRenderer.h"
#include "dvs/platform/FrameMailbox.h"
#include "dvs/platform/GraphicsDeviceBroker.h"
#include "dvs/platform/PresentationAckMailbox.h"
#include "dvs/platform/RenderActivitySink.h"
#include "dvs/ui/GraphicsBackend.h"

#include "RenderRetry.h"

#include <QMatrix4x4>
#include <QQuickWindow>
#include <QSGRenderNode>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

namespace dvs::ui {

class ComparisonSurface::Services final {
public:
    Services(std::shared_ptr<platform::GraphicsDeviceBroker> deviceBrokerValue,
             std::shared_ptr<platform::FrameMailbox> frameMailboxValue,
             std::shared_ptr<platform::PresentationAckMailbox> acknowledgementMailboxValue,
             std::weak_ptr<platform::IRenderActivitySink> activitySinkValue) noexcept
        : deviceBroker(std::move(deviceBrokerValue)), frameMailbox(std::move(frameMailboxValue)),
          acknowledgementMailbox(std::move(acknowledgementMailboxValue)),
          activitySink(std::move(activitySinkValue)) {}

    std::shared_ptr<platform::GraphicsDeviceBroker> deviceBroker;
    std::shared_ptr<platform::FrameMailbox> frameMailbox;
    std::shared_ptr<platform::PresentationAckMailbox> acknowledgementMailbox;
    std::weak_ptr<platform::IRenderActivitySink> activitySink;
};

namespace {

struct PresentationOptions final {
    platform::SurfaceViewMode viewMode = platform::SurfaceViewMode::SideBySide;
    platform::SurfaceDifferenceMetric differenceMetric =
        platform::SurfaceDifferenceMetric::RgbAbsolute;
    platform::SurfaceDifferenceGain differenceGain = platform::SurfaceDifferenceGain::Gain1x;
    platform::SurfaceDifferenceEdge differenceEdge = platform::SurfaceDifferenceEdge::Between0And1;
    platform::SurfaceDifferenceFilter differenceFilter =
        platform::SurfaceDifferenceFilter::Bilinear;
    float wipePosition = 0.5F;
    bool exactPlaneAvailable = false;
    bool thresholdEnabled = false;
    float threshold = 0.0F;
    platform::SurfaceThresholdPolicy thresholdPolicy = platform::SurfaceThresholdPolicy::AnyChannel;
    bool differenceSuppressed = false;
    platform::SurfaceViewTransform viewTransform;
    bool roiEnabled = false;
    platform::SurfaceNormalizedRect roi;
    std::uint8_t referenceSlot = 0U;
};

[[nodiscard]] platform::SurfaceViewMode
nativeViewMode(const ComparisonSurface::ViewMode value) noexcept {
    return static_cast<platform::SurfaceViewMode>(value);
}

[[nodiscard]] platform::SurfaceThresholdPolicy
nativeThresholdPolicy(const ComparisonSurface::ThresholdPolicy value) noexcept {
    return static_cast<platform::SurfaceThresholdPolicy>(value);
}

[[nodiscard]] platform::SurfaceDifferenceMetric
nativeDifferenceMetric(const ComparisonSurface::DifferenceMetric value) noexcept {
    return static_cast<platform::SurfaceDifferenceMetric>(value);
}

[[nodiscard]] platform::SurfaceDifferenceGain
nativeDifferenceGain(const ComparisonSurface::DifferenceGain value) noexcept {
    return static_cast<platform::SurfaceDifferenceGain>(value);
}

[[nodiscard]] platform::SurfaceDifferenceEdge
nativeDifferenceEdge(const ComparisonSurface::DifferenceEdge value) noexcept {
    return static_cast<platform::SurfaceDifferenceEdge>(value);
}

[[nodiscard]] platform::SurfaceDifferenceFilter
nativeDifferenceFilter(const ComparisonSurface::DifferenceFilter value) noexcept {
    return static_cast<platform::SurfaceDifferenceFilter>(value);
}

[[nodiscard]] PresentationOptions presentationOptions(const ComparisonSurface& surface) noexcept {
    return PresentationOptions{
        .viewMode = nativeViewMode(surface.viewMode()),
        .differenceMetric = nativeDifferenceMetric(surface.differenceMetric()),
        .differenceGain = nativeDifferenceGain(surface.differenceGain()),
        .differenceEdge = nativeDifferenceEdge(surface.differenceEdge()),
        .differenceFilter = nativeDifferenceFilter(surface.differenceFilter()),
        .wipePosition = static_cast<float>(surface.wipePosition()),
        .exactPlaneAvailable = surface.exactPlaneAvailable(),
        .thresholdEnabled = surface.thresholdEnabled(),
        .threshold = static_cast<float>(surface.threshold()),
        .thresholdPolicy = nativeThresholdPolicy(surface.thresholdPolicy()),
        .differenceSuppressed = surface.differenceSuppressed(),
        .viewTransform =
            platform::SurfaceViewTransform{
                .centerX = static_cast<float>(surface.viewCenterX()),
                .centerY = static_cast<float>(surface.viewCenterY()),
                .scale = static_cast<float>(surface.viewScale()),
            },
        .roiEnabled = surface.roiEnabled(),
        .roi =
            platform::SurfaceNormalizedRect{
                .left = static_cast<float>(surface.roiLeft()),
                .top = static_cast<float>(surface.roiTop()),
                .right = static_cast<float>(surface.roiRight()),
                .bottom = static_cast<float>(surface.roiBottom()),
            },
        .referenceSlot = static_cast<std::uint8_t>(surface.referenceSlot()),
    };
}

[[nodiscard]] std::array<platform::SurfaceDisplayExtent, 3U> sourceDisplayExtents(
    const ComparisonSurface& surface, const bool roiEnabled, const QSizeF& roiExtent) {
    std::array<platform::SurfaceDisplayExtent, 3U> result{};
    const QVariantList info = surface.sourceDisplayInfo();
    const qreal roiWidth = roiEnabled ? roiExtent.width() : 1.0;
    const qreal roiHeight = roiEnabled ? roiExtent.height() : 1.0;
    for (qsizetype index = 0; index < info.size() && index < 3; ++index) {
        const QVariantMap source = info[index].toMap();
        const double width = source.value(QStringLiteral("width")).toDouble();
        const double height = source.value(QStringLiteral("height")).toDouble();
        const double sarNumerator =
            source.value(QStringLiteral("sampleAspectNumerator"), 1U).toDouble();
        const double sarDenominator =
            source.value(QStringLiteral("sampleAspectDenominator"), 1U).toDouble();
        const int rotation = source.value(QStringLiteral("rotationDegrees")).toInt();
        if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(sarNumerator) ||
            !std::isfinite(sarDenominator) || width <= 0.0 || height <= 0.0 ||
            sarNumerator <= 0.0 || sarDenominator <= 0.0) {
            continue;
        }
        const float displayWidth =
            static_cast<float>(width * roiWidth * sarNumerator / sarDenominator);
        const float displayHeight = static_cast<float>(height * roiHeight);
        if (rotation == 90 || rotation == 270) {
            result[static_cast<std::size_t>(index)] =
                platform::SurfaceDisplayExtent{.width = displayHeight, .height = displayWidth};
        } else {
            result[static_cast<std::size_t>(index)] =
                platform::SurfaceDisplayExtent{.width = displayWidth, .height = displayHeight};
        }
    }
    return result;
}

[[nodiscard]] platform::SurfacePresentationGeometry surfacePresentationGeometry(
    const ComparisonSurface& surface, const bool roiEnabled, const QSizeF& roiExtent) {
    const qreal devicePixelRatio =
        surface.window() != nullptr ? surface.window()->effectiveDevicePixelRatio() : 1.0;
    const auto pixelWidth = static_cast<std::uint32_t>(
        std::max<qreal>(1.0, std::round(surface.width() * devicePixelRatio)));
    const auto pixelHeight = static_cast<std::uint32_t>(
        std::max<qreal>(1.0, std::round(surface.height() * devicePixelRatio)));
    return platform::computeSurfacePresentationGeometry(
        nativeViewMode(surface.viewMode()),
        static_cast<float>(surface.width()),
        static_cast<float>(surface.height()),
        pixelWidth,
        pixelHeight,
        static_cast<std::uint8_t>(surface.referenceSlot()),
        nativeDifferenceEdge(surface.differenceEdge()),
        static_cast<float>(surface.wipePosition()),
        sourceDisplayExtents(surface, roiEnabled, roiExtent));
}

[[nodiscard]] platform::SurfacePresentationGeometry
surfacePresentationGeometry(const ComparisonSurface& surface) {
    return surfacePresentationGeometry(
        surface,
        surface.roiEnabled(),
        QSizeF{surface.roiRight() - surface.roiLeft(), surface.roiBottom() - surface.roiTop()});
}

[[nodiscard]] int sourceRotationDegrees(const ComparisonSurface& surface, const int sourceSlot) {
    if (sourceSlot < 0 || sourceSlot >= surface.sourceDisplayInfo().size()) {
        return 0;
    }
    const int raw = surface.sourceDisplayInfo()[sourceSlot]
                        .toMap()
                        .value(QStringLiteral("rotationDegrees"))
                        .toInt();
    const int normalized = ((raw % 360) + 360) % 360;
    return normalized == 90 || normalized == 180 || normalized == 270 ? normalized : 0;
}

[[nodiscard]] std::pair<qreal, qreal>
sourceOrientedPoint(const qreal displayX, const qreal displayY, const int rotationDegrees) {
    switch (rotationDegrees) {
    case 90:
        return {1.0 - displayY, displayX};
    case 180:
        return {1.0 - displayX, 1.0 - displayY};
    case 270:
        return {displayY, 1.0 - displayX};
    default:
        return {displayX, displayY};
    }
}

[[nodiscard]] platform::SurfacePixelGrid surfacePixelGrid(const ComparisonSurface& surface) {
    const qreal dpr = surface.window() ? surface.window()->effectiveDevicePixelRatio() : 1.0;
    const QPointF origin = surface.mapToScene(QPointF{0.0, 0.0});
    const QPointF xUnit = surface.mapToScene(QPointF{1.0, 0.0}) - origin;
    const QPointF yUnit = surface.mapToScene(QPointF{0.0, 1.0}) - origin;
    const QPointF far = surface.mapToScene(QPointF{surface.width(), surface.height()});
    const QPointF affineFar = origin + xUnit * surface.width() + yUnit * surface.height();
    if (xUnit.y() != 0.0 || yUnit.x() != 0.0 || std::abs(far.x() - affineFar.x()) > 0.0001 ||
        std::abs(far.y() - affineFar.y()) > 0.0001) {
        return {};
    }
    const auto windowGrid = surface.window()
                                ? platform::surfacePixelGridForWindow(
                                      surface.window()->width(), surface.window()->height(), dpr)
                                : platform::SurfacePixelGrid{1.0F, 1.0F, 0.0F, 0.0F};
    return platform::SurfacePixelGrid{static_cast<float>(xUnit.x() * windowGrid.scaleX),
                                      static_cast<float>(yUnit.y() * windowGrid.scaleY),
                                      static_cast<float>(origin.x() * windowGrid.scaleX),
                                      static_cast<float>(origin.y() * windowGrid.scaleY)};
}

[[nodiscard]] qreal minimumScaleForGeometry(const platform::SurfacePresentationGeometry& geometry,
                                            const platform::SurfacePixelGrid& grid) {
    if (!grid.isValid()) {
        return 1.0;
    }
    bool foundContent = false;
    qreal minimum = presentation::kMinimumViewportScale;
    const auto consider = [&](const platform::SurfaceRect& rect) {
        if (!rect.isValid()) {
            return;
        }
        const qreal physicalWidth = static_cast<qreal>(rect.width) * grid.scaleX;
        const qreal physicalHeight = static_cast<qreal>(rect.height) * grid.scaleY;
        if (!std::isfinite(physicalWidth) || !std::isfinite(physicalHeight) ||
            physicalWidth <= 0.0 || physicalHeight <= 0.0) {
            minimum = 1.0;
            return;
        }
        foundContent = true;
        minimum = (std::max)(minimum, 1.0 / (std::min)(physicalWidth, physicalHeight));
    };
    for (std::size_t index = 0U; index < geometry.panels.sourceCount; ++index) {
        consider(geometry.sourceContentRects[index]);
    }
    if (geometry.differenceContentRect.has_value()) {
        consider(*geometry.differenceContentRect);
    }
    return foundContent ? (std::min)(minimum, 1.0) : 1.0;
}

[[nodiscard]] platform::SurfaceRect actualContentRect(const ComparisonSurface& surface,
                                                      const platform::SurfaceRect& bounds,
                                                      const platform::SurfaceRect& fitted,
                                                      const int sourceSlot,
                                                      const platform::SurfaceNormalizedRect& sample,
                                                      const qreal requestedScale = -1.0) {
    const qreal scale = requestedScale > 0.0 ? requestedScale : surface.viewScale();
    if (scale >= 1.0) {
        return fitted;
    }
    platform::SurfaceRect pixels;
    if (sourceSlot >= 0 && sourceSlot < surface.sourceDisplayInfo().size()) {
        const QVariantMap info = surface.sourceDisplayInfo()[sourceSlot].toMap();
        pixels = platform::orientedSurfaceSamplePixels(
            info.value(QStringLiteral("width")).toFloat(),
            info.value(QStringLiteral("height")).toFloat(),
            static_cast<std::uint16_t>(sourceRotationDegrees(surface, sourceSlot)),
            sample);
    }
    return platform::transformedSurfaceContentRect(
        bounds,
        fitted,
        platform::SurfaceViewTransform{
            scale < 1.0 ? 0.5F : static_cast<float>(surface.viewCenterX()),
            scale < 1.0 ? 0.5F : static_cast<float>(surface.viewCenterY()),
            static_cast<float>(scale)},
        surfacePixelGrid(surface),
        pixels,
        static_cast<float>(surface.window() ? surface.window()->effectiveDevicePixelRatio() : 1.0));
}

[[nodiscard]] bool scaleDisplayable(const ComparisonSurface& surface,
                                    const platform::SurfacePresentationGeometry& geometry,
                                    const qreal scale,
                                    const bool roiEnabled,
                                    const platform::SurfaceNormalizedRect& roi) {
    if (!std::isfinite(scale) || scale < presentation::kMinimumViewportScale ||
        scale > presentation::kMaximumViewportScale) {
        return false;
    }
    if (scale >= 1.0) {
        return true;
    }
    const auto sample = platform::effectiveSurfaceSampleRect(
        {0.5F, 0.5F, static_cast<float>(scale)}, roiEnabled, roi);
    const platform::SurfaceRect full{
        0.0F, 0.0F, static_cast<float>(surface.width()), static_cast<float>(surface.height())};
    if (geometry.wipeContentRect.has_value()) {
        return actualContentRect(surface,
                                 full,
                                 *geometry.wipeContentRect,
                                 geometry.panels.sourceSlots[0U],
                                 sample,
                                 scale)
            .isValid();
    }
    bool found = false;
    for (std::size_t index = 0U; index < geometry.panels.sourceCount; ++index) {
        if (geometry.sourceContentRects[index].isValid()) {
            found = true;
            if (!actualContentRect(surface,
                                   geometry.panels.sourceRects[index],
                                   geometry.sourceContentRects[index],
                                   geometry.panels.sourceSlots[index],
                                   sample,
                                   scale)
                     .isValid()) {
                return false;
            }
        }
    }
    if (geometry.differenceContentRect.has_value()) {
        found = true;
        const int slot = surface.differenceEdge() == ComparisonSurface::Edge1And2 ? 1 : 0;
        if (!actualContentRect(surface,
                               *geometry.panels.differenceRect,
                               *geometry.differenceContentRect,
                               slot,
                               sample,
                               scale)
                 .isValid()) {
            return false;
        }
    }
    return found;
}

[[nodiscard]] std::uint32_t pixelExtent(const qreal logicalExtent, const qreal dpr) noexcept {
    if (!std::isfinite(logicalExtent) || !std::isfinite(dpr) || logicalExtent <= 0.0 ||
        dpr <= 0.0) {
        return 0U;
    }
    const qreal pixels = std::round(logicalExtent * dpr);
    if (pixels <= 0.0 || pixels > static_cast<qreal>((std::numeric_limits<std::uint32_t>::max)())) {
        return 0U;
    }
    return static_cast<std::uint32_t>(pixels);
}

[[nodiscard]] std::array<float, 16U> matrixValues(const QMatrix4x4& matrix) noexcept {
    std::array<float, 16U> result{};
    matrix.copyDataTo(result.data());
    return result;
}

} // namespace

class ComparisonRenderNode final : public QSGRenderNode {
public:
    ComparisonRenderNode(QQuickWindow& window,
                         const std::shared_ptr<const ComparisonSurface::Services>& services)
        : window_(window), services_(services), renderer_(services_->deviceBroker,
                                                          services_->frameMailbox,
                                                          services_->acknowledgementMailbox,
                                                          services_->activitySink) {}

    void synchronize(const QRectF& bounds,
                     const qreal devicePixelRatio,
                     const PresentationOptions& options) noexcept {
        bounds_ = bounds;
        devicePixelRatio_ = devicePixelRatio;
        presentationOptions_ = options;
    }

    [[nodiscard]] bool usesServices(
        const std::shared_ptr<const ComparisonSurface::Services>& services) const noexcept {
        return services_ == services;
    }

    void render(const RenderState* const renderState) override {
        const GraphicsBackendResult backendResult =
            bindGraphicsBackendOnRenderThread(window_, *services_->deviceBroker);
        if (backendResult != GraphicsBackendResult::Ready &&
            backendResult != GraphicsBackendResult::AlreadyReady) {
            retry_.retryContendedRender(window_, backendResult);
            return;
        }
        if (renderState == nullptr || renderState->projectionMatrix() == nullptr ||
            matrix() == nullptr || !bounds_.isValid()) {
            return;
        }

        const QMatrix4x4 clipFromItem = *renderState->projectionMatrix() * *matrix();
        const QRect scissor = renderState->scissorRect();
        const int stencilValue = renderState->stencilValue();
        const platform::SurfaceRenderState state{
            .clipFromItem = matrixValues(clipFromItem),
            .logicalWidth = static_cast<float>(bounds_.width()),
            .logicalHeight = static_cast<float>(bounds_.height()),
            .pixelWidth = pixelExtent(bounds_.width(), devicePixelRatio_),
            .pixelHeight = pixelExtent(bounds_.height(), devicePixelRatio_),
            .nominalDevicePixelRatio = static_cast<float>(devicePixelRatio_),
            .opacity = static_cast<float>(std::clamp(inheritedOpacity(), 0.0, 1.0)),
            .scissorEnabled = renderState->scissorEnabled(),
            .scissor =
                platform::SurfaceScissorRect{
                    .left = scissor.left(),
                    .bottom = scissor.top(),
                    .right = scissor.right() + 1,
                    .top = scissor.bottom() + 1,
                },
            .stencilEnabled = renderState->stencilEnabled(),
            .stencilReference = static_cast<std::uint32_t>(std::clamp(stencilValue, 0, 255)),
            .viewMode = presentationOptions_.viewMode,
            .differenceMetric = presentationOptions_.differenceMetric,
            .differenceGain = presentationOptions_.differenceGain,
            .differenceEdge = presentationOptions_.differenceEdge,
            .differenceFilter = presentationOptions_.differenceFilter,
            .wipePosition = presentationOptions_.wipePosition,
            .exactPlaneAvailable = presentationOptions_.exactPlaneAvailable,
            .thresholdEnabled = presentationOptions_.thresholdEnabled,
            .threshold = presentationOptions_.threshold,
            .thresholdPolicy = presentationOptions_.thresholdPolicy,
            .differenceSuppressed = presentationOptions_.differenceSuppressed,
            .viewTransform = presentationOptions_.viewTransform,
            .roiEnabled = presentationOptions_.roiEnabled,
            .roi = presentationOptions_.roi,
            .referenceSlot = presentationOptions_.referenceSlot,
        };
        retry_.retryContendedRender(window_, renderer_.render(state));
    }

    void releaseResources() override {
        renderer_.releaseResources();
    }

    [[nodiscard]] StateFlags changedStates() const override {
        return ScissorState | DepthState | StencilState | ColorState | BlendState | CullState;
    }

    [[nodiscard]] RenderingFlags flags() const override {
        return BoundedRectRendering | DepthAwareRendering;
    }

    [[nodiscard]] QRectF rect() const override {
        return bounds_;
    }

private:
    QQuickWindow& window_;
    std::shared_ptr<const ComparisonSurface::Services> services_;
    platform::D3d11ComparisonRenderer renderer_;
    detail::RenderRetry retry_;
    QRectF bounds_;
    qreal devicePixelRatio_ = 1.0;
    PresentationOptions presentationOptions_;
};

ComparisonSurface::ComparisonSurface(QQuickItem* const parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

ComparisonSurface::~ComparisonSurface() = default;

ComparisonSurface::ViewMode ComparisonSurface::viewMode() const noexcept {
    return viewMode_;
}

void ComparisonSurface::setViewMode(const ViewMode value) {
    if ((value != SideBySide && value != ThreeUp && value != ReferenceFocus &&
         value != Difference && value != AnalysisGrid && value != Wipe && value != Single &&
         value != Fade) ||
        viewMode_ == value) {
        return;
    }
    viewMode_ = value;
    clampSmallViewportToGeometry();
    emit viewModeChanged();
    emit presentationGeometryChanged();
    update();
}

ComparisonSurface::DifferenceMetric ComparisonSurface::differenceMetric() const noexcept {
    return differenceMetric_;
}

void ComparisonSurface::setDifferenceMetric(const DifferenceMetric value) {
    if ((value != RgbAbsolute && value != Luma && value != Chroma && value != Heatmap &&
         value != ExactPlanes && value != SignedSubtract && value != Highlight &&
         value != Crossfade) ||
        differenceMetric_ == value) {
        return;
    }
    differenceMetric_ = value;
    emit differenceMetricChanged();
    update();
}

ComparisonSurface::DifferenceGain ComparisonSurface::differenceGain() const noexcept {
    return differenceGain_;
}

void ComparisonSurface::setDifferenceGain(const DifferenceGain value) {
    if ((value != Gain1x && value != Gain2x && value != Gain4x && value != Gain8x &&
         value != Gain16x) ||
        differenceGain_ == value) {
        return;
    }
    differenceGain_ = value;
    emit differenceGainChanged();
    update();
}

ComparisonSurface::DifferenceEdge ComparisonSurface::differenceEdge() const noexcept {
    return differenceEdge_;
}

void ComparisonSurface::setDifferenceEdge(const DifferenceEdge value) {
    if ((value != Edge0And1 && value != Edge0And2 && value != Edge1And2) ||
        differenceEdge_ == value) {
        return;
    }
    differenceEdge_ = value;
    clampSmallViewportToGeometry();
    emit differenceEdgeChanged();
    emit presentationGeometryChanged();
    update();
}

ComparisonSurface::DifferenceFilter ComparisonSurface::differenceFilter() const noexcept {
    return differenceFilter_;
}

void ComparisonSurface::setDifferenceFilter(const DifferenceFilter value) {
    if ((value != Nearest && value != Bilinear && value != Bicubic) || differenceFilter_ == value) {
        return;
    }
    differenceFilter_ = value;
    emit differenceFilterChanged();
    update();
}

qreal ComparisonSurface::wipePosition() const noexcept {
    return wipePosition_;
}

void ComparisonSurface::setWipePosition(const qreal value) {
    if (!std::isfinite(value)) {
        return;
    }
    const qreal clamped = std::clamp(value, 0.0, 1.0);
    if (qFuzzyCompare(wipePosition_, clamped)) {
        return;
    }
    wipePosition_ = clamped;
    emit wipePositionChanged();
    emit presentationGeometryChanged();
    update();
}

qreal ComparisonSurface::wipeSplitLogicalX() const {
    return width() * wipePosition_;
}

QVariantList ComparisonSurface::sourcePanelRects() const {
    const platform::SurfacePresentationGeometry geometry = surfacePresentationGeometry(*this);
    const platform::SurfacePanelLayout& layout = geometry.panels;
    QVariantList result;
    result.reserve(static_cast<qsizetype>(layout.sourceCount));
    for (std::size_t index = 0U; index < layout.sourceCount; ++index) {
        const platform::SurfaceRect& rect = layout.sourceRects[index];
        QVariantMap item;
        item.insert(QStringLiteral("slot"), static_cast<int>(layout.sourceSlots[index]));
        item.insert(QStringLiteral("x"), rect.x);
        item.insert(QStringLiteral("y"), rect.y);
        item.insert(QStringLiteral("width"), rect.width);
        item.insert(QStringLiteral("height"), rect.height);
        // Keep panel bounds for labels/dividers, but expose the fitted video extent for
        // pixel-scale readouts. These remain the unscaled Fit basis; viewScale is applied
        // exactly once by the readout. Hit mapping uses actualContentRect below Fit.
        // In wipe mode this is the full composite, not the split mask.
        item.insert(QStringLiteral("contentWidth"), geometry.sourceContentRects[index].width);
        item.insert(QStringLiteral("contentHeight"), geometry.sourceContentRects[index].height);
        result.push_back(std::move(item));
    }
    return result;
}

QVariantList ComparisonSurface::sourceDisplayInfo() const {
    return sourceDisplayInfo_;
}

void ComparisonSurface::setSourceDisplayInfo(const QVariantList& value) {
    if (sourceDisplayInfo_ == value) {
        return;
    }
    sourceDisplayInfo_ = value;
    clampSmallViewportToGeometry();
    emit presentationGeometryChanged();
}

qreal ComparisonSurface::wipePositionForLogicalX(const qreal x) const {
    if (!std::isfinite(x) || width() <= 0.0 || viewMode_ != Wipe) {
        return 0.5;
    }
    return std::clamp(x / width(), 0.0, 1.0);
}

QVariantMap ComparisonSurface::mapSurfacePoint(const qreal x, const qreal y) const {
    if (!std::isfinite(x) || !std::isfinite(y) || width() <= 0.0 || height() <= 0.0) {
        return {};
    }
    const platform::SurfacePresentationGeometry geometry = surfacePresentationGeometry(*this);
    const auto contains = [x, y](const platform::SurfaceRect& rect) {
        return rect.isValid() && x >= rect.x && y >= rect.y && x <= rect.x + rect.width &&
               y <= rect.y + rect.height;
    };
    const platform::SurfaceNormalizedRect sample = platform::effectiveSurfaceSampleRect(
        platform::SurfaceViewTransform{
            .centerX = static_cast<float>(viewCenterX_),
            .centerY = static_cast<float>(viewCenterY_),
            .scale = static_cast<float>(viewScale_),
        },
        roiEnabled_,
        platform::SurfaceNormalizedRect{
            .left = static_cast<float>(roiLeft_),
            .top = static_cast<float>(roiTop_),
            .right = static_cast<float>(roiRight_),
            .bottom = static_cast<float>(roiBottom_),
        });
    const auto hit = [this, x, y, &sample](const SurfaceRegionKind region,
                                           const int panelIndex,
                                           const int sourceSlot,
                                           const int coordinateSlot,
                                           const bool insidePanel,
                                           const platform::SurfaceRect& content) {
        const bool insideContent = content.isValid() && x >= content.x && y >= content.y &&
                                   x <= content.x + content.width &&
                                   y <= content.y + content.height;
        QVariantMap result{
            {QStringLiteral("region"), region},
            {QStringLiteral("panel"), panelIndex},
            {QStringLiteral("panelIndex"), panelIndex},
            {QStringLiteral("sourceSlot"), sourceSlot},
            {QStringLiteral("insidePanel"), insidePanel},
            {QStringLiteral("insideContent"), insideContent},
            {QStringLiteral("insideVideoContent"), insideContent},
        };
        if (insideContent) {
            const qreal displayX = std::clamp((x - content.x) / content.width, 0.0, 1.0);
            const qreal displayY = std::clamp((y - content.y) / content.height, 0.0, 1.0);
            const auto [normalizedX, normalizedY] = sourceOrientedPoint(
                displayX, displayY, sourceRotationDegrees(*this, coordinateSlot));
            result.insert(QStringLiteral("displayX"), displayX);
            result.insert(QStringLiteral("displayY"), displayY);
            result.insert(QStringLiteral("x"), normalizedX);
            result.insert(QStringLiteral("y"), normalizedY);
            result.insert(QStringLiteral("normalizedX"), normalizedX);
            result.insert(QStringLiteral("normalizedY"), normalizedY);
            if (sample.isValid()) {
                result.insert(QStringLiteral("sourceX"),
                              sample.left + (normalizedX * (sample.right - sample.left)));
                result.insert(QStringLiteral("sourceY"),
                              sample.top + (normalizedY * (sample.bottom - sample.top)));
            }
        }
        return result;
    };

    if (viewMode_ == Wipe && geometry.wipeContentRect.has_value()) {
        const bool insidePanel = x >= 0.0 && y >= 0.0 && x <= width() && y <= height();
        const int panelIndex = x <= wipeSplitLogicalX() ? 0 : 1;
        const int sourceSlot = static_cast<int>(geometry.panels.sourceSlots[panelIndex]);
        return hit(WipeCompositeRegion,
                   panelIndex,
                   sourceSlot,
                   sourceSlot,
                   insidePanel,
                   actualContentRect(
                       *this,
                       platform::SurfaceRect{
                           0.0F, 0.0F, static_cast<float>(width()), static_cast<float>(height())},
                       *geometry.wipeContentRect,
                       static_cast<int>(geometry.panels.sourceSlots[0U]),
                       sample));
    }
    for (std::size_t index = 0U; index < geometry.panels.sourceCount; ++index) {
        if (contains(geometry.panels.sourceRects[index])) {
            return hit(SourceRegion,
                       static_cast<int>(index),
                       static_cast<int>(geometry.panels.sourceSlots[index]),
                       static_cast<int>(geometry.panels.sourceSlots[index]),
                       true,
                       actualContentRect(*this,
                                         geometry.panels.sourceRects[index],
                                         geometry.sourceContentRects[index],
                                         static_cast<int>(geometry.panels.sourceSlots[index]),
                                         sample));
        }
    }
    if (geometry.panels.differenceRect.has_value() && contains(*geometry.panels.differenceRect)) {
        const int firstDifferenceSlot = differenceEdge_ == Edge1And2 ? 1 : 0;
        return hit(
            DifferenceRegion,
            static_cast<int>(geometry.panels.sourceCount),
            -1,
            firstDifferenceSlot,
            true,
            actualContentRect(*this,
                              *geometry.panels.differenceRect,
                              geometry.differenceContentRect.value_or(platform::SurfaceRect{}),
                              firstDifferenceSlot,
                              sample));
    }
    return QVariantMap{
        {QStringLiteral("region"), EmptyRegion},
        {QStringLiteral("panel"), -1},
        {QStringLiteral("panelIndex"), -1},
        {QStringLiteral("sourceSlot"), -1},
        {QStringLiteral("insidePanel"), false},
        {QStringLiteral("insideContent"), false},
        {QStringLiteral("insideVideoContent"), false},
    };
}

bool ComparisonSurface::exactPlaneAvailable() const noexcept {
    return exactPlaneAvailable_;
}

void ComparisonSurface::setExactPlaneAvailable(const bool value) {
    if (exactPlaneAvailable_ == value) {
        return;
    }
    exactPlaneAvailable_ = value;
    emit exactPlaneAvailableChanged();
    update();
}

bool ComparisonSurface::thresholdEnabled() const noexcept {
    return thresholdEnabled_;
}

void ComparisonSurface::setThresholdEnabled(const bool value) {
    if (thresholdEnabled_ == value) {
        return;
    }
    thresholdEnabled_ = value;
    emit thresholdChanged();
    update();
}

qreal ComparisonSurface::threshold() const noexcept {
    return threshold_;
}

void ComparisonSurface::setThreshold(const qreal value) {
    if (!std::isfinite(value)) {
        return;
    }
    const qreal normalized = std::clamp(value, 0.0, 1.0);
    if (qFuzzyCompare(threshold_, normalized)) {
        return;
    }
    threshold_ = normalized;
    emit thresholdChanged();
    update();
}

ComparisonSurface::ThresholdPolicy ComparisonSurface::thresholdPolicy() const noexcept {
    return thresholdPolicy_;
}

void ComparisonSurface::setThresholdPolicy(const ThresholdPolicy value) {
    if ((value != ThresholdLumaOnly && value != ThresholdAnyChannel &&
         value != ThresholdAllChannels) ||
        thresholdPolicy_ == value) {
        return;
    }
    thresholdPolicy_ = value;
    emit thresholdChanged();
    update();
}

qreal ComparisonSurface::viewCenterX() const noexcept {
    return viewCenterX_;
}

qreal ComparisonSurface::viewCenterY() const noexcept {
    return viewCenterY_;
}

qreal ComparisonSurface::viewScale() const noexcept {
    return viewScale_;
}

bool ComparisonSurface::roiEnabled() const noexcept {
    return roiEnabled_;
}

void ComparisonSurface::clampSmallViewportToGeometry() {
    if (viewScale_ >= 1.0 || canDisplayViewScale(viewScale_)) {
        return;
    }
    const qreal minimum = minimumViewScale();
    const qreal candidate = (std::max)(viewScale_, minimum);
    const qreal next = canDisplayViewScale(candidate) ? candidate : 1.0;
    if (viewScale_ != next) {
        viewScale_ = next;
        viewCenterX_ = 0.5;
        viewCenterY_ = 0.5;
        emit viewportChanged();
        update();
    }
}

qreal ComparisonSurface::minimumViewScale() const {
    return minimumScaleForGeometry(surfacePresentationGeometry(*this), surfacePixelGrid(*this));
}

qreal ComparisonSurface::roiLeft() const noexcept {
    return roiLeft_;
}

qreal ComparisonSurface::roiTop() const noexcept {
    return roiTop_;
}

qreal ComparisonSurface::roiRight() const noexcept {
    return roiRight_;
}

qreal ComparisonSurface::roiBottom() const noexcept {
    return roiBottom_;
}

bool ComparisonSurface::canDisplayViewScale(const qreal scale) const {
    return scaleDisplayable(*this,
                            surfacePresentationGeometry(*this),
                            scale,
                            roiEnabled_,
                            {static_cast<float>(roiLeft_),
                             static_cast<float>(roiTop_),
                             static_cast<float>(roiRight_),
                             static_cast<float>(roiBottom_)});
}

void ComparisonSurface::zoomAt(const qreal normalizedX,
                               const qreal normalizedY,
                               const qreal factor) {
    if (!std::isfinite(normalizedX) || !std::isfinite(normalizedY) || !std::isfinite(factor) ||
        factor <= 0.0) {
        return;
    }
    const qreal focalX = std::clamp(normalizedX, 0.0, 1.0);
    const qreal focalY = std::clamp(normalizedY, 0.0, 1.0);
    const qreal oldVisible = 1.0 / (std::max)(viewScale_, 1.0);
    const qreal oldLeft = std::clamp(viewCenterX_ - (oldVisible * 0.5), 0.0, 1.0 - oldVisible);
    const qreal oldTop = std::clamp(viewCenterY_ - (oldVisible * 0.5), 0.0, 1.0 - oldVisible);
    const qreal sourceX = oldLeft + (focalX * oldVisible);
    const qreal sourceY = oldTop + (focalY * oldVisible);
    const qreal requestedScale =
        (std::min)(viewScale_ * factor, static_cast<qreal>(presentation::kMaximumViewportScale));
    const qreal nextScale =
        canDisplayViewScale(requestedScale)
            ? requestedScale
            : std::clamp(requestedScale,
                         minimumViewScale(),
                         static_cast<qreal>(presentation::kMaximumViewportScale));
    if (!canDisplayViewScale(nextScale)) {
        return;
    }
    const qreal nextVisible = 1.0 / (std::max)(nextScale, 1.0);
    const qreal nextLeft = std::clamp(sourceX - (focalX * nextVisible), 0.0, 1.0 - nextVisible);
    const qreal nextTop = std::clamp(sourceY - (focalY * nextVisible), 0.0, 1.0 - nextVisible);
    viewScale_ = nextScale;
    viewCenterX_ = nextLeft + (nextVisible * 0.5);
    viewCenterY_ = nextTop + (nextVisible * 0.5);
    emit viewportChanged();
    update();
}

void ComparisonSurface::panBy(const qreal normalizedDeltaX, const qreal normalizedDeltaY) {
    if (!std::isfinite(normalizedDeltaX) || !std::isfinite(normalizedDeltaY) || viewScale_ <= 1.0) {
        return;
    }
    const qreal visible = 1.0 / viewScale_;
    const qreal minimumCenter = visible * 0.5;
    const qreal maximumCenter = 1.0 - minimumCenter;
    const qreal nextX =
        std::clamp(viewCenterX_ - (normalizedDeltaX * visible), minimumCenter, maximumCenter);
    const qreal nextY =
        std::clamp(viewCenterY_ - (normalizedDeltaY * visible), minimumCenter, maximumCenter);
    if (qFuzzyCompare(nextX, viewCenterX_) && qFuzzyCompare(nextY, viewCenterY_)) {
        return;
    }
    viewCenterX_ = nextX;
    viewCenterY_ = nextY;
    emit viewportChanged();
    update();
}

void ComparisonSurface::resetViewport() {
    if (qFuzzyCompare(viewCenterX_, 0.5) && qFuzzyCompare(viewCenterY_, 0.5) &&
        qFuzzyCompare(viewScale_, 1.0)) {
        return;
    }
    viewCenterX_ = 0.5;
    viewCenterY_ = 0.5;
    viewScale_ = 1.0;
    emit viewportChanged();
    update();
}

void ComparisonSurface::zoomToNormalizedRect(const qreal left,
                                             const qreal top,
                                             const qreal right,
                                             const qreal bottom) {
    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) ||
        !std::isfinite(bottom)) {
        return;
    }
    const qreal normalizedLeft = std::clamp((std::min)(left, right), 0.0, 1.0);
    const qreal normalizedTop = std::clamp((std::min)(top, bottom), 0.0, 1.0);
    const qreal normalizedRight = std::clamp((std::max)(left, right), 0.0, 1.0);
    const qreal normalizedBottom = std::clamp((std::max)(top, bottom), 0.0, 1.0);
    const qreal boxWidth = normalizedRight - normalizedLeft;
    const qreal boxHeight = normalizedBottom - normalizedTop;
    if (boxWidth < 0.005 || boxHeight < 0.005) {
        return;
    }
    // Fit the larger box dimension so the whole selection stays visible.
    const qreal nextScale = std::clamp(1.0 / (std::max)(boxWidth, boxHeight), 1.0, 64.0);
    const qreal visible = 1.0 / nextScale;
    const qreal centerX = (normalizedLeft + normalizedRight) * 0.5;
    const qreal centerY = (normalizedTop + normalizedBottom) * 0.5;
    viewScale_ = nextScale;
    viewCenterX_ = std::clamp(centerX, visible * 0.5, 1.0 - visible * 0.5);
    viewCenterY_ = std::clamp(centerY, visible * 0.5, 1.0 - visible * 0.5);
    emit viewportChanged();
    update();
}

void ComparisonSurface::setRoiNormalized(const qreal left,
                                         const qreal top,
                                         const qreal right,
                                         const qreal bottom) {
    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) ||
        !std::isfinite(bottom)) {
        return;
    }
    const qreal normalizedLeft = std::clamp((std::min)(left, right), 0.0, 1.0);
    const qreal normalizedTop = std::clamp((std::min)(top, bottom), 0.0, 1.0);
    const qreal normalizedRight = std::clamp((std::max)(left, right), 0.0, 1.0);
    const qreal normalizedBottom = std::clamp((std::max)(top, bottom), 0.0, 1.0);
    if ((normalizedRight - normalizedLeft) < 0.001 || (normalizedBottom - normalizedTop) < 0.001) {
        return;
    }
    roiEnabled_ = true;
    roiLeft_ = normalizedLeft;
    roiTop_ = normalizedTop;
    roiRight_ = normalizedRight;
    roiBottom_ = normalizedBottom;
    viewCenterX_ = 0.5;
    viewCenterY_ = 0.5;
    viewScale_ = 1.0;
    emit viewportChanged();
    emit presentationGeometryChanged();
    update();
}

void ComparisonSurface::clearRoi() {
    if (!roiEnabled_) {
        return;
    }
    roiEnabled_ = false;
    roiLeft_ = 0.0;
    roiTop_ = 0.0;
    roiRight_ = 1.0;
    roiBottom_ = 1.0;
    viewCenterX_ = 0.5;
    viewCenterY_ = 0.5;
    viewScale_ = 1.0;
    emit viewportChanged();
    emit presentationGeometryChanged();
    update();
}

void ComparisonSurface::restoreViewport(const qreal centerX,
                                        const qreal centerY,
                                        const qreal scale,
                                        const bool roiEnabled,
                                        const qreal roiLeft,
                                        const qreal roiTop,
                                        const qreal roiRight,
                                        const qreal roiBottom) {
    if (!std::isfinite(centerX) || !std::isfinite(centerY) || !std::isfinite(scale) ||
        !std::isfinite(roiLeft) || !std::isfinite(roiTop) || !std::isfinite(roiRight) ||
        !std::isfinite(roiBottom) || scale < presentation::kMinimumViewportScale ||
        scale > presentation::kMaximumViewportScale) {
        return;
    }
    const qreal visible = 1.0 / (std::max)(scale, 1.0);
    const qreal minimumCenter = visible * 0.5;
    const qreal maximumCenter = 1.0 - minimumCenter;
    if (centerX < minimumCenter || centerX > maximumCenter || centerY < minimumCenter ||
        centerY > maximumCenter ||
        (roiEnabled && (roiLeft < 0.0 || roiTop < 0.0 || roiRight > 1.0 || roiBottom > 1.0 ||
                        roiLeft >= roiRight || roiTop >= roiBottom))) {
        return;
    }

    if (!scaleDisplayable(*this,
                          surfacePresentationGeometry(
                              *this, roiEnabled, QSizeF{roiRight - roiLeft, roiBottom - roiTop}),
                          scale,
                          roiEnabled,
                          {static_cast<float>(roiLeft),
                           static_cast<float>(roiTop),
                           static_cast<float>(roiRight),
                           static_cast<float>(roiBottom)})) {
        return;
    }

    viewCenterX_ = centerX;
    viewCenterY_ = centerY;
    viewScale_ = scale;
    roiEnabled_ = roiEnabled;
    roiLeft_ = roiEnabled ? roiLeft : 0.0;
    roiTop_ = roiEnabled ? roiTop : 0.0;
    roiRight_ = roiEnabled ? roiRight : 1.0;
    roiBottom_ = roiEnabled ? roiBottom : 1.0;
    emit viewportChanged();
    emit presentationGeometryChanged();
    update();
}

int ComparisonSurface::referenceSlot() const noexcept {
    return referenceSlot_;
}

void ComparisonSurface::setReferenceSlot(const int value) {
    if (value < 0 || value > 2 || referenceSlot_ == value) {
        return;
    }
    referenceSlot_ = value;
    clampSmallViewportToGeometry();
    emit referenceSlotChanged();
    emit presentationGeometryChanged();
    update();
}

bool ComparisonSurface::differenceSuppressed() const noexcept {
    return differenceSuppressed_;
}

void ComparisonSurface::setDifferenceSuppressed(const bool value) {
    if (differenceSuppressed_ == value) {
        return;
    }
    differenceSuppressed_ = value;
    emit differenceSuppressedChanged();
    update();
}

bool ComparisonSurface::attachRendererServices(
    std::shared_ptr<platform::GraphicsDeviceBroker> deviceBroker,
    std::shared_ptr<platform::FrameMailbox> frameMailbox,
    std::shared_ptr<platform::PresentationAckMailbox> acknowledgementMailbox,
    std::weak_ptr<platform::IRenderActivitySink> activitySink,
    std::function<std::uint64_t()> droppedFrameProbe) {
    if (!deviceBroker || !frameMailbox || !acknowledgementMailbox) {
        return false;
    }
    services_ = std::make_shared<Services>(std::move(deviceBroker),
                                           std::move(frameMailbox),
                                           std::move(acknowledgementMailbox),
                                           std::move(activitySink));
    droppedFrameProbe_ = std::move(droppedFrameProbe);
    droppedFrames_ = 0U;
    update();
    return true;
}

void ComparisonSurface::detachRendererServices() noexcept {
    services_.reset();
    droppedFrameProbe_ = {};
    update();
}

bool ComparisonSurface::hasRendererServices() const noexcept {
    return services_ != nullptr;
}

qulonglong ComparisonSurface::droppedFrames() const noexcept {
    return droppedFrames_;
}

QSGNode* ComparisonSurface::updatePaintNode(QSGNode* const oldNode, UpdatePaintNodeData*) {
    QQuickWindow* const itemWindow = window();
    // Sync-time probe of the relay's gap counter. The probe itself is a lock-free atomic read
    // on the relay side; only a changed value emits, so steady playback costs one comparison.
    if (droppedFrameProbe_) {
        const qulonglong probed = static_cast<qulonglong>(droppedFrameProbe_.operator()());
        if (probed != droppedFrames_) {
            droppedFrames_ = probed;
            emit droppedFramesChanged();
        }
    }
    if (!services_ || itemWindow == nullptr || width() <= 0.0 || height() <= 0.0) {
        delete oldNode;
        return nullptr;
    }

    auto* node = static_cast<ComparisonRenderNode*>(oldNode);
    if (node != nullptr && !node->usesServices(services_)) {
        delete node;
        node = nullptr;
    }
    if (node == nullptr) {
        node = new ComparisonRenderNode{*itemWindow, services_};
    }

    node->synchronize(
        boundingRect(), itemWindow->effectiveDevicePixelRatio(), presentationOptions(*this));
    return node;
}

void ComparisonSurface::itemChange(const ItemChange change, const ItemChangeData& value) {
    QQuickItem::itemChange(change, value);
    if (change == ItemSceneChange || change == ItemDevicePixelRatioHasChanged) {
        clampSmallViewportToGeometry();
        emit presentationGeometryChanged();
        update();
    }
}

void ComparisonSurface::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        clampSmallViewportToGeometry();
        emit presentationGeometryChanged();
        update();
    }
}

} // namespace dvs::ui
