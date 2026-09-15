#include "dvs/ui/ReviewController.h"

#include "dvs/application/ComparisonExactness.h"
#include "dvs/ui/SourceIdentity.h"
#include "dvs/ui/SourceListModel.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QMetaObject>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace dvs::ui {
namespace {

constexpr int kFallbackProjectionIntervalMilliseconds = 10;
constexpr int kSourceDiskStatusPollMilliseconds = 50;
constexpr qint64 kSourceDiskStatusRefreshMilliseconds = 250;
constexpr qsizetype kMaximumAlignmentTimelineMarkers = 256;

struct SourceDiskStatusRequest final {
    QString key;
    QString path;
    std::uint64_t expectedBytes = 0U;
    std::int64_t expectedModifiedMilliseconds = 0;
    bool hasExpectedIdentity = false;
};

struct SourceDiskStatusResult final {
    QString key;
    bool changedOnDisk = false;
};

struct SourceDiskStatusCompletion final {
    std::uint64_t generation = 0U;
    std::optional<std::vector<SourceDiskStatusResult>> results;
};

class SourceDiskStatusMailbox final {
public:
    void publish(const std::uint64_t generation,
                 std::optional<std::vector<SourceDiskStatusResult>> results) noexcept {
        try {
            auto completion =
                std::make_shared<const SourceDiskStatusCompletion>(SourceDiskStatusCompletion{
                    .generation = generation, .results = std::move(results)});
            std::lock_guard lock(mutex_);
            if (generation < highestPublishedGeneration_) {
                return;
            }
            highestPublishedGeneration_ = generation;
            completion_ = std::move(completion);
        } catch (...) {
        }
    }

    [[nodiscard]] std::shared_ptr<const SourceDiskStatusCompletion> tryTake() noexcept {
        try {
            std::unique_lock lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock()) {
                return {};
            }
            return std::exchange(completion_, {});
        } catch (...) {
            return {};
        }
    }

    void taskScheduled() noexcept {
        try {
            std::lock_guard lock(mutex_);
            ++activeTasks_;
        } catch (...) {
        }
    }

    void taskFinished() noexcept {
        try {
            {
                std::lock_guard lock(mutex_);
                if (activeTasks_ > 0U) {
                    --activeTasks_;
                }
            }
            idleCondition_.notify_all();
        } catch (...) {
        }
    }

    [[nodiscard]] bool waitForIdle(const std::chrono::milliseconds timeout) noexcept {
        try {
            std::unique_lock lock(mutex_);
            return idleCondition_.wait_for(lock, timeout, [this] { return activeTasks_ == 0U; });
        } catch (...) {
            return false;
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable idleCondition_;
    std::uint64_t highestPublishedGeneration_ = 0U;
    std::size_t activeTasks_ = 0U;
    std::shared_ptr<const SourceDiskStatusCompletion> completion_;
};

class SourceDiskStatusTaskLease final {
public:
    explicit SourceDiskStatusTaskLease(std::shared_ptr<SourceDiskStatusMailbox> mailbox) noexcept
        : mailbox_(std::move(mailbox)) {
        mailbox_->taskScheduled();
    }

    ~SourceDiskStatusTaskLease() {
        finish();
    }

    SourceDiskStatusTaskLease(const SourceDiskStatusTaskLease&) = delete;
    SourceDiskStatusTaskLease& operator=(const SourceDiskStatusTaskLease&) = delete;

    void finish() noexcept {
        if (mailbox_) {
            mailbox_->taskFinished();
            mailbox_.reset();
        }
    }

private:
    std::shared_ptr<SourceDiskStatusMailbox> mailbox_;
};

class SourceDiskStatusTaskState final {
public:
    SourceDiskStatusTaskState(
        std::shared_ptr<SourceDiskStatusMailbox> mailbox,
        std::function<ReviewController::SourceFileMetadata(const QString&)> metadataProbe,
        const std::uint64_t generation,
        std::vector<SourceDiskStatusRequest> requests)
        : lease_(mailbox), mailbox_(std::move(mailbox)), metadataProbe_(std::move(metadataProbe)),
          generation_(generation), requests_(std::move(requests)) {}

    void run() noexcept {
        try {
            std::vector<SourceDiskStatusResult> results;
            results.reserve(requests_.size());
            for (const SourceDiskStatusRequest& request : requests_) {
                ReviewController::SourceFileMetadata live;
                if (metadataProbe_) {
                    live = metadataProbe_(request.path);
                } else {
                    const QFileInfo liveInfo{request.path};
                    live.exists = liveInfo.exists();
                    if (live.exists) {
                        live.byteSize = static_cast<std::uint64_t>(liveInfo.size());
                        live.modifiedUtcMilliseconds = liveInfo.lastModified().toMSecsSinceEpoch();
                    }
                }
                bool changed = !live.exists;
                if (live.exists && request.hasExpectedIdentity) {
                    changed = live.byteSize != request.expectedBytes ||
                              live.modifiedUtcMilliseconds != request.expectedModifiedMilliseconds;
                }
                results.push_back(SourceDiskStatusResult{
                    .key = request.key,
                    .changedOnDisk = changed,
                });
            }
            mailbox_->publish(generation_, std::move(results));
        } catch (...) {
            mailbox_->publish(generation_, std::nullopt);
        }
    }

private:
    // Declared first so its destructor runs after the payload and mailbox members. Shutdown does
    // not observe the task as idle while Qt/standard-library payload destruction is still active.
    SourceDiskStatusTaskLease lease_;
    std::shared_ptr<SourceDiskStatusMailbox> mailbox_;
    std::function<ReviewController::SourceFileMetadata(const QString&)> metadataProbe_;
    std::uint64_t generation_ = 0U;
    std::vector<SourceDiskStatusRequest> requests_;
};

[[nodiscard]] QString twoDigits(const std::int64_t value) {
    return QString::number(value).rightJustified(2, QLatin1Char{'0'});
}

[[nodiscard]] QString mediaClock(const domain::MediaTime time) {
    const std::int64_t milliseconds = std::max<std::int64_t>(0, time.microseconds() / 1'000);
    const std::int64_t hours = milliseconds / 3'600'000;
    const std::int64_t minutes = (milliseconds / 60'000) % 60;
    const std::int64_t seconds = (milliseconds / 1'000) % 60;
    const std::int64_t millis = milliseconds % 1'000;
    return QStringLiteral("%1:%2:%3.%4")
        .arg(twoDigits(hours),
             twoDigits(minutes),
             twoDigits(seconds),
             QString::number(millis).rightJustified(3, QLatin1Char{'0'}));
}

[[nodiscard]] bool supportsDropFrame(const domain::RationalRate& rate) noexcept {
    return (rate.numerator() == 30'000 && rate.denominator() == 1'001) ||
           (rate.numerator() == 60'000 && rate.denominator() == 1'001);
}

[[nodiscard]] QString cfrTimecode(std::int64_t frame, const int nominalFps, const bool dropFrame) {
    frame = std::max<std::int64_t>(0, frame);
    std::int64_t timecodeFrame = frame;
    if (dropFrame) {
        const std::int64_t droppedPerMinute = nominalFps == 60 ? 4 : 2;
        const std::int64_t framesPerTenMinutes = nominalFps * 600 - droppedPerMinute * 9;
        const std::int64_t framesPerMinute = nominalFps * 60 - droppedPerMinute;
        const std::int64_t tenMinuteBlocks = frame / framesPerTenMinutes;
        const std::int64_t remaining = frame % framesPerTenMinutes;
        timecodeFrame += droppedPerMinute * 9 * tenMinuteBlocks;
        if (remaining >= droppedPerMinute) {
            timecodeFrame += droppedPerMinute * ((remaining - droppedPerMinute) / framesPerMinute);
        }
    }
    const std::int64_t frames = timecodeFrame % nominalFps;
    const std::int64_t totalSeconds = timecodeFrame / nominalFps;
    const std::int64_t seconds = totalSeconds % 60;
    const std::int64_t minutes = (totalSeconds / 60) % 60;
    const std::int64_t hours = (totalSeconds / 3'600) % 24;
    const QLatin1Char separator = dropFrame ? QLatin1Char{';'} : QLatin1Char{':'};
    return QStringLiteral("%1:%2:%3%4%5")
        .arg(twoDigits(hours),
             twoDigits(minutes),
             twoDigits(seconds),
             QString{separator},
             twoDigits(frames));
}

[[nodiscard]] QString timingModeName(const domain::TimingConfidence confidence) {
    switch (confidence) {
    case domain::TimingConfidence::kDeclaredCfr:
        return QStringLiteral("Declared CFR");
    case domain::TimingConfidence::kVerifiedCfr:
        return QStringLiteral("Verified CFR");
    case domain::TimingConfidence::kVariableFrameRate:
        return QStringLiteral("VFR");
    }
    return QStringLiteral("Unknown");
}

[[nodiscard]] QString colorMatrixName(const domain::ColorMatrix matrix) {
    switch (matrix) {
    case domain::ColorMatrix::kBt601:
        return QStringLiteral("BT.601");
    case domain::ColorMatrix::kBt709:
        return QStringLiteral("BT.709");
    }
    return QStringLiteral("Unknown");
}

[[nodiscard]] QString colorRangeName(const domain::ColorRange range) {
    return range == domain::ColorRange::kFull ? QStringLiteral("Full") : QStringLiteral("Limited");
}

struct LocalFileCandidate final {
    std::filesystem::path path;
    QString filename;
};

struct LocalFileValidation final {
    std::optional<LocalFileCandidate> candidate;
    QString errorKey;
};

[[nodiscard]] LocalFileValidation validateLocalFile(const QUrl& url) {
    if (!url.isValid() || !url.isLocalFile() || url.toLocalFile().isEmpty()) {
        return LocalFileValidation{.errorKey = QStringLiteral("invalid-argument")};
    }

    const QFileInfo input{url.toLocalFile()};
    const QString canonicalPath = input.canonicalFilePath();
    if (!input.isFile() || canonicalPath.isEmpty()) {
        return LocalFileValidation{.errorKey = QStringLiteral("source-missing")};
    }

    const QFileInfo canonical{canonicalPath};
    return LocalFileValidation{
        .candidate =
            LocalFileCandidate{
                .path = std::filesystem::path{canonicalPath.toStdWString()},
                .filename = canonical.fileName(),
            },
    };
}

[[nodiscard]] bool isStillImageFile(const QUrl& url) {
    const QString suffix = QFileInfo{url.toLocalFile()}.suffix().toLower();
    static const QStringList kImageSuffixes = {
        QStringLiteral("png"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"),
        QStringLiteral("bmp"),  QStringLiteral("gif"),  QStringLiteral("webp"),
        QStringLiteral("tif"),  QStringLiteral("tiff"),
    };
    return kImageSuffixes.contains(suffix);
}

[[nodiscard]] QVariantMap rejectedDrop(const QString& errorKey, const QString& detail = {}) {
    return QVariantMap{
        {QStringLiteral("accepted"), false},
        {QStringLiteral("errorKey"), errorKey},
        {QStringLiteral("detail"), detail},
    };
}

[[nodiscard]] ReviewController::ReviewDisplayState
mapDisplayState(const domain::SessionState state) noexcept {
    switch (state) {
    case domain::SessionState::kEmpty:
        return ReviewController::ReviewDisplayState::Empty;
    case domain::SessionState::kLoading:
        return ReviewController::ReviewDisplayState::Loading;
    case domain::SessionState::kReady:
        return ReviewController::ReviewDisplayState::Ready;
    case domain::SessionState::kInvalid:
        return ReviewController::ReviewDisplayState::Invalid;
    case domain::SessionState::kError:
        return ReviewController::ReviewDisplayState::Error;
    }
    return ReviewController::ReviewDisplayState::Error;
}

struct ReviewView final {
    QString sourceAFilename;
    QString sourceBFilename;
    QString sourceCFilename;
    QVariantList sourceUrls;
    int sourceCount = 0;
    int canonicalSourceIndex = -1;
    int referenceSourceIndex = -1;
    ReviewController::ReviewDisplayState displayState = ReviewController::ReviewDisplayState::Empty;
    bool busy = false;
    bool framePending = false;
    bool playing = false;
    bool graphicsReady = false;
    qint64 currentFrame = -1;
    qulonglong totalFrames = 0U;
    int oneSecondStepFrames = 30;
    qint64 currentMediaTime = -1;
    QString currentTimecode = QStringLiteral("00:00:00:00");
    QString rationalFrameRate;
    QString timingMode;
    bool dropFrameTimecodeAvailable = false;
    QVariantList sourceMediaInfo;
    QString sourceAErrorKey;
    QString sourceBErrorKey;
    QString sourceCErrorKey;
    bool sourceAMissing = true;
    bool sourceBMissing = true;
    bool sourceCMissing = true;
    QString pairErrorKey;
    QString lastErrorTechnicalDetail;
    QString frameMappingStatus;
    QString alignmentEstimateStatus;
    QString sequenceAlignmentStatus;
    bool alignmentAnalysisRunning = false;
    qreal alignmentAnalysisProgress = 0.0;
    QString alignmentAnalysisStatus;
    QString manualAnchorStatus;
    QVariantList alignmentTimelineMarkers;
    qulonglong alignmentTimelineMarkerOverflowCount = 0U;
    bool manualAnchorActive = false;
    bool autoAlignmentActive = false;
    bool alignmentRequired = false;
    bool automaticAlignmentPending = false;
    bool canConfirmAutomaticAlignment = false;
    bool canUndoAutomaticAlignment = false;
    QVariantList compatibilityFindings;
    QVariantList differenceEdges;
    bool canOpen = false;
    bool canFirst = false;
    bool canPrevious = false;
    bool canNext = false;
    bool canLast = false;
    bool canPlay = false;
    bool canPause = false;

    [[nodiscard]] bool operator==(const ReviewView&) const = default;

    [[nodiscard]] bool sameFrameState(const ReviewView& other) const {
        return currentFrame == other.currentFrame && sourceAMissing == other.sourceAMissing &&
               sourceBMissing == other.sourceBMissing && sourceCMissing == other.sourceCMissing &&
               frameMappingStatus == other.frameMappingStatus &&
               autoAlignmentActive == other.autoAlignmentActive &&
               differenceEdges == other.differenceEdges && canPrevious == other.canPrevious &&
               canNext == other.canNext;
    }
};

[[nodiscard]] QString sourceName(const domain::SourceId sourceId) {
    return sourceId < 3U ? QString{QChar{static_cast<char16_t>(u'A' + sourceId)}}
                         : QString::number(sourceId);
}

void appendTimelineMarker(QVariantList& markers,
                          qulonglong& overflowCount,
                          const domain::FrameId frameId,
                          const QString& kind,
                          const QString& source,
                          const int confidence = 100) {
    if (markers.size() >= kMaximumAlignmentTimelineMarkers) {
        ++overflowCount;
        return;
    }
    markers.push_back(QVariantMap{
        {QStringLiteral("frame"), QVariant::fromValue<qulonglong>(frameId.value())},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("source"), source},
        {QStringLiteral("confidence"), confidence},
    });
}

} // namespace

class ReviewController::Impl final {
public:
    Impl(ReviewController& owner, Dependencies dependencies)
        : owner_(owner), dependencies_(std::move(dependencies)), sourceModel_(&owner) {
        if (!dependencies_.submit || !dependencies_.snapshot ||
            !dependencies_.takeCompletedCommands) {
            throw std::invalid_argument{"Review controller dependencies must not be empty."};
        }

        QObject::connect(&projectionTimer_, &QTimer::timeout, &owner_, [this] { refresh(); });
        projectionTimer_.setInterval(kFallbackProjectionIntervalMilliseconds);
        QObject::connect(&sourceDiskStatusPollTimer_, &QTimer::timeout, &owner_, [this] {
            consumeSourceDiskStatusCompletion();
        });
        sourceDiskStatusPollTimer_.setInterval(kSourceDiskStatusPollMilliseconds);
        QObject::connect(&sourceDiskStatusRefreshTimer_, &QTimer::timeout, &owner_, [this] {
            scheduleSourceDiskStatusRefresh();
        });
        sourceDiskStatusRefreshTimer_.setSingleShot(true);
        refresh();
        if (!stopped_ && !dependencies_.eventDriven) {
            projectionTimer_.start();
        }
    }

    ~Impl() {
        projectionTimer_.stop();
        sourceDiskStatusPollTimer_.stop();
        sourceDiskStatusRefreshTimer_.stop();
    }

    [[nodiscard]] const ReviewView& view() const noexcept {
        return view_;
    }

    [[nodiscard]] QAbstractItemModel* sources() noexcept {
        return &sourceModel_;
    }

    [[nodiscard]] std::uint64_t lastSubmittedCommandId() const noexcept {
        return lastSubmittedCommandId_;
    }

    [[nodiscard]] QString timecodeForFrame(const qint64 frame, const bool dropFrame) const {
        if (frame < 0 || !snapshot_ || !snapshot_->validatedComparison) {
            return QStringLiteral("00:00:00:00");
        }
        const domain::MediaDescriptor& descriptor =
            snapshot_->validatedComparison->canonicalDescriptor();
        std::optional<domain::MediaTime> frameTime;
        if (snapshot_->canonicalTimeline.has_value()) {
            auto converted = domain::canonicalFrameStartTime(
                *snapshot_->canonicalTimeline, domain::FrameId{static_cast<std::int64_t>(frame)});
            if (converted) {
                frameTime = converted.value();
            }
        } else if (descriptor.frameRate.has_value()) {
            auto converted = descriptor.frameRate->frameStartTime(
                domain::FrameId{static_cast<std::int64_t>(frame)});
            if (converted) {
                frameTime = converted.value();
            }
        }

        if (descriptor.timingConfidence == domain::TimingConfidence::kVariableFrameRate ||
            !descriptor.frameRate.has_value()) {
            return frameTime.has_value()
                       ? QStringLiteral("%1 · Frame %2")
                             .arg(mediaClock(*frameTime), QString::number(frame + 1))
                       : QStringLiteral("Frame %1").arg(frame + 1);
        }

        const domain::RationalRate& rate = *descriptor.frameRate;
        if (rate.denominator() == 1) {
            return cfrTimecode(frame, static_cast<int>(rate.numerator()), false);
        }
        if (supportsDropFrame(rate)) {
            return cfrTimecode(frame, rate.numerator() == 60'000 ? 60 : 30, dropFrame) +
                   (dropFrame ? QStringLiteral(" DF") : QStringLiteral(" NDF"));
        }
        return frameTime.has_value() ? QStringLiteral("%1 · Frame %2")
                                           .arg(mediaClock(*frameTime), QString::number(frame + 1))
                                     : QStringLiteral("Frame %1").arg(frame + 1);
    }

    [[nodiscard]] qint64 mediaTimeForFrame(const qint64 frame) const {
        if (frame < 0 || !snapshot_ || !snapshot_->canonicalTimeline.has_value()) {
            return -1;
        }
        auto converted = domain::canonicalFrameStartTime(
            *snapshot_->canonicalTimeline, domain::FrameId{static_cast<std::int64_t>(frame)});
        return converted ? converted.value().microseconds() : -1;
    }

    [[nodiscard]] qint64 frameForMediaTime(const qint64 microseconds) const {
        if (microseconds < 0 || !snapshot_ || !snapshot_->canonicalTimeline.has_value() ||
            snapshot_->canonicalFrameCount == 0U) {
            return -1;
        }
        auto converted = domain::canonicalFrameAtOrBefore(*snapshot_->canonicalTimeline,
                                                          domain::MediaTime{microseconds});
        if (!converted) {
            return -1;
        }
        return std::clamp<std::int64_t>(
            converted.value().value(),
            0,
            static_cast<std::int64_t>(snapshot_->canonicalFrameCount - 1U));
    }

    [[nodiscard]] bool openComparison(const QUrl& first, const QUrl& second) {
        return openComparisonSet(first, second, QUrl{}, 0);
    }

    [[nodiscard]] bool openComparisonSet(const QUrl& first,
                                         const QUrl& second,
                                         const QUrl& third,
                                         const int referenceSourceIndex) {
        QVariantList urls{first, second};
        if (!third.isEmpty()) {
            urls.push_back(third);
        }
        return openSources(
            urls, referenceSourceIndex, false, application::OpenReviewIntent::NewReview);
    }

    [[nodiscard]] bool openSources(const QVariantList& urls,
                                   const int referenceSourceIndex,
                                   const bool preserveDisplayedTime,
                                   const application::OpenReviewIntent intent) {
        if (!onOwnerThread() || stopped_) {
            return false;
        }
        refresh();
        if (stopped_ || pendingCommand_.has_value()) {
            return false;
        }

        if (urls.empty() || urls.size() > 3 || referenceSourceIndex < 0 ||
            referenceSourceIndex >= urls.size()) {
            return false;
        }

        std::array<LocalFileValidation, 3U> validated;
        try {
            for (qsizetype index = 0; index < urls.size(); ++index) {
                validated[static_cast<std::size_t>(index)] = validateLocalFile(urls[index].toUrl());
            }
        } catch (...) {
            for (qsizetype index = 0; index < urls.size(); ++index) {
                validated[static_cast<std::size_t>(index)] =
                    LocalFileValidation{.errorKey = QStringLiteral("invalid-argument")};
            }
        }

        const int sourceCount = static_cast<int>(urls.size());
        candidateSourceAErrorKey_ = validated[0U].errorKey;
        candidateSourceBErrorKey_ = sourceCount > 1 ? validated[1U].errorKey : QString{};
        candidateSourceCErrorKey_ = sourceCount > 2 ? validated[2U].errorKey : QString{};
        if (!validated[0U].candidate.has_value() ||
            (sourceCount > 1 && !validated[1U].candidate.has_value()) ||
            (sourceCount > 2 && !validated[2U].candidate.has_value()) || !view_.canOpen) {
            publishProjection();
            return false;
        }
        const std::optional<application::CommandContext> context = allocateCommandContext();
        if (!context.has_value()) {
            failClosed();
            return false;
        }
        std::vector<application::OpenComparisonSource> sources;
        sources.reserve(static_cast<std::size_t>(sourceCount));
        const auto appendSource = [&sources, referenceSourceIndex](const LocalFileCandidate& source,
                                                                   const int index) {
            sources.push_back(application::OpenComparisonSource{
                .path = source.path,
                .role = index == referenceSourceIndex ? domain::ComparisonRole::kReference
                                                      : domain::ComparisonRole::kPrediction,
                .displayName = source.filename.toStdString(),
            });
        };
        appendSource(*validated[0U].candidate, 0);
        if (sourceCount > 1) {
            appendSource(*validated[1U].candidate, 1);
        }
        if (sourceCount > 2) {
            appendSource(*validated[2U].candidate, 2);
        }
        return dispatch(application::OpenComparisonCommand{
            .context = *context,
            .sources = std::move(sources),
            .intent = intent,
            .preserveDisplayedTime = preserveDisplayedTime,
        });
    }

    [[nodiscard]] bool changeReference(const int sourceIndex) {
        refresh();
        if (sourceIndex < 0 || sourceIndex >= view_.sourceCount ||
            sourceIndex == view_.canonicalSourceIndex || view_.sourceUrls.isEmpty()) {
            return false;
        }
        return openSources(
            view_.sourceUrls, sourceIndex, true, application::OpenReviewIntent::ChangeReference);
    }

    [[nodiscard]] bool closeSources() {
        if (!onOwnerThread() || stopped_) {
            return false;
        }
        refresh();
        if (stopped_ || pendingCommand_.has_value()) {
            return false;
        }
        if (view_.sourceCount == 0) {
            clearCandidateSourceErrors();
            return true;
        }
        const std::optional<application::CommandContext> context = allocateCommandContext();
        if (!context.has_value()) {
            failClosed();
            return false;
        }
        return dispatch(application::CloseSessionCommand{.context = *context});
    }

    [[nodiscard]] bool first() {
        return dispatchNavigation([](const ReviewView& view) { return view.canFirst; },
                                  [](const application::CommandContext& context) {
                                      return application::PlaybackCommand{
                                          application::FirstFrameCommand{.context = context}};
                                  });
    }

    [[nodiscard]] bool previous() {
        return stepFrames(-1);
    }

    [[nodiscard]] bool next() {
        return stepFrames(1);
    }

    [[nodiscard]] bool last() {
        return dispatchNavigation([](const ReviewView& view) { return view.canLast; },
                                  [](const application::CommandContext& context) {
                                      return application::PlaybackCommand{
                                          application::LastFrameCommand{.context = context}};
                                  });
    }

    [[nodiscard]] bool stepFrames(const qint64 delta) {
        if (delta == 0) {
            return false;
        }
        return dispatchNavigation(
            [delta](const ReviewView& view) { return delta < 0 ? view.canPrevious : view.canNext; },
            [delta](const application::CommandContext& context) {
                return application::PlaybackCommand{application::StepFramesCommand{
                    .context = context,
                    .delta = static_cast<std::int64_t>(delta),
                }};
            });
    }

    [[nodiscard]] bool seekFrame(const qint64 frame) {
        return dispatchNavigation(
            [frame](const ReviewView& view) {
                return view.canFirst && frame >= 0 &&
                       static_cast<qulonglong>(frame) < view.totalFrames &&
                       frame != view.currentFrame;
            },
            [frame](const application::CommandContext& context) {
                return application::PlaybackCommand{application::SeekFrameCommand{
                    .context = context,
                    .frameId = domain::FrameId{static_cast<std::int64_t>(frame)},
                }};
            });
    }

    [[nodiscard]] bool applyAlignmentOffsets(const qint64 sourceAFrames,
                                             const qint64 sourceBFrames,
                                             const qint64 sourceCFrames) {
        return dispatchStateChange(
            [](const ReviewView& view) { return view.canFirst; },
            [this, sourceAFrames, sourceBFrames, sourceCFrames](
                const application::CommandContext& context) {
                std::vector<application::SourceFrameOffset> offsets;
                const std::array<qint64, 3U> values{sourceAFrames, sourceBFrames, sourceCFrames};
                const std::size_t sourceCount = static_cast<std::size_t>(sourceModel_.rowCount());
                offsets.reserve(sourceCount);
                for (std::size_t index = 0U; index < sourceCount; ++index) {
                    if (values[index] != 0) {
                        offsets.push_back(application::SourceFrameOffset{
                            .sourceId = static_cast<domain::SourceId>(index),
                            .frames = static_cast<std::int64_t>(values[index]),
                        });
                    }
                }
                return application::PlaybackCommand{application::SetAlignmentOffsetsCommand{
                    .context = context,
                    .sourceOffsets = std::move(offsets),
                }};
            });
    }

    [[nodiscard]] bool applySourceOffsets(const QVariantList& values) {
        if (values.size() > 3) {
            return false;
        }

        std::vector<application::SourceFrameOffset> offsets;
        offsets.reserve(static_cast<std::size_t>(values.size()));
        for (const QVariant& value : values) {
            const QVariantMap item = value.toMap();
            bool sourceOk = false;
            bool framesOk = false;
            const qulonglong sourceValue =
                item.value(QStringLiteral("sourceId")).toULongLong(&sourceOk);
            const qlonglong framesValue =
                item.value(QStringLiteral("frames")).toLongLong(&framesOk);
            if (!sourceOk || !framesOk ||
                sourceValue > std::numeric_limits<domain::SourceId>::max()) {
                return false;
            }
            const domain::SourceId sourceId = static_cast<domain::SourceId>(sourceValue);
            if (std::any_of(offsets.begin(),
                            offsets.end(),
                            [sourceId](const application::SourceFrameOffset& offset) {
                                return offset.sourceId == sourceId;
                            })) {
                return false;
            }
            offsets.push_back(application::SourceFrameOffset{
                .sourceId = sourceId,
                .frames = static_cast<std::int64_t>(framesValue),
            });
        }

        return dispatchStateChange(
            [](const ReviewView& view) { return view.canFirst; },
            [offsets = std::move(offsets)](const application::CommandContext& context) mutable {
                return application::PlaybackCommand{application::SetAlignmentOffsetsCommand{
                    .context = context,
                    .sourceOffsets = std::move(offsets),
                }};
            });
    }

    [[nodiscard]] bool estimateAlignment() {
        return dispatchBackgroundAnalysis(
            [](const ReviewView& view) { return view.canFirst; },
            [](const application::CommandContext& context) {
                return application::PlaybackCommand{
                    application::EstimateAlignmentCommand{.context = context}};
            });
    }

    [[nodiscard]] bool analyzeSequenceAlignment() {
        return dispatchBackgroundAnalysis(
            [](const ReviewView& view) { return view.canFirst; },
            [](const application::CommandContext& context) {
                return application::PlaybackCommand{
                    application::AnalyzeSequenceAlignmentCommand{.context = context}};
            });
    }

    [[nodiscard]] bool cancelAlignmentAnalysis() {
        return dispatchBackgroundAnalysis(
            [](const ReviewView& view) { return view.alignmentAnalysisRunning; },
            [](const application::CommandContext& context) {
                return application::PlaybackCommand{
                    application::CancelAlignmentAnalysisCommand{.context = context}};
            });
    }

    [[nodiscard]] bool confirmAutomaticAlignment() {
        return dispatchStateChange(
            [](const ReviewView& view) { return view.canConfirmAutomaticAlignment; },
            [](const application::CommandContext& context) {
                return application::PlaybackCommand{
                    application::ConfirmAutomaticAlignmentCommand{.context = context}};
            });
    }

    [[nodiscard]] bool undoAutomaticAlignment() {
        return dispatchStateChange(
            [](const ReviewView& view) { return view.canUndoAutomaticAlignment; },
            [](const application::CommandContext& context) {
                return application::PlaybackCommand{
                    application::UndoAutomaticAlignmentCommand{.context = context}};
            });
    }

    [[nodiscard]] bool setManualAlignmentAnchor(const int sourceIndex,
                                                const qint64 canonicalFrame,
                                                const qint64 sourceFrame) {
        if (sourceIndex < 0 || sourceIndex >= view_.sourceCount || canonicalFrame < 0 ||
            sourceFrame < 0) {
            return false;
        }
        return dispatchStateChange(
            [](const ReviewView& view) { return view.canFirst; },
            [sourceIndex, canonicalFrame, sourceFrame](const application::CommandContext& context) {
                return application::PlaybackCommand{application::SetManualAlignmentAnchorCommand{
                    .context = context,
                    .sourceId = static_cast<domain::SourceId>(sourceIndex),
                    .anchor =
                        application::ManualAlignmentAnchor{
                            .canonicalFrameId =
                                domain::FrameId{static_cast<std::int64_t>(canonicalFrame)},
                            .sourceFrameId =
                                domain::FrameId{static_cast<std::int64_t>(sourceFrame)},
                        },
                }};
            });
    }

    [[nodiscard]] bool clearManualAlignmentAnchors() {
        return dispatchStateChange(
            [](const ReviewView& view) { return view.canFirst; },
            [](const application::CommandContext& context) {
                return application::PlaybackCommand{
                    application::ClearManualAlignmentAnchorsCommand{.context = context}};
            });
    }

    [[nodiscard]] bool play() {
        return dispatchTransport([](const ReviewView& view) { return view.canPlay; },
                                 [](const application::CommandContext& context) {
                                     return application::PlaybackCommand{
                                         application::PlayCommand{.context = context}};
                                 });
    }

    [[nodiscard]] bool pause() {
        return dispatchTransport([](const ReviewView& view) { return view.canPause; },
                                 [](const application::CommandContext& context) {
                                     return application::PlaybackCommand{
                                         application::PauseCommand{.context = context}};
                                 });
    }

    [[nodiscard]] bool togglePlayback() {
        if (!onOwnerThread() || stopped_) {
            return false;
        }
        refresh();
        if (stopped_) {
            return false;
        }
        return view_.playing ? pause() : play();
    }

    void refreshProjection() noexcept {
        refresh();
    }

    [[nodiscard]] bool
    waitForSourceDiskStatusIdle(const std::chrono::milliseconds timeout) noexcept {
        return sourceDiskStatusMailbox_->waitForIdle(timeout);
    }

    [[nodiscard]] QString frozenSourceIdentity(const QUrl& source) const {
        if (!source.isLocalFile()) {
            return canonicalSourceIdentity(source);
        }
        const QString key = QDir::cleanPath(source.toLocalFile()).toCaseFolded();
        const auto it = frozenIdentitiesByPath_.constFind(key);
        if (it != frozenIdentitiesByPath_.cend()) {
            return *it;
        }
        return canonicalSourceIdentity(source);
    }

    void clearCandidateSourceErrors() noexcept {
        if (!onOwnerThread() || stopped_) {
            return;
        }
        if (candidateSourceAErrorKey_.isEmpty() && candidateSourceBErrorKey_.isEmpty() &&
            candidateSourceCErrorKey_.isEmpty()) {
            return;
        }
        candidateSourceAErrorKey_.clear();
        candidateSourceBErrorKey_.clear();
        candidateSourceCErrorKey_.clear();
        publishProjection();
    }

    void stop() noexcept {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        projectionTimer_.stop();
        sourceDiskStatusPollTimer_.stop();
        sourceDiskStatusRefreshTimer_.stop();
        ++sourceDiskStatusGeneration_;
        sourceDiskStatusCheckPending_ = false;
        pendingCommand_.reset();
        pendingNavigationCommand_.reset();
        pendingTransportCommand_.reset();
        publishProjection();
    }

private:
    [[nodiscard]] bool onOwnerThread() const noexcept {
        return owner_.thread() == QThread::currentThread();
    }

    [[nodiscard]] std::vector<DecoderBackendState> decoderBackendStates() const noexcept {
        if (!dependencies_.decoderBackendStates) {
            return {};
        }
        try {
            return dependencies_.decoderBackendStates();
        } catch (...) {
            return {};
        }
    }

    void scheduleSourceDiskStatusRefresh() noexcept {
        try {
            if (stopped_ || !frozenComparison_ || sourceDiskStatusCheckPending_) {
                return;
            }
            if (sourceDiskStatusTimer_.isValid() &&
                !sourceDiskStatusTimer_.hasExpired(kSourceDiskStatusRefreshMilliseconds)) {
                const qint64 remaining =
                    kSourceDiskStatusRefreshMilliseconds - sourceDiskStatusTimer_.elapsed();
                sourceDiskStatusRefreshTimer_.start(
                    static_cast<int>(std::max<qint64>(1, remaining)));
                return;
            }
            sourceDiskStatusRefreshTimer_.stop();

            std::vector<SourceDiskStatusRequest> requests;
            requests.reserve(frozenComparison_->sources().size());
            for (const domain::ComparisonSource& source : frozenComparison_->sources()) {
                const QString path =
                    QString::fromStdWString(source.descriptor.normalizedPath.wstring());
                const auto& identity = source.descriptor.sourceIdentity;
                requests.push_back(SourceDiskStatusRequest{
                    .key = QDir::cleanPath(path).toCaseFolded(),
                    .path = path,
                    .expectedBytes = identity.has_value() ? identity->byteSize : 0U,
                    .expectedModifiedMilliseconds =
                        identity.has_value() ? identity->modifiedUtcMilliseconds : 0,
                    .hasExpectedIdentity = identity.has_value(),
                });
            }

            sourceDiskStatusCheckPending_ = true;
            sourceDiskStatusTimer_.restart();
            const std::uint64_t generation = ++sourceDiskStatusGeneration_;
            const std::shared_ptr<SourceDiskStatusMailbox> mailbox = sourceDiskStatusMailbox_;
            if (dependencies_.scheduleBackgroundTask) {
                auto state = std::make_shared<SourceDiskStatusTaskState>(
                    mailbox,
                    dependencies_.sourceFileMetadataProbe,
                    generation,
                    std::move(requests));
                // The injected scheduler owns its std::function copy. Clang's analyzer cannot
                // model that type-erased ownership transfer and reports the shared state as a
                // leak; component tests exercise queued, failed, and released task lifetimes.
                // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
                Dependencies::BackgroundTask task = [state = std::move(state)]() noexcept {
                    state->run();
                };
                dependencies_.scheduleBackgroundTask(std::move(task));
            } else {
                auto state = std::make_unique<SourceDiskStatusTaskState>(
                    mailbox,
                    dependencies_.sourceFileMetadataProbe,
                    generation,
                    std::move(requests));
                QThreadPool::globalInstance()->start(
                    [state = std::move(state)]() noexcept { state->run(); });
            }
            sourceDiskStatusPollTimer_.start();
        } catch (...) {
            sourceDiskStatusCheckPending_ = false;
            sourceDiskStatusPollTimer_.stop();
            if (frozenComparison_ && !stopped_) {
                sourceDiskStatusRefreshTimer_.start(
                    static_cast<int>(kSourceDiskStatusRefreshMilliseconds));
            }
        }
    }

    void consumeSourceDiskStatusCompletion() noexcept {
        const std::shared_ptr<const SourceDiskStatusCompletion> completion =
            sourceDiskStatusMailbox_->tryTake();
        if (!completion || completion->generation != sourceDiskStatusGeneration_) {
            return;
        }
        sourceDiskStatusCheckPending_ = false;
        sourceDiskStatusPollTimer_.stop();
        if (stopped_) {
            return;
        }
        if (!completion->results.has_value()) {
            scheduleSourceDiskStatusRefresh();
            return;
        }
        changedOnDiskByPath_.clear();
        for (const SourceDiskStatusResult& result : *completion->results) {
            changedOnDiskByPath_.insert(result.key, result.changedOnDisk);
        }
        publishProjection();
    }

    void refresh() noexcept {
        if (stopped_ || !onOwnerThread()) {
            return;
        }

        try {
            std::optional<application::CommandTerminal> completedForeground;
            bool clearCandidateErrors = false;
            for (const application::CommandTerminal& terminal :
                 dependencies_.takeCompletedCommands()) {
                if (pendingCommand_.has_value() && terminal.context == *pendingCommand_) {
                    completedForeground = terminal;
                    clearCandidateErrors = pendingCommandClearsCandidateErrors_;
                    pendingCommand_.reset();
                    pendingCommandClearsCandidateErrors_ = false;
                }
                if (pendingNavigationCommand_.has_value() &&
                    terminal.context == *pendingNavigationCommand_) {
                    pendingNavigationCommand_.reset();
                }
                if (pendingTransportCommand_.has_value() &&
                    terminal.context == *pendingTransportCommand_) {
                    pendingTransportCommand_.reset();
                }
            }

            std::shared_ptr<const application::SessionSnapshot> snapshot = dependencies_.snapshot();
            if (!snapshot || !snapshot->isConsistent()) {
                failClosed();
                return;
            }
            snapshot_ = std::move(snapshot);
            if (snapshot_->validatedComparison != frozenComparison_) {
                frozenComparison_ = snapshot_->validatedComparison;
                frozenIdentitiesByPath_.clear();
                changedOnDiskByPath_.clear();
                ++sourceDiskStatusGeneration_;
                sourceDiskStatusCheckPending_ = false;
                sourceDiskStatusPollTimer_.stop();
                sourceDiskStatusRefreshTimer_.stop();
                sourceDiskStatusTimer_.invalidate();
                if (frozenComparison_) {
                    for (const domain::ComparisonSource& source : frozenComparison_->sources()) {
                        const QString path =
                            QString::fromStdWString(source.descriptor.normalizedPath.wstring());
                        const QString key = QDir::cleanPath(path).toCaseFolded();
                        const auto& identity = source.descriptor.sourceIdentity;
                        if (identity.has_value()) {
                            frozenIdentitiesByPath_.insert(
                                key,
                                composeSourceIdentity(path,
                                                      static_cast<std::int64_t>(identity->byteSize),
                                                      identity->modifiedUtcMilliseconds));
                        } else {
                            frozenIdentitiesByPath_.insert(key, key);
                        }
                    }
                }
            }
            if (completedForeground.has_value() && clearCandidateErrors &&
                completedForeground->outcome == application::CommandOutcome::Succeeded) {
                candidateSourceAErrorKey_.clear();
                candidateSourceBErrorKey_.clear();
                candidateSourceCErrorKey_.clear();
            }
            publishProjection();
            if (completedForeground.has_value()) {
                const QString errorKey =
                    completedForeground->error.has_value()
                        ? QString::fromStdString(completedForeground->error->userMessageKey)
                        : QString{};
                Q_EMIT owner_.foregroundCommandFinished(
                    completedForeground->context.commandId.value(),
                    static_cast<int>(completedForeground->outcome),
                    errorKey);
            }
        } catch (...) {
            failClosed();
        }
    }

    void failClosed() noexcept {
        stopped_ = true;
        projectionTimer_.stop();
        sourceDiskStatusPollTimer_.stop();
        sourceDiskStatusRefreshTimer_.stop();
        ++sourceDiskStatusGeneration_;
        sourceDiskStatusCheckPending_ = false;
        pendingCommand_.reset();
        pendingNavigationCommand_.reset();
        pendingTransportCommand_.reset();
        publishProjection();
    }

    void publishProjection() noexcept {
        ReviewView next;
        next.sourceAErrorKey = candidateSourceAErrorKey_;
        next.sourceBErrorKey = candidateSourceBErrorKey_;
        next.sourceCErrorKey = candidateSourceCErrorKey_;
        if (snapshot_) {
            next.displayState = mapDisplayState(snapshot_->sessionState);
            next.graphicsReady = snapshot_->graphicsReady && !stopped_;
            next.currentFrame = snapshot_->displayedFrame.has_value()
                                    ? static_cast<qint64>(snapshot_->displayedFrame->value())
                                    : -1;
            next.totalFrames = static_cast<qulonglong>(snapshot_->canonicalFrameCount);
            if (snapshot_->validatedComparison &&
                snapshot_->validatedComparison->canonicalRate().has_value()) {
                next.oneSecondStepFrames = std::clamp(
                    qRound(snapshot_->validatedComparison->canonicalRate()->displayFps()), 1, 1000);
            }
            if (snapshot_->validatedComparison) {
                const std::vector<DecoderBackendState> decoderBackends = decoderBackendStates();
                next.canonicalSourceIndex =
                    static_cast<int>(snapshot_->validatedComparison->canonicalSourceId());
                const domain::MediaDescriptor& canonical =
                    snapshot_->validatedComparison->canonicalDescriptor();
                next.timingMode = timingModeName(canonical.timingConfidence);
                if (canonical.frameRate.has_value()) {
                    next.rationalFrameRate = QStringLiteral("%1/%2")
                                                 .arg(canonical.frameRate->numerator())
                                                 .arg(canonical.frameRate->denominator());
                    next.dropFrameTimecodeAvailable = supportsDropFrame(*canonical.frameRate);
                }
                for (const domain::ComparisonSource& source :
                     snapshot_->validatedComparison->sources()) {
                    const QString sourcePath =
                        QString::fromStdWString(source.descriptor.normalizedPath.wstring());
                    next.sourceUrls.push_back(QUrl::fromLocalFile(sourcePath));
                    const QString filename = QFileInfo{sourcePath}.fileName();
                    if (source.role == domain::ComparisonRole::kReference) {
                        next.referenceSourceIndex = static_cast<int>(source.id);
                    }
                    if (source.id == 0U) {
                        next.sourceAFilename = filename;
                    } else if (source.id == 1U) {
                        next.sourceBFilename = filename;
                    } else if (source.id == 2U) {
                        next.sourceCFilename = filename;
                    }
                    const domain::MediaDescriptor& descriptor = source.descriptor;
                    const auto backend = std::find_if(decoderBackends.begin(),
                                                      decoderBackends.end(),
                                                      [&source](const DecoderBackendState& state) {
                                                          return state.sourceId == source.id;
                                                      });
                    const QString decodeBackend =
                        backend == decoderBackends.end()
                            ? QStringLiteral("Initializing")
                            : (backend->d3d11Va ? QStringLiteral("D3D11VA")
                                                : QStringLiteral("Software"));
                    const QString decodeFallbackReason =
                        backend != decoderBackends.end()
                            ? QString::fromStdString(backend->fallbackReason)
                            : QString{};
                    const QString rate = descriptor.frameRate.has_value()
                                             ? QStringLiteral("%1/%2 fps")
                                                   .arg(descriptor.frameRate->numerator())
                                                   .arg(descriptor.frameRate->denominator())
                                             : QStringLiteral("VFR");
                    next.sourceMediaInfo.push_back(QVariantMap{
                        {QStringLiteral("label"), sourceName(source.id)},
                        {QStringLiteral("filename"), filename},
                        {QStringLiteral("width"), descriptor.extent.width},
                        {QStringLiteral("height"), descriptor.extent.height},
                        {QStringLiteral("rotationDegrees"), descriptor.rotationDegrees},
                        {QStringLiteral("sampleAspectNumerator"),
                         descriptor.sampleAspectRatio.numerator},
                        {QStringLiteral("sampleAspectDenominator"),
                         descriptor.sampleAspectRatio.denominator},
                        {QStringLiteral("frameRate"), rate},
                        {QStringLiteral("frameCount"), descriptor.frameCount.value},
                        {QStringLiteral("timingMode"), timingModeName(descriptor.timingConfidence)},
                        {QStringLiteral("codec"), QString::fromStdString(descriptor.codecId)},
                        {QStringLiteral("pixelFormat"),
                         QString::fromStdString(descriptor.pixelFormatId)},
                        {QStringLiteral("bitDepth"), descriptor.bitDepth},
                        {QStringLiteral("colorMatrix"),
                         colorMatrixName(descriptor.colorMetadata.matrix)},
                        {QStringLiteral("colorRange"),
                         colorRangeName(descriptor.colorMetadata.range)},
                        {QStringLiteral("decodeBackend"), decodeBackend},
                        {QStringLiteral("decodeFallbackReason"), decodeFallbackReason},
                        {QStringLiteral("role"),
                         source.id == snapshot_->validatedComparison->canonicalSourceId()
                             ? QStringLiteral("Canonical")
                             : (source.role == domain::ComparisonRole::kReference
                                    ? QStringLiteral("Reference")
                                    : QStringLiteral("Prediction"))},
                    });
                }
            }
            if (next.currentFrame >= 0) {
                next.currentTimecode = timecodeForFrame(next.currentFrame, false);
                if (snapshot_->canonicalTimeline.has_value()) {
                    auto converted = domain::canonicalFrameStartTime(
                        *snapshot_->canonicalTimeline,
                        domain::FrameId{static_cast<std::int64_t>(next.currentFrame)});
                    if (converted) {
                        next.currentMediaTime = converted.value().microseconds();
                    }
                }
            }
            next.playing = snapshot_->playbackState == domain::PlaybackState::kPlaying ||
                           snapshot_->playbackState == domain::PlaybackState::kBuffering;
            next.alignmentRequired = snapshot_->alignmentRequired;
            next.automaticAlignmentPending = snapshot_->automaticAlignmentPending;
            next.canConfirmAutomaticAlignment = snapshot_->canConfirmAutomaticAlignment;
            next.canUndoAutomaticAlignment = snapshot_->canUndoAutomaticAlignment;
            next.alignmentAnalysisRunning = snapshot_->alignmentAnalysisJobId.has_value();
            if (next.alignmentAnalysisRunning) {
                const qulonglong completed = snapshot_->alignmentAnalysisCompletedUnits;
                const qulonglong total = snapshot_->alignmentAnalysisWork.totalUnits;
                next.alignmentAnalysisProgress =
                    total == 0U ? 0.0 : static_cast<qreal>(completed) / static_cast<qreal>(total);
                if (total == 0U || !snapshot_->alignmentAnalysisPhase.has_value()) {
                    next.alignmentAnalysisStatus = QStringLiteral("Preparing alignment analysis…");
                } else {
                    const QString phase =
                        *snapshot_->alignmentAnalysisPhase ==
                                application::AlignmentAnalysisPhase::CollectingSignatures
                            ? QStringLiteral("Collecting signatures")
                            : QStringLiteral("Computing alignment");
                    next.alignmentAnalysisStatus = QStringLiteral("%1… %2%").arg(phase).arg(
                        qRound(next.alignmentAnalysisProgress * 100.0));
                }
            }

            QStringList mappingParts;
            for (const application::PresentedSourceState& source : snapshot_->presentedSources) {
                const QString currentSourceName = sourceName(source.sourceId);
                const bool missing = source.matchKind == application::FrameMatchKind::Missing;
                if (source.sourceId == 0U) {
                    next.sourceAMissing = missing;
                } else if (source.sourceId == 1U) {
                    next.sourceBMissing = missing;
                } else if (source.sourceId == 2U) {
                    next.sourceCMissing = missing;
                }
                if (missing) {
                    QString reason = QStringLiteral("alignment gap");
                    if (source.missingReason == application::MissingReason::BeforeSourceStart) {
                        reason = QStringLiteral("before source start");
                    } else if (source.missingReason == application::MissingReason::AfterSourceEnd) {
                        reason = QStringLiteral("after source end");
                    }
                    mappingParts.push_back(
                        QStringLiteral("%1: Missing frame (%2)").arg(currentSourceName, reason));
                } else if ((source.matchKind == application::FrameMatchKind::GlobalOffset ||
                            source.matchKind == application::FrameMatchKind::AutoAligned ||
                            source.matchKind == application::FrameMatchKind::ManualAnchor) &&
                           source.sourceFrameId.has_value()) {
                    const qint64 offset = next.currentFrame >= 0
                                              ? source.sourceFrameId->value() - next.currentFrame
                                              : 0;
                    QString origin = QStringLiteral("manual");
                    if (source.matchKind == application::FrameMatchKind::AutoAligned) {
                        origin = QStringLiteral("auto");
                    } else if (source.matchKind == application::FrameMatchKind::ManualAnchor) {
                        origin = QStringLiteral("anchor");
                    }
                    mappingParts.push_back(
                        QStringLiteral("%1: source frame %2 (%3 offset %4%5, %6%)")
                            .arg(currentSourceName)
                            .arg(source.sourceFrameId->value() + 1)
                            .arg(origin)
                            .arg(offset >= 0 ? QStringLiteral("+") : QString{})
                            .arg(offset)
                            .arg(qRound(source.alignmentConfidence * 100.0F)));
                    if (source.matchKind == application::FrameMatchKind::AutoAligned) {
                        next.autoAlignmentActive = true;
                    }
                }
            }
            next.frameMappingStatus = mappingParts.join(QStringLiteral("  |  "));
            QStringList estimateParts;
            for (const application::GlobalOffsetEstimate& estimate :
                 snapshot_->alignmentEstimates) {
                const QString currentSourceName = sourceName(estimate.sourceId);
                const QString signedOffset =
                    QStringLiteral("%1%2")
                        .arg(estimate.bestOffset >= 0 ? QStringLiteral("+") : QString{})
                        .arg(estimate.bestOffset);
                const int confidence = qRound(estimate.confidence * 100.0F);
                if (estimate.autoApplicable) {
                    const QString mode = snapshot_->automaticAlignmentPending
                                             ? QStringLiteral("proposal")
                                             : QStringLiteral("auto");
                    estimateParts.push_back(QStringLiteral("%1: %2 %3 (%4%)")
                                                .arg(currentSourceName, mode, signedOffset)
                                                .arg(confidence));
                } else {
                    estimateParts.push_back(
                        QStringLiteral("%1: suggested %2 (%3%, review manually)")
                            .arg(currentSourceName, signedOffset)
                            .arg(confidence));
                }
            }
            next.alignmentEstimateStatus = estimateParts.join(QStringLiteral("  |  "));
            QStringList sequenceParts;
            QVariantList lowConfidenceTimelineMarkers;
            qulonglong lowConfidenceMarkerOverflowCount = 0U;
            for (const application::SequenceAlignmentSummary& result :
                 snapshot_->sequenceAlignments) {
                const QString currentSourceName = sourceName(result.sourceId);
                QStringList anomalyParts;
                for (const application::SequenceAlignmentAnomaly& anomaly : result.anomalies) {
                    QString kind;
                    switch (anomaly.kind) {
                    case application::SequenceAlignmentAnomalyKind::TargetFrameMissing:
                        kind = QStringLiteral("missing");
                        break;
                    case application::SequenceAlignmentAnomalyKind::TargetFrameExtra:
                        kind = QStringLiteral("extra");
                        break;
                    case application::SequenceAlignmentAnomalyKind::TargetFrameDuplicate:
                        kind = QStringLiteral("duplicate");
                        break;
                    }
                    const std::optional<domain::FrameId> position =
                        anomaly.canonicalFrameId.has_value() ? anomaly.canonicalFrameId
                                                             : anomaly.sourceFrameId;
                    if (position.has_value()) {
                        appendTimelineMarker(next.alignmentTimelineMarkers,
                                             next.alignmentTimelineMarkerOverflowCount,
                                             *position,
                                             kind,
                                             currentSourceName);
                    }
                    anomalyParts.push_back(
                        position.has_value()
                            ? QStringLiteral("%1 @ %2").arg(kind).arg(position->value() + 1)
                            : kind);
                    if (anomalyParts.size() >= 3) {
                        break;
                    }
                }
                const QString mode = snapshot_->automaticAlignmentPending
                                         ? QStringLiteral("proposal")
                                         : (result.autoApplicable ? QStringLiteral("mapped")
                                                                  : QStringLiteral("suggested"));
                qsizetype reviewSegments = 0;
                qsizetype rejectedSegments = 0;
                for (const application::SequenceAlignmentSegment& segment : result.segments) {
                    if (segment.state == application::AlignmentSegmentState::Accepted) {
                        continue;
                    }
                    const bool rejected =
                        segment.state == application::AlignmentSegmentState::Rejected;
                    if (rejected) {
                        ++rejectedSegments;
                    } else {
                        ++reviewSegments;
                    }
                    appendTimelineMarker(next.alignmentTimelineMarkers,
                                         next.alignmentTimelineMarkerOverflowCount,
                                         segment.firstCanonicalFrame,
                                         rejected ? QStringLiteral("rejected-segment")
                                                  : QStringLiteral("review-segment"),
                                         currentSourceName,
                                         qRound(segment.p10Confidence * 100.0F));
                }
                QString segmentStatus;
                if (reviewSegments > 0 || rejectedSegments > 0) {
                    segmentStatus = QStringLiteral(", %1 review / %2 rejected segments")
                                        .arg(reviewSegments)
                                        .arg(rejectedSegments);
                }
                sequenceParts.push_back(QStringLiteral("%1: sequence %2, %3% confidence%4%5")
                                            .arg(currentSourceName)
                                            .arg(mode)
                                            .arg(qRound(result.confidence * 100.0F))
                                            .arg(anomalyParts.isEmpty()
                                                     ? QStringLiteral(", no local anomalies")
                                                     : QStringLiteral(", %1").arg(
                                                           anomalyParts.join(QStringLiteral(", "))))
                                            .arg(segmentStatus));
                for (const application::SequenceAlignmentLowConfidenceRun& run :
                     result.lowConfidenceRuns) {
                    appendTimelineMarker(lowConfidenceTimelineMarkers,
                                         lowConfidenceMarkerOverflowCount,
                                         run.firstCanonicalFrame,
                                         QStringLiteral("low-confidence"),
                                         currentSourceName,
                                         qRound(run.minimumConfidence * 100.0F));
                }
            }
            next.sequenceAlignmentStatus = sequenceParts.join(QStringLiteral("  |  "));
            next.alignmentTimelineMarkerOverflowCount += lowConfidenceMarkerOverflowCount;
            QStringList anchorParts;
            for (const application::SourceAlignmentAnchors& sourceAnchors :
                 snapshot_->manualAlignmentAnchors) {
                const QString currentSourceName = sourceName(sourceAnchors.sourceId);
                QStringList pairs;
                for (const application::ManualAlignmentAnchor& anchor : sourceAnchors.anchors) {
                    pairs.push_back(QStringLiteral("%1↔%2")
                                        .arg(anchor.canonicalFrameId.value() + 1)
                                        .arg(anchor.sourceFrameId.value() + 1));
                    appendTimelineMarker(next.alignmentTimelineMarkers,
                                         next.alignmentTimelineMarkerOverflowCount,
                                         anchor.canonicalFrameId,
                                         QStringLiteral("anchor"),
                                         currentSourceName);
                }
                anchorParts.push_back(
                    QStringLiteral("%1: anchors %2").arg(currentSourceName, pairs.join(", ")));
            }
            next.manualAnchorStatus = anchorParts.join(QStringLiteral("  |  "));
            next.manualAnchorActive = !snapshot_->manualAlignmentAnchors.empty();
            for (const QVariant& marker : lowConfidenceTimelineMarkers) {
                if (next.alignmentTimelineMarkers.size() >= kMaximumAlignmentTimelineMarkers) {
                    ++next.alignmentTimelineMarkerOverflowCount;
                    continue;
                }
                next.alignmentTimelineMarkers.push_back(marker);
            }
            for (const application::CompatibilityFindingView& finding :
                 snapshot_->compatibilityFindings) {
                QVariantList sourceIds;
                sourceIds.reserve(static_cast<qsizetype>(finding.sources.size()));
                for (const domain::SourceId sourceId : finding.sources) {
                    sourceIds.push_back(QVariant::fromValue<qulonglong>(sourceId));
                }
                const std::string_view stableCode = domain::stableId(finding.code);
                next.compatibilityFindings.push_back(QVariantMap{
                    {QStringLiteral("severity"), static_cast<int>(finding.severity)},
                    {QStringLiteral("code"),
                     QString::fromLatin1(stableCode.data(),
                                         static_cast<qsizetype>(stableCode.size()))},
                    {QStringLiteral("sources"), sourceIds},
                });
            }

            if (snapshot_->lastError.has_value()) {
                const QString key = QString::fromStdString(snapshot_->lastError->userMessageKey);
                next.lastErrorTechnicalDetail =
                    QString::fromStdString(snapshot_->lastError->technicalDetail);
                const std::optional<domain::SourceId> errorSource = snapshot_->lastError->source;
                if (errorSource.has_value() && *errorSource == 0U) {
                    if (next.sourceAErrorKey.isEmpty()) {
                        next.sourceAErrorKey = key;
                    }
                } else if (errorSource.has_value() && *errorSource == 1U) {
                    if (next.sourceBErrorKey.isEmpty()) {
                        next.sourceBErrorKey = key;
                    }
                } else if (errorSource.has_value() && *errorSource == 2U) {
                    if (next.sourceCErrorKey.isEmpty()) {
                        next.sourceCErrorKey = key;
                    }
                } else {
                    next.pairErrorKey = key;
                }
            }
        }

        scheduleSourceDiskStatusRefresh();

        std::vector<SourceListRow> sourceRows;
        if (snapshot_) {
            sourceRows.reserve(snapshot_->sources.size());
            for (const application::SessionSourceView& source : snapshot_->sources) {
                const auto presented =
                    std::find_if(snapshot_->presentedSources.begin(),
                                 snapshot_->presentedSources.end(),
                                 [&source](const application::PresentedSourceState& value) {
                                     return value.sourceId == source.sourceId;
                                 });
                QString errorKey;
                if (source.sourceId == 0U) {
                    errorKey = next.sourceAErrorKey;
                } else if (source.sourceId == 1U) {
                    errorKey = next.sourceBErrorKey;
                } else if (source.sourceId == 2U) {
                    errorKey = next.sourceCErrorKey;
                }
                SourceListRow row{
                    .sourceId = source.sourceId,
                    .role = static_cast<int>(source.role),
                    .filename = QString::fromStdString(source.displayName),
                    .errorKey = std::move(errorKey),
                };
                if (snapshot_->validatedComparison) {
                    const auto sourceDescriptor =
                        std::find_if(snapshot_->validatedComparison->sources().begin(),
                                     snapshot_->validatedComparison->sources().end(),
                                     [&source](const domain::ComparisonSource& value) {
                                         return value.id == source.sourceId;
                                     });
                    if (sourceDescriptor != snapshot_->validatedComparison->sources().end()) {
                        const QString path = QString::fromStdWString(
                            sourceDescriptor->descriptor.normalizedPath.wstring());
                        const QString frozenKey = QDir::cleanPath(path).toCaseFolded();
                        const auto frozenIt = frozenIdentitiesByPath_.constFind(frozenKey);
                        if (frozenIt != frozenIdentitiesByPath_.cend()) {
                            row.sourceIdentity = *frozenIt;
                        } else {
                            // A validated descriptor normally has a frozen identity. Keep the
                            // projection filesystem-free when an incomplete adapter omits it.
                            row.sourceIdentity = QDir::cleanPath(path).toCaseFolded();
                        }
                        const auto changedOnDisk = changedOnDiskByPath_.constFind(frozenKey);
                        if (changedOnDisk != changedOnDiskByPath_.cend()) {
                            row.changedOnDisk = *changedOnDisk;
                        }
                    }
                }
                if (presented != snapshot_->presentedSources.end()) {
                    row.currentSourceFrame =
                        presented->sourceFrameId.has_value()
                            ? std::optional<qint64>{presented->sourceFrameId->value()}
                            : std::nullopt;
                    row.matchKind = static_cast<int>(presented->matchKind);
                    row.confidence = presented->alignmentConfidence;
                    row.missing = !presented->sourceFrameId.has_value();
                    row.missingReason = presented->missingReason.has_value()
                                            ? static_cast<int>(*presented->missingReason)
                                            : -1;
                    if (presented->matchKind == application::FrameMatchKind::GlobalOffset &&
                        presented->sourceFrameId.has_value() && next.currentFrame >= 0) {
                        row.manualOffset = presented->sourceFrameId->value() - next.currentFrame;
                    }
                }
                sourceRows.push_back(std::move(row));
            }
        }
        for (std::size_t first = 0U; first < sourceRows.size(); ++first) {
            for (std::size_t second = first + 1U; second < sourceRows.size(); ++second) {
                const int preferenceValue = first == 0U && second == 1U ? 0 : (first == 0U ? 1 : 2);
                next.differenceEdges.push_back(QVariantMap{
                    {QStringLiteral("label"),
                     QStringLiteral("%1 ↔ %2").arg(sourceName(sourceRows[first].sourceId),
                                                   sourceName(sourceRows[second].sourceId))},
                    {QStringLiteral("preferenceValue"), preferenceValue},
                    {QStringLiteral("firstSourceId"),
                     QVariant::fromValue<qulonglong>(sourceRows[first].sourceId)},
                    {QStringLiteral("secondSourceId"),
                     QVariant::fromValue<qulonglong>(sourceRows[second].sourceId)},
                    {QStringLiteral("exactness"),
                     static_cast<int>(
                         snapshot_ ? application::comparisonExactness(*snapshot_,
                                                                      sourceRows[first].sourceId,
                                                                      sourceRows[second].sourceId)
                                   : application::ComparisonExactness::Unavailable)},
                });
            }
        }
        next.sourceCount = static_cast<int>(sourceRows.size());
        sourceModel_.setRows(std::move(sourceRows));

        next.busy = pendingCommand_.has_value() && !stopped_;
        next.framePending = !stopped_ && (pendingNavigationCommand_.has_value() ||
                                          (snapshot_ && snapshot_->requestedFrame.has_value() &&
                                           snapshot_->requestedFrame != snapshot_->displayedFrame));
        const bool transportPending = pendingTransportCommand_.has_value();
        const bool playbackDraining =
            snapshot_ && !next.playing && snapshot_->requestedFrame.has_value();
        const bool playbackBlocksCommands = next.playing || playbackDraining || transportPending;
        next.canOpen = next.graphicsReady && !next.busy && !playbackBlocksCommands;
        // Navigation stays available while playing or while an earlier navigation is in flight:
        // the coordinator pauses playback on the first frame step and coalesces rapid presses
        // onto the newest target (USERPLAN 3.1/6.2). Opens remain gated by transport activity.
        const bool canNavigate = next.graphicsReady && !next.busy &&
                                 next.displayState == ReviewDisplayState::Ready &&
                                 next.totalFrames != 0U;
        next.canFirst = canNavigate;
        next.canLast = canNavigate;
        next.canPrevious = canNavigate && next.currentFrame > 0;
        // Navigation is allowed up to the newest requested target, not only the last displayed
        // frame, so a burst of +1 presses that has been queued but not yet presented does not
        // repeatedly submit boundary commands once the displayed frame nears the end
        // (USERPLAN 3.1).
        const std::optional<domain::FrameId> navigationTarget =
            snapshot_->requestedFrame.has_value() ? snapshot_->requestedFrame
                                                  : snapshot_->displayedFrame;
        const qint64 navigationBase = navigationTarget.has_value() ? navigationTarget->value() : -1;
        next.canNext = canNavigate && navigationBase >= 0 &&
                       static_cast<qulonglong>(navigationBase) + 1U < next.totalFrames;
        next.canPlay = next.graphicsReady && !next.busy && !playbackBlocksCommands &&
                       next.displayState == ReviewDisplayState::Ready && next.currentFrame >= 0 &&
                       next.totalFrames > 1U;
        next.canPause = !next.busy && !transportPending && next.playing;

        const bool changed = !(next == view_);
        const bool frameStateChanged = !next.sameFrameState(view_);
        const bool otherStateChanged = changed && (!frameStateChanged || [&] {
                                           ReviewView normalized = next;
                                           normalized.currentFrame = view_.currentFrame;
                                           normalized.sourceAMissing = view_.sourceAMissing;
                                           normalized.sourceBMissing = view_.sourceBMissing;
                                           normalized.sourceCMissing = view_.sourceCMissing;
                                           normalized.frameMappingStatus = view_.frameMappingStatus;
                                           normalized.autoAlignmentActive =
                                               view_.autoAlignmentActive;
                                           normalized.differenceEdges = view_.differenceEdges;
                                           normalized.canPrevious = view_.canPrevious;
                                           normalized.canNext = view_.canNext;
                                           return !(normalized == view_);
                                       }());
        if (changed) {
            view_ = std::move(next);
        }
        if (otherStateChanged) {
            Q_EMIT owner_.stateChanged();
        }
        if (frameStateChanged) {
            Q_EMIT owner_.frameStateChanged();
        }
    }

    [[nodiscard]] std::optional<application::CommandContext> allocateCommandContext() noexcept {
        if (!snapshot_ || commandIdsExhausted_) {
            return std::nullopt;
        }
        const std::uint64_t commandId = nextCommandId_;
        if (nextCommandId_ == (std::numeric_limits<std::uint64_t>::max)()) {
            commandIdsExhausted_ = true;
        } else {
            ++nextCommandId_;
        }
        return application::CommandContext{
            .sessionId = snapshot_->sessionId,
            .sessionEpoch = snapshot_->sessionEpoch,
            .commandId = domain::CommandId{commandId},
        };
    }

    [[nodiscard]] bool dispatch(application::PlaybackCommand command) noexcept {
        const application::CommandContext context = application::commandContext(command);
        const bool clearsCandidateErrors =
            std::holds_alternative<application::OpenComparisonCommand>(command) ||
            std::holds_alternative<application::CloseSessionCommand>(command);
        application::PortSubmitResult result = application::PortSubmitResult::Closed;
        try {
            result = dependencies_.submit(std::move(command));
        } catch (...) {
            failClosed();
            return false;
        }
        if (result != application::PortSubmitResult::Accepted) {
            return false;
        }
        pendingCommand_ = context;
        pendingCommandClearsCandidateErrors_ = clearsCandidateErrors;
        lastSubmittedCommandId_ = context.commandId.value();
        publishProjection();
        return true;
    }

    template <typename Enabled, typename Factory>
    [[nodiscard]] bool dispatchNavigation(Enabled enabled, Factory factory) {
        if (!onOwnerThread() || stopped_) {
            return false;
        }
        refresh();
        if (stopped_) {
            return false;
        }

        if (!enabled(view_)) {
            return false;
        }
        const std::optional<application::CommandContext> context = allocateCommandContext();
        if (!context.has_value()) {
            failClosed();
            return false;
        }
        application::PlaybackCommand command = factory(*context);
        application::PortSubmitResult result = application::PortSubmitResult::Closed;
        try {
            result = dependencies_.submit(std::move(command));
        } catch (...) {
            failClosed();
            return false;
        }
        if (result != application::PortSubmitResult::Accepted) {
            return false;
        }
        pendingNavigationCommand_ = *context;
        publishProjection();
        return true;
    }

    template <typename Enabled, typename Factory>
    [[nodiscard]] bool dispatchStateChange(Enabled enabled, Factory factory) {
        if (!onOwnerThread() || stopped_) {
            return false;
        }
        refresh();
        if (stopped_ || !enabled(view_)) {
            return false;
        }
        const std::optional<application::CommandContext> context = allocateCommandContext();
        if (!context.has_value()) {
            failClosed();
            return false;
        }
        return dispatch(factory(*context));
    }

    template <typename Enabled, typename Factory>
    [[nodiscard]] bool dispatchBackgroundAnalysis(Enabled enabled, Factory factory) {
        if (!onOwnerThread() || stopped_) {
            return false;
        }
        refresh();
        if (stopped_ || !enabled(view_)) {
            return false;
        }
        const std::optional<application::CommandContext> context = allocateCommandContext();
        if (!context.has_value()) {
            failClosed();
            return false;
        }
        try {
            return dependencies_.submit(factory(*context)) ==
                   application::PortSubmitResult::Accepted;
        } catch (...) {
            failClosed();
            return false;
        }
    }

    template <typename Enabled, typename Factory>
    [[nodiscard]] bool dispatchTransport(Enabled enabled, Factory factory) {
        if (!onOwnerThread() || stopped_) {
            return false;
        }
        refresh();
        if (stopped_ || !enabled(view_)) {
            return false;
        }
        const std::optional<application::CommandContext> context = allocateCommandContext();
        if (!context.has_value()) {
            failClosed();
            return false;
        }

        application::PlaybackCommand command = factory(*context);
        application::PortSubmitResult result = application::PortSubmitResult::Closed;
        try {
            result = dependencies_.submit(std::move(command));
        } catch (...) {
            failClosed();
            return false;
        }
        if (result != application::PortSubmitResult::Accepted) {
            return false;
        }
        pendingTransportCommand_ = *context;
        publishProjection();
        return true;
    }

    ReviewController& owner_;
    Dependencies dependencies_;
    SourceListModel sourceModel_;
    QTimer projectionTimer_;
    QTimer sourceDiskStatusPollTimer_;
    QTimer sourceDiskStatusRefreshTimer_;
    std::shared_ptr<const application::SessionSnapshot> snapshot_;
    QString candidateSourceAErrorKey_;
    QString candidateSourceBErrorKey_;
    QString candidateSourceCErrorKey_;
    std::optional<application::CommandContext> pendingCommand_;
    bool pendingCommandClearsCandidateErrors_ = false;
    std::optional<application::CommandContext> pendingNavigationCommand_;
    std::optional<application::CommandContext> pendingTransportCommand_;
    std::uint64_t nextCommandId_ = 1U;
    bool commandIdsExhausted_ = false;
    std::uint64_t lastSubmittedCommandId_ = 0U;
    bool stopped_ = false;
    ReviewView view_;
    std::shared_ptr<const domain::ValidatedComparisonSet> frozenComparison_;
    QHash<QString, QString> frozenIdentitiesByPath_;
    QHash<QString, bool> changedOnDiskByPath_;
    QElapsedTimer sourceDiskStatusTimer_;
    std::uint64_t sourceDiskStatusGeneration_ = 0U;
    bool sourceDiskStatusCheckPending_ = false;
    std::shared_ptr<SourceDiskStatusMailbox> sourceDiskStatusMailbox_ =
        std::make_shared<SourceDiskStatusMailbox>();
};

ReviewController::ReviewController(Dependencies dependencies, QObject* const parent)
    : QObject(parent), impl_(std::make_unique<Impl>(*this, std::move(dependencies))) {}

ReviewController::~ReviewController() = default;

QString ReviewController::sourceAFilename() const {
    return impl_->view().sourceAFilename;
}

QString ReviewController::sourceBFilename() const {
    return impl_->view().sourceBFilename;
}

QString ReviewController::sourceCFilename() const {
    return impl_->view().sourceCFilename;
}

QVariantList ReviewController::sourceUrls() const {
    return impl_->view().sourceUrls;
}

QVariantList ReviewController::activeSources() const {
    return impl_->view().sourceUrls;
}

QAbstractItemModel* ReviewController::sources() const noexcept {
    return impl_->sources();
}

int ReviewController::sourceCount() const noexcept {
    return impl_->sources()->rowCount();
}

int ReviewController::canonicalSourceIndex() const noexcept {
    return impl_->view().canonicalSourceIndex;
}

int ReviewController::referenceSourceIndex() const noexcept {
    return impl_->view().referenceSourceIndex;
}

ReviewController::ReviewDisplayState ReviewController::displayState() const noexcept {
    return impl_->view().displayState;
}

bool ReviewController::busy() const noexcept {
    return impl_->view().busy;
}

bool ReviewController::framePending() const noexcept {
    return impl_->view().framePending;
}

bool ReviewController::playing() const noexcept {
    return impl_->view().playing;
}

bool ReviewController::graphicsReady() const noexcept {
    return impl_->view().graphicsReady;
}

qint64 ReviewController::currentFrame() const noexcept {
    return impl_->view().currentFrame;
}

qulonglong ReviewController::totalFrames() const noexcept {
    return impl_->view().totalFrames;
}

int ReviewController::oneSecondStepFrames() const noexcept {
    return impl_->view().oneSecondStepFrames;
}

qint64 ReviewController::currentMediaTime() const noexcept {
    return impl_->view().currentMediaTime;
}

QString ReviewController::currentTimecode() const {
    return impl_->view().currentTimecode;
}

QString ReviewController::rationalFrameRate() const {
    return impl_->view().rationalFrameRate;
}

QString ReviewController::timingMode() const {
    return impl_->view().timingMode;
}

bool ReviewController::dropFrameTimecodeAvailable() const noexcept {
    return impl_->view().dropFrameTimecodeAvailable;
}

QVariantList ReviewController::sourceMediaInfo() const {
    return impl_->view().sourceMediaInfo;
}

QString ReviewController::sourceAErrorKey() const {
    return impl_->view().sourceAErrorKey;
}

QString ReviewController::sourceBErrorKey() const {
    return impl_->view().sourceBErrorKey;
}

QString ReviewController::sourceCErrorKey() const {
    return impl_->view().sourceCErrorKey;
}

bool ReviewController::sourceAMissing() const noexcept {
    return impl_->view().sourceAMissing;
}

bool ReviewController::sourceBMissing() const noexcept {
    return impl_->view().sourceBMissing;
}

bool ReviewController::sourceCMissing() const noexcept {
    return impl_->view().sourceCMissing;
}

QString ReviewController::pairErrorKey() const {
    return impl_->view().pairErrorKey;
}

QString ReviewController::lastErrorTechnicalDetail() const {
    return impl_->view().lastErrorTechnicalDetail;
}

QString ReviewController::frameMappingStatus() const {
    return impl_->view().frameMappingStatus;
}

QString ReviewController::alignmentEstimateStatus() const {
    return impl_->view().alignmentEstimateStatus;
}

QString ReviewController::sequenceAlignmentStatus() const {
    return impl_->view().sequenceAlignmentStatus;
}

bool ReviewController::alignmentAnalysisRunning() const noexcept {
    return impl_->view().alignmentAnalysisRunning;
}

qreal ReviewController::alignmentAnalysisProgress() const noexcept {
    return impl_->view().alignmentAnalysisProgress;
}

QString ReviewController::alignmentAnalysisStatus() const {
    return impl_->view().alignmentAnalysisStatus;
}

QString ReviewController::manualAnchorStatus() const {
    return impl_->view().manualAnchorStatus;
}

QVariantList ReviewController::alignmentTimelineMarkers() const {
    return impl_->view().alignmentTimelineMarkers;
}

qulonglong ReviewController::alignmentTimelineMarkerOverflowCount() const noexcept {
    return impl_->view().alignmentTimelineMarkerOverflowCount;
}

bool ReviewController::manualAnchorActive() const noexcept {
    return impl_->view().manualAnchorActive;
}

bool ReviewController::autoAlignmentActive() const noexcept {
    return impl_->view().autoAlignmentActive;
}

bool ReviewController::alignmentRequired() const noexcept {
    return impl_->view().alignmentRequired;
}

bool ReviewController::automaticAlignmentPending() const noexcept {
    return impl_->view().automaticAlignmentPending;
}

bool ReviewController::canConfirmAutomaticAlignment() const noexcept {
    return impl_->view().canConfirmAutomaticAlignment;
}

bool ReviewController::canUndoAutomaticAlignment() const noexcept {
    return impl_->view().canUndoAutomaticAlignment;
}

QVariantList ReviewController::compatibilityFindings() const {
    return impl_->view().compatibilityFindings;
}

QVariantList ReviewController::differenceEdges() const {
    return impl_->view().differenceEdges;
}

bool ReviewController::canOpen() const noexcept {
    return impl_->view().canOpen;
}

bool ReviewController::canFirst() const noexcept {
    return impl_->view().canFirst;
}

bool ReviewController::canPrevious() const noexcept {
    return impl_->view().canPrevious;
}

bool ReviewController::canNext() const noexcept {
    return impl_->view().canNext;
}

bool ReviewController::canLast() const noexcept {
    return impl_->view().canLast;
}

bool ReviewController::canPlay() const noexcept {
    return impl_->view().canPlay;
}

bool ReviewController::canPause() const noexcept {
    return impl_->view().canPause;
}

qulonglong ReviewController::lastSubmittedCommandId() const noexcept {
    return impl_->lastSubmittedCommandId();
}

bool ReviewController::openComparison(const QUrl& first, const QUrl& second) {
    return impl_->openComparison(first, second);
}

bool ReviewController::openSources(const QVariantList& urls, const int referenceSourceIndex) {
    return impl_->openSources(
        urls, referenceSourceIndex, false, application::OpenReviewIntent::NewReview);
}

bool ReviewController::reopenSources(const QVariantList& urls, const int referenceSourceIndex) {
    return impl_->openSources(
        urls, referenceSourceIndex, true, application::OpenReviewIntent::ReplaceSources);
}

bool ReviewController::changeReference(const int sourceIndex) {
    return impl_->changeReference(sourceIndex);
}

bool ReviewController::openComparisonSet(const QUrl& first,
                                         const QUrl& second,
                                         const QUrl& third,
                                         const int referenceSourceIndex) {
    return impl_->openComparisonSet(first, second, third, referenceSourceIndex);
}

bool ReviewController::closeSources() {
    return impl_->closeSources();
}

void ReviewController::clearCandidateSourceErrors() noexcept {
    impl_->clearCandidateSourceErrors();
}

QVariantMap ReviewController::handleDroppedUrls(const QVariantList& urls) const {
    if (urls.isEmpty()) {
        return rejectedDrop(QStringLiteral("drop-empty"));
    }
    if (urls.size() > 3) {
        return rejectedDrop(QStringLiteral("drop-too-many"));
    }

    QVariantList normalizedUrls;
    normalizedUrls.reserve(urls.size());
    QSet<QString> uniquePaths;

    for (const QVariant& value : urls) {
        const LocalFileValidation validated = validateLocalFile(value.toUrl());
        if (!validated.candidate.has_value()) {
            return rejectedDrop(validated.errorKey == QStringLiteral("source-missing")
                                    ? QStringLiteral("drop-missing")
                                    : QStringLiteral("drop-invalid-local"),
                                value.toString());
        }

        const LocalFileCandidate& candidate = *validated.candidate;
        const QString canonicalPath = QString::fromStdWString(candidate.path.wstring());
        const QString comparisonPath = canonicalPath.toCaseFolded();
        if (uniquePaths.contains(comparisonPath)) {
            return rejectedDrop(QStringLiteral("drop-duplicate"), candidate.filename);
        }
        uniquePaths.insert(comparisonPath);
        normalizedUrls.push_back(QUrl::fromLocalFile(canonicalPath));
    }

    bool allImages = true;
    for (const QVariant& value : urls) {
        if (!isStillImageFile(value.toUrl())) {
            allImages = false;
            break;
        }
    }

    return QVariantMap{
        {QStringLiteral("accepted"), true},
        {QStringLiteral("kind"), allImages ? QStringLiteral("images") : QStringLiteral("videos")},
        {QStringLiteral("urls"), normalizedUrls},
    };
}

bool ReviewController::first() {
    return impl_->first();
}

bool ReviewController::previous() {
    return impl_->previous();
}

bool ReviewController::next() {
    return impl_->next();
}

bool ReviewController::last() {
    return impl_->last();
}

bool ReviewController::stepFrames(const qint64 delta) {
    return impl_->stepFrames(delta);
}

bool ReviewController::seekFrame(const qint64 frame) {
    return impl_->seekFrame(frame);
}

QString ReviewController::timecodeForFrame(const qint64 frame, const bool dropFrame) const {
    return impl_->timecodeForFrame(frame, dropFrame);
}

qint64 ReviewController::mediaTimeForFrame(const qint64 frame) const {
    return impl_->mediaTimeForFrame(frame);
}

qint64 ReviewController::frameForMediaTime(const qint64 microseconds) const {
    return impl_->frameForMediaTime(microseconds);
}

bool ReviewController::applyAlignmentOffsets(const qint64 sourceAFrames,
                                             const qint64 sourceBFrames,
                                             const qint64 sourceCFrames) {
    return impl_->applyAlignmentOffsets(sourceAFrames, sourceBFrames, sourceCFrames);
}

bool ReviewController::applySourceOffsets(const QVariantList& offsets) {
    return impl_->applySourceOffsets(offsets);
}

bool ReviewController::estimateAlignment() {
    return impl_->estimateAlignment();
}

bool ReviewController::analyzeSequenceAlignment() {
    return impl_->analyzeSequenceAlignment();
}

bool ReviewController::cancelAlignmentAnalysis() {
    return impl_->cancelAlignmentAnalysis();
}

bool ReviewController::confirmAutomaticAlignment() {
    return impl_->confirmAutomaticAlignment();
}

bool ReviewController::undoAutomaticAlignment() {
    return impl_->undoAutomaticAlignment();
}

bool ReviewController::setManualAlignmentAnchor(const int sourceIndex,
                                                const qint64 canonicalFrame,
                                                const qint64 sourceFrame) {
    return impl_->setManualAlignmentAnchor(sourceIndex, canonicalFrame, sourceFrame);
}

bool ReviewController::clearManualAlignmentAnchors() {
    return impl_->clearManualAlignmentAnchors();
}

bool ReviewController::play() {
    return impl_->play();
}

bool ReviewController::pause() {
    return impl_->pause();
}

bool ReviewController::togglePlayback() {
    return impl_->togglePlayback();
}

void ReviewController::refreshProjection() noexcept {
    impl_->refreshProjection();
}

QString ReviewController::frozenSourceIdentity(const QUrl& source) const {
    return impl_->frozenSourceIdentity(source);
}

void ReviewController::stop() noexcept {
    if (thread() != QThread::currentThread()) {
        static_cast<void>(QMetaObject::invokeMethod(this, "stop", Qt::QueuedConnection));
        return;
    }
    impl_->stop();
}

bool ReviewController::waitForSourceDiskStatusIdle(
    const std::chrono::milliseconds timeout) noexcept {
    return impl_->waitForSourceDiskStatusIdle(timeout);
}

} // namespace dvs::ui
