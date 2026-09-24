#pragma once

#include <QImage>
#include <QList>
#include <QUrl>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace dvs {
namespace application {
class IIssueRecordRepository;
}
namespace ui {

class ComparisonSurface;
class PairMetricsController;
class PreviewThumbnailController;
class ReviewController;
class ReviewPreferencesController;

struct DesktopApplicationOptions final {
    bool smokeMode = false;
    bool preferSoftwareDevice = false;
    bool preferHighRefreshScreen = false;
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
                            SurfaceBinder bindSurface,
                            PairMetricsController* pairMetrics = nullptr,
                            PreviewThumbnailController* previewThumbnails = nullptr);
    // Optional T7 injection. Must be set before load(); the composition root owns the concrete
    // persistence adapter so ui_qml never links persistence_json directly.
    void setIssueRecordRepository(application::IIssueRecordRepository* repository) noexcept;
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
    [[nodiscard]] std::optional<std::string>
    objectStringPropertyForAutomation(std::string_view objectName,
                                      std::string_view propertyName) const;
    [[nodiscard]] std::optional<int>
    objectIntPropertyForAutomation(std::string_view objectName,
                                   std::string_view propertyName) const;
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
