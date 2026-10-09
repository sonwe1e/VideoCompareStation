#include "dvs/application/Alignment.h"
#include "dvs/application/SessionSnapshot.h"
#include "dvs/domain/ComparisonValidator.h"
#include "dvs/ui/ClipExportController.h"
#include "dvs/ui/ComparisonExportController.h"
#include "dvs/ui/ComparisonSurface.h"
#include "dvs/ui/EditImageProvider.h"
#include "dvs/ui/GraphicsBackend.h"
#include "dvs/ui/ImageEditController.h"
#include "dvs/ui/ImageFolderPairModel.h"
#include "dvs/ui/ImageReviewController.h"
#include "dvs/ui/IssueLogController.h"
#include "dvs/ui/ReviewController.h"
#include "dvs/ui/ReviewImageProvider.h"
#include "dvs/ui/ReviewPreferencesController.h"
#include "dvs/ui/ReviewSessionFacade.h"
#include "dvs/ui/ReviewShellController.h"
#include "dvs/ui/SourceIdentity.h"
#include "dvs/ui/SourceListModel.h"
#include "dvs/ui/VideoFolderModel.h"

#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QImage>
#include <QJSValue>
#include <QKeyEvent>
#include <QList>
#include <QMouseEvent>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QResource>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>
#include <QtQml/qqml.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <mutex>
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

// Key events go to the focused item, mirroring how Qt delivers real key presses. Falls
// back to the given item when nothing has focus (the offscreen/basic render loop does not
// always move focus the way a desktop session does).
void sendKeyToFocus(QObject* fallback, QEvent::Type type, int key) {
    QObject* receiver = QGuiApplication::focusObject();
    if (receiver == nullptr) {
        receiver = fallback;
    }
    QKeyEvent event{type, key, Qt::NoModifier};
    QCoreApplication::sendEvent(receiver, &event);
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

// The image editor keeps the three most-used tools on the row and folds mosaic/fill/clear/
// rect/arrow/text into the 「更多工具」 menu, so a test that wants one of those must open the
// menu the way a user would. Menu rows live in the popup's content object, which only exists
// once the menu opens, so this opens first and looks up second.
QQuickItem* openEditToolsItem(QObject* root, const QString& objectName) {
    auto* const menuButton =
        root->findChild<QQuickItem*>(QStringLiteral("imageEditMoreToolsButton"));
    if (!menuButton) {
        return nullptr;
    }
    QMetaObject::invokeMethod(menuButton, "clicked");
    return root->findChild<QQuickItem*>(objectName);
}

// Types a new value into an editable SpinBox. Assigning `value` straight from C++ only moves
// the control, because a SpinBox reports a user edit through valueModified; forcing that
// signal is the programmatic equivalent of committing an edit in the field.
[[nodiscard]] bool typeSpinBoxValue(QQuickItem* field, int value) {
    if (!field) {
        return false;
    }
    field->setProperty("value", value);
    return QMetaObject::invokeMethod(field, "valueModified");
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

// C1 resume: completes a load carrying one pre-seeded resume entry, so a contract test can start
// from a document that already says "this file was last watched at frame N" without writing to the
// user's real settings.
class SeededResumeSettingsRepository final : public application::ISettingsRepository {
public:
    explicit SeededResumeSettingsRepository(std::map<std::string, std::string, std::less<>> values)
        : values_(std::move(values)) {}

    [[nodiscard]] application::PortSubmitResult
    submit(const application::SettingsLoadRequest& request,
           std::shared_ptr<application::IApplicationEventSink> events) override {
        // The document rides on SettingsLoaded; the terminal only carries the outcome, same as the
        // real adapter posts it.
        application::SettingsSnapshot loaded;
        loaded.values = values_;
        static_cast<void>(
            events->postCritical(application::ApplicationEvent{application::SettingsLoaded{
                .context = request.context, .settings = std::move(loaded)}}));
        static_cast<void>(events->postCritical(application::ApplicationEvent{
            application::RequestTerminal{application::RequestSucceeded{
                .context = application::EventContext{request.context}}}}));
        return application::PortSubmitResult::Accepted;
    }

    [[nodiscard]] application::PortSubmitResult
    submit(const application::SettingsSaveRequest&,
           std::shared_ptr<application::IApplicationEventSink>) override {
        return application::PortSubmitResult::Accepted;
    }

    void cancel(const application::RequestContext&) noexcept override {}

private:
    std::map<std::string, std::string, std::less<>> values_;
};

// Records the demux/remux work the GUI asked for and finishes instantly, so a contract test can
// assert the request that crossed the QML boundary without decoding or writing anything. Both
// entry points are called from the controller's worker thread, hence the mutex around the record.
class RecordingClipExporter final : public application::IClipExporter {
public:
    [[nodiscard]] std::vector<std::int64_t>
    keyframeTimes(const std::filesystem::path& sourcePath,
                  const std::atomic_bool& cancelRequested) override {
        const std::lock_guard guard{mutex};
        queriedSources.push_back(sourcePath);
        if (cancelRequested.load()) {
            return {};
        }
        return keyframes;
    }

    [[nodiscard]] application::ClipExportReport
    perform(const application::ClipExportJob& job,
            const std::atomic_bool& cancelRequested) override {
        const std::lock_guard guard{mutex};
        jobs.push_back(job);
        application::ClipExportReport report{};
        report.requestId = job.requestId;
        report.outcome = cancelRequested.load() ? application::ClipExportOutcome::kCanceled
                                                : application::ClipExportOutcome::kCompleted;
        report.packetsWritten = 5;
        return report;
    }

    [[nodiscard]] std::vector<application::ClipExportJob> performedJobs() const {
        const std::lock_guard guard{mutex};
        return jobs;
    }

    [[nodiscard]] std::vector<std::filesystem::path> queriedSourcePaths() const {
        const std::lock_guard guard{mutex};
        return queriedSources;
    }

    // Single GOP starting at time zero: the aligned plan must pre-roll back to it, which is the
    // behaviour the dialog documents to the user.
    std::vector<std::int64_t> keyframes{0};

private:
    mutable std::mutex mutex;
    std::vector<std::filesystem::path> queriedSources;
    std::vector<application::ClipExportJob> jobs;
};

// Installs a validated video comparison on a snapshot. Clip export resolves its source through the
// validated set (canonical source id, descriptor path, canonical rate), so a snapshot whose
// `sources` are bare SessionSourceView entries can never export anything - the same reason the
// real composition root publishes the validated set with every projection.
[[nodiscard]] bool
installValidatedVideoSet(const std::shared_ptr<application::SessionSnapshot>& snapshot,
                         const std::vector<std::filesystem::path>& paths,
                         const domain::RationalRate& rate,
                         const std::int64_t frameCount,
                         const std::int64_t durationMicroseconds) {
    std::vector<domain::ComparisonSource> sources;
    for (std::size_t index = 0U; index < paths.size(); ++index) {
        const QFileInfo info{QString::fromStdWString(paths[index].wstring())};
        sources.push_back(domain::ComparisonSource{
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
                            .value = frameCount,
                            .origin = domain::FrameCountOrigin::kReported,
                        },
                    .duration = domain::MediaTime{durationMicroseconds},
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

    auto validated = domain::ComparisonValidator::validate(std::move(sources));
    if (!validated) {
        return false;
    }
    snapshot->validatedComparison =
        std::make_shared<const domain::ValidatedComparisonSet>(std::move(validated).value().set);
    snapshot->canonicalFrameCount =
        static_cast<std::uint64_t>(snapshot->validatedComparison->canonicalFrameCount());
    snapshot->canonicalTimeline = rate;
    snapshot->sources.clear();
    snapshot->presentedSources.clear();
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
    return true;
}

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
        if (withVideoFolder) {
            videoFolder.attachPreferences(preferences);
            videoFolder.attachPlayback(*controller, *shell);
            engine.rootContext()->setContextProperty(QStringLiteral("videoFolder"), &videoFolder);
        }
        engine.addImageProvider(QStringLiteral("vcs-review"),
                                new ReviewImageProvider(&imageReview));
        if (withImageEdit) {
            imageEdit.setSourceImageProvider(
                [this](const int slot) { return imageReview.rawImageForSlot(slot); });
            engine.rootContext()->setContextProperty(QStringLiteral("imageEdit"), &imageEdit);
            engine.addImageProvider(QStringLiteral("dvs-edit"), new EditImageProvider(&imageEdit));
        }
        if (withClipExport) {
            clipExport = std::make_unique<ClipExportController>(ClipExportController::Dependencies{
                .snapshot = [this] { return snapshot; },
                .exporter = clipExporter,
            });
            engine.rootContext()->setContextProperty(QStringLiteral("clipExport"),
                                                     clipExport.get());
        }
        if (withIssueLog) {
            issueLog.setReviewController(controller.get());
            issueLog.setPreferences(&preferences);
            issueLog.setFolderModel(&folderPairs);
            issueLog.setImageController(&imageReview);
            engine.rootContext()->setContextProperty(QStringLiteral("issueLog"), &issueLog);
        }
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

    [[nodiscard]] bool commitWorkspace(const int media, const QString& identity) {
        QVariant result;
        return QMetaObject::invokeMethod(root.get(),
                                         "commitWorkspace",
                                         Q_RETURN_ARG(QVariant, result),
                                         Q_ARG(QVariant, QVariant{media}),
                                         Q_ARG(QVariant, QVariant{identity})) &&
               result.toBool();
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

    // settle() only drains the queue, so a 100-120 ms colour Behavior is still at its start
    // colour when grabWindow() runs; evidence captures wait the transitions out first.
    void settleAnimations(const int milliseconds = 250) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < milliseconds) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(5U);
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
    VideoFolderModel videoFolder;
    bool withVideoFolder = false;
    // Step-3 editing stays opt-in so harnesses that predate it keep their exact layout.
    ImageEditController imageEdit;
    bool withImageEdit = false;
    // Clip export is opt-in for the same reason, and for one more: with no exporter the context
    // property is absent, which is exactly the composition-root state the transport must survive.
    std::shared_ptr<RecordingClipExporter> clipExporter = std::make_shared<RecordingClipExporter>();
    std::unique_ptr<ClipExportController> clipExport;
    bool withClipExport = false;
    // The issue log is also opt-in: only the composition root publishes one, and its absence
    // is a state M-key and the panel must survive without errors. Capture works without a
    // repository; only save/load would need dvs_persistence_json.
    IssueLogController issueLog{static_cast<application::IIssueRecordRepository*>(nullptr)};
    bool withIssueLog = false;
    QQmlEngine engine;
    std::unique_ptr<QObject> root;
    QQuickWindow* window = nullptr;
    std::string error;
};

// The real inspector's Repeater consumes [label, value] pairs. A bare label string
// is indexable too, but silently renders its first two characters instead of the ratio.
TEST(MainQmlContractTests, PairMetricsMismatchRowPreservesPolicyAndPercentage) {
    WorkspaceHarness harness;
    harness.shell->setInspectorVisible(true);
    harness.preferences.setViewMode(ReviewPreferencesController::ViewMode::Difference);
    ASSERT_TRUE(harness.create()) << harness.error;
    auto* const inspector = harness.root->findChild<QQuickItem*>(QStringLiteral("tabbedInspector"));
    ASSERT_NE(inspector, nullptr);
    auto* const readout = inspector->findChild<QQuickItem*>(QStringLiteral("metricsReadoutBlock"));
    ASSERT_NE(readout, nullptr);
    QJSValue inspectorObject = harness.engine.newQObject(inspector);
    QJSValue rowsFunction = inspectorObject.property(QStringLiteral("metricsRows"));
    ASSERT_TRUE(rowsFunction.isCallable());

    struct RatioCase {
        double ratio;
        const char* text;
    };
    // Zero keeps the existing small-percentage convention; this fix only restores the
    // missing pair. The repeated 25% case also rejects a stale value after changing samples.
    const std::array<RatioCase, 6U> cases{{
        {0.25, "25.00%"},
        {0.0, "< 0.01%"},
        {0.00005, "< 0.01%"},
        {0.0001, "0.01%"},
        {1.0, "100.00%"},
        {0.25, "25.00%"},
    }};
    const QStringList policies{
        QStringLiteral("亮度"), QStringLiteral("任一通道"), QStringLiteral("全部通道")};
    const std::array<int, 3U> thresholds{0, 16, 255};
    for (int policy = 0; policy < 3; ++policy) {
        for (const RatioCase& sample : cases) {
            const int threshold = thresholds[static_cast<std::size_t>(policy)];
            const QVariantMap metrics{
                {QStringLiteral("available"), true},
                {QStringLiteral("errorKey"), QString{}},
                {QStringLiteral("hasCurrentSample"), true},
                {QStringLiteral("currentComparable"), true},
                {QStringLiteral("metricId"), QStringLiteral("cpu-rgb-absolute-v1")},
                {QStringLiteral("thresholdPolicy"), policy},
                {QStringLiteral("threshold"), threshold},
                {QStringLiteral("currentMae"), 2.0},
                {QStringLiteral("currentMse"), 4.0},
                {QStringLiteral("currentPsnrDb"), 42.11},
                {QStringLiteral("currentMaxAbsError"), 8.0},
                {QStringLiteral("currentMismatchRatio"), sample.ratio},
                {QStringLiteral("currentMismatchPixels"), sample.ratio * 1'000'000.0},
                {QStringLiteral("currentPixelCount"), 1'000'000},
            };
            ASSERT_TRUE(inspector->setProperty("metrics", metrics));
            harness.settle();
            const QJSValue rows = rowsFunction.callWithInstance(inspectorObject);
            ASSERT_TRUE(rows.isArray());
            ASSERT_EQ(rows.property(QStringLiteral("length")).toInt(), 7);
            const QJSValue row = rows.property(4U);
            ASSERT_TRUE(row.isArray());
            ASSERT_EQ(row.property(QStringLiteral("length")).toInt(), 2);
            const QString expectedLabel =
                QStringLiteral("坏点占比（%1 ≥ 阈值 %2）").arg(policies[policy]).arg(threshold);
            EXPECT_EQ(row.property(0U).toString(), expectedLabel);
            EXPECT_EQ(row.property(1U).toString(), QString::fromLatin1(sample.text));

            // Inspect the actual delegate's two labels too, so a correct helper result
            // cannot hide a broken model-to-text connection.
            int matchedRows = 0;
            QList<QQuickItem*> pending{readout};
            while (!pending.isEmpty()) {
                QQuickItem* const object = pending.takeLast();
                pending.append(object->childItems());
                if (object->property("text").toString() != expectedLabel) {
                    continue;
                }
                ASSERT_NE(object->parentItem(), nullptr);
                QStringList siblingTexts;
                for (QQuickItem* sibling : object->parentItem()->childItems()) {
                    if (sibling->property("text").isValid()) {
                        siblingTexts.push_back(sibling->property("text").toString());
                    }
                }
                EXPECT_EQ(siblingTexts,
                          (QStringList{expectedLabel, QString::fromLatin1(sample.text)}));
                ++matchedRows;
            }
            EXPECT_EQ(matchedRows, 1);
        }
    }
}

// Instantiate only the real inspector item: no Main window, media service or rendered pixels.
TEST(MainQmlContractTests, PairMetricsScopeNoteExplainsSampleSpaceAndWraps) {
    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/TabbedInspector.qml")}};
    const QVariantMap properties{
        {QStringLiteral("controller"),
         QVariantMap{{QStringLiteral("alignmentMode"), 0},
                     {QStringLiteral("activePairTimeInfo"), QVariantMap{}},
                     {QStringLiteral("currentInexactReason"), QString{}},
                     {QStringLiteral("sources"), QVariantList{}},
                     {QStringLiteral("dropFrameTimecodeAvailable"), false},
                     {QStringLiteral("alignmentTimelineMarkerOverflowCount"), 0},
                     {QStringLiteral("sourceMediaInfo"), QVariantList{}}}},
        {QStringLiteral("preferences"),
         QVariantMap{{QStringLiteral("differenceMetric"), 0},
                     {QStringLiteral("differenceGain"), 0},
                     {QStringLiteral("differenceFilter"), 0},
                     {QStringLiteral("oscMode"), 0}}},
        {QStringLiteral("session"), QVariantMap{}},
        {QStringLiteral("metrics"),
         QVariantMap{{QStringLiteral("available"), true},
                     {QStringLiteral("errorKey"), QString{}},
                     {QStringLiteral("hasCurrentSample"), false},
                     {QStringLiteral("sampling"), false},
                     {QStringLiteral("metricId"), QStringLiteral("cpu-rgb-absolute-v1")}}},
        {QStringLiteral("borderColor"), QColor{Qt::gray}},
        {QStringLiteral("primaryTextColor"), QColor{Qt::white}},
        {QStringLiteral("mutedTextColor"), QColor{Qt::gray}},
        {QStringLiteral("singleMode"), false},
        {QStringLiteral("sourceCount"), 2},
        {QStringLiteral("wipeMode"), false},
        {QStringLiteral("differenceMode"), true},
        {QStringLiteral("analysisGridMode"), false},
        {QStringLiteral("differenceEdges"), QVariantList{}},
        {QStringLiteral("sourceIdentities"), QVariantList{}},
        {QStringLiteral("differenceEdge"), 0},
        {QStringLiteral("referenceSourceIndex"), 0},
        {QStringLiteral("differenceThresholdEnabled"), false},
        {QStringLiteral("differenceThresholdCode"), 0},
        {QStringLiteral("differenceThresholdPolicy"), 1},
        {QStringLiteral("wipePosition"), 0.5},
        {QStringLiteral("roiEnabled"), false},
        {QStringLiteral("graphicsReady"), false},
        {QStringLiteral("dropFrameTimecode"), false},
        {QStringLiteral("currentFrame"), 0},
        {QStringLiteral("inFrame"), -1},
        {QStringLiteral("outFrame"), -1},
        {QStringLiteral("rangePlaybackActive"), false},
        {QStringLiteral("width"), 300},
        {QStringLiteral("height"), 1200},
    };
    const std::unique_ptr<QObject> root{component.createWithInitialProperties(properties)};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    auto* const note = root->findChild<QQuickItem*>(QStringLiteral("metricsScopeNote"));
    ASSERT_NE(note, nullptr);
    EXPECT_EQ(note->window(), nullptr);
    EXPECT_TRUE(note->isVisible());
    EXPECT_EQ(note->property("text").toString(),
              QStringLiteral("解码转换后的全帧 RGBA8 RGB 值（忽略 Alpha），非原始码值；"
                             "10 位输入也按 8 位统计。ROI、视图缩放、显示旋转／重采样及"
                             "显示增益不改变统计，不等同当前屏幕差异。"));
    QQuickItem* const column = note->parentItem();
    ASSERT_NE(column, nullptr);
    // Probe the metrics column directly so this remains windowless. These are component
    // geometry checks, not a native inspector screenshot or a full-window resize acceptance.
    for (const qreal width : {252.0, 332.0, 252.0}) {
        column->setWidth(width);
        ASSERT_TRUE(QMetaObject::invokeMethod(note, "forceLayout"));
        ASSERT_TRUE(QMetaObject::invokeMethod(column, "forceLayout"));
        QCoreApplication::processEvents();
        EXPECT_DOUBLE_EQ(note->width(), width);
        EXPECT_GT(note->height(), 22.0);
        EXPECT_LE(note->property("contentWidth").toReal(), width + 1.0);
        EXPECT_GE(note->height(), note->property("contentHeight").toReal());
        EXPECT_GE(column->implicitHeight(), note->y() + note->height());
    }
}

// Legend swatches must follow the same semantic mapping as the timeline, never color-name prose.
TEST(MainQmlContractTests, TimelineMarkerLegendUsesThemeColorsAndWraps) {
    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/TabbedInspector.qml")}};
    const QVariantMap properties{
        {QStringLiteral("controller"),
         QVariantMap{{QStringLiteral("alignmentMode"), 0},
                     {QStringLiteral("activePairTimeInfo"), QVariantMap{}},
                     {QStringLiteral("currentInexactReason"), QString{}},
                     {QStringLiteral("sources"), QVariantList{}},
                     {QStringLiteral("dropFrameTimecodeAvailable"), false},
                     {QStringLiteral("alignmentTimelineMarkerOverflowCount"), 2},
                     {QStringLiteral("sourceMediaInfo"), QVariantList{}}}},
        {QStringLiteral("preferences"),
         QVariantMap{{QStringLiteral("differenceMetric"), 0},
                     {QStringLiteral("differenceGain"), 0},
                     {QStringLiteral("differenceFilter"), 0},
                     {QStringLiteral("oscMode"), 0}}},
        {QStringLiteral("session"), QVariantMap{}},
        {QStringLiteral("metrics"),
         QVariantMap{{QStringLiteral("available"), true},
                     {QStringLiteral("errorKey"), QString{}},
                     {QStringLiteral("hasCurrentSample"), false},
                     {QStringLiteral("sampling"), false},
                     {QStringLiteral("metricId"), QStringLiteral("cpu-rgb-absolute-v1")}}},
        {QStringLiteral("borderColor"), QColor{Qt::gray}},
        {QStringLiteral("primaryTextColor"), QColor{Qt::white}},
        {QStringLiteral("mutedTextColor"), QColor{Qt::gray}},
        {QStringLiteral("singleMode"), false},
        {QStringLiteral("sourceCount"), 2},
        {QStringLiteral("wipeMode"), false},
        {QStringLiteral("differenceMode"), true},
        {QStringLiteral("analysisGridMode"), false},
        {QStringLiteral("differenceEdges"), QVariantList{}},
        {QStringLiteral("sourceIdentities"), QVariantList{}},
        {QStringLiteral("differenceEdge"), 0},
        {QStringLiteral("referenceSourceIndex"), 0},
        {QStringLiteral("differenceThresholdEnabled"), false},
        {QStringLiteral("differenceThresholdCode"), 0},
        {QStringLiteral("differenceThresholdPolicy"), 1},
        {QStringLiteral("wipePosition"), 0.5},
        {QStringLiteral("roiEnabled"), false},
        {QStringLiteral("graphicsReady"), false},
        {QStringLiteral("dropFrameTimecode"), false},
        {QStringLiteral("currentFrame"), 0},
        {QStringLiteral("inFrame"), -1},
        {QStringLiteral("outFrame"), -1},
        {QStringLiteral("rangePlaybackActive"), false},
        {QStringLiteral("width"), 300},
        {QStringLiteral("height"), 760},
    };
    const std::unique_ptr<QObject> root{component.createWithInitialProperties(properties)};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    auto* const tabs = root->findChild<QQuickItem*>(QStringLiteral("inspectorTabBar"));
    ASSERT_NE(tabs, nullptr);
    tabs->setProperty("currentIndex", 1);
    auto* const legend = root->findChild<QQuickItem*>(QStringLiteral("timelineMarkerLegend"));
    auto* const note = root->findChild<QQuickItem*>(QStringLiteral("timelineMarkerLegendNote"));
    auto* const overflow = root->findChild<QQuickItem*>(QStringLiteral("markerOverflowNotice"));
    ASSERT_NE(legend, nullptr);
    ASSERT_NE(note, nullptr);
    ASSERT_NE(overflow, nullptr);
    EXPECT_TRUE(legend->isVisible());
    EXPECT_EQ(note->property("text").toString(),
              QStringLiteral("低置信度、待复核和已拒绝区间共用中性色；"
                             "悬停时间轴标记可查看类型与置信度。"));

    QQmlComponent themeProbe{&engine};
    themeProbe.setData(R"qml(
        import QtQuick
        import "qrc:/qml/VcsTheme.js" as Theme
        QtObject {
            property color neutral: Theme.markerOther
            function markerColor(kind) { return Theme.timelineMarkerColor(kind); }
        }
    )qml",
                       QUrl{});
    const std::unique_ptr<QObject> theme{themeProbe.create()};
    ASSERT_NE(theme, nullptr) << componentErrors(themeProbe);
    for (const QString& kind : {QStringLiteral("low-confidence"),
                                QStringLiteral("review-segment"),
                                QStringLiteral("rejected-segment")}) {
        QVariant actualColor;
        ASSERT_TRUE(QMetaObject::invokeMethod(theme.get(),
                                              "markerColor",
                                              Q_RETURN_ARG(QVariant, actualColor),
                                              Q_ARG(QVariant, kind)));
        EXPECT_EQ(QColor{actualColor.toString()}, theme->property("neutral").value<QColor>());
    }
    const std::array<std::pair<QString, QString>, 5> entries{{
        {QStringLiteral("missing"), QStringLiteral("缺失")},
        {QStringLiteral("duplicate"), QStringLiteral("重复")},
        {QStringLiteral("extra"), QStringLiteral("多余")},
        {QStringLiteral("anchor"), QStringLiteral("锚点")},
        {QStringLiteral("low-confidence"), QStringLiteral("低置信度")},
    }};
    auto* const repeater =
        legend->findChild<QObject*>(QStringLiteral("timelineMarkerLegendEntries"));
    ASSERT_NE(repeater, nullptr);
    EXPECT_EQ(repeater->property("count").toInt(), static_cast<int>(entries.size()));
    QQuickItem* const column = legend->parentItem();
    ASSERT_NE(column, nullptr);
    // Resize the real panel, including a return to the minimum size. No Main or renderer is used.
    for (const qreal width : {300.0, 380.0, 300.0}) {
        root->setProperty("width", width);
        QCoreApplication::processEvents();
        ASSERT_TRUE(QMetaObject::invokeMethod(legend, "forceLayout"));
        ASSERT_TRUE(QMetaObject::invokeMethod(note, "forceLayout"));
        ASSERT_TRUE(QMetaObject::invokeMethod(column, "forceLayout"));
        EXPECT_DOUBLE_EQ(legend->width(), width - 28.0);
        int entryIndex = 0;
        for (const auto& [kind, labelText] : entries) {
            SCOPED_TRACE(kind.toStdString());
            QQuickItem* entry = nullptr;
            ASSERT_TRUE(QMetaObject::invokeMethod(
                repeater, "itemAt", Q_RETURN_ARG(QQuickItem*, entry), Q_ARG(int, entryIndex)));
            ++entryIndex;
            ASSERT_NE(entry, nullptr);
            auto* const swatch =
                entry->findChild<QQuickItem*>(QStringLiteral("timelineMarkerLegendSwatch-") + kind);
            auto* const label =
                entry->findChild<QQuickItem*>(QStringLiteral("timelineMarkerLegendLabel-") + kind);
            ASSERT_NE(swatch, nullptr);
            ASSERT_NE(label, nullptr);
            QVariant expectedColor;
            ASSERT_TRUE(QMetaObject::invokeMethod(theme.get(),
                                                  "markerColor",
                                                  Q_RETURN_ARG(QVariant, expectedColor),
                                                  Q_ARG(QVariant, kind)));
            EXPECT_EQ(swatch->property("color").value<QColor>(), QColor{expectedColor.toString()});
            EXPECT_EQ(label->property("text").toString(), labelText);
            EXPECT_TRUE(swatch->isVisible());
            EXPECT_TRUE(label->isVisible());
            EXPECT_GT(label->width(), 0.0);
            EXPECT_GT(label->height(), 0.0);
            EXPECT_GE(swatch->width(), 8.0);
            EXPECT_GE(swatch->height(), 8.0);
            EXPECT_GE(entry->x(), 0.0);
            EXPECT_LE(entry->x() + entry->width(), legend->width());
            EXPECT_LE(label->x() + label->width(), entry->width());
            EXPECT_GE(legend->height(), entry->y() + entry->height());
        }
        EXPECT_GE(note->y(), legend->y() + legend->height());
        EXPECT_LE(note->property("contentWidth").toReal(), note->width() + 1.0);
        EXPECT_GE(note->height(), note->property("contentHeight").toReal());
        EXPECT_TRUE(overflow->isVisible());
        EXPECT_GE(overflow->y(), note->y() + note->height());
    }
}

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
    // The alignment chip (对齐：…) is always present in a video session, so the analysis
    // chrome stays visible even without difference/ROI/threshold conditions.
    EXPECT_TRUE(analysisChrome->property("visible").toBool());
    EXPECT_EQ(surfaceLabelRepeater->property("count").toInt(), 2);
    EXPECT_EQ(activeSourceRepeater->property("count").toInt(), 2);
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
    // The inspector's tab tree is instantiated on first open, so its children are looked up
    // after the open, exactly like a user reaching them through the inspector toggle.
    QObject* const setInButton = root->findChild<QObject*>(QStringLiteral("setInButton"));
    QObject* const setOutButton = root->findChild<QObject*>(QStringLiteral("setOutButton"));
    QObject* const clearRangeButton = root->findChild<QObject*>(QStringLiteral("clearRangeButton"));
    QObject* const loopRangeButton = root->findChild<QObject*>(QStringLiteral("loopRangeButton"));
    QObject* const mediaInfoRepeater =
        root->findChild<QObject*>(QStringLiteral("mediaInfoRepeater"));
    ASSERT_NE(setInButton, nullptr);
    ASSERT_NE(setOutButton, nullptr);
    ASSERT_NE(clearRangeButton, nullptr);
    ASSERT_NE(loopRangeButton, nullptr);
    ASSERT_NE(mediaInfoRepeater, nullptr);
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
    EXPECT_EQ(activeSourceRepeater->property("count").toInt(), 2);
    EXPECT_FALSE(analysisGridMenuItem->property("enabled").toBool());

    snapshot->sources.resize(1U);
    snapshot->presentedSources.resize(1U);
    controller.refreshProjection();
    QCoreApplication::processEvents();
    EXPECT_EQ(surfaceLabelRepeater->property("count").toInt(), 1);
    EXPECT_EQ(activeSourceRepeater->property("count").toInt(), 1);
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
    EXPECT_FALSE(transport->isVisible());
    EXPECT_FALSE(transport->property("controlsEnabled").toBool())
        << "hidden auto-hide panel must expose controlsEnabled == false";
}

// A2/A3. The backlog carried two narrow-window claims that were never reproduced: the floating
// transport covering the viewport's own fit/reset row below ~1100 px, and the transient notice
// stack running under the right-hand drawer. Both only occur in the overlay topology, so this
// uses a single source, and both are asserted at Main.qml's declared 960 px minimum width. If the
// defect does not reproduce, this test is the evidence that closes it - it must not be deleted
// after a green run, because a passing geometry assertion is exactly what settles the claim.
TEST(MainQmlContractTests, NarrowWindowKeepsViewportCornerControlsReachable) {
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
        for (int iteration = 0; iteration < 8; ++iteration) {
            QCoreApplication::processEvents();
        }
    };
    processLayout();

    ASSERT_EQ(controller.sourceCount(), 1);
    // A single source resolves to the auto-hide overlay transport - the only topology in which the
    // floating bar can sit on top of the viewport at all.
    ASSERT_FALSE(root->property("transportDocked").toBool());
    ASSERT_FALSE(root->property("transportHidden").toBool());

    const auto rectInContent = [window](QQuickItem* item) {
        const QPointF tl = item->mapToItem(window->contentItem(), QPointF{0.0, 0.0});
        return QRectF(tl.x(), tl.y(), item->width(), item->height());
    };

    auto* const transport = root->findChild<QQuickItem*>(QStringLiteral("transport"));
    auto* const viewCommands = root->findChild<QQuickItem*>(QStringLiteral("viewportViewCommands"));
    ASSERT_NE(transport, nullptr);
    ASSERT_NE(viewCommands, nullptr);
    ASSERT_TRUE(viewCommands->isVisible())
        << "the fit/reset row is only reachable when the chrome is visible";
    ASSERT_TRUE(transport->isVisible()) << "the overlay transport must be revealed for this claim";

    const QRectF transportRect = rectInContent(transport);
    const QRectF viewCommandsRect = rectInContent(viewCommands);
    EXPECT_GT(viewCommandsRect.width(), 0.0);
    EXPECT_GT(viewCommandsRect.height(), 0.0);

    EXPECT_FALSE(transportRect.intersects(viewCommandsRect))
        << "the floating transport must not cover the viewport's fit/reset row at 960 px "
        << "(transport=" << transportRect.x() << "," << transportRect.y() << " "
        << transportRect.width() << "x" << transportRect.height()
        << " viewCommands=" << viewCommandsRect.x() << "," << viewCommandsRect.y() << " "
        << viewCommandsRect.width() << "x" << viewCommandsRect.height() << ")";

    // Both controls have to stay inside the window as well, or they are unreachable regardless of
    // whether anything overlaps them.
    const QRectF contentRect{
        0.0, 0.0, window->contentItem()->width(), window->contentItem()->height()};
    EXPECT_TRUE(contentRect.contains(viewCommandsRect))
        << "the fit/reset row must stay inside the window at the minimum width";
}

// The transport's range row is the second entry point to the in/out range (the inspector and the
// I / O / \ shortcuts being the others). These contracts pin the chips to the very same session
// range state so the row cannot drift into a parallel, decorative copy of it.
TEST(MainQmlContractTests, TransportRangeRowMarksInAndOutFromTheCurrentFrame) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    ASSERT_TRUE(harness.root->property("transportDocked").toBool())
        << "range row evidence assumes the docked transport of a two-source session";

    auto* const markIn = harness.root->findChild<QObject*>(QStringLiteral("transportMarkInButton"));
    auto* const markOut =
        harness.root->findChild<QObject*>(QStringLiteral("transportMarkOutButton"));
    auto* const playRange =
        harness.root->findChild<QObject*>(QStringLiteral("transportPlayRangeButton"));
    auto* const loopRange =
        harness.root->findChild<QObject*>(QStringLiteral("transportLoopRangeButton"));
    auto* const clearRange =
        harness.root->findChild<QObject*>(QStringLiteral("transportClearRangeButton"));
    auto* const label = harness.root->findChild<QObject*>(QStringLiteral("transportRangeLabel"));
    ASSERT_NE(markIn, nullptr);
    ASSERT_NE(markOut, nullptr);
    ASSERT_NE(playRange, nullptr);
    ASSERT_NE(loopRange, nullptr);
    ASSERT_NE(clearRange, nullptr);
    ASSERT_NE(label, nullptr);

    // A session without endpoints: only the two mark chips are usable, and the label teaches the
    // shortcut instead of leaving five unexplained grey chips.
    EXPECT_EQ(harness.shell->inFrame(), -1);
    EXPECT_EQ(harness.shell->outFrame(), -1);
    EXPECT_TRUE(markIn->property("chipEnabled").toBool());
    EXPECT_TRUE(markOut->property("chipEnabled").toBool());
    EXPECT_FALSE(playRange->property("chipEnabled").toBool());
    EXPECT_FALSE(loopRange->property("chipEnabled").toBool());
    EXPECT_FALSE(clearRange->property("chipEnabled").toBool());
    EXPECT_EQ(label->property("text").toString(), QStringLiteral("未设区间（I / O 设点）"));

    // Mark the in point at the displayed frame (41, injected by the shared harness) and check the
    // media time travels with it.
    ASSERT_TRUE(QMetaObject::invokeMethod(markIn, "clicked"));
    harness.settle();
    EXPECT_EQ(harness.shell->inFrame(), 41);
    EXPECT_DOUBLE_EQ(harness.shell->inMediaTime(),
                     static_cast<double>(harness.controller->mediaTimeForFrame(41)));
    // An open-ended range cannot be played or looped, but can be cleared.
    EXPECT_FALSE(playRange->property("chipEnabled").toBool());
    EXPECT_TRUE(clearRange->property("chipEnabled").toBool());
    EXPECT_EQ(label->property("text").toString(), QStringLiteral("入 42 · 出 —（区间无效）"))
        << "an in point alone is not a range";

    // Seek to frame 47 and mark the matching out point.
    harness.snapshot->displayedFrame = domain::FrameId{47};
    harness.controller->refreshProjection();
    harness.settle();
    ASSERT_TRUE(QMetaObject::invokeMethod(markOut, "clicked"));
    harness.settle();
    EXPECT_EQ(harness.shell->inFrame(), 41);
    EXPECT_EQ(harness.shell->outFrame(), 47);
    EXPECT_DOUBLE_EQ(harness.shell->outMediaTime(),
                     static_cast<double>(harness.controller->mediaTimeForFrame(47)));
    EXPECT_TRUE(playRange->property("chipEnabled").toBool());
    EXPECT_TRUE(loopRange->property("chipEnabled").toBool());
    EXPECT_EQ(label->property("text").toString(), QStringLiteral("入 42 · 出 48 · 7 帧"));

    // The chips and the transport read the same range: the row forwards what the shell holds.
    auto* const transportBar = harness.root->findChild<QQuickItem*>(QStringLiteral("transportBar"));
    ASSERT_NE(transportBar, nullptr);
    EXPECT_TRUE(transportBar->property("rangeControlsVisible").toBool());
    EXPECT_EQ(transportBar->property("rangeInFrame").toInt(), 41);
    EXPECT_EQ(transportBar->property("rangeOutFrame").toInt(), 47);
    EXPECT_FALSE(transportBar->property("rangeLoopActive").toBool());
}

TEST(MainQmlContractTests, TransportRangeRowPlaysAndLoopsTheMarkedRange) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    harness.shell->setRangeIn(41, static_cast<double>(harness.controller->mediaTimeForFrame(41)));
    harness.shell->setRangeOut(47, static_cast<double>(harness.controller->mediaTimeForFrame(47)));
    harness.settle();

    auto* const playRange =
        harness.root->findChild<QObject*>(QStringLiteral("transportPlayRangeButton"));
    auto* const loopRange =
        harness.root->findChild<QObject*>(QStringLiteral("transportLoopRangeButton"));
    auto* const label = harness.root->findChild<QObject*>(QStringLiteral("transportRangeLabel"));
    ASSERT_NE(playRange, nullptr);
    ASSERT_NE(loopRange, nullptr);
    ASSERT_NE(label, nullptr);

    // "Play range" installs the range and starts the run in one command, with the canonical frame
    // numbers the chips collected - not a re-derived, off-by-one range.
    ASSERT_TRUE(QMetaObject::invokeMethod(playRange, "clicked"));
    harness.settle();
    EXPECT_TRUE(harness.shell->rangePlaybackActive());
    ASSERT_FALSE(harness.submitted.empty());
    const auto* const start =
        std::get_if<application::StartRangePlaybackCommand>(&harness.submitted.back());
    ASSERT_NE(start, nullptr);
    EXPECT_EQ(start->range.inInclusive.value(), 41);
    EXPECT_EQ(start->range.outInclusive.value(), 47);
    EXPECT_TRUE(start->loop);

    // Range-loop display state is projection-driven, exactly like every other playback field: the
    // chips light up when the session snapshot reports the loop, never from the button press
    // alone. Mirror the accepted command into the harness snapshot the way the coordinator would.
    harness.snapshot->playbackRangeIn = domain::FrameId{41};
    harness.snapshot->playbackRangeOut = domain::FrameId{47};
    harness.snapshot->playbackRangeLoop = true;
    harness.snapshot->playbackRangeLoopActive = true;
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_TRUE(harness.root->property("rangePlaybackActive").toBool());
    EXPECT_TRUE(loopRange->property("chipActive").toBool());
    EXPECT_TRUE(label->property("text").toString().endsWith(QStringLiteral("· 循环")));

    // Optional evidence capture for the visible QML change: a complete, loop-active range row is
    // the state worth reviewing, captured at the default window size and at the supported floor.
    const auto evidenceDirectory = qEnvironmentVariable("DVS_REVIEW_EVIDENCE_DIR");
    if (!evidenceDirectory.isEmpty()) {
        ASSERT_TRUE(QDir().mkpath(evidenceDirectory));
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(evidenceDirectory).filePath(QStringLiteral("transport-range-row.png"))));
        // Main.qml sets minimumWidth 960; the range row shares the transport column, so it must
        // stay complete next to the seven adjacent-frame chips.
        harness.window->resize(960, 640);
        harness.settle();
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(evidenceDirectory).filePath(QStringLiteral("transport-range-row-min-width.png"))));
        harness.window->resize(1280, 800);
        harness.settle();
    }

    // Clicking the active loop chip stops the loop. The stop is a real command, not a view flip:
    // the range stays installed with loop off, so the endpoints survive.
    const auto submittedBeforeStop = harness.submitted.size();
    ASSERT_TRUE(QMetaObject::invokeMethod(loopRange, "clicked"));
    harness.settle();
    EXPECT_FALSE(harness.shell->rangePlaybackActive());
    ASSERT_GT(harness.submitted.size(), submittedBeforeStop);
    const auto* const stop =
        std::get_if<application::SetPlaybackRangeCommand>(&harness.submitted.back());
    ASSERT_NE(stop, nullptr);
    ASSERT_TRUE(stop->range.has_value());
    EXPECT_EQ(stop->range->inInclusive.value(), 41);
    EXPECT_EQ(stop->range->outInclusive.value(), 47);
    EXPECT_FALSE(stop->loop);

    // The chip and the label clear once the snapshot agrees (the projection is authoritative for
    // every playback field, the range loop included), and the endpoints stay untouched.
    harness.snapshot->playbackRangeLoop = false;
    harness.snapshot->playbackRangeLoopActive = false;
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_FALSE(harness.root->property("rangePlaybackActive").toBool());
    EXPECT_FALSE(loopRange->property("chipActive").toBool());
    EXPECT_EQ(label->property("text").toString(), QStringLiteral("入 42 · 出 48 · 7 帧"));
    EXPECT_EQ(harness.shell->inFrame(), 41);
    EXPECT_EQ(harness.shell->outFrame(), 47);
    EXPECT_TRUE(playRange->property("chipEnabled").toBool());
}

// Range commands reach the authoritative snapshot after the shell has already notified QML.
// Keep that ordering: pre-seeding the snapshot before marking endpoints masks a disabled chip.
TEST(MainQmlContractTests, ExportRangeEnablesAfterAuthoritativeSnapshotArrives) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    const QString sourcePath = temporaryDirectory.filePath(QStringLiteral("export-gate.mp4"));
    QFile source{sourcePath};
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    ASSERT_EQ(source.write("clip-source", 11), 11);
    source.close();

    auto snapshot = std::make_shared<application::SessionSnapshot>();
    snapshot->graphicsReady = true;
    snapshot->sessionState = domain::SessionState::kReady;
    snapshot->playbackState = domain::PlaybackState::kPaused;
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate);
    ASSERT_TRUE(installValidatedVideoSet(
        snapshot, {std::filesystem::path{sourcePath.toStdWString()}}, rate.value(), 12, 400'000));
    snapshot->displayedFrame = domain::FrameId{3};
    std::vector<application::PlaybackCommand> submitted;
    ReviewController controller{ReviewController::Dependencies{
        .submit =
            [&submitted](application::PlaybackCommand command) {
                submitted.push_back(std::move(command));
                return application::PortSubmitResult::Accepted;
            },
        .snapshot = [&snapshot] { return snapshot; },
        .takeCompletedCommands = [] { return std::vector<application::CommandTerminal>{}; },
    }};
    ReviewPreferencesController preferences{std::make_shared<ClosedSettingsRepository>()};
    ReviewShellController shell{controller, preferences};
    ReviewSessionFacade facade{controller, preferences, shell};
    auto exporter = std::make_shared<RecordingClipExporter>();
    ClipExportController clipExport{ClipExportController::Dependencies{
        .snapshot = [&controller] { return controller.currentSnapshot(); },
        .exporter = exporter,
    }};
    QObject::connect(&controller,
                     &ReviewController::snapshotRefreshed,
                     &clipExport,
                     &ClipExportController::refreshAvailability);
    int availabilityNotifications = 0;
    QObject::connect(&clipExport,
                     &ClipExportController::availabilityChanged,
                     &clipExport,
                     [&availabilityNotifications] { ++availabilityNotifications; });

    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    engine.rootContext()->setContextProperty(QStringLiteral("reviewController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewPreferences"), &preferences);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewSession"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewFacade"), &facade);
    engine.rootContext()->setContextProperty(QStringLiteral("clipExport"), &clipExport);
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);
    std::unique_ptr<QObject> root{component.create()};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);
    window->resize(1280, 800);
    window->show();
    QCoreApplication::processEvents();
    auto* const chip = root->findChild<QQuickItem*>(QStringLiteral("transportExportRangeButton"));
    ASSERT_NE(chip, nullptr);
    ASSERT_FALSE(chip->property("chipEnabled").toBool());

    ASSERT_TRUE(QMetaObject::invokeMethod(root.get(), "setInPoint"));
    EXPECT_EQ(shell.inFrame(), 3);
    snapshot = std::make_shared<application::SessionSnapshot>(*snapshot);
    snapshot->displayedFrame = domain::FrameId{7};
    controller.refreshProjection();
    ASSERT_TRUE(QMetaObject::invokeMethod(root.get(), "setOutPoint"));
    EXPECT_EQ(shell.outFrame(), 7);
    EXPECT_EQ(controller.playbackRangeIn(), 3);
    EXPECT_EQ(controller.playbackRangeOut(), 7);
    EXPECT_FALSE(controller.currentSnapshot()->playbackRangeOut.has_value());
    EXPECT_FALSE(clipExport.canExport());
    EXPECT_FALSE(chip->property("chipEnabled").toBool());
    EXPECT_EQ(availabilityNotifications, 0);

    // The coordinator now acknowledges the range. Its values match the optimistic view, so a
    // stateChanged-only subscription cannot observe this transition to an exportable snapshot.
    int viewNotifications = 0;
    QObject::connect(&controller,
                     &ReviewController::stateChanged,
                     &controller,
                     [&viewNotifications] { ++viewNotifications; });
    snapshot = std::make_shared<application::SessionSnapshot>(*snapshot);
    snapshot->playbackRangeIn = domain::FrameId{3};
    snapshot->playbackRangeOut = domain::FrameId{7};
    controller.refreshProjection();
    EXPECT_EQ(viewNotifications, 0);
    EXPECT_TRUE(clipExport.canExport());
    EXPECT_TRUE(root->property("rangeExportEnabled").toBool());
    ASSERT_TRUE(chip->property("chipEnabled").toBool());
    EXPECT_EQ(availabilityNotifications, 1);
    controller.refreshProjection();
    EXPECT_EQ(availabilityNotifications, 1);

    // Exercise real mouse delivery rather than invoking the clicked signal on a disabled item.
    const QPointF chipCenter = chip->mapToScene(QPointF{chip->width() / 2, chip->height() / 2});
    sendMousePress(*window, chipCenter);
    sendMouseRelease(*window, chipCenter);
    auto* const popup = root->findChild<QObject*>(QStringLiteral("clipExportPopup"));
    ASSERT_NE(popup, nullptr);
    EXPECT_TRUE(popup->property("visible").toBool());
    EXPECT_TRUE(exporter->performedJobs().empty());

    // An authoritative invalidation must disable export without needing another shell edit.
    snapshot = std::make_shared<application::SessionSnapshot>(*snapshot);
    snapshot->playbackRangeIn.reset();
    snapshot->playbackRangeOut.reset();
    controller.refreshProjection();
    EXPECT_EQ(shell.inFrame(), 3);
    EXPECT_EQ(shell.outFrame(), 7);
    EXPECT_FALSE(clipExport.canExport());
    EXPECT_FALSE(chip->property("chipEnabled").toBool());
    EXPECT_EQ(availabilityNotifications, 2);
    window->close();
}

// Issue records capture ROI, centre and scale, and the product promise is that restoring an
// issue returns to that same viewport. Main.applyIssueRestore must therefore feed the saved
// view fields into ComparisonSurface.restoreViewport - not just mode, pair and frame.
TEST(MainQmlContractTests, IssueRestoreReplaysSavedViewportOntoSurface) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    auto* const surface = harness.root->findChild<QQuickItem*>(QStringLiteral("dualVideoSurface"));
    ASSERT_NE(surface, nullptr);
    // Restore validates the requested scale against live presentation geometry; give the
    // surface the extent the recorded session would have presented.
    ASSERT_TRUE(surface->setProperty("sourceDisplayInfo",
                                     QVariantList{QVariantMap{
                                         {QStringLiteral("width"), 1920},
                                         {QStringLiteral("height"), 1080},
                                     }}));
    harness.settle();
    EXPECT_FALSE(surface->property("roiEnabled").toBool());
    EXPECT_NEAR(surface->property("viewScale").toDouble(), 1.0, 0.000001);

    // The payload shape IssueLogController::restoreIssue emits for a Ready video decision.
    const QVariantMap payload{
        {QStringLiteral("decision"), QStringLiteral("ready")},
        {QStringLiteral("kind"), QStringLiteral("video")},
        {QStringLiteral("viewMode"), 0},
        {QStringLiteral("roiEnabled"), true},
        {QStringLiteral("roiLeft"), 0.25},
        {QStringLiteral("roiTop"), 0.25},
        {QStringLiteral("roiRight"), 0.75},
        {QStringLiteral("roiBottom"), 0.75},
        {QStringLiteral("zoom"), 2.0},
        {QStringLiteral("centerX"), 0.5},
        {QStringLiteral("centerY"), 0.5},
    };
    QVariant restoreResult;
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(),
                                          "applyIssueRestore",
                                          Q_RETURN_ARG(QVariant, restoreResult),
                                          Q_ARG(QVariant, QVariant::fromValue(payload))));
    EXPECT_TRUE(restoreResult.toBool());
    harness.settle();

    EXPECT_TRUE(surface->property("roiEnabled").toBool());
    EXPECT_NEAR(surface->property("viewScale").toDouble(), 2.0, 0.000001);
    EXPECT_NEAR(surface->property("roiLeft").toDouble(), 0.25, 0.000001);
    EXPECT_NEAR(surface->property("roiTop").toDouble(), 0.25, 0.000001);
    EXPECT_NEAR(surface->property("roiRight").toDouble(), 0.75, 0.000001);
    EXPECT_NEAR(surface->property("roiBottom").toDouble(), 0.75, 0.000001);
    EXPECT_NEAR(surface->property("viewCenterX").toDouble(), 0.5, 0.000001);
    EXPECT_NEAR(surface->property("viewCenterY").toDouble(), 0.5, 0.000001);
    harness.window->close();
}

// The export chip is the only way into a clip export, and it must hand the controller the range the
// user marked - the same inclusive endpoints the transport shows, never a re-derived pair. The fake
// exporter completes instantly, so the whole QML -> controller -> adapter path runs in-process.
TEST(MainQmlContractTests, ExportRangeButtonStartsAClipExport) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    std::vector<std::filesystem::path> sourcePaths;
    for (const char* name : {"clip_source_a.mp4", "clip_source_b.mp4"}) {
        const QString path = temporaryDirectory.filePath(QString::fromLatin1(name));
        QFile file{path};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(file.write("fake-clip-source", 16), 16);
        file.close();
        sourcePaths.push_back(std::filesystem::path{path.toStdWString()});
    }

    WorkspaceHarness harness;
    harness.withClipExport = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    auto* const exportChip =
        harness.root->findChild<QObject*>(QStringLiteral("transportExportRangeButton"));
    ASSERT_NE(exportChip, nullptr);
    // The composition root published a service, so the chip belongs to the row...
    EXPECT_TRUE(exportChip->property("visible").toBool());
    EXPECT_EQ(exportChip->property("chipText").toString(), QStringLiteral("导出"));
    // ...while the row still gates it: with no endpoints there is nothing to export, and saying so
    // is the chip's job instead of opening an empty dialog.
    EXPECT_FALSE(exportChip->property("chipEnabled").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(exportChip, "clicked"));
    harness.settle();
    EXPECT_EQ(harness.root->property("immersiveHudText").toString(),
              QStringLiteral("当前视频或所选区间暂不可导出，请检查素材和入点、出点。"));
    EXPECT_TRUE(harness.clipExporter->performedJobs().empty());

    // A validated canonical source plus a marked range is what makes a session exportable.
    const auto rateResult = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rateResult);
    ASSERT_TRUE(
        installValidatedVideoSet(harness.snapshot, sourcePaths, rateResult.value(), 12, 400'000));
    harness.snapshot->displayedFrame = domain::FrameId{3};
    harness.snapshot->playbackRangeIn = domain::FrameId{3};
    harness.snapshot->playbackRangeOut = domain::FrameId{7};
    harness.controller->refreshProjection();
    harness.settle();
    harness.shell->setRangeIn(3, static_cast<double>(harness.controller->mediaTimeForFrame(3)));
    harness.shell->setRangeOut(7, static_cast<double>(harness.controller->mediaTimeForFrame(7)));
    harness.settle();

    ASSERT_NE(harness.clipExport, nullptr);
    EXPECT_TRUE(harness.clipExport->canExport());
    EXPECT_EQ(harness.clipExport->rangeSummary(), QStringLiteral("入 4 · 出 8 · 5 帧"));
    EXPECT_EQ(harness.clipExport->suggestedFileName(),
              QStringLiteral("clip_source_a_clip_4-8.mp4"));
    EXPECT_TRUE(exportChip->property("chipEnabled").toBool());

    // Optional evidence capture for the visible QML change: the idle chip inside the range row is
    // the state a reviewer sees first, then the dialog itself at the default size and at the
    // supported width floor (Main.qml sets minimumWidth 960).
    const auto evidenceDirectory = qEnvironmentVariable("DVS_REVIEW_EVIDENCE_DIR");
    if (!evidenceDirectory.isEmpty()) {
        ASSERT_TRUE(QDir().mkpath(evidenceDirectory));
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(evidenceDirectory).filePath(QStringLiteral("transport-export-chip.png"))));
    }

    // Clicking the chip opens the dialog, which reads the very same range off the service.
    ASSERT_TRUE(QMetaObject::invokeMethod(exportChip, "clicked"));
    harness.settle();
    auto* const popup = harness.root->findChild<QObject*>(QStringLiteral("clipExportPopup"));
    ASSERT_NE(popup, nullptr);
    EXPECT_TRUE(popup->property("visible").toBool());
    // The picker is part of the dialog but must never open by itself: a test cannot answer a system
    // modal, and neither can a user who only wanted to read the range before committing to a file.
    auto* const picker =
        harness.root->findChild<QObject*>(QStringLiteral("clipExportTargetDialog"));
    ASSERT_NE(picker, nullptr);
    EXPECT_FALSE(picker->property("visible").toBool());

    // The dialog must show the range as it is now, not as it was at the last export: the range
    // lives on the shell, and marking an in point emits no controller notification, so this is
    // the assertion that keeps the dialog from silently going stale.
    auto* const exportDialog =
        harness.root->findChild<QObject*>(QStringLiteral("clipExportDialog"));
    ASSERT_NE(exportDialog, nullptr);
    EXPECT_EQ(exportDialog->property("rangeSummary").toString(),
              QStringLiteral("入 4 · 出 8 · 5 帧"));
    EXPECT_EQ(exportDialog->property("fileName").toString(),
              QStringLiteral("clip_source_a_clip_4-8.mp4"));

    if (!evidenceDirectory.isEmpty()) {
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(evidenceDirectory).filePath(QStringLiteral("clip-export-dialog.png"))));
        harness.window->resize(960, 640);
        harness.settle();
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(evidenceDirectory).filePath(QStringLiteral("clip-export-dialog-min-width.png"))));
        harness.window->resize(1280, 800);
        harness.settle();
    }

    // Accepting the picker calls exactly this, so the path under test is the dialog's own path. The
    // destination must be a real local file URL, which is also why the controller is strict about
    // it.
    const QString targetPath = temporaryDirectory.filePath(QStringLiteral("clip_4-8.mp4"));
    ASSERT_TRUE(harness.clipExport->exportRange(QUrl::fromLocalFile(targetPath)));
    EXPECT_TRUE(harness.clipExport->busy());
    EXPECT_TRUE(harness.waitUntil([&harness] { return !harness.clipExport->busy(); }));

    const auto jobs = harness.clipExporter->performedJobs();
    ASSERT_EQ(jobs.size(), 1U);
    EXPECT_EQ(jobs.front().sourcePath, sourcePaths.front());
    EXPECT_EQ(jobs.front().outputPath, std::filesystem::path{targetPath.toStdWString()});
    EXPECT_NE(jobs.front().requestId, application::kInvalidClipExportRequestId);
    // Frame 3 starts at 100 ms, but the only keyframe is at zero, so the copy starts there and the
    // plan records the pre-roll the dialog warns about. The end is the start of the frame after the
    // out point, kept exclusive.
    EXPECT_EQ(jobs.front().plan.startMicroseconds, 0);
    EXPECT_EQ(jobs.front().plan.startShiftMicroseconds, -100'000);
    ASSERT_TRUE(jobs.front().plan.endMicroseconds.has_value());
    EXPECT_EQ(*jobs.front().plan.endMicroseconds, 266'667);
    ASSERT_TRUE(jobs.front().plan.firstExportedFrame.has_value());
    EXPECT_EQ(jobs.front().plan.firstExportedFrame->value(), 0);
    EXPECT_EQ(jobs.front().plan.requestedFrameCount, 5);
    EXPECT_EQ(harness.clipExporter->queriedSourcePaths().size(), 1U);

    // The outcome lands once, in both channels the user can see.
    EXPECT_EQ(harness.root->property("immersiveHudText").toString(),
              QStringLiteral("已导出所选区间。"));
    EXPECT_EQ(harness.clipExport->lastStatus(), QStringLiteral("已导出所选区间。"));
    EXPECT_EQ(harness.clipExport->lastOutputPath(), QDir::toNativeSeparators(targetPath));
    EXPECT_TRUE(harness.clipExport->lastFailureDetail().isEmpty());
    EXPECT_DOUBLE_EQ(harness.clipExport->progress(), 1.0);
    // The chip returns to its idle label, and the dialog stayed open on the result so the user can
    // see where the file went.
    EXPECT_EQ(exportChip->property("chipText").toString(), QStringLiteral("导出"));
    EXPECT_FALSE(exportChip->property("chipActive").toBool());
    EXPECT_TRUE(popup->property("visible").toBool());
}

TEST(MainQmlContractTests, TransportRangeRowClearsTheRangeAndGatesChipsWithoutMedia) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    harness.shell->setRangeIn(41, static_cast<double>(harness.controller->mediaTimeForFrame(41)));
    harness.shell->setRangeOut(47, static_cast<double>(harness.controller->mediaTimeForFrame(47)));
    harness.settle();

    auto* const markIn = harness.root->findChild<QObject*>(QStringLiteral("transportMarkInButton"));
    auto* const playRange =
        harness.root->findChild<QObject*>(QStringLiteral("transportPlayRangeButton"));
    auto* const clearRange =
        harness.root->findChild<QObject*>(QStringLiteral("transportClearRangeButton"));
    auto* const label = harness.root->findChild<QObject*>(QStringLiteral("transportRangeLabel"));
    ASSERT_NE(markIn, nullptr);
    ASSERT_NE(playRange, nullptr);
    ASSERT_NE(clearRange, nullptr);
    ASSERT_NE(label, nullptr);

    ASSERT_TRUE(QMetaObject::invokeMethod(clearRange, "clicked"));
    harness.settle();
    EXPECT_EQ(harness.shell->inFrame(), -1);
    EXPECT_EQ(harness.shell->outFrame(), -1);
    EXPECT_EQ(label->property("text").toString(), QStringLiteral("未设区间（I / O 设点）"));
    EXPECT_FALSE(playRange->property("chipEnabled").toBool());
    EXPECT_FALSE(clearRange->property("chipEnabled").toBool());
    // Clearing the range must not strand the session in range playback.
    EXPECT_FALSE(harness.root->property("rangePlaybackActive").toBool());

    // Closing the session hides the transport and disables marking, so the row can never act on a
    // frame that does not exist.
    harness.snapshot->sessionState = domain::SessionState::kEmpty;
    harness.snapshot->sources.clear();
    harness.snapshot->presentedSources.clear();
    harness.snapshot->displayedFrame.reset();
    harness.snapshot->canonicalFrameCount = 0U;
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_TRUE(harness.root->property("transportHidden").toBool());
    EXPECT_FALSE(markIn->property("chipEnabled").toBool());
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

// C1. Resume must restore the recorded position and say so, but only for the exact
// path|size|mtime identity it was recorded under, and never past the end of a timeline that changed
// underneath the stored entry. A stale or foreign entry must leave the playhead at frame 0.
TEST(MainQmlContractTests, ResumeRestoresRecordedPositionAndStaysOffForeignOrStaleEntries) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    const std::filesystem::path pathA =
        std::filesystem::path(tempDir.path().toStdWString()) / "resume_sourceA.mp4";
    const std::filesystem::path pathB =
        std::filesystem::path(tempDir.path().toStdWString()) / "resume_sourceB.mp4";
    for (const std::filesystem::path& path : {pathA, pathB}) {
        QFile file{QString::fromStdWString(path.wstring())};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("resume-source-bytes", 19);
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

    const QString identity = canonicalSourceIdentity(QUrl::fromLocalFile(QString::fromStdWString(
        snapshot->validatedComparison->sources().front().descriptor.normalizedPath.wstring())));
    ASSERT_FALSE(identity.isEmpty());

    // Three entries, one per behaviour under test:
    //   identity  -> a real position inside a 12-frame timeline, must be restored;
    //   samePathWrongSize -> the same file at a different size/mtime, must be ignored;
    //   pastTheEnd -> inside this timeline's identity but beyond its last frame, must be ignored.
    std::map<std::string, std::string, std::less<>> seed{
        {QStringLiteral("resume.%1").arg(identity).toStdString(), "1700000000000:6"},
        {QStringLiteral("resume.c:/other/clip.mp4|1|2").toStdString(), "1700000000001:7"},
        {QStringLiteral("resume.%1").arg(QStringLiteral("%1|999").arg(identity)).toStdString(),
         "1700000000002:6"},
    };
    ReviewPreferencesController preferences{
        std::make_shared<SeededResumeSettingsRepository>(std::move(seed))};
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
    window->resize(1280, 800);
    window->show();
    for (int iteration = 0; iteration < 16; ++iteration) {
        QCoreApplication::processEvents();
    }

    // The controller is the source of truth for where the playhead actually is.
    const auto seekTargets = [&submitted] {
        std::vector<std::int64_t> targets;
        for (const application::PlaybackCommand& command : submitted) {
            if (const auto* const seek = std::get_if<application::SeekFrameCommand>(&command);
                seek != nullptr) {
                targets.push_back(seek->frameId.value());
            }
        }
        return targets;
    };

    EXPECT_EQ(controller.currentSourceIdentity(), identity);
    // The recorded position is requested exactly once, and only for the matching identity.
    const std::vector<std::int64_t> seeks = seekTargets();
    EXPECT_EQ(std::count(seeks.begin(), seeks.end(), std::int64_t{6}), 1)
        << "resume must seek to the recorded frame exactly once";
    EXPECT_EQ(root->property("resumeAttemptedIdentity").toString(), identity)
        << "resume must record which identity it already handled";
    EXPECT_GT(root->property("intentMessage").toString().size(), 0)
        << "a silent jump is indistinguishable from the app losing the user's place";

    // A stored position past the end must never be applied, even under the right identity.
    const auto staleSeek = [&controller] { return controller.currentFrame(); };
    preferences.rememberResumeFrame(QStringLiteral("%1|999").arg(identity), 9'000);
    controller.refreshProjection();
    for (int iteration = 0; iteration < 16; ++iteration) {
        QCoreApplication::processEvents();
    }
    EXPECT_LE(staleSeek(), 11) << "a resume past the end of the timeline must not be applied";
    preferences.stop();
    controller.stop();
}

// A4. The active-source strip is a plain Row with fixed-width chips, so it can neither wrap nor
// scroll. At Main.qml's declared 960 px minimum width three tagged chips plus the add button are
// wider than the panel, which would leave trailing chips off-stage and unreachable. This is
// measured before any fix: if the assertion passes on the current source, the backlog claim is
// wrong and the claim should be corrected rather than the code bent to match it.
TEST(MainQmlContractTests, ActiveSourceStripKeepsEveryChipReachableAtMinimumWidth) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    std::vector<std::filesystem::path> paths;
    for (int index = 0; index < 3; ++index) {
        paths.push_back(std::filesystem::path(tempDir.path().toStdWString()) /
                        QStringLiteral("strip_source_%1.mp4").arg(index).toStdWString());
        QFile file{QString::fromStdWString(paths.back().wstring())};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("strip-source-bytes", 18);
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
    window->resize(960, 640);
    window->show();
    for (int iteration = 0; iteration < 16; ++iteration) {
        QCoreApplication::processEvents();
    }
    ASSERT_EQ(controller.sourceCount(), 3);

    auto* const strip = root->findChild<QQuickItem*>(QStringLiteral("activeSourceStrip"));
    ASSERT_NE(strip, nullptr);
    ASSERT_TRUE(strip->isVisible());
    const QPointF stripTopLeft = strip->mapToItem(window->contentItem(), QPointF{0.0, 0.0});
    const QRectF stripRect{stripTopLeft.x(), stripTopLeft.y(), strip->width(), strip->height()};
    const QQuickItem* repeater =
        root->findChild<QQuickItem*>(QStringLiteral("activeSourceRepeater"));
    ASSERT_NE(repeater, nullptr);
    // A Repeater is itself a QQuickItem, but its delegates are parented to the Repeater's parent -
    // here the chip Row. Reading childItems() off the Repeater yields nothing.
    const QQuickItem* chipRow = repeater->parentItem();
    ASSERT_NE(chipRow, nullptr);

    // Every chip, plus the add button, must land inside the strip. This strip is the only place a
    // source can be removed or re-referenced from, so a chip pushed past the right edge is a source
    // the user cannot manage at all.
    const QList<QQuickItem*> chipItems = chipRow->childItems();
    // childItems() also yields the Repeater itself, a zero-sized bookkeeping item. QRectF::contains
    // rejects empty rectangles by definition, so an unmoved width-0 item would fail for a reason
    // that has nothing to do with reachability.
    std::vector<const QQuickItem*> paintedChips;
    for (const QQuickItem* item : chipItems) {
        if (item->width() > 0.0 && item->height() > 0.0) {
            paintedChips.push_back(item);
        }
    }
    // Three source chips plus the add button.
    ASSERT_GE(paintedChips.size(), 4);
    for (const QQuickItem* chip : paintedChips) {
        const QPointF topLeft = chip->mapToItem(window->contentItem(), QPointF{0.0, 0.0});
        const QRectF chipRect{topLeft.x(), topLeft.y(), chip->width(), chip->height()};
        EXPECT_TRUE(stripRect.contains(chipRect))
            << "chip is outside the strip at the 960 px minimum width (chip=" << chipRect.x() << ","
            << chipRect.y() << " " << chipRect.width() << "x" << chipRect.height()
            << " strip width=" << stripRect.width() << ")";
    }
    auto* const addButton = root->findChild<QQuickItem*>(QStringLiteral("addSourceChipButton"));
    ASSERT_NE(addButton, nullptr);
    const QPointF addTopLeft = addButton->mapToItem(window->contentItem(), QPointF{0.0, 0.0});
    const QRectF addRect{addTopLeft.x(), addTopLeft.y(), addButton->width(), addButton->height()};
    EXPECT_TRUE(stripRect.contains(addRect))
        << "the add-source button is outside the strip at the 960 px minimum width (button="
        << addRect.x() << "," << addRect.y() << " " << addRect.width() << "x" << addRect.height()
        << ")";
    controller.stop();
}

// C2. Reordering the strip must move the chips and nothing else. The chip carries its sourceId,
// which is what decides its A/B/C role, its Theme.sourceColor accent and its pairing, so if a
// reorder were to renumber a delegate the review would silently mean something different from the
// one the user set up. This watches the chips' geometry follow the shell's display order while
// every chip keeps the sourceId it was created with.
TEST(MainQmlContractTests, StripChipsFollowDisplayOrderWithoutChangingTheirSourceId) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    std::vector<std::filesystem::path> paths;
    for (int index = 0; index < 3; ++index) {
        paths.push_back(std::filesystem::path(tempDir.path().toStdWString()) /
                        QStringLiteral("order_source_%1.mp4").arg(index).toStdWString());
        QFile file{QString::fromStdWString(paths.back().wstring())};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("order-source-bytes", 18);
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
                        domain::FrameCountInfo{.value = 12,
                                               .origin = domain::FrameCountOrigin::kReported},
                    .duration = domain::MediaTime{400'000},
                    .codecId = "h264",
                    .pixelFormatId = "nv12",
                    .bitDepth = 8U,
                    .decodeCapabilities =
                        domain::DecodeCapabilities{.softwareDecode = true, .d3d11VaDecode = true},
                    .timingConfidence = domain::TimingConfidence::kVerifiedCfr,
                    .sourceIdentity =
                        domain::SourceFileIdentity{
                            .byteSize = static_cast<std::uint64_t>(info.size()),
                            .modifiedUtcMilliseconds = info.lastModified().toMSecsSinceEpoch(),
                            .fingerprintSha256 = std::string(64U, '0'),
                        },
                },
            .displayName = std::string{"Order "} + static_cast<char>('A' + index),
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
    window->resize(960, 640);
    window->show();
    for (int iteration = 0; iteration < 16; ++iteration) {
        QCoreApplication::processEvents();
    }
    ASSERT_EQ(controller.sourceCount(), 3);

    const QStringList openedIdentities = shell.activeSourceIdentities();
    ASSERT_EQ(openedIdentities.size(), 3);

    const auto collectChips = [&root] {
        QHash<QString, const QQuickItem*> byIdentity;
        QHash<QString, int> sourceIdsByIdentity;
        const QQuickItem* repeater =
            root->findChild<QQuickItem*>(QStringLiteral("activeSourceRepeater"));
        if (repeater == nullptr || repeater->parentItem() == nullptr) {
            return qMakePair(byIdentity, sourceIdsByIdentity);
        }
        for (const QQuickItem* item : repeater->parentItem()->childItems()) {
            if (item->width() <= 0.0 || item->height() <= 0.0) {
                continue;
            }
            const QVariant identity = item->property("resolvedSourceIdentity");
            if (!identity.isValid() || identity.toString().isEmpty()) {
                continue;
            }
            byIdentity.insert(identity.toString(), item);
            sourceIdsByIdentity.insert(identity.toString(), item->property("sourceId").toInt());
        }
        return qMakePair(byIdentity, sourceIdsByIdentity);
    };

    auto [chipsBefore, sourceIdsBefore] = collectChips();
    ASSERT_EQ(chipsBefore.size(), 3);
    for (const QString& identity : openedIdentities) {
        EXPECT_TRUE(chipsBefore.contains(identity))
            << "no chip for identity " << identity.toStdString();
    }

    // Move the last source to the head of the strip.
    ASSERT_TRUE(shell.moveSourceInDisplayOrder(2, 0));
    for (int iteration = 0; iteration < 8; ++iteration) {
        QCoreApplication::processEvents();
    }

    const QStringList displayIdentities = shell.displaySourceIdentities();
    ASSERT_EQ(displayIdentities.size(), 3);
    EXPECT_EQ(displayIdentities.at(0), openedIdentities.at(2));
    EXPECT_EQ(displayIdentities.at(1), openedIdentities.at(0));
    EXPECT_EQ(displayIdentities.at(2), openedIdentities.at(1));

    auto [chipsAfter, sourceIdsAfter] = collectChips();
    ASSERT_EQ(chipsAfter.size(), 3);
    // Compare positions pairwise along the display order rather than looking for a single minimum.
    // If the strip never positioned its chips they would all sit at x = 0, and "the smallest x"
    // would then be the first entry of the order this loop walks - a strip that ignored the order
    // entirely would pass. The first mutation of this test relied on exactly that hole.
    std::vector<double> positionsAlongOrder;
    std::vector<int> reportedDisplayIndexes;
    positionsAlongOrder.reserve(displayIdentities.size());
    reportedDisplayIndexes.reserve(displayIdentities.size());
    for (const QString& identity : displayIdentities) {
        const QQuickItem* chip = chipsAfter.value(identity, nullptr);
        ASSERT_NE(chip, nullptr) << "no chip for identity " << identity.toStdString();
        positionsAlongOrder.push_back(chip->x());
        // -1 here means the strip's displayOrder never reached this chip, which is a different
        // failure from "the order arrived but the chips were not moved".
        reportedDisplayIndexes.push_back(chip->property("displayIndex").toInt());
    }
    EXPECT_GT(positionsAlongOrder.front(), 0.0) << "the strip never positioned its chips";
    for (std::size_t index = 1U; index < positionsAlongOrder.size(); ++index) {
        EXPECT_GT(positionsAlongOrder[index], positionsAlongOrder[index - 1U])
            << "chip " << index
            << " does not sit right of its predecessor; the strip did not follow "
               "the display order (x =";
        for (const double position : positionsAlongOrder) {
            std::cerr << ' ' << position;
        }
        std::cerr << " ; displayIndex =";
        for (const int reported : reportedDisplayIndexes) {
            std::cerr << ' ' << reported;
        }
        std::cerr << ')';
    }
    for (const QString& identity : openedIdentities) {
        EXPECT_EQ(chipsAfter.value(identity, nullptr), chipsBefore.value(identity, nullptr))
            << "a reorder replaced the chip instance for " << identity.toStdString();
        EXPECT_EQ(sourceIdsAfter.value(identity), sourceIdsBefore.value(identity))
            << "a reorder renumbered a chip's sourceId, which would change its role and accent "
               "colour";
    }

    // Multi-select: the batch action only appears once something is selected, and the shell still
    // refuses to act on media truth when the selection changes.
    auto* const removeSelectedButton =
        root->findChild<QQuickItem*>(QStringLiteral("removeSelectedChipButton"));
    ASSERT_NE(removeSelectedButton, nullptr);
    EXPECT_FALSE(removeSelectedButton->isVisible())
        << "a batch action is offered with no selection";
    // The review publishes its own commands while it settles, so compare against the count
    // taken here rather than against zero.
    const std::size_t submittedBeforeSelection = submitted.size();
    shell.toggleSourceSelection(openedIdentities.at(1));
    for (int iteration = 0; iteration < 4; ++iteration) {
        QCoreApplication::processEvents();
    }
    EXPECT_TRUE(removeSelectedButton->isVisible());
    EXPECT_EQ(shell.selectedSourceIdentities(), QStringList{openedIdentities.at(1)});
    // Selection is a view concern: it must not have reached media truth.
    EXPECT_EQ(submitted.size(), submittedBeforeSelection)
        << "selecting a chip submitted a media command";
    controller.stop();
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

    // The panel owns a visible close action even when the outer comparison toolbar is absent.
    auto* const closeButton = root->findChild<QQuickItem*>(QStringLiteral("inspectorCloseButton"));
    auto* const tabBar = root->findChild<QObject*>(QStringLiteral("inspectorTabBar"));
    ASSERT_NE(closeButton, nullptr);
    ASSERT_NE(tabBar, nullptr);
    EXPECT_EQ(closeButton->size(), QSizeF(34.0, 34.0));
    EXPECT_DOUBLE_EQ(inspectorItem->width(), 300.0);
    EXPECT_EQ(QQmlProperty(closeButton, QStringLiteral("Accessible.name"), qmlContext(closeButton))
                  .read()
                  .toString(),
              QStringLiteral("关闭检查器"));
    ASSERT_TRUE(tabBar->setProperty("currentIndex", 2));
    processLayout();
    const auto captureCloseControl = [&](const QString& name) {
        const QString directory = qEnvironmentVariable("VCS_INSPECTOR_CLOSE_EVIDENCE_DIR");
        if (!directory.isEmpty()) {
            window->requestUpdate();
            processLayout();
            EXPECT_TRUE(window->grabWindow().save(QDir{directory}.filePath(name)));
        }
    };
    const auto expectClosedWithViewerFocus = [&] {
        processLayout();
        EXPECT_FALSE(shell.inspectorVisible());
        EXPECT_TRUE(viewportItem->hasActiveFocus());
        EXPECT_EQ(tabBar->property("currentIndex").toInt(), 2);
        EXPECT_EQ(tabBar->property("count").toInt(), 3);
    };

    // Multi-source drawer: click the actual panel button, then preserve the selected Info tab.
    ASSERT_TRUE(root->property("drawerMode").toBool());
    captureCloseControl(QStringLiteral("multi-drawer-close-button.png"));
    const QPointF closePoint = closeButton->mapToItem(
        window->contentItem(), QPointF{closeButton->width() / 2.0, closeButton->height() / 2.0});
    sendMousePress(*window, closePoint);
    sendMouseRelease(*window, closePoint);
    expectClosedWithViewerFocus();

    // Reuse the real controller with a validated single-source snapshot. No media is opened.
    auto single = domain::ComparisonValidator::validate(
        std::vector<domain::ComparisonSource>{snapshot->validatedComparison->sources().front()});
    ASSERT_TRUE(single);
    snapshot->validatedComparison =
        std::make_shared<const domain::ValidatedComparisonSet>(std::move(single).value().set);
    snapshot->sources.resize(1);
    snapshot->presentedSources.resize(1);
    controller.refreshProjection();
    window->hide();
    processLayout();
    // Make the docked precondition explicit for this fixture, even on a small desktop.
    // This changes only the test window, not Main.qml's production minimum width.
    window->setMinimumWidth(1120);
    window->resize(1120, 640);
    processLayout();
    layoutRoot->setSize(QSizeF{1120.0, 640.0});
    window->show();
    window->requestActivate();
    shell.setInspectorVisible(true);
    processLayout();
    ASSERT_EQ(root->property("sourceCount").toInt(), 1);
    ASSERT_FALSE(root->property("drawerMode").toBool());
    EXPECT_TRUE(closeButton->isVisible());
    EXPECT_EQ(tabBar->property("currentIndex").toInt(), 2);
    captureCloseControl(QStringLiteral("single-docked-close-button.png"));
    const QPointF singleClosePoint = closeButton->mapToItem(
        window->contentItem(), QPointF{closeButton->width() / 2.0, closeButton->height() / 2.0});
    sendMousePress(*window, singleClosePoint);
    sendMouseRelease(*window, singleClosePoint);
    expectClosedWithViewerFocus();
    shell.setInspectorVisible(true);
    processLayout();
    closeButton->forceActiveFocus(Qt::TabFocusReason);
    processLayout();
    EXPECT_FALSE(root->property("globalMediaShortcutsEnabled").toBool());
    const auto commandsBeforeClose = submitted.size();
    sendKey(*window, Qt::Key_Space);
    expectClosedWithViewerFocus();
    EXPECT_EQ(submitted.size(), commandsBeforeClose);
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

// Step-2 acceptance "找到一次缺陷后，切对象不用重新定位": with three sources and a fixed
// reference (GT), switching the candidate must flip the active pair between the two
// reference-anchored edges while the frame, zoom/pan and the wipe split stay untouched.
// Entering from a prediction-vs-prediction pair keeps the displayed primary slot and brings
// GT in; the C shortcut drives the same switch as the toolbar button.
TEST(MainQmlContractTests, SwitchingCandidateKeepsReferenceAnchoredPairAndObservation) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    const std::array<std::filesystem::path, 3> sourcePaths = {
        std::filesystem::path(tempDir.path().toStdWString()) / "predA.mp4",
        std::filesystem::path(tempDir.path().toStdWString()) / "groundTruth.mp4",
        std::filesystem::path(tempDir.path().toStdWString()) / "predC.mp4",
    };
    for (const auto& path : sourcePaths) {
        QFile file{QString::fromStdWString(path.wstring())};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(QByteArray("switch-bytes-") + path.filename().string().c_str());
    }

    const auto rateResult = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rateResult);
    const domain::RationalRate rate = rateResult.value();

    std::vector<domain::ComparisonSource> comparisonSources;
    for (std::size_t index = 0U; index < sourcePaths.size(); ++index) {
        const QFileInfo info{QString::fromStdWString(sourcePaths[index].wstring())};
        comparisonSources.push_back(domain::ComparisonSource{
            .id = static_cast<domain::SourceId>(index),
            // Slot 1 is the reference (GT); slots 0 and 2 are the two predictions.
            .role = index == 1U ? domain::ComparisonRole::kReference
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
    // Start on the GT-anchored pair {predA(0), GT(1)} — renderer edge ordinal 0.
    snapshot->activeComparisonPair = domain::ComparisonPair{
        .first = domain::SourceId{0U},
        .second = domain::SourceId{1U},
    };

    std::vector<application::PlaybackCommand> submitted;
    ReviewController controller{
        ReviewController::Dependencies{
            // The fake session applies the pair selection to the snapshot, exactly like
            // the application layer would, so the projection reflects each switch.
            .submit =
                [&submitted, &snapshot](application::PlaybackCommand command) {
                    if (const auto* const pairCommand =
                            std::get_if<application::SetActiveComparisonPairCommand>(&command);
                        pairCommand != nullptr && pairCommand->pair.has_value()) {
                        snapshot->activeComparisonPair = *pairCommand->pair;
                    }
                    submitted.push_back(std::move(command));
                    return application::PortSubmitResult::Accepted;
                },
            .snapshot = [snapshot] { return snapshot; },
            .takeCompletedCommands = [] { return std::vector<application::CommandTerminal>{}; },
        },
    };
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
    window->resize(1440, 900);
    window->show();
    QCoreApplication::processEvents();

    ASSERT_EQ(root->property("sourceCount").toInt(), 3);
    EXPECT_EQ(root->property("referenceSourceIndex").toInt(), 1);
    EXPECT_EQ(root->property("differenceEdge").toInt(), ComparisonSurface::Edge0And1);

    auto* const switchButton =
        root->findChild<QQuickItem*>(QStringLiteral("switchCandidateButton"));
    ASSERT_NE(switchButton, nullptr);
    EXPECT_TRUE(switchButton->isVisible());
    auto* const surfaceItem = root->findChild<QQuickItem*>(QStringLiteral("dualVideoSurface"));
    ASSERT_NE(surfaceItem, nullptr);
    auto* const surface = qobject_cast<ComparisonSurface*>(surfaceItem);
    ASSERT_NE(surface, nullptr);
    auto* const viewport = root->findChild<QQuickItem*>(QStringLiteral("mediaViewportFocusTarget"));
    ASSERT_NE(viewport, nullptr);

    // Locate a defect: zoom in and park the wipe split somewhere specific.
    surface->zoomAt(0.5, 0.5, 2.0);
    const qreal zoomedScale = surface->viewScale();
    EXPECT_GT(zoomedScale, 1.0);
    root->setProperty("wipePosition", 0.3);
    QCoreApplication::processEvents();
    const qreal parkedWipe = root->property("wipePosition").toDouble();
    EXPECT_DOUBLE_EQ(parkedWipe, 0.3);

    // Switch the candidate: the pair flips to the other GT-anchored edge {GT(1), predC(2)}
    // (edge ordinal 2) while the zoom and the split stay exactly where they were.
    ASSERT_TRUE(QMetaObject::invokeMethod(switchButton, "clicked"));
    controller.refreshProjection();
    QCoreApplication::processEvents();
    EXPECT_EQ(root->property("differenceEdge").toInt(), ComparisonSurface::Edge1And2);
    EXPECT_DOUBLE_EQ(surface->viewScale(), zoomedScale);
    EXPECT_DOUBLE_EQ(root->property("wipePosition").toDouble(), parkedWipe);
    ASSERT_FALSE(submitted.empty());
    EXPECT_NE(std::get_if<application::SetActiveComparisonPairCommand>(&submitted.back()), nullptr);

    // Switch back: the pair returns to {predA(0), GT(1)} with the observation intact.
    ASSERT_TRUE(QMetaObject::invokeMethod(switchButton, "clicked"));
    controller.refreshProjection();
    QCoreApplication::processEvents();
    EXPECT_EQ(root->property("differenceEdge").toInt(), ComparisonSurface::Edge0And1);
    EXPECT_DOUBLE_EQ(surface->viewScale(), zoomedScale);
    EXPECT_DOUBLE_EQ(root->property("wipePosition").toDouble(), parkedWipe);

    // Entering from a prediction-vs-prediction pair {predA(0), predC(2)}: the switch keeps
    // the currently displayed primary slot (predA) and brings GT in on the anchored edge.
    snapshot->activeComparisonPair = domain::ComparisonPair{
        .first = domain::SourceId{0U},
        .second = domain::SourceId{2U},
    };
    controller.refreshProjection();
    QCoreApplication::processEvents();
    EXPECT_EQ(root->property("differenceEdge").toInt(), ComparisonSurface::Edge0And2);
    ASSERT_TRUE(QMetaObject::invokeMethod(switchButton, "clicked"));
    controller.refreshProjection();
    QCoreApplication::processEvents();
    EXPECT_EQ(root->property("differenceEdge").toInt(), ComparisonSurface::Edge0And1);
    EXPECT_DOUBLE_EQ(surface->viewScale(), zoomedScale);

    // The keyboard path (C) drives the same switch with the observation still intact.
    viewport->forceActiveFocus();
    QCoreApplication::processEvents();
    EXPECT_TRUE(root->property("globalMediaShortcutsEnabled").toBool());
    sendKey(*window, Qt::Key_C);
    controller.refreshProjection();
    QCoreApplication::processEvents();
    EXPECT_EQ(root->property("differenceEdge").toInt(), ComparisonSurface::Edge1And2);
    EXPECT_DOUBLE_EQ(surface->viewScale(), zoomedScale);
    EXPECT_DOUBLE_EQ(root->property("wipePosition").toDouble(), parkedWipe);
}

// Step-2 difference entry: the 差异 button offers the two flavors the review named —
// overlay highlight and pure diff — in one dropdown, and while Difference mode is active
// a hold-to-peek button toggles the surface's differenceSuppressed state (raw first
// source instead of the difference pass) without any mode or metric change.
TEST(MainQmlContractTests, DifferenceButtonOffersFlavorsAndPeekTogglesSuppression) {
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
    ReviewController controller{
        ReviewController::Dependencies{
            .submit =
                [&submitted](application::PlaybackCommand command) {
                    submitted.push_back(std::move(command));
                    return application::PortSubmitResult::Accepted;
                },
            .snapshot = [snapshot] { return snapshot; },
            .takeCompletedCommands = [] { return std::vector<application::CommandTerminal>{}; },
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

    auto* const diffButton = root->findChild<QQuickItem*>(QStringLiteral("diffModeButton"));
    ASSERT_NE(diffButton, nullptr);
    EXPECT_TRUE(diffButton->isVisible());
    auto* const highlightItem =
        root->findChild<QQuickItem*>(QStringLiteral("diffHighlightMenuItem"));
    auto* const pureItem = root->findChild<QQuickItem*>(QStringLiteral("diffPureMenuItem"));
    ASSERT_NE(highlightItem, nullptr);
    ASSERT_NE(pureItem, nullptr);
    auto* const peekButton = root->findChild<QQuickItem*>(QStringLiteral("differencePeekButton"));
    ASSERT_NE(peekButton, nullptr);
    auto* const surfaceItem = root->findChild<QQuickItem*>(QStringLiteral("dualVideoSurface"));
    ASSERT_NE(surfaceItem, nullptr);

    // Side-by-side: the peek button is hidden; the diff button shows the plain label.
    EXPECT_EQ(root->property("effectiveViewMode").toInt(), ComparisonSurface::SideBySide);
    EXPECT_FALSE(peekButton->isVisible());
    EXPECT_FALSE(surfaceItem->property("differenceSuppressed").toBool());

    // Overlay highlight: enters Difference mode with the Highlight metric, and the
    // button names the active flavor.
    ASSERT_TRUE(QMetaObject::invokeMethod(highlightItem, "triggered"));
    QCoreApplication::processEvents();
    EXPECT_EQ(static_cast<int>(preferences.viewMode()),
              static_cast<int>(ReviewPreferencesController::ViewMode::Difference));
    EXPECT_EQ(static_cast<int>(preferences.differenceMetric()),
              static_cast<int>(ComparisonSurface::Highlight));
    EXPECT_EQ(root->property("effectiveViewMode").toInt(), ComparisonSurface::Difference);
    EXPECT_TRUE(diffButton->property("text").toString().contains(QStringLiteral("叠加高亮")));
    EXPECT_TRUE(peekButton->isVisible());

    // Hold-to-peek: pressed suppresses the difference pass on the surface; released
    // restores it — no mode or metric change in between.
    ASSERT_TRUE(QMetaObject::invokeMethod(peekButton, "pressed"));
    QCoreApplication::processEvents();
    EXPECT_TRUE(surfaceItem->property("differenceSuppressed").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(peekButton, "released"));
    QCoreApplication::processEvents();
    EXPECT_FALSE(surfaceItem->property("differenceSuppressed").toBool());
    EXPECT_EQ(static_cast<int>(preferences.differenceMetric()),
              static_cast<int>(ComparisonSurface::Highlight));
    EXPECT_EQ(root->property("effectiveViewMode").toInt(), ComparisonSurface::Difference);

    // Pure diff: same mode, RgbAbsolute metric, flavor label follows.
    ASSERT_TRUE(QMetaObject::invokeMethod(pureItem, "triggered"));
    QCoreApplication::processEvents();
    EXPECT_EQ(static_cast<int>(preferences.differenceMetric()),
              static_cast<int>(ComparisonSurface::RgbAbsolute));
    EXPECT_TRUE(diffButton->property("text").toString().contains(QStringLiteral("纯差异图")));

    // Leaving Difference mode hides the peek button again.
    preferences.setViewMode(ReviewPreferencesController::ViewMode::SideBySide);
    QCoreApplication::processEvents();
    EXPECT_FALSE(peekButton->isVisible());
}

// Step-2 comparison export: the composed capture is the exact viewport crop plus a
// neutral-gray caption bar (true grays — an exported image must not tint color judgment).
TEST(MainQmlContractTests, ComparisonExportComposesCaptionBarInNeutralGray) {
    QImage windowCapture(64, 32, QImage::Format_RGBA8888);
    windowCapture.fill(QColor{200, 40, 30});
    const QRect viewportRect{8, 4, 48, 24};
    const QStringList caption{QStringLiteral("GT:a.mp4 / P:b.mp4"),
                              QStringLiteral("split - frame 12")};

    const QImage composed = ComparisonExportController::composeLabeledCapture(
        windowCapture, viewportRect, caption, 1.0);
    ASSERT_FALSE(composed.isNull());
    EXPECT_EQ(composed.width(), 48);
    const int barHeight = ComparisonExportController::captionBarHeight(2, 1.0);
    EXPECT_GT(barHeight, 0);
    EXPECT_EQ(composed.height(), 24 + barHeight);

    // The viewport part is the exact crop of the source capture.
    EXPECT_EQ(composed.pixelColor(24, 12), QColor(200, 40, 30));

    // The bar background is true neutral gray and clearly darker than the content.
    const QColor bar = composed.pixelColor(4, 24 + barHeight / 2);
    EXPECT_EQ(bar.red(), bar.green());
    EXPECT_EQ(bar.green(), bar.blue());
    EXPECT_LT(bar.lightness(), 64);

    // The elided caption text actually drew ink into the bar.
    int inkPixels = 0;
    for (int y = 24; y < composed.height(); ++y) {
        for (int x = 0; x < composed.width(); ++x) {
            if (composed.pixelColor(x, y).lightness() > bar.lightness() + 40) {
                ++inkPixels;
            }
        }
    }
    EXPECT_GT(inkPixels, 20);

    // An empty caption returns the bare crop; an empty crop returns a null image.
    EXPECT_EQ(
        ComparisonExportController::composeLabeledCapture(windowCapture, viewportRect, {}, 1.0)
            .height(),
        24);
    EXPECT_TRUE(ComparisonExportController::composeLabeledCapture(
                    windowCapture, QRect{100, 100, 10, 10}, caption, 1.0)
                    .isNull());
}

// Step-2 comparison export wiring: the toolbar button composes the labeled viewport
// capture onto the clipboard, and the controller saves the same composition as a PNG.
TEST(MainQmlContractTests, CopyComparisonButtonPutsLabeledViewportOnClipboard) {
    auto snapshot = std::make_shared<application::SessionSnapshot>();
    snapshot->graphicsReady = true;
    snapshot->sessionState = domain::SessionState::kReady;
    snapshot->playbackState = domain::PlaybackState::kPaused;
    snapshot->displayedFrame = domain::FrameId{11};
    snapshot->canonicalFrameCount = 12U;
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
            .sourceFrameId = domain::FrameId{11},
            .matchKind = application::FrameMatchKind::ExactIndex,
        },
        application::PresentedSourceState{
            .sourceId = 1U,
            .sourceFrameId = domain::FrameId{11},
            .matchKind = application::FrameMatchKind::ExactIndex,
        },
    };
    std::vector<application::PlaybackCommand> submitted;
    ReviewController controller{
        ReviewController::Dependencies{
            .submit =
                [&submitted](application::PlaybackCommand command) {
                    submitted.push_back(std::move(command));
                    return application::PortSubmitResult::Accepted;
                },
            .snapshot = [snapshot] { return snapshot; },
            .takeCompletedCommands = [] { return std::vector<application::CommandTerminal>{}; },
        },
    };
    ReviewPreferencesController preferences{std::make_shared<ClosedSettingsRepository>()};
    preferences.setViewMode(ReviewPreferencesController::ViewMode::Wipe);
    ReviewShellController shell{controller, preferences};
    ReviewSessionFacade facade{controller, preferences, shell};
    ComparisonExportController exportController;

    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    engine.rootContext()->setContextProperty(QStringLiteral("reviewController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewPreferences"), &preferences);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewSession"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewFacade"), &facade);
    engine.rootContext()->setContextProperty(QStringLiteral("comparisonExport"), &exportController);
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);

    std::unique_ptr<QObject> root{component.create()};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);
    window->resize(1440, 900);
    window->show();
    QCoreApplication::processEvents();

    auto* const copyButton = root->findChild<QQuickItem*>(QStringLiteral("copyComparisonButton"));
    ASSERT_NE(copyButton, nullptr);
    EXPECT_TRUE(copyButton->isVisible());
    auto* const viewport = root->findChild<QQuickItem*>(QStringLiteral("mediaViewportFocusTarget"));
    ASSERT_NE(viewport, nullptr);

    // A sentinel makes the clipboard state deterministic before the copy. The Windows
    // OLE clipboard completes asynchronously, so pump events between writes and retry
    // the copy when another process briefly holds the clipboard.
    QImage sentinel(3, 3, QImage::Format_RGBA8888);
    sentinel.fill(Qt::black);
    QGuiApplication::clipboard()->setImage(sentinel);
    for (int iteration = 0; iteration < 10; ++iteration) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10U);
    }

    QImage pasted;
    for (int attempt = 0; attempt < 3 && pasted.isNull(); ++attempt) {
        ASSERT_TRUE(QMetaObject::invokeMethod(copyButton, "clicked"));
        for (int iteration = 0; iteration < 10; ++iteration) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(10U);
        }
        pasted = QGuiApplication::clipboard()->image();
    }
    ASSERT_FALSE(pasted.isNull());
    // grabWindow() yields nothing on a desktop that cannot render, and the production
    // path reports exactly that instead of copying: the self-hosted runner hit this
    // between attached sessions (run 37719185877) while the same test passed whenever
    // a session was attached. Skip only on that reported capture failure; a reported
    // success with the sentinel still on the clipboard stays a hard failure.
    if (exportController.lastStatus().contains(QStringLiteral("无法抓取当前画面"))) {
        GTEST_SKIP() << "window capture unavailable on this desktop: "
                     << exportController.lastStatus().toStdString();
    }
    EXPECT_NE(pasted.size(), QSize(3, 3));
    // The capture is the viewport's device-pixel rect plus the caption bar.
    const qreal devicePixelRatio = window->devicePixelRatio();
    EXPECT_NEAR(pasted.width(), viewport->width() * devicePixelRatio, 3.0);
    EXPECT_GT(pasted.height(), qRound(viewport->height() * devicePixelRatio));
    EXPECT_TRUE(exportController.lastStatus().contains(QStringLiteral("已复制")))
        << exportController.lastStatus().toStdString();

    // The same composition saves transactionally as a PNG file.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("comparison.png"));
    ASSERT_TRUE(
        exportController.saveComparison(viewport,
                                        QStringList{QStringLiteral("A"), QStringLiteral("B")},
                                        QUrl::fromLocalFile(path)));
    QFile saved{path};
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    const QByteArray bytes = saved.readAll();
    ASSERT_GT(bytes.size(), 1000);
    // PNG magic: the saved file is a real PNG.
    EXPECT_EQ(static_cast<std::uint8_t>(bytes[0]), 0x89U);
    EXPECT_EQ(bytes[1], 'P');
    EXPECT_EQ(bytes[2], 'N');
    EXPECT_EQ(bytes[3], 'G');
    EXPECT_TRUE(exportController.lastStatus().contains(QStringLiteral("已保存")))
        << exportController.lastStatus().toStdString();
}

// Step-3 image editing: entering edit mode shows the working copy in the pane, a crop drag
// in viewport coordinates becomes an image-pixel crop, undo/redo walk it, and the committed
// original buffer stays untouched.
TEST(MainQmlContractTests, ImageEditModeCropsAWorkingCopyAndKeepsTheOriginal) {
    WorkspaceHarness harness;
    harness.withImageEdit = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    QImage primary(64, 48, QImage::Format_ARGB32);
    primary.fill(QColor(180, 40, 60, 200));
    QImage secondary(32, 32, QImage::Format_ARGB32);
    secondary.fill(QColor(20, 160, 90));
    ASSERT_TRUE(harness.imageReview.openPairImages(std::move(primary),
                                                   QStringLiteral("a.png"),
                                                   std::move(secondary),
                                                   QStringLiteral("b.png")));
    ASSERT_TRUE(harness.activateWorkspace(1));
    harness.settle();

    auto* const workspace =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
    ASSERT_NE(workspace, nullptr);
    auto* const startButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditStartButton"));
    ASSERT_NE(startButton, nullptr);
    EXPECT_TRUE(startButton->isVisible());
    auto* const applyCrop =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditApplyCropButton"));
    auto* const undoButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditUndoButton"));
    auto* const redoButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditRedoButton"));
    ASSERT_NE(applyCrop, nullptr);
    ASSERT_NE(undoButton, nullptr);
    ASSERT_NE(redoButton, nullptr);
    auto* const viewport = harness.root->findChild<QQuickItem*>(QStringLiteral("primaryViewport"));
    auto* const preview = harness.root->findChild<QQuickItem*>(QStringLiteral("imageViewport-2"));
    ASSERT_NE(viewport, nullptr);
    ASSERT_NE(preview, nullptr);

    // Entering edit mode starts the session from the committed original.
    ASSERT_TRUE(QMetaObject::invokeMethod(startButton, "clicked"));
    harness.settle();
    EXPECT_TRUE(harness.imageEdit.active());
    EXPECT_TRUE(workspace->property("editModeActive").toBool());
    EXPECT_EQ(harness.imageEdit.imageWidth(), 64);
    EXPECT_EQ(harness.imageEdit.imageHeight(), 48);
    EXPECT_TRUE(applyCrop->isVisible());
    EXPECT_FALSE(undoButton->property("enabled").toBool());
    // The pane now serves the working copy through the edit provider.
    const QString editedSource = preview->property("source").toString();
    EXPECT_TRUE(editedSource.startsWith(QStringLiteral("image://dvs-edit/")))
        << editedSource.toStdString();

    // A drag in viewport coordinates maps to an image-pixel selection.
    const qreal scale =
        preview->width() / static_cast<qreal>(preview->property("sourceSize").toSize().width());
    ASSERT_GT(scale, 0.0);
    viewport->setProperty("cropStart", QPointF{preview->x() + 8 * scale, preview->y() + 6 * scale});
    viewport->setProperty("cropCurrent",
                          QPointF{preview->x() + 40 * scale, preview->y() + 30 * scale});
    ASSERT_TRUE(QMetaObject::invokeMethod(
        workspace,
        "updateCropSelection",
        Q_ARG(QVariant, QVariant::fromValue(static_cast<QObject*>(viewport)))));
    const QVariantMap selection = workspace->property("cropSelection").toMap();
    ASSERT_FALSE(selection.isEmpty());
    EXPECT_NEAR(selection.value(QStringLiteral("x")).toInt(), 8, 1);
    EXPECT_NEAR(selection.value(QStringLiteral("y")).toInt(), 6, 1);
    EXPECT_NEAR(selection.value(QStringLiteral("width")).toInt(), 32, 1);
    EXPECT_NEAR(selection.value(QStringLiteral("height")).toInt(), 24, 1);
    EXPECT_TRUE(applyCrop->property("enabled").toBool());

    // Applying the crop edits the working copy only.
    ASSERT_TRUE(QMetaObject::invokeMethod(applyCrop, "clicked"));
    harness.settle();
    EXPECT_EQ(harness.imageEdit.imageWidth(), 32);
    EXPECT_EQ(harness.imageEdit.imageHeight(), 24);
    EXPECT_TRUE(harness.imageEdit.dirty());
    EXPECT_TRUE(undoButton->property("enabled").toBool());
    EXPECT_TRUE(workspace->property("cropSelection").toMap().isEmpty());
    EXPECT_EQ(harness.imageReview.rawImageForSlot(ImageReviewController::PrimarySlot).size(),
              QSize(64, 48));

    // Undo/redo walk the crop while the original keeps its pixels.
    ASSERT_TRUE(QMetaObject::invokeMethod(undoButton, "clicked"));
    harness.settle();
    EXPECT_EQ(harness.imageEdit.imageWidth(), 64);
    EXPECT_EQ(harness.imageEdit.imageHeight(), 48);
    EXPECT_EQ(harness.imageReview.rawImageForSlot(ImageReviewController::PrimarySlot).size(),
              QSize(64, 48));
    ASSERT_TRUE(QMetaObject::invokeMethod(redoButton, "clicked"));
    harness.settle();
    EXPECT_EQ(harness.imageEdit.imageWidth(), 32);

    // Leaving edit mode drops the session and restores the committed pane source.
    ASSERT_TRUE(QMetaObject::invokeMethod(startButton, "clicked"));
    harness.settle();
    EXPECT_FALSE(harness.imageEdit.active());
    EXPECT_FALSE(workspace->property("editModeActive").toBool());
    EXPECT_TRUE(
        preview->property("source").toString().startsWith(QStringLiteral("image://vcs-review/")))
        << preview->property("source").toString().toStdString();
}

// Step-3 second increment: the brush paints through the workspace in image coordinates and
// the mosaic obscures a selected region; both share the same bounded undo history as crop.
TEST(MainQmlContractTests, ImageEditBrushAndMosaicToolsEditTheWorkingCopy) {
    WorkspaceHarness harness;
    harness.withImageEdit = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    QImage primary(64, 48, QImage::Format_ARGB32);
    primary.fill(QColor(10, 20, 30));
    QImage secondary(32, 32, QImage::Format_ARGB32);
    secondary.fill(QColor(40, 40, 40));
    ASSERT_TRUE(harness.imageReview.openPairImages(std::move(primary),
                                                   QStringLiteral("a.png"),
                                                   std::move(secondary),
                                                   QStringLiteral("b.png")));
    ASSERT_TRUE(harness.activateWorkspace(1));
    harness.settle();

    auto* const workspace =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
    auto* const viewport = harness.root->findChild<QQuickItem*>(QStringLiteral("primaryViewport"));
    auto* const preview = harness.root->findChild<QQuickItem*>(QStringLiteral("imageViewport-2"));
    auto* const startButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditStartButton"));
    auto* const brushTool =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditToolBrushButton"));
    auto* const applyButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditApplyCropButton"));
    auto* const undoButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditUndoButton"));
    auto* const brushOpacitySlider =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditBrushOpacitySlider"));
    ASSERT_NE(workspace, nullptr);
    ASSERT_NE(viewport, nullptr);
    ASSERT_NE(preview, nullptr);
    ASSERT_NE(startButton, nullptr);
    ASSERT_NE(brushTool, nullptr);
    ASSERT_NE(applyButton, nullptr);
    ASSERT_NE(undoButton, nullptr);
    ASSERT_NE(brushOpacitySlider, nullptr);

    ASSERT_TRUE(QMetaObject::invokeMethod(startButton, "clicked"));
    harness.settle();

    // Mosaic now lives behind 「更多工具」; the menu must open before its row exists.
    auto* const mosaicTool =
        openEditToolsItem(harness.root.get(), QStringLiteral("imageEditToolMosaicButton"));
    ASSERT_NE(mosaicTool, nullptr);
    // The dropdown button names the active tool, so the selection is not hidden.
    auto* const moreToolsButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditMoreToolsButton"));
    ASSERT_NE(moreToolsButton, nullptr);
    EXPECT_TRUE(moreToolsButton->property("text").toString().contains(QStringLiteral("裁剪")));

    // Brush tool: the tool switch changes what the left button does and shows its controls.
    ASSERT_TRUE(QMetaObject::invokeMethod(brushTool, "clicked"));
    harness.settle();
    EXPECT_EQ(workspace->property("editTool").toString(), QStringLiteral("brush"));
    EXPECT_TRUE(brushOpacitySlider->isVisible());
    EXPECT_FALSE(applyButton->isVisible());

    const qreal scale =
        preview->width() / static_cast<qreal>(preview->property("sourceSize").toSize().width());
    ASSERT_GT(scale, 0.0);
    const QVariant viewportArgument = QVariant::fromValue(static_cast<QObject*>(viewport));
    // The workspace functions take (view, mouseX, mouseY) — two numbers, not a point.
    const auto toViewportX = [&](const qreal imageX) {
        return QVariant{preview->x() + (imageX * scale)};
    };
    const auto toViewportY = [&](const qreal imageY) {
        return QVariant{preview->y() + (imageY * scale)};
    };
    const auto toViewportPoint = [&](const qreal imageX, const qreal imageY) {
        return QPointF{preview->x() + (imageX * scale), preview->y() + (imageY * scale)};
    };
    ASSERT_TRUE(QMetaObject::invokeMethod(workspace,
                                          "beginBrushStroke",
                                          Q_ARG(QVariant, viewportArgument),
                                          Q_ARG(QVariant, toViewportX(10)),
                                          Q_ARG(QVariant, toViewportY(10))));
    EXPECT_TRUE(harness.imageEdit.strokeActive());
    ASSERT_TRUE(QMetaObject::invokeMethod(workspace,
                                          "continueBrushStroke",
                                          Q_ARG(QVariant, viewportArgument),
                                          Q_ARG(QVariant, toViewportX(40)),
                                          Q_ARG(QVariant, toViewportY(10))));
    ASSERT_TRUE(QMetaObject::invokeMethod(workspace, "endBrushStroke"));
    harness.settle();
    EXPECT_FALSE(harness.imageEdit.strokeActive());

    const QImage painted = harness.imageEdit.editedImage();
    const QColor strokePixel = painted.pixelColor(25, 10);
    EXPECT_GT(strokePixel.red(), 200);
    EXPECT_LT(strokePixel.green(), 90);
    EXPECT_EQ(painted.pixelColor(25, 40), QColor(10, 20, 30));
    EXPECT_TRUE(undoButton->property("enabled").toBool());

    // Mosaic tool: a rect selection becomes blocky opaque pixels; nothing outside moves.
    ASSERT_TRUE(QMetaObject::invokeMethod(mosaicTool, "clicked"));
    harness.settle();
    EXPECT_EQ(workspace->property("editTool").toString(), QStringLiteral("mosaic"));
    EXPECT_TRUE(applyButton->isVisible());
    EXPECT_TRUE(applyButton->property("text").toString().contains(QStringLiteral("马赛克")));
    workspace->setProperty("mosaicBlock", 8);
    viewport->setProperty("cropStart", toViewportPoint(24, 24));
    viewport->setProperty("cropCurrent", toViewportPoint(40, 40));
    ASSERT_TRUE(QMetaObject::invokeMethod(
        workspace, "updateCropSelection", Q_ARG(QVariant, viewportArgument)));
    ASSERT_TRUE(QMetaObject::invokeMethod(applyButton, "clicked"));
    harness.settle();

    const QImage mosaic = harness.imageEdit.editedImage();
    EXPECT_EQ(mosaic.pixelColor(25, 25), mosaic.pixelColor(30, 30));
    EXPECT_EQ(mosaic.pixelColor(25, 25).alpha(), 255);
    EXPECT_EQ(mosaic.pixelColor(4, 4), QColor(10, 20, 30));
    EXPECT_TRUE(workspace->property("cropSelection").toMap().isEmpty());

    // Both tools share one ordered history: two undos return to the committed original.
    ASSERT_TRUE(QMetaObject::invokeMethod(undoButton, "clicked"));
    harness.settle();
    ASSERT_TRUE(QMetaObject::invokeMethod(undoButton, "clicked"));
    harness.settle();
    EXPECT_FALSE(harness.imageEdit.canUndo());
    EXPECT_FALSE(harness.imageEdit.dirty());
    EXPECT_EQ(harness.imageEdit.editedImage().pixelColor(25, 10), QColor(10, 20, 30));
    EXPECT_EQ(
        harness.imageReview.rawImageForSlot(ImageReviewController::PrimarySlot).pixelColor(25, 10),
        QColor(10, 20, 30));
    ASSERT_TRUE(QMetaObject::invokeMethod(startButton, "clicked"));
}

// Step-3 third increment: fill/clear are one-shot rect edits, and the annotation tools build
// a flat list that renders into the working copy — selectable, movable and deletable, all on
// the same bounded history, with the committed original untouched throughout.
TEST(MainQmlContractTests, ImageEditAnnotationAndFillToolsEditTheWorkingCopy) {
    WorkspaceHarness harness;
    harness.withImageEdit = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    QImage primary(96, 64, QImage::Format_ARGB32);
    primary.fill(QColor(10, 20, 30));
    QImage secondary(32, 32, QImage::Format_ARGB32);
    secondary.fill(QColor(40, 40, 40));
    ASSERT_TRUE(harness.imageReview.openPairImages(std::move(primary),
                                                   QStringLiteral("a.png"),
                                                   std::move(secondary),
                                                   QStringLiteral("b.png")));
    ASSERT_TRUE(harness.activateWorkspace(1));
    harness.settle();

    auto* const workspace =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
    auto* const viewport = harness.root->findChild<QQuickItem*>(QStringLiteral("primaryViewport"));
    auto* const preview = harness.root->findChild<QQuickItem*>(QStringLiteral("imageViewport-2"));
    auto* const startButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditStartButton"));
    // Only the select tool stays on the row; rect/arrow/text and fill/clear moved into the
    // 「更多工具」 dropdown together with their delete/clear-all actions.
    auto* const selectTool =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditToolSelectButton"));
    auto* const deleteButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditDeleteAnnotationButton"));
    auto* const clearAllButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditClearAnnotationsButton"));
    auto* const undoButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditUndoButton"));
    ASSERT_NE(workspace, nullptr);
    ASSERT_NE(viewport, nullptr);
    ASSERT_NE(preview, nullptr);
    ASSERT_NE(startButton, nullptr);
    ASSERT_NE(selectTool, nullptr);
    ASSERT_NE(deleteButton, nullptr);
    ASSERT_NE(clearAllButton, nullptr);
    ASSERT_NE(undoButton, nullptr);

    ASSERT_TRUE(QMetaObject::invokeMethod(startButton, "clicked"));
    harness.settle();

    auto* const rectTool =
        openEditToolsItem(harness.root.get(), QStringLiteral("imageEditToolRectButton"));
    ASSERT_NE(rectTool, nullptr);
    auto* const arrowTool =
        openEditToolsItem(harness.root.get(), QStringLiteral("imageEditToolArrowButton"));
    ASSERT_NE(arrowTool, nullptr);
    auto* const textTool =
        openEditToolsItem(harness.root.get(), QStringLiteral("imageEditToolTextButton"));
    ASSERT_NE(textTool, nullptr);
    auto* const fillTool =
        openEditToolsItem(harness.root.get(), QStringLiteral("imageEditToolFillButton"));
    ASSERT_NE(fillTool, nullptr);
    auto* const clearTool =
        openEditToolsItem(harness.root.get(), QStringLiteral("imageEditToolClearButton"));
    ASSERT_NE(clearTool, nullptr);

    const qreal scale =
        preview->width() / static_cast<qreal>(preview->property("sourceSize").toSize().width());
    ASSERT_GT(scale, 0.0);
    const QVariant viewportArgument = QVariant::fromValue(static_cast<QObject*>(viewport));
    const auto toViewportX = [&](const qreal imageX) {
        return QVariant{preview->x() + (imageX * scale)};
    };
    const auto toViewportY = [&](const qreal imageY) {
        return QVariant{preview->y() + (imageY * scale)};
    };
    const auto toViewportPoint = [&](const qreal imageX, const qreal imageY) {
        return QPointF{preview->x() + (imageX * scale), preview->y() + (imageY * scale)};
    };
    QVariant applied;
    const auto dragRect = [&](const qreal x0, const qreal y0, const qreal x1, const qreal y1) {
        viewport->setProperty("cropStart", toViewportPoint(x0, y0));
        viewport->setProperty("cropCurrent", toViewportPoint(x1, y1));
        return QMetaObject::invokeMethod(
            workspace, "updateCropSelection", Q_ARG(QVariant, viewportArgument));
    };
    const auto applyRect = [&]() {
        return QMetaObject::invokeMethod(
            workspace, "applyRectTool", Q_RETURN_ARG(QVariant, applied));
    };

    // Rectangle annotation: the tool commits on release, so the list grows immediately.
    ASSERT_TRUE(QMetaObject::invokeMethod(rectTool, "clicked"));
    harness.settle();
    EXPECT_EQ(workspace->property("editTool").toString(), QStringLiteral("rect"));
    ASSERT_TRUE(dragRect(10, 10, 40, 30));
    ASSERT_TRUE(applyRect());
    EXPECT_TRUE(applied.toBool());
    harness.settle();
    EXPECT_EQ(harness.imageEdit.annotationCount(), 1);
    EXPECT_TRUE(workspace->property("cropSelection").toMap().isEmpty());
    EXPECT_GT(harness.imageEdit.editedImage().pixelColor(20, 10).red(), 180);

    // Arrow annotation on top of it.
    ASSERT_TRUE(QMetaObject::invokeMethod(arrowTool, "clicked"));
    ASSERT_TRUE(dragRect(50, 40, 80, 40));
    ASSERT_TRUE(applyRect());
    harness.settle();
    EXPECT_EQ(harness.imageEdit.annotationCount(), 2);

    // Text annotation: typed content placed by a click.
    ASSERT_TRUE(QMetaObject::invokeMethod(textTool, "clicked"));
    workspace->setProperty("annotationText", QStringLiteral("AB"));
    workspace->setProperty("annotationTextSize", 20);
    ASSERT_TRUE(QMetaObject::invokeMethod(workspace,
                                          "placeTextAt",
                                          Q_ARG(QVariant, viewportArgument),
                                          Q_ARG(QVariant, toViewportX(8)),
                                          Q_ARG(QVariant, toViewportY(58))));
    harness.settle();
    EXPECT_EQ(harness.imageEdit.annotationCount(), 3);

    // Select tool: a click on empty space clears the selection, then dragging the rectangle
    // moves it — one history step for the whole gesture.
    ASSERT_TRUE(QMetaObject::invokeMethod(selectTool, "clicked"));
    harness.settle();
    EXPECT_TRUE(deleteButton->isVisible());
    ASSERT_TRUE(QMetaObject::invokeMethod(workspace,
                                          "beginAnnotationDrag",
                                          Q_ARG(QVariant, viewportArgument),
                                          Q_ARG(QVariant, toViewportX(90)),
                                          Q_ARG(QVariant, toViewportY(60))));
    EXPECT_EQ(harness.imageEdit.selectedAnnotation(), -1);
    EXPECT_FALSE(deleteButton->property("enabled").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(workspace,
                                          "beginAnnotationDrag",
                                          Q_ARG(QVariant, viewportArgument),
                                          Q_ARG(QVariant, toViewportX(20)),
                                          Q_ARG(QVariant, toViewportY(10))));
    EXPECT_EQ(harness.imageEdit.selectedAnnotation(), 0);
    ASSERT_TRUE(QMetaObject::invokeMethod(workspace,
                                          "continueAnnotationDrag",
                                          Q_ARG(QVariant, viewportArgument),
                                          Q_ARG(QVariant, toViewportX(25)),
                                          Q_ARG(QVariant, toViewportY(18))));
    ASSERT_TRUE(QMetaObject::invokeMethod(workspace, "endAnnotationDrag"));
    harness.settle();
    EXPECT_EQ(harness.imageEdit.editedImage().pixelColor(20, 10), QColor(10, 20, 30));
    EXPECT_GT(harness.imageEdit.editedImage().pixelColor(25, 18).red(), 180);
    ASSERT_TRUE(QMetaObject::invokeMethod(undoButton, "clicked"));
    harness.settle();
    EXPECT_GT(harness.imageEdit.editedImage().pixelColor(20, 10).red(), 180);

    EXPECT_TRUE(deleteButton->property("enabled").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(deleteButton, "clicked"));
    harness.settle();
    EXPECT_EQ(harness.imageEdit.annotationCount(), 2);
    ASSERT_TRUE(QMetaObject::invokeMethod(undoButton, "clicked"));
    harness.settle();
    EXPECT_EQ(harness.imageEdit.annotationCount(), 3);

    // Fill then clear the same region: opaque block first, transparent after.
    ASSERT_TRUE(QMetaObject::invokeMethod(fillTool, "clicked"));
    ASSERT_TRUE(dragRect(60, 5, 80, 20));
    ASSERT_TRUE(applyRect());
    harness.settle();
    const QColor fillColor = harness.imageEdit.editedImage().pixelColor(70, 12);
    EXPECT_EQ(fillColor.alpha(), 255);
    EXPECT_GT(fillColor.red(), 180);
    EXPECT_EQ(harness.imageEdit.undoLabel(), QStringLiteral("填充"));

    ASSERT_TRUE(QMetaObject::invokeMethod(clearTool, "clicked"));
    ASSERT_TRUE(dragRect(60, 5, 80, 20));
    ASSERT_TRUE(applyRect());
    harness.settle();
    EXPECT_EQ(harness.imageEdit.editedImage().pixelColor(70, 12).alpha(), 0);
    EXPECT_EQ(harness.imageEdit.undoLabel(), QStringLiteral("清除"));
    ASSERT_TRUE(QMetaObject::invokeMethod(undoButton, "clicked"));
    harness.settle();
    EXPECT_EQ(harness.imageEdit.editedImage().pixelColor(70, 12).alpha(), 255);

    // Clear-all is one step, and the committed original never carried any of this.
    ASSERT_TRUE(QMetaObject::invokeMethod(selectTool, "clicked"));
    ASSERT_TRUE(QMetaObject::invokeMethod(clearAllButton, "clicked"));
    harness.settle();
    EXPECT_EQ(harness.imageEdit.annotationCount(), 0);
    ASSERT_TRUE(QMetaObject::invokeMethod(undoButton, "clicked"));
    harness.settle();
    EXPECT_EQ(harness.imageEdit.annotationCount(), 3);
    const QImage original = harness.imageReview.rawImageForSlot(ImageReviewController::PrimarySlot);
    EXPECT_EQ(original.pixelColor(20, 10), QColor(10, 20, 30));
    EXPECT_EQ(original.pixelColor(70, 12).alpha(), 255);

    ASSERT_TRUE(QMetaObject::invokeMethod(startButton, "clicked"));
}

// Phase-1 UI increment: the pixel-size and canvas-fill work moves off the header row into two
// small dialogs. Both still funnel through the same controller, so the contract that matters is
// unchanged: the working copy resizes or the original is centred on a larger canvas, the source
// file is never touched, and each action is exactly one undo step.
TEST(MainQmlContractTests, ImageEditScaleDialogResizesTheWorkingCopy) {
    WorkspaceHarness harness;
    harness.withImageEdit = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    QImage primary(96, 64, QImage::Format_ARGB32);
    primary.fill(QColor(30, 90, 150));
    QImage secondary(32, 32, QImage::Format_ARGB32);
    secondary.fill(QColor(40, 40, 40));
    ASSERT_TRUE(harness.imageReview.openPairImages(std::move(primary),
                                                   QStringLiteral("a.png"),
                                                   std::move(secondary),
                                                   QStringLiteral("b.png")));
    ASSERT_TRUE(harness.activateWorkspace(1));
    harness.settle();

    auto* const startButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditStartButton"));
    auto* const scaleButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditScaleButton"));
    auto* const undoButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditUndoButton"));
    auto* const openButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageOpenButton"));
    auto* const saveCopyButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditSaveCopyButton"));
    ASSERT_NE(startButton, nullptr);
    ASSERT_NE(scaleButton, nullptr);
    ASSERT_NE(undoButton, nullptr);
    ASSERT_NE(openButton, nullptr);
    ASSERT_NE(saveCopyButton, nullptr);
    EXPECT_TRUE(openButton->property("prominent").toBool());

    ASSERT_TRUE(QMetaObject::invokeMethod(startButton, "clicked"));
    harness.settle();
    EXPECT_TRUE(scaleButton->isVisible());
    // One filled button per view: while editing, 另存副本… is the primary action and
    // 打开图片… steps back to a neutral button.
    EXPECT_FALSE(openButton->property("prominent").toBool());
    EXPECT_TRUE(saveCopyButton->property("prominent").toBool());

    // Optional evidence capture for the visible QML change: the header row after the tool
    // trim, still inside edit mode so every action for this step is on screen.
    const auto evidenceDirectory = qEnvironmentVariable("DVS_REVIEW_EVIDENCE_DIR");
    if (!evidenceDirectory.isEmpty()) {
        ASSERT_TRUE(QDir().mkpath(evidenceDirectory));
        harness.settleAnimations();
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(evidenceDirectory).filePath(QStringLiteral("image-edit-header.png"))));
        // The header row is the widest element in edit mode; it must stay complete at the
        // window's minimum width (Main.qml sets minimumWidth 960).
        harness.window->resize(960, 640);
        harness.settle();
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(evidenceDirectory).filePath(QStringLiteral("image-edit-header-min-width.png"))));
        harness.window->resize(1280, 800);
        harness.settle();
    }

    ASSERT_TRUE(QMetaObject::invokeMethod(scaleButton, "clicked"));
    harness.settle();
    // The shell derives from Popup, not Item, so it is looked up as a plain object.
    auto* const dialog = harness.root->findChild<QObject*>(QStringLiteral("imageEditScaleDialog"));
    auto* const widthField =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditScaleWidthField"));
    auto* const heightField =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditScaleHeightField"));
    auto* const lockAspect =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditScaleLockAspect"));
    auto* const applyButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditScaleApplyButton"));
    ASSERT_NE(dialog, nullptr);
    ASSERT_NE(widthField, nullptr);
    ASSERT_NE(heightField, nullptr);
    ASSERT_NE(lockAspect, nullptr);
    ASSERT_NE(applyButton, nullptr);
    EXPECT_TRUE(dialog->property("visible").toBool());
    EXPECT_EQ(widthField->property("value").toInt(), 96);
    EXPECT_EQ(heightField->property("value").toInt(), 64);
    EXPECT_TRUE(lockAspect->property("checked").toBool());

    if (!evidenceDirectory.isEmpty()) {
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(evidenceDirectory).filePath(QStringLiteral("image-edit-scale-dialog.png"))));
    }

    // With the aspect locked, one edited edge recomputes the other by the ratio the dialog
    // opened with (96:64 = 3:2).
    ASSERT_TRUE(typeSpinBoxValue(heightField, 48));
    harness.settle();
    EXPECT_EQ(widthField->property("value").toInt(), 72);

    ASSERT_TRUE(QMetaObject::invokeMethod(applyButton, "clicked"));
    harness.settle();
    EXPECT_FALSE(dialog->property("visible").toBool());
    EXPECT_EQ(harness.imageEdit.imageWidth(), 72);
    EXPECT_EQ(harness.imageEdit.imageHeight(), 48);
    EXPECT_EQ(harness.imageEdit.undoLabel(), QStringLiteral("缩放"));
    EXPECT_TRUE(undoButton->property("enabled").toBool());

    // The committed original keeps its own size; leaving edit mode drops the working copy.
    ASSERT_TRUE(QMetaObject::invokeMethod(undoButton, "clicked"));
    harness.settle();
    EXPECT_EQ(harness.imageEdit.imageWidth(), 96);
    EXPECT_EQ(harness.imageEdit.imageHeight(), 64);
    EXPECT_EQ(harness.imageReview.rawImageForSlot(ImageReviewController::PrimarySlot).size(),
              QSize(96, 64));

    ASSERT_TRUE(QMetaObject::invokeMethod(startButton, "clicked"));
    harness.settle();
    EXPECT_FALSE(harness.imageEdit.active());
}

// The canvas-fill dialog's contract: the image keeps its pixels and lands in the middle of the
// requested canvas, the ring is the chosen colour, and the step is one undo away.
TEST(MainQmlContractTests, ImageEditFillDialogPadsTheCanvasAndKeepsTheImage) {
    WorkspaceHarness harness;
    harness.withImageEdit = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    QImage primary(64, 48, QImage::Format_ARGB32);
    primary.fill(QColor(200, 60, 40));
    QImage secondary(32, 32, QImage::Format_ARGB32);
    secondary.fill(QColor(40, 40, 40));
    ASSERT_TRUE(harness.imageReview.openPairImages(std::move(primary),
                                                   QStringLiteral("a.png"),
                                                   std::move(secondary),
                                                   QStringLiteral("b.png")));
    ASSERT_TRUE(harness.activateWorkspace(1));
    harness.settle();

    auto* const workspace =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
    auto* const startButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditStartButton"));
    auto* const fillCanvasButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditFillButton"));
    auto* const undoButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditUndoButton"));
    ASSERT_NE(workspace, nullptr);
    ASSERT_NE(startButton, nullptr);
    ASSERT_NE(fillCanvasButton, nullptr);
    ASSERT_NE(undoButton, nullptr);

    ASSERT_TRUE(QMetaObject::invokeMethod(startButton, "clicked"));
    harness.settle();
    ASSERT_TRUE(QMetaObject::invokeMethod(fillCanvasButton, "clicked"));
    harness.settle();

    auto* const dialog = harness.root->findChild<QObject*>(QStringLiteral("imageEditFillDialog"));
    auto* const widthField =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditFillWidthField"));
    auto* const heightField =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditFillHeightField"));
    auto* const applyButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditFillApplyButton"));
    ASSERT_NE(dialog, nullptr);
    ASSERT_NE(widthField, nullptr);
    ASSERT_NE(heightField, nullptr);
    ASSERT_NE(applyButton, nullptr);
    EXPECT_TRUE(dialog->property("visible").toBool());
    EXPECT_EQ(widthField->property("value").toInt(), 64);
    EXPECT_EQ(heightField->property("value").toInt(), 48);

    // Optional evidence capture: the canvas dialog with its target size and colour swatches.
    const auto evidenceDirectory = qEnvironmentVariable("DVS_REVIEW_EVIDENCE_DIR");
    if (!evidenceDirectory.isEmpty()) {
        ASSERT_TRUE(QDir().mkpath(evidenceDirectory));
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(evidenceDirectory).filePath(QStringLiteral("image-edit-fill-dialog.png"))));
    }

    // Black is the pre-selected ring colour; the image is 64x48 placed at (8,8) on 80x64.
    ASSERT_TRUE(typeSpinBoxValue(widthField, 80));
    ASSERT_TRUE(typeSpinBoxValue(heightField, 64));
    harness.settle();
    ASSERT_TRUE(QMetaObject::invokeMethod(applyButton, "clicked"));
    harness.settle();
    EXPECT_FALSE(dialog->property("visible").toBool());
    EXPECT_EQ(harness.imageEdit.imageWidth(), 80);
    EXPECT_EQ(harness.imageEdit.imageHeight(), 64);
    EXPECT_EQ(harness.imageEdit.undoLabel(), QStringLiteral("填充画布"));

    const QImage padded = harness.imageEdit.editedImage();
    EXPECT_EQ(padded.pixelColor(0, 0), QColor(0, 0, 0));
    EXPECT_EQ(padded.pixelColor(79, 63), QColor(0, 0, 0));
    EXPECT_EQ(padded.pixelColor(40, 32), QColor(200, 60, 40));
    EXPECT_EQ(padded.pixelColor(8, 8), QColor(200, 60, 40));

    // One undo restores the original geometry and the original buffer stays untouched.
    ASSERT_TRUE(QMetaObject::invokeMethod(undoButton, "clicked"));
    harness.settle();
    EXPECT_EQ(harness.imageEdit.editedImage().size(), QSize(64, 48));
    EXPECT_EQ(harness.imageReview.rawImageForSlot(ImageReviewController::PrimarySlot).size(),
              QSize(64, 48));

    ASSERT_TRUE(QMetaObject::invokeMethod(startButton, "clicked"));
    harness.settle();
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

    // The image workspace is instantiated on first activation, and this test inspects the
    // viewport before any image is opened, so activate the (empty) workspace first.
    QVariant imageWorkspaceActivated;
    ASSERT_TRUE(QMetaObject::invokeMethod(root.get(),
                                          "activateWorkspace",
                                          Q_RETURN_ARG(QVariant, imageWorkspaceActivated),
                                          Q_ARG(QVariant, QVariant{1})));
    ASSERT_TRUE(imageWorkspaceActivated.toBool());
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

    // The image workspace is instantiated on first activation, and this environment never
    // renders frames (positioner relayouts are frame-driven), so the workspace must be
    // created while the layout is still stable — before the folder flow starts changing
    // content state. A real session settles the same layouts on its first rendered frame.
    QVariant workspaceActivatedEarly;
    ASSERT_TRUE(QMetaObject::invokeMethod(root.get(),
                                          "activateWorkspace",
                                          Q_RETURN_ARG(QVariant, workspaceActivatedEarly),
                                          Q_ARG(QVariant, QVariant{1})));
    ASSERT_TRUE(workspaceActivatedEarly.toBool());
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

TEST(MainQmlContractTests, StartupVideoOpenCommitsWorkspaceOnlyAfterSuccess) {
    for (const int sourceCount : {1, 2, 3}) {
        SCOPED_TRACE(sourceCount);
        WorkspaceHarness harness;
        ASSERT_TRUE(harness.create()) << harness.error;
        harness.window->show();
        QImage retained{16, 16, QImage::Format_ARGB32};
        retained.fill(QColor(10, 20, 30));
        ASSERT_TRUE(harness.imageReview.openPrimaryImage(std::move(retained),
                                                         QStringLiteral("retained-image")));
        ASSERT_TRUE(harness.commitWorkspace(1, QStringLiteral("retained-image")));
        harness.settle();
        QObject* const session =
            harness.root->findChild<QObject*>(QStringLiteral("workspaceSession"));
        auto* const imageWorkspace =
            harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
        auto* const viewport =
            harness.root->findChild<QQuickItem*>(QStringLiteral("mediaViewportFocusTarget"));
        ASSERT_NE(session, nullptr);
        ASSERT_NE(imageWorkspace, nullptr);
        ASSERT_NE(viewport, nullptr);
        const int revisionBefore = session->property("commitRevision").toInt();

        QTemporaryDir directory;
        ASSERT_TRUE(directory.isValid());
        QVariantList urls;
        std::vector<std::filesystem::path> paths;
        QStringList expectedIdentity;
        for (int index = 0; index < sourceCount; ++index) {
            const QString path = directory.filePath(QStringLiteral("startup_%1.mp4").arg(index));
            // The fake media backend supplies validated video descriptors; these bytes only
            // establish existing local files for ReviewController's synchronous path validation.
            ASSERT_TRUE(writeTestPng(path));
            const QUrl url = QUrl::fromLocalFile(path);
            urls.push_back(url);
            paths.emplace_back(path.toStdWString());
            expectedIdentity.push_back(url.toString());
        }
        // DesktopApplication uses this same entry point for normal CLI and shell requests,
        // unlike the automation path which explicitly activates the video workspace.
        ASSERT_TRUE(harness.shell->enqueueStartupRequest(sourceCount == 1 ? 1 : 2, urls));
        const application::CommandContext context =
            application::commandContext(harness.submitted.back());
        harness.settle();
        EXPECT_EQ(harness.root->property("workspaceMode").toInt(), 1);
        EXPECT_EQ(session->property("commitRevision").toInt(), revisionBefore);
        EXPECT_TRUE(imageWorkspace->hasActiveFocus());
        // The real open command needs a first-frame ACK before its successful terminal.
        // Its surface must render beneath the retained image task without accepting input.
        EXPECT_TRUE(viewport->isVisible());
        EXPECT_FALSE(viewport->isEnabled());

        ASSERT_TRUE(installValidatedVideoSet(
            harness.snapshot, paths, domain::RationalRate::create(30, 1).value(), 100, 3'333'334));
        harness.snapshot->displayedFrame = domain::FrameId{0};
        harness.terminals.push_back(application::CommandTerminal{
            .context = context,
            .outcome = application::CommandOutcome::Succeeded,
        });
        harness.controller->refreshProjection();
        harness.settle();
        EXPECT_EQ(harness.root->property("workspaceMode").toInt(), 0);
        EXPECT_EQ(session->property("committedMedia").toInt(), 0);
        EXPECT_EQ(session->property("committedIdentity").toString(),
                  expectedIdentity.join(QStringLiteral("\n")));
        EXPECT_EQ(session->property("commitRevision").toInt(), revisionBefore + 1);
        EXPECT_FALSE(imageWorkspace->isVisible());
        EXPECT_TRUE(viewport->hasActiveFocus());
        EXPECT_TRUE(viewport->isEnabled());
        EXPECT_EQ(harness.imageReview.primaryPath(), QStringLiteral("retained-image"));
    }
}

TEST(MainQmlContractTests, UnsuccessfulStartupVideoOpenKeepsImageWorkspace) {
    for (const auto outcome :
         {application::CommandOutcome::Failed, application::CommandOutcome::Canceled}) {
        SCOPED_TRACE(static_cast<int>(outcome));
        WorkspaceHarness harness;
        ASSERT_TRUE(harness.create()) << harness.error;
        harness.window->show();
        QImage retained{16, 16, QImage::Format_ARGB32};
        retained.fill(QColor(10, 20, 30));
        ASSERT_TRUE(harness.imageReview.openPrimaryImage(std::move(retained),
                                                         QStringLiteral("retained-image")));
        ASSERT_TRUE(harness.commitWorkspace(1, QStringLiteral("retained-image")));
        harness.settle();
        QObject* const session =
            harness.root->findChild<QObject*>(QStringLiteral("workspaceSession"));
        auto* const imageWorkspace =
            harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
        ASSERT_NE(session, nullptr);
        ASSERT_NE(imageWorkspace, nullptr);
        const int revisionBefore = session->property("commitRevision").toInt();
        const QString identityBefore = session->property("committedIdentity").toString();
        QTemporaryDir directory;
        ASSERT_TRUE(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("startup.mp4"));
        ASSERT_TRUE(writeTestPng(path));
        ASSERT_TRUE(harness.shell->enqueueStartupRequest(1, {QUrl::fromLocalFile(path)}));
        harness.terminals.push_back(application::CommandTerminal{
            .context = application::commandContext(harness.submitted.back()),
            .outcome = outcome,
        });
        harness.controller->refreshProjection();
        harness.settle();
        EXPECT_EQ(harness.root->property("workspaceMode").toInt(), 1);
        EXPECT_EQ(session->property("commitRevision").toInt(), revisionBefore);
        EXPECT_EQ(session->property("committedIdentity").toString(), identityBefore);
        EXPECT_TRUE(imageWorkspace->isVisible());
        EXPECT_TRUE(imageWorkspace->hasActiveFocus());
        EXPECT_EQ(harness.imageReview.primaryPath(), QStringLiteral("retained-image"));
    }
}

TEST(MainQmlContractTests, ClosingRetainedVideoDoesNotReplaceImageWorkspace) {
    WorkspaceHarness harness;
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString videoPath = directory.filePath(QStringLiteral("retained.mp4"));
    ASSERT_TRUE(writeTestPng(videoPath));
    ASSERT_TRUE(installValidatedVideoSet(harness.snapshot,
                                         {std::filesystem::path{videoPath.toStdWString()}},
                                         domain::RationalRate::create(30, 1).value(),
                                         100,
                                         3'333'334));
    harness.snapshot->displayedFrame = domain::FrameId{0};
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    QImage retained{16, 16, QImage::Format_ARGB32};
    retained.fill(QColor(10, 20, 30));
    ASSERT_TRUE(harness.imageReview.openPrimaryImage(std::move(retained),
                                                     QStringLiteral("retained-image")));
    ASSERT_TRUE(harness.commitWorkspace(1, QStringLiteral("retained-image")));
    harness.settle();
    QObject* const session = harness.root->findChild<QObject*>(QStringLiteral("workspaceSession"));
    auto* const imageWorkspace =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
    ASSERT_NE(session, nullptr);
    ASSERT_NE(imageWorkspace, nullptr);
    const int revisionBefore = session->property("commitRevision").toInt();
    const QString identityBefore = session->property("committedIdentity").toString();

    ASSERT_TRUE(harness.shell->closeSources());
    const application::CommandContext context =
        application::commandContext(harness.submitted.back());
    harness.snapshot->sessionState = domain::SessionState::kEmpty;
    harness.snapshot->sources.clear();
    harness.snapshot->presentedSources.clear();
    harness.snapshot->validatedComparison.reset();
    harness.snapshot->canonicalTimeline.reset();
    harness.snapshot->displayedFrame.reset();
    harness.snapshot->canonicalFrameCount = 0U;
    harness.terminals.push_back(application::CommandTerminal{
        .context = context,
        .outcome = application::CommandOutcome::Succeeded,
    });
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_EQ(harness.root->property("workspaceMode").toInt(), 1);
    EXPECT_EQ(session->property("commitRevision").toInt(), revisionBefore);
    EXPECT_EQ(session->property("committedIdentity").toString(), identityBefore);
    EXPECT_TRUE(imageWorkspace->isVisible());
    EXPECT_TRUE(imageWorkspace->hasActiveFocus());
    EXPECT_EQ(harness.imageReview.primaryPath(), QStringLiteral("retained-image"));
}

TEST(MainQmlContractTests, WorkspaceOpenIntentDoesNotOverrideCommittedWorkspace) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    QObject* const session = harness.root->findChild<QObject*>(QStringLiteral("workspaceSession"));
    ASSERT_NE(session, nullptr);
    // The image workspace is instantiated on first activation; activate it once and return
    // to the committed video workspace so the hidden-panel assertions below address the
    // real item instead of an absent one.
    ASSERT_TRUE(harness.activateWorkspace(1));
    ASSERT_TRUE(harness.activateWorkspace(0));
    auto* const imageWorkspace =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
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

TEST(MainQmlContractTests, InvalidImagePairSelectionKeepsCurrentWorkspace) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    QObject* const session = harness.root->findChild<QObject*>(QStringLiteral("workspaceSession"));
    ASSERT_NE(session, nullptr);

    const QVariantList oneImage{QUrl{QStringLiteral("file:///first.png")}};
    QVariantList threeImages = oneImage;
    threeImages.push_back(QUrl{QStringLiteral("file:///second.png")});
    threeImages.push_back(QUrl{QStringLiteral("file:///third.png")});
    const auto rejectsWithoutSwitching = [&](const char* method, const QVariantList& urls) {
        ASSERT_TRUE(harness.beginWorkspaceOpen(1));
        QVariant result;
        ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(),
                                              method,
                                              Q_RETURN_ARG(QVariant, result),
                                              Q_ARG(QVariant, QVariant{urls})));
        EXPECT_FALSE(result.toBool());
        EXPECT_EQ(harness.root->property("workspaceMode").toInt(), 0);
        EXPECT_EQ(session->property("pendingMedia").toInt(), -1);
        EXPECT_FALSE(harness.imageReview.hasPrimary());
        EXPECT_FALSE(harness.root->property("dropError").toString().isEmpty());
    };

    rejectsWithoutSwitching("performImagePairSelection", oneImage);
    rejectsWithoutSwitching("performImagePairSelection", threeImages);
    rejectsWithoutSwitching("performImageReview", threeImages);
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
    ASSERT_NE(viewport, nullptr);

    ASSERT_TRUE(harness.activateWorkspace(1));
    harness.settle();
    // The image workspace is instantiated on first activation, so the lookup follows it.
    auto* const imageWorkspace =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
    ASSERT_NE(imageWorkspace, nullptr);
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

    auto* const openImagePairBtn =
        harness.root->findChild<QQuickItem*>(QStringLiteral("emptyOpenImagePairButton"));
    ASSERT_NE(openImagePairBtn, nullptr);
    EXPECT_TRUE(openImagePairBtn->isVisible());

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

TEST(MainQmlContractTests, ImageWorkspaceManualFlickerContract) {
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

    // The canvas exposes a full-size click target; Space switches A/B without auto playback.
    auto* const canvasArea =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageCanvasMouseArea-2"));
    ASSERT_NE(canvasArea, nullptr);
    EXPECT_TRUE(canvasArea->isVisible());
    EXPECT_GT(canvasArea->width(), 100);

    imageWorkspace->forceActiveFocus();
    sendKey(*harness.window, Qt::Key_Space);
    harness.settle();
    EXPECT_EQ(primaryViewport->property("slot").toInt(), 3);
    sendKey(*harness.window, Qt::Key_Space);
    harness.settle();
    EXPECT_EQ(primaryViewport->property("slot").toInt(), 2);

    // The Fade chip is visible again and clicking it must actually engage the mode: the
    // controller used to reject compareMode 7, so the entry was hidden (C-02).
    auto* const fadeBtn = harness.root->findChild<QQuickItem*>(QStringLiteral("imageModeFade"));
    ASSERT_NE(fadeBtn, nullptr);
    EXPECT_TRUE(fadeBtn->isVisible());
    ASSERT_TRUE(QMetaObject::invokeMethod(fadeBtn, "clicked"));
    harness.settle();
    EXPECT_EQ(harness.imageReview.compareMode(), 7);
    auto* const fadeOverlay = harness.root->findChild<QQuickItem*>(QStringLiteral("fadeOverlay"));
    ASSERT_NE(fadeOverlay, nullptr);
    EXPECT_TRUE(fadeOverlay->isVisible());
    auto* const fadeSlider =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageFadeSlider"));
    ASSERT_NE(fadeSlider, nullptr);
    EXPECT_TRUE(fadeSlider->isVisible());
    // Optional evidence capture for the visible QML change (same env-gated pattern as the
    // high-depth alpha readout): the shot shows the restored chip, overlay and slider.
    const auto evidenceDirectory = qEnvironmentVariable("DVS_REVIEW_EVIDENCE_DIR");
    if (!evidenceDirectory.isEmpty()) {
        ASSERT_TRUE(QDir().mkpath(evidenceDirectory));
        harness.settleAnimations();
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(evidenceDirectory).filePath(QStringLiteral("image-fade-mode.png"))));
    }
    // Back to manual flicker: the mode leaves Fade and the primary viewport is restored.
    auto* const manualChip =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageModePrimary"));
    ASSERT_NE(manualChip, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(manualChip, "clicked"));
    harness.settle();
    EXPECT_EQ(harness.imageReview.compareMode(), 0);

    auto* const badgeText =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageInPlaceBadge"));
    ASSERT_NE(badgeText, nullptr);
    EXPECT_LE(badgeText->x() + badgeText->width(), badgeText->parentItem()->width());

    auto* const diffButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageDiffModeButton"));
    auto* const absDiff = harness.root->findChild<QObject*>(QStringLiteral("imageModeAbsDiff"));
    ASSERT_NE(diffButton, nullptr);
    ASSERT_NE(absDiff, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(absDiff, "triggered"));
    harness.settle();
    EXPECT_EQ(harness.imageReview.compareMode(), 2);
    EXPECT_TRUE(diffButton->property("text").toString().contains(QStringLiteral("绝对差异")));

    auto* const manualMode =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageModePrimary"));
    ASSERT_NE(manualMode, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(manualMode, "clicked"));
    harness.settle();
    EXPECT_EQ(primaryViewport->property("slot").toInt(), 2);
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

TEST(MainQmlContractTests, HighBitDepthAlphaIsVisibleInPixelReadout) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();
    // The image workspace is instantiated on first activation, and this test inspects the
    // pixel readout before and after an image opens, so activate the workspace up front.
    ASSERT_TRUE(harness.activateWorkspace(1));
    harness.settle();
    auto* workspace = harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
    auto* readout = harness.root->findChild<QQuickItem*>(QStringLiteral("imagePixelReadout"));
    ASSERT_NE(workspace, nullptr);
    ASSERT_NE(readout, nullptr);
    struct LoaderReset final {
        ~LoaderReset() {
            ImageReviewController::setProcessStillImageLoader({});
        }
    } reset;
    ImageReviewController::setProcessStillImageLoader([](const QByteArray&,
                                                         QImage* image,
                                                         QImage* native,
                                                         StillImageSourceInfo* info,
                                                         std::string*) {
        *image = QImage(1, 1, QImage::Format_ARGB32);
        image->fill(QColor(0, 0, 0, 128));
        *native = QImage(1, 1, QImage::Format_RGBA64);
        *reinterpret_cast<QRgba64*>(native->bits()) = qRgba64(0, 0, 0, 32768);
        info->bitDepth = 16;
        info->hasAlpha = true;
        info->channels = 4;
        info->sourceFormat = QStringLiteral("rgba64le");
        info->displayConverted = true;
        return true;
    });
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("deep.png"));
    QImage input(1, 1, QImage::Format_ARGB32);
    input.fill(Qt::transparent);
    ASSERT_TRUE(input.save(path));
    ASSERT_TRUE(harness.imageReview.openPrimary(QUrl::fromLocalFile(path)));
    ASSERT_TRUE(harness.activateWorkspace(1));
    harness.settle();
    harness.imageReview.updateCursorPixel(ImageReviewController::PrimarySlot, 0, 0);
    harness.settle();
    const auto text = readout->property("text").toString();
    EXPECT_TRUE(text.contains(QStringLiteral("A 32768"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("50.0008%"))) << text.toStdString();
    const auto evidenceDirectory = qEnvironmentVariable("DVS_REVIEW_EVIDENCE_DIR");
    if (!evidenceDirectory.isEmpty()) {
        ASSERT_TRUE(QDir().mkpath(evidenceDirectory));
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(evidenceDirectory).filePath(QStringLiteral("high-depth-alpha.png"))));
    }
}

TEST(MainQmlContractTests, ImageWorkspaceAlphaAndBackgroundSelectionContract) {
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

    // Initial state: viewMode = 0 (RgbaView), backgroundMode = 1 (neutral checkerboard —
    // the default, so the content area never tints color judgment of translucent pixels).
    EXPECT_EQ(harness.imageReview.viewMode(), 0);
    EXPECT_EQ(imageWorkspace->property("backgroundMode").toInt(), 1);

    auto* const alphaBadge =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageAlphaObservationBadge"));
    ASSERT_NE(alphaBadge, nullptr);
    EXPECT_FALSE(alphaBadge->isVisible());

    // Evidence capture for the visible default-background change (same env-gated pattern
    // as the high-depth alpha and fade-mode shots): the default state renders the neutral
    // checkerboard behind the translucent pair, with no observation badge.
    const auto evidenceDirectory = qEnvironmentVariable("DVS_REVIEW_EVIDENCE_DIR");
    if (!evidenceDirectory.isEmpty()) {
        ASSERT_TRUE(QDir().mkpath(evidenceDirectory));
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(evidenceDirectory).filePath(QStringLiteral("image-default-checkerboard.png"))));
    }

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

    // Background changes through the explicit menu selection. The checkerboard IS the
    // default now, so selecting it explicitly keeps the observation badge hidden.
    auto* const checkerItem =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageBgChecker"));
    ASSERT_NE(checkerItem, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(checkerItem, "triggered"));
    harness.settle();
    EXPECT_EQ(imageWorkspace->property("backgroundMode").toInt(), 1);
    EXPECT_FALSE(alphaBadge->isVisible());

    // The old cycling shortcut no longer changes the selected background.
    sendKey(*harness.window, Qt::Key_B);
    harness.settle();
    EXPECT_EQ(imageWorkspace->property("backgroundMode").toInt(), 1);

    auto* const blackItem = harness.root->findChild<QQuickItem*>(QStringLiteral("imageBgBlack"));
    ASSERT_NE(blackItem, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(blackItem, "triggered"));
    harness.settle();
    EXPECT_EQ(imageWorkspace->property("backgroundMode").toInt(), 2);
    EXPECT_TRUE(alphaBadge->isVisible());

    auto* const darkItem = harness.root->findChild<QQuickItem*>(QStringLiteral("imageBgDark"));
    ASSERT_NE(darkItem, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(darkItem, "triggered"));
    harness.settle();
    EXPECT_EQ(imageWorkspace->property("backgroundMode").toInt(), 0);
    // Dark is no longer the default, so it counts as an active observation background.
    EXPECT_TRUE(alphaBadge->isVisible());
    // Returning to the default checkerboard hides the badge again.
    imageWorkspace->setProperty("backgroundMode", 1);
    harness.settle();
    EXPECT_FALSE(alphaBadge->isVisible());

    // View-mode menu item toggles: triggering "Alpha Gray" sets viewMode 1, triggering
    // again returns to RGBA (the chip was promoted into the 观察 dropdown menu).
    auto* const alphaGrayItem =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageViewAlphaGray"));
    ASSERT_NE(alphaGrayItem, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(alphaGrayItem, "triggered"));
    harness.settle();
    EXPECT_EQ(harness.imageReview.viewMode(), 1);
    EXPECT_TRUE(alphaBadge->isVisible());

    ASSERT_TRUE(QMetaObject::invokeMethod(alphaGrayItem, "triggered"));
    harness.settle();
    EXPECT_EQ(harness.imageReview.viewMode(), 0);
    EXPECT_FALSE(alphaBadge->isVisible());

    // Reset background to the default (neutral checkerboard).
    imageWorkspace->setProperty("backgroundMode", 1);
    harness.settle();
    EXPECT_FALSE(alphaBadge->isVisible());

    // Verify shortcut help has imagePreset set when in image workspace.
    auto* const shortcutHelp =
        harness.root->findChild<QObject*>(QStringLiteral("shortcutHelpOverlay"));
    ASSERT_NE(shortcutHelp, nullptr);
    EXPECT_TRUE(shortcutHelp->property("imagePreset").toBool());
}

// The help table must describe what ReviewShortcuts actually binds. Left/Right step one frame in
// every preset (the player preset once claimed 5-second skips), Shift+arrows and Down/Up step five
// frames, and Home/End / Alt+arrows are listed instead of silently missing.
TEST(MainQmlContractTests, ShortcutHelpMatchesBoundShortcuts) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;

    auto* const shortcutHelp =
        harness.root->findChild<QObject*>(QStringLiteral("shortcutHelpOverlay"));
    ASSERT_NE(shortcutHelp, nullptr);

    const auto helpPairs = [&shortcutHelp](const bool playerPreset) {
        shortcutHelp->setProperty("playerPreset", playerPreset);
        shortcutHelp->setProperty("imagePreset", false);
        return shortcutHelp->property("shortcutModel").toList();
    };
    const auto descriptionFor = [](const QVariantList& model, const QString& keys) {
        for (const QVariant& entry : model) {
            const QVariantList pair = entry.toList();
            if (pair.size() == 2 && pair.front().toString() == keys) {
                return pair.back().toString();
            }
        }
        return QString{};
    };

    for (const bool playerPreset : {false, true}) {
        const QVariantList model = helpPairs(playerPreset);
        ASSERT_GE(model.size(), 10);
        EXPECT_EQ(descriptionFor(model, QStringLiteral("← / →")),
                  QStringLiteral("上一帧 / 下一帧"));
        EXPECT_EQ(descriptionFor(model, QStringLiteral("↓ / ↑")),
                  QStringLiteral("后退 / 前进 5 帧"));
        EXPECT_EQ(descriptionFor(model, QStringLiteral("Shift+← / →")),
                  QStringLiteral("后退 / 前进 5 帧"));
        EXPECT_EQ(descriptionFor(model, QStringLiteral("Home / End")),
                  QStringLiteral("第一帧 / 最后一帧"));
        EXPECT_EQ(descriptionFor(model, QStringLiteral("Alt+← / →")),
                  QStringLiteral("Wipe 分屏线左移 / 右移"));
        EXPECT_FALSE(descriptionFor(model, QStringLiteral("A / D")).isEmpty());
    }

    // The player preset alone documents the 30-second jumps and its comma/period stepping.
    const QVariantList playerModel = helpPairs(true);
    EXPECT_EQ(descriptionFor(playerModel, QStringLiteral("Ctrl+← / →")),
              QStringLiteral("快退 / 快进 30 秒"));
    EXPECT_FALSE(descriptionFor(playerModel, QStringLiteral(", / .")).isEmpty());
    const QVariantList frameModel = helpPairs(false);
    EXPECT_EQ(descriptionFor(frameModel, QStringLiteral("Ctrl+← / →")),
              QStringLiteral("后退 / 前进 1 秒"));
    EXPECT_TRUE(descriptionFor(frameModel, QStringLiteral(", / .")).isEmpty());
}

// Digit keys switch comparison modes with the mode bar's own availability (pair modes need
// two sources, three-source modes stay disabled here), F/+/- drive the viewport zoom path, and
// the backtick pair toggles/locks the raw-reference peek. The help table must be the entries
// ReviewShortcuts itself declares plus the static mouse tails - not a hand-maintained copy.
TEST(MainQmlContractTests, ViewShortcutsSwitchModesZoomAndGenerateHelp) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    auto* const reviewShortcuts =
        harness.root->findChild<QObject*>(QStringLiteral("reviewShortcuts"));
    ASSERT_NE(reviewShortcuts, nullptr);
    auto* const shortcutHelp =
        harness.root->findChild<QObject*>(QStringLiteral("shortcutHelpOverlay"));
    ASSERT_NE(shortcutHelp, nullptr);

    // Shortcut objects carry their declared sequence either as `sequence` or as the first
    // entry of `sequences`; the property itself is the reliable marker among the binding
    // table's direct children.
    const auto shortcutBySequence = [&reviewShortcuts](const QString& wanted) {
        const QList<QObject*> declared =
            reviewShortcuts->findChildren<QObject*>(Qt::FindDirectChildrenOnly);
        for (QObject* const object : declared) {
            if (!object->property("sequence").isValid() &&
                !object->property("sequences").isValid()) {
                continue;
            }
            QString sequence = object->property("sequence").toString();
            if (sequence.isEmpty()) {
                const QVariantList multi = object->property("sequences").toList();
                if (!multi.isEmpty()) {
                    sequence = multi.front().toString();
                }
            }
            if (sequence == wanted) {
                return object;
            }
        }
        return static_cast<QObject*>(nullptr);
    };

    // Two sources: pair-mode keys available, three-source modes follow the mode bar and stay
    // disabled until a third source exists. Keys are driven by emitting each Shortcut's own
    // `activated` signal: the ctest process cannot take window foreground, and Qt matches
    // ApplicationShortcuts only in the active window, so real key events would couple this
    // contract to the runner's foreground rights instead of the bindings under test.
    QObject* const sideKey = shortcutBySequence(QStringLiteral("1"));
    ASSERT_NE(sideKey, nullptr);
    QObject* const wipeKey = shortcutBySequence(QStringLiteral("2"));
    ASSERT_NE(wipeKey, nullptr);
    QObject* const differenceKey = shortcutBySequence(QStringLiteral("3"));
    ASSERT_NE(differenceKey, nullptr);
    QObject* const threeUpKey = shortcutBySequence(QStringLiteral("4"));
    ASSERT_NE(threeUpKey, nullptr);
    EXPECT_TRUE(sideKey->property("enabled").toBool());
    EXPECT_FALSE(threeUpKey->property("enabled").toBool());

    ASSERT_TRUE(QMetaObject::invokeMethod(sideKey, "activated"));
    EXPECT_EQ(harness.preferences.viewModeCode(), ComparisonSurface::SideBySide);
    ASSERT_TRUE(QMetaObject::invokeMethod(wipeKey, "activated"));
    EXPECT_EQ(harness.preferences.viewModeCode(), ComparisonSurface::Wipe);
    ASSERT_TRUE(QMetaObject::invokeMethod(differenceKey, "activated"));
    EXPECT_EQ(harness.preferences.viewModeCode(), ComparisonSurface::Difference);
    harness.settle();

    // Zoom and view-command shortcuts exist and stay enabled with a drawable stage; the
    // viewport-side semantics (fit/native toggle, centred zoom step) are pinned by
    // ViewportPixelScaleTests against real mocked geometry. Pressing them must leave the
    // comparison mode untouched.
    QObject* const fitKey = shortcutBySequence(QStringLiteral("F"));
    ASSERT_NE(fitKey, nullptr);
    QObject* const nativeKey = shortcutBySequence(QStringLiteral("Ctrl+0"));
    ASSERT_NE(nativeKey, nullptr);
    QObject* const zoomInKey = shortcutBySequence(QStringLiteral("Plus"));
    ASSERT_NE(zoomInKey, nullptr);
    QObject* const resetKey = shortcutBySequence(QStringLiteral("R"));
    ASSERT_NE(resetKey, nullptr);
    QObject* const peekKey = shortcutBySequence(QStringLiteral("`"));
    ASSERT_NE(peekKey, nullptr);
    QObject* const peekLockKey = shortcutBySequence(QStringLiteral("Shift+`"));
    ASSERT_NE(peekLockKey, nullptr);
    EXPECT_TRUE(fitKey->property("enabled").toBool());
    EXPECT_TRUE(nativeKey->property("enabled").toBool());
    EXPECT_TRUE(zoomInKey->property("enabled").toBool());
    EXPECT_TRUE(resetKey->property("enabled").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(fitKey, "activated"));
    ASSERT_TRUE(QMetaObject::invokeMethod(zoomInKey, "activated"));
    ASSERT_TRUE(QMetaObject::invokeMethod(resetKey, "activated"));
    EXPECT_EQ(harness.preferences.viewModeCode(), ComparisonSurface::Difference);

    // Backtick toggles the raw-reference peek; Shift+backtick locks it, and a plain tap on a
    // locked peek unlocks instead of flipping the raw view back on.
    EXPECT_TRUE(harness.root->property("differenceMode").toBool());
    EXPECT_TRUE(peekKey->property("enabled").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(peekKey, "activated"));
    EXPECT_TRUE(harness.root->property("differencePeekActive").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(peekKey, "activated"));
    EXPECT_FALSE(harness.root->property("differencePeekActive").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(peekLockKey, "activated"));
    EXPECT_TRUE(harness.root->property("differencePeekActive").toBool());
    EXPECT_TRUE(harness.root->property("differencePeekLocked").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(peekKey, "activated"));
    EXPECT_FALSE(harness.root->property("differencePeekActive").toBool());
    EXPECT_FALSE(harness.root->property("differencePeekLocked").toBool());

    // The help model is generated: the binding-table entries (filtered to the active preset)
    // first, then the static mouse tails. The merged "F11 或双击" row is gone; double-click
    // now documents the unified native/fit toggle and full screen stays F11-only.
    const QVariantList helpEntries = reviewShortcuts->property("helpEntries").toList();
    EXPECT_GE(helpEntries.size(), 10);
    shortcutHelp->setProperty("imagePreset", false);
    shortcutHelp->setProperty("playerPreset", false);
    const QVariantList model = shortcutHelp->property("shortcutModel").toList();
    // Entry rows are [keycaps, label, playerLabel, presetMask]; the review preset keeps
    // every row except the player-only ones.
    QVariantList expectedEntries;
    for (const QVariant& entry : helpEntries) {
        const QVariantList row = entry.toList();
        if (row.size() >= 4 && row.at(3).toInt() == 1) {
            continue;
        }
        expectedEntries.push_back(row);
    }
    ASSERT_EQ(model.size(), expectedEntries.size() + 6);
    for (int i = 0; i < expectedEntries.size(); ++i) {
        EXPECT_EQ(model.at(static_cast<qsizetype>(i)).toList().front().toString(),
                  expectedEntries.at(static_cast<qsizetype>(i)).toList().front().toString())
            << "help row " << i << " must come from the binding table";
    }
    const auto descriptionFor = [&model](const QString& keys) {
        for (const QVariant& entry : model) {
            const QVariantList pair = entry.toList();
            if (pair.size() == 2 && pair.front().toString() == keys) {
                return pair.back().toString();
            }
        }
        return QString{};
    };
    EXPECT_EQ(descriptionFor(QStringLiteral("1")), QStringLiteral("切换视图：并排"));
    EXPECT_EQ(descriptionFor(QStringLiteral("F")), QStringLiteral("适应窗口"));
    EXPECT_EQ(descriptionFor(QStringLiteral("Ctrl+0")), QStringLiteral("100% 真实尺寸"));
    EXPECT_EQ(descriptionFor(QStringLiteral("F11")), QStringLiteral("全屏"));
    EXPECT_EQ(descriptionFor(QStringLiteral("双击")),
              QStringLiteral("切换 100% 真实尺寸 / 适应窗口"));
    EXPECT_TRUE(descriptionFor(QStringLiteral("F11 或双击")).isEmpty());
}

// M captures an issue through the note dialog: the dialog opens first, Enter records the typed
// note, and Escape still records - just without a note. The capture itself must carry the note
// text into the stored record instead of the empty string the panel button used to pass.
TEST(MainQmlContractTests, MarkKeyCapturesIssueThroughNoteDialog) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    const QString sourcePath = temporaryDirectory.filePath(QStringLiteral("issue-mark.mp4"));
    QFile source{sourcePath};
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    ASSERT_EQ(source.write("mark-source", 11), 11);
    source.close();

    WorkspaceHarness harness;
    harness.withIssueLog = true;
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate);
    ASSERT_TRUE(installValidatedVideoSet(harness.snapshot,
                                         {std::filesystem::path{sourcePath.toStdWString()}},
                                         rate.value(),
                                         12,
                                         400'000));
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();
    // The projection refresh pulls sourceUrls out of the validated comparison.
    harness.controller->refreshProjection();
    harness.settle();

    auto* const noteDialog = harness.root->findChild<QObject*>(QStringLiteral("issueNoteDialog"));
    ASSERT_NE(noteDialog, nullptr);
    EXPECT_FALSE(noteDialog->property("visible").toBool());
    EXPECT_EQ(harness.issueLog.count(), 0);

    // The panel/menu entry point now opens the same dialog instead of capturing immediately.
    QVariant opened;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        harness.root.get(), "captureIssueLog", Q_RETURN_ARG(QVariant, opened)));
    EXPECT_TRUE(opened.toBool());
    harness.settle();
    EXPECT_TRUE(noteDialog->property("visible").toBool());
    EXPECT_EQ(harness.issueLog.count(), 0);

    auto* const noteField = noteDialog->findChild<QObject*>(QStringLiteral("issueNoteField"));
    ASSERT_NE(noteField, nullptr);
    noteField->setProperty("text", QStringLiteral("块状伪影"));
    ASSERT_TRUE(QMetaObject::invokeMethod(noteDialog, "save"));
    harness.settle();
    EXPECT_FALSE(noteDialog->property("visible").toBool());
    ASSERT_EQ(harness.issueLog.count(), 1);
    const QVariantMap recorded = harness.issueLog.issueAt(0);
    EXPECT_EQ(recorded.value(QStringLiteral("note")).toString(), QStringLiteral("块状伪影"));
    EXPECT_TRUE(harness.root->property("issueLogPanelVisible").toBool());

    // Escape records without a note rather than cancelling the capture. Send a real key
    // event so the field's Keys.onEscapePressed handler is the code under test.
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(), "captureIssueLog"));
    harness.settle();
    EXPECT_TRUE(noteDialog->property("visible").toBool());
    sendKeyToFocus(noteField, QEvent::KeyPress, Qt::Key_Escape);
    sendKeyToFocus(noteField, QEvent::KeyRelease, Qt::Key_Escape);
    harness.settle();
    EXPECT_FALSE(noteDialog->property("visible").toBool());
    ASSERT_EQ(harness.issueLog.count(), 2);
    const QVariantMap emptyNote = harness.issueLog.issueAt(1);
    EXPECT_TRUE(emptyNote.value(QStringLiteral("note")).toString().isEmpty());
    harness.window->close();
}

// The video issue restore is staged and identity-correct: openSources() receives the recorded
// reference (GT) slot - not the timeline's canonical source - and a same-session restore
// replays mode and viewport only through the staged machine, with a session whose sources no
// longer match cancelling instead of touching the viewport.
TEST(MainQmlContractTests, IssueRestoreUsesReferenceIdentityAndStagesViewport) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    const QString pathA = temporaryDirectory.filePath(QStringLiteral("restore-a.mp4"));
    const QString pathB = temporaryDirectory.filePath(QStringLiteral("restore-b.mp4"));
    for (const QString& path : {pathA, pathB}) {
        QFile file{path};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_GT(file.write("restore-source", 15), 0);
    }

    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();
    auto* const surface = harness.root->findChild<QQuickItem*>(QStringLiteral("dualVideoSurface"));
    ASSERT_NE(surface, nullptr);
    const int initialMode = harness.preferences.viewModeCode();
    ASSERT_NE(initialMode, ComparisonSurface::Difference);

    QVariantMap payload{
        {QStringLiteral("decision"), QStringLiteral("ready")},
        {QStringLiteral("kind"), QStringLiteral("video")},
        {QStringLiteral("urls"),
         QVariantList{QUrl::fromLocalFile(pathA), QUrl::fromLocalFile(pathB)}},
        {QStringLiteral("referenceSourceIndex"), 1},
        {QStringLiteral("canonicalSourceIndex"), 0},
        {QStringLiteral("viewMode"), ComparisonSurface::Difference},
        {QStringLiteral("differenceEdge"), 0},
        {QStringLiteral("frame"), QVariant::fromValue<qint64>(41)},
        {QStringLiteral("roiEnabled"), false},
        {QStringLiteral("roiLeft"), 0.0},
        {QStringLiteral("roiTop"), 0.0},
        {QStringLiteral("roiRight"), 1.0},
        {QStringLiteral("roiBottom"), 1.0},
        {QStringLiteral("centerX"), 0.35},
        {QStringLiteral("centerY"), 0.62},
        {QStringLiteral("zoom"), 2.5},
    };

    QVariant returned;
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(),
                                          "applyIssueRestore",
                                          Q_RETURN_ARG(QVariant, returned),
                                          Q_ARG(QVariant, QVariant::fromValue(payload))));
    EXPECT_TRUE(returned.toBool());
    harness.settle();

    // The open command must carry the recorded reference identity on slot 1, not the
    // canonical index.
    ASSERT_FALSE(harness.submitted.empty());
    const auto* const open =
        std::get_if<application::OpenComparisonCommand>(&harness.submitted.back());
    ASSERT_NE(open, nullptr);
    ASSERT_EQ(open->sources.size(), 2U);
    EXPECT_EQ(open->sources[0].role, domain::ComparisonRole::kPrediction);
    EXPECT_EQ(open->sources[1].role, domain::ComparisonRole::kReference);

    // The staged open never completes against a session whose sources are not the recorded
    // ones: after the open command settles, the machine cancels without touching the viewport.
    harness.terminals.push_back(application::CommandTerminal{
        .context = application::commandContext(harness.submitted.back()),
        .outcome = application::CommandOutcome::Succeeded,
    });
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_FALSE(harness.root->property("busy").toBool());
    EXPECT_FALSE(harness.root->property("imageWorkspaceActive").toBool());
    EXPECT_GT(harness.root->property("sourceCount").toInt(), 0);
    EXPECT_EQ(harness.root->property("pendingIssueRestoreStage").toInt(), 0);
    EXPECT_DOUBLE_EQ(surface->property("viewScale").toDouble(), 1.0);

    // Same-session restore (no urls to reopen): the staged machine applies the mode and the
    // saved zoom/pan without a seek when the recorded frame is already presented.
    QVariantMap sameSession = payload;
    sameSession.remove(QStringLiteral("urls"));
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(),
                                          "applyIssueRestore",
                                          Q_RETURN_ARG(QVariant, returned),
                                          Q_ARG(QVariant, QVariant::fromValue(sameSession))));
    EXPECT_TRUE(returned.toBool());
    harness.settle();
    EXPECT_EQ(harness.root->property("pendingIssueRestoreStage").toInt(), 0);
    EXPECT_EQ(harness.preferences.viewModeCode(), ComparisonSurface::Difference);
    EXPECT_DOUBLE_EQ(surface->property("viewScale").toDouble(), 2.5);
    EXPECT_DOUBLE_EQ(surface->property("viewCenterX").toDouble(), 0.35);
    EXPECT_DOUBLE_EQ(surface->property("viewCenterY").toDouble(), 0.62);
    harness.window->close();
}

TEST(MainQmlContractTests, VideoFolderEntryOpensModalPickerWithoutChangingWorkspace) {
    WorkspaceHarness harness;
    harness.withVideoFolder = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.root->setProperty("workspaceMode", 1);
    harness.settle();
    const auto before = harness.submitted.size();
    auto* const entry =
        harness.root->findChild<QObject*>(QStringLiteral("openVideoFolderMenuItem"));
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(entry, "triggered"));
    harness.settle();
    auto* const picker = harness.root->findChild<QObject*>(QStringLiteral("videoFolderDialog"));
    ASSERT_NE(picker, nullptr);
    EXPECT_TRUE(picker->property("visible").toBool());
    EXPECT_EQ(harness.root->property("inputContext").toInt(), 3);
    EXPECT_EQ(harness.root->property("workspaceMode").toInt(), 1);
    ASSERT_TRUE(QMetaObject::invokeMethod(picker, "reject"));
    harness.settle();
    EXPECT_EQ(harness.submitted.size(), before);
    EXPECT_NE(harness.root->findChild<QObject*>(QStringLiteral("emptyOpenVideoFolderButton")),
              nullptr);
}

TEST(MainQmlContractTests, VideoFolderRowOpensOneSourceThenPlaysOnlyAfterSuccess) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    for (const auto* name : {"clip1.mp4", "clip2.mp4", "clip10.mp4"}) {
        QFile file{directory.filePath(QString::fromLatin1(name))};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("fake"), 4);
    }
    WorkspaceHarness harness;
    harness.withVideoFolder = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.root->setProperty("workspaceMode", 1);
    harness.root->setProperty("videoFolderSidebarVisible", true);
    harness.window->show();
    ASSERT_TRUE(harness.videoFolder.loadFolder(QUrl::fromLocalFile(directory.path())));
    ASSERT_TRUE(harness.waitUntil([&] { return !harness.videoFolder.scanning(); }));
    harness.settle();
    auto* const fileList = harness.root->findChild<QObject*>(QStringLiteral("videoFolderFileList"));
    ASSERT_NE(fileList, nullptr);
    QQuickItem* row = nullptr;
    ASSERT_TRUE(harness.waitUntil([&] {
        QMetaObject::invokeMethod(
            fileList, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, row), Q_ARG(int, 1));
        return row != nullptr;
    }));
    EXPECT_EQ(row->objectName(), "videoFolderRow-1");
    harness.submitted.clear();
    ASSERT_TRUE(QMetaObject::invokeMethod(row, "clicked"));
    ASSERT_EQ(harness.submitted.size(), 1U);
    const auto* const open = std::get_if<application::OpenComparisonCommand>(&harness.submitted[0]);
    ASSERT_NE(open, nullptr);
    ASSERT_EQ(open->sources.size(), 1U);
    const auto path = std::filesystem::path{directory.filePath("clip2.mp4").toStdWString()};
    EXPECT_EQ(open->sources.front().path, path);
    EXPECT_FALSE(open->preserveDisplayedTime);
    EXPECT_EQ(open->intent, application::OpenReviewIntent::NewReview);
    EXPECT_EQ(harness.root->property("workspaceMode").toInt(), 1);
    const auto context = open->context;
    auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate.hasValue());
    ASSERT_TRUE(installValidatedVideoSet(harness.snapshot, {path}, rate.value(), 100, 3'333'333));
    const auto sourceViews = harness.snapshot->validatedComparison->sources();
    std::vector<domain::ComparisonSource> sources{sourceViews.begin(), sourceViews.end()};
    QFile opened{QString::fromStdWString(path.wstring())};
    ASSERT_TRUE(opened.open(QIODevice::ReadOnly));
    sources.front().descriptor.sourceIdentity->fingerprintSha256 =
        QCryptographicHash::hash(opened.readAll(), QCryptographicHash::Sha256)
            .toHex()
            .toStdString();
    auto validated = domain::ComparisonValidator::validate(std::move(sources));
    ASSERT_TRUE(validated.hasValue());
    harness.snapshot->validatedComparison =
        std::make_shared<const domain::ValidatedComparisonSet>(std::move(validated).value().set);
    harness.snapshot->sessionId = context.sessionId;
    harness.snapshot->sessionEpoch = context.sessionEpoch;
    harness.snapshot->displayedFrame = domain::FrameId{0};
    harness.terminals.push_back(
        {.context = context, .outcome = application::CommandOutcome::Succeeded});
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_EQ(std::count_if(harness.submitted.begin(),
                            harness.submitted.end(),
                            [](const auto& command) {
                                return std::holds_alternative<application::PlayCommand>(command);
                            }),
              1);
    EXPECT_EQ(harness.videoFolder.currentRow(), 1);
    EXPECT_EQ(harness.root->property("workspaceMode").toInt(), 0);
    auto* const list = harness.root->findChild<QQuickItem*>(QStringLiteral("videoFolderSidebar"));
    auto* const viewport =
        harness.root->findChild<QQuickItem*>(QStringLiteral("mediaViewportFocusTarget"));
    ASSERT_NE(list, nullptr);
    ASSERT_NE(viewport, nullptr);
    EXPECT_LE(list->mapToScene({list->width(), 0}).x(), viewport->mapToScene({0, 0}).x());
    const QString evidence = qEnvironmentVariable("DVS_VIDEO_FOLDER_EVIDENCE_DIR");
    if (!evidence.isEmpty()) {
        harness.settleAnimations();
        ASSERT_TRUE(QDir{}.mkpath(evidence));
        const auto image = harness.window->grabWindow();
        ASSERT_FALSE(image.isNull());
        ASSERT_TRUE(image.save(QDir{evidence}.filePath(QStringLiteral("folder-sidebar.png"))));
    }
    harness.shell->setChromeVisible(false);
    harness.settle();
    EXPECT_FALSE(harness.root->property("videoFolderListVisible").toBool());
}

TEST(MainQmlContractTests, VideoFolderFailureKeepsWorkspaceAndSourcesWithoutPlaying) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QFile file{directory.filePath("broken.mp4")};
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    WorkspaceHarness harness;
    harness.withVideoFolder = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.root->setProperty("workspaceMode", 1);
    ASSERT_TRUE(harness.videoFolder.loadFolder(QUrl::fromLocalFile(directory.path())));
    ASSERT_TRUE(harness.waitUntil([&] { return !harness.videoFolder.scanning(); }));
    const auto original = harness.shell->activeSources();
    harness.submitted.clear();
    QVariant accepted;
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(),
                                          "openFolderVideo",
                                          Q_RETURN_ARG(QVariant, accepted),
                                          Q_ARG(QVariant, QVariant{0})));
    ASSERT_TRUE(accepted.toBool());
    ASSERT_EQ(harness.submitted.size(), 1U);
    const auto context = application::commandContext(harness.submitted.front());
    harness.terminals.push_back(
        {.context = context,
         .outcome = application::CommandOutcome::Failed,
         .error = domain::makeMediaError(domain::MediaErrorCode::kMediaOpenFailed,
                                         domain::MediaOperation::kMediaProbe,
                                         std::nullopt,
                                         true)});
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_EQ(harness.shell->activeSources(), original);
    EXPECT_EQ(harness.root->property("workspaceMode").toInt(), 1);
    EXPECT_EQ(harness.videoFolder.currentRow(), -1);
    EXPECT_FALSE(harness.videoFolder.openPending());
    EXPECT_EQ(harness.videoFolder.errorText(), "media-open-failed");
    EXPECT_EQ(std::count_if(harness.submitted.begin(),
                            harness.submitted.end(),
                            [](const auto& command) {
                                return std::holds_alternative<application::PlayCommand>(command);
                            }),
              0);
}

TEST(MainQmlContractTests, NewerNonBrowserIntentSuppressesOldFolderAutoplay) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QFile file{directory.filePath("clip1.mp4")};
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    WorkspaceHarness harness;
    harness.withVideoFolder = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    ASSERT_TRUE(harness.videoFolder.loadFolder(QUrl::fromLocalFile(directory.path())));
    ASSERT_TRUE(harness.waitUntil([&] { return !harness.videoFolder.scanning(); }));
    harness.submitted.clear();
    ASSERT_TRUE(harness.videoFolder.openAt(0));
    const auto context = application::commandContext(harness.submitted.front());
    ASSERT_NE(harness.shell->openVideo(QUrl::fromLocalFile(directory.filePath("clip2.mp4"))), 0U);
    EXPECT_FALSE(harness.videoFolder.openPending());
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate.hasValue());
    const auto path = std::filesystem::path{file.fileName().toStdWString()};
    ASSERT_TRUE(installValidatedVideoSet(harness.snapshot, {path}, rate.value(), 100, 3'333'333));
    harness.snapshot->sessionId = context.sessionId;
    harness.snapshot->sessionEpoch = context.sessionEpoch;
    harness.terminals.push_back(
        {.context = context, .outcome = application::CommandOutcome::Succeeded});
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_EQ(std::count_if(harness.submitted.begin(),
                            harness.submitted.end(),
                            [](const auto& command) {
                                return std::holds_alternative<application::PlayCommand>(command);
                            }),
              0);
}

TEST(MainQmlContractTests, OrdinarySingleVideoOpenShowsItsFolderAndRecordsOnlySuccessfulHistory) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QString::fromUtf8("片段2.mp4"));
    for (const auto& name : {path, directory.filePath(QString::fromUtf8("片段10.mp4"))}) {
        QFile file{name};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("fake"), 4);
    }
    WorkspaceHarness harness;
    harness.withVideoFolder = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.submitted.clear();
    const QUrl url = QUrl::fromLocalFile(path);
    ASSERT_NE(harness.shell->openVideo(url), 0U);
    ASSERT_EQ(harness.submitted.size(), 1U);
    const auto context = application::commandContext(harness.submitted.front());
    EXPECT_FALSE(harness.root->property("videoFolderSidebarVisible").toBool());
    EXPECT_TRUE(harness.videoFolder.recentFiles().isEmpty());
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate.hasValue());
    ASSERT_TRUE(installValidatedVideoSet(harness.snapshot,
                                         {std::filesystem::path{path.toStdWString()}},
                                         rate.value(),
                                         100,
                                         3'333'333));
    harness.snapshot->sessionId = context.sessionId;
    harness.snapshot->sessionEpoch = context.sessionEpoch;
    harness.terminals.push_back(
        {.context = context, .outcome = application::CommandOutcome::Succeeded});
    harness.controller->refreshProjection();
    ASSERT_TRUE(harness.waitUntil([&] { return !harness.videoFolder.scanning(); }));
    harness.settle();
    EXPECT_TRUE(harness.root->property("videoFolderSidebarVisible").toBool());
    EXPECT_EQ(harness.videoFolder.folderUrl(), QUrl::fromLocalFile(directory.path()));
    EXPECT_EQ(harness.videoFolder.fileCount(), 2);
    EXPECT_EQ(harness.videoFolder.currentRow(), 0);
    ASSERT_EQ(harness.videoFolder.recentFiles().size(), 1);
    EXPECT_EQ(QUrl{harness.preferences.recentVideoFiles().front()}, url);
    // Successful opens reapply pair, continuity and range preferences. None may start playback
    // or submit a second open.
    EXPECT_EQ(std::count_if(harness.submitted.begin(),
                            harness.submitted.end(),
                            [](const auto& command) {
                                return std::holds_alternative<application::OpenComparisonCommand>(
                                    command);
                            }),
              1);
    EXPECT_EQ(std::count_if(harness.submitted.begin(),
                            harness.submitted.end(),
                            [](const auto& command) {
                                return std::holds_alternative<application::PlayCommand>(command);
                            }),
              0);

    auto* const tab = harness.root->findChild<QObject*>(QStringLiteral("videoRecentTab"));
    ASSERT_NE(tab, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(tab, "clicked"));
    harness.settle();
    auto* const list = harness.root->findChild<QObject*>(QStringLiteral("videoRecentFileList"));
    ASSERT_NE(list, nullptr);
    QQuickItem* row = nullptr;
    ASSERT_TRUE(QMetaObject::invokeMethod(list, "forceLayout"));
    ASSERT_TRUE(harness.waitUntil([&] {
        QMetaObject::invokeMethod(
            list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, row), Q_ARG(int, 0));
        return row != nullptr;
    }));
    EXPECT_TRUE(row->property("highlighted").toBool());
    const auto history = harness.videoFolder.recentFiles();
    harness.submitted.clear();
    ASSERT_TRUE(QMetaObject::invokeMethod(row, "clicked"));
    ASSERT_EQ(harness.submitted.size(), 1U);
    const auto* const reopened =
        std::get_if<application::OpenComparisonCommand>(&harness.submitted.front());
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->sources.size(), 1U);
    EXPECT_EQ(reopened->intent, application::OpenReviewIntent::NewReview);
    const auto failedContext = reopened->context;
    harness.terminals.push_back(
        {.context = failedContext,
         .outcome = application::CommandOutcome::Failed,
         .error = domain::makeMediaError(domain::MediaErrorCode::kMediaOpenFailed,
                                         domain::MediaOperation::kMediaProbe,
                                         std::nullopt,
                                         true)});
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_EQ(harness.videoFolder.recentFiles(), history);
    EXPECT_EQ(harness.videoFolder.currentUrl(), url);
    EXPECT_EQ(harness.root->property("workspaceMode").toInt(), 0);
    EXPECT_EQ(harness.submitted.size(), 1U);
}

TEST(MainQmlContractTests, SidebarMenuBlocksShortcutsBeforeAnyRowHasFocus) {
    WorkspaceHarness harness;
    harness.withVideoFolder = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.root->setProperty("videoFolderSidebarVisible", true);
    harness.window->show();
    harness.window->requestActivate();
    harness.settleAnimations();
    auto* const menu = harness.root->findChild<QObject*>(QStringLiteral("videoFileContextMenu"));
    ASSERT_NE(menu, nullptr);
    harness.submitted.clear();
    ASSERT_TRUE(QMetaObject::invokeMethod(menu, "open"));
    ASSERT_TRUE(harness.waitUntil([&] { return menu->property("opened").toBool(); }));
    ASSERT_EQ(menu->property("currentIndex").toInt(), -1);
    EXPECT_EQ(harness.root->property("inputContext").toInt(), 2);
    EXPECT_FALSE(harness.root->property("globalMediaShortcutsEnabled").toBool());
    EXPECT_FALSE(harness.root->property("presentationShortcutsEnabled").toBool());
    sendKey(*harness.window, Qt::Key_Space);
    sendKey(*harness.window, Qt::Key_Left);
    sendKey(*harness.window, Qt::Key_O, Qt::ControlModifier);
    harness.settle();
    EXPECT_TRUE(harness.submitted.empty());
    EXPECT_EQ(harness.root->property("inputContext").toInt(), 2);
    ASSERT_TRUE(QMetaObject::invokeMethod(menu, "close"));
    ASSERT_TRUE(harness.waitUntil([&] { return !menu->property("visible").toBool(); }));
    auto* const viewport =
        harness.root->findChild<QQuickItem*>(QStringLiteral("mediaViewportFocusTarget"));
    ASSERT_NE(viewport, nullptr);
    viewport->forceActiveFocus();
    harness.settle();
    ASSERT_EQ(harness.root->property("inputContext").toInt(), 0);
    ASSERT_TRUE(harness.root->property("globalMediaShortcutsEnabled").toBool());
    sendKey(*harness.window, Qt::Key_Space);
    EXPECT_EQ(std::count_if(harness.submitted.begin(),
                            harness.submitted.end(),
                            [](const auto& command) {
                                return std::holds_alternative<application::PlayCommand>(command);
                            }),
              1);
}

TEST(MainQmlContractTests, SidebarComparisonEscapeAndAcceptPreserveCurrentReference) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    std::vector<std::filesystem::path> paths;
    for (const auto* name : {"prediction.mp4", "gt.mp4", "candidate.mp4"}) {
        QFile file{directory.filePath(QString::fromLatin1(name))};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("fake"), 4);
        paths.emplace_back(file.fileName().toStdWString());
    }
    WorkspaceHarness harness;
    harness.withVideoFolder = true;
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate.hasValue());
    ASSERT_TRUE(installValidatedVideoSet(
        harness.snapshot, {paths[0], paths[1]}, rate.value(), 100, 3'333'333));
    const auto views = harness.snapshot->validatedComparison->sources();
    std::vector<domain::ComparisonSource> sources{views.begin(), views.end()};
    sources[0].role = domain::ComparisonRole::kPrediction;
    sources[1].role = domain::ComparisonRole::kReference;
    auto validated = domain::ComparisonValidator::validate(std::move(sources));
    ASSERT_TRUE(validated.hasValue());
    harness.snapshot->validatedComparison =
        std::make_shared<const domain::ValidatedComparisonSet>(std::move(validated).value().set);
    harness.snapshot->sources[0].role = domain::ComparisonRole::kPrediction;
    harness.snapshot->sources[1].role = domain::ComparisonRole::kReference;
    harness.controller->refreshProjection();
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.root->setProperty("videoFolderSidebarVisible", true);
    harness.window->show();
    harness.window->requestActivate();
    harness.settleAnimations();
    harness.submitted.clear();
    auto* sidebar = harness.root->findChild<QObject*>(QStringLiteral("videoFolderSidebar"));
    ASSERT_NE(sidebar, nullptr);
    const auto original = harness.shell->activeSources();
    const QUrl candidate = QUrl::fromLocalFile(QString::fromStdWString(paths[2].wstring()));
    ASSERT_TRUE(QMetaObject::invokeMethod(sidebar, "compareRequested", Q_ARG(QUrl, candidate)));
    harness.settleAnimations();
    auto* dialog = harness.root->findChild<QObject*>(QStringLiteral("dropReviewDialog"));
    ASSERT_NE(dialog, nullptr);
    ASSERT_TRUE(dialog->property("visible").toBool());
    EXPECT_TRUE(harness.shell->sidebarAppendStaged());
    EXPECT_EQ(dialog->property("referenceIndex").toInt(), 1);
    EXPECT_TRUE(harness.submitted.empty());
    sendKey(*harness.window, Qt::Key_Escape);
    ASSERT_TRUE(harness.waitUntil([&] { return !dialog->property("visible").toBool(); }));
    EXPECT_FALSE(harness.root->property("pendingSidebarComparison").toBool());
    EXPECT_FALSE(harness.shell->sidebarAppendStaged());
    EXPECT_TRUE(harness.submitted.empty());
    EXPECT_EQ(harness.shell->activeSources(), original);
    EXPECT_EQ(harness.shell->referenceSourceIndex(), 1);

    ASSERT_TRUE(QMetaObject::invokeMethod(sidebar, "compareRequested", Q_ARG(QUrl, candidate)));
    harness.settleAnimations();
    ASSERT_TRUE(dialog->property("visible").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "accept"));
    harness.settle();
    EXPECT_FALSE(harness.root->property("pendingSidebarComparison").toBool());
    ASSERT_EQ(harness.submitted.size(), 1U);
    const auto* open = std::get_if<application::OpenComparisonCommand>(&harness.submitted[0]);
    ASSERT_NE(open, nullptr);
    ASSERT_EQ(open->sources.size(), 3U);
    EXPECT_EQ(open->sources[1].path, paths[1]);
    EXPECT_EQ(open->sources[1].role, domain::ComparisonRole::kReference);
    EXPECT_EQ(open->sources[2].path, paths[2]);
    EXPECT_TRUE(open->preserveDisplayedTime);
    EXPECT_EQ(open->intent, application::OpenReviewIntent::ReplaceSources);
}

TEST(MainQmlContractTests, OpeningComparisonDoesNotAutoBrowseOrRecordASingleVideo) {
    WorkspaceHarness harness;
    harness.withVideoFolder = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    std::vector<std::filesystem::path> paths;
    for (const auto* name : {"gt.mp4", "pred.mp4"}) {
        QFile file{directory.filePath(QString::fromLatin1(name))};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("fake"), 4);
        paths.emplace_back(file.fileName().toStdWString());
    }
    ASSERT_TRUE(harness.shell->stageSources(
        {QUrl::fromLocalFile(QString::fromStdWString(paths[0].wstring())),
         QUrl::fromLocalFile(QString::fromStdWString(paths[1].wstring()))},
        1));
    harness.submitted.clear();
    ASSERT_TRUE(harness.shell->openStagedSources(false));
    ASSERT_EQ(harness.submitted.size(), 1U);
    const auto context = application::commandContext(harness.submitted.front());
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate.hasValue());
    ASSERT_TRUE(installValidatedVideoSet(harness.snapshot, paths, rate.value(), 100, 3'333'333));
    const auto views = harness.snapshot->validatedComparison->sources();
    std::vector<domain::ComparisonSource> sources{views.begin(), views.end()};
    sources[0].role = domain::ComparisonRole::kPrediction;
    sources[1].role = domain::ComparisonRole::kReference;
    auto validated = domain::ComparisonValidator::validate(std::move(sources));
    ASSERT_TRUE(validated.hasValue());
    harness.snapshot->validatedComparison =
        std::make_shared<const domain::ValidatedComparisonSet>(std::move(validated).value().set);
    harness.snapshot->sources[0].role = domain::ComparisonRole::kPrediction;
    harness.snapshot->sources[1].role = domain::ComparisonRole::kReference;
    harness.snapshot->sessionId = context.sessionId;
    harness.snapshot->sessionEpoch = context.sessionEpoch;
    harness.terminals.push_back(
        {.context = context, .outcome = application::CommandOutcome::Succeeded});
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_EQ(harness.shell->activeSources().size(), 2);
    EXPECT_EQ(harness.shell->referenceSourceIndex(), 1);
    EXPECT_TRUE(harness.videoFolder.recentFiles().isEmpty());
    EXPECT_TRUE(harness.videoFolder.folderUrl().isEmpty());
    EXPECT_FALSE(harness.videoFolder.scanning());
    EXPECT_FALSE(harness.root->property("videoFolderSidebarVisible").toBool());
    EXPECT_EQ(std::count_if(harness.submitted.begin(),
                            harness.submitted.end(),
                            [](const auto& command) {
                                return std::holds_alternative<application::OpenComparisonCommand>(
                                    command);
                            }),
              1);
    EXPECT_EQ(std::count_if(harness.submitted.begin(),
                            harness.submitted.end(),
                            [](const auto& command) {
                                return std::holds_alternative<application::PlayCommand>(command);
                            }),
              0);
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
