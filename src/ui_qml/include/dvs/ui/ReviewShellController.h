#pragma once

#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <deque>
#include <optional>

namespace dvs::ui {

class ReviewController;
class ReviewPreferencesController;

// Owns review-shell state that must remain coherent across QML dialogs and external startup
// requests. Media truth stays in ReviewController; this object separates active backend sources
// from staged user choices and serializes startup ingress on the GUI thread.
class ReviewShellController final : public QObject {
    Q_OBJECT

    Q_PROPERTY(QVariantList activeSources READ activeSources NOTIFY stateChanged)
    Q_PROPERTY(QVariantList stagedSources READ stagedSources NOTIFY stateChanged)
    Q_PROPERTY(bool sidebarAppendStaged READ sidebarAppendStaged NOTIFY stateChanged)
    Q_PROPERTY(int canonicalSourceIndex READ canonicalSourceIndex NOTIFY stateChanged)
    Q_PROPERTY(qulonglong activeGeneration READ activeGeneration NOTIFY stateChanged)
    Q_PROPERTY(int effectiveViewMode READ effectiveViewMode NOTIFY stateChanged)
    Q_PROPERTY(int effectiveDifferenceEdge READ effectiveDifferenceEdge NOTIFY stateChanged)
    Q_PROPERTY(int stagedReferenceIndex READ stagedReferenceIndex WRITE setStagedReferenceIndex
                   NOTIFY stateChanged)
    Q_PROPERTY(int queuedIntentCount READ queuedIntentCount NOTIFY stateChanged)
    Q_PROPERTY(QVariantList queuedIntents READ queuedIntents NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap activeIntent READ activeIntent NOTIFY stateChanged)
    Q_PROPERTY(QStringList activeSourceIdentities READ activeSourceIdentities NOTIFY stateChanged)
    Q_PROPERTY(QString canonicalSourceIdentity READ canonicalSourceIdentity NOTIFY stateChanged)
    Q_PROPERTY(int referenceSourceIndex READ referenceSourceIndex NOTIFY stateChanged)
    Q_PROPERTY(QString referenceSourceIdentity READ referenceSourceIdentity NOTIFY stateChanged)
    Q_PROPERTY(QStringList pendingSourceIdentities READ pendingSourceIdentities NOTIFY stateChanged)
    Q_PROPERTY(bool chromeVisible READ chromeVisible WRITE setChromeVisible NOTIFY stateChanged)
    Q_PROPERTY(
        bool inspectorVisible READ inspectorVisible WRITE setInspectorVisible NOTIFY stateChanged)
    Q_PROPERTY(bool hasPendingAction READ hasPendingAction NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap pendingAction READ pendingAction NOTIFY stateChanged)
    Q_PROPERTY(int openIntent READ openIntent NOTIFY stateChanged)
    Q_PROPERTY(qint64 inFrame READ inFrame NOTIFY stateChanged)
    Q_PROPERTY(qint64 outFrame READ outFrame NOTIFY stateChanged)
    Q_PROPERTY(double inMediaTime READ inMediaTime NOTIFY stateChanged)
    Q_PROPERTY(double outMediaTime READ outMediaTime NOTIFY stateChanged)
    Q_PROPERTY(bool rangePlaybackActive READ rangePlaybackActive NOTIFY stateChanged)
    Q_PROPERTY(bool rangeStartPending READ rangeStartPending NOTIFY stateChanged)
    // C2. Display order and selection are a view concern kept here, in the shell, precisely so that
    // they cannot reach media truth. Both are keyed by frozen source identity, never by index, so
    // that rearranging the list can never change a source's role, the pairing, or the reference.
    Q_PROPERTY(QStringList displaySourceIdentities READ displaySourceIdentities NOTIFY stateChanged)
    Q_PROPERTY(
        QStringList selectedSourceIdentities READ selectedSourceIdentities NOTIFY stateChanged)
    // V02 (P05): full-canvas direct-look focus - which participating source the whole canvas
    // shows. Same C2 rule as display order: presentation state keyed by frozen source identity,
    // kept in the shell so switching it cannot reach media truth. Switching submits no playback
    // command (no seek, no re-pairing), never touches the reference role or the comparison pair,
    // and clears when the focused source leaves the active set. The slot projection addresses
    // the session order (A/B/C), matching the mode bar's vocabulary.
    Q_PROPERTY(QString canvasFocusIdentity READ canvasFocusIdentity NOTIFY canvasFocusChanged)
    Q_PROPERTY(int canvasFocusSlot READ canvasFocusSlot NOTIFY canvasFocusChanged)

public:
    enum OpenIntent {
        NewReview = 0,
        ReplaceSources = 1,
        ChangeReference = 2,
    };
    Q_ENUM(OpenIntent)

    enum ReviewIntentKind {
        OpenSourcesIntent = 0,
        ReplaceSourcesIntent = 1,
        AddSourcesIntent = 2,
        RemoveSourceIntent = 3,
        ChangeReferenceIntent = 4,
        CloseSourcesIntent = 5,
    };
    Q_ENUM(ReviewIntentKind)

    enum ReviewIntentOrigin {
        UserInterfaceOrigin = 0,
        StartupOrigin = 1,
    };
    Q_ENUM(ReviewIntentOrigin)

    // I01: why the sources are being opened, carried per intent instead of a global flag so a
    // startup playback open, a review open, an append and a replace can never overwrite each
    // other's purpose. Only OpenSourcesIntent consumes it: appends and replaces deliberately
    // never start playback. OpenForPlayback = "act like a player" (a double-clicked file that
    // should play once its first frame is confirmed); OpenForReview = the app's compare open,
    // which stays paused so the start point can be checked.
    enum OpenPurpose {
        OpenForReview = 0,
        OpenForPlayback = 1,
    };
    Q_ENUM(OpenPurpose)

    // V02 (P05): request codes for the full-canvas direct-look switch. Values mirror
    // domain::CanvasFocusRequest exactly; the contract tests cross-check the two so a drift on
    // either side fails a build instead of silently re-targeting the canvas.
    enum CanvasFocusRequest {
        CanvasFocusOff = 0,
        CanvasFocusReference = 1,
        CanvasFocusSourceSlot = 2,
    };
    Q_ENUM(CanvasFocusRequest)

    enum ReviewIntentStatus {
        QueuedStatus = 0,
        RunningStatus = 1,
        SucceededStatus = 2,
        FailedStatus = 3,
        CanceledStatus = 4,
        RejectedStatus = 5,
        ReplacedStatus = 6,
    };
    Q_ENUM(ReviewIntentStatus)

    enum ReviewIntentError {
        NoIntentError = 0,
        InvalidIntentError = 1,
        QueueFullError = 2,
        StaleTopologyError = 3,
        SubmissionRejectedError = 4,
        CommandFailedError = 5,
    };
    Q_ENUM(ReviewIntentError)

    explicit ReviewShellController(ReviewController& review, QObject* parent = nullptr);
    ReviewShellController(ReviewController& review,
                          ReviewPreferencesController& preferences,
                          QObject* parent = nullptr);

    [[nodiscard]] QVariantList activeSources() const;
    [[nodiscard]] QVariantList stagedSources() const;
    [[nodiscard]] bool sidebarAppendStaged() const noexcept;
    [[nodiscard]] int canonicalSourceIndex() const noexcept;
    [[nodiscard]] qulonglong activeGeneration() const noexcept;
    [[nodiscard]] int effectiveViewMode() const noexcept;
    [[nodiscard]] int effectiveDifferenceEdge() const noexcept;
    [[nodiscard]] int stagedReferenceIndex() const noexcept;
    [[nodiscard]] int queuedIntentCount() const noexcept;
    [[nodiscard]] QVariantList queuedIntents() const;
    [[nodiscard]] QVariantMap activeIntent() const;
    [[nodiscard]] QStringList activeSourceIdentities() const;
    [[nodiscard]] QString canonicalSourceIdentity() const;
    [[nodiscard]] int referenceSourceIndex() const noexcept;
    [[nodiscard]] QString referenceSourceIdentity() const;
    [[nodiscard]] QStringList pendingSourceIdentities() const;
    [[nodiscard]] bool chromeVisible() const noexcept;
    [[nodiscard]] bool inspectorVisible() const noexcept;
    [[nodiscard]] bool hasPendingAction() const noexcept;
    [[nodiscard]] QVariantMap pendingAction() const;
    [[nodiscard]] int openIntent() const noexcept;
    [[nodiscard]] qint64 inFrame() const noexcept;
    [[nodiscard]] qint64 outFrame() const noexcept;
    [[nodiscard]] double inMediaTime() const noexcept;
    [[nodiscard]] double outMediaTime() const noexcept;
    [[nodiscard]] bool rangePlaybackActive() const noexcept;
    [[nodiscard]] bool rangeStartPending() const noexcept;
    [[nodiscard]] QStringList displaySourceIdentities() const;
    [[nodiscard]] QStringList selectedSourceIdentities() const;
    // V02 (P05): the frozen identity of the source the full canvas is focused on, or an empty
    // string when the canvas shows the regular comparison view.
    [[nodiscard]] QString canvasFocusIdentity() const;
    // Session-order slot (A/B/C) of the focused source, or -1 when off. Display convenience for
    // the mode bar's checked states; the identity above is the durable state.
    [[nodiscard]] int canvasFocusSlot() const;

    void setStagedReferenceIndex(int sourceIndex);
    void setChromeVisible(bool visible);
    void setInspectorVisible(bool visible);

    Q_INVOKABLE bool stageSources(const QVariantList& sources, int referenceIndex);
    Q_INVOKABLE void clearStagedSources();
    // A sidebar append may never become a replacement or rebase onto a different review.
    Q_INVOKABLE QString sidebarAppendError(const QUrl& source) const;
    Q_INVOKABLE bool stageSidebarAppend(const QUrl& source);
    Q_INVOKABLE bool openSidebarAppend(int referenceIndex);
    Q_INVOKABLE bool moveStagedSource(int fromIndex, int toIndex);
    Q_INVOKABLE bool openStagedSources(bool preserveDisplayedTime);
    // One browser activation, serialized with all other source intents. The returned identity
    // is echoed by intentFinished; submitting it neither changes selection nor starts playback.
    Q_INVOKABLE qulonglong openVideo(const QUrl& source);
    Q_INVOKABLE bool removeActiveSource(int sourceIndex);
    Q_INVOKABLE bool removeActiveSourceByIdentity(const QString& sourceIdentity);
    Q_INVOKABLE bool changeReferenceByIdentity(const QString& sourceIdentity);
    Q_INVOKABLE bool closeSources();
    Q_INVOKABLE bool cancelQueuedIntent(qulonglong intentId);
    Q_INVOKABLE void cancelAllQueuedIntents();
    Q_INVOKABLE bool beginPendingAction(const QVariantMap& action);
    Q_INVOKABLE QVariantMap takePendingAction();
    Q_INVOKABLE void cancelPendingAction();
    Q_INVOKABLE bool enqueueStartupRequest(int kind, const QVariantList& files);
    Q_INVOKABLE bool setRangeIn(qint64 frame, double mediaTime);
    Q_INVOKABLE bool setRangeOut(qint64 frame, double mediaTime);
    Q_INVOKABLE void remapRange(qint64 inFrame, qint64 outFrame);
    Q_INVOKABLE void clearRange();
    Q_INVOKABLE bool setRangePlaybackState(bool active, bool startPending);
    Q_INVOKABLE void setRangeStartPending(bool pending);
    // Reorders the list as the user sees it and nothing else. Both indices address the display
    // order published by displaySourceIdentities. The underlying active set, its order, the
    // reference and the pairing are deliberately left alone - reordering those is a different
    // decision that the product has not made. Returns false and changes nothing when an index is
    // out of range.
    Q_INVOKABLE bool moveSourceInDisplayOrder(int fromIndex, int toIndex);
    Q_INVOKABLE void setSourceSelected(const QString& sourceIdentity, bool selected);
    Q_INVOKABLE void toggleSourceSelection(const QString& sourceIdentity);
    Q_INVOKABLE void clearSourceSelection();
    // Removes every selected source by identity, newest selection last, and reports how many were
    // actually removed. Identities that left the active set in the meantime are skipped rather than
    // failing the whole batch.
    Q_INVOKABLE int removeSelectedSources();
    // V02 (P05): switches which participating source the full canvas shows, at the frame the
    // session is already presenting. requestCode is a CanvasFocusRequest value; sourceSlot is
    // the session slot for CanvasFocusSourceSlot. The switch is presentation state only: it
    // submits no playback command (no seek, no locate, no re-pairing), leaves the reference
    // role and the active comparison pair untouched, and every participating source keeps
    // being maintained by the same frame set. Returns false and changes nothing when the
    // request cannot be resolved (for example "GT" in a session without a reference).
    Q_INVOKABLE bool requestCanvasFocus(int requestCode, int sourceSlot = 0);

Q_SIGNALS:
    void stateChanged();
    void canvasFocusChanged();
    void intentEvent(qulonglong intentId, int status, int kind, int error, int sourceCount);
    void intentFinished(qulonglong intentId, int kind, int outcome, const QString& errorKey);
    // I01: fired after a successful open whose purpose was OpenForPlayback and whose autoplay
    // was not disabled by preference. `started` is the transport's answer; false means the open
    // succeeded but playback could not start, which the UI must not swallow silently.
    void openPlaybackRequested(qulonglong intentId, bool started);

private:
    struct EffectiveComparisonState final {
        int viewMode = 6;
        int differenceEdge = 0;
    };

    struct SidebarAppendSnapshot final {
        qulonglong generation = 0U;
        qulonglong completedIntentRevision = 0U;
        QStringList activeIdentities;
        QString referenceIdentity;
        QUrl candidate;
        QString candidateIdentity;
    };

    struct ReviewIntent final {
        std::uint64_t id = 0U;
        ReviewIntentKind kind = OpenSourcesIntent;
        ReviewIntentOrigin origin = UserInterfaceOrigin;
        OpenPurpose purpose = OpenForReview;
        QVariantList sources;
        int referenceIndex = 0;
        QString referenceIdentity;
        QString targetIdentity;
        qulonglong expectedGeneration = 0U;
        QStringList expectedSources;
        std::uint64_t commandId = 0U;
        std::optional<SidebarAppendSnapshot> sidebarAppend;
    };

    [[nodiscard]] bool sidebarAppendCurrent(const SidebarAppendSnapshot& snapshot) const;
    void synchronizeActiveSources(bool advanceGeneration = false);
    [[nodiscard]] bool submitOrQueue(ReviewIntent intent, qulonglong* acceptedId = nullptr);
    [[nodiscard]] bool submitIntent(ReviewIntent& intent);
    [[nodiscard]] bool enqueueIntent(ReviewIntent intent);
    void drainIntentQueue();
    void finishIntent(int outcome, const QString& errorKey);
    [[nodiscard]] bool rebaseIntent(ReviewIntent& intent) const;
    [[nodiscard]] QVariantMap intentMap(const ReviewIntent& intent,
                                        ReviewIntentStatus status) const;
    [[nodiscard]] bool
    queueRemoveActiveSource(const QString& sourceIdentity, int sourceIndex, bool mergePending);
    [[nodiscard]] bool hasPendingSourceIntent(ReviewIntentKind kind,
                                              const QString& sourceIdentity) const;
    [[nodiscard]] std::uint64_t allocateIntentId() noexcept;
    [[nodiscard]] EffectiveComparisonState effectiveComparisonState() const noexcept;

    ReviewController& review_;
    ReviewPreferencesController* preferences_ = nullptr;
    QVariantList activeSources_;
    QStringList frozenActiveIdentities_;
    QVariantList stagedSources_;
    std::optional<SidebarAppendSnapshot> stagedSidebarAppend_;
    int canonicalSourceIndex_ = -1;
    int referenceSourceIndex_ = -1;
    qulonglong activeGeneration_ = 0U;
    qulonglong completedIntentRevision_ = 0U;
    int stagedReferenceIndex_ = 0;
    std::deque<ReviewIntent> reviewIntents_;
    std::optional<ReviewIntent> activeIntent_;
    std::uint64_t nextIntentId_ = 1U;
    bool intentIdsExhausted_ = false;
    bool chromeVisible_ = true;
    bool inspectorVisible_ = false;
    QVariantMap pendingAction_;
    int openIntent_ = NewReview;
    qint64 inFrame_ = -1;
    qint64 outFrame_ = -1;
    double inMediaTime_ = -1.0;
    double outMediaTime_ = -1.0;
    bool rangePlaybackActive_ = false;
    bool rangeStartPending_ = false;
    QStringList displaySourceIdentities_;
    QStringList selectedSourceIdentities_;
    // V02 (P05): frozen identity of the full-canvas focus target; empty when off. Cleared by
    // synchronizeActiveSources() once the focused source leaves the active set - never remapped
    // onto whatever source took its slot.
    QString canvasFocusIdentity_;
};

} // namespace dvs::ui
