#pragma once

#include <QObject>
#include <QStringList>
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

    void setStagedReferenceIndex(int sourceIndex);
    void setChromeVisible(bool visible);
    void setInspectorVisible(bool visible);

    Q_INVOKABLE bool stageSources(const QVariantList& sources, int referenceIndex);
    Q_INVOKABLE void clearStagedSources();
    Q_INVOKABLE bool moveStagedSource(int fromIndex, int toIndex);
    Q_INVOKABLE bool openStagedSources(bool preserveDisplayedTime);
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

Q_SIGNALS:
    void stateChanged();
    void intentEvent(qulonglong intentId, int status, int kind, int error, int sourceCount);
    void intentFinished(qulonglong intentId, int kind, int outcome, const QString& errorKey);

private:
    struct EffectiveComparisonState final {
        int viewMode = 6;
        int differenceEdge = 0;
    };

    struct ReviewIntent final {
        std::uint64_t id = 0U;
        ReviewIntentKind kind = OpenSourcesIntent;
        ReviewIntentOrigin origin = UserInterfaceOrigin;
        QVariantList sources;
        int referenceIndex = 0;
        QString referenceIdentity;
        QString targetIdentity;
        qulonglong expectedGeneration = 0U;
        QStringList expectedSources;
        std::uint64_t commandId = 0U;
    };

    void synchronizeActiveSources(bool advanceGeneration = false);
    [[nodiscard]] bool submitOrQueue(ReviewIntent intent);
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
    int canonicalSourceIndex_ = -1;
    qulonglong activeGeneration_ = 0U;
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
};

} // namespace dvs::ui
