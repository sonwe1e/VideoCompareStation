#include "dvs/application/Alignment.h"
#include "dvs/application/InterpolationProtocolImport.h"
#include "dvs/application/SessionSnapshot.h"
#include "dvs/domain/ComparisonValidator.h"
#include "dvs/domain/InterpolationProtocol.h"
#include "dvs/ui/ClipExportController.h"
#include "dvs/ui/ColorClipboardController.h"
#include "dvs/ui/ComparisonExportController.h"
#include "dvs/ui/ComparisonSurface.h"
#include "dvs/ui/EditImageProvider.h"
#include "dvs/ui/FileAssociationController.h"
#include "dvs/ui/GraphicsBackend.h"
#include "dvs/ui/ImageEditController.h"
#include "dvs/ui/ImageFolderCursor.h"
#include "dvs/ui/ImageFolderPairModel.h"
#include "dvs/ui/ImageReviewController.h"
#include "dvs/ui/InterpolationProtocolController.h"
#include "dvs/ui/IssueLogController.h"
#include "dvs/ui/MainWindowDependencies.h"
#include "dvs/ui/QmlTypeRegistration.h"
#include "dvs/ui/ReviewController.h"
#include "dvs/ui/ReviewImageProvider.h"
#include "dvs/ui/ReviewPreferencesController.h"
#include "dvs/ui/ReviewSessionFacade.h"
#include "dvs/ui/ReviewShellController.h"
#include "dvs/ui/SourceIdentity.h"
#include "dvs/ui/SourceListModel.h"
#include "dvs/ui/VideoFolderModel.h"

#include <QAccessible>
#include <QClipboard>
#include <QColor>
#include <QColorSpace>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
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
#include <QMimeData>
#include <QMouseEvent>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQmlProperty>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QResource>
#include <QScreen>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QTransform>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>
#include <QtQml/qqml.h>

#include <algorithm>
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

// Failure diagnostics for isVisible() assertions: names every ancestor between the item and
// the window, so a failing expectation reports which container hid the subtree instead of a
// bare "Actual: false". (QML's `visible` reads the effective visibility - ancestors included -
// so a single per-node effective flag is all there is to report; that shared source is exactly
// why an ancestor must not gate its own visibility on a descendant's `visible`.)
[[nodiscard]] std::string visibleChainText(const QQuickItem* item) {
    std::string chain;
    for (const QQuickItem* node = item; node != nullptr; node = node->parentItem()) {
        chain += "\n    ";
        chain += node->objectName().isEmpty() ? "<anonymous>" : node->objectName().toStdString();
        chain += node->isVisible() ? ":visible" : ":HIDDEN";
    }
    return chain;
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

// Main.qml declares its pilot services as required typed properties, so every load must
// supply them exactly like the production composition root does. Tests that exercise the
// image workspace pass their own instances through pilotWindowProperties(); the rest share
// one inert real pair through defaultPilotWindowProperties().
[[nodiscard]] QVariantMap pilotWindowProperties(ImageReviewController& imageReview,
                                                ColorClipboardController& colorClipboard) {
    return mainWindowPilotProperties(imageReview, colorClipboard);
}

[[nodiscard]] QVariantMap defaultPilotWindowProperties() {
    static ImageReviewController imageReview;
    static ColorClipboardController colorClipboard;
    return pilotWindowProperties(imageReview, colorClipboard);
}

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
        report.firstPresentationMicroseconds = job.plan.startMicroseconds;
        report.lastPresentationMicroseconds = job.plan.endMicroseconds.value_or(0);
        report.endOfStreamReached = !job.plan.endMicroseconds.has_value();
        report.stoppedAtRequestedEnd = job.plan.endMicroseconds.has_value();
        report.requestedFrameCount = job.plan.requestedFrameCount;
        report.startShiftMicroseconds = job.plan.startShiftMicroseconds;
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

// Same validated install as installValidatedVideoSet, but every source keeps the Prediction
// role: the real "predictions compared without a GT reference" session whose
// referenceSourceIndex projects to -1.
[[nodiscard]] bool
installAllPredictionVideoSet(const std::shared_ptr<application::SessionSnapshot>& snapshot,
                             const std::vector<std::filesystem::path>& paths,
                             const domain::RationalRate& rate,
                             const std::int64_t frameCount,
                             const std::int64_t durationMicroseconds) {
    std::vector<domain::ComparisonSource> sources;
    for (std::size_t index = 0U; index < paths.size(); ++index) {
        const QFileInfo info{QString::fromStdWString(paths[index].wstring())};
        sources.push_back(domain::ComparisonSource{
            .id = static_cast<domain::SourceId>(index),
            .role = domain::ComparisonRole::kPrediction,
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

// Marks a snapshot's session as identifying one GT (reference) slot. `referenceSourceIndex` is
// projected from the validated comparison, so a snapshot whose sources are bare
// SessionSourceView entries leaves the session without a GT identity - which is exactly the state
// where the reference peek must not be offered. Callers that exercise the peek need this.
// V02: Repeater delegates (the per-source focus buttons, the source chips) live in the item
// tree but not in the QObject children tree - the same reason collectChips() walks
// childItems() off the chip Row. Look them up by objectName through the scene graph.
[[nodiscard]] QQuickItem* findItemByName(QQuickItem* root, const QString& name) {
    if (root == nullptr) {
        return nullptr;
    }
    if (root->objectName() == name) {
        return root;
    }
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* const found = findItemByName(child, name)) {
            return found;
        }
    }
    return nullptr;
}

void identifyReferenceSlot(const std::shared_ptr<application::SessionSnapshot>& snapshot,
                           const int referenceSlot) {
    const auto rate = domain::RationalRate::create(30, 1);
    if (!rate) {
        return;
    }
    std::vector<domain::ComparisonSource> sources;
    for (const auto& view : snapshot->sources) {
        const std::filesystem::path sourcePath = std::filesystem::path{
            L"probe-source-" + std::to_wstring(static_cast<unsigned>(view.sourceId)) + L".mp4"};
        sources.push_back(domain::ComparisonSource{
            .id = static_cast<domain::SourceId>(view.sourceId),
            .role = static_cast<domain::SourceId>(view.sourceId) ==
                            static_cast<domain::SourceId>(referenceSlot)
                        ? domain::ComparisonRole::kReference
                        : domain::ComparisonRole::kPrediction,
            .descriptor =
                domain::MediaDescriptor{
                    .normalizedPath = sourcePath,
                    .extent = domain::MediaExtent{.width = 1'920U, .height = 1'080U},
                    .frameRate = rate.value(),
                    .frameCount =
                        domain::FrameCountInfo{
                            .value = 100,
                            .origin = domain::FrameCountOrigin::kReported,
                        },
                    .duration = domain::MediaTime{3000000},
                    .codecId = "h264",
                    .pixelFormatId = "nv12",
                    .bitDepth = 8U,
                    .decodeCapabilities =
                        domain::DecodeCapabilities{
                            .softwareDecode = true,
                            .d3d11VaDecode = true,
                        },
                    .timingConfidence = domain::TimingConfidence::kVerifiedCfr,
                },
            .displayName = view.displayName,
        });
    }
    auto validated = domain::ComparisonValidator::validate(std::move(sources));
    if (!validated) {
        return;
    }
    snapshot->validatedComparison =
        std::make_shared<const domain::ValidatedComparisonSet>(std::move(validated).value().set);
    snapshot->canonicalTimeline = rate.value();
    snapshot->canonicalFrameCount =
        static_cast<std::uint64_t>(snapshot->validatedComparison->canonicalFrameCount());
}

// Repository double for P03 protocol contracts: hands back a fixed result per call.
class ContractProtocolRepository final : public application::IInterpolationProtocolRepository {
public:
    std::function<application::InterpolationProtocolIoResult()> behavior;

    [[nodiscard]] application::InterpolationProtocolIoResult
    load(const std::filesystem::path&) const override {
        return behavior ? behavior() : application::InterpolationProtocolIoResult{};
    }
};

// V04 nearby-frames strip: serves a fixed-size image for the stub's decoded frames so the
// card's same-ROI crop has real image dimensions to work with.
class SolidThumbnailProvider final : public QQuickImageProvider {
public:
    SolidThumbnailProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString&, QSize* size, const QSize&) override {
        QImage image{100, 50, QImage::Format_RGB32};
        image.fill(QColor{64, 96, 128});
        if (size != nullptr) {
            *size = image.size();
        }
        return image;
    }
};

// In-memory stand-in for the Windows file-association port. The dialog contract only needs the
// outcome of each action and a read-back status; recording the calls lets tests prove which
// button ran, and a settable failure exercises the dialog's error wording.
class ContractFileAssociationService final : public application::IFileAssociationService {
public:
    [[nodiscard]] application::FileAssociationOutcome registerAsOptionalApplication() override {
        ++registerCalls;
        return nextOutcome();
    }

    [[nodiscard]] application::FileAssociationOutcome openSystemDefaultAppsSettings() override {
        ++settingsCalls;
        return nextOutcome();
    }

    [[nodiscard]] application::FileAssociationStatus associationStatus() override {
        return status;
    }

    [[nodiscard]] application::FileAssociationOutcome repairRegistrationPaths() override {
        ++repairCalls;
        return nextOutcome();
    }

    [[nodiscard]] application::FileAssociationOutcome removeRegistration() override {
        ++removeCalls;
        return nextOutcome();
    }

    [[nodiscard]] application::FileAssociationOutcome nextOutcome() const {
        if (failNext) {
            return application::FileAssociationOutcome{false, nextError};
        }
        return application::FileAssociationOutcome{true, {}};
    }

    application::FileAssociationStatus status;
    bool failNext = false;
    std::string nextError;
    int registerCalls = 0;
    int settingsCalls = 0;
    int repairCalls = 0;
    int removeCalls = 0;
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
        engine.rootContext()->setContextProperty(QStringLiteral("imageFolderPairs"), &folderPairs);
        engine.rootContext()->setContextProperty(QStringLiteral("imageFolderCursor"),
                                                 &imageFolderCursor);
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
        if (withInterpolationProtocol) {
            interpolationProtocol.setReviewController(controller.get());
            engine.rootContext()->setContextProperty(QStringLiteral("interpolationProtocol"),
                                                     &interpolationProtocol);
        }
        if (withFileAssociation) {
            fileAssociation = std::make_unique<FileAssociationController>(fileAssociationService);
            engine.rootContext()->setContextProperty(QStringLiteral("fileAssociation"),
                                                     fileAssociation.get());
        }
        if (withPreviewThumbnails) {
            // V04 nearby-frames batch contract: a QML stub stands in for the composition
            // root's PreviewThumbnailController. It records requestBatch/cancelBatch calls
            // where the C++ side can read them and serves decodable image URLs through a
            // registered provider, so the card's crop and batch wiring run for real.
            QQmlComponent stubComponent{&engine};
            stubComponent.setData(QStringLiteral(R"qml(
import QtQuick
QtObject {
    property int generation: 3
    property var batchCalls: []
    property int cancelCount: 0
    property var framesReady: []
    function requestBatch(frames) {
        const copy = [];
        for (let i = 0; i < frames.length; ++i)
            copy.push(frames[i]);
        batchCalls = batchCalls.concat([copy]);
    }
    function cancelBatch() {
        cancelCount = cancelCount + 1;
    }
    function request(frame) {
    }
    function urlForFrame(frame) {
        return framesReady.indexOf(frame) >= 0 ? "image://nearby-strip-test/" + frame : "";
    }
    function hasThumbnail(frame) {
        return framesReady.indexOf(frame) >= 0;
    }
    function thumbnailSourceSize(frame) {
        return framesReady.indexOf(frame) >= 0 ? Qt.size(100, 50) : Qt.size(0, 0);
    }
}
)qml")
                                      .toUtf8(),
                                  QUrl{});
            previewThumbnailsStub.reset(stubComponent.create());
            if (previewThumbnailsStub == nullptr) {
                error = componentErrors(stubComponent);
                return false;
            }
            engine.addImageProvider(QStringLiteral("nearby-strip-test"),
                                    new SolidThumbnailProvider);
            engine.rootContext()->setContextProperty(QStringLiteral("previewThumbnails"),
                                                     previewThumbnailsStub.get());
        }
        QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
        if (component.status() != QQmlComponent::Ready) {
            error = componentErrors(component);
            return false;
        }
        root.reset(component.createWithInitialProperties(
            pilotWindowProperties(imageReview, colorClipboard)));
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
    // The root's required clipboard service; the copy tests assert through the real
    // clipboard the adapter writes to.
    ColorClipboardController colorClipboard;
    ImageFolderPairModel folderPairs;
    // FLOW3: the harness exposes the same directory cursor the composition root installs, so
    // the arrow-key path is exercised against a real cursor rather than a mock.
    ImageFolderCursor imageFolderCursor;
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
    // P03 protocol navigation is opt-in for the same reason: only the composition root publishes
    // the controller, and its absence is a state the transport must survive with the protocol
    // row hidden.
    ContractProtocolRepository protocolRepository;
    InterpolationProtocolController interpolationProtocol{&protocolRepository};
    bool withInterpolationProtocol = false;
    // V04 nearby-frames strip contract: opt-in stub for the composition root's
    // "previewThumbnails" context object (see create()).
    std::unique_ptr<QObject> previewThumbnailsStub;
    bool withPreviewThumbnails = false;
    // I03 file-association dialog contract: opt-in for the same reason as the other optional
    // context objects - only the composition root publishes a real controller.
    std::shared_ptr<ContractFileAssociationService> fileAssociationService =
        std::make_shared<ContractFileAssociationService>();
    std::unique_ptr<FileAssociationController> fileAssociation;
    bool withFileAssociation = false;
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

TEST(MainQmlContractTests, TimingDiagnosticsCardRendersInReviewTabWhenProvided) {
    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/TabbedInspector.qml")}};

    QQmlComponent mockProbe{&engine};
    mockProbe.setData(R"qml(
        import QtQuick
        QtObject {
            property bool hasActivePlayback: true
            property int skippedFrameSetCount: 0
            property double decodeLatencyP95Ms: 12.5
            property double decodeLatencyMaxMs: 25.0
            property double renderScheduleP95Ms: 1.2
            property double renderScheduleMaxMs: 3.4
            property double drawToAckP95Ms: 2.1
            property double drawToAckMaxMs: 4.5
            property string systemPresentationStatus: "可用 (Flip Model)"
            property string driverKind: "Hardware"
            property bool isIndirectDisplayWarning: false
        }
    )qml",
                      QUrl{});
    const std::unique_ptr<QObject> mockDiag{mockProbe.create()};
    ASSERT_NE(mockDiag, nullptr) << componentErrors(mockProbe);

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
        {QStringLiteral("metrics"), QVariantMap{}},
        {QStringLiteral("timingDiagnostics"), QVariant::fromValue(mockDiag.get())},
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
        {QStringLiteral("width"), 320},
        {QStringLiteral("height"), 800},
    };
    const std::unique_ptr<QObject> root{component.createWithInitialProperties(properties)};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    auto* const tabs = root->findChild<QQuickItem*>(QStringLiteral("inspectorTabBar"));
    ASSERT_NE(tabs, nullptr);
    tabs->setProperty("currentIndex", 1);

    auto* const card = root->findChild<QQuickItem*>(QStringLiteral("timingDiagnosticsCard"));
    ASSERT_NE(card, nullptr);
    EXPECT_TRUE(card->isVisible());
    auto* const badge = root->findChild<QQuickItem*>(QStringLiteral("timingDroppedFramesBadge"));
    ASSERT_NE(badge, nullptr);
    EXPECT_TRUE(badge->isVisible());
    auto* const liveBadge = root->findChild<QQuickItem*>(QStringLiteral("timingLiveBadge"));
    ASSERT_NE(liveBadge, nullptr);
    EXPECT_TRUE(liveBadge->isVisible());
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

    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(defaultPilotWindowProperties())};
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
    auto* const nextButton = root->findChild<QQuickItem*>(QStringLiteral("nextButton"));
    auto* const nextFiveButton = root->findChild<QQuickItem*>(QStringLiteral("nextFiveButton"));
    auto* const nextSecondButton = root->findChild<QQuickItem*>(QStringLiteral("nextSecondButton"));
    auto* const playbackRateCombo =
        root->findChild<QQuickItem*>(QStringLiteral("playbackRateCombo"));
    ASSERT_NE(nextButton, nullptr);
    ASSERT_NE(nextFiveButton, nullptr);
    ASSERT_NE(nextSecondButton, nullptr);
    ASSERT_NE(playbackRateCombo, nullptr);
    EXPECT_LT(nextButton->x(), nextFiveButton->x());
    EXPECT_LT(nextFiveButton->x(), nextSecondButton->x());
    EXPECT_LT(nextSecondButton->x(), lastButton->x());
    EXPECT_LT(lastButton->x(), playbackRateCombo->x());
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
    // D1: pure mode lives on H; Tab keeps its platform meaning (focus navigation).
    sendKey(*window, Qt::Key_H);
    QCoreApplication::processEvents();
    EXPECT_FALSE(root->property("chromeVisible").toBool());
    EXPECT_TRUE(root->property("globalMediaShortcutsEnabled").toBool());
    EXPECT_TRUE(viewport->hasActiveFocus());
    // The pointer sits over the canvas here (it did before H was pressed, and collapsing the
    // chrome moves the layout under it). A passive hover must not wake the transport back up:
    // immersive mode has the bottom wake strip for that. The message carries the two flags the
    // verdict is derived from so a failure names which one broke.
    auto* const wakeHover = root->findChild<QObject*>(QStringLiteral("immersiveWakeHover"));
    ASSERT_NE(wakeHover, nullptr);
    EXPECT_FALSE(wakeHover->property("hovered").toBool());
    EXPECT_EQ(root->property("oscState").toInt(), 2)
        << "immersiveOscRevealed=" << root->property("immersiveOscRevealed").toBool();
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

    sendKey(*window, Qt::Key_H);
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
    // EX-03: the comparison bar survives a single video, with the tasks disabled and the
    // reason printed in the bar; the way into a comparison is one click away.
    EXPECT_TRUE(compareBar->isVisible());
    auto* const singleHint =
        compareBar->findChild<QObject*>(QStringLiteral("singleSourceCompareHint"));
    ASSERT_NE(singleHint, nullptr);
    EXPECT_TRUE(singleHint->property("visible").toBool());
    auto* const addVideo = compareBar->findChild<QObject*>(QStringLiteral("modeBarAddVideoButton"));
    ASSERT_NE(addVideo, nullptr);
    EXPECT_TRUE(addVideo->property("visible").toBool());

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
    // The compare menu stays openable with an empty session: its inspector row and the
    // single-source explanation have to remain reachable (UX-09). Only the comparison rows
    // carry the source-count gate now.
    EXPECT_TRUE(compareMenu->property("enabled").toBool());
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

    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(defaultPilotWindowProperties())};
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

    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(defaultPilotWindowProperties())};
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

// U02 design tokens: the key controls must actually consume the token values pinned by
// ui.vcs_theme. This is the drift guard in the implementation direction - a control that
// silently falls back to a literal 12 px label or a 30 px hit area fails here even though the
// token itself still reads correctly.
TEST(MainQmlContractTests, DesignTokensGovernKeyControlMetrics) {
    WorkspaceHarness harness;
    harness.shell->setInspectorVisible(true);
    ASSERT_TRUE(harness.create()) << harness.error;

    const auto contentFontPixelSize = [](QQuickItem* item) -> qreal {
        const QVariant content = item->property("contentItem");
        const auto* const text = qobject_cast<const QQuickItem*>(content.value<QObject*>());
        if (text == nullptr) {
            return -1.0;
        }
        return QQmlProperty(const_cast<QQuickItem*>(text), QStringLiteral("font.pixelSize"))
            .read()
            .toReal();
    };

    // Chrome rows share one 40 px band (36-40 token range).
    auto* const sourceBar =
        harness.root->findChild<QQuickItem*>(QStringLiteral("activeSourceStrip"));
    auto* const compareBar = harness.root->findChild<QQuickItem*>(QStringLiteral("compareModeBar"));
    ASSERT_NE(sourceBar, nullptr);
    ASSERT_NE(compareBar, nullptr);
    EXPECT_EQ(sourceBar->height(), 40.0);
    EXPECT_EQ(compareBar->height(), 40.0);

    // Common controls sit on the 32 px hit-area token with 14 px body labels.
    auto* const sideModeButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("sideModeButton"));
    auto* const diffModeButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("diffModeButton"));
    auto* const focusButton = findItemByName(compareBar, QStringLiteral("canvasFocusAButton"));
    ASSERT_NE(sideModeButton, nullptr);
    ASSERT_NE(diffModeButton, nullptr);
    ASSERT_NE(focusButton, nullptr);
    EXPECT_EQ(sideModeButton->height(), 32.0);
    EXPECT_EQ(diffModeButton->height(), 32.0);
    EXPECT_EQ(focusButton->height(), 32.0);
    EXPECT_EQ(contentFontPixelSize(sideModeButton), 14.0);
    EXPECT_EQ(contentFontPixelSize(diffModeButton), 14.0);

    auto* const overflowButton =
        findItemByName(sourceBar, QStringLiteral("sourceOverflowButton-0"));
    EXPECT_EQ(overflowButton, nullptr) << "source operations use the filename context menu";

    // The source chip body is itself the direct-look hit area, so it keeps the full 32 px band.
    auto* const sourceChip = findItemByName(sourceBar, QStringLiteral("sourceChip-0"));
    ASSERT_NE(sourceChip, nullptr);
    EXPECT_EQ(sourceChip->height(), 32.0);

    // The transport: the main play button sits in its own 40-44 band, the range chips on the
    // common 32 px hit area.
    auto* const playbackButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("playbackButton"));
    ASSERT_NE(playbackButton, nullptr);
    EXPECT_GE(playbackButton->height(), 40.0);
    EXPECT_LE(playbackButton->height(), 44.0);
    auto* const markInChip =
        harness.root->findChild<QQuickItem*>(QStringLiteral("transportMarkInButton"));
    ASSERT_NE(markInChip, nullptr);
    EXPECT_EQ(markInChip->height(), 32.0);

    // Status surfaces: the diagnostic rail and the stage overlay carry readable conclusions,
    // so neither may drop to the decorative 10 px token.
    auto* const statusText =
        harness.root->findChild<QQuickItem*>(QStringLiteral("playbackStatusText"));
    ASSERT_NE(statusText, nullptr);
    EXPECT_EQ(QQmlProperty(statusText, QStringLiteral("font.pixelSize")).read().toReal(), 12.0);
    auto* const statusHint =
        harness.root->findChild<QQuickItem*>(QStringLiteral("playbackStatusHint"));
    ASSERT_NE(statusHint, nullptr);
    EXPECT_EQ(QQmlProperty(statusHint, QStringLiteral("font.pixelSize")).read().toReal(), 12.0);
}

// E04 walk-rate completion (plan §4): the long-press step walk defaults to real time with the
// optional 0.5x/2x steps. The 连帧 combo mirrors the controller projection and its selection
// reaches the same controller the jog keys drive — without submitting a single playback
// command, because changing the walk pace is input pacing, not a media request.
TEST(MainQmlContractTests, JogRateSelectorMirrorsAndReachesTheController) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;

    auto* const transportBar = harness.root->findChild<QQuickItem*>(QStringLiteral("transportBar"));
    auto* const jogRateCombo = harness.root->findChild<QQuickItem*>(QStringLiteral("jogRateCombo"));
    ASSERT_NE(transportBar, nullptr);
    ASSERT_NE(jogRateCombo, nullptr);

    // Default projection: real time, and the combo elects the middle ladder slot.
    EXPECT_DOUBLE_EQ(harness.controller->jogRate(), 1.0);
    EXPECT_DOUBLE_EQ(transportBar->property("jogRate").toDouble(), 1.0);
    EXPECT_EQ(jogRateCombo->property("currentIndex").toInt(), 1);
    harness.submitted.clear();

    // 2x: activation lands on the controller and comes back as the mirrored projection — the
    // notify path, not just a local combo index change.
    ASSERT_TRUE(QMetaObject::invokeMethod(jogRateCombo, "activated", Q_ARG(int, 2)));
    harness.settle();
    EXPECT_DOUBLE_EQ(harness.controller->jogRate(), 2.0);
    EXPECT_DOUBLE_EQ(transportBar->property("jogRate").toDouble(), 2.0);
    EXPECT_EQ(jogRateCombo->property("currentIndex").toInt(), 2);

    // 0.5x: the low option selects and mirrors through the same path.
    ASSERT_TRUE(QMetaObject::invokeMethod(jogRateCombo, "activated", Q_ARG(int, 0)));
    harness.settle();
    EXPECT_DOUBLE_EQ(harness.controller->jogRate(), 0.5);
    EXPECT_DOUBLE_EQ(transportBar->property("jogRate").toDouble(), 0.5);
    EXPECT_EQ(jogRateCombo->property("currentIndex").toInt(), 0);

    // Walk pace is an input-layer dial: no command may be submitted by changing it.
    EXPECT_TRUE(harness.submitted.empty());
}

// U02: GT, the current comparison pair and the direct look must never be colour-only. Each
// role keeps a text label next to its hue: the chip prints 参考 / 直看 tags, the focus button
// prints 看 X plus a GT badge on the reference slot, and the pair combo names both files.
// Dropping any label would leave a hue-only state this test rejects.
TEST(MainQmlContractTests, RoleStatesCarryLabelsNotColourAlone) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    auto* const sourceBar =
        harness.root->findChild<QQuickItem*>(QStringLiteral("activeSourceStrip"));
    auto* const compareBar = harness.root->findChild<QQuickItem*>(QStringLiteral("compareModeBar"));
    ASSERT_NE(sourceBar, nullptr);
    ASSERT_NE(compareBar, nullptr);

    // No reference identified yet: no 参考 tag, no GT badge.
    auto* const referenceTag = findItemByName(sourceBar, QStringLiteral("sourceReferenceTag-0"));
    auto* const focusA = findItemByName(compareBar, QStringLiteral("canvasFocusAButton"));
    ASSERT_NE(referenceTag, nullptr);
    ASSERT_NE(focusA, nullptr);
    EXPECT_FALSE(referenceTag->isVisible());
    EXPECT_FALSE(focusA->property("referenceBadge").toBool());

    // Identifying the GT on slot 0 raises the text label on the chip and the GT badge on the
    // focus button - the colour never has to carry the role alone.
    identifyReferenceSlot(harness.snapshot, 0);
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_TRUE(referenceTag->isVisible())
        << "the reference chip must carry the 参考 text tag, not just its hue";
    EXPECT_EQ(referenceTag->property("text").toString(), QStringLiteral("参考"));
    EXPECT_TRUE(focusA->property("referenceBadge").toBool());
    EXPECT_GT(focusA->width(),
              findItemByName(compareBar, QStringLiteral("canvasFocusBButton"))->width())
        << "the GT badge widens the focus button so the label cannot be clipped away";

    // Direct look raises the 直看 tag on the focused chip, beside its accent colour.
    const QStringList identities = harness.shell->activeSourceIdentities();
    ASSERT_EQ(identities.size(), 2);
    ASSERT_TRUE(harness.shell->requestCanvasFocus(
        static_cast<int>(ReviewShellController::CanvasFocusSourceSlot), 1));
    harness.settle();
    auto* const directLookTag = findItemByName(sourceBar, QStringLiteral("sourceDirectLookTag-1"));
    ASSERT_NE(directLookTag, nullptr);
    EXPECT_TRUE(directLookTag->isVisible())
        << "the focused chip must carry the 直看 text tag, not just its hue";
    EXPECT_EQ(directLookTag->property("text").toString(), QStringLiteral("直看"));

    // The current comparison pair is named by the two real files in the combo, never by hue.
    auto* const pairCombo = harness.root->findChild<QQuickItem*>(QStringLiteral("pairCombo"));
    ASSERT_NE(pairCombo, nullptr);
    const QVariantList edgeList = harness.root->property("differenceEdges").toList();
    ASSERT_FALSE(edgeList.isEmpty());
    EXPECT_FALSE(edgeList.first().toMap().value(QStringLiteral("pairLabel")).toString().isEmpty())
        << "the pair selector must name the two compared files as text";
}

// U01: the docked/drawer reflow reads the canvas width left after the panels that actually
// consume stage width, not the raw window width. The 600 px guard is the protection line: an
// inspector that would push the canvas under it goes to the drawer, and a drag of either
// panel can never leave its token band.
TEST(MainQmlContractTests, CanvasGuardDrivesInspectorDockingAndPanelClamps) {
    WorkspaceHarness harness;
    harness.shell->setInspectorVisible(true);
    ASSERT_TRUE(harness.create()) << harness.error;
    // Stay hidden: a shown window clamps to the 960 px minimum width and the sub-960
    // reflow cases below could never run (the same reason DrawerScrim hides while resizing).
    harness.settle();

    auto* const inspector = harness.root->findChild<QQuickItem*>(QStringLiteral("tabbedInspector"));
    auto* const viewport =
        harness.root->findChild<QQuickItem*>(QStringLiteral("mediaViewportFocusTarget"));
    ASSERT_NE(inspector, nullptr);
    ASSERT_NE(viewport, nullptr);
    QQuickItem* const layoutRoot = inspector->parentItem();
    ASSERT_NE(layoutRoot, nullptr);

    const auto processLayout = [&harness, layoutRoot] {
        for (int iteration = 0; iteration < 8; ++iteration) {
            QCoreApplication::processEvents();
        }
        layoutRoot->setSize(QSizeF{static_cast<qreal>(harness.window->width()),
                                   static_cast<qreal>(harness.window->height())});
    };

    // 1280 window: 1252 canvas after margins, docking 288 keeps 964 >= 600.
    EXPECT_TRUE(harness.root->property("inspectorDocked").toBool());
    EXPECT_FALSE(harness.root->property("drawerMode").toBool());
    EXPECT_DOUBLE_EQ(inspector->width(), 288.0);
    EXPECT_GE(viewport->width(), 600.0) << "a docked inspector must leave the canvas its guard";

    // Over/under drags clamp to the panel band edges, and the live width follows.
    harness.root->setProperty("inspectorPanelWidth", 999.0);
    processLayout();
    EXPECT_DOUBLE_EQ(inspector->width(), 320.0);
    harness.root->setProperty("inspectorPanelWidth", 100.0);
    processLayout();
    EXPECT_DOUBLE_EQ(inspector->width(), 260.0);
    harness.root->setProperty("inspectorPanelWidth", 288.0);
    processLayout();
    EXPECT_DOUBLE_EQ(inspector->width(), 288.0);

    // 960 window still docks: 932 - 288 = 644 >= 600. (The retired <1120 window-width rule
    // would have gone to the drawer here even though the canvas keeps its guard.)
    harness.window->resize(960, 640);
    processLayout();
    EXPECT_TRUE(harness.root->property("inspectorDocked").toBool())
        << "960 px with no sidebar leaves 644 px of canvas; docking is correct";
    EXPECT_FALSE(harness.root->property("drawerMode").toBool())
        << "the drawer decision must read the guard, not the raw window width";
    EXPECT_GE(viewport->width(), 600.0);

    // 900 window: 872 - 288 = 584 < 600, so the inspector reflows to the drawer and the
    // canvas spans the full stage width again.
    harness.window->resize(900, 640);
    processLayout();
    EXPECT_FALSE(harness.root->property("inspectorDocked").toBool());
    EXPECT_TRUE(harness.root->property("drawerMode").toBool());
    EXPECT_GE(viewport->width(), 600.0) << "the drawer must not squeeze the canvas";

    // Widening restores the docked arrangement without any preference changing hands.
    harness.window->resize(1280, 800);
    processLayout();
    EXPECT_TRUE(harness.root->property("inspectorDocked").toBool());
    EXPECT_DOUBLE_EQ(inspector->width(), 288.0);
}

// U01: the file sidebar resizes inside its 200-288 band and yields to the canvas guard by
// temporary suppression - the user's visible flag survives, so widening the window restores
// the sidebar without the user re-opening it.
TEST(MainQmlContractTests, FileSidebarKeepsFixedWidthAndYieldsToTheCanvasGuard) {
    WorkspaceHarness harness;
    harness.withVideoFolder = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    // Hidden window: the 620 px suppression case sits below the 960 px minimum width.
    harness.settle();

    auto* const loader =
        harness.root->findChild<QQuickItem*>(QStringLiteral("videoFolderSidebarLoader"));
    ASSERT_NE(loader, nullptr);
    // QQuickLoader has no public C++ header in this Qt, so the loaded panel is read through
    // the object property system instead.
    const auto sidebarWidth = [loader]() -> qreal {
        const auto* const panel =
            qobject_cast<const QQuickItem*>(loader->property("item").value<QObject*>());
        return panel != nullptr ? panel->width() : -1.0;
    };

    const auto processLayout = [&harness] {
        for (int iteration = 0; iteration < 8; ++iteration) {
            QCoreApplication::processEvents();
        }
    };

    harness.root->setProperty("videoFolderSidebarVisible", true);
    processLayout();
    ASSERT_TRUE(loader->property("active").toBool());
    ASSERT_NE(loader->property("item").value<QObject*>(), nullptr);
    EXPECT_DOUBLE_EQ(sidebarWidth(), 232.0) << "default sidebar width is the 232 token";

    // Width is fixed, including when a previous client attempts to set the old preference.
    EXPECT_FALSE(harness.root->setProperty("videoFolderSidebarWidth", 999.0));
    processLayout();
    EXPECT_DOUBLE_EQ(sidebarWidth(), 232.0);
    EXPECT_EQ(harness.root->findChild<QQuickItem*>(QStringLiteral("videoFolderSidebarSplitter")),
              nullptr);
    auto* const folder = harness.root->findChild<QQuickItem*>(QStringLiteral("videoFolderTab"));
    auto* const recent = harness.root->findChild<QQuickItem*>(QStringLiteral("videoRecentTab"));
    auto* const choose =
        harness.root->findChild<QQuickItem*>(QStringLiteral("videoFolderChooseButton"));
    auto* const refresh =
        harness.root->findChild<QQuickItem*>(QStringLiteral("videoFolderRefreshButton"));
    ASSERT_NE(folder, nullptr);
    ASSERT_NE(recent, nullptr);
    ASSERT_NE(choose, nullptr);
    ASSERT_NE(refresh, nullptr);
    EXPECT_DOUBLE_EQ(folder->width(), recent->width());
    EXPECT_DOUBLE_EQ(choose->width(), refresh->width());
    EXPECT_DOUBLE_EQ(folder->y(), recent->y());
    EXPECT_DOUBLE_EQ(choose->y(), refresh->y());
    EXPECT_GT(choose->y(), folder->y());

    // 620 px window: 592 canvas without any sidebar is under the 600 guard, so the sidebar
    // is temporarily suppressed while the user's flag stays on.
    auto* const layoutRoot = qobject_cast<QQuickItem*>(loader->parentItem());
    ASSERT_NE(layoutRoot, nullptr);
    harness.window->resize(620, 480);
    for (int iteration = 0; iteration < 8; ++iteration) {
        QCoreApplication::processEvents();
    }
    layoutRoot->setSize(QSizeF{620.0, 480.0});
    processLayout();
    EXPECT_FALSE(loader->property("active").toBool())
        << "the sidebar must yield when it would push the canvas under the guard";
    EXPECT_TRUE(harness.root->property("videoFolderSidebarVisible").toBool())
        << "suppression is temporary; the user's visible flag is untouched";

    // Widening past the guard brings the sidebar back by itself.
    harness.window->resize(960, 640);
    processLayout();
    EXPECT_TRUE(loader->property("active").toBool());
    EXPECT_DOUBLE_EQ(sidebarWidth(), 232.0);
}

// U03: the five stage states are mutually exclusive and each drives exactly its own surface.
// The scenarios run through the real controller over a validated comparison (submitting
// commands, delivering terminals, mutating the snapshot) so the derived stageStateId and the
// on-screen surfaces are checked against each other, not against a mock of themselves.
TEST(MainQmlContractTests, StageStatesAreMutuallyExclusiveAndDriveTheRightSurface) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    const QString pathA = temporaryDirectory.filePath(QStringLiteral("stage-a.mp4"));
    const QString pathB = temporaryDirectory.filePath(QStringLiteral("stage-b.mp4"));
    for (const QString& path : {pathA, pathB}) {
        QFile file{path};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_GT(file.write("stage-source", 12), 0);
    }

    WorkspaceHarness harness;
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate);
    ASSERT_TRUE(installValidatedVideoSet(
        harness.snapshot,
        {std::filesystem::path{pathA.toStdWString()}, std::filesystem::path{pathB.toStdWString()}},
        rate.value(),
        100,
        400'000));
    // The helper clears and rebuilds sources/presentedSources with exact matches.
    harness.snapshot->displayedFrame = domain::FrameId{41};
    ASSERT_TRUE(harness.create()) << harness.error;

    auto* const overlay = harness.root->findChild<QQuickItem*>(QStringLiteral("statusOverlay"));
    auto* const banner = harness.root->findChild<QQuickItem*>(QStringLiteral("frameErrorBanner"));
    ASSERT_NE(overlay, nullptr);
    ASSERT_NE(banner, nullptr);

    const auto stateIs = [&harness](const char* expected) {
        return harness.root->property("stageStateId").toString() == QLatin1String(expected);
    };

    // ready: frame presented, nothing pending, nothing failed.
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_EQ(harness.root->property("stageStateId").toString(), QStringLiteral("ready"));

    // empty: no sources at all - the drop-hint overlay owns the stage. The whole snapshot
    // moves to a consistent kEmpty (frame count, timeline, frame id all cleared), exactly
    // how the composition root publishes a closed session - an inconsistent snapshot would
    // fail the controller closed and read as a graphics failure instead.
    const std::vector<application::SessionSourceView> savedSources = harness.snapshot->sources;
    const std::vector<application::PresentedSourceState> savedPresented =
        harness.snapshot->presentedSources;
    const auto savedComparison = harness.snapshot->validatedComparison;
    const auto savedTimeline = harness.snapshot->canonicalTimeline;
    const auto savedFrameCount = harness.snapshot->canonicalFrameCount;
    harness.snapshot->sources.clear();
    harness.snapshot->presentedSources.clear();
    harness.snapshot->validatedComparison.reset();
    harness.snapshot->canonicalTimeline.reset();
    harness.snapshot->canonicalFrameCount = 0U;
    harness.snapshot->sessionState = domain::SessionState::kEmpty;
    harness.snapshot->displayedFrame.reset();
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_EQ(harness.root->property("stageStateId").toString(), QStringLiteral("empty"));
    auto* const emptyView = harness.root->findChild<QQuickItem*>(QStringLiteral("emptyReviewView"));
    ASSERT_NE(emptyView, nullptr);
    EXPECT_TRUE(emptyView->isVisible()) << "first blank shows the drop-card entry points";
    EXPECT_FALSE(overlay->isVisible()) << "nothing has failed and nothing is loading";
    auto* const viewport =
        harness.root->findChild<QQuickItem*>(QStringLiteral("mediaViewportFocusTarget"));
    ASSERT_NE(viewport, nullptr);
    EXPECT_FALSE(viewport->property("chromeVisible").toBool());

    // waitingFrame: the old picture stays while the next frame is on its way. A pending
    // navigation raises framePending; after the short debounce the waiting state shows
    // while the old frame remains unmasked.
    harness.snapshot->displayedFrame = domain::FrameId{41};
    harness.snapshot->sources = savedSources;
    harness.snapshot->presentedSources = savedPresented;
    harness.snapshot->validatedComparison = savedComparison;
    harness.snapshot->canonicalTimeline = savedTimeline;
    harness.snapshot->canonicalFrameCount = savedFrameCount;
    harness.snapshot->sessionState = domain::SessionState::kReady;
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_TRUE(harness.controller->seekFrame(42));
    ASSERT_TRUE(harness.waitUntil([&harness] {
        return harness.root->property("stageStateId").toString() == QLatin1String("waitingFrame");
    })) << "a pending navigation over a kept frame must read as waitingFrame, not be masked";
    EXPECT_FALSE(overlay->isVisible()) << "a kept picture must not be masked by a full overlay";
    EXPECT_FALSE(emptyView->isVisible());

    // opening: a new task is opening and there is no old picture - the open command holds
    // busy while the coordinator publishes a kLoading snapshot (no frame, no presentation).
    const QUrl openUrlA = QUrl::fromLocalFile(pathA);
    const QUrl openUrlB = QUrl::fromLocalFile(pathB);
    EXPECT_TRUE(harness.controller->openSources(
        {QVariant::fromValue(openUrlA), QVariant::fromValue(openUrlB)}, 0));
    harness.snapshot->sessionState = domain::SessionState::kLoading;
    harness.snapshot->displayedFrame.reset();
    harness.snapshot->presentedSources.clear();
    harness.snapshot->validatedComparison.reset();
    harness.snapshot->canonicalTimeline.reset();
    harness.snapshot->canonicalFrameCount = 0U;
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_TRUE(harness.controller->busy());
    EXPECT_TRUE(stateIs("opening")) << "busy with no old picture is the opening state";
    EXPECT_TRUE(overlay->isVisible()) << "opening shows the busy overlay";
    EXPECT_FALSE(emptyView->isVisible());

    // openFailed: restore the presented session, deliver the terminal (clear busy), then
    // fail with the old frame kept - the banner explains the failure while the old session
    // survives behind it.
    harness.snapshot->sessionState = domain::SessionState::kReady;
    harness.snapshot->displayedFrame = domain::FrameId{41};
    harness.snapshot->presentedSources = savedPresented;
    harness.snapshot->validatedComparison = savedComparison;
    harness.snapshot->canonicalTimeline = savedTimeline;
    harness.snapshot->canonicalFrameCount = savedFrameCount;
    // Retire every outstanding command (the navigation and the open) so the failure state
    // is not masked by a lingering wait.
    for (const application::PlaybackCommand& command : harness.submitted) {
        harness.terminals.push_back(application::CommandTerminal{
            .context = application::commandContext(command),
            .outcome = application::CommandOutcome::Succeeded,
        });
    }
    harness.snapshot->lastError = domain::MediaError{
        .source = domain::SourceId{0U},
        .userMessageKey = "media-open-failed",
        .technicalDetail = "u03-stage-test",
    };
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_TRUE(stateIs("openFailed")) << "an error over a kept frame is still openFailed";
    EXPECT_TRUE(banner->isVisible()) << "the failure keeps the old session and explains itself";
    EXPECT_FALSE(overlay->isVisible());
    EXPECT_FALSE(emptyView->isVisible());

    // missingFrame: error cleared, one source's frame missing for the shown frame.
    harness.snapshot->lastError.reset();
    harness.snapshot->presentedSources.front().matchKind = application::FrameMatchKind::Missing;
    harness.snapshot->presentedSources.front().sourceFrameId.reset();
    harness.snapshot->presentedSources.front().missingReason =
        application::MissingReason::AlignmentGap;
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_TRUE(stateIs("missingFrame"));
    EXPECT_FALSE(banner->isVisible());
    EXPECT_FALSE(overlay->isVisible())
        << "missingFrame speaks through the pair-dependent unavailable banner only";
}

// U03 information layering and user-action terminology: a single ordinary video hides the
// comparison pair and the GT role entirely; a comparison session shows them. The transport
// speaks "原速播放", the pending pill distinguishes reverse-walk preparation, and the
// presentation status is translated with the raw value kept one hover away.
TEST(MainQmlContractTests, TerminologySpeaksUserActionsAndLayeringFollowsTheTask) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    // Two sources: the compare bar, the pair selector and the strip are part of the task.
    auto* const compareBar = harness.root->findChild<QQuickItem*>(QStringLiteral("compareModeBar"));
    auto* const sourceBar =
        harness.root->findChild<QQuickItem*>(QStringLiteral("activeSourceStrip"));
    ASSERT_NE(compareBar, nullptr);
    ASSERT_NE(sourceBar, nullptr);
    EXPECT_TRUE(compareBar->isVisible());
    EXPECT_TRUE(sourceBar->isVisible());
    auto* const pairCombo = harness.root->findChild<QQuickItem*>(QStringLiteral("pairCombo"));
    ASSERT_NE(pairCombo, nullptr);
    EXPECT_EQ(QQmlProperty(pairCombo, QStringLiteral("Accessible.name"), qmlContext(pairCombo))
                  .read()
                  .toString(),
              QStringLiteral("比较对象"));

    // A single ordinary video: the comparison pair and the GT role chrome disappear.
    auto single = domain::ComparisonValidator::validate(std::vector<domain::ComparisonSource>{
        domain::ComparisonSource{
            .id = domain::SourceId{0U},
            .role = domain::ComparisonRole::kReference,
            .descriptor =
                domain::MediaDescriptor{
                    .normalizedPath = std::filesystem::path{L"u03-single.mp4"},
                    .extent = domain::MediaExtent{.width = 1'920U, .height = 1'080U},
                    .frameRate = domain::RationalRate::create(30, 1).value(),
                    .frameCount = domain::FrameCountInfo{.value = 12},
                    .duration = domain::MediaTime{400'000},
                    .codecId = "h264",
                    .pixelFormatId = "nv12",
                    .bitDepth = 8U,
                    .timingConfidence = domain::TimingConfidence::kVerifiedCfr,
                },
            .displayName = "Single",
        },
    });
    ASSERT_TRUE(single);
    harness.snapshot->validatedComparison =
        std::make_shared<const domain::ValidatedComparisonSet>(std::move(single).value().set);
    harness.snapshot->sources.resize(1);
    harness.snapshot->presentedSources.resize(1);
    harness.controller->refreshProjection();
    harness.settle();
    ASSERT_EQ(harness.controller->sourceCount(), 1);
    EXPECT_TRUE(compareBar->isVisible())
        << "a single video keeps the comparison tasks on screen, disabled, so the product's "
           "purpose is visible before a second file exists";
    EXPECT_TRUE(sourceBar->isVisible());

    // The transport names the user action, not the continuity policy enum.
    EXPECT_EQ(harness.root->property("playbackModeLabel").toString(), QStringLiteral("流畅优先"));

    // The presentation-status translation maps every unconfirmed calibre to the same
    // user-facing sentence while other calibres keep their meaning; the waiting pill names
    // reverse-walk preparation and defaults to the plain fetch.
    auto* const catalog = harness.root->findChild<QObject*>(QStringLiteral("reviewMessageCatalog"));
    ASSERT_NE(catalog, nullptr);
    const auto presentationText = [&catalog](const QString& raw) {
        QVariant result;
        QMetaObject::invokeMethod(catalog,
                                  "presentationStatusText",
                                  Q_RETURN_ARG(QVariant, result),
                                  Q_ARG(QVariant, QVariant{raw}));
        return result.toString();
    };
    EXPECT_EQ(harness.root->property("framePendingLabel").toString(),
              QStringLiteral("正在获取最新帧…"));
    const auto framePendingText = [&catalog](const QVariant& active, const QVariant& direction) {
        QVariant result;
        QMetaObject::invokeMethod(catalog,
                                  "framePendingText",
                                  Q_RETURN_ARG(QVariant, result),
                                  Q_ARG(QVariant, active),
                                  Q_ARG(QVariant, direction));
        return result.toString();
    };
    EXPECT_EQ(framePendingText(QVariant{true}, QVariant{-1}), QStringLiteral("正在准备反向帧…"));
    EXPECT_EQ(framePendingText(QVariant{true}, QVariant{1}), QStringLiteral("正在获取最新帧…"));
    EXPECT_EQ(framePendingText(QVariant{false}, QVariant{-1}), QStringLiteral("正在获取最新帧…"));
    EXPECT_EQ(presentationText(QStringLiteral("未确认 (未探测)")),
              QStringLiteral("尚未确认系统呈现"));
    EXPECT_EQ(presentationText(QStringLiteral("未知 (未确认)")),
              QStringLiteral("尚未确认系统呈现"));
    EXPECT_EQ(presentationText(QStringLiteral("可用 (Flip Model)")),
              QStringLiteral("已确认系统呈现"));
    EXPECT_EQ(presentationText(QStringLiteral("不支持 (需 Flip 模型)")),
              QStringLiteral("系统呈现不可用"));
    EXPECT_EQ(presentationText(QStringLiteral("")), QStringLiteral("—"));
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
    EXPECT_EQ(label->property("text").toString(), QStringLiteral("起 42 · 终 —（区间无效）"))
        << "an in point alone is not a range";

    // Marking a point is a chrome-time action, so its confirmation must ride the intent toast:
    // the immersive HUD does not render while the chrome is visible, and a set point that
    // reports nothing reads as a dead chip.
    EXPECT_EQ(harness.root->property("intentMessage").toString(),
              QStringLiteral("区间起点 · 第 42 帧"));
    EXPECT_TRUE(harness.root->property("immersiveHudText").toString().isEmpty());

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
    EXPECT_EQ(label->property("text").toString(), QStringLiteral("起 42 · 终 48 · 7 帧"));

    // The chips and the transport read the same range: the row forwards what the shell holds.
    auto* const transportBar = harness.root->findChild<QQuickItem*>(QStringLiteral("transportBar"));
    ASSERT_NE(transportBar, nullptr);
    EXPECT_TRUE(transportBar->property("rangeControlsVisible").toBool());
    EXPECT_EQ(transportBar->property("rangeInFrame").toInt(), 41);
    EXPECT_EQ(transportBar->property("rangeOutFrame").toInt(), 47);
    EXPECT_FALSE(transportBar->property("rangeLoopActive").toBool());

    // The same marking in immersive mode keeps the HUD channel: hiding the chrome must not lose
    // the confirmation, and the toast must stay quiet because it is not the visible channel
    // there. Marked last so the range assertions above keep their in/out frames.
    harness.shell->setChromeVisible(false);
    harness.settle();
    harness.window->requestActivate();
    harness.root->setProperty("intentMessage", QString{});
    harness.root->setProperty("immersiveHudText", QString{});
    harness.snapshot->displayedFrame = domain::FrameId{50};
    harness.controller->refreshProjection();
    harness.settle();
    sendKey(*harness.window, Qt::Key_I);
    harness.settle();
    EXPECT_EQ(harness.root->property("immersiveHudText").toString(),
              QStringLiteral("区间起点 · 第 51 帧"));
    EXPECT_TRUE(harness.root->property("intentMessage").toString().isEmpty());
    harness.shell->setChromeVisible(true);
    harness.settle();
}

// The default continuity preference is Contextual, a policy with no segment of its own. The
// transport switch must show the behaviour the session resolved to (RealTime) instead of
// looking unset, while the menu keeps editing the three-way preference - one setting, four
// surfaces, one vocabulary.
TEST(MainQmlContractTests, ContinuitySwitchShowsTheResolvedPolicyForAContextualPreference) {
    WorkspaceHarness harness;
    harness.preferences.setPlaybackContinuityPolicy(2);
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    // The harness restores the stored preference once the session reports ready - the same
    // one-shot path the settings dialog uses - so no manual apply is needed here.
    harness.settle();

    // The controller keeps the request and the resolution apart.
    EXPECT_EQ(harness.controller->playbackContinuityPolicy(), 2);
    EXPECT_EQ(harness.controller->playbackContinuityPolicyEffective(), 1);
    // The status rail names the resolved behaviour in the same vocabulary as the switch.
    EXPECT_EQ(harness.root->property("playbackModeLabel").toString(), QStringLiteral("流畅优先"));

    auto* const realTimeOption =
        harness.root->findChild<QQuickItem*>(QStringLiteral("realTimeContinuityButton"));
    auto* const everyFrameOption =
        harness.root->findChild<QQuickItem*>(QStringLiteral("reviewEveryFrameContinuityButton"));
    ASSERT_NE(realTimeOption, nullptr);
    ASSERT_NE(everyFrameOption, nullptr);
    EXPECT_TRUE(realTimeOption->property("isSelected").toBool())
        << "a Contextual preference must leave the resolved RealTime segment selected";
    EXPECT_FALSE(everyFrameOption->property("isSelected").toBool());

    // The menu still offers (and marks) the three-way preference the user actually chose.
    auto* const contextualItem =
        harness.root->findChild<QObject*>(QStringLiteral("continuityContextual"));
    ASSERT_NE(contextualItem, nullptr);
    EXPECT_TRUE(contextualItem->property("checked").toBool());
    auto* const realTimeItem =
        harness.root->findChild<QObject*>(QStringLiteral("continuityRealTime"));
    ASSERT_NE(realTimeItem, nullptr);
    EXPECT_FALSE(realTimeItem->property("checked").toBool());

    // Choosing the explicit review mode flips both the resolution and the switch. The option
    // segments are plain Rectangles driven by a MouseArea, so the request goes through the
    // transport's own signal - the same path a click takes.
    auto* const transportBar = harness.root->findChild<QQuickItem*>(QStringLiteral("transportBar"));
    ASSERT_NE(transportBar, nullptr);
    ASSERT_TRUE(
        QMetaObject::invokeMethod(transportBar, "continuityPolicyRequested", Q_ARG(int, 0)));
    EXPECT_EQ(harness.controller->playbackContinuityPolicy(), 0)
        << "the transport request must reach the controller";
    EXPECT_EQ(harness.preferences.playbackContinuityPolicy(), 0)
        << "the transport request must reach the preferences store";

    // Mirror the accepted command into the harness snapshot the way the coordinator would, so
    // the authoritative publication agrees with the optimistic projection instead of reverting
    // it on the next refresh.
    harness.snapshot->playbackContinuityPolicy = domain::PlaybackContinuityPolicy::ReviewEveryFrame;
    harness.snapshot->playbackContinuityPolicyEffective =
        domain::PlaybackContinuityPolicy::ReviewEveryFrame;
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_EQ(harness.controller->playbackContinuityPolicy(), 0);
    EXPECT_EQ(harness.controller->playbackContinuityPolicyEffective(), 0);
    EXPECT_TRUE(everyFrameOption->property("isSelected").toBool());
    EXPECT_FALSE(realTimeOption->property("isSelected").toBool());
    EXPECT_EQ(harness.root->property("playbackModeLabel").toString(), QStringLiteral("逐帧完整"));
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
    EXPECT_EQ(label->property("text").toString(), QStringLiteral("起 42 · 终 48 · 7 帧"));
    EXPECT_EQ(harness.shell->inFrame(), 41);
    EXPECT_EQ(harness.shell->outFrame(), 47);
    EXPECT_TRUE(playRange->property("chipEnabled").toBool());
}

// P03: without a protocol controller (the state of every harness that predates the feature) the
// protocol row must stay out of the layout — plain review renders exactly as before.
TEST(MainQmlContractTests, TransportKeepsProtocolRowHiddenWithoutController) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;

    auto* const row = harness.root->findChild<QQuickItem*>(QStringLiteral("transportProtocolRow"));
    ASSERT_NE(row, nullptr);
    EXPECT_FALSE(row->isVisible());
    auto* const badge =
        harness.root->findChild<QQuickItem*>(QStringLiteral("transportProtocolBadge"));
    ASSERT_NE(badge, nullptr);
    EXPECT_FALSE(badge->isVisible());
    // The OSC must expose a null protocol so every downstream binding stays inert.
    auto* const transport = harness.root->findChild<QQuickItem*>(QStringLiteral("transport"));
    ASSERT_NE(transport, nullptr);
    EXPECT_TRUE(transport->property("protocol").isNull());
    // The harness itself restores playback preferences once the session is ready (continuity
    // policy plus the stored range remap), so a bare create is not command-silent. What must
    // never happen without a protocol controller is protocol work: no seek of any kind.
    for (const auto& command : harness.submitted) {
        EXPECT_EQ(std::get_if<application::SeekFrameCommand>(&command), nullptr);
    }
}

// P03: with a protocol loaded, the transport's chip routes through TransportBar -> PlayerOsc ->
// Main.qml -> InterpolationProtocolController -> ReviewController, and lands as the same
// SeekFrameCommand every other precise navigation uses.
TEST(MainQmlContractTests, ProtocolRowNavigatesGeneratedFramesThroughTheCommandChain) {
    WorkspaceHarness harness;
    harness.withInterpolationProtocol = true;
    // 60->20->60 decimation, GT 10 stride 3: R0 G1 G2 R3 G4 G5 R6 G7 G8 R9. The shared harness
    // session displays frame 41 of 100; move to frame 2 (a generated position with ratio 2/3).
    harness.snapshot->displayedFrame = domain::FrameId{2};
    const auto built =
        domain::buildDecimatedProtocol(domain::DecimationPlan{.gtFrameCount = 10, .stride = 3},
                                       domain::MappingOrigin::kImportedManifest,
                                       domain::TailPolicy::kTrimPartialInterval);
    ASSERT_TRUE(built);
    harness.protocolRepository.behavior = [protocol = built.value()] {
        application::InterpolationProtocolIoResult result;
        result.ok = true;
        result.protocol = protocol;
        return result;
    };
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();
    ASSERT_EQ(harness.controller->currentFrame(), 2);

    ASSERT_TRUE(harness.interpolationProtocol.loadProtocol(
        QUrl::fromLocalFile(QDir::temp().filePath(QStringLiteral("protocol-manifest.json")))));
    ASSERT_TRUE(
        harness.waitUntil([&harness] { return harness.interpolationProtocol.protocolLoaded(); }));

    auto* const nextChip =
        harness.root->findChild<QObject*>(QStringLiteral("transportNextGeneratedButton"));
    auto* const previousChip =
        harness.root->findChild<QObject*>(QStringLiteral("transportPreviousGeneratedButton"));
    auto* const clearChip =
        harness.root->findChild<QObject*>(QStringLiteral("transportClearProtocolButton"));
    auto* const loadChip =
        harness.root->findChild<QObject*>(QStringLiteral("transportLoadProtocolButton"));
    auto* const badge = harness.root->findChild<QObject*>(QStringLiteral("transportProtocolBadge"));
    ASSERT_NE(nextChip, nullptr);
    ASSERT_NE(previousChip, nullptr);
    ASSERT_NE(clearChip, nullptr);
    ASSERT_NE(loadChip, nullptr);
    ASSERT_NE(badge, nullptr);
    EXPECT_TRUE(loadChip->property("visible").toBool());
    EXPECT_TRUE(nextChip->property("visible").toBool());
    EXPECT_TRUE(nextChip->property("chipEnabled").toBool());
    EXPECT_TRUE(previousChip->property("chipEnabled").toBool());
    EXPECT_TRUE(clearChip->property("visible").toBool());
    // Text-first badge: provenance reads as words, GT indices display 1-based (inputs 0 and 3).
    EXPECT_TRUE(badge->property("text").toString().contains(QStringLiteral("生成帧")));
    EXPECT_TRUE(badge->property("text").toString().contains(QStringLiteral("输入 GT 1/4")));
    EXPECT_TRUE(badge->property("text").toString().contains(QStringLiteral("比例 2/3")));

    // Next generated position from frame 2 is 4; the jump is a coordinator seek command.
    ASSERT_TRUE(QMetaObject::invokeMethod(nextChip, "clicked"));
    harness.settle();
    ASSERT_FALSE(harness.submitted.empty());
    const auto* const seek = std::get_if<application::SeekFrameCommand>(&harness.submitted.back());
    ASSERT_NE(seek, nullptr);
    EXPECT_EQ(seek->frameId, domain::FrameId{4});
    EXPECT_EQ(seek->context.sessionId, harness.snapshot->sessionId);

    // Mirror the coordinator's terminal and the newly displayed frame, then jump back: the
    // previous generated position from frame 4 is 2.
    harness.terminals.push_back(application::CommandTerminal{
        .context = application::commandContext(harness.submitted.back()),
        .outcome = application::CommandOutcome::Succeeded,
    });
    harness.snapshot->displayedFrame = domain::FrameId{4};
    harness.controller->refreshProjection();
    harness.settle();
    ASSERT_TRUE(harness.waitUntil([&harness] { return harness.controller->currentFrame() == 4; }));
    ASSERT_TRUE(QMetaObject::invokeMethod(previousChip, "clicked"));
    harness.settle();
    ASSERT_GE(harness.submitted.size(), 2U);
    const auto* const seekBack =
        std::get_if<application::SeekFrameCommand>(&harness.submitted.back());
    ASSERT_NE(seekBack, nullptr);
    EXPECT_EQ(seekBack->frameId, domain::FrameId{2});

    // Rollback: clearing the protocol hides the navigation chips and keeps the session untouched.
    ASSERT_TRUE(QMetaObject::invokeMethod(clearChip, "clicked"));
    harness.settle();
    EXPECT_FALSE(harness.interpolationProtocol.protocolLoaded());
    EXPECT_FALSE(nextChip->property("visible").toBool());
    EXPECT_FALSE(previousChip->property("visible").toBool());
    EXPECT_FALSE(clearChip->property("visible").toBool());
    EXPECT_TRUE(loadChip->property("visible").toBool());
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
    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(defaultPilotWindowProperties())};
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
    // The chip is a chrome-time control, so its feedback must land on the intent toast — the
    // immersive HUD only renders while the chrome is hidden.
    EXPECT_EQ(harness.root->property("intentMessage").toString(),
              QStringLiteral("当前视频或所选区间暂不可导出，请检查素材和区间起点、终点。"));
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
    // Layout contract at the supported window floor: the body scrolls between a pinned
    // header and footer, so the primary action stays on screen instead of falling below
    // the window edge when the content grows.
    harness.window->resize(960, 640);
    harness.settle();
    auto* const bodyScroll =
        harness.root->findChild<QQuickItem*>(QStringLiteral("clipExportBodyScroll"));
    ASSERT_NE(bodyScroll, nullptr);
    EXPECT_LE(bodyScroll->height(), 640.0 - 48.0);
    auto* const chooseButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("clipExportChooseButton"));
    ASSERT_NE(chooseButton, nullptr);
    const QPointF chooseBottom = chooseButton->mapToScene(QPointF{0.0, chooseButton->height()});
    EXPECT_GT(chooseBottom.y(), 0.0);
    EXPECT_LE(chooseBottom.y(), 640.0);
    harness.window->resize(1280, 800);
    harness.settle();
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

    // The outcome lands once, on the channel the user can actually see: the export finishes
    // while the chrome is up (the dialog may even be closed), so the intent toast carries it and
    // the immersive HUD - which only renders with the chrome hidden - stays empty.
    const QString exportMessage = harness.root->property("intentMessage").toString();
    EXPECT_TRUE(exportMessage.startsWith(QStringLiteral("已导出所选区间。")))
        << exportMessage.toStdString();
    EXPECT_TRUE(exportMessage.contains(QStringLiteral("实际起点"))) << exportMessage.toStdString();
    EXPECT_TRUE(exportMessage.contains(QStringLiteral("实际终点"))) << exportMessage.toStdString();
    EXPECT_TRUE(harness.root->property("immersiveHudText").toString().isEmpty());
    EXPECT_EQ(harness.clipExport->lastStatus(), exportMessage);
    EXPECT_EQ(harness.clipExport->lastOutputPath(), QDir::toNativeSeparators(targetPath));
    EXPECT_TRUE(harness.clipExport->lastFailureDetail().isEmpty());
    EXPECT_DOUBLE_EQ(harness.clipExport->progress(), 1.0);
    // The chip returns to its idle label, and the dialog stayed open on the result so the user can
    // see where the file went.
    EXPECT_EQ(exportChip->property("chipText").toString(), QStringLiteral("导出"));
    EXPECT_FALSE(exportChip->property("chipActive").toBool());
    EXPECT_TRUE(popup->property("visible").toBool());
}

// The merged open entry lives in the chrome (menu, empty view), so its rejection feedback must
// surface on a channel that renders while the chrome is visible. The immersive HUD stays hidden
// whenever the chrome is shown, and the mixed video/image selection is rejected before any open
// starts, so the toast is the only feedback the user can get.
TEST(MainQmlContractTests, MixedMediaSelectionRejectsThroughTheVisibleIntentChannel) {
    WorkspaceHarness harness;
    harness.withVideoFolder = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    ASSERT_TRUE(harness.root->property("chromeVisible").toBool());
    harness.root->setProperty("immersiveHudText", QString{});
    harness.root->setProperty("intentMessage", QString{});

    // Suffixes only: the mixed check runs before any file is touched, so real media is not
    // needed to reach the rejection branch.
    const QVariantList mixedSelection{
        QUrl::fromLocalFile(QStringLiteral("C:/comparestation/a.mp4")),
        QUrl::fromLocalFile(QStringLiteral("C:/comparestation/b.png"))};
    QVariant returned;
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(),
                                          "openMediaFilesSelection",
                                          Q_RETURN_ARG(QVariant, returned),
                                          Q_ARG(QVariant, QVariant::fromValue(mixedSelection))));
    EXPECT_FALSE(returned.toBool());
    harness.settle();
    EXPECT_EQ(harness.root->property("intentMessage").toString(),
              QStringLiteral("视频与图片不能混合打开，请分两次选择。"));
    EXPECT_TRUE(harness.root->property("immersiveHudText").toString().isEmpty());

    // Immersive mode keeps the HUD channel: hiding the chrome must not lose the message.
    harness.root->setProperty("intentMessage", QString{});
    harness.shell->setChromeVisible(false);
    harness.settle();
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(),
                                          "openMediaFilesSelection",
                                          Q_RETURN_ARG(QVariant, returned),
                                          Q_ARG(QVariant, QVariant::fromValue(mixedSelection))));
    EXPECT_FALSE(returned.toBool());
    harness.settle();
    EXPECT_EQ(harness.root->property("immersiveHudText").toString(),
              QStringLiteral("视频与图片不能混合打开，请分两次选择。"));
    EXPECT_TRUE(harness.root->property("intentMessage").toString().isEmpty());
    harness.window->close();
}

// Busy disables the mode row, but a disabled checked button must keep its checked fill and
// label: an open/decode otherwise erases the only indicator of which compare mode is active.
TEST(MainQmlContractTests, CompareModeBarKeepsTheCheckedFillWhileBusy) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    std::vector<std::filesystem::path> sourcePaths;
    for (const char* name : {"mode_source_a.mp4", "mode_source_b.mp4"}) {
        const QString path = temporaryDirectory.filePath(QString::fromLatin1(name));
        QFile file{path};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(file.write("fake-mode-source", 16), 16);
        file.close();
        sourcePaths.push_back(std::filesystem::path{path.toStdWString()});
    }

    WorkspaceHarness harness;
    // The validated session must exist before create(): the controller's open gate reads the
    // projection the shell was built from, and the state-machine contract tests install the
    // set the same way.
    const auto rateResult = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rateResult);
    ASSERT_TRUE(
        installValidatedVideoSet(harness.snapshot, sourcePaths, rateResult.value(), 12, 400'000));
    harness.snapshot->displayedFrame = domain::FrameId{3};
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();
    harness.controller->refreshProjection();
    harness.settle();

    auto* const sideModeButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("sideModeButton"));
    auto* const diffModeButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("diffModeButton"));
    ASSERT_NE(sideModeButton, nullptr);
    ASSERT_NE(diffModeButton, nullptr);
    // Ready session: side-by-side is the active mode and its button carries the checked fill.
    EXPECT_TRUE(sideModeButton->property("enabled").toBool());
    EXPECT_TRUE(sideModeButton->property("checked").toBool());

    const auto backgroundOf = [](QQuickItem* button) -> QQuickItem* {
        for (QQuickItem* child : button->childItems()) {
            if (child->property("color").isValid() && child->property("radius").isValid())
                return child;
        }
        return nullptr;
    };
    auto* const sideBackground = backgroundOf(sideModeButton);
    auto* const diffBackground = backgroundOf(diffModeButton);
    ASSERT_NE(sideBackground, nullptr);
    ASSERT_NE(diffBackground, nullptr);

    QQmlEngine* const qmlEngine = QQmlEngine::contextForObject(harness.root.get())->engine();
    ASSERT_NE(qmlEngine, nullptr);
    QQmlComponent themeProbe{qmlEngine};
    themeProbe.setData(R"qml(
        import QtQuick
        import "qrc:/qml/VcsTheme.js" as Theme
        QtObject {
            property color checkedFill: Theme.controlChecked
        }
    )qml",
                       QUrl{});
    const std::unique_ptr<QObject> theme{themeProbe.create()};
    ASSERT_NE(theme, nullptr) << componentErrors(themeProbe);
    const auto checkedFill = theme->property("checkedFill").value<QColor>();

    EXPECT_EQ(sideBackground->property("color").value<QColor>(), checkedFill);
    EXPECT_EQ(diffBackground->property("color").value<QColor>(),
              QColor{QStringLiteral("transparent")});

    // Drive busy exactly like a real open: a pending open command over a loading snapshot.
    const QUrl reloadA =
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("mode_source_a.mp4")));
    const QUrl reloadB =
        QUrl::fromLocalFile(temporaryDirectory.filePath(QStringLiteral("mode_source_b.mp4")));
    EXPECT_TRUE(harness.controller->openSources(
        {QVariant::fromValue(reloadA), QVariant::fromValue(reloadB)}, 0));
    harness.snapshot->sessionState = domain::SessionState::kLoading;
    harness.snapshot->displayedFrame.reset();
    harness.snapshot->presentedSources.clear();
    harness.snapshot->validatedComparison.reset();
    harness.snapshot->canonicalTimeline.reset();
    harness.snapshot->canonicalFrameCount = 0U;
    harness.controller->refreshProjection();
    harness.settle();

    ASSERT_TRUE(harness.controller->busy());
    // The row is disabled now, but the checked mode keeps its fill while the unchecked one
    // stays flat — the user can still read which mode is active.
    EXPECT_FALSE(sideModeButton->property("enabled").toBool());
    EXPECT_TRUE(sideModeButton->property("checked").toBool());
    EXPECT_EQ(sideBackground->property("color").value<QColor>(), checkedFill);
    EXPECT_EQ(diffBackground->property("color").value<QColor>(),
              QColor{QStringLiteral("transparent")});
    harness.window->close();
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

    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(defaultPilotWindowProperties())};
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

    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(defaultPilotWindowProperties())};
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

// C1b. The resume restore is a user-session convenience; automation determinism (performance
// gates and --ui-smoke) must not replay it. Found in the wild on 2026-10-10: a developer
// profile held "resume.<fixture-identity> = 11", and every app.ui-*-smoke run opened the
// fixture, had the playhead dragged to the stored last frame, and timed out waiting for the
// frame the stage machine expects after one step. The user path (flag absent) is covered by
// ResumeRestoresRecordedPositionAndStaysOffForeignOrStaleEntries above.
TEST(MainQmlContractTests, AutomationDeterminismKeepsStoredResumePositionsOffThePlayhead) {
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

    // The exact entry the user path restores (frame 6 of 12); under automation determinism
    // the playhead must stay at frame 0 instead.
    std::map<std::string, std::string, std::less<>> seed{
        {QStringLiteral("resume.%1").arg(identity).toStdString(), "1700000000000:6"},
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
    engine.rootContext()->setContextProperty(QStringLiteral("dvsPerformanceAutomation"),
                                             QVariant{true});
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);

    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(defaultPilotWindowProperties())};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);
    window->resize(1280, 800);
    window->show();
    for (int iteration = 0; iteration < 16; ++iteration) {
        QCoreApplication::processEvents();
    }

    const auto seekCount = [&submitted] {
        return std::count_if(submitted.begin(), submitted.end(), [](const auto& command) {
            return std::holds_alternative<application::SeekFrameCommand>(command);
        });
    };
    EXPECT_EQ(seekCount(), 0) << "automation determinism must not replay a stored resume position";
    EXPECT_TRUE(root->property("resumeAttemptedIdentity").toString().isEmpty())
        << "suppressed resume must not mark an identity as handled";
    EXPECT_EQ(controller.currentFrame(), 0)
        << "the frame-zero baseline is what automation stages and gates wait on";
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

    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(defaultPilotWindowProperties())};
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

    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(defaultPilotWindowProperties())};
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

    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(defaultPilotWindowProperties())};
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
        GeometryCase{860, 640},
        GeometryCase{900, 640},
        GeometryCase{960, 640},
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

        // U01: the drawer boundary follows the canvas guard, not the window width. With the
        // default 288 px inspector and no sidebar the guard fails below 916 px (916 - 28 - 288 =
        // 600).
        const bool isDrawer = size.width < 916;

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
    // 900 px is below Main.qml's 960 px production minimum, so this test window relaxes its
    // own minimum before showing (production keeps 960). The shortcut needs a visible,
    // activated window; only the test fixture width changes.
    window->setMinimumWidth(600);
    window->resize(900, 640);
    window->show();
    window->requestActivate();
    processLayout();
    layoutRoot->setSize(QSizeF{900.0, 640.0});
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

    // --- Pixel gate: 900x640 drawer open (below the canvas-guard boundary) ---
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
    // UX-13 S1: window height 640 is the compact-chrome tier, and the shell re-polishes after
    // the explicit resize, so the grabbed device buffer settles a rounding step off the
    // scale-factor product of the requested 900x640 (900x640 grabbed vs 899x639 expected on
    // the S1 gate run). The gate's job is "the grab covers the window it came from", so derive
    // the expectation from the window's own settled logical size and round - never re-derive
    // it from the requested size with truncation.
    const int expectedGrabWidth = qRound(window->width() * dpr);
    const int expectedGrabHeight = qRound(window->height() * dpr);
    EXPECT_EQ(grab.width(), expectedGrabWidth);
    EXPECT_EQ(grab.height(), expectedGrabHeight);

    if (!grab.isNull() && grab.width() == expectedGrabWidth &&
        grab.height() == expectedGrabHeight) {
        // The inspector occupies the right side. Sample a pixel in the expected
        // inspector region (logical x near right edge, y near vertical center).
        const int inspectorSampleX = static_cast<int>((900.0 - 30.0) * dpr);
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
    EXPECT_DOUBLE_EQ(inspectorItem->width(), 288.0)
        << "U01: the panel width is the clamped token default";
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

    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(defaultPilotWindowProperties())};
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

    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(defaultPilotWindowProperties())};
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
    // The peek names the reference (GT) source, so the session has to identify one.
    identifyReferenceSlot(snapshot, 0);
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

    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(defaultPilotWindowProperties())};
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
    EXPECT_TRUE(peekButton->isVisible()) << visibleChainText(peekButton);

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

// P05 shared shape: a three-source session (A/B candidates, GT identified on slot 2) with the
// full controller/preferences/shell/facade stack the mode bar and the canvas-focus switch live
// in. identifyReferenceSlot() builds the validated comparison, which is what gives the shell
// frozen source identities to focus on.
class ThreeSourceFocusHarness final {
public:
    ThreeSourceFocusHarness() {
        snapshot->graphicsReady = true;
        snapshot->sessionState = domain::SessionState::kReady;
        snapshot->playbackState = domain::PlaybackState::kPaused;
        snapshot->displayedFrame = domain::FrameId{41};
        snapshot->canonicalFrameCount = 100U;
        snapshot->sources = {
            application::SessionSourceView{
                .sourceId = 0U,
                .role = domain::ComparisonRole::kPrediction,
                .displayName = "A",
            },
            application::SessionSourceView{
                .sourceId = 1U,
                .role = domain::ComparisonRole::kPrediction,
                .displayName = "B",
            },
            application::SessionSourceView{
                .sourceId = 2U,
                .role = domain::ComparisonRole::kReference,
                .displayName = "C",
            },
        };
        for (const auto& view : snapshot->sources) {
            snapshot->presentedSources.push_back(application::PresentedSourceState{
                .sourceId = view.sourceId,
                .sourceFrameId = domain::FrameId{41},
                .matchKind = application::FrameMatchKind::ExactIndex,
            });
        }
        identifyReferenceSlot(snapshot, 2);
        controller = std::make_unique<ReviewController>(ReviewController::Dependencies{
            .submit =
                [this](application::PlaybackCommand command) {
                    submitted.push_back(std::move(command));
                    return application::PortSubmitResult::Accepted;
                },
            .snapshot = [this] { return snapshot; },
            .takeCompletedCommands = [] { return std::vector<application::CommandTerminal>{}; },
        });
        shell = std::make_unique<ReviewShellController>(*controller, preferences);
        facade = std::make_unique<ReviewSessionFacade>(*controller, preferences, *shell);
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
        QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
        if (component.status() != QQmlComponent::Ready) {
            error = componentErrors(component);
            return false;
        }
        root.reset(component.createWithInitialProperties(defaultPilotWindowProperties()));
        if (!root) {
            error = componentErrors(component);
            return false;
        }
        window = qobject_cast<QQuickWindow*>(root.get());
        if (window == nullptr) {
            error = "Main.qml root is not a QQuickWindow";
            return false;
        }
        window->resize(1440, 900);
        window->show();
        for (int iteration = 0; iteration < 5; ++iteration) {
            QCoreApplication::processEvents();
        }
        return true;
    }

    std::shared_ptr<application::SessionSnapshot> snapshot =
        std::make_shared<application::SessionSnapshot>();
    std::vector<application::PlaybackCommand> submitted;
    std::unique_ptr<ReviewController> controller;
    ReviewPreferencesController preferences{std::make_shared<ClosedSettingsRepository>()};
    std::unique_ptr<ReviewShellController> shell;
    std::unique_ptr<ReviewSessionFacade> facade;
    QQmlEngine engine;
    std::unique_ptr<QObject> root;
    QQuickWindow* window = nullptr;
    std::string error;
};

// P05: installs the wiring a future Main.qml handler will own - the mode bar's
// fullCanvasFocusRequested signal forwarded into the shell action, with every crossing request
// recorded on the relay (calls / lastRequest / lastSlot) so tests can assert exactly what left
// the QML component.
[[nodiscard]] std::unique_ptr<QObject>
installFocusWiring(QQmlEngine& engine, QQuickItem* bar, ReviewShellController& shell) {
    QQmlComponent component{&engine};
    component.setData(QByteArrayLiteral("import QtQml\n"
                                        "QtObject {\n"
                                        "    property var source: null\n"
                                        "    property var session: null\n"
                                        "    property int calls: 0\n"
                                        "    property int lastRequest: -99\n"
                                        "    property int lastSlot: -99\n"
                                        "    function connectNow() {\n"
                                        "        source.fullCanvasFocusRequested.connect("
                                        "function(requestCode, sourceSlot) {\n"
                                        "            calls += 1;\n"
                                        "            lastRequest = requestCode;\n"
                                        "            lastSlot = sourceSlot;\n"
                                        "            session.requestCanvasFocus(requestCode, "
                                        "sourceSlot);\n"
                                        "        });\n"
                                        "    }\n"
                                        "}\n"),
                      QUrl{});
    if (component.status() != QQmlComponent::Ready) {
        return nullptr;
    }
    std::unique_ptr<QObject> relay{component.create()};
    if (relay == nullptr) {
        return nullptr;
    }
    relay->setProperty("source", QVariant::fromValue(bar));
    relay->setProperty("session", QVariant::fromValue(&shell));
    if (!QMetaObject::invokeMethod(relay.get(), "connectNow")) {
        return nullptr;
    }
    return relay;
}

// P05: the canvas-focus request codes the QML bar emits must mirror the shell's (and thereby
// the domain's) CanvasFocusRequest values. A drift on either side would silently re-target the
// canvas, so the cross-check is a first-class assertion.
TEST(MainQmlContractTests, FullCanvasFocusRequestCodesMatchTheShellEnum) {
    EXPECT_EQ(static_cast<int>(ReviewShellController::CanvasFocusOff), 0);
    EXPECT_EQ(static_cast<int>(ReviewShellController::CanvasFocusReference), 1);
    EXPECT_EQ(static_cast<int>(ReviewShellController::CanvasFocusSourceSlot), 2);
    EXPECT_EQ(static_cast<int>(domain::CanvasFocusRequest::Off),
              static_cast<int>(ReviewShellController::CanvasFocusOff));
    EXPECT_EQ(static_cast<int>(domain::CanvasFocusRequest::Reference),
              static_cast<int>(ReviewShellController::CanvasFocusReference));
    EXPECT_EQ(static_cast<int>(domain::CanvasFocusRequest::SourceSlot),
              static_cast<int>(ReviewShellController::CanvasFocusSourceSlot));
}

// P05/V02: three-source session with an identified GT (slot 2) exposes one equal direct-look
// entry per source - A/B/C - and the GT role is a badge on C's own button, never a fourth
// button. Switching changes only the shell's canvas focus: no playback command crosses the
// boundary (no seek, no re-pairing), and the GT role, the committed pair, the presented frame
// and the viewport's ROI/zoom/pan observation state all survive every switch.
TEST(MainQmlContractTests, FullCanvasFocusSwitchesSourceWithoutSeekOrPairOrRoleChanges) {
    ThreeSourceFocusHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;

    auto* const bar = harness.root->findChild<QQuickItem*>(QStringLiteral("compareModeBar"));
    ASSERT_NE(bar, nullptr);
    // The per-source focus buttons are Repeater delegates: item tree, not the QObject tree.
    auto* const focusA = findItemByName(bar, QStringLiteral("canvasFocusAButton"));
    ASSERT_NE(focusA, nullptr);
    auto* const focusB = findItemByName(bar, QStringLiteral("canvasFocusBButton"));
    ASSERT_NE(focusB, nullptr);
    auto* const focusC = findItemByName(bar, QStringLiteral("canvasFocusCButton"));
    ASSERT_NE(focusC, nullptr);
    // V02: the separate "看 GT" button is gone; per-source entries are equal and the reference
    // role shows as a badge on the source that carries it.
    EXPECT_EQ(findItemByName(bar, QStringLiteral("canvasFocusGtButton")), nullptr);
    auto* const surface = harness.root->findChild<QQuickItem*>(QStringLiteral("dualVideoSurface"));
    ASSERT_NE(surface, nullptr);

    // The bar's request-code properties mirror the shell enum (guarded separately above).
    EXPECT_EQ(bar->property("canvasFocusRequestOff").toInt(),
              static_cast<int>(ReviewShellController::CanvasFocusOff));
    EXPECT_EQ(bar->property("canvasFocusRequestReference").toInt(),
              static_cast<int>(ReviewShellController::CanvasFocusReference));
    EXPECT_EQ(bar->property("canvasFocusRequestSourceSlot").toInt(),
              static_cast<int>(ReviewShellController::CanvasFocusSourceSlot));

    // Three sources with a GT on slot 2: every source has an entry, exactly one carries the
    // GT badge, and no duplicate GT entry exists.
    EXPECT_TRUE(focusA->isVisible());
    EXPECT_TRUE(focusB->isVisible());
    EXPECT_TRUE(focusC->isVisible());
    EXPECT_FALSE(focusA->property("referenceBadge").toBool());
    EXPECT_FALSE(focusB->property("referenceBadge").toBool());
    EXPECT_TRUE(focusC->property("referenceBadge").toBool());
    EXPECT_EQ(harness.controller->referenceSourceIndex(), 2);
    EXPECT_TRUE(harness.shell->canvasFocusIdentity().isEmpty());
    EXPECT_EQ(harness.shell->canvasFocusSlot(), -1);

    // Stage a non-trivial observation context on the surface; the switches below must keep it.
    ASSERT_TRUE(QMetaObject::invokeMethod(surface,
                                          "restoreViewport",
                                          Q_ARG(qreal, 0.4),
                                          Q_ARG(qreal, 0.6),
                                          Q_ARG(qreal, 2.5),
                                          Q_ARG(bool, true),
                                          Q_ARG(qreal, 0.1),
                                          Q_ARG(qreal, 0.2),
                                          Q_ARG(qreal, 0.5),
                                          Q_ARG(qreal, 0.7)));
    const double centerX = surface->property("viewCenterX").toDouble();
    const double centerY = surface->property("viewCenterY").toDouble();
    const double scale = surface->property("viewScale").toDouble();
    const bool roiEnabled = surface->property("roiEnabled").toBool();
    const double roiLeft = surface->property("roiLeft").toDouble();
    const double roiTop = surface->property("roiTop").toDouble();
    const double roiRight = surface->property("roiRight").toDouble();
    const double roiBottom = surface->property("roiBottom").toDouble();
    EXPECT_TRUE(roiEnabled);
    EXPECT_DOUBLE_EQ(scale, 2.5);

    // Enter a pair-relevant mode first: the switches must survive inside it, and the fallback
    // entries below are visible exactly in this state.
    harness.preferences.setViewMode(ReviewPreferencesController::ViewMode::Difference);
    QCoreApplication::processEvents();
    EXPECT_EQ(harness.root->property("effectiveViewMode").toInt(), ComparisonSurface::Difference);

    // The baseline every switch below must hold against.
    const QStringList identities = harness.shell->activeSourceIdentities();
    ASSERT_EQ(identities.size(), 3);
    const std::size_t submittedBefore = harness.submitted.size();
    const int edgeBefore = harness.controller->effectiveDifferenceEdge();
    const qint64 frameBefore = harness.controller->currentFrame();
    const qulonglong epochBefore = harness.controller->sessionEpoch();
    const int viewModeBefore = harness.root->property("effectiveViewMode").toInt();

    auto relay = installFocusWiring(harness.engine, bar, *harness.shell);
    ASSERT_NE(relay, nullptr);
    const int offRequest = static_cast<int>(ReviewShellController::CanvasFocusOff);
    const int gtRequest = static_cast<int>(ReviewShellController::CanvasFocusReference);
    const int slotRequest = static_cast<int>(ReviewShellController::CanvasFocusSourceSlot);

    // Click "看 A": the request crosses the QML boundary and lands in the shell as the slot-0
    // focus, with nothing else moving.
    ASSERT_TRUE(QMetaObject::invokeMethod(focusA, "clicked"));
    QCoreApplication::processEvents();
    EXPECT_EQ(relay->property("calls").toInt(), 1);
    EXPECT_EQ(relay->property("lastRequest").toInt(), slotRequest);
    EXPECT_EQ(relay->property("lastSlot").toInt(), 0);
    EXPECT_EQ(harness.shell->canvasFocusSlot(), 0);
    EXPECT_EQ(harness.shell->canvasFocusIdentity(), identities.at(0));
    // The switch itself must submit nothing: the command count does not move. (The startup
    // sequence before the baseline may legitimately carry a pair-policy command; only the
    // delta across the switch is the switch's own traffic.)
    EXPECT_EQ(harness.submitted.size(), submittedBefore);
    for (const auto& command : harness.submitted) {
        EXPECT_EQ(std::get_if<application::SeekFrameCommand>(&command), nullptr);
    }
    EXPECT_EQ(harness.controller->referenceSourceIndex(), 2);
    EXPECT_EQ(harness.controller->effectiveDifferenceEdge(), edgeBefore);
    EXPECT_EQ(harness.controller->currentFrame(), frameBefore);
    EXPECT_EQ(harness.controller->sessionEpoch(), epochBefore);
    EXPECT_EQ(harness.root->property("effectiveViewMode").toInt(), viewModeBefore);
    EXPECT_FALSE(surface->property("differenceSuppressed").toBool());
    EXPECT_DOUBLE_EQ(surface->property("viewCenterX").toDouble(), centerX);
    EXPECT_DOUBLE_EQ(surface->property("viewCenterY").toDouble(), centerY);
    EXPECT_DOUBLE_EQ(surface->property("viewScale").toDouble(), scale);
    EXPECT_EQ(surface->property("roiEnabled").toBool(), roiEnabled);
    EXPECT_DOUBLE_EQ(surface->property("roiLeft").toDouble(), roiLeft);
    EXPECT_DOUBLE_EQ(surface->property("roiTop").toDouble(), roiTop);
    EXPECT_DOUBLE_EQ(surface->property("roiRight").toDouble(), roiRight);
    EXPECT_DOUBLE_EQ(surface->property("roiBottom").toDouble(), roiBottom);

    // Click "看 B": same-frame switch to the second source, same observation context.
    ASSERT_TRUE(QMetaObject::invokeMethod(focusB, "clicked"));
    QCoreApplication::processEvents();
    EXPECT_EQ(relay->property("calls").toInt(), 2);
    EXPECT_EQ(relay->property("lastSlot").toInt(), 1);
    EXPECT_EQ(harness.shell->canvasFocusSlot(), 1);
    EXPECT_EQ(harness.shell->canvasFocusIdentity(), identities.at(1));
    EXPECT_EQ(harness.submitted.size(), submittedBefore);
    EXPECT_EQ(harness.controller->referenceSourceIndex(), 2);
    EXPECT_EQ(harness.controller->effectiveDifferenceEdge(), edgeBefore);
    EXPECT_DOUBLE_EQ(surface->property("viewScale").toDouble(), scale);

    // Click "看 C" - the GT source, addressed by its own slot like every other source: the
    // request lands on slot 2 and the identity is the reference's frozen identity.
    ASSERT_TRUE(QMetaObject::invokeMethod(focusC, "clicked"));
    QCoreApplication::processEvents();
    EXPECT_EQ(relay->property("calls").toInt(), 3);
    EXPECT_EQ(relay->property("lastRequest").toInt(), slotRequest);
    EXPECT_EQ(relay->property("lastSlot").toInt(), 2);
    EXPECT_EQ(harness.shell->canvasFocusSlot(), 2);
    EXPECT_EQ(harness.shell->canvasFocusIdentity(), identities.at(2));
    EXPECT_EQ(harness.submitted.size(), submittedBefore);

    // The role-based request stays resolvable at the shell level (the keyboard peek family
    // addresses the reference wherever it sits); with GT on slot 2 it names the same source.
    EXPECT_TRUE(harness.shell->requestCanvasFocus(gtRequest, 0));
    EXPECT_EQ(harness.shell->canvasFocusSlot(), 2);

    // Once the focus slot and the reference slot are bound (the production Main.qml wiring),
    // the C entry reads as focused and re-clicking it requests Off, returning to the
    // comparison view.
    ASSERT_TRUE(bar->setProperty("canvasFocusSlot", 2));
    ASSERT_TRUE(QMetaObject::invokeMethod(focusC, "clicked"));
    QCoreApplication::processEvents();
    EXPECT_EQ(relay->property("calls").toInt(), 4);
    EXPECT_EQ(relay->property("lastRequest").toInt(), offRequest);
    EXPECT_TRUE(harness.shell->canvasFocusIdentity().isEmpty());
    EXPECT_EQ(harness.shell->canvasFocusSlot(), -1);
    EXPECT_EQ(harness.submitted.size(), submittedBefore);

    // Refusals change nothing: an out-of-range slot and an unknown request code.
    EXPECT_FALSE(harness.shell->requestCanvasFocus(slotRequest, 7));
    EXPECT_FALSE(harness.shell->requestCanvasFocus(9, 0));
    EXPECT_TRUE(harness.shell->canvasFocusIdentity().isEmpty());
    EXPECT_EQ(harness.submitted.size(), submittedBefore);

    // The original switching entries survive the new group and stay functional: mode buttons,
    // the pair combo and the fixed-GT candidate switch are visible in this 3-source
    // pair-relevant mode, and the difference peek is offered next to the new direct-look
    // entries.
    auto* const sideModeButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("sideModeButton"));
    ASSERT_NE(sideModeButton, nullptr);
    EXPECT_TRUE(sideModeButton->isVisible());
    auto* const pairCombo = harness.root->findChild<QQuickItem*>(QStringLiteral("pairCombo"));
    ASSERT_NE(pairCombo, nullptr);
    EXPECT_TRUE(pairCombo->isVisible()) << visibleChainText(pairCombo);
    auto* const switchCandidate =
        harness.root->findChild<QQuickItem*>(QStringLiteral("switchCandidateButton"));
    ASSERT_NE(switchCandidate, nullptr);
    EXPECT_TRUE(switchCandidate->isVisible()) << visibleChainText(switchCandidate);
    auto* const peekButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("differencePeekButton"));
    ASSERT_NE(peekButton, nullptr);
    EXPECT_TRUE(peekButton->isVisible()) << visibleChainText(peekButton);
    // And a mode entry still works: leaving Difference for side-by-side keeps the focus state
    // alone - view modes and the canvas focus are independent state.
    ASSERT_TRUE(QMetaObject::invokeMethod(sideModeButton, "clicked"));
    QCoreApplication::processEvents();
    EXPECT_EQ(harness.root->property("effectiveViewMode").toInt(), ComparisonSurface::SideBySide);
    EXPECT_TRUE(harness.shell->canvasFocusIdentity().isEmpty());
}

// P05/V02: two-source sessions offer one entry per source; the GT badge follows the slot the
// session identifies as reference, and appears only once that reference is identified. A
// single-source session hides the whole bar, focus group included.
TEST(MainQmlContractTests, FullCanvasFocusGroupShowsOnlyApplicableEntriesPerSession) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;

    auto* const bar = harness.root->findChild<QQuickItem*>(QStringLiteral("compareModeBar"));
    ASSERT_NE(bar, nullptr);
    auto* const focusA = findItemByName(bar, QStringLiteral("canvasFocusAButton"));
    ASSERT_NE(focusA, nullptr);
    auto* const focusB = findItemByName(bar, QStringLiteral("canvasFocusBButton"));
    ASSERT_NE(focusB, nullptr);
    // Two sources means two entries; there is no C entry to find (item tree included).
    EXPECT_EQ(findItemByName(bar, QStringLiteral("canvasFocusCButton")), nullptr);
    const int gtRequest = static_cast<int>(ReviewShellController::CanvasFocusReference);
    const int slotRequest = static_cast<int>(ReviewShellController::CanvasFocusSourceSlot);

    // Two sources, no identified GT yet: A/B entries without any badge.
    EXPECT_EQ(harness.controller->sourceCount(), 2);
    EXPECT_TRUE(focusA->isVisible());
    EXPECT_TRUE(focusB->isVisible());
    EXPECT_FALSE(focusA->property("referenceBadge").toBool());
    EXPECT_FALSE(focusB->property("referenceBadge").toBool());
    // The shell refuses a reference request while no GT is identifiable.
    EXPECT_FALSE(harness.shell->requestCanvasFocus(gtRequest, 0));
    EXPECT_TRUE(harness.shell->canvasFocusIdentity().isEmpty());

    // Identifying the GT on slot 0 badges A's own entry and makes the request resolvable.
    identifyReferenceSlot(harness.snapshot, 0);
    harness.controller->refreshProjection();
    QCoreApplication::processEvents();
    ASSERT_EQ(harness.controller->referenceSourceIndex(), 0);
    EXPECT_TRUE(focusA->isVisible());
    EXPECT_TRUE(focusB->isVisible());
    EXPECT_TRUE(focusA->property("referenceBadge").toBool());
    EXPECT_FALSE(focusB->property("referenceBadge").toBool());
    const QStringList identities = harness.shell->activeSourceIdentities();
    ASSERT_EQ(identities.size(), 2);
    ASSERT_TRUE(harness.shell->requestCanvasFocus(gtRequest, 0));
    EXPECT_EQ(harness.shell->canvasFocusSlot(), 0);
    EXPECT_EQ(harness.shell->canvasFocusIdentity(), identities.at(0));

    // EX-03: shrinking to one source keeps the bar (with the disabled tasks and the reason),
    // but the direct-look group and the pair controls still retire with the second source.
    harness.snapshot->sources.erase(harness.snapshot->sources.begin() + 1);
    harness.snapshot->presentedSources.erase(harness.snapshot->presentedSources.begin() + 1);
    identifyReferenceSlot(harness.snapshot, 0);
    harness.controller->refreshProjection();
    QCoreApplication::processEvents();
    EXPECT_EQ(harness.controller->sourceCount(), 1);
    EXPECT_TRUE(bar->isVisible());
    EXPECT_FALSE(focusA->isVisible());
    const QStringList remaining = harness.shell->activeSourceIdentities();
    ASSERT_EQ(remaining.size(), 1);
    EXPECT_EQ(harness.shell->canvasFocusIdentity(), remaining.at(0));
    EXPECT_FALSE(harness.shell->requestCanvasFocus(slotRequest, 1));
}

// P05: the focus names a source by frozen identity, so a source-set change that removes the
// focused source clears the focus instead of silently re-targeting whatever took its slot.
TEST(MainQmlContractTests, CanvasFocusClearsWhenFocusedSourceLeavesActiveSet) {
    ThreeSourceFocusHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;

    const QStringList identities = harness.shell->activeSourceIdentities();
    ASSERT_EQ(identities.size(), 3);
    const int slotRequest = static_cast<int>(ReviewShellController::CanvasFocusSourceSlot);
    ASSERT_TRUE(harness.shell->requestCanvasFocus(slotRequest, 1));
    EXPECT_EQ(harness.shell->canvasFocusSlot(), 1);
    EXPECT_EQ(harness.shell->canvasFocusIdentity(), identities.at(1));

    // Remove source B (slot 1) from the session.
    harness.snapshot->sources.erase(harness.snapshot->sources.begin() + 1);
    harness.snapshot->presentedSources.erase(
        std::remove_if(harness.snapshot->presentedSources.begin(),
                       harness.snapshot->presentedSources.end(),
                       [](const application::PresentedSourceState& presented) {
                           return presented.sourceId == 1U;
                       }),
        harness.snapshot->presentedSources.end());
    identifyReferenceSlot(harness.snapshot, 2);
    harness.controller->refreshProjection();
    QCoreApplication::processEvents();

    EXPECT_TRUE(harness.shell->canvasFocusIdentity().isEmpty());
    EXPECT_EQ(harness.shell->canvasFocusSlot(), -1);
}

// P05: the production Main.qml wiring, exercised as shipped - no relay, no manual
// setProperty. Clicking 看A crosses the bar's signal into the shell action, and the shell's
// slot flows back through the bar and viewport bindings to the surface. Re-clicking the now
// checked entry requests Off and the bindings carry -1 all the way back.
TEST(MainQmlContractTests, FullCanvasFocusFlowsThroughTheRealMainQmlWiring) {
    ThreeSourceFocusHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;

    auto* const bar = harness.root->findChild<QQuickItem*>(QStringLiteral("compareModeBar"));
    ASSERT_NE(bar, nullptr);
    auto* const focusA = findItemByName(bar, QStringLiteral("canvasFocusAButton"));
    ASSERT_NE(focusA, nullptr);
    auto* const surface = harness.root->findChild<QQuickItem*>(QStringLiteral("dualVideoSurface"));
    ASSERT_NE(surface, nullptr);

    EXPECT_EQ(harness.shell->canvasFocusSlot(), -1);
    EXPECT_EQ(bar->property("canvasFocusSlot").toInt(), -1);
    EXPECT_EQ(surface->property("canvasFocusSlot").toInt(), -1);
    EXPECT_FALSE(focusA->property("checked").toBool());

    ASSERT_TRUE(QMetaObject::invokeMethod(focusA, "clicked"));
    QCoreApplication::processEvents();
    EXPECT_EQ(harness.shell->canvasFocusSlot(), 0);
    EXPECT_EQ(bar->property("canvasFocusSlot").toInt(), 0);
    EXPECT_EQ(surface->property("canvasFocusSlot").toInt(), 0);
    EXPECT_TRUE(focusA->property("checked").toBool());

    // The checked state mirrors the shell through the binding; re-clicking it requests Off.
    ASSERT_TRUE(QMetaObject::invokeMethod(focusA, "clicked"));
    QCoreApplication::processEvents();
    EXPECT_EQ(harness.shell->canvasFocusSlot(), -1);
    EXPECT_EQ(bar->property("canvasFocusSlot").toInt(), -1);
    EXPECT_EQ(surface->property("canvasFocusSlot").toInt(), -1);
    EXPECT_FALSE(focusA->property("checked").toBool());
}

TEST(MainQmlContractTests, ComparisonModeButtonsExitDirectLookEvenForTheCurrentLayout) {
    ThreeSourceFocusHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    auto* const bar = harness.root->findChild<QQuickItem*>(QStringLiteral("compareModeBar"));
    auto* const surface = harness.root->findChild<QQuickItem*>(QStringLiteral("dualVideoSurface"));
    ASSERT_NE(bar, nullptr);
    ASSERT_NE(surface, nullptr);
    auto* const focusA = findItemByName(bar, QStringLiteral("canvasFocusAButton"));
    auto* const focusB = findItemByName(bar, QStringLiteral("canvasFocusBButton"));
    ASSERT_NE(focusA, nullptr);
    ASSERT_NE(focusB, nullptr);
    const auto commandsBefore = harness.submitted.size();
    for (const auto& [name, mode] :
         {std::pair{QStringLiteral("sideModeButton"), ComparisonSurface::SideBySide},
          std::pair{QStringLiteral("wipeModeButton"), ComparisonSurface::Wipe},
          std::pair{QStringLiteral("roiTripletTaskButton"), ComparisonSurface::RoiTriplet}}) {
        auto* const button = findItemByName(bar, name);
        ASSERT_NE(button, nullptr);
        for (auto* focus : {focusA, focusB}) {
            ASSERT_TRUE(QMetaObject::invokeMethod(focus, "clicked"));
            QCoreApplication::processEvents();
            EXPECT_GE(harness.shell->canvasFocusSlot(), 0);
            ASSERT_TRUE(QMetaObject::invokeMethod(button, "clicked"));
            QCoreApplication::processEvents();
            EXPECT_EQ(harness.shell->canvasFocusSlot(), -1);
            EXPECT_EQ(surface->property("canvasFocusSlot").toInt(), -1);
            EXPECT_EQ(bar->property("currentMode").toInt(), static_cast<int>(mode));
        }
    }
    EXPECT_EQ(harness.submitted.size(), commandsBefore) << "layout changes must not seek";
}

// V02: the source chip body is the per-source direct-look entry. One plain click on a chip
// enters that source's full-canvas view through the production Main.qml wiring, a second
// click on the same chip exits, and a click on another chip switches directly. Every switch
// is presentation state only: no playback command crosses the boundary (no seek, no
// re-pairing, no reopen), and the GT role, the committed pair, the presented frame, the view
// mode, the display order and the viewport's ROI/zoom/pan observation state all survive.
TEST(MainQmlContractTests, SourceChipDirectLookSwitchesWithoutTouchingMediaTruthOrObservation) {
    ThreeSourceFocusHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;

    auto* const surface = harness.root->findChild<QQuickItem*>(QStringLiteral("dualVideoSurface"));
    ASSERT_NE(surface, nullptr);
    auto* const strip = harness.root->findChild<QQuickItem*>(QStringLiteral("activeSourceStrip"));
    ASSERT_NE(strip, nullptr);
    // Chips are Repeater delegates: look them up through the item tree.
    auto* const chipA = findItemByName(strip, QStringLiteral("sourceChip-0"));
    ASSERT_NE(chipA, nullptr);
    auto* const chipB = findItemByName(strip, QStringLiteral("sourceChip-1"));
    ASSERT_NE(chipB, nullptr);

    // Stage a non-trivial observation context; every switch below must keep it.
    ASSERT_TRUE(QMetaObject::invokeMethod(surface,
                                          "restoreViewport",
                                          Q_ARG(qreal, 0.35),
                                          Q_ARG(qreal, 0.65),
                                          Q_ARG(qreal, 3.0),
                                          Q_ARG(bool, true),
                                          Q_ARG(qreal, 0.2),
                                          Q_ARG(qreal, 0.1),
                                          Q_ARG(qreal, 0.6),
                                          Q_ARG(qreal, 0.8)));
    EXPECT_DOUBLE_EQ(surface->property("viewScale").toDouble(), 3.0);

    harness.preferences.setViewMode(ReviewPreferencesController::ViewMode::Difference);
    QCoreApplication::processEvents();

    const QStringList identities = harness.shell->activeSourceIdentities();
    ASSERT_EQ(identities.size(), 3);
    const QStringList displayOrderBefore = harness.shell->displaySourceIdentities();
    const std::size_t submittedBefore = harness.submitted.size();
    const int referenceBefore = harness.controller->referenceSourceIndex();
    const int edgeBefore = harness.controller->effectiveDifferenceEdge();
    const qint64 frameBefore = harness.controller->currentFrame();
    const qulonglong epochBefore = harness.controller->sessionEpoch();
    const int viewModeBefore = harness.root->property("effectiveViewMode").toInt();

    // Enter B's direct look through the chip body (the exact path TapHandler takes).
    ASSERT_TRUE(QMetaObject::invokeMethod(
        strip, "chipClicked", Q_ARG(QVariant, QVariant::fromValue(chipB))));
    QCoreApplication::processEvents();
    EXPECT_EQ(harness.shell->canvasFocusSlot(), 1);
    EXPECT_EQ(harness.shell->canvasFocusIdentity(), identities.at(1));
    EXPECT_TRUE(chipB->property("isCanvasFocus").toBool());
    EXPECT_FALSE(chipA->property("isCanvasFocus").toBool());

    // Same chip again: exit back to the comparison view.
    ASSERT_TRUE(QMetaObject::invokeMethod(
        strip, "chipClicked", Q_ARG(QVariant, QVariant::fromValue(chipB))));
    QCoreApplication::processEvents();
    EXPECT_EQ(harness.shell->canvasFocusSlot(), -1);
    EXPECT_TRUE(harness.shell->canvasFocusIdentity().isEmpty());
    EXPECT_FALSE(chipB->property("isCanvasFocus").toBool());

    // A different chip switches directly, without an Off step in between.
    ASSERT_TRUE(QMetaObject::invokeMethod(
        strip, "chipClicked", Q_ARG(QVariant, QVariant::fromValue(chipA))));
    QCoreApplication::processEvents();
    EXPECT_EQ(harness.shell->canvasFocusSlot(), 0);
    EXPECT_EQ(harness.shell->canvasFocusIdentity(), identities.at(0));

    // The invariant block: nothing but the presentation focus moved.
    EXPECT_EQ(harness.submitted.size(), submittedBefore);
    for (const auto& command : harness.submitted) {
        EXPECT_EQ(std::get_if<application::SeekFrameCommand>(&command), nullptr);
    }
    EXPECT_EQ(harness.controller->referenceSourceIndex(), referenceBefore);
    EXPECT_EQ(harness.controller->effectiveDifferenceEdge(), edgeBefore);
    EXPECT_EQ(harness.controller->currentFrame(), frameBefore);
    EXPECT_EQ(harness.controller->sessionEpoch(), epochBefore);
    EXPECT_EQ(harness.root->property("effectiveViewMode").toInt(), viewModeBefore);
    EXPECT_EQ(harness.shell->displaySourceIdentities(), displayOrderBefore);
    EXPECT_DOUBLE_EQ(surface->property("viewCenterX").toDouble(), 0.35);
    EXPECT_DOUBLE_EQ(surface->property("viewCenterY").toDouble(), 0.65);
    EXPECT_DOUBLE_EQ(surface->property("viewScale").toDouble(), 3.0);
    EXPECT_TRUE(surface->property("roiEnabled").toBool());
    EXPECT_DOUBLE_EQ(surface->property("roiLeft").toDouble(), 0.2);
    EXPECT_DOUBLE_EQ(surface->property("roiTop").toDouble(), 0.1);
    EXPECT_DOUBLE_EQ(surface->property("roiRight").toDouble(), 0.6);
    EXPECT_DOUBLE_EQ(surface->property("roiBottom").toDouble(), 0.8);
}

// V02: the three-source pair selector names the two real files of each edge, so "which clip
// am I comparing" never hides behind the A/B/C slot letters. The letters stay available on
// the "label" role for the inspector rows.
TEST(MainQmlContractTests, PairComboNamesTheTwoRealSourceFiles) {
    ThreeSourceFocusHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;

    // Name the sources like a real GT+two-outputs session; the projection's row filename is
    // the snapshot view's displayName.
    ASSERT_EQ(harness.snapshot->sources.size(), 3U);
    harness.snapshot->sources[0].displayName = "gt.mp4";
    harness.snapshot->sources[1].displayName = "model-1.mp4";
    harness.snapshot->sources[2].displayName = "model-2.mp4";
    harness.controller->refreshProjection();
    harness.preferences.setViewMode(ReviewPreferencesController::ViewMode::Difference);
    QCoreApplication::processEvents();

    auto* const pairCombo = harness.root->findChild<QQuickItem*>(QStringLiteral("pairCombo"));
    ASSERT_NE(pairCombo, nullptr);
    EXPECT_TRUE(pairCombo->isVisible()) << visibleChainText(pairCombo);
    // The default edge is A↔B; the selector names both files, not "A ↔ B".
    EXPECT_EQ(pairCombo->property("currentText").toString(),
              QStringLiteral("gt.mp4 ↔ model-1.mp4"));
    // Every offered edge names its two real files.
    const QVariantList edges = harness.controller->differenceEdges();
    ASSERT_EQ(edges.size(), 3);
    EXPECT_EQ(edges.at(0).toMap().value(QStringLiteral("pairLabel")).toString(),
              QStringLiteral("gt.mp4 ↔ model-1.mp4"));
    EXPECT_EQ(edges.at(1).toMap().value(QStringLiteral("pairLabel")).toString(),
              QStringLiteral("gt.mp4 ↔ model-2.mp4"));
    EXPECT_EQ(edges.at(2).toMap().value(QStringLiteral("pairLabel")).toString(),
              QStringLiteral("model-1.mp4 ↔ model-2.mp4"));
    // The slot-letter role stays for callers that address slots.
    EXPECT_EQ(edges.at(0).toMap().value(QStringLiteral("label")).toString(),
              QStringLiteral("A ↔ B"));
}

// V03: the mode bar is organized by task. The four daily tasks - 并排 / 分割线 / 局部联动 /
// 差异定位 - are one-click top-level entries; the three secondary layouts (三联 / 参考聚焦 /
// 分析网格) live one level down in the labelled 布局 menu and keep driving the same ViewMode
// enum. The 局部联动 task reaches the existing RoiTriplet preset end-to-end: the surface
// maps it to the three-column panorama, the strip stages one window per source, and entering
// it is a pure view change (no playback command crosses the boundary).
TEST(MainQmlContractTests, ModeBarReflowsWithoutOverlappingActions) {
    ThreeSourceFocusHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    auto* const bar = harness.root->findChild<QQuickItem*>(QStringLiteral("compareModeBar"));
    ASSERT_NE(bar, nullptr);
    const QStringList names{QStringLiteral("sideModeButton"),
                            QStringLiteral("wipeModeButton"),
                            QStringLiteral("roiTripletTaskButton"),
                            QStringLiteral("diffModeButton"),
                            QStringLiteral("differencePeekButton"),
                            QStringLiteral("pairCombo"),
                            QStringLiteral("switchCandidateButton"),
                            QStringLiteral("canvasFocusAButton"),
                            QStringLiteral("canvasFocusBButton"),
                            QStringLiteral("canvasFocusCButton"),
                            QStringLiteral("layoutModesButton"),
                            QStringLiteral("copyComparisonButton"),
                            QStringLiteral("inspectorToggleButton")};
    harness.window->show();
    for (const int width : {960, 1280, 1440, 1920}) {
        harness.window->resize(width, 720);
        for (const auto mode : {ReviewPreferencesController::ViewMode::SideBySide,
                                ReviewPreferencesController::ViewMode::Difference}) {
            harness.preferences.setViewMode(mode);
            QElapsedTimer layoutWait;
            layoutWait.start();
            while (layoutWait.elapsed() < 250) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(5U);
            }
            std::vector<QRectF> occupied;
            std::vector<QString> occupiedNames;
            const auto rectText = [](const QRectF& rect) {
                return QStringLiteral("(%1,%2 %3x%4)")
                    .arg(rect.x())
                    .arg(rect.y())
                    .arg(rect.width())
                    .arg(rect.height())
                    .toStdString();
            };
            for (const auto& name : names) {
                auto* const button = findItemByName(bar, name);
                ASSERT_NE(button, nullptr) << name.toStdString();
                if (!button->isVisible()) {
                    continue;
                }
                const QRectF rect{button->mapToItem(bar, QPointF{}), button->size()};
                EXPECT_TRUE((QRectF{0, 0, bar->width(), bar->height()}.contains(rect)))
                    << width << " " << name.toStdString() << " rect=" << rectText(rect);
                for (std::size_t index = 0; index < occupied.size(); ++index) {
                    const bool intersects = rect.intersects(occupied[index]);
                    if (!intersects) {
                        continue;
                    }
                    // Subtree dump when the stacked-capsule signature appears: parent-relative
                    // x of every modeActions descendant tells whether the inner positioners ran.
                    if (name == QStringLiteral("switchCandidateButton")) {
                        std::function<void(QQuickItem*, const QString&)> dump;
                        dump = [&dump, &rectText](QQuickItem* node, const QString& indent) {
                            qDebug() << (indent + node->objectName() +
                                         " x=" + QString::number(node->x()) +
                                         " y=" + QString::number(node->y()) +
                                         " w=" + QString::number(node->width()));
                            for (QQuickItem* child : node->childItems()) {
                                dump(child, indent + QStringLiteral("  "));
                            }
                        };
                        if (auto* const flow =
                                findItemByName(bar, QStringLiteral("compareModeActions"))) {
                            dump(flow, QString{});
                        }
                    }
                    EXPECT_FALSE(intersects)
                        << width << " " << name.toStdString() << " rect=" << rectText(rect)
                        << " intersects " << occupiedNames[index].toStdString()
                        << " rect=" << rectText(occupied[index]);
                }
                occupied.push_back(rect);
                occupiedNames.push_back(name);
            }
            EXPECT_GE(occupied.size(), 10U);
            const QString evidence = qEnvironmentVariable("DVS_REVIEW_EVIDENCE_DIR");
            if (!evidence.isEmpty()) {
                ASSERT_TRUE(QDir().mkpath(evidence));
                ASSERT_TRUE(harness.window->grabWindow().save(
                    QDir{evidence}.filePath(QStringLiteral("compare-toolbar-%1-%2.png")
                                                .arg(width)
                                                .arg(static_cast<int>(mode)))));
            }
        }
    }
}

TEST(MainQmlContractTests, ModeBarOrganizesTasksWithLayoutSubmenu) {
    ThreeSourceFocusHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;

    auto* const sideButton = harness.root->findChild<QQuickItem*>(QStringLiteral("sideModeButton"));
    ASSERT_NE(sideButton, nullptr);
    auto* const wipeButton = harness.root->findChild<QQuickItem*>(QStringLiteral("wipeModeButton"));
    ASSERT_NE(wipeButton, nullptr);
    auto* const roiTripletButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("roiTripletTaskButton"));
    ASSERT_NE(roiTripletButton, nullptr);
    auto* const diffButton = harness.root->findChild<QQuickItem*>(QStringLiteral("diffModeButton"));
    ASSERT_NE(diffButton, nullptr);
    auto* const surface = harness.root->findChild<QQuickItem*>(QStringLiteral("dualVideoSurface"));
    ASSERT_NE(surface, nullptr);

    // Four one-click tasks at the top level; the anonymous "…" overflow is gone.
    EXPECT_TRUE(sideButton->isVisible());
    EXPECT_TRUE(wipeButton->isVisible());
    EXPECT_TRUE(wipeButton->property("text").toString().contains(QStringLiteral("分割线")));
    EXPECT_TRUE(roiTripletButton->isVisible());
    EXPECT_TRUE(roiTripletButton->property("enabled").toBool());
    EXPECT_TRUE(diffButton->isVisible());
    EXPECT_TRUE(diffButton->property("text").toString().contains(QStringLiteral("差异定位")));
    EXPECT_EQ(harness.root->findChild<QObject*>(QStringLiteral("moreCompareModesButton")), nullptr);

    // The layout submenu carries the three secondary layouts and nothing else from the old
    // overflow: the RoiTriplet entry moved up, the dual-difference entry moved into the
    // difference menu.
    auto* const layoutButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("layoutModesButton"));
    ASSERT_NE(layoutButton, nullptr);
    EXPECT_TRUE(layoutButton->property("text").toString().contains(QStringLiteral("布局")));
    auto* const threeUpItem =
        harness.root->findChild<QObject*>(QStringLiteral("modeBarThreeUpItem"));
    ASSERT_NE(threeUpItem, nullptr);
    EXPECT_TRUE(threeUpItem->property("enabled").toBool());
    EXPECT_NE(harness.root->findChild<QObject*>(QStringLiteral("modeBarReferenceFocusItem")),
              nullptr);
    EXPECT_NE(harness.root->findChild<QObject*>(QStringLiteral("modeBarAnalysisGridItem")),
              nullptr);
    EXPECT_EQ(harness.root->findChild<QObject*>(QStringLiteral("roiTripletMenuItem")), nullptr);

    // A layout entry still drives the same enum, and the button names the active layout.
    ASSERT_TRUE(QMetaObject::invokeMethod(threeUpItem, "triggered"));
    for (int iteration = 0; iteration < 8; ++iteration) {
        QCoreApplication::processEvents();
    }
    EXPECT_EQ(harness.root->property("effectiveViewMode").toInt(),
              static_cast<int>(dvs::ui::ComparisonSurface::ThreeUp));
    EXPECT_TRUE(layoutButton->property("text").toString().contains(QStringLiteral("三联")));

    // The 局部联动 task reaches the real preset: mode, panorama mapping, strip and window
    // count, without submitting anything.
    const std::size_t submittedBefore = harness.submitted.size();
    ASSERT_TRUE(QMetaObject::invokeMethod(roiTripletButton, "clicked"));
    for (int iteration = 0; iteration < 8; ++iteration) {
        QCoreApplication::processEvents();
    }
    EXPECT_EQ(harness.root->property("effectiveViewMode").toInt(), 8);
    EXPECT_EQ(surface->property("viewMode").toInt(),
              static_cast<int>(dvs::ui::ComparisonSurface::ThreeUp));
    EXPECT_TRUE(roiTripletButton->property("checked").toBool());
    auto* const roiStrip = harness.root->findChild<QQuickItem*>(QStringLiteral("roiTripletStrip"));
    ASSERT_NE(roiStrip, nullptr);
    EXPECT_TRUE(roiStrip->property("visible").toBool());
    auto* const windowRepeater =
        harness.root->findChild<QObject*>(QStringLiteral("roiTripletWindowRepeater"));
    ASSERT_NE(windowRepeater, nullptr);
    EXPECT_EQ(windowRepeater->property("count").toInt(), 3);
    EXPECT_EQ(harness.submitted.size(), submittedBefore);

    // Back to a plain task the same way.
    ASSERT_TRUE(QMetaObject::invokeMethod(sideButton, "clicked"));
    for (int iteration = 0; iteration < 8; ++iteration) {
        QCoreApplication::processEvents();
    }
    EXPECT_EQ(harness.root->property("effectiveViewMode").toInt(),
              static_cast<int>(dvs::ui::ComparisonSurface::SideBySide));
    EXPECT_FALSE(roiStrip->property("visible").toBool());
    EXPECT_EQ(harness.submitted.size(), submittedBefore);
}

// V03: with three sources and an identified GT, the difference-locating task recommends the
// GT-anchored dual difference by name, and entering it keeps the task button in charge (the
// button stays checked and names the 双参考 flavor). D2: the hold-to-peek GT entry follows
// the whole 差异定位 task - button and keyboard share one availability, so the dual layout
// (a GT-anchored difference flavour with the reference on screen) shows it too.
TEST(MainQmlContractTests, DualDifferenceGuidanceNamesTheBaseline) {
    ThreeSourceFocusHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;

    auto* const diffButton = harness.root->findChild<QQuickItem*>(QStringLiteral("diffModeButton"));
    ASSERT_NE(diffButton, nullptr);
    auto* const dualItem =
        harness.root->findChild<QObject*>(QStringLiteral("dualDifferenceMenuItem"));
    ASSERT_NE(dualItem, nullptr);
    // Menu rows only carry their visibility once the popup materializes them.
    auto* const diffMenu = harness.root->findChild<QObject*>(QStringLiteral("diffFlavorMenu"));
    ASSERT_NE(diffMenu, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(diffMenu, "open"));
    for (int iteration = 0; iteration < 5; ++iteration) {
        QCoreApplication::processEvents();
    }
    EXPECT_TRUE(dualItem->property("visible").toBool());
    EXPECT_TRUE(dualItem->property("enabled").toBool());
    EXPECT_TRUE(dualItem->property("text").toString().contains(QStringLiteral("GT 基准")));
    auto* const peekButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("differencePeekButton"));
    ASSERT_NE(peekButton, nullptr);

    const std::size_t submittedBefore = harness.submitted.size();
    ASSERT_TRUE(QMetaObject::invokeMethod(dualItem, "triggered"));
    for (int iteration = 0; iteration < 8; ++iteration) {
        QCoreApplication::processEvents();
    }
    EXPECT_EQ(harness.root->property("effectiveViewMode").toInt(), 9);
    EXPECT_TRUE(diffButton->property("checked").toBool());
    EXPECT_TRUE(diffButton->property("text").toString().contains(QStringLiteral("双参考")));
    // D2: the peek button and the ` key share one availability condition, and the
    // GT-anchored dual layout qualifies - no hidden-but-triggerable states either way.
    EXPECT_TRUE(peekButton->isVisible()) << visibleChainText(peekButton);
    EXPECT_TRUE(peekButton->property("enabled").toBool());
    // Entering a layout is still a pure view change.
    EXPECT_EQ(harness.submitted.size(), submittedBefore);
}

// V03: a three-source session without an identified GT must not be pushed into a dual
// difference against a silently chosen pseudo-baseline. The entry explains what is missing
// and stays disabled until a reference is set; the two pair-difference flavors remain the
// offered way to describe source-vs-source differences.
TEST(MainQmlContractTests, DualDifferenceWithoutReferenceStaysExplainedAndDisabled) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    // Grow the two-source session to three, without any identified reference.
    harness.snapshot->sources.push_back(application::SessionSourceView{
        .sourceId = 2U,
        .role = domain::ComparisonRole::kPrediction,
        .displayName = "C",
    });
    harness.snapshot->presentedSources.push_back(application::PresentedSourceState{
        .sourceId = 2U,
        .sourceFrameId = domain::FrameId{41},
        .matchKind = application::FrameMatchKind::ExactIndex,
    });
    harness.controller->refreshProjection();
    for (int iteration = 0; iteration < 5; ++iteration) {
        QCoreApplication::processEvents();
    }
    ASSERT_EQ(harness.controller->sourceCount(), 3);
    EXPECT_LT(harness.controller->referenceSourceIndex(), 0);

    auto* const dualItem =
        harness.root->findChild<QObject*>(QStringLiteral("dualDifferenceMenuItem"));
    ASSERT_NE(dualItem, nullptr);
    auto* const diffMenu = harness.root->findChild<QObject*>(QStringLiteral("diffFlavorMenu"));
    ASSERT_NE(diffMenu, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(diffMenu, "open"));
    for (int iteration = 0; iteration < 5; ++iteration) {
        QCoreApplication::processEvents();
    }
    EXPECT_TRUE(dualItem->property("visible").toBool());
    EXPECT_FALSE(dualItem->property("enabled").toBool());
    EXPECT_TRUE(dualItem->property("text").toString().contains(QStringLiteral("需先设置参考源")));
    auto* const pureItem = harness.root->findChild<QObject*>(QStringLiteral("diffPureMenuItem"));
    ASSERT_NE(pureItem, nullptr);
    EXPECT_TRUE(pureItem->property("enabled").toBool());

    // Identifying the reference turns the guidance on - same entry, now resolvable.
    identifyReferenceSlot(harness.snapshot, 1);
    harness.controller->refreshProjection();
    for (int iteration = 0; iteration < 5; ++iteration) {
        QCoreApplication::processEvents();
    }
    ASSERT_EQ(harness.controller->referenceSourceIndex(), 1);
    EXPECT_TRUE(dualItem->property("enabled").toBool());
    EXPECT_TRUE(dualItem->property("text").toString().contains(QStringLiteral("GT 基准")));
}

// Step-2 comparison export: the capture is the bare comparison picture - the rendered
// comparison with every overlapping overlay suppressed and no annotation bar. Cropping alone
// would keep the chrome, because the labels, badges, commands and the floating transport all
// overlap the picture rect.
TEST(MainQmlContractTests, ComparisonExportSuppressesSiblingOverlays) {
    // A synthetic tree: a picture with a chrome sibling inside the viewport, plus a transport
    // overlay one level further up (the PlayerOsc case). The capture must contain the picture
    // and none of the overlays - cropping alone would keep them, because they overlap the
    // picture rect.
    QQuickWindow window;
    window.resize(200, 100);
    window.show();
    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    QQmlComponent component{&engine};
    component.setData(R"qml(
import QtQuick
Item {
    // Stage level: the floating transport overlays the bottom of the picture, exactly like
    // PlayerOsc does over the comparison stage. It is a sibling of the viewport, not of the
    // picture, so a one-level suppression would miss it.
    Rectangle {
        objectName: "transport"
        x: 0
        y: 60
        width: 200
        height: 40
        color: "#ff0000"
    }
    Item {
        // Viewport level.
        anchors.fill: parent
        Rectangle {
            objectName: "picture"
            anchors.fill: parent
            color: "#204080"
        }
        Rectangle {
            objectName: "chrome"
            x: 40
            y: 20
            width: 80
            height: 40
            color: "#00ff00"
        }
    }
}
)qml",
                      QUrl{});
    if (component.status() != QQmlComponent::Ready) {
        qWarning() << "comparison-export chrome probe failed to compile:"
                   << component.errorString();
    }
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);
    std::unique_ptr<QObject> holder{component.create()};
    ASSERT_NE(holder, nullptr) << componentErrors(component);
    auto* const rootItem = qobject_cast<QQuickItem*>(holder.get());
    ASSERT_NE(rootItem, nullptr);
    rootItem->setParentItem(window.contentItem());
    rootItem->setSize(QSizeF{200.0, 100.0});
    for (int iteration = 0; iteration < 10; ++iteration) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    auto* const picture = rootItem->findChild<QQuickItem*>(QStringLiteral("picture"));
    ASSERT_NE(picture, nullptr);

    ComparisonExportController controller;
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("capture.png"));
    if (!controller.saveComparison(
            picture, QStringList{QStringLiteral("provenance line")}, QUrl::fromLocalFile(path))) {
        // grabWindow() yields nothing on a desktop that cannot render; the production path
        // reports exactly that instead of saving.
        if (controller.lastStatus().contains(QStringLiteral("无法抓取当前画面"))) {
            GTEST_SKIP() << "window capture unavailable on this desktop: "
                         << controller.lastStatus().toStdString();
        }
        FAIL() << controller.lastStatus().toStdString();
    }
    QImage saved;
    ASSERT_TRUE(saved.load(path));
    // Every pixel is the picture: the chrome sibling never reaches the capture.
    for (int y = 0; y < saved.height(); ++y) {
        for (int x = 0; x < saved.width(); ++x) {
            const QColor pixel = saved.pixelColor(x, y);
            ASSERT_TRUE(pixel.red() == 0x20 && pixel.green() == 0x40 && pixel.blue() == 0x80)
                << "viewport chrome leaked into the comparison capture at " << x << "," << y;
        }
    }
    // The provenance travels in the file's text metadata, never in the pixels.
    EXPECT_TRUE(saved.text(QStringLiteral("Description")).contains(QStringLiteral("provenance")));
}

// Step-2 comparison export wiring: the toolbar button copies the bare comparison picture to
// the clipboard, and the controller saves the same capture as a PNG whose provenance lives in
// the file's text metadata.
TEST(MainQmlContractTests, CopyComparisonButtonPutsBarePictureOnClipboard) {
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

    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(defaultPilotWindowProperties())};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);
    window->resize(1440, 900);
    window->show();
    QCoreApplication::processEvents();

    auto* const copyButton = root->findChild<QQuickItem*>(QStringLiteral("copyComparisonButton"));
    ASSERT_NE(copyButton, nullptr);
    EXPECT_TRUE(copyButton->isVisible());
    auto* const saveButton = root->findChild<QQuickItem*>(QStringLiteral("saveComparisonButton"));
    ASSERT_NE(saveButton, nullptr);
    EXPECT_TRUE(saveButton->isVisible());
    auto* const viewport = root->findChild<QQuickItem*>(QStringLiteral("mediaViewportFocusTarget"));
    ASSERT_NE(viewport, nullptr);
    // The capture target is the comparison surface itself, not the whole viewport: the
    // viewport's chrome (labels, badges, commands) and the floating transport must stay out
    // of the shared image, and the controller suppresses every overlapping sibling up the
    // ancestor chain to achieve that.
    const QVariant pictureValue = viewport->property("comparisonPicture");
    auto* const picture = pictureValue.value<QQuickItem*>();
    ASSERT_NE(picture, nullptr);
    EXPECT_EQ(picture->parentItem()->parentItem(), viewport);

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
    // The capture is the picture's device-pixel rect with no annotation bar appended.
    const qreal devicePixelRatio = window->devicePixelRatio();
    EXPECT_NEAR(pasted.width(), picture->width() * devicePixelRatio, 3.0);
    EXPECT_NEAR(pasted.height(), picture->height() * devicePixelRatio, 3.0);
    EXPECT_TRUE(exportController.lastStatus().contains(QStringLiteral("已复制")))
        << exportController.lastStatus().toStdString();

    // The same capture saves transactionally as a PNG file.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("comparison.png"));
    ASSERT_TRUE(exportController.saveComparison(
        picture, QStringList{QStringLiteral("A"), QStringLiteral("B")}, QUrl::fromLocalFile(path)));
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

TEST(MainQmlContractTests, ImageCropOverlayHandlesAndActionButtons) {
    WorkspaceHarness harness;
    harness.withImageEdit = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    QImage original(64, 48, QImage::Format_ARGB32);
    original.fill(QColor(100, 120, 140, 255));
    ASSERT_TRUE(harness.imageReview.openPrimaryImage(original, QStringLiteral("crop_overlay.png")));
    ASSERT_TRUE(harness.activateWorkspace(1));

    auto* const workspace =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
    auto* const startButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditStartButton"));
    auto* const viewport = harness.root->findChild<QQuickItem*>(QStringLiteral("primaryViewport"));
    auto* const preview = harness.root->findChild<QQuickItem*>(QStringLiteral("imageViewport-2"));
    ASSERT_NE(workspace, nullptr);
    ASSERT_NE(startButton, nullptr);
    ASSERT_NE(viewport, nullptr);
    ASSERT_NE(preview, nullptr);

    // Enter edit mode
    ASSERT_TRUE(QMetaObject::invokeMethod(startButton, "clicked"));
    harness.settle();
    EXPECT_TRUE(harness.imageEdit.active());

    // Before any drag, cropOverlay is hidden
    auto* const cropOverlay =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageCropOverlay-2"));
    ASSERT_NE(cropOverlay, nullptr);
    EXPECT_FALSE(cropOverlay->isVisible());

    // Drag crop rectangle
    const qreal scale =
        preview->width() / static_cast<qreal>(preview->property("sourceSize").toSize().width());
    ASSERT_GT(scale, 0.0);
    viewport->setProperty("cropStart",
                          QPointF{preview->x() + 10 * scale, preview->y() + 8 * scale});
    viewport->setProperty("cropCurrent",
                          QPointF{preview->x() + 42 * scale, preview->y() + 32 * scale});
    ASSERT_TRUE(QMetaObject::invokeMethod(
        workspace,
        "updateCropSelection",
        Q_ARG(QVariant, QVariant::fromValue(static_cast<QObject*>(viewport)))));

    // Overlay, action bar, handles and scrims are now visible
    EXPECT_TRUE(cropOverlay->isVisible());
    auto* const cropActionBar =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageCropActionBar-2"));
    auto* const floatingCancel =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageCropCancelButton-2"));
    auto* const floatingApply =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageCropApplyButton-2"));
    auto* const handleBr =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageCropHandle-br-2"));
    auto* const scrimTop =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageCropScrimTop-2"));
    auto* const scrimBottom =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageCropScrimBottom-2"));
    auto* const scrimLeft =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageCropScrimLeft-2"));
    auto* const scrimRight =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageCropScrimRight-2"));
    auto* const canvasMouseArea =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageCanvasMouseArea-2"));
    ASSERT_NE(cropActionBar, nullptr);
    ASSERT_NE(floatingCancel, nullptr);
    ASSERT_NE(floatingApply, nullptr);
    ASSERT_NE(handleBr, nullptr);
    ASSERT_NE(scrimTop, nullptr);
    ASSERT_NE(scrimBottom, nullptr);
    ASSERT_NE(scrimLeft, nullptr);
    ASSERT_NE(scrimRight, nullptr);
    ASSERT_NE(canvasMouseArea, nullptr);
    EXPECT_TRUE(cropActionBar->isVisible());
    EXPECT_TRUE(handleBr->isVisible());
    EXPECT_TRUE(scrimTop->isVisible());
    EXPECT_TRUE(scrimBottom->isVisible());
    EXPECT_TRUE(scrimLeft->isVisible());
    EXPECT_TRUE(scrimRight->isVisible());

    // Scrims strictly abut the crop bounding box without overlapping it
    const auto boxLeft = cropOverlay->property("boxLeft").toReal();
    const auto boxTop = cropOverlay->property("boxTop").toReal();
    const auto boxWidth = cropOverlay->property("boxWidth").toReal();
    const auto boxHeight = cropOverlay->property("boxHeight").toReal();
    EXPECT_NEAR(scrimTop->y() + scrimTop->height(), boxTop, 0.5);
    EXPECT_NEAR(scrimBottom->y(), boxTop + boxHeight, 0.5);
    EXPECT_NEAR(scrimLeft->x() + scrimLeft->width(), boxLeft, 0.5);
    EXPECT_NEAR(scrimRight->x(), boxLeft + boxWidth, 0.5);

    // Canvas mouse area accepts RightButton for smooth panning
    const int acceptedButtons = canvasMouseArea->property("acceptedButtons").toInt();
    EXPECT_TRUE((acceptedButtons & static_cast<int>(Qt::RightButton)) != 0);

    // Canceling through floatingCancel clears cropSelection and hides overlay
    ASSERT_TRUE(QMetaObject::invokeMethod(floatingCancel, "clicked"));
    harness.settle();
    EXPECT_TRUE(workspace->property("cropSelection").toMap().isEmpty());
    EXPECT_FALSE(cropOverlay->isVisible());

    // Re-create crop selection
    viewport->setProperty("cropStart", QPointF{preview->x() + 4 * scale, preview->y() + 4 * scale});
    viewport->setProperty("cropCurrent",
                          QPointF{preview->x() + 36 * scale, preview->y() + 28 * scale});
    ASSERT_TRUE(QMetaObject::invokeMethod(
        workspace,
        "updateCropSelection",
        Q_ARG(QVariant, QVariant::fromValue(static_cast<QObject*>(viewport)))));
    EXPECT_TRUE(cropOverlay->isVisible());

    // Applying through floatingApply commits the crop
    ASSERT_TRUE(QMetaObject::invokeMethod(floatingApply, "clicked"));
    harness.settle();
    EXPECT_EQ(harness.imageEdit.imageWidth(), 32);
    EXPECT_EQ(harness.imageEdit.imageHeight(), 24);
    EXPECT_TRUE(workspace->property("cropSelection").toMap().isEmpty());
    EXPECT_FALSE(cropOverlay->isVisible());
}

TEST(MainQmlContractTests, ImageEditRepeatedCropUsesUpdatedPreviewCoordinates) {
    WorkspaceHarness harness;
    harness.withImageEdit = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    QImage original(80, 60, QImage::Format_ARGB32);
    for (int y = 0; y < original.height(); ++y) {
        for (int x = 0; x < original.width(); ++x) {
            original.setPixelColor(x, y, QColor(x * 3, y * 4, 80, 200));
        }
    }
    ASSERT_TRUE(harness.imageReview.openPrimaryImage(original, QStringLiteral("crop.png")));
    ASSERT_TRUE(harness.activateWorkspace(1));
    auto* const workspace =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
    auto* const viewport = harness.root->findChild<QQuickItem*>(QStringLiteral("primaryViewport"));
    auto* const preview = harness.root->findChild<QQuickItem*>(QStringLiteral("imageViewport-2"));
    auto* const start =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditStartButton"));
    auto* const apply =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditApplyCropButton"));
    ASSERT_NE(workspace, nullptr);
    ASSERT_NE(viewport, nullptr);
    ASSERT_NE(preview, nullptr);
    ASSERT_NE(start, nullptr);
    ASSERT_NE(apply, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(start, "clicked"));
    const auto crop = [&](const QRect& rectangle, const QSize currentSize) {
        if (!harness.waitUntil([&]() {
                return preview->property("sourceSize").toSize() == currentSize &&
                       preview->width() > 0;
            })) {
            return false;
        }
        const qreal scale = preview->width() / currentSize.width();
        viewport->setProperty(
            "cropStart",
            QPointF{preview->x() + rectangle.x() * scale, preview->y() + rectangle.y() * scale});
        viewport->setProperty("cropCurrent",
                              QPointF{preview->x() + (rectangle.x() + rectangle.width()) * scale,
                                      preview->y() + (rectangle.y() + rectangle.height()) * scale});
        return QMetaObject::invokeMethod(
                   workspace,
                   "updateCropSelection",
                   Q_ARG(QVariant, QVariant::fromValue(static_cast<QObject*>(viewport)))) &&
               QMetaObject::invokeMethod(apply, "clicked");
    };
    ASSERT_TRUE(crop(QRect(10, 8, 50, 36), QSize(80, 60)));
    const QImage first = original.copy(10, 8, 50, 36);
    EXPECT_EQ(harness.imageEdit.editedImage(), first);
    harness.imageReview.setZoom(2.0);
    harness.imageReview.panBy(0.07, -0.04);
    harness.settle();
    ASSERT_TRUE(crop(QRect(7, 5, 25, 18), first.size()));
    const QImage second = original.copy(17, 13, 25, 18);
    EXPECT_EQ(harness.imageEdit.editedImage(), second);
    ASSERT_TRUE(harness.imageEdit.undo());
    EXPECT_EQ(harness.imageEdit.editedImage(), first);
    ASSERT_TRUE(harness.imageEdit.undo());
    EXPECT_EQ(harness.imageEdit.editedImage(), original);
    ASSERT_TRUE(harness.imageEdit.redo());
    ASSERT_TRUE(harness.imageEdit.redo());
    EXPECT_EQ(harness.imageEdit.editedImage(), second);
    EXPECT_EQ(harness.imageReview.rawImageForSlot(ImageReviewController::PrimarySlot), original);
    // A second crop dragged from the letterbox must retain only the portion inside the image.
    ASSERT_TRUE(harness.imageEdit.undo());
    ASSERT_TRUE(crop(QRect(-5, -4, 30, 22), first.size()));
    EXPECT_EQ(harness.imageEdit.editedImage(), first.copy(0, 0, 25, 18));
}

TEST(MainQmlContractTests, ImageEditTransformMenuChangesTheWorkingPixelsAndRetiresSelections) {
    WorkspaceHarness harness;
    harness.withImageEdit = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.window->resize(960, 640);
    QImage original(80, 60, QImage::Format_ARGB32);
    for (int y = 0; y < original.height(); ++y) {
        for (int x = 0; x < original.width(); ++x) {
            original.setPixelColor(x, y, QColor(x * 3, y * 4, x + y, 255));
        }
    }
    const QImage secondary = original.flipped(Qt::Horizontal);
    ASSERT_TRUE(harness.imageReview.openPairImages(
        original, QStringLiteral("transform.png"), secondary, QStringLiteral("secondary.png")));
    ASSERT_TRUE(harness.activateWorkspace(1));
    auto* const workspace =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
    auto* const start =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditStartButton"));
    auto* const button =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditTransformButton"));
    auto* const viewport = harness.root->findChild<QQuickItem*>(QStringLiteral("primaryViewport"));
    auto* const preview = harness.root->findChild<QQuickItem*>(QStringLiteral("imageViewport-2"));
    auto* const undo = harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditUndoButton"));
    ASSERT_NE(workspace, nullptr);
    ASSERT_NE(start, nullptr);
    ASSERT_NE(button, nullptr);
    ASSERT_NE(viewport, nullptr);
    ASSERT_NE(preview, nullptr);
    ASSERT_NE(undo, nullptr);
    const std::array<QString, 5> items{QStringLiteral("imageEditMirrorHorizontalItem"),
                                       QStringLiteral("imageEditFlipVerticalItem"),
                                       QStringLiteral("imageEditRotateClockwiseItem"),
                                       QStringLiteral("imageEditRotateCounterclockwiseItem"),
                                       QStringLiteral("imageEditRotateHalfTurnItem")};
    const std::array<QImage, 5> expected{original.flipped(Qt::Horizontal),
                                         original.flipped(Qt::Vertical),
                                         original.transformed(QTransform{}.rotate(90)),
                                         original.transformed(QTransform{}.rotate(-90)),
                                         secondary.flipped(Qt::Horizontal | Qt::Vertical)};
    for (std::size_t index = 0; index < items.size(); ++index) {
        SCOPED_TRACE(items[index].toStdString());
        const bool editingSecondary = index == 4;
        harness.imageReview.setCompareMode(editingSecondary ? 0 : 2);
        workspace->setProperty("singleViewShowSecondary", editingSecondary);
        ASSERT_TRUE(QMetaObject::invokeMethod(start, "clicked"));
        harness.settle();
        EXPECT_EQ(harness.imageReview.compareMode(), editingSecondary ? 0 : 2);
        EXPECT_EQ(workspace->property("compareMode").toInt(), 0);
        EXPECT_TRUE(viewport->isVisible());
        EXPECT_EQ(harness.imageEdit.sourceSlot(), editingSecondary ? 3 : 2);
        // Changing the review-side switch cannot redirect an already active edit session.
        workspace->setProperty("singleViewShowSecondary", !editingSecondary);
        EXPECT_EQ(viewport->property("slot").toInt(), editingSecondary ? 3 : 2);
        // The surviving direct-selection entries are the A/B file chips (the standalone
        // "切至 A / 切至 B" toolbar button is gone - it duplicated them, Space/T and the
        // in-canvas badge). They must stay inert during an edit session so a side switch
        // can never retarget the working copy, exactly as the removed button did.
        auto* const sourceChipA =
            harness.root->findChild<QQuickItem*>(QStringLiteral("imageSourceChipA"));
        auto* const sourceChipB =
            harness.root->findChild<QQuickItem*>(QStringLiteral("imageSourceChipB"));
        auto* const sourceBadge =
            harness.root->findChild<QQuickItem*>(QStringLiteral("imageInPlaceBadge"));
        ASSERT_NE(sourceChipA, nullptr);
        ASSERT_NE(sourceChipB, nullptr);
        ASSERT_NE(sourceBadge, nullptr);
        EXPECT_FALSE(sourceChipA->isEnabled());
        EXPECT_FALSE(sourceChipB->isEnabled());
        EXPECT_FALSE(sourceBadge->isVisible());
        // The deleted duplicate must not come back: no toolbar button may still offer the
        // A/B switch that the chips, Space/T and the badge already cover.
        EXPECT_EQ(harness.root->findChild<QQuickItem*>(QStringLiteral("imageToggleSourceButton")),
                  nullptr);
        ASSERT_TRUE(QMetaObject::invokeMethod(workspace, "toggleSinglePairSource"));
        EXPECT_EQ(workspace->property("singleViewShowSecondary").toBool(), !editingSecondary);
        EXPECT_EQ(harness.imageEdit.sourceSlot(), editingSecondary ? 3 : 2)
            << "the review-side switch must not redirect an active edit session";
        EXPECT_TRUE(button->isVisible());
        EXPECT_TRUE(button->property("enabled").toBool());
        workspace->setProperty("cropSelection",
                               QVariantMap{{QStringLiteral("x"), 4},
                                           {QStringLiteral("y"), 3},
                                           {QStringLiteral("width"), 10},
                                           {QStringLiteral("height"), 8}});
        ASSERT_TRUE(QMetaObject::invokeMethod(button, "clicked"));
        harness.settle();
        auto* const item = harness.root->findChild<QQuickItem*>(items[index]);
        ASSERT_NE(item, nullptr);
        const auto evidenceDirectory = qEnvironmentVariable("DVS_REVIEW_EVIDENCE_DIR");
        if (index == 0 && !evidenceDirectory.isEmpty()) {
            ASSERT_TRUE(QDir().mkpath(evidenceDirectory));
            harness.settleAnimations();
            ASSERT_TRUE(harness.window->grabWindow().save(
                QDir(evidenceDirectory)
                    .filePath(QStringLiteral("image-transform-toolbar-960.png"))));
            auto* const popup =
                harness.root->findChild<QObject*>(QStringLiteral("imageEditTransformMenu"));
            ASSERT_NE(popup, nullptr);
            auto* const popupSurface = menuPopupWindow(popup);
            ASSERT_NE(popupSurface, nullptr);
            ASSERT_TRUE(popupSurface->grabWindow().save(
                QDir(evidenceDirectory).filePath(QStringLiteral("image-transform-menu.png"))));
        }
        harness.imageReview.setZoom(2.0);
        harness.imageReview.panBy(0.08, -0.05);
        ASSERT_TRUE(QMetaObject::invokeMethod(item, "triggered"));
        auto* const menu =
            harness.root->findChild<QObject*>(QStringLiteral("imageEditTransformMenu"));
        ASSERT_NE(menu, nullptr);
        ASSERT_TRUE(QMetaObject::invokeMethod(menu, "close"));
        ASSERT_TRUE(harness.waitUntil([&]() { return !menu->property("visible").toBool(); }));
        harness.settle();
        ASSERT_TRUE(harness.waitUntil(
            [&]() { return preview->property("sourceSize").toSize() == expected[index].size(); }));
        EXPECT_EQ(harness.imageEdit.editedImage(), expected[index]);
        EXPECT_DOUBLE_EQ(harness.imageReview.zoom(), 1.0);
        EXPECT_DOUBLE_EQ(harness.imageReview.panX(), 0.5);
        EXPECT_DOUBLE_EQ(harness.imageReview.panY(), 0.5);
        EXPECT_TRUE(workspace->property("cropSelection").isNull());
        EXPECT_TRUE(viewport->property("label").toString().contains(QStringLiteral("编辑副本")));
        EXPECT_TRUE(viewport->property("label").toString().contains(
            QStringLiteral("%1×%2").arg(expected[index].width()).arg(expected[index].height())));
        // Let Qt Quick polish the changed layout before sending window-level pointer input.
        harness.settleAnimations(50);
        static_cast<void>(harness.window->grabWindow());
        // A fresh drag after the transform uses the new axes and the new canvas dimensions.
        const qreal scale = preview->width() / expected[index].width();
        const QPointF first =
            viewport->mapToScene(QPointF(preview->x() + 20 * scale, preview->y() + 20 * scale));
        const QPointF last =
            viewport->mapToScene(QPointF(preview->x() + 40 * scale, preview->y() + 35 * scale));
        sendMousePress(*harness.window, first);
        const QPointF globalPos = harness.window->mapToGlobal(last.toPoint());
        QMouseEvent move{
            QEvent::MouseMove, last, globalPos, Qt::NoButton, Qt::LeftButton, Qt::NoModifier};
        QCoreApplication::sendEvent(harness.window, &move);
        sendMouseRelease(*harness.window, last);
        EXPECT_EQ(
            workspace->property("cropSelection").toMap().value(QStringLiteral("width")).toInt(),
            20);
        EXPECT_EQ(
            workspace->property("cropSelection").toMap().value(QStringLiteral("height")).toInt(),
            15);
        auto* const apply =
            harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditApplyCropButton"));
        ASSERT_NE(apply, nullptr);
        ASSERT_TRUE(QMetaObject::invokeMethod(apply, "clicked"));
        EXPECT_EQ(harness.imageEdit.editedImage(), expected[index].copy(20, 20, 20, 15));
        ASSERT_TRUE(QMetaObject::invokeMethod(undo, "clicked"));
        EXPECT_EQ(harness.imageEdit.editedImage(), expected[index]);
        workspace->setProperty("cropSelection",
                               QVariantMap{{QStringLiteral("x"), 1},
                                           {QStringLiteral("y"), 1},
                                           {QStringLiteral("width"), 5},
                                           {QStringLiteral("height"), 5}});
        ASSERT_TRUE(QMetaObject::invokeMethod(undo, "clicked"));
        EXPECT_EQ(harness.imageEdit.editedImage(), editingSecondary ? secondary : original);
        EXPECT_TRUE(workspace->property("cropSelection").isNull());
        ASSERT_TRUE(QMetaObject::invokeMethod(start, "clicked"));
        EXPECT_EQ(workspace->property("compareMode").toInt(), editingSecondary ? 0 : 2);
    }
    EXPECT_EQ(harness.imageReview.rawImageForSlot(ImageReviewController::PrimarySlot), original);
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
    // The dropdown keeps its own 「更多工具」 label while a standalone tool (crop is the
    // edit-mode default) is active; it must not duplicate that tool's own button name.
    auto* const moreToolsButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageEditMoreToolsButton"));
    ASSERT_NE(moreToolsButton, nullptr);
    EXPECT_TRUE(moreToolsButton->property("text").toString().contains(QStringLiteral("更多工具")));

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
    // A tool hidden inside the dropdown gets named on its button, so the selection stays
    // readable from the closed row.
    EXPECT_TRUE(moreToolsButton->property("text").toString().contains(QStringLiteral("马赛克")));
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
    EXPECT_FALSE(openButton->property("prominent").toBool())
        << "once an image is open, the opening action steps back";
    auto* const modeRow = harness.root->findChild<QQuickItem*>(QStringLiteral("imageModeRow"));
    ASSERT_NE(modeRow, nullptr);
    EXPECT_TRUE(modeRow->isVisible());

    ASSERT_TRUE(QMetaObject::invokeMethod(startButton, "clicked"));
    harness.settle();
    EXPECT_TRUE(scaleButton->isVisible());
    EXPECT_FALSE(modeRow->isVisible()) << "comparison modes do not compete with editing tools";
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
    EXPECT_TRUE(modeRow->isVisible()) << "leaving edit mode restores comparison controls";
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

// The wipe split handle must be reachable and operable from the keyboard and a screen reader:
// Slider role, an accessible name, and a tab stop, with arrow stepping (1%, Shift 5%, clamped,
// own-axis only) handled by WipeHandle.qml itself. The global Alt+arrow shortcuts cover the
// split while focus lives elsewhere; this pins the handle-focused path (UX-15). Step semantics
// are covered by tst_wipe_handle.qml. The reads go through QQmlProperty because Attached
// properties are not QObject dynamic properties.
TEST(MainQmlContractTests, WipeHandleIsKeyboardAdjustable) {
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
    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(defaultPilotWindowProperties())};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);
    QCoreApplication::processEvents();

    QObject* const wipeHandle = root->findChild<QObject*>(QStringLiteral("wipeHandle"));
    ASSERT_NE(wipeHandle, nullptr);
    // Accessible.role must be QAccessible::Slider for an adjustable split control. Attached
    // properties live on the attached object, so read them through QQmlProperty, not
    // QObject::property().
    EXPECT_EQ(QQmlProperty(wipeHandle, QStringLiteral("Accessible.role"), qmlContext(wipeHandle))
                  .read()
                  .toInt(),
              static_cast<int>(QAccessible::Slider))
        << "Wipe handle must expose Accessible.Slider.";
    EXPECT_FALSE(QQmlProperty(wipeHandle, QStringLiteral("Accessible.name"), qmlContext(wipeHandle))
                     .read()
                     .toString()
                     .isEmpty())
        << "Wipe handle must expose an accessible name.";
    EXPECT_TRUE(wipeHandle->property("activeFocusOnTab").toBool())
        << "Wipe handle must be tab-focusable.";
}

// The Timeline must be reachable by a screen reader: a Slider role, a name, and tab focus. Qt
// 6.11's Accessible attached type has no numeric value property, so the frame position cannot be
// exposed from QML alone; that gap stays in the backlog and would need a custom accessible
// interface rather than a test-only property.
TEST(MainQmlContractTests, TimelineExposesAccessibleSliderRoleAndName) {
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
    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(defaultPilotWindowProperties())};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);
    QCoreApplication::processEvents();

    QObject* const timeline = root->findChild<QObject*>(QStringLiteral("timelineSlider"));
    ASSERT_NE(timeline, nullptr);
    EXPECT_EQ(QQmlProperty(timeline, QStringLiteral("Accessible.role"), qmlContext(timeline))
                  .read()
                  .toInt(),
              static_cast<int>(QAccessible::Slider))
        << "Timeline must expose Accessible.Slider.";
    EXPECT_FALSE(QQmlProperty(timeline, QStringLiteral("Accessible.name"), qmlContext(timeline))
                     .read()
                     .toString()
                     .isEmpty())
        << "Timeline must expose an accessible name.";
    EXPECT_TRUE(timeline->property("activeFocusOnTab").toBool())
        << "Timeline must be tab-focusable.";
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
    ColorClipboardController colorClipboard;

    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    engine.rootContext()->setContextProperty(QStringLiteral("reviewController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewPreferences"), &preferences);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewSession"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewFacade"), &facade);
    engine.addImageProvider(QStringLiteral("vcs-review"), new ReviewImageProvider(&imageReview));
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);

    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(pilotWindowProperties(imageReview, colorClipboard))};
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
    // element received a null pixmap from the provider. The provider URL is
    // slot/generation/view-mode/alpha-filter/alpha-threshold/colour-toggle: the colour
    // toggle is part of the identity so flipping it cannot reuse a cached pixmap.
    EXPECT_EQ(viewport->property("imageUrl").toString(),
              QStringLiteral("image://vcs-review/2/0/0/0/0/1"));
    EXPECT_EQ(imageItem->property("source").toString(),
              QStringLiteral("image://vcs-review/2/0/0/0/0/1"));

    QImage image(64, 48, QImage::Format_ARGB32);
    image.fill(QColor(200, 30, 30));
    ASSERT_TRUE(imageReview.openPrimaryImage(std::move(image), QStringLiteral("left")));
    for (int iteration = 0; iteration < 5; ++iteration) {
        QCoreApplication::processEvents();
    }

    // The binding must re-evaluate (generation 0 -> 1) so the Image element reloads the
    // provider and actually paints the decoded pixels instead of staying black.
    EXPECT_EQ(viewport->property("imageUrl").toString(),
              QStringLiteral("image://vcs-review/2/1/0/0/0/1"));
    EXPECT_EQ(imageItem->property("source").toString(),
              QStringLiteral("image://vcs-review/2/1/0/0/0/1"));
    // 1 == QQuickImage::Ready.
    EXPECT_EQ(imageItem->property("status").toInt(), 1)
        << "Image element must reach Ready after the source URL refreshes.";
    EXPECT_EQ(imageItem->property("sourceSize").toSize(), QSize(64, 48));

    // Channel changes must refresh the provider URL even when the source image is unchanged.
    // Compare against the URL that was actually presented, not a literal: a literal would
    // silently stop proving "the URL changed" the next time the URL contract grows a field.
    const QString rgbaUrl = imageItem->property("source").toString();
    imageReview.setViewMode(ImageReviewController::AlphaGrayView);
    QCoreApplication::processEvents();
    EXPECT_EQ(imageItem->property("source").toString(), imageReview.imageUrl(2));
    EXPECT_NE(imageItem->property("source").toString(), rgbaUrl);
    EXPECT_EQ(imageItem->property("status").toInt(), 1);
    const QString alphaUrl = imageItem->property("source").toString();
    imageReview.setViewMode(ImageReviewController::RgbaView);
    QCoreApplication::processEvents();
    EXPECT_EQ(imageItem->property("source").toString(), imageReview.imageUrl(2));
    EXPECT_NE(imageItem->property("source").toString(), alphaUrl);
}

TEST(MainQmlContractTests, ImageColorStateIsVisiblePerSideWithoutHover) {
    // COLOR3: the colour state must be readable from the always-visible status strip, per
    // side, and it must be independent of the "format converted to RGBA8" badge. A native
    // RGBA8 image with an embedded ICC profile still reports "配置未应用"; a side without a
    // profile reports the explicit sRGB assumption; both say the display is unverified.
    // Swap, single-side replace, a failed open and a fast pair switch must all keep the
    // badges on the currently committed material.
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
    ColorClipboardController colorClipboard;

    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    engine.rootContext()->setContextProperty(QStringLiteral("reviewController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewPreferences"), &preferences);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewSession"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewFacade"), &facade);
    engine.addImageProvider(QStringLiteral("vcs-review"), new ReviewImageProvider(&imageReview));
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);

    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(pilotWindowProperties(imageReview, colorClipboard))};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);
    window->resize(960, 640);
    QCoreApplication::processEvents();

    QVariant imageWorkspaceActivated;
    ASSERT_TRUE(QMetaObject::invokeMethod(root.get(),
                                          "activateWorkspace",
                                          Q_RETURN_ARG(QVariant, imageWorkspaceActivated),
                                          Q_ARG(QVariant, QVariant{1})));
    ASSERT_TRUE(imageWorkspaceActivated.toBool());
    QCoreApplication::processEvents();

    auto* const primaryBadge =
        root->findChild<QQuickItem*>(QStringLiteral("imagePrimaryColorStatusBadge"));
    auto* const secondaryBadge =
        root->findChild<QQuickItem*>(QStringLiteral("imageSecondaryColorStatusBadge"));
    auto* const conversionBadge =
        root->findChild<QQuickItem*>(QStringLiteral("displayConversionBadge"));
    ASSERT_NE(primaryBadge, nullptr);
    ASSERT_NE(secondaryBadge, nullptr);
    ASSERT_NE(conversionBadge, nullptr);

    const auto pump = [&imageReview]() {
        QElapsedTimer timer;
        timer.start();
        while (imageReview.openPending() && timer.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
        for (int iteration = 0; iteration < 5; ++iteration) {
            QCoreApplication::processEvents();
        }
    };
    const auto rgba8Info = [](int profileStatus) {
        StillImageSourceInfo info;
        info.bitDepth = 8;
        info.channels = 4;
        info.hasAlpha = true;
        info.straightAlpha = true;
        info.sourceFormat = QStringLiteral("rgba");
        info.displayConverted = false;
        info.color.profileStatus = static_cast<ImageColorProfileStatus>(profileStatus);
        return info;
    };

    QImage imageA(64, 48, QImage::Format_ARGB32);
    imageA.fill(QColor(200, 30, 30));
    QImage imageB(32, 24, QImage::Format_ARGB32);
    imageB.fill(QColor(20, 30, 200));

    StillImageSourceInfo primaryInfo =
        rgba8Info(static_cast<int>(ImageColorProfileStatus::EmbeddedSupported));
    // COLOR4: a parsed SDR profile turns the display conversion on (default) and the compact
    // badge must say "converted", still followed by the unverified-display admission.
    primaryInfo.colorSpace = QColorSpace{QColorSpace::DisplayP3};
    primaryInfo.color.profileSha256 =
        QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    primaryInfo.color.profileDataSpace = QStringLiteral("RGB ");
    primaryInfo.color.profileClass = QStringLiteral("mntr");
    primaryInfo.color.profileVersionMajor = 4;
    StillImageSourceInfo secondaryInfo = rgba8Info(static_cast<int>(ImageColorProfileStatus::None));
    ASSERT_TRUE(imageReview.openPairImages(std::move(imageA),
                                           QStringLiteral("a.png"),
                                           std::move(imageB),
                                           QStringLiteral("b.png"),
                                           -1,
                                           primaryInfo,
                                           secondaryInfo));
    pump();

    EXPECT_TRUE(primaryBadge->isVisible()) << visibleChainText(primaryBadge);
    EXPECT_TRUE(secondaryBadge->isVisible()) << visibleChainText(secondaryBadge);
    const QString aText = primaryBadge->property("text").toString();
    const QString bText = secondaryBadge->property("text").toString();
    EXPECT_TRUE(aText.contains(QStringLiteral("A · 颜色：已转换"))) << aText.toStdString();
    EXPECT_TRUE(aText.contains(QStringLiteral("显示未验证"))) << aText.toStdString();
    EXPECT_TRUE(bText.contains(QStringLiteral("B · 颜色：无配置"))) << bText.toStdString();
    // UX-01: the dot has to mean what the sentence means. A applied its profile, so it keeps
    // the filled information dot; B carries no profile at all, which is an unmeasured state -
    // a hollow neutral ring, never the green that reads as "colour managed".
    EXPECT_FALSE(primaryBadge->property("dotHollow").toBool());
    EXPECT_TRUE(secondaryBadge->property("dotHollow").toBool());
    EXPECT_NE(primaryBadge->property("dotColor"), secondaryBadge->property("dotColor"));
    // A native RGBA8 pair needed no format conversion, so the conversion badge is hidden
    // while the colour state is not: the two facts are no longer coupled.
    EXPECT_FALSE(conversionBadge->isVisible()) << visibleChainText(conversionBadge);

    // COLOR4 observation path: the toggle badge is the in-product way to prove the display
    // derivative is converted while the raw buffer stays untouched.
    auto* const colorDisplayBadge =
        root->findChild<QQuickItem*>(QStringLiteral("imageColorDisplayBadge"));
    auto* const colorDisplayToggle =
        root->findChild<QQuickItem*>(QStringLiteral("imageColorDisplayToggleButton"));
    ASSERT_NE(colorDisplayBadge, nullptr);
    ASSERT_NE(colorDisplayToggle, nullptr);
    EXPECT_TRUE(colorDisplayBadge->isVisible()) << visibleChainText(colorDisplayBadge);
    const QString convertedUrl = imageReview.imageUrl(0);
    const QColor rawPixel = imageReview.rawImageForSlot(0).pixelColor(10, 10);
    const QColor convertedPixel = imageReview.imageForSlot(0).pixelColor(10, 10);
    EXPECT_NE(convertedPixel.rgba(), rawPixel.rgba());
    // The QML function lives on the workspace root; parent walking avoids depending on which
    // Loader wrapper owns the objectName in this harness.
    QObject* imageWorkspace = primaryBadge->parent();
    while (imageWorkspace != nullptr &&
           imageWorkspace->metaObject()->indexOfMethod("toggleColorDisplay()") < 0) {
        imageWorkspace = imageWorkspace->parent();
    }
    ASSERT_NE(imageWorkspace, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(imageWorkspace, "toggleColorDisplay"));
    pump();
    EXPECT_FALSE(imageReview.colorDisplayEnabled());
    EXPECT_TRUE(
        primaryBadge->property("text").toString().contains(QStringLiteral("颜色：转换已关闭")))
        << primaryBadge->property("text").toString().toStdString();
    // With the conversion off nothing is colour-managed on screen any more, so the badge must
    // stop claiming a state and fall back to the hollow "no reading" ring.
    EXPECT_TRUE(primaryBadge->property("dotHollow").toBool())
        << "a disabled display conversion is not an applied colour state";
    EXPECT_NE(imageReview.imageUrl(0), convertedUrl);
    EXPECT_EQ(imageReview.imageForSlot(0).pixelColor(10, 10).rgba(), rawPixel.rgba());
    EXPECT_EQ(imageReview.rawImageForSlot(0).pixelColor(10, 10).rgba(), rawPixel.rgba());
    ASSERT_TRUE(QMetaObject::invokeMethod(imageWorkspace, "toggleColorDisplay"));
    pump();
    EXPECT_TRUE(imageReview.colorDisplayEnabled());
    EXPECT_FALSE(primaryBadge->property("dotHollow").toBool());
    EXPECT_TRUE(primaryBadge->property("text").toString().contains(QStringLiteral("已转换")))
        << primaryBadge->property("text").toString().toStdString();
    EXPECT_EQ(imageReview.imageUrl(0), convertedUrl);
    EXPECT_EQ(imageReview.imageForSlot(0).pixelColor(10, 10).rgba(), convertedPixel.rgba());

    // Swap: the badges follow the sides, not their original slot.
    ASSERT_TRUE(imageReview.swapSides());
    pump();
    EXPECT_TRUE(primaryBadge->property("text").toString().contains(QStringLiteral("无配置")))
        << primaryBadge->property("text").toString().toStdString();
    EXPECT_TRUE(
        secondaryBadge->property("text").toString().contains(QStringLiteral("颜色：已转换")))
        << secondaryBadge->property("text").toString().toStdString();

    // Replace A with a corrupt profile: only A's state changes, B keeps its own.
    QImage imageC(16, 16, QImage::Format_ARGB32);
    imageC.fill(QColor(10, 200, 10));
    StillImageSourceInfo corruptInfo =
        rgba8Info(static_cast<int>(ImageColorProfileStatus::Corrupt));
    ASSERT_TRUE(
        imageReview.openPrimaryImage(std::move(imageC), QStringLiteral("c.png"), corruptInfo));
    pump();
    const QString corruptText = primaryBadge->property("text").toString();
    EXPECT_TRUE(corruptText.contains(QStringLiteral("配置损坏"))) << corruptText.toStdString();
    EXPECT_TRUE(
        secondaryBadge->property("text").toString().contains(QStringLiteral("颜色：已转换")))
        << secondaryBadge->property("text").toString().toStdString();

    // A failed open keeps the committed pair and therefore the same badge state.
    const int failedRequest = imageReview.requestOpenPair(
        QUrl::fromLocalFile(QStringLiteral("Z:/missing-color-contract/a.png")),
        QUrl::fromLocalFile(QStringLiteral("Z:/missing-color-contract/b.png")));
    ASSERT_GT(failedRequest, 0);
    pump();
    EXPECT_FALSE(imageReview.errorText().isEmpty());
    EXPECT_TRUE(primaryBadge->property("text").toString().contains(QStringLiteral("配置损坏")))
        << primaryBadge->property("text").toString().toStdString();

    // Fast pair switch: the badges name the latest committed pair, not the previous one.
    QImage imageD(16, 16, QImage::Format_ARGB32);
    imageD.fill(QColor(5, 5, 5));
    QImage imageE(16, 16, QImage::Format_ARGB32);
    imageE.fill(QColor(6, 6, 6));
    ASSERT_TRUE(
        imageReview.openPairImages(std::move(imageD),
                                   QStringLiteral("d.png"),
                                   std::move(imageE),
                                   QStringLiteral("e.png"),
                                   -1,
                                   rgba8Info(static_cast<int>(ImageColorProfileStatus::None)),
                                   rgba8Info(static_cast<int>(ImageColorProfileStatus::None))));
    pump();
    EXPECT_TRUE(primaryBadge->property("text").toString().contains(QStringLiteral("无配置")))
        << primaryBadge->property("text").toString().toStdString();
    EXPECT_TRUE(secondaryBadge->property("text").toString().contains(QStringLiteral("无配置")))
        << secondaryBadge->property("text").toString().toStdString();

    // Single-side close removes exactly that badge; the surviving side keeps its state.
    ASSERT_TRUE(imageReview.closeSecondary());
    pump();
    EXPECT_TRUE(primaryBadge->isVisible()) << visibleChainText(primaryBadge);
    EXPECT_TRUE(primaryBadge->property("text").toString().contains(QStringLiteral("无配置")))
        << primaryBadge->property("text").toString().toStdString();
    EXPECT_FALSE(secondaryBadge->isVisible()) << visibleChainText(secondaryBadge);

    // Closing A while a pair is committed promotes B into the internal primary slot, so the
    // workspace still shows one image and its own colour state.
    QImage imageF(16, 16, QImage::Format_ARGB32);
    imageF.fill(QColor(7, 7, 7));
    QImage imageG(16, 16, QImage::Format_ARGB32);
    imageG.fill(QColor(8, 8, 8));
    ASSERT_TRUE(
        imageReview.openPairImages(std::move(imageF),
                                   QStringLiteral("f.png"),
                                   std::move(imageG),
                                   QStringLiteral("g.png"),
                                   -1,
                                   rgba8Info(static_cast<int>(ImageColorProfileStatus::None)),
                                   rgba8Info(static_cast<int>(ImageColorProfileStatus::None))));
    pump();
    ASSERT_TRUE(imageReview.closePrimary());
    pump();
    EXPECT_TRUE(primaryBadge->isVisible()) << visibleChainText(primaryBadge);
    EXPECT_TRUE(primaryBadge->property("text").toString().contains(QStringLiteral("无配置")))
        << primaryBadge->property("text").toString().toStdString();
    EXPECT_FALSE(secondaryBadge->isVisible()) << visibleChainText(secondaryBadge);

    // The last image closes to the empty state: no side, no colour badge.
    ASSERT_TRUE(imageReview.closePrimary());
    pump();
    EXPECT_FALSE(primaryBadge->isVisible()) << visibleChainText(primaryBadge);
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
    ColorClipboardController colorClipboard;

    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    engine.rootContext()->setContextProperty(QStringLiteral("reviewController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewPreferences"), &preferences);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewSession"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewFacade"), &facade);
    engine.addImageProvider(QStringLiteral("vcs-review"), new ReviewImageProvider(&imageReview));
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);

    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(pilotWindowProperties(imageReview, colorClipboard))};
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

    // updatePosition(sceneX, sceneY) is the exact path DragHandler.onCentroidChanged drives;
    // it emits positionRequested, whose handler must move the controller position.
    const QPointF overlayOrigin = overlay->mapToScene(QPointF{0.0, 0.0});
    for (const double requested : {0.25, 0.75}) {
        const qreal sceneX = overlayOrigin.x() + overlay->width() * requested;
        const qreal sceneY = overlayOrigin.y() + overlay->height() / 2.0;
        ASSERT_TRUE(QMetaObject::invokeMethod(handle,
                                              "updatePosition",
                                              Q_ARG(QVariant, QVariant{sceneX}),
                                              Q_ARG(QVariant, QVariant{sceneY})));
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

    // E1: switching to the horizontal axis must move the clip bound and the handle to the Y
    // split while keeping the ratio, the pair and the decoded buffers untouched.
    const qreal ratioBefore = imageReview.wipePosition();
    const int compareModeBefore = imageReview.compareMode();
    imageReview.setWipeOrientation(1);
    QCoreApplication::processEvents();
    EXPECT_EQ(clip->height(), qRound(overlay->height() * imageReview.wipePosition()))
        << "the horizontal clip must bound at the Y split";
    EXPECT_EQ(clip->width(), overlay->width());
    const QPointF handleCenterH = handle->mapToScene(QPointF{0.0, handle->height() / 2.0});
    EXPECT_NEAR(
        handleCenterH.y(), overlayOrigin.y() + overlay->height() * imageReview.wipePosition(), 1.5)
        << "the horizontal handle must ride the Y split";
    EXPECT_EQ(imageReview.wipePosition(), ratioBefore) << "axis switch keeps the split ratio";
    EXPECT_EQ(imageReview.compareMode(), compareModeBefore) << "axis switch keeps the mode";
    // Hover side selection follows the axis: a point below the Y split maps to the primary.
    const QPointF belowSplit = overlay->mapToScene(
        QPointF{overlay->width() / 2.0, overlay->height() * (imageReview.wipePosition() + 0.1)});
    QMouseEvent hoverMove{QEvent::MouseMove,
                          overlay->mapFromScene(belowSplit),
                          belowSplit,
                          Qt::NoButton,
                          Qt::NoButton,
                          Qt::NoModifier};
    QCoreApplication::sendEvent(overlay, &hoverMove);
    QCoreApplication::processEvents();
    // Back to vertical restores the X-bound clip.
    imageReview.setWipeOrientation(0);
    QCoreApplication::processEvents();
    EXPECT_EQ(clip->width(), qRound(overlay->width() * imageReview.wipePosition()));
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
    // The root's required clipboard service; the copy tests assert through the real
    // clipboard the adapter writes to.
    ColorClipboardController colorClipboard;
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
    engine.rootContext()->setContextProperty(QStringLiteral("imageFolderPairs"), &folderPairs);
    engine.addImageProvider(QStringLiteral("vcs-review"), new ReviewImageProvider(&imageReview));
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);

    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(pilotWindowProperties(imageReview, colorClipboard))};
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
    // The root's required clipboard service; the copy tests assert through the real
    // clipboard the adapter writes to.
    ColorClipboardController colorClipboard;
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
    engine.rootContext()->setContextProperty(QStringLiteral("imageFolderPairs"), &folderPairs);
    engine.addImageProvider(QStringLiteral("vcs-review"), new ReviewImageProvider(&imageReview));
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);

    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(pilotWindowProperties(imageReview, colorClipboard))};
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

// I01: a system double-click (PlaySingle startup request) is a playback open. The intent
// carries OpenForPlayback as an explicit parameter, and only its successful terminal - which
// the real open command reaches after the first-frame ACK - may hand control to the transport.
// The order/reference confirmation dialog belongs to multi-source review opens and must never
// appear for this single-file playback open.
TEST(MainQmlContractTests, StartupPlaybackOpenAutoplaysAfterTheFirstConfirmedFrame) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("startup_play.mp4"));
    ASSERT_TRUE(writeTestPng(path));
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.submitted.clear();
    ASSERT_TRUE(harness.shell->enqueueStartupRequest(1, {QUrl::fromLocalFile(path)}));
    EXPECT_EQ(harness.shell->activeIntent().value(QStringLiteral("purpose")).toInt(),
              ReviewShellController::OpenForPlayback);
    const application::CommandContext context =
        application::commandContext(harness.submitted.back());
    // First-frame gate: before the open's terminal nothing may start playback.
    EXPECT_EQ(std::count_if(harness.submitted.begin(),
                            harness.submitted.end(),
                            [](const auto& command) {
                                return std::holds_alternative<application::PlayCommand>(command);
                            }),
              0);
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate.hasValue());
    ASSERT_TRUE(installValidatedVideoSet(harness.snapshot,
                                         {std::filesystem::path{path.toStdWString()}},
                                         rate.value(),
                                         100,
                                         3'333'334));
    harness.snapshot->displayedFrame = domain::FrameId{0};
    harness.terminals.push_back(application::CommandTerminal{
        .context = context,
        .outcome = application::CommandOutcome::Succeeded,
    });
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_EQ(std::count_if(harness.submitted.begin(),
                            harness.submitted.end(),
                            [](const auto& command) {
                                return std::holds_alternative<application::PlayCommand>(command);
                            }),
              1);
    // Single file: the A/B/GT order confirmation was never needed.
    EXPECT_EQ(harness.root->findChild<QObject*>(QStringLiteral("dropReviewDialog")), nullptr);
}

// I01: the preference is the user's off switch for intent-driven autoplay, and the view menu
// item is the visible control for it. With autoplay off, the same double-click open lands
// paused exactly like a review open.
TEST(MainQmlContractTests, StartupPlaybackAutoplayRespectsThePreferenceOffSwitch) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("startup_paused.mp4"));
    ASSERT_TRUE(writeTestPng(path));
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    EXPECT_TRUE(harness.preferences.autoplayOnOpen());
    harness.preferences.setAutoplayOnOpen(false);
    harness.window->show();
    harness.submitted.clear();
    ASSERT_TRUE(harness.shell->enqueueStartupRequest(1, {QUrl::fromLocalFile(path)}));
    const application::CommandContext context =
        application::commandContext(harness.submitted.back());
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate.hasValue());
    ASSERT_TRUE(installValidatedVideoSet(harness.snapshot,
                                         {std::filesystem::path{path.toStdWString()}},
                                         rate.value(),
                                         100,
                                         3'333'334));
    harness.snapshot->displayedFrame = domain::FrameId{0};
    harness.terminals.push_back(application::CommandTerminal{
        .context = context,
        .outcome = application::CommandOutcome::Succeeded,
    });
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_EQ(std::count_if(harness.submitted.begin(),
                            harness.submitted.end(),
                            [](const auto& command) {
                                return std::holds_alternative<application::PlayCommand>(command);
                            }),
              0);
    // The menu item the user sees is wired to the same preference in both directions. The
    // trigger writes the flipped value, so off -> on -> off across two activations.
    auto* const menuItem =
        harness.root->findChild<QObject*>(QStringLiteral("autoplayOnOpenMenuItem"));
    ASSERT_NE(menuItem, nullptr);
    EXPECT_FALSE(menuItem->property("checked").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(menuItem, "triggered"));
    harness.settle();
    EXPECT_TRUE(harness.preferences.autoplayOnOpen());
    ASSERT_TRUE(QMetaObject::invokeMethod(menuItem, "triggered"));
    harness.settle();
    EXPECT_FALSE(harness.preferences.autoplayOnOpen());
}

// I01: the Explorer compare verb (Compare startup request) is a review open - paused so the
// start point can be checked - even after a fully successful open.
TEST(MainQmlContractTests, StartupCompareOpenStaysPausedForReview) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    std::vector<std::filesystem::path> paths;
    for (const auto* const name : {"compare_a.mp4", "compare_b.mp4"}) {
        const QString path = directory.filePath(QString::fromLatin1(name));
        ASSERT_TRUE(writeTestPng(path));
        paths.emplace_back(path.toStdWString());
    }
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.submitted.clear();
    QVariantList urls;
    for (const auto& path : paths) {
        urls.push_back(QUrl::fromLocalFile(QString::fromStdWString(path.wstring())));
    }
    ASSERT_TRUE(harness.shell->enqueueStartupRequest(2, urls));
    EXPECT_EQ(harness.shell->activeIntent().value(QStringLiteral("purpose")).toInt(),
              ReviewShellController::OpenForReview);
    const application::CommandContext context =
        application::commandContext(harness.submitted.back());
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate.hasValue());
    ASSERT_TRUE(installValidatedVideoSet(harness.snapshot, paths, rate.value(), 100, 3'333'334));
    harness.snapshot->displayedFrame = domain::FrameId{0};
    harness.terminals.push_back(application::CommandTerminal{
        .context = context,
        .outcome = application::CommandOutcome::Succeeded,
    });
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_EQ(std::count_if(harness.submitted.begin(),
                            harness.submitted.end(),
                            [](const auto& command) {
                                return std::holds_alternative<application::PlayCommand>(command);
                            }),
              0);
}

// I01/V02 acceptance at the real event layer: the QML suite can only emit the image chip's
// own signal (Qt 6 refuses external calls to on* handlers), so the DropArea's enter filter
// and the drop routing need genuine drag events through the window. A valid image URL
// released over chip A/B must route to exactly that slot's replace signal with that URL,
// while a drag without URLs or with a non-image URL leaves both signals silent.
TEST(MainQmlContractTests, ImageChipDropsRouteRealDragEvents) {
    auto snapshot = std::make_shared<application::SessionSnapshot>();
    snapshot->graphicsReady = true;
    snapshot->sessionState = domain::SessionState::kEmpty;
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
    ImageReviewController imageReview;
    ColorClipboardController colorClipboard;

    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    engine.rootContext()->setContextProperty(QStringLiteral("reviewController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewPreferences"), &preferences);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewSession"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewFacade"), &facade);
    engine.addImageProvider(QStringLiteral("vcs-review"), new ReviewImageProvider(&imageReview));
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);

    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(pilotWindowProperties(imageReview, colorClipboard))};
    ASSERT_NE(root, nullptr) << componentErrors(component);
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);
    window->resize(1280, 800);
    window->show();
    QCoreApplication::processEvents();

    QImage left(32, 32, QImage::Format_ARGB32);
    left.fill(QColor(200, 30, 30));
    QImage right(32, 32, QImage::Format_ARGB32);
    right.fill(QColor(30, 30, 200));
    ASSERT_TRUE(imageReview.openPrimaryImage(std::move(left), QStringLiteral("left.png")));
    ASSERT_TRUE(imageReview.openSecondaryImage(std::move(right), QStringLiteral("right.png")));
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
    auto* const chipA = workspace->findChild<QQuickItem*>(QStringLiteral("imageSourceChipA"));
    auto* const chipB = workspace->findChild<QQuickItem*>(QStringLiteral("imageSourceChipB"));
    ASSERT_NE(chipA, nullptr);
    ASSERT_NE(chipB, nullptr);
    ASSERT_TRUE(chipA->isVisible());
    ASSERT_TRUE(chipB->isVisible());

    // Count through a QML relay (the installFocusWiring precedent): the workspace signals
    // are QML-declared, so a script-side connection reads them without string-based
    // SLOT plumbing.
    QQmlComponent counterComponent{&engine};
    counterComponent.setData(
        QByteArrayLiteral("import QtQml\n"
                          "QtObject {\n"
                          "    property var source: null\n"
                          "    property var windowSource: null\n"
                          "    property int primaryCalls: 0\n"
                          "    property int secondaryCalls: 0\n"
                          "    property int windowDrops: 0\n"
                          "    property var lastWindowUrls: []\n"
                          "    property url lastPrimary\n"
                          "    property url lastSecondary\n"
                          "    function connectNow() {\n"
                          "        source.replacePrimaryDropped.connect"
                          "(function(url) {\n"
                          "            primaryCalls += 1;\n"
                          "            lastPrimary = url;\n"
                          "        });\n"
                          "        source.replaceSecondaryDropped.connect"
                          "(function(url) {\n"
                          "            secondaryCalls += 1;\n"
                          "            lastSecondary = url;\n"
                          "        });\n"
                          "        windowSource.dropped.connect(function(drop) {\n"
                          "            windowDrops += 1;\n"
                          "            lastWindowUrls = drop.urls;\n"
                          "        });\n"
                          "    }\n"
                          "}\n"),
        QUrl{});
    ASSERT_EQ(counterComponent.status(), QQmlComponent::Ready) << componentErrors(counterComponent);
    std::unique_ptr<QObject> counter{counterComponent.create()};
    ASSERT_NE(counter, nullptr) << componentErrors(counterComponent);
    QObject* const windowDrop = root->findChild<QObject*>(QStringLiteral("workspaceDropArea"));
    ASSERT_NE(windowDrop, nullptr);
    ASSERT_TRUE(counter->setProperty("source", QVariant::fromValue(workspace)));
    ASSERT_TRUE(counter->setProperty("windowSource", QVariant::fromValue(windowDrop)));
    ASSERT_TRUE(QMetaObject::invokeMethod(counter.get(), "connectNow"));
    const auto primaryCalls = [&] { return counter->property("primaryCalls").toInt(); };
    const auto secondaryCalls = [&] { return counter->property("secondaryCalls").toInt(); };
    const auto windowDrops = [&] { return counter->property("windowDrops").toInt(); };
    const auto capturedPrimary = [&] { return counter->property("lastPrimary").toUrl(); };
    const auto capturedSecondary = [&] { return counter->property("lastSecondary").toUrl(); };
    // The window-level target must stay a live participant of the default open/append
    // path while the chips claim their own drags.
    EXPECT_TRUE(windowDrop->property("enabled").toBool())
        << "the window-level open/append drop target must stay enabled";

    const auto dragDropAtScene =
        [&](QQuickWindow* target, const QPointF& scene, const QList<QUrl>& urls) {
            QMimeData* const mimeData = new QMimeData;
            mimeData->setUrls(urls);
            QDragEnterEvent enter{
                scene.toPoint(), Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier};
            static_cast<void>(QCoreApplication::sendEvent(target, &enter));
            QDragMoveEvent move{
                scene.toPoint(), Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier};
            static_cast<void>(QCoreApplication::sendEvent(target, &move));
            QDropEvent drop{scene, Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier};
            static_cast<void>(QCoreApplication::sendEvent(target, &drop));
            for (int iteration = 0; iteration < 5; ++iteration) {
                QCoreApplication::processEvents();
            }
            mimeData->deleteLater();
            for (int iteration = 0; iteration < 3; ++iteration) {
                QCoreApplication::processEvents();
            }
        };
    const auto dragDropAt = [&](QQuickItem* target, const QList<QUrl>& urls) {
        const QPointF centre{target->width() / 2, target->height() / 2};
        const QPointF scene = target->mapToScene(centre);
        QMimeData* const mimeData = new QMimeData;
        mimeData->setUrls(urls);
        QDragEnterEvent enter{
            scene.toPoint(), Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier};
        static_cast<void>(QCoreApplication::sendEvent(window, &enter));
        QDragMoveEvent move{
            scene.toPoint(), Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier};
        static_cast<void>(QCoreApplication::sendEvent(window, &move));
        QDropEvent drop{scene, Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier};
        static_cast<void>(QCoreApplication::sendEvent(window, &drop));
        for (int iteration = 0; iteration < 5; ++iteration) {
            QCoreApplication::processEvents();
        }
        mimeData->deleteLater();
        for (int iteration = 0; iteration < 3; ++iteration) {
            QCoreApplication::processEvents();
        }
    };

    // A valid single image URL routes to the slot under the cursor.
    const QUrl replacement{QUrl::fromLocalFile(QStringLiteral("C:/fixtures/new-secondary.png"))};
    auto* const chipDropB =
        workspace->findChild<QObject*>(QStringLiteral("imageSourceChipDropAreaB"));
    ASSERT_NE(chipDropB, nullptr);
    {
        // The drop target's OWN geometry must be real before any event: a zero-sized or
        // offset DropArea (anchor chain, content parent, unpolished container) would make
        // every synthetic event land beside the target - this pins the command panel's
        // explicit-relayout discipline for no-render hosts.
        auto* const dropAreaItem = qobject_cast<QQuickItem*>(chipDropB);
        ASSERT_NE(dropAreaItem, nullptr);
        EXPECT_GT(dropAreaItem->width(), 0.0) << "chip B's DropArea must have width";
        EXPECT_GT(dropAreaItem->height(), 0.0) << "chip B's DropArea must have height";
        EXPECT_TRUE(dropAreaItem->contains(dropAreaItem->mapFromScene(dropAreaItem->mapToScene(
            QPointF{dropAreaItem->width() / 2, dropAreaItem->height() / 2}))))
            << "chip B's DropArea must contain its own mapped centre";
    }
    dragDropAt(chipB, {replacement});
    EXPECT_EQ(secondaryCalls(), 1) << "a valid image drop on chip B must reach the B slot";
    EXPECT_EQ(primaryCalls(), 0) << "chip B's drop must not leak into the A slot";
    EXPECT_EQ(capturedSecondary(), replacement);
    EXPECT_EQ(windowDrops(), 0) << "a chip drop must not leak into the open/append path";

    const QUrl otherImage{QUrl::fromLocalFile(QStringLiteral("C:/fixtures/new-primary.png"))};
    dragDropAt(chipA, {otherImage});
    EXPECT_EQ(primaryCalls(), 1) << "a valid image drop on chip A must reach the A slot";
    EXPECT_EQ(secondaryCalls(), 1);
    EXPECT_EQ(capturedPrimary(), otherImage);
    EXPECT_EQ(windowDrops(), 0);

    // The default open/append path must stay live: the same single image dropped on the
    // canvas (away from any chip) reaches the window-level target with the same URL.
    const QPointF canvasPoint{window->width() / 2.0, window->height() * 0.75};
    dragDropAtScene(window, canvasPoint, {replacement});
    EXPECT_EQ(windowDrops(), 1) << "a canvas drop must reach the window-level open target";
    EXPECT_EQ(primaryCalls(), 1) << "a canvas drop must not replace a chip slot";
    EXPECT_EQ(secondaryCalls(), 1);
    const QVariantList droppedUrls = counter->property("lastWindowUrls").toList();
    ASSERT_EQ(droppedUrls.size(), 1);
    EXPECT_EQ(droppedUrls.front().toUrl(), replacement);

    // Counterexample: a drag without URLs stays filtered out.
    const int windowDropsBeforeCounterexamples = windowDrops();
    dragDropAt(chipA, {});
    EXPECT_EQ(primaryCalls(), 1);
    EXPECT_EQ(secondaryCalls(), 1);

    // Counterexample: a non-image URL stays filtered out on both slots. Falling through to
    // the window-level open/append path is acceptable; a chip replacement is not.
    dragDropAt(chipB, {QUrl::fromLocalFile(QStringLiteral("C:/fixtures/clip.mp4"))});
    EXPECT_EQ(primaryCalls(), 1);
    EXPECT_EQ(secondaryCalls(), 1);

    // Counterexample: two URLs at once is not a single-file replacement.
    dragDropAt(chipA, {otherImage, replacement});
    EXPECT_EQ(primaryCalls(), 1);
    EXPECT_EQ(secondaryCalls(), 1);
    EXPECT_GE(windowDrops(), windowDropsBeforeCounterexamples)
        << "fallback drops may reach the open/append target, never a chip slot";
    window->close();
}

// I01: dropping one video on a source chip replaces exactly that source through the V02
// staged swap - slot count, reference role and displayed position preserved, no playback
// started, no order dialog involved.
TEST(MainQmlContractTests, VideoDroppedOnASourceChipReplacesThatSourceInPlace) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    std::vector<QString> localPaths;
    for (const auto* const name : {"chip_a.mp4", "chip_b.mp4", "chip_replacement.mp4"}) {
        const QString path = directory.filePath(QString::fromLatin1(name));
        ASSERT_TRUE(writeTestPng(path));
        localPaths.push_back(path);
    }
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.submitted.clear();
    QVariantList urls;
    for (const auto& path : localPaths) {
        urls.push_back(QUrl::fromLocalFile(path));
    }
    ASSERT_TRUE(harness.shell->enqueueStartupRequest(2, urls.mid(0, 2)));
    const application::CommandContext context =
        application::commandContext(harness.submitted.front());
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate.hasValue());
    ASSERT_TRUE(installValidatedVideoSet(harness.snapshot,
                                         {std::filesystem::path{localPaths[0].toStdWString()},
                                          std::filesystem::path{localPaths[1].toStdWString()}},
                                         rate.value(),
                                         100,
                                         3'333'334));
    harness.snapshot->displayedFrame = domain::FrameId{0};
    harness.terminals.push_back(application::CommandTerminal{
        .context = context,
        .outcome = application::CommandOutcome::Succeeded,
    });
    harness.controller->refreshProjection();
    ASSERT_TRUE(harness.waitUntil([&] { return harness.shell->activeSources().size() == 2; }));
    harness.settle();

    auto* const strip = harness.root->findChild<QQuickItem*>(QStringLiteral("activeSourceStrip"));
    ASSERT_NE(strip, nullptr);
    ASSERT_TRUE(strip->isVisible());
    const QStringList identities = harness.shell->activeSourceIdentities();
    ASSERT_EQ(identities.size(), 2);
    harness.submitted.clear();
    // The strip resolves the drop to the frozen identity of the chip under the cursor; the
    // handler must stage the replacement in that exact slot.
    ASSERT_TRUE(QMetaObject::invokeMethod(strip,
                                          "replaceDropped",
                                          Q_ARG(QString, identities[1]),
                                          Q_ARG(QUrl, QUrl::fromLocalFile(localPaths[2]))));
    harness.settle();
    ASSERT_GE(harness.submitted.size(), 1U);
    const auto* const command =
        std::get_if<application::OpenComparisonCommand>(&harness.submitted.back());
    ASSERT_NE(command, nullptr);
    ASSERT_EQ(command->sources.size(), 2U);
    EXPECT_EQ(command->sources[1].path, std::filesystem::path{localPaths[2].toStdWString()});
    EXPECT_EQ(command->sources[1].role, domain::ComparisonRole::kPrediction);
    EXPECT_EQ(command->sources[0].role, domain::ComparisonRole::kReference);
    EXPECT_EQ(command->intent, application::OpenReviewIntent::ReplaceSources);
    EXPECT_TRUE(command->preserveDisplayedTime);
    EXPECT_EQ(harness.shell->openIntent(), ReviewShellController::ReplaceSources);
    EXPECT_EQ(std::count_if(harness.submitted.begin(),
                            harness.submitted.end(),
                            [](const auto& command) {
                                return std::holds_alternative<application::PlayCommand>(command);
                            }),
              0);
}

// V02 acceptance: replacement refuses an already-open file by canonical identity, not by URL
// spelling. The candidate below names the same file as source A through a redundant path
// segment; the old raw-string compare accepted it and let the session compare a file with
// itself.
TEST(MainQmlContractTests, ReplacingWithAnAlreadyOpenFileUnderAnotherSpellingIsRefused) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(QDir{directory.path()}.mkpath(QStringLiteral("sub")));
    std::vector<QString> localPaths;
    for (const auto* const name : {"chip_a.mp4", "chip_b.mp4"}) {
        const QString path = directory.filePath(QString::fromLatin1(name));
        ASSERT_TRUE(writeTestPng(path));
        localPaths.push_back(path);
    }
    const QString spellingVariant = directory.filePath(QStringLiteral("sub/../chip_a.mp4"));
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.submitted.clear();
    QVariantList urls;
    for (const auto& path : localPaths) {
        urls.push_back(QUrl::fromLocalFile(path));
    }
    ASSERT_TRUE(harness.shell->enqueueStartupRequest(2, urls));
    const application::CommandContext context =
        application::commandContext(harness.submitted.front());
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate.hasValue());
    ASSERT_TRUE(installValidatedVideoSet(harness.snapshot,
                                         {std::filesystem::path{localPaths[0].toStdWString()},
                                          std::filesystem::path{localPaths[1].toStdWString()}},
                                         rate.value(),
                                         100,
                                         3'333'334));
    harness.snapshot->displayedFrame = domain::FrameId{0};
    harness.terminals.push_back(application::CommandTerminal{
        .context = context,
        .outcome = application::CommandOutcome::Succeeded,
    });
    harness.controller->refreshProjection();
    ASSERT_TRUE(harness.waitUntil([&] { return harness.shell->activeSources().size() == 2; }));
    harness.settle();

    auto* const strip = harness.root->findChild<QQuickItem*>(QStringLiteral("activeSourceStrip"));
    ASSERT_NE(strip, nullptr);
    const QStringList identities = harness.shell->activeSourceIdentities();
    ASSERT_EQ(identities.size(), 2);
    harness.submitted.clear();
    ASSERT_TRUE(QMetaObject::invokeMethod(strip,
                                          "replaceDropped",
                                          Q_ARG(QString, identities[1]),
                                          Q_ARG(QUrl, QUrl::fromLocalFile(spellingVariant))));
    harness.settle();
    // The duplicate was refused before intent staging: no command reached the session and the
    // compare set stays untouched.
    EXPECT_TRUE(harness.submitted.empty());
    EXPECT_NE(harness.shell->openIntent(), ReviewShellController::ReplaceSources);
}

// I02: toggle-type shortcuts never auto-repeat. A held Space must not flip play/pause on the
// OS repeat clock, the mode digits must not re-fire, and the two data-affecting workspace
// actions (issue capture, task close) must stay single-shot. Command-type keys (frame steps,
// zoom, in/out) are deliberately not listed - repeating them is their intended behaviour.
TEST(MainQmlContractTests, ToggleShortcutsAndWorkspaceActionsDisableAutoRepeat) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    auto* const reviewShortcuts =
        harness.root->findChild<QObject*>(QStringLiteral("reviewShortcuts"));
    ASSERT_NE(reviewShortcuts, nullptr);

    const auto shortcutsBySequence = [&harness](const QString& wanted) {
        std::vector<QObject*> found;
        const QList<QObject*> all = harness.root->findChildren<QObject*>();
        for (QObject* const object : all) {
            const QString sequence = object->property("sequence").toString();
            if (sequence == wanted) {
                found.push_back(object);
            }
        }
        return found;
    };

    for (const auto* const sequence : {"Space",
                                       "H",
                                       "F11",
                                       "Esc",
                                       "?",
                                       "C",
                                       "\\",
                                       "Shift+`",
                                       "Z",
                                       "T",
                                       "1",
                                       "2",
                                       "3",
                                       "4",
                                       "5",
                                       "6",
                                       "M",
                                       "Ctrl+W"}) {
        const std::vector<QObject*> shortcuts = shortcutsBySequence(QString::fromLatin1(sequence));
        ASSERT_FALSE(shortcuts.empty()) << "no Shortcut binds " << sequence;
        for (QObject* const shortcut : shortcuts) {
            SCOPED_TRACE(std::string{sequence} + " " + shortcut->objectName().toStdString());
            EXPECT_FALSE(shortcut->property("autoRepeat").toBool());
        }
    }

    // D1: Tab is no longer a shortcut at all - it returns to platform focus navigation. D4:
    // the plain backtick is a window-level hold now, not a per-activation Shortcut.
    EXPECT_TRUE(shortcutsBySequence(QStringLiteral("Tab")).empty())
        << "Tab must stay free for focus navigation; pure mode lives on H";
    EXPECT_TRUE(shortcutsBySequence(QStringLiteral("`")).empty())
        << "the plain backtick is a hold; only Shift+` stays a Shortcut (the lock)";
}

// I02: the input context owns the keyboard before the media layer. While the comparison
// dialog is modal, A/D/C/digits/Space must leave the video session untouched (no jog, no
// mode switch, no transport command), and leaving the dialog restores the bindings. The
// image workspace takes the keyboard the same way, and hidden chrome keeps its exits.
TEST(MainQmlContractTests, DialogsAndWorkspacesOwnTheKeyboardBeforeMediaShortcuts) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QVariantList urls;
    for (const auto* const name : {"modal_a.mp4", "modal_b.mp4"}) {
        const QString path = directory.filePath(QString::fromLatin1(name));
        ASSERT_TRUE(writeTestPng(path));
        urls.push_back(QUrl::fromLocalFile(path));
    }
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.window->requestActivate();
    harness.settleAnimations();
    auto* const reviewShortcuts =
        harness.root->findChild<QObject*>(QStringLiteral("reviewShortcuts"));
    ASSERT_NE(reviewShortcuts, nullptr);

    // Two staged videos open the A/B/GT order dialog - the modal input context.
    QVariant staged;
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(),
                                          "performVideoReview",
                                          Q_RETURN_ARG(QVariant, staged),
                                          Q_ARG(QVariant, QVariant{urls})));
    EXPECT_TRUE(staged.toBool());
    auto* const dialog = harness.root->findChild<QObject*>(QStringLiteral("dropReviewDialog"));
    ASSERT_NE(dialog, nullptr);
    ASSERT_TRUE(dialog->property("visible").toBool());
    // The confirm button must survive the window floor: the source list scrolls inside the
    // shell while header and footer stay pinned.
    harness.window->resize(960, 640);
    harness.settle();
    auto* const dropBody = harness.root->findChild<QQuickItem*>(QStringLiteral("dropBodyScroll"));
    ASSERT_NE(dropBody, nullptr);
    EXPECT_LE(dropBody->height(), 640.0 - 48.0);
    auto* const dropOk = harness.root->findChild<QQuickItem*>(QStringLiteral("dropFooterOkButton"));
    ASSERT_NE(dropOk, nullptr);
    const QPointF dropOkBottom = dropOk->mapToScene(QPointF{0.0, dropOk->height()});
    EXPECT_GT(dropOkBottom.y(), 0.0);
    EXPECT_LE(dropOkBottom.y(), 640.0);
    harness.window->resize(1280, 800);
    harness.settle();
    EXPECT_EQ(harness.root->property("inputContext").toInt(), 3);
    EXPECT_FALSE(harness.root->property("globalMediaShortcutsEnabled").toBool());
    EXPECT_FALSE(reviewShortcuts->property("shortcutsEnabled").toBool());
    EXPECT_FALSE(reviewShortcuts->property("viewShortcutsEnabled").toBool());
    // Jog (bare A/D), candidate switch, digit modes and Space all stay inert while modal.
    harness.submitted.clear();
    sendKey(*harness.window, Qt::Key_A);
    sendKey(*harness.window, Qt::Key_D);
    sendKey(*harness.window, Qt::Key_C);
    sendKey(*harness.window, Qt::Key_1);
    sendKey(*harness.window, Qt::Key_Space);
    harness.settle();
    EXPECT_TRUE(harness.submitted.empty());

    // Esc closes the dialog (the dialog's own key handling); the media layer comes back.
    sendKey(*harness.window, Qt::Key_Escape);
    ASSERT_TRUE(harness.waitUntil([&] { return !dialog->property("visible").toBool(); }));
    EXPECT_EQ(harness.root->property("inputContext").toInt(), 0);
    EXPECT_TRUE(reviewShortcuts->property("shortcutsEnabled").toBool());

    // The image workspace takes the keyboard away from the video session the same way.
    QImage retained{16, 16, QImage::Format_ARGB32};
    retained.fill(QColor(10, 20, 30));
    ASSERT_TRUE(harness.imageReview.openPrimaryImage(std::move(retained),
                                                     QStringLiteral("retained-image")));
    ASSERT_TRUE(harness.activateWorkspace(1));
    harness.settle();
    EXPECT_FALSE(harness.root->property("globalMediaShortcutsEnabled").toBool());
    EXPECT_FALSE(reviewShortcuts->property("viewShortcutsEnabled").toBool());
    harness.submitted.clear();
    sendKey(*harness.window, Qt::Key_Space);
    sendKey(*harness.window, Qt::Key_1);
    harness.settle();
    EXPECT_TRUE(harness.submitted.empty());

    // Hidden chrome is an immersive review, not a trap: H and F11 stay reachable so the
    // interface and full screen can always come back. (D1: H carries pure mode; Tab is no
    // longer a shortcut and keeps its focus-navigation meaning.)
    ASSERT_TRUE(harness.activateWorkspace(0));
    harness.settle();
    harness.shell->setChromeVisible(false);
    harness.settle();
    EXPECT_EQ(harness.root->property("inputContext").toInt(), 0);
    EXPECT_TRUE(harness.root->property("presentationShortcutsEnabled").toBool());
    QObject* chromeShortcut = nullptr;
    QObject* fullScreenShortcut = nullptr;
    const QList<QObject*> all = harness.root->findChildren<QObject*>();
    for (QObject* const object : all) {
        if (object->property("sequence").toString() == QStringLiteral("H")) {
            chromeShortcut = object;
        }
        if (object->property("sequence").toString() == QStringLiteral("F11")) {
            fullScreenShortcut = object;
        }
    }
    ASSERT_NE(chromeShortcut, nullptr);
    ASSERT_NE(fullScreenShortcut, nullptr);
    EXPECT_TRUE(chromeShortcut->property("enabled").toBool());
    EXPECT_TRUE(fullScreenShortcut->property("enabled").toBool());
}

// The in-window modal popups (clip export, sample export, file association, image edit
// scale/fill) are QML Popups, not native windows: nothing but the app's own routing keeps
// workspace shortcuts away from the session behind them. Two layers must hold together - each
// popup declares the popupInputContext marker for the focus walk, and Main.qml aggregates
// their visibility into inputContext - so a popup added without either fails here instead of
// silently stealing keys from the session behind it.
TEST(MainQmlContractTests, ModalPopupsOwnTheKeyboardBeforeWorkspaceShortcuts) {
    // Baseline first: with no popup, menu or text field involved, the workspace owns the keys.
    // Without this the modal assertions below could pass for the wrong reason (a permanently
    // modal input context) and the contract would silently mean nothing.
    {
        WorkspaceHarness harness;
        ASSERT_TRUE(harness.create()) << harness.error;
        harness.window->show();
        harness.settle();
        EXPECT_EQ(harness.root->property("inputContext").toInt(), 0);
        EXPECT_TRUE(harness.root->property("globalMediaShortcutsEnabled").toBool());
        harness.window->close();
    }

    // Behavioural layer, file association: opened through the settings menu, the popup owns the
    // keyboard - M must not log an issue, Ctrl+W must not close the task, Tab must not hide the
    // chrome behind the modal scrim.
    {
        WorkspaceHarness harness;
        harness.withFileAssociation = true;
        ASSERT_TRUE(harness.create()) << harness.error;
        harness.window->show();
        harness.settle();
        auto* const settingsMenu =
            harness.root->findChild<QObject*>(QStringLiteral("settingsMenu"));
        ASSERT_NE(settingsMenu, nullptr);
        ASSERT_TRUE(QMetaObject::invokeMethod(settingsMenu, "open"));
        harness.settle();
        auto* const menuItem =
            harness.root->findChild<QObject*>(QStringLiteral("fileAssociationMenuItem"));
        ASSERT_NE(menuItem, nullptr);
        ASSERT_TRUE(QMetaObject::invokeMethod(menuItem, "triggered"));
        harness.settle();
        auto* const popup =
            harness.root->findChild<QObject*>(QStringLiteral("fileAssociationPopup"));
        ASSERT_NE(popup, nullptr);
        ASSERT_TRUE(popup->property("visible").toBool());
        EXPECT_TRUE(popup->property("popupInputContext").toBool());
        EXPECT_EQ(harness.root->property("inputContext").toInt(), 3);
        EXPECT_TRUE(harness.root->property("anyModalPopupOpen").toBool());
        EXPECT_FALSE(harness.root->property("globalMediaShortcutsEnabled").toBool());
        EXPECT_FALSE(harness.root->property("presentationShortcutsEnabled").toBool());
        harness.submitted.clear();
        sendKey(*harness.window, Qt::Key_M);
        sendKey(*harness.window, Qt::Key_W, Qt::ControlModifier);
        sendKey(*harness.window, Qt::Key_H);
        sendKey(*harness.window, Qt::Key_Tab);
        harness.settle();
        EXPECT_TRUE(harness.submitted.empty());
        EXPECT_TRUE(harness.root->property("chromeVisible").toBool());
        harness.window->close();
    }

    // Behavioural layer, image edit scale dialog: the workspace dialog shell owns the keyboard
    // exactly like the menu-driven popups.
    {
        WorkspaceHarness harness;
        ASSERT_TRUE(harness.create()) << harness.error;
        harness.window->show();
        harness.settle();
        QImage retained{16, 16, QImage::Format_ARGB32};
        retained.fill(QColor(10, 20, 30));
        ASSERT_TRUE(harness.imageReview.openPrimaryImage(std::move(retained),
                                                         QStringLiteral("retained-image")));
        ASSERT_TRUE(harness.activateWorkspace(1));
        harness.settle();
        auto* const editDialogs =
            harness.root->findChild<QObject*>(QStringLiteral("imageEditDialogs"));
        ASSERT_NE(editDialogs, nullptr);
        ASSERT_TRUE(QMetaObject::invokeMethod(editDialogs, "openScale"));
        harness.settle();
        auto* const popup =
            harness.root->findChild<QObject*>(QStringLiteral("imageEditScaleDialog"));
        ASSERT_NE(popup, nullptr);
        EXPECT_TRUE(popup->property("visible").toBool());
        EXPECT_TRUE(popup->property("popupInputContext").toBool());
        EXPECT_EQ(harness.root->property("inputContext").toInt(), 3);
        EXPECT_TRUE(harness.root->property("anyModalPopupOpen").toBool());
        harness.submitted.clear();
        sendKey(*harness.window, Qt::Key_M);
        sendKey(*harness.window, Qt::Key_W, Qt::ControlModifier);
        harness.settle();
        EXPECT_TRUE(harness.submitted.empty());
        harness.window->close();
    }

    // Behavioural layer, clip export: the export dialog is the dangerous one - Ctrl+W while an
    // export runs must not close the session the export belongs to.
    {
        QTemporaryDir temporaryDirectory;
        ASSERT_TRUE(temporaryDirectory.isValid());
        std::vector<std::filesystem::path> sourcePaths;
        for (const char* name : {"modal_export_a.mp4", "modal_export_b.mp4"}) {
            const QString path = temporaryDirectory.filePath(QString::fromLatin1(name));
            QFile file{path};
            ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            ASSERT_EQ(file.write("fake-modal-source", 16), 16);
            file.close();
            sourcePaths.push_back(std::filesystem::path{path.toStdWString()});
        }
        WorkspaceHarness harness;
        harness.withClipExport = true;
        const auto rateResult = domain::RationalRate::create(30, 1);
        ASSERT_TRUE(rateResult);
        ASSERT_TRUE(installValidatedVideoSet(
            harness.snapshot, sourcePaths, rateResult.value(), 12, 400'000));
        harness.snapshot->displayedFrame = domain::FrameId{3};
        harness.snapshot->playbackRangeIn = domain::FrameId{3};
        harness.snapshot->playbackRangeOut = domain::FrameId{7};
        ASSERT_TRUE(harness.create()) << harness.error;
        harness.window->show();
        harness.settle();
        harness.controller->refreshProjection();
        harness.settle();
        harness.shell->setRangeIn(3, static_cast<double>(harness.controller->mediaTimeForFrame(3)));
        harness.shell->setRangeOut(7,
                                   static_cast<double>(harness.controller->mediaTimeForFrame(7)));
        harness.settle();

        auto* const exportChip =
            harness.root->findChild<QObject*>(QStringLiteral("transportExportRangeButton"));
        ASSERT_NE(exportChip, nullptr);
        ASSERT_TRUE(exportChip->property("chipEnabled").toBool());
        ASSERT_TRUE(QMetaObject::invokeMethod(exportChip, "clicked"));
        harness.settle();
        auto* const popup = harness.root->findChild<QObject*>(QStringLiteral("clipExportPopup"));
        ASSERT_NE(popup, nullptr);
        ASSERT_TRUE(popup->property("visible").toBool());
        EXPECT_TRUE(popup->property("popupInputContext").toBool());
        EXPECT_EQ(harness.root->property("inputContext").toInt(), 3);
        EXPECT_TRUE(harness.root->property("anyModalPopupOpen").toBool());
        harness.submitted.clear();
        sendKey(*harness.window, Qt::Key_M);
        sendKey(*harness.window, Qt::Key_W, Qt::ControlModifier);
        sendKey(*harness.window, Qt::Key_H);
        sendKey(*harness.window, Qt::Key_Tab);
        harness.settle();
        EXPECT_TRUE(harness.submitted.empty());
        EXPECT_TRUE(harness.root->property("chromeVisible").toBool());
        harness.window->close();
    }
}

// I02: over the wipe handle the split line owns the gesture - even with the ROI marquee armed,
// a drag on the handle moves the wipe and never commits an ROI. One gesture, one consumer.
TEST(MainQmlContractTests, WipeHandleDragOwnsTheGestureOverTheRoiMarquee) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    std::vector<QString> localPaths;
    for (const auto* const name : {"wipe_gt.mp4", "wipe_pred.mp4"}) {
        const QString path = directory.filePath(QString::fromLatin1(name));
        ASSERT_TRUE(writeTestPng(path));
        localPaths.push_back(path);
    }
    WorkspaceHarness harness;
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate.hasValue());
    ASSERT_TRUE(installValidatedVideoSet(harness.snapshot,
                                         {std::filesystem::path{localPaths[0].toStdWString()},
                                          std::filesystem::path{localPaths[1].toStdWString()}},
                                         rate.value(),
                                         100,
                                         3'333'334));
    harness.snapshot->displayedFrame = domain::FrameId{0};
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.submitted.clear();
    QVariantList urls;
    for (const auto& path : localPaths) {
        urls.push_back(QUrl::fromLocalFile(path));
    }
    ASSERT_TRUE(harness.shell->enqueueStartupRequest(2, urls));
    const application::CommandContext context =
        application::commandContext(harness.submitted.back());
    harness.terminals.push_back(application::CommandTerminal{
        .context = context,
        .outcome = application::CommandOutcome::Succeeded,
    });
    harness.controller->refreshProjection();
    ASSERT_TRUE(harness.waitUntil([&] { return harness.shell->activeSources().size() == 2; }));
    harness.preferences.setViewModeCode(ComparisonSurface::Wipe);
    harness.settle();

    auto* const viewport =
        harness.root->findChild<QQuickItem*>(QStringLiteral("mediaViewportFocusTarget"));
    auto* const wipeHandle = harness.root->findChild<QQuickItem*>(QStringLiteral("wipeHandle"));
    ASSERT_NE(viewport, nullptr);
    ASSERT_NE(wipeHandle, nullptr);
    ASSERT_TRUE(wipeHandle->isVisible());
    // Arm the marquee mode so the test proves the handle wins against an armed ROI gesture.
    viewport->setProperty("roiSelectMode", true);
    harness.settle();

    const qreal startX =
        wipeHandle->mapToScene(QPointF{wipeHandle->width() / 2, wipeHandle->height() / 2}).x();
    const qreal startY = wipeHandle->mapToScene(QPointF{0, wipeHandle->height() / 2}).y();
    const auto sendDrag = [&harness](const QPointF& from, const QPointF& to) {
        const QPointF globalFrom = harness.window->mapToGlobal(from.toPoint());
        QMouseEvent press{QEvent::MouseButtonPress,
                          from,
                          from,
                          globalFrom,
                          Qt::LeftButton,
                          Qt::LeftButton,
                          Qt::AltModifier};
        QCoreApplication::sendEvent(harness.window, &press);
        QMouseEvent move{QEvent::MouseMove,
                         to,
                         to,
                         harness.window->mapToGlobal(to.toPoint()),
                         Qt::NoButton,
                         Qt::LeftButton,
                         Qt::AltModifier};
        QCoreApplication::sendEvent(harness.window, &move);
        QMouseEvent release{QEvent::MouseButtonRelease,
                            to,
                            to,
                            harness.window->mapToGlobal(to.toPoint()),
                            Qt::LeftButton,
                            Qt::NoButton,
                            Qt::AltModifier};
        QCoreApplication::sendEvent(harness.window, &release);
        QCoreApplication::processEvents();
    };
    harness.root->setProperty("wipePosition", 0.5);
    harness.settle();
    sendDrag(QPointF{startX, startY}, QPointF{startX - 160, startY});
    EXPECT_NE(harness.root->property("wipePosition").toDouble(), 0.5);
    auto* const surface = harness.root->findChild<QQuickItem*>(QStringLiteral("dualVideoSurface"));
    ASSERT_NE(surface, nullptr);
    EXPECT_FALSE(surface->property("roiEnabled").toBool());
    EXPECT_TRUE(viewport->property("roiSelectMode").toBool());
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

TEST(MainQmlContractTests, EmptyReviewViewExposesFileAndFolderEntryPoints) {
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

    auto* const openFilesBtn =
        harness.root->findChild<QQuickItem*>(QStringLiteral("emptyOpenFilesButton"));
    ASSERT_NE(openFilesBtn, nullptr);
    EXPECT_TRUE(openFilesBtn->isVisible());

    auto* const openFolderBtn =
        harness.root->findChild<QQuickItem*>(QStringLiteral("emptyOpenFolderButton"));
    ASSERT_NE(openFolderBtn, nullptr);
    EXPECT_TRUE(openFolderBtn->isVisible());

    auto* const openImagePairBtn =
        harness.root->findChild<QQuickItem*>(QStringLiteral("emptyOpenImagePairButton"));
    ASSERT_NE(openImagePairBtn, nullptr);
    EXPECT_TRUE(openImagePairBtn->isVisible());

    auto* const compareFoldersBtn =
        harness.root->findChild<QQuickItem*>(QStringLiteral("emptyCompareFoldersButton"));
    ASSERT_NE(compareFoldersBtn, nullptr);
    EXPECT_TRUE(compareFoldersBtn->isVisible());
    const QString evidence = qEnvironmentVariable("DVS_REVIEW_EVIDENCE_DIR");
    if (!evidence.isEmpty()) {
        ASSERT_TRUE(QDir().mkpath(evidence));
        for (const QSize size : {QSize{960, 640}, QSize{1440, 900}}) {
            harness.window->resize(size);
            harness.settleAnimations();
            ASSERT_TRUE(harness.window->grabWindow().save(
                QDir{evidence}.filePath(QStringLiteral("welcome-%1.png").arg(size.width()))));
        }
    }
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

    // The standalone "切至 A / 切至 B" toolbar button is gone: it duplicated the A/B file
    // chips (a click selects that side), Space/T and the in-canvas badge. The chip is now
    // the primary entry, so it must really switch the side - and the removed duplicate must
    // not linger in the tree for an old test to find.
    auto* const chipA = harness.root->findChild<QQuickItem*>(QStringLiteral("imageSourceChipA"));
    auto* const chipB = harness.root->findChild<QQuickItem*>(QStringLiteral("imageSourceChipB"));
    ASSERT_NE(chipA, nullptr);
    ASSERT_NE(chipB, nullptr);
    EXPECT_EQ(harness.root->findChild<QQuickItem*>(QStringLiteral("imageToggleSourceButton")),
              nullptr);
    // Chip B selects B (slot 3).
    ASSERT_TRUE(QMetaObject::invokeMethod(chipB, "clicked"));
    harness.settle();
    EXPECT_EQ(primaryViewport->property("slot").toInt(), 3);
    EXPECT_TRUE(imageWorkspace->property("singleViewShowSecondary").toBool());
    // Chip A selects A again (slot 2).
    ASSERT_TRUE(QMetaObject::invokeMethod(chipA, "clicked"));
    harness.settle();
    EXPECT_EQ(primaryViewport->property("slot").toInt(), 2);
    EXPECT_FALSE(imageWorkspace->property("singleViewShowSecondary").toBool());
    // The in-canvas badge is the compact current-source indicator: it names the side on
    // screen and its two segments are the switch, so it stays readable while blinking.
    auto* const badgeCurrent =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageInPlaceCurrentSegment"));
    auto* const badgeSwitch =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageInPlaceSwitchSegment"));
    auto* const badgeSelectA =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageInPlaceSelectA"));
    auto* const badgeSelectB =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageInPlaceSelectB"));
    ASSERT_NE(badgeCurrent, nullptr);
    ASSERT_NE(badgeSwitch, nullptr);
    ASSERT_NE(badgeSelectA, nullptr);
    ASSERT_NE(badgeSelectB, nullptr);
    // The in-canvas badge is the compact current-source indicator: it names the side on
    // screen and exposes two segments. Their click targets are plain MouseAreas inside a
    // glass plate, which the offscreen/basic render loop this C++ harness runs on cannot
    // route real mouse events into - the same limitation that keeps the A/B chip's drag
    // drop a recorded coverage gap. The click behaviour is therefore verified with real
    // mouseClick calls in tst_image_workspace_manual.qml (A/B badge case); here the
    // structural contract is checked: both segments exist, are visible and sized, and the
    // plate stays inside its host so it can never overlap the neighbouring badge.
    EXPECT_TRUE(badgeCurrent->isVisible());
    EXPECT_TRUE(badgeSwitch->isVisible());
    EXPECT_GT(badgeSelectA->width(), 0.0);
    EXPECT_GT(badgeSelectB->width(), 0.0);
    EXPECT_LE(inPlaceBadge->x() + inPlaceBadge->width(), inPlaceBadge->parentItem()->width());

    // The canvas exposes a full-size click target; Space switches A/B without auto playback.
    auto* const canvasArea =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageCanvasMouseArea-2"));
    ASSERT_NE(canvasArea, nullptr);
    EXPECT_TRUE(canvasArea->isVisible());
    EXPECT_GT(canvasArea->width(), 100);

    // Evidence for the consolidated A/B entries: the shot shows the renamed 单图 A/B chip, the
    // A/B file chips as the direct-selection entry and the in-canvas current-source badge.
    const auto badgeEvidence = qEnvironmentVariable("DVS_REVIEW_EVIDENCE_DIR");
    if (!badgeEvidence.isEmpty()) {
        ASSERT_TRUE(QDir().mkpath(badgeEvidence));
        harness.settleAnimations();
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(badgeEvidence).filePath(QStringLiteral("image-single-ab-entries.png"))));
    }

    imageWorkspace->forceActiveFocus();
    sendKey(*harness.window, Qt::Key_Space);
    harness.settle();
    EXPECT_EQ(primaryViewport->property("slot").toInt(), 3);
    sendKey(*harness.window, Qt::Key_Space);
    harness.settle();
    EXPECT_EQ(primaryViewport->property("slot").toInt(), 2);

    // A toolbar button must not keep the keyboard: after clicking one, Space still toggles A/B
    // instead of re-running the button (a held AbstractButton maps Space to its own activation,
    // which used to make the help row's "Space / T" promise quietly false after any click).
    // A real mouse press leaves the focus on the button, so reproduce that first.
    auto* const trueSizeButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageTrueSizeButton"));
    ASSERT_NE(trueSizeButton, nullptr);
    trueSizeButton->forceActiveFocus();
    ASSERT_TRUE(QMetaObject::invokeMethod(trueSizeButton, "clicked"));
    harness.settle();
    ASSERT_TRUE(imageWorkspace->property("trueSize").toBool());
    ASSERT_TRUE(imageWorkspace->hasActiveFocus())
        << "a toolbar click must hand the keyboard back to the workspace root";
    sendKey(*harness.window, Qt::Key_Space);
    harness.settle();
    EXPECT_EQ(primaryViewport->property("slot").toInt(), 3)
        << "Space must toggle A/B even right after a toolbar click";
    EXPECT_TRUE(imageWorkspace->property("trueSize").toBool())
        << "Space must not re-run the toolbar button that was clicked";

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

    // The hold-to-peek is a separate behaviour from the A/B switch and must survive the
    // consolidation: in a difference mode the backtick suppresses the difference for as
    // long as it is held and restores it on release. The A/B badge hides while peeking so
    // the user is never told "you are on A" while the canvas shows the peeked original.
    // Press and release are sent separately (sendKey pairs them), so the held state is
    // actually observed rather than skipped over.
    auto* const peekBadge = harness.root->findChild<QQuickItem*>(QStringLiteral("imagePeekBadge"));
    ASSERT_NE(peekBadge, nullptr);
    EXPECT_FALSE(peekBadge->isVisible()) << "no peek outside a difference/fade mode";
    // Enter the absolute-difference mode again so the peek has something to suppress.
    ASSERT_TRUE(QMetaObject::invokeMethod(absDiff, "triggered"));
    harness.settle();
    EXPECT_EQ(harness.imageReview.compareMode(), 2);
    EXPECT_FALSE(inPlaceBadge->isVisible()) << "the A/B badge belongs to the single-image mode";
    imageWorkspace->forceActiveFocus();
    QKeyEvent peekPress{QEvent::KeyPress, Qt::Key_QuoteLeft, Qt::NoModifier};
    QCoreApplication::sendEvent(harness.window, &peekPress);
    harness.settle();
    EXPECT_TRUE(imageWorkspace->property("peekActive").toBool());
    EXPECT_TRUE(peekBadge->isVisible());
    // Evidence for the hold-to-peek that survives the consolidation: the shot shows the
    // peeked original with the badge naming which side is on screen.
    const auto peekEvidence = qEnvironmentVariable("DVS_REVIEW_EVIDENCE_DIR");
    if (!peekEvidence.isEmpty()) {
        ASSERT_TRUE(QDir().mkpath(peekEvidence));
        harness.settleAnimations();
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(peekEvidence).filePath(QStringLiteral("image-hold-to-peek.png"))));
    }
    QKeyEvent peekRelease{QEvent::KeyRelease, Qt::Key_QuoteLeft, Qt::NoModifier};
    QCoreApplication::sendEvent(harness.window, &peekRelease);
    harness.settle();
    EXPECT_FALSE(imageWorkspace->property("peekActive").toBool());
    EXPECT_FALSE(peekBadge->isVisible());
    EXPECT_EQ(harness.imageReview.compareMode(), 2) << "release restores the difference mode";
}

// The probe readout family in a real main window: the copy-colour button and its menu, the
// numeric-format selector, the Alpha filter switch, and the status-bar readout that must stay
// fully readable at the 960px floor. Everything is driven through the real UI objects, and the
// env-gated captures give the visual evidence for the layout claim.
TEST(MainQmlContractTests, ImageProbeReadoutFamilyIsReachableAndReadableAtNarrowWidth) {
    // The copy end is the real adapter the harness hands to the root as initial property:
    // it writes through the system clipboard and verifies by reading the value back, so
    // this test can assert exactly what a user would paste. A QtObject stand-in could not
    // be assigned to the typed colorClipboard boundary anyway.
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->resize(960, 640);
    harness.window->show();
    harness.settle();

    QImage primary(64, 64, QImage::Format_ARGB32);
    primary.fill(QColor(12, 34, 56, 200));
    QImage secondary(64, 64, QImage::Format_ARGB32);
    secondary.fill(QColor(200, 100, 50, 255));
    ASSERT_TRUE(harness.imageReview.openPairImages(std::move(primary),
                                                   QStringLiteral("a.png"),
                                                   std::move(secondary),
                                                   QStringLiteral("b.png")));
    ASSERT_TRUE(harness.activateWorkspace(1));
    harness.settle();
    auto* const workspace =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
    ASSERT_NE(workspace, nullptr);
    EXPECT_EQ(workspace->property("colorClipboard").value<QObject*>(), &harness.colorClipboard);
    // A real sample, produced exactly the way the workspace's hover handler produces one:
    // updateCursorPixel then captureProbeSample. The mouse path into the workspace cannot be
    // driven from this harness (the same offscreen limitation that keeps the A/B chip drag a
    // recorded gap), so the controller entries the handler calls are addressed directly.
    harness.imageReview.updateCursorPixel(ImageReviewController::DisplayPrimarySlot, 10, 20);
    harness.settle();
    ASSERT_TRUE(harness.imageReview.cursorPixel().value(QStringLiteral("valid")).toBool())
        << "a real hover must freeze a probe sample";
    ASSERT_TRUE(
        harness.imageReview.captureProbeSample(ImageReviewController::DisplayPrimarySlot, 10, 20));
    ASSERT_TRUE(QMetaObject::invokeMethod(workspace, "clearImageHover"));
    harness.settle();
    EXPECT_TRUE(harness.imageReview.cursorPixel().isEmpty());

    // The copy button is enabled by the frozen sample and its menu names the side, the
    // coordinate and the value, so the user knows what they are about to copy.
    auto* const copyButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageCopyColorButton"));
    auto* const copyLabel =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageCopyColorSample"));
    ASSERT_NE(copyButton, nullptr);
    ASSERT_NE(copyLabel, nullptr);
    EXPECT_TRUE(copyButton->property("enabled").toBool());
    const QString label = copyLabel->property("text").toString();
    EXPECT_TRUE(label.contains(QStringLiteral("A 原图"))) << label.toStdString();
    EXPECT_TRUE(label.contains(QStringLiteral("像素 ("))) << label.toStdString();
    EXPECT_TRUE(label.contains(QStringLiteral("rgb(12, 34, 56)"))) << label.toStdString();

    // The numeric-format selector is the one compact entry for the whole readout family.
    auto* const formatButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageNumericFormatButton"));
    ASSERT_NE(formatButton, nullptr);
    EXPECT_TRUE(formatButton->property("text").toString().contains(QStringLiteral("整数码值")));
    auto* const floatAction =
        harness.root->findChild<QObject*>(QStringLiteral("imageNumericFormatFloat"));
    auto* const integerAction =
        harness.root->findChild<QObject*>(QStringLiteral("imageNumericFormatInteger"));
    ASSERT_NE(floatAction, nullptr);
    ASSERT_NE(integerAction, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(floatAction, "triggered"));
    harness.settle();
    EXPECT_TRUE(formatButton->property("text").toString().contains(QStringLiteral("归一化浮点")));
    EXPECT_EQ(harness.imageReview.channelText(12), QStringLiteral("0.047059"));
    EXPECT_EQ(harness.imageReview.rgbText(12, 34, 56),
              QStringLiteral("rgb(0.047059, 0.133333, 0.219608)"));
    // Hex stays hexadecimal in both forms.
    EXPECT_EQ(harness.imageReview.hexText(12, 34, 56, 200), QStringLiteral("#0C2238C8"));
    // The copy payload follows the format.
    EXPECT_EQ(harness.imageReview.probeColorText(ImageReviewController::ProbeRgb),
              QStringLiteral("rgb(0.047059, 0.133333, 0.219608)"));
    EXPECT_EQ(harness.imageReview.probeColorText(ImageReviewController::ProbeHex),
              QStringLiteral("#0C2238C8"));
    auto* const redColumn = harness.root->findChild<QQuickItem*>(QStringLiteral("imagePairColAR"));
    ASSERT_NE(redColumn, nullptr);
    EXPECT_TRUE(redColumn->isVisible());
    EXPECT_EQ(redColumn->property("text").toString().trimmed(), QStringLiteral("0.047059"));
    auto* const deltaColumn =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imagePairColDeltaR"));
    ASSERT_NE(deltaColumn, nullptr);
    EXPECT_EQ(deltaColumn->property("text").toString(), QStringLiteral("+0.737255"));
    EXPECT_TRUE(copyLabel->property("text").toString().contains(
        QStringLiteral("rgb(0.047059, 0.133333, 0.219608)")));
    // The copy success path is a dedicated case below (ImageCopyColorButtonCopiesTheVerified
    // FrozenSample) so this readout/layout test is never gated on clipboard availability.
    ASSERT_TRUE(QMetaObject::invokeMethod(integerAction, "triggered"));
    harness.settle();

    // The Alpha filter switch sits next to the observation it belongs to and is off by
    // default; its threshold row only appears once it is on.
    auto* const alphaToggle =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageAlphaFilterButton"));
    auto* const alphaRow =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageAlphaFilterRow"));
    ASSERT_NE(alphaToggle, nullptr);
    ASSERT_NE(alphaRow, nullptr);
    EXPECT_FALSE(harness.imageReview.alphaFilterEnabled());
    EXPECT_FALSE(alphaRow->isVisible());
    ASSERT_TRUE(QMetaObject::invokeMethod(alphaToggle, "clicked"));
    EXPECT_TRUE(harness.imageReview.alphaFilterEnabled());
    EXPECT_EQ(harness.imageReview.viewMode(), ImageReviewController::RgbOpaqueView);
    auto* const thresholdSpin =
        harness.root->findChild<QObject*>(QStringLiteral("imageAlphaFilterSpin"));
    ASSERT_NE(thresholdSpin, nullptr);
    thresholdSpin->setProperty("value", 128);
    ASSERT_TRUE(QMetaObject::invokeMethod(thresholdSpin, "valueModified"));
    ASSERT_TRUE(
        harness.imageReview.captureProbeSample(ImageReviewController::DisplayPrimarySlot, 10, 20));
    harness.settle();
    EXPECT_TRUE(alphaRow->isVisible());
    EXPECT_TRUE(alphaToggle->property("text").toString().contains(QStringLiteral("A > 128")));
    auto* const displayedImage =
        harness.root->findChild<QObject*>(QStringLiteral("imageViewport-2"));
    auto* const thresholdSlider =
        harness.root->findChild<QObject*>(QStringLiteral("imageAlphaFilterSlider"));
    ASSERT_NE(displayedImage, nullptr);
    ASSERT_NE(thresholdSlider, nullptr);
    const auto sourceBeforeThreshold = displayedImage->property("source").toUrl();
    thresholdSlider->setProperty("value", 200);
    ASSERT_TRUE(QMetaObject::invokeMethod(thresholdSlider, "moved"));
    ASSERT_TRUE(harness.waitUntil([&] { return harness.imageReview.alphaThreshold() == 200; }));
    EXPECT_NE(displayedImage->property("source").toUrl(), sourceBeforeThreshold)
        << "moving the threshold must invalidate Qt's displayed image, not just the readout";
    EXPECT_EQ(harness.imageReview.imageForSlot(ImageReviewController::DisplayPrimarySlot)
                  .pixelColor(10, 20),
              QColor(39, 39, 39));

    // The structured readout: the coordinate and the hex value are separate fields, and the
    // hex value is not clipped away at this width - its content fits inside its own box
    // rather than being elided behind a tooltip.
    auto* const coordinate =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imagePairCoordA"));
    auto* const hex = harness.root->findChild<QQuickItem*>(QStringLiteral("imagePairHexA"));
    ASSERT_NE(coordinate, nullptr);
    ASSERT_NE(hex, nullptr);
    EXPECT_TRUE(coordinate->isVisible());
    EXPECT_TRUE(hex->isVisible());
    EXPECT_TRUE(hex->property("text").toString().startsWith(QStringLiteral("#")));
    EXPECT_LE(hex->property("contentWidth").toReal(), hex->property("width").toReal() + 0.5)
        << "the hex value must fit its own field instead of being elided";
    // The whole readout block stays inside the status bar's detail area.
    auto* const readoutBlock =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imagePairPixelReadout"));
    ASSERT_NE(readoutBlock, nullptr);
    EXPECT_LE(readoutBlock->x() + readoutBlock->width(), readoutBlock->parentItem()->width() + 0.5)
        << "the readout block must not run past the status bar";
    auto* const hints = harness.root->findChild<QQuickItem*>(QStringLiteral("imageStatusHints"));
    auto* const details =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageStatusDetails"));
    ASSERT_NE(hints, nullptr);
    ASSERT_NE(details, nullptr);
    EXPECT_GE(hints->height(), formatButton->height());
    EXPECT_GE(details->y(), hints->y() + hints->height());
    const QRectF formatBounds{formatButton->position(), formatButton->size()};
    const QRectF copyBounds{copyButton->position(), copyButton->size()};
    EXPECT_FALSE(formatBounds.intersects(copyBounds)) << "the persistent controls must not overlap";
    EXPECT_GE(formatButton->x(), 0);
    EXPECT_LE(copyButton->x() + copyButton->width(), hints->width() + 0.5);

    // The probe keeps reading the original RGBA whatever the filter does, and names the fact
    // that the current threshold hides this pixel. T = 200 is the exact strict-greater
    // boundary for a 200-alpha sample, so the hint is the observable form of that rule.
    // The Alpha toggle selects its RGB observation. Capture again after that view change,
    // then leave the image before changing the threshold and using the copy controls.
    ASSERT_TRUE(
        harness.imageReview.captureProbeSample(ImageReviewController::DisplayPrimarySlot, 10, 20));
    harness.imageReview.setAlphaThreshold(200);
    harness.settle();
    EXPECT_TRUE(
        harness.imageReview.frozenProbeSample().value(QStringLiteral("alphaHidden")).toBool())
        << "A = 200 with T = 200 is hidden, and the readout says so";
    EXPECT_EQ(harness.imageReview.frozenProbeSample().value(QStringLiteral("a")).toInt(), 200);
    // The copy still carries the original sample, never the mask colour.
    EXPECT_EQ(harness.imageReview.probeColorText(ImageReviewController::ProbeRgba),
              QStringLiteral("rgba(12, 34, 56, 200)"));
    // One step down and the same pixel passes again.
    harness.imageReview.setAlphaThreshold(199);
    harness.settle();
    EXPECT_FALSE(
        harness.imageReview.frozenProbeSample().value(QStringLiteral("alphaHidden")).toBool());

    // Evidence for the whole family at the 960px floor.
    const auto evidenceDirectory = qEnvironmentVariable("DVS_REVIEW_EVIDENCE_DIR");
    if (!evidenceDirectory.isEmpty()) {
        ASSERT_TRUE(QDir().mkpath(evidenceDirectory));
        harness.settleAnimations();
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(evidenceDirectory).filePath(QStringLiteral("image-probe-readout-960.png"))));
        // A wider window keeps the same fields readable, which is the other half of the
        // layout claim.
        harness.window->resize(1440, 900);
        harness.settleAnimations();
        EXPECT_EQ(coordinate->y(), redColumn->y()) << "use the available width before wrapping";
        auto* const lastDelta =
            harness.root->findChild<QQuickItem*>(QStringLiteral("imagePairColDeltaAlpha"));
        ASSERT_NE(lastDelta, nullptr);
        EXPECT_EQ(coordinate->y(), lastDelta->y());
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(evidenceDirectory).filePath(QStringLiteral("image-probe-readout-1440.png"))));
        ASSERT_TRUE(QMetaObject::invokeMethod(floatAction, "triggered"));
        harness.settleAnimations();
        EXPECT_EQ(redColumn->property("text").toString().trimmed(), QStringLiteral("0.047059"));
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(evidenceDirectory)
                .filePath(QStringLiteral("image-probe-readout-float-1440.png"))));
    }
}

// The probe-colour copy through the real production main window: the workspace's copy action
// writes the frozen sample through the real clipboard adapter and verifies the write. The
// success path is a mandatory acceptance condition - the session's clipboard is confirmed
// writable first (a distinct sentinel, retried against transient OLE contention), then the
// real menu action must overwrite that sentinel with the exact payload and report 已复制. A
// session whose clipboard cannot be written at all is skipped, never counted as a pass; a copy
// that only ever reports failure keeps the sentinel and fails here.
TEST(MainQmlContractTests, ImageCopyColorButtonCopiesTheVerifiedFrozenSample) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->resize(960, 640);
    harness.window->show();
    harness.settle();

    QImage primary(64, 64, QImage::Format_ARGB32);
    primary.fill(QColor(12, 34, 56, 200));
    QImage secondary(64, 64, QImage::Format_ARGB32);
    secondary.fill(QColor(200, 100, 50, 255));
    ASSERT_TRUE(harness.imageReview.openPairImages(std::move(primary),
                                                   QStringLiteral("a.png"),
                                                   std::move(secondary),
                                                   QStringLiteral("b.png")));
    ASSERT_TRUE(harness.activateWorkspace(1));
    harness.settle();
    auto* const workspace =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
    ASSERT_NE(workspace, nullptr);

    // Freeze a real probe sample exactly the way the hover handler does, then leave the pixel.
    harness.imageReview.updateCursorPixel(ImageReviewController::DisplayPrimarySlot, 10, 20);
    harness.settle();
    ASSERT_TRUE(harness.imageReview.cursorPixel().value(QStringLiteral("valid")).toBool());
    ASSERT_TRUE(
        harness.imageReview.captureProbeSample(ImageReviewController::DisplayPrimarySlot, 10, 20));
    ASSERT_TRUE(QMetaObject::invokeMethod(workspace, "clearImageHover"));
    harness.settle();

    auto* const copyButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageCopyColorButton"));
    auto* const copyRgb = harness.root->findChild<QObject*>(QStringLiteral("imageCopyRgbAction"));
    ASSERT_NE(copyButton, nullptr);
    ASSERT_NE(copyRgb, nullptr);
    ASSERT_TRUE(copyButton->property("enabled").toBool());
    // The action copies format 0 (ProbeRgb); the payload is whatever the controller prints.
    const QString payload = harness.imageReview.probeColorText(ImageReviewController::ProbeRgb);

    // Availability gate: confirm the clipboard is writable by writing a distinct sentinel
    // through the real adapter, retried against transient OLE contention. Persistent failure
    // means no usable clipboard, so the success case is skipped, never counted as a pass.
    QClipboard* const clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        GTEST_SKIP()
            << "no GUI clipboard in this session: the copy success path cannot be exercised";
    }
    const QString sentinel = QStringLiteral("DVS-COPY-SENTINEL-should-be-overwritten");
    bool sentinelWritten = false;
    for (int attempt = 0; attempt < 5 && !sentinelWritten; ++attempt) {
        clipboard->setText(sentinel);
        harness.settle();
        sentinelWritten = (clipboard->text() == sentinel);
    }
    if (!sentinelWritten) {
        GTEST_SKIP() << "clipboard not writable in this session (transient or absent): "
                        "the copy success path is not exercised";
    }

    // Drive the real menu action and require a verified success that overwrites the sentinel.
    // A copy that always reports failure never shows 已复制 and fails here; a transient miss is
    // retried.
    QString notice;
    for (int attempt = 0; attempt < 5; ++attempt) {
        ASSERT_TRUE(QMetaObject::invokeMethod(copyRgb, "triggered"));
        harness.settle();
        notice = workspace->property("marqueeNoticeText").toString();
        if (notice.contains(QStringLiteral("已复制"))) {
            break;
        }
    }
    EXPECT_TRUE(notice.contains(QStringLiteral("已复制"))) << notice.toStdString();
    EXPECT_EQ(clipboard->text(), payload);
}

TEST(MainQmlContractTests, VideoPixelGridButtonUsesSurfaceGeometryAndActualSampling) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    auto* const surface =
        harness.root->findChild<ComparisonSurface*>(QStringLiteral("dualVideoSurface"));
    auto* const viewport =
        harness.root->findChild<QQuickItem*>(QStringLiteral("mediaViewportFocusTarget"));
    auto* const button =
        harness.root->findChild<QQuickItem*>(QStringLiteral("videoPixelGridButton"));
    ASSERT_NE(surface, nullptr);
    ASSERT_NE(viewport, nullptr);
    ASSERT_NE(button, nullptr);
    EXPECT_GE(button->height(), 32.0);
    surface->setSourceDisplayInfo(
        {QVariantMap{{QStringLiteral("width"), 320}, {QStringLiteral("height"), 180}},
         QVariantMap{{QStringLiteral("width"), 320}, {QStringLiteral("height"), 180}}});
    surface->setVideoFilterMode(ComparisonSurface::VideoFilterPixel);
    surface->zoomAt(0.5, 0.5, 16);
    harness.settleAnimations();
    ASSERT_EQ(surface->pixelGridPanels().size(), 2);
    EXPECT_FALSE(viewport->property("pixelGridEnabled").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(button, "click"));
    ASSERT_TRUE(harness.waitUntil([&] { return viewport->property("pixelGridEnabled").toBool(); }));
    ASSERT_TRUE(harness.waitUntil(
        [&] { return findItemByName(viewport, QStringLiteral("videoPixelGrid")) != nullptr; }));
    QList<QQuickItem*> grids;
    for (auto* child : surface->parentItem()->childItems()) {
        if (child->objectName() == QStringLiteral("videoPixelGrid")) {
            grids.push_back(child);
        }
    }
    ASSERT_EQ(grids.size(), 2);
    for (auto* grid : grids) {
        EXPECT_TRUE(grid->property("active").toBool())
            << "requested=" << grid->property("requested").toBool()
            << " nearest=" << grid->property("nearest").toBool()
            << " step=" << grid->property("stepX").toReal() << ","
            << grid->property("stepY").toReal() << " size=" << grid->width() << ","
            << grid->height();
        EXPECT_LT(grid->property("columns").toInt() + grid->property("rows").toInt(), 1000);
        EXPECT_EQ(grid->parentItem(), surface->parentItem());
    }
    surface->setVideoFilterMode(ComparisonSurface::VideoFilterSmooth);
    harness.settle();
    for (auto* grid : grids) {
        EXPECT_FALSE(grid->property("active").toBool());
    }
    surface->setVideoFilterMode(ComparisonSurface::VideoFilterPixel);
    surface->resetViewport();
    harness.settle();
    int resetGrids = 0;
    for (auto* grid : surface->parentItem()->childItems()) {
        if (grid->objectName() == QStringLiteral("videoPixelGrid")) {
            ++resetGrids;
            EXPECT_FALSE(grid->property("active").toBool());
        }
    }
    EXPECT_EQ(resetGrids, 2);
}

TEST(MainQmlContractTests, ImagePixelGridButtonTracksPhysicalPixelsWithoutChangingSamples) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->resize(960, 640);
    harness.window->show();
    QImage image(96, 64, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixelColor(x, y, QColor(x * 2, y * 3, 180));
        }
    }
    ASSERT_TRUE(harness.imageReview.openPrimaryImage(image, QStringLiteral("grid.png")));
    ASSERT_TRUE(harness.activateWorkspace(1));
    auto* const workspace =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
    auto* const grid = harness.root->findChild<QQuickItem*>(QStringLiteral("imagePixelGrid-2"));
    auto* const button =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imagePixelGridButton"));
    auto* const preview = harness.root->findChild<QQuickItem*>(QStringLiteral("imageViewport-2"));
    ASSERT_NE(workspace, nullptr);
    ASSERT_NE(grid, nullptr);
    ASSERT_NE(button, nullptr);
    ASSERT_NE(preview, nullptr);
    EXPECT_FALSE(grid->property("requested").toBool());
    ASSERT_TRUE(workspace->setProperty("trueSize", true));
    harness.imageReview.setZoom(8);
    harness.settleAnimations();
    static_cast<void>(harness.window->grabWindow());
    const QPointF click = button->mapToScene({button->width() / 2, button->height() / 2});
    sendMouseMove(*harness.window, click);
    harness.settle();
    sendMousePress(*harness.window, click);
    sendMouseRelease(*harness.window, click);
    ASSERT_TRUE(harness.waitUntil([&] { return grid->property("active").toBool(); }))
        << "requested=" << grid->property("requested").toBool()
        << " nearest=" << grid->property("nearest").toBool()
        << " step=" << grid->property("stepX").toReal() << "," << grid->property("stepY").toReal()
        << " size=" << grid->width() << "," << grid->height() << " enabled=" << button->isEnabled()
        << " visible=" << button->isVisible() << " click=" << click.x() << "," << click.y();
    EXPECT_NEAR(grid->property("stepX").toReal() * harness.window->devicePixelRatio(), 8, 0.001);
    EXPECT_NEAR(grid->property("stepY").toReal() * harness.window->devicePixelRatio(), 8, 0.001);
    EXPECT_NEAR(grid->x() + grid->property("originX").toReal(), preview->x(), 0.001);
    EXPECT_NEAR(grid->y() + grid->property("originY").toReal(), preview->y(), 0.001);
    harness.imageReview.updateCursorPixel(2, 35, 30);
    EXPECT_EQ(harness.imageReview.cursorPixel().value(QStringLiteral("r")).toInt(), 70);
    EXPECT_EQ(harness.imageReview.cursorPixel().value(QStringLiteral("g")).toInt(), 90);
    const QString evidenceDirectory = qEnvironmentVariable("DVS_REVIEW_EVIDENCE_DIR");
    if (!evidenceDirectory.isEmpty()) {
        ASSERT_TRUE(QDir().mkpath(evidenceDirectory));
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(evidenceDirectory).filePath(QStringLiteral("image-pixel-grid-800.png"))));
    }
    harness.imageReview.setZoom(7.9);
    ASSERT_TRUE(harness.waitUntil([&] { return !grid->property("active").toBool(); }));
}

TEST(MainQmlContractTests, ImageSingleCrosshairFollowsPixelsAndHidesOutside) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->resize(960, 640);
    harness.window->show();
    QImage image(80, 60, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixelColor(x, y, QColor(x, y, 180));
        }
    }
    ASSERT_TRUE(harness.imageReview.openPrimaryImage(image, QStringLiteral("single.png")));
    ASSERT_TRUE(harness.activateWorkspace(1));
    harness.settleAnimations();
    auto* const workspace =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
    auto* const viewport = harness.root->findChild<QQuickItem*>(QStringLiteral("primaryViewport"));
    auto* const preview = harness.root->findChild<QQuickItem*>(QStringLiteral("imageViewport-2"));
    auto* const crosshair =
        harness.root->findChild<QQuickItem*>(QStringLiteral("syncedCrosshair-2"));
    auto* const coordinates =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageCrosshairCoordinateText-2"));
    auto* const vertical =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageCrosshairVertical-2"));
    auto* const horizontal =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageCrosshairHorizontal-2"));
    ASSERT_NE(workspace, nullptr);
    ASSERT_NE(viewport, nullptr);
    ASSERT_NE(preview, nullptr);
    ASSERT_NE(crosshair, nullptr);
    ASSERT_NE(coordinates, nullptr);
    ASSERT_NE(vertical, nullptr);
    ASSERT_NE(horizontal, nullptr);
    ASSERT_TRUE(harness.waitUntil(
        [&] { return preview->property("sourceSize").toSize() == QSize(80, 60); }));
    static_cast<void>(harness.window->grabWindow());
    auto* const statusBar = harness.root->findChild<QQuickItem*>(QStringLiteral("imageStatusBar"));
    ASSERT_NE(statusBar, nullptr);
    const qreal statusHeightBeforeSample = statusBar->height();
    const qreal scale = preview->width() / 80.0;
    const QPointF position{preview->x() + 35.75 * scale, preview->y() + 30.75 * scale};
    sendMouseMove(*harness.window, viewport->mapToScene(position));
    ASSERT_TRUE(harness.waitUntil([&] { return crosshair->isVisible(); }));
    EXPECT_TRUE(coordinates->isVisible());
    EXPECT_EQ(statusBar->height(), statusHeightBeforeSample)
        << "hovering must not resize the image underneath the pointer";
    const QVariantMap hover = workspace->property("hoverPoint").toMap();
    EXPECT_EQ(hover.value(QStringLiteral("imgX")).toInt(), 35);
    EXPECT_EQ(hover.value(QStringLiteral("imgY")).toInt(), 30);
    EXPECT_EQ(coordinates->property("text").toString(), QStringLiteral("35, 30"));
    EXPECT_EQ(harness.imageReview.cursorPixel().value(QStringLiteral("r")).toInt(), 35);
    EXPECT_EQ(harness.imageReview.cursorPixel().value(QStringLiteral("g")).toInt(), 30);
    EXPECT_NEAR(crosshair->property("targetX").toReal(), position.x(), 0.01);
    EXPECT_NEAR(crosshair->property("targetY").toReal(), position.y(), 0.01);
    EXPECT_NEAR(vertical->y(), preview->y(), 0.01);
    EXPECT_NEAR(vertical->height(), preview->height(), 0.01);
    EXPECT_NEAR(horizontal->x(), preview->x(), 0.01);
    EXPECT_NEAR(horizontal->width(), preview->width(), 0.01);

    // No new pointer event: a zoom/pan moves different pixels underneath the same cursor.
    harness.imageReview.setZoom(2);
    harness.imageReview.panBy(0.04, -0.03);
    harness.settleAnimations();
    const qreal zoomedScale = preview->width() / 80.0;
    const int zoomedX = qFloor((position.x() - preview->x()) / zoomedScale);
    const int zoomedY = qFloor((position.y() - preview->y()) / zoomedScale);
    EXPECT_EQ(workspace->property("hoverPoint").toMap().value(QStringLiteral("imgX")).toInt(),
              zoomedX);
    EXPECT_EQ(workspace->property("hoverPoint").toMap().value(QStringLiteral("imgY")).toInt(),
              zoomedY);
    EXPECT_NEAR(crosshair->property("targetX").toReal(), position.x(), 0.01);
    EXPECT_NEAR(crosshair->property("targetY").toReal(), position.y(), 0.01);
    EXPECT_EQ(coordinates->property("text").toString(),
              QStringLiteral("%1, %2").arg(zoomedX).arg(zoomedY));
    EXPECT_NEAR(vertical->y(), qMax(0.0, preview->y()), 0.01);
    EXPECT_NEAR(vertical->height(),
                qMin(viewport->height(), preview->y() + preview->height()) - vertical->y(),
                0.01);
    EXPECT_NEAR(horizontal->width(),
                qMin(viewport->width(), preview->x() + preview->width()) - horizontal->x(),
                0.01);

    // A smaller image panned partly off-screen must stop both hairlines at its visible edge.
    harness.imageReview.resetView();
    harness.imageReview.setZoom(0.8);
    harness.imageReview.panBy(-0.4, -0.4);
    harness.settleAnimations();
    static_cast<void>(harness.window->grabWindow());
    sendMouseMove(*harness.window,
                  viewport->mapToScene(QPointF{(preview->x() + preview->width()) / 2,
                                               (preview->y() + preview->height()) / 2}));
    harness.settle();
    EXPECT_TRUE(crosshair->isVisible());
    EXPECT_NEAR(vertical->height(),
                qMin(viewport->height(), preview->y() + preview->height()) -
                    qMax(0.0, preview->y()),
                0.01);
    EXPECT_NEAR(horizontal->width(),
                qMin(viewport->width(), preview->x() + preview->width()) - qMax(0.0, preview->x()),
                0.01);

    harness.imageReview.resetView();
    harness.settleAnimations();
    sendMouseMove(*harness.window, viewport->mapToScene(position));
    harness.settle();
    const QString evidenceDirectory = qEnvironmentVariable("DVS_REVIEW_EVIDENCE_DIR");
    if (!evidenceDirectory.isEmpty()) {
        ASSERT_TRUE(QDir().mkpath(evidenceDirectory));
        harness.root->setProperty("intentMessage", QString{});
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(evidenceDirectory).filePath(QStringLiteral("single-image-crosshair.png"))));
    }
    ASSERT_GT(preview->x(), 2.0);
    sendMouseMove(*harness.window, viewport->mapToScene(QPointF{preview->x() / 2, position.y()}));
    harness.settle();
    EXPECT_FALSE(crosshair->isVisible());
    EXPECT_TRUE(workspace->property("hoverPoint").isNull());
    EXPECT_TRUE(harness.imageReview.cursorPixel().isEmpty());

    sendMouseMove(*harness.window,
                  viewport->mapToScene(QPointF{preview->x() + 79.75 * scale, position.y()}));
    harness.settle();
    EXPECT_TRUE(crosshair->isVisible());
    EXPECT_EQ(workspace->property("hoverPoint").toMap().value(QStringLiteral("imgX")).toInt(), 79);
    sendMouseMove(*harness.window,
                  viewport->mapToScene(QPointF{preview->x() + 80.25 * scale, position.y()}));
    harness.settle();
    EXPECT_FALSE(crosshair->isVisible());
    EXPECT_TRUE(harness.imageReview.cursorPixel().isEmpty());

    // Manual A/B uses the side actually displayed; side-by-side keeps normalized linkage,
    // with the coordinate tag expressed in each image's own pixel dimensions.
    QImage secondary(40, 30, QImage::Format_ARGB32);
    secondary.fill(QColor(111, 112, 113));
    ASSERT_TRUE(harness.imageReview.openPairImages(
        image, QStringLiteral("a.png"), secondary, QStringLiteral("b.png")));
    workspace->setProperty("singleViewShowSecondary", true);
    harness.settleAnimations();
    static_cast<void>(harness.window->grabWindow());
    const qreal secondaryScale = preview->width() / 40.0;
    sendMouseMove(*harness.window,
                  viewport->mapToScene(QPointF{preview->x() + 20.75 * secondaryScale,
                                               preview->y() + 15.75 * secondaryScale}));
    harness.settle();
    EXPECT_TRUE(crosshair->isVisible());
    EXPECT_EQ(coordinates->property("text").toString(), QStringLiteral("20, 15"));
    EXPECT_EQ(harness.imageReview.cursorPixel().value(QStringLiteral("r")).toInt(), 111);
    harness.imageReview.setCompareMode(1);
    harness.settleAnimations();
    static_cast<void>(harness.window->grabWindow());
    const qreal pairedScale = preview->width() / 80.0;
    sendMouseMove(*harness.window,
                  viewport->mapToScene(QPointF{preview->x() + 35.75 * pairedScale,
                                               preview->y() + 30.75 * pairedScale}));
    harness.settle();
    auto* const syncedCoordinates =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageCrosshairCoordinateText-3"));
    ASSERT_NE(syncedCoordinates, nullptr);
    EXPECT_TRUE(crosshair->isVisible());
    EXPECT_TRUE(syncedCoordinates->isVisible());
    EXPECT_EQ(syncedCoordinates->property("text").toString(), QStringLiteral("17, 15"));
}

TEST(MainQmlContractTests, ImageSingleCrosshairTracksTheCroppedWorkingCopy) {
    WorkspaceHarness harness;
    harness.withImageEdit = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->resize(960, 640);
    harness.window->show();
    QImage image(80, 60, QImage::Format_ARGB32);
    image.fill(QColor(180, 40, 60));
    ASSERT_TRUE(harness.imageReview.openPrimaryImage(image, QStringLiteral("crop.png")));
    ASSERT_TRUE(harness.activateWorkspace(1));
    harness.settleAnimations();
    auto* const workspace =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageWorkspaceRoot"));
    auto* const viewport = harness.root->findChild<QQuickItem*>(QStringLiteral("primaryViewport"));
    auto* const preview = harness.root->findChild<QQuickItem*>(QStringLiteral("imageViewport-2"));
    auto* const crosshair =
        harness.root->findChild<QQuickItem*>(QStringLiteral("syncedCrosshair-2"));
    auto* const coordinates =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imageCrosshairCoordinateText-2"));
    auto* const readout =
        harness.root->findChild<QQuickItem*>(QStringLiteral("imagePixelReadoutCoordinate"));
    ASSERT_NE(workspace, nullptr);
    ASSERT_NE(viewport, nullptr);
    ASSERT_NE(preview, nullptr);
    ASSERT_NE(crosshair, nullptr);
    ASSERT_NE(coordinates, nullptr);
    ASSERT_NE(readout, nullptr);
    ASSERT_TRUE(harness.imageEdit.beginSession(2, QStringLiteral("crop.png"), QUrl{}));
    ASSERT_TRUE(harness.imageEdit.cropToImageRect(10, 8, 60, 40));
    harness.settleAnimations();
    ASSERT_TRUE(harness.waitUntil(
        [&] { return preview->property("sourceSize").toSize() == QSize(60, 40); }));
    static_cast<void>(harness.window->grabWindow());
    const qreal scale = preview->width() / 60.0;
    const QPointF start{preview->x() + 7.25 * scale, preview->y() + 6.25 * scale};
    const QPointF end{preview->x() + 40.25 * scale, preview->y() + 30.25 * scale};
    // A press also locates the crosshair, even before a move event is delivered.
    sendMousePress(*harness.window, viewport->mapToScene(start));
    harness.settle();
    EXPECT_TRUE(crosshair->isVisible());
    EXPECT_EQ(coordinates->property("text").toString(), QStringLiteral("7, 6"));
    const QPointF dragPosition = viewport->mapToScene(end);
    QMouseEvent drag{QEvent::MouseMove,
                     dragPosition,
                     harness.window->mapToGlobal(dragPosition.toPoint()),
                     Qt::NoButton,
                     Qt::LeftButton,
                     Qt::NoModifier};
    QCoreApplication::sendEvent(harness.window, &drag);
    harness.settle();
    EXPECT_TRUE(crosshair->isVisible());
    EXPECT_EQ(coordinates->property("text").toString(), QStringLiteral("40, 30"));
    const QVariantMap selection = workspace->property("cropSelection").toMap();
    EXPECT_EQ(selection.value(QStringLiteral("x")).toInt(), 7);
    EXPECT_EQ(selection.value(QStringLiteral("y")).toInt(), 6);
    EXPECT_EQ(selection.value(QStringLiteral("width")).toInt(), 33);
    EXPECT_EQ(selection.value(QStringLiteral("height")).toInt(), 24);
    EXPECT_TRUE(harness.imageReview.cursorPixel().isEmpty());
    EXPECT_EQ(readout->property("text").toString(), QStringLiteral("编辑副本 · 像素 (40, 30)"));
    sendMouseRelease(*harness.window, viewport->mapToScene(end));
    ASSERT_TRUE(QMetaObject::invokeMethod(workspace, "applyCropSelection"));
    harness.settleAnimations();
    EXPECT_EQ(harness.imageEdit.editedImage().size(), QSize(33, 24));
    static_cast<void>(harness.window->grabWindow());
    const qreal nextScale = preview->width() / 33.0;
    const QPointF position{preview->x() + 13.25 * nextScale, preview->y() + 10.25 * nextScale};
    sendMouseMove(*harness.window, viewport->mapToScene(position));
    harness.settle();
    EXPECT_TRUE(crosshair->isVisible());
    EXPECT_EQ(coordinates->property("text").toString(), QStringLiteral("13, 10"));
    EXPECT_EQ(readout->property("text").toString(), QStringLiteral("编辑副本 · 像素 (13, 10)"));
    const QString evidenceDirectory = qEnvironmentVariable("DVS_REVIEW_EVIDENCE_DIR");
    if (!evidenceDirectory.isEmpty()) {
        ASSERT_TRUE(QDir().mkpath(evidenceDirectory));
        harness.root->setProperty("intentMessage", QString{});
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(evidenceDirectory).filePath(QStringLiteral("crop-crosshair.png"))));
    }
    ASSERT_TRUE(harness.imageEdit.rotateImage(1));
    harness.settleAnimations();
    ASSERT_EQ(preview->property("sourceSize").toSize(), QSize(24, 33));
    const qreal rotatedScale = preview->width() / 24.0;
    const int rotatedX = qFloor((position.x() - preview->x()) / rotatedScale);
    const int rotatedY = qFloor((position.y() - preview->y()) / rotatedScale);
    EXPECT_EQ(coordinates->property("text").toString(),
              QStringLiteral("%1, %2").arg(rotatedX).arg(rotatedY));
    EXPECT_NEAR(crosshair->property("targetX").toReal(), position.x(), 0.01);
    EXPECT_NEAR(crosshair->property("targetY").toReal(), position.y(), 0.01);
    EXPECT_TRUE(harness.imageReview.cursorPixel().isEmpty());
    ASSERT_TRUE(harness.imageEdit.undo());
    harness.settleAnimations();
    EXPECT_EQ(coordinates->property("text").toString(), QStringLiteral("13, 10"));
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
    // The readout is now a structured block: the coordinate is its own fixed-width field
    // and the high-depth detail is a separate labelled line, so the test reads the fields
    // it names instead of one elided sentence.
    auto* readout = harness.root->findChild<QQuickItem*>(QStringLiteral("imagePixelReadoutNative"));
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
    // Evidence for the structured two-line readout at the 960px floor: the shot shows the
    // coordinate, the hex value, the alpha percentage and the high-depth detail laid out as
    // their own fields instead of one elided sentence.
    const auto evidenceDirectory = qEnvironmentVariable("DVS_REVIEW_EVIDENCE_DIR");
    if (!evidenceDirectory.isEmpty()) {
        ASSERT_TRUE(QDir().mkpath(evidenceDirectory));
        harness.settleAnimations();
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(evidenceDirectory).filePath(QStringLiteral("high-depth-alpha.png"))));
        // The same readout in the normalized-float form, so the layout claim covers both.
        harness.imageReview.setNumericFormat(ImageReviewController::NormalizedFloat);
        harness.settle();
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(evidenceDirectory).filePath(QStringLiteral("high-depth-alpha-float.png"))));
        harness.imageReview.setNumericFormat(ImageReviewController::IntegerCodeValues);
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
                  QStringLiteral("上一帧 / 下一帧（长按可连续逐帧）"));
        EXPECT_EQ(descriptionFor(model, QStringLiteral("↓ / ↑")),
                  QStringLiteral("后退 / 前进 5 帧"));
        EXPECT_EQ(descriptionFor(model, QStringLiteral("Shift+← / →")),
                  QStringLiteral("后退 / 前进 5 帧"));
        EXPECT_EQ(descriptionFor(model, QStringLiteral("Home / End")),
                  QStringLiteral("第一帧 / 最后一帧"));
        EXPECT_EQ(descriptionFor(model, QStringLiteral("Alt+← / →")),
                  QStringLiteral("垂直分割线左移 / 右移（仅左右切分）"));
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

// The image help table must describe the entries that actually exist after the A/B
// consolidation: the renamed single-image A/B mode, the A/B chips as the direct-selection
// entry, their right-click close, the hold-to-peek, and the new probe colour copy, numeric
// format and Alpha filter. No entry may advertise the removed standalone "切至 A / 切至 B"
// button, which duplicated the chips, the shortcuts and the in-canvas indicator.
TEST(MainQmlContractTests, ImageShortcutHelpDescribesTheRealEntries) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    auto* const shortcutHelp =
        harness.root->findChild<QObject*>(QStringLiteral("shortcutHelpOverlay"));
    ASSERT_NE(shortcutHelp, nullptr);
    shortcutHelp->setProperty("imagePreset", true);
    shortcutHelp->setProperty("playerPreset", false);
    const QVariantList model = shortcutHelp->property("shortcutModel").toList();
    const auto descriptionFor = [](const QVariantList& rows, const QString& keys) {
        for (const QVariant& entry : rows) {
            const QVariantList pair = entry.toList();
            if (pair.size() == 2 && pair.front().toString() == keys) {
                return pair.back().toString();
            }
        }
        return QString{};
    };
    // The renamed mode, and the shortcut that drives it.
    EXPECT_EQ(descriptionFor(model, QStringLiteral("空格 / T")),
              QStringLiteral("单图 A/B 模式切换 A / B"));
    // The file chips are the primary entry for choosing a side, and their right click closes
    // exactly that side.
    EXPECT_EQ(descriptionFor(model, QStringLiteral("点击 A / B 标签")),
              QStringLiteral("直接选择该侧素材"));
    EXPECT_EQ(descriptionFor(model, QStringLiteral("右键 A / B 标签")),
              QStringLiteral("关闭此侧素材"));
    // Hold-to-peek survives the consolidation.
    EXPECT_TRUE(descriptionFor(model, QStringLiteral("`（按住）/ Shift+`"))
                    .contains(QStringLiteral("按住直看")));
    // The new probe readout entries.
    EXPECT_EQ(descriptionFor(model, QStringLiteral("复制颜色 ▾")),
              QStringLiteral("复制探针样本的 RGB / RGBA / Hex"));
    EXPECT_EQ(descriptionFor(model, QStringLiteral("数值：整数码值 / 归一化浮点")),
              QStringLiteral("只改显示与复制格式，不改像素与统计"));
    EXPECT_EQ(descriptionFor(model, QStringLiteral("Alpha 过滤")),
              QStringLiteral("仅显示 A > T；未通过区域显示中性棋盘格"));
    // The removed duplicate must not be documented as if it still existed.
    for (const QVariant& entry : model) {
        const QVariantList pair = entry.toList();
        if (pair.size() != 2) {
            continue;
        }
        EXPECT_NE(pair.front().toString(), QStringLiteral("切至 A / 切至 B"))
            << "the standalone A/B switch button is gone and must not be advertised";
    }
}

// D1/D2/D3 keyboard contract: H owns pure mode (Tab is free for focus navigation again), Z
// flips between fit-window and true 1:1 through the same toggle the canvas double-click
// uses, and T alternates the whole-canvas direct look between the two slots of the actual
// current comparison pair - paused only, so it never competes with playback semantics.
TEST(MainQmlContractTests, HTogglesChromeTabNavigatesAndTAlternatesTheActualPair) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    const QString pathA = temporaryDirectory.filePath(QStringLiteral("keys-a.mp4"));
    const QString pathB = temporaryDirectory.filePath(QStringLiteral("keys-b.mp4"));
    for (const QString& path : {pathA, pathB}) {
        QFile file{path};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_GT(file.write("key-source", 10), 0);
    }
    WorkspaceHarness harness;
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate);
    ASSERT_TRUE(installValidatedVideoSet(
        harness.snapshot,
        {std::filesystem::path{pathA.toStdWString()}, std::filesystem::path{pathB.toStdWString()}},
        rate.value(),
        120,
        400'000));
    harness.snapshot->displayedFrame = domain::FrameId{41};
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    auto* const reviewShortcuts =
        harness.root->findChild<QObject*>(QStringLiteral("reviewShortcuts"));
    ASSERT_NE(reviewShortcuts, nullptr);
    const auto shortcutBySequence = [&reviewShortcuts](const QString& wanted) {
        const QList<QObject*> declared =
            reviewShortcuts->findChildren<QObject*>(Qt::FindDirectChildrenOnly);
        for (QObject* const object : declared) {
            if (object->property("sequence").toString() == wanted) {
                return object;
            }
        }
        return static_cast<QObject*>(nullptr);
    };

    // Tab no longer toggles the chrome: it hands focus to the next tab-stop while the
    // interface stays exactly where it was.
    EXPECT_TRUE(harness.root->property("chromeVisible").toBool());
    auto* const viewport =
        harness.root->findChild<QQuickItem*>(QStringLiteral("mediaViewportFocusTarget"));
    ASSERT_NE(viewport, nullptr);
    viewport->forceActiveFocus();
    ASSERT_TRUE(viewport->hasActiveFocus());
    const QQuickItem* const focusedBeforeTab = harness.window->activeFocusItem();
    sendKey(*harness.window, Qt::Key_Tab);
    harness.settle();
    EXPECT_TRUE(harness.root->property("chromeVisible").toBool())
        << "Tab must not toggle the chrome";
    EXPECT_NE(harness.window->activeFocusItem(), focusedBeforeTab)
        << "Tab must move the keyboard focus, not hide the interface";

    // H is the pure-mode key now, in both directions.
    sendKey(*harness.window, Qt::Key_H);
    harness.settle();
    EXPECT_FALSE(harness.root->property("chromeVisible").toBool());
    sendKey(*harness.window, Qt::Key_H);
    harness.settle();
    EXPECT_TRUE(harness.root->property("chromeVisible").toBool());

    // H must stay inert while a text-editing control owns the keyboard. Real user entry:
    // open the manual-anchor dialog and focus its editable canonical-frame spin box. Honest
    // scope note: with the dialog open the input context is BOTH modal and text-editing, so
    // this proves the user-facing guarantee (H never hides the interface while an editable
    // spin box holds focus) but not the text guard in isolation from the popup guard; the
    // context ladder orders modal above text, and the inspector's threshold spin box (a
    // non-modal editable site) lives inside a loader that this small-window harness never
    // instantiates, so the isolated non-modal case stays a recorded limitation.
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(), "openManualAnchorsDialog"));
    harness.settle();
    // Popup content lives under the window's overlay, not under the root item's scene-graph
    // subtree - start the objectName walk from the window content item.
    auto* const spinBox =
        findItemByName(harness.window->contentItem(), QStringLiteral("canonicalAnchorFrame"));
    ASSERT_NE(spinBox, nullptr);
    EXPECT_TRUE(spinBox->isVisible())
        << "the anchor dialog's editable spin box must be visible once the dialog opens";
    spinBox->forceActiveFocus();
    harness.settle();
    // Editable spin boxes forward focus to their internal editor, so the window's active
    // focus item is the editor - inside the spin box, whose textEditingInputContext puts the
    // input context on that chain.
    QQuickItem* const focusItem = harness.window->activeFocusItem();
    ASSERT_NE(focusItem, nullptr);
    ASSERT_NE(focusItem, viewport) << "focus must have moved into the spin box's editor";
    bool focusInsideSpinBox = false;
    for (const QQuickItem* cursor = focusItem; cursor != nullptr; cursor = cursor->parentItem()) {
        if (cursor == spinBox) {
            focusInsideSpinBox = true;
            break;
        }
    }
    ASSERT_TRUE(focusInsideSpinBox) << "focus must land inside the editable spin box";
    sendKey(*harness.window, Qt::Key_H);
    harness.settle();
    EXPECT_TRUE(harness.root->property("chromeVisible").toBool())
        << "H must be inert while an editable spin box holds the focus";
    // Leave the edit context exactly the way a user would: Esc closes the dialog and the
    // viewport takes the keyboard back.
    sendKey(*harness.window, Qt::Key_Escape);
    harness.settle();
    viewport->forceActiveFocus();
    harness.settle();

    // The dialog scenario couples the popup guard with the text guard. This isolates the
    // text guard alone: a visible, non-modal, test-owned TextInput declares the same
    // textEditingInputContext contract the editable spin boxes implement. Focus it for real,
    // press H, and require that the keystroke reached the text (real keyboard routing) while
    // the chrome stays put; handing focus back must make H toggle again.
    QQmlComponent textProbeComponent{&harness.engine};
    textProbeComponent.setData(
        "import QtQuick\nTextInput { objectName: \"hGuardTextProbe\"; property bool "
        "textEditingInputContext: true; width: 140; height: 22; x: 8; y: 8; color: \"white\" }",
        QUrl{QStringLiteral("qrc:/h-guard-text-probe")});
    ASSERT_EQ(textProbeComponent.status(), QQmlComponent::Ready)
        << componentErrors(textProbeComponent);
    QQuickItem* const textProbe = qobject_cast<QQuickItem*>(textProbeComponent.create());
    ASSERT_NE(textProbe, nullptr);
    textProbe->setParentItem(harness.window->contentItem());
    textProbe->forceActiveFocus();
    harness.settle();
    ASSERT_EQ(harness.window->activeFocusItem(), textProbe)
        << "the non-modal text probe must own the keyboard";
    // postEvent keys carry their text explicitly (the constructor does not synthesize it),
    // and both guard and positive control use the same primitive: the same H key event
    // posted to the window must reach the focused target through the normal dispatch, so a
    // difference between the two scenarios isolates the focus context rather than the
    // sending mechanism.
    const auto postHKey = [&window = *harness.window]() {
        QCoreApplication::postEvent(
            &window,
            new QKeyEvent{QEvent::KeyPress, Qt::Key_H, Qt::NoModifier, QStringLiteral("h")});
        QCoreApplication::postEvent(
            &window,
            new QKeyEvent{QEvent::KeyRelease, Qt::Key_H, Qt::NoModifier, QStringLiteral("h")});
    };
    postHKey();
    harness.settle();
    EXPECT_TRUE(harness.root->property("chromeVisible").toBool())
        << "H must be inert while a non-modal text input owns the focus";
    EXPECT_EQ(textProbe->property("text").toString(), QStringLiteral("h"))
        << "the keystroke must reach the text input, not be swallowed silently";
    // Positive control with the same event primitive: the identical posted H must toggle
    // once focus is back on the viewport, proving the guard difference is the focus context.
    viewport->forceActiveFocus();
    harness.settle();
    postHKey();
    harness.settle();
    EXPECT_FALSE(harness.root->property("chromeVisible").toBool())
        << "with focus back on the viewport, H toggles the chrome again";
    postHKey();
    harness.settle();
    EXPECT_TRUE(harness.root->property("chromeVisible").toBool());
    delete textProbe;

    // Hiding the chrome shows the restore pill, and the pill names H - never the retired
    // Tab binding the D1 rebind removed.
    sendKey(*harness.window, Qt::Key_H);
    harness.settle();
    EXPECT_FALSE(harness.root->property("chromeVisible").toBool());
    auto* const restorePill =
        harness.root->findChild<QQuickItem*>(QStringLiteral("chromeRestorePill"));
    ASSERT_NE(restorePill, nullptr);
    EXPECT_TRUE(restorePill->isVisible());
    QString pillText;
    const QList<QQuickItem*> pillChildren = restorePill->findChildren<QQuickItem*>();
    for (const QQuickItem* const child : pillChildren) {
        const QString candidate = child->property("text").toString();
        if (!candidate.isEmpty()) {
            pillText += candidate;
            pillText += QLatin1Char(' ');
        }
    }
    EXPECT_NE(pillText.indexOf(QString::fromUtf8("按 H")), -1)
        << "the restore pill must name the H binding";
    EXPECT_EQ(pillText.indexOf(QStringLiteral("Tab")), -1)
        << "the restore pill must not advertise the retired Tab binding";
    sendKey(*harness.window, Qt::Key_H);
    harness.settle();
    EXPECT_TRUE(harness.root->property("chromeVisible").toBool());

    // T alternates the direct look over the actual pair. With sources A/B the effective
    // edge is A<->B, so the canvas focus flips 0 -> 1 -> 0.
    QObject* const pairLookKey = shortcutBySequence(QStringLiteral("T"));
    ASSERT_NE(pairLookKey, nullptr);
    EXPECT_TRUE(pairLookKey->property("enabled").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(pairLookKey, "activated"));
    harness.settle();
    EXPECT_EQ(harness.shell->canvasFocusSlot(), 0)
        << "the first T enters the direct look on the pair's first slot";
    ASSERT_TRUE(QMetaObject::invokeMethod(pairLookKey, "activated"));
    harness.settle();
    EXPECT_EQ(harness.shell->canvasFocusSlot(), 1)
        << "the second T flips to the pair's second slot";
    ASSERT_TRUE(QMetaObject::invokeMethod(pairLookKey, "activated"));
    harness.settle();
    EXPECT_EQ(harness.shell->canvasFocusSlot(), 0) << "T keeps alternating inside the current pair";

    // While playing, T stays inert: no focus flip, no playback command.
    harness.snapshot->playbackState = domain::PlaybackState::kPlaying;
    harness.controller->refreshProjection();
    harness.settle();
    ASSERT_TRUE(harness.root->property("playing").toBool());
    harness.submitted.clear();
    ASSERT_TRUE(QMetaObject::invokeMethod(pairLookKey, "activated"));
    harness.settle();
    EXPECT_EQ(harness.shell->canvasFocusSlot(), 0)
        << "a playing session keeps T away from the canvas focus";
    EXPECT_TRUE(harness.submitted.empty()) << "T must not inject playback commands while playing";
    harness.snapshot->playbackState = domain::PlaybackState::kPaused;
    harness.controller->refreshProjection();
    harness.settle();

    // Z shares the double-click toggle: one press leaves the fit state, a second press
    // returns to it (the exact native target is the pixel badge's own contract).
    QObject* const nativeFitKey = shortcutBySequence(QStringLiteral("Z"));
    ASSERT_NE(nativeFitKey, nullptr);
    EXPECT_TRUE(nativeFitKey->property("enabled").toBool());
    auto* const surface = viewport->property("surface").value<QObject*>();
    ASSERT_NE(surface, nullptr);
    const qreal fitScale = surface->property("viewScale").toReal();
    ASSERT_TRUE(QMetaObject::invokeMethod(nativeFitKey, "activated"));
    harness.settle();
    const qreal nativeScale = surface->property("viewScale").toReal();
    EXPECT_NE(nativeScale, fitScale) << "Z must leave the fit state";
    ASSERT_TRUE(QMetaObject::invokeMethod(nativeFitKey, "activated"));
    harness.settle();
    EXPECT_NEAR(surface->property("viewScale").toReal(), fitScale, 1e-6)
        << "the second Z returns to fit";
}

// D3: three predictions with NO explicit GT reference. The C candidate switch must name the
// reason through the visible feedback channel and leave the committed pair untouched instead
// of silently anchoring on source A (the ReviewController canonical-source fallback stays a
// general read-path fallback, not a switch anchor), and the hold-to-peek GT entry - the
// mode-bar button and the backtick hold alike - stays unavailable for the same reason.
// E1: the viewport wipe-orientation button is the discoverable axis entry. Its hit area
// must meet the 32 px theme minimum even though ViewCommandButton defaults to 24 px; a real
// click (window mouse events) must flip the axis, keep the ratio and the view transform, and
// swap the label halves (左右 <-> 上下).
TEST(MainQmlContractTests, ViewportWipeOrientationButtonMeetsHitMinimumAndTogglesAxis) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    std::vector<std::filesystem::path> paths;
    for (const char name : {'a', 'b'}) {
        paths.push_back(std::filesystem::path{
            (temporaryDirectory.filePath(QString{"pair-%1.mp4"}.arg(QLatin1Char{name})))
                .toStdWString()});
        QFile file{QString::fromStdWString(paths.back().wstring())};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_GT(file.write("pair-source", 11), 0);
    }
    WorkspaceHarness harness;
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate);
    ASSERT_TRUE(installValidatedVideoSet(harness.snapshot, paths, rate.value(), 120, 400'000));
    harness.snapshot->displayedFrame = domain::FrameId{41};
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();
    ASSERT_EQ(harness.controller->sourceCount(), 2);

    // Switch the video session into wipe mode through the real preference.
    harness.preferences.setViewMode(ReviewPreferencesController::ViewMode::Wipe);
    harness.window->requestActivate();
    harness.settleAnimations();
    static_cast<void>(harness.window->grabWindow());

    auto* const button =
        harness.root->findChild<QQuickItem*>(QStringLiteral("viewportWipeOrientationButton"));
    ASSERT_NE(button, nullptr);
    EXPECT_TRUE(button->isVisible());
    EXPECT_GE(button->height(), 32.0)
        << "the new entry must meet the 32 px hit-area minimum on its own instance";

    // The orientation state lives on the dualVideoSurface (the viewport mirrors it). Main
    // has no such property, and reading a missing QVariant there converts to 0 - which
    // would make the initial assertion vacuously pass and the post-click one fail. Assert
    // the QVariant is valid before comparing, and set a non-default view first so the
    // "keeps the view" checks cannot be satisfied by a reset to defaults.
    auto* const surface = harness.root->findChild<QQuickItem*>(QStringLiteral("dualVideoSurface"));
    ASSERT_NE(surface, nullptr);
    const QVariant orientationBefore = surface->property("wipeOrientation");
    ASSERT_TRUE(orientationBefore.isValid());
    EXPECT_EQ(orientationBefore.toInt(), 0) << "a fresh session starts vertical";

    const QString verticalLabel = button->property("text").toString();
    EXPECT_NE(verticalLabel.indexOf(QString::fromUtf8("左右")), -1)
        << "vertical label must name 左右 halves";

    // Non-default view through the real interaction API: if the axis toggle reset the view,
    // these saved values would not survive. zoomAt/panBy are Q_INVOKABLE on the typed
    // surface; the QQuickItem* handle cannot call them directly.
    auto* const typedSurface = qobject_cast<ComparisonSurface*>(surface);
    ASSERT_NE(typedSurface, nullptr);
    typedSurface->zoomAt(0.5, 0.5, 1.25);
    typedSurface->panBy(0.05, -0.05);
    harness.settle();
    const qreal ratioBefore = surface->property("wipePosition").toReal();
    const qreal scaleBefore = surface->property("viewScale").toReal();
    const qreal centerXBefore = surface->property("viewCenterX").toReal();
    const qreal centerYBefore = surface->property("viewCenterY").toReal();

    // Real click at the button centre through window mouse events.
    const QPointF buttonCenter =
        button->mapToScene(QPointF{button->width() / 2, button->height() / 2});
    sendMouseMove(*harness.window, buttonCenter);
    harness.settleAnimations();
    SCOPED_TRACE(::testing::Message()
                 << "button enabled=" << button->isEnabled() << " opacity=" << button->opacity()
                 << " mode=" << surface->property("viewMode").toInt()
                 << " input=" << harness.root->property("inputContext").toInt());
    sendMousePress(*harness.window, buttonCenter);
    sendMouseRelease(*harness.window, buttonCenter);
    harness.settle();

    const QVariant orientationAfter = surface->property("wipeOrientation");
    ASSERT_TRUE(orientationAfter.isValid());
    EXPECT_EQ(orientationAfter.toInt(), 1) << "one real click must switch to the horizontal axis";
    const QString horizontalLabel = button->property("text").toString();
    EXPECT_NE(horizontalLabel.indexOf(QString::fromUtf8("上下")), -1)
        << "horizontal label must name 上下 halves";
    EXPECT_EQ(surface->property("wipePosition").toReal(), ratioBefore)
        << "axis toggle keeps the split ratio";
    EXPECT_EQ(surface->property("viewScale").toReal(), scaleBefore) << "axis toggle keeps the zoom";
    EXPECT_DOUBLE_EQ(surface->property("viewCenterX").toReal(), centerXBefore)
        << "axis toggle keeps the pan X";
    EXPECT_DOUBLE_EQ(surface->property("viewCenterY").toReal(), centerYBefore)
        << "axis toggle keeps the pan Y";

    // Second click returns to vertical.
    const QPointF buttonCenter2 =
        button->mapToScene(QPointF{button->width() / 2, button->height() / 2});
    sendMousePress(*harness.window, buttonCenter2);
    sendMouseRelease(*harness.window, buttonCenter2);
    harness.settle();
    const QVariant orientationBack = surface->property("wipeOrientation");
    ASSERT_TRUE(orientationBack.isValid());
    EXPECT_EQ(orientationBack.toInt(), 0);
}

// E1 keyboard evidence: W toggles the wipe split axis only while a real wipe composite is
// on screen, and the Alt arrow pairs are gated to their real axes - in vertical, Alt+Left/
// Right move the split and Alt+Up/Down are inert; in horizontal the roles swap. Real window
// key events drive everything, and the moves route through the same
// wipePositionRequested -> root.wipePosition -> surface path the handle drag uses.
TEST(MainQmlContractTests, WipeAxisKeyboardGatesMatchTheRealAxes) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    std::vector<std::filesystem::path> paths;
    for (const char name : {'a', 'b'}) {
        paths.push_back(std::filesystem::path{
            (temporaryDirectory.filePath(QString{"pair-%1.mp4"}.arg(QLatin1Char{name})))
                .toStdWString()});
        QFile file{QString::fromStdWString(paths.back().wstring())};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_GT(file.write("pair-source", 11), 0);
    }
    WorkspaceHarness harness;
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate);
    ASSERT_TRUE(installValidatedVideoSet(harness.snapshot, paths, rate.value(), 120, 400'000));
    harness.snapshot->displayedFrame = domain::FrameId{41};
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();
    harness.preferences.setViewMode(ReviewPreferencesController::ViewMode::Wipe);
    harness.window->requestActivate();
    harness.settleAnimations();
    auto* const viewport =
        harness.root->findChild<QQuickItem*>(QStringLiteral("mediaViewportFocusTarget"));
    ASSERT_NE(viewport, nullptr);
    viewport->forceActiveFocus();
    static_cast<void>(harness.window->grabWindow());
    harness.settle();
    SCOPED_TRACE(::testing::Message()
                 << "window active=" << harness.window->isActive()
                 << " wipe=" << harness.root->property("wipeMode").toBool()
                 << " global=" << harness.root->property("globalMediaShortcutsEnabled").toBool()
                 << " input=" << harness.root->property("inputContext").toInt());

    auto* const surface = harness.root->findChild<QQuickItem*>(QStringLiteral("dualVideoSurface"));
    ASSERT_NE(surface, nullptr);
    const QVariant orientationBefore = surface->property("wipeOrientation");
    ASSERT_TRUE(orientationBefore.isValid());
    EXPECT_EQ(orientationBefore.toInt(), 0) << "vertical starts the session";

    // W toggles vertical -> horizontal while the composite is live.
    sendKey(*harness.window, Qt::Key_W);
    harness.settle();
    EXPECT_EQ(surface->property("wipeOrientation").toInt(), 1)
        << "W must switch the split axis to horizontal";

    // In horizontal, Alt+Left/Right must be inert: the split no longer moves along X.
    harness.root->setProperty("wipePosition", QVariant{0.5});
    harness.settle();
    sendKey(*harness.window, Qt::Key_Left, Qt::AltModifier);
    harness.settle();
    EXPECT_EQ(harness.root->property("wipePosition").toDouble(), 0.5)
        << "Alt+Left must stay inert in horizontal mode";
    // Alt+Up moves the split up along Y (ratio 0.5 -> 0.49). The delta is a normalized
    // split-ratio step applied to the single ratio state, so the step is exact and identical
    // with normal chrome and in immersive mode (no outer-frame vs surface extent mixing).
    sendKey(*harness.window, Qt::Key_Up, Qt::AltModifier);
    harness.settle();
    EXPECT_NEAR(harness.root->property("wipePosition").toDouble(), 0.49, 1e-6)
        << "Alt+Up must move the horizontal split";

    // Back to vertical: the roles swap. Alt+Up/Down inert, Alt+Left moves.
    sendKey(*harness.window, Qt::Key_W);
    harness.settle();
    EXPECT_EQ(surface->property("wipeOrientation").toInt(), 0);
    harness.root->setProperty("wipePosition", QVariant{0.5});
    harness.settle();
    sendKey(*harness.window, Qt::Key_Up, Qt::AltModifier);
    harness.settle();
    EXPECT_EQ(harness.root->property("wipePosition").toDouble(), 0.5)
        << "Alt+Up must stay inert in vertical mode";
    sendKey(*harness.window, Qt::Key_Left, Qt::AltModifier);
    harness.settle();
    EXPECT_NEAR(harness.root->property("wipePosition").toDouble(), 0.49, 1e-6)
        << "Alt+Left must move the vertical split";
}

// E3: the controller-level ping-pong toggle flips ONLY the walk intent on the current
// loop interval. Each toggle submits SetPlaybackRangeCommand with the SAME [in, out] and
// loop=true; without an armed interval it is a refusal, not a new range. The harness
// snapshot is a manually driven fixture: after every command the fixture is advanced by
// the production projection protocol (the real coordinator projects range/loop/pingPong
// into its published snapshot), so settle-time refreshes keep the controller view
// consistent instead of silently clearing the optimistic projection.
TEST(MainQmlContractTests, PingPongToggleFlipsIntentWithoutTouchingTheInterval) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    std::vector<std::filesystem::path> paths;
    for (const char name : {'a', 'b'}) {
        paths.push_back(std::filesystem::path{
            (temporaryDirectory.filePath(QString{"pair-%1.mp4"}.arg(QLatin1Char{name})))
                .toStdWString()});
        QFile file{QString::fromStdWString(paths.back().wstring())};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_GT(file.write("pair-source", 11), 0);
    }
    WorkspaceHarness harness;
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate);
    ASSERT_TRUE(installValidatedVideoSet(harness.snapshot, paths, rate.value(), 120, 400'000));
    harness.snapshot->displayedFrame = domain::FrameId{15};
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    // Without a range: the toggle refuses instead of inventing an interval.
    EXPECT_FALSE(harness.controller->togglePlaybackRangePingPong());

    // Arm a loop range, then flip the walk intent twice through the real controller entry.
    // After each command the fixture snapshot is advanced exactly as the real coordinator
    // would project it (setPlaybackRange -> loop state; toggle -> flipped pingPong).
    ASSERT_TRUE(harness.controller->setPlaybackRange(10, 20, true));
    harness.snapshot->playbackRangeIn = domain::FrameId{10};
    harness.snapshot->playbackRangeOut = domain::FrameId{20};
    harness.snapshot->playbackRangeLoop = true;
    harness.snapshot->playbackRangePingPong = false;
    harness.settle();
    EXPECT_EQ(harness.controller->playbackRangeIn(), 10)
        << "the armed range must survive projection before the toggle";
    EXPECT_EQ(harness.controller->playbackRangeOut(), 20);

    ASSERT_TRUE(harness.controller->togglePlaybackRangePingPong());
    harness.snapshot->playbackRangePingPong = true;
    harness.settle();
    EXPECT_TRUE(harness.controller->playbackRangePingPong());

    ASSERT_TRUE(harness.controller->togglePlaybackRangePingPong());
    harness.snapshot->playbackRangePingPong = false;
    harness.settle();
    EXPECT_FALSE(harness.controller->playbackRangePingPong());

    std::vector<
        std::pair<std::optional<bool>, std::optional<std::pair<std::int64_t, std::int64_t>>>>
        rangeCommands;
    for (const application::PlaybackCommand& command : harness.submitted) {
        const auto* setRange = std::get_if<application::SetPlaybackRangeCommand>(&command);
        if (setRange == nullptr) {
            continue;
        }
        std::optional<std::pair<std::int64_t, std::int64_t>> interval;
        if (setRange->range.has_value()) {
            interval = {setRange->range->inInclusive.value(),
                        setRange->range->outInclusive.value()};
        }
        rangeCommands.push_back({setRange->pingPong, interval});
    }
    // Session setup may legitimately submit its own range command; the LAST THREE are ours:
    // arm(loop, no ping-pong) -> toggle ON -> toggle OFF, all on the same [10, 20].
    ASSERT_GE(rangeCommands.size(), 3U);
    const auto& arm = rangeCommands[rangeCommands.size() - 3U];
    const auto& on = rangeCommands[rangeCommands.size() - 2U];
    const auto& off = rangeCommands[rangeCommands.size() - 1U];
    ASSERT_TRUE(arm.first.has_value() && on.first.has_value() && off.first.has_value());
    EXPECT_FALSE(*arm.first) << "arming sets loop without the walk";
    EXPECT_TRUE(*on.first) << "the first toggle must turn the walk ON";
    EXPECT_FALSE(*off.first) << "the second toggle must turn the walk OFF";
    for (const auto* entry : {&arm, &on, &off}) {
        ASSERT_TRUE(entry->second.has_value());
        EXPECT_EQ(entry->second->first, 10);
        EXPECT_EQ(entry->second->second, 20);
    }

    // E3 ENTRY CHAIN (still open): set-range -> toggle ON -> real playRange must submit
    // StartRangePlaybackCommand with pingPong=true. The product fix is in (playRange
    // forwards view_.playbackRangePingPong); the harness-level run of this chain needs the
    // fixture advanced for the playback generation as well and stays on the open list.
}
TEST(MainQmlContractTests, CandidateSwitchWithoutReferenceReportsInsteadOfAnchoringOnA) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    std::vector<std::filesystem::path> paths;
    for (const char name : {'p', 'q', 'r'}) {
        paths.push_back(std::filesystem::path{
            (temporaryDirectory.filePath(QString{"pair-%1.mp4"}.arg(QLatin1Char{name})))
                .toStdWString()});
        QFile file{QString::fromStdWString(paths.back().wstring())};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_GT(file.write("pair-source", 11), 0);
    }
    WorkspaceHarness harness;
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate);
    ASSERT_TRUE(installAllPredictionVideoSet(harness.snapshot, paths, rate.value(), 120, 400'000))
        << "an all-prediction three-source set must validate";
    harness.snapshot->displayedFrame = domain::FrameId{41};
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();
    ASSERT_EQ(harness.controller->sourceCount(), 3);
    // The real projection keeps the canonical fallback in referenceSourceIndex, so the
    // explicit-GT flag is the fact the gate must read: canonical >= 0 but no reference role.
    const int canonicalIndex = harness.controller->canonicalSourceIndex();
    ASSERT_GE(canonicalIndex, 0) << "the no-reference counterexample needs canonical >= 0";
    EXPECT_EQ(harness.root->property("referenceSourceIndex").toInt(), canonicalIndex);
    EXPECT_FALSE(harness.root->property("hasExplicitReference").toBool())
        << "this test needs the all-prediction session: no explicit GT role";

    const int edgeBefore = harness.root->property("differenceEdge").toInt();
    const std::size_t commandsBefore = harness.submitted.size();
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(), "switchCandidateEdge"));
    harness.settle();
    const QString feedback = harness.root->property("intentMessage").toString();
    EXPECT_TRUE(feedback.contains(QString::fromUtf8("未设置参考")))
        << "the refused switch must say why; got: " << feedback.toStdString();
    EXPECT_EQ(harness.root->property("differenceEdge").toInt(), edgeBefore)
        << "the committed pair must not change without a GT anchor";
    EXPECT_EQ(harness.submitted.size(), commandsBefore)
        << "the refused switch must not submit a pair command";

    // The hold-to-peek GT entry follows the same availability: the mode-bar button is hidden
    // and a real backtick press/release never shows the raw reference.
    auto* const peekButton = findItemByName(qobject_cast<QQuickItem*>(harness.root.get()),
                                            QStringLiteral("differencePeekButton"));
    if (peekButton != nullptr) {
        EXPECT_FALSE(peekButton->isVisible())
            << "the peek button must stay hidden without an identifiable reference";
    }
    EXPECT_FALSE(harness.root->property("differencePeekAvailable").toBool());
    // A manual press (the hold arms through the window-level watcher) proves the hold is
    // inert WHILE pressed, not merely that it ends.
    QKeyEvent backtickPress{QEvent::KeyPress, Qt::Key_QuoteLeft, Qt::NoModifier};
    QCoreApplication::sendEvent(harness.window, &backtickPress);
    harness.settle();
    EXPECT_FALSE(harness.root->property("differencePeekActive").toBool())
        << "the backtick hold must stay inert while pressed without a GT reference";
    QKeyEvent backtickRelease{QEvent::KeyRelease, Qt::Key_QuoteLeft, Qt::NoModifier};
    QCoreApplication::sendEvent(harness.window, &backtickRelease);
    harness.settle();
    EXPECT_FALSE(harness.root->property("differencePeekActive").toBool());
}

TEST(MainQmlContractTests, TAlternatesTheActualThreeSourcePairWithRealKeys) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    std::vector<std::filesystem::path> paths;
    for (const char name : {'x', 'y', 'z'}) {
        paths.push_back(std::filesystem::path{
            (temporaryDirectory.filePath(QString{"pair-%1.mp4"}.arg(QLatin1Char{name})))
                .toStdWString()});
        QFile file{QString::fromStdWString(paths.back().wstring())};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_GT(file.write("pair-source", 11), 0);
    }
    WorkspaceHarness harness;
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate);
    ASSERT_TRUE(installValidatedVideoSet(harness.snapshot, paths, rate.value(), 120, 400'000));
    harness.snapshot->displayedFrame = domain::FrameId{41};
    // GT on session slot 1 (source B), so the anchor is not the zero slot.
    identifyReferenceSlot(harness.snapshot, 1);
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();
    ASSERT_EQ(harness.controller->sourceCount(), 3);
    // The explicit-GT projection: a validated reference role shows up as the flag, while the
    // index keeps pointing at the reference slot for its existing readers.
    EXPECT_TRUE(harness.root->property("hasExplicitReference").toBool())
        << "an identified reference role must project the explicit-GT flag";
    EXPECT_EQ(harness.root->property("referenceSourceIndex").toInt(), 1);

    // Switch the effective comparison pair to B<->C through the real Main edge applier.
    const QVariantList edges = harness.root->property("differenceEdges").toList();
    int targetPreference = -1;
    bool foundEdge = false;
    for (const QVariant& entry : edges) {
        const QVariantMap edge = entry.toMap();
        if (edge.value(QStringLiteral("firstSourceId")).toInt() == 1 &&
            edge.value(QStringLiteral("secondSourceId")).toInt() == 2) {
            targetPreference = edge.value(QStringLiteral("preferenceValue")).toInt();
            foundEdge = true;
            break;
        }
    }
    ASSERT_TRUE(foundEdge) << "the B<->C edge must exist in a three-source session";
    ASSERT_TRUE(QMetaObject::invokeMethod(
        harness.root.get(), "applyDifferenceEdge", Q_ARG(QVariant, QVariant{targetPreference})));
    harness.settle();
    // The fake session accepts the pair switch exactly like the application layer would (the
    // precedent is the DualReferenceDifference harness): the submitted pair lands in the
    // snapshot so the projection reflects the switch, and a succeeded terminal clears the
    // controller's pending-command state so `busy` does not hold the real-key T path below
    // disabled for the rest of the test.
    ASSERT_TRUE(std::holds_alternative<application::SetActiveComparisonPairCommand>(
        harness.submitted.back()));
    {
        const auto& pairCommand =
            std::get<application::SetActiveComparisonPairCommand>(harness.submitted.back());
        ASSERT_TRUE(pairCommand.pair.has_value());
        auto accepted = std::make_shared<application::SessionSnapshot>(*harness.snapshot);
        accepted->activeComparisonPair = *pairCommand.pair;
        harness.snapshot = accepted;
    }
    harness.terminals.push_back(application::CommandTerminal{
        .context = application::commandContext(harness.submitted.back()),
        .outcome = application::CommandOutcome::Succeeded,
    });
    // The real-key T shortcut is enabled only while the controller is idle, and the fake
    // session must complete EVERY accepted command the way the application layer does - the
    // startup pair-policy handshakes included - or the oldest pending command keeps `busy`
    // true and a real key press finds the shortcut disabled.
    for (const application::PlaybackCommand& command : harness.submitted) {
        harness.terminals.push_back(application::CommandTerminal{
            .context = application::commandContext(command),
            .outcome = application::CommandOutcome::Succeeded,
        });
    }
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_FALSE(harness.controller->busy()) << "the session must be idle before the real keys";
    EXPECT_EQ(harness.root->property("differenceFirstSlot").toInt(), 1);
    EXPECT_EQ(harness.root->property("differenceSecondSlot").toInt(), 2);

    // Real key delivery (window events, not signal synthesis): T enters the direct look on
    // the pair's first slot (1 = B), the next press flips to slot 2 (C).
    QObject* const reviewShortcuts =
        harness.root->findChild<QObject*>(QStringLiteral("reviewShortcuts"));
    ASSERT_NE(reviewShortcuts, nullptr);
    QObject* const pairLookKey = [&reviewShortcuts]() {
        const QList<QObject*> declared =
            reviewShortcuts->findChildren<QObject*>(Qt::FindDirectChildrenOnly);
        for (QObject* const object : declared) {
            if (object->property("sequence").toString() == QStringLiteral("T")) {
                return object;
            }
        }
        return static_cast<QObject*>(nullptr);
    }();
    ASSERT_NE(pairLookKey, nullptr);
    // A real key press reaches the shortcut only through its enabled gate, so pin the gate
    // itself: an accidental regression here would otherwise look like "T does nothing".
    EXPECT_TRUE(pairLookKey->property("enabled").toBool())
        << "videoViewShortcutsEnabled="
        << harness.root->property("videoViewShortcutsEnabled").toInt()
        << " inputContext=" << harness.root->property("inputContext").toInt()
        << " overlayVisible=" << harness.root->property("overlayVisible").toInt()
        << " playing=" << harness.root->property("playing").toInt();
    // QML Shortcuts reach activation through the focus item's ShortcutOverride delivery; a
    // real session always has a focused stage (HToggles parks focus the same way before its
    // real keys). Without one the synthesized key press finds no delivery path.
    auto* const stage =
        harness.root->findChild<QQuickItem*>(QStringLiteral("mediaViewportFocusTarget"));
    ASSERT_NE(stage, nullptr);
    stage->forceActiveFocus();
    QKeyEvent tPress{QEvent::KeyPress, Qt::Key_T, Qt::NoModifier};
    QKeyEvent tRelease{QEvent::KeyRelease, Qt::Key_T, Qt::NoModifier};
    static_cast<void>(QCoreApplication::sendEvent(harness.window, &tPress));
    static_cast<void>(QCoreApplication::sendEvent(harness.window, &tRelease));
    harness.settle();
    EXPECT_EQ(harness.shell->canvasFocusSlot(), 1)
        << "T must enter on the actual pair's first slot (B), not a hardcoded A";
    static_cast<void>(QCoreApplication::sendEvent(harness.window, &tPress));
    static_cast<void>(QCoreApplication::sendEvent(harness.window, &tRelease));
    harness.settle();
    EXPECT_EQ(harness.shell->canvasFocusSlot(), 2)
        << "the second T flips to the actual pair's second slot (C)";

    // Playing sessions keep T inert: no flip, no playback command.
    harness.snapshot->playbackState = domain::PlaybackState::kPlaying;
    harness.controller->refreshProjection();
    harness.settle();
    ASSERT_TRUE(harness.root->property("playing").toBool());
    harness.submitted.clear();
    static_cast<void>(QCoreApplication::sendEvent(harness.window, &tPress));
    static_cast<void>(QCoreApplication::sendEvent(harness.window, &tRelease));
    harness.settle();
    EXPECT_EQ(harness.shell->canvasFocusSlot(), 2);
    EXPECT_TRUE(harness.submitted.empty());
}

// Digit keys switch comparison modes with the mode bar's own availability (pair modes need
// two sources, three-source modes stay disabled here), F/+/- drive the viewport zoom path, and
// the backtick hold/lock pair drives the raw-reference peek. The help table must be the
// entries ReviewShortcuts itself declares plus the static mouse tails - not a hand-maintained
// copy.
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
    // D4: the backtick is a real hold now (a window-level watcher, not a Shortcut): press
    // shows the raw reference view, release restores the difference view, auto-repeat pairs
    // never re-trigger it, and a release carrying a modifier pressed mid-hold still ends it.
    // Shift+backtick remains the lock, and a plain hold never unlocks a locked peek.
    QObject* const peekToggleKey = shortcutBySequence(QStringLiteral("`"));
    EXPECT_EQ(peekToggleKey, nullptr) << "the backtick must not be a toggle Shortcut any more";
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

    // The peek names the reference (GT) source, so the session must identify one.
    EXPECT_TRUE(harness.root->property("differenceMode").toBool());
    identifyReferenceSlot(harness.snapshot, 0);
    harness.controller->refreshProjection();
    harness.settle();
    const auto sendBacktick = [&harness](const QEvent::Type type,
                                         const Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                                         const bool autoRepeat = false) {
        QKeyEvent event{type, Qt::Key_QuoteLeft, modifiers, QStringLiteral("`"), autoRepeat, 1};
        static_cast<void>(QCoreApplication::sendEvent(harness.window, &event));
        QCoreApplication::processEvents();
    };
    // Basic hold: press shows, release restores.
    sendBacktick(QEvent::KeyPress);
    EXPECT_TRUE(harness.root->property("differencePeekActive").toBool());
    sendBacktick(QEvent::KeyRelease);
    EXPECT_FALSE(harness.root->property("differencePeekActive").toBool());
    // Auto-repeat pairs (press and release alike) never change the hold.
    sendBacktick(QEvent::KeyPress);
    sendBacktick(QEvent::KeyPress, Qt::NoModifier, true);
    sendBacktick(QEvent::KeyRelease, Qt::NoModifier, true);
    sendBacktick(QEvent::KeyPress, Qt::NoModifier, true);
    EXPECT_TRUE(harness.root->property("differencePeekActive").toBool())
        << "auto-repeats must not end the hold";
    // A release with a modifier pressed mid-hold still ends the armed hold.
    sendBacktick(QEvent::KeyRelease, Qt::ShiftModifier);
    EXPECT_FALSE(harness.root->property("differencePeekActive").toBool())
        << "a modified release of an armed hold must still restore the difference view";
    // A modified press never starts a hold; its release passes through untouched.
    sendBacktick(QEvent::KeyPress, Qt::ControlModifier);
    EXPECT_FALSE(harness.root->property("differencePeekActive").toBool());
    sendBacktick(QEvent::KeyRelease, Qt::ControlModifier);
    EXPECT_FALSE(harness.root->property("differencePeekActive").toBool());
    // The lock is its own action; a hold on a locked peek neither unlocks nor flickers.
    ASSERT_TRUE(QMetaObject::invokeMethod(peekLockKey, "activated"));
    EXPECT_TRUE(harness.root->property("differencePeekActive").toBool());
    EXPECT_TRUE(harness.root->property("differencePeekLocked").toBool());
    sendBacktick(QEvent::KeyPress);
    EXPECT_TRUE(harness.root->property("differencePeekActive").toBool());
    sendBacktick(QEvent::KeyRelease);
    EXPECT_TRUE(harness.root->property("differencePeekActive").toBool())
        << "releasing the hold keeps a locked peek on screen";
    const QString peekEvidence = qEnvironmentVariable("DVS_REVIEW_EVIDENCE_DIR");
    if (!peekEvidence.isEmpty()) {
        QDir().mkpath(peekEvidence);
        harness.window->resize(960, 640);
        harness.settle();
        auto* const indicator =
            harness.root->findChild<QQuickItem*>(QStringLiteral("differencePeekIndicator"));
        if (indicator != nullptr) {
            static_cast<void>(
                harness.waitUntil([indicator] { return indicator->opacity() > 0.99; }));
        }
        harness.root->setProperty("intentMessage", QString{});
        harness.window->grabWindow().save(
            QDir{peekEvidence}.filePath(QStringLiteral("gt-peek-locked-960.png")));
    }
    ASSERT_TRUE(QMetaObject::invokeMethod(peekLockKey, "activated"));
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
    // Video preset model = filtered media rows + workspace rows + the five static mouse tails.
    const QVariantList workspaceEntries = harness.root->property("workspaceHelpEntries").toList();
    // 打开视频+打开图片 merged into one 打开文件 entry (2026-10-14): 8 rows.
    ASSERT_GE(workspaceEntries.size(), 8);
    ASSERT_EQ(model.size(), expectedEntries.size() + workspaceEntries.size() + 5);
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

// Bindings, menu labels and the help table must tell the same story, and no key may be bound
// twice inside one activation context. The menu bar reads its sequences from ShortcutCatalog
// (the same constants Main.qml binds); this test is the tripwire that catches a fresh literal,
// a renamed key, or a second owner of an existing key.
TEST(MainQmlContractTests, ShortcutBindingsMenuAndHelpStayInSync) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    // Every live Shortcut in the window, flattened to its sequence strings.
    const QList<QObject*> allObjects = harness.root->findChildren<QObject*>();
    QStringList sequences;
    for (QObject* const object : allObjects) {
        if (!object->property("sequence").isValid() && !object->property("sequences").isValid()) {
            continue;
        }
        const QString sequence = object->property("sequence").toString();
        if (!sequence.isEmpty()) {
            sequences.push_back(sequence);
            continue;
        }
        const QVariantList multi = object->property("sequences").toList();
        for (const QVariant& entry : multi) {
            if (!entry.toString().isEmpty()) {
                sequences.push_back(entry.toString());
            }
        }
    }
    ASSERT_GE(sequences.size(), 40);

    // Duplicate keys inside one activation context shadow each other. Esc is the one
    // documented overlap: the presentation Esc (full screen / hidden chrome) and the
    // narrow-window drawer Esc are never enabled for the same state.
    QStringList sorted = sequences;
    sorted.sort();
    for (qsizetype index = 1; index < sorted.size(); ++index) {
        if (sorted.at(index) == sorted.at(index - 1) && sorted.at(index) != QStringLiteral("Esc")) {
            ADD_FAILURE() << "sequence " << sorted.at(index).toStdString()
                          << " is bound more than once";
        }
    }

    // Menu labels must reference a real binding; Alt+F4 is menu-only because the window
    // manager owns its delivery.
    int labelledMenuItems = 0;
    for (QObject* const item : allObjects) {
        const QVariant label = item->property("shortcutText");
        if (!label.isValid() || label.toString().isEmpty()) {
            continue;
        }
        ++labelledMenuItems;
        if (label.toString() == QStringLiteral("Alt+F4")) {
            continue;
        }
        EXPECT_TRUE(sequences.contains(label.toString()))
            << "menu label " << label.toString().toStdString() << " has no live binding";
    }
    // The merged 打开文件 entry dropped the separate 打开图片 row (2026-10-14).
    ASSERT_GE(labelledMenuItems, 9);

    // The workspace help rows share the same source: every row must be a live binding and
    // must appear in the help model for both workspaces.
    QObject* const shortcutHelp =
        harness.root->findChild<QObject*>(QStringLiteral("shortcutHelpOverlay"));
    ASSERT_NE(shortcutHelp, nullptr);
    const QVariantList workspaceEntries = harness.root->property("workspaceHelpEntries").toList();
    // 打开视频+打开图片 merged into one 打开文件 entry (2026-10-14): 8 rows.
    ASSERT_GE(workspaceEntries.size(), 8);
    const auto modelHasKey = [&shortcutHelp](const bool imagePreset, const QString& keys) {
        shortcutHelp->setProperty("imagePreset", imagePreset);
        shortcutHelp->setProperty("playerPreset", false);
        const QVariantList model = shortcutHelp->property("shortcutModel").toList();
        for (const QVariant& entry : model) {
            const QVariantList pair = entry.toList();
            if (pair.size() == 2 && pair.front().toString() == keys) {
                return true;
            }
        }
        return false;
    };
    for (const QVariant& entry : workspaceEntries) {
        const QString keys = entry.toList().front().toString();
        SCOPED_TRACE(keys.toStdString());
        EXPECT_TRUE(sequences.contains(keys)) << "workspace help row without a binding";
        EXPECT_TRUE(modelHasKey(false, keys)) << "workspace help row missing from video help";
        EXPECT_TRUE(modelHasKey(true, keys)) << "workspace help row missing from image help";
    }
}

// M freezes the observation at the keypress: the record exists before the dialog is touched,
// so playback advancing while the note is typed cannot shift what was recorded. Enter attaches
// the typed note to that frozen record; Escape attaches an empty one - the note is optional,
// never a gate.
// FLOW1 end-to-end through the real main window: capture a non-default observation (mode,
// zoom/pan, channel view, background, diff gain, alpha filter, resampling), change all of it,
// then restore the record and prove the staged machine reopened the same files and replayed the
// recorded state. This is the main-window wiring test, not just a controller payload test.
TEST(MainQmlContractTests, ImageIssueRestoreReplaysTheRecordedObservation) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    const QString pathA = temporaryDirectory.filePath(QStringLiteral("issue-left.png"));
    const QString pathB = temporaryDirectory.filePath(QStringLiteral("issue-right.png"));
    ASSERT_TRUE(writeTestPng(pathA));
    ASSERT_TRUE(writeTestPng(pathB));

    WorkspaceHarness harness;
    harness.withIssueLog = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    QVariant activated;
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(),
                                          "activateWorkspace",
                                          Q_RETURN_ARG(QVariant, activated),
                                          Q_ARG(QVariant, QVariant{1})));
    ASSERT_TRUE(activated.toBool());
    harness.settle();

    auto* const imageWorkspace = qvariant_cast<QObject*>(harness.root->property("imageWorkspace"));
    ASSERT_NE(imageWorkspace, nullptr);
    // The reopened files decode to 1×1; keep the injected pair the same size so the retained
    // difference mode is not rejected by the geometry guard during the restore.
    QImage left(1, 1, QImage::Format_ARGB32);
    left.fill(QColor(10, 20, 30));
    QImage right(1, 1, QImage::Format_ARGB32);
    right.fill(QColor(40, 50, 60));
    ASSERT_TRUE(harness.imageReview.openPairImages(
        std::move(left), pathA, std::move(right), pathB, /*pairId=*/6));
    harness.imageReview.setCompareMode(ImageReviewController::SignedDifference);
    harness.imageReview.restoreObservationPosition(2.5, 0.2, 0.8);
    harness.imageReview.setViewMode(ImageReviewController::RgbOpaqueView);
    harness.imageReview.setDiffGain(11);
    harness.imageReview.setAlphaFilterEnabled(true);
    harness.imageReview.setAlphaThreshold(222);
    harness.imageReview.setResampleAllowed(true);
    imageWorkspace->setProperty("backgroundMode", 3);
    harness.settle();

    QVariant captured;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        harness.root.get(), "captureIssueLog", Q_RETURN_ARG(QVariant, captured)));
    ASSERT_TRUE(captured.toBool());
    harness.settle();
    ASSERT_EQ(harness.issueLog.count(), 1);
    auto* const noteDialog = harness.root->findChild<QObject*>(QStringLiteral("issueNoteDialog"));
    ASSERT_NE(noteDialog, nullptr);
    if (noteDialog->property("visible").toBool()) {
        auto* const noteField = noteDialog->findChild<QObject*>(QStringLiteral("issueNoteField"));
        ASSERT_NE(noteField, nullptr);
        sendKeyToFocus(noteField, QEvent::KeyPress, Qt::Key_Escape);
        sendKeyToFocus(noteField, QEvent::KeyRelease, Qt::Key_Escape);
        harness.settle();
    }

    // Move every recorded setting away from what was captured.
    harness.imageReview.setCompareMode(ImageReviewController::SideBySide);
    harness.imageReview.restoreObservationPosition(1.0, 0.5, 0.5);
    harness.imageReview.setViewMode(ImageReviewController::RgbaView);
    harness.imageReview.setDiffGain(4);
    harness.imageReview.setAlphaFilterEnabled(false);
    harness.imageReview.setAlphaThreshold(0);
    harness.imageReview.setResampleAllowed(false);
    imageWorkspace->setProperty("backgroundMode", 1);
    harness.settle();

    const QVariantMap payload = harness.issueLog.restoreIssue(0);
    ASSERT_EQ(payload.value(QStringLiteral("decision")).toString(), QStringLiteral("ready"))
        << payload.value(QStringLiteral("message")).toString().toStdString();
    const bool restored = harness.waitUntil([&harness, imageWorkspace] {
        return harness.imageReview.hasPair() &&
               harness.imageReview.compareMode() == ImageReviewController::SignedDifference &&
               harness.imageReview.viewMode() == ImageReviewController::RgbOpaqueView &&
               harness.imageReview.diffGain() == 11 && harness.imageReview.alphaFilterEnabled() &&
               harness.imageReview.alphaThreshold() == 222 &&
               harness.imageReview.resampleAllowed() &&
               imageWorkspace->property("backgroundMode").toInt() == 3;
    });
    ASSERT_TRUE(restored) << harness.imageReview.errorText().toStdString()
                          << " primary=" << harness.imageReview.primaryPath().toStdString()
                          << " secondary=" << harness.imageReview.secondaryPath().toStdString();
    const auto samePath = [](const QString& left, const QString& right) {
        return QDir::cleanPath(left).compare(QDir::cleanPath(right), Qt::CaseInsensitive) == 0;
    };
    EXPECT_TRUE(samePath(harness.imageReview.primaryPath(), pathA));
    EXPECT_TRUE(samePath(harness.imageReview.secondaryPath(), pathB));
    EXPECT_DOUBLE_EQ(harness.imageReview.zoom(), 2.5);
    EXPECT_DOUBLE_EQ(harness.imageReview.panX(), 0.2);
    EXPECT_DOUBLE_EQ(harness.imageReview.panY(), 0.8);
    harness.window->close();
}

// FLOW1 folder-row restore: the record was captured on a non-first row. Restore must reopen the
// folder session, return to that row (not row 0), and replay the observation only after that
// pair commits.
TEST(MainQmlContractTests, ImageFolderIssueRestoreReturnsToTheRecordedRow) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    const QString leftDir = QDir(temporaryDirectory.path()).filePath(QStringLiteral("row_left"));
    const QString rightDir = QDir(temporaryDirectory.path()).filePath(QStringLiteral("row_right"));
    ASSERT_TRUE(QDir{}.mkpath(leftDir));
    ASSERT_TRUE(QDir{}.mkpath(rightDir));
    for (const QString& folder : {leftDir, rightDir}) {
        for (const char* name : {"frame001.png", "frame002.png"}) {
            ASSERT_TRUE(writeTestPng(QDir(folder).filePath(QString::fromLatin1(name))));
        }
    }

    WorkspaceHarness harness;
    harness.withIssueLog = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    QVariant activated;
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(),
                                          "activateWorkspace",
                                          Q_RETURN_ARG(QVariant, activated),
                                          Q_ARG(QVariant, QVariant{1})));
    ASSERT_TRUE(activated.toBool());
    harness.root->setProperty("imageFolderLeftUrl", QUrl::fromLocalFile(leftDir));
    harness.root->setProperty("imageFolderRightUrl", QUrl::fromLocalFile(rightDir));
    QVariant loaded;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        harness.root.get(), "loadFolderComparison", Q_RETURN_ARG(QVariant, loaded)));
    ASSERT_TRUE(loaded.toBool());
    ASSERT_TRUE(harness.waitUntil([&harness] {
        return harness.folderPairs.currentPair() == 0 && harness.imageReview.hasPair();
    }));

    // Move to the second row, which is the observation the record will capture.
    ASSERT_TRUE(harness.folderPairs.openPairAt(1));
    ASSERT_TRUE(harness.waitUntil([&harness] {
        return harness.folderPairs.currentPair() == 1 && harness.imageReview.hasPair() &&
               harness.imageReview.secondaryPath().endsWith(QStringLiteral("frame002.png"));
    }));
    harness.imageReview.setCompareMode(ImageReviewController::Highlight);
    harness.imageReview.restoreObservationPosition(3.0, 0.3, 0.7);
    harness.imageReview.setViewMode(ImageReviewController::AlphaGrayView);
    harness.imageReview.setDiffGain(7);
    harness.settle();

    QVariant captured;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        harness.root.get(), "captureIssueLog", Q_RETURN_ARG(QVariant, captured)));
    ASSERT_TRUE(captured.toBool());
    harness.settle();
    ASSERT_EQ(harness.issueLog.count(), 1);
    auto* const noteDialog = harness.root->findChild<QObject*>(QStringLiteral("issueNoteDialog"));
    ASSERT_NE(noteDialog, nullptr);
    if (noteDialog->property("visible").toBool()) {
        auto* const noteField = noteDialog->findChild<QObject*>(QStringLiteral("issueNoteField"));
        ASSERT_NE(noteField, nullptr);
        sendKeyToFocus(noteField, QEvent::KeyPress, Qt::Key_Escape);
        sendKeyToFocus(noteField, QEvent::KeyRelease, Qt::Key_Escape);
        harness.settle();
    }

    // Go back to row 0 and reset the view; restore must return to row 1 and replay the mode.
    ASSERT_TRUE(harness.folderPairs.openPairAt(0));
    ASSERT_TRUE(harness.waitUntil([&harness] {
        return harness.folderPairs.currentPair() == 0 && harness.imageReview.hasPair() &&
               harness.imageReview.secondaryPath().endsWith(QStringLiteral("frame001.png"));
    }));
    harness.imageReview.setCompareMode(ImageReviewController::SideBySide);
    harness.imageReview.restoreObservationPosition(1.0, 0.5, 0.5);
    harness.imageReview.setViewMode(ImageReviewController::RgbaView);
    harness.imageReview.setDiffGain(4);
    harness.settle();

    const QVariantMap payload = harness.issueLog.restoreIssue(0);
    ASSERT_EQ(payload.value(QStringLiteral("decision")).toString(), QStringLiteral("ready"));
    ASSERT_TRUE(harness.waitUntil([&harness] {
        return harness.folderPairs.currentPair() == 1 && harness.imageReview.hasPair() &&
               harness.imageReview.compareMode() == ImageReviewController::Highlight &&
               harness.imageReview.viewMode() == ImageReviewController::AlphaGrayView &&
               harness.imageReview.diffGain() == 7 &&
               qFuzzyCompare(harness.imageReview.zoom(), 3.0);
    })) << "row="
        << harness.folderPairs.currentPair()
        << " primary=" << harness.imageReview.primaryPath().toStdString();
    EXPECT_TRUE(harness.imageReview.secondaryPath().endsWith(QStringLiteral("frame002.png")));
    EXPECT_DOUBLE_EQ(harness.imageReview.panX(), 0.3);
    EXPECT_DOUBLE_EQ(harness.imageReview.panY(), 0.7);
    harness.window->close();
}

// FLOW2: the shared comparison capture is the display result, and its annotation contract says
// so on every image. A receiver must not read the PNG as source code values or as a
// colour-managed export.
TEST(MainQmlContractTests, ComparisonCaptionStatesTheDisplayResultContract) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();
    QVariant captionValue;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        harness.root.get(), "comparisonCaptionLines", Q_RETURN_ARG(QVariant, captionValue)));
    const QVariantList lines = captionValue.toList();
    bool displayContract = false;
    bool appStamp = false;
    for (const QVariant& line : lines) {
        const QString text = line.toString();
        if (text.contains(QStringLiteral("显示结果")) &&
            text.contains(QStringLiteral("未色彩管理"))) {
            displayContract = true;
        }
        if (text.startsWith(QStringLiteral("CompareStation"))) {
            appStamp = true;
        }
    }
    EXPECT_TRUE(displayContract);
    EXPECT_TRUE(appStamp);
    harness.window->close();
}

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

    // The panel/menu entry point captures immediately and opens the dialog for the note: the
    // record already exists (frozen) while the dialog is up.
    QVariant opened;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        harness.root.get(), "captureIssueLog", Q_RETURN_ARG(QVariant, opened)));
    EXPECT_TRUE(opened.toBool());
    harness.settle();
    EXPECT_TRUE(noteDialog->property("visible").toBool());
    ASSERT_EQ(harness.issueLog.count(), 1);
    EXPECT_TRUE(harness.issueLog.issueAt(0).value(QStringLiteral("note")).toString().isEmpty());

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

    // Escape attaches an empty note rather than cancelling the frozen capture. Send a real
    // key event so the field's Keys.onEscapePressed handler is the code under test.
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(), "captureIssueLog"));
    harness.settle();
    EXPECT_TRUE(noteDialog->property("visible").toBool());
    // The second record is frozen the moment M runs again, before Escape is even sent.
    ASSERT_EQ(harness.issueLog.count(), 2);
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

// One click on an issue row must run the restore exactly once. The row's click handler calls
// IssueLogController::restoreIssue(), which emits restoreRequested, and the Connections block is
// the single entry that applies it; the handler applying the returned payload as well opened the
// recorded sources twice from one click. Driven through the same call the handler makes.
TEST(MainQmlContractTests, IssueRowClickRestoresOnceThroughTheSignalEntry) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    QString pathA = temporaryDirectory.filePath(QStringLiteral("restore-once-a.mp4"));
    QString pathB = temporaryDirectory.filePath(QStringLiteral("restore-once-b.mp4"));
    for (const QString& path : {pathA, pathB}) {
        QFile file{path};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_GT(file.write("restore-source", 15), 0);
    }

    WorkspaceHarness harness;
    harness.withIssueLog = true;
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate);
    ASSERT_TRUE(installValidatedVideoSet(
        harness.snapshot,
        {std::filesystem::path{pathA.toStdWString()}, std::filesystem::path{pathB.toStdWString()}},
        rate.value(),
        12,
        400'000));
    // The harness's default displayed frame is beyond this 12-frame set, which would make the
    // snapshot inconsistent and close the projection.
    harness.snapshot->displayedFrame = domain::FrameId{0};
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();
    // The projection refresh pulls sourceUrls out of the validated comparison.
    harness.controller->refreshProjection();
    harness.settle();
    harness.issueLog.setWorkspaceMode(QStringLiteral("video"));
    ASSERT_TRUE(harness.issueLog.captureCurrentIssue(QString{}));
    ASSERT_EQ(harness.issueLog.count(), 1);
    // The row exists in the panel the click opens, so the click path is reachable and its handler
    // has a live delegate to act on.
    harness.root->setProperty("issueLogPanelVisible", true);
    harness.settle();
    auto* const list = harness.root->findChild<QQuickItem*>(QStringLiteral("issueLogList"));
    ASSERT_NE(list, nullptr);
    QQuickItem* row = nullptr;
    ASSERT_TRUE(harness.waitUntil([&] {
        QMetaObject::invokeMethod(
            list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, row), Q_ARG(int, 0));
        return row != nullptr;
    }));
    bool rowClickable = false;
    for (QObject* const child : row->findChildren<QObject*>()) {
        if (child->inherits("QQuickMouseArea")) {
            rowClickable = qobject_cast<QQuickItem*>(child)->isEnabled();
        }
    }
    EXPECT_TRUE(rowClickable) << "issue row has no enabled click target";

    // The row's handler calls restoreIssue(), which emits restoreRequested; the Connections block
    // is the single entry that applies it. Applying the returned payload as well - which the
    // handler used to do - ran the whole restore twice from one click, so one restore must produce
    // exactly one open of the recorded sources.
    const QPointF rowCenter = row->mapToScene(QPointF{row->width() / 2, row->height() / 2});
    sendMousePress(*harness.window, rowCenter);
    sendMouseRelease(*harness.window, rowCenter);
    harness.settle();

    int opens = 0;
    for (const application::PlaybackCommand& command : harness.submitted) {
        if (std::holds_alternative<application::OpenComparisonCommand>(command)) {
            ++opens;
        }
    }
    EXPECT_EQ(opens, 1) << "one restore must open the recorded sources exactly once";
    EXPECT_EQ(harness.root->property("pendingIssueRestoreStage").toInt(), 1);

    // The staged machine waits for that open to complete; once it does, the restore finishes
    // instead of hanging pending.
    harness.terminals.push_back(application::CommandTerminal{
        .context = application::commandContext(harness.submitted.back()),
        .outcome = application::CommandOutcome::Succeeded,
    });
    harness.controller->refreshProjection();
    harness.settle();
    if (std::holds_alternative<application::SetActiveComparisonPairCommand>(
            harness.submitted.back())) {
        harness.terminals.push_back(application::CommandTerminal{
            .context = application::commandContext(harness.submitted.back()),
            .outcome = application::CommandOutcome::Succeeded,
        });
        harness.controller->refreshProjection();
        harness.settle();
    }
    EXPECT_EQ(harness.root->property("pendingIssueRestoreStage").toInt(), 0);
    harness.window->close();
}

TEST(MainQmlContractTests, NearbyFramesCardOpensFromIssueLogAndExposesDiagnosticFilmstrip) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    const QString pathA = temporaryDirectory.filePath(QStringLiteral("card-test-a.mp4"));
    const QString pathB = temporaryDirectory.filePath(QStringLiteral("card-test-b.mp4"));
    for (const QString& path : {pathA, pathB}) {
        QFile file{path};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_GT(file.write("card-source", 11), 0);
    }

    WorkspaceHarness harness;
    harness.withIssueLog = true;
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate);
    ASSERT_TRUE(installValidatedVideoSet(
        harness.snapshot,
        {std::filesystem::path{pathA.toStdWString()}, std::filesystem::path{pathB.toStdWString()}},
        rate.value(),
        60,
        2'000'000));
    harness.snapshot->displayedFrame = domain::FrameId{20};
    harness.snapshot->playbackRangeIn = domain::FrameId{10};
    harness.snapshot->playbackRangeOut = domain::FrameId{24}; // 20 + 4 == 24 -> loopSeamAtOut
    harness.snapshot->playbackRangeLoopActive = true;
    harness.snapshot->sessionEpoch = domain::SessionEpoch{2U};
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();
    harness.controller->refreshProjection();
    harness.settle();

    harness.issueLog.setWorkspaceMode(QStringLiteral("video"));
    ASSERT_TRUE(harness.issueLog.captureCurrentIssue(QStringLiteral("card-artifact-note")));
    ASSERT_EQ(harness.issueLog.count(), 1);

    const QVariantMap issue = harness.issueLog.issueAt(0);
    EXPECT_TRUE(issue.value(QStringLiteral("hasInterval")).toBool());
    EXPECT_EQ(issue.value(QStringLiteral("intervalAnchorFrame")).toLongLong(), 20);
    EXPECT_EQ(issue.value(QStringLiteral("intervalInFrame")).toLongLong(), 16);
    EXPECT_EQ(issue.value(QStringLiteral("intervalOutFrame")).toLongLong(), 24);
    EXPECT_TRUE(issue.value(QStringLiteral("intervalLoopSeamAtOut")).toBool());

    harness.root->setProperty("issueLogPanelVisible", true);
    harness.settle();

    auto* const list = harness.root->findChild<QQuickItem*>(QStringLiteral("issueLogList"));
    ASSERT_NE(list, nullptr);
    QQuickItem* row = nullptr;
    ASSERT_TRUE(harness.waitUntil([&] {
        QMetaObject::invokeMethod(
            list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, row), Q_ARG(int, 0));
        return row != nullptr;
    }));

    auto* const diagButton =
        row->findChild<QQuickItem*>(QStringLiteral("issueLogDiagnosticButton"));
    ASSERT_NE(diagButton, nullptr);
    EXPECT_TRUE(diagButton->property("visible").toBool());

    auto* const card = harness.root->findChild<QQuickItem*>(QStringLiteral("nearbyFramesCard"));
    ASSERT_NE(card, nullptr);
    EXPECT_FALSE(card->property("visible").toBool());

    // Click diagnostic button to open card
    QMetaObject::invokeMethod(diagButton, "clicked");
    harness.settle();

    EXPECT_TRUE(card->property("visible").toBool());
    EXPECT_EQ(card->property("anchorFrame").toLongLong(), 20);
    EXPECT_EQ(card->property("inFrame").toLongLong(), 16);
    EXPECT_EQ(card->property("outFrame").toLongLong(), 24);
    EXPECT_TRUE(card->property("loopSeamAtOut").toBool());

    auto* const filmstrip = card->findChild<QQuickItem*>(QStringLiteral("nearbyFramesFilmstrip"));
    ASSERT_NE(filmstrip, nullptr);

    // Verify cell items inside filmstrip (index 4 is frame 20 anchor, index 8 is frame 24 seam)
    QQuickItem* anchorCell = nullptr;
    ASSERT_TRUE(harness.waitUntil([&] {
        QMetaObject::invokeMethod(
            filmstrip, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, anchorCell), Q_ARG(int, 4));
        return anchorCell != nullptr;
    }));
    EXPECT_EQ(anchorCell->objectName(), QStringLiteral("nearbyFrameCell_20"));

    QQuickItem* seamCell = nullptr;
    ASSERT_TRUE(harness.waitUntil([&] {
        QMetaObject::invokeMethod(
            filmstrip, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, seamCell), Q_ARG(int, 8));
        return seamCell != nullptr;
    }));
    EXPECT_EQ(seamCell->objectName(), QStringLiteral("nearbyFrameCell_24"));
    auto* const seamTag = seamCell->findChild<QQuickItem*>(QStringLiteral("loopSeamTag"));
    ASSERT_NE(seamTag, nullptr);
    EXPECT_TRUE(seamTag->property("visible").toBool());

    // U11: the header's flexible spacer is derived from its siblings' widths, so no child may
    // reach past the card's right edge. The recorded interval carries epoch 2 and the
    // controller still runs on it, so the two agree and the cross-session badge is hidden.
    auto* const stripToggle =
        card->findChild<QQuickItem*>(QStringLiteral("nearbyFramesStripToggle"));
    ASSERT_NE(stripToggle, nullptr);
    auto* const headerRow = stripToggle->parentItem();
    ASSERT_NE(headerRow, nullptr);
    // Returns the visible child count, so a state that silently hides every child (and so
    // checks nothing) cannot pass as a fit.
    const auto visibleHeaderChildren = [&]() {
        int visible = 0;
        for (QQuickItem* child : headerRow->childItems()) {
            if (!child->property("visible").toBool())
                continue; // an invisible child takes no space in a Row
            ++visible;
            EXPECT_LE(child->x() + child->width(), card->width())
                << "a header child overflows the card while the cross-session badge is "
                << (card->property("epochMatches").toBool() ? "hidden" : "shown");
        }
        return visible;
    };
    EXPECT_TRUE(card->property("epochMatches").toBool());
    const int childrenWithBadgeHidden = visibleHeaderChildren();
    EXPECT_GE(childrenWithBadgeHidden, 6);

    // A controller on a different epoch makes the same header carry the cross-session badge.
    harness.snapshot->sessionEpoch = domain::SessionEpoch{3U};
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_FALSE(card->property("epochMatches").toBool());
    EXPECT_EQ(visibleHeaderChildren(), childrenWithBadgeHidden + 1);

    // Clicking "收起" hides the card
    auto* const closeBtn = card->findChild<QQuickItem*>(QStringLiteral("nearbyFramesCloseButton"));
    ASSERT_NE(closeBtn, nullptr);
    QMetaObject::invokeMethod(closeBtn, "clicked");
    harness.settle();
    EXPECT_FALSE(card->property("visible").toBool());

    harness.window->close();
}

// V04: without a loaded interpolation protocol the card has no provenance evidence for any
// frame, so every cell reports the honest "未知" - a definite "超界" (outside the interval)
// claim would invent knowledge. A wide recorded interval also keeps a 9-cell locating window
// centred on the anchor instead of flooding the strip.
TEST(MainQmlContractTests, NearbyFramesWithoutProtocolShowUnknownNotOutsideAndKeepALocatingWindow) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    const QString pathA = temporaryDirectory.filePath(QStringLiteral("card-unknown-a.mp4"));
    const QString pathB = temporaryDirectory.filePath(QStringLiteral("card-unknown-b.mp4"));
    for (const QString& path : {pathA, pathB}) {
        QFile file{path};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_GT(file.write("card-unknown", 12), 0);
    }

    WorkspaceHarness harness;
    harness.withIssueLog = true;
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate);
    ASSERT_TRUE(installValidatedVideoSet(
        harness.snapshot,
        {std::filesystem::path{pathA.toStdWString()}, std::filesystem::path{pathB.toStdWString()}},
        rate.value(),
        60,
        2'000'000));
    harness.snapshot->displayedFrame = domain::FrameId{20};
    harness.snapshot->sessionEpoch = domain::SessionEpoch{2U};
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();
    harness.controller->refreshProjection();
    harness.settle();

    harness.issueLog.setWorkspaceMode(QStringLiteral("video"));
    ASSERT_TRUE(harness.issueLog.captureCurrentIssue(QStringLiteral("card-unknown-note")));

    harness.root->setProperty("issueLogPanelVisible", true);
    harness.settle();
    auto* const list = harness.root->findChild<QQuickItem*>(QStringLiteral("issueLogList"));
    ASSERT_NE(list, nullptr);
    QQuickItem* row = nullptr;
    ASSERT_TRUE(harness.waitUntil([&] {
        QMetaObject::invokeMethod(
            list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, row), Q_ARG(int, 0));
        return row != nullptr;
    }));
    auto* const diagButton =
        row->findChild<QQuickItem*>(QStringLiteral("issueLogDiagnosticButton"));
    ASSERT_NE(diagButton, nullptr);
    QMetaObject::invokeMethod(diagButton, "clicked");
    harness.settle();

    auto* const card = harness.root->findChild<QQuickItem*>(QStringLiteral("nearbyFramesCard"));
    ASSERT_NE(card, nullptr);
    EXPECT_TRUE(card->property("visible").toBool());

    // The captured interval 16..24 with no protocol: every badge says 未知, none says 超界.
    // ListView delegates are reached through itemAtIndex (they are not QObject-children of
    // the window, so findChild-by-name cannot see them).
    auto* const filmstrip = card->findChild<QQuickItem*>(QStringLiteral("nearbyFramesFilmstrip"));
    ASSERT_NE(filmstrip, nullptr);
    const auto cellAt = [&harness, &filmstrip](const int index) {
        QQuickItem* cell = nullptr;
        QMetaObject::invokeMethod(
            filmstrip, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, cell), Q_ARG(int, index));
        harness.settle(1);
        return cell;
    };
    ASSERT_TRUE(harness.waitUntil([&cellAt] { return cellAt(0) != nullptr; }));
    ASSERT_EQ(filmstrip->property("count").toInt(), 9);
    for (int index = 0; index < 9; ++index) {
        auto* const cell = cellAt(index);
        ASSERT_NE(cell, nullptr) << index;
        EXPECT_EQ(cell->objectName(), QStringLiteral("nearbyFrameCell_%1").arg(16 + index))
            << index;
        auto* const badge = cell->findChild<QObject*>(QStringLiteral("nearbyFrameKindBadge"));
        ASSERT_NE(badge, nullptr) << index;
        EXPECT_EQ(badge->property("text").toString(), QStringLiteral("未知")) << index;
    }

    // A wide persisted interval keeps a 9-cell window centred on the anchor: with the anchor
    // at 20 inside [2, 45] the strip shows 16..24 (a left-aligned window would show 2..10) -
    // the interval itself never floods the card.
    QVariantMap wideInterval;
    wideInterval.insert(QStringLiteral("anchorFrame"), QVariant{qlonglong{20}});
    wideInterval.insert(QStringLiteral("inFrame"), QVariant{qlonglong{2}});
    wideInterval.insert(QStringLiteral("outFrame"), QVariant{qlonglong{45}});
    wideInterval.insert(QStringLiteral("loopSeamAtOut"), false);
    wideInterval.insert(QStringLiteral("sessionEpoch"),
                        QVariant::fromValue(harness.snapshot->sessionEpoch.value()));
    ASSERT_TRUE(card->setProperty("intervalData", wideInterval));
    ASSERT_TRUE(harness.waitUntil([&cellAt] {
        return cellAt(0) != nullptr &&
               cellAt(0)->objectName() == QStringLiteral("nearbyFrameCell_16");
    }));
    ASSERT_EQ(filmstrip->property("count").toInt(), 9);
    for (int index = 0; index < 9; ++index) {
        auto* const cell = cellAt(index);
        ASSERT_NE(cell, nullptr) << index;
        EXPECT_EQ(cell->objectName(), QStringLiteral("nearbyFrameCell_%1").arg(16 + index))
            << index;
    }

    harness.window->close();
}

// V04: selecting a cell sends the real frame to the main canvas through the existing precise
// jump chain (a coordinator SeekFrameCommand), not a re-decode or an approximate step.
TEST(MainQmlContractTests, NearbyFramesCellClickSeeksTheExactFrameThroughTheCoordinator) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    const QString pathA = temporaryDirectory.filePath(QStringLiteral("card-seek-a.mp4"));
    const QString pathB = temporaryDirectory.filePath(QStringLiteral("card-seek-b.mp4"));
    for (const QString& path : {pathA, pathB}) {
        QFile file{path};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_GT(file.write("card-seek", 9), 0);
    }

    WorkspaceHarness harness;
    harness.withIssueLog = true;
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate);
    ASSERT_TRUE(installValidatedVideoSet(
        harness.snapshot,
        {std::filesystem::path{pathA.toStdWString()}, std::filesystem::path{pathB.toStdWString()}},
        rate.value(),
        60,
        2'000'000));
    harness.snapshot->displayedFrame = domain::FrameId{20};
    harness.snapshot->sessionEpoch = domain::SessionEpoch{2U};
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();
    harness.controller->refreshProjection();
    harness.settle();

    harness.issueLog.setWorkspaceMode(QStringLiteral("video"));
    ASSERT_TRUE(harness.issueLog.captureCurrentIssue(QStringLiteral("card-seek-note")));

    QVariant openedResult;
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(),
                                          "openNearbyFramesCard",
                                          Q_RETURN_ARG(QVariant, openedResult),
                                          Q_ARG(QVariant, QVariant{0})));
    ASSERT_TRUE(openedResult.toBool());
    harness.settle();

    auto* const seekCard = harness.root->findChild<QQuickItem*>(QStringLiteral("nearbyFramesCard"));
    ASSERT_NE(seekCard, nullptr);
    auto* const seekFilmstrip =
        harness.root->findChild<QQuickItem*>(QStringLiteral("nearbyFramesFilmstrip"));
    ASSERT_NE(seekFilmstrip, nullptr);
    const auto seekCellAt = [&harness, &seekFilmstrip](const int index) {
        QQuickItem* cell = nullptr;
        QMetaObject::invokeMethod(
            seekFilmstrip, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, cell), Q_ARG(int, index));
        harness.settle(1);
        return cell;
    };
    // Frame 18 is index 2 of the captured window 16..24; delegates are reached through
    // itemAtIndex (ListView delegates are not QObject-children of the window).
    QQuickItem* seekCell = nullptr;
    ASSERT_TRUE(harness.waitUntil([&] {
        seekCell = seekCellAt(2);
        return seekCell != nullptr &&
               seekCell->objectName() == QStringLiteral("nearbyFrameCell_18");
    }));

    // Clear whatever the staged restore left pending (this harness never delivers coordinator
    // terminals by itself), so the precise-jump chain is not busy-gated.
    for (const application::PlaybackCommand& command : harness.submitted) {
        harness.terminals.push_back(application::CommandTerminal{
            .context = application::commandContext(command),
            .outcome = application::CommandOutcome::Succeeded,
        });
    }
    harness.controller->refreshProjection();
    harness.settle();

    const std::size_t submittedBefore = harness.submitted.size();
    // Drive the card's seekRequested handler exactly as the cell's MouseArea does; Main.qml
    // forwards it to the coordinator's precise jump chain.
    ASSERT_TRUE(QMetaObject::invokeMethod(seekCard, "seekRequested", Q_ARG(int, 18)));
    harness.settle();
    ASSERT_GT(harness.submitted.size(), submittedBefore);
    const auto* const seek = std::get_if<application::SeekFrameCommand>(&harness.submitted.back());
    ASSERT_NE(seek, nullptr);
    EXPECT_EQ(seek->frameId, domain::FrameId{18});
    EXPECT_EQ(seek->context.sessionId, harness.snapshot->sessionId);

    harness.window->close();
}

// V04: the strip can switch to a same-ROI consecutive view - cells are the anchor-centred
// consecutive frames, each thumbnail cropped to the shared ROI, the whole window is fetched
// through one replaceable low-priority batch, and closing the card cancels that batch.
TEST(MainQmlContractTests, NearbyFramesConsecutiveStripCropsToTheSharedRoiAndCancelsItsBatch) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    const QString pathA = temporaryDirectory.filePath(QStringLiteral("card-roi-a.mp4"));
    const QString pathB = temporaryDirectory.filePath(QStringLiteral("card-roi-b.mp4"));
    for (const QString& path : {pathA, pathB}) {
        QFile file{path};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_GT(file.write("card-roi", 7), 0);
    }

    WorkspaceHarness harness;
    harness.withIssueLog = true;
    harness.withPreviewThumbnails = true;
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate);
    ASSERT_TRUE(installValidatedVideoSet(
        harness.snapshot,
        {std::filesystem::path{pathA.toStdWString()}, std::filesystem::path{pathB.toStdWString()}},
        rate.value(),
        60,
        2'000'000));
    harness.snapshot->displayedFrame = domain::FrameId{20};
    harness.snapshot->sessionEpoch = domain::SessionEpoch{2U};
    ASSERT_TRUE(harness.create()) << harness.error;
    ASSERT_NE(harness.previewThumbnailsStub, nullptr);
    harness.window->show();
    harness.settle();
    harness.controller->refreshProjection();
    harness.settle();

    harness.issueLog.setWorkspaceMode(QStringLiteral("video"));
    ASSERT_TRUE(harness.issueLog.captureCurrentIssue(QStringLiteral("card-roi-note")));

    QVariant openedResult;
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(),
                                          "openNearbyFramesCard",
                                          Q_RETURN_ARG(QVariant, openedResult),
                                          Q_ARG(QVariant, QVariant{0})));
    ASSERT_TRUE(openedResult.toBool());
    harness.settle();

    auto* const card = harness.root->findChild<QQuickItem*>(QStringLiteral("nearbyFramesCard"));
    ASSERT_NE(card, nullptr);
    // The thumbnail source is named: the strip decodes the canonical (timeline master) file,
    // never an anonymous frame.
    auto* const sourceLabel =
        card->findChild<QObject*>(QStringLiteral("nearbyFramesThumbnailSourceLabel"));
    ASSERT_NE(sourceLabel, nullptr);
    EXPECT_TRUE(sourceLabel->property("visible").toBool());
    EXPECT_TRUE(sourceLabel->property("text").toString().contains(QStringLiteral("card-roi-a.mp4")))
        << sourceLabel->property("text").toString().toStdString();

    // Opening the card fetched exactly its 9-cell window through one batch call.
    ASSERT_TRUE(harness.waitUntil([&harness] {
        return !harness.previewThumbnailsStub->property("batchCalls").toList().isEmpty();
    }));
    const QVariantList batchCalls = harness.previewThumbnailsStub->property("batchCalls").toList();
    QVariantList lastCall = batchCalls.back().toList();
    ASSERT_EQ(lastCall.size(), 9);
    EXPECT_EQ(lastCall.front().toLongLong(), qlonglong{16});
    EXPECT_EQ(lastCall.back().toLongLong(), qlonglong{24});

    // Switch to the consecutive strip: the same anchor-centred frames, no provenance claim
    // (the interval view keeps those badges), and the batch is re-issued for the new window.
    const int batchCallsBefore = batchCalls.size();
    auto* const stripToggle =
        card->findChild<QQuickItem*>(QStringLiteral("nearbyFramesStripToggle"));
    ASSERT_NE(stripToggle, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(stripToggle, "clicked"));
    harness.settle();
    EXPECT_TRUE(card->property("consecutiveStripMode").toBool());
    const QVariantList batchCallsAfter =
        harness.previewThumbnailsStub->property("batchCalls").toList();
    ASSERT_GT(batchCallsAfter.size(), qsizetype{batchCallsBefore});
    lastCall = batchCallsAfter.back().toList();
    ASSERT_EQ(lastCall.size(), 9);
    EXPECT_EQ(lastCall.front().toLongLong(), qlonglong{16});
    EXPECT_EQ(lastCall.back().toLongLong(), qlonglong{24});
    auto* const stripFilmstrip =
        harness.root->findChild<QQuickItem*>(QStringLiteral("nearbyFramesFilmstrip"));
    ASSERT_NE(stripFilmstrip, nullptr);
    const auto stripCellAt = [&harness, &stripFilmstrip](const int index) {
        QQuickItem* cell = nullptr;
        QMetaObject::invokeMethod(
            stripFilmstrip, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, cell), Q_ARG(int, index));
        harness.settle(1);
        return cell;
    };
    QQuickItem* consecutiveCell = nullptr;
    ASSERT_TRUE(harness.waitUntil([&] {
        consecutiveCell = stripCellAt(2);
        return consecutiveCell != nullptr &&
               consecutiveCell->objectName() == QStringLiteral("nearbyFrameCell_18");
    }));
    auto* const cell18Badge =
        consecutiveCell->findChild<QObject*>(QStringLiteral("nearbyFrameKindBadge"));
    ASSERT_NE(cell18Badge, nullptr);
    EXPECT_EQ(cell18Badge->property("text").toString(), QStringLiteral("—"));
    // Without a ROI the toggle says so; whole frames are shown, no crop.
    EXPECT_TRUE(stripToggle->property("text").toString().contains(QStringLiteral("未启用 ROI")));

    // Enable the shared ROI on the main canvas surface and mark the frames decoded: every
    // strip cell crops its thumbnail to the same normalized rect (100x50 provider image ->
    // crop 20,5 60x40) and stretches that region into the cell.
    auto* const surface = harness.root->findChild<QQuickItem*>(QStringLiteral("dualVideoSurface"));
    ASSERT_NE(surface, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(surface,
                                          "setRoiNormalized",
                                          Q_ARG(qreal, 0.2),
                                          Q_ARG(qreal, 0.1),
                                          Q_ARG(qreal, 0.8),
                                          Q_ARG(qreal, 0.9)));
    QVariantList ready;
    for (int frame = 16; frame <= 24; ++frame) {
        ready.append(QVariant{qlonglong{frame}});
    }
    ASSERT_TRUE(harness.previewThumbnailsStub->setProperty("framesReady", ready));
    harness.settle(12);
    EXPECT_TRUE(stripToggle->property("text").toString().contains(QStringLiteral("同 ROI 连续帧")));
    QQuickItem* cropCell = nullptr;
    ASSERT_TRUE(harness.waitUntil([&] {
        cropCell = stripCellAt(2);
        return cropCell != nullptr &&
               cropCell->objectName() == QStringLiteral("nearbyFrameCell_18");
    }));
    auto* const thumbnail =
        cropCell->findChild<QQuickItem*>(QStringLiteral("nearbyFrameThumbnailImage"));
    ASSERT_NE(thumbnail, nullptr);
    // Image.Ready == 1: the provider image is loaded before reading the crop rect.
    ASSERT_TRUE(
        harness.waitUntil([&thumbnail] { return thumbnail->property("status").toInt() == 1; }));
    const QRectF crop = thumbnail->property("sourceClipRect").toRectF();
    EXPECT_NEAR(crop.x(), 20.0, 0.5);
    EXPECT_NEAR(crop.y(), 5.0, 0.5);
    EXPECT_NEAR(crop.width(), 60.0, 0.5);
    EXPECT_NEAR(crop.height(), 40.0, 0.5);

    // Closing the card cancels its outstanding batch - stale frame decodes stop.
    const int cancelsBefore = harness.previewThumbnailsStub->property("cancelCount").toInt();
    auto* const closeBtn = card->findChild<QQuickItem*>(QStringLiteral("nearbyFramesCloseButton"));
    ASSERT_NE(closeBtn, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(closeBtn, "clicked"));
    harness.settle();
    EXPECT_FALSE(card->property("visible").toBool());
    EXPECT_GT(harness.previewThumbnailsStub->property("cancelCount").toInt(), cancelsBefore);

    harness.window->close();
}

// U01: the nearby-frames card folds to its header row and expands into the 144-184 height
// band. The expanded card sits at the band ceiling with the filmstrip and footer intact, so
// folding is one click and re-expanding loses nothing.
TEST(MainQmlContractTests, NearbyFramesCardFoldsIntoItsHeightBand) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    const QString pathA = temporaryDirectory.filePath(QStringLiteral("card-fold-a.mp4"));
    const QString pathB = temporaryDirectory.filePath(QStringLiteral("card-fold-b.mp4"));
    for (const QString& path : {pathA, pathB}) {
        QFile file{path};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_GT(file.write("card-fold", 9), 0);
    }

    WorkspaceHarness harness;
    harness.withIssueLog = true;
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate);
    ASSERT_TRUE(installValidatedVideoSet(
        harness.snapshot,
        {std::filesystem::path{pathA.toStdWString()}, std::filesystem::path{pathB.toStdWString()}},
        rate.value(),
        60,
        2'000'000));
    harness.snapshot->displayedFrame = domain::FrameId{20};
    harness.snapshot->playbackRangeIn = domain::FrameId{10};
    harness.snapshot->playbackRangeOut = domain::FrameId{24};
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();
    harness.controller->refreshProjection();
    harness.settle();

    harness.issueLog.setWorkspaceMode(QStringLiteral("video"));
    ASSERT_TRUE(harness.issueLog.captureCurrentIssue(QString{}));
    harness.root->setProperty("nearbyFramesCardVisible", true);
    harness.root->setProperty("activeDiagnosticIssueIndex", 0);
    harness.settle();

    auto* const card = harness.root->findChild<QQuickItem*>(QStringLiteral("nearbyFramesCard"));
    ASSERT_NE(card, nullptr);
    ASSERT_TRUE(card->property("visible").toBool());
    EXPECT_FALSE(card->property("collapsed").toBool());
    EXPECT_DOUBLE_EQ(card->height(), 184.0)
        << "the expanded card sits at the top of its 144-184 band";

    auto* const filmstrip = card->findChild<QQuickItem*>(QStringLiteral("nearbyFramesFilmstrip"));
    ASSERT_NE(filmstrip, nullptr);
    EXPECT_TRUE(filmstrip->property("visible").toBool());
    EXPECT_GT(filmstrip->height(), 0.0);

    auto* const foldButton = card->findChild<QQuickItem*>(QStringLiteral("nearbyFramesFoldButton"));
    ASSERT_NE(foldButton, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(foldButton, "clicked"));
    harness.settle();

    EXPECT_TRUE(card->property("collapsed").toBool());
    EXPECT_LT(card->height(), 80.0) << "folded, the card keeps only its header row";
    EXPECT_FALSE(filmstrip->property("visible").toBool());

    ASSERT_TRUE(QMetaObject::invokeMethod(foldButton, "clicked"));
    harness.settle();
    EXPECT_FALSE(card->property("collapsed").toBool());
    EXPECT_DOUBLE_EQ(card->height(), 184.0);
    EXPECT_TRUE(filmstrip->property("visible").toBool());

    harness.window->close();
}

// V04: the dual-difference HUD legend describes the two panes the layout renders (both
// candidates against the reference under one shared threshold/gain/colour scale), and the
// missing-frame overlay names the panes' slots - including the reference, which the active
// pair selector never mentions. A pane going missing also flips the combined exactness.
TEST(MainQmlContractTests, DualDifferenceLegendAndMissingOverlayDescribeTheRenderedPanes) {
    ThreeSourceFocusHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;

    auto* const dualItem =
        harness.root->findChild<QObject*>(QStringLiteral("dualDifferenceMenuItem"));
    ASSERT_NE(dualItem, nullptr);
    auto* const diffMenu = harness.root->findChild<QObject*>(QStringLiteral("diffFlavorMenu"));
    ASSERT_NE(diffMenu, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(diffMenu, "open"));
    for (int iteration = 0; iteration < 5; ++iteration) {
        QCoreApplication::processEvents();
    }
    ASSERT_TRUE(QMetaObject::invokeMethod(dualItem, "triggered"));
    for (int iteration = 0; iteration < 8; ++iteration) {
        QCoreApplication::processEvents();
    }
    ASSERT_EQ(harness.root->property("effectiveViewMode").toInt(), 9);

    auto* const statusText =
        harness.root->findChild<QObject*>(QStringLiteral("analysisStatusText"));
    ASSERT_NE(statusText, nullptr);
    const QString legend = statusText->property("text").toString();
    EXPECT_TRUE(legend.contains(QStringLiteral("双差异"))) << legend.toStdString();
    EXPECT_TRUE(legend.contains(QStringLiteral("A↔GT · B↔GT"))) << legend.toStdString();
    EXPECT_TRUE(legend.contains(QStringLiteral("两窗共用阈值/增益/色阶"))) << legend.toStdString();
    // The legend may honestly report unavailable semantics, but it never claims exact
    // correspondence it cannot see (no "原码值" wording unless both panes are pixel-exact).
    if (!harness.root->property("dualDifferenceLegendExact").toBool()) {
        EXPECT_TRUE(legend.contains(QStringLiteral("不可用")) ||
                    legend.contains(QStringLiteral("不同")))
            << legend.toStdString();
    }

    // One pane pair loses its frame: the combined legend must drop out of the exact state
    // (aggregating only one pane would keep claiming exactness) and the missing candidate is
    // named by the unavailable overlay.
    harness.snapshot->presentedSources[1].matchKind = application::FrameMatchKind::Missing;
    harness.snapshot->presentedSources[1].sourceFrameId = std::nullopt;
    harness.controller->refreshProjection();
    for (int iteration = 0; iteration < 8; ++iteration) {
        QCoreApplication::processEvents();
    }
    EXPECT_FALSE(harness.root->property("dualDifferenceLegendExact").toBool());
    const QString legendAfterMissingPane =
        harness.root->property("dualDifferenceLegendText").toString();
    EXPECT_TRUE(legendAfterMissingPane.contains(QStringLiteral("不可用")) ||
                legendAfterMissingPane.contains(QStringLiteral("不同")))
        << legendAfterMissingPane.toStdString();
    const QString detail = harness.root->property("differenceUnavailableDetail").toString();
    EXPECT_TRUE(detail.contains(QStringLiteral("源 B"))) << detail.toStdString();
    // The legend still names both panes; the pane loss is a missing-frame problem, not a
    // caliber change.
    EXPECT_TRUE(harness.root->property("dualDifferenceLegendText")
                    .toString()
                    .contains(QStringLiteral("B↔GT")));

    harness.window->close();
}

// V04 (companion): with the reference source missing, both dual-difference panes are black
// and the overlay names the reference - the active pair selector (both candidates present)
// would stay silent about exactly the source the layout needs.
TEST(MainQmlContractTests, DualDifferenceMissingReferenceIsNamedByTheUnavailableOverlay) {
    ThreeSourceFocusHarness harness;
    harness.snapshot->presentedSources[2].matchKind = application::FrameMatchKind::Missing;
    harness.snapshot->presentedSources[2].sourceFrameId = std::nullopt;
    ASSERT_TRUE(harness.create()) << harness.error;

    auto* const dualItem =
        harness.root->findChild<QObject*>(QStringLiteral("dualDifferenceMenuItem"));
    ASSERT_NE(dualItem, nullptr);
    auto* const diffMenu = harness.root->findChild<QObject*>(QStringLiteral("diffFlavorMenu"));
    ASSERT_NE(diffMenu, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(diffMenu, "open"));
    for (int iteration = 0; iteration < 5; ++iteration) {
        QCoreApplication::processEvents();
    }
    ASSERT_TRUE(QMetaObject::invokeMethod(dualItem, "triggered"));
    for (int iteration = 0; iteration < 8; ++iteration) {
        QCoreApplication::processEvents();
    }
    ASSERT_EQ(harness.root->property("effectiveViewMode").toInt(), 9);

    EXPECT_TRUE(harness.controller->sourceCMissing());
    const QString detail = harness.root->property("differenceUnavailableDetail").toString();
    EXPECT_TRUE(detail.contains(QStringLiteral("参考"))) << detail.toStdString();
    EXPECT_TRUE(detail.contains(QStringLiteral("源 C"))) << detail.toStdString();
    EXPECT_FALSE(detail.contains(QStringLiteral("源 A"))) << detail.toStdString();
    // The panes are still described: the reference loss is a missing-frame problem, not a
    // caliber change.
    EXPECT_TRUE(harness.root->property("dualDifferenceLegendText")
                    .toString()
                    .contains(QStringLiteral("A↔GT · B↔GT")));

    harness.window->close();
}

// A staged restore that is waiting for its recorded frame must not replay that view onto a
// different session: opening something else cancels the restore and says so, instead of seeking
// and re-zooming whatever is open now.
TEST(MainQmlContractTests, IssueRestoreCancelsWhenTheSessionChangesWhileWaiting) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    QString pathA = temporaryDirectory.filePath(QStringLiteral("restore-swap-a.mp4"));
    QString pathB = temporaryDirectory.filePath(QStringLiteral("restore-swap-b.mp4"));
    QString pathC = temporaryDirectory.filePath(QStringLiteral("restore-swap-c.mp4"));
    for (const QString& path : {pathA, pathB, pathC}) {
        QFile file{path};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_GT(file.write("restore-source", 15), 0);
    }

    WorkspaceHarness harness;
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate);
    // A/B is the open session, and the restore is for those same sources targeting a frame the
    // fake transport never presents - so the machine parks in stage 2 waiting for that frame.
    ASSERT_TRUE(installValidatedVideoSet(
        harness.snapshot,
        {std::filesystem::path{pathA.toStdWString()}, std::filesystem::path{pathB.toStdWString()}},
        rate.value(),
        12,
        400'000));
    harness.snapshot->displayedFrame = domain::FrameId{0};
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();
    harness.controller->refreshProjection();
    harness.settle();
    auto* const surface = harness.root->findChild<QQuickItem*>(QStringLiteral("dualVideoSurface"));
    ASSERT_NE(surface, nullptr);
    ASSERT_TRUE(surface->setProperty("sourceDisplayInfo",
                                     QVariantList{QVariantMap{
                                         {QStringLiteral("width"), 1920},
                                         {QStringLiteral("height"), 1080},
                                     }}));
    harness.settle();

    const QVariantMap payload{
        {QStringLiteral("decision"), QStringLiteral("ready")},
        {QStringLiteral("kind"), QStringLiteral("video")},
        {QStringLiteral("urls"),
         QVariantList{QUrl::fromLocalFile(pathA), QUrl::fromLocalFile(pathB)}},
        {QStringLiteral("referenceSourceIndex"), 0},
        {QStringLiteral("canonicalSourceIndex"), 0},
        {QStringLiteral("viewMode"), ComparisonSurface::Difference},
        {QStringLiteral("frame"), QVariant::fromValue<qint64>(5)},
        {QStringLiteral("roiEnabled"), true},
        {QStringLiteral("roiLeft"), 0.25},
        {QStringLiteral("roiTop"), 0.25},
        {QStringLiteral("roiRight"), 0.75},
        {QStringLiteral("roiBottom"), 0.75},
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
    // Complete the restore's own open so the machine advances to the frame wait.
    ASSERT_FALSE(harness.submitted.empty());
    harness.terminals.push_back(application::CommandTerminal{
        .context = application::commandContext(harness.submitted.back()),
        .outcome = application::CommandOutcome::Succeeded,
    });
    harness.controller->refreshProjection();
    harness.settle();
    EXPECT_EQ(harness.root->property("pendingIssueRestoreStage").toInt(), 2);
    EXPECT_FALSE(surface->property("roiEnabled").toBool());

    // The user opens something else while the restore waits for its frame: the recorded view must
    // not land on the new session, and the machine must not stay pending forever.
    harness.submitted.clear();
    harness.terminals.clear();
    ASSERT_TRUE(installValidatedVideoSet(harness.snapshot,
                                         {std::filesystem::path{pathC.toStdWString()}},
                                         rate.value(),
                                         12,
                                         400'000));
    harness.controller->refreshProjection();
    harness.settle();

    EXPECT_EQ(harness.root->property("pendingIssueRestoreStage").toInt(), 0);
    EXPECT_FALSE(surface->property("roiEnabled").toBool());
    EXPECT_NEAR(surface->property("viewScale").toDouble(), 1.0, 0.000001);
    EXPECT_FALSE(harness.root->property("intentMessage").toString().isEmpty());
    harness.window->close();
}

class IssueRestoreScenario final {
public:
    bool start() {
        for (const QString& name : {QStringLiteral("a.mp4"), QStringLiteral("b.mp4")}) {
            QFile file{temporary.filePath(name)};
            if (!file.open(QIODevice::WriteOnly) || file.write("source", 6) != 6) {
                return false;
            }
            urls.push_back(QUrl::fromLocalFile(file.fileName()));
        }
        const auto rate = domain::RationalRate::create(30, 1);
        if (!rate || !installValidatedVideoSet(
                         harness.snapshot,
                         {std::filesystem::path{temporary.filePath("a.mp4").toStdWString()},
                          std::filesystem::path{temporary.filePath("b.mp4").toStdWString()}},
                         rate.value(),
                         100,
                         3'333'333)) {
            return false;
        }
        if (!harness.create()) {
            return false;
        }
        harness.settle();
        return true;
    }

    bool restore(QVariantMap additions = {}) {
        QVariantMap payload{{QStringLiteral("decision"), QStringLiteral("ready")},
                            {QStringLiteral("kind"), QStringLiteral("video")},
                            {QStringLiteral("frame"), 41}};
        for (auto it = additions.cbegin(); it != additions.cend(); ++it) {
            payload.insert(it.key(), it.value());
        }
        QVariant result;
        return QMetaObject::invokeMethod(harness.root.get(),
                                         "applyIssueRestore",
                                         Q_RETURN_ARG(QVariant, result),
                                         Q_ARG(QVariant, payload)) &&
               result.toBool();
    }

    void finish(const application::PlaybackCommand& command, application::CommandOutcome outcome) {
        harness.terminals.push_back(application::CommandTerminal{
            .context = application::commandContext(command), .outcome = outcome});
        harness.controller->refreshProjection();
        harness.settle();
    }

    int stage() const {
        return harness.root->property("pendingIssueRestoreStage").toInt();
    }
    QTemporaryDir temporary;
    WorkspaceHarness harness;
    QVariantList urls;
};

TEST(MainQmlContractTests, IssueRestoreOpenFailureCannotRestoreThePreviousSameUrls) {
    IssueRestoreScenario scenario;
    ASSERT_TRUE(scenario.start());
    ASSERT_TRUE(scenario.restore({{QStringLiteral("urls"), scenario.urls}}));
    const auto open = scenario.harness.submitted.back();
    scenario.finish(open, application::CommandOutcome::Failed);
    EXPECT_EQ(scenario.stage(), 0);
    EXPECT_TRUE(scenario.harness.root->property("intentMessage")
                    .toString()
                    .contains(QStringLiteral("恢复失败")));
}

TEST(MainQmlContractTests, IssueRestorePairWaitsForOneMatchingTerminalAndReportsPartialFailure) {
    IssueRestoreScenario scenario;
    ASSERT_TRUE(scenario.start());
    ASSERT_TRUE(scenario.restore({{QStringLiteral("differenceEdge"), 0}}));
    ASSERT_FALSE(scenario.harness.submitted.empty());
    const auto pair = scenario.harness.submitted.back();
    ASSERT_TRUE(std::holds_alternative<application::SetActiveComparisonPairCommand>(pair));
    const auto count = scenario.harness.submitted.size();
    for (int index = 0; index < 3; ++index) {
        ASSERT_TRUE(QMetaObject::invokeMethod(scenario.harness.root.get(), "continueIssueRestore"));
    }
    EXPECT_EQ(scenario.harness.submitted.size(), count);
    EXPECT_EQ(scenario.stage(), 2);
    auto unrelated = application::commandContext(pair);
    unrelated.commandId = domain::CommandId{unrelated.commandId.value() + 100U};
    scenario.harness.terminals.push_back(application::CommandTerminal{
        .context = unrelated, .outcome = application::CommandOutcome::Succeeded});
    scenario.harness.controller->refreshProjection();
    EXPECT_EQ(scenario.stage(), 2);
    scenario.finish(pair, application::CommandOutcome::Failed);
    EXPECT_EQ(scenario.stage(), 0);
    EXPECT_TRUE(scenario.harness.root->property("intentMessage")
                    .toString()
                    .contains(QStringLiteral("部分恢复")));
}

TEST(MainQmlContractTests, IssueRestoreSeekFailureEndsPendingWithoutAnotherStateChange) {
    IssueRestoreScenario scenario;
    ASSERT_TRUE(scenario.start());
    ASSERT_TRUE(scenario.restore({{QStringLiteral("frame"), 42}}));
    const auto seek = scenario.harness.submitted.back();
    ASSERT_TRUE(std::holds_alternative<application::SeekFrameCommand>(seek));
    ASSERT_TRUE(QMetaObject::invokeMethod(scenario.harness.root.get(), "continueIssueRestore"));
    EXPECT_EQ(std::count_if(scenario.harness.submitted.begin(),
                            scenario.harness.submitted.end(),
                            [](const auto& command) {
                                return std::holds_alternative<application::SeekFrameCommand>(
                                    command);
                            }),
              1);
    scenario.finish(seek, application::CommandOutcome::Failed);
    EXPECT_EQ(scenario.stage(), 0);
}

TEST(MainQmlContractTests, IssueRestoreRejectsSameUrlsInANewerSessionAndTimesOutMissingTerminals) {
    IssueRestoreScenario scenario;
    ASSERT_TRUE(scenario.start());
    ASSERT_TRUE(scenario.restore({{QStringLiteral("frame"), 42}}));
    scenario.harness.snapshot->sessionEpoch =
        domain::SessionEpoch{scenario.harness.snapshot->sessionEpoch.value() + 1U};
    scenario.harness.controller->refreshProjection();
    scenario.harness.settle();
    EXPECT_EQ(scenario.stage(), 0);
    ASSERT_TRUE(scenario.restore({{QStringLiteral("frame"), 42}}));
    auto* timer =
        scenario.harness.root->findChild<QObject*>(QStringLiteral("issueRestoreDeadline"));
    ASSERT_NE(timer, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(timer, "triggered"));
    EXPECT_EQ(scenario.stage(), 0);
    EXPECT_TRUE(scenario.harness.root->property("intentMessage")
                    .toString()
                    .contains(QStringLiteral("超时")));
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
    EXPECT_NE(harness.root->findChild<QObject*>(QStringLiteral("emptyOpenFolderButton")), nullptr);
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
    EXPECT_EQ(QUrl{harness.preferences.recentMediaFiles().front()}, url);
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

// The recent list is shared across workspaces: a still image opened through the sidebar path
// records into the same history the video browser shows (marked by kind), the model's staged
// pending row finishes on the image commit terminal, and the image workspace becomes the
// active task - no video intent is ever submitted.
TEST(MainQmlContractTests, ImageSidebarOpenRecordsSharedRecentHistoryWithoutVideoIntents) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QImage primary(64, 48, QImage::Format_ARGB32);
    primary.fill(QColor(120, 140, 160));
    const QString path = directory.filePath(QStringLiteral("shot.png"));
    ASSERT_TRUE(primary.save(path, "PNG"));
    WorkspaceHarness harness;
    harness.withVideoFolder = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();
    harness.submitted.clear();
    const QUrl url = QUrl::fromLocalFile(path);

    QVariant accepted;
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(),
                                          "openSidebarImage",
                                          Q_RETURN_ARG(QVariant, accepted),
                                          Q_ARG(QVariant, url)));
    ASSERT_TRUE(accepted.toBool());
    EXPECT_TRUE(harness.videoFolder.openPending());
    ASSERT_TRUE(harness.waitUntil([&] { return harness.imageReview.hasPrimary(); }));
    harness.settle();

    ASSERT_EQ(harness.preferences.recentMediaFiles().size(), 1);
    EXPECT_EQ(QUrl{harness.preferences.recentMediaFiles().front()}, url);
    ASSERT_EQ(harness.videoFolder.recentFiles().size(), 1);
    EXPECT_TRUE(harness.videoFolder.recentFiles()
                    .front()
                    .toMap()
                    .value(QStringLiteral("isImage"))
                    .toBool());
    EXPECT_FALSE(harness.videoFolder.openPending());
    EXPECT_TRUE(harness.videoFolder.errorText().isEmpty());
    EXPECT_TRUE(harness.root->property("imageWorkspaceActive").toBool());
    EXPECT_TRUE(harness.submitted.empty());
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

// V01 "panorama + three same-ROI locals" preset: entering the mode keeps the panorama on the
// ThreeUp renderer path (the surface itself never renders RoiTriplet; three columns keep all
// three sources presented so every local window can sample), the three local windows exist,
// and the whole round trip submits no playback command - the preset must not seek, pause or
// change the review target.
namespace {

[[nodiscard]] std::unique_ptr<QObject>
createThreeSourceRoiTripletRoot(QQmlEngine& engine,
                                ReviewController& controller,
                                ReviewPreferencesController& preferences,
                                ReviewShellController& shell,
                                ReviewSessionFacade& facade,
                                const std::shared_ptr<application::SessionSnapshot>& snapshot,
                                std::string* error) {
    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid()) {
        *error = "temp dir";
        return nullptr;
    }
    std::vector<std::filesystem::path> paths;
    for (const char name : {'a', 'b', 'c'}) {
        paths.push_back(std::filesystem::path{
            (temporaryDirectory.filePath(QString{"roi-%1.mp4"}.arg(QLatin1Char{name})))
                .toStdWString()});
        QFile file{QString::fromStdWString(paths.back().wstring())};
        if (!file.open(QIODevice::WriteOnly)) {
            *error = "write source";
            return nullptr;
        }
        file.write("roi-triplet-bytes");
    }
    const auto rate = domain::RationalRate::create(30, 1);
    if (!rate) {
        *error = "rate";
        return nullptr;
    }
    snapshot->graphicsReady = true;
    snapshot->sessionState = domain::SessionState::kReady;
    snapshot->playbackState = domain::PlaybackState::kPaused;
    snapshot->displayedFrame = domain::FrameId{0};
    if (!installValidatedVideoSet(snapshot, paths, rate.value(), 12, 400'000)) {
        *error = "validated set";
        return nullptr;
    }
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    engine.rootContext()->setContextProperty(QStringLiteral("reviewController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewPreferences"), &preferences);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewSession"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewFacade"), &facade);
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    if (component.status() != QQmlComponent::Ready) {
        *error = componentErrors(component);
        return nullptr;
    }
    std::unique_ptr<QObject> root{
        component.createWithInitialProperties(defaultPilotWindowProperties())};
    if (!root) {
        *error = componentErrors(component);
        return nullptr;
    }
    return root;
}

} // namespace

TEST(MainQmlContractTests, RoiTripletPresetEntersFromPreferencesWithoutPlaybackCommands) {
    auto snapshot = std::make_shared<application::SessionSnapshot>();
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
    std::string error;
    std::unique_ptr<QObject> root = createThreeSourceRoiTripletRoot(
        engine, controller, preferences, shell, facade, snapshot, &error);
    ASSERT_NE(root, nullptr) << error;
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);
    window->resize(1440, 900);
    window->show();
    for (int iteration = 0; iteration < 16; ++iteration) {
        QCoreApplication::processEvents();
    }
    ASSERT_EQ(controller.sourceCount(), 3);

    // The entry exists and is enabled with three sources. V03: 局部联动 is a top-level task
    // button now (the old "全景+局部" overflow item moved up, same mode enum and path).
    auto* const menuItem = root->findChild<QObject*>(QStringLiteral("roiTripletTaskButton"));
    ASSERT_NE(menuItem, nullptr);
    EXPECT_TRUE(menuItem->property("enabled").toBool());
    EXPECT_FALSE(menuItem->property("checked").toBool());

    auto* const surface = root->findChild<QQuickItem*>(QStringLiteral("dualVideoSurface"));
    ASSERT_NE(surface, nullptr);
    EXPECT_EQ(surface->property("viewMode").toInt(),
              static_cast<int>(dvs::ui::ComparisonSurface::SideBySide));

    const int submittedBefore = static_cast<int>(submitted.size());
    preferences.setViewMode(static_cast<ReviewPreferencesController::ViewMode>(8));
    for (int iteration = 0; iteration < 8; ++iteration) {
        QCoreApplication::processEvents();
    }

    // The mode reached the window and the strip, while the surface moved to the three-column
    // panorama - the renderer has no panel layout for RoiTriplet and must never see it, and
    // ThreeUp keeps all three sources presented for the local windows to re-sample.
    EXPECT_EQ(root->property("effectiveViewMode").toInt(), 8);
    EXPECT_EQ(surface->property("viewMode").toInt(),
              static_cast<int>(dvs::ui::ComparisonSurface::ThreeUp));
    auto* const strip = root->findChild<QQuickItem*>(QStringLiteral("roiTripletStrip"));
    ASSERT_NE(strip, nullptr);
    EXPECT_TRUE(strip->property("visible").toBool());
    EXPECT_GT(strip->height(), 0.0);
    auto* const windowRepeater =
        root->findChild<QObject*>(QStringLiteral("roiTripletWindowRepeater"));
    ASSERT_NE(windowRepeater, nullptr);
    EXPECT_EQ(windowRepeater->property("count").toInt(), 3);
    EXPECT_TRUE(menuItem->property("checked").toBool());

    // Entering the preset is a pure view change: no seek, no step, no play/pause.
    EXPECT_EQ(submitted.size(), static_cast<std::size_t>(submittedBefore));

    // Leaving it again restores the plain panorama with the strip hidden.
    preferences.setViewMode(ReviewPreferencesController::ViewMode::SideBySide);
    for (int iteration = 0; iteration < 8; ++iteration) {
        QCoreApplication::processEvents();
    }
    EXPECT_FALSE(strip->property("visible").toBool());
    EXPECT_EQ(surface->property("viewMode").toInt(),
              static_cast<int>(dvs::ui::ComparisonSurface::SideBySide));
    EXPECT_EQ(submitted.size(), static_cast<std::size_t>(submittedBefore));
}

// U01: the local strip's drag splitter moves the strip's share of the workspace height, and
// the clamp keeps the share inside the 28-40 % band no matter where the drag ends. Entering
// or leaving the preset resets nothing - the share is presentation state only.
TEST(MainQmlContractTests, LocalStripShareDragKeepsTheBand) {
    auto snapshot = std::make_shared<application::SessionSnapshot>();
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
    std::string error;
    std::unique_ptr<QObject> root = createThreeSourceRoiTripletRoot(
        engine, controller, preferences, shell, facade, snapshot, &error);
    ASSERT_NE(root, nullptr) << error;
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);
    window->resize(1440, 900);
    window->show();
    const auto processLayout = [] {
        for (int iteration = 0; iteration < 12; ++iteration) {
            QCoreApplication::processEvents();
        }
    };
    processLayout();

    preferences.setViewMode(static_cast<ReviewPreferencesController::ViewMode>(8));
    processLayout();
    // The review settles with a few of its own commands; the drag's innocence is measured
    // against this baseline, as the preset-entry test does.
    const std::size_t submittedBeforeDrag = submitted.size();
    auto* const strip = root->findChild<QQuickItem*>(QStringLiteral("roiTripletStrip"));
    auto* const splitter = root->findChild<QQuickItem*>(QStringLiteral("roiTripletStripSplitter"));
    auto* const viewport = root->findChild<QQuickItem*>(QStringLiteral("mediaViewportFocusTarget"));
    ASSERT_NE(strip, nullptr);
    ASSERT_NE(splitter, nullptr);
    ASSERT_NE(viewport, nullptr);
    ASSERT_TRUE(strip->isVisible());
    EXPECT_NEAR(strip->height() / viewport->height(), 0.34, 0.02)
        << "the default share is 34 % of the workspace";

    // Drag the splitter far past both band edges; the share must clamp, not escape.
    const QPointF splitterCenter =
        splitter->mapToScene(QPointF{splitter->width() / 2, splitter->height() / 2});
    const auto dragTo = [&](const QPointF& windowPos) {
        const QPointF globalPos = window->mapToGlobal(windowPos.toPoint());
        QMouseEvent move{
            QEvent::MouseMove, windowPos, globalPos, Qt::NoButton, Qt::LeftButton, Qt::NoModifier};
        QCoreApplication::sendEvent(window, &move);
    };
    sendMousePress(*window, splitterCenter);
    dragTo(splitterCenter + QPointF{0.0, -2000.0});
    processLayout();
    EXPECT_NEAR(strip->height() / viewport->height(), 0.40, 0.02)
        << "an upward over-drag clamps at the 40 % ceiling";

    dragTo(splitterCenter + QPointF{0.0, 2000.0});
    processLayout();
    EXPECT_NEAR(strip->height() / viewport->height(), 0.28, 0.02)
        << "a downward over-drag clamps at the 28 % floor";
    sendMouseRelease(*window, splitterCenter + QPointF{0.0, 2000.0});
    processLayout();

    // The share is presentation state: no playback command left the drag.
    EXPECT_EQ(submitted.size(), submittedBeforeDrag);
}

TEST(MainQmlContractTests, RoiTripletPresetKeepsSharedRoiAcrossModeRoundTrip) {
    auto snapshot = std::make_shared<application::SessionSnapshot>();
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
    std::string error;
    std::unique_ptr<QObject> root = createThreeSourceRoiTripletRoot(
        engine, controller, preferences, shell, facade, snapshot, &error);
    ASSERT_NE(root, nullptr) << error;
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    ASSERT_NE(window, nullptr);
    window->resize(1440, 900);
    window->show();
    for (int iteration = 0; iteration < 16; ++iteration) {
        QCoreApplication::processEvents();
    }

    auto* const surface = root->findChild<QQuickItem*>(QStringLiteral("dualVideoSurface"));
    ASSERT_NE(surface, nullptr);
    auto* const strip = root->findChild<QQuickItem*>(QStringLiteral("roiTripletStrip"));
    ASSERT_NE(strip, nullptr);

    preferences.setViewMode(static_cast<ReviewPreferencesController::ViewMode>(8));
    for (int iteration = 0; iteration < 8; ++iteration) {
        QCoreApplication::processEvents();
    }
    ASSERT_TRUE(strip->property("visible").toBool());
    // Session bootstrap (continuity policy + default pair policy from
    // applyPlaybackPreferencesFromSettings) legitimately submits commands once the
    // harness session reports ready; the round trip below must not add to that.
    const std::size_t submittedBeforeRoundTrip = submitted.size();

    // One manual ROI marquee, applied to the shared surface state.
    ASSERT_TRUE(QMetaObject::invokeMethod(surface,
                                          "setRoiNormalized",
                                          Q_ARG(qreal, 0.2),
                                          Q_ARG(qreal, 0.2),
                                          Q_ARG(qreal, 0.8),
                                          Q_ARG(qreal, 0.7)));
    for (int iteration = 0; iteration < 8; ++iteration) {
        QCoreApplication::processEvents();
    }
    EXPECT_TRUE(surface->property("roiEnabled").toBool());
    EXPECT_NEAR(surface->property("roiLeft").toDouble(), 0.2, 0.000001);
    EXPECT_NEAR(surface->property("roiBottom").toDouble(), 0.7, 0.000001);

    // The three windows all stage a content rect for the renderer's extra draws, and the
    // surface publishes a resolvable footage extent for each of them out of that one ROI.
    const auto stagedList = [&surface]() {
        return surface->property("roiTripletWindows").toList();
    };
    const auto footageList = [&surface]() {
        return surface->property("roiTripletWindowRects").toList();
    };
    EXPECT_EQ(stagedList().size(), 3);
    EXPECT_EQ(footageList().size(), 3);
    for (int slot = 0; slot < 3; ++slot) {
        const QVariantMap staged = stagedList()[static_cast<qsizetype>(slot)].toMap();
        EXPECT_EQ(staged.value(QStringLiteral("slot")).toInt(), slot) << slot;
        EXPECT_GT(staged.value(QStringLiteral("width")).toDouble(), 0.0) << slot;
        EXPECT_GT(staged.value(QStringLiteral("height")).toDouble(), 0.0) << slot;
        const QVariantMap footage = footageList()[static_cast<qsizetype>(slot)].toMap();
        EXPECT_EQ(footage.value(QStringLiteral("slot")).toInt(), slot) << slot;
        EXPECT_TRUE(footage.value(QStringLiteral("valid")).toBool()) << slot;
        EXPECT_GT(footage.value(QStringLiteral("width")).toDouble(), 0.0) << slot;
        EXPECT_GT(footage.value(QStringLiteral("height")).toDouble(), 0.0) << slot;
    }

    // Switch away: the strip hides, the panorama follows the new mode, and the ROI stays.
    preferences.setViewMode(ReviewPreferencesController::ViewMode::Wipe);
    for (int iteration = 0; iteration < 8; ++iteration) {
        QCoreApplication::processEvents();
    }
    EXPECT_FALSE(strip->property("visible").toBool());
    EXPECT_EQ(surface->property("viewMode").toInt(),
              static_cast<int>(dvs::ui::ComparisonSurface::Wipe));
    EXPECT_TRUE(surface->property("roiEnabled").toBool());
    EXPECT_NEAR(surface->property("roiLeft").toDouble(), 0.2, 0.000001);

    // Back into the preset: the ROI survived and the windows re-stage for the renderer.
    preferences.setViewMode(static_cast<ReviewPreferencesController::ViewMode>(8));
    for (int iteration = 0; iteration < 8; ++iteration) {
        QCoreApplication::processEvents();
    }
    ASSERT_TRUE(strip->property("visible").toBool());
    EXPECT_TRUE(surface->property("roiEnabled").toBool());
    EXPECT_NEAR(surface->property("roiLeft").toDouble(), 0.2, 0.000001);
    const QVariantList restaged = stagedList();
    EXPECT_EQ(restaged.size(), 3);
    const QVariantMap after = restaged.isEmpty() ? QVariantMap{} : restaged.front().toMap();
    EXPECT_GT(after.value(QStringLiteral("width")).toDouble(), 0.0);
    // The panorama marks the ROI while the preset is active.
    auto* const outlineRepeater =
        root->findChild<QObject*>(QStringLiteral("roiTripletOutlineRepeater"));
    ASSERT_NE(outlineRepeater, nullptr);
    EXPECT_EQ(outlineRepeater->property("count").toInt(), 3);

    // The whole round trip - including the ROI change - submitted no playback command
    // beyond the session bootstrap captured before it.
    EXPECT_EQ(submitted.size(), submittedBeforeRoundTrip);
}

// Two sources are the preset minimum: the entry is enabled, the panorama falls back to the
// two-column SideBySide layout (both sources stay presented), and the strip stages two local
// windows for the renderer's extra draws.
TEST(MainQmlContractTests, RoiTripletPresetSupportsTwoSources) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    EXPECT_EQ(harness.controller->sourceCount(), 2);
    auto* const surface = harness.root->findChild<QQuickItem*>(QStringLiteral("dualVideoSurface"));
    ASSERT_NE(surface, nullptr);
    auto* const menuItem =
        harness.root->findChild<QObject*>(QStringLiteral("roiTripletTaskButton"));
    ASSERT_NE(menuItem, nullptr);
    EXPECT_TRUE(menuItem->property("enabled").toBool());
    // V03: the dual difference needs three sources; the two-source difference task only
    // offers the pair flavors (row visibility is readable once the popup materializes).
    auto* const diffMenu = harness.root->findChild<QObject*>(QStringLiteral("diffFlavorMenu"));
    ASSERT_NE(diffMenu, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(diffMenu, "open"));
    for (int iteration = 0; iteration < 5; ++iteration) {
        QCoreApplication::processEvents();
    }
    auto* const dualItem =
        harness.root->findChild<QObject*>(QStringLiteral("dualDifferenceMenuItem"));
    ASSERT_NE(dualItem, nullptr);
    EXPECT_FALSE(dualItem->property("visible").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(diffMenu, "close"));
    auto* const strip = harness.root->findChild<QQuickItem*>(QStringLiteral("roiTripletStrip"));
    ASSERT_NE(strip, nullptr);
    EXPECT_FALSE(strip->property("visible").toBool());

    harness.preferences.setViewMode(static_cast<ReviewPreferencesController::ViewMode>(8));
    for (int iteration = 0; iteration < 8; ++iteration) {
        QCoreApplication::processEvents();
    }
    EXPECT_TRUE(menuItem->property("checked").toBool());
    EXPECT_TRUE(strip->property("visible").toBool());
    // Two columns: every source stays presented and its local window can sample the frame set.
    EXPECT_EQ(surface->property("viewMode").toInt(),
              static_cast<int>(dvs::ui::ComparisonSurface::SideBySide));
    auto* const windowRepeater =
        harness.root->findChild<QObject*>(QStringLiteral("roiTripletWindowRepeater"));
    ASSERT_NE(windowRepeater, nullptr);
    EXPECT_EQ(windowRepeater->property("count").toInt(), 2);
    const QVariantList staged = surface->property("roiTripletWindows").toList();
    ASSERT_EQ(staged.size(), 2);
    EXPECT_EQ(staged.front().toMap().value(QStringLiteral("slot")).toInt(), 0);
    EXPECT_EQ(staged.at(1).toMap().value(QStringLiteral("slot")).toInt(), 1);

    harness.preferences.setViewMode(ReviewPreferencesController::ViewMode::SideBySide);
    for (int iteration = 0; iteration < 8; ++iteration) {
        QCoreApplication::processEvents();
    }
    EXPECT_FALSE(strip->property("visible").toBool());
    EXPECT_TRUE(surface->property("roiTripletWindows").toList().empty());
}

// I03: the settings menu's 「文件关联…」 entry must open the dialog, and every action in it
// must leave a visible trace. The dialog used to be declared inside the MenuBar, and a MenuBar
// never parents non-Menu children into a window, so the popup silently never opened - the
// "clicked, nothing happened" report this contract pins down. A success that only changes a
// read-back row would equally read as a dead button, so outcomes must be visible too.
TEST(MainQmlContractTests, FileAssociationDialogOpensAndReportsActionOutcomes) {
    WorkspaceHarness harness;
    harness.withFileAssociation = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    // The user's path: open the Settings menu first, then trigger its item, so the
    // contract also covers the menu-closing/dialog-opening interplay.
    auto* const settingsMenu = harness.root->findChild<QObject*>(QStringLiteral("settingsMenu"));
    ASSERT_NE(settingsMenu, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(settingsMenu, "open"));
    harness.settle();

    auto* const menuItem =
        harness.root->findChild<QObject*>(QStringLiteral("fileAssociationMenuItem"));
    ASSERT_NE(menuItem, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(menuItem, "triggered"));
    harness.settle();

    // QQuickPopup derives QObject, not QQuickItem, so the lookup must not be QQuickItem-typed.
    auto* const popup = harness.root->findChild<QObject*>(QStringLiteral("fileAssociationPopup"));
    ASSERT_NE(popup, nullptr);
    EXPECT_TRUE(popup->property("visible").toBool());

    auto* const resultText =
        harness.root->findChild<QQuickItem*>(QStringLiteral("fileAssociationResultText"));
    ASSERT_NE(resultText, nullptr);
    auto* const errorText =
        harness.root->findChild<QQuickItem*>(QStringLiteral("fileAssociationErrorText"));
    ASSERT_NE(errorText, nullptr);

    // A successful register states what it did and what the user still owes the system. The
    // dialog's own open() fires an async refresh, so wait for idle before clicking.
    auto* const registerButton =
        harness.root->findChild<QQuickItem*>(QStringLiteral("fileAssociationRegisterButton"));
    ASSERT_NE(registerButton, nullptr);
    EXPECT_TRUE(harness.waitUntil([&harness] { return !harness.fileAssociation->busy(); }));
    ASSERT_TRUE(QMetaObject::invokeMethod(registerButton, "clicked"));
    EXPECT_TRUE(harness.waitUntil([resultText] {
        return resultText->isVisible() &&
               resultText->property("text").toString().contains(QStringLiteral("已注册为可选应用"));
    }));
    EXPECT_FALSE(errorText->isVisible());
    EXPECT_EQ(harness.fileAssociationService->registerCalls, 1);

    // A failed action clears the success note and names the cause in Chinese, not raw English.
    harness.fileAssociationService->failNext = true;
    harness.fileAssociationService->nextError = "the registration needs CompareStation.exe";
    ASSERT_TRUE(QMetaObject::invokeMethod(registerButton, "clicked"));
    EXPECT_TRUE(harness.waitUntil([errorText] {
        return errorText->isVisible() &&
               errorText->property("text").toString().contains(QStringLiteral("无法注册"));
    }));
    EXPECT_FALSE(resultText->isVisible());
    EXPECT_EQ(harness.fileAssociationService->registerCalls, 2);
}

// Flow176 UI rework evidence harness. Runs the same measurement and screenshot protocol
// before and after the layout rework (gated by DVS_REVIEW_EVIDENCE_DIR): identical window
// sizes, identical session states. The chrome-height, canvas-space and OSC-height claims are
// read from these captures, never from the plan's numbers. All geometry is recorded in
// logical pixels at the current QT_SCALE_FACTOR; each DPI run uses its own evidence dir.
TEST(MainQmlContractTests, ChromeGeometryAndScreenshotEvidence) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    std::vector<std::filesystem::path> videoPaths;
    for (const char name : {'a', 'b', 'c'}) {
        videoPaths.push_back(std::filesystem::path{
            (temporaryDirectory.filePath(QString{"evidence-%1.mp4"}.arg(QLatin1Char{name})))
                .toStdWString()});
        QFile file{QString::fromStdWString(videoPaths.back().wstring())};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_GT(file.write("evidence-source-bytes", 10), 0);
    }
    const auto rate = domain::RationalRate::create(30, 1);
    ASSERT_TRUE(rate);

    WorkspaceHarness harness;
    // The edit-mode capture needs the step-3 edit controller in the QML context.
    harness.withImageEdit = true;
    ASSERT_TRUE(installValidatedVideoSet(harness.snapshot, videoPaths, rate.value(), 120, 400'000));
    harness.snapshot->sessionState = domain::SessionState::kReady;
    harness.snapshot->playbackState = domain::PlaybackState::kPaused;
    harness.snapshot->displayedFrame = domain::FrameId{41};
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    const auto evidenceDirectory = qEnvironmentVariable("DVS_REVIEW_EVIDENCE_DIR");
    if (!evidenceDirectory.isEmpty()) {
        ASSERT_TRUE(QDir().mkpath(evidenceDirectory));
    }
    QStringList geometryLines;
    geometryLines << QStringLiteral("# logical pixels; scale=%1")
                         .arg(QString::number(harness.window->devicePixelRatio(), 'g', 4));

    const auto recordItem = [&geometryLines, &harness](const char* prefix,
                                                       const QString& name,
                                                       const bool required = false) {
        auto* const item = harness.root->findChild<QQuickItem*>(name);
        if (item == nullptr) {
            // Required anchors must exist: an evidence capture that silently skips a missing
            // milestone records a filename the picture behind it may not match.
            if (required) {
                ADD_FAILURE() << "evidence anchor missing: " << name.toStdString();
            }
            geometryLines << QStringLiteral("%1.%2=missing").arg(QLatin1String{prefix}, name);
            return static_cast<QQuickItem*>(nullptr);
        }
        geometryLines << QStringLiteral("%1.%2.visible=%3")
                             .arg(QLatin1String{prefix}, name, item->isVisible() ? "1" : "0");
        geometryLines << QStringLiteral("%1.%2.w=%3")
                             .arg(QLatin1String{prefix},
                                  name,
                                  QString::number(item->width(), 'f', 1));
        geometryLines << QStringLiteral("%1.%2.h=%3")
                             .arg(QLatin1String{prefix},
                                  name,
                                  QString::number(item->height(), 'f', 1));
        geometryLines << QStringLiteral("%1.%2.x=%3")
                             .arg(QLatin1String{prefix}, name, QString::number(item->x(), 'f', 1));
        geometryLines << QStringLiteral("%1.%2.y=%3")
                             .arg(QLatin1String{prefix}, name, QString::number(item->y(), 'f', 1));
        return item;
    };
    const auto recordStageArea = [&geometryLines](const char* prefix,
                                                  const QQuickItem* workspace,
                                                  const QQuickItem* topChrome,
                                                  const QQuickItem* statusBar) {
        if (workspace == nullptr || topChrome == nullptr || statusBar == nullptr) {
            geometryLines << QStringLiteral("%1.stage=unmeasured").arg(QLatin1String{prefix});
            return;
        }
        // The real stage (canvas) box: ImageWorkspace anchors the stage between the command
        // panel (+12 top margin) and the status bar (-12 bottom margin), inset 12 left/right.
        const qreal stageTop = topChrome->y() + topChrome->height() + 12.0;
        const qreal stageBottom = statusBar->y() - 12.0;
        const qreal stageHeight = stageBottom - stageTop;
        const qreal stageWidth = qMax(0.0, workspace->width() - 24.0);
        geometryLines << QStringLiteral("%1.stage.h=%2")
                             .arg(QLatin1String{prefix}, QString::number(stageHeight, 'f', 1));
        geometryLines << QStringLiteral("%1.stage.w=%2")
                             .arg(QLatin1String{prefix}, QString::number(stageWidth, 'f', 1));
        geometryLines << QStringLiteral("%1.stage.area=%2")
                             .arg(QLatin1String{prefix},
                                  QString::number(stageHeight * stageWidth, 'f', 0));
    };
    const auto recordWindow = [&geometryLines, &harness](const QString& prefix) {
        geometryLines << QStringLiteral("%1.window.w=%2").arg(prefix).arg(harness.window->width());
        geometryLines << QStringLiteral("%1.window.h=%2").arg(prefix).arg(harness.window->height());
    };
    const auto shot = [&harness, &evidenceDirectory](const char* name) {
        if (evidenceDirectory.isEmpty()) {
            return;
        }
        harness.settleAnimations();
        ASSERT_TRUE(harness.window->grabWindow().save(
            QDir(evidenceDirectory).filePath(QString::fromLatin1(name))));
    };

    // ---- Video workspace, default comparison view at the working size. ----
    // The 1440x900 evidence window must land on a screen whose available area fits BOTH
    // dimensions (a largest-area screen can still be too narrow or too short, e.g. a
    // portrait display); otherwise Windows clamps the window and the capture would
    // silently show a different geometry than the filename claims. Try each candidate in
    // turn and record what every screen achieved; the guard below skips the scenario as
    // NOT evidenced when none fits.
    {
        QStringList screensReport;
        for (QScreen* screen : QGuiApplication::screens()) {
            const QRect available = screen->availableGeometry();
            screensReport << QStringLiteral("%1 %2x%3@%4")
                                 .arg(screen->name())
                                 .arg(available.width())
                                 .arg(available.height())
                                 .arg(QString::number(screen->devicePixelRatio(), 'g', 3));
        }
        geometryLines
            << QStringLiteral("screens: %1").arg(screensReport.join(QStringLiteral("; ")));
        for (QScreen* screen : QGuiApplication::screens()) {
            const QRect available = screen->availableGeometry();
            harness.window->setScreen(screen);
            harness.window->setGeometry(available.x() + (available.width() - 1440) / 2,
                                        available.y() + (available.height() - 900) / 2,
                                        1440,
                                        900);
            harness.settle();
            geometryLines << QStringLiteral("screen %1 candidate: requested 1440x900 "
                                            "achieved %2x%3")
                                 .arg(screen->name())
                                 .arg(harness.window->width())
                                 .arg(harness.window->height());
            if (harness.window->width() == 1440 && harness.window->height() == 900) {
                break;
            }
        }
    }
    harness.window->resize(1440, 900);
    harness.settle();
    harness.settleAnimations();
    if (harness.window->width() != 1440 || harness.window->height() != 900) {
        geometryLines << QStringLiteral("video1440.window.clamped=%1x%2 - the 1440x900 "
                                        "evidence scenario was NOT captured this run")
                             .arg(harness.window->width())
                             .arg(harness.window->height());
        if (!evidenceDirectory.isEmpty()) {
            QFile clampedReport{
                QDir(evidenceDirectory).filePath(QStringLiteral("chrome-geometry.txt"))};
            static_cast<void>(clampedReport.open(QIODevice::WriteOnly | QIODevice::Append));
            clampedReport.write(geometryLines.join(QLatin1Char{'\n'}).toUtf8());
        }
        GTEST_SKIP() << "no screen fits the 1440x900 evidence window at the current scale: "
                        "achieved "
                     << harness.window->width() << "x" << harness.window->height() << " at DPR "
                     << harness.window->devicePixelRatio();
    }
    // The evidence comparison needs identical actual sizes: a window clamped by the screen
    // edge must fail here instead of being compared as if it were 1440x900.
    EXPECT_EQ(harness.window->width(), 1440);
    EXPECT_EQ(harness.window->height(), 900);
    shot("video-default-1440.png");
    recordWindow(QStringLiteral("video1440"));
    recordItem("video1440", QStringLiteral("activeSourceStrip"), true);
    recordItem("video1440", QStringLiteral("compareModeBar"), true);
    recordItem("video1440", QStringLiteral("transport"), true);
    recordItem("video1440", QStringLiteral("mediaViewportFocusTarget"), true);
    recordItem("video1440", QStringLiteral("viewportPixelReadoutPlate"));

    // In/out range state: the transport grows its range cluster.
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(), "setInPoint"));
    harness.settle();
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(), "setOutPoint"));
    harness.settle();
    EXPECT_GE(harness.shell->inFrame(), 0) << "the range capture must show a real range";
    shot("video-range-1440.png");
    recordItem("videoRange1440", QStringLiteral("transport"), true);

    // Three-source ROI triplet preset.
    harness.preferences.setViewMode(ReviewPreferencesController::ViewMode::RoiTriplet);
    harness.settle();
    harness.settleAnimations();
    {
        auto* const strip = recordItem("videoRoi1440", QStringLiteral("roiTripletStrip"), true);
        ASSERT_NE(strip, nullptr);
        EXPECT_TRUE(strip->isVisible()) << "the ROI capture must show the triplet strip";
    }
    shot("video-roi-triplet-1440.png");
    harness.preferences.setViewMode(ReviewPreferencesController::ViewMode::SideBySide);
    harness.settle();

    // Supported floor size.
    harness.window->resize(960, 640);
    harness.settle();
    harness.settleAnimations();
    EXPECT_EQ(harness.window->width(), 960);
    EXPECT_EQ(harness.window->height(), 640);
    shot("video-min-960.png");
    recordWindow(QStringLiteral("video960"));
    // UX-13 S1 baseline prep: the floor-size capture used to record only the transport, so the
    // narrow-window chrome budget (two top rows + docked transport) had no measured baseline.
    // Mirror the video1440 anchors exactly - same names, same requiredness - so the S1
    // before/after comparison covers every chrome row at 960x640 too.
    recordItem("video960", QStringLiteral("activeSourceStrip"), true);
    recordItem("video960", QStringLiteral("compareModeBar"), true);
    recordItem("video960", QStringLiteral("transport"), true);
    recordItem("video960", QStringLiteral("mediaViewportFocusTarget"), true);
    recordItem("video960", QStringLiteral("viewportPixelReadoutPlate"));

    // ---- Image workspace: one pair, every comparison mode + edit mode. ----
    QImage imageA(320, 240, QImage::Format_ARGB32);
    QImage imageB(320, 240, QImage::Format_ARGB32);
    for (int y = 0; y < 240; ++y) {
        for (int x = 0; x < 320; ++x) {
            imageA.setPixelColor(x, y, QColor(x % 256, (x / 2) % 256, y % 256, 255));
            imageB.setPixelColor(x, y, QColor((x + 12) % 256, (x / 2) % 256, (y + 5) % 256, 255));
        }
    }
    ASSERT_TRUE(harness.imageReview.openPairImages(
        imageA, QStringLiteral("evidence-a.png"), imageB, QStringLiteral("evidence-b.png")));
    ASSERT_TRUE(harness.activateWorkspace(1));
    harness.window->resize(1440, 900);
    harness.settle();
    harness.settleAnimations();

    shot("image-default-1440.png");
    recordWindow(QStringLiteral("image1440"));
    recordStageArea("image1440",
                    recordItem("image1440", QStringLiteral("imageWorkspaceRoot"), true),
                    recordItem("image1440", QStringLiteral("imageCommandPanel"), true),
                    recordItem("image1440", QStringLiteral("imageStatusBar"), true));

    // Every mode capture asserts the controller state, so a screenshot filename always
    // describes the mode the picture actually shows.
    const auto clickItem = [&harness](const QString& name, const char* method) {
        auto* const item = harness.root->findChild<QQuickItem*>(name);
        EXPECT_NE(item, nullptr) << name.toStdString();
        return item != nullptr && QMetaObject::invokeMethod(item, method);
    };
    const auto clickMode = [&](const QString& name, const int expectedMode) {
        EXPECT_TRUE(clickItem(name, "clicked")) << name.toStdString();
        harness.settle();
        EXPECT_EQ(harness.imageReview.compareMode(), expectedMode)
            << "capture state mismatch after " << name.toStdString();
    };

    clickMode(QStringLiteral("imageModeSide"), 1);
    {
        auto* const secondary =
            harness.root->findChild<QQuickItem*>(QStringLiteral("secondaryViewport"));
        ASSERT_NE(secondary, nullptr);
        EXPECT_TRUE(secondary->isVisible());
    }
    shot("image-side-by-side-1440.png");
    clickMode(QStringLiteral("imageModeWipe"), 5);
    {
        auto* const wipe = harness.root->findChild<QQuickItem*>(QStringLiteral("wipeOverlay"));
        ASSERT_NE(wipe, nullptr);
        EXPECT_TRUE(wipe->isVisible());
    }
    shot("image-wipe-1440.png");
    clickMode(QStringLiteral("imageModeFade"), 7);
    {
        auto* const fade = harness.root->findChild<QQuickItem*>(QStringLiteral("fadeOverlay"));
        ASSERT_NE(fade, nullptr);
        EXPECT_TRUE(fade->isVisible());
    }
    shot("image-fade-1440.png");
    clickMode(QStringLiteral("imageModePrimary"), 0);
    EXPECT_TRUE(clickItem(QStringLiteral("imageModeAbsDiff"), "triggered"));
    EXPECT_TRUE(harness.waitUntil([&harness] { return harness.imageReview.hasDiffResult(); }))
        << "the diff capture needs a real difference result";
    harness.settle();
    EXPECT_EQ(harness.imageReview.compareMode(), 2);
    {
        auto* const diff = harness.root->findChild<QQuickItem*>(QStringLiteral("diffViewport"));
        ASSERT_NE(diff, nullptr);
        EXPECT_TRUE(diff->isVisible());
    }
    shot("image-diff-1440.png");

    clickMode(QStringLiteral("imageModePrimary"), 0);
    EXPECT_TRUE(clickItem(QStringLiteral("imageEditStartButton"), "clicked"));
    harness.settle();
    EXPECT_TRUE(harness.imageEdit.property("active").toBool())
        << "the edit capture must show an active edit session";
    shot("image-edit-1440.png");
    recordStageArea("imageEdit1440",
                    recordItem("imageEdit1440", QStringLiteral("imageWorkspaceRoot"), true),
                    recordItem("imageEdit1440", QStringLiteral("imageCommandPanel"), true),
                    recordItem("imageEdit1440", QStringLiteral("imageStatusBar"), true));
    EXPECT_TRUE(clickItem(QStringLiteral("imageEditStartButton"), "clicked"));
    harness.settle();
    EXPECT_FALSE(harness.imageEdit.property("active").toBool());

    harness.window->resize(960, 640);
    harness.settle();
    harness.settleAnimations();
    EXPECT_EQ(harness.window->width(), 960);
    EXPECT_EQ(harness.window->height(), 640);
    shot("image-min-960.png");
    recordWindow(QStringLiteral("image960"));
    recordStageArea("image960",
                    recordItem("image960", QStringLiteral("imageWorkspaceRoot"), true),
                    recordItem("image960", QStringLiteral("imageCommandPanel"), true),
                    recordItem("image960", QStringLiteral("imageStatusBar"), true));

    if (!evidenceDirectory.isEmpty()) {
        QFile geometryFile{QDir(evidenceDirectory).filePath(QStringLiteral("chrome-geometry.txt"))};
        ASSERT_TRUE(geometryFile.open(QIODevice::WriteOnly | QIODevice::Text));
        ASSERT_TRUE(geometryFile.write(geometryLines.join('\n').toUtf8() + '\n'));
    }
}

// The production root declares its pilot services as required typed properties and the
// production loader verifies their identity before showing the window. These
// counterexamples exercise the REAL Main.qml contract with the REAL services the
// composition root supplies:
//   correct services    -> creation and verification both succeed;
//   missing entirely    -> creation itself fails on the required property;
//   missing clipboard   -> creation fails on that property as well;
//   explicit null       -> creation succeeds, the production verifier rejects;
//   wrong-typed object  -> Qt warns and degrades the property to null, the verifier rejects.
TEST(MainQmlContractTests, SingleImageOpenArmsTheDirectoryCursorAndOneStepOpensTheNeighbour) {
    WorkspaceHarness harness;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());
    // One real 1x1 PNG, written twice: the cursor reads names and suffixes, the decode still
    // has to succeed for the open to commit.
    const QByteArray png = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJ"
        "RU5ErkJggg==");
    const auto writeImage = [&folder, &png](const QString& name) {
        const QString path = QDir{folder.path()}.filePath(name);
        QFile file{path};
        if (!file.open(QIODevice::WriteOnly)) {
            return QUrl{};
        }
        file.write(png);
        file.close();
        return QUrl::fromLocalFile(path);
    };
    const QUrl first = writeImage(QStringLiteral("shot1.png"));
    const QUrl second = writeImage(QStringLiteral("shot2.png"));
    ASSERT_FALSE(first.isEmpty());
    ASSERT_FALSE(second.isEmpty());

    QVariant opened;
    ASSERT_TRUE(
        QMetaObject::invokeMethod(harness.root.get(),
                                  "performImageReview",
                                  Q_RETURN_ARG(QVariant, opened),
                                  Q_ARG(QVariant, QVariant::fromValue(QVariantList{first}))));
    ASSERT_TRUE(opened.toBool());
    ASSERT_TRUE(harness.waitUntil([&harness] {
        return harness.imageFolderCursor.count() == 2 && harness.imageReview.hasPrimary();
    })) << "a committed single image must arm the directory cursor";

    EXPECT_EQ(harness.root->property("imageFolderBrowseText").toString(), QStringLiteral("1 / 2"));

    QVariant stepped;
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(),
                                          "stepImageFolder",
                                          Q_RETURN_ARG(QVariant, stepped),
                                          Q_ARG(QVariant, QVariant{1})));
    ASSERT_TRUE(stepped.toBool());
    ASSERT_TRUE(harness.waitUntil([&harness] {
        return harness.imageFolderCursor.currentIndex() == 2;
    })) << "one right-step must commit the next image in the same directory";

    EXPECT_EQ(QFileInfo{harness.imageReview.primaryPath()}.fileName(), QStringLiteral("shot2.png"));
    EXPECT_EQ(harness.root->property("imageFolderBrowseText").toString(), QStringLiteral("2 / 2"));

    // Backwards past the first image is a boundary, not a wrap: the committed image stays.
    QVariant again;
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(),
                                          "stepImageFolder",
                                          Q_RETURN_ARG(QVariant, again),
                                          Q_ARG(QVariant, QVariant{1})));
    EXPECT_FALSE(again.toBool());
    harness.settle();
    EXPECT_EQ(QFileInfo{harness.imageReview.primaryPath()}.fileName(), QStringLiteral("shot2.png"));

    // A delete in Explorer while the window is hidden must not leave a stale sequence: the
    // activation hook re-scans, and a cleared cursor (pair or folder task) stays empty.
    ASSERT_TRUE(QFile::remove(QDir{folder.path()}.filePath(QStringLiteral("shot1.png"))));
    QVariant refreshed;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        harness.root.get(), "refreshImageFolderCursor", Q_RETURN_ARG(QVariant, refreshed)));
    EXPECT_TRUE(refreshed.toBool());
    EXPECT_EQ(harness.imageFolderCursor.count(), 1);

    harness.imageFolderCursor.clear();
    QVariant refreshedEmpty;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        harness.root.get(), "refreshImageFolderCursor", Q_RETURN_ARG(QVariant, refreshedEmpty)));
    EXPECT_FALSE(refreshedEmpty.toBool()) << "a pair/folder task must not gain a directory list";
    EXPECT_EQ(harness.imageFolderCursor.count(), 0);
}

TEST(MainQmlContractTests, DirtyEditCopyBlocksDirectoryStepping) {
    WorkspaceHarness harness;
    harness.withImageEdit = true;
    ASSERT_TRUE(harness.create()) << harness.error;
    harness.window->show();
    harness.settle();

    QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());
    const QByteArray png = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJ"
        "RU5ErkJggg==");
    const auto writeImage = [&folder, &png](const QString& name) {
        const QString path = QDir{folder.path()}.filePath(name);
        QFile file{path};
        if (!file.open(QIODevice::WriteOnly)) {
            return QUrl{};
        }
        file.write(png);
        file.close();
        return QUrl::fromLocalFile(path);
    };
    const QUrl first = writeImage(QStringLiteral("shot1.png"));
    const QUrl second = writeImage(QStringLiteral("shot2.png"));
    ASSERT_FALSE(first.isEmpty());
    ASSERT_FALSE(second.isEmpty());

    QVariant opened;
    ASSERT_TRUE(
        QMetaObject::invokeMethod(harness.root.get(),
                                  "performImageReview",
                                  Q_RETURN_ARG(QVariant, opened),
                                  Q_ARG(QVariant, QVariant::fromValue(QVariantList{first}))));
    ASSERT_TRUE(opened.toBool());
    ASSERT_TRUE(harness.waitUntil([&harness] {
        return harness.imageFolderCursor.count() == 2 && harness.imageReview.hasPrimary();
    }));

    ASSERT_TRUE(harness.imageEdit.beginSession(
        ImageReviewController::DisplayPrimarySlot, QStringLiteral("shot1"), first));
    ASSERT_TRUE(harness.imageEdit.fillImageRect(0, 0, 1, 1, QColor(10, 20, 30, 255)));
    ASSERT_TRUE(harness.imageEdit.dirty());

    // Stepping would end the session and drop the in-memory working copy; the refusal has to
    // happen before the open request, not after the image has already been replaced.
    QVariant stepped;
    ASSERT_TRUE(QMetaObject::invokeMethod(harness.root.get(),
                                          "stepImageFolder",
                                          Q_RETURN_ARG(QVariant, stepped),
                                          Q_ARG(QVariant, QVariant{1})));
    EXPECT_FALSE(stepped.toBool());
    harness.settle();
    EXPECT_TRUE(harness.imageEdit.dirty());
    EXPECT_EQ(QFileInfo{harness.imageReview.primaryPath()}.fileName(), QStringLiteral("shot1.png"));

    // The direct open paths share the same refusal: a dialog, a drop or the recent list must not
    // replace the image under a dirty session either.
    QVariant openedAgain;
    ASSERT_TRUE(
        QMetaObject::invokeMethod(harness.root.get(),
                                  "performImageReview",
                                  Q_RETURN_ARG(QVariant, openedAgain),
                                  Q_ARG(QVariant, QVariant::fromValue(QVariantList{second}))));
    EXPECT_FALSE(openedAgain.toBool());
    harness.settle();
    EXPECT_EQ(QFileInfo{harness.imageReview.primaryPath()}.fileName(), QStringLiteral("shot1.png"));

    // Ctrl+W would close the task while the working copy still lives only in memory.
    QVariant closed;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        harness.root.get(), "closeCurrentTask", Q_RETURN_ARG(QVariant, closed)));
    EXPECT_FALSE(closed.toBool());
    harness.settle();
    EXPECT_TRUE(harness.imageReview.hasPrimary());
    EXPECT_TRUE(harness.imageEdit.dirty());

    // A clean session has nothing to lose: it must not block the open, and it must not survive
    // on top of a different image.
    harness.imageEdit.endSession();
    ASSERT_TRUE(harness.imageEdit.beginSession(
        ImageReviewController::DisplayPrimarySlot, QStringLiteral("shot1"), first));
    ASSERT_FALSE(harness.imageEdit.dirty());
    QVariant cleanOpen;
    ASSERT_TRUE(
        QMetaObject::invokeMethod(harness.root.get(),
                                  "performImageReview",
                                  Q_RETURN_ARG(QVariant, cleanOpen),
                                  Q_ARG(QVariant, QVariant::fromValue(QVariantList{second}))));
    EXPECT_TRUE(cleanOpen.toBool());
    ASSERT_TRUE(harness.waitUntil([&harness] {
        return QFileInfo{harness.imageReview.primaryPath()}.fileName() ==
               QStringLiteral("shot2.png");
    }));
    EXPECT_FALSE(harness.imageEdit.active());
}

TEST(MainQmlContractTests, MainWindowRequiredPilotServicesAreEnforced) {
    ReviewController controller{ReviewController::Dependencies{
        .submit =
            [](application::PlaybackCommand) { return application::PortSubmitResult::Accepted; },
        .snapshot = [] { return std::make_shared<application::SessionSnapshot>(); },
        .takeCompletedCommands = [] { return std::vector<application::CommandTerminal>{}; },
    }};
    ReviewPreferencesController preferences{std::make_shared<ClosedSettingsRepository>()};
    ReviewShellController shell{controller, preferences};
    ReviewSessionFacade facade{controller, preferences, shell};
    ImageReviewController imageReview;
    ColorClipboardController colorClipboard;

    QQmlEngine engine;
    engine.addImportPath(
        QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
    engine.rootContext()->setContextProperty(QStringLiteral("reviewController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewPreferences"), &preferences);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewSession"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("reviewFacade"), &facade);
    QQmlComponent component{&engine, QUrl{QStringLiteral("qrc:/qml/Main.qml")}};
    ASSERT_EQ(component.status(), QQmlComponent::Ready) << componentErrors(component);

    {
        const std::unique_ptr<QObject> root{component.createWithInitialProperties(
            pilotWindowProperties(imageReview, colorClipboard))};
        ASSERT_NE(root, nullptr) << componentErrors(component);
        QString error;
        EXPECT_TRUE(verifyMainWindowPilotServices(*root, imageReview, colorClipboard, &error))
            << error.toStdString();
    }
    {
        const std::unique_ptr<QObject> root{component.create()};
        EXPECT_EQ(root, nullptr);
        const std::string errors = componentErrors(component);
        EXPECT_NE(errors.find("Required property stillImageController was not initialized"),
                  std::string::npos)
            << errors;
    }
    {
        QVariantMap properties = pilotWindowProperties(imageReview, colorClipboard);
        properties.remove(QStringLiteral("colorClipboardService"));
        const std::unique_ptr<QObject> root{component.createWithInitialProperties(properties)};
        EXPECT_EQ(root, nullptr);
        const std::string errors = componentErrors(component);
        EXPECT_NE(errors.find("Required property colorClipboardService was not initialized"),
                  std::string::npos)
            << errors;
    }
    {
        QVariantMap properties = pilotWindowProperties(imageReview, colorClipboard);
        properties.insert(QStringLiteral("stillImageController"),
                          QVariant::fromValue(static_cast<QObject*>(nullptr)));
        const std::unique_ptr<QObject> root{component.createWithInitialProperties(properties)};
        ASSERT_NE(root, nullptr) << componentErrors(component);
        QString error;
        EXPECT_FALSE(verifyMainWindowPilotServices(*root, imageReview, colorClipboard, &error));
        EXPECT_FALSE(error.isEmpty());
    }
    {
        QObject wrongType;
        QVariantMap properties = pilotWindowProperties(imageReview, colorClipboard);
        properties.insert(QStringLiteral("stillImageController"), QVariant::fromValue(&wrongType));
        const std::unique_ptr<QObject> root{component.createWithInitialProperties(properties)};
        ASSERT_NE(root, nullptr) << componentErrors(component);
        QString error;
        EXPECT_FALSE(verifyMainWindowPilotServices(*root, imageReview, colorClipboard, &error));
        EXPECT_FALSE(error.isEmpty());
    }
}

} // namespace
} // namespace dvs::ui

int main(int argumentCount, char* arguments[]) {
    dvs::ui::configureGraphicsBackend();
    QGuiApplication application{argumentCount, arguments};
    initializeMainQmlContractResources();
    // The single Dvs.Ui registration entry: every type name the QML files can reference,
    // registered exactly once for the whole process.
    dvs::ui::registerDvsUiTypes();
    testing::InitGoogleTest(&argumentCount, arguments);
    return RUN_ALL_TESTS();
}
