#include "dvs/ui/SampleExportController.h"

#include "dvs/domain/ComparisonValidator.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QUrl>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace dvs::ui {
namespace {

[[nodiscard]] std::string sanitizeSampleId(const QString& raw, const int frame) {
    std::string sanitized;
    for (const QChar c : raw) {
        if (c.isLetterOrNumber() || c == '_' || c == '-') {
            sanitized.push_back(c.toLatin1());
        } else if (c == ' ' || c == '.') {
            sanitized.push_back('_');
        }
    }
    if (sanitized.empty() || sanitized == "." || sanitized == "..") {
        sanitized = "sample_" + std::to_string(frame + 1);
    }
    return sanitized;
}

} // namespace

SampleExportController::SampleExportController(Dependencies dependencies, QObject* parent)
    : QObject(parent), dependencies_(std::move(dependencies)) {}

SampleExportController::~SampleExportController() {
    cancelRequested_.store(true);
    if (workerThread_.joinable()) {
        workerThread_.request_stop();
    }
}

bool SampleExportController::busy() const noexcept {
    return busy_;
}

double SampleExportController::progress() const noexcept {
    return progress_;
}

QString SampleExportController::lastStatus() const {
    return lastStatus_;
}

QString SampleExportController::lastFailureDetail() const {
    return lastFailureDetail_;
}

QString SampleExportController::lastOutputPath() const {
    return lastOutputPath_;
}

int SampleExportController::targetFrame() const noexcept {
    return targetFrame_;
}

bool SampleExportController::canExport() const {
    return canExportAtFrame(targetFrame_);
}

void SampleExportController::setTargetFrame(const int frame) {
    if (targetFrame_ == frame) {
        return;
    }
    targetFrame_ = frame;
    emit targetFrameChanged();
}

bool SampleExportController::canExportAtFrame(const int frame) const {
    if (frame < 0 || !dependencies_.protocol) {
        return false;
    }
    const auto& protocol = dependencies_.protocol();
    if (!protocol.has_value()) {
        return false;
    }
    const auto snapshot = dependencies_.snapshot ? dependencies_.snapshot() : nullptr;
    if (!snapshot || !snapshot->validatedComparison) {
        return false;
    }
    const auto planResult = application::planSampleTriplet(
        *protocol, domain::FrameId{static_cast<std::int64_t>(frame)});
    return planResult.hasValue();
}

QVariantMap SampleExportController::planSummaryAtFrame(const int frame) const {
    QVariantMap result;
    if (frame < 0 || !dependencies_.protocol) {
        return result;
    }
    const auto& protocol = dependencies_.protocol();
    if (!protocol.has_value()) {
        return result;
    }
    const auto snapshot = dependencies_.snapshot ? dependencies_.snapshot() : nullptr;
    if (!snapshot || !snapshot->validatedComparison) {
        return result;
    }

    const auto planResult = application::planSampleTriplet(
        *protocol, domain::FrameId{static_cast<std::int64_t>(frame)});
    if (!planResult.hasValue()) {
        return result;
    }

    const auto& plan = planResult.value();
    result.insert(QStringLiteral("input0Frame"), static_cast<qint64>(plan.input0GtFrame.value()));
    result.insert(QStringLiteral("input0Ordinal"), static_cast<qint64>(plan.input0GtFrame.value()));
    result.insert(QStringLiteral("input1Frame"), static_cast<qint64>(plan.input1GtFrame.value()));
    result.insert(QStringLiteral("input1Ordinal"), static_cast<qint64>(plan.input1GtFrame.value()));
    result.insert(QStringLiteral("outputFrame"), static_cast<qint64>(frame));

    const double ratio = static_cast<double>(plan.targetRatioNumerator) /
                         static_cast<double>(plan.targetRatioDenominator);
    result.insert(QStringLiteral("outputRatio"), ratio);
    result.insert(QStringLiteral("hasGt"), true);
    const auto cropOpt =
        dependencies_.activeCropRect ? dependencies_.activeCropRect() : std::nullopt;
    result.insert(QStringLiteral("hasCrop"), cropOpt.has_value());
    if (cropOpt.has_value()) {
        result.insert(QStringLiteral("cropX"), static_cast<qint64>(cropOpt->x));
        result.insert(QStringLiteral("cropY"), static_cast<qint64>(cropOpt->y));
        result.insert(QStringLiteral("cropWidth"), static_cast<qint64>(cropOpt->width));
        result.insert(QStringLiteral("cropHeight"), static_cast<qint64>(cropOpt->height));
    }
    return result;
}

QString SampleExportController::suggestedSampleId(const int frame) const {
    if (frame < 0 || !dependencies_.protocol) {
        return QStringLiteral("sample_%1").arg(frame + 1);
    }
    const auto& protocol = dependencies_.protocol();
    if (!protocol.has_value()) {
        return QStringLiteral("sample_%1").arg(frame + 1);
    }
    const auto planResult = application::planSampleTriplet(
        *protocol, domain::FrameId{static_cast<std::int64_t>(frame)});
    if (!planResult.hasValue()) {
        return QStringLiteral("sample_%1").arg(frame + 1);
    }
    const auto& plan = planResult.value();
    const double ratio = static_cast<double>(plan.targetRatioNumerator) /
                         static_cast<double>(plan.targetRatioDenominator);
    return QStringLiteral("sample_%1_%2").arg(frame + 1).arg(QString::number(ratio, 'f', 3));
}

QString SampleExportController::suggestedTargetDirectory() const {
    const auto snapshot = dependencies_.snapshot ? dependencies_.snapshot() : nullptr;
    if (snapshot && snapshot->validatedComparison) {
        const auto refId = snapshot->validatedComparison->referenceSourceId();
        if (refId.has_value()) {
            if (const auto* const src = snapshot->validatedComparison->find(*refId)) {
                const std::filesystem::path p{src->descriptor.normalizedPath};
                if (p.has_parent_path()) {
                    return QString::fromStdWString(p.parent_path().wstring());
                }
            }
        }
    }
    return QDir::tempPath();
}

std::optional<SampleExportController::CapturedJob>
SampleExportController::prepareJob(const int frame,
                                   const std::filesystem::path& targetDir,
                                   const std::string& sampleId,
                                   const bool cropToRoi) {
    if (frame < 0 || !dependencies_.protocol || !dependencies_.exporter) {
        setStatus(tr("无法启动导出：导出组件未就绪。"));
        return std::nullopt;
    }
    const auto& protocol = dependencies_.protocol();
    if (!protocol.has_value()) {
        setStatus(tr("无法启动导出：未加载插帧协议。"));
        return std::nullopt;
    }
    const auto snapshot = dependencies_.snapshot ? dependencies_.snapshot() : nullptr;
    if (!snapshot || !snapshot->validatedComparison) {
        setStatus(tr("无法启动导出：会话未处于有效对比状态。"));
        return std::nullopt;
    }

    const auto sources = snapshot->validatedComparison->sources();
    if (sources.size() < 2U) {
        setStatus(tr("无法启动导出：至少需要两个源（参考源与候选源）。"));
        return std::nullopt;
    }

    const domain::ComparisonSource* gtSource = nullptr;
    const auto refId = snapshot->validatedComparison->referenceSourceId();
    if (refId.has_value()) {
        gtSource = snapshot->validatedComparison->find(*refId);
    }
    if (gtSource == nullptr) {
        for (const auto& src : sources) {
            if (src.role == domain::ComparisonRole::kReference) {
                gtSource = &src;
                break;
            }
        }
    }
    if (gtSource == nullptr) {
        setStatus(tr("无法启动导出：未设置参考源（GT）。"));
        return std::nullopt;
    }

    const domain::ComparisonSource* candidateSource = nullptr;
    for (const auto& src : sources) {
        if (src.id != gtSource->id) {
            candidateSource = &src;
            break;
        }
    }
    if (candidateSource == nullptr) {
        setStatus(tr("无法启动导出：未找到候选生成源。"));
        return std::nullopt;
    }

    std::optional<application::SampleCropRect> cropRect;
    std::optional<application::SampleFrameSize> frameSize;
    const auto& extent = gtSource->descriptor.extent;
    if (extent.isValid()) {
        const auto w = static_cast<std::int64_t>(extent.width);
        const auto h = static_cast<std::int64_t>(extent.height);
        frameSize = application::SampleFrameSize{w, h};
        if (cropToRoi && dependencies_.activeCropRect) {
            cropRect = dependencies_.activeCropRect();
        }
    }

    auto planResult = application::planSampleTriplet(
        *protocol, domain::FrameId{static_cast<std::int64_t>(frame)}, cropRect, frameSize);
    if (!planResult.hasValue()) {
        setStatus(tr("生成导出计划失败。"));
        return std::nullopt;
    }

    CapturedJob captured;
    captured.job = application::SampleExportJob{
        .requestId = ++nextRequestId_,
        .sessionId = snapshot->sessionId,
        .sessionEpoch = snapshot->sessionEpoch,
        .sampleId = sampleId,
        .plan = planResult.value(),
        .gtAssetPath = gtSource->descriptor.normalizedPath,
        .candidateAssetPath = candidateSource->descriptor.normalizedPath,
        .outputDirectory = targetDir,
    };
    return captured;
}

bool SampleExportController::exportTriplet(const QUrl& outputDirectory,
                                           const QString& sampleId,
                                           const bool cropToRoi,
                                           int frame) {
    if (busy_) {
        setStatus(tr("已有样本导出任务在运行。"));
        return false;
    }
    if (frame < 0) {
        frame = targetFrame_;
    }
    if (frame < 0) {
        setStatus(tr("未指定导出帧。"));
        return false;
    }

    std::filesystem::path outPath;
    if (outputDirectory.isLocalFile()) {
        outPath = outputDirectory.toLocalFile().toStdWString();
    } else {
        outPath = outputDirectory.toString().toStdWString();
    }
    if (outPath.empty()) {
        setStatus(tr("请选择有效的导出目标目录。"));
        return false;
    }

    const std::string sanitizedId = sanitizeSampleId(sampleId, frame);
    auto capturedOpt = prepareJob(frame, outPath, sanitizedId, cropToRoi);
    if (!capturedOpt.has_value()) {
        return false;
    }

    busy_ = true;
    progress_ = 0.0;
    lastStatus_ = tr("正在导出样本…");
    lastFailureDetail_.clear();
    lastOutputPath_.clear();
    cancelRequested_.store(false);
    emit stateChanged();

    workerThread_ = std::jthread([this, job = std::move(capturedOpt->job)]() mutable {
        const auto exporter = dependencies_.exporter;
        auto report = exporter->perform(job, cancelRequested_);

        QMetaObject::invokeMethod(
            this,
            [this, report = std::move(report), sampleDir = job.outputDirectory / job.sampleId] {
                busy_ = false;
                if (report.outcome == application::SampleExportOutcome::kCompleted) {
                    progress_ = 1.0;
                    lastStatus_ = tr("样本三元组导出完成。");
                    lastOutputPath_ = QString::fromStdWString(sampleDir.wstring());
                    lastFailureDetail_.clear();
                    emit stateChanged();
                    emit exportFinished(true, lastStatus_);
                } else if (report.outcome == application::SampleExportOutcome::kCanceled) {
                    lastStatus_ = tr("导出已取消。");
                    lastFailureDetail_.clear();
                    emit stateChanged();
                    emit exportFinished(false, lastStatus_);
                } else {
                    lastStatus_ = tr("导出失败。");
                    lastFailureDetail_ = QString::fromStdString(report.technicalDetail);
                    emit stateChanged();
                    emit exportFinished(false, lastStatus_);
                }
            });
    });

    return true;
}

void SampleExportController::cancelExport() {
    if (busy_) {
        cancelRequested_.store(true);
    }
}

void SampleExportController::setStatus(QString status, QString failureDetail) {
    lastStatus_ = std::move(status);
    lastFailureDetail_ = std::move(failureDetail);
    emit stateChanged();
}

} // namespace dvs::ui
