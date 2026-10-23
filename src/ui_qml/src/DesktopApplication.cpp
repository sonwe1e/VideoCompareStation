#include "dvs/ui/DesktopApplication.h"

#include "dvs/application/MediaPaths.h"
#include "dvs/ui/ClipExportController.h"
#include "dvs/ui/ColorClipboardController.h"
#include "dvs/ui/ComparisonExportController.h"
#include "dvs/ui/ComparisonSurface.h"
#include "dvs/ui/DiagnosticsProbe.h"
#include "dvs/ui/EditImageProvider.h"
#include "dvs/ui/FileAssociationController.h"
#include "dvs/ui/ImageEditController.h"
#include "dvs/ui/ImageFolderCursor.h"
#include "dvs/ui/ImageFolderPairModel.h"
#include "dvs/ui/ImageReviewController.h"
#include "dvs/ui/ImageTaskHost.h"
#include "dvs/ui/InterpolationProtocolController.h"
#include "dvs/ui/IssueLogController.h"
#include "dvs/ui/MainWindowDependencies.h"
#include "dvs/ui/PairMetricsController.h"
#include "dvs/ui/QmlTypeRegistration.h"
#include "dvs/ui/ReviewController.h"
#include "dvs/ui/ReviewImageProvider.h"
#include "dvs/ui/ReviewPreferencesController.h"
#include "dvs/ui/ReviewSessionFacade.h"
#include "dvs/ui/ReviewShellController.h"
#include "dvs/ui/SampleExportController.h"
#include "dvs/ui/StartupMilestone.h"
#include "dvs/ui/StartupSplash.h"
#include "dvs/ui/TimingDiagnosticsController.h"
#include "dvs/ui/VideoFolderModel.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickGraphicsConfiguration>
#include <QQuickItem>
#include <QQuickWindow>
#include <QResource>
#include <QScreen>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QtQml/qqml.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

// clang-format off
#include <windows.h>
#include <dwmapi.h>
// clang-format on
#endif

void initializeQmlResources() {
    Q_INIT_RESOURCE(dvs_ui_qml_resources);
}

namespace dvs::ui {
namespace {

#if defined(_WIN32)
void applyWindowsNativeChrome(QQuickWindow* const window) noexcept {
    if (window == nullptr) {
        return;
    }
    // WId-to-HWND reinterpret is the documented Qt/Win32 interop idiom.
    const auto hwnd = reinterpret_cast<HWND>(window->winId()); // NOLINT(performance-no-int-to-ptr)
    if (hwnd == nullptr) {
        return;
    }
    // Measured split: creating the native handle here (before show()) is the suspected cost, not
    // the four DWM attribute round-trips. See docs/engineering/startup-performance-v2.md.
    markStartupMilestone("native-hwnd");

    const auto setAttr = [hwnd](const DWORD attr, const auto* const val) noexcept {
        static_cast<void>(::DwmSetWindowAttribute(hwnd, attr, val, sizeof(*val)));
    };

    // 1. Windows 11 / Windows 10 Immersive Dark Mode for window frame and caption.
    const BOOL darkMode = TRUE;
    constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
    constexpr DWORD kDwmwaUseImmersiveDarkModeBefore20H1 = 19;
    if (FAILED(::DwmSetWindowAttribute(
            hwnd, kDwmwaUseImmersiveDarkMode, &darkMode, sizeof(darkMode)))) {
        setAttr(kDwmwaUseImmersiveDarkModeBefore20H1, &darkMode);
    }

    // 2. Windows 11 Rounded Corners: DWMWA_WINDOW_CORNER_PREFERENCE (33) -> DWMWCP_ROUND (2).
    constexpr DWORD kDwmwaWindowCornerPreference = 33;
    constexpr DWORD kDwmwcpRound = 2;
    setAttr(kDwmwaWindowCornerPreference, &kDwmwcpRound);

    // 3. Caption colour matches Theme.headerBackground (#1c1c1e) in VcsTheme.js, so the title
    //    bar and the menu bar read as one header. No system backdrop is requested: the window
    //    paints an opaque background, so Mica would never show through.
    constexpr DWORD kDwmwaCaptionColor = 35;
    constexpr COLORREF kCaptionColor = RGB(28, 28, 30);
    setAttr(kDwmwaCaptionColor, &kCaptionColor);

    // 4. Caption text matches Theme.primaryText (#f5f5f7).
    constexpr DWORD kDwmwaTextColor = 36;
    constexpr COLORREF kTextColor = RGB(245, 245, 247);
    setAttr(kDwmwaTextColor, &kTextColor);
    markStartupMilestone("native-attrs");
}
#endif

void registerQmlTypes() {
    registerDvsUiTypes();
}

[[nodiscard]] bool disableNativeMenuWindows() {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeMenuWindows);
    return true;
}

} // namespace

class DesktopApplication::Impl final {
public:
    Impl(int& argc, char** argv, DesktopApplicationOptions options)
        : options_(options), nativeMenuWindowsDisabled_(disableNativeMenuWindows()),
          application_(argc, argv) {
        initializeQmlResources();
        registerQmlTypes();
        // Deliberately no setApplicationDisplayName: on Windows the platform window title
        // formatter appends the display name to every title that does not already end with
        // it, which turned "CompareStation 2.5.1 - 视觉审查" into "... - CompareStation" in the
        // title bar, the taskbar and every screenshot (UX-12). Main.qml owns the full title;
        // dialogs that fall back to applicationName still get "CompareStation".
        application_.setApplicationName(QStringLiteral("CompareStation"));
        application_.setApplicationVersion(QString::fromLatin1(DVS_PROJECT_VERSION));
        application_.setOrganizationName(QStringLiteral("CompareStation"));
        application_.setWindowIcon(QIcon{QStringLiteral(":/branding/comparestation-icon.png")});

        // Wire the UI family token (VcsTheme.js `fontFamilyUi`) to the application font. QML's
        // `font` value type carries one family with no fallback list, which is why the token had
        // no consumer; QFont does carry the list, so the fallback chain lives here instead of a
        // single-family binding. Segoe UI Variable ships with Windows 11 (the first family);
        // Windows 10 resolves the classic Segoe UI, and CJK text keeps the system UI face
        // rather than an arbitrary substitute. Text that should keep the platform default still
        // leaves `font.family` unset, as the token's comment promises.
        QFont uiFont = QGuiApplication::font();
        uiFont.setFamilies({QStringLiteral("Segoe UI Variable Text"),
                            QStringLiteral("Segoe UI"),
                            QStringLiteral("Microsoft YaHei UI")});
        QGuiApplication::setFont(uiFont);
    }

    ~Impl() {
        releaseSceneGraph();
    }

    void setIssueRecordRepository(application::IIssueRecordRepository* repository) noexcept {
        issueRecordRepository_ = repository;
    }

    void setInterpolationProtocolRepository(
        application::IInterpolationProtocolRepository* repository) noexcept {
        interpolationProtocolRepository_ = repository;
    }

    void setClipExporter(std::shared_ptr<application::IClipExporter> exporter) noexcept {
        clipExporter_ = std::move(exporter);
    }

    void setSampleExporter(std::shared_ptr<application::ISampleExporter> exporter) noexcept {
        sampleExporter_ = std::move(exporter);
    }

    void setFileAssociationService(
        std::shared_ptr<application::IFileAssociationService> service) noexcept {
        fileAssociationService_ = std::move(service);
    }

    [[nodiscard]] bool load(ReviewController& controller,
                            ReviewPreferencesController& preferences,
                            const SurfaceBinder& bindSurface,
                            PairMetricsController* pairMetrics,
                            PreviewThumbnailController* previewThumbnails) {
        if (engine_ || !bindSurface) {
            return false;
        }
        markStartupMilestone("load-enter");

        // B2: the splash is a guard, not a plain local, so every `return false`
        // below dismisses it on the way out and no early failure can leave an
        // orphan window on screen. On success the ownership moves to splash_,
        // which survives past window->show() and is dismissed when the first
        // frame has actually been presented (frameSwapped): between show() and
        // that swap the window is on screen but unrendered - a black block.
        auto splash = std::make_unique<StartupSplash>();
        if (!options_.smokeMode) {
            splash->show("CompareStation", QObject::tr("正在准备审阅环境").toStdString());
        }
        struct SplashGuard final {
            std::unique_ptr<StartupSplash> pending;
            ~SplashGuard() {
                if (pending) {
                    pending->requestDismiss();
                    pending->join();
                }
            }
        } splashGuard{std::move(splash)};

        qmlWarnings_.clear();
        auto engine = std::make_unique<QQmlApplicationEngine>();
        engine->addImportPath(
            QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
        // Automation determinism: QML reads this to keep user-session conveniences that move the
        // playhead (resume restore) out of the performance gates' frame-zero baseline. Smoke runs
        // share that contract: a developer profile holding a resume position for a smoke fixture
        // must not drag the smoke's own frame-zero stage machine off frame 0. Absent in every
        // other embedding, so QML must guard the read.
        engine->rootContext()->setContextProperty(
            QStringLiteral("dvsPerformanceAutomation"),
            QVariant{options_.performanceAutomation || options_.smokeMode});
        // Pair metrics stay optional so isolated harnesses can load the shell without a
        // metrics service; the QML guards every read on the context property being set.
        if (pairMetrics != nullptr) {
            engine->rootContext()->setContextProperty(QStringLiteral("pairMetrics"), pairMetrics);
        }
        if (previewThumbnails != nullptr) {
            engine->rootContext()->setContextProperty(QStringLiteral("previewThumbnails"),
                                                      previewThumbnails);
            engine->addImageProvider(QStringLiteral("timeline-preview"),
                                     new TimelinePreviewImageProvider(previewThumbnails));
        }
        shellController_ = std::make_unique<ReviewShellController>(controller, preferences);
        videoFolder_ = std::make_unique<VideoFolderModel>();
        videoFolder_->attachPreferences(preferences);
        videoFolder_->attachPlayback(controller, *shellController_);
        engine->rootContext()->setContextProperty(QStringLiteral("videoFolder"),
                                                  videoFolder_.get());
        sessionFacade_ =
            std::make_unique<ReviewSessionFacade>(controller, preferences, *shellController_);
        engine->rootContext()->setContextProperty(QStringLiteral("reviewFacade"),
                                                  sessionFacade_.get());
        imageReview_ = std::make_unique<ImageReviewController>();
        imageTasks_ = std::make_unique<ImageTaskHost>(imageReview_.get());
        engine->rootContext()->setContextProperty(QStringLiteral("imageTasks"), imageTasks_.get());
        engine->addImageProvider(
            QStringLiteral("vcs-review"),
            new ReviewImageProvider(
                imageReview_.get(), [host = imageTasks_.get()](const QString& token) {
                    return host != nullptr ? host->controllerForToken(token) : nullptr;
                }));
        // Step-3 image editing: an edit session always starts from the committed decoded
        // buffer (never a derived channel view) and saves to a copy. The editable source is
        // the active task's controller, so switching tabs cannot edit another tab's pixels.
        imageEdit_ = std::make_unique<ImageEditController>();
        imageEdit_->setSourceImageProvider([host = imageTasks_.get()](const int slot) {
            ImageReviewController* const active =
                host != nullptr ? host->activeController() : nullptr;
            return active != nullptr ? active->rawImageForSlot(slot) : QImage{};
        });
        engine->rootContext()->setContextProperty(QStringLiteral("imageEdit"), imageEdit_.get());
        engine->addImageProvider(QStringLiteral("dvs-edit"),
                                 new EditImageProvider(imageEdit_.get()));
        folderPairs_ = std::make_unique<ImageFolderPairModel>();
        folderPairs_->setAsyncPairOpener(
            [host = imageTasks_.get()](
                const QUrl& primary, const QUrl& secondary, const int pairId, QString* error) {
                // Folder rows open into the active image tab, creating one only when the
                // workspace is still empty. The host keeps the previous committed pair
                // visible until both sides decode, then emits openFinished once.
                return host != nullptr
                           ? host->requestOpenPairInActiveTask(primary, secondary, pairId, error)
                           : -1;
            });
        folderPairs_->setAsyncPairCancel([host = imageTasks_.get()](const int requestId) {
            if (host != nullptr) {
                host->cancelOpenRequest(requestId);
            }
        });
        // T6: single-sided rows open the side that exists. The row identity rides on the
        // request as pairId, so the completion can advance that row's selection; each
        // terminal handler ignores request ids it does not own.
        folderPairs_->setSingleSideOpener(
            [host = imageTasks_.get()](const QUrl& url, const int row, QString* error) {
                return host != nullptr ? host->requestOpenPrimaryInActiveTask(url, row, error) : -1;
            });
        QObject::connect(
            imageTasks_.get(),
            &ImageTaskHost::controllerOpenFinished,
            folderPairs_.get(),
            [folderPairs = folderPairs_.get()](
                const int requestId, const int pairId, const bool success, const QString& error) {
                if (pairId >= 0) {
                    // Folder rows use the row index as pairId; loose image opens use -1 and
                    // must never move the sidebar selection. Pair and single-side pending
                    // states each ignore request ids they do not own.
                    folderPairs->completePairOpen(static_cast<quint64>(requestId), success, error);
                    folderPairs->completeSingleSideOpen(
                        static_cast<quint64>(requestId), success, error);
                }
            });
        engine->rootContext()->setContextProperty(QStringLiteral("imageFolderPairs"),
                                                  folderPairs_.get());
        // FLOW3 single-directory browsing. The cursor only remembers the neighbour list; every
        // step still goes through the ordinary asynchronous open so a failed decode keeps the
        // current image and stale requests stay unable to commit.
        imageFolderCursor_ = std::make_unique<ImageFolderCursor>();
        engine->rootContext()->setContextProperty(QStringLiteral("imageFolderCursor"),
                                                  imageFolderCursor_.get());
        issueLog_ = std::make_unique<IssueLogController>(issueRecordRepository_);
        issueLog_->setReviewController(&controller);
        issueLog_->setPreferences(&preferences);
        issueLog_->setFolderModel(folderPairs_.get());
        issueLog_->setImageController(imageReview_.get());
        QObject::connect(imageTasks_.get(), &ImageTaskHost::activeChanged, issueLog_.get(), [this] {
            ImageReviewController* const active =
                imageTasks_ != nullptr ? imageTasks_->activeController() : nullptr;
            issueLog_->setImageController(active != nullptr ? active : imageReview_.get());
        });
        engine->rootContext()->setContextProperty(QStringLiteral("issueLog"), issueLog_.get());
        // P03 protocol import and generated-frame navigation. Optional like the issue log: the
        // composition root owns the persistence adapter, and when it is absent the protocol row
        // stays hidden because Main.qml guards every read on the context property.
        if (interpolationProtocolRepository_ != nullptr) {
            interpolationProtocol_ =
                std::make_unique<InterpolationProtocolController>(interpolationProtocolRepository_);
            interpolationProtocol_->setReviewController(&controller);
            engine->rootContext()->setContextProperty(QStringLiteral("interpolationProtocol"),
                                                      interpolationProtocol_.get());
        }
        // Step-2 comparison export: clipboard/save capture of the comparison viewport
        // with an annotation bar. Optional so isolated harnesses can omit it; Main.qml
        // guards every read on the context property being set.
        comparisonExport_ = std::make_unique<ComparisonExportController>();
        engine->rootContext()->setContextProperty(QStringLiteral("comparisonExport"),
                                                  comparisonExport_.get());
        // Pixel-probe colour copy. The composition root owns the adapter and hands it to
        // the root object below; a session without a GUI clipboard is reported by the
        // adapter itself (copyText verifies its write and reports the failure).
        colorClipboard_ = std::make_unique<ColorClipboardController>();
        // Range clip export: only offered when the composition root injected an exporter
        // adapter (ui_qml never links media_ffmpeg). Main.qml guards every read on the
        // context property being set, so isolated harnesses can omit it.
        if (clipExporter_) {
            clipExport_ = std::make_unique<ClipExportController>(ClipExportController::Dependencies{
                .snapshot = [&controller] { return controller.currentSnapshot(); },
                .exporter = clipExporter_,
            });
            QObject::connect(&controller,
                             &ReviewController::snapshotRefreshed,
                             clipExport_.get(),
                             &ClipExportController::refreshAvailability);
            engine->rootContext()->setContextProperty(QStringLiteral("clipExport"),
                                                      clipExport_.get());
        }
        // E01 training-sample export: only offered when the composition root injected a sample
        // exporter adapter (ui_qml never links media_ffmpeg).
        if (sampleExporter_) {
            sampleExport_ =
                std::make_unique<SampleExportController>(SampleExportController::Dependencies{
                    .snapshot = [&controller] { return controller.currentSnapshot(); },
                    .exporter = sampleExporter_,
                    .protocol = [proto = interpolationProtocol_.get()]()
                        -> const std::optional<domain::InterpolationProtocol>& {
                        static const std::optional<domain::InterpolationProtocol> kEmpty;
                        return proto != nullptr ? proto->protocol() : kEmpty;
                    },
                    .activeCropRect =
                        [this, &controller]() -> std::optional<application::SampleCropRect> {
                        if (surface_ != nullptr && surface_->roiEnabled()) {
                            const auto snapshot = controller.currentSnapshot();
                            if (snapshot && snapshot->validatedComparison) {
                                const auto& extent =
                                    snapshot->validatedComparison->canonicalDescriptor().extent;
                                if (extent.isValid()) {
                                    const auto w = static_cast<double>(extent.width);
                                    const auto h = static_cast<double>(extent.height);
                                    const auto cx = static_cast<std::int64_t>(
                                        std::clamp(surface_->roiLeft() * w, 0.0, w - 1.0));
                                    const auto cy = static_cast<std::int64_t>(
                                        std::clamp(surface_->roiTop() * h, 0.0, h - 1.0));
                                    const auto cw = static_cast<std::int64_t>(
                                        std::clamp((surface_->roiRight() - surface_->roiLeft()) * w,
                                                   1.0,
                                                   w - static_cast<double>(cx)));
                                    const auto ch = static_cast<std::int64_t>(
                                        std::clamp((surface_->roiBottom() - surface_->roiTop()) * h,
                                                   1.0,
                                                   h - static_cast<double>(cy)));
                                    return application::SampleCropRect{cx, cy, cw, ch};
                                }
                            }
                        }
                        return std::nullopt;
                    },
                });
            engine->rootContext()->setContextProperty(QStringLiteral("sampleExport"),
                                                      sampleExport_.get());
        }
        // I03 file-association settings: the Windows default-app registration port, injected by
        // the composition root. ui_qml never links shell_windows; without the service the
        // dialog's menu entry stays hidden, so lightweight harnesses stay valid.
        if (fileAssociationService_) {
            fileAssociation_ = std::make_unique<FileAssociationController>(fileAssociationService_);
            engine->rootContext()->setContextProperty(QStringLiteral("fileAssociation"),
                                                      fileAssociation_.get());
        }
        // UI observation bridge: forwards QML scene-graph operations (timeline thumbnail grabs)
        // into the bounded trace buffer so playback evidence can correlate them with pipeline
        // timing. With tracing disabled every call is one atomic load and a branch.
        diagnosticsProbe_ = std::make_unique<DiagnosticsProbe>();
        engine->rootContext()->setContextProperty(QStringLiteral("dvsDiagnostics"),
                                                  diagnosticsProbe_.get());
        // E01: construction only builds lightweight state now. The presentation capability probe
        // (hidden window, D3D11 device, swap chain) is requested by TabbedInspector when the
        // diagnostics surface opens and runs on the controller's worker thread, so nothing here
        // blocks engine->load() below.
        timingDiagnostics_ = std::make_unique<TimingDiagnosticsController>();
        engine->rootContext()->setContextProperty(QStringLiteral("timingDiagnostics"),
                                                  timingDiagnostics_.get());
        const QMetaObject::Connection warningConnection = QObject::connect(
            engine.get(),
            &QQmlEngine::warnings,
            engine.get(),
            [this](const QList<QQmlError>& warnings) {
                qmlWarnings_.insert(qmlWarnings_.end(), warnings.cbegin(), warnings.cend());
            });
        markStartupMilestone("context-ready");
        // The root declares its pilot services as required typed properties; the
        // composition root supplies them as initial properties, so a missing service
        // fails creation instead of degrading an unseen workspace.
        engine->setInitialProperties(mainWindowPilotProperties(*imageReview_, *colorClipboard_));
        engine->load(QUrl{QStringLiteral("qrc:/qml/Main.qml")});
        markStartupMilestone("qml-loaded");
        static_cast<void>(QObject::disconnect(warningConnection));
        if (engine->rootObjects().size() != 1) {
            reportWarnings(qmlWarnings_);
            return false;
        }

        auto* const window = qobject_cast<QQuickWindow*>(engine->rootObjects().front());
        auto* const surface =
            window != nullptr
                ? window->findChild<ComparisonSurface*>(QStringLiteral("dualVideoSurface"))
                : nullptr;
        if (window == nullptr || surface == nullptr || !bindSurface(*surface)) {
            reportWarnings(qmlWarnings_);
            return false;
        }
        // Creation already fails when a required property is absent; an explicit null or
        // a wrong-typed object only degrades the property to null with a warning, so the
        // services' identity is verified before the window is shown.
        QString dependencyError;
        if (!verifyMainWindowPilotServices(
                *window, *imageReview_, *colorClipboard_, &dependencyError)) {
            qWarning("%s", qUtf8Printable(dependencyError));
            reportWarnings(qmlWarnings_);
            return false;
        }
        markStartupMilestone("surface-bound");
        // Fires on the render thread while show() may still be blocking the GUI thread;
        // DirectConnection keeps the mark at the true signal time instead of queueing it
        // into the event loop that has not started yet.
        auto sceneGraphInitializedMarked = std::make_shared<bool>(false);
        QObject::connect(
            window,
            &QQuickWindow::sceneGraphInitialized,
            window,
            [sceneGraphInitializedMarked, this] {
                if (!*sceneGraphInitializedMarked) {
                    *sceneGraphInitializedMarked = true;
                    markStartupMilestone("sg-initialized");
                    // Not the splash dismissal point: the scene graph existing only
                    // proves the renderer is ready, and the window stays an
                    // unrendered black block until the first frame is swapped. The
                    // frameSwapped handler below owns the dismissal.
                }
            },
            Qt::DirectConnection);

        // E01 startup milestones: the first presented main-window frame is the honest
        // "interactive" mark (sg-initialized only proves the scene graph exists), and the first
        // presented material frame separates window readiness from content readiness. Both are
        // one-shot marks printed under DVS_STARTUP_TIMING alongside the existing milestones.
        auto windowFirstFrameMarked = std::make_shared<bool>(false);
        QObject::connect(
            window,
            &QQuickWindow::frameSwapped,
            window,
            [windowFirstFrameMarked, window, this] {
                if (!*windowFirstFrameMarked) {
                    *windowFirstFrameMarked = true;
                    markStartupMilestone("window-first-frame");
                    // The window was shown with opacity 0 because between show() and
                    // this first swap it is on screen but unrendered - the black
                    // block. The first presented frame is therefore also the reveal
                    // moment: make the window visible and dismiss the splash, whose
                    // whole job is to cover that gap. setOpacity must run on the GUI
                    // thread, so it is queued (the event loop starts right after
                    // load()); requestDismiss() is non-blocking and safe from this
                    // render thread. The worker ends on its own and is reaped by the
                    // splash destructor (or by its lifetime ceiling if no frame ever
                    // arrives). splash_ is assigned before show(), so no frame can
                    // swap before the pointer is set.
                    if (!options_.smokeMode) {
                        QMetaObject::invokeMethod(
                            window, [window] { window->setOpacity(1.0); }, Qt::QueuedConnection);
                    }
                    if (splash_ != nullptr) {
                        splash_->requestDismiss();
                    }
                }
            },
            Qt::DirectConnection);

        auto sourceFirstFrameMarked = std::make_shared<bool>(false);
        QObject::connect(&controller,
                         &ReviewController::frameStateChanged,
                         &controller,
                         [controller = &controller, sourceFirstFrameMarked] {
                             if (!*sourceFirstFrameMarked && controller->currentFrame() >= 0) {
                                 *sourceFirstFrameMarked = true;
                                 markStartupMilestone("source-first-frame");
                             }
                         });

        QQuickGraphicsConfiguration configuration;
        configuration.setPreferSoftwareDevice(options_.preferSoftwareDevice);
        configuration.setDepthBufferFor2D(true);
        window->setGraphicsConfiguration(configuration);
        markStartupMilestone("graphics-config");
        if (options_.preferHighRefreshScreen) {
            const QList<QScreen*> screens = QGuiApplication::screens();
            const auto selected = std::max_element(
                screens.cbegin(), screens.cend(), [](const auto* lhs, const auto* rhs) {
                    return lhs->refreshRate() < rhs->refreshRate();
                });
            if (selected != screens.cend() && *selected != nullptr) {
                const QRect available = (*selected)->availableGeometry();
                window->setScreen(*selected);
                window->setPosition(available.center() -
                                    QPoint{window->width() / 2, window->height() / 2});
                activeScreenRefreshRate_ = (*selected)->refreshRate();
            }
        }
        markStartupMilestone("screen-select");
        if (options_.smokeMode) {
            window->setFlags(Qt::Tool | Qt::FramelessWindowHint);
            window->setOpacity(0.0);
            window->resize(960, 640);
            if (QScreen* const screen = QGuiApplication::primaryScreen()) {
                const QRect desktop = screen->virtualGeometry();
                window->setPosition(desktop.right() + 32, desktop.bottom() + 32);
            }
        }

        engine_ = std::move(engine);
        window_ = window;
        surface_ = surface;
        if (issueLog_ != nullptr && surface_ != nullptr) {
            issueLog_->setVideoSurface(surface_);
        }
        windowDestroyedConnection_ =
            QObject::connect(window, &QObject::destroyed, [this] { window_ = nullptr; });
        surfaceDestroyedConnection_ =
            QObject::connect(surface, &QObject::destroyed, [this] { surface_ = nullptr; });
// Native chrome stays BEFORE show(). An A/B ran moving it after show() (B0-fix trial):
// the 22 ms winId() cost did not disappear, it was absorbed by show() instead, and
// show-enter -> sg-initialized grew from ~168 ms to ~214-235 ms while the window still
// appeared no earlier. post-show -> native-hwnd measuring 0.2 ms confirms show()
// creates the HWND anyway, so deferring only relocated the cost and added a
// title-bar flash risk. Kept pre-show on that evidence.
// See docs/engineering/startup-performance-v2.md (Step 8).
#if defined(_WIN32)
        if (!options_.smokeMode) {
            applyWindowsNativeChrome(window_);
        }
#endif
        markStartupMilestone("native-chrome");
        // Hand the splash over before show(): show() is where the render thread
        // starts, and both the sceneGraphInitialized and frameSwapped handlers run
        // on that thread - the latter reads splash_ to dismiss it. Assigning
        // afterwards both loses the dismissal and races the render thread's read.
        splash_ = std::move(splashGuard.pending);
        // The window must go through show() before it can render anything (show is
        // what starts the scene graph), and between show() and the first swap the
        // compositor fills its surface with black. Showing it fully transparent
        // keeps the pipeline identical while making that black block invisible; the
        // frameSwapped handler reveals it once real content exists. Smoke mode
        // stays invisible for its whole run and sets its own opacity below.
        if (!options_.smokeMode) {
            window->setOpacity(0.0);
        }
        markStartupMilestone("show-enter");
        window_->show();
        markStartupMilestone("post-show");
        window_->raise();
        window_->requestActivate();
#if defined(_WIN32)
        if (!options_.smokeMode) {
            // Also ask the shell directly so a launch from Explorer lands in front.
            if (const auto hwnd =
                    reinterpret_cast<HWND>(window_->winId()); // NOLINT(performance-no-int-to-ptr)
                hwnd != nullptr) {
                ::SetForegroundWindow(hwnd);
            }
        }
#endif
        window_->requestUpdate();
        surface_->update();
        if (window_->screen() != nullptr) {
            activeScreenRefreshRate_ = window_->screen()->refreshRate();
        }
        markStartupMilestone("window-shown");
        return true;
    }

    [[nodiscard]] int exec() {
        if (!engine_ || window_ == nullptr) {
            return EXIT_FAILURE;
        }
        // exec() deliberately does not dismiss the splash. Dismissal belongs to the
        // first frameSwapped, which may only fire once this event loop is running;
        // force-dismissing here would re-expose the gap the splash is covering, and
        // joining an undismissed worker would block the GUI thread before the loop
        // that the first frame needs. The worker ends itself after the dismissal (or
        // its lifetime ceiling); the splash destructor, which runs after this loop
        // returns, reaps the thread.
        if (!options_.smokeMode) {
            // Safety net for the opacity-0 reveal: if no frame was ever swapped, the
            // queued setOpacity(1.0) never ran and a permanently transparent window
            // would hide even the error UI, so after a bound far beyond any healthy
            // startup the window is revealed regardless. Idempotent otherwise.
            QTimer::singleShot(3'000, window_, [window = window_] { window->setOpacity(1.0); });
        }
        return application_.exec();
    }

    void exit(const int exitCode) noexcept {
        application_.exit(exitCode);
    }

    [[nodiscard]] double activeScreenRefreshRate() const noexcept {
        return activeScreenRefreshRate_;
    }

    [[nodiscard]] bool enqueueStartupRequest(const int kind, const QList<QUrl>& files) {
        if (window_ == nullptr || shellController_ == nullptr || kind < 0) {
            return false;
        }
        if (isStillImageInvocation(files)) {
            return openStillImages(files);
        }
        QVariantList values;
        values.reserve(files.size());
        for (const QUrl& file : files) {
            values.push_back(file);
        }
        return shellController_->enqueueStartupRequest(kind, values);
    }

    // One or two still images belong to the image workspace, not to the video session runner: a PNG
    // handed to the video path is probed by FFmpeg and fails. Every other selection, including a
    // mixed one, keeps the video route it had before.
    [[nodiscard]] static bool isStillImageInvocation(const QList<QUrl>& files) {
        std::vector<std::filesystem::path> paths;
        paths.reserve(static_cast<std::size_t>(files.size()));
        for (const QUrl& file : files) {
            const QString localPath = file.toLocalFile();
            if (localPath.isEmpty()) {
                return false;
            }
            paths.emplace_back(localPath.toStdWString());
        }
        return application::isStillImageInvocation(paths);
    }

    // Route through the same QML commit boundary as the menu, drop and automation paths so
    // workspace intent and committed task identity stay consistent.
    [[nodiscard]] bool openStillImages(const QList<QUrl>& files) {
        QVariantList urls;
        urls.reserve(files.size());
        for (const QUrl& file : files) {
            urls.push_back(file);
        }
        QVariant opened;
        if (!QMetaObject::invokeMethod(window_,
                                       "openImageInNewTab",
                                       Q_RETURN_ARG(QVariant, opened),
                                       Q_ARG(QVariant, QVariant{urls}))) {
            return false;
        }
        return opened.toBool();
    }

    void activateWindow() noexcept {
        if (window_ == nullptr) {
            return;
        }
        // Restore a minimised window without touching its other states: a maximised or
        // full-screen review must stay that way when another file is sent to it.
        if (Qt::WindowStates states = window_->windowStates();
            states.testFlag(Qt::WindowMinimized)) {
            states.setFlag(Qt::WindowMinimized, false);
            window_->setWindowStates(states);
        }
        window_->show();
        window_->raise();
        window_->requestActivate();
#if defined(_WIN32)
        if (const auto hwnd =
                reinterpret_cast<HWND>(window_->winId()); // NOLINT(performance-no-int-to-ptr)
            hwnd != nullptr) {
            ::SetForegroundWindow(hwnd);
        }
#endif
    }

    [[nodiscard]] bool openSourcesForAutomation(const QList<QUrl>& sources) noexcept {
        if (shellController_ == nullptr || sources.isEmpty() || sources.size() > 3) {
            return false;
        }
        QVariantList values;
        values.reserve(sources.size());
        for (const QUrl& source : sources) {
            values.push_back(source);
        }
        if (!shellController_->stageSources(values, 0)) {
            return false;
        }
        // Automation reports whether the UI accepted the action. Media validation failures are
        // intentionally projected through ReviewController and asserted by the smoke state machine,
        // so a synchronous submit rejection must not be reported as an automation failure here.
        static_cast<void>(shellController_->openStagedSources(false));
        static_cast<void>(
            QMetaObject::invokeMethod(window_, "activateWorkspace", Q_ARG(QVariant, QVariant{0})));
        return true;
    }

    [[nodiscard]] bool openStillImageForAutomation(const QUrl& url) noexcept {
        if (window_ == nullptr || !url.isValid()) {
            return false;
        }
        // Route through the same QML commit boundary as the menu/drop paths so workspace
        // intent and committed task identity stay consistent under automation.
        QVariantList urls;
        urls.push_back(url);
        QVariant opened;
        if (!QMetaObject::invokeMethod(window_,
                                       "openImageInNewTab",
                                       Q_RETURN_ARG(QVariant, opened),
                                       Q_ARG(QVariant, QVariant{urls}))) {
            return false;
        }
        return opened.toBool();
    }

    [[nodiscard]] bool loadFolderComparisonForAutomation(const QUrl& left,
                                                         const QUrl& right) noexcept {
        if (folderPairs_ == nullptr || window_ == nullptr || !left.isValid() || !right.isValid()) {
            return false;
        }
        if (!window_->setProperty("imageFolderLeftUrl", left) ||
            !window_->setProperty("imageFolderRightUrl", right)) {
            return false;
        }
        QVariant loaded;
        if (!QMetaObject::invokeMethod(
                window_, "loadFolderComparison", Q_RETURN_ARG(QVariant, loaded))) {
            return false;
        }
        return loaded.toBool();
    }

    [[nodiscard]] QObject* imageReviewForAutomation() const noexcept {
        return imageReview_.get();
    }

    [[nodiscard]] QObject* folderPairModelForAutomation() const noexcept {
        return folderPairs_.get();
    }

    [[nodiscard]] bool clickControlForAutomation(const std::string_view objectName) noexcept {
        if (window_ == nullptr || objectName.empty()) {
            return false;
        }
        QObject* const control = window_->findChild<QObject*>(
            QString::fromUtf8(objectName.data(), static_cast<qsizetype>(objectName.size())));
        if (control != nullptr && !control->property("enabled").toBool()) {
            QObject* const transport = window_->findChild<QObject*>(QStringLiteral("transport"));
            if (transport != nullptr && transport->property("controllerState").toInt() == 1) {
                // Automation mirrors the user's bottom-edge wake gesture before clicking an
                // auto-hidden OSC control. Normal pointer input remains blocked while hidden.
                static_cast<void>(transport->setProperty("revealActive", true));
            }
        }
        if (control == nullptr || !control->property("enabled").toBool()) {
            return false;
        }
        if (control->metaObject()->indexOfMethod("click()") >= 0) {
            return QMetaObject::invokeMethod(control, "click", Qt::DirectConnection);
        }
        // Custom QML chips expose pointer handlers rather than AbstractButton::click().
        // Drive their real hit path, including effective visibility and window bounds.
        auto* const item = qobject_cast<QQuickItem*>(control);
        if (item == nullptr || !item->isVisible() || item->width() <= 0 || item->height() <= 0) {
            return false;
        }
        const QPointF local = item->mapToScene({item->width() / 2, item->height() / 2});
        if (local.x() < 0 || local.y() < 0 || local.x() >= window_->width() ||
            local.y() >= window_->height()) {
            return false;
        }
        const QPointF global = window_->mapToGlobal(local.toPoint());
        QMouseEvent press{QEvent::MouseButtonPress,
                          local,
                          global,
                          Qt::LeftButton,
                          Qt::LeftButton,
                          Qt::NoModifier};
        QMouseEvent release{QEvent::MouseButtonRelease,
                            local,
                            global,
                            Qt::LeftButton,
                            Qt::NoButton,
                            Qt::NoModifier};
        const bool pressed = QCoreApplication::sendEvent(window_, &press);
        return QCoreApplication::sendEvent(window_, &release) && pressed;
    }

    [[nodiscard]] bool focusControlForAutomation(const std::string_view objectName) noexcept {
        if (window_ == nullptr || objectName.empty()) {
            return false;
        }
        auto* const item = window_->findChild<QQuickItem*>(
            QString::fromUtf8(objectName.data(), static_cast<qsizetype>(objectName.size())));
        if (item == nullptr || !item->isEnabled() || !item->isVisible()) {
            return false;
        }
        item->forceActiveFocus(Qt::OtherFocusReason);
        return item->hasActiveFocus();
    }

    [[nodiscard]] bool clickTimelineForAutomation(const double normalizedPosition) noexcept {
        if (window_ == nullptr || !std::isfinite(normalizedPosition) || normalizedPosition < 0.0 ||
            normalizedPosition > 1.0) {
            return false;
        }
        auto* const timeline = window_->findChild<QQuickItem*>(QStringLiteral("timelineSlider"));
        if (timeline == nullptr || !timeline->isEnabled() || timeline->width() <= 0.0 ||
            timeline->height() <= 0.0) {
            return false;
        }
        const qreal horizontalInset = (std::min)(0.5, timeline->width() * 0.25);
        const qreal localX = (std::clamp)(timeline->width() * normalizedPosition,
                                          horizontalInset,
                                          timeline->width() - horizontalInset);
        const QPointF scenePosition =
            timeline->mapToScene(QPointF{localX, timeline->height() * 0.5});
        const QPointF globalPosition = window_->mapToGlobal(scenePosition.toPoint());
        QMouseEvent press{QEvent::MouseButtonPress,
                          scenePosition,
                          globalPosition,
                          Qt::LeftButton,
                          Qt::LeftButton,
                          Qt::NoModifier};
        QMouseEvent release{QEvent::MouseButtonRelease,
                            scenePosition,
                            globalPosition,
                            Qt::LeftButton,
                            Qt::NoButton,
                            Qt::NoModifier};
        const bool pressed = QCoreApplication::sendEvent(window_, &press);
        const bool released = QCoreApplication::sendEvent(window_, &release);
        return pressed && released;
    }

    [[nodiscard]] bool sendKeyForAutomation(const int key, const int modifiers) noexcept {
        if (window_ == nullptr || key == 0) {
            return false;
        }
        const Qt::KeyboardModifiers keyboardModifiers{modifiers};
        QKeyEvent press{QEvent::KeyPress, key, keyboardModifiers};
        QKeyEvent release{QEvent::KeyRelease, key, keyboardModifiers};
        static_cast<void>(QCoreApplication::sendEvent(window_, &press));
        static_cast<void>(QCoreApplication::sendEvent(window_, &release));
        return true;
    }

    [[nodiscard]] bool sendMouseMoveForAutomation(const double sceneX,
                                                  const double sceneY) noexcept {
        if (window_ == nullptr || !std::isfinite(sceneX) || !std::isfinite(sceneY) ||
            sceneX < 0.0 || sceneY < 0.0 || sceneX > window_->width() ||
            sceneY > window_->height()) {
            return false;
        }
        const QPointF localPosition{sceneX, sceneY};
        const QPointF globalPosition = window_->mapToGlobal(localPosition.toPoint());
        QMouseEvent move{QEvent::MouseMove,
                         localPosition,
                         globalPosition,
                         Qt::NoButton,
                         Qt::NoButton,
                         Qt::NoModifier};
        return QCoreApplication::sendEvent(window_, &move);
    }

    [[nodiscard]] std::optional<QPointF>
    objectScenePointForAutomation(const std::string_view objectName,
                                  const double fractionalX,
                                  const double fractionalY) const {
        if (window_ == nullptr) {
            return std::nullopt;
        }
        auto* const item = window_->findChild<QQuickItem*>(
            QString::fromUtf8(objectName.data(), static_cast<qsizetype>(objectName.size())));
        if (item == nullptr || item->width() <= 0.0 || item->height() <= 0.0) {
            return std::nullopt;
        }
        return item->mapToScene(QPointF{item->width() * fractionalX, item->height() * fractionalY});
    }

    [[nodiscard]] std::optional<std::string>
    objectStringMethodForAutomation(const std::string_view objectName,
                                    const std::string_view methodName,
                                    const int argument) const {
        if (window_ == nullptr || objectName.empty() || methodName.empty()) {
            return std::nullopt;
        }
        QObject* const object = window_->findChild<QObject*>(
            QString::fromUtf8(objectName.data(), static_cast<qsizetype>(objectName.size())));
        if (object == nullptr) {
            return std::nullopt;
        }
        QVariant result;
        // The const char* invokeMethod overload (the one the contract suite uses for the same
        // QML function) resolves QML-defined methods; the QString overload does not accept the
        // return/argument macro combination.
        const std::string method{methodName};
        const bool invoked = QMetaObject::invokeMethod(object,
                                                       method.c_str(),
                                                       Q_RETURN_ARG(QVariant, result),
                                                       Q_ARG(QVariant, QVariant{argument}));
        if (!invoked || !result.isValid()) {
            return std::nullopt;
        }
        return std::optional<std::string>{result.toString().toStdString()};
    }

    [[nodiscard]] std::optional<QImage> captureWindowForAutomation() const {
        if (window_ == nullptr || window_->width() <= 0 || window_->height() <= 0) {
            return std::nullopt;
        }
        QImage image = window_->grabWindow();
        if (image.isNull()) {
            return std::nullopt;
        }
        return image;
    }

    [[nodiscard]] std::optional<std::string>
    objectStringPropertyForAutomation(const std::string_view objectName,
                                      const std::string_view propertyName) const {
        if (window_ == nullptr || objectName.empty() || propertyName.empty()) {
            return std::nullopt;
        }
        const QObject* const object = window_->findChild<QObject*>(
            QString::fromUtf8(objectName.data(), static_cast<qsizetype>(objectName.size())));
        if (object == nullptr) {
            return std::nullopt;
        }
        const std::string property{propertyName};
        const QVariant value = object->property(property.c_str());
        return value.isValid() ? std::optional<std::string>{value.toString().toStdString()}
                               : std::nullopt;
    }

    [[nodiscard]] std::optional<int>
    objectIntPropertyForAutomation(const std::string_view objectName,
                                   const std::string_view propertyName) const {
        if (window_ == nullptr || objectName.empty() || propertyName.empty()) {
            return std::nullopt;
        }
        const QObject* const object = window_->findChild<QObject*>(
            QString::fromUtf8(objectName.data(), static_cast<qsizetype>(objectName.size())));
        if (object == nullptr) {
            return std::nullopt;
        }
        const std::string property{propertyName};
        bool converted = false;
        const int value = object->property(property.c_str()).toInt(&converted);
        return converted ? std::optional<int>{value} : std::nullopt;
    }

    // Generic property read for structured automation facts (e.g. the viewport's
    // pixelReadoutResults map). An unreachable object or property yields an invalid QVariant,
    // which callers must surface as "unknown" rather than a defaulted value.
    [[nodiscard]] QVariant
    objectVariantPropertyForAutomation(const std::string_view objectName,
                                       const std::string_view propertyName) const {
        if (window_ == nullptr || objectName.empty() || propertyName.empty()) {
            return {};
        }
        const QObject* const object = window_->findChild<QObject*>(
            QString::fromUtf8(objectName.data(), static_cast<qsizetype>(objectName.size())));
        if (object == nullptr) {
            return {};
        }
        const std::string property{propertyName};
        return object->property(property.c_str());
    }

    // Walks the item's parent chain and records the properties that decide whether the item
    // can actually appear: per-ancestor visibility, opacity, geometry, z and clip, plus the
    // mapped scene position. Read-only diagnostics for the forensic entry points.
    [[nodiscard]] QVariantList
    itemAncestorReportForAutomation(const std::string_view objectName) const {
        QVariantList report;
        if (window_ == nullptr || objectName.empty()) {
            return report;
        }
        auto* const item = window_->findChild<QQuickItem*>(
            QString::fromUtf8(objectName.data(), static_cast<qsizetype>(objectName.size())));
        if (item == nullptr) {
            return report;
        }
        for (const QQuickItem* cursor = item; cursor != nullptr; cursor = cursor->parentItem()) {
            QVariantMap entry;
            entry.insert(QStringLiteral("objectName"), cursor->objectName());
            entry.insert(QStringLiteral("className"), cursor->metaObject()->className());
            entry.insert(QStringLiteral("visible"), cursor->isVisible());
            entry.insert(QStringLiteral("opacity"), cursor->opacity());
            entry.insert(QStringLiteral("width"), cursor->width());
            entry.insert(QStringLiteral("height"), cursor->height());
            entry.insert(QStringLiteral("x"), cursor->x());
            entry.insert(QStringLiteral("y"), cursor->y());
            entry.insert(QStringLiteral("z"), cursor->z());
            entry.insert(QStringLiteral("clip"), cursor->clip());
            const QPointF sceneTopLeft = cursor->mapToScene(QPointF{});
            entry.insert(QStringLiteral("sceneX"), sceneTopLeft.x());
            entry.insert(QStringLiteral("sceneY"), sceneTopLeft.y());
            report.append(entry);
        }
        return report;
    }

    [[nodiscard]] std::optional<QImage>
    captureControlForAutomation(const std::string_view objectName) const {
        if (window_ == nullptr || objectName.empty() || window_->width() <= 0 ||
            window_->height() <= 0) {
            return std::nullopt;
        }
        auto* const item = window_->findChild<QQuickItem*>(
            QString::fromUtf8(objectName.data(), static_cast<qsizetype>(objectName.size())));
        if (item == nullptr || !item->isVisible() || item->width() <= 0.0 ||
            item->height() <= 0.0) {
            return std::nullopt;
        }
        const QImage windowImage = window_->grabWindow();
        if (windowImage.isNull()) {
            return std::nullopt;
        }
        const QPointF topLeft = item->mapToScene(QPointF{});
        const qreal scaleX = static_cast<qreal>(windowImage.width()) / window_->width();
        const qreal scaleY = static_cast<qreal>(windowImage.height()) / window_->height();
        const QRect pixelBounds{
            static_cast<int>(std::floor(topLeft.x() * scaleX)),
            static_cast<int>(std::floor(topLeft.y() * scaleY)),
            static_cast<int>(std::ceil(item->width() * scaleX)),
            static_cast<int>(std::ceil(item->height() * scaleY)),
        };
        const QRect bounded = pixelBounds.intersected(windowImage.rect());
        if (bounded.isEmpty()) {
            return std::nullopt;
        }
        return windowImage.copy(bounded);
    }

    [[nodiscard]] bool openMenuForAutomation(const std::string_view objectName) noexcept {
        if (window_ == nullptr || objectName.empty()) {
            return false;
        }
        const QString target =
            QString::fromUtf8(objectName.data(), static_cast<qsizetype>(objectName.size()));
        if (QObject* const anchor = sourceMenuAnchorForAutomation(target); anchor != nullptr) {
            if (!anchor->property("visible").toBool() || anchor->property("pending").toBool() ||
                anchor->property("resolvedSourceIdentity").toString().isEmpty()) {
                return false;
            }
        }
        QObject* const menu = menuForAutomation(target);
        if (menu == nullptr || !menu->property("enabled").toBool()) {
            return false;
        }
        return QMetaObject::invokeMethod(menu, "open");
    }

    [[nodiscard]] bool closeMenuForAutomation(const std::string_view objectName) noexcept {
        if (window_ == nullptr || objectName.empty()) {
            return false;
        }
        QObject* const menu = menuForAutomation(
            QString::fromUtf8(objectName.data(), static_cast<qsizetype>(objectName.size())));
        if (menu == nullptr) {
            return false;
        }
        return QMetaObject::invokeMethod(menu, "close");
    }

    [[nodiscard]] bool menuIsOpenForAutomation(const std::string_view objectName) const noexcept {
        if (window_ == nullptr || objectName.empty()) {
            return false;
        }
        const QObject* const menu = menuForAutomation(
            QString::fromUtf8(objectName.data(), static_cast<qsizetype>(objectName.size())));
        if (menu == nullptr) {
            return false;
        }
        return menu->property("opened").toBool();
    }

    [[nodiscard]] std::optional<QImage>
    capturePopupWindowForAutomation(const std::string_view objectName) const {
        if (window_ == nullptr || objectName.empty()) {
            return std::nullopt;
        }
        const QString target =
            QString::fromUtf8(objectName.data(), static_cast<qsizetype>(objectName.size()));

        // Primary strategy: resolve the popup window through the menu's own contentItem.
        // QML Popup.Window menus host their visual tree in a contentItem that lives inside the
        // separate top-level QQuickWindow created for the popup.
        QObject* const menuObject = menuForAutomation(target);
        if (menuObject != nullptr) {
            const QVariant contentItemVariant = menuObject->property("contentItem");
            if (contentItemVariant.isValid()) {
                auto* const contentItem = contentItemVariant.value<QQuickItem*>();
                if (contentItem != nullptr) {
                    QQuickWindow* const popupWindow = contentItem->window();
                    if (popupWindow != nullptr && popupWindow != window_ &&
                        popupWindow->width() > 0 && popupWindow->height() > 0) {
                        QImage grabbed = popupWindow->grabWindow();
                        if (!grabbed.isNull()) {
                            return grabbed;
                        }
                    }
                }
            }
        }

        // Fallback: scan top-level windows for a child matching the target name.
        const QList<QWindow*> topLevelWindows = QGuiApplication::topLevelWindows();
        for (QWindow* topLevelWindow : topLevelWindows) {
            if (topLevelWindow == window_) {
                continue;
            }
            auto* quickWindow = qobject_cast<QQuickWindow*>(topLevelWindow);
            if (quickWindow == nullptr) {
                continue;
            }
            QObject* directChild = quickWindow->findChild<QObject*>(target);
            if (directChild == nullptr && quickWindow->contentItem() != nullptr) {
                directChild = quickWindow->contentItem()->findChild<QObject*>(target);
            }
            if (directChild == nullptr) {
                continue;
            }
            if (quickWindow->width() <= 0 || quickWindow->height() <= 0) {
                return std::nullopt;
            }
            QImage grabbed = quickWindow->grabWindow();
            if (grabbed.isNull()) {
                return std::nullopt;
            }
            return grabbed;
        }
        return std::nullopt;
    }

    void releaseSceneGraph() noexcept {
        if (!engine_) {
            return;
        }
        if (window_ != nullptr) {
            window_->hide();
            window_->releaseResources();
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        static_cast<void>(QObject::disconnect(surfaceDestroyedConnection_));
        static_cast<void>(QObject::disconnect(windowDestroyedConnection_));
        surfaceDestroyedConnection_ = {};
        windowDestroyedConnection_ = {};
        surface_ = nullptr;
        window_ = nullptr;
        engine_.reset();
        sessionFacade_.reset();
        shellController_.reset();
        // The engine owns the image providers; their token resolver points at the host, so
        // the host must outlive the engine.
        imageTasks_.reset();
        imageReview_.reset();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }

private:
    // Tree walk allocates Qt containers; callers are noexcept smoke-harness paths, and Qt
    // terminates on OOM by design.
    // NOLINTBEGIN(bugprone-exception-escape)
    [[nodiscard]] QQuickItem* visualItemForAutomation(const QString& target) const noexcept {
        // NOLINTEND(bugprone-exception-escape)
        if (window_ == nullptr || window_->contentItem() == nullptr || target.isEmpty()) {
            return nullptr;
        }
        std::vector<QQuickItem*> pending{window_->contentItem()};
        while (!pending.empty()) {
            QQuickItem* const item = pending.back();
            pending.pop_back();
            if (item->objectName() == target) {
                return item;
            }
            const QList<QQuickItem*> children = item->childItems();
            pending.insert(pending.end(), children.cbegin(), children.cend());
        }
        return nullptr;
    }

    [[nodiscard]] QObject* sourceMenuAnchorForAutomation(const QString& target) const noexcept {
        static const QString kSourceMenuPrefix = QStringLiteral("sourceMenu-");
        if (window_ == nullptr || !target.startsWith(kSourceMenuPrefix)) {
            return nullptr;
        }
        const QString anchorName =
            QStringLiteral("sourceChip-") + target.sliced(kSourceMenuPrefix.size());
        if (QObject* const anchor = window_->findChild<QObject*>(anchorName); anchor != nullptr) {
            return anchor;
        }
        return visualItemForAutomation(anchorName);
    }

    [[nodiscard]] QObject* menuForAutomation(const QString& target) const noexcept {
        if (window_ == nullptr || target.isEmpty()) {
            return nullptr;
        }
        if (QObject* const menu = window_->findChild<QObject*>(target); menu != nullptr) {
            return menu;
        }
        QObject* const anchor = sourceMenuAnchorForAutomation(target);
        if (anchor == nullptr) {
            return nullptr;
        }
        return anchor->property("sourceMenuControl").value<QObject*>();
    }

    static void reportWarnings(const std::vector<QQmlError>& warnings) {
        for (const QQmlError& error : warnings) {
            std::cerr << error.toString().toStdString() << '\n';
        }
    }

    DesktopApplicationOptions options_;
    bool nativeMenuWindowsDisabled_ = false;
    QGuiApplication application_;
    std::unique_ptr<QQmlApplicationEngine> engine_;
    std::unique_ptr<StartupSplash> splash_;
    std::unique_ptr<ReviewShellController> shellController_;
    std::unique_ptr<VideoFolderModel> videoFolder_;
    std::unique_ptr<ReviewSessionFacade> sessionFacade_;
    std::unique_ptr<ImageReviewController> imageReview_;
    std::unique_ptr<ImageTaskHost> imageTasks_;
    std::unique_ptr<ImageFolderPairModel> folderPairs_;
    std::unique_ptr<ImageFolderCursor> imageFolderCursor_;
    application::IIssueRecordRepository* issueRecordRepository_ = nullptr;
    std::unique_ptr<IssueLogController> issueLog_;
    application::IInterpolationProtocolRepository* interpolationProtocolRepository_ = nullptr;
    std::unique_ptr<InterpolationProtocolController> interpolationProtocol_;
    std::unique_ptr<ComparisonExportController> comparisonExport_;
    std::unique_ptr<ColorClipboardController> colorClipboard_;
    std::shared_ptr<application::IClipExporter> clipExporter_;
    std::unique_ptr<ClipExportController> clipExport_;
    std::shared_ptr<application::ISampleExporter> sampleExporter_;
    std::unique_ptr<SampleExportController> sampleExport_;
    std::shared_ptr<application::IFileAssociationService> fileAssociationService_;
    std::unique_ptr<FileAssociationController> fileAssociation_;
    std::unique_ptr<ImageEditController> imageEdit_;
    std::unique_ptr<DiagnosticsProbe> diagnosticsProbe_;
    std::unique_ptr<TimingDiagnosticsController> timingDiagnostics_;
    QQuickWindow* window_ = nullptr;
    ComparisonSurface* surface_ = nullptr;
    double activeScreenRefreshRate_ = 0.0;
    QMetaObject::Connection windowDestroyedConnection_;
    QMetaObject::Connection surfaceDestroyedConnection_;
    std::vector<QQmlError> qmlWarnings_;
};

DesktopApplication::DesktopApplication(int& argc, char** argv, DesktopApplicationOptions options)
    : impl_(std::make_unique<Impl>(argc, argv, options)) {}

DesktopApplication::~DesktopApplication() = default;

bool DesktopApplication::load(ReviewController& controller,
                              ReviewPreferencesController& preferences,
                              const SurfaceBinder& bindSurface,
                              PairMetricsController* pairMetrics,
                              PreviewThumbnailController* previewThumbnails) {
    return impl_->load(controller, preferences, bindSurface, pairMetrics, previewThumbnails);
}

void DesktopApplication::setIssueRecordRepository(
    application::IIssueRecordRepository* repository) noexcept {
    impl_->setIssueRecordRepository(repository);
}

void DesktopApplication::setInterpolationProtocolRepository(
    application::IInterpolationProtocolRepository* repository) noexcept {
    impl_->setInterpolationProtocolRepository(repository);
}

void DesktopApplication::setClipExporter(
    std::shared_ptr<application::IClipExporter> exporter) noexcept {
    impl_->setClipExporter(std::move(exporter));
}

void DesktopApplication::setSampleExporter(
    std::shared_ptr<application::ISampleExporter> exporter) noexcept {
    impl_->setSampleExporter(std::move(exporter));
}

void DesktopApplication::setFileAssociationService(
    std::shared_ptr<application::IFileAssociationService> service) noexcept {
    impl_->setFileAssociationService(std::move(service));
}

int DesktopApplication::exec() {
    return impl_->exec();
}

void DesktopApplication::exit(const int exitCode) noexcept {
    impl_->exit(exitCode);
}

double DesktopApplication::activeScreenRefreshRate() const noexcept {
    return impl_->activeScreenRefreshRate();
}

bool DesktopApplication::enqueueStartupRequest(const int kind, const QList<QUrl>& files) {
    return impl_->enqueueStartupRequest(kind, files);
}

void DesktopApplication::activateWindow() noexcept {
    impl_->activateWindow();
}

bool DesktopApplication::openSourcesForAutomation(const QList<QUrl>& sources) noexcept {
    return impl_->openSourcesForAutomation(sources);
}

bool DesktopApplication::openStillImageForAutomation(const QUrl& url) noexcept {
    return impl_->openStillImageForAutomation(url);
}

bool DesktopApplication::loadFolderComparisonForAutomation(const QUrl& left,
                                                           const QUrl& right) noexcept {
    return impl_->loadFolderComparisonForAutomation(left, right);
}

QObject* DesktopApplication::imageReviewForAutomation() const noexcept {
    return impl_->imageReviewForAutomation();
}

QObject* DesktopApplication::folderPairModelForAutomation() const noexcept {
    return impl_->folderPairModelForAutomation();
}

bool DesktopApplication::clickControlForAutomation(const std::string_view objectName) noexcept {
    return impl_->clickControlForAutomation(objectName);
}

bool DesktopApplication::focusControlForAutomation(const std::string_view objectName) noexcept {
    return impl_->focusControlForAutomation(objectName);
}

bool DesktopApplication::clickTimelineForAutomation(const double normalizedPosition) noexcept {
    return impl_->clickTimelineForAutomation(normalizedPosition);
}

bool DesktopApplication::sendKeyForAutomation(const int key, const int modifiers) noexcept {
    return impl_->sendKeyForAutomation(key, modifiers);
}

bool DesktopApplication::sendMouseMoveForAutomation(const double sceneX,
                                                    const double sceneY) noexcept {
    return impl_->sendMouseMoveForAutomation(sceneX, sceneY);
}

std::optional<QImage> DesktopApplication::captureWindowForAutomation() const {
    return impl_->captureWindowForAutomation();
}

std::optional<QPointF> DesktopApplication::objectScenePointForAutomation(
    const std::string_view objectName, const double fractionalX, const double fractionalY) const {
    return impl_->objectScenePointForAutomation(objectName, fractionalX, fractionalY);
}

std::optional<std::string>
DesktopApplication::objectStringMethodForAutomation(const std::string_view objectName,
                                                    const std::string_view methodName,
                                                    const int argument) const {
    return impl_->objectStringMethodForAutomation(objectName, methodName, argument);
}

std::optional<std::string>
DesktopApplication::objectStringPropertyForAutomation(const std::string_view objectName,
                                                      const std::string_view propertyName) const {
    return impl_->objectStringPropertyForAutomation(objectName, propertyName);
}

std::optional<int>
DesktopApplication::objectIntPropertyForAutomation(const std::string_view objectName,
                                                   const std::string_view propertyName) const {
    return impl_->objectIntPropertyForAutomation(objectName, propertyName);
}

QVariant
DesktopApplication::objectVariantPropertyForAutomation(const std::string_view objectName,
                                                       const std::string_view propertyName) const {
    return impl_->objectVariantPropertyForAutomation(objectName, propertyName);
}

QVariantList
DesktopApplication::itemAncestorReportForAutomation(const std::string_view objectName) const {
    return impl_->itemAncestorReportForAutomation(objectName);
}

std::optional<QImage>
DesktopApplication::captureControlForAutomation(const std::string_view objectName) const {
    return impl_->captureControlForAutomation(objectName);
}

bool DesktopApplication::openMenuForAutomation(const std::string_view objectName) noexcept {
    return impl_->openMenuForAutomation(objectName);
}

bool DesktopApplication::closeMenuForAutomation(const std::string_view objectName) noexcept {
    return impl_->closeMenuForAutomation(objectName);
}

bool DesktopApplication::menuIsOpenForAutomation(const std::string_view objectName) const noexcept {
    return impl_->menuIsOpenForAutomation(objectName);
}

std::optional<QImage>
DesktopApplication::capturePopupWindowForAutomation(const std::string_view objectName) const {
    return impl_->capturePopupWindowForAutomation(objectName);
}

void DesktopApplication::releaseSceneGraph() noexcept {
    impl_->releaseSceneGraph();
}

} // namespace dvs::ui
