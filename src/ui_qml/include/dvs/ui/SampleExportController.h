#pragma once

#include "dvs/application/SampleExport.h"
#include "dvs/application/SampleExportWriter.h"
#include "dvs/application/SessionSnapshot.h"
#include "dvs/domain/Identifiers.h"
#include "dvs/domain/InterpolationProtocol.h"

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantMap>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace dvs::ui {

// GUI-thread coordinator for training-sample triplet exports (E01).
// Bridges the user's export intent from the diagnostic card or transport bar to the worker-driven
// SampleExportWriter, extracting exact frames (input0, GT, input1, prediction) and publishing an
// atomic sample directory with manifest.
class SampleExportController final : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY stateChanged)
    Q_PROPERTY(QString lastStatus READ lastStatus NOTIFY stateChanged)
    Q_PROPERTY(QString lastFailureDetail READ lastFailureDetail NOTIFY stateChanged)
    Q_PROPERTY(QString lastOutputPath READ lastOutputPath NOTIFY stateChanged)
    Q_PROPERTY(int targetFrame READ targetFrame NOTIFY targetFrameChanged)
    Q_PROPERTY(bool canExport READ canExport NOTIFY targetFrameChanged)

public:
    struct Dependencies final {
        std::function<std::shared_ptr<const application::SessionSnapshot>()> snapshot;
        std::shared_ptr<application::ISampleExporter> exporter;
        std::function<const std::optional<domain::InterpolationProtocol>&()> protocol;
        std::function<std::optional<application::SampleCropRect>()> activeCropRect;
    };

    explicit SampleExportController(Dependencies dependencies, QObject* parent = nullptr);
    ~SampleExportController() override;

    SampleExportController(const SampleExportController&) = delete;
    SampleExportController& operator=(const SampleExportController&) = delete;
    SampleExportController(SampleExportController&&) = delete;
    SampleExportController& operator=(SampleExportController&&) = delete;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] double progress() const noexcept;
    [[nodiscard]] QString lastStatus() const;
    [[nodiscard]] QString lastFailureDetail() const;
    [[nodiscard]] QString lastOutputPath() const;
    [[nodiscard]] int targetFrame() const noexcept;
    [[nodiscard]] bool canExport() const;

    Q_INVOKABLE void setTargetFrame(int frame);
    [[nodiscard]] Q_INVOKABLE bool canExportAtFrame(int frame) const;
    [[nodiscard]] Q_INVOKABLE QVariantMap planSummaryAtFrame(int frame) const;
    [[nodiscard]] Q_INVOKABLE QString suggestedSampleId(int frame) const;
    [[nodiscard]] Q_INVOKABLE QString suggestedTargetDirectory() const;

    Q_INVOKABLE bool exportTriplet(const QUrl& outputDirectory,
                                   const QString& sampleId,
                                   bool cropToRoi,
                                   int frame = -1);
    Q_INVOKABLE void cancelExport();

Q_SIGNALS:
    void stateChanged();
    void targetFrameChanged();
    void exportFinished(bool succeeded, const QString& message);

private:
    struct CapturedJob final {
        application::SampleExportJob job;
    };

    [[nodiscard]] std::optional<CapturedJob> prepareJob(int frame,
                                                        const std::filesystem::path& targetDir,
                                                        const std::string& sampleId,
                                                        bool cropToRoi);

    void setStatus(QString status, QString failureDetail = {});

    Dependencies dependencies_;
    int targetFrame_ = -1;
    bool busy_ = false;
    double progress_ = 0.0;
    QString lastStatus_;
    QString lastFailureDetail_;
    QString lastOutputPath_;
    std::atomic_bool cancelRequested_{false};
    std::jthread workerThread_;
    application::SampleExportRequestId nextRequestId_ = 0;
};

} // namespace dvs::ui
