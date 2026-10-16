#pragma once

#include <QImage>
#include <QList>
#include <QPointF>
#include <QUrl>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace dvs {
namespace application {
class IClipExporter;
class IFileAssociationService;
class IInterpolationProtocolRepository;
class IIssueRecordRepository;
class ISampleExporter;
} // namespace application
namespace ui {

class ComparisonSurface;
class InterpolationProtocolController;
class PairMetricsController;
class PreviewThumbnailController;
class ReviewController;
class ReviewPreferencesController;

struct DesktopApplicationOptions final {
    bool smokeMode = false;
    bool preferSoftwareDevice = false;
    bool preferHighRefreshScreen = false;
    // Performance gates open fixed fixtures and require a deterministic frame-zero baseline, so
    // the automation surface must not fight them: user-session conveniences that move the
    // playhead after an open (resume restore) stay off. Smoke mode already implies determinism
    // because it never advances past the first frame; performance mode plays for minutes and
    // would otherwise both read and leave behind resume entries for the gate fixtures.
    bool performanceAutomation = false;
};

// Owns QGuiApplication, the QML engine, and the top-level window. The composition root is created
// only after this object so Qt's process-wide application state always outlives runtime QObjects.
class DesktopApplication final {
public:
    using SurfaceBinder = std::function<bool(ComparisonSurface&)>;

    DesktopApplication(int& argc,
                       char** argv,
                       DesktopApplicationOptions options = DesktopApplicationOptions{});
    ~DesktopApplication();

    DesktopApplication(const DesktopApplication&) = delete;
    DesktopApplication& operator=(const DesktopApplication&) = delete;
    DesktopApplication(DesktopApplication&&) = delete;
    DesktopApplication& operator=(DesktopApplication&&) = delete;

    [[nodiscard]] bool load(ReviewController& controller,
                            ReviewPreferencesController& preferences,
                            const SurfaceBinder& bindSurface,
                            PairMetricsController* pairMetrics = nullptr,
                            PreviewThumbnailController* previewThumbnails = nullptr);
    // Optional T7 injection. Must be set before load(); the composition root owns the concrete
    // persistence adapter so ui_qml never links persistence_json directly.
    void setIssueRecordRepository(application::IIssueRecordRepository* repository) noexcept;
    // Optional clip-export injection. Must be set before load(); when absent the range export
    // controls stay hidden and the `clipExport` context property is never published.
    void setClipExporter(std::shared_ptr<application::IClipExporter> exporter) noexcept;
    // Optional E01 training-sample export injection. Must be set before load(); when absent
    // sample export controls stay hidden and the `sampleExport` context property is never
    // published.
    void setSampleExporter(std::shared_ptr<application::ISampleExporter> exporter) noexcept;
    // Optional I03 file-association injection. Must be set before load(); when absent the
    // settings dialog's menu entry stays hidden and the `fileAssociation` context property is
    // never published.
    void setFileAssociationService(
        std::shared_ptr<application::IFileAssociationService> service) noexcept;
    // Optional P03 protocol-import injection. Must be set before load(); the composition root
    // owns the concrete persistence adapter so ui_qml never links persistence_json directly.
    // When absent the protocol navigation row stays hidden and the `interpolationProtocol`
    // context property is never published.
    void setInterpolationProtocolRepository(
        application::IInterpolationProtocolRepository* repository) noexcept;
    [[nodiscard]] int exec();
    void exit(int exitCode) noexcept;
    [[nodiscard]] double activeScreenRefreshRate() const noexcept;
    [[nodiscard]] bool enqueueStartupRequest(int kind, const QList<QUrl>& files);
    void activateWindow() noexcept;

    // Drives the same QML properties, button handlers, and window key events as a user. These
    // helpers keep the end-to-end smoke path on the declarative UI boundary instead of calling
    // the controller behind the controls.
    [[nodiscard]] bool openSourcesForAutomation(const QList<QUrl>& sources) noexcept;
    // Opens a still image in the image workspace (smoke/debug and CLI --open-still).
    [[nodiscard]] bool openStillImageForAutomation(const QUrl& url) noexcept;
    // Loads two folders into the image-folder pair model and opens the first complete pair
    // (P4 evidence automation). Returns false when the UI is not ready, the folders fail to
    // load, or no complete pair exists.
    [[nodiscard]] bool loadFolderComparisonForAutomation(const QUrl& left,
                                                         const QUrl& right) noexcept;
    // Automation accessors for the P4 image-folder evidence entry. Non-owning; the returned
    // objects are valid for the lifetime of the DesktopApplication.
    [[nodiscard]] QObject* imageReviewForAutomation() const noexcept;
    [[nodiscard]] QObject* folderPairModelForAutomation() const noexcept;
    [[nodiscard]] bool clickControlForAutomation(std::string_view objectName) noexcept;
    [[nodiscard]] bool focusControlForAutomation(std::string_view objectName) noexcept;
    [[nodiscard]] bool clickTimelineForAutomation(double normalizedPosition) noexcept;
    [[nodiscard]] bool sendKeyForAutomation(int key, int modifiers = 0) noexcept;
    // Posts a window-level hover move in logical coordinates. The UI evidence automation uses
    // it to drive the real hover paths (pixel probe plates, timeline previews) with the same
    // event delivery a moving mouse produces.
    [[nodiscard]] bool sendMouseMoveForAutomation(double sceneX, double sceneY) noexcept;
    // Maps a fractional point (0..1) of the named item into window scene coordinates, so hover
    // automation reaches the real on-screen position. Item x/y properties alone are
    // parent-relative and go stale whenever an ancestor layout moves; mapToScene stays exact.
    [[nodiscard]] std::optional<QPointF> objectScenePointForAutomation(std::string_view objectName,
                                                                       double fractionalX,
                                                                       double fractionalY) const;
    // Invokes a QML function of the named object that takes one numeric argument and returns a
    // string. The pixel-readout row is such a function: since the row became an aligned table,
    // its container is no longer a Text whose "text" property could be read directly.
    [[nodiscard]] std::optional<std::string> objectStringMethodForAutomation(
        std::string_view objectName, std::string_view methodName, int argument) const;
    // Captures the whole top-level window, not just one item - the video evidence automation
    // needs the chrome and the footage in one shot.
    [[nodiscard]] std::optional<QImage> captureWindowForAutomation() const;
    [[nodiscard]] std::optional<std::string>
    objectStringPropertyForAutomation(std::string_view objectName,
                                      std::string_view propertyName) const;
    [[nodiscard]] std::optional<int>
    objectIntPropertyForAutomation(std::string_view objectName,
                                   std::string_view propertyName) const;
    // Generic property read for structured automation facts (e.g. the viewport's
    // pixelReadoutResults map). An invalid QVariant means the object or property was
    // unreachable - callers surface that as "unknown", never as a defaulted value.
    [[nodiscard]] QVariant objectVariantPropertyForAutomation(std::string_view objectName,
                                                              std::string_view propertyName) const;
    // Walks the named item's parent chain and records, per ancestor: objectName, className,
    // visible, opacity, width, height, x, y, z, clip and the mapped scene position - the
    // properties that decide whether the item can actually appear. Read-only diagnostics for
    // the forensic entry points; an unknown item yields an empty list.
    [[nodiscard]] QVariantList itemAncestorReportForAutomation(std::string_view objectName) const;
    // Captures the named visible QML item's rendered pixels. Hardware gates use this to prove
    // that a selected comparison mode produced content, not merely that its button accepted a
    // click.
    [[nodiscard]] std::optional<QImage>
    captureControlForAutomation(std::string_view objectName) const;

    // Opens or closes a QML Menu by invoking its open()/close() slot. Returns whether the
    // invocation succeeded. Used by the popup pixel probe to exercise Popup.Window menus.
    [[nodiscard]] bool openMenuForAutomation(std::string_view objectName) noexcept;
    [[nodiscard]] bool closeMenuForAutomation(std::string_view objectName) noexcept;
    // Returns whether the named QML Menu reports opened == true.
    [[nodiscard]] bool menuIsOpenForAutomation(std::string_view objectName) const noexcept;

    // Captures the rendered pixels of the popup top-level window that contains the named QML
    // object. The popup pixel probe uses this to assert that Popup.Window backgrounds are fully
    // opaque (alpha == 255) across DPI scale factors.
    [[nodiscard]] std::optional<QImage>
    capturePopupWindowForAutomation(std::string_view objectName) const;

    // Called after the runtime has detached its renderer services. Destroying the engine is the
    // scene-graph barrier that releases the render node's pinned GPU publication.
    void releaseSceneGraph() noexcept;

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace ui
} // namespace dvs
