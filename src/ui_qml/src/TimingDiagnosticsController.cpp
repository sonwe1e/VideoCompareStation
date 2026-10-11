#include "dvs/ui/TimingDiagnosticsController.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace dvs::ui {
namespace {

// Single-flight capability probing cost cap. Hardware device creation dominates and the probe's
// own verification presents add a few vsync intervals; past this the panel shows the explicit
// unconfirmed state while the worker keeps running and a current-generation answer still applies.
constexpr int kProbeTimeoutMs = 4000;

[[nodiscard]] QString unprobedStatusText() {
    return QStringLiteral("未确认 (未探测)");
}

[[nodiscard]] QString probingStatusText() {
    return QStringLiteral("未确认 (探测中)");
}

[[nodiscard]] QString probeTimeoutStatusText() {
    return QStringLiteral("未确认 (探测超时)");
}

// A finished capability answer plus the request generation it was produced for. The controller
// applies an outcome only while its generation is the current one, so an answer that arrives
// after a newer request or a replaced presentation target can never overwrite the panel (E01).
struct ProbeOutcome final {
    std::uint64_t generation = 0U;
    platform::PresentationStatisticsCapability capability{};
};

} // namespace

// Runs every platform probe call on one dedicated thread. The probe lazily creates a hidden
// top-level window, a D3D11 device and a flip-model swap chain; constructing, sampling and
// destroying all of them stays on this thread (E01), and no HWND is ever used from another
// thread. Commands are coalesced flags, so a request issued while a call runs is picked up on
// the next wake; answers are single-slot mailboxes the GUI thread drains at its own cadence.
class PresentationProbeWorker final {
public:
    explicit PresentationProbeWorker(std::unique_ptr<platform::PresentationStatisticsProbe> seed)
        : seed_(std::move(seed)) {}

    ~PresentationProbeWorker() {
        shutdown();
    }

    PresentationProbeWorker(const PresentationProbeWorker&) = delete;
    PresentationProbeWorker& operator=(const PresentationProbeWorker&) = delete;

    void requestCapability(const std::uint64_t generation) {
        startIfNeeded();
        {
            std::lock_guard lock(mutex_);
            capabilityRequested_ = true;
            requestedGeneration_ = generation;
        }
        condition_.notify_all();
    }

    void requestSample() {
        startIfNeeded();
        {
            std::lock_guard lock(mutex_);
            sampleRequested_ = true;
        }
        condition_.notify_all();
    }

    [[nodiscard]] bool tryTakeCapability(ProbeOutcome& out) {
        std::lock_guard lock(mutex_);
        if (!capabilityAnswer_.has_value()) {
            return false;
        }
        out = std::move(*capabilityAnswer_);
        capabilityAnswer_.reset();
        return true;
    }

    [[nodiscard]] bool tryTakeSample(platform::PresentationStatisticsSample& out) {
        std::lock_guard lock(mutex_);
        if (!sampleAnswer_.has_value()) {
            return false;
        }
        out = std::move(*sampleAnswer_);
        sampleAnswer_.reset();
        return true;
    }

    // Joins the worker; the probe's window/device/swap chain are released inside run() on the
    // worker thread before it exits. Safe to call twice.
    void shutdown() {
        {
            std::lock_guard lock(startMutex_);
            if (!started_) {
                return;
            }
        }
        {
            std::lock_guard lock(mutex_);
            shutdownRequested_ = true;
        }
        condition_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void startIfNeeded() {
        std::lock_guard startLock(startMutex_);
        if (!started_) {
            started_ = true;
            thread_ = std::thread([this] { run(); });
        }
    }

    void run() {
        std::unique_ptr<platform::PresentationStatisticsProbe> probe;
        {
            std::lock_guard lock(mutex_);
            probe = std::move(seed_);
        }
        for (;;) {
            bool probeRequested = false;
            bool sampleRequested = false;
            std::uint64_t generation = 0U;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] {
                    return shutdownRequested_ || capabilityRequested_ || sampleRequested_;
                });
                if (shutdownRequested_) {
                    // probe (and with it window, device and swap chain) dies here, on this thread.
                    return;
                }
                probeRequested = capabilityRequested_;
                capabilityRequested_ = false;
                generation = requestedGeneration_;
                sampleRequested = sampleRequested_;
                sampleRequested_ = false;
            }
            // Expensive platform work runs without holding the mutex so the GUI thread can drain
            // earlier answers and issue coalesced commands meanwhile.
            if (probeRequested && probe != nullptr) {
                platform::PresentationStatisticsCapability capability = probe->probeCapability();
                std::lock_guard lock(mutex_);
                capabilityAnswer_ = ProbeOutcome{generation, std::move(capability)};
            }
            if (sampleRequested && probe != nullptr) {
                platform::PresentationStatisticsSample sample = probe->sampleFrameStatistics();
                std::lock_guard lock(mutex_);
                sampleAnswer_ = std::move(sample);
            }
        }
    }

    std::mutex startMutex_;
    std::thread thread_;
    bool started_ = false;

    std::mutex mutex_;
    std::condition_variable condition_;
    std::unique_ptr<platform::PresentationStatisticsProbe> seed_;
    bool shutdownRequested_ = false;
    bool capabilityRequested_ = false;
    std::uint64_t requestedGeneration_ = 0U;
    bool sampleRequested_ = false;
    std::optional<ProbeOutcome> capabilityAnswer_;
    std::optional<platform::PresentationStatisticsSample> sampleAnswer_;
};

TimingDiagnosticsController::TimingDiagnosticsController(QObject* parent)
    : TimingDiagnosticsController(std::make_unique<platform::PresentationStatisticsProbe>(),
                                  parent) {}

TimingDiagnosticsController::TimingDiagnosticsController(
    std::unique_ptr<platform::PresentationStatisticsProbe> probe, QObject* parent)
    : QObject(parent), probeSeed_(std::move(probe)) {
    // E01: construction sits on the startup critical path, so it only builds lightweight state.
    // The capability probe (hidden window + D3D11 device + swap chain) is no longer run here;
    // the diagnostics surface requests it on demand and the worker thread executes it.
    // E02: monitoring defaults off, so nothing is installed on the trace and the trace clock
    // stays wherever its owner left it — an ordinary run records nothing on behalf of this
    // panel. setMonitoring(true) installs the observer (which enables the clock through its
    // existing contract), opens the observer gate and starts the refresh timer.
    systemPresentationStatus_ = unprobedStatusText();
    driverKind_ = QStringLiteral("未知");

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(500);
    connect(refreshTimer_, &QTimer::timeout, this, &TimingDiagnosticsController::refresh);

    probeTimeoutTimer_ = new QTimer(this);
    probeTimeoutTimer_->setSingleShot(true);
    connect(probeTimeoutTimer_, &QTimer::timeout, this, [this] {
        if (!probeInFlight_) {
            return;
        }
        // Honest unconfirmed state; the single flight keeps running and a current-generation
        // answer still applies whenever it arrives.
        systemPresentationStatus_ = probeTimeoutStatusText();
        Q_EMIT diagnosticsUpdated();
    });
}

TimingDiagnosticsController::~TimingDiagnosticsController() {
    // Shuts the worker down and joins it; the probe resources are released on the worker thread.
    probeWorker_.reset();
    application::PlaybackTrace& trace = application::PlaybackTrace::instance();
    // Lifecycle fence (E02): the gate stops new dispatches without waiting; the observer
    // uninstall takes the unique lock, so it returns only after a callback already inside has
    // finished — no dispatch can touch a destructing controller. Restoring the default-open gate
    // leaves observers installed later by anyone else unaffected.
    trace.setObserverEnabled(false);
    trace.installObserver(nullptr);
    trace.setObserverEnabled(true);
}

void TimingDiagnosticsController::onTraceEvent(const application::TraceEvent& event) noexcept {
    // Second line of defence behind the PlaybackTrace gate (E02): an event that passed the gate
    // check while monitoring was being switched off must not ingest either.
    if (!monitoring_.load(std::memory_order_acquire)) {
        return;
    }
    try {
        collector_.ingest(event);
    } catch (...) { // NOLINT(bugprone-empty-catch): observer must never fail playback workers
    }
}

bool TimingDiagnosticsController::isMonitoring() const noexcept {
    return monitoring_.load(std::memory_order_acquire);
}

void TimingDiagnosticsController::setMonitoring(const bool monitoring) {
    if (isMonitoring() == monitoring) {
        return;
    }
    application::PlaybackTrace& trace = application::PlaybackTrace::instance();
    if (monitoring) {
        // First activation installs the observer; installObserver enables the trace clock when
        // nobody else did. Later toggles only flip the gate, so repeated on/off cycles never
        // re-run the installation.
        trace.installObserver(this);
        trace.setObserverEnabled(true);
        monitoring_.store(true, std::memory_order_release);
        if (refreshTimer_ != nullptr) {
            refreshTimer_->start();
        }
    } else {
        // Close the gate BEFORE the local flag: producers stop dispatching at the gate check,
        // and any dispatch already past the gate re-checks this flag before ingesting. The
        // trace clock and any sink are left alone so file captures keep running (E02).
        trace.setObserverEnabled(false);
        monitoring_.store(false, std::memory_order_release);
        if (refreshTimer_ != nullptr) {
            refreshTimer_->stop();
        }
    }
    Q_EMIT monitoringChanged();
}

void TimingDiagnosticsController::requestPresentationProbe() {
    if (probeApplied_ || probeInFlight_) {
        // A completed capability stays the cached answer; a running single flight re-checks its
        // generation on completion, so a second request cannot duplicate the platform work.
        return;
    }
    if (probeWorker_ == nullptr) {
        probeWorker_ = std::make_unique<PresentationProbeWorker>(std::move(probeSeed_));
    }
    ++probeGeneration_;
    probeInFlight_ = true;
    systemPresentationStatus_ = probingStatusText();
    if (probeTimeoutTimer_ != nullptr) {
        probeTimeoutTimer_->start(kProbeTimeoutMs);
    }
    probeWorker_->requestCapability(probeGeneration_);
    Q_EMIT diagnosticsUpdated();
}

void TimingDiagnosticsController::notePresentationTargetReplaced() {
    ++probeGeneration_;
    probeInFlight_ = false;
    probeApplied_ = false;
    appliedProbeGeneration_ = 0U;
    if (probeTimeoutTimer_ != nullptr) {
        probeTimeoutTimer_->stop();
    }
    systemPresentationStatus_ = unprobedStatusText();
    driverKind_ = QStringLiteral("未知");
    isIndirectDisplayWarning_ = false;
    presentationDetail_.clear();
    presentCount_ = 0U;
    presentRefreshCount_ = 0U;
    Q_EMIT diagnosticsUpdated();
}

bool TimingDiagnosticsController::hasActivePlayback() const noexcept {
    return hasActivePlayback_;
}

quint64 TimingDiagnosticsController::runId() const noexcept {
    return runId_;
}

quint64 TimingDiagnosticsController::skippedFrameSetCount() const noexcept {
    return skippedFrameSetCount_;
}

double TimingDiagnosticsController::decodeLatencyP95Ms() const noexcept {
    return decodeLatencyP95Ms_;
}

double TimingDiagnosticsController::decodeLatencyMaxMs() const noexcept {
    return decodeLatencyMaxMs_;
}

double TimingDiagnosticsController::renderScheduleP95Ms() const noexcept {
    return renderScheduleP95Ms_;
}

double TimingDiagnosticsController::renderScheduleMaxMs() const noexcept {
    return renderScheduleMaxMs_;
}

double TimingDiagnosticsController::drawToAckP95Ms() const noexcept {
    return drawToAckP95Ms_;
}

double TimingDiagnosticsController::drawToAckMaxMs() const noexcept {
    return drawToAckMaxMs_;
}

double TimingDiagnosticsController::renderToAckP95Ms() const noexcept {
    return renderToAckP95Ms_;
}

double TimingDiagnosticsController::renderToAckMaxMs() const noexcept {
    return renderToAckMaxMs_;
}

QString TimingDiagnosticsController::systemPresentationStatus() const {
    return systemPresentationStatus_;
}

QString TimingDiagnosticsController::driverKind() const {
    return driverKind_;
}

bool TimingDiagnosticsController::isIndirectDisplayWarning() const noexcept {
    return isIndirectDisplayWarning_;
}

QString TimingDiagnosticsController::presentationDetail() const {
    return presentationDetail_;
}

quint32 TimingDiagnosticsController::presentCount() const noexcept {
    return presentCount_;
}

quint32 TimingDiagnosticsController::presentRefreshCount() const noexcept {
    return presentRefreshCount_;
}

quint64 TimingDiagnosticsController::appliedProbeGeneration() const noexcept {
    return appliedProbeGeneration_;
}

void TimingDiagnosticsController::ingestTraceEvent(const application::TraceEvent& event) {
    collector_.ingest(event);
    refresh();
}

void TimingDiagnosticsController::refresh() {
    pollPresentationProbe();
    const application::PlaybackEvidenceSummary summary = collector_.summary();
    updateFromSummary(summary);
    Q_EMIT diagnosticsUpdated();
}

void TimingDiagnosticsController::reset() {
    collector_.reset();
    refresh();
}

void TimingDiagnosticsController::pollPresentationProbe() {
    if (probeWorker_ == nullptr) {
        return;
    }

    ProbeOutcome outcome;
    if (probeWorker_->tryTakeCapability(outcome)) {
        probeInFlight_ = false;
        if (probeTimeoutTimer_ != nullptr) {
            probeTimeoutTimer_->stop();
        }
        // A stale answer (superseded request or replaced presentation target) is dropped, never
        // applied (E01).
        if (outcome.generation == probeGeneration_ && !probeApplied_) {
            probeApplied_ = true;
            appliedProbeGeneration_ = outcome.generation;
            applyProbeCapability(outcome.capability);
        }
    }

    platform::PresentationStatisticsSample sample;
    if (probeWorker_->tryTakeSample(sample)) {
        sampleInFlight_ = false;
        if (sample.status == platform::PresentationStatisticsStatus::kAvailable &&
            sample.snapshot.has_value()) {
            presentCount_ = sample.snapshot->presentCount;
            presentRefreshCount_ = sample.snapshot->presentRefreshCount;
        }
    }

    if (monitoring_ && probeApplied_ && !sampleInFlight_) {
        probeWorker_->requestSample();
        sampleInFlight_ = true;
    }
}

void TimingDiagnosticsController::applyProbeCapability(
    const platform::PresentationStatisticsCapability& capability) {
    isIndirectDisplayWarning_ = capability.isIndirectDisplayPath;
    presentationDetail_ = QString::fromStdString(capability.technicalDetail);

    if (capability.driverKind.has_value()) {
        switch (*capability.driverKind) {
        case platform::PresentationStatisticsDriverKind::kHardware:
            driverKind_ = QStringLiteral("Hardware");
            break;
        case platform::PresentationStatisticsDriverKind::kWarp:
            driverKind_ = QStringLiteral("WARP");
            break;
        }
    } else {
        driverKind_ = QStringLiteral("未知");
    }

    switch (capability.status) {
    case platform::PresentationStatisticsStatus::kAvailable:
        systemPresentationStatus_ = QStringLiteral("可用 (Flip Model)");
        break;
    case platform::PresentationStatisticsStatus::kMediaStatisticsUnsupported:
        systemPresentationStatus_ = QStringLiteral("基础可用 (媒体统计受限)");
        break;
    case platform::PresentationStatisticsStatus::kRequiresFlipModel:
        systemPresentationStatus_ = QStringLiteral("不支持 (需 Flip 模型)");
        break;
    case platform::PresentationStatisticsStatus::kStatisticsDisjoint:
        systemPresentationStatus_ = QStringLiteral("不连贯 (重试中)");
        break;
    case platform::PresentationStatisticsStatus::kDeviceUnavailable:
        systemPresentationStatus_ = QStringLiteral("设备不可用");
        break;
    case platform::PresentationStatisticsStatus::kWindowUnavailable:
        systemPresentationStatus_ = QStringLiteral("窗口不可用");
        break;
    case platform::PresentationStatisticsStatus::kUnknown:
    default:
        systemPresentationStatus_ = QStringLiteral("未知 (未确认)");
        break;
    }
}

void TimingDiagnosticsController::updateFromSummary(
    const application::PlaybackEvidenceSummary& summary) {
    if (summary.runs.empty()) {
        hasActivePlayback_ = false;
        runId_ = 0U;
        skippedFrameSetCount_ = 0U;
        decodeLatencyP95Ms_ = 0.0;
        decodeLatencyMaxMs_ = 0.0;
        renderScheduleP95Ms_ = 0.0;
        renderScheduleMaxMs_ = 0.0;
        drawToAckP95Ms_ = 0.0;
        drawToAckMaxMs_ = 0.0;
        renderToAckP95Ms_ = 0.0;
        renderToAckMaxMs_ = 0.0;
        return;
    }

    const application::PlaybackRunEvidence& latestRun = summary.runs.back();
    hasActivePlayback_ = latestRun.open;
    runId_ = latestRun.runId;
    skippedFrameSetCount_ = latestRun.skippedFrameSetCount;

    const auto& decodeStage = latestRun.stages[static_cast<std::size_t>(
        application::PlaybackEvidenceStage::kSubmitToDecodeCompleted)];
    decodeLatencyP95Ms_ = static_cast<double>(decodeStage.p95Microseconds) / 1000.0;
    decodeLatencyMaxMs_ = static_cast<double>(decodeStage.maxMicroseconds) / 1000.0;

    const auto& scheduleStage =
        latestRun
            .stages[static_cast<std::size_t>(application::PlaybackEvidenceStage::kRenderSchedule)];
    renderScheduleP95Ms_ = static_cast<double>(scheduleStage.p95Microseconds) / 1000.0;
    renderScheduleMaxMs_ = static_cast<double>(scheduleStage.maxMicroseconds) / 1000.0;

    const auto& drawStage = latestRun.stages[static_cast<std::size_t>(
        application::PlaybackEvidenceStage::kDrawToAckPublished)];
    drawToAckP95Ms_ = static_cast<double>(drawStage.p95Microseconds) / 1000.0;
    drawToAckMaxMs_ = static_cast<double>(drawStage.maxMicroseconds) / 1000.0;

    const auto& ackStage = latestRun.stages[static_cast<std::size_t>(
        application::PlaybackEvidenceStage::kRenderToPresentationAck)];
    renderToAckP95Ms_ = static_cast<double>(ackStage.p95Microseconds) / 1000.0;
    renderToAckMaxMs_ = static_cast<double>(ackStage.maxMicroseconds) / 1000.0;
}

} // namespace dvs::ui
