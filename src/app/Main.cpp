#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "dvs/application/PlaybackTrace.h"
#include "dvs/media/StillImageDecoder.h"
#include "dvs/platform/ProcessTelemetry.h"
#include "dvs/platform/TraceSink.h"
#include "dvs/ui/ComparisonSurface.h"
#include "dvs/ui/DesktopApplication.h"
#include "dvs/ui/GraphicsBackend.h"
#include "dvs/ui/ImageFolderPairModel.h"
#include "dvs/ui/ImageReviewController.h"
#include "dvs/ui/ReviewController.h"
#include "dvs/ui/ReviewPreferencesController.h"
#include "dvs/ui/SourceListModel.h"

#include "PlaybackTraceEnvironment.h"
#include "ReviewRuntime.h"
#include "StartupFailureReporter.h"
#include "StartupRequest.h"
#include "StartupRequestBroker.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QUrl>

#include <Windows.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class SmokeStage {
    WaitingForGraphics,
    WaitingForFirstFrame,
    WaitingForNextFrame,
    WaitingForPreviousFrame,
    WaitingForLastFrame,
    WaitingForReturnToFirst,
    WaitingForPlaybackAdvance,
    WaitingForPlaybackPause,
    WaitingForPlaybackReturnToFirst,
    WaitingForFocusedComboShortcuts,
    WaitingForShortcutLast,
    WaitingForShortcutFirst,
    WaitingForShortcutNext,
    WaitingForShortcutPrevious,
    WaitingForLargeStepForward,
    WaitingForLargeStepBackward,
    WaitingForTimelineMiddle,
    WaitingForTimelineLast,
    WaitingForLocalizedError,
};

struct SmokeSources final {
    std::filesystem::path first;
    std::optional<std::filesystem::path> second;
    std::optional<std::filesystem::path> third;
};

enum class PerformanceComparisonMode {
    Side,
    Wipe,
    Difference,
};

struct PerformanceInvocation final {
    SmokeSources sources;
    std::chrono::seconds duration;
    PerformanceComparisonMode comparisonMode = PerformanceComparisonMode::Side;
};

struct PerformanceMetrics final {
    std::uint64_t presentedFrames = 0U;
    std::uint64_t droppedFrames = 0U;
    std::uint64_t sourceSplitObservations = 0U;
    std::size_t peakFrameBytes = 0U;
    std::size_t peakWorkingSetBytes = 0U;
    std::size_t baselineThreads = 0U;
    std::size_t peakThreads = 0U;
    std::size_t finalThreads = 0U;
    qint64 playbackResponseMilliseconds = -1;
    qint64 openFirstFrameMilliseconds = -1;
    qint64 seekP50Milliseconds = -1;
    qint64 seekP95Milliseconds = -1;
    qint64 warmStepP50Milliseconds = -1;
    qint64 warmStepP95Milliseconds = -1;
    // Held-forward step gate (plan 1.6 M1.1). Latency percentiles are computed over the
    // committed-frame latency samples collected during Stage::HeldStepping.
    qint64 heldStepP50Milliseconds = -1;
    qint64 heldStepP95Milliseconds = -1;
    qint64 heldStepP99Milliseconds = -1;
    std::uint64_t heldStepPresentedFrames = 0U;
    std::uint64_t heldStepSequenceErrors = 0U;
    std::uint64_t heldStepGenerationDelta = 0U;
    std::uint64_t heldStepExactSeekDelta = 0U;
    std::uint64_t heldStepSequentialRequestCount = 0U;
    std::uint64_t heldStepCancelCount = 0U;
    std::uint64_t heldStepDecoderReopenCount = 0U;
    // Fraction of held-window decode operations that were sequential continuations (not exact
    // seeks). 1.0 means every decode continued from the previous frame; lower means the decoder
    // fell back to exact seeks. Computed in finalizeHeldStep from decoder backend status deltas.
    double heldStepSequentialRatio = 0.0;
    qint64 analysisMilliseconds = -1;
    std::uint64_t analysisDecodedFrames = 0U;
    qint64 shutdownMilliseconds = -1;
    std::uint64_t comparisonSampledPixels = 0U;
    double comparisonBrightPixelRatio = 0.0;
    bool comparisonModeVerified = false;
    bool comparisonFrameRetained = false;
    // T0 evidence baseline (plan: correctness reproduction and playback evidence). These are
    // additive observations only; no playback behavior changes. "New content display interval"
    // is the GUI-thread elapsed time between successive committed canonical frames during the
    // Running window (post-warmup) — the metric behind "longest pause" and the P50/P95/P99
    // display cadence the plan requires instead of average FPS alone.
    std::vector<qint64> displayIntervalsMilliseconds;
    std::vector<qint64> uiLoopGapMicroseconds;
    qint64 displayIntervalP50Milliseconds = -1;
    qint64 displayIntervalP95Milliseconds = -1;
    qint64 displayIntervalP99Milliseconds = -1;
    qint64 displayIntervalMaximumMilliseconds = -1;
    qint64 uiLoopGapP50Milliseconds = -1;
    qint64 uiLoopGapP95Milliseconds = -1;
    qint64 uiLoopGapP99Milliseconds = -1;
    qint64 uiLoopGapMaximumMilliseconds = -1;
};

// Tail-latency summary used by the T0 evidence baseline for display intervals and UI loop gaps.
struct TailSummary final {
    qint64 p50 = -1;
    qint64 p95 = -1;
    qint64 p99 = -1;
    qint64 maximum = -1;
};

void writeStandardError(std::string_view message) noexcept {
    const HANDLE standardError = GetStdHandle(STD_ERROR_HANDLE);
    if (standardError == nullptr || standardError == INVALID_HANDLE_VALUE) {
        return;
    }
    while (!message.empty()) {
        const std::size_t chunkSize =
            std::min<std::size_t>(message.size(), (std::numeric_limits<DWORD>::max)());
        DWORD written = 0U;
        if (WriteFile(
                standardError, message.data(), static_cast<DWORD>(chunkSize), &written, nullptr) ==
                FALSE ||
            written == 0U) {
            return;
        }
        message.remove_prefix(written);
    }
}

[[nodiscard]] std::optional<std::chrono::seconds> parseDuration(const std::string_view text) {
    std::int64_t seconds = 0;
    const auto [position, error] = std::from_chars(text.data(), text.data() + text.size(), seconds);
    if (error != std::errc{} || position != text.data() + text.size() || seconds < 5 ||
        seconds > 3600) {
        return std::nullopt;
    }
    return std::chrono::seconds{seconds};
}

[[nodiscard]] std::optional<PerformanceComparisonMode>
parsePerformanceComparisonMode(const std::string_view text) {
    if (text == "side") {
        return PerformanceComparisonMode::Side;
    }
    if (text == "wipe") {
        return PerformanceComparisonMode::Wipe;
    }
    if (text == "diff") {
        return PerformanceComparisonMode::Difference;
    }
    return std::nullopt;
}

[[nodiscard]] std::string_view
performanceComparisonModeName(const PerformanceComparisonMode comparisonMode) noexcept {
    switch (comparisonMode) {
    case PerformanceComparisonMode::Side:
        return "side";
    case PerformanceComparisonMode::Wipe:
        return "wipe";
    case PerformanceComparisonMode::Difference:
        return "diff";
    }
    return "unknown";
}

[[nodiscard]] std::string_view
performanceModeControlName(const PerformanceComparisonMode comparisonMode) noexcept {
    switch (comparisonMode) {
    case PerformanceComparisonMode::Side:
        return "sideModeButton";
    case PerformanceComparisonMode::Wipe:
        return "wipeModeButton";
    case PerformanceComparisonMode::Difference:
        return "diffModeButton";
    }
    return {};
}

[[nodiscard]] dvs::ui::ComparisonSurface::ViewMode
performanceSurfaceMode(const PerformanceComparisonMode comparisonMode) noexcept {
    switch (comparisonMode) {
    case PerformanceComparisonMode::Side:
        return dvs::ui::ComparisonSurface::SideBySide;
    case PerformanceComparisonMode::Wipe:
        return dvs::ui::ComparisonSurface::Wipe;
    case PerformanceComparisonMode::Difference:
        return dvs::ui::ComparisonSurface::Difference;
    }
    return dvs::ui::ComparisonSurface::SideBySide;
}

void installPlaybackTrace(dvs::app::ReviewRuntime& runtime) {
    const std::optional<std::filesystem::path> tracePath =
        dvs::app::playbackTracePathFromEnvironment();
    if (!tracePath.has_value()) {
        return;
    }
    auto sink = std::make_shared<dvs::platform::FileTraceSink>(*tracePath);
    dvs::application::PlaybackTrace::instance().installSink(sink.get());
    dvs::application::PlaybackTrace::instance().enable(dvs::application::traceNowMicroseconds);
    runtime.setTraceSink(std::move(sink));
}

[[nodiscard]] std::optional<PerformanceInvocation> parsePerformanceInvocation(const int argc,
                                                                              char** const argv) {
    if (argc < 5 || argv == nullptr || std::string_view{argv[1]} != "--ui-performance") {
        return std::nullopt;
    }
    std::vector<std::filesystem::path> sourcePaths;
    std::optional<std::chrono::seconds> duration;
    PerformanceComparisonMode comparisonMode = PerformanceComparisonMode::Side;
    for (int index = 2; index < argc;) {
        const std::string_view argument{argv[index]};
        if (argument == "--seconds") {
            if (duration.has_value() || index + 1 >= argc) {
                return std::nullopt;
            }
            duration = parseDuration(argv[index + 1]);
            if (!duration.has_value()) {
                return std::nullopt;
            }
            index += 2;
            continue;
        }
        if (argument == "--mode") {
            if (index + 1 >= argc) {
                return std::nullopt;
            }
            const std::optional<PerformanceComparisonMode> parsed =
                parsePerformanceComparisonMode(argv[index + 1]);
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            comparisonMode = *parsed;
            index += 2;
            continue;
        }
        if (argument.starts_with("--") || sourcePaths.size() == 3U) {
            return std::nullopt;
        }
        sourcePaths.emplace_back(argv[index]);
        ++index;
    }
    if (sourcePaths.empty() || !duration.has_value() ||
        (comparisonMode != PerformanceComparisonMode::Side && sourcePaths.size() < 2U)) {
        return std::nullopt;
    }
    SmokeSources sources{.first = std::move(sourcePaths.front())};
    if (sourcePaths.size() >= 2U) {
        sources.second = std::move(sourcePaths[1U]);
    }
    if (sourcePaths.size() == 3U) {
        sources.third = std::move(sourcePaths[2U]);
    }
    return PerformanceInvocation{
        .sources = std::move(sources),
        .duration = *duration,
        .comparisonMode = comparisonMode,
    };
}

[[nodiscard]] double brightPixelRatio(const QImage& source, std::uint64_t& sampledPixels) {
    const QImage image = source.convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull()) {
        sampledPixels = 0U;
        return 0.0;
    }
    const int horizontalStep = (std::max)(1, image.width() / 192);
    const int verticalStep = (std::max)(1, image.height() / 108);
    std::uint64_t brightPixels = 0U;
    sampledPixels = 0U;
    for (int y = 0; y < image.height(); y += verticalStep) {
        const auto* const row = image.constScanLine(y);
        for (int x = 0; x < image.width(); x += horizontalStep) {
            const int offset = x * 4;
            const unsigned int intensity = static_cast<unsigned int>(row[offset]) +
                                           static_cast<unsigned int>(row[offset + 1]) +
                                           static_cast<unsigned int>(row[offset + 2]);
            ++sampledPixels;
            if (intensity >= 96U) {
                ++brightPixels;
            }
        }
    }
    return sampledPixels == 0U
               ? 0.0
               : static_cast<double>(brightPixels) / static_cast<double>(sampledPixels);
}

[[nodiscard]] bool hasReviewError(const dvs::ui::ReviewController& controller) {
    return !controller.sourceAErrorKey().isEmpty() || !controller.sourceBErrorKey().isEmpty() ||
           !controller.sourceCErrorKey().isEmpty() || !controller.pairErrorKey().isEmpty();
}

[[nodiscard]] QUrl localFileUrl(const std::filesystem::path& path) {
    return QUrl::fromLocalFile(QString::fromStdWString(path.wstring()));
}

[[nodiscard]] bool applyStartupRequest(const dvs::app::StartupRequest& request,
                                       dvs::ui::DesktopApplication& desktop) {
    desktop.activateWindow();
    switch (request.kind) {
    case dvs::app::StartupRequest::Kind::Empty:
        return true;
    case dvs::app::StartupRequest::Kind::PlaySingle:
    case dvs::app::StartupRequest::Kind::Compare: {
        QList<QUrl> sources;
        sources.reserve(static_cast<qsizetype>(request.sources.size()));
        for (const auto& source : request.sources) {
            sources.push_back(localFileUrl(source));
        }
        return desktop.enqueueStartupRequest(static_cast<int>(request.kind), sources);
    }
    }
    return false;
}

[[nodiscard]] int
runDesktop(int& argc,
           char** argv,
           const bool smokeMode,
           const std::optional<SmokeSources>& smokeSources = std::nullopt,
           const bool shutdownDuringOpen = false,
           const std::optional<std::filesystem::path>& stillImage = std::nullopt) {
    dvs::ui::configureGraphicsBackend();
    if (stillImage.has_value()) {
        std::ofstream early{
            std::filesystem::path{std::filesystem::temp_directory_path() / "dvs_still_open.log"}};
        early << "START " << stillImage->string() << '\n';
    }
    dvs::ui::DesktopApplication desktop{
        argc,
        argv,
        dvs::ui::DesktopApplicationOptions{
            .smokeMode = smokeMode,
            .preferSoftwareDevice = smokeMode,
        },
    };
    std::unique_ptr<dvs::app::StartupRequestBroker> startupBroker;
    dvs::app::StartupRequest startupRequest;
    if (!smokeMode && !stillImage.has_value()) {
        const dvs::app::StartupRequestParseResult parsed =
            dvs::app::parseStartupRequest(QCoreApplication::arguments());
        if (!parsed) {
            return dvs::app::reportFatalStartup(parsed.error.toStdString(), false);
        }
        startupRequest = *parsed.request;
        startupBroker = std::make_unique<dvs::app::StartupRequestBroker>();
        const auto brokerResult = startupBroker->startOrForward(startupRequest);
        if (brokerResult == dvs::app::StartupRequestBroker::StartResult::Forwarded) {
            return EXIT_SUCCESS;
        }
        if (brokerResult == dvs::app::StartupRequestBroker::StartResult::Failed) {
            return dvs::app::reportFatalStartup(
                "The VCStation startup request broker could not be initialized.", false);
        }
    }
    std::unique_ptr<dvs::app::ReviewRuntime> runtime = dvs::app::ReviewRuntime::create();
    if (!runtime || runtime->controller() == nullptr || runtime->preferences() == nullptr ||
        !desktop.load(*runtime->controller(),
                      *runtime->preferences(),
                      [&runtime](dvs::ui::ComparisonSurface& surface) {
                          return runtime->attachSurface(surface);
                      })) {
        std::cerr << "DVS_UI_LOAD_FAILED\n";
        if (runtime) {
            runtime->prepareForSceneGraphRelease();
        }
        desktop.releaseSceneGraph();
        if (runtime && !runtime->shutdownAfterSceneGraphRelease()) {
            std::cerr << "DVS_RUNTIME_SHUTDOWN_TIMEOUT\n" << std::flush;
            static_cast<void>(dvs::app::reportFatalStartup(
                "DVS_UI_LOAD_FAILED; DVS_RUNTIME_SHUTDOWN_TIMEOUT", smokeMode));
            std::_Exit(EXIT_FAILURE);
        }
        return dvs::app::reportFatalStartup("DVS_UI_LOAD_FAILED", smokeMode);
    }
    if (!smokeMode && !stillImage.has_value() && !applyStartupRequest(startupRequest, desktop)) {
        runtime->prepareForSceneGraphRelease();
        desktop.releaseSceneGraph();
        if (!runtime->shutdownAfterSceneGraphRelease()) {
            writeStandardError("DVS_RUNTIME_SHUTDOWN_TIMEOUT\n");
            std::_Exit(EXIT_FAILURE);
        }
        return dvs::app::reportFatalStartup("The requested startup action could not be opened.",
                                            false);
    }
    if (stillImage.has_value()) {
        std::ofstream log{
            std::filesystem::path{std::filesystem::temp_directory_path() / "dvs_still_open.log"},
            std::ios::app};
        log << "AFTER_LOAD\n";
        const bool opened = desktop.openStillImageForAutomation(localFileUrl(*stillImage));
        log << (opened ? "OK\n" : "FAILED\n") << stillImage->string() << '\n';
        writeStandardError(opened ? "DVS_STILL_OPEN_OK\n" : "DVS_STILL_OPEN_FAILED\n");
        if (!opened) {
            std::cerr << "still=" << stillImage->string() << '\n' << std::flush;
        }
        const int exitCode = opened ? EXIT_SUCCESS : EXIT_FAILURE;
        QTimer::singleShot(opened ? 300 : 0, QCoreApplication::instance(), [&desktop, exitCode] {
            desktop.exit(exitCode);
        });
    }
    if (startupBroker) {
        startupBroker->setRequestHandler([&desktop](dvs::app::StartupRequest request) {
            return applyStartupRequest(request, desktop);
        });
    }
    QTimer smokePoll;
    QTimer smokeTimeout;
    SmokeStage smokeStage = SmokeStage::WaitingForGraphics;
    bool smokeCompleted = false;
    if (smokeMode) {
        smokePoll.setInterval(5);
        QObject::connect(&smokePoll, &QTimer::timeout, runtime->controller(), [&] {
            dvs::ui::ReviewController& controller = *runtime->controller();
            if (smokeStage != SmokeStage::WaitingForLocalizedError && hasReviewError(controller) &&
                !controller.busy()) {
                std::cerr << "DVS_UI_SMOKE_MEDIA_ERROR"
                          << " stage=" << static_cast<int>(smokeStage)
                          << " sourceA=" << controller.sourceAErrorKey().toStdString()
                          << " sourceB=" << controller.sourceBErrorKey().toStdString()
                          << " sourceC=" << controller.sourceCErrorKey().toStdString()
                          << " comparison=" << controller.pairErrorKey().toStdString()
                          << " detail=" << controller.lastErrorTechnicalDetail().toStdString()
                          << '\n';
                desktop.exit(EXIT_FAILURE);
                return;
            }

            switch (smokeStage) {
            case SmokeStage::WaitingForGraphics: {
                if (!controller.graphicsReady()) {
                    return;
                }
                if (!smokeSources.has_value()) {
                    smokeCompleted = true;
                    desktop.exit(EXIT_SUCCESS);
                    return;
                }
                QList<QUrl> sources{localFileUrl(smokeSources->first)};
                if (smokeSources->second.has_value()) {
                    sources.push_back(localFileUrl(*smokeSources->second));
                }
                if (smokeSources->third.has_value()) {
                    sources.push_back(localFileUrl(*smokeSources->third));
                }
                const bool openAccepted = desktop.openSourcesForAutomation(sources);
                if (!openAccepted) {
                    std::cerr << "DVS_UI_SMOKE_OPEN_REJECTED\n";
                    desktop.exit(EXIT_FAILURE);
                    return;
                }
                smokeStage = SmokeStage::WaitingForFirstFrame;
                if (shutdownDuringOpen) {
                    if (!controller.busy()) {
                        std::cerr << "DVS_UI_SHUTDOWN_SMOKE_OPEN_NOT_PENDING\n";
                        desktop.exit(EXIT_FAILURE);
                        return;
                    }
                    smokeCompleted = true;
                    desktop.exit(EXIT_SUCCESS);
                }
                return;
            }
            case SmokeStage::WaitingForFirstFrame:
                if (controller.busy() || controller.currentFrame() != 0) {
                    return;
                }
                if (!desktop.clickControlForAutomation("nextButton")) {
                    std::cerr << "DVS_UI_SMOKE_NEXT_REJECTED\n";
                    desktop.exit(EXIT_FAILURE);
                    return;
                }
                smokeStage = SmokeStage::WaitingForNextFrame;
                return;
            case SmokeStage::WaitingForNextFrame:
                if (controller.busy() || controller.currentFrame() != 1) {
                    return;
                }
                if (!desktop.clickControlForAutomation("previousButton")) {
                    std::cerr << "DVS_UI_SMOKE_PREVIOUS_REJECTED\n";
                    desktop.exit(EXIT_FAILURE);
                    return;
                }
                smokeStage = SmokeStage::WaitingForPreviousFrame;
                return;
            case SmokeStage::WaitingForPreviousFrame:
                if (controller.busy() || controller.currentFrame() != 0) {
                    return;
                }
                if (!desktop.clickControlForAutomation("lastButton")) {
                    std::cerr << "DVS_UI_SMOKE_LAST_REJECTED\n";
                    desktop.exit(EXIT_FAILURE);
                    return;
                }
                smokeStage = SmokeStage::WaitingForLastFrame;
                return;
            case SmokeStage::WaitingForLastFrame:
                if (controller.busy() || controller.totalFrames() == 0U ||
                    controller.currentFrame() !=
                        static_cast<qint64>(controller.totalFrames() - 1U)) {
                    return;
                }
                if (!desktop.clickControlForAutomation("firstButton")) {
                    std::cerr << "DVS_UI_SMOKE_FIRST_REJECTED\n";
                    desktop.exit(EXIT_FAILURE);
                    return;
                }
                smokeStage = SmokeStage::WaitingForReturnToFirst;
                return;
            case SmokeStage::WaitingForReturnToFirst:
                if (controller.busy() || controller.currentFrame() != 0) {
                    return;
                }
                if (!desktop.clickControlForAutomation("playbackButton")) {
                    std::cerr << "DVS_UI_SMOKE_PLAY_REJECTED\n";
                    desktop.exit(EXIT_FAILURE);
                    return;
                }
                smokeStage = SmokeStage::WaitingForPlaybackAdvance;
                return;
            case SmokeStage::WaitingForPlaybackAdvance:
                if (controller.currentFrame() <= 0 || !controller.canPause()) {
                    return;
                }
                if (!desktop.focusControlForAutomation("mediaViewportFocusTarget") ||
                    !desktop.sendKeyForAutomation(Qt::Key_Space)) {
                    std::cerr << "DVS_UI_SMOKE_SHORTCUT_PAUSE_REJECTED\n";
                    desktop.exit(EXIT_FAILURE);
                    return;
                }
                smokeStage = SmokeStage::WaitingForPlaybackPause;
                return;
            case SmokeStage::WaitingForPlaybackPause:
                if (controller.playing() || !controller.canFirst()) {
                    return;
                }
                if (!desktop.clickControlForAutomation("firstButton")) {
                    std::cerr << "DVS_UI_SMOKE_PLAYBACK_FIRST_REJECTED\n";
                    desktop.exit(EXIT_FAILURE);
                    return;
                }
                smokeStage = SmokeStage::WaitingForPlaybackReturnToFirst;
                return;
            case SmokeStage::WaitingForPlaybackReturnToFirst:
                if (controller.busy() || controller.currentFrame() != 0) {
                    return;
                }
                if (!smokeSources->second.has_value()) {
                    if (!desktop.focusControlForAutomation("mediaViewportFocusTarget") ||
                        !desktop.sendKeyForAutomation(Qt::Key_End)) {
                        std::cerr << "DVS_UI_SMOKE_SINGLE_SHORTCUT_END_REJECTED\n";
                        desktop.exit(EXIT_FAILURE);
                        return;
                    }
                    smokeStage = SmokeStage::WaitingForShortcutLast;
                    return;
                }
                if (!desktop.focusControlForAutomation("sideModeButton") ||
                    !desktop.sendKeyForAutomation(Qt::Key_Space) ||
                    !desktop.sendKeyForAutomation(Qt::Key_Up)) {
                    std::cerr << "DVS_UI_SMOKE_FOCUSED_COMBO_KEYS_REJECTED\n";
                    desktop.exit(EXIT_FAILURE);
                    return;
                }
                smokeStage = SmokeStage::WaitingForFocusedComboShortcuts;
                return;
            case SmokeStage::WaitingForFocusedComboShortcuts:
                if (controller.playing() || controller.busy() || controller.currentFrame() != 0 ||
                    !controller.canPlay()) {
                    std::cerr << "DVS_UI_SMOKE_FOCUSED_COMBO_TRIGGERED_MEDIA_SHORTCUT\n";
                    desktop.exit(EXIT_FAILURE);
                    return;
                }
                if (!desktop.sendKeyForAutomation(Qt::Key_Escape) ||
                    !desktop.focusControlForAutomation("mediaViewportFocusTarget") ||
                    !desktop.sendKeyForAutomation(Qt::Key_End)) {
                    std::cerr << "DVS_UI_SMOKE_SHORTCUT_END_REJECTED\n";
                    desktop.exit(EXIT_FAILURE);
                    return;
                }
                smokeStage = SmokeStage::WaitingForShortcutLast;
                return;
            case SmokeStage::WaitingForShortcutLast:
                if (controller.busy() || controller.totalFrames() == 0U ||
                    controller.currentFrame() !=
                        static_cast<qint64>(controller.totalFrames() - 1U)) {
                    return;
                }
                if (!desktop.sendKeyForAutomation(Qt::Key_Home)) {
                    std::cerr << "DVS_UI_SMOKE_SHORTCUT_HOME_REJECTED\n";
                    desktop.exit(EXIT_FAILURE);
                    return;
                }
                smokeStage = SmokeStage::WaitingForShortcutFirst;
                return;
            case SmokeStage::WaitingForShortcutFirst:
                if (controller.busy() || controller.currentFrame() != 0) {
                    return;
                }
                // A/D are exact previous/next in every shortcut preset. Arrow keys intentionally
                // change meaning in the Premiere preset, so they cannot make this smoke test
                // deterministic when it runs with the user's persisted preferences.
                if (!desktop.sendKeyForAutomation(Qt::Key_D)) {
                    std::cerr << "DVS_UI_SMOKE_SHORTCUT_NEXT_REJECTED\n";
                    desktop.exit(EXIT_FAILURE);
                    return;
                }
                smokeStage = SmokeStage::WaitingForShortcutNext;
                return;
            case SmokeStage::WaitingForShortcutNext:
                if (controller.busy() || controller.currentFrame() != 1) {
                    return;
                }
                if (!desktop.sendKeyForAutomation(Qt::Key_A)) {
                    std::cerr << "DVS_UI_SMOKE_SHORTCUT_PREVIOUS_REJECTED\n";
                    desktop.exit(EXIT_FAILURE);
                    return;
                }
                smokeStage = SmokeStage::WaitingForShortcutPrevious;
                return;
            case SmokeStage::WaitingForShortcutPrevious:
                if (!controller.busy() && controller.currentFrame() == 0) {
                    if (!desktop.sendKeyForAutomation(Qt::Key_Right,
                                                      static_cast<int>(Qt::ShiftModifier))) {
                        std::cerr << "DVS_UI_SMOKE_SHORTCUT_SHIFT_RIGHT_REJECTED\n";
                        desktop.exit(EXIT_FAILURE);
                        return;
                    }
                    smokeStage = SmokeStage::WaitingForLargeStepForward;
                }
                return;
            case SmokeStage::WaitingForLargeStepForward:
                if (!controller.busy() && controller.totalFrames() > 0U) {
                    const qint64 expected =
                        std::min<qint64>(static_cast<qint64>(controller.totalFrames() - 1U), 5);
                    if (controller.currentFrame() != expected) {
                        return;
                    }
                    if (!desktop.sendKeyForAutomation(Qt::Key_Left,
                                                      static_cast<int>(Qt::ShiftModifier))) {
                        std::cerr << "DVS_UI_SMOKE_SHORTCUT_SHIFT_LEFT_REJECTED\n";
                        desktop.exit(EXIT_FAILURE);
                        return;
                    }
                    smokeStage = SmokeStage::WaitingForLargeStepBackward;
                }
                return;
            case SmokeStage::WaitingForLargeStepBackward:
                if (!controller.busy() && controller.currentFrame() == 0) {
                    if (!desktop.clickTimelineForAutomation(0.5)) {
                        std::cerr << "DVS_UI_SMOKE_TIMELINE_MIDDLE_REJECTED\n";
                        desktop.exit(EXIT_FAILURE);
                        return;
                    }
                    smokeStage = SmokeStage::WaitingForTimelineMiddle;
                }
                return;
            case SmokeStage::WaitingForTimelineMiddle:
                if (!controller.busy() && controller.totalFrames() > 0U &&
                    controller.currentFrame() ==
                        static_cast<qint64>((controller.totalFrames() - 1U + 1U) / 2U)) {
                    if (!desktop.clickTimelineForAutomation(1.0)) {
                        std::cerr << "DVS_UI_SMOKE_TIMELINE_LAST_REJECTED\n";
                        desktop.exit(EXIT_FAILURE);
                        return;
                    }
                    smokeStage = SmokeStage::WaitingForTimelineLast;
                }
                return;
            case SmokeStage::WaitingForTimelineLast:
                if (!controller.busy() && controller.totalFrames() > 0U &&
                    controller.currentFrame() ==
                        static_cast<qint64>(controller.totalFrames() - 1U)) {
                    if (!smokeSources->second.has_value()) {
                        smokeCompleted = true;
                        desktop.exit(EXIT_SUCCESS);
                        return;
                    }
                    std::filesystem::path missingSource = smokeSources->first;
                    missingSource += ".missing";
                    QList<QUrl> errorSources{localFileUrl(missingSource)};
                    if (smokeSources->second.has_value()) {
                        errorSources.push_back(localFileUrl(*smokeSources->second));
                    }
                    const bool errorOpenAccepted = desktop.openSourcesForAutomation(errorSources);
                    if (!errorOpenAccepted) {
                        std::cerr << "DVS_UI_SMOKE_ERROR_PATH_REJECTED\n";
                        desktop.exit(EXIT_FAILURE);
                        return;
                    }
                    smokeStage = SmokeStage::WaitingForLocalizedError;
                }
                return;
            case SmokeStage::WaitingForLocalizedError:
                if (controller.busy() || !hasReviewError(controller)) {
                    return;
                }
                if (const std::optional<std::string> detail =
                        desktop.objectStringPropertyForAutomation("frameErrorBannerDetail", "text");
                    !detail.has_value() || detail->find("source-missing") != std::string::npos ||
                    detail->find("所选源文件不存在或无法读取") == std::string::npos ||
                    desktop.objectStringPropertyForAutomation("frameErrorBanner", "visible") !=
                        std::optional<std::string>{"true"} ||
                    desktop.objectStringPropertyForAutomation("statusOverlay", "visible") !=
                        std::optional<std::string>{"false"}) {
                    std::cerr << "DVS_UI_SMOKE_ERROR_NOT_LOCALIZED\n";
                    desktop.exit(EXIT_FAILURE);
                } else {
                    smokeCompleted = true;
                    desktop.exit(EXIT_SUCCESS);
                }
                return;
            }
        });
        smokeTimeout.setSingleShot(true);
        smokeTimeout.setInterval(30'000);
        QObject::connect(&smokeTimeout,
                         &QTimer::timeout,
                         runtime->controller(),
                         [&desktop, &smokeStage, controller = runtime->controller()] {
                             std::cerr
                                 << "DVS_UI_SMOKE_TIMEOUT stage=" << static_cast<int>(smokeStage)
                                 << " frame=" << controller->currentFrame()
                                 << " busy=" << controller->busy()
                                 << " canNext=" << controller->canNext() << '\n';
                             desktop.exit(EXIT_FAILURE);
                         });
        smokePoll.start();
        smokeTimeout.start();
    }

    // Default-off Phase 0 trace. The sink is drained by the background runtime shutdown work.
    installPlaybackTrace(*runtime);

    int result = desktop.exec();
    smokePoll.stop();
    smokeTimeout.stop();
    if (smokeMode && !smokeCompleted) {
        std::cerr << "DVS_UI_SMOKE_INCOMPLETE\n";
        result = EXIT_FAILURE;
    }
    runtime->prepareForSceneGraphRelease();
    desktop.releaseSceneGraph();
    if (!runtime->shutdownAfterSceneGraphRelease()) {
        std::cerr << "DVS_RUNTIME_SHUTDOWN_TIMEOUT\n" << std::flush;
        static_cast<void>(dvs::app::reportFatalStartup("DVS_RUNTIME_SHUTDOWN_TIMEOUT", smokeMode));
        std::_Exit(EXIT_FAILURE);
    }
    return result;
}

[[nodiscard]] int runUpgradeSettingsSmoke(int& argc, char** argv, const SmokeSources& sources) {
    if (!sources.second.has_value()) {
        writeStandardError("DVS_UPGRADE_SETTINGS_SMOKE_REQUIRES_TWO_SOURCES\n");
        return EXIT_FAILURE;
    }
    dvs::ui::configureGraphicsBackend();
    dvs::ui::DesktopApplication desktop{
        argc,
        argv,
        dvs::ui::DesktopApplicationOptions{
            .smokeMode = true,
            .preferSoftwareDevice = true,
        },
    };
    std::unique_ptr<dvs::app::ReviewRuntime> runtime = dvs::app::ReviewRuntime::create();
    if (!runtime || runtime->controller() == nullptr || runtime->preferences() == nullptr ||
        !desktop.load(*runtime->controller(),
                      *runtime->preferences(),
                      [&runtime](dvs::ui::ComparisonSurface& surface) {
                          return runtime->attachSurface(surface);
                      })) {
        writeStandardError("DVS_UPGRADE_SETTINGS_UI_LOAD_FAILED\n");
        if (runtime) {
            runtime->prepareForSceneGraphRelease();
            desktop.releaseSceneGraph();
            if (!runtime->shutdownAfterSceneGraphRelease()) {
                writeStandardError("DVS_RUNTIME_SHUTDOWN_TIMEOUT\n");
                std::_Exit(EXIT_FAILURE);
            }
        }
        return EXIT_FAILURE;
    }

    enum class Stage {
        WaitingForPreferences,
        WaitingForGraphics,
        WaitingForFirstFrame,
        WaitingForEffectiveState,
    };
    Stage stage = Stage::WaitingForPreferences;
    bool passed = false;
    bool failed = false;
    std::string failureReason;
    const auto fail = [&](std::string reason) {
        if (!failed) {
            failed = true;
            failureReason = std::move(reason);
            desktop.exit(EXIT_FAILURE);
        }
    };

    QTimer poll;
    poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, runtime->controller(), [&] {
        dvs::ui::ReviewController& controller = *runtime->controller();
        const dvs::ui::ReviewPreferencesController& preferences = *runtime->preferences();
        switch (stage) {
        case Stage::WaitingForPreferences:
            if (preferences.viewMode() != dvs::ui::ReviewPreferencesController::ViewMode::Wipe ||
                preferences.differenceEdge() !=
                    dvs::ui::ReviewPreferencesController::DifferenceEdge::Edge0And2) {
                return;
            }
            stage = Stage::WaitingForGraphics;
            return;
        case Stage::WaitingForGraphics: {
            if (!controller.graphicsReady()) {
                return;
            }
            const QList<QUrl> automationSources{localFileUrl(sources.first),
                                                localFileUrl(*sources.second)};
            if (!desktop.openSourcesForAutomation(automationSources)) {
                fail("upgrade-settings-open-rejected");
                return;
            }
            stage = Stage::WaitingForFirstFrame;
            return;
        }
        case Stage::WaitingForFirstFrame:
            if (controller.busy()) {
                return;
            }
            if (hasReviewError(controller)) {
                fail("upgrade-settings-media-error");
                return;
            }
            if (controller.currentFrame() != 0) {
                return;
            }
            stage = Stage::WaitingForEffectiveState;
            return;
        case Stage::WaitingForEffectiveState: {
            const std::optional<int> effectiveMode =
                desktop.objectIntPropertyForAutomation("dualVideoSurface", "viewMode");
            const std::optional<int> effectiveEdge =
                desktop.objectIntPropertyForAutomation("dualVideoSurface", "differenceEdge");
            if (!effectiveMode.has_value() || !effectiveEdge.has_value()) {
                return;
            }
            if (*effectiveMode != static_cast<int>(dvs::ui::ComparisonSurface::Wipe) ||
                *effectiveEdge != static_cast<int>(dvs::ui::ComparisonSurface::Edge0And1)) {
                return;
            }
            passed = true;
            desktop.exit(EXIT_SUCCESS);
            return;
        }
        }
    });

    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(15'000);
    QObject::connect(&timeout, &QTimer::timeout, runtime->controller(), [&] {
        fail("upgrade-settings-timeout");
    });
    poll.start();
    timeout.start();
    int result = desktop.exec();
    poll.stop();
    timeout.stop();
    if (!passed) {
        result = EXIT_FAILURE;
        if (!failed) {
            failureReason = "upgrade-settings-incomplete";
        }
        writeStandardError("DVS_UPGRADE_SETTINGS_SMOKE_FAILED " + failureReason + '\n');
    }

    runtime->prepareForSceneGraphRelease();
    desktop.releaseSceneGraph();
    if (!runtime->shutdownAfterSceneGraphRelease()) {
        writeStandardError("DVS_RUNTIME_SHUTDOWN_TIMEOUT\n");
        std::_Exit(EXIT_FAILURE);
    }
    return result;
}

[[nodiscard]] int runPerformance(int& argc,
                                 char** argv,
                                 const SmokeSources& sources,
                                 const std::chrono::seconds duration,
                                 const PerformanceComparisonMode comparisonMode) {
    constexpr auto kWarmup = std::chrono::seconds{2};
    constexpr std::size_t kMaximumFrameBytes = 256U * 1024U * 1024U;
    dvs::ui::configureGraphicsBackend();
    dvs::ui::DesktopApplication desktop{
        argc,
        argv,
        dvs::ui::DesktopApplicationOptions{
            .smokeMode = false,
            .preferSoftwareDevice = false,
            .preferHighRefreshScreen = true,
        },
    };
    std::unique_ptr<dvs::app::ReviewRuntime> runtime = dvs::app::ReviewRuntime::create();
    if (!runtime || runtime->controller() == nullptr || runtime->preferences() == nullptr ||
        !desktop.load(*runtime->controller(),
                      *runtime->preferences(),
                      [&runtime](dvs::ui::ComparisonSurface& surface) {
                          return runtime->attachSurface(surface);
                      })) {
        writeStandardError("DVS_PERFORMANCE_UI_LOAD_FAILED\n");
        if (runtime) {
            runtime->prepareForSceneGraphRelease();
            desktop.releaseSceneGraph();
            if (!runtime->shutdownAfterSceneGraphRelease()) {
                writeStandardError("DVS_RUNTIME_SHUTDOWN_TIMEOUT\n");
                std::_Exit(EXIT_FAILURE);
            }
        }
        return EXIT_FAILURE;
    }
    installPlaybackTrace(*runtime);
    enum class Stage {
        WaitingForGraphics,
        WaitingForFirstFrame,
        WaitingForComparisonMode,
        WaitingForComparisonPixels,
        WaitingForPlayback,
        Running,
        WaitingForPause,
        Seeking,
        WarmStepping,
        HeldStepping,
        Analyzing,
    };
    Stage stage = Stage::WaitingForGraphics;
    PerformanceMetrics metrics;
    QElapsedTimer responseTimer;
    QElapsedTimer openTimer;
    QElapsedTimer playbackTimer;
    QElapsedTimer seekTimer;
    QElapsedTimer warmStepTimer;
    QElapsedTimer analysisTimer;
    QElapsedTimer comparisonModeTimer;
    qint64 lastCountedFrame = -1;
    qint64 comparisonFrameBeforeSwitch = -1;
    // T0 evidence: monotonic playback-clock position (ms) of the last counted new-content
    // commit, and the heartbeat clock for UI event-loop gap observation during Running.
    qint64 lastCountedFrameTimeMilliseconds = -1;
    QElapsedTimer uiLoopHeartbeat;
    qint64 lastHeartbeatTimeMilliseconds = -1;
    std::vector<qint64> seekTargets;
    std::vector<qint64> seekMilliseconds;
    std::vector<qint64> warmStepMilliseconds;
    std::vector<dvs::media::DecoderBackendStatus> playbackBackends;
    dvs::platform::GpuTransferStatistics playbackTransfer;
    std::optional<dvs::ui::RenderAckRelayStatistics> playbackRelayBaseline;
    std::optional<dvs::ui::RenderAckRelayStatistics> playbackRelayEnd;
    std::size_t seekIndex = 0U;
    std::size_t warmStepIndex = 0U;
    qint64 warmStepTarget = -1;
    // Held-forward step gate (plan 1.6 M1.1). The cadence test submits up to 300 consecutive +1
    // steps at 25-35 Hz and records per committed frame: the latency, and whether the presented
    // FrameId is exactly +1 from the previous (sequence error if not). Provider statistics deltas
    // captured across the window yield the sequential-request, generation, exact-seek, cancel and
    // decoder-reopen counters. The saturation test (submit-when-bounded-lookahead-accepts) is
    // deferred: it requires per-submit lookahead-accept feedback the controller does not expose.
    // The sample count is a runtime value (set once the fixture length is known after the seek)
    // capped at the frames remaining past the middle seek target — otherwise a fixture shorter
    // than ~2 * 300 frames would run past its end and hard-fail as "held-step-rejected".
    static constexpr std::size_t kHeldStepSamplesMax = 300U;
    std::size_t heldStepSamples = kHeldStepSamplesMax;
    // 25-35 Hz input cadence with deterministic jitter (milliseconds between submits). 34 ms base
    // +/- 5 ms jitter keeps the interval in 29-39 ms (25.6-34.5 Hz), inside the 25-35 Hz band.
    static constexpr qint64 kHeldStepCadenceBaseMs = 34U;
    static constexpr qint64 kHeldStepCadenceJitterMs = 5U;
    std::vector<qint64> heldStepMilliseconds;
    std::deque<qint64> heldStepSubmitTimes;
    std::size_t heldStepIndex = 0U;
    // Submitted vs presented counts expose completion gaps the committed-frame poll cannot see
    // (a step rejected or absorbed without a commit would otherwise only depress the sample
    // count, letting a short window pass as a healthy one).
    std::uint64_t heldStepSubmittedFrames = 0U;
    qint64 heldStepLastPresentedFrame = -1;
    qint64 heldStepSeekTarget = -1;
    std::uint64_t heldStepSequenceErrors = 0U;
    bool heldStepSeekedToMiddle = false;
    QElapsedTimer heldStepTimer;
    qint64 heldStepNextDeadlineMs = 0;
    std::optional<dvs::media::FrameProviderStatistics> heldStepProviderBaseline;
    std::optional<dvs::media::FrameProviderStatistics> heldStepProviderEnd;
    std::vector<dvs::media::DecoderBackendStatus> heldStepDecoderBaseline;
    std::vector<dvs::media::DecoderBackendStatus> heldStepDecoderEnd;
    std::uint64_t analysisSignatureBaseline = 0U;
    bool analysisObservedRunning = false;
    bool completed = false;
    bool failed = false;
    std::string failureReason;
    const std::size_t expectedSourceCount = 1U +
                                            static_cast<std::size_t>(sources.second.has_value()) +
                                            static_cast<std::size_t>(sources.third.has_value());
    const bool requiresComparisonPixels =
        expectedSourceCount > 1U && comparisonMode != PerformanceComparisonMode::Side;

    const auto fail = [&](std::string reason) {
        if (!failed) {
            failed = true;
            failureReason = std::move(reason);
            desktop.exit(EXIT_FAILURE);
        }
    };
    const auto samplePresentedSources = [&] {
        auto* const model = runtime->controller()->sources();
        const qint64 canonical = runtime->controller()->currentFrame();
        if (model == nullptr || canonical < 0 ||
            model->rowCount() != static_cast<int>(expectedSourceCount)) {
            ++metrics.sourceSplitObservations;
            return;
        }
        for (int row = 0; row < model->rowCount(); ++row) {
            const QVariant sourceFrame =
                model->data(model->index(row, 0), dvs::ui::SourceListModel::CurrentSourceFrameRole);
            if (!sourceFrame.isValid() || sourceFrame.toLongLong() != canonical) {
                ++metrics.sourceSplitObservations;
                return;
            }
        }
    };
    const auto sampleFrame = [&] {
        if (stage != Stage::Running) {
            return;
        }
        const qint64 current = runtime->controller()->currentFrame();
        if (current < 0 || current == lastCountedFrame) {
            return;
        }
        if (playbackTimer.elapsed() < kWarmup.count() * 1000) {
            return;
        }
        if (lastCountedFrame >= 0 && current <= lastCountedFrame) {
            fail("canonical-frame-regressed");
            return;
        }
        lastCountedFrame = current;
        const qint64 frameTime = playbackTimer.elapsed();
        if (lastCountedFrameTimeMilliseconds >= 0) {
            metrics.displayIntervalsMilliseconds.push_back(frameTime -
                                                           lastCountedFrameTimeMilliseconds);
        }
        lastCountedFrameTimeMilliseconds = frameTime;
        samplePresentedSources();
    };
    QObject::connect(runtime->controller(),
                     &dvs::ui::ReviewController::stateChanged,
                     runtime->controller(),
                     sampleFrame);
    const auto startSeek = [&] {
        if (seekIndex >= seekTargets.size()) {
            return false;
        }
        seekTimer.start();
        return runtime->controller()->seekFrame(seekTargets[seekIndex]);
    };
    const auto startWarmStep = [&] {
        constexpr std::size_t kWarmStepSamples = 20U;
        if (warmStepIndex >= kWarmStepSamples) {
            return false;
        }
        const qint64 delta = (warmStepIndex % 2U) == 0U ? 1 : -1;
        warmStepTarget = runtime->controller()->currentFrame() + delta;
        warmStepTimer.start();
        return runtime->controller()->stepFrames(delta);
    };
    // Held-forward cadence: deterministic jitter around a 25-35 Hz input rate. Each call advances
    // the deadline by a fresh jittered interval; the first call arms the initial deadline.
    const auto heldStepCadenceMs = [&] {
        const qint64 jitter =
            static_cast<qint64>((heldStepIndex * 5U) % (2U * kHeldStepCadenceJitterMs + 1U)) -
            kHeldStepCadenceJitterMs;
        heldStepNextDeadlineMs = heldStepTimer.elapsed() + kHeldStepCadenceBaseMs + jitter;
    };
    // Compute held-step latency percentiles and provider-statistic deltas for the window.
    const auto finalizeHeldStep = [&] {
        if (!heldStepProviderBaseline.has_value() || !heldStepProviderEnd.has_value()) {
            fail("held-step-statistics-missing");
            return;
        }
        const auto& base = *heldStepProviderBaseline;
        const auto& end = *heldStepProviderEnd;
        metrics.heldStepSequentialRequestCount =
            end.sequentialRequestCount - base.sequentialRequestCount;
        metrics.heldStepCancelCount = end.cancelCount - base.cancelCount;
        metrics.heldStepDecoderReopenCount = end.decoderReopenCount - base.decoderReopenCount;
        metrics.heldStepGenerationDelta = end.generationDeltaCount - base.generationDeltaCount;
        const auto accumulateExactSeeks =
            [](const std::vector<dvs::media::DecoderBackendStatus>& statuses) {
                return std::accumulate(statuses.begin(),
                                       statuses.end(),
                                       std::uint64_t{0U},
                                       [](const std::uint64_t total, const auto& status) {
                                           return total + status.exactSeekCount;
                                       });
            };
        const std::uint64_t baseExactSeeks = accumulateExactSeeks(heldStepDecoderBaseline);
        const std::uint64_t endExactSeeks = accumulateExactSeeks(heldStepDecoderEnd);
        metrics.heldStepExactSeekDelta = endExactSeeks - baseExactSeeks;
        // Sequential continuations are decodes that did NOT exact-seek. The ratio is the share of
        // held-window decodes served by the sequential cursor, which is exactly the "sequential
        // continuation >= 95%" gate. A healthy held-forward run on a warmed pipeline is ~1.0.
        const auto accumulateDecodes =
            [](const std::vector<dvs::media::DecoderBackendStatus>& statuses) {
                return std::accumulate(statuses.begin(),
                                       statuses.end(),
                                       std::uint64_t{0U},
                                       [](const std::uint64_t total, const auto& status) {
                                           return total + status.completedDecodeCount;
                                       });
            };
        const std::uint64_t baseDecodes = accumulateDecodes(heldStepDecoderBaseline);
        const std::uint64_t endDecodes = accumulateDecodes(heldStepDecoderEnd);
        const std::uint64_t windowDecodes = endDecodes - baseDecodes;
        const std::uint64_t windowExactSeeks = endExactSeeks - baseExactSeeks;
        if (windowDecodes == 0U) {
            metrics.heldStepSequentialRatio = 0.0;
        } else if (windowExactSeeks >= windowDecodes) {
            metrics.heldStepSequentialRatio = 0.0;
        } else {
            metrics.heldStepSequentialRatio =
                static_cast<double>(windowDecodes - windowExactSeeks) /
                static_cast<double>(windowDecodes);
        }
        metrics.heldStepSequenceErrors = heldStepSequenceErrors;
        std::ranges::sort(heldStepMilliseconds);
        const std::size_t samples = heldStepMilliseconds.size();
        if (samples > 0U) {
            metrics.heldStepP50Milliseconds = heldStepMilliseconds[samples / 2U];
            metrics.heldStepP95Milliseconds =
                heldStepMilliseconds[static_cast<std::size_t>(samples * 95U / 100U)];
            metrics.heldStepP99Milliseconds =
                heldStepMilliseconds[static_cast<std::size_t>(samples * 99U / 100U)];
        }
    };

    // T0 evidence: a 1 ms heartbeat observes UI event-loop gaps during the Running window.
    // A main-thread stall (synchronous decode, file I/O, scene-graph hitch) appears as a gap
    // far above the timer baseline. Reports percentiles and the maximum rather than relying on
    // average FPS. Runs only while playback is being measured so seek/analysis stages do not
    // contaminate the playback-window signal.
    QTimer uiLoopHeartbeatTimer;
    uiLoopHeartbeatTimer.setInterval(1);
    QObject::connect(&uiLoopHeartbeatTimer, &QTimer::timeout, runtime->controller(), [&] {
        const qint64 now = uiLoopHeartbeat.elapsed();
        if (lastHeartbeatTimeMilliseconds >= 0) {
            metrics.uiLoopGapMicroseconds.push_back((now - lastHeartbeatTimeMilliseconds) * 1000);
        }
        lastHeartbeatTimeMilliseconds = now;
    });

    QTimer poll;
    poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, runtime->controller(), [&] {
        dvs::ui::ReviewController& controller = *runtime->controller();
        if (hasReviewError(controller) && !controller.busy()) {
            fail("media-error:" + controller.lastErrorTechnicalDetail().toStdString());
            return;
        }

        metrics.peakFrameBytes = std::max(metrics.peakFrameBytes, runtime->reservedFrameBytes());

        switch (stage) {
        case Stage::WaitingForGraphics: {
            if (!controller.graphicsReady()) {
                return;
            }
            QList<QUrl> automationSources{localFileUrl(sources.first)};
            if (sources.second.has_value()) {
                automationSources.push_back(localFileUrl(*sources.second));
            }
            if (sources.third.has_value()) {
                automationSources.push_back(localFileUrl(*sources.third));
            }
            const bool openAccepted = desktop.openSourcesForAutomation(automationSources);
            if (!openAccepted) {
                fail("open-rejected");
                return;
            }
            openTimer.start();
            stage = Stage::WaitingForFirstFrame;
            return;
        }
        case Stage::WaitingForFirstFrame:
            if (controller.busy() || controller.currentFrame() != 0) {
                return;
            }
            metrics.openFirstFrameMilliseconds = openTimer.elapsed();
            metrics.baselineThreads = dvs::platform::sampleCurrentProcessTelemetry().threadCount;
            metrics.peakThreads = metrics.baselineThreads;
            if (expectedSourceCount > 1U) {
                comparisonFrameBeforeSwitch = controller.currentFrame();
                if (!desktop.clickControlForAutomation(
                        performanceModeControlName(comparisonMode))) {
                    fail("comparison-mode-click-rejected");
                    return;
                }
                stage = Stage::WaitingForComparisonMode;
                return;
            }
            metrics.comparisonModeVerified = true;
            responseTimer.start();
            if (!controller.play()) {
                fail("play-rejected");
                return;
            }
            stage = Stage::WaitingForPlayback;
            return;
        case Stage::WaitingForComparisonMode: {
            if (controller.busy() || comparisonFrameBeforeSwitch < 0 ||
                controller.currentFrame() != comparisonFrameBeforeSwitch) {
                fail("comparison-mode-lost-frame");
                return;
            }
            const std::optional<int> effectiveMode =
                desktop.objectIntPropertyForAutomation("dualVideoSurface", "viewMode");
            if (!effectiveMode.has_value()) {
                fail("comparison-mode-surface-missing");
                return;
            }
            if (*effectiveMode != static_cast<int>(performanceSurfaceMode(comparisonMode))) {
                return;
            }
            comparisonModeTimer.start();
            stage = Stage::WaitingForComparisonPixels;
            return;
        }
        case Stage::WaitingForComparisonPixels:
            // A scene-graph frame must be allowed to consume the selected mode before its pixels
            // are captured. The capture is intentionally performed through the visible surface.
            if (controller.busy() || comparisonFrameBeforeSwitch < 0 ||
                controller.currentFrame() != comparisonFrameBeforeSwitch) {
                fail("comparison-mode-lost-frame");
                return;
            }
            if (comparisonModeTimer.elapsed() < 100) {
                return;
            }
            if (requiresComparisonPixels) {
                const std::optional<QImage> image =
                    desktop.captureControlForAutomation("dualVideoSurface");
                if (!image.has_value()) {
                    fail("comparison-mode-capture-failed");
                    return;
                }
                metrics.comparisonBrightPixelRatio =
                    brightPixelRatio(*image, metrics.comparisonSampledPixels);
                metrics.comparisonModeVerified = metrics.comparisonSampledPixels > 0U &&
                                                 metrics.comparisonBrightPixelRatio >= 0.01;
                if (!metrics.comparisonModeVerified) {
                    fail("comparison-mode-black-output");
                    return;
                }
            } else {
                metrics.comparisonModeVerified = true;
            }
            metrics.comparisonFrameRetained = true;
            responseTimer.start();
            if (!controller.play()) {
                fail("play-rejected");
                return;
            }
            stage = Stage::WaitingForPlayback;
            return;
        case Stage::WaitingForPlayback:
            if (!controller.playing()) {
                return;
            }
            metrics.playbackResponseMilliseconds = responseTimer.elapsed();
            playbackTimer.start();
            uiLoopHeartbeat.start();
            lastHeartbeatTimeMilliseconds = -1;
            uiLoopHeartbeatTimer.start();
            stage = Stage::Running;
            return;
        case Stage::Running:
            sampleFrame();
            if (!controller.playing() && playbackTimer.elapsed() < duration.count() * 1000) {
                fail("playback-ended-before-duration");
                return;
            }
            if (!playbackRelayBaseline.has_value() &&
                playbackTimer.elapsed() >= kWarmup.count() * 1000) {
                playbackRelayBaseline = runtime->renderRelayStatistics();
            }
            if (playbackTimer.elapsed() < duration.count() * 1000) {
                return;
            }
            uiLoopHeartbeatTimer.stop();
            playbackRelayEnd = runtime->renderRelayStatistics();
            if (!controller.pause()) {
                fail("pause-rejected");
                return;
            }
            stage = Stage::WaitingForPause;
            return;
        case Stage::WaitingForPause:
            if (controller.playing()) {
                return;
            }
            playbackBackends = runtime->decoderBackendStatuses();
            playbackTransfer = runtime->transferStatistics();
            if (controller.totalFrames() < 100U) {
                fail("insufficient-frames-for-seek-sampling");
                return;
            }
            seekTargets.reserve(20U);
            for (std::size_t index = 0U; index < 20U; ++index) {
                const qulonglong percentile = ((index * 37U) % 97U) + 1U;
                const qulonglong frame = (controller.totalFrames() - 1U) * percentile / 100U;
                seekTargets.push_back(static_cast<qint64>(frame));
            }
            if (!startSeek()) {
                fail("seek-rejected");
                return;
            }
            stage = Stage::Seeking;
            return;
        case Stage::Seeking:
            if (controller.busy() || controller.currentFrame() != seekTargets[seekIndex]) {
                return;
            }
            seekMilliseconds.push_back(seekTimer.elapsed());
            ++seekIndex;
            if (seekIndex < seekTargets.size()) {
                if (!startSeek()) {
                    fail("seek-rejected");
                }
                return;
            }
            std::ranges::sort(seekMilliseconds);
            metrics.seekP50Milliseconds = seekMilliseconds[9U];
            metrics.seekP95Milliseconds = seekMilliseconds[18U];
            if (!startWarmStep()) {
                fail("warm-step-rejected");
                return;
            }
            stage = Stage::WarmStepping;
            return;
        case Stage::WarmStepping:
            if (controller.busy() || controller.currentFrame() != warmStepTarget) {
                return;
            }
            warmStepMilliseconds.push_back(warmStepTimer.elapsed());
            ++warmStepIndex;
            if (warmStepIndex < 20U) {
                if (!startWarmStep()) {
                    fail("warm-step-rejected");
                }
                return;
            }
            std::ranges::sort(warmStepMilliseconds);
            metrics.warmStepP50Milliseconds = warmStepMilliseconds[9U];
            metrics.warmStepP95Milliseconds = warmStepMilliseconds[18U];
            // Held-forward step gate (plan 1.6 M1.1): hold forward through 300 consecutive +1 steps
            // and measure latency + correctness before completing the run.
            heldStepSeekTarget = -1;
            heldStepSeekedToMiddle = false;
            stage = Stage::HeldStepping;
            return;
        case Stage::HeldStepping:
            if (!heldStepSeekedToMiddle) {
                if (heldStepSeekTarget < 0) {
                    // Seek to a safe middle frame so 300 forward steps cannot run past the end.
                    heldStepSeekTarget = static_cast<qint64>(controller.totalFrames() / 2U);
                    if (!controller.seekFrame(heldStepSeekTarget)) {
                        fail("held-step-seek-rejected");
                    }
                    return;
                }
                if (controller.busy() || controller.currentFrame() != heldStepSeekTarget) {
                    return;
                }
                heldStepSeekedToMiddle = true;
                heldStepLastPresentedFrame = controller.currentFrame();
                // Cap the window to the frames remaining past the middle seek target so the run can
                // never step past the fixture end (which would hard-fail as "held-step-rejected" on
                // short fixtures). totalFrames() is authoritative only after the seek commits.
                heldStepSamples =
                    std::min(kHeldStepSamplesMax,
                             static_cast<std::size_t>(std::max<qint64>(
                                 0, controller.totalFrames() - heldStepSeekTarget - 1)));
                heldStepTimer.start();
                heldStepCadenceMs();
                heldStepProviderBaseline = runtime->frameProviderStatistics();
                heldStepDecoderBaseline = runtime->decoderBackendStatuses();
                return;
            }
            // Scoped so its local declarations cannot be skipped by a later case label.
            {
                const qint64 presented = controller.currentFrame();
                if (presented != heldStepLastPresentedFrame) {
                    // Count missing intermediate FrameIds. A clean +1 advance is error-free; a jump
                    // of N frames means (N - 1) intermediate FrameIds were skipped. This maps
                    // directly to the "0 missing intermediate FrameIds" gate. A regression
                    // (presented <= last) is also a sequence error (and is separately caught by the
                    // canonical-regression gate).
                    if (presented > heldStepLastPresentedFrame) {
                        heldStepSequenceErrors +=
                            static_cast<std::uint64_t>(presented - heldStepLastPresentedFrame - 1);
                    } else {
                        ++heldStepSequenceErrors;
                    }
                    if (!heldStepSubmitTimes.empty()) {
                        heldStepMilliseconds.push_back(heldStepTimer.elapsed() -
                                                       heldStepSubmitTimes.front());
                        heldStepSubmitTimes.pop_front();
                    }
                    heldStepLastPresentedFrame = presented;
                    ++metrics.heldStepPresentedFrames;
                }
            } // end presented-frame detection scope
            if (heldStepIndex >= heldStepSamples) {
                if (controller.busy()) {
                    return;
                }
                heldStepProviderEnd = runtime->frameProviderStatistics();
                heldStepDecoderEnd = runtime->decoderBackendStatuses();
                finalizeHeldStep();
                if (expectedSourceCount == 1U) {
                    metrics.finalThreads =
                        dvs::platform::sampleCurrentProcessTelemetry().threadCount;
                    completed = true;
                    desktop.exit(EXIT_SUCCESS);
                    return;
                }
                analysisSignatureBaseline = runtime->decodedSignatureCount();
                analysisTimer.start();
                if (!controller.estimateAlignment()) {
                    fail("analysis-rejected");
                    return;
                }
                stage = Stage::Analyzing;
                return;
            }
            // Paced cadence: never submit while the controller is busy. dispatchNavigation rejects
            // stepFrames when busy (canNavigate requires !busy), so submitting unconditionally
            // would fail the run. The cadence deadline sets the minimum interval; the controller's
            // own pacing sets the effective rate when steps take longer than the cadence.
            if (!controller.busy() && heldStepTimer.elapsed() >= heldStepNextDeadlineMs) {
                heldStepSubmitTimes.push_back(heldStepTimer.elapsed());
                if (!controller.stepFrames(1)) {
                    fail("held-step-rejected");
                    return;
                }
                ++heldStepIndex;
                ++heldStepSubmittedFrames;
                heldStepCadenceMs();
            }
            return;
        case Stage::Analyzing:
            if (controller.alignmentAnalysisRunning()) {
                analysisObservedRunning = true;
                return;
            }
            if (!analysisObservedRunning || controller.busy()) {
                return;
            }
            metrics.analysisMilliseconds = analysisTimer.elapsed();
            metrics.analysisDecodedFrames =
                runtime->decodedSignatureCount() - analysisSignatureBaseline;
            metrics.finalThreads = dvs::platform::sampleCurrentProcessTelemetry().threadCount;
            completed = true;
            desktop.exit(EXIT_SUCCESS);
            return;
        }
    });

    // OS thread enumeration can take several display intervals. Keep it off the GUI loop;
    // the sampler owns its counters until the event loop has exited and the worker is joined.
    dvs::platform::ProcessTelemetrySampler telemetrySampler;

    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(static_cast<int>((duration + std::chrono::seconds{90}).count() * 1000));
    QObject::connect(
        &timeout, &QTimer::timeout, runtime->controller(), [&] { fail("performance-timeout"); });
    poll.start();
    timeout.start();
    int result = desktop.exec();
    poll.stop();
    timeout.stop();
    uiLoopHeartbeatTimer.stop();
    const dvs::platform::ProcessTelemetry telemetryPeaks = telemetrySampler.stopAndTakePeaks();
    metrics.peakWorkingSetBytes =
        std::max(metrics.peakWorkingSetBytes, telemetryPeaks.workingSetBytes);
    metrics.peakThreads = std::max(metrics.peakThreads, telemetryPeaks.threadCount);

    const dvs::platform::GpuTransferStatistics transfer = runtime->transferStatistics();
    const dvs::ui::RenderAckRelayStatistics relay = runtime->renderRelayStatistics();
    const dvs::media::MediaProbeStatistics probe = runtime->mediaProbeStatistics();
    const dvs::media::FrameProviderStatistics provider = runtime->frameProviderStatistics();
    const std::vector<dvs::media::DecoderBackendStatus> backends =
        runtime->decoderBackendStatuses();
    if (playbackRelayBaseline.has_value() && playbackRelayEnd.has_value()) {
        metrics.presentedFrames = playbackRelayEnd->acknowledgementsPopped -
                                  playbackRelayBaseline->acknowledgementsPopped;
        metrics.droppedFrames =
            playbackRelayEnd->canonicalFrameGaps - playbackRelayBaseline->canonicalFrameGaps;
        if (playbackRelayEnd->canonicalFrameRegressions !=
            playbackRelayBaseline->canonicalFrameRegressions) {
            failed = true;
            failureReason = "canonical-frame-regressed";
            result = EXIT_FAILURE;
        }
    } else {
        if (!failed) {
            failed = true;
            failureReason = "playback-ack-window-missing";
        }
        result = EXIT_FAILURE;
    }
    const std::uint64_t totalCounted = metrics.presentedFrames + metrics.droppedFrames;
    const double dropRatio = totalCounted == 0U ? 1.0
                                                : static_cast<double>(metrics.droppedFrames) /
                                                      static_cast<double>(totalCounted);
    const bool allHardware = backends.size() == expectedSourceCount &&
                             std::all_of(backends.begin(), backends.end(), [](const auto& status) {
                                 return status.backend == dvs::media::DecoderBackend::D3d11Va &&
                                        status.deviceGeneration.value() != 0U;
                             });
    const std::uint64_t completedDecodes =
        std::accumulate(playbackBackends.begin(),
                        playbackBackends.end(),
                        std::uint64_t{0U},
                        [](const std::uint64_t total, const auto& status) {
                            return total + status.completedDecodeCount;
                        });
    const std::uint64_t cacheHits = std::accumulate(
        playbackBackends.begin(),
        playbackBackends.end(),
        std::uint64_t{0U},
        [](const std::uint64_t total, const auto& status) { return total + status.cacheHitCount; });
    const std::uint64_t exactSeeks =
        std::accumulate(playbackBackends.begin(),
                        playbackBackends.end(),
                        std::uint64_t{0U},
                        [](const std::uint64_t total, const auto& status) {
                            return total + status.exactSeekCount;
                        });
    const std::uint64_t totalDecodeMicroseconds =
        std::accumulate(playbackBackends.begin(),
                        playbackBackends.end(),
                        std::uint64_t{0U},
                        [](const std::uint64_t total, const auto& status) {
                            return total + status.totalDecodeMicroseconds;
                        });
    const std::uint64_t maximumDecodeMicroseconds =
        std::accumulate(playbackBackends.begin(),
                        playbackBackends.end(),
                        std::uint64_t{0U},
                        [](const std::uint64_t maximum, const auto& status) {
                            return std::max(maximum, status.maximumDecodeMicroseconds);
                        });
    const double cacheHitRatio = completedDecodes == 0U ? 0.0
                                                        : static_cast<double>(cacheHits) /
                                                              static_cast<double>(completedDecodes);
    const double analysisFramesPerSecond =
        metrics.analysisMilliseconds <= 0
            ? 0.0
            : static_cast<double>(metrics.analysisDecodedFrames) * 1000.0 /
                  static_cast<double>(metrics.analysisMilliseconds);
    if (!completed || metrics.sourceSplitObservations != 0U || dropRatio > 0.005 ||
        metrics.playbackResponseMilliseconds < 0 || metrics.playbackResponseMilliseconds > 100 ||
        metrics.seekP95Milliseconds < 0 || metrics.seekP95Milliseconds > 500 ||
        metrics.warmStepP95Milliseconds < 0 ||
        // Every held-window submission must produce a committed frame and a latency sample;
        // otherwise a run that silently absorbs steps would report a healthy pass.
        metrics.heldStepPresentedFrames != heldStepSubmittedFrames ||
        heldStepMilliseconds.size() != heldStepSubmittedFrames ||
        (expectedSourceCount > 1U && metrics.analysisDecodedFrames == 0U) ||
        metrics.peakFrameBytes > kMaximumFrameBytes || !allHardware ||
        metrics.finalThreads > metrics.baselineThreads + 2U || !metrics.comparisonModeVerified ||
        (expectedSourceCount > 1U && !metrics.comparisonFrameRetained) ||
        transfer.deviceLossReports != 0U) {
        result = EXIT_FAILURE;
    }

    QElapsedTimer shutdownTimer;
    shutdownTimer.start();
    runtime->prepareForSceneGraphRelease();
    desktop.releaseSceneGraph();
    const bool shutdownCompleted = runtime->shutdownAfterSceneGraphRelease();
    metrics.shutdownMilliseconds = shutdownTimer.elapsed();
    if (!shutdownCompleted || metrics.shutdownMilliseconds > 7000) {
        result = EXIT_FAILURE;
    }

    QJsonObject report;
    const auto addNumber = [&report](const QString& key, const auto value) {
        report.insert(key, static_cast<double>(value));
    };
    // T0 evidence: summarize the new-content display interval and UI loop gap distributions
    // (P50/P95/P99 + maximum) so the report carries tail latency and longest-pause evidence
    // instead of only average FPS.
    const auto summarizeTail = [](std::vector<qint64>& samples) {
        std::ranges::sort(samples);
        return TailSummary{
            .p50 = samples.empty() ? -1 : samples[samples.size() / 2U],
            .p95 = samples.empty() ? -1
                                   : samples[static_cast<std::size_t>(samples.size() * 95U / 100U)],
            .p99 = samples.empty() ? -1
                                   : samples[static_cast<std::size_t>(samples.size() * 99U / 100U)],
            .maximum = samples.empty() ? -1 : samples.back(),
        };
    };
    const TailSummary displayIntervals = summarizeTail(metrics.displayIntervalsMilliseconds);
    metrics.displayIntervalP50Milliseconds = displayIntervals.p50;
    metrics.displayIntervalP95Milliseconds = displayIntervals.p95;
    metrics.displayIntervalP99Milliseconds = displayIntervals.p99;
    metrics.displayIntervalMaximumMilliseconds = displayIntervals.maximum;
    const TailSummary uiLoopGaps = summarizeTail(metrics.uiLoopGapMicroseconds);
    metrics.uiLoopGapP50Milliseconds = uiLoopGaps.p50 < 0 ? -1 : uiLoopGaps.p50 / 1000;
    metrics.uiLoopGapP95Milliseconds = uiLoopGaps.p95 < 0 ? -1 : uiLoopGaps.p95 / 1000;
    metrics.uiLoopGapP99Milliseconds = uiLoopGaps.p99 < 0 ? -1 : uiLoopGaps.p99 / 1000;
    metrics.uiLoopGapMaximumMilliseconds = uiLoopGaps.maximum < 0 ? -1 : uiLoopGaps.maximum / 1000;
    addNumber(QStringLiteral("duration_seconds"), duration.count());
    addNumber(QStringLiteral("screen_refresh_hz"), desktop.activeScreenRefreshRate());
    addNumber(QStringLiteral("presented_frames"), metrics.presentedFrames);
    addNumber(QStringLiteral("dropped_frames"), metrics.droppedFrames);
    addNumber(QStringLiteral("drop_ratio"), dropRatio);
    addNumber(QStringLiteral("source_split_observations"), metrics.sourceSplitObservations);
    addNumber(QStringLiteral("display_interval_p50_ms"), metrics.displayIntervalP50Milliseconds);
    addNumber(QStringLiteral("display_interval_p95_ms"), metrics.displayIntervalP95Milliseconds);
    addNumber(QStringLiteral("display_interval_p99_ms"), metrics.displayIntervalP99Milliseconds);
    addNumber(QStringLiteral("display_interval_max_ms"),
              metrics.displayIntervalMaximumMilliseconds);
    addNumber(QStringLiteral("display_interval_count"),
              static_cast<double>(metrics.displayIntervalsMilliseconds.size()));
    addNumber(QStringLiteral("ui_loop_gap_p50_ms"), metrics.uiLoopGapP50Milliseconds);
    addNumber(QStringLiteral("ui_loop_gap_p95_ms"), metrics.uiLoopGapP95Milliseconds);
    addNumber(QStringLiteral("ui_loop_gap_p99_ms"), metrics.uiLoopGapP99Milliseconds);
    addNumber(QStringLiteral("ui_loop_gap_max_ms"), metrics.uiLoopGapMaximumMilliseconds);
    addNumber(QStringLiteral("ui_loop_gap_count"),
              static_cast<double>(metrics.uiLoopGapMicroseconds.size()));
    addNumber(QStringLiteral("open_first_frame_ms"), metrics.openFirstFrameMilliseconds);
    addNumber(QStringLiteral("playback_response_ms"), metrics.playbackResponseMilliseconds);
    addNumber(QStringLiteral("cold_seek_p50_ms"), metrics.seekP50Milliseconds);
    addNumber(QStringLiteral("seek_p95_ms"), metrics.seekP95Milliseconds);
    QJsonArray seekSamples;
    for (const qint64 sample : seekMilliseconds) {
        seekSamples.append(static_cast<double>(sample));
    }
    report.insert(QStringLiteral("seek_samples_ms"), seekSamples);
    addNumber(QStringLiteral("warm_step_p50_ms"), metrics.warmStepP50Milliseconds);
    addNumber(QStringLiteral("warm_step_p95_ms"), metrics.warmStepP95Milliseconds);
    addNumber(QStringLiteral("held_step_p50_ms"), metrics.heldStepP50Milliseconds);
    addNumber(QStringLiteral("held_step_p95_ms"), metrics.heldStepP95Milliseconds);
    addNumber(QStringLiteral("held_step_p99_ms"), metrics.heldStepP99Milliseconds);
    addNumber(QStringLiteral("held_step_presented_frames"), metrics.heldStepPresentedFrames);
    addNumber(QStringLiteral("held_step_submitted_frames"),
              static_cast<std::uint64_t>(heldStepSubmittedFrames));
    addNumber(QStringLiteral("held_step_sequence_errors"), metrics.heldStepSequenceErrors);
    addNumber(QStringLiteral("held_step_generation_delta"), metrics.heldStepGenerationDelta);
    addNumber(QStringLiteral("held_step_exact_seek_delta"), metrics.heldStepExactSeekDelta);
    addNumber(QStringLiteral("held_step_sequential_request_count"),
              metrics.heldStepSequentialRequestCount);
    addNumber(QStringLiteral("held_step_cancel_count"), metrics.heldStepCancelCount);
    addNumber(QStringLiteral("held_step_decoder_reopen_count"), metrics.heldStepDecoderReopenCount);
    addNumber(QStringLiteral("held_step_sequential_ratio"), metrics.heldStepSequentialRatio);
    addNumber(QStringLiteral("analysis_ms"), metrics.analysisMilliseconds);
    addNumber(QStringLiteral("analysis_decoded_frames"), metrics.analysisDecodedFrames);
    addNumber(QStringLiteral("analysis_frames_per_second"), analysisFramesPerSecond);
    const std::string_view comparisonModeName = performanceComparisonModeName(comparisonMode);
    report.insert(QStringLiteral("comparison_mode"),
                  QString::fromUtf8(comparisonModeName.data(),
                                    static_cast<qsizetype>(comparisonModeName.size())));
    addNumber(QStringLiteral("comparison_sampled_pixels"), metrics.comparisonSampledPixels);
    addNumber(QStringLiteral("comparison_bright_pixel_ratio"), metrics.comparisonBrightPixelRatio);
    report.insert(QStringLiteral("comparison_mode_verified"), metrics.comparisonModeVerified);
    report.insert(QStringLiteral("comparison_frame_retained"), metrics.comparisonFrameRetained);
    addNumber(QStringLiteral("shutdown_ms"), metrics.shutdownMilliseconds);
    addNumber(QStringLiteral("peak_frame_bytes"), metrics.peakFrameBytes);
    addNumber(QStringLiteral("peak_working_set_bytes"), metrics.peakWorkingSetBytes);
    addNumber(QStringLiteral("baseline_threads"), metrics.baselineThreads);
    addNumber(QStringLiteral("peak_threads"), metrics.peakThreads);
    addNumber(QStringLiteral("final_threads"), metrics.finalThreads);
    addNumber(QStringLiteral("submitted_sets"), playbackTransfer.submittedSets);
    addNumber(QStringLiteral("replaced_sets"), playbackTransfer.replacedSets);
    addNumber(QStringLiteral("published_sets"), playbackTransfer.publishedSets);
    addNumber(QStringLiteral("failed_sets"), transfer.failedSets);
    addNumber(QStringLiteral("cancelled_sets"), transfer.cancelledSets);
    addNumber(QStringLiteral("transfer_average_us"),
              playbackTransfer.completedTransfers == 0U
                  ? 0U
                  : playbackTransfer.totalTransferMicroseconds /
                        playbackTransfer.completedTransfers);
    addNumber(QStringLiteral("transfer_maximum_us"), playbackTransfer.maximumTransferMicroseconds);
    addNumber(QStringLiteral("zero_copy_sets"), playbackTransfer.zeroCopySets);
    addNumber(QStringLiteral("render_frame_notifications"), relay.frameNotifications);
    addNumber(QStringLiteral("render_ack_notifications"), relay.ackNotifications);
    addNumber(QStringLiteral("render_ack_backpressure"), relay.ackBackpressureNotifications);
    addNumber(QStringLiteral("render_acknowledgements"), relay.acknowledgementsPopped);
    addNumber(QStringLiteral("render_canonical_gaps"), relay.canonicalFrameGaps);
    addNumber(QStringLiteral("render_canonical_regressions"), relay.canonicalFrameRegressions);
    addNumber(QStringLiteral("render_update_requests"), relay.updateRequests);
    addNumber(QStringLiteral("render_item_updates"), relay.itemUpdates);
    addNumber(QStringLiteral("render_frame_to_start_average_us"),
              relay.frameToRenderSamples == 0U
                  ? 0U
                  : relay.totalFrameToRenderMicroseconds / relay.frameToRenderSamples);
    addNumber(QStringLiteral("render_start_to_ack_average_us"),
              relay.renderToAckSamples == 0U
                  ? 0U
                  : relay.totalRenderToAckMicroseconds / relay.renderToAckSamples);
    addNumber(QStringLiteral("render_frame_to_ack_average_us"),
              relay.frameToAckSamples == 0U
                  ? 0U
                  : relay.totalFrameToAckMicroseconds / relay.frameToAckSamples);
    addNumber(QStringLiteral("render_frame_to_ack_maximum_us"),
              relay.maximumFrameToAckMicroseconds);
    addNumber(QStringLiteral("device_loss_reports"), transfer.deviceLossReports);
    addNumber(QStringLiteral("decoder_calls"), completedDecodes);
    addNumber(QStringLiteral("decoder_cache_hits"), cacheHits);
    addNumber(QStringLiteral("decoder_cache_hit_ratio"), cacheHitRatio);
    addNumber(QStringLiteral("decoder_exact_seeks"), exactSeeks);
    addNumber(QStringLiteral("decoder_average_us"),
              completedDecodes == 0U ? 0U : totalDecodeMicroseconds / completedDecodes);
    addNumber(QStringLiteral("decoder_maximum_us"), maximumDecodeMicroseconds);
    addNumber(QStringLiteral("probe_index_count"), probe.completedProbes);
    addNumber(QStringLiteral("probe_index_average_us"),
              probe.completedProbes == 0U
                  ? 0U
                  : probe.totalProbeIndexMicroseconds / probe.completedProbes);
    addNumber(QStringLiteral("probe_index_maximum_us"), probe.maximumProbeIndexMicroseconds);
    addNumber(QStringLiteral("frameset_assembly_count"), provider.assembledFrameSets);
    addNumber(QStringLiteral("frameset_assembly_average_us"),
              provider.assembledFrameSets == 0U
                  ? 0U
                  : provider.totalAssemblyMicroseconds / provider.assembledFrameSets);
    addNumber(QStringLiteral("frameset_assembly_maximum_us"), provider.maximumAssemblyMicroseconds);
    addNumber(QStringLiteral("frameset_cache_hits"), provider.frameSetCacheHits);

    QJsonArray sourceDecode;
    for (std::size_t index = 0U; index < playbackBackends.size(); ++index) {
        const auto& backend = playbackBackends[index];
        sourceDecode.append(QJsonObject{
            {QStringLiteral("source_id"), static_cast<double>(backend.sourceId)},
            {QStringLiteral("backend"),
             QString::fromUtf8(
                 backend.backend == dvs::media::DecoderBackend::D3d11Va ? "d3d11va" : "software")},
            {QStringLiteral("fallback_reason"), QString::fromStdString(backend.fallbackReason)},
            {QStringLiteral("device_generation"),
             static_cast<double>(backend.deviceGeneration.value())},
            {QStringLiteral("cache_hits"), static_cast<double>(backend.cacheHitCount)},
            {QStringLiteral("exact_seeks"), static_cast<double>(backend.exactSeekCount)},
            {QStringLiteral("calls"), static_cast<double>(backend.completedDecodeCount)},
            {QStringLiteral("average_us"),
             static_cast<double>(backend.completedDecodeCount == 0U
                                     ? 0U
                                     : backend.totalDecodeMicroseconds /
                                           backend.completedDecodeCount)},
            {QStringLiteral("maximum_us"), static_cast<double>(backend.maximumDecodeMicroseconds)},
        });
    }
    report.insert(QStringLiteral("per_source_decode"), sourceDecode);
    addNumber(QStringLiteral("expected_source_count"), expectedSourceCount);
    report.insert(QStringLiteral("all_d3d11va"), allHardware);
    report.insert(QStringLiteral("shutdown_completed"), shutdownCompleted);
    report.insert(QStringLiteral("passed"), result == EXIT_SUCCESS);
    if (failed) {
        report.insert(QStringLiteral("failure"), QString::fromStdString(failureReason));
    }
    const auto encodedReport = QJsonDocument{report}.toJson(QJsonDocument::Compact);
    writeStandardError("DVS_PERFORMANCE_RESULT " + encodedReport.toStdString() + '\n');
    if (!shutdownCompleted) {
        writeStandardError("DVS_RUNTIME_SHUTDOWN_TIMEOUT\n");
        std::_Exit(EXIT_FAILURE);
    }
    return result;
}

// P4 image-folder evidence entry (T0 baseline). Loads two folders, walks every paired row,
// records the committed image state (paths, sizes, hashes, generation, error text), the
// synchronous open cost, and the UI event-loop gaps caused by GUI-thread decoding. Corrupt /
// missing / different-size / alpha-only fixtures intentionally expose the current behavior —
// including a stale previous pair surviving a failed open — so the "wrong object" symptom is
// reproducible evidence before T1/T2 change any behavior.
struct ImageFolderInvocation final {
    std::filesystem::path left;
    std::filesystem::path right;
};

struct ImageRowEvidence final {
    int row = -1;
    std::string fileName;
    std::string leftPath;
    std::string rightPath;
    bool hasLeft = false;
    bool hasRight = false;
    bool opened = false;
    bool hasPair = false;
    int primaryWidth = 0;
    int primaryHeight = 0;
    int secondaryWidth = 0;
    int secondaryHeight = 0;
    std::string leftSha256;
    std::string rightSha256;
    qint64 openMilliseconds = -1;
    int contentGeneration = -1;
    std::string errorText;
};

[[nodiscard]] std::optional<ImageFolderInvocation> parseImageFolderInvocation(const int argc,
                                                                              char** const argv) {
    if (argc != 4 || argv == nullptr || std::string_view{argv[1]} != "--ui-image-folder") {
        return std::nullopt;
    }
    return ImageFolderInvocation{
        .left = std::filesystem::path{argv[2]},
        .right = std::filesystem::path{argv[3]},
    };
}

[[nodiscard]] std::string fileSha256Hex(const std::filesystem::path& path) {
    QFile file(QString::fromStdString(path.string()));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray bytes = file.readAll();
    if (bytes.isEmpty()) {
        return {};
    }
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())
        .toStdString();
}

[[nodiscard]] int
runImageFolderEvidence(int& argc, char** argv, const ImageFolderInvocation& invocation) {
    constexpr std::size_t kMaximumImagePairs = 4096U;
    dvs::ui::configureGraphicsBackend();
    dvs::ui::DesktopApplication desktop{
        argc,
        argv,
        dvs::ui::DesktopApplicationOptions{
            .smokeMode = false,
            .preferSoftwareDevice = false,
            .preferHighRefreshScreen = true,
        },
    };
    std::unique_ptr<dvs::app::ReviewRuntime> runtime = dvs::app::ReviewRuntime::create();
    if (!runtime || runtime->controller() == nullptr || runtime->preferences() == nullptr ||
        !desktop.load(*runtime->controller(),
                      *runtime->preferences(),
                      [&runtime](dvs::ui::ComparisonSurface& surface) {
                          return runtime->attachSurface(surface);
                      })) {
        writeStandardError("DVS_IMAGE_UI_LOAD_FAILED\n");
        if (runtime) {
            runtime->prepareForSceneGraphRelease();
            desktop.releaseSceneGraph();
            static_cast<void>(runtime->shutdownAfterSceneGraphRelease());
        }
        return EXIT_FAILURE;
    }
    installPlaybackTrace(*runtime);

    enum class ImageStage {
        WaitingForGraphics,
        WaitingForFolderFirstPair,
        IteratingRows,
        WaitingForRowOpen,
        DifferenceProbe,
        WaitingForDiffPairOpen,
        WaitingForDiff,
        CancelProbe,
        WaitingForReopen,
        Completed,
    };
    ImageStage stage = ImageStage::WaitingForGraphics;
    bool failed = false;
    bool completed = false;
    std::string failureReason;
    QElapsedTimer folderLoadTimer;
    QElapsedTimer openTimer;
    QElapsedTimer diffTimer;
    QElapsedTimer uiLoopHeartbeat;
    qint64 lastHeartbeatTimeMilliseconds = -1;
    std::vector<qint64> uiLoopGapMicroseconds;
    std::size_t peakWorkingSetBytes = 0U;
    std::size_t baselineThreads = 0U;
    std::size_t peakThreads = 0U;
    qint64 loadFoldersMilliseconds = -1;
    std::vector<ImageRowEvidence> rows;
    int iterateRow = 1;
    int folderFirstRow = -1;
    int waitingRow = -1;
    int diffPairRow = -1;
    qint64 diffRecomputeMilliseconds = -1;
    int diffMaxAbsDifference = -1;
    double diffMeanAbsDifference = -1.0;
    bool diffHasResult = false;
    bool diffResampled = false;
    bool diffAlphaOnly = false;
    bool cancelClosed = false;
    bool cancelReopened = false;
    int cancelReopenGeneration = -1;
    QElapsedTimer settleTimer;

    const auto fail = [&](std::string reason) {
        if (!failed) {
            failed = true;
            failureReason = std::move(reason);
            desktop.exit(EXIT_FAILURE);
        }
    };

    auto* const review =
        qobject_cast<dvs::ui::ImageReviewController*>(desktop.imageReviewForAutomation());
    auto* const model =
        qobject_cast<dvs::ui::ImageFolderPairModel*>(desktop.folderPairModelForAutomation());

    const auto captureRow = [&](const int row, const qint64 openMilliseconds, const bool opened) {
        ImageRowEvidence evidence;
        evidence.row = row;
        evidence.opened = opened;
        evidence.openMilliseconds = openMilliseconds;
        if (model != nullptr) {
            const QModelIndex index = model->index(row, 0);
            evidence.fileName = model->data(index, dvs::ui::ImageFolderPairModel::FileNameRole)
                                    .toString()
                                    .toStdString();
            const QUrl leftUrl =
                model->data(index, dvs::ui::ImageFolderPairModel::LeftPathRole).toUrl();
            const QUrl rightUrl =
                model->data(index, dvs::ui::ImageFolderPairModel::RightPathRole).toUrl();
            evidence.hasLeft =
                model->data(index, dvs::ui::ImageFolderPairModel::HasLeftRole).toBool();
            evidence.hasRight =
                model->data(index, dvs::ui::ImageFolderPairModel::HasRightRole).toBool();
            if (leftUrl.isLocalFile()) {
                evidence.leftPath = leftUrl.toLocalFile().toStdString();
            }
            if (rightUrl.isLocalFile()) {
                evidence.rightPath = rightUrl.toLocalFile().toStdString();
            }
        }
        if (review != nullptr) {
            evidence.hasPair = review->hasPair();
            evidence.primaryWidth = review->primaryWidth();
            evidence.primaryHeight = review->primaryHeight();
            evidence.secondaryWidth = review->secondaryWidth();
            evidence.secondaryHeight = review->secondaryHeight();
            evidence.contentGeneration = review->contentGeneration();
            evidence.errorText = review->errorText().toStdString();
        }
        if (evidence.errorText.empty() && model != nullptr) {
            evidence.errorText = model->errorText().toStdString();
        }
        if (!evidence.leftPath.empty()) {
            evidence.leftSha256 = fileSha256Hex(std::filesystem::path{evidence.leftPath});
        }
        if (!evidence.rightPath.empty()) {
            evidence.rightSha256 = fileSha256Hex(std::filesystem::path{evidence.rightPath});
        }
        rows.push_back(std::move(evidence));
    };

    QTimer poll;
    poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, runtime->controller(), [&] {
        dvs::ui::ReviewController& controller = *runtime->controller();
        if (failed || completed) {
            return;
        }
        if (stage != ImageStage::WaitingForGraphics && hasReviewError(controller) &&
            !controller.busy()) {
            fail("media-error:" + controller.lastErrorTechnicalDetail().toStdString());
            return;
        }
        switch (stage) {
        case ImageStage::WaitingForGraphics: {
            if (!controller.graphicsReady()) {
                return;
            }
            baselineThreads = dvs::platform::sampleCurrentProcessTelemetry().threadCount;
            peakThreads = baselineThreads;
            uiLoopHeartbeat.start();
            lastHeartbeatTimeMilliseconds = -1;
            folderLoadTimer.start();
            if (!desktop.loadFolderComparisonForAutomation(
                    QUrl::fromLocalFile(QString::fromStdString(invocation.left.string())),
                    QUrl::fromLocalFile(QString::fromStdString(invocation.right.string())))) {
                fail("folder-load-rejected");
                return;
            }
            loadFoldersMilliseconds = folderLoadTimer.elapsed();
            if (model == nullptr || model->pairCount() <= 0 ||
                model->pairCount() > static_cast<int>(kMaximumImagePairs)) {
                fail("folder-pair-count-invalid");
                return;
            }
            folderFirstRow = model->firstCompleteRow();
            if (folderFirstRow < 0) {
                // No complete pair: the sidebar still gets a row walk with every row
                // reporting opened=false.
                stage = ImageStage::IteratingRows;
                iterateRow = 0;
                return;
            }
            // Folder load accepted the first candidate asynchronously; wait for its actual
            // commit before the uniform row walk starts (T4 pending/committed boundary).
            stage = ImageStage::WaitingForFolderFirstPair;
            return;
        }
        case ImageStage::WaitingForFolderFirstPair: {
            if (model == nullptr) {
                fail("folder-model-missing");
                return;
            }
            if (model->openPending()) {
                return;
            }
            if (model->currentPair() != folderFirstRow) {
                fail("first-pair-open-rejected:" + model->errorText().toStdString());
                return;
            }
            stage = ImageStage::IteratingRows;
            iterateRow = 0;
            return;
        }
        case ImageStage::IteratingRows: {
            if (model == nullptr) {
                fail("folder-model-missing");
                return;
            }
            if (iterateRow >= model->pairCount()) {
                stage = ImageStage::DifferenceProbe;
                return;
            }
            const int row = iterateRow;
            const bool complete =
                model->data(model->index(row, 0), dvs::ui::ImageFolderPairModel::HasBothRole)
                    .toBool();
            if (!complete) {
                captureRow(row, 0, false);
                ++iterateRow;
                return;
            }
            openTimer.start();
            if (!model->openPairAt(row)) {
                fail("row-open-rejected:" + model->errorText().toStdString());
                return;
            }
            waitingRow = row;
            stage = ImageStage::WaitingForRowOpen;
            return;
        }
        case ImageStage::WaitingForRowOpen: {
            if (model == nullptr) {
                fail("folder-model-missing");
                return;
            }
            if (model->openPending()) {
                return;
            }
            captureRow(waitingRow, openTimer.elapsed(), model->currentPair() == waitingRow);
            ++iterateRow;
            stage = ImageStage::IteratingRows;
            return;
        }
        case ImageStage::DifferenceProbe: {
            if (diffPairRow < 0) {
                if (model == nullptr) {
                    fail("folder-model-missing");
                    return;
                }
                diffPairRow = model->firstCompleteRow();
                if (diffPairRow < 0) {
                    fail("no-complete-pair-for-diff");
                    return;
                }
                if (!model->openPairAt(diffPairRow)) {
                    fail("diff-pair-open-rejected:" + model->errorText().toStdString());
                    return;
                }
                stage = ImageStage::WaitingForDiffPairOpen;
                return;
            }
            stage = ImageStage::WaitingForDiffPairOpen;
            return;
        }
        case ImageStage::WaitingForDiffPairOpen: {
            if (model == nullptr || review == nullptr) {
                fail("image-review-missing");
                return;
            }
            if (model->openPending()) {
                return;
            }
            if (model->currentPair() != diffPairRow) {
                fail("diff-pair-open-rejected:" + model->errorText().toStdString());
                return;
            }
            // T2: unequal-size pairs only compute a diff after the explicit resample opt-in.
            // T4: this request runs on the loader worker and the probe measures commit latency.
            review->setResampleAllowed(true);
            diffTimer.start();
            review->setCompareMode(static_cast<int>(dvs::ui::ImageReviewController::AbsDifference));
            stage = ImageStage::WaitingForDiff;
            return;
        }
        case ImageStage::WaitingForDiff: {
            if (review == nullptr) {
                fail("image-review-missing");
                return;
            }
            if (review->diffPending()) {
                return;
            }
            diffRecomputeMilliseconds = diffTimer.elapsed();
            diffMaxAbsDifference = review->maxAbsDifference();
            diffMeanAbsDifference = review->meanAbsDifference();
            diffHasResult = review->hasDiffResult();
            diffResampled = review->diffResampled();
            diffAlphaOnly = review->alphaDifferenceOnly();
            review->setCompareMode(static_cast<int>(dvs::ui::ImageReviewController::SideBySide));
            review->setResampleAllowed(false);
            stage = ImageStage::CancelProbe;
            return;
        }
        case ImageStage::CancelProbe: {
            if (review == nullptr) {
                fail("image-review-missing");
                return;
            }
            if (!cancelClosed) {
                review->closeAll();
                cancelClosed = !review->hasPrimary() && !review->hasSecondary();
                return;
            }
            if (!cancelReopened) {
                if (model == nullptr || diffPairRow < 0) {
                    fail("folder-model-missing");
                    return;
                }
                if (!model->openPairAt(diffPairRow)) {
                    fail("cancel-reopen-rejected:" + model->errorText().toStdString());
                    return;
                }
                stage = ImageStage::WaitingForReopen;
                return;
            }
            stage = ImageStage::Completed;
            completed = true;
            desktop.exit(EXIT_SUCCESS);
            return;
        }
        case ImageStage::WaitingForReopen: {
            if (model == nullptr || review == nullptr) {
                fail("image-review-missing");
                return;
            }
            if (model->openPending()) {
                return;
            }
            cancelReopened = review->hasPair() && model->currentPair() == diffPairRow;
            cancelReopenGeneration = review->contentGeneration();
            stage = ImageStage::CancelProbe;
            return;
        }
        case ImageStage::Completed:
            return;
        }
    });

    QTimer heartbeatTimer;
    heartbeatTimer.setInterval(1);
    QObject::connect(&heartbeatTimer, &QTimer::timeout, runtime->controller(), [&] {
        const qint64 now = uiLoopHeartbeat.elapsed();
        if (lastHeartbeatTimeMilliseconds >= 0) {
            uiLoopGapMicroseconds.push_back((now - lastHeartbeatTimeMilliseconds) * 1000);
        }
        lastHeartbeatTimeMilliseconds = now;
    });

    dvs::platform::ProcessTelemetrySampler telemetrySampler;

    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(90 * 1000);
    QObject::connect(
        &timeout, &QTimer::timeout, runtime->controller(), [&] { fail("image-evidence-timeout"); });

    poll.start();
    heartbeatTimer.start();
    timeout.start();
    int result = desktop.exec();
    poll.stop();
    heartbeatTimer.stop();
    timeout.stop();
    const dvs::platform::ProcessTelemetry telemetryPeaks = telemetrySampler.stopAndTakePeaks();
    peakWorkingSetBytes = std::max(peakWorkingSetBytes, telemetryPeaks.workingSetBytes);
    peakThreads = std::max(peakThreads, telemetryPeaks.threadCount);

    QElapsedTimer shutdownTimer;
    shutdownTimer.start();
    runtime->prepareForSceneGraphRelease();
    desktop.releaseSceneGraph();
    const bool shutdownCompleted = runtime->shutdownAfterSceneGraphRelease();
    const qint64 shutdownMilliseconds = shutdownTimer.elapsed();
    if (!shutdownCompleted || shutdownMilliseconds > 7000) {
        result = EXIT_FAILURE;
    }

    const auto summarizeGaps = [](std::vector<qint64>& samples) {
        std::ranges::sort(samples);
        return TailSummary{
            .p50 = samples.empty() ? -1 : samples[samples.size() / 2U],
            .p95 = samples.empty() ? -1
                                   : samples[static_cast<std::size_t>(samples.size() * 95U / 100U)],
            .p99 = samples.empty() ? -1
                                   : samples[static_cast<std::size_t>(samples.size() * 99U / 100U)],
            .maximum = samples.empty() ? -1 : samples.back(),
        };
    };
    const TailSummary gaps = summarizeGaps(uiLoopGapMicroseconds);

    QJsonObject report;
    const auto addNumber = [&report](const QString& key, const auto value) {
        report.insert(key, static_cast<double>(value));
    };
    report.insert(QStringLiteral("passed"), result == EXIT_SUCCESS);
    if (failed) {
        report.insert(QStringLiteral("failure"), QString::fromStdString(failureReason));
    }
    addNumber(QStringLiteral("screen_refresh_hz"), desktop.activeScreenRefreshRate());
    report.insert(QStringLiteral("left_folder"), QString::fromStdString(invocation.left.string()));
    report.insert(QStringLiteral("right_folder"),
                  QString::fromStdString(invocation.right.string()));
    addNumber(QStringLiteral("pair_count"), model == nullptr ? 0 : model->pairCount());
    addNumber(QStringLiteral("load_folders_ms"), loadFoldersMilliseconds);
    QJsonArray rowArray;
    for (const ImageRowEvidence& row : rows) {
        rowArray.append(QJsonObject{
            {QStringLiteral("row"), row.row},
            {QStringLiteral("fileName"), QString::fromStdString(row.fileName)},
            {QStringLiteral("hasLeft"), row.hasLeft},
            {QStringLiteral("hasRight"), row.hasRight},
            {QStringLiteral("opened"), row.opened},
            {QStringLiteral("hasPair"), row.hasPair},
            {QStringLiteral("primaryWidth"), row.primaryWidth},
            {QStringLiteral("primaryHeight"), row.primaryHeight},
            {QStringLiteral("secondaryWidth"), row.secondaryWidth},
            {QStringLiteral("secondaryHeight"), row.secondaryHeight},
            {QStringLiteral("leftSha256"), QString::fromStdString(row.leftSha256)},
            {QStringLiteral("rightSha256"), QString::fromStdString(row.rightSha256)},
            {QStringLiteral("open_ms"), static_cast<double>(row.openMilliseconds)},
            {QStringLiteral("contentGeneration"), row.contentGeneration},
            {QStringLiteral("errorText"), QString::fromStdString(row.errorText)},
        });
    }
    report.insert(QStringLiteral("rows"), rowArray);
    report.insert(
        QStringLiteral("diff_probe"),
        QJsonObject{
            {QStringLiteral("max_abs_difference"), diffMaxAbsDifference},
            {QStringLiteral("mean_abs_difference"), diffMeanAbsDifference},
            {QStringLiteral("recompute_ms"), static_cast<double>(diffRecomputeMilliseconds)},
            {QStringLiteral("has_diff_result"), diffHasResult},
            {QStringLiteral("diff_resampled"), diffResampled},
            {QStringLiteral("alpha_difference_only"), diffAlphaOnly},
        });
    report.insert(QStringLiteral("cancel_probe"),
                  QJsonObject{
                      {QStringLiteral("closed"), cancelClosed},
                      {QStringLiteral("reopened"), cancelReopened},
                      {QStringLiteral("reopen_generation"), cancelReopenGeneration},
                  });
    addNumber(QStringLiteral("ui_loop_gap_p50_ms"), gaps.p50 < 0 ? -1 : gaps.p50 / 1000);
    addNumber(QStringLiteral("ui_loop_gap_p95_ms"), gaps.p95 < 0 ? -1 : gaps.p95 / 1000);
    addNumber(QStringLiteral("ui_loop_gap_p99_ms"), gaps.p99 < 0 ? -1 : gaps.p99 / 1000);
    addNumber(QStringLiteral("ui_loop_gap_max_ms"), gaps.maximum < 0 ? -1 : gaps.maximum / 1000);
    addNumber(QStringLiteral("ui_loop_gap_count"),
              static_cast<double>(uiLoopGapMicroseconds.size()));
    addNumber(QStringLiteral("peak_working_set_bytes"), peakWorkingSetBytes);
    addNumber(QStringLiteral("baseline_threads"), baselineThreads);
    addNumber(QStringLiteral("peak_threads"), peakThreads);
    addNumber(QStringLiteral("shutdown_ms"), shutdownMilliseconds);
    const auto encodedReport = QJsonDocument{report}.toJson(QJsonDocument::Compact);
    writeStandardError("DVS_IMAGE_RESULT " + encodedReport.toStdString() + '\n');
    if (!shutdownCompleted) {
        writeStandardError("DVS_RUNTIME_SHUTDOWN_TIMEOUT\n");
        std::_Exit(EXIT_FAILURE);
    }
    return result;
}

struct PopupProbeTarget final {
    std::string_view label;
    std::string_view menuObjectName;
    std::string_view parentMenuObjectName;
};

struct PopupProbeResult final {
    std::string label;
    bool captured = false;
    std::uint64_t sampledPixels = 0U;
    int minAlpha = 0;
    bool passed = false;
    std::string failureReason;
};

[[nodiscard]] PopupProbeResult samplePopupBackground(const std::string_view label,
                                                     const QImage& image) {
    PopupProbeResult result;
    result.label = std::string{label};
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull()) {
        result.failureReason = "convert-failed";
        return result;
    }
    result.captured = true;
    // Popup.Window preserves the rounded VcsMenu corners as transparent pixels. Exclude only those
    // four corners so the gate still verifies the top, bottom, left, and right edge centers.
    constexpr qreal kSafeLogicalCornerExtent = 10.0;
    const int cornerExtent =
        static_cast<int>(std::ceil(kSafeLogicalCornerExtent * rgba.devicePixelRatio()));
    if (cornerExtent <= 0 || rgba.width() <= cornerExtent * 2 ||
        rgba.height() <= cornerExtent * 2) {
        result.failureReason = "no-pixels";
        return result;
    }
    const int horizontalStep = (std::max)(1, rgba.width() / 48);
    const int verticalStep = (std::max)(1, rgba.height() / 48);
    int minimumAlpha = 255;
    std::uint64_t sampledPixels = 0U;
    for (int y = 0; y < rgba.height(); y += verticalStep) {
        const auto* const row = rgba.constScanLine(y);
        const bool inVerticalCorner = y < cornerExtent || y >= rgba.height() - cornerExtent;
        for (int x = 0; x < rgba.width(); x += horizontalStep) {
            const bool inHorizontalCorner = x < cornerExtent || x >= rgba.width() - cornerExtent;
            if (inVerticalCorner && inHorizontalCorner) {
                continue;
            }
            const int offset = x * 4;
            const int alpha = static_cast<int>(row[offset + 3]);
            ++sampledPixels;
            if (alpha < minimumAlpha) {
                minimumAlpha = alpha;
            }
        }
    }
    result.sampledPixels = sampledPixels;
    result.minAlpha = minimumAlpha;
    result.passed = sampledPixels > 0U && minimumAlpha == 255;
    if (!result.passed) {
        result.failureReason = sampledPixels == 0U ? "no-pixels" : "transparent-background";
    }
    return result;
}

[[nodiscard]] int runPopupPixelProbe(int& argc, char** argv, const SmokeSources& sources) {
    dvs::ui::configureGraphicsBackend();
    dvs::ui::DesktopApplication desktop{
        argc,
        argv,
        dvs::ui::DesktopApplicationOptions{
            .smokeMode = true,
            .preferSoftwareDevice = true,
        },
    };
    std::unique_ptr<dvs::app::ReviewRuntime> runtime = dvs::app::ReviewRuntime::create();
    if (!runtime || runtime->controller() == nullptr || runtime->preferences() == nullptr ||
        !desktop.load(*runtime->controller(),
                      *runtime->preferences(),
                      [&runtime](dvs::ui::ComparisonSurface& surface) {
                          return runtime->attachSurface(surface);
                      })) {
        writeStandardError("DVS_POPUP_PIXEL_UI_LOAD_FAILED\n");
        if (runtime) {
            runtime->prepareForSceneGraphRelease();
            desktop.releaseSceneGraph();
            if (!runtime->shutdownAfterSceneGraphRelease()) {
                writeStandardError("DVS_RUNTIME_SHUTDOWN_TIMEOUT\n");
                std::_Exit(EXIT_FAILURE);
            }
        }
        return EXIT_FAILURE;
    }

    enum class Stage {
        WaitingForGraphics,
        WaitingForFirstFrame,
        ProbingMenus,
        Completed,
    };

    std::vector<PopupProbeTarget> probeTargets{
        {.label = "file", .menuObjectName = "fileMenu", .parentMenuObjectName = {}},
    };
    if (sources.second.has_value()) {
        probeTargets.push_back(
            {.label = "compare", .menuObjectName = "compareMenu", .parentMenuObjectName = {}});
    }
    if (sources.third.has_value()) {
        probeTargets.push_back({.label = "layout",
                                .menuObjectName = "layoutMenu",
                                .parentMenuObjectName = "compareMenu"});
    }
    probeTargets.push_back(
        {.label = "source", .menuObjectName = "sourceMenu-0", .parentMenuObjectName = {}});

    Stage stage = Stage::WaitingForGraphics;
    std::vector<PopupProbeResult> results;
    bool completed = false;
    bool failed = false;
    std::string failureReason;
    std::size_t probeIndex = 0U;
    QElapsedTimer probeDelay;
    bool probeMenuOpened = false;

    const auto fail = [&](std::string reason) {
        if (!failed) {
            failed = true;
            failureReason = std::move(reason);
            desktop.exit(EXIT_FAILURE);
        }
    };

    QTimer poll;
    poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, runtime->controller(), [&] {
        if (failed || completed) {
            return;
        }
        dvs::ui::ReviewController& controller = *runtime->controller();
        if (hasReviewError(controller) && !controller.busy() && stage != Stage::ProbingMenus) {
            fail("media-error:" + controller.lastErrorTechnicalDetail().toStdString());
            return;
        }

        switch (stage) {
        case Stage::WaitingForGraphics: {
            if (!controller.graphicsReady()) {
                return;
            }
            QList<QUrl> automationSources{localFileUrl(sources.first)};
            if (sources.second.has_value()) {
                automationSources.push_back(localFileUrl(*sources.second));
            }
            if (sources.third.has_value()) {
                automationSources.push_back(localFileUrl(*sources.third));
            }
            if (!desktop.openSourcesForAutomation(automationSources)) {
                fail("open-rejected");
                return;
            }
            stage = Stage::WaitingForFirstFrame;
            return;
        }
        case Stage::WaitingForFirstFrame:
            if (controller.busy()) {
                return;
            }
            if (hasReviewError(controller)) {
                fail("media-error:" + controller.lastErrorTechnicalDetail().toStdString());
                return;
            }
            if (!sources.second.has_value()) {
                PopupProbeResult disabledCompare;
                disabledCompare.label = "compare-disabled";
                disabledCompare.minAlpha = 255;
                disabledCompare.passed = !desktop.openMenuForAutomation("compareMenu") &&
                                         !desktop.menuIsOpenForAutomation("compareMenu");
                if (!disabledCompare.passed) {
                    disabledCompare.failureReason = "disabled-menu-opened";
                }
                results.push_back(std::move(disabledCompare));
            }
            stage = Stage::ProbingMenus;
            probeIndex = 0U;
            probeMenuOpened = false;
            return;
        case Stage::ProbingMenus: {
            if (probeIndex >= probeTargets.size()) {
                stage = Stage::Completed;
                completed = true;
                desktop.exit(EXIT_SUCCESS);
                return;
            }
            const auto& target = probeTargets[probeIndex];
            if (!probeMenuOpened) {
                // Lazy-init retry timer for menu open.
                if (!probeDelay.isValid()) {
                    probeDelay.start();
                }
                // Open parent menu first if needed.
                if (!target.parentMenuObjectName.empty()) {
                    if (!desktop.menuIsOpenForAutomation(target.parentMenuObjectName)) {
                        if (!desktop.openMenuForAutomation(target.parentMenuObjectName)) {
                            if (probeDelay.hasExpired(5000)) {
                                fail(std::string{target.label} + "-parent-open-rejected");
                                return;
                            }
                            return;
                        }
                    }
                }
                if (!desktop.openMenuForAutomation(target.menuObjectName)) {
                    // Retry for up to 5 seconds (the source menu may need QML rendering time).
                    if (probeDelay.hasExpired(5000)) {
                        fail(std::string{target.label} + "-open-rejected");
                        return;
                    }
                    return;
                }
                probeMenuOpened = true;
                probeDelay.restart();
                return;
            }
            // Wait for the popup window to be created and rendered.
            if (probeDelay.elapsed() < 200) {
                return;
            }
            // Capture the popup window.
            const std::optional<QImage> popupImage =
                desktop.capturePopupWindowForAutomation(target.menuObjectName);
            PopupProbeResult probeResult;
            probeResult.label = std::string{target.label};
            if (popupImage.has_value()) {
                probeResult = samplePopupBackground(target.label, *popupImage);
            } else {
                probeResult.captured = false;
                probeResult.failureReason = "popup-window-not-found";
            }
            results.push_back(probeResult);

            // Close menus.
            static_cast<void>(desktop.closeMenuForAutomation(target.menuObjectName));
            if (!target.parentMenuObjectName.empty()) {
                static_cast<void>(desktop.closeMenuForAutomation(target.parentMenuObjectName));
            }
            // Also close any remaining open menus to reset state.
            for (const auto* const name :
                 {"fileMenu", "compareMenu", "analyzeMenu", "viewMenu", "sourceMenu-0"}) {
                if (desktop.menuIsOpenForAutomation(name)) {
                    static_cast<void>(desktop.closeMenuForAutomation(name));
                }
            }

            ++probeIndex;
            probeMenuOpened = false;
            // Reset the retry timer for the next probe target.
            probeDelay = QElapsedTimer{};
            return;
        }
        case Stage::Completed:
            return;
        }
    });

    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(30'000);
    QObject::connect(&timeout, &QTimer::timeout, runtime->controller(), [&] {
        fail("popup-pixel-probe-timeout");
    });
    poll.start();
    timeout.start();
    int result = desktop.exec();
    poll.stop();
    timeout.stop();

    if (!completed && !failed) {
        failed = true;
        failureReason = "popup-pixel-probe-incomplete";
        result = EXIT_FAILURE;
    }

    // Build JSON report.
    QJsonArray probeArray;
    bool allPassed = true;
    for (const auto& probeResult : results) {
        QJsonObject entry;
        entry.insert(QStringLiteral("name"), QString::fromStdString(probeResult.label));
        entry.insert(QStringLiteral("captured"), probeResult.captured);
        entry.insert(QStringLiteral("sampled_pixels"),
                     static_cast<double>(probeResult.sampledPixels));
        entry.insert(QStringLiteral("min_alpha"), probeResult.minAlpha);
        entry.insert(QStringLiteral("pass"), probeResult.passed);
        if (!probeResult.passed) {
            entry.insert(QStringLiteral("failure"),
                         QString::fromStdString(probeResult.failureReason));
            allPassed = false;
        }
        probeArray.append(entry);
    }
    QJsonObject report;
    report.insert(QStringLiteral("probes"), probeArray);
    report.insert(QStringLiteral("passed"), completed && allPassed && !failed);
    if (failed) {
        report.insert(QStringLiteral("failure"), QString::fromStdString(failureReason));
    }
    const auto encodedReport = QJsonDocument{report}.toJson(QJsonDocument::Compact);
    writeStandardError("DVS_POPUP_PIXEL_RESULT " + encodedReport.toStdString() + '\n');

    runtime->prepareForSceneGraphRelease();
    desktop.releaseSceneGraph();
    if (!runtime->shutdownAfterSceneGraphRelease()) {
        writeStandardError("DVS_RUNTIME_SHUTDOWN_TIMEOUT\n");
        std::_Exit(EXIT_FAILURE);
    }
    return result;
}

} // namespace

int main(int argc, char* argv[]) {
    // Qt imageformat plugins may omit PNG/JPEG on this deploy; route still-image open
    // through FFmpeg so File → Open image… works for common formats.
    dvs::ui::ImageReviewController::setProcessStillImageLoader([](const QByteArray& bytes,
                                                                  QImage* image,
                                                                  std::string* error) {
        if (bytes.isEmpty() || image == nullptr) {
            if (error != nullptr) {
                *error = "Empty image payload.";
            }
            return false;
        }
        dvs::media::StillImage still;
        if (!dvs::media::decodeStillImageBytes(
                reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                static_cast<std::size_t>(bytes.size()),
                &still,
                error)) {
            return false;
        }
        if (still.width <= 0 || still.height <= 0 ||
            still.rgba.size() != static_cast<std::size_t>(still.width) *
                                     static_cast<std::size_t>(still.height) * 4U) {
            if (error != nullptr) {
                *error = "Decoded image buffer is invalid.";
            }
            return false;
        }
        const QImage decoded(
            still.rgba.data(), still.width, still.height, still.width * 4, QImage::Format_RGBA8888);
        *image = decoded.copy();
        return !image->isNull();
    });
    dvs::ui::ImageReviewController::setProcessStillImageProbe(
        [](const QByteArray& bytes, QSize* size) {
            if (bytes.isEmpty() || size == nullptr) {
                return false;
            }
            int width = 0;
            int height = 0;
            std::string error;
            if (!dvs::media::probeStillImageBytes(
                    reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                    static_cast<std::size_t>(bytes.size()),
                    &width,
                    &height,
                    &error)) {
                return false;
            }
            if (width <= 0 || height <= 0) {
                return false;
            }
            size->setWidth(width);
            size->setHeight(height);
            return true;
        });

    const bool smokeArgument = argc >= 2 && std::string_view{argv[1]}.starts_with("--ui-");
    try {
        if (argc == 3 && std::string_view{argv[1]} == "--open-still") {
            return runDesktop(
                argc, argv, false, std::nullopt, false, std::filesystem::path{argv[2]});
        }
        if (argc == 2 && std::string_view{argv[1]} == "--ui-stderr-smoke") {
            writeStandardError("DVS_GUI_STDERR_OK\n");
            return EXIT_SUCCESS;
        }
        if (argc == 2 && std::string_view{argv[1]} == "--ui-smoke") {
            return runDesktop(argc, argv, true);
        }
        if (argc == 3 && std::string_view{argv[1]} == "--ui-smoke") {
            return runDesktop(argc,
                              argv,
                              true,
                              SmokeSources{
                                  .first = std::filesystem::path{argv[2]},
                              });
        }
        if (argc == 4 && std::string_view{argv[1]} == "--ui-smoke") {
            return runDesktop(argc,
                              argv,
                              true,
                              SmokeSources{
                                  .first = std::filesystem::path{argv[2]},
                                  .second = std::filesystem::path{argv[3]},
                              });
        }
        if (argc == 5 && std::string_view{argv[1]} == "--ui-smoke") {
            return runDesktop(argc,
                              argv,
                              true,
                              SmokeSources{
                                  .first = std::filesystem::path{argv[2]},
                                  .second = std::filesystem::path{argv[3]},
                                  .third = std::filesystem::path{argv[4]},
                              });
        }
        if (argc >= 3 && argc <= 5 && std::string_view{argv[1]} == "--ui-popup-pixels") {
            SmokeSources popupSources{.first = std::filesystem::path{argv[2]}};
            if (argc >= 4) {
                popupSources.second = std::filesystem::path{argv[3]};
            }
            if (argc == 5) {
                popupSources.third = std::filesystem::path{argv[4]};
            }
            return runPopupPixelProbe(argc, argv, popupSources);
        }
        if (argc == 4 && std::string_view{argv[1]} == "--ui-upgrade-settings-smoke") {
            return runUpgradeSettingsSmoke(argc,
                                           argv,
                                           SmokeSources{
                                               .first = std::filesystem::path{argv[2]},
                                               .second = std::filesystem::path{argv[3]},
                                           });
        }
        if (argc >= 2 && std::string_view{argv[1]} == "--ui-performance") {
            const std::optional<PerformanceInvocation> invocation =
                parsePerformanceInvocation(argc, argv);
            if (!invocation.has_value()) {
                return dvs::app::reportFatalStartup(
                    "Usage: --ui-performance <one to three sources> --seconds <5-3600> "
                    "[--mode side|wipe|diff]. Wipe and diff require at least two sources.",
                    true);
            }
            return runPerformance(
                argc, argv, invocation->sources, invocation->duration, invocation->comparisonMode);
        }
        if (argc == 4 && std::string_view{argv[1]} == "--ui-image-folder") {
            const std::optional<ImageFolderInvocation> invocation =
                parseImageFolderInvocation(argc, argv);
            if (!invocation.has_value()) {
                return dvs::app::reportFatalStartup(
                    "Usage: --ui-image-folder <leftFolder> <rightFolder>", true);
            }
            return runImageFolderEvidence(argc, argv, *invocation);
        }
        if (argc == 4 && std::string_view{argv[1]} == "--ui-shutdown-smoke") {
            return runDesktop(argc,
                              argv,
                              true,
                              SmokeSources{
                                  .first = std::filesystem::path{argv[2]},
                                  .second = std::filesystem::path{argv[3]},
                              },
                              true);
        }
        if (argc == 2 && std::string_view{argv[1]} == "--ui-fatal-startup-smoke") {
            return dvs::app::reportFatalStartup("DVS_UI_FATAL_STARTUP_SMOKE", true);
        }
        return runDesktop(argc, argv, false);
    } catch (const std::exception& exception) {
        return dvs::app::reportFatalStartup(exception.what(), smokeArgument);
    } catch (...) {
        return dvs::app::reportFatalStartup("Unknown fatal startup exception.", smokeArgument);
    }
}
