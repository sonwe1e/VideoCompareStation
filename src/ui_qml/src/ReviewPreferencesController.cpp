#include "dvs/ui/ReviewPreferencesController.h"

#include "RecentMediaFiles.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace dvs::ui {
namespace {

constexpr int kSaveDebounceMilliseconds = 150;
constexpr auto kShutdownSaveFlushTimeout = std::chrono::milliseconds{500};

// Legacy key name: the list widened from videos to all recent media; keep it so existing
// history survives.
constexpr std::string_view kRecentMediaFilesKey = "review.recent-video-files";
constexpr std::string_view kLegacyLargeStepKey = "review.large-step-frames";
constexpr std::string_view kShortcutPresetKey = "review.shortcut-preset";
constexpr std::string_view kDropFrameTimecodeKey = "review.drop-frame-timecode";
constexpr std::string_view kViewModeKey = "review.view-mode";
constexpr std::string_view kDifferenceMetricKey = "review.difference-metric";
constexpr std::string_view kDifferenceGainKey = "review.difference-gain";
constexpr std::string_view kDifferenceEdgeKey = "review.difference-edge";
constexpr std::string_view kDifferenceFilterKey = "review.difference-filter";
constexpr std::string_view kOscModeKey = "review.osc-mode";
constexpr std::string_view kPlaybackContinuityPolicyKey = "review.playback-continuity-policy";
constexpr std::string_view kDefaultPairPolicyKey = "review.default-pair-policy";

// C1 resume entries reuse the settings document instead of a second store. The key carries the
// path|size|mtime identity (so an edited or replaced file never resumes onto the wrong position)
// and the value carries "<lastTouchedMillis>:<frame>", so eviction can order by recency without a
// second index. 48 entries is far more review sessions than anyone keeps in rotation and still
// bounds the document at a few kilobytes.
constexpr std::string_view kResumeKeyPrefix = "resume.";
constexpr std::size_t kResumeEntryCap = 48;

// QML hands back the identity already case-folded by SourceIdentity, so the key stays a plain
// concat and never re-normalises. An identity containing a ':' cannot collide with the value
// separator because the value is only ever parsed as "<millis>:<frame>" after the "resume." prefix.
[[nodiscard]] std::string resumeKeyFor(const QString& sourceIdentity) {
    return std::string{kResumeKeyPrefix} + sourceIdentity.toStdString();
}

class SettingsEventQueue final : public application::IApplicationEventSink {
public:
    explicit SettingsEventQueue(std::function<void()> wakeup) : wakeup_(std::move(wakeup)) {}

    [[nodiscard]] application::EventPostResult
    postCritical(application::ApplicationEvent event) noexcept override {
        {
            std::scoped_lock lock{mutex_};
            if (closed_) {
                return application::EventPostResult::Closed;
            }
            try {
                events_.push_back(std::move(event));
            } catch (...) {
                return application::EventPostResult::Closed;
            }
        }
        try {
            wakeup_();
        } catch (...) { // NOLINT(bugprone-empty-catch): wake-up loss is recovered by drain
            // The event is already accepted. A missed wake-up is recovered by shutdown draining.
        }
        return application::EventPostResult::Accepted;
    }

    [[nodiscard]] application::EventPostResult
    postRealtime(application::ApplicationEvent event) noexcept override {
        return postCritical(std::move(event));
    }

    void closeRealtimeIngress() noexcept override {}

    void closeCriticalIngress() noexcept override {
        std::scoped_lock lock{mutex_};
        closed_ = true;
    }

    [[nodiscard]] std::vector<application::ApplicationEvent> takeAll() noexcept {
        std::scoped_lock lock{mutex_};
        std::vector<application::ApplicationEvent> result;
        try {
            result.reserve(events_.size());
            while (!events_.empty()) {
                result.push_back(std::move(events_.front()));
                events_.pop_front();
            }
        } catch (...) {
            events_.clear();
        }
        return result;
    }

private:
    std::mutex mutex_;
    std::deque<application::ApplicationEvent> events_;
    std::function<void()> wakeup_;
    bool closed_ = false;
};

// std::visit over a fully valueless-safe variant cannot throw.
// NOLINTBEGIN(bugprone-exception-escape)
[[nodiscard]] const application::RequestContext*
terminalContext(const application::RequestTerminal& terminal) noexcept {
    // NOLINTEND(bugprone-exception-escape)
    return std::visit(
        [](const auto& outcome) -> const application::RequestContext* {
            return std::get_if<application::RequestContext>(&outcome.context);
        },
        terminal);
}

// One named option per persisted enum value. The tables below are the single source for both
// directions of the settings mapping: parsing a stored string and writing the stored string,
// so the two can never drift apart.
template <typename Enum> struct NamedEnumOption {
    std::string_view text;
    Enum value;
};

constexpr NamedEnumOption<ReviewPreferencesController::ViewMode> kViewModeOptions[] = {
    {"side-by-side", ReviewPreferencesController::ViewMode::SideBySide},
    {"three-up", ReviewPreferencesController::ViewMode::ThreeUp},
    {"reference-focus", ReviewPreferencesController::ViewMode::ReferenceFocus},
    {"difference", ReviewPreferencesController::ViewMode::Difference},
    {"analysis-grid", ReviewPreferencesController::ViewMode::AnalysisGrid},
    {"wipe", ReviewPreferencesController::ViewMode::Wipe},
    {"fade", ReviewPreferencesController::ViewMode::Fade},
};
constexpr NamedEnumOption<ReviewPreferencesController::DifferenceMetric>
    kDifferenceMetricOptions[] = {
        {"rgb-absolute", ReviewPreferencesController::DifferenceMetric::RgbAbsolute},
        {"luma", ReviewPreferencesController::DifferenceMetric::Luma},
        {"chroma", ReviewPreferencesController::DifferenceMetric::Chroma},
        {"heatmap", ReviewPreferencesController::DifferenceMetric::Heatmap},
        {"exact-planes", ReviewPreferencesController::DifferenceMetric::ExactPlanes},
        {"signed-subtract", ReviewPreferencesController::DifferenceMetric::SignedSubtract},
        {"highlight", ReviewPreferencesController::DifferenceMetric::Highlight},
};
constexpr NamedEnumOption<ReviewPreferencesController::DifferenceGain> kDifferenceGainOptions[] = {
    {"1x", ReviewPreferencesController::DifferenceGain::Gain1x},
    {"2x", ReviewPreferencesController::DifferenceGain::Gain2x},
    {"4x", ReviewPreferencesController::DifferenceGain::Gain4x},
    {"8x", ReviewPreferencesController::DifferenceGain::Gain8x},
    {"16x", ReviewPreferencesController::DifferenceGain::Gain16x},
};
constexpr NamedEnumOption<ReviewPreferencesController::DifferenceEdge> kDifferenceEdgeOptions[] = {
    {"0-1", ReviewPreferencesController::DifferenceEdge::Edge0And1},
    {"0-2", ReviewPreferencesController::DifferenceEdge::Edge0And2},
    {"1-2", ReviewPreferencesController::DifferenceEdge::Edge1And2},
};
constexpr NamedEnumOption<ReviewPreferencesController::DifferenceFilter>
    kDifferenceFilterOptions[] = {
        {"nearest", ReviewPreferencesController::DifferenceFilter::Nearest},
        {"bilinear", ReviewPreferencesController::DifferenceFilter::Bilinear},
        {"bicubic", ReviewPreferencesController::DifferenceFilter::Bicubic},
};
// The osc mode, continuity policy and default pair policy persist as named int codes; the
// unset sentinel (-1 for the osc mode) maps to the trailing default name on write.
constexpr NamedEnumOption<int> kOscModeOptions[] = {
    {"pinned", 0},
    {"auto", 1},
    {"hidden", 2},
};
constexpr NamedEnumOption<int> kContinuityPolicyOptions[] = {
    {"review-every-frame", 0},
    {"real-time", 1},
    {"contextual", 2},
};
constexpr NamedEnumOption<int> kPairPolicyOptions[] = {
    {"reference-and-first-candidate", 0},
    {"last-two-active-sources", 1},
    {"preserve-if-available", 2},
};

template <typename Enum, std::size_t optionCount>
[[nodiscard]] std::optional<Enum>
parseEnum(const std::map<std::string, std::string, std::less<>>& values,
          const std::string_view key,
          const NamedEnumOption<Enum> (&options)[optionCount]) {
    const auto iterator = values.find(key);
    if (iterator == values.end()) {
        return std::nullopt;
    }
    for (const auto& [text, value] : options) {
        if (iterator->second == text) {
            return value;
        }
    }
    return std::nullopt;
}

template <typename Enum, std::size_t optionCount>
void writeEnum(std::map<std::string, std::string, std::less<>>& values,
               const std::string_view key,
               const NamedEnumOption<Enum> (&options)[optionCount],
               const Enum value,
               const std::string_view fallbackText) {
    for (const auto& [text, optionValue] : options) {
        if (optionValue == value) {
            values.insert_or_assign(std::string{key}, std::string{text});
            return;
        }
    }
    values.insert_or_assign(std::string{key}, std::string{fallbackText});
}

} // namespace

class ReviewPreferencesController::Impl final {
public:
    Impl(ReviewPreferencesController& owner,
         std::shared_ptr<application::ISettingsRepository> repository)
        : owner_(owner), repository_(std::move(repository)) {
        if (!repository_) {
            throw std::invalid_argument{"Review preferences require a settings repository."};
        }
        const QPointer<ReviewPreferencesController> guardedOwner{&owner_};
        events_ = std::make_shared<SettingsEventQueue>([guardedOwner] {
            if (guardedOwner.isNull()) {
                return;
            }
            static_cast<void>(QMetaObject::invokeMethod(
                guardedOwner,
                [guardedOwner] {
                    if (!guardedOwner.isNull()) {
                        guardedOwner->processRepositoryEvents();
                    }
                },
                Qt::QueuedConnection));
        });
        QObject::connect(&saveTimer_, &QTimer::timeout, &owner_, [this] { saveNow(); });
        saveTimer_.setSingleShot(true);
        saveTimer_.setInterval(kSaveDebounceMilliseconds);
        beginLoad();
    }

    ~Impl() {
        stop();
    }

    [[nodiscard]] int shortcutPreset() const noexcept {
        return shortcutPreset_;
    }

    [[nodiscard]] bool dropFrameTimecode() const noexcept {
        return dropFrameTimecode_;
    }

    [[nodiscard]] ViewMode viewMode() const noexcept {
        return viewMode_;
    }

    [[nodiscard]] DifferenceMetric differenceMetric() const noexcept {
        return differenceMetric_;
    }

    [[nodiscard]] DifferenceGain differenceGain() const noexcept {
        return differenceGain_;
    }

    [[nodiscard]] DifferenceEdge differenceEdge() const noexcept {
        return differenceEdge_;
    }

    [[nodiscard]] DifferenceFilter differenceFilter() const noexcept {
        return differenceFilter_;
    }

    [[nodiscard]] int oscMode() const noexcept {
        return oscMode_;
    }

    [[nodiscard]] int playbackContinuityPolicy() const noexcept {
        return playbackContinuityPolicy_;
    }

    [[nodiscard]] int defaultPairPolicy() const noexcept {
        return defaultPairPolicy_;
    }

    void setShortcutPreset(const int value) {
        if ((value != 0 && value != 1) || value == shortcutPreset_) {
            return;
        }
        shortcutPreset_ = value;
        changed();
    }

    void setDropFrameTimecode(const bool value) {
        if (value == dropFrameTimecode_) {
            return;
        }
        dropFrameTimecode_ = value;
        changed();
    }

    void setViewMode(const ViewMode value) {
        setEnum(viewMode_, value, ViewMode::SideBySide, ViewMode::Fade);
    }

    void setDifferenceMetric(const DifferenceMetric value) {
        if (value < DifferenceMetric::RgbAbsolute || value > DifferenceMetric::Highlight ||
            value == differenceMetric_) {
            return;
        }
        differenceMetric_ = value;
        changed();
    }

    void setDifferenceGain(const DifferenceGain value) {
        if (value < DifferenceGain::Gain1x || value > DifferenceGain::Gain16x ||
            value == differenceGain_) {
            return;
        }
        differenceGain_ = value;
        changed();
    }

    void setDifferenceEdge(const DifferenceEdge value) {
        setEnum(differenceEdge_, value, DifferenceEdge::Edge0And1, DifferenceEdge::Edge1And2);
    }

    void setDifferenceFilter(const DifferenceFilter value) {
        if (value < DifferenceFilter::Nearest || value > DifferenceFilter::Bicubic ||
            value == differenceFilter_) {
            return;
        }
        differenceFilter_ = value;
        changed();
    }

    void setOscMode(const int value) {
        if (value < -1 || value > 2 || value == oscMode_) {
            return;
        }
        oscMode_ = value;
        changed();
    }

    void setPlaybackContinuityPolicy(const int value) {
        // 0 ReviewEveryFrame, 1 RealTime, 2 Contextual (domain::PlaybackContinuityPolicy).
        if (value < 0 || value > 2 || value == playbackContinuityPolicy_) {
            return;
        }
        playbackContinuityPolicy_ = value;
        changed();
    }

    void setDefaultPairPolicy(const int value) {
        // 0 ReferenceAndFirstCandidate, 1 LastTwoActiveSources, 2 PreserveIfAvailable.
        if (value < 0 || value > 2 || value == defaultPairPolicy_) {
            return;
        }
        defaultPairPolicy_ = value;
        changed();
    }

    [[nodiscard]] QStringList recentMediaFiles() const {
        return recentMediaFiles_;
    }

    void rememberMediaFile(const QUrl& url) {
        const QStringList next =
            detail::mergeRecentMediaFiles({url.toString(QUrl::FullyEncoded)}, recentMediaFiles_);
        if (next == recentMediaFiles_) {
            return;
        }
        recentMediaFiles_ = next;
        // History must not set localChanges_: a successful early open must not suppress the
        // asynchronous load of the user's unrelated playback and comparison preferences.
        dirty_ = true;
        Q_EMIT owner_.recentMediaFilesChanged();
        scheduleSave();
    }

    void stop() noexcept {
        if (stopped_ || stopping_) {
            return;
        }
        stopping_ = true;
        saveTimer_.stop();
        try {
            flushPendingChangesBeforeStop();
        } catch (...) { // NOLINT(bugprone-empty-catch): bounded shutdown is the contract
            // Shutdown remains bounded even if snapshot construction or an adapter submission
            // unexpectedly throws. Outstanding requests are canceled below.
        }
        stopped_ = true;
        if (repository_ && pendingLoad_.has_value()) {
            repository_->cancel(*pendingLoad_);
        }
        if (repository_ && pendingSave_.has_value()) {
            repository_->cancel(*pendingSave_);
        }
        pendingLoad_.reset();
        pendingSave_.reset();
        events_->closeRealtimeIngress();
        events_->closeCriticalIngress();
        repository_.reset();
        stopping_ = false;
    }

    void processRepositoryEvents() noexcept {
        drainEvents();
    }

private:
    template <typename Enum>
    void setEnum(Enum& target, const Enum value, const Enum minimum, const Enum maximum) {
        if (value < minimum || value > maximum || value == target) {
            return;
        }
        target = value;
        changed();
    }

    void changed() {
        localChanges_ = true;
        dirty_ = true;
        Q_EMIT owner_.preferencesChanged();
        scheduleSave();
    }

    [[nodiscard]] application::RequestContext nextContext() noexcept {
        const std::uint64_t requestId = nextRequestId_;
        if (nextRequestId_ != (std::numeric_limits<std::uint64_t>::max)()) {
            ++nextRequestId_;
        }
        return application::RequestContext{
            .sessionId = domain::SessionId{1U},
            .sessionEpoch = domain::SessionEpoch{1U},
            .requestId = domain::RequestId{requestId},
        };
    }

    void beginLoad() {
        const application::RequestContext context = nextContext();
        if (repository_->submit(application::SettingsLoadRequest{.context = context}, events_) ==
            application::PortSubmitResult::Accepted) {
            pendingLoad_ = context;
            return;
        }
        loadFinished_ = true;
    }

    void scheduleSave() {
        if (!stopped_ && !stopping_ && loadFinished_ && !pendingSave_.has_value()) {
            saveTimer_.start();
        }
    }

    void saveNow() {
        if (stopped_ || !dirty_ || !loadFinished_ || pendingSave_.has_value()) {
            return;
        }
        writeKnownValues(settings_.values);
        const application::RequestContext context = nextContext();
        const application::SettingsSaveRequest request{
            .context = context,
            .settings = settings_,
        };
        const application::PortSubmitResult result = repository_->submit(request, events_);
        if (result == application::PortSubmitResult::Accepted) {
            pendingSave_ = context;
            pendingSaveSnapshot_ = request.settings;
            dirty_ = false;
        } else if (result == application::PortSubmitResult::Busy && !stopping_) {
            saveTimer_.start();
        }
    }

    void flushPendingChangesBeforeStop() {
        if (!repository_ || (!dirty_ && !pendingSave_.has_value())) {
            return;
        }

        // The Qt event loop has normally stopped by this point. Drain repository completions
        // directly so an already-delivered load can preserve unknown keys and the latest
        // debounced values can reach the atomic save boundary before cancellation.
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + kShutdownSaveFlushTimeout;
        do {
            drainEvents();
            if (loadFinished_ && dirty_ && !pendingSave_.has_value()) {
                saveNow();
            }
            drainEvents();
            if (!dirty_ && !pendingSave_.has_value()) {
                return;
            }
            QThread::msleep(1U);
        } while (std::chrono::steady_clock::now() < deadline);
    }

    void drainEvents() noexcept {
        if (stopped_) {
            return;
        }
        try {
            for (application::ApplicationEvent& event : events_->takeAll()) {
                if (auto* loaded = std::get_if<application::SettingsLoaded>(&event)) {
                    if (pendingLoad_.has_value() && loaded->context == *pendingLoad_) {
                        settings_ = std::move(loaded->settings);
                        if (settings_.values.erase(std::string{kLegacyLargeStepKey}) > 0U) {
                            dirty_ = true;
                        }
                        QStringList storedRecent;
                        if (const auto entry = settings_.values.find(kRecentMediaFilesKey);
                            entry != settings_.values.end()) {
                            const QJsonArray array =
                                QJsonDocument::fromJson(QByteArray::fromStdString(entry->second))
                                    .array();
                            for (const auto& value : array) {
                                if (value.isString()) {
                                    storedRecent.push_back(value.toString());
                                }
                            }
                        }
                        const auto recent =
                            detail::mergeRecentMediaFiles(recentMediaFiles_, storedRecent);
                        if (recent != recentMediaFiles_) {
                            recentMediaFiles_ = recent;
                            Q_EMIT owner_.recentMediaFilesChanged();
                        }
                        if (!localChanges_) {
                            applyKnownValues(settings_.values);
                        }
                    }
                    continue;
                }
                const auto* terminal = std::get_if<application::RequestTerminal>(&event);
                if (terminal == nullptr) {
                    continue;
                }
                const application::RequestContext* context = terminalContext(*terminal);
                if (context == nullptr) {
                    continue;
                }
                if (pendingLoad_.has_value() && *context == *pendingLoad_) {
                    pendingLoad_.reset();
                    loadFinished_ = true;
                    scheduleSave();
                    continue;
                }
                if (pendingSave_.has_value() && *context == *pendingSave_) {
                    const bool succeeded =
                        std::holds_alternative<application::RequestSucceeded>(*terminal);
                    pendingSave_.reset();
                    if (succeeded) {
                        settings_ = pendingSaveSnapshot_;
                    } else {
                        dirty_ = true;
                    }
                    scheduleSave();
                }
            }
        } catch (...) {
            stop();
        }
    }

public:
    void rememberResumeFrame(const std::string& key, qint64 frame) {
        if (!loadFinished_ || stopped_ || stopping_) {
            return;
        }
        const qint64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        // Recency stamp, not a clock reading: eviction picks the smallest stamp, so two writes
        // inside one millisecond must still order. Without this the tie falls back to the key and
        // eviction stops meaning "least recently touched" exactly when writes arrive in a burst -
        // which is what a resume-record debounce does while the user scrubs.
        lastResumeStamp_ = std::max(now, lastResumeStamp_ + 1);
        const std::string payload = std::to_string(lastResumeStamp_) + ":" + std::to_string(frame);
        const auto [iterator, inserted] = settings_.values.insert_or_assign(key, payload);
        if (!inserted && iterator->second == payload) {
            return;
        }
        evictOverflowingResumeEntries();
        changed();
    }

    void forgetResumeFrame(const std::string& key) {
        if (!loadFinished_ || stopped_ || stopping_) {
            return;
        }
        if (settings_.values.erase(key) == 0U) {
            return;
        }
        changed();
    }

    [[nodiscard]] std::optional<std::pair<qint64, qint64>>
    readResumeEntry(const std::string& key) const {
        const auto iterator = settings_.values.find(key);
        if (iterator == settings_.values.end()) {
            return std::nullopt;
        }
        const std::string_view payload{iterator->second};
        const auto separator = payload.find(':');
        if (separator == std::string_view::npos) {
            return std::nullopt;
        }
        bool touchedOk = false;
        bool frameOk = false;
        const qint64 touched = QString::fromStdString(std::string{payload.substr(0, separator)})
                                   .toLongLong(&touchedOk);
        const qint64 frame =
            QString::fromStdString(std::string{payload.substr(separator + 1)}).toLongLong(&frameOk);
        // A malformed entry is treated as absent rather than guessed at: resuming a hand-edited or
        // corrupted position would be worse than starting at the beginning.
        if (!touchedOk || !frameOk || frame < 0) {
            return std::nullopt;
        }
        return std::make_pair(touched, frame);
    }

    void evictOverflowingResumeEntries() {
        std::vector<std::pair<qint64, std::string>> resumeEntries;
        resumeEntries.reserve(settings_.values.size());
        for (const auto& [key, value] : settings_.values) {
            if (!key.starts_with(std::string{kResumeKeyPrefix})) {
                continue;
            }
            bool ok = false;
            qint64 touched = QString::fromStdString(value).section(':', 0, 0).toLongLong(&ok);
            if (!ok) {
                touched = 0;
            }
            resumeEntries.emplace_back(touched, key);
        }
        if (resumeEntries.size() <= kResumeEntryCap) {
            return;
        }
        // Oldest first, then by key so the eviction order is deterministic for equal timestamps
        // and a test can predict exactly which entries survive.
        std::sort(resumeEntries.begin(), resumeEntries.end());
        const std::size_t excess = resumeEntries.size() - kResumeEntryCap;
        for (std::size_t index = 0; index < excess; ++index) {
            settings_.values.erase(resumeEntries[index].second);
        }
    }

private:
    void applyKnownValues(const std::map<std::string, std::string, std::less<>>& values) {
        int nextShortcutPreset = 0;
        if (const auto iterator = values.find(kShortcutPresetKey);
            iterator != values.end() && iterator->second == "player") {
            nextShortcutPreset = 1;
        }
        const bool nextDropFrameTimecode = values.find(kDropFrameTimecodeKey) != values.end() &&
                                           values.find(kDropFrameTimecodeKey)->second == "true";
        const ViewMode nextViewMode = parseEnum<ViewMode>(values, kViewModeKey, kViewModeOptions)
                                          .value_or(ViewMode::SideBySide);
        const DifferenceMetric nextMetric =
            parseEnum<DifferenceMetric>(values, kDifferenceMetricKey, kDifferenceMetricOptions)
                .value_or(DifferenceMetric::RgbAbsolute);
        const DifferenceGain nextGain =
            parseEnum<DifferenceGain>(values, kDifferenceGainKey, kDifferenceGainOptions)
                .value_or(DifferenceGain::Gain1x);
        const DifferenceEdge nextReference =
            parseEnum<DifferenceEdge>(values, kDifferenceEdgeKey, kDifferenceEdgeOptions)
                .value_or(DifferenceEdge::Edge0And1);
        const DifferenceFilter nextFilter =
            parseEnum<DifferenceFilter>(values, kDifferenceFilterKey, kDifferenceFilterOptions)
                .value_or(DifferenceFilter::Bilinear);
        const int nextOscMode = parseEnum<int>(values, kOscModeKey, kOscModeOptions).value_or(-1);
        // C-07 continuity: domain enum codes 0/1/2; Contextual is smoothness-first RealTime.
        const int nextContinuity =
            parseEnum<int>(values, kPlaybackContinuityPolicyKey, kContinuityPolicyOptions)
                .value_or(2);
        // C-02 pair policy: PreserveIfAvailable is the session-friendly default.
        const int nextPairPolicy =
            parseEnum<int>(values, kDefaultPairPolicyKey, kPairPolicyOptions).value_or(2);

        const bool changed =
            shortcutPreset_ != nextShortcutPreset || dropFrameTimecode_ != nextDropFrameTimecode ||
            viewMode_ != nextViewMode || differenceMetric_ != nextMetric ||
            differenceGain_ != nextGain || differenceEdge_ != nextReference ||
            differenceFilter_ != nextFilter || oscMode_ != nextOscMode ||
            playbackContinuityPolicy_ != nextContinuity || defaultPairPolicy_ != nextPairPolicy;
        shortcutPreset_ = nextShortcutPreset;
        dropFrameTimecode_ = nextDropFrameTimecode;
        viewMode_ = nextViewMode;
        differenceMetric_ = nextMetric;
        differenceGain_ = nextGain;
        differenceEdge_ = nextReference;
        differenceFilter_ = nextFilter;
        oscMode_ = nextOscMode;
        playbackContinuityPolicy_ = nextContinuity;
        defaultPairPolicy_ = nextPairPolicy;
        if (changed) {
            Q_EMIT owner_.preferencesChanged();
        }
    }

    void writeKnownValues(std::map<std::string, std::string, std::less<>>& values) const {
        const auto recent = QJsonDocument{QJsonArray::fromStringList(recentMediaFiles_)};
        values.insert_or_assign(std::string{kRecentMediaFilesKey},
                                recent.toJson(QJsonDocument::Compact).toStdString());
        values.erase(std::string{kLegacyLargeStepKey});
        values.insert_or_assign(std::string{kShortcutPresetKey},
                                shortcutPreset_ == 1 ? "player" : "review");
        values.insert_or_assign(std::string{kDropFrameTimecodeKey},
                                dropFrameTimecode_ ? "true" : "false");
        writeEnum(values, kViewModeKey, kViewModeOptions, viewMode_, "side-by-side");
        writeEnum(values,
                  kDifferenceMetricKey,
                  kDifferenceMetricOptions,
                  differenceMetric_,
                  "rgb-absolute");
        writeEnum(values, kDifferenceGainKey, kDifferenceGainOptions, differenceGain_, "1x");
        writeEnum(values, kDifferenceEdgeKey, kDifferenceEdgeOptions, differenceEdge_, "0-1");
        writeEnum(
            values, kDifferenceFilterKey, kDifferenceFilterOptions, differenceFilter_, "bilinear");
        writeEnum(values, kOscModeKey, kOscModeOptions, oscMode_, "contextual");
        writeEnum(values,
                  kPlaybackContinuityPolicyKey,
                  kContinuityPolicyOptions,
                  playbackContinuityPolicy_,
                  "contextual");
        writeEnum(values,
                  kDefaultPairPolicyKey,
                  kPairPolicyOptions,
                  defaultPairPolicy_,
                  "preserve-if-available");
    }

    ReviewPreferencesController& owner_;
    std::shared_ptr<application::ISettingsRepository> repository_;
    std::shared_ptr<SettingsEventQueue> events_;
    QTimer saveTimer_;
    application::SettingsSnapshot settings_;
    application::SettingsSnapshot pendingSaveSnapshot_;
    std::optional<application::RequestContext> pendingLoad_;
    std::optional<application::RequestContext> pendingSave_;
    std::uint64_t nextRequestId_ = 1U;
    // Strictly increasing recency stamp for resume entries; see rememberResumeFrame.
    qint64 lastResumeStamp_ = std::numeric_limits<qint64>::min();
    int shortcutPreset_ = 0;
    bool dropFrameTimecode_ = false;
    ViewMode viewMode_ = ViewMode::SideBySide;
    DifferenceMetric differenceMetric_ = DifferenceMetric::RgbAbsolute;
    DifferenceGain differenceGain_ = DifferenceGain::Gain1x;
    DifferenceEdge differenceEdge_ = DifferenceEdge::Edge0And1;
    DifferenceFilter differenceFilter_ = DifferenceFilter::Bilinear;
    int oscMode_ = -1;
    int playbackContinuityPolicy_ = 2;
    int defaultPairPolicy_ = 2;
    QStringList recentMediaFiles_;
    bool loadFinished_ = false;
    bool localChanges_ = false;
    bool dirty_ = false;
    bool stopping_ = false;
    bool stopped_ = false;
};

ReviewPreferencesController::ReviewPreferencesController(
    std::shared_ptr<application::ISettingsRepository> repository, QObject* const parent)
    // QPointer owns Qt's weak control block correctly; the analyzer mistakes its last temporary
    // copy release for a use-after-free inside Qt's custom operator delete.
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDelete)
    : QObject(parent), impl_(std::make_unique<Impl>(*this, std::move(repository))) {}

ReviewPreferencesController::~ReviewPreferencesController() = default;

QStringList ReviewPreferencesController::recentMediaFiles() const {
    return impl_->recentMediaFiles();
}

void ReviewPreferencesController::rememberMediaFile(const QUrl& url) {
    impl_->rememberMediaFile(url);
}

void ReviewPreferencesController::rememberMediaPath(const QString& localPath) {
    impl_->rememberMediaFile(QUrl::fromLocalFile(localPath));
}

int ReviewPreferencesController::shortcutPreset() const noexcept {
    return impl_->shortcutPreset();
}

bool ReviewPreferencesController::dropFrameTimecode() const noexcept {
    return impl_->dropFrameTimecode();
}

ReviewPreferencesController::ViewMode ReviewPreferencesController::viewMode() const noexcept {
    return impl_->viewMode();
}

ReviewPreferencesController::DifferenceMetric
ReviewPreferencesController::differenceMetric() const noexcept {
    return impl_->differenceMetric();
}

ReviewPreferencesController::DifferenceGain
ReviewPreferencesController::differenceGain() const noexcept {
    return impl_->differenceGain();
}

ReviewPreferencesController::DifferenceEdge
ReviewPreferencesController::differenceEdge() const noexcept {
    return impl_->differenceEdge();
}

ReviewPreferencesController::DifferenceFilter
ReviewPreferencesController::differenceFilter() const noexcept {
    return impl_->differenceFilter();
}

int ReviewPreferencesController::viewModeCode() const noexcept {
    return static_cast<int>(viewMode());
}

int ReviewPreferencesController::differenceMetricCode() const noexcept {
    return static_cast<int>(differenceMetric());
}

int ReviewPreferencesController::differenceGainCode() const noexcept {
    return static_cast<int>(differenceGain());
}

int ReviewPreferencesController::differenceEdgeCode() const noexcept {
    return static_cast<int>(differenceEdge());
}

int ReviewPreferencesController::differenceFilterCode() const noexcept {
    return static_cast<int>(differenceFilter());
}

int ReviewPreferencesController::oscMode() const noexcept {
    return impl_->oscMode();
}

void ReviewPreferencesController::setShortcutPreset(const int value) {
    impl_->setShortcutPreset(value);
}

void ReviewPreferencesController::setDropFrameTimecode(const bool value) {
    impl_->setDropFrameTimecode(value);
}

void ReviewPreferencesController::setViewMode(const ViewMode value) {
    impl_->setViewMode(value);
}

void ReviewPreferencesController::setDifferenceMetric(const DifferenceMetric value) {
    impl_->setDifferenceMetric(value);
}

void ReviewPreferencesController::setDifferenceGain(const DifferenceGain value) {
    impl_->setDifferenceGain(value);
}

void ReviewPreferencesController::setDifferenceEdge(const DifferenceEdge value) {
    impl_->setDifferenceEdge(value);
}

void ReviewPreferencesController::setDifferenceFilter(const DifferenceFilter value) {
    impl_->setDifferenceFilter(value);
}

void ReviewPreferencesController::setViewModeCode(const int value) {
    setViewMode(static_cast<ViewMode>(value));
}

void ReviewPreferencesController::setDifferenceMetricCode(const int value) {
    setDifferenceMetric(static_cast<DifferenceMetric>(value));
}

void ReviewPreferencesController::setDifferenceGainCode(const int value) {
    setDifferenceGain(static_cast<DifferenceGain>(value));
}

void ReviewPreferencesController::setDifferenceEdgeCode(const int value) {
    setDifferenceEdge(static_cast<DifferenceEdge>(value));
}

void ReviewPreferencesController::setDifferenceFilterCode(const int value) {
    setDifferenceFilter(static_cast<DifferenceFilter>(value));
}

void ReviewPreferencesController::setOscMode(const int value) {
    impl_->setOscMode(value);
}

int ReviewPreferencesController::playbackContinuityPolicy() const noexcept {
    return impl_->playbackContinuityPolicy();
}

int ReviewPreferencesController::defaultPairPolicy() const noexcept {
    return impl_->defaultPairPolicy();
}

void ReviewPreferencesController::setPlaybackContinuityPolicy(const int value) {
    impl_->setPlaybackContinuityPolicy(value);
}

void ReviewPreferencesController::setDefaultPairPolicy(const int value) {
    impl_->setDefaultPairPolicy(value);
}

qint64 ReviewPreferencesController::resumeFrameFor(const QString& sourceIdentity) const {
    if (sourceIdentity.isEmpty() || thread() != QThread::currentThread()) {
        return -1;
    }
    const std::optional<std::pair<qint64, qint64>> entry =
        impl_->readResumeEntry(resumeKeyFor(sourceIdentity));
    return entry.has_value() ? entry->second : -1;
}

void ReviewPreferencesController::rememberResumeFrame(const QString& sourceIdentity,
                                                      const qint64 frame) {
    if (sourceIdentity.isEmpty() || frame < 0 || thread() != QThread::currentThread()) {
        return;
    }
    impl_->rememberResumeFrame(resumeKeyFor(sourceIdentity), frame);
}

void ReviewPreferencesController::forgetResumeFrame(const QString& sourceIdentity) {
    if (sourceIdentity.isEmpty() || thread() != QThread::currentThread()) {
        return;
    }
    impl_->forgetResumeFrame(resumeKeyFor(sourceIdentity));
}

void ReviewPreferencesController::stop() noexcept {
    if (thread() != QThread::currentThread()) {
        static_cast<void>(QMetaObject::invokeMethod(this, "stop", Qt::QueuedConnection));
        return;
    }
    impl_->stop();
}

void ReviewPreferencesController::processRepositoryEvents() noexcept {
    impl_->processRepositoryEvents();
}

} // namespace dvs::ui
