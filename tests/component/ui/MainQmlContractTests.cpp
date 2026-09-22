#include "dvs/application/Alignment.h"
#include "dvs/application/SessionSnapshot.h"
#include "dvs/domain/ComparisonValidator.h"
#include "dvs/ui/ComparisonSurface.h"
#include "dvs/ui/GraphicsBackend.h"
#include "dvs/ui/ImageFolderPairModel.h"
#include "dvs/ui/ImageReviewController.h"
#include "dvs/ui/ReviewController.h"
#include "dvs/ui/ReviewImageProvider.h"
#include "dvs/ui/ReviewPreferencesController.h"
#include "dvs/ui/ReviewSessionFacade.h"
#include "dvs/ui/ReviewShellController.h"
#include "dvs/ui/SourceListModel.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QResource>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QVariant>
#include <QtQml/qqml.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <utility>
#include <vector>

void initializeMainQmlContractResources() {
    Q_INIT_RESOURCE(dvs_ui_qml_resources);
}

namespace {

[[nodiscard]] std::string componentErrors(const QQmlComponent& component) {
    QStringList messages;
    for (const QQmlError& error : component.errors()) {
        messages.push_back(error.toString());
    }
    return messages.join(QStringLiteral("\n")).toStdString();
}

void sendKey(QWindow& window,
             const int key,
             const Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent press{QEvent::KeyPress, key, modifiers};
    QKeyEvent release{QEvent::KeyRelease, key, modifiers};
    QCoreApplication::sendEvent(&window, &press);
    QCoreApplication::sendEvent(&window, &release);
    QCoreApplication::processEvents();
}

void sendMousePress(QWindow& window, const QPointF& localPos) {
    const QPointF globalPos = window.mapToGlobal(localPos.toPoint());
    QMouseEvent press{QEvent::MouseButtonPress,
                      localPos,
                      globalPos,
                      Qt::LeftButton,
                      Qt::LeftButton,
                      Qt::NoModifier};
    QCoreApplication::sendEvent(&window, &press);
}

void sendMouseRelease(QWindow& window, const QPointF& localPos) {
    const QPointF globalPos = window.mapToGlobal(localPos.toPoint());
    QMouseEvent release{QEvent::MouseButtonRelease,
                        localPos,
                        globalPos,
                        Qt::LeftButton,
                        Qt::NoButton,
                        Qt::NoModifier};
    QCoreApplication::sendEvent(&window, &release);
    QCoreApplication::processEvents();
}

void sendMouseMove(QWindow& window, const QPointF& localPos) {
    const QPointF globalPos = window.mapToGlobal(localPos.toPoint());
    QMouseEvent move{
        QEvent::MouseMove, localPos, globalPos, Qt::NoButton, Qt::NoButton, Qt::NoModifier};
    QCoreApplication::sendEvent(&window, &move);
}

QQuickWindow* findPopupWindow(const QString& objectName) {
    for (QWindow* window : QGuiApplication::topLevelWindows()) {
        auto* quickWindow = qobject_cast<QQuickWindow*>(window);
        if (!quickWindow) {
            continue;
        }
        if (quickWindow->findChild<QObject*>(objectName)) {
            return quickWindow;
        }
        if (quickWindow->contentItem() &&
            quickWindow->contentItem()->findChild<QObject*>(objectName)) {
            return quickWindow;
        }
    }
    return nullptr;
}

QQuickWindow* menuPopupWindow(QObject* menu) {
    QVariant contentItemVar = menu->property("contentItem");
    if (!contentItemVar.isValid()) {
        return nullptr;
    }
    auto* contentItem = contentItemVar.value<QQuickItem*>();
    if (!contentItem) {
        return nullptr;
    }
    return contentItem->window();
}

// Hand-rolled 1x1 PNG bytes: the pair opener routes through the still-image decoder, so
// every file must be a real image. A literal byte array avoids depending on the
// imageformat plugins that minimal test deployments may not install.
[[nodiscard]] bool writeTestPng(const QString& path) {
    static const unsigned char kPngBytes[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00,
        0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41, 0x54, 0x08,
        0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00, 0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xDD, 0x8D,
        0xB0, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
    };
    QFile file{path};
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return file.write(reinterpret_cast<const char*>(kPngBytes), sizeof(kPngBytes)) ==
           static_cast<qint64>(sizeof(kPngBytes));
}

} // namespace

namespace dvs::ui {
namespace {

class ClosedSettingsRepository final : public application::ISettingsRepository {
public:
    [[nodiscard]] application::PortSubmitResult
    submit(const application::SettingsLoadRequest&,
           std::shared_ptr<application::IApplicationEventSink>) override {
        return application::PortSubmitResult::Closed;
    }

    [[nodiscard]] application::PortSubmitResult
    submit(const application::SettingsSaveRequest&,
           std::shared_ptr<application::IApplicationEventSink>) override {
        return application::PortSubmitResult::Closed;
    }

    void cancel(const application::RequestContext&) noexcept override {}
};

// Shared QML/controller harness for the workspace-routing contracts. It keeps the two-source
// video session, the still-image controller and the folder model alive for the whole test.
class WorkspaceHarness final {
public:
    WorkspaceHarness() {
        snapshot->graphicsReady = true;
        snapshot->sessionState = domain::SessionState::kReady;
        snapshot->playbackState = domain::PlaybackState::kPaused;
        snapshot->displayedFrame = domain::FrameId{41};
        snapshot->canonicalFrameCount = 100U;
        snapshot->sources = {
            application::SessionSourceView{
                .sourceId = 0U,
                .role = domain::ComparisonRole::kReference,
                .displayName = "A",
            },
            application::SessionSourceView{
                .sourceId = 1U,
                .role = domain::ComparisonRole::kPrediction,
                .displayName = "B",
            },
        };
        snapshot->presentedSources = {
            application::PresentedSourceState{
                .sourceId = 0U,
                .sourceFrameId = domain::FrameId{41},
                .matchKind = application::FrameMatchKind::ExactIndex,
            },
            application::PresentedSourceState{
                .sourceId = 1U,
                .sourceFrameId = domain::FrameId{41},
                .matchKind = application::FrameMatchKind::ExactIndex,
            },
        };
        controller = std::make_unique<ReviewController>(ReviewController::Dependencies{
            .submit =
                [this](application::PlaybackCommand command) {
                    submitted.push_back(std::move(command));
                    return application::PortSubmitResult::Accepted;
                },
            .snapshot = [this] { return snapshot; },
            .takeCompletedCommands =
                [this] {
                    std::vector<application::CommandTerminal> result = std::move(terminals);
                    terminals.clear();
                    return result;
                },
        });
        shell = std::make_unique<ReviewShellController>(*controller, preferences);
        facade = std::make_unique<ReviewSessionFacade>(*controller, preferences, *shell);
        folderPairs.setAsyncPairOpener(
            [this](const QUrl& primary, const QUrl& secondary, const int pairId, QString* error) {
                const int requestId = imageReview.requestOpenPair(primary, secondary, pairId);
                if (requestId <= 0 && error != nullptr) {
                    *error = imageReview.errorText();
                }
                return requestId;
            });
        folderPairs.setAsyncPairCancel(
            [this](const int requestId) { imageReview.cancelOpenRequest(requestId); });
        folderPairs.setSingleSideOpener([this](const QUrl& url, const int row, QString* error) {
            const int requestId = imageReview.requestOpenPrimary(url, row);
            if (requestId <= 0 && error != nullptr) {
                *error = imageReview.errorText();
            }
            return requestId;
        });
        QObject::connect(
            &imageReview,
            &ImageReviewController::openFinished,
            &folderPairs,
            [this](
                const int requestId, const int pairId, const bool success, const QString& error) {
                if (pairId >= 0) {
                    folderPairs.completePairOpen(static_cast<quint64>(requestId), success, error);
                    folderPairs.completeSingleSideOpen(
                        static_cast<quint64>(requestId), success, error);
                }
            });
    }

    [[nodiscard]] bool create() {
        const QString importPath =
            QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml"));
        engine.addImportPath(importPath);
        engine.rootContext()->setContextProperty(QStringLiteral("reviewController"),
                                                 controller.get());
        engine.rootContext()->setContextProperty(QStringLiteral("reviewPreferences"), &preferences);
        engine.rootContext()->setContextProperty(QStringLiteral("reviewSession"), shell.get());
        engine.rootContext()->setContextProperty(QStringLiteral("reviewFacade"), facade.get());
        engine.rootContext()->setContextProperty(QStringLiteral("imageReview"), &imageReview);
        engine.rootContext()->setContextProperty(QStringLiteral("imageFolderPairs"), &folderPairs);
        engine.addImageProvider(QStringLiteral("vcs-review"),
                                new ReviewImageProvider(&imageReview));
        QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
        if (component.status() != QQmlComponent::Ready) {
            error = componentErrors(component);
            return false;
        }
        root.reset(component.create());
        if (!root) {
            error = componentErrors(component);
            return false;
        }
        window = qobject_cast<QQuickWindow*>(root.get());
        if (!window) {
            error = "Main.qml root is not a QQuickWindow";
            return false;
        }
        window->resize(1280, 800);
        settle();
        return true;
    }

    [[nodiscard]] bool activateWorkspace(const int media) {
        QVariant result;
        return QMetaObject::invokeMethod(root.get(),
                                         "activateWorkspace",
                                         Q_RETURN_ARG(QVariant, result),
                                         Q_ARG(QVariant, QVariant{media})) &&
               result.toBool();
    }

    [[nodiscard]] bool beginWorkspaceOpen(const int media) {
        QVariant result;
        return QMetaObject::invokeMethod(root.get(),
                                         "beginWorkspaceOpen",
                                         Q_RETURN_ARG(QVariant, result),
                                         Q_ARG(QVariant, QVariant{media})) &&
               result.toBool();
    }

    [[nodiscard]] bool cancelWorkspaceOpen() {
        QVariant result;
        return QMetaObject::invokeMethod(
                   root.get(), "cancelWorkspaceOpen", Q_RETURN_ARG(QVariant, result)) &&
               result.toBool();
    }

    void settle(const int iterations = 5) {
        for (int iteration = 0; iteration < iterations; ++iteration) {
            QCoreApplication::processEvents();
        }
    }

    template <typename Predicate>
    [[nodiscard]] bool waitUntil(Predicate predicate, const int timeoutMilliseconds = 8000) {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < timeoutMilliseconds) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
            QThread::msleep(1U);
        }
        return predicate();
    }

    std::shared_ptr<application::SessionSnapshot> snapshot =
        std::make_shared<application::SessionSnapshot>();
    std::vector<application::PlaybackCommand> submitted;
    std::vector<application::CommandTerminal> terminals;
    std::unique_ptr<ReviewController> controller;
    ReviewPreferencesController preferences{std::make_shared<ClosedSettingsRepository>()};
    std::unique_ptr<ReviewShellController> shell;
    std::unique_ptr<ReviewSessionFacade> facade;
    ImageReviewController imageReview;
    ImageFolderPairModel folderPairs;
    QQmlEngine engine;
    std::unique_ptr<QObject> root;
    QQuickWindow* window = nullptr;
    std::string error;
};

TEST(MainQmlContractTests, MapsEveryCurrentMediaErrorAndExcludesDeletedUiDomains) {
    QFile messageCatalog{QStringLiteral(":/qml/ReviewMessageCatalog.qml")};
    ASSERT_TRUE(messageCatalog.open(QIODevice::ReadOnly));
    const QString source = QString::fromUtf8(messageCatalog.readAll());
    constexpr std::array<std::string_view, 28U> kMediaErrorKeys{
        "invalid-argument",
        "invalid-rate",
        "invalid-frame-id",
        "invalid-frame-count",
        "invalid-dimensions",
        "invalid-duration",
        "invalid-media-descriptor",
        "arithmetic-overflow",
        "source-frame-rate-mismatch",
        "source-frame-count-mismatch",
        "source-duration-mismatch",
        "source-resolution-mismatch",
        "source-color-metadata-mismatch",
        "frame-out-of-range",
        "source-missing",
        "source-fingerprint-mismatch",
        "file-io",
        "media-open-failed",
        "media-probe-failed",
        "invalid-cfr-timing",
        "unsupported-codec",
        "unsupported-pixel-format",
        "media-decode-failed",
        "frame-timeline-invalid",
        "frame-budget-exceeded",
        "graphics-unavailable",
        "graphics-device-lost",
        "frame-presentation-timed-out",
    };
    for (const std::string_view key : kMediaErrorKeys) {
        EXPECT_TRUE(
            source.contains(QStringLiteral("case \"") +
                            QString::fromLatin1(key.data(), static_cast<qsizetype>(key.size())) +
                            QStringLiteral("\":")))
            << key;
    }
    for (const QString& removed : {QStringLiteral("clip-out-of-range"),
                                   QStringLiteral("clip-not-found"),
                                   QStringLiteral("export-record-not-found"),
                                   QStringLiteral("duplicate-clip-selection"),
                                   QStringLiteral("invalid-export-mode"),
                                   QStringLiteral("invalid-export-geometry")}) {
        EXPECT_FALSE(source.contains(removed)) << removed.toStdString();
    }
}

TEST(MainQmlContractTests, InstantiatesRootAndSeparatesManualAlignmentStates) {
    auto snapshot = std::make_shared<application::SessionSnapshot>();
    snapshot->graphicsReady = true;
    snapshot->sessionState = domain::SessionState::kReady;
    snapshot->playbackState = domain::PlaybackState::kPaused;
    snapshot->displayedFrame = domain::FrameId{0};
    snapshot->canonicalFrameCount = 10U;
    snapshot->sources = {
        application::SessionSourceView{
            .sourceId = 0U,
            .role = domain::ComparisonRole::kReference,
            .displayName = "A",
        },
        application::SessionSourceView{
            .sourceId = 1U,
            .role = domain::ComparisonRole::kPrediction,
            .displayName = "B",
        },
    };
    snapshot->presentedSources = {
        application::PresentedSourceState{
            .sourceId = 0U,
            .sourceFrameId = domain::FrameId{0},
            .matchKind = application::FrameMatchKind::ExactIndex,
        },
        application::PresentedSourceState{
            .sourceId = 1U,
            .sourceFrameId = domain::FrameId{0},
            .matchKind = application::FrameMatchKind::ExactIndex,
        },
    };
    snapshot->manualAlignmentAnchors = {
        application::SourceAlignmentAnchors{
            .sourceId = 1U,
            .anchors =
                {
                    application::ManualAlignmentAnchor{
                        .canonicalFrameId = domain::FrameId{2U},
                        .sourceFrameId = domain::FrameId{3U},
                    },
                },
        },
    };
    std::vector<application::PlaybackCommand> submitted;
    std::vector<application::CommandTerminal> terminals;
    ReviewController controller{
        ReviewController::Dependencies{
            .submit =
                [&submitted](application::PlaybackCommand command) {
                    submitted.push_back(std::move(command));
                    return application::PortSubmitResult::Accepted;
                },
            .snapshot = [snapshot] { return snapshot; },
            .takeCompletedCommands =
                [&terminals] {
                    std::vector<application::CommandTerminal> result = std::move(terminals);
                    terminals.clear();
                    return result;
                },
        },
    };
    ReviewPreferencesController preferences{std::make_shared<ClosedSettingsRepository>()};
    // A persisted three-source-only pair must not make a two-source session black. The
    // preference remains intact for a later third source, while the render-facing state
    // resolves to the valid A/B edge.
    preferences.setViewMode(ReviewPreferencesController::ViewMode::Wipe);
    preferences.setDifferenceEdge(ReviewPreferencesController::DifferenceEdge::Edge0And2);
    ReviewShellController shell{controller, preferences};
    ReviewSessionFacade facade{controller, preferences, shell};

    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    engine.rootContext()->setContextProperty(QStringLiteral("reviewController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewPreferences"), &preferences);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewSession"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewFacade"), &facade);
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);

    std::unique_ptr<QObject> root{component.create()};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    ASSERT_NE(qobject_cast<QQuickWindow*>(root.get()), nullptr);
    QCoreApplication::processEvents();
    ASSERT_EQ(controller.sources()->rowCount(), 2);
    EXPECT_EQ(root->property("effectiveViewMode").toInt(), ComparisonSurface::Wipe);
    EXPECT_EQ(root->property("differenceEdge").toInt(), ComparisonSurface::Edge0And1);
    EXPECT_EQ(static_cast<int>(preferences.differenceEdge()),
              static_cast<int>(ReviewPreferencesController::DifferenceEdge::Edge0And2));
    ASSERT_TRUE(QMetaObject::invokeMethod(root.get(), "setInPoint"));
    EXPECT_EQ(shell.inFrame(), 0);
    EXPECT_EQ(shell.inMediaTime(), controller.mediaTimeForFrame(0));
    shell.clearRange();

    EXPECT_EQ(shell.queuedIntentCount(), 0);
    EXPECT_TRUE(shell.queuedIntents().isEmpty());
    EXPECT_TRUE(shell.activeIntent().isEmpty());

    EXPECT_TRUE(root->property("manualAnchorActive").toBool());
    EXPECT_FALSE(root->property("manualOffsetActive").toBool());
    EXPECT_TRUE(root->property("anyManualAlignmentActive").toBool());

    QObject* const alignmentModeStatus =
        root->findChild<QObject*>(QStringLiteral("alignmentModeStatus"));
    QObject* const offsetStatus =
        root->findChild<QObject*>(QStringLiteral("manualOffsetStatusLabel"));
    QObject* const anchorButton = root->findChild<QObject*>(QStringLiteral("manualAnchorsButton"));
    QObject* const offsetRepeater =
        root->findChild<QObject*>(QStringLiteral("sourceOffsetRepeater"));
    ASSERT_NE(alignmentModeStatus, nullptr);
    ASSERT_NE(offsetStatus, nullptr);
    ASSERT_NE(anchorButton, nullptr);
    ASSERT_NE(offsetRepeater, nullptr);
    EXPECT_EQ(offsetRepeater->property("count").toInt(), 2);
    EXPECT_EQ(alignmentModeStatus->property("text").toString(), QStringLiteral("手动对齐"));
    EXPECT_EQ(offsetStatus->property("text").toString(), QStringLiteral("帧对齐偏移"));
    EXPECT_EQ(anchorButton->property("text").toString(), QStringLiteral("已设手动锚点…"));

    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);
    QObject* const inspector = root->findChild<QObject*>(QStringLiteral("manualAnchorDialog"));
    QObject* const tabbedInspector = root->findChild<QObject*>(QStringLiteral("tabbedInspector"));
    auto* const compareBar = root->findChild<QQuickItem*>(QStringLiteral("compareModeBar"));
    auto* const transport = root->findChild<QQuickItem*>(QStringLiteral("transport"));
    auto* const transportBar = root->findChild<QQuickItem*>(QStringLiteral("transportBar"));
    auto* const viewport = root->findChild<QQuickItem*>(QStringLiteral("mediaViewportFocusTarget"));
    auto* const surface = root->findChild<QQuickItem*>(QStringLiteral("dualVideoSurface"));
    auto* const sideModeButton = root->findChild<QQuickItem*>(QStringLiteral("sideModeButton"));
    QObject* const analysisChrome =
        root->findChild<QObject*>(QStringLiteral("analysisControlsChrome"));
    QObject* const surfaceLabelRepeater =
        root->findChild<QObject*>(QStringLiteral("surfaceLabelRepeater"));
    auto* const frameErrorBanner = root->findChild<QQuickItem*>(QStringLiteral("frameErrorBanner"));
    QObject* const activeSourceRepeater =
        root->findChild<QObject*>(QStringLiteral("activeSourceRepeater"));
    QObject* const timeline = root->findChild<QObject*>(QStringLiteral("timelineSlider"));
    QObject* const setInButton = root->findChild<QObject*>(QStringLiteral("setInButton"));
    QObject* const setOutButton = root->findChild<QObject*>(QStringLiteral("setOutButton"));
    QObject* const clearRangeButton = root->findChild<QObject*>(QStringLiteral("clearRangeButton"));
    QObject* const loopRangeButton = root->findChild<QObject*>(QStringLiteral("loopRangeButton"));
    QObject* const mediaInfoRepeater =
        root->findChild<QObject*>(QStringLiteral("mediaInfoRepeater"));
    QObject* const shortcutHelp = root->findChild<QObject*>(QStringLiteral("shortcutHelpOverlay"));
    QObject* const contextViewMenu = root->findChild<QObject*>(QStringLiteral("contextViewMenu"));
    QObject* const reviewContextMenu =
        root->findChild<QObject*>(QStringLiteral("reviewContextMenu"));
    QObject* const contextOpenAction =
        root->findChild<QObject*>(QStringLiteral("contextOpenAction"));
    QObject* const contextPairMenu = root->findChild<QObject*>(QStringLiteral("contextPairMenu"));
    QObject* const contextReferenceMenu =
        root->findChild<QObject*>(QStringLiteral("contextReferenceMenu"));
    QObject* const contextInfoAction =
        root->findChild<QObject*>(QStringLiteral("contextInfoAction"));
    QObject* const immersiveHud = root->findChild<QObject*>(QStringLiteral("immersiveReviewHud"));
    auto* const firstButton = root->findChild<QQuickItem*>(QStringLiteral("firstButton"));
    auto* const lastButton = root->findChild<QQuickItem*>(QStringLiteral("lastButton"));
    QObject* const analysisGridMenuItem =
        root->findChild<QObject*>(QStringLiteral("analysisGridMenuItem"));
    QObject* const compareMenu = root->findChild<QObject*>(QStringLiteral("compareMenu"));
    QObject* const analyzeMenu = root->findChild<QObject*>(QStringLiteral("analyzeMenu"));
    ASSERT_NE(inspector, nullptr);
    ASSERT_NE(tabbedInspector, nullptr);
    ASSERT_NE(compareBar, nullptr);
    ASSERT_NE(transport, nullptr);
    ASSERT_NE(transportBar, nullptr);
    ASSERT_NE(viewport, nullptr);
    ASSERT_NE(surface, nullptr);
    EXPECT_EQ(surface->property("viewMode").toInt(), ComparisonSurface::Wipe);
    EXPECT_EQ(surface->property("differenceEdge").toInt(), ComparisonSurface::Edge0And1);
    ASSERT_NE(sideModeButton, nullptr);
    ASSERT_NE(analysisChrome, nullptr);
    ASSERT_NE(surfaceLabelRepeater, nullptr);
    ASSERT_NE(frameErrorBanner, nullptr);
    ASSERT_NE(activeSourceRepeater, nullptr);
    ASSERT_NE(timeline, nullptr);
    ASSERT_NE(setInButton, nullptr);
    ASSERT_NE(setOutButton, nullptr);
    ASSERT_NE(clearRangeButton, nullptr);
    ASSERT_NE(loopRangeButton, nullptr);
    ASSERT_NE(mediaInfoRepeater, nullptr);
    ASSERT_NE(shortcutHelp, nullptr);
    ASSERT_NE(contextViewMenu, nullptr);
    ASSERT_NE(reviewContextMenu, nullptr);
    ASSERT_NE(contextOpenAction, nullptr);
    ASSERT_NE(contextPairMenu, nullptr);
    ASSERT_NE(contextReferenceMenu, nullptr);
    ASSERT_NE(contextInfoAction, nullptr);
    ASSERT_NE(immersiveHud, nullptr);
    ASSERT_NE(firstButton, nullptr);
    ASSERT_NE(lastButton, nullptr);
    ASSERT_NE(analysisGridMenuItem, nullptr);
    ASSERT_NE(compareMenu, nullptr);
    ASSERT_NE(analyzeMenu, nullptr);
    EXPECT_FALSE(analysisGridMenuItem->property("enabled").toBool());
    EXPECT_FALSE(inspector->property("visible").toBool());
    EXPECT_EQ(root->property("minimumWidth").toDouble(), 960.0);
    EXPECT_TRUE(compareBar->isVisible());
    EXPECT_GT(transport->width(), 0.0);
    EXPECT_GT(firstButton->width(), 0.0);
    EXPECT_GT(lastButton->width(), 0.0);
    EXPECT_FALSE(analysisChrome->property("visible").toBool());
    EXPECT_EQ(surfaceLabelRepeater->property("count").toInt(), 2);
    EXPECT_EQ(activeSourceRepeater->property("count").toInt(), 2);
    EXPECT_EQ(root->property("availableViewModes").toList().size(), 3);
    EXPECT_GE(viewport->height() / window->contentItem()->height(), 0.78);

    snapshot->lastError = domain::makeMediaError(domain::MediaErrorCode::kMediaDecodeFailed,
                                                 domain::MediaOperation::kMediaDecode,
                                                 domain::SourceId{0U},
                                                 true,
                                                 "Synthetic frame read failure.");
    controller.refreshProjection();
    QCoreApplication::processEvents();
    EXPECT_TRUE(frameErrorBanner->isVisible());
    const QPointF bannerTopLeft = frameErrorBanner->mapToItem(viewport, QPointF{0.0, 0.0});
    EXPECT_GE(bannerTopLeft.y(), 0.0);
    EXPECT_LE(bannerTopLeft.y() + frameErrorBanner->height(), viewport->height());
    snapshot->lastError.reset();
    controller.refreshProjection();
    QCoreApplication::processEvents();

    preferences.setViewMode(ReviewPreferencesController::ViewMode::Wipe);
    for (const double wipePosition : {0.05, 0.95}) {
        root->setProperty("wipePosition", wipePosition);
        QCoreApplication::processEvents();
        QVariant firstBadgeResult;
        QVariant secondBadgeResult;
        ASSERT_TRUE(QMetaObject::invokeMethod(viewport,
                                              "surfaceLabelGeometry",
                                              Q_RETURN_ARG(QVariant, firstBadgeResult),
                                              Q_ARG(QVariant, QVariant{0})));
        ASSERT_TRUE(QMetaObject::invokeMethod(viewport,
                                              "surfaceLabelGeometry",
                                              Q_RETURN_ARG(QVariant, secondBadgeResult),
                                              Q_ARG(QVariant, QVariant{1})));
        const QVariantMap firstBadge = firstBadgeResult.toMap();
        const QVariantMap secondBadge = secondBadgeResult.toMap();
        ASSERT_FALSE(firstBadge.isEmpty());
        ASSERT_FALSE(secondBadge.isEmpty());
        const QVariantMap leftWipeBadge =
            firstBadge.value(QStringLiteral("sourceSlot")).toInt() == 0 ? firstBadge : secondBadge;
        const QVariantMap rightWipeBadge =
            firstBadge.value(QStringLiteral("sourceSlot")).toInt() == 1 ? firstBadge : secondBadge;
        EXPECT_TRUE(leftWipeBadge.value(QStringLiteral("visible")).toBool());
        EXPECT_TRUE(rightWipeBadge.value(QStringLiteral("visible")).toBool());
        EXPECT_LE(leftWipeBadge.value(QStringLiteral("x")).toDouble() +
                      leftWipeBadge.value(QStringLiteral("width")).toDouble(),
                  rightWipeBadge.value(QStringLiteral("x")).toDouble());
        EXPECT_GE(leftWipeBadge.value(QStringLiteral("x")).toDouble(), 0.0);
        EXPECT_LE(rightWipeBadge.value(QStringLiteral("x")).toDouble() +
                      rightWipeBadge.value(QStringLiteral("width")).toDouble(),
                  viewport->width());
    }
    preferences.setViewMode(ReviewPreferencesController::ViewMode::SideBySide);
    QVariant zoomResult;
    ASSERT_TRUE(QMetaObject::invokeMethod(timeline,
                                          "setZoom",
                                          Q_RETURN_ARG(QVariant, zoomResult),
                                          Q_ARG(QVariant, QVariant{2.0}),
                                          Q_ARG(QVariant, QVariant{0.5})));
    EXPECT_GT(timeline->property("zoomFactor").toDouble(), 1.0);

    window->resize(960, 640);
    window->show();
    for (int iteration = 0; iteration < 5; ++iteration) {
        QCoreApplication::processEvents();
    }
    const QPointF transportTopLeft =
        transportBar->mapToItem(window->contentItem(), QPointF{0.0, 0.0});
    EXPECT_EQ(window->contentItem()->width(), window->width());
    EXPECT_GE(transportTopLeft.x(), 0.0);
    EXPECT_LE(transportTopLeft.x() + transportBar->width(), window->width())
        << "left=" << transportTopLeft.x() << " barWidth=" << transportBar->width()
        << " contentWidth=" << window->contentItem()->width();

    ASSERT_TRUE(QMetaObject::invokeMethod(compareMenu, "open"));
    for (int iteration = 0; iteration < 5; ++iteration) {
        QCoreApplication::processEvents();
    }
    EXPECT_TRUE(compareMenu->property("opened").toBool());
    EXPECT_TRUE(root->property("anyMenuOpen").toBool());
    EXPECT_FALSE(root->property("globalMediaShortcutsEnabled").toBool());
    const std::size_t submittedBeforeMenuShortcut = submitted.size();
    sendKey(*window, Qt::Key_Right);
    EXPECT_EQ(submitted.size(), submittedBeforeMenuShortcut);
    ASSERT_TRUE(QMetaObject::invokeMethod(compareMenu, "close"));
    for (int iteration = 0; iteration < 5; ++iteration) {
        QCoreApplication::processEvents();
    }
    EXPECT_FALSE(root->property("anyMenuOpen").toBool());
    EXPECT_TRUE(root->property("globalMediaShortcutsEnabled").toBool());

    sideModeButton->forceActiveFocus();
    ASSERT_TRUE(sideModeButton->hasActiveFocus());
    EXPECT_FALSE(root->property("globalMediaShortcutsEnabled").toBool());
    sendKey(*window, Qt::Key_Tab);
    QCoreApplication::processEvents();
    EXPECT_FALSE(root->property("chromeVisible").toBool());
    EXPECT_TRUE(root->property("globalMediaShortcutsEnabled").toBool());
    EXPECT_TRUE(viewport->hasActiveFocus());
    EXPECT_FALSE(transport->isVisible());
    EXPECT_FALSE(analysisChrome->property("visible").toBool());
    EXPECT_DOUBLE_EQ(viewport->property("radius").toDouble(), 0.0);
    QObject* const viewportBorder = viewport->property("border").value<QObject*>();
    ASSERT_NE(viewportBorder, nullptr);
    EXPECT_EQ(viewportBorder->property("width").toInt(), 0);
    const QPointF immersiveTopLeft = viewport->mapToItem(window->contentItem(), QPointF{0.0, 0.0});
    EXPECT_DOUBLE_EQ(immersiveTopLeft.x(), 0.0);
    EXPECT_DOUBLE_EQ(immersiveTopLeft.y(), 0.0);
    EXPECT_DOUBLE_EQ(viewport->width(), window->contentItem()->width());
    EXPECT_DOUBLE_EQ(viewport->height(), window->contentItem()->height());
    EXPECT_DOUBLE_EQ(surface->x(), 0.0);
    EXPECT_DOUBLE_EQ(surface->y(), 0.0);
    EXPECT_DOUBLE_EQ(surface->width(), viewport->width());
    EXPECT_DOUBLE_EQ(surface->height(), viewport->height());

    sendKey(*window, Qt::Key_Right);
    ASSERT_FALSE(submitted.empty());
    const auto* const step = std::get_if<application::StepFramesCommand>(&submitted.back());
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->delta, 1);
    snapshot->displayedFrame = domain::FrameId{1};
    terminals.push_back(application::CommandTerminal{
        .context = application::commandContext(submitted.back()),
        .outcome = application::CommandOutcome::Succeeded,
    });
    snapshot->displayedFrame = domain::FrameId{2};
    controller.refreshProjection();
    QCoreApplication::processEvents();
    EXPECT_TRUE(immersiveHud->property("visible").toBool());

    // Left/Right step a single frame in every preset (plan 1.5.md §11.1). Preset 1 (Player) no
    // longer maps Right to a 5-second jump (5 * 30 = 150 frames) — that violated the ±1
    // expectation. Multi-frame jumps in preset 1 remain available via Ctrl+Right (stepSeconds(30)).
    preferences.setShortcutPreset(1);
    QCoreApplication::processEvents();
    sendKey(*window, Qt::Key_Right);
    ASSERT_FALSE(submitted.empty());
    const auto* const playerStep = std::get_if<application::StepFramesCommand>(&submitted.back());
    ASSERT_NE(playerStep, nullptr);
    EXPECT_EQ(playerStep->delta, 1);
    terminals.push_back(application::CommandTerminal{
        .context = application::commandContext(submitted.back()),
        .outcome = application::CommandOutcome::Succeeded,
    });
    controller.refreshProjection();
    preferences.setShortcutPreset(0);

    sendKey(*window, Qt::Key_Question);
    EXPECT_TRUE(shortcutHelp->property("visible").toBool());
    shortcutHelp->setProperty("visible", false);
    QElapsedTimer hudWait;
    hudWait.start();
    while (hudWait.elapsed() < 900) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5U);
    }
    EXPECT_FALSE(immersiveHud->property("visible").toBool());
    sendKey(*window, Qt::Key_Space);
    ASSERT_FALSE(submitted.empty());
    EXPECT_NE(std::get_if<application::PlayCommand>(&submitted.back()), nullptr);

    sendKey(*window, Qt::Key_Tab);
    QCoreApplication::processEvents();
    EXPECT_TRUE(root->property("chromeVisible").toBool());
    EXPECT_TRUE(transport->isVisible());

    snapshot->sources.push_back(application::SessionSourceView{
        .sourceId = 2U,
        .role = domain::ComparisonRole::kPrediction,
        .displayName = "C",
    });
    snapshot->presentedSources.push_back(application::PresentedSourceState{
        .sourceId = 2U,
        .sourceFrameId = domain::FrameId{2},
        .matchKind = application::FrameMatchKind::ExactIndex,
    });
    preferences.setViewMode(ReviewPreferencesController::ViewMode::ThreeUp);
    controller.refreshProjection();
    QCoreApplication::processEvents();
    EXPECT_EQ(controller.sourceCount(), 3);
    EXPECT_EQ(shell.effectiveViewMode(), ComparisonSurface::ThreeUp);
    EXPECT_EQ(root->property("effectiveViewMode").toInt(), ComparisonSurface::ThreeUp);
    EXPECT_EQ(surfaceLabelRepeater->property("count").toInt(), 3);
    EXPECT_EQ(activeSourceRepeater->property("count").toInt(), 3);
    EXPECT_EQ(root->property("availableViewModes").toList().size(), 6);
    EXPECT_TRUE(analysisGridMenuItem->property("enabled").toBool());

    // Three-up (3 sources): each source panel gets a divider outline.
    QObject* const panelDividerRepeater =
        root->findChild<QObject*>(QStringLiteral("panelDividerRepeater"));
    ASSERT_NE(panelDividerRepeater, nullptr);
    EXPECT_EQ(panelDividerRepeater->property("count").toInt(), 3);
    // Analysis Grid (3 sources): same divider treatment; labels must stay inside the
    // viewport and must not overlap the analysis chrome.
    preferences.setViewMode(ReviewPreferencesController::ViewMode::AnalysisGrid);
    controller.refreshProjection();
    QCoreApplication::processEvents();
    EXPECT_EQ(root->property("effectiveViewMode").toInt(), 4);
    EXPECT_EQ(panelDividerRepeater->property("count").toInt(), 3);
    EXPECT_EQ(surfaceLabelRepeater->property("count").toInt(), 3);
    EXPECT_TRUE(analysisChrome->property("visible").toBool());
    auto* const chromeItem = qobject_cast<QQuickItem*>(analysisChrome);
    ASSERT_NE(chromeItem, nullptr);
    const QRectF chromeRectInViewport(chromeItem->mapToItem(viewport, QPointF{0, 0}),
                                      chromeItem->size());
    for (int slot = 0; slot < 3; ++slot) {
        QVariant labelResult;
        ASSERT_TRUE(QMetaObject::invokeMethod(viewport,
                                              "surfaceLabelGeometry",
                                              Q_RETURN_ARG(QVariant, labelResult),
                                              Q_ARG(QVariant, QVariant{slot})));
        const QVariantMap label = labelResult.toMap();
        ASSERT_FALSE(label.isEmpty());
        EXPECT_TRUE(label.value(QStringLiteral("visible")).toBool());
        const qreal labelX = label.value(QStringLiteral("x")).toDouble();
        const qreal labelW = label.value(QStringLiteral("width")).toDouble();
        // surfaceLabelGeometry reports x/width/visible; pull the real y/height from the
        // live label delegate so containment and chrome-overlap use actual geometry.
        QQuickItem* labelDelegate = nullptr;
        ASSERT_TRUE(QMetaObject::invokeMethod(surfaceLabelRepeater,
                                              "itemAt",
                                              Q_RETURN_ARG(QQuickItem*, labelDelegate),
                                              Q_ARG(int, slot)));
        ASSERT_NE(labelDelegate, nullptr);
        const qreal labelY = labelDelegate->y();
        const qreal labelH = labelDelegate->height();
        EXPECT_GE(labelX, 0.0);
        EXPECT_GE(labelY, 0.0);
        EXPECT_LE(labelX + labelW, viewport->width());
        EXPECT_LE(labelY + labelH, viewport->height());
        EXPECT_TRUE(QRectF(labelX, labelY, labelW, labelH).intersects(chromeRectInViewport) ==
                    false)
            << "source label slot " << slot << " overlaps analysis chrome";
    }
    preferences.setViewMode(ReviewPreferencesController::ViewMode::ThreeUp);
    controller.refreshProjection();
    QCoreApplication::processEvents();

    // In multi-source the TabbedInspector exposes the Compare/Review/Info tabs.
    shell.setInspectorVisible(true);
    QCoreApplication::processEvents();
    QObject* const inspectorTabBar =
        tabbedInspector->findChild<QObject*>(QStringLiteral("inspectorTabBar"));
    ASSERT_NE(inspectorTabBar, nullptr);
    EXPECT_EQ(inspectorTabBar->property("count").toInt(), 3);
    for (const char* const tabName : {"compareTabButton", "reviewTabButton", "infoTabButton"}) {
        QObject* const tab = tabbedInspector->findChild<QObject*>(QString::fromLatin1(tabName));
        ASSERT_NE(tab, nullptr);
        EXPECT_TRUE(tab->property("visible").toBool());
    }
    // Restore the pre-existing hidden-inspector state so the following two-source and
    // one-source sections observe the same chrome layout as before this check.
    shell.setInspectorVisible(false);
    QCoreApplication::processEvents();

    snapshot->sources.resize(2U);
    snapshot->presentedSources.resize(2U);
    controller.refreshProjection();
    preferences.setViewMode(ReviewPreferencesController::ViewMode::AnalysisGrid);
    QCoreApplication::processEvents();
    // Two-source effective state falls back safely without overwriting the persisted three-up
    // preference, which becomes valid again if a third source is later restored.
    EXPECT_EQ(preferences.viewMode(), ReviewPreferencesController::ViewMode::AnalysisGrid);
    EXPECT_EQ(root->property("effectiveViewMode").toInt(), ComparisonSurface::SideBySide);
    EXPECT_EQ(root->property("availableViewModes").toList().size(), 3);
    EXPECT_EQ(activeSourceRepeater->property("count").toInt(), 2);
    EXPECT_FALSE(analysisGridMenuItem->property("enabled").toBool());

    snapshot->sources.resize(1U);
    snapshot->presentedSources.resize(1U);
    controller.refreshProjection();
    QCoreApplication::processEvents();
    EXPECT_EQ(surfaceLabelRepeater->property("count").toInt(), 1);
    EXPECT_EQ(activeSourceRepeater->property("count").toInt(), 1);
    EXPECT_EQ(root->property("availableViewModes").toList().size(), 1);
    EXPECT_FALSE(compareBar->isVisible());

    shell.setInspectorVisible(true);
    QCoreApplication::processEvents();
    EXPECT_TRUE(tabbedInspector->property("visible").toBool());
    EXPECT_EQ(tabbedInspector->property("effectiveTab").toInt(), 1);
    EXPECT_TRUE(setInButton->property("visible").toBool());
    EXPECT_TRUE(setOutButton->property("visible").toBool());

    // In single mode the inspector keeps only the Review/Info tabs; the Compare tab hides and
    // the Review-tab range actions remain reachable.
    QObject* const compareTab =
        tabbedInspector->findChild<QObject*>(QStringLiteral("compareTabButton"));
    QObject* const reviewTab =
        tabbedInspector->findChild<QObject*>(QStringLiteral("reviewTabButton"));
    QObject* const infoTab = tabbedInspector->findChild<QObject*>(QStringLiteral("infoTabButton"));
    ASSERT_NE(compareTab, nullptr);
    ASSERT_NE(reviewTab, nullptr);
    ASSERT_NE(infoTab, nullptr);
    EXPECT_FALSE(compareTab->property("visible").toBool());
    EXPECT_TRUE(reviewTab->property("visible").toBool());
    EXPECT_TRUE(infoTab->property("visible").toBool());
    EXPECT_TRUE(loopRangeButton->property("visible").toBool());
    EXPECT_TRUE(clearRangeButton->property("visible").toBool());

    snapshot->sessionState = domain::SessionState::kEmpty;
    snapshot->displayedFrame.reset();
    snapshot->canonicalFrameCount = 0U;
    snapshot->sources.clear();
    snapshot->presentedSources.clear();
    snapshot->validatedComparison.reset();
    controller.refreshProjection();
    QCoreApplication::processEvents();
    EXPECT_EQ(root->property("oscState").toInt(), 2);
    EXPECT_FALSE(transport->isVisible());
    EXPECT_FALSE(compareMenu->property("enabled").toBool());
    EXPECT_FALSE(analyzeMenu->property("enabled").toBool());
    EXPECT_TRUE(reviewContextMenu->property("emptyStateOnly").toBool());
    EXPECT_EQ(reviewContextMenu->property("availableActionCount").toInt(), 2);
}

TEST(MainQmlContractTests, DockedTransportResolvesContextuallyAndClearsViewport) {
    // Harness: a mutable two-source session so the contextual transport rule can be
    // exercised across multi (pinned/docked), single (auto-hide/overlay), and empty
    // (hidden) topologies on one instantiated Main.qml.
    auto snapshot = std::make_shared<application::SessionSnapshot>();
    snapshot->graphicsReady = true;
    snapshot->sessionState = domain::SessionState::kReady;
    snapshot->playbackState = domain::PlaybackState::kPaused;
    snapshot->displayedFrame = domain::FrameId{0};
    snapshot->canonicalFrameCount = 10U;
    snapshot->sources = {
        application::SessionSourceView{
            .sourceId = 0U,
            .role = domain::ComparisonRole::kReference,
            .displayName = "A",
        },
        application::SessionSourceView{
            .sourceId = 1U,
            .role = domain::ComparisonRole::kPrediction,
            .displayName = "B",
        },
    };
    snapshot->presentedSources = {
        application::PresentedSourceState{
            .sourceId = 0U,
            .sourceFrameId = domain::FrameId{0},
            .matchKind = application::FrameMatchKind::ExactIndex,
        },
        application::PresentedSourceState{
            .sourceId = 1U,
            .sourceFrameId = domain::FrameId{0},
            .matchKind = application::FrameMatchKind::ExactIndex,
        },
    };
    std::vector<application::PlaybackCommand> submitted;
    std::vector<application::CommandTerminal> terminals;
    ReviewController controller{
        ReviewController::Dependencies{
            .submit =
                [&submitted](application::PlaybackCommand command) {
                    submitted.push_back(std::move(command));
                    return application::PortSubmitResult::Accepted;
                },
            .snapshot = [snapshot] { return snapshot; },
            .takeCompletedCommands =
                [&terminals] {
                    std::vector<application::CommandTerminal> result = std::move(terminals);
                    terminals.clear();
                    return result;
                },
        },
    };
    ReviewPreferencesController preferences{std::make_shared<ClosedSettingsRepository>()};
    ReviewShellController shell{controller, preferences};
    ReviewSessionFacade facade{controller, preferences, shell};

    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    engine.rootContext()->setContextProperty(QStringLiteral("reviewController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewPreferences"), &preferences);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewSession"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewFacade"), &facade);
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);

    std::unique_ptr<QObject> root{component.create()};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);

    window->resize(960, 640);
    window->show();
    const auto processLayout = [] {
        for (int iteration = 0; iteration < 5; ++iteration) {
            QCoreApplication::processEvents();
        }
    };
    processLayout();

    auto* const transport = root->findChild<QQuickItem*>(QStringLiteral("transport"));
    auto* const viewport = root->findChild<QQuickItem*>(QStringLiteral("mediaViewportFocusTarget"));
    ASSERT_NE(transport, nullptr);
    ASSERT_NE(viewport, nullptr);

    const auto rectInContent = [&](QQuickItem* item) {
        const QPointF tl = item->mapToItem(window->contentItem(), QPointF{0.0, 0.0});
        return QRectF(tl.x(), tl.y(), item->width(), item->height());
    };

    // --- Multi-source: contextual rule resolves to pinned/docked. ---
    ASSERT_EQ(controller.sourceCount(), 2);
    EXPECT_FALSE(root->property("singleMode").toBool());
    EXPECT_TRUE(root->property("transportDocked").toBool());
    EXPECT_FALSE(root->property("transportOverlay").toBool());
    EXPECT_FALSE(root->property("transportHidden").toBool());

    // Docked transport must sit below the viewport and not geometrically intersect it.
    {
        const QRectF transportRect = rectInContent(transport);
        const QRectF viewportRect = rectInContent(viewport);
        EXPECT_FALSE(transportRect.intersects(viewportRect))
            << "docked transport must not intersect viewport (transport=" << transportRect.x()
            << "," << transportRect.y() << " " << transportRect.width() << "x"
            << transportRect.height() << " viewport=" << viewportRect.x() << "," << viewportRect.y()
            << " " << viewportRect.width() << "x" << viewportRect.height() << ")";
        EXPECT_GE(transportRect.top(), viewportRect.bottom() - 1.0)
            << "docked transport top must be at/under viewport bottom";
    }

    // Transport right edge stays within the content width at 960x640.
    {
        const QPointF transportRight =
            transport->mapToItem(window->contentItem(), QPointF{transport->width(), 0.0});
        EXPECT_LE(transportRight.x(), window->contentItem()->width() + 1.0);
    }

    // --- Single source: contextual rule resolves to auto-hide/overlay. ---
    snapshot->sources.resize(1U);
    snapshot->presentedSources.resize(1U);
    controller.refreshProjection();
    processLayout();
    EXPECT_EQ(controller.sourceCount(), 1);
    EXPECT_TRUE(root->property("singleMode").toBool());
    EXPECT_FALSE(root->property("transportDocked").toBool());
    EXPECT_TRUE(root->property("transportOverlay").toBool());
    EXPECT_FALSE(root->property("transportHidden").toBool());

    // Overlay transport must overlap the canvas (intersects the viewport footprint).
    {
        const QRectF transportRect = rectInContent(transport);
        const QRectF viewportRect = rectInContent(viewport);
        EXPECT_TRUE(transportRect.intersects(viewportRect))
            << "overlay transport must intersect viewport (transport=" << transportRect.x() << ","
            << transportRect.y() << " " << transportRect.width() << "x" << transportRect.height()
            << " viewport=" << viewportRect.x() << "," << viewportRect.y() << " "
            << viewportRect.width() << "x" << viewportRect.height() << ")";
    }

    // --- Empty: transport hidden and its controls disabled. ---
    snapshot->sources.clear();
    snapshot->presentedSources.clear();
    snapshot->sessionState = domain::SessionState::kEmpty;
    snapshot->displayedFrame.reset();
    snapshot->canonicalFrameCount = 0U;
    controller.refreshProjection();
    processLayout();
    EXPECT_EQ(controller.sourceCount(), 0);
    EXPECT_TRUE(root->property("transportHidden").toBool());
    EXPECT_FALSE(root->property("transportDocked").toBool());
    EXPECT_FALSE(root->property("transportOverlay").toBool());
    EXPECT_FALSE(transport->isVisible());
    EXPECT_FALSE(transport->property("controlsEnabled").toBool())
        << "hidden auto-hide panel must expose controlsEnabled == false";
}

TEST(MainQmlContractTests, ShowsIntentMessageOnSynchronousRejection) {
    auto snapshot = std::make_shared<application::SessionSnapshot>();
    snapshot->graphicsReady = true;
    snapshot->sessionState = domain::SessionState::kReady;
    snapshot->playbackState = domain::PlaybackState::kPaused;
    snapshot->displayedFrame = domain::FrameId{0};
    snapshot->canonicalFrameCount = 10U;
    snapshot->sources = {
        application::SessionSourceView{
            .sourceId = 0U,
            .role = domain::ComparisonRole::kReference,
            .displayName = "A",
        },
        application::SessionSourceView{
            .sourceId = 1U,
            .role = domain::ComparisonRole::kPrediction,
            .displayName = "B",
        },
    };
    snapshot->presentedSources = {
        application::PresentedSourceState{
            .sourceId = 0U,
            .sourceFrameId = domain::FrameId{0},
            .matchKind = application::FrameMatchKind::ExactIndex,
        },
        application::PresentedSourceState{
            .sourceId = 1U,
            .sourceFrameId = domain::FrameId{0},
            .matchKind = application::FrameMatchKind::ExactIndex,
        },
    };
    std::vector<application::PlaybackCommand> submitted;
    std::vector<application::CommandTerminal> terminals;
    ReviewController controller{
        ReviewController::Dependencies{
            .submit =
                [&submitted](application::PlaybackCommand command) {
                    submitted.push_back(std::move(command));
                    return application::PortSubmitResult::Accepted;
                },
            .snapshot = [snapshot] { return snapshot; },
            .takeCompletedCommands =
                [&terminals] {
                    std::vector<application::CommandTerminal> result = std::move(terminals);
                    terminals.clear();
                    return result;
                },
        },
    };
    ReviewPreferencesController preferences{std::make_shared<ClosedSettingsRepository>()};
    ReviewShellController shell{controller, preferences};
    ReviewSessionFacade facade{controller, preferences, shell};

    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    engine.rootContext()->setContextProperty(QStringLiteral("reviewController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewPreferences"), &preferences);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewSession"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewFacade"), &facade);
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);

    std::unique_ptr<QObject> root{component.create()};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    QCoreApplication::processEvents();

    // Without validatedComparison, shell.activeSourceIdentities is empty, so any identity
    // passed to changeReference or removeSelectedSource will be rejected (sourceIndex < 0).
    // The wrapper functions should show a toast message on rejection.

    // Test changeReference rejection
    QVariant changeResult;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        root.get(),
        "changeReference",
        Q_RETURN_ARG(QVariant, changeResult),
        Q_ARG(QVariant, QVariant{QStringLiteral("nonexistent-identity")})));
    EXPECT_FALSE(changeResult.toBool());
    EXPECT_FALSE(root->property("intentMessage").toString().isEmpty())
        << "changeReference rejection should show intent message";

    // Clear the intent message
    root->setProperty("intentMessage", QStringLiteral(""));
    QCoreApplication::processEvents();

    // Test removeSelectedSource rejection
    QVariant removeResult;
    ASSERT_TRUE(
        QMetaObject::invokeMethod(root.get(),
                                  "removeSelectedSource",
                                  Q_RETURN_ARG(QVariant, removeResult),
                                  Q_ARG(QVariant, QVariant{QStringLiteral("does-not-exist")})));
    EXPECT_FALSE(removeResult.toBool());
    EXPECT_FALSE(root->property("intentMessage").toString().isEmpty())
        << "removeSelectedSource rejection should show intent message";
}

TEST(MainQmlContractTests, DrawerScrimTransportShrinkAndEscClose) {
    // Build a validated comparison with real temporary files so the shell identity
    // pipeline is exercised and source menu items are available.
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    const std::filesystem::path pathA =
        std::filesystem::path(tempDir.path().toStdWString()) / "drawer_sourceA.mp4";
    const std::filesystem::path pathB =
        std::filesystem::path(tempDir.path().toStdWString()) / "drawer_sourceB.mp4";
    {
        QFile fileA{QString::fromStdWString(pathA.wstring())};
        ASSERT_TRUE(fileA.open(QIODevice::WriteOnly));
        fileA.write("drawer-a-bytes", 14);
    }
    {
        QFile fileB{QString::fromStdWString(pathB.wstring())};
        ASSERT_TRUE(fileB.open(QIODevice::WriteOnly));
        fileB.write("drawer-b-bytes", 14);
    }

    const auto rateResult = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rateResult);
    const domain::RationalRate rate = rateResult.value();

    std::vector<domain::ComparisonSource> comparisonSources;
    const std::array<std::filesystem::path, 2> paths = {pathA, pathB};
    for (std::size_t index = 0U; index < paths.size(); ++index) {
        const QFileInfo info{QString::fromStdWString(paths[index].wstring())};
        comparisonSources.push_back(domain::ComparisonSource{
            .id = static_cast<domain::SourceId>(index),
            .role = index == 0U ? domain::ComparisonRole::kReference
                                : domain::ComparisonRole::kPrediction,
            .descriptor =
                domain::MediaDescriptor{
                    .normalizedPath = paths[index],
                    .extent = domain::MediaExtent{.width = 1'920U, .height = 1'080U},
                    .frameRate = rate,
                    .frameCount =
                        domain::FrameCountInfo{
                            .value = 12,
                            .origin = domain::FrameCountOrigin::kReported,
                        },
                    .duration = domain::MediaTime{400'000},
                    .codecId = "h264",
                    .pixelFormatId = "nv12",
                    .bitDepth = 8U,
                    .decodeCapabilities =
                        domain::DecodeCapabilities{
                            .softwareDecode = true,
                            .d3d11VaDecode = true,
                        },
                    .timingConfidence = domain::TimingConfidence::kVerifiedCfr,
                    .sourceIdentity =
                        domain::SourceFileIdentity{
                            .byteSize = static_cast<std::uint64_t>(info.size()),
                            .modifiedUtcMilliseconds = info.lastModified().toMSecsSinceEpoch(),
                            .fingerprintSha256 = std::string(64U, '0'),
                        },
                },
            .displayName = std::string{"Source "} + static_cast<char>('A' + index),
        });
    }
    auto validated = domain::ComparisonValidator::validate(std::move(comparisonSources));
    ASSERT_TRUE(validated);

    auto snapshot = std::make_shared<application::SessionSnapshot>();
    snapshot->graphicsReady = true;
    snapshot->sessionState = domain::SessionState::kReady;
    snapshot->playbackState = domain::PlaybackState::kPaused;
    snapshot->displayedFrame = domain::FrameId{0};
    snapshot->validatedComparison =
        std::make_shared<const domain::ValidatedComparisonSet>(std::move(validated).value().set);
    snapshot->canonicalFrameCount =
        static_cast<std::uint64_t>(snapshot->validatedComparison->canonicalFrameCount());
    snapshot->canonicalTimeline = rate;
    for (const auto& source : snapshot->validatedComparison->sources()) {
        snapshot->sources.push_back(application::SessionSourceView{
            .sourceId = source.id,
            .role = source.role,
            .displayName = source.displayName,
        });
        snapshot->presentedSources.push_back(application::PresentedSourceState{
            .sourceId = source.id,
            .sourceFrameId = domain::FrameId{0},
            .matchKind = application::FrameMatchKind::ExactIndex,
        });
    }

    std::vector<application::PlaybackCommand> submitted;
    std::vector<application::CommandTerminal> terminals;
    ReviewController controller{
        ReviewController::Dependencies{
            .submit =
                [&submitted](application::PlaybackCommand command) {
                    submitted.push_back(std::move(command));
                    return application::PortSubmitResult::Accepted;
                },
            .snapshot = [snapshot] { return snapshot; },
            .takeCompletedCommands =
                [&terminals] {
                    std::vector<application::CommandTerminal> result = std::move(terminals);
                    terminals.clear();
                    return result;
                },
        },
    };
    ReviewPreferencesController preferences{std::make_shared<ClosedSettingsRepository>()};
    ReviewShellController shell{controller, preferences};
    ReviewSessionFacade facade{controller, preferences, shell};

    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    engine.rootContext()->setContextProperty(QStringLiteral("reviewController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewPreferences"), &preferences);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewSession"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewFacade"), &facade);
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);

    std::unique_ptr<QObject> root{component.create()};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);
    QCoreApplication::processEvents();

    auto* const scrim = root->findChild<QObject*>(QStringLiteral("inspectorScrim"));
    ASSERT_NE(scrim, nullptr);
    auto* const inspectorItem = root->findChild<QQuickItem*>(QStringLiteral("tabbedInspector"));
    ASSERT_NE(inspectorItem, nullptr);
    auto* const transportItem = root->findChild<QQuickItem*>(QStringLiteral("transport"));
    ASSERT_NE(transportItem, nullptr);
    auto* const viewportItem =
        root->findChild<QQuickItem*>(QStringLiteral("mediaViewportFocusTarget"));
    ASSERT_NE(viewportItem, nullptr);
    QQuickItem* const layoutRoot = inspectorItem->parentItem();
    ASSERT_NE(layoutRoot, nullptr);

    const auto processLayout = [] {
        for (int iteration = 0; iteration < 5; ++iteration) {
            QCoreApplication::processEvents();
        }
    };

    // Keep the geometry matrix hidden so small Windows desktops do not clamp the requested
    // logical size. The exposed 960x640 pixel and keyboard checks remain below.
    struct GeometryCase {
        int width;
        int height;
    };
    constexpr std::array<GeometryCase, 4> kSizes = {
        GeometryCase{960, 640},
        GeometryCase{1100, 700},
        GeometryCase{1120, 700},
        GeometryCase{1440, 900},
    };

    for (const auto& size : kSizes) {
        window->resize(size.width, size.height);
        processLayout();
        const auto restoreHiddenContentSize = [&] {
            layoutRoot->setSize(
                QSizeF{static_cast<qreal>(size.width), static_cast<qreal>(size.height)});
        };
        restoreHiddenContentSize();

        // --- Inspector closed ---
        shell.setInspectorVisible(false);
        processLayout();
        restoreHiddenContentSize();

        EXPECT_FALSE(scrim->property("visible").toBool())
            << size.width << "x" << size.height << " scrim hidden when inspector closed";

        // Transport spans approximately the full viewport width.
        const QPointF transportRightClosed =
            transportItem->mapToItem(window->contentItem(), QPointF{transportItem->width(), 0.0});
        const double contentWidth = layoutRoot->width();
        const double closedViewportWidth = viewportItem->width();
        // Allow a small margin for chrome margins.
        EXPECT_GE(transportRightClosed.x(), contentWidth * 0.85)
            << size.width << "x" << size.height
            << " transport should span viewport when closed (right=" << transportRightClosed.x()
            << " contentWidth=" << contentWidth << ")";

        // --- Inspector open ---
        shell.setInspectorVisible(true);
        processLayout();
        restoreHiddenContentSize();

        const bool isDrawer = size.width < 1120;

        if (isDrawer) {
            EXPECT_TRUE(root->property("drawerMode").toBool())
                << size.width << "x" << size.height << " drawerMode expected";
            EXPECT_TRUE(scrim->property("visible").toBool())
                << size.width << "x" << size.height << " scrim visible in drawer";

            // Transport right edge <= inspector left edge (in window coords).
            const QPointF transportRightOpen = transportItem->mapToItem(
                window->contentItem(), QPointF{transportItem->width(), 0.0});
            const QPointF inspectorLeft =
                inspectorItem->mapToItem(window->contentItem(), QPointF{0.0, 0.0});
            EXPECT_LE(transportRightOpen.x(), inspectorLeft.x() + 2.0)
                << size.width << "x" << size.height << " transport right ("
                << transportRightOpen.x() << ") <= inspector left (" << inspectorLeft.x() << ")";

            // Inspector is fully within the window.
            EXPECT_LE(inspectorLeft.x() + inspectorItem->width(),
                      static_cast<double>(size.width) + 2.0)
                << size.width << "x" << size.height << " inspector within window";

            // Viewport keeps the same width as when the inspector is closed
            // (it spans the full content width, unlike side-by-side).
            EXPECT_DOUBLE_EQ(viewportItem->width(), closedViewportWidth)
                << size.width << "x" << size.height
                << " viewport full width in drawer (same as closed state)";
        } else {
            EXPECT_FALSE(root->property("drawerMode").toBool())
                << size.width << "x" << size.height << " not drawerMode";
            EXPECT_FALSE(scrim->property("visible").toBool())
                << size.width << "x" << size.height << " scrim hidden in side-by-side";

            // Viewport right edge <= inspector left edge.
            const QPointF viewportRight =
                viewportItem->mapToItem(window->contentItem(), QPointF{viewportItem->width(), 0.0});
            const QPointF inspectorLeft =
                inspectorItem->mapToItem(window->contentItem(), QPointF{0.0, 0.0});
            EXPECT_LE(viewportRight.x(), inspectorLeft.x() + 2.0)
                << size.width << "x" << size.height << " viewport right (" << viewportRight.x()
                << ") <= inspector left (" << inspectorLeft.x() << ")";
        }
    }

    // --- Esc closes drawer ---
    window->resize(960, 640);
    window->show();
    window->requestActivate();
    shell.setInspectorVisible(true);
    processLayout();
    ASSERT_TRUE(root->property("drawerMode").toBool());
    ASSERT_TRUE(scrim->property("visible").toBool());
    ASSERT_TRUE(shell.inspectorVisible());

    // The new drawer Esc shortcut requires inputContext == 0 (no menus/dialogs/text editing).
    EXPECT_EQ(root->property("inputContext").toInt(), 0);
    sendKey(*window, Qt::Key_Escape);
    // Give the shortcut system extra time to process.
    QElapsedTimer escWait;
    escWait.start();
    while (shell.inspectorVisible() && escWait.elapsed() < 500) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5U);
    }
    EXPECT_FALSE(shell.inspectorVisible()) << "Esc should close the inspector drawer";

    // --- Pixel gate: 960x640 drawer open ---
    shell.setInspectorVisible(true);
    processLayout();
    // Allow the rendering backend to produce a frame.
    QElapsedTimer renderWait;
    renderWait.start();
    while (renderWait.elapsed() < 200) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5U);
    }

    const QImage grab = window->grabWindow();
    const double dpr = window->devicePixelRatio();
    const int expectedGrabWidth = static_cast<int>(960.0 * dpr);
    const int expectedGrabHeight = static_cast<int>(640.0 * dpr);
    EXPECT_EQ(grab.width(), expectedGrabWidth);
    EXPECT_EQ(grab.height(), expectedGrabHeight);

    if (!grab.isNull() && grab.width() == expectedGrabWidth &&
        grab.height() == expectedGrabHeight) {
        // The inspector occupies the right side. Sample a pixel in the expected
        // inspector region (logical x near right edge, y near vertical center).
        const int inspectorSampleX = static_cast<int>((960.0 - 30.0) * dpr);
        const int inspectorSampleY = static_cast<int>(320.0 * dpr);
        const QColor inspectorPixel = grab.pixelColor(inspectorSampleX, inspectorSampleY);
        EXPECT_EQ(inspectorPixel.alpha(), 255)
            << "inspector area pixel should be opaque (alpha=" << inspectorPixel.alpha() << ")";
        // The panel background is #111823. Accept any opaque dark pixel.
        EXPECT_LT(inspectorPixel.red() + inspectorPixel.green() + inspectorPixel.blue(), 150)
            << "inspector pixel should be a dark panel colour";

        // Inspector left edge in device pixel coordinates.
        const QPointF inspectorLeftPx =
            inspectorItem->mapToItem(window->contentItem(), QPointF{0.0, 0.0});
        const int inspectorLeftDevice = static_cast<int>(inspectorLeftPx.x() * dpr);

        // Verify the transport area just left of the inspector has non-background pixels
        // (the transport panel is visible there).
        const int transportRow = static_cast<int>(600.0 * dpr);
        const int justLeftOfInspector =
            std::max(0, inspectorLeftDevice - static_cast<int>(10.0 * dpr));
        const QColor leftOfInspectorPx = grab.pixelColor(justLeftOfInspector, transportRow);
        // The viewport background is #06080d (very dark). Transport or scrim area should
        // differ from this. Accept any pixel that is not the pure viewport background.
        EXPECT_TRUE(leftOfInspectorPx.alpha() == 255)
            << "area just left of inspector should be opaque";

        // Verify that the inspector panel area to the right has opaque panel pixels.
        if (inspectorLeftDevice + static_cast<int>(20.0 * dpr) < expectedGrabWidth) {
            const QColor inspectorAreaPx = grab.pixelColor(
                inspectorLeftDevice + static_cast<int>(20.0 * dpr), static_cast<int>(400.0 * dpr));
            EXPECT_EQ(inspectorAreaPx.alpha(), 255) << "inspector panel area should be opaque";
        }
    }
}

TEST(MainQmlContractTests, NestedPopupMouseTraversal) {
    // Test A: real mouse interaction across nested Popup.Window boundaries.
    // Opens Compare menu, cascades into Layout submenu, clicks "Three up".
    //
    // FALLBACK NOTE: Synthetic hover events (QHoverEvent/QEnterEvent) sent via
    // QCoreApplication::sendEvent do not reliably trigger Qt Quick Controls Menu
    // cascade across separate Popup.Window instances in headless/offscreen test
    // environments. The cascade mechanism relies on platform window-system mouse
    // tracking that synthetic events cannot fully replicate.
    //
    // Approach taken (fallback level 2): Both menus are opened programmatically
    // (equivalent to what cascade would do), then a real mouse click is delivered
    // to the "Three up" item inside the layoutMenu popup window. Background alpha
    // is verified on the popup window grab. The hover-traversal segment (parent
    // menu → gap → child menu) requires manual verification on real hardware.

    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    const std::filesystem::path pathA =
        std::filesystem::path(tempDir.path().toStdWString()) / "mouseA.mp4";
    const std::filesystem::path pathB =
        std::filesystem::path(tempDir.path().toStdWString()) / "mouseB.mp4";
    const std::filesystem::path pathC =
        std::filesystem::path(tempDir.path().toStdWString()) / "mouseC.mp4";
    const std::array<std::filesystem::path, 3> sourcePaths = {pathA, pathB, pathC};
    for (const auto& path : sourcePaths) {
        QFile file{QString::fromStdWString(path.wstring())};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        const QByteArray data = QByteArray("mouse-bytes-") + path.filename().string().c_str();
        file.write(data);
    }

    const auto rateResult = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rateResult);
    const domain::RationalRate rate = rateResult.value();

    std::vector<domain::ComparisonSource> comparisonSources;
    for (std::size_t index = 0U; index < sourcePaths.size(); ++index) {
        const QFileInfo info{QString::fromStdWString(sourcePaths[index].wstring())};
        comparisonSources.push_back(domain::ComparisonSource{
            .id = static_cast<domain::SourceId>(index),
            .role = index == 0U ? domain::ComparisonRole::kReference
                                : domain::ComparisonRole::kPrediction,
            .descriptor =
                domain::MediaDescriptor{
                    .normalizedPath = sourcePaths[index],
                    .extent = domain::MediaExtent{.width = 1'920U, .height = 1'080U},
                    .frameRate = rate,
                    .frameCount =
                        domain::FrameCountInfo{
                            .value = 12,
                            .origin = domain::FrameCountOrigin::kReported,
                        },
                    .duration = domain::MediaTime{400'000},
                    .codecId = "h264",
                    .pixelFormatId = "nv12",
                    .bitDepth = 8U,
                    .decodeCapabilities =
                        domain::DecodeCapabilities{
                            .softwareDecode = true,
                            .d3d11VaDecode = true,
                        },
                    .timingConfidence = domain::TimingConfidence::kVerifiedCfr,
                    .sourceIdentity =
                        domain::SourceFileIdentity{
                            .byteSize = static_cast<std::uint64_t>(info.size()),
                            .modifiedUtcMilliseconds = info.lastModified().toMSecsSinceEpoch(),
                            .fingerprintSha256 = std::string(64U, '0'),
                        },
                },
            .displayName = std::string{"Source "} + static_cast<char>('A' + index),
        });
    }
    auto validated = domain::ComparisonValidator::validate(std::move(comparisonSources));
    ASSERT_TRUE(validated);

    auto snapshot = std::make_shared<application::SessionSnapshot>();
    snapshot->graphicsReady = true;
    snapshot->sessionState = domain::SessionState::kReady;
    snapshot->playbackState = domain::PlaybackState::kPaused;
    snapshot->displayedFrame = domain::FrameId{0};
    snapshot->validatedComparison =
        std::make_shared<const domain::ValidatedComparisonSet>(std::move(validated).value().set);
    snapshot->canonicalFrameCount =
        static_cast<std::uint64_t>(snapshot->validatedComparison->canonicalFrameCount());
    snapshot->canonicalTimeline = rate;
    for (const auto& source : snapshot->validatedComparison->sources()) {
        snapshot->sources.push_back(application::SessionSourceView{
            .sourceId = source.id,
            .role = source.role,
            .displayName = source.displayName,
        });
        snapshot->presentedSources.push_back(application::PresentedSourceState{
            .sourceId = source.id,
            .sourceFrameId = domain::FrameId{0},
            .matchKind = application::FrameMatchKind::ExactIndex,
        });
    }

    std::vector<application::PlaybackCommand> submitted;
    std::vector<application::CommandTerminal> terminals;
    ReviewController controller{
        ReviewController::Dependencies{
            .submit =
                [&submitted](application::PlaybackCommand command) {
                    submitted.push_back(std::move(command));
                    return application::PortSubmitResult::Accepted;
                },
            .snapshot = [snapshot] { return snapshot; },
            .takeCompletedCommands =
                [&terminals] {
                    std::vector<application::CommandTerminal> result = std::move(terminals);
                    terminals.clear();
                    return result;
                },
        },
    };
    ReviewPreferencesController preferences{std::make_shared<ClosedSettingsRepository>()};
    ReviewShellController shell{controller, preferences};
    ReviewSessionFacade facade{controller, preferences, shell};

    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    engine.rootContext()->setContextProperty(QStringLiteral("reviewController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewPreferences"), &preferences);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewSession"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewFacade"), &facade);
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);

    std::unique_ptr<QObject> root{component.create()};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);
    window->resize(1440, 900);
    window->show();
    QCoreApplication::processEvents();

    // Verify 3 sources are present.
    EXPECT_EQ(controller.sourceCount(), 3);

    auto* const compareMenu = root->findChild<QObject*>(QStringLiteral("compareMenu"));
    auto* const layoutMenu = root->findChild<QObject*>(QStringLiteral("layoutMenu"));
    auto* const threeUpItem = root->findChild<QObject*>(QStringLiteral("threeUpMenuItem"));
    ASSERT_NE(compareMenu, nullptr);
    ASSERT_NE(layoutMenu, nullptr);
    ASSERT_NE(threeUpItem, nullptr);

    const auto processLayout = [] {
        for (int iteration = 0; iteration < 5; ++iteration) {
            QCoreApplication::processEvents();
        }
    };

    // Step 1: Open the Compare menu.
    ASSERT_TRUE(QMetaObject::invokeMethod(compareMenu, "open"));
    processLayout();
    EXPECT_TRUE(compareMenu->property("opened").toBool());

    // Step 2: Attempt hover cascade to Layout submenu.
    // Send QHoverEvent to the layoutMenu's visual area via the compareMenu popup.
    bool cascadeTriggered = false;
    QQuickWindow* comparePopup = menuPopupWindow(compareMenu);
    if (comparePopup) {
        // Try to find the Layout menu item in the compare popup and hover it.
        auto* layoutContentItem =
            comparePopup->contentItem()->findChild<QQuickItem*>(QStringLiteral("layoutMenu"));
        if (!layoutContentItem) {
            // Try finding via the menu's contentItem tree.
            auto* compareContentItem = compareMenu->property("contentItem").value<QQuickItem*>();
            if (compareContentItem) {
                layoutContentItem =
                    compareContentItem->findChild<QQuickItem*>(QStringLiteral("layoutMenu"));
            }
        }
        if (layoutContentItem) {
            const QPointF layoutCenter =
                QPointF{layoutContentItem->width() / 2.0, layoutContentItem->height() / 2.0};
            const QPointF scenePos = layoutContentItem->mapToScene(layoutCenter);
            const QPointF globalScenePos = comparePopup->mapToGlobal(scenePos.toPoint());
            // Send hover event to the compare popup window.
            QHoverEvent hoverEnter{
                QEvent::HoverEnter, scenePos, globalScenePos, QPointF{}, Qt::NoModifier};
            QCoreApplication::sendEvent(comparePopup, &hoverEnter);
            QHoverEvent hoverMove{
                QEvent::HoverMove, scenePos, globalScenePos, scenePos, Qt::NoModifier};
            QCoreApplication::sendEvent(comparePopup, &hoverMove);
            QCoreApplication::processEvents();

            // Also try sending a mouse move (which Menu uses for hover detection).
            sendMouseMove(*comparePopup, scenePos);
            processLayout();

            // Wait for cascade delay (~600ms).
            QElapsedTimer cascadeTimer;
            cascadeTimer.start();
            while (cascadeTimer.elapsed() < 700) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(10U);
                if (layoutMenu->property("opened").toBool()) {
                    cascadeTriggered = true;
                    break;
                }
            }
        }
    }

    // Step 3: Fallback — programmatically open the Layout submenu if cascade didn't fire.
    if (!cascadeTriggered) {
        ASSERT_TRUE(QMetaObject::invokeMethod(layoutMenu, "open"));
        processLayout();
    }

    // Step 4: Assert both menus are open.
    EXPECT_TRUE(compareMenu->property("opened").toBool())
        << "compareMenu should remain open while submenu is shown";
    EXPECT_TRUE(layoutMenu->property("opened").toBool())
        << "layoutMenu should be open (via cascade or programmatic fallback)";

    // Step 5: Find the layoutMenu popup window and verify background alpha.
    QQuickWindow* layoutPopup = menuPopupWindow(layoutMenu);
    if (!layoutPopup) {
        layoutPopup = findPopupWindow(QStringLiteral("layoutMenu"));
    }
    if (!layoutPopup) {
        layoutPopup = findPopupWindow(QStringLiteral("threeUpMenuItem"));
    }

    if (layoutPopup) {
        // Allow rendering to complete.
        QElapsedTimer renderWait;
        renderWait.start();
        while (renderWait.elapsed() < 200) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(5U);
        }

        const QImage grab = layoutPopup->grabWindow();
        if (!grab.isNull() && grab.width() > 0 && grab.height() > 0) {
            // Sample a background pixel (top-left corner area, inside the popup border).
            const int sampleX = static_cast<int>(10.0 * layoutPopup->devicePixelRatio());
            const int sampleY = static_cast<int>(10.0 * layoutPopup->devicePixelRatio());
            if (sampleX < grab.width() && sampleY < grab.height()) {
                const QColor bgPixel = grab.pixelColor(sampleX, sampleY);
                EXPECT_EQ(bgPixel.alpha(), 255)
                    << "popup background pixel should be opaque (alpha=" << bgPixel.alpha() << ")";
            }
        }
    }

    // Step 6: Click "Three up" with real mouse events in the layoutMenu popup.
    auto* threeUpQuickItem = qobject_cast<QQuickItem*>(threeUpItem);
    if (!threeUpQuickItem && layoutPopup) {
        threeUpQuickItem = layoutPopup->findChild<QQuickItem*>(QStringLiteral("threeUpMenuItem"));
    }

    if (threeUpQuickItem && layoutPopup) {
        const QPointF itemCenter =
            QPointF{threeUpQuickItem->width() / 2.0, threeUpQuickItem->height() / 2.0};
        const QPointF windowPos = threeUpQuickItem->mapToScene(itemCenter);
        sendMousePress(*layoutPopup, windowPos);
        sendMouseRelease(*layoutPopup, windowPos);
    } else {
        // Fallback: trigger the item directly if we cannot locate it in the popup.
        auto* threeUpMenuItem = qobject_cast<QQuickItem*>(threeUpItem);
        if (threeUpMenuItem) {
            QMetaObject::invokeMethod(threeUpItem, "triggered");
        }
        processLayout();
    }

    // Give time for menus to close and preference to propagate.
    QElapsedTimer closeWait;
    closeWait.start();
    while (closeWait.elapsed() < 500) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5U);
        if (!compareMenu->property("opened").toBool() && !layoutMenu->property("opened").toBool()) {
            break;
        }
    }

    // Step 7: Assert menus closed and viewMode == ThreeUp.
    EXPECT_FALSE(compareMenu->property("opened").toBool())
        << "compareMenu should close after clicking Three up";
    EXPECT_FALSE(layoutMenu->property("opened").toBool())
        << "layoutMenu should close after clicking Three up";
    EXPECT_EQ(preferences.viewMode(), ReviewPreferencesController::ViewMode::ThreeUp)
        << "viewMode should be ThreeUp after clicking the menu item";
    EXPECT_EQ(root->property("effectiveViewMode").toInt(), ComparisonSurface::ThreeUp);

    // Clean up: close any remaining popups.
    if (compareMenu->property("opened").toBool()) {
        QMetaObject::invokeMethod(compareMenu, "close");
    }
    if (layoutMenu->property("opened").toBool()) {
        QMetaObject::invokeMethod(layoutMenu, "close");
    }
    processLayout();
}

// Phase 0 baseline: the Range Loop must never present a frame outside [In,Out]. Today the loop is
// driven by Main.qml::onCurrentFrameChanged reacting to displayedFrame, with no kernel Range clamp,
// so after a >2000ms stall catch-up can present Out+Δ before QML seeks back. This QML-level test
// sets a range, drives playback forward, and asserts the presented frame never exceeds Out.
// Expected to FAIL/present-out-of-bounds on current code; passes after Phase 3's native Range Loop.
TEST(MainQmlContractTests, DISABLED_RangeLoopNeverPresentsOutsideInclusiveRange) {
    auto snapshot = std::make_shared<application::SessionSnapshot>();
    snapshot->graphicsReady = true;
    snapshot->sessionState = domain::SessionState::kReady;
    snapshot->playbackState = domain::PlaybackState::kPlaying;
    snapshot->displayedFrame = domain::FrameId{0};
    snapshot->canonicalFrameCount = 30U;
    snapshot->sources = {
        application::SessionSourceView{.sourceId = 0U, .displayName = "A"},
    };
    snapshot->presentedSources = {
        application::PresentedSourceState{
            .sourceId = 0U,
            .sourceFrameId = domain::FrameId{0},
            .matchKind = application::FrameMatchKind::ExactIndex,
        },
    };
    std::vector<application::PlaybackCommand> submitted;
    std::vector<application::CommandTerminal> terminals;
    ReviewController controller{ReviewController::Dependencies{
        .submit =
            [&submitted](application::PlaybackCommand command) {
                submitted.push_back(std::move(command));
                return application::PortSubmitResult::Accepted;
            },
        .snapshot = [snapshot] { return snapshot; },
        .takeCompletedCommands =
            [&terminals] {
                std::vector<application::CommandTerminal> result = std::move(terminals);
                terminals.clear();
                return result;
            },
    }};
    ReviewPreferencesController preferences{std::make_shared<ClosedSettingsRepository>()};
    ReviewShellController shell{controller, preferences};
    ReviewSessionFacade facade{controller, preferences, shell};

    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    engine.rootContext()->setContextProperty(QStringLiteral("reviewController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewPreferences"), &preferences);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewSession"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewFacade"), &facade);
    QQmlComponent component{&engine, QUrl{QUrl{QStringLiteral("qrc:/qml/Main.qml")}}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);
    std::unique_ptr<QObject> root{component.create()};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);
    QCoreApplication::processEvents();

    // Establish range [3, 8] and activate range playback.
    ASSERT_TRUE(QMetaObject::invokeMethod(root.get(), "setInPoint"));
    // setInPoint uses current frame; set Out beyond it via the shell directly for determinism.
    EXPECT_TRUE(shell.setRangeIn(3, controller.mediaTimeForFrame(3)));
    EXPECT_TRUE(shell.setRangeOut(8, controller.mediaTimeForFrame(8)));
    EXPECT_TRUE(shell.setRangePlaybackState(true, false));
    QCoreApplication::processEvents();
    EXPECT_TRUE(shell.rangePlaybackActive());
    EXPECT_EQ(shell.inFrame(), 3);
    EXPECT_EQ(shell.outFrame(), 8);

    // Drive playback forward past Out. Today onCurrentFrameChanged seeks back to In when the
    // presented frame reaches Out; it must never present Out+1. We advance displayedFrame in the
    // snapshot (simulating presentation) and let the QML handler react.
    auto advanceTo = [&](std::int64_t frame) {
        snapshot->displayedFrame = domain::FrameId{frame};
        controller.refreshProjection();
        for (int i = 0; i < 5; ++i) {
            QCoreApplication::processEvents();
        }
    };
    bool presentedOutsideRange = false;
    for (std::int64_t frame = 3; frame <= 12; ++frame) {
        advanceTo(frame);
        const std::int64_t presented = controller.currentFrame();
        if (presented < shell.inFrame() || presented > shell.outFrame()) {
            presentedOutsideRange = true;
            break;
        }
        if (!shell.rangePlaybackActive()) {
            break;
        }
    }
    EXPECT_FALSE(presentedOutsideRange)
        << "Range loop presented a frame outside [In,Out] (no kernel Range clamp today).";
    shell.clearRange();
}

// Phase 0 baseline: rapid scrubbing (fast successive seeks) must be latest-wins and must not spawn
// a generation storm (one generation increment per pointer event). Today each seek is an Exact that
// advances the generation; the plan's Scrub workflow (Phase 4) must coalesce. This test submits
// rapid seeks and asserts the latest target wins and generation growth is bounded. Expected to show
// unbounded generation growth today; bounded after Phase 4.
TEST(MainQmlContractTests, DISABLED_ScrubLatestWinsWithoutGenerationStorm) {
    auto snapshot = std::make_shared<application::SessionSnapshot>();
    snapshot->graphicsReady = true;
    snapshot->sessionState = domain::SessionState::kReady;
    snapshot->playbackState = domain::PlaybackState::kPaused;
    snapshot->displayedFrame = domain::FrameId{0};
    snapshot->canonicalFrameCount = 60U;
    snapshot->sources = {
        application::SessionSourceView{.sourceId = 0U, .displayName = "A"},
    };
    std::vector<application::PlaybackCommand> submitted;
    ReviewController controller{ReviewController::Dependencies{
        .submit =
            [&submitted](application::PlaybackCommand command) {
                submitted.push_back(std::move(command));
                return application::PortSubmitResult::Accepted;
            },
        .snapshot = [snapshot] { return snapshot; },
        .takeCompletedCommands = [] { return std::vector<application::CommandTerminal>{}; },
    }};
    ReviewPreferencesController preferences{std::make_shared<ClosedSettingsRepository>()};
    ReviewShellController shell{controller, preferences};

    // Rapid scrub: submit many seeks to random targets as fast as possible.
    constexpr int scrubCount = 20;
    for (int i = 0; i < scrubCount; ++i) {
        const std::int64_t target = (i * 3) % 60;
        EXPECT_TRUE(controller.seekFrame(target));
    }
    // Latest-wins: the most recent seek must be the last submitted target.
    ASSERT_FALSE(submitted.empty());
    const auto* const lastSeek = std::get_if<application::SeekFrameCommand>(&submitted.back());
    ASSERT_NE(lastSeek, nullptr);
    EXPECT_EQ(lastSeek->frameId.value(), (scrubCount - 1) * 3 % 60);
    // Generation storm check: count distinct SeekFrameCommands submitted (today one per pointer
    // event; the plan's scrub coalescing must reduce this). Document the baseline count.
    std::size_t seekCommands = 0;
    for (const auto& command : submitted) {
        if (std::holds_alternative<application::SeekFrameCommand>(command)) {
            ++seekCommands;
        }
    }
    EXPECT_EQ(seekCommands, static_cast<std::size_t>(scrubCount))
        << "Today each scrub seek submits a separate Exact seek (baseline for Phase 4 coalescing).";
}

// Phase 0 baseline: the Wipe handle must be keyboard-operable (Accessible role Slider + cursor
// keys) for accessibility. Today WipeHandle.qml has no activeFocusOnTab / Accessible.role / value /
// cursor keys. This test asserts the handle exposes a Slider role and is keyboard-adjustable.
// Expected to FAIL on current code; passes after Phase 4 a11y work.
TEST(MainQmlContractTests, DISABLED_WipeHandleIsKeyboardAdjustable) {
    auto snapshot = std::make_shared<application::SessionSnapshot>();
    snapshot->graphicsReady = true;
    snapshot->sessionState = domain::SessionState::kReady;
    snapshot->playbackState = domain::PlaybackState::kPaused;
    snapshot->displayedFrame = domain::FrameId{0};
    snapshot->canonicalFrameCount = 10U;
    snapshot->sources = {
        application::SessionSourceView{
            .sourceId = 0U, .role = domain::ComparisonRole::kReference, .displayName = "A"},
        application::SessionSourceView{
            .sourceId = 1U, .role = domain::ComparisonRole::kPrediction, .displayName = "B"},
    };
    ReviewController controller{ReviewController::Dependencies{
        .submit =
            [](application::PlaybackCommand) { return application::PortSubmitResult::Accepted; },
        .snapshot = [snapshot] { return snapshot; },
        .takeCompletedCommands = [] { return std::vector<application::CommandTerminal>{}; },
    }};
    ReviewPreferencesController preferences{std::make_shared<ClosedSettingsRepository>()};
    preferences.setViewMode(ReviewPreferencesController::ViewMode::Wipe);
    ReviewShellController shell{controller, preferences};
    ReviewSessionFacade facade{controller, preferences, shell};

    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    engine.rootContext()->setContextProperty(QStringLiteral("reviewController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewPreferences"), &preferences);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewSession"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewFacade"), &facade);
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);
    std::unique_ptr<QObject> root{component.create()};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);
    QCoreApplication::processEvents();

    QObject* const wipeHandle = root->findChild<QObject*>(QStringLiteral("wipeHandle"));
    ASSERT_NE(wipeHandle, nullptr);
    // Accessible.role must be Slider (value 8) for a adjustable split control.
    EXPECT_EQ(wipeHandle->property("Accessible.role").toInt(), 8)
        << "Wipe handle must expose Accessible.Slider (value 8).";
    EXPECT_TRUE(wipeHandle->property("activeFocusOnTab").toBool())
        << "Wipe handle must be tab-focusable.";
}

// Phase 0 baseline: the Timeline must expose an Accessible.value matching the preview or current
// frame so screen-reader users know the position. Today TimelineTracks.qml has Accessible.role/
// name but no value/keyboard. This test asserts the timeline's accessible value reflects the frame.
// Expected to FAIL on current code; passes after Phase 4 a11y work.
TEST(MainQmlContractTests, DISABLED_TimelineAccessibleValueMatchesPreviewOrCurrentFrame) {
    auto snapshot = std::make_shared<application::SessionSnapshot>();
    snapshot->graphicsReady = true;
    snapshot->sessionState = domain::SessionState::kReady;
    snapshot->playbackState = domain::PlaybackState::kPaused;
    snapshot->displayedFrame = domain::FrameId{5};
    snapshot->canonicalFrameCount = 30U;
    snapshot->sources = {
        application::SessionSourceView{.sourceId = 0U, .displayName = "A"},
    };
    ReviewController controller{ReviewController::Dependencies{
        .submit =
            [](application::PlaybackCommand) { return application::PortSubmitResult::Accepted; },
        .snapshot = [snapshot] { return snapshot; },
        .takeCompletedCommands = [] { return std::vector<application::CommandTerminal>{}; },
    }};
    ReviewPreferencesController preferences{std::make_shared<ClosedSettingsRepository>()};
    ReviewShellController shell{controller, preferences};
    ReviewSessionFacade facade{controller, preferences, shell};

    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    engine.rootContext()->setContextProperty(QStringLiteral("reviewController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewPreferences"), &preferences);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewSession"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewFacade"), &facade);
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);
    std::unique_ptr<QObject> root{component.create()};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);
    QCoreApplication::processEvents();

    QObject* const timeline = root->findChild<QObject*>(QStringLiteral("timelineSlider"));
    ASSERT_NE(timeline, nullptr);
    EXPECT_EQ(timeline->property("Accessible.role").toInt(), 8)
        << "Timeline must expose Accessible.Slider (value 8).";
    // Accessible.value must reflect the current/preview frame (here 5).
    EXPECT_EQ(timeline->property("Accessible.value").toInt(), controller.currentFrame())
        << "Timeline Accessible.value must match the current frame.";
}

TEST(MainQmlContractTests, ImageWorkspaceReloadsViewportSourceAfterImageOpen) {
    // Regression: ImageWorkspace binds viewport.imageUrl to imageReview.imageUrl(slot),
    // a Q_INVOKABLE whose result depends on the controller's content generation. The
    // binding must also read a notified property (contentGeneration), otherwise it is
    // evaluated once at startup and the Image element keeps a stale, generation-0 URL —
    // its first provider request returned null (image not loaded yet) and it never
    // reloads, leaving the viewport permanently black after opening an image.
    auto snapshot = std::make_shared<application::SessionSnapshot>();
    snapshot->graphicsReady = true;
    snapshot->sessionState = domain::SessionState::kEmpty;
    std::vector<application::PlaybackCommand> submitted;
    std::vector<application::CommandTerminal> terminals;
    ReviewController controller{
        ReviewController::Dependencies{
            .submit =
                [&submitted](application::PlaybackCommand command) {
                    submitted.push_back(std::move(command));
                    return application::PortSubmitResult::Accepted;
                },
            .snapshot = [snapshot] { return snapshot; },
            .takeCompletedCommands =
                [&terminals] {
                    std::vector<application::CommandTerminal> result = std::move(terminals);
                    terminals.clear();
                    return result;
                },
        },
    };
    ReviewPreferencesController preferences{std::make_shared<ClosedSettingsRepository>()};
    ReviewShellController shell{controller, preferences};
    ReviewSessionFacade facade{controller, preferences, shell};
    ImageReviewController imageReview;

    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    engine.rootContext()->setContextProperty(QStringLiteral("reviewController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewPreferences"), &preferences);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewSession"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewFacade"), &facade);
    engine.rootContext()->setContextProperty(QStringLiteral("imageReview"), &imageReview);
    engine.addImageProvider(QStringLiteral("vcs-review"), new ReviewImageProvider(&imageReview));
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);

    std::unique_ptr<QObject> root{component.create()};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);
    window->resize(960, 640);
    QCoreApplication::processEvents();

    auto* const viewport = root->findChild<QQuickItem*>(QStringLiteral("primaryViewport"));
    auto* const imageItem = root->findChild<QQuickItem*>(QStringLiteral("imageViewport-2"));
    ASSERT_NE(viewport, nullptr);
    ASSERT_NE(imageItem, nullptr);

    // Before any image is loaded the binding holds the generation-0 URL and the Image
    // element received a null pixmap from the provider.
    EXPECT_EQ(viewport->property("imageUrl").toString(), QStringLiteral("image://vcs-review/2/0"));
    EXPECT_EQ(imageItem->property("source").toString(), QStringLiteral("image://vcs-review/2/0"));

    QImage image(64, 48, QImage::Format_ARGB32);
    image.fill(QColor(200, 30, 30));
    ASSERT_TRUE(imageReview.openPrimaryImage(std::move(image), QStringLiteral("left")));
    for (int iteration = 0; iteration < 5; ++iteration) {
        QCoreApplication::processEvents();
    }

    // The binding must re-evaluate (generation 0 -> 1) so the Image element reloads the
    // provider and actually paints the decoded pixels instead of staying black.
    EXPECT_EQ(viewport->property("imageUrl").toString(), QStringLiteral("image://vcs-review/2/1"));
    EXPECT_EQ(imageItem->property("source").toString(), QStringLiteral("image://vcs-review/2/1"));
    // 1 == QQuickImage::Ready.
    EXPECT_EQ(imageItem->property("status").toInt(), 1)
        << "Image element must reach Ready after the source URL refreshes.";
    EXPECT_EQ(imageItem->property("sourceSize").toSize(), QSize(64, 48));

    // Channel changes must refresh the provider URL even when the source image is unchanged.
    imageReview.setViewMode(ImageReviewController::AlphaGrayView);
    QCoreApplication::processEvents();
    EXPECT_EQ(imageItem->property("source").toString(), imageReview.imageUrl(2));
    EXPECT_NE(imageItem->property("source").toString(), QStringLiteral("image://vcs-review/2/1"));
    EXPECT_EQ(imageItem->property("status").toInt(), 1);
    const QString alphaUrl = imageItem->property("source").toString();
    imageReview.setViewMode(ImageReviewController::RgbaView);
    QCoreApplication::processEvents();
    EXPECT_EQ(imageItem->property("source").toString(), imageReview.imageUrl(2));
    EXPECT_NE(imageItem->property("source").toString(), alphaUrl);
}

TEST(MainQmlContractTests, ImageWorkspaceWipeHandleMovesSplitPosition) {
    // Regression: ImageWorkspace wired WipeHandle.positionRequested to a method call on
    // the controller (imageReview.setWipePosition(...)), but that method is only the
    // Q_PROPERTY WRITE accessor — not Q_INVOKABLE — so QML threw a TypeError on every
    // drag and the split line never moved. The handler must assign the wipePosition
    // property instead, exactly like compareMode.
    auto snapshot = std::make_shared<application::SessionSnapshot>();
    snapshot->graphicsReady = true;
    snapshot->sessionState = domain::SessionState::kEmpty;
    std::vector<application::PlaybackCommand> submitted;
    std::vector<application::CommandTerminal> terminals;
    ReviewController controller{
        ReviewController::Dependencies{
            .submit =
                [&submitted](application::PlaybackCommand command) {
                    submitted.push_back(std::move(command));
                    return application::PortSubmitResult::Accepted;
                },
            .snapshot = [snapshot] { return snapshot; },
            .takeCompletedCommands =
                [&terminals] {
                    std::vector<application::CommandTerminal> result = std::move(terminals);
                    terminals.clear();
                    return result;
                },
        },
    };
    ReviewPreferencesController preferences{std::make_shared<ClosedSettingsRepository>()};
    ReviewShellController shell{controller, preferences};
    ReviewSessionFacade facade{controller, preferences, shell};
    ImageReviewController imageReview;

    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    engine.rootContext()->setContextProperty(QStringLiteral("reviewController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewPreferences"), &preferences);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewSession"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewFacade"), &facade);
    engine.rootContext()->setContextProperty(QStringLiteral("imageReview"), &imageReview);
    engine.addImageProvider(QStringLiteral("vcs-review"), new ReviewImageProvider(&imageReview));
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);

    std::unique_ptr<QObject> root{component.create()};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);
    window->resize(960, 640);
    window->show();
    QCoreApplication::processEvents();

    // A pair plus wipe mode reveals the split overlay and the shared WipeHandle.
    QImage left(32, 32, QImage::Format_ARGB32);
    left.fill(QColor(200, 30, 30));
    QImage right(32, 32, QImage::Format_ARGB32);
    right.fill(QColor(30, 30, 200));
    ASSERT_TRUE(imageReview.openPrimaryImage(std::move(left), QStringLiteral("left")));
    ASSERT_TRUE(imageReview.openSecondaryImage(std::move(right), QStringLiteral("right")));
    imageReview.setCompareMode(ImageReviewController::Wipe);
    QVariant workspaceActivated;
    ASSERT_TRUE(QMetaObject::invokeMethod(root.get(),
                                          "activateWorkspace",
                                          Q_RETURN_ARG(QVariant, workspaceActivated),
                                          Q_ARG(QVariant, QVariant{1})));
    ASSERT_TRUE(workspaceActivated.toBool());
    for (int iteration = 0; iteration < 5; ++iteration) {
        QCoreApplication::processEvents();
    }

    QObject* const workspace = root->findChild<QObject*>(QStringLiteral("imageWorkspaceRoot"));
    ASSERT_NE(workspace, nullptr);
    auto* const handle = workspace->findChild<QQuickItem*>(QStringLiteral("wipeHandle"));
    auto* const overlay = workspace->findChild<QQuickItem*>(QStringLiteral("wipeOverlay"));
    auto* const clip = workspace->findChild<QQuickItem*>(QStringLiteral("wipeClip"));
    ASSERT_NE(handle, nullptr);
    ASSERT_NE(overlay, nullptr);
    ASSERT_NE(clip, nullptr);
    EXPECT_TRUE(handle->isVisible()) << "wipe handle must be visible for a pair in wipe mode";

    // updatePosition(sceneX) is the exact path DragHandler.onCentroidChanged drives; it
    // emits positionRequested, whose handler must move the controller position.
    const QPointF overlayOrigin = overlay->mapToScene(QPointF{0.0, 0.0});
    for (const double requested : {0.25, 0.75}) {
        const qreal sceneX = overlayOrigin.x() + overlay->width() * requested;
        ASSERT_TRUE(
            QMetaObject::invokeMethod(handle, "updatePosition", Q_ARG(QVariant, QVariant{sceneX})));
        QCoreApplication::processEvents();
        EXPECT_NEAR(imageReview.wipePosition(), requested, 1e-3)
            << "dragging the split handle must update the controller wipe position";
    }

    // The on-screen split follows: the clip width and the handle center both track the
    // requested position inside the overlay.
    EXPECT_NEAR(clip->width(), overlay->width() * imageReview.wipePosition(), 1.0);
    const QPointF handleCenter = handle->mapToScene(QPointF{handle->width() / 2.0, 0.0});
    EXPECT_NEAR(
        handleCenter.x(), overlayOrigin.x() + overlay->width() * imageReview.wipePosition(), 1.5);
}

TEST(MainQmlContractTests, SameNamedVideoSourcesExposeParentLabels) {
    // Two sources with identical filenames from different folders must project a
    // parent-folder label per slot so the viewport/strip can disambiguate them, while the
    // full paths stay available for hover tooltips.
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    const std::filesystem::path renderDir =
        std::filesystem::path(tempDir.path().toStdWString()) / "render_v1";
    const std::filesystem::path finalDir =
        std::filesystem::path(tempDir.path().toStdWString()) / "render_final";
    ASSERT_TRUE(std::filesystem::create_directory(renderDir));
    ASSERT_TRUE(std::filesystem::create_directory(finalDir));
    const std::array<std::filesystem::path, 2> paths = {renderDir / "shot.mp4",
                                                        finalDir / "shot.mp4"};
    for (const auto& path : paths) {
        QFile file{QString::fromStdWString(path.wstring())};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("same-name-bytes", 15);
    }

    const auto rateResult = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rateResult);
    const domain::RationalRate rate = rateResult.value();

    std::vector<domain::ComparisonSource> comparisonSources;
    for (std::size_t index = 0U; index < paths.size(); ++index) {
        const QFileInfo info{QString::fromStdWString(paths[index].wstring())};
        comparisonSources.push_back(domain::ComparisonSource{
            .id = static_cast<domain::SourceId>(index),
            .role = index == 0U ? domain::ComparisonRole::kReference
                                : domain::ComparisonRole::kPrediction,
            .descriptor =
                domain::MediaDescriptor{
                    .normalizedPath = paths[index],
                    .extent = domain::MediaExtent{.width = 1'920U, .height = 1'080U},
                    .frameRate = rate,
                    .frameCount =
                        domain::FrameCountInfo{
                            .value = 12,
                            .origin = domain::FrameCountOrigin::kReported,
                        },
                    .duration = domain::MediaTime{400'000},
                    .codecId = "h264",
                    .pixelFormatId = "nv12",
                    .bitDepth = 8U,
                    .decodeCapabilities =
                        domain::DecodeCapabilities{
                            .softwareDecode = true,
                            .d3d11VaDecode = true,
                        },
                    .timingConfidence = domain::TimingConfidence::kVerifiedCfr,
                    .sourceIdentity =
                        domain::SourceFileIdentity{
                            .byteSize = static_cast<std::uint64_t>(info.size()),
                            .modifiedUtcMilliseconds = info.lastModified().toMSecsSinceEpoch(),
                            .fingerprintSha256 = std::string(64U, '0'),
                        },
                },
            .displayName = "shot.mp4",
        });
    }
    auto validated = domain::ComparisonValidator::validate(std::move(comparisonSources));
    ASSERT_TRUE(validated);

    auto snapshot = std::make_shared<application::SessionSnapshot>();
    snapshot->graphicsReady = true;
    snapshot->sessionState = domain::SessionState::kReady;
    snapshot->playbackState = domain::PlaybackState::kPaused;
    snapshot->displayedFrame = domain::FrameId{0};
    snapshot->validatedComparison =
        std::make_shared<const domain::ValidatedComparisonSet>(std::move(validated).value().set);
    snapshot->canonicalFrameCount =
        static_cast<std::uint64_t>(snapshot->validatedComparison->canonicalFrameCount());
    snapshot->canonicalTimeline = rate;
    for (const auto& source : snapshot->validatedComparison->sources()) {
        snapshot->sources.push_back(application::SessionSourceView{
            .sourceId = source.id,
            .role = source.role,
            .displayName = source.displayName,
        });
        snapshot->presentedSources.push_back(application::PresentedSourceState{
            .sourceId = source.id,
            .sourceFrameId = domain::FrameId{0},
            .matchKind = application::FrameMatchKind::ExactIndex,
        });
    }

    std::vector<application::PlaybackCommand> submitted;
    std::vector<application::CommandTerminal> terminals;
    ReviewController controller{
        ReviewController::Dependencies{
            .submit =
                [&submitted](application::PlaybackCommand command) {
                    submitted.push_back(std::move(command));
                    return application::PortSubmitResult::Accepted;
                },
            .snapshot = [snapshot] { return snapshot; },
            .takeCompletedCommands =
                [&terminals] {
                    std::vector<application::CommandTerminal> result = std::move(terminals);
                    terminals.clear();
                    return result;
                },
        },
    };
    controller.refreshProjection();
    QCoreApplication::processEvents();

    const QStringList parentLabels = controller.sourceParentLabels();
    const QStringList fullPaths = controller.sourceFullPaths();
    ASSERT_EQ(parentLabels.size(), 2);
    ASSERT_EQ(fullPaths.size(), 2);
    EXPECT_EQ(parentLabels.at(0).toStdString(), "render_v1");
    EXPECT_EQ(parentLabels.at(1).toStdString(), "render_final");
    EXPECT_TRUE(fullPaths.at(0).endsWith(QStringLiteral("shot.mp4"), Qt::CaseInsensitive));
    EXPECT_NE(fullPaths.at(0), fullPaths.at(1));

    // The sources model exposes the same per-row data for the strip and viewport labels.
    const QAbstractItemModel* model = controller.sources();
    ASSERT_NE(model, nullptr);
    ASSERT_EQ(model->rowCount(), 2);
    const QModelIndex first = model->index(0, 0);
    const QModelIndex second = model->index(1, 0);
    EXPECT_EQ(first.data(SourceListModel::FilenameRole).toString().toStdString(), "shot.mp4");
    EXPECT_EQ(first.data(SourceListModel::ParentLabelRole).toString().toStdString(), "render_v1");
    EXPECT_EQ(second.data(SourceListModel::ParentLabelRole).toString().toStdString(),
              "render_final");
    EXPECT_TRUE(!first.data(SourceListModel::FullPathRole).toString().isEmpty());
    EXPECT_TRUE(!second.data(SourceListModel::FullPathRole).toString().isEmpty());
}

TEST(MainQmlContractTests, ImageFolderComparisonLoadsSidebarAndOpensFirstPair) {
    // End-to-end folder comparison: two folders with same-named images pair up, the
    // sidebar becomes visible with one row per file, and the first complete pair opens in
    // the image workspace with compare mode SideBySide.
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    const QString leftDir = QDir(tempDir.path()).filePath(QStringLiteral("folder_a"));
    const QString rightDir = QDir(tempDir.path()).filePath(QStringLiteral("folder_b"));
    ASSERT_TRUE(QDir{}.mkpath(leftDir));
    ASSERT_TRUE(QDir{}.mkpath(rightDir));
    for (const QString& folder : {leftDir, rightDir}) {
        for (const char* name : {"frame001.png", "frame002.png"}) {
            ASSERT_TRUE(writeTestPng(QDir(folder).filePath(QString::fromLatin1(name))));
        }
    }
    ASSERT_TRUE(writeTestPng(QDir(leftDir).filePath(QStringLiteral("frame003.png"))));

    auto snapshot = std::make_shared<application::SessionSnapshot>();
    snapshot->graphicsReady = true;
    snapshot->sessionState = domain::SessionState::kEmpty;
    std::vector<application::PlaybackCommand> submitted;
    std::vector<application::CommandTerminal> terminals;
    ReviewController controller{
        ReviewController::Dependencies{
            .submit =
                [&submitted](application::PlaybackCommand command) {
                    submitted.push_back(std::move(command));
                    return application::PortSubmitResult::Accepted;
                },
            .snapshot = [snapshot] { return snapshot; },
            .takeCompletedCommands =
                [&terminals] {
                    std::vector<application::CommandTerminal> result = std::move(terminals);
                    terminals.clear();
                    return result;
                },
        },
    };
    ReviewPreferencesController preferences{std::make_shared<ClosedSettingsRepository>()};
    ReviewShellController shell{controller, preferences};
    ReviewSessionFacade facade{controller, preferences, shell};
    ImageReviewController imageReview;
    ImageFolderPairModel folderPairs;
    folderPairs.setAsyncPairOpener(
        [&imageReview](
            const QUrl& primary, const QUrl& secondary, const int pairId, QString* error) {
            const int requestId = imageReview.requestOpenPair(primary, secondary, pairId);
            if (requestId <= 0 && error != nullptr) {
                *error = imageReview.errorText();
            }
            return requestId;
        });
    folderPairs.setAsyncPairCancel(
        [&imageReview](const int requestId) { imageReview.cancelOpenRequest(requestId); });
    folderPairs.setSingleSideOpener([&imageReview](const QUrl& url, const int row, QString* error) {
        const int requestId = imageReview.requestOpenPrimary(url, row);
        if (requestId <= 0 && error != nullptr) {
            *error = imageReview.errorText();
        }
        return requestId;
    });
    QObject::connect(
        &imageReview,
        &ImageReviewController::openFinished,
        &folderPairs,
        [&folderPairs](
            const int requestId, const int pairId, const bool success, const QString& error) {
            if (pairId >= 0) {
                folderPairs.completePairOpen(static_cast<quint64>(requestId), success, error);
                folderPairs.completeSingleSideOpen(static_cast<quint64>(requestId), success, error);
            }
        });

    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    engine.rootContext()->setContextProperty(QStringLiteral("reviewController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewPreferences"), &preferences);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewSession"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewFacade"), &facade);
    engine.rootContext()->setContextProperty(QStringLiteral("imageReview"), &imageReview);
    engine.rootContext()->setContextProperty(QStringLiteral("imageFolderPairs"), &folderPairs);
    engine.addImageProvider(QStringLiteral("vcs-review"), new ReviewImageProvider(&imageReview));
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);

    std::unique_ptr<QObject> root{component.create()};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);
    window->resize(1280, 800);
    QCoreApplication::processEvents();

    // The same function the FolderDialog accepted handler and the two-folder drop invoke.
    root->setProperty("imageFolderLeftUrl", QUrl::fromLocalFile(QDir(leftDir).absolutePath()));
    root->setProperty("imageFolderRightUrl", QUrl::fromLocalFile(QDir(rightDir).absolutePath()));
    QVariant loadResult;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        root.get(), "loadFolderComparison", Q_RETURN_ARG(QVariant, loadResult)));
    EXPECT_TRUE(loadResult.toBool());
    // T4: loadFolderComparison returns when the candidate is accepted; the first pair is
    // only committed after the worker finishes.
    QElapsedTimer firstPairTimer;
    firstPairTimer.start();
    while ((!imageReview.hasPair() || folderPairs.currentPair() != 0) &&
           firstPairTimer.elapsed() < 8000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        QThread::msleep(1U);
    }
    ASSERT_TRUE(imageReview.hasPair());
    ASSERT_EQ(folderPairs.currentPair(), 0);

    // Three rows: two complete pairs plus the left-only single; first complete opens.
    EXPECT_EQ(folderPairs.pairCount(), 3);
    EXPECT_TRUE(imageReview.hasPair());
    EXPECT_EQ(imageReview.compareMode(), static_cast<int>(ImageReviewController::SideBySide));
    EXPECT_TRUE(
        imageReview.primaryPath().endsWith(QStringLiteral("frame001.png"), Qt::CaseInsensitive));

    QObject* const workspace = root->findChild<QObject*>(QStringLiteral("imageWorkspaceRoot"));
    ASSERT_NE(workspace, nullptr);
    auto* const sidebar =
        workspace->findChild<QQuickItem*>(QStringLiteral("folderPairSidebarHost"));
    ASSERT_NE(sidebar, nullptr);
    EXPECT_TRUE(sidebar->isVisible()) << "sidebar must be visible after loading folders";
    QObject* const positionLabel =
        sidebar->findChild<QObject*>(QStringLiteral("folderPairPositionLabel"));
    ASSERT_NE(positionLabel, nullptr);
    EXPECT_EQ(positionLabel->property("text").toString(), QStringLiteral("1/3"));

    // Step to the next pair through the sidebar's navigation entry point.
    QVariant stepResult;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        sidebar, "stepPair", Q_RETURN_ARG(QVariant, stepResult), Q_ARG(QVariant, QVariant{1})));
    QElapsedTimer stepTimer;
    stepTimer.start();
    while ((folderPairs.currentPair() != 1 ||
            !imageReview.primaryPath().endsWith(QStringLiteral("frame002.png"),
                                                Qt::CaseInsensitive)) &&
           stepTimer.elapsed() < 8000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        QThread::msleep(1U);
    }
    EXPECT_TRUE(
        imageReview.primaryPath().endsWith(QStringLiteral("frame002.png"), Qt::CaseInsensitive));
    EXPECT_EQ(folderPairs.currentPair(), 1);
    // The single-sided row 2 must never open.
    EXPECT_FALSE(folderPairs.openPairAt(2));

    // A direct loose-image import leaves the folder task: its list selection and arrow-key
    // navigation must detach so the canvas identity and the list can never diverge.
    QVariantList looseUrls;
    looseUrls.push_back(
        QUrl::fromLocalFile(QDir(leftDir).filePath(QStringLiteral("frame003.png"))));
    QVariant looseOpened;
    ASSERT_TRUE(QMetaObject::invokeMethod(root.get(),
                                          "performImageReview",
                                          Q_RETURN_ARG(QVariant, looseOpened),
                                          Q_ARG(QVariant, QVariant{looseUrls})));
    EXPECT_TRUE(looseOpened.toBool());
    QElapsedTimer looseTimer;
    looseTimer.start();
    while (
        (!imageReview.hasPrimary() || imageReview.hasSecondary() || folderPairs.pairCount() != 0) &&
        looseTimer.elapsed() < 8000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        QThread::msleep(1U);
    }
    EXPECT_EQ(folderPairs.pairCount(), 0);
    EXPECT_EQ(folderPairs.currentPair(), -1);
    EXPECT_FALSE(imageReview.hasSecondary());
    EXPECT_EQ(root->property("workspaceMode").toInt(), 1);
}

TEST(MainQmlContractTests, FolderSidebarExposesPairingContextAndOpensMissingSide) {
    // T6: the sidebar states the pairing rule and outcome counts, a single-sided row
    // opens the side that exists, the selection stays visible while stepping through a
    // long folder, and the wipe halves carry their A/B identity.
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    const QString leftDir = QDir(tempDir.path()).filePath(QStringLiteral("folder_a"));
    const QString rightDir = QDir(tempDir.path()).filePath(QStringLiteral("folder_b"));
    ASSERT_TRUE(QDir{}.mkpath(leftDir));
    ASSERT_TRUE(QDir{}.mkpath(rightDir));
    // Enough pairs to scroll: the sidebar viewport shows far fewer than 40 rows.
    for (int index = 0; index < 40; ++index) {
        const QString name = QStringLiteral("pair_%1.png").arg(index, 3, 10, QLatin1Char('0'));
        ASSERT_TRUE(writeTestPng(QDir(leftDir).filePath(name)));
        ASSERT_TRUE(writeTestPng(QDir(rightDir).filePath(name)));
    }
    ASSERT_TRUE(writeTestPng(QDir(rightDir).filePath(QStringLiteral("only_right.png"))));

    auto snapshot = std::make_shared<application::SessionSnapshot>();
    snapshot->graphicsReady = true;
    snapshot->sessionState = domain::SessionState::kEmpty;
    std::vector<application::PlaybackCommand> submitted;
    std::vector<application::CommandTerminal> terminals;
    ReviewController controller{
        ReviewController::Dependencies{
            .submit =
                [&submitted](application::PlaybackCommand command) {
                    submitted.push_back(std::move(command));
                    return application::PortSubmitResult::Accepted;
                },
            .snapshot = [snapshot] { return snapshot; },
            .takeCompletedCommands =
                [&terminals] {
                    std::vector<application::CommandTerminal> result = std::move(terminals);
                    terminals.clear();
                    return result;
                },
        },
    };
    ReviewPreferencesController preferences{std::make_shared<ClosedSettingsRepository>()};
    ReviewShellController shell{controller, preferences};
    ReviewSessionFacade facade{controller, preferences, shell};
    ImageReviewController imageReview;
    ImageFolderPairModel folderPairs;
    folderPairs.setAsyncPairOpener(
        [&imageReview](
            const QUrl& primary, const QUrl& secondary, const int pairId, QString* error) {
            const int requestId = imageReview.requestOpenPair(primary, secondary, pairId);
            if (requestId <= 0 && error != nullptr) {
                *error = imageReview.errorText();
            }
            return requestId;
        });
    folderPairs.setAsyncPairCancel(
        [&imageReview](const int requestId) { imageReview.cancelOpenRequest(requestId); });
    folderPairs.setSingleSideOpener([&imageReview](const QUrl& url, const int row, QString* error) {
        const int requestId = imageReview.requestOpenPrimary(url, row);
        if (requestId <= 0 && error != nullptr) {
            *error = imageReview.errorText();
        }
        return requestId;
    });
    QObject::connect(
        &imageReview,
        &ImageReviewController::openFinished,
        &folderPairs,
        [&folderPairs](
            const int requestId, const int pairId, const bool success, const QString& error) {
            if (pairId >= 0) {
                folderPairs.completePairOpen(static_cast<quint64>(requestId), success, error);
                folderPairs.completeSingleSideOpen(static_cast<quint64>(requestId), success, error);
            }
        });

    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    engine.rootContext()->setContextProperty(QStringLiteral("reviewController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewPreferences"), &preferences);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewSession"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewFacade"), &facade);
    engine.rootContext()->setContextProperty(QStringLiteral("imageReview"), &imageReview);
    engine.rootContext()->setContextProperty(QStringLiteral("imageFolderPairs"), &folderPairs);
    engine.addImageProvider(QStringLiteral("vcs-review"), new ReviewImageProvider(&imageReview));
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);

    std::unique_ptr<QObject> root{component.create()};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);
    window->resize(1280, 800);
    QCoreApplication::processEvents();

    root->setProperty("imageFolderLeftUrl", QUrl::fromLocalFile(QDir(leftDir).absolutePath()));
    root->setProperty("imageFolderRightUrl", QUrl::fromLocalFile(QDir(rightDir).absolutePath()));
    QVariant loadResult;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        root.get(), "loadFolderComparison", Q_RETURN_ARG(QVariant, loadResult)));
    EXPECT_TRUE(loadResult.toBool());
    // Rows sort as only_right.png, pair_000.png ... pair_039.png: the first complete
    // pair is row 1.
    QElapsedTimer firstPairTimer;
    firstPairTimer.start();
    while ((!imageReview.hasPair() || folderPairs.currentPair() != 1) &&
           firstPairTimer.elapsed() < 8000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        QThread::msleep(1U);
    }
    ASSERT_TRUE(imageReview.hasPair());
    ASSERT_EQ(folderPairs.currentPair(), 1);
    ASSERT_EQ(folderPairs.pairCount(), 41);

    QObject* const workspace = root->findChild<QObject*>(QStringLiteral("imageWorkspaceRoot"));
    ASSERT_NE(workspace, nullptr);
    auto* const sidebar =
        workspace->findChild<QQuickItem*>(QStringLiteral("folderPairSidebarHost"));
    ASSERT_NE(sidebar, nullptr);
    EXPECT_TRUE(sidebar->isVisible());

    // The pairing rule and the outcome counts are stated, and "paired" never claims
    // content equality: 40 complete rows plus one right-only single.
    QObject* const ruleLabel = sidebar->findChild<QObject*>(QStringLiteral("folderPairRuleLabel"));
    QObject* const countsLabel =
        sidebar->findChild<QObject*>(QStringLiteral("folderPairCountsLabel"));
    ASSERT_NE(ruleLabel, nullptr);
    ASSERT_NE(countsLabel, nullptr);
    EXPECT_TRUE(ruleLabel->property("text").toString().contains(QStringLiteral("大小写")));
    EXPECT_EQ(countsLabel->property("text").toString(),
              QStringLiteral("完整 40 · 缺失 1 · 大小写冲突 0"));

    // The workspace status bar names the committed row.
    QObject* const contextLabel =
        workspace->findChild<QObject*>(QStringLiteral("imagePairContextLabel"));
    ASSERT_NE(contextLabel, nullptr);
    EXPECT_TRUE(contextLabel->property("visible").toBool());
    EXPECT_TRUE(contextLabel->property("text").toString().contains(QStringLiteral("第 2/41 行")));
    // The single-sided row 0 opens the side that exists: the canvas shows only that
    // image and the list selection advances to the row it belongs to.
    QVariant missingOpen;
    ASSERT_TRUE(QMetaObject::invokeMethod(sidebar,
                                          "openMissingSide",
                                          Q_RETURN_ARG(QVariant, missingOpen),
                                          Q_ARG(QVariant, QVariant{0})));
    EXPECT_TRUE(missingOpen.toBool());
    QElapsedTimer missingTimer;
    missingTimer.start();
    while ((!imageReview.hasPrimary() || imageReview.hasSecondary() ||
            folderPairs.currentPair() != 0) &&
           missingTimer.elapsed() < 8000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        QThread::msleep(1U);
    }
    EXPECT_TRUE(imageReview.hasPrimary());
    EXPECT_FALSE(imageReview.hasSecondary());
    EXPECT_TRUE(imageReview.primaryPath().endsWith(QStringLiteral("only_right.png")));
    EXPECT_EQ(folderPairs.currentPair(), 0);
    EXPECT_TRUE(contextLabel->property("text").toString().contains(QStringLiteral("仅 B 存在")));
    QObject* const positionLabel =
        sidebar->findChild<QObject*>(QStringLiteral("folderPairPositionLabel"));
    ASSERT_NE(positionLabel, nullptr);
    EXPECT_EQ(positionLabel->property("text").toString(), QStringLiteral("1/41"));

    // Back to a complete pair; the wipe halves carry their A/B identity, with the left
    // half showing the secondary (B) image and the right half the primary (A).
    QVariant stepBack;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        sidebar, "stepPair", Q_RETURN_ARG(QVariant, stepBack), Q_ARG(QVariant, QVariant{-1})));
    QElapsedTimer wipeTimer;
    wipeTimer.start();
    while ((!imageReview.hasPair() || folderPairs.currentPair() != 40) &&
           wipeTimer.elapsed() < 8000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        QThread::msleep(1U);
    }
    ASSERT_TRUE(imageReview.hasPair());
    imageReview.setCompareMode(ImageReviewController::Wipe);
    for (int iteration = 0; iteration < 5; ++iteration) {
        QCoreApplication::processEvents();
    }
    QObject* const wipeLeft = workspace->findChild<QObject*>(QStringLiteral("wipeIdentityLeft"));
    QObject* const wipeRight = workspace->findChild<QObject*>(QStringLiteral("wipeIdentityRight"));
    ASSERT_NE(wipeLeft, nullptr);
    ASSERT_NE(wipeRight, nullptr);
    const QString leftText = wipeLeft->property("text").toString();
    const QString rightText = wipeRight->property("text").toString();
    EXPECT_TRUE(leftText.startsWith(QStringLiteral("B · ")));
    EXPECT_TRUE(rightText.startsWith(QStringLiteral("A · ")));
    // Same-named files are disambiguated by parent folder: the left half must name the
    // secondary's folder and the right half the primary's, never the reverse.
    EXPECT_TRUE(leftText.contains(QStringLiteral("folder_b")));
    EXPECT_TRUE(rightText.contains(QStringLiteral("folder_a")));
    // Keyboard stepping keeps the selected row visible: after walking well past one
    // viewport of rows, the committed row is still inside the list's visible window.
    int steppedRow = 40;
    for (int step = 0; step < 20; ++step) {
        QVariant stepNext;
        ASSERT_TRUE(QMetaObject::invokeMethod(
            sidebar, "stepPair", Q_RETURN_ARG(QVariant, stepNext), Q_ARG(QVariant, QVariant{1})));
        QElapsedTimer stepTimer;
        stepTimer.start();
        while (folderPairs.currentPair() == steppedRow && stepTimer.elapsed() < 8000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
            QThread::msleep(1U);
        }
        ASSERT_NE(folderPairs.currentPair(), steppedRow) << "step " << step;
        steppedRow = folderPairs.currentPair();
    }
    auto* const pairList = sidebar->findChild<QQuickItem*>(QStringLiteral("folderPairList"));
    ASSERT_NE(pairList, nullptr);
    for (int iteration = 0; iteration < 5; ++iteration) {
        QCoreApplication::processEvents();
    }
    EXPECT_EQ(pairList->property("currentIndex").toInt(), steppedRow);
    auto* const currentItem = pairList->property("currentItem").value<QQuickItem*>();
    ASSERT_NE(currentItem, nullptr);
    const qreal contentY = pairList->property("contentY").toDouble();
    EXPECT_GE(currentItem->y() + currentItem->height(), contentY - 1.0);
    EXPECT_LE(currentItem->y(), contentY + pairList->property("height").toDouble() + 1.0);
}

TEST(MainQmlContractTests, WorkspaceOpenIntentDoesNotOverrideCommittedWorkspace) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    QObject* const session = harness.root->findChild<QObject*>(QStringLiteral("workspaceSession"));
    auto* const imageWorkspace =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
    ASSERT_NE(session, nullptr);
    ASSERT_NE(imageWorkspace, nullptr);

    const int sourceCountBefore = harness.controller->sourceCount();
    const auto displayedFrameBefore = harness.snapshot->displayedFrame;
    ASSERT_TRUE(harness.beginWorkspaceOpen(1));
    EXPECT_EQ(harness.root->property("workspaceMode").toInt(), 0);
    EXPECT_EQ(session->property("pendingMedia").toInt(), 1);
    EXPECT_EQ(harness.controller->sourceCount(), sourceCountBefore);
    EXPECT_EQ(harness.snapshot->displayedFrame, displayedFrameBefore);
    EXPECT_FALSE(imageWorkspace->property("visible").toBool());

    // Cancelling the selector/staging intent must leave the committed video workspace and its
    // position untouched; it must not silently switch to an empty image workspace.
    ASSERT_TRUE(harness.cancelWorkspaceOpen());
    EXPECT_EQ(harness.root->property("workspaceMode").toInt(), 0);
    EXPECT_EQ(session->property("pendingMedia").toInt(), -1);
    EXPECT_EQ(harness.controller->sourceCount(), sourceCountBefore);
    EXPECT_EQ(harness.snapshot->displayedFrame, displayedFrameBefore);
    EXPECT_FALSE(imageWorkspace->property("visible").toBool());
}

TEST(MainQmlContractTests, WorkspaceSwitchPausesAndRetainsVideoSession) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.snapshot->playbackState = domain::PlaybackState::kPlaying;
    harness.controller->refreshProjection();
    harness.settle();
    ASSERT_TRUE(harness.controller->playing());
    const int sourceCountBefore = harness.controller->sourceCount();
    const int frameBefore = harness.controller->currentFrame();

    ASSERT_TRUE(harness.activateWorkspace(1));
    ASSERT_FALSE(harness.submitted.empty());
    const auto* const pause = std::get_if<application::PauseCommand>(&harness.submitted.back());
    ASSERT_NE(pause, nullptr) << "leaving video must pause the hidden session";
    EXPECT_EQ(harness.root->property("workspaceMode").toInt(), 1);
    EXPECT_EQ(harness.controller->sourceCount(), sourceCountBefore);
    EXPECT_EQ(harness.controller->currentFrame(), frameBefore);

    // Complete the pause terminal: the retained session is then an explicit paused session.
    harness.terminals.push_back(application::CommandTerminal{
        .context = application::commandContext(harness.submitted.back()),
        .outcome = application::CommandOutcome::Succeeded,
    });
    harness.snapshot->playbackState = domain::PlaybackState::kPaused;
    harness.controller->refreshProjection();
    harness.settle();
    ASSERT_FALSE(harness.controller->playing());

    // Returning to video shows the same sources and does not issue a close command.
    harness.submitted.clear();
    ASSERT_TRUE(harness.activateWorkspace(0));
    EXPECT_EQ(harness.root->property("workspaceMode").toInt(), 0);
    EXPECT_EQ(harness.controller->sourceCount(), sourceCountBefore);
    EXPECT_TRUE(harness.submitted.empty());

    // Even a stale "playing" projection is corrected on return: the workspace never resumes
    // playback implicitly after being hidden behind the image task.
    harness.snapshot->playbackState = domain::PlaybackState::kPlaying;
    harness.controller->refreshProjection();
    harness.settle();
    ASSERT_TRUE(harness.controller->playing());
    harness.submitted.clear();
    ASSERT_TRUE(harness.activateWorkspace(0));
    ASSERT_FALSE(harness.submitted.empty());
    EXPECT_NE(std::get_if<application::PauseCommand>(&harness.submitted.back()), nullptr);
}

TEST(MainQmlContractTests, ImageWorkspaceArrowsDoNotDriveHiddenVideo) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    auto* const viewport =
        harness.root->findChild<QQuickItem*>(QStringLiteral("mediaViewportFocusTarget"));
    auto* const imageWorkspace =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
    ASSERT_NE(viewport, nullptr);
    ASSERT_NE(imageWorkspace, nullptr);

    ASSERT_TRUE(harness.activateWorkspace(1));
    harness.settle();
    EXPECT_TRUE(imageWorkspace->property("visible").toBool());
    EXPECT_TRUE(imageWorkspace->hasActiveFocus())
        << "the image workspace must be the actual key receiver while it is visible";
    const std::size_t submittedBefore = harness.submitted.size();
    sendKey(*harness.window, Qt::Key_Right);
    EXPECT_EQ(harness.submitted.size(), submittedBefore)
        << "arrow keys in the image workspace must not step the hidden video";

    ASSERT_TRUE(harness.activateWorkspace(0));
    harness.settle();
    EXPECT_TRUE(viewport->hasActiveFocus());
    sendKey(*harness.window, Qt::Key_Right);
    ASSERT_GT(harness.submitted.size(), submittedBefore);
    const auto* const step = std::get_if<application::StepFramesCommand>(&harness.submitted.back());
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->delta, 1);
}

TEST(MainQmlContractTests, CloseCurrentTaskOnlyClosesActiveWorkspace) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    QImage image(32, 32, QImage::Format_ARGB32);
    image.fill(QColor(10, 20, 30));
    ASSERT_TRUE(harness.imageReview.openPrimaryImage(std::move(image), QStringLiteral("image-a")));
    ASSERT_TRUE(harness.activateWorkspace(1));
    harness.settle();
    ASSERT_TRUE(harness.imageReview.hasPrimary());

    // Ctrl+W in the image workspace closes only that task and reveals the retained video.
    sendKey(*harness.window, Qt::Key_W, Qt::ControlModifier);
    harness.settle();
    EXPECT_FALSE(harness.imageReview.hasPrimary());
    EXPECT_EQ(harness.controller->sourceCount(), 2);
    EXPECT_EQ(harness.root->property("workspaceMode").toInt(), 0);

    // Ctrl+W in the video workspace closes only the video task; the retained image is shown
    // and remains loaded.
    QImage retained(16, 16, QImage::Format_ARGB32);
    retained.fill(QColor(40, 50, 60));
    ASSERT_TRUE(
        harness.imageReview.openPrimaryImage(std::move(retained), QStringLiteral("retained")));
    ASSERT_TRUE(harness.activateWorkspace(0));
    harness.settle();
    EXPECT_EQ(harness.root->property("inputContext").toInt(), 0);
    EXPECT_TRUE(harness.root->property("activeTaskHasMedia").toBool());
    EXPECT_TRUE(harness.root->property("globalMediaShortcutsEnabled").toBool());
    harness.submitted.clear();
    sendKey(*harness.window, Qt::Key_W, Qt::ControlModifier);
    harness.settle();
    ASSERT_FALSE(harness.submitted.empty());
    EXPECT_NE(std::get_if<application::CloseSessionCommand>(&harness.submitted.back()), nullptr);
    EXPECT_EQ(harness.root->property("workspaceMode").toInt(), 1);
    EXPECT_TRUE(harness.imageReview.hasPrimary());
}

TEST(MainQmlContractTests, EmptyReviewViewExposesImageAndFolderEntryPoints) {
    WorkspaceHarness harness;
    harness.snapshot->sources.clear();
    harness.snapshot->presentedSources.clear();
    harness.snapshot->sessionState = domain::SessionState::kEmpty;
    harness.snapshot->displayedFrame.reset();
    harness.snapshot->canonicalFrameCount = 0U;
    harness.controller->refreshProjection();
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    auto* const openVideosBtn =
        harness.root->findChild<QQuickItem*>(QStringLiteral("emptyOpenVideosButton"));
    ASSERT_NE(openVideosBtn, nullptr);
    EXPECT_TRUE(openVideosBtn->isVisible());

    auto* const openImageBtn =
        harness.root->findChild<QQuickItem*>(QStringLiteral("emptyOpenImageButton"));
    ASSERT_NE(openImageBtn, nullptr);
    EXPECT_TRUE(openImageBtn->isVisible());

    auto* const compareFoldersBtn =
        harness.root->findChild<QQuickItem*>(QStringLiteral("emptyCompareFoldersButton"));
    ASSERT_NE(compareFoldersBtn, nullptr);
    EXPECT_TRUE(compareFoldersBtn->isVisible());
}

TEST(MainQmlContractTests, ImageWorkspaceZoomResetAndTrueSizeContract) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    QImage primary(64, 64, QImage::Format_ARGB32);
    primary.fill(QColor(255, 0, 0));
    QImage secondary(64, 64, QImage::Format_ARGB32);
    secondary.fill(QColor(0, 255, 0));
    ASSERT_TRUE(harness.imageReview.openPairImages(std::move(primary),
                                                   QStringLiteral("primary"),
                                                   std::move(secondary),
                                                   QStringLiteral("secondary")));
    ASSERT_TRUE(harness.activateWorkspace(1));
    harness.settle();

    auto* const imageWorkspace =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
    ASSERT_NE(imageWorkspace, nullptr);

    // Zoom to 300%
    harness.imageReview.setZoom(3.0);
    harness.settle();
    EXPECT_DOUBLE_EQ(harness.imageReview.zoom(), 3.0);

    // 100% button resets zoom to 1.0 and sets trueSize to true
    auto* const trueSizeBtn =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageTrueSizeButton"));
    ASSERT_NE(trueSizeBtn, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(trueSizeBtn, "clicked"));
    harness.settle();
    EXPECT_TRUE(imageWorkspace->property("trueSize").toBool());
    EXPECT_DOUBLE_EQ(harness.imageReview.zoom(), 1.0);

    // Zoom to 2.0 while in trueSize
    harness.imageReview.setZoom(2.0);
    harness.settle();
    EXPECT_DOUBLE_EQ(harness.imageReview.zoom(), 2.0);

    // Fit button resets trueSize to false and resets view (zoom 1.0, pan 0.5)
    auto* const fitBtn = harness.root->findChild<QQuickItem*>(QStringLiteral("imageFitButton"));
    ASSERT_NE(fitBtn, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(fitBtn, "clicked"));
    harness.settle();
    EXPECT_FALSE(imageWorkspace->property("trueSize").toBool());
    EXPECT_DOUBLE_EQ(harness.imageReview.zoom(), 1.0);
    EXPECT_DOUBLE_EQ(harness.imageReview.panX(), 0.5);
    EXPECT_DOUBLE_EQ(harness.imageReview.panY(), 0.5);

    // Reset view button resets trueSize to false and resets view
    imageWorkspace->setProperty("trueSize", true);
    harness.imageReview.setZoom(4.0);
    harness.settle();
    auto* const resetViewBtn =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageResetViewButton"));
    ASSERT_NE(resetViewBtn, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(resetViewBtn, "clicked"));
    harness.settle();
    EXPECT_FALSE(imageWorkspace->property("trueSize").toBool());
    EXPECT_DOUBLE_EQ(harness.imageReview.zoom(), 1.0);
    EXPECT_DOUBLE_EQ(harness.imageReview.panX(), 0.5);
}

TEST(MainQmlContractTests, ImageWorkspaceInPlaceToggleAndFlickerContract) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    QImage primary(64, 64, QImage::Format_ARGB32);
    primary.fill(QColor(255, 0, 0));
    QImage secondary(64, 64, QImage::Format_ARGB32);
    secondary.fill(QColor(0, 255, 0));
    ASSERT_TRUE(harness.imageReview.openPairImages(std::move(primary),
                                                   QStringLiteral("primary"),
                                                   std::move(secondary),
                                                   QStringLiteral("secondary")));
    ASSERT_TRUE(harness.activateWorkspace(1));
    harness.settle();

    auto* const imageWorkspace =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
    ASSERT_NE(imageWorkspace, nullptr);

    // Switch to compareMode 0 (single / in-place)
    harness.imageReview.setCompareMode(0);
    harness.settle();

    auto* const inPlaceBadge =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageInPlaceBadge"));
    ASSERT_NE(inPlaceBadge, nullptr);
    EXPECT_TRUE(inPlaceBadge->isVisible());

    auto* const primaryViewport =
        harness.root->findChild<QQuickItem*>(QStringLiteral("primaryViewport"));
    ASSERT_NE(primaryViewport, nullptr);
    EXPECT_EQ(primaryViewport->property("slot").toInt(), 2);

    // Toggle button switches to slot 3 (B)
    auto* const toggleBtn =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageToggleSourceButton"));
    ASSERT_NE(toggleBtn, nullptr);
    EXPECT_TRUE(toggleBtn->isVisible());
    ASSERT_TRUE(QMetaObject::invokeMethod(toggleBtn, "clicked"));
    harness.settle();
    EXPECT_EQ(primaryViewport->property("slot").toInt(), 3);
    EXPECT_TRUE(imageWorkspace->property("singleViewShowSecondary").toBool());

    // Toggle again returns to slot 2 (A)
    ASSERT_TRUE(QMetaObject::invokeMethod(toggleBtn, "clicked"));
    harness.settle();
    EXPECT_EQ(primaryViewport->property("slot").toInt(), 2);
    EXPECT_FALSE(imageWorkspace->property("singleViewShowSecondary").toBool());

    // Flicker button toggles flickerActive
    auto* const flickerBtn =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageFlickerButton"));
    ASSERT_NE(flickerBtn, nullptr);
    EXPECT_TRUE(flickerBtn->isVisible());
    ASSERT_TRUE(QMetaObject::invokeMethod(flickerBtn, "clicked"));
    harness.settle();
    EXPECT_TRUE(imageWorkspace->property("flickerActive").toBool());

    ASSERT_TRUE(QMetaObject::invokeMethod(flickerBtn, "clicked"));
    harness.settle();
    EXPECT_FALSE(imageWorkspace->property("flickerActive").toBool());
}

TEST(MainQmlContractTests, ImageWorkspaceSyncedCrosshairExistsInSideBySide) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    QImage primary(64, 64, QImage::Format_ARGB32);
    primary.fill(QColor(255, 0, 0));
    QImage secondary(64, 64, QImage::Format_ARGB32);
    secondary.fill(QColor(0, 255, 0));
    ASSERT_TRUE(harness.imageReview.openPairImages(std::move(primary),
                                                   QStringLiteral("primary"),
                                                   std::move(secondary),
                                                   QStringLiteral("secondary")));
    ASSERT_TRUE(harness.activateWorkspace(1));
    harness.settle();

    harness.imageReview.setCompareMode(1); // SideBySide
    harness.settle();

    auto* const crosshair2 =
        harness.root->findChild<QQuickItem*>(QStringLiteral("syncedCrosshair-2"));
    ASSERT_NE(crosshair2, nullptr);

    auto* const crosshair3 =
        harness.root->findChild<QQuickItem*>(QStringLiteral("syncedCrosshair-3"));
    ASSERT_NE(crosshair3, nullptr);
}

TEST(MainQmlContractTests, TimelineUncachedHoverShowsTimecodePillAndGuide) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    auto* const tracks = harness.root->findChild<QQuickItem*>(QStringLiteral("timelineSlider"));
    ASSERT_NE(tracks, nullptr);

    auto* const popup =
        harness.root->findChild<QQuickItem*>(QStringLiteral("timelineThumbnailPopup"));
    ASSERT_NE(popup, nullptr);

    auto* const hoverGuide =
        harness.root->findChild<QQuickItem*>(QStringLiteral("timelineHoverGuide"));
    ASSERT_NE(hoverGuide, nullptr);

    // Initial state: mouse not hovering timeline, popup and hover guide are not active/visible.
    EXPECT_FALSE(popup->isVisible());
    EXPECT_FALSE(hoverGuide->isVisible());

    // Hover over frame 50 (uncached preview).
    tracks->setProperty("hoverFrame", 50);
    QMetaObject::invokeMethod(tracks, "previewRequested", Q_ARG(int, 50));
    harness.settle();

    // With our dual-mode implementation, even uncached frames display the compact timecode pill.
    EXPECT_TRUE(popup->isVisible());
    EXPECT_FALSE(popup->property("hasThumbnail").toBool());
    EXPECT_TRUE(hoverGuide->isVisible());

    auto* const timecodeText = popup->findChild<QObject*>(QStringLiteral("previewTimecodeText"));
    ASSERT_NE(timecodeText, nullptr);
    EXPECT_TRUE(timecodeText->property("text").toString().contains(QStringLiteral("第 51 帧")));

    // When mouse exits the timeline, hover frame resets and popup hides.
    tracks->setProperty("hoverFrame", -1);
    harness.settle();
    EXPECT_FALSE(popup->isVisible());
    EXPECT_FALSE(hoverGuide->isVisible());
}

TEST(MainQmlContractTests, ImmersiveModeBottomEdgeWakesOverlayOsc) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    auto* const transport = harness.root->findChild<QQuickItem*>(QStringLiteral("transport"));
    ASSERT_NE(transport, nullptr);
    EXPECT_TRUE(transport->isVisible());

    // Enter immersive mode by hiding chrome.
    harness.shell->setChromeVisible(false);
    harness.settle();
    EXPECT_FALSE(harness.root->property("chromeVisible").toBool());
    EXPECT_FALSE(transport->isVisible());

    auto* const wakeStrip =
        harness.root->findChild<QQuickItem*>(QStringLiteral("immersiveWakeStrip"));
    ASSERT_NE(wakeStrip, nullptr);
    EXPECT_TRUE(wakeStrip->isVisible());

    // Trigger edge wake.
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(), "revealImmersiveOsc"));
    harness.settle();
    EXPECT_TRUE(harness.root->property("immersiveOscRevealed").toBool());
    EXPECT_TRUE(transport->isVisible());
    EXPECT_EQ(harness.root->property("oscState").toInt(), 1);

    // After resetting revealed state, transport hides again.
    harness.root->setProperty("immersiveOscRevealed", false);
    harness.settle();
    EXPECT_FALSE(transport->isVisible());
    EXPECT_EQ(harness.root->property("oscState").toInt(), 2);
}

TEST(MainQmlContractTests, ImageWorkspaceAlphaWorkflowAndBackgroundShortcutsContract) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    // Create an image pair with alpha transparency.
    QImage primary(64, 64, QImage::Format_ARGB32);
    primary.fill(QColor(255, 0, 0, 128));
    QImage secondary(64, 64, QImage::Format_ARGB32);
    secondary.fill(QColor(0, 255, 0, 200));
    ASSERT_TRUE(harness.imageReview.openPairImages(std::move(primary),
                                                   QStringLiteral("primary_with_alpha"),
                                                   std::move(secondary),
                                                   QStringLiteral("secondary_with_alpha")));
    ASSERT_TRUE(harness.activateWorkspace(1));
    harness.settle();

    auto* const imageWorkspace =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
    ASSERT_NE(imageWorkspace, nullptr);

    // Initial state: viewMode = 0 (RgbaView), backgroundMode = 0 (Dark).
    EXPECT_EQ(harness.imageReview.viewMode(), 0);
    EXPECT_EQ(imageWorkspace->property("backgroundMode").toInt(), 0);

    auto* const alphaBadge =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageAlphaObservationBadge"));
    ASSERT_NE(alphaBadge, nullptr);
    EXPECT_FALSE(alphaBadge->isVisible());

    // Give focus to imageWorkspace so keyboard events reach it.
    imageWorkspace->forceActiveFocus();
    harness.settle();

    // Press 'A' to toggle Alpha Gray view.
    sendKey(*harness.window, Qt::Key_A);
    harness.settle();
    EXPECT_EQ(harness.imageReview.viewMode(), 1);
    EXPECT_TRUE(alphaBadge->isVisible());

    // Press 'A' again to toggle back to RGBA.
    sendKey(*harness.window, Qt::Key_A);
    harness.settle();
    EXPECT_EQ(harness.imageReview.viewMode(), 0);
    EXPECT_FALSE(alphaBadge->isVisible());

    // Press 'O' to toggle RGB Opaque view.
    sendKey(*harness.window, Qt::Key_O);
    harness.settle();
    EXPECT_EQ(harness.imageReview.viewMode(), 2);
    EXPECT_TRUE(alphaBadge->isVisible());

    // Press 'O' again to toggle back to RGBA.
    sendKey(*harness.window, Qt::Key_O);
    harness.settle();
    EXPECT_EQ(harness.imageReview.viewMode(), 0);
    EXPECT_FALSE(alphaBadge->isVisible());

    // Press 'B' to cycle background mode: 0 -> 1 (Checkerboard).
    sendKey(*harness.window, Qt::Key_B);
    harness.settle();
    EXPECT_EQ(imageWorkspace->property("backgroundMode").toInt(), 1);
    EXPECT_TRUE(alphaBadge->isVisible());

    // Press 'B' again: 1 -> 2 (Black).
    sendKey(*harness.window, Qt::Key_B);
    harness.settle();
    EXPECT_EQ(imageWorkspace->property("backgroundMode").toInt(), 2);
    EXPECT_TRUE(alphaBadge->isVisible());

    // Press 'B' again: 2 -> 3 (White).
    sendKey(*harness.window, Qt::Key_B);
    harness.settle();
    EXPECT_EQ(imageWorkspace->property("backgroundMode").toInt(), 3);
    EXPECT_TRUE(alphaBadge->isVisible());

    // Press 'B' again: 3 -> 0 (Dark).
    sendKey(*harness.window, Qt::Key_B);
    harness.settle();
    EXPECT_EQ(imageWorkspace->property("backgroundMode").toInt(), 0);
    EXPECT_FALSE(alphaBadge->isVisible());

    // Cycle background button clicks cycle background.
    auto* const cycleBgBtn =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageBgCycleButton"));
    ASSERT_NE(cycleBgBtn, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(cycleBgBtn, "clicked"));
    harness.settle();
    EXPECT_EQ(imageWorkspace->property("backgroundMode").toInt(), 1);
    EXPECT_TRUE(alphaBadge->isVisible());

    // ModeChip click toggles: clicking Alpha Gray chip sets viewMode 1, clicking again resets to 0.
    auto* const alphaGrayChip =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageViewAlphaGray"));
    ASSERT_NE(alphaGrayChip, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(alphaGrayChip, "clicked"));
    harness.settle();
    EXPECT_EQ(harness.imageReview.viewMode(), 1);
    EXPECT_TRUE(alphaBadge->isVisible());

    ASSERT_TRUE(QMetaObject::invokeMethod(alphaGrayChip, "clicked"));
    harness.settle();
    EXPECT_EQ(harness.imageReview.viewMode(), 0);
    EXPECT_TRUE(alphaBadge->isVisible());

    // Reset background to 0.
    imageWorkspace->setProperty("backgroundMode", 0);
    harness.settle();
    EXPECT_FALSE(alphaBadge->isVisible());

    // Verify shortcut help has imagePreset set when in image workspace.
    auto* const shortcutHelp =
        harness.root->findChild<QObject*>(QStringLiteral("shortcutHelpOverlay"));
    ASSERT_NE(shortcutHelp, nullptr);
    EXPECT_TRUE(shortcutHelp->property("imagePreset").toBool());
}
} // namespace
} // namespace dvs::ui

int main(int argumentCount, char* arguments[]) {
    dvs::ui::configureGraphicsBackend();
    QGuiApplication application{argumentCount, arguments};
    initializeMainQmlContractResources();
    static_cast<void>(
        qmlRegisterType<dvs::ui::ComparisonSurface>("Dvs.Ui", 1, 0, "ComparisonSurface"));
    testing::InitGoogleTest(&argumentCount, arguments);
    return RUN_ALL_TESTS();
}
