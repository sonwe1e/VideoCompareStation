#include "dvs/ui/ReviewShellController.h"

#include "dvs/presentation/ComparisonContract.h"
#include "dvs/ui/ReviewController.h"
#include "dvs/ui/ReviewPreferencesController.h"

#include <QUrl>

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

namespace dvs::ui {
namespace {

constexpr std::size_t kMaximumQueuedIntents = 8U;

} // namespace

ReviewShellController::ReviewShellController(ReviewController& review, QObject* const parent)
    : QObject(parent), review_(review) {
    QObject::connect(&review_, &ReviewController::stateChanged, this, [this] {
        if (!activeIntent_.has_value()) {
            synchronizeActiveSources();
        }
        if (!review_.busy() && !activeIntent_.has_value() && !reviewIntents_.empty()) {
            QMetaObject::invokeMethod(this, [this] { drainIntentQueue(); }, Qt::QueuedConnection);
        }
        // Effective comparison mode also depends on the controller's committed source count.
        // That count can change while the Active Sources identity list remains stable, so forward
        // every controller projection to QML instead of relying only on list synchronization.
        Q_EMIT stateChanged();
    });
    QObject::connect(
        &review_,
        &ReviewController::foregroundCommandFinished,
        this,
        [this](const qulonglong commandId, const int outcome, const QString& errorKey) {
            if (!activeIntent_.has_value() || activeIntent_->commandId != commandId) {
                return;
            }
            finishIntent(outcome, errorKey);
        });
    synchronizeActiveSources();
}

ReviewShellController::ReviewShellController(ReviewController& review,
                                             ReviewPreferencesController& preferences,
                                             QObject* const parent)
    : ReviewShellController(review, parent) {
    preferences_ = &preferences;
    QObject::connect(preferences_, &ReviewPreferencesController::preferencesChanged, this, [this] {
        Q_EMIT stateChanged();
    });
    Q_EMIT stateChanged();
}

QVariantList ReviewShellController::activeSources() const {
    return activeSources_;
}

QVariantList ReviewShellController::stagedSources() const {
    return stagedSources_;
}

int ReviewShellController::canonicalSourceIndex() const noexcept {
    return canonicalSourceIndex_;
}

qulonglong ReviewShellController::activeGeneration() const noexcept {
    return activeGeneration_;
}

int ReviewShellController::effectiveViewMode() const noexcept {
    return effectiveComparisonState().viewMode;
}

int ReviewShellController::effectiveDifferenceEdge() const noexcept {
    return effectiveComparisonState().differenceEdge;
}

int ReviewShellController::stagedReferenceIndex() const noexcept {
    return stagedReferenceIndex_;
}

int ReviewShellController::queuedIntentCount() const noexcept {
    return static_cast<int>(reviewIntents_.size());
}

QVariantList ReviewShellController::queuedIntents() const {
    QVariantList result;
    result.reserve(static_cast<qsizetype>(reviewIntents_.size()));
    for (const ReviewIntent& intent : reviewIntents_) {
        result.push_back(intentMap(intent, QueuedStatus));
    }
    return result;
}

QVariantMap ReviewShellController::activeIntent() const {
    return activeIntent_.has_value() ? intentMap(*activeIntent_, RunningStatus) : QVariantMap{};
}

QString ReviewShellController::canonicalSourceIdentity() const {
    const QStringList identities = activeSourceIdentities();
    if (canonicalSourceIndex_ < 0 || canonicalSourceIndex_ >= identities.size()) {
        return {};
    }
    return identities[canonicalSourceIndex_];
}

QStringList ReviewShellController::pendingSourceIdentities() const {
    QStringList result;
    const auto appendIntent = [&result](const ReviewIntent& intent) {
        QString identity;
        switch (intent.kind) {
        case RemoveSourceIntent:
            identity = intent.targetIdentity;
            break;
        case ChangeReferenceIntent:
            identity = intent.referenceIdentity;
            break;
        case OpenSourcesIntent:
        case ReplaceSourcesIntent:
        case AddSourcesIntent:
        case CloseSourcesIntent:
            return;
        }
        if (!identity.isEmpty() && !result.contains(identity)) {
            result.push_back(identity);
        }
    };
    if (activeIntent_.has_value()) {
        appendIntent(*activeIntent_);
    }
    for (const ReviewIntent& intent : reviewIntents_) {
        appendIntent(intent);
    }
    return result;
}

bool ReviewShellController::chromeVisible() const noexcept {
    return chromeVisible_;
}

bool ReviewShellController::inspectorVisible() const noexcept {
    return inspectorVisible_;
}

bool ReviewShellController::hasPendingAction() const noexcept {
    return !pendingAction_.isEmpty();
}

QVariantMap ReviewShellController::pendingAction() const {
    return pendingAction_;
}

int ReviewShellController::openIntent() const noexcept {
    return openIntent_;
}

qint64 ReviewShellController::inFrame() const noexcept {
    return inFrame_;
}

qint64 ReviewShellController::outFrame() const noexcept {
    return outFrame_;
}

double ReviewShellController::inMediaTime() const noexcept {
    return inMediaTime_;
}

double ReviewShellController::outMediaTime() const noexcept {
    return outMediaTime_;
}

bool ReviewShellController::rangePlaybackActive() const noexcept {
    return rangePlaybackActive_;
}

bool ReviewShellController::rangeStartPending() const noexcept {
    return rangeStartPending_;
}

void ReviewShellController::setStagedReferenceIndex(const int sourceIndex) {
    if (sourceIndex < 0 || sourceIndex >= stagedSources_.size() ||
        stagedReferenceIndex_ == sourceIndex) {
        return;
    }
    stagedReferenceIndex_ = sourceIndex;
    Q_EMIT stateChanged();
}

void ReviewShellController::setChromeVisible(const bool visible) {
    if (chromeVisible_ == visible) {
        return;
    }
    chromeVisible_ = visible;
    if (!chromeVisible_) {
        inspectorVisible_ = false;
    }
    Q_EMIT stateChanged();
}

void ReviewShellController::setInspectorVisible(const bool visible) {
    const bool next = chromeVisible_ && visible;
    if (inspectorVisible_ == next) {
        return;
    }
    inspectorVisible_ = next;
    Q_EMIT stateChanged();
}

bool ReviewShellController::stageSources(const QVariantList& sources, const int referenceIndex) {
    if (sources.isEmpty() || sources.size() > 3 || referenceIndex < 0 ||
        referenceIndex >= sources.size()) {
        return false;
    }
    stagedSources_ = sources;
    stagedReferenceIndex_ = referenceIndex;
    Q_EMIT stateChanged();
    return true;
}

void ReviewShellController::clearStagedSources() {
    if (stagedSources_.isEmpty() && stagedReferenceIndex_ == 0) {
        return;
    }
    stagedSources_.clear();
    stagedReferenceIndex_ = 0;
    Q_EMIT stateChanged();
}

bool ReviewShellController::moveStagedSource(const int fromIndex, const int toIndex) {
    if (fromIndex < 0 || toIndex < 0 || fromIndex >= stagedSources_.size() ||
        toIndex >= stagedSources_.size() || fromIndex == toIndex) {
        return false;
    }
    stagedSources_.move(fromIndex, toIndex);
    if (stagedReferenceIndex_ == fromIndex) {
        stagedReferenceIndex_ = toIndex;
    } else if (fromIndex < stagedReferenceIndex_ && toIndex >= stagedReferenceIndex_) {
        --stagedReferenceIndex_;
    } else if (fromIndex > stagedReferenceIndex_ && toIndex <= stagedReferenceIndex_) {
        ++stagedReferenceIndex_;
    }
    Q_EMIT stateChanged();
    return true;
}

bool ReviewShellController::openStagedSources(const bool preserveDisplayedTime) {
    if (stagedSources_.isEmpty()) {
        return false;
    }
    openIntent_ = preserveDisplayedTime ? ReplaceSources : NewReview;
    Q_EMIT stateChanged();
    ReviewIntentKind kind = preserveDisplayedTime ? ReplaceSourcesIntent : OpenSourcesIntent;
    if (preserveDisplayedTime && stagedSources_.size() > activeSources_.size()) {
        kind = AddSourcesIntent;
    }
    return submitOrQueue(ReviewIntent{
        .kind = kind,
        .sources = stagedSources_,
        .referenceIndex = stagedReferenceIndex_,
        .referenceIdentity =
            review_.frozenSourceIdentity(stagedSources_[stagedReferenceIndex_].toUrl()),
    });
}

bool ReviewShellController::removeActiveSource(const int sourceIndex) {
    if (sourceIndex < 0 || sourceIndex >= activeSources_.size() || activeSources_.size() <= 1) {
        return false;
    }
    // Preserve the legacy index API's queueing contract. The user-facing QML path uses the
    // identity API below and merges repeat clicks before rebuilding a source topology.
    return queueRemoveActiveSource(activeSourceIdentities()[sourceIndex], sourceIndex, false);
}

bool ReviewShellController::removeActiveSourceByIdentity(const QString& sourceIdentity) {
    const QStringList identities = activeSourceIdentities();
    const int sourceIndex = identities.indexOf(sourceIdentity);
    if (sourceIdentity.isEmpty() || sourceIndex < 0 || activeSources_.size() <= 1) {
        return false;
    }
    return queueRemoveActiveSource(sourceIdentity, sourceIndex, true);
}

bool ReviewShellController::queueRemoveActiveSource(const QString& sourceIdentity,
                                                    const int sourceIndex,
                                                    const bool mergePending) {
    if (mergePending && hasPendingSourceIntent(RemoveSourceIntent, sourceIdentity)) {
        return true;
    }
    QVariantList replacement = activeSources_;
    replacement.removeAt(sourceIndex);
    int reference = canonicalSourceIndex_;
    if (reference == sourceIndex) {
        reference = 0;
    } else if (reference > sourceIndex) {
        --reference;
    }
    if (!stageSources(replacement, std::max(0, reference))) {
        return false;
    }
    openIntent_ = ReplaceSources;
    Q_EMIT stateChanged();
    return submitOrQueue(ReviewIntent{
        .kind = RemoveSourceIntent,
        .sources = std::move(replacement),
        .referenceIndex = stagedReferenceIndex_,
        .referenceIdentity =
            review_.frozenSourceIdentity(stagedSources_[stagedReferenceIndex_].toUrl()),
        .targetIdentity = sourceIdentity,
    });
}

bool ReviewShellController::changeReferenceByIdentity(const QString& sourceIdentity) {
    const QStringList identities = activeSourceIdentities();
    const int sourceIndex = identities.indexOf(sourceIdentity);
    if (sourceIdentity.isEmpty() || sourceIndex < 0 || sourceIndex == canonicalSourceIndex_) {
        return false;
    }
    if (hasPendingSourceIntent(ChangeReferenceIntent, sourceIdentity)) {
        return true;
    }
    openIntent_ = ChangeReference;
    Q_EMIT stateChanged();
    return submitOrQueue(ReviewIntent{
        .kind = ChangeReferenceIntent,
        .referenceIndex = sourceIndex,
        .referenceIdentity = sourceIdentity,
    });
}

bool ReviewShellController::closeSources() {
    return submitOrQueue(ReviewIntent{.kind = CloseSourcesIntent});
}

bool ReviewShellController::cancelQueuedIntent(const qulonglong intentId) {
    const auto found =
        std::find_if(reviewIntents_.begin(),
                     reviewIntents_.end(),
                     [intentId](const ReviewIntent& intent) { return intent.id == intentId; });
    if (found == reviewIntents_.end()) {
        return false;
    }
    const ReviewIntent canceled = *found;
    reviewIntents_.erase(found);
    Q_EMIT stateChanged();
    Q_EMIT intentEvent(
        canceled.id, CanceledStatus, canceled.kind, NoIntentError, canceled.sources.size());
    return true;
}

void ReviewShellController::cancelAllQueuedIntents() {
    std::deque<ReviewIntent> canceled = std::move(reviewIntents_);
    reviewIntents_.clear();
    if (!canceled.empty()) {
        Q_EMIT stateChanged();
    }
    for (const ReviewIntent& intent : canceled) {
        Q_EMIT intentEvent(
            intent.id, CanceledStatus, intent.kind, NoIntentError, intent.sources.size());
    }
}

bool ReviewShellController::beginPendingAction(const QVariantMap& action) {
    if (action.isEmpty() || !pendingAction_.isEmpty()) {
        return false;
    }
    pendingAction_ = action;
    Q_EMIT stateChanged();
    return true;
}

QVariantMap ReviewShellController::takePendingAction() {
    if (pendingAction_.isEmpty()) {
        return {};
    }
    QVariantMap action = std::move(pendingAction_);
    pendingAction_.clear();
    Q_EMIT stateChanged();
    return action;
}

void ReviewShellController::cancelPendingAction() {
    if (pendingAction_.isEmpty()) {
        return;
    }
    pendingAction_.clear();
    Q_EMIT stateChanged();
}

bool ReviewShellController::enqueueStartupRequest(const int kind, const QVariantList& files) {
    if (kind <= 0 || files.isEmpty() || files.size() > 3) {
        return false;
    }
    return submitOrQueue(ReviewIntent{
        .kind = OpenSourcesIntent,
        .origin = StartupOrigin,
        .sources = files,
        .referenceIndex = 0,
        .referenceIdentity = review_.frozenSourceIdentity(files.front().toUrl()),
    });
}

bool ReviewShellController::setRangeIn(const qint64 frame, const double mediaTime) {
    if (frame < 0) {
        return false;
    }
    inFrame_ = frame;
    inMediaTime_ = mediaTime;
    if (outFrame_ >= 0 && outFrame_ < inFrame_) {
        outFrame_ = -1;
        outMediaTime_ = -1.0;
        rangePlaybackActive_ = false;
        rangeStartPending_ = false;
    }
    Q_EMIT stateChanged();
    return true;
}

bool ReviewShellController::setRangeOut(const qint64 frame, const double mediaTime) {
    if (frame < 0) {
        return false;
    }
    outFrame_ = frame;
    outMediaTime_ = mediaTime;
    if (inFrame_ >= 0 && outFrame_ < inFrame_) {
        inFrame_ = -1;
        inMediaTime_ = -1.0;
        rangePlaybackActive_ = false;
        rangeStartPending_ = false;
    }
    Q_EMIT stateChanged();
    return true;
}

void ReviewShellController::remapRange(const qint64 inFrame, const qint64 outFrame) {
    const qint64 nextIn = inMediaTime_ >= 0.0 ? inFrame : -1;
    const qint64 nextOut = outMediaTime_ >= 0.0 ? outFrame : -1;
    if (inFrame_ == nextIn && outFrame_ == nextOut) {
        return;
    }
    inFrame_ = nextIn;
    outFrame_ = nextOut;
    if (inFrame_ < 0 || outFrame_ < inFrame_) {
        rangePlaybackActive_ = false;
        rangeStartPending_ = false;
    }
    Q_EMIT stateChanged();
}

void ReviewShellController::clearRange() {
    if (inFrame_ < 0 && outFrame_ < 0 && inMediaTime_ < 0.0 && outMediaTime_ < 0.0 &&
        !rangePlaybackActive_ && !rangeStartPending_) {
        return;
    }
    inFrame_ = -1;
    outFrame_ = -1;
    inMediaTime_ = -1.0;
    outMediaTime_ = -1.0;
    rangePlaybackActive_ = false;
    rangeStartPending_ = false;
    Q_EMIT stateChanged();
}

bool ReviewShellController::setRangePlaybackState(const bool active, const bool startPending) {
    if (active && (inFrame_ < 0 || outFrame_ < inFrame_)) {
        return false;
    }
    const bool nextPending = active && startPending;
    if (rangePlaybackActive_ == active && rangeStartPending_ == nextPending) {
        return true;
    }
    rangePlaybackActive_ = active;
    rangeStartPending_ = nextPending;
    Q_EMIT stateChanged();
    return true;
}

void ReviewShellController::setRangeStartPending(const bool pending) {
    const bool next = rangePlaybackActive_ && pending;
    if (rangeStartPending_ == next) {
        return;
    }
    rangeStartPending_ = next;
    Q_EMIT stateChanged();
}

void ReviewShellController::synchronizeActiveSources(const bool advanceGeneration) {
    // The review controller publishes loading snapshots while an open is in flight. Those
    // snapshots describe the candidate provider state, not a committed review. Keep the shell's
    // Active Sources stable until the command terminal arrives; a success then adopts the new
    // validated set, while a failure continues to expose the restored prior set.
    if (review_.busy()) {
        return;
    }
    const QVariantList nextSources = review_.activeSources();
    const int nextCanonical = review_.canonicalSourceIndex();
    if (activeSources_ == nextSources && canonicalSourceIndex_ == nextCanonical) {
        return;
    }
    activeSources_ = nextSources;
    canonicalSourceIndex_ = nextCanonical;
    if (advanceGeneration) {
        ++activeGeneration_;
    }
    QStringList nextIdentities;
    nextIdentities.reserve(activeSources_.size());
    for (const QVariant& source : activeSources_) {
        nextIdentities.push_back(review_.frozenSourceIdentity(source.toUrl()));
    }
    frozenActiveIdentities_ = std::move(nextIdentities);
    if (!review_.busy()) {
        stagedSources_ = activeSources_;
        stagedReferenceIndex_ = activeSources_.isEmpty() ? 0 : std::max(0, canonicalSourceIndex_);
    }
    Q_EMIT stateChanged();
}

bool ReviewShellController::submitOrQueue(ReviewIntent intent) {
    intent.id = allocateIntentId();
    if (intent.id == 0U) {
        Q_EMIT intentEvent(
            0U, RejectedStatus, intent.kind, InvalidIntentError, intent.sources.size());
        return false;
    }
    intent.expectedGeneration = activeGeneration_;
    intent.expectedSources = activeSourceIdentities();

    if (intent.kind == CloseSourcesIntent) {
        cancelAllQueuedIntents();
        if (activeSources_.isEmpty() && !review_.busy() && !activeIntent_.has_value()) {
            review_.clearCandidateSourceErrors();
            Q_EMIT intentEvent(
                intent.id, SucceededStatus, intent.kind, NoIntentError, intent.sources.size());
            Q_EMIT intentFinished(intent.id,
                                  intent.kind,
                                  static_cast<int>(application::CommandOutcome::Succeeded),
                                  QString{});
            return true;
        }
    }

    if (review_.busy() || activeIntent_.has_value() || !reviewIntents_.empty()) {
        return enqueueIntent(std::move(intent));
    }
    if (!submitIntent(intent)) {
        Q_EMIT intentEvent(
            intent.id, RejectedStatus, intent.kind, SubmissionRejectedError, intent.sources.size());
        return false;
    }
    return true;
}

bool ReviewShellController::submitIntent(ReviewIntent& intent) {
    if (!rebaseIntent(intent)) {
        Q_EMIT intentEvent(
            intent.id, RejectedStatus, intent.kind, StaleTopologyError, intent.sources.size());
        return false;
    }

    bool accepted = false;
    switch (intent.kind) {
    case OpenSourcesIntent:
        accepted = review_.openSources(intent.sources, intent.referenceIndex);
        break;
    case ReplaceSourcesIntent:
    case AddSourcesIntent:
    case RemoveSourceIntent:
        accepted = review_.reopenSources(intent.sources, intent.referenceIndex);
        break;
    case ChangeReferenceIntent:
        accepted = review_.changeReference(intent.referenceIndex);
        break;
    case CloseSourcesIntent:
        accepted = review_.closeSources();
        break;
    }
    if (!accepted) {
        return false;
    }
    intent.commandId = review_.lastSubmittedCommandId();
    if (intent.commandId == 0U) {
        return false;
    }
    activeIntent_ = intent;
    Q_EMIT stateChanged();
    Q_EMIT intentEvent(intent.id, RunningStatus, intent.kind, NoIntentError, intent.sources.size());
    return true;
}

bool ReviewShellController::enqueueIntent(ReviewIntent intent) {
    if (intent.kind == OpenSourcesIntent || intent.kind == ReplaceSourcesIntent) {
        for (auto existing = reviewIntents_.begin(); existing != reviewIntents_.end();) {
            if (existing->kind != OpenSourcesIntent && existing->kind != ReplaceSourcesIntent) {
                ++existing;
                continue;
            }
            const ReviewIntent replaced = *existing;
            existing = reviewIntents_.erase(existing);
            Q_EMIT intentEvent(
                replaced.id, ReplacedStatus, replaced.kind, NoIntentError, replaced.sources.size());
        }
    } else if (intent.kind == ChangeReferenceIntent) {
        const auto existing = std::find_if(
            reviewIntents_.rbegin(), reviewIntents_.rend(), [](const ReviewIntent& queued) {
                return queued.kind == ChangeReferenceIntent;
            });
        if (existing != reviewIntents_.rend()) {
            const ReviewIntent replaced = *existing;
            *existing = std::move(intent);
            Q_EMIT stateChanged();
            Q_EMIT intentEvent(
                replaced.id, ReplacedStatus, replaced.kind, NoIntentError, replaced.sources.size());
            Q_EMIT intentEvent(existing->id,
                               QueuedStatus,
                               existing->kind,
                               NoIntentError,
                               existing->sources.size());
            return true;
        }
    }

    if (reviewIntents_.size() >= kMaximumQueuedIntents) {
        Q_EMIT intentEvent(
            intent.id, RejectedStatus, intent.kind, QueueFullError, intent.sources.size());
        return false;
    }

    reviewIntents_.push_back(std::move(intent));
    Q_EMIT stateChanged();
    const ReviewIntent& queued = reviewIntents_.back();
    Q_EMIT intentEvent(queued.id, QueuedStatus, queued.kind, NoIntentError, queued.sources.size());
    return true;
}

void ReviewShellController::drainIntentQueue() {
    if (review_.busy() || activeIntent_.has_value() || reviewIntents_.empty()) {
        return;
    }
    ReviewIntent intent = std::move(reviewIntents_.front());
    reviewIntents_.pop_front();
    Q_EMIT stateChanged();
    if (!submitIntent(intent)) {
        if (!review_.busy() && !reviewIntents_.empty()) {
            QMetaObject::invokeMethod(this, [this] { drainIntentQueue(); }, Qt::QueuedConnection);
        }
    }
}

void ReviewShellController::finishIntent(const int outcome, const QString& errorKey) {
    if (!activeIntent_.has_value()) {
        return;
    }
    const ReviewIntent completed = *activeIntent_;
    activeIntent_.reset();
    const bool succeeded = outcome == static_cast<int>(application::CommandOutcome::Succeeded);
    if (succeeded) {
        synchronizeActiveSources(true);
    } else {
        synchronizeActiveSources();
        if (completed.kind == RemoveSourceIntent || completed.kind == ChangeReferenceIntent) {
            stagedSources_ = activeSources_;
            stagedReferenceIndex_ =
                activeSources_.isEmpty() ? 0 : std::max(0, canonicalSourceIndex_);
            openIntent_ = NewReview;
        }
    }
    Q_EMIT stateChanged();
    Q_EMIT intentEvent(completed.id,
                       succeeded ? SucceededStatus : FailedStatus,
                       completed.kind,
                       succeeded ? NoIntentError : CommandFailedError,
                       completed.sources.size());
    Q_EMIT intentFinished(completed.id, completed.kind, outcome, errorKey);
    if (!reviewIntents_.empty()) {
        QMetaObject::invokeMethod(this, [this] { drainIntentQueue(); }, Qt::QueuedConnection);
    }
}

bool ReviewShellController::rebaseIntent(ReviewIntent& intent) const {
    if (intent.expectedGeneration == activeGeneration_) {
        return true;
    }

    const QStringList activeIdentities = activeSourceIdentities();
    switch (intent.kind) {
    case OpenSourcesIntent:
    case CloseSourcesIntent:
        return true;
    case ReplaceSourcesIntent:
        return false;
    case AddSourcesIntent: {
        QVariantList rebased = activeSources_;
        QStringList rebasedIdentities = activeIdentities;
        for (const QVariant& source : intent.sources) {
            const QString identity = review_.frozenSourceIdentity(source.toUrl());
            if (intent.expectedSources.contains(identity) || rebasedIdentities.contains(identity)) {
                continue;
            }
            if (rebased.size() >= 3) {
                return false;
            }
            rebased.push_back(source);
            rebasedIdentities.push_back(identity);
        }
        if (rebased.size() == activeSources_.size()) {
            return false;
        }
        intent.sources = std::move(rebased);
        intent.referenceIndex = rebasedIdentities.indexOf(intent.referenceIdentity);
        if (intent.referenceIndex < 0) {
            intent.referenceIndex =
                std::clamp(canonicalSourceIndex_, 0, static_cast<int>(intent.sources.size()) - 1);
        }
        return true;
    }
    case RemoveSourceIntent: {
        const int targetIndex = activeIdentities.indexOf(intent.targetIdentity);
        if (targetIndex < 0 || activeSources_.size() <= 1) {
            return false;
        }
        intent.sources = activeSources_;
        intent.sources.removeAt(targetIndex);
        QStringList remaining = activeIdentities;
        remaining.removeAt(targetIndex);
        intent.referenceIndex = remaining.indexOf(intent.referenceIdentity);
        if (intent.referenceIndex < 0) {
            intent.referenceIndex = 0;
        }
        return true;
    }
    case ChangeReferenceIntent:
        intent.referenceIndex = activeIdentities.indexOf(intent.referenceIdentity);
        return intent.referenceIndex >= 0;
    }
    return false;
}

QVariantMap ReviewShellController::intentMap(const ReviewIntent& intent,
                                             const ReviewIntentStatus status) const {
    return QVariantMap{
        {QStringLiteral("id"), QVariant::fromValue<qulonglong>(intent.id)},
        {QStringLiteral("status"), status},
        {QStringLiteral("kind"), intent.kind},
        {QStringLiteral("origin"), intent.origin},
        {QStringLiteral("sourceCount"), intent.sources.size()},
        {QStringLiteral("referenceIndex"), intent.referenceIndex},
        {QStringLiteral("generation"), QVariant::fromValue<qulonglong>(intent.expectedGeneration)},
    };
}

QStringList ReviewShellController::activeSourceIdentities() const {
    if (frozenActiveIdentities_.size() == activeSources_.size()) {
        return frozenActiveIdentities_;
    }
    QStringList identities;
    identities.reserve(activeSources_.size());
    for (const QVariant& source : activeSources_) {
        identities.push_back(review_.frozenSourceIdentity(source.toUrl()));
    }
    return identities;
}

bool ReviewShellController::hasPendingSourceIntent(const ReviewIntentKind kind,
                                                   const QString& sourceIdentity) const {
    if (sourceIdentity.isEmpty()) {
        return false;
    }
    const auto matches = [kind, &sourceIdentity](const ReviewIntent& intent) {
        if (intent.kind != kind) {
            return false;
        }
        return kind == RemoveSourceIntent ? intent.targetIdentity == sourceIdentity
                                          : intent.referenceIdentity == sourceIdentity;
    };
    return (activeIntent_.has_value() && matches(*activeIntent_)) ||
           std::any_of(reviewIntents_.begin(), reviewIntents_.end(), matches);
}

std::uint64_t ReviewShellController::allocateIntentId() noexcept {
    if (intentIdsExhausted_) {
        return 0U;
    }
    const std::uint64_t result = nextIntentId_;
    if (nextIntentId_ == std::numeric_limits<std::uint64_t>::max()) {
        intentIdsExhausted_ = true;
    } else {
        ++nextIntentId_;
    }
    return result;
}

ReviewShellController::EffectiveComparisonState
ReviewShellController::effectiveComparisonState() const noexcept {
    using presentation::DifferenceEdge;
    using presentation::ViewMode;

    constexpr int kEdge0And1 = static_cast<int>(DifferenceEdge::Between0And1);

    // The shell deliberately keeps Active Sources unchanged while a transaction is loading.
    // Outside that window, the controller's validated projection is authoritative even if the
    // shell was constructed before its first stateChanged signal.
    const int sourceCount =
        review_.busy() ? static_cast<int>(activeSources_.size()) : review_.sourceCount();
    if (sourceCount <= 1) {
        return EffectiveComparisonState{
            .viewMode = static_cast<int>(ViewMode::Single),
            .differenceEdge = kEdge0And1,
        };
    }

    const int requestedView = preferences_ != nullptr ? static_cast<int>(preferences_->viewMode())
                                                      : static_cast<int>(ViewMode::SideBySide);
    const int requestedEdge =
        preferences_ != nullptr ? static_cast<int>(preferences_->differenceEdge()) : kEdge0And1;
    const ViewMode effectiveMode = presentation::effectiveViewMode(
        static_cast<ViewMode>(requestedView), static_cast<std::size_t>(sourceCount));
    const bool validEdge = requestedEdge >= kEdge0And1 &&
                           requestedEdge <= static_cast<int>(DifferenceEdge::Between1And2);
    return EffectiveComparisonState{
        .viewMode = static_cast<int>(effectiveMode),
        .differenceEdge = sourceCount == 2 ? kEdge0And1 : (validEdge ? requestedEdge : kEdge0And1),
    };
}

} // namespace dvs::ui
