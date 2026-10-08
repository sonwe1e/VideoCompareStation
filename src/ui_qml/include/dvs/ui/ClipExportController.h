#pragma once

#include "dvs/application/ClipExport.h"
#include "dvs/application/SessionSnapshot.h"
#include "dvs/domain/Identifiers.h"

#include <QObject>
#include <QString>
#include <QUrl>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

namespace dvs::ui {

// Exports the marked playback range (入点/出点) of the canonical source as a standalone clip.
// The work is a packet-level stream copy: no decode, no re-encode, so the exported frames are
// bit-identical to the source. Two honest limits are part of the contract and must stay visible
// in the UI: the clip start snaps back to the preceding keyframe (the plan records that pre-roll)
// and a reordered (B-frame) source can carry a few reference frames past the out point. Frame-exact
// cuts are the separate re-encode phase.
//
// Threading: exportRange() captures the whole job on the GUI thread and hands it to one worker
// thread (jthread, joined by the destructor); the worker never reads the snapshot. Completion and
// progress hop back through queued meta-calls tagged with the request id, so a stale completion
// cannot overwrite a newer run. One export at a time; cancelExport() is cooperative—the worker
// stops between keyframe reads and between packets, and no partial file is kept.
class ClipExportController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(bool canExport READ canExport NOTIFY stateChanged)
    Q_PROPERTY(QString suggestedFileName READ suggestedFileName NOTIFY stateChanged)
    Q_PROPERTY(QString rangeSummary READ rangeSummary NOTIFY stateChanged)
    Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY stateChanged)
    Q_PROPERTY(QString lastStatus READ lastStatus NOTIFY stateChanged)
    Q_PROPERTY(QString lastFailureDetail READ lastFailureDetail NOTIFY stateChanged)
    Q_PROPERTY(QString lastOutputPath READ lastOutputPath NOTIFY stateChanged)
public:
    struct Dependencies final {
        std::function<std::shared_ptr<const application::SessionSnapshot>()> snapshot;
        // Shared (not raw) so a running export keeps its adapter alive across a teardown hand-off.
        std::shared_ptr<application::IClipExporter> exporter;
    };

    explicit ClipExportController(Dependencies dependencies, QObject* parent = nullptr);
    ~ClipExportController() override;

    ClipExportController(const ClipExportController&) = delete;
    ClipExportController& operator=(const ClipExportController&) = delete;
    ClipExportController(ClipExportController&&) = delete;
    ClipExportController& operator=(ClipExportController&&) = delete;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] qreal progress() const noexcept;
    // True when the current snapshot has a complete marked range on a canonical video source.
    [[nodiscard]] bool canExport() const;
    // Readouts share the captured request while busy, otherwise they describe the current session.
    // "<stem>_clip_<in>-<out><source extension>" over the canonical source file, or empty.
    [[nodiscard]] QString suggestedFileName() const;
    // One-line, 1-based frame label matching the transport range label, or empty.
    [[nodiscard]] QString rangeSummary() const;
    // Native path of the canonical export source; pinned to the captured job while busy.
    [[nodiscard]] QString sourcePath() const;
    [[nodiscard]] QString lastStatus() const;
    [[nodiscard]] QString lastFailureDetail() const;
    [[nodiscard]] QString lastOutputPath() const;

    // Canonical source folder + suggestedFileName(); an invalid URL when nothing is exportable.
    [[nodiscard]] Q_INVOKABLE QUrl suggestedTarget() const;
    // Starts an export of the marked range to target (must be a local file URL). Returns false,
    // with the reason in lastStatus, when there is nothing exportable or an export is running.
    Q_INVOKABLE bool exportRange(const QUrl& target);
    // Cooperative cancellation of the running export; the worker reports kCanceled and keeps no
    // partial file.
    Q_INVOKABLE void cancelExport();

signals:
    void stateChanged();
    void progressChanged();
    void exportFinished(bool succeeded, const QString& message);

private:
    // Everything the worker needs, captured on the GUI thread before launch.
    struct Request final {
        application::ClipExportRequestId id = application::kInvalidClipExportRequestId;
        domain::SessionId sessionId{0U};
        domain::SessionEpoch sessionEpoch{0U};
        std::filesystem::path sourcePath;
        std::filesystem::path targetPath;
        // optional because CanonicalTimeline is a variant whose first alternative has no default
        // constructor; makeRequest() only produces a Request once the timeline is known good.
        std::optional<domain::CanonicalTimeline> timeline;
        std::int64_t canonicalFrameCount = 0;
        application::PlaybackRange range{};
    };

    [[nodiscard]] std::optional<Request> makeRequest(std::filesystem::path targetPath) const;
    [[nodiscard]] std::optional<Request> displayRequest() const;
    void runExport(const Request& request) noexcept;
    void postProgress(application::ClipExportRequestId requestId, qreal value);
    void postFinished(application::ClipExportReport report);
    void applyProgress(application::ClipExportRequestId requestId, qreal value);
    void applyFinished(const application::ClipExportReport& report);
    void setBusy(bool value);
    void setProgress(qreal value);
    void setStatus(QString text);

    Dependencies dependencies_;
    std::atomic_bool cancel_{false};
    application::ClipExportRequestId nextRequestId_ = application::kInvalidClipExportRequestId;
    application::ClipExportRequestId activeRequestId_ = application::kInvalidClipExportRequestId;
    std::jthread worker_;
    bool busy_ = false;
    qreal progress_ = 0.0;
    QString lastStatus_;
    QString lastFailureDetail_;
    QString lastOutputPath_;
    // Native-separator target path captured at launch; shown as lastOutputPath after success.
    QString pendingOutputPath_;
    // One captured request owns all running-job readouts; idle readouts use the current session.
    std::optional<Request> activeRequest_;
};

} // namespace dvs::ui
