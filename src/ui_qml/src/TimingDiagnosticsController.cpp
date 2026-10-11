#include "dvs/ui/TimingDiagnosticsController.h"

#include <utility>

namespace dvs::ui {

TimingDiagnosticsController::TimingDiagnosticsController(QObject* parent)
    : TimingDiagnosticsController(std::make_unique<platform::PresentationStatisticsProbe>(),
                                  parent) {}

TimingDiagnosticsController::TimingDiagnosticsController(
    std::unique_ptr<platform::PresentationStatisticsProbe> probe, QObject* parent)
    : QObject(parent), probe_(std::move(probe)) {
    evaluateProbeCapability();
    application::PlaybackTrace::instance().installObserver(this);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(500);
    connect(refreshTimer_, &QTimer::timeout, this, &TimingDiagnosticsController::refresh);
    if (monitoring_) {
        refreshTimer_->start();
    }
}

TimingDiagnosticsController::~TimingDiagnosticsController() {
    application::PlaybackTrace::instance().installObserver(nullptr);
}

void TimingDiagnosticsController::onTraceEvent(const application::TraceEvent& event) noexcept {
    try {
        collector_.ingest(event);
    } catch (...) { // NOLINT(bugprone-empty-catch): observer must never fail playback workers
    }
}

bool TimingDiagnosticsController::isMonitoring() const noexcept {
    return monitoring_;
}

void TimingDiagnosticsController::setMonitoring(const bool monitoring) {
    if (monitoring_ == monitoring) {
        return;
    }
    monitoring_ = monitoring;
    if (refreshTimer_ != nullptr) {
        if (monitoring_) {
            refreshTimer_->start();
        } else {
            refreshTimer_->stop();
        }
    }
    Q_EMIT monitoringChanged();
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

void TimingDiagnosticsController::ingestTraceEvent(const application::TraceEvent& event) {
    collector_.ingest(event);
    refresh();
}

void TimingDiagnosticsController::refresh() {
    const application::PlaybackEvidenceSummary summary = collector_.summary();
    updateFromSummary(summary);

    if (probe_ != nullptr) {
        const platform::PresentationStatisticsSample sample = probe_->sampleFrameStatistics();
        if (sample.status == platform::PresentationStatisticsStatus::kAvailable &&
            sample.snapshot.has_value()) {
            presentCount_ = sample.snapshot->presentCount;
            presentRefreshCount_ = sample.snapshot->presentRefreshCount;
        }
    }

    Q_EMIT diagnosticsUpdated();
}

void TimingDiagnosticsController::reset() {
    collector_.reset();
    refresh();
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

void TimingDiagnosticsController::evaluateProbeCapability() {
    if (probe_ == nullptr) {
        systemPresentationStatus_ = QStringLiteral("不可用 (无探针)");
        driverKind_ = QStringLiteral("未知");
        isIndirectDisplayWarning_ = false;
        presentationDetail_.clear();
        return;
    }

    const platform::PresentationStatisticsCapability capability = probe_->probeCapability();
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

} // namespace dvs::ui
