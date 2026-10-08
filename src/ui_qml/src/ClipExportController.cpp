#include "dvs/ui/ClipExportController.h"

#include "dvs/domain/ValidatedComparisonSet.h"

#include <QDir>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace dvs::ui {
namespace {

[[nodiscard]] QString nativePath(const std::filesystem::path& path) {
    return QDir::toNativeSeparators(QDir::cleanPath(QString::fromStdWString(path.wstring())));
}

[[nodiscard]] application::ClipExportReport
makeReport(const application::ClipExportRequestId requestId,
           const application::ClipExportOutcome outcome,
           std::string detail) {
    application::ClipExportReport report{};
    report.requestId = requestId;
    report.outcome = outcome;
    report.technicalDetail = std::move(detail);
    return report;
}

} // namespace

ClipExportController::ClipExportController(Dependencies dependencies, QObject* parent)
    : QObject(parent), dependencies_(std::move(dependencies)) {
    lastAvailability_ = canExport();
}

ClipExportController::~ClipExportController() {
    // Join the worker before any member dies: runExport() touches cancel_ and posts lambdas that
    // capture this, and the queued ones still carry the request id they will compare against.
    cancel_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool ClipExportController::busy() const noexcept {
    return busy_;
}

qreal ClipExportController::progress() const noexcept {
    return progress_;
}

bool ClipExportController::canExport() const {
    return makeRequest({}).has_value();
}

void ClipExportController::refreshAvailability() {
    const bool available = canExport();
    if (available != lastAvailability_) {
        lastAvailability_ = available;
        emit availabilityChanged();
    }
}

QString ClipExportController::suggestedFileName() const {
    const auto request = displayRequest();
    if (!request.has_value()) {
        return {};
    }
    const QString stem = QString::fromStdWString(request->sourcePath.stem().wstring());
    QString extension = QString::fromStdWString(request->sourcePath.extension().wstring());
    if (extension.isEmpty()) {
        // The muxer is guessed from the extension; keep the source container whenever possible.
        extension = QStringLiteral(".mp4");
    }
    return tr("%1_clip_%2-%3%4")
        .arg(stem)
        .arg(request->range.inInclusive.value() + 1)
        .arg(request->range.outInclusive.value() + 1)
        .arg(extension);
}

QString ClipExportController::rangeSummary() const {
    const auto request = displayRequest();
    if (!request.has_value()) {
        return {};
    }
    const std::int64_t frameCount =
        request->range.outInclusive.value() - request->range.inInclusive.value() + 1;
    return tr("入 %1 · 出 %2 · %3 帧")
        .arg(request->range.inInclusive.value() + 1)
        .arg(request->range.outInclusive.value() + 1)
        .arg(frameCount);
}

QString ClipExportController::sourcePath() const {
    const auto request = displayRequest();
    return request.has_value() ? nativePath(request->sourcePath) : QString{};
}

QString ClipExportController::lastStatus() const {
    return lastStatus_;
}

QString ClipExportController::lastFailureDetail() const {
    return lastFailureDetail_;
}

QString ClipExportController::lastOutputPath() const {
    return lastOutputPath_;
}

QUrl ClipExportController::suggestedTarget() const {
    const auto request = displayRequest();
    if (!request.has_value()) {
        return {};
    }
    const QString folder = QString::fromStdWString(request->sourcePath.parent_path().wstring());
    return QUrl::fromLocalFile(QDir{folder}.filePath(suggestedFileName()));
}

bool ClipExportController::exportRange(const QUrl& target) {
    if (busy_) {
        setStatus(tr("已有导出任务在运行。"));
        return false;
    }
    if (!target.isLocalFile() || target.toLocalFile().isEmpty()) {
        setStatus(tr("请选择导出文件的位置。"));
        return false;
    }
    auto request = makeRequest(target.toLocalFile().toStdWString());
    if (!request.has_value()) {
        setStatus(tr("当前没有可导出的区间：请确认视频会话与入点、出点有效。"));
        return false;
    }

    request->id = ++nextRequestId_;
    pendingOutputPath_ = nativePath(request->targetPath);
    activeRequest_ = request;
    activeRequestId_ = request->id;
    cancel_.store(false);
    lastFailureDetail_.clear();
    lastOutputPath_.clear();
    setProgress(0.0);
    setBusy(true);
    setStatus(tr("正在导出所选区间…"));

    // One worker per export. The request is fully captured here, so the thread never touches the
    // session snapshot; cancel_ is the only shared state and it is atomic.
    if (worker_.joinable()) {
        worker_.join(); // Defensive: busy_ already gates this, so the join never blocks.
    }
    worker_ = std::jthread([this, captured = std::move(*request)] { runExport(captured); });
    return true;
}

void ClipExportController::cancelExport() {
    if (!busy_) {
        return;
    }
    cancel_.store(true);
    setStatus(tr("正在取消导出…"));
}

void ClipExportController::runExport(const Request& request) noexcept {
    try {
        runExportBody(request);
    } catch (...) {
        // The worker is noexcept; an escaped exception would terminate the process. Turn any
        // unexpected failure into a failed report instead.
        postFinished(
            makeReport(request.id, application::ClipExportOutcome::kFailed, std::string{}));
    }
}

void ClipExportController::runExportBody(const Request& request) {
    application::IClipExporter& exporter = *dependencies_.exporter;

    // An empty keyframe table means both "canceled" and "no keyframe information"; the flag
    // disambiguates (the same rule the IClipExporter contract documents).
    const std::vector<std::int64_t> keyframes = exporter.keyframeTimes(request.sourcePath, cancel_);
    if (cancel_.load()) {
        postFinished(makeReport(request.id, application::ClipExportOutcome::kCanceled, {}));
        return;
    }
    if (keyframes.empty()) {
        postFinished(makeReport(request.id,
                                application::ClipExportOutcome::kFailed,
                                "The source exposes no keyframe information, so a lossless clip "
                                "cannot be aligned."));
        return;
    }
    const auto plan =
        application::planClipExport(*request.timeline, request.canonicalFrameCount, request.range);
    if (!plan.hasValue()) {
        postFinished(makeReport(
            request.id, application::ClipExportOutcome::kFailed, plan.error().technicalDetail));
        return;
    }
    const auto aligned =
        application::alignClipExportStart(*request.timeline, plan.value(), keyframes);
    if (!aligned.hasValue()) {
        postFinished(makeReport(
            request.id, application::ClipExportOutcome::kFailed, aligned.error().technicalDetail));
        return;
    }

    application::ClipExportJob job{};
    job.requestId = request.id;
    job.sessionId = request.sessionId;
    job.sessionEpoch = request.sessionEpoch;
    job.sourcePath = request.sourcePath;
    job.outputPath = request.targetPath;
    job.plan = aligned.value();
    const auto requestId = request.id;
    job.progress = [this, requestId](const double value) {
        postProgress(requestId, static_cast<qreal>(value));
    };
    postFinished(exporter.perform(job, cancel_));
}

void ClipExportController::postProgress(const application::ClipExportRequestId requestId,
                                        const qreal value) {
    QMetaObject::invokeMethod(
        this,
        [this, requestId, value]() { applyProgress(requestId, value); },
        Qt::QueuedConnection);
}

void ClipExportController::postFinished(application::ClipExportReport report) {
    QMetaObject::invokeMethod(
        this,
        [this, captured = std::move(report)]() { applyFinished(captured); },
        Qt::QueuedConnection);
}

void ClipExportController::applyProgress(const application::ClipExportRequestId requestId,
                                         const qreal value) {
    if (requestId != activeRequestId_ || !busy_) {
        return;
    }
    // Monotone: packet progress can stall or repeat, and the bar should never move backwards.
    if (value > progress_) {
        setProgress(std::clamp(value, 0.0, 1.0));
    }
}

void ClipExportController::applyFinished(const application::ClipExportReport& report) {
    if (report.requestId != activeRequestId_) {
        return; // Stale completion of a superseded request.
    }
    activeRequestId_ = application::kInvalidClipExportRequestId;
    activeRequest_.reset();
    setBusy(false);
    if (worker_.joinable()) {
        worker_.join(); // The worker posted this completion as its last action, so this returns.
    }

    switch (report.outcome) {
    case application::ClipExportOutcome::kCompleted:
        setProgress(1.0);
        lastOutputPath_ = pendingOutputPath_;
        lastFailureDetail_.clear();
        setStatus(tr("已导出所选区间。"));
        emit exportFinished(true, lastStatus_);
        break;
    case application::ClipExportOutcome::kCanceled:
        setProgress(0.0);
        lastOutputPath_.clear();
        lastFailureDetail_.clear();
        setStatus(tr("已取消导出，未生成文件。"));
        emit exportFinished(false, lastStatus_);
        break;
    case application::ClipExportOutcome::kFailed:
        setProgress(0.0);
        lastOutputPath_.clear();
        lastFailureDetail_ = QString::fromStdString(report.technicalDetail);
        setStatus(tr("导出失败，未生成文件。"));
        emit exportFinished(false, lastStatus_);
        break;
    }
}

std::optional<ClipExportController::Request> ClipExportController::displayRequest() const {
    return busy_ ? activeRequest_ : makeRequest({});
}

std::optional<ClipExportController::Request>
ClipExportController::makeRequest(std::filesystem::path targetPath) const {
    const auto snapshot = dependencies_.snapshot ? dependencies_.snapshot() : nullptr;
    if (!snapshot || !dependencies_.exporter) {
        return std::nullopt;
    }
    if (!snapshot->canonicalTimeline.has_value() || snapshot->canonicalFrameCount == 0U ||
        snapshot->canonicalFrameCount >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    if (!snapshot->playbackRangeIn.has_value() || !snapshot->playbackRangeOut.has_value()) {
        return std::nullopt;
    }
    const auto& validated = snapshot->validatedComparison;
    if (!validated) {
        return std::nullopt;
    }
    const domain::ComparisonSource* const source = validated->find(validated->canonicalSourceId());
    if (source == nullptr) {
        return std::nullopt;
    }

    Request request{};
    request.sessionId = snapshot->sessionId;
    request.sessionEpoch = snapshot->sessionEpoch;
    request.sourcePath = source->descriptor.normalizedPath;
    request.targetPath = std::move(targetPath);
    request.timeline = *snapshot->canonicalTimeline;
    request.canonicalFrameCount = static_cast<std::int64_t>(snapshot->canonicalFrameCount);
    request.range.inInclusive = *snapshot->playbackRangeIn;
    request.range.outInclusive = *snapshot->playbackRangeOut;
    // This is the video session snapshot; still images use ImageReviewController separately.
    // A valid VFR source has no nominal rate, so admission follows its indexed timeline instead.
    if (const auto* const variable =
            std::get_if<std::shared_ptr<const domain::FrameTimeline>>(&*request.timeline);
        variable != nullptr &&
        (!*variable || (*variable)->frameCount() != request.canonicalFrameCount)) {
        return std::nullopt;
    }
    if (request.sourcePath.empty() ||
        !application::planClipExport(*request.timeline, request.canonicalFrameCount, request.range)
             .hasValue()) {
        return std::nullopt;
    }
    return request;
}

void ClipExportController::setBusy(const bool value) {
    if (busy_ != value) {
        busy_ = value;
        emit stateChanged();
    }
}

void ClipExportController::setProgress(const qreal value) {
    if (progress_ != value) {
        progress_ = value;
        emit progressChanged();
    }
}

void ClipExportController::setStatus(QString text) {
    if (lastStatus_ != text) {
        lastStatus_ = std::move(text);
        emit stateChanged();
    }
}

} // namespace dvs::ui
