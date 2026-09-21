#pragma once

#include "dvs/application/Commands.h"
#include "dvs/application/Events.h"
#include "dvs/application/Ports.h"
#include "dvs/application/SessionSnapshot.h"

#include <QAbstractItemModel>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class QUrl;

namespace dvs::ui {

// GUI-thread projection of the immutable application snapshot. Production wakes this projection
// from coordinator publication events; a short fallback timer exists only for isolated adapters
// that do not provide a wake bridge.
class ReviewController final : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString sourceAFilename READ sourceAFilename NOTIFY stateChanged)
    Q_PROPERTY(QString sourceBFilename READ sourceBFilename NOTIFY stateChanged)
    Q_PROPERTY(QString sourceCFilename READ sourceCFilename NOTIFY stateChanged)
    Q_PROPERTY(QVariantList sourceUrls READ sourceUrls NOTIFY stateChanged)
    Q_PROPERTY(QStringList sourceParentLabels READ sourceParentLabels NOTIFY stateChanged)
    Q_PROPERTY(QStringList sourceFullPaths READ sourceFullPaths NOTIFY stateChanged)
    Q_PROPERTY(QVariantList activeSources READ activeSources NOTIFY stateChanged)
    Q_PROPERTY(QAbstractItemModel* sources READ sources CONSTANT)
    Q_PROPERTY(int sourceCount READ sourceCount NOTIFY stateChanged)
    Q_PROPERTY(int canonicalSourceIndex READ canonicalSourceIndex NOTIFY stateChanged)
    Q_PROPERTY(int referenceSourceIndex READ referenceSourceIndex NOTIFY stateChanged)
    Q_PROPERTY(
        QString playbackContinuityPolicyName READ playbackContinuityPolicyName NOTIFY stateChanged)
    Q_PROPERTY(int playbackContinuityPolicy READ playbackContinuityPolicy NOTIFY stateChanged)
    Q_PROPERTY(
        qulonglong playbackSkippedFrameSets READ playbackSkippedFrameSets NOTIFY stateChanged)
    Q_PROPERTY(int effectiveDifferenceEdge READ effectiveDifferenceEdge NOTIFY stateChanged)
    Q_PROPERTY(ReviewDisplayState displayState READ displayState NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool framePending READ framePending NOTIFY stateChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY stateChanged)
    Q_PROPERTY(qreal playbackRate READ playbackRate NOTIFY stateChanged)
    Q_PROPERTY(qint64 playbackRangeIn READ playbackRangeIn NOTIFY stateChanged)
    Q_PROPERTY(qint64 playbackRangeOut READ playbackRangeOut NOTIFY stateChanged)
    Q_PROPERTY(bool playbackRangeLoop READ playbackRangeLoop NOTIFY stateChanged)
    Q_PROPERTY(bool playbackRangeLoopActive READ playbackRangeLoopActive NOTIFY stateChanged)
    Q_PROPERTY(bool graphicsReady READ graphicsReady NOTIFY stateChanged)
    Q_PROPERTY(qint64 currentFrame READ currentFrame NOTIFY frameStateChanged)
    Q_PROPERTY(qulonglong totalFrames READ totalFrames NOTIFY stateChanged)
    Q_PROPERTY(int oneSecondStepFrames READ oneSecondStepFrames NOTIFY stateChanged)
    Q_PROPERTY(qint64 currentMediaTime READ currentMediaTime NOTIFY frameStateChanged)
    Q_PROPERTY(QString currentTimecode READ currentTimecode NOTIFY frameStateChanged)
    Q_PROPERTY(QString rationalFrameRate READ rationalFrameRate NOTIFY stateChanged)
    Q_PROPERTY(QString timingMode READ timingMode NOTIFY stateChanged)
    Q_PROPERTY(bool dropFrameTimecodeAvailable READ dropFrameTimecodeAvailable NOTIFY stateChanged)
    Q_PROPERTY(QVariantList sourceMediaInfo READ sourceMediaInfo NOTIFY stateChanged)
    Q_PROPERTY(QString sourceAErrorKey READ sourceAErrorKey NOTIFY stateChanged)
    Q_PROPERTY(QString sourceBErrorKey READ sourceBErrorKey NOTIFY stateChanged)
    Q_PROPERTY(QString sourceCErrorKey READ sourceCErrorKey NOTIFY stateChanged)
    Q_PROPERTY(bool sourceAMissing READ sourceAMissing NOTIFY frameStateChanged)
    Q_PROPERTY(bool sourceBMissing READ sourceBMissing NOTIFY frameStateChanged)
    Q_PROPERTY(bool sourceCMissing READ sourceCMissing NOTIFY frameStateChanged)
    Q_PROPERTY(QString pairErrorKey READ pairErrorKey NOTIFY stateChanged)
    Q_PROPERTY(QString frameMappingStatus READ frameMappingStatus NOTIFY frameStateChanged)
    Q_PROPERTY(QString alignmentEstimateStatus READ alignmentEstimateStatus NOTIFY stateChanged)
    Q_PROPERTY(QString sequenceAlignmentStatus READ sequenceAlignmentStatus NOTIFY stateChanged)
    Q_PROPERTY(bool alignmentAnalysisRunning READ alignmentAnalysisRunning NOTIFY stateChanged)
    Q_PROPERTY(qreal alignmentAnalysisProgress READ alignmentAnalysisProgress NOTIFY stateChanged)
    Q_PROPERTY(QString alignmentAnalysisStatus READ alignmentAnalysisStatus NOTIFY stateChanged)
    Q_PROPERTY(QString manualAnchorStatus READ manualAnchorStatus NOTIFY stateChanged)
    Q_PROPERTY(
        QVariantList alignmentTimelineMarkers READ alignmentTimelineMarkers NOTIFY stateChanged)
    Q_PROPERTY(qulonglong alignmentTimelineMarkerOverflowCount READ
                   alignmentTimelineMarkerOverflowCount NOTIFY stateChanged)
    Q_PROPERTY(bool manualAnchorActive READ manualAnchorActive NOTIFY stateChanged)
    Q_PROPERTY(bool autoAlignmentActive READ autoAlignmentActive NOTIFY frameStateChanged)
    Q_PROPERTY(bool alignmentRequired READ alignmentRequired NOTIFY stateChanged)
    Q_PROPERTY(bool automaticAlignmentPending READ automaticAlignmentPending NOTIFY stateChanged)
    Q_PROPERTY(
        bool canConfirmAutomaticAlignment READ canConfirmAutomaticAlignment NOTIFY stateChanged)
    Q_PROPERTY(bool canUndoAutomaticAlignment READ canUndoAutomaticAlignment NOTIFY stateChanged)
    Q_PROPERTY(QVariantList compatibilityFindings READ compatibilityFindings NOTIFY stateChanged)
    Q_PROPERTY(QVariantList differenceEdges READ differenceEdges NOTIFY frameStateChanged)
    Q_PROPERTY(bool canOpen READ canOpen NOTIFY stateChanged)
    Q_PROPERTY(bool canFirst READ canFirst NOTIFY stateChanged)
    Q_PROPERTY(bool canPrevious READ canPrevious NOTIFY frameStateChanged)
    Q_PROPERTY(bool canNext READ canNext NOTIFY frameStateChanged)
    Q_PROPERTY(bool canLast READ canLast NOTIFY stateChanged)
    Q_PROPERTY(bool canPlay READ canPlay NOTIFY stateChanged)
    Q_PROPERTY(bool canPause READ canPause NOTIFY stateChanged)

public:
    enum class ReviewDisplayState {
        Empty,
        Loading,
        Ready,
        Invalid,
        Error,
    };
    Q_ENUM(ReviewDisplayState)

    // Adapter-neutral snapshot of the decoder selected for a source. The application runtime
    // publishes these from coordinator work; the QML projection must not query decoder actors.
    struct DecoderBackendState final {
        domain::SourceId sourceId = 0U;
        bool d3d11Va = false;
        std::string fallbackReason;
    };

    struct SourceFileMetadata final {
        bool exists = false;
        std::uint64_t byteSize = 0U;
        std::int64_t modifiedUtcMilliseconds = 0;
    };

    struct Dependencies final {
        using BackgroundTask = std::function<void()>;

        std::function<application::PortSubmitResult(application::PlaybackCommand)> submit;
        std::function<std::shared_ptr<const application::SessionSnapshot>()> snapshot;
        std::function<std::vector<application::CommandTerminal>()> takeCompletedCommands;
        std::function<std::vector<DecoderBackendState>()> decoderBackendStates;
        bool eventDriven = false;
        // Optional test/adapter seam. The default probe uses QFileInfo on a background worker.
        std::function<SourceFileMetadata(const QString&)> sourceFileMetadataProbe;
        std::function<void(BackgroundTask)> scheduleBackgroundTask;
    };

    explicit ReviewController(Dependencies dependencies, QObject* parent = nullptr);
    ~ReviewController() override;

    ReviewController(const ReviewController&) = delete;
    ReviewController& operator=(const ReviewController&) = delete;
    ReviewController(ReviewController&&) = delete;
    ReviewController& operator=(ReviewController&&) = delete;

    [[nodiscard]] QString sourceAFilename() const;
    [[nodiscard]] QString sourceBFilename() const;
    [[nodiscard]] QString sourceCFilename() const;
    [[nodiscard]] QVariantList sourceUrls() const;
    [[nodiscard]] QStringList sourceParentLabels() const;
    [[nodiscard]] QStringList sourceFullPaths() const;
    [[nodiscard]] QVariantList activeSources() const;
    [[nodiscard]] QAbstractItemModel* sources() const noexcept;
    [[nodiscard]] int sourceCount() const noexcept;
    [[nodiscard]] int canonicalSourceIndex() const noexcept;
    [[nodiscard]] int referenceSourceIndex() const noexcept;
    [[nodiscard]] QString playbackContinuityPolicyName() const;
    [[nodiscard]] int playbackContinuityPolicy() const noexcept;
    [[nodiscard]] qulonglong playbackSkippedFrameSets() const noexcept;
    [[nodiscard]] int effectiveDifferenceEdge() const noexcept;
    [[nodiscard]] ReviewDisplayState displayState() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool framePending() const noexcept;
    [[nodiscard]] bool playing() const noexcept;
    [[nodiscard]] qreal playbackRate() const noexcept;
    [[nodiscard]] qint64 playbackRangeIn() const noexcept;
    [[nodiscard]] qint64 playbackRangeOut() const noexcept;
    [[nodiscard]] bool playbackRangeLoop() const noexcept;
    [[nodiscard]] bool playbackRangeLoopActive() const noexcept;
    [[nodiscard]] bool graphicsReady() const noexcept;
    // Zero-based canonical frame ID. -1 means that no frame has been presented.
    [[nodiscard]] qint64 currentFrame() const noexcept;
    [[nodiscard]] qulonglong totalFrames() const noexcept;
    [[nodiscard]] int oneSecondStepFrames() const noexcept;
    [[nodiscard]] qint64 currentMediaTime() const noexcept;
    [[nodiscard]] QString currentTimecode() const;
    [[nodiscard]] QString rationalFrameRate() const;
    [[nodiscard]] QString timingMode() const;
    [[nodiscard]] bool dropFrameTimecodeAvailable() const noexcept;
    [[nodiscard]] QVariantList sourceMediaInfo() const;
    [[nodiscard]] QString sourceAErrorKey() const;
    [[nodiscard]] QString sourceBErrorKey() const;
    [[nodiscard]] QString sourceCErrorKey() const;
    [[nodiscard]] bool sourceAMissing() const noexcept;
    [[nodiscard]] bool sourceBMissing() const noexcept;
    [[nodiscard]] bool sourceCMissing() const noexcept;
    [[nodiscard]] QString pairErrorKey() const;
    [[nodiscard]] QString lastErrorTechnicalDetail() const;
    [[nodiscard]] QString frameMappingStatus() const;
    [[nodiscard]] QString alignmentEstimateStatus() const;
    [[nodiscard]] QString sequenceAlignmentStatus() const;
    [[nodiscard]] bool alignmentAnalysisRunning() const noexcept;
    [[nodiscard]] qreal alignmentAnalysisProgress() const noexcept;
    [[nodiscard]] QString alignmentAnalysisStatus() const;
    [[nodiscard]] QString manualAnchorStatus() const;
    [[nodiscard]] QVariantList alignmentTimelineMarkers() const;
    [[nodiscard]] qulonglong alignmentTimelineMarkerOverflowCount() const noexcept;
    [[nodiscard]] bool manualAnchorActive() const noexcept;
    [[nodiscard]] bool autoAlignmentActive() const noexcept;
    [[nodiscard]] bool alignmentRequired() const noexcept;
    [[nodiscard]] bool automaticAlignmentPending() const noexcept;
    [[nodiscard]] bool canConfirmAutomaticAlignment() const noexcept;
    [[nodiscard]] bool canUndoAutomaticAlignment() const noexcept;
    [[nodiscard]] QVariantList compatibilityFindings() const;
    [[nodiscard]] QVariantList differenceEdges() const;
    [[nodiscard]] bool canOpen() const noexcept;
    [[nodiscard]] bool canFirst() const noexcept;
    [[nodiscard]] bool canPrevious() const noexcept;
    [[nodiscard]] bool canNext() const noexcept;
    [[nodiscard]] bool canLast() const noexcept;
    [[nodiscard]] bool canPlay() const noexcept;
    [[nodiscard]] bool canPause() const noexcept;
    // The most recently accepted foreground session command. Shell orchestration uses this
    // identity to associate the coordinator's exact terminal with one ReviewIntent.
    [[nodiscard]] qulonglong lastSubmittedCommandId() const noexcept;

    Q_INVOKABLE bool openComparison(const QUrl& first, const QUrl& second);
    Q_INVOKABLE bool openSources(const QVariantList& urls, int referenceSourceIndex);
    // Rebuilds a ready session with a new 1-3 source topology while mapping the current
    // canonical MediaTime onto the replacement timeline. The rebuilt session stays paused.
    Q_INVOKABLE bool reopenSources(const QVariantList& urls, int referenceSourceIndex);
    // Rebuilds the active review with a new canonical/reference source. The active source list
    // remains unchanged until the rebuild succeeds.
    Q_INVOKABLE bool changeReference(int sourceIndex);
    // Opens two required sources plus one optional third source. The reference index is the
    // zero-based canonical source. General 1-3 source opening uses openSources().
    Q_INVOKABLE bool openComparisonSet(const QUrl& first,
                                       const QUrl& second,
                                       const QUrl& third,
                                       int referenceSourceIndex);
    Q_INVOKABLE bool closeSources();
    Q_INVOKABLE void clearCandidateSourceErrors() noexcept;
    Q_INVOKABLE QVariantMap handleDroppedUrls(const QVariantList& urls) const;
    // Whether the url refers to an existing local directory. Folder drops are routed to
    // the image folder comparison before the file-only drop validation runs.
    Q_INVOKABLE bool isFolderPath(const QUrl& url) const;
    Q_INVOKABLE bool first();
    Q_INVOKABLE bool previous();
    Q_INVOKABLE bool next();
    Q_INVOKABLE bool last();
    Q_INVOKABLE bool stepFrames(qint64 delta);
    Q_INVOKABLE bool seekFrame(qint64 frame);
    Q_INVOKABLE QString timecodeForFrame(qint64 frame, bool dropFrame = false) const;
    Q_INVOKABLE qint64 mediaTimeForFrame(qint64 frame) const;
    Q_INVOKABLE qint64 frameForMediaTime(qint64 microseconds) const;
    Q_INVOKABLE bool
    applyAlignmentOffsets(qint64 sourceAFrames, qint64 sourceBFrames, qint64 sourceCFrames);
    Q_INVOKABLE bool applySourceOffsets(const QVariantList& offsets);
    Q_INVOKABLE bool estimateAlignment();
    Q_INVOKABLE bool analyzeSequenceAlignment();
    Q_INVOKABLE bool cancelAlignmentAnalysis();
    Q_INVOKABLE bool confirmAutomaticAlignment();
    Q_INVOKABLE bool undoAutomaticAlignment();
    Q_INVOKABLE bool
    setManualAlignmentAnchor(int sourceIndex, qint64 canonicalFrame, qint64 sourceFrame);
    Q_INVOKABLE bool clearManualAlignmentAnchors();
    Q_INVOKABLE bool play();
    Q_INVOKABLE bool pause();
    // Selects the visual playback rate. Accepted while playing (re-anchors the cadence) and
    // while paused (applied by the next play()).
    Q_INVOKABLE bool setPlaybackRate(qreal rate);
    Q_INVOKABLE bool togglePlayback();
    // Kernel playback-range authority. in/out < 0 clears the range. loop only applies with a
    // valid closed interval.
    Q_INVOKABLE bool setPlaybackRange(qint64 inFrame, qint64 outFrame, bool loop);
    Q_INVOKABLE bool playRange(qint64 inFrame, qint64 outFrame);
    Q_INVOKABLE bool stopRangeLoop();
    // C-07: domain::PlaybackContinuityPolicy code (0 ReviewEveryFrame, 1 RealTime, 2 Contextual).
    Q_INVOKABLE bool setPlaybackContinuityPolicy(int policyCode);
    // C-02: project a DifferenceEdge preference (0/1/2) onto a session ComparisonPair using
    // the live differenceEdges list, then submit SetActiveComparisonPairCommand.
    Q_INVOKABLE bool applyComparisonPairFromEdge(int preferenceValue, int pairPolicyCode = 2);
    Q_INVOKABLE void refreshProjection() noexcept;
    // Returns the snapshot-frozen identity for a source URL. The value is rebuilt only when the
    // validated comparison pointer changes, so callers see a stable string between commits.
    // Falls back to the live filesystem identity when no cached entry exists.
    Q_INVOKABLE QString frozenSourceIdentity(const QUrl& source) const;

    // Stops timer/backend access and makes every command fail closed. Calls from another thread
    // are queued to the controller's GUI thread; the runtime calls this before closing ingress.
    Q_INVOKABLE void stop() noexcept;

    // Shutdown-only ownership fence. Call after stop(); no new probes can then start. A timeout
    // leaves the probe running in the background and requires the executable host to terminate
    // without normal static destruction.
    [[nodiscard]] bool waitForSourceDiskStatusIdle(std::chrono::milliseconds timeout) noexcept;

Q_SIGNALS:
    void stateChanged();
    void frameStateChanged();
    void foregroundCommandFinished(qulonglong commandId, int outcome, const QString& errorKey);

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace dvs::ui
