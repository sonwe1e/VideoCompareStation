#include "dvs/ui/ReviewPreferencesController.h"

#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QTimer>

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
        } catch (...) {
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

[[nodiscard]] const application::RequestContext*
terminalContext(const application::RequestTerminal& terminal) noexcept {
    return std::visit(
        [](const auto& outcome) -> const application::RequestContext* {
            return std::get_if<application::RequestContext>(&outcome.context);
        },
        terminal);
}

template <typename Enum>
[[nodiscard]] std::optional<Enum>
parseEnum(const std::map<std::string, std::string, std::less<>>& values,
          const std::string_view key,
          const std::initializer_list<std::pair<std::string_view, Enum>> options) {
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

    void stop() noexcept {
        if (stopped_ || stopping_) {
            return;
        }
        stopping_ = true;
        saveTimer_.stop();
        try {
            flushPendingChangesBeforeStop();
        } catch (...) {
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

    void applyKnownValues(const std::map<std::string, std::string, std::less<>>& values) {
        int nextShortcutPreset = 0;
        if (const auto iterator = values.find(kShortcutPresetKey);
            iterator != values.end() && iterator->second == "player") {
            nextShortcutPreset = 1;
        }
        const bool nextDropFrameTimecode = values.find(kDropFrameTimecodeKey) != values.end() &&
                                           values.find(kDropFrameTimecodeKey)->second == "true";
        const ViewMode nextViewMode =
            parseEnum<ViewMode>(values,
                                kViewModeKey,
                                {{"side-by-side", ViewMode::SideBySide},
                                 {"three-up", ViewMode::ThreeUp},
                                 {"reference-focus", ViewMode::ReferenceFocus},
                                 {"difference", ViewMode::Difference},
                                 {"analysis-grid", ViewMode::AnalysisGrid},
                                 {"wipe", ViewMode::Wipe},
                                 {"fade", ViewMode::Fade}})
                .value_or(ViewMode::SideBySide);
        const DifferenceMetric nextMetric =
            parseEnum<DifferenceMetric>(values,
                                        kDifferenceMetricKey,
                                        {{"rgb-absolute", DifferenceMetric::RgbAbsolute},
                                         {"luma", DifferenceMetric::Luma},
                                         {"chroma", DifferenceMetric::Chroma},
                                         {"heatmap", DifferenceMetric::Heatmap},
                                         {"exact-planes", DifferenceMetric::ExactPlanes},
                                         {"signed-subtract", DifferenceMetric::SignedSubtract},
                                         {"highlight", DifferenceMetric::Highlight}})
                .value_or(DifferenceMetric::RgbAbsolute);
        const DifferenceGain nextGain =
            parseEnum<DifferenceGain>(values,
                                      kDifferenceGainKey,
                                      {{"1x", DifferenceGain::Gain1x},
                                       {"2x", DifferenceGain::Gain2x},
                                       {"4x", DifferenceGain::Gain4x},
                                       {"8x", DifferenceGain::Gain8x},
                                       {"16x", DifferenceGain::Gain16x}})
                .value_or(DifferenceGain::Gain1x);
        const DifferenceEdge nextReference =
            parseEnum<DifferenceEdge>(values,
                                      kDifferenceEdgeKey,
                                      {{"0-1", DifferenceEdge::Edge0And1},
                                       {"0-2", DifferenceEdge::Edge0And2},
                                       {"1-2", DifferenceEdge::Edge1And2}})
                .value_or(DifferenceEdge::Edge0And1);
        const DifferenceFilter nextFilter =
            parseEnum<DifferenceFilter>(values,
                                        kDifferenceFilterKey,
                                        {{"nearest", DifferenceFilter::Nearest},
                                         {"bilinear", DifferenceFilter::Bilinear},
                                         {"bicubic", DifferenceFilter::Bicubic}})
                .value_or(DifferenceFilter::Bilinear);
        int nextOscMode = -1;
        if (const auto iterator = values.find(kOscModeKey); iterator != values.end()) {
            if (iterator->second == "pinned") {
                nextOscMode = 0;
            } else if (iterator->second == "auto") {
                nextOscMode = 1;
            } else if (iterator->second == "hidden") {
                nextOscMode = 2;
            }
        }
        // C-07 continuity: domain enum codes 0/1/2; Contextual is the multi-source default.
        int nextContinuity = 2;
        if (const auto iterator = values.find(kPlaybackContinuityPolicyKey);
            iterator != values.end()) {
            if (iterator->second == "review-every-frame") {
                nextContinuity = 0;
            } else if (iterator->second == "real-time") {
                nextContinuity = 1;
            } else if (iterator->second == "contextual") {
                nextContinuity = 2;
            }
        }
        // C-02 pair policy: PreserveIfAvailable is the session-friendly default.
        int nextPairPolicy = 2;
        if (const auto iterator = values.find(kDefaultPairPolicyKey); iterator != values.end()) {
            if (iterator->second == "reference-and-first-candidate") {
                nextPairPolicy = 0;
            } else if (iterator->second == "last-two-active-sources") {
                nextPairPolicy = 1;
            } else if (iterator->second == "preserve-if-available") {
                nextPairPolicy = 2;
            }
        }

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
        values.erase(std::string{kLegacyLargeStepKey});
        values.insert_or_assign(std::string{kShortcutPresetKey},
                                shortcutPreset_ == 1 ? "player" : "review");
        values.insert_or_assign(std::string{kDropFrameTimecodeKey},
                                dropFrameTimecode_ ? "true" : "false");
        const char* viewModeName = "side-by-side";
        switch (viewMode_) {
        case ViewMode::ThreeUp:
            viewModeName = "three-up";
            break;
        case ViewMode::ReferenceFocus:
            viewModeName = "reference-focus";
            break;
        case ViewMode::Difference:
            viewModeName = "difference";
            break;
        case ViewMode::AnalysisGrid:
            viewModeName = "analysis-grid";
            break;
        case ViewMode::Wipe:
            viewModeName = "wipe";
            break;
        case ViewMode::Fade:
            viewModeName = "fade";
            break;
        case ViewMode::SideBySide:
            break;
        }
        values.insert_or_assign(std::string{kViewModeKey}, std::string{viewModeName});
        static constexpr std::string_view metrics[] = {"rgb-absolute",
                                                       "luma",
                                                       "chroma",
                                                       "heatmap",
                                                       "exact-planes",
                                                       "signed-subtract",
                                                       "highlight"};
        static constexpr std::string_view gains[] = {"1x", "2x", "4x", "8x", "16x"};
        static constexpr std::string_view filters[] = {"nearest", "bilinear", "bicubic"};
        const auto metricIndex = static_cast<std::size_t>(differenceMetric_);
        values.insert_or_assign(
            std::string{kDifferenceMetricKey},
            std::string{metricIndex < std::size(metrics) ? metrics[metricIndex] : metrics[0]});
        values.insert_or_assign(std::string{kDifferenceGainKey},
                                std::string{gains[static_cast<std::size_t>(differenceGain_)]});
        const char* edgeName = "0-1";
        switch (differenceEdge_) {
        case DifferenceEdge::Edge0And2:
            edgeName = "0-2";
            break;
        case DifferenceEdge::Edge1And2:
            edgeName = "1-2";
            break;
        case DifferenceEdge::Edge0And1:
            break;
        }
        values.insert_or_assign(std::string{kDifferenceEdgeKey}, std::string{edgeName});
        values.insert_or_assign(std::string{kDifferenceFilterKey},
                                std::string{filters[static_cast<std::size_t>(differenceFilter_)]});
        const char* oscModeName = "contextual";
        if (oscMode_ == 0) {
            oscModeName = "pinned";
        } else if (oscMode_ == 1) {
            oscModeName = "auto";
        } else if (oscMode_ == 2) {
            oscModeName = "hidden";
        }
        values.insert_or_assign(std::string{kOscModeKey}, std::string{oscModeName});
        const char* continuityName = "contextual";
        if (playbackContinuityPolicy_ == 0) {
            continuityName = "review-every-frame";
        } else if (playbackContinuityPolicy_ == 1) {
            continuityName = "real-time";
        }
        values.insert_or_assign(std::string{kPlaybackContinuityPolicyKey},
                                std::string{continuityName});
        const char* pairPolicyName = "preserve-if-available";
        if (defaultPairPolicy_ == 0) {
            pairPolicyName = "reference-and-first-candidate";
        } else if (defaultPairPolicy_ == 1) {
            pairPolicyName = "last-two-active-sources";
        }
        values.insert_or_assign(std::string{kDefaultPairPolicyKey}, std::string{pairPolicyName});
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
