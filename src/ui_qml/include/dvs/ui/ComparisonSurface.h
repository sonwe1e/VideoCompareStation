#pragma once

#include "dvs/presentation/ComparisonContract.h"

#include <QQuickItem>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <memory>

namespace dvs::platform {
class FrameMailbox;
class GraphicsDeviceBroker;
class IRenderActivitySink;
class PresentationAckMailbox;
} // namespace dvs::platform

namespace dvs::ui {

class ComparisonRenderNode;

// Thin Qt Quick synchronization boundary. All D3D resources and rendering behavior remain owned
// by the platform renderer living in the scene-graph node.
// Qt's QML type wrapper derives from registered C++ elements, so this visual boundary cannot be
// final even though production code does not subclass it directly.
class ComparisonSurface : public QQuickItem {
    Q_OBJECT

    Q_PROPERTY(ViewMode viewMode READ viewMode WRITE setViewMode NOTIFY viewModeChanged)
    Q_PROPERTY(DifferenceMetric differenceMetric READ differenceMetric WRITE setDifferenceMetric
                   NOTIFY differenceMetricChanged)
    Q_PROPERTY(DifferenceGain differenceGain READ differenceGain WRITE setDifferenceGain NOTIFY
                   differenceGainChanged)
    Q_PROPERTY(DifferenceEdge differenceEdge READ differenceEdge WRITE setDifferenceEdge NOTIFY
                   differenceEdgeChanged)
    Q_PROPERTY(DifferenceFilter differenceFilter READ differenceFilter WRITE setDifferenceFilter
                   NOTIFY differenceFilterChanged)
    Q_PROPERTY(
        qreal wipePosition READ wipePosition WRITE setWipePosition NOTIFY wipePositionChanged)
    Q_PROPERTY(qreal wipeSplitLogicalX READ wipeSplitLogicalX NOTIFY presentationGeometryChanged)
    Q_PROPERTY(
        QVariantList sourcePanelRects READ sourcePanelRects NOTIFY presentationGeometryChanged)
    Q_PROPERTY(QVariantList sourceDisplayInfo READ sourceDisplayInfo WRITE setSourceDisplayInfo
                   NOTIFY presentationGeometryChanged)
    Q_PROPERTY(bool exactPlaneAvailable READ exactPlaneAvailable WRITE setExactPlaneAvailable NOTIFY
                   exactPlaneAvailableChanged)
    Q_PROPERTY(bool thresholdEnabled READ thresholdEnabled WRITE setThresholdEnabled NOTIFY
                   thresholdChanged)
    Q_PROPERTY(qreal threshold READ threshold WRITE setThreshold NOTIFY thresholdChanged)
    Q_PROPERTY(ThresholdPolicy thresholdPolicy READ thresholdPolicy WRITE setThresholdPolicy NOTIFY
                   thresholdChanged)
    Q_PROPERTY(qreal viewCenterX READ viewCenterX NOTIFY viewportChanged)
    Q_PROPERTY(qreal viewCenterY READ viewCenterY NOTIFY viewportChanged)
    Q_PROPERTY(qreal viewScale READ viewScale NOTIFY viewportChanged)
    Q_PROPERTY(bool roiEnabled READ roiEnabled NOTIFY viewportChanged)
    Q_PROPERTY(qreal roiLeft READ roiLeft NOTIFY viewportChanged)
    Q_PROPERTY(qreal roiTop READ roiTop NOTIFY viewportChanged)
    Q_PROPERTY(qreal roiRight READ roiRight NOTIFY viewportChanged)
    Q_PROPERTY(qreal roiBottom READ roiBottom NOTIFY viewportChanged)
    Q_PROPERTY(
        int referenceSlot READ referenceSlot WRITE setReferenceSlot NOTIFY referenceSlotChanged)
    // Canonical frame gaps observed by the render-acknowledgement relay since surface attach.
    // Refreshed on every scene-graph sync; an empty provider leaves the counter at zero.
    Q_PROPERTY(qulonglong droppedFrames READ droppedFrames NOTIFY droppedFramesChanged)

public:
    enum ViewMode {
        SideBySide = static_cast<int>(presentation::ViewMode::SideBySide),
        ThreeUp = static_cast<int>(presentation::ViewMode::ThreeUp),
        ReferenceFocus = static_cast<int>(presentation::ViewMode::ReferenceFocus),
        Difference = static_cast<int>(presentation::ViewMode::Difference),
        AnalysisGrid = static_cast<int>(presentation::ViewMode::AnalysisGrid),
        Wipe = static_cast<int>(presentation::ViewMode::Wipe),
        Single = static_cast<int>(presentation::ViewMode::Single),
        Fade = static_cast<int>(presentation::ViewMode::Fade),
    };
    Q_ENUM(ViewMode)

    enum DifferenceMetric {
        RgbAbsolute = static_cast<int>(presentation::DifferenceMetric::RgbAbsolute),
        Luma = static_cast<int>(presentation::DifferenceMetric::Luma),
        Chroma = static_cast<int>(presentation::DifferenceMetric::Chroma),
        Heatmap = static_cast<int>(presentation::DifferenceMetric::Heatmap),
        ExactPlanes = static_cast<int>(presentation::DifferenceMetric::ExactPlanes),
        SignedSubtract = static_cast<int>(presentation::DifferenceMetric::SignedSubtract),
        Highlight = static_cast<int>(presentation::DifferenceMetric::Highlight),
        Crossfade = static_cast<int>(presentation::DifferenceMetric::Crossfade),
    };
    Q_ENUM(DifferenceMetric)

    enum DifferenceGain {
        Gain1x = static_cast<int>(presentation::DifferenceGain::Gain1x),
        Gain2x = static_cast<int>(presentation::DifferenceGain::Gain2x),
        Gain4x = static_cast<int>(presentation::DifferenceGain::Gain4x),
        Gain8x = static_cast<int>(presentation::DifferenceGain::Gain8x),
        Gain16x = static_cast<int>(presentation::DifferenceGain::Gain16x),
    };
    Q_ENUM(DifferenceGain)

    // Selects which two source slots the difference view compares (slot order = session
    // source order).
    enum DifferenceEdge {
        Edge0And1 = static_cast<int>(presentation::DifferenceEdge::Edge0And1),
        Edge0And2 = static_cast<int>(presentation::DifferenceEdge::Edge0And2),
        Edge1And2 = static_cast<int>(presentation::DifferenceEdge::Edge1And2),
    };
    Q_ENUM(DifferenceEdge)

    enum DifferenceFilter {
        Nearest = static_cast<int>(presentation::DifferenceFilter::Nearest),
        Bilinear = static_cast<int>(presentation::DifferenceFilter::Bilinear),
        Bicubic = static_cast<int>(presentation::DifferenceFilter::Bicubic),
    };
    Q_ENUM(DifferenceFilter)

    enum ThresholdPolicy {
        ThresholdLumaOnly = static_cast<int>(presentation::ThresholdPolicy::LumaOnly),
        ThresholdAnyChannel = static_cast<int>(presentation::ThresholdPolicy::AnyChannel),
        ThresholdAllChannels = static_cast<int>(presentation::ThresholdPolicy::AllChannels),
    };
    Q_ENUM(ThresholdPolicy)

    enum SurfaceRegionKind {
        SourceRegion = 0,
        DifferenceRegion = 1,
        WipeCompositeRegion = 2,
        EmptyRegion = 3,
    };
    Q_ENUM(SurfaceRegionKind)

    explicit ComparisonSurface(QQuickItem* parent = nullptr);
    ~ComparisonSurface() override;

    ComparisonSurface(const ComparisonSurface&) = delete;
    ComparisonSurface& operator=(const ComparisonSurface&) = delete;

    [[nodiscard]] ViewMode viewMode() const noexcept;
    void setViewMode(ViewMode value);
    [[nodiscard]] DifferenceMetric differenceMetric() const noexcept;
    void setDifferenceMetric(DifferenceMetric value);
    [[nodiscard]] DifferenceGain differenceGain() const noexcept;
    void setDifferenceGain(DifferenceGain value);
    [[nodiscard]] DifferenceEdge differenceEdge() const noexcept;
    void setDifferenceEdge(DifferenceEdge value);
    [[nodiscard]] DifferenceFilter differenceFilter() const noexcept;
    void setDifferenceFilter(DifferenceFilter value);
    [[nodiscard]] qreal wipePosition() const noexcept;
    void setWipePosition(qreal value);
    [[nodiscard]] qreal wipeSplitLogicalX() const;
    [[nodiscard]] QVariantList sourcePanelRects() const;
    [[nodiscard]] QVariantList sourceDisplayInfo() const;
    void setSourceDisplayInfo(const QVariantList& value);
    [[nodiscard]] bool exactPlaneAvailable() const noexcept;
    void setExactPlaneAvailable(bool value);
    [[nodiscard]] bool thresholdEnabled() const noexcept;
    void setThresholdEnabled(bool value);
    [[nodiscard]] qreal threshold() const noexcept;
    void setThreshold(qreal value);
    [[nodiscard]] ThresholdPolicy thresholdPolicy() const noexcept;
    void setThresholdPolicy(ThresholdPolicy value);
    [[nodiscard]] qreal viewCenterX() const noexcept;
    [[nodiscard]] qreal viewCenterY() const noexcept;
    [[nodiscard]] qreal viewScale() const noexcept;
    [[nodiscard]] bool roiEnabled() const noexcept;
    [[nodiscard]] qreal roiLeft() const noexcept;
    [[nodiscard]] qreal roiTop() const noexcept;
    [[nodiscard]] qreal roiRight() const noexcept;
    [[nodiscard]] qreal roiBottom() const noexcept;
    [[nodiscard]] int referenceSlot() const noexcept;
    void setReferenceSlot(int value);
    [[nodiscard]] qulonglong droppedFrames() const noexcept;

    Q_INVOKABLE void zoomAt(qreal normalizedX, qreal normalizedY, qreal factor);
    Q_INVOKABLE void panBy(qreal normalizedDeltaX, qreal normalizedDeltaY);
    Q_INVOKABLE void resetViewport();
    // Fits a normalized source-space rect into the viewport (same product gesture as the
    // image workspace marquee zoom).
    Q_INVOKABLE void zoomToNormalizedRect(qreal left, qreal top, qreal right, qreal bottom);
    Q_INVOKABLE void setRoiNormalized(qreal left, qreal top, qreal right, qreal bottom);
    Q_INVOKABLE void clearRoi();
    Q_INVOKABLE void restoreViewport(qreal centerX,
                                     qreal centerY,
                                     qreal scale,
                                     bool roiEnabled,
                                     qreal roiLeft,
                                     qreal roiTop,
                                     qreal roiRight,
                                     qreal roiBottom);
    Q_INVOKABLE qreal wipePositionForLogicalX(qreal x) const;
    Q_INVOKABLE QVariantMap mapSurfacePoint(qreal x, qreal y) const;

    [[nodiscard]] bool
    attachRendererServices(std::shared_ptr<platform::GraphicsDeviceBroker> deviceBroker,
                           std::shared_ptr<platform::FrameMailbox> frameMailbox,
                           std::shared_ptr<platform::PresentationAckMailbox> acknowledgementMailbox,
                           std::weak_ptr<platform::IRenderActivitySink> activitySink,
                           std::function<std::uint64_t()> droppedFrameProbe = {});
    void detachRendererServices() noexcept;
    [[nodiscard]] bool hasRendererServices() const noexcept;

signals:
    void viewModeChanged();
    void differenceMetricChanged();
    void differenceGainChanged();
    void differenceEdgeChanged();
    void differenceFilterChanged();
    void wipePositionChanged();
    void presentationGeometryChanged();
    void exactPlaneAvailableChanged();
    void thresholdChanged();
    void viewportChanged();
    void referenceSlotChanged();
    void droppedFramesChanged();

protected:
    [[nodiscard]] QSGNode* updatePaintNode(QSGNode* oldNode,
                                           UpdatePaintNodeData* updateData) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    friend class ComparisonRenderNode;

    class Services;

    std::shared_ptr<const Services> services_;
    ViewMode viewMode_ = SideBySide;
    DifferenceMetric differenceMetric_ = RgbAbsolute;
    DifferenceGain differenceGain_ = Gain1x;
    DifferenceEdge differenceEdge_ = Edge0And1;
    DifferenceFilter differenceFilter_ = Bilinear;
    qreal wipePosition_ = 0.5;
    QVariantList sourceDisplayInfo_;
    bool exactPlaneAvailable_ = false;
    bool thresholdEnabled_ = false;
    qreal threshold_ = 0.0;
    ThresholdPolicy thresholdPolicy_ = ThresholdAnyChannel;
    qreal viewCenterX_ = 0.5;
    qreal viewCenterY_ = 0.5;
    qreal viewScale_ = 1.0;
    bool roiEnabled_ = false;
    qreal roiLeft_ = 0.0;
    qreal roiTop_ = 0.0;
    qreal roiRight_ = 1.0;
    qreal roiBottom_ = 1.0;
    int referenceSlot_ = 0;
    std::function<std::uint64_t()> droppedFrameProbe_;
    qulonglong droppedFrames_ = 0U;
};

} // namespace dvs::ui
