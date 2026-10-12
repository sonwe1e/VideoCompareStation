#pragma once

#include "dvs/application/PlaybackEvidence.h"
#include "dvs/application/PlaybackTrace.h"
#include "dvs/platform/PresentationStatisticsProbe.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <memory>

namespace dvs::ui {

// Owns the presentation statistics probe on exactly one worker thread (E01). Forward declared so
// the probe's thread and lifetime never leak into controller users; defined in the .cpp together
// with the thread body that creates, uses and destroys the probe's hidden window, D3D11 device
// and swap chain.
class PresentationProbeWorker;

class TimingDiagnosticsController final : public QObject, public application::ITraceObserver {
    Q_OBJECT

    Q_PROPERTY(bool monitoring READ isMonitoring WRITE setMonitoring NOTIFY monitoringChanged)
    Q_PROPERTY(bool hasActivePlayback READ hasActivePlayback NOTIFY diagnosticsUpdated)
    Q_PROPERTY(quint64 runId READ runId NOTIFY diagnosticsUpdated)
    Q_PROPERTY(quint64 skippedFrameSetCount READ skippedFrameSetCount NOTIFY diagnosticsUpdated)
    Q_PROPERTY(double decodeLatencyP95Ms READ decodeLatencyP95Ms NOTIFY diagnosticsUpdated)
    Q_PROPERTY(double decodeLatencyMaxMs READ decodeLatencyMaxMs NOTIFY diagnosticsUpdated)
    Q_PROPERTY(double renderScheduleP95Ms READ renderScheduleP95Ms NOTIFY diagnosticsUpdated)
    Q_PROPERTY(double renderScheduleMaxMs READ renderScheduleMaxMs NOTIFY diagnosticsUpdated)
    Q_PROPERTY(double drawToAckP95Ms READ drawToAckP95Ms NOTIFY diagnosticsUpdated)
    Q_PROPERTY(double drawToAckMaxMs READ drawToAckMaxMs NOTIFY diagnosticsUpdated)
    Q_PROPERTY(double renderToAckP95Ms READ renderToAckP95Ms NOTIFY diagnosticsUpdated)
    Q_PROPERTY(double renderToAckMaxMs READ renderToAckMaxMs NOTIFY diagnosticsUpdated)
    Q_PROPERTY(
        QString systemPresentationStatus READ systemPresentationStatus NOTIFY diagnosticsUpdated)
    Q_PROPERTY(QString driverKind READ driverKind NOTIFY diagnosticsUpdated)
    Q_PROPERTY(
        bool isIndirectDisplayWarning READ isIndirectDisplayWarning NOTIFY diagnosticsUpdated)
    Q_PROPERTY(QString presentationDetail READ presentationDetail NOTIFY diagnosticsUpdated)
    Q_PROPERTY(quint32 presentCount READ presentCount NOTIFY diagnosticsUpdated)
    Q_PROPERTY(quint32 presentRefreshCount READ presentRefreshCount NOTIFY diagnosticsUpdated)

public:
    explicit TimingDiagnosticsController(QObject* parent = nullptr);
    TimingDiagnosticsController(std::unique_ptr<platform::PresentationStatisticsProbe> probe,
                                QObject* parent = nullptr);
    ~TimingDiagnosticsController() override;

    // application::ITraceObserver interface. Defence in depth behind the PlaybackTrace observer
    // gate (E02): an event that passed the gate while monitoring was switching off still must
    // not ingest.
    void onTraceEvent(const application::TraceEvent& event) noexcept override;

    // E02: monitoring splits four states. Off (the default) stops the observer ingest at the
    // PlaybackTrace gate, stops the refresh timer, and — when this controller's installation was
    // what enabled the process-global trace clock and no file sink shares it — disables that
    // clock again, so a stopped panel leaves no per-event work behind. On installs this
    // controller as the trace observer the first time (which enables the trace clock through
    // that existing contract), opens the gate, and runs the refresh timer. The capability probe
    // is independent of this switch and requested on demand (E01).
    [[nodiscard]] bool isMonitoring() const noexcept;
    void setMonitoring(bool monitoring);

    // E01: the capability probe builds its own hidden window, D3D11 device and swap chain.
    // Construction no longer runs it: the diagnostics surface requests it when it opens, and the
    // probe runs as a single-flight task on the worker thread. Failure and timeout surface as
    // explicit "未确认" states; playback is never blocked. Repeated requests reuse the cached
    // answer, and a completed probe is never re-run.
    Q_INVOKABLE void requestPresentationProbe();

    // Invalidation hook for window/session handover: bumps the probe generation so any in-flight
    // capability answer carries a stale identity and is dropped instead of overwriting the state
    // the new target starts from (E01). Reserved: no production path calls it yet - the probe
    // owns its own hidden window/device, so a main-window swap does not invalidate its
    // machine-level answer - and the tests drive it directly. The panel returns to
    // "未确认 (未探测)" until a fresh request completes; note that the worker's probe caches its
    // capability, so a re-request after this hook re-applies the cached answer until a real
    // handover consumer teaches the probe to invalidate (tracked in the backlog).
    void notePresentationTargetReplaced();

    [[nodiscard]] bool hasActivePlayback() const noexcept;
    [[nodiscard]] quint64 runId() const noexcept;
    [[nodiscard]] quint64 skippedFrameSetCount() const noexcept;
    [[nodiscard]] double decodeLatencyP95Ms() const noexcept;
    [[nodiscard]] double decodeLatencyMaxMs() const noexcept;
    [[nodiscard]] double renderScheduleP95Ms() const noexcept;
    [[nodiscard]] double renderScheduleMaxMs() const noexcept;
    [[nodiscard]] double drawToAckP95Ms() const noexcept;
    [[nodiscard]] double drawToAckMaxMs() const noexcept;
    [[nodiscard]] double renderToAckP95Ms() const noexcept;
    [[nodiscard]] double renderToAckMaxMs() const noexcept;
    [[nodiscard]] QString systemPresentationStatus() const;
    [[nodiscard]] QString driverKind() const;
    [[nodiscard]] bool isIndirectDisplayWarning() const noexcept;
    [[nodiscard]] QString presentationDetail() const;
    [[nodiscard]] quint32 presentCount() const noexcept;
    [[nodiscard]] quint32 presentRefreshCount() const noexcept;
    // Generation of the capability answer currently displayed, or 0 while unprobed. Lets gates
    // and tests observe which request an answer belongs to without trusting status text.
    [[nodiscard]] quint64 appliedProbeGeneration() const noexcept;

    // Test helper to feed events directly without requiring a live PlaybackTrace singleton
    void ingestTraceEvent(const application::TraceEvent& event);

public Q_SLOTS:
    void refresh();
    void reset();

Q_SIGNALS:
    void monitoringChanged();
    void diagnosticsUpdated();

private:
    Q_DISABLE_COPY_MOVE(TimingDiagnosticsController)

    // Drains the worker's finished capability/sample answers, applies the ones whose generation
    // is still current, and schedules the next present-count sample while diagnostics run.
    void pollPresentationProbe();
    void applyProbeCapability(const platform::PresentationStatisticsCapability& capability);
    void updateFromSummary(const application::PlaybackEvidenceSummary& summary);
    // E02: if this controller's observer installation enabled the process-global trace clock and
    // no file sink shares it, restores the default-off clock so "monitoring off" really removes
    // the per-event work this controller added.
    void releaseTraceClockIfOwned();
    [[nodiscard]] bool probeApplied() const noexcept {
        return appliedProbeGeneration_ != 0U;
    }

    application::PlaybackEvidenceCollector collector_;
    // Probe seed adopted by the worker on first use; never used on the adopting thread after the
    // worker exists, so every probe resource stays on the worker thread.
    std::unique_ptr<platform::PresentationStatisticsProbe> probeSeed_;
    std::unique_ptr<PresentationProbeWorker> probeWorker_;
    QTimer* refreshTimer_ = nullptr;
    QTimer* probeTimeoutTimer_ = nullptr;

    // Master diagnostics switch, written by the GUI thread and read by every trace-producer
    // thread inside onTraceEvent; acquire/release pairing makes the stop visible without locks.
    std::atomic<bool> monitoring_{false};
    // E02 lifecycle bookkeeping: whether this controller installed the trace observer, and
    // whether that installation was the one that enabled the process-global trace clock. The
    // ownership flag is sticky while the observer is installed, so a later activation can re-arm
    // the clock an earlier stop disabled; only an owned enable is ever restored to the default.
    bool observerInstalled_ = false;
    bool traceClockEnabledByThis_ = false;
    bool hasActivePlayback_ = false;
    quint64 runId_ = 0U;
    quint64 skippedFrameSetCount_ = 0U;
    double decodeLatencyP95Ms_ = 0.0;
    double decodeLatencyMaxMs_ = 0.0;
    double renderScheduleP95Ms_ = 0.0;
    double renderScheduleMaxMs_ = 0.0;
    double drawToAckP95Ms_ = 0.0;
    double drawToAckMaxMs_ = 0.0;
    double renderToAckP95Ms_ = 0.0;
    double renderToAckMaxMs_ = 0.0;
    QString systemPresentationStatus_;
    QString driverKind_;
    bool isIndirectDisplayWarning_ = false;
    QString presentationDetail_;
    quint32 presentCount_ = 0U;
    quint32 presentRefreshCount_ = 0U;
    quint64 probeGeneration_ = 0U;
    quint64 appliedProbeGeneration_ = 0U;
    bool probeInFlight_ = false;
    bool sampleInFlight_ = false;
    // Generations of the capability/sample requests currently in flight: a superseded answer
    // must not clear a newer request's in-flight state or stop its timeout (E01).
    quint64 probeInFlightGeneration_ = 0U;
    quint64 sampleInFlightGeneration_ = 0U;
};

} // namespace dvs::ui
