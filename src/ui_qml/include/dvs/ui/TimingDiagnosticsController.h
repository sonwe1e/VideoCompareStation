#pragma once

#include "dvs/application/PlaybackEvidence.h"
#include "dvs/application/PlaybackTrace.h"
#include "dvs/platform/PresentationStatisticsProbe.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

namespace dvs::ui {

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

    // application::ITraceObserver interface
    void onTraceEvent(const application::TraceEvent& event) noexcept override;

    [[nodiscard]] bool isMonitoring() const noexcept;
    void setMonitoring(bool monitoring);

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

    void updateFromSummary(const application::PlaybackEvidenceSummary& summary);
    void evaluateProbeCapability();

    application::PlaybackEvidenceCollector collector_;
    std::unique_ptr<platform::PresentationStatisticsProbe> probe_;
    QTimer* refreshTimer_ = nullptr;

    bool monitoring_ = true;
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
};

} // namespace dvs::ui
