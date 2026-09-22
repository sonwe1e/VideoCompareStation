#include "dvs/ui/IssueLogController.h"

#include "dvs/ui/ImageFolderPairModel.h"
#include "dvs/ui/ReviewController.h"
#include "dvs/ui/ReviewPreferencesController.h"

#include <QDateTime>
#include <QFileInfo>
#include <QUrl>
#include <QVariantList>

#include <utility>

namespace dvs::ui {
namespace {

using application::FrameMatchKind;
using application::IssueRecord;
using application::IssueRecordKind;
using application::IssueRestoreDecision;
using application::IssueSourceRef;
using application::kIssueRecordSchemaVersion;
using application::ObservedSourceIdentity;
using application::PresentedSourceState;

[[nodiscard]] std::string fromQString(const QString& value) {
    return value.toStdString();
}

[[nodiscard]] QString toQString(const std::string& value) {
    return QString::fromStdString(value);
}

// Position of the canonical source inside record.sources. sources are recorded in sourceId order
// (0, 1, 2), so the canonical index is a valid position whenever the record is well formed.
[[nodiscard]] std::size_t canonicalSourcePosition(const IssueRecord& record) {
    if (record.sources.empty()) {
        return 0U;
    }
    const std::int64_t canonical = record.canonicalSourceIndex;
    if (canonical >= 0 && static_cast<std::size_t>(canonical) < record.sources.size()) {
        return static_cast<std::size_t>(canonical);
    }
    return 0U;
}

[[nodiscard]] IssueSourceRef observeAsRecorded(const QString& path) {
    IssueSourceRef source;
    source.path = fromQString(path);
    const QFileInfo info{path};
    if (info.exists() && info.isFile()) {
        source.byteSize = info.size();
        source.modifiedUtcMilliseconds = info.lastModified().toMSecsSinceEpoch();
    }
    return source;
}

} // namespace

IssueLogController::IssueLogController(application::IIssueRecordRepository* repository,
                                       QObject* parent)
    : QObject(parent), repository_(repository) {}

IssueLogController::~IssueLogController() = default;

void IssueLogController::setReviewController(ReviewController* const controller) noexcept {
    review_ = controller;
}

void IssueLogController::setPreferences(ReviewPreferencesController* const preferences) noexcept {
    preferences_ = preferences;
}

void IssueLogController::setFolderModel(ImageFolderPairModel* const model) noexcept {
    folderModel_ = model;
}

void IssueLogController::setImageController(QObject* const imageController) noexcept {
    imageController_ = imageController;
}

void IssueLogController::setVideoSurface(QObject* const videoSurface) noexcept {
    videoSurface_ = videoSurface;
}

void IssueLogController::setWorkspaceMode(const QString& mode) noexcept {
    workspaceMode_ = mode;
}

void IssueLogController::setDefaultDocumentPath(const QUrl& path) noexcept {
    documentPath_ = path.isLocalFile() ? path.toLocalFile() : path.toString();
}

int IssueLogController::count() const noexcept {
    return static_cast<int>(issues_.size());
}

QString IssueLogController::statusText() const noexcept {
    return statusText_;
}

QString IssueLogController::lastError() const noexcept {
    return lastError_;
}

QString IssueLogController::documentPath() const noexcept {
    return documentPath_;
}

void IssueLogController::setStatus(QString text, QString error) {
    statusText_ = std::move(text);
    lastError_ = std::move(error);
    Q_EMIT statusChanged();
}

application::ObservedSourceIdentity IssueLogController::observePath(const std::string& path) const {
    ObservedSourceIdentity observed;
    if (path.empty()) {
        return observed;
    }
    const QFileInfo info{QString::fromStdString(path)};
    observed.exists = info.exists() && info.isFile();
    if (observed.exists) {
        observed.byteSize = info.size();
        observed.modifiedUtcMilliseconds = info.lastModified().toMSecsSinceEpoch();
    }
    return observed;
}

QString IssueLogController::summarize(const IssueRecord& record) {
    if (record.kind == IssueRecordKind::ImagePair) {
        return QStringLiteral("%1 · %2")
            .arg(
                QString::fromStdString(record.fileName.empty() ? record.leftPath : record.fileName))
            .arg(record.hasValidPresentation ? QStringLiteral("pair")
                                             : QStringLiteral("no-presentation"));
    }
    const QString frameText =
        record.hasValidPresentation
            ? QStringLiteral("frame %1")
                  .arg(record.sources.empty()
                           ? 0
                           : record.sources[canonicalSourcePosition(record)].displayIndex)
            : QStringLiteral("no-presentation");
    return QStringLiteral("video · %1 · %2 source(s)").arg(frameText).arg(record.sources.size());
}

bool IssueLogController::captureVideoIssue(StoredIssue& issue, const QString& note) {
    if (review_ == nullptr) {
        setStatus(QStringLiteral("无法记录视频问题"), QStringLiteral("review controller missing"));
        return false;
    }
    IssueRecord record;
    record.schemaVersion = kIssueRecordSchemaVersion;
    record.kind = IssueRecordKind::Video;
    record.createdAtUtcMilliseconds = QDateTime::currentMSecsSinceEpoch();
    record.note = fromQString(note);
    record.screenshotIsDisplayResult = true;

    const QVariantList sourceUrls = review_->sourceUrls();
    for (int index = 0; index < sourceUrls.size(); ++index) {
        const QUrl url = sourceUrls[index].toUrl();
        const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
        IssueSourceRef source = observeAsRecorded(path);
        record.sources.push_back(source);
    }

    // Capture per-side presentation from the single committed snapshot backing the projection.
    // One side may be offset (GlobalOffset/AutoAligned/ManualAnchor) or Missing while the
    // canonical frame is still displayed; each recorded source carries its own actual frame,
    // PTS and mapping kind instead of the canonical frame number repeated across every side.
    const std::shared_ptr<const application::SessionSnapshot> snapshot = review_->currentSnapshot();
    const bool hasFrame = snapshot != nullptr && snapshot->displayedFrame.has_value();
    record.hasValidPresentation = hasFrame && !record.sources.empty();
    if (snapshot != nullptr) {
        record.alignmentRevision = snapshot->alignmentRevision;
        for (std::size_t index = 0U; index < record.sources.size(); ++index) {
            IssueSourceRef& source = record.sources[index];
            const PresentedSourceState* presented = nullptr;
            for (const PresentedSourceState& candidate : snapshot->presentedSources) {
                if (candidate.sourceId == index) {
                    presented = &candidate;
                    break;
                }
            }
            if (presented == nullptr) {
                // No committed mapping entry for this side: never claim a presented frame.
                source.hasPresentation = false;
                source.displayIndex = -1;
                source.presentationMatchKind =
                    static_cast<std::int32_t>(application::FrameMatchKind::Missing);
                continue;
            }
            if (!presented->sourceFrameId.has_value()) {
                source.hasPresentation = false;
                source.displayIndex = -1;
                source.presentationMatchKind = static_cast<std::int32_t>(presented->matchKind);
                if (presented->missingReason.has_value()) {
                    // Stored as enum value + 1 so 0 means "not applicable".
                    source.presentationMissingReason =
                        static_cast<std::int32_t>(*presented->missingReason) + 1;
                }
                continue;
            }
            source.hasPresentation = true;
            source.displayIndex = presented->sourceFrameId->value();
            source.presentationTimestampTicks = presented->presentationTime.microseconds();
            source.timeBaseNumerator = 1;
            source.timeBaseDenominator = 1'000'000;
            source.presentationMatchKind = static_cast<std::int32_t>(presented->matchKind);
        }
    }
    record.canonicalSourceIndex = review_->canonicalSourceIndex();
    if (preferences_ != nullptr) {
        record.view.viewMode = preferences_->viewModeCode();
        // D07: record the committed effective pair edge, not the legacy preference slot.
        record.view.differenceEdge = review_->effectiveDifferenceEdge();
    }
    if (videoSurface_ != nullptr) {
        record.view.roiEnabled = videoSurface_->property("roiEnabled").toBool();
        record.view.roiLeft = videoSurface_->property("roiLeft").toDouble();
        record.view.roiTop = videoSurface_->property("roiTop").toDouble();
        record.view.roiRight = videoSurface_->property("roiRight").toDouble();
        record.view.roiBottom = videoSurface_->property("roiBottom").toDouble();
        record.view.centerX = videoSurface_->property("viewCenterX").toDouble();
        record.view.centerY = videoSurface_->property("viewCenterY").toDouble();
    }
    issue.record = std::move(record);
    issue.summary = summarize(issue.record);
    return true;
}

bool IssueLogController::captureImageIssue(StoredIssue& issue, const QString& note) {
    IssueRecord record;
    record.schemaVersion = kIssueRecordSchemaVersion;
    record.kind = IssueRecordKind::ImagePair;
    record.createdAtUtcMilliseconds = QDateTime::currentMSecsSinceEpoch();
    record.note = fromQString(note);
    record.screenshotIsDisplayResult = true;

    if (folderModel_ != nullptr) {
        record.leftFolder = fromQString(folderModel_->leftFolderPath());
        record.rightFolder = fromQString(folderModel_->rightFolderPath());
        const int row = folderModel_->currentPair();
        record.rowIndex = row;
        if (row >= 0) {
            const QVariantMap urls = folderModel_->pairUrlsAt(row);
            record.leftPath =
                fromQString(urls.value(QStringLiteral("leftUrl")).toUrl().toLocalFile());
            record.rightPath =
                fromQString(urls.value(QStringLiteral("rightUrl")).toUrl().toLocalFile());
            const QModelIndex index = folderModel_->index(row, 0);
            record.fileName = fromQString(
                folderModel_->data(index, ImageFolderPairModel::FileNameRole).toString());
        }
    }
    if (imageController_ != nullptr) {
        if (record.leftPath.empty()) {
            record.leftPath = fromQString(imageController_->property("primaryPath").toString());
        }
        if (record.rightPath.empty()) {
            record.rightPath = fromQString(imageController_->property("secondaryPath").toString());
        }
        record.view.compareMode = imageController_->property("compareMode").toInt();
        record.view.zoom = imageController_->property("zoom").toDouble();
        record.view.panX = imageController_->property("panX").toDouble();
        record.view.panY = imageController_->property("panY").toDouble();
        record.view.resampleAllowed = imageController_->property("resampleAllowed").toBool();
        // Committed pair identity only — never a pending candidate.
        record.hasValidPresentation =
            imageController_->property("hasPair").toBool() && !record.leftPath.empty();
    }
    if (!record.leftPath.empty()) {
        record.sources.push_back(observeAsRecorded(QString::fromStdString(record.leftPath)));
    }
    if (!record.rightPath.empty()) {
        record.sources.push_back(observeAsRecorded(QString::fromStdString(record.rightPath)));
    }
    if (record.sources.empty()) {
        // No committed pair: still export a record, but mark it as lacking presentation so
        // restore cannot invent a frame/pair identity.
        record.hasValidPresentation = false;
        issue.record = std::move(record);
        issue.summary = summarize(issue.record);
        return true;
    }
    issue.record = std::move(record);
    issue.summary = summarize(issue.record);
    return true;
}

bool IssueLogController::captureCurrentIssue(const QString& note) {
    StoredIssue issue;
    const bool captured = workspaceMode_ == QStringLiteral("image")
                              ? captureImageIssue(issue, note)
                              : captureVideoIssue(issue, note);
    if (!captured) {
        return false;
    }
    issues_.push_back(std::move(issue));
    setStatus(QStringLiteral("已记录问题 #%1").arg(issues_.size()));
    Q_EMIT issuesChanged();
    return true;
}

bool IssueLogController::saveDefault() {
    return saveIssues(QUrl{});
}

bool IssueLogController::saveIssues(const QUrl& fileUrl) {
    if (repository_ == nullptr) {
        setStatus(QStringLiteral("无法保存问题记录"), QStringLiteral("repository missing"));
        return false;
    }
    const QString pathText = fileUrl.isLocalFile() ? fileUrl.toLocalFile() : fileUrl.toString();
    const std::filesystem::path target =
        pathText.isEmpty() ? std::filesystem::path{} : std::filesystem::path{fromQString(pathText)};
    std::vector<IssueRecord> records;
    records.reserve(issues_.size());
    for (const StoredIssue& issue : issues_) {
        records.push_back(issue.record);
    }
    std::string error;
    if (!repository_->save(target, records, &error)) {
        setStatus(QStringLiteral("保存失败"), toQString(error));
        return false;
    }
    if (!pathText.isEmpty()) {
        documentPath_ = pathText;
    }
    setStatus(QStringLiteral("已保存 %1 条问题记录").arg(records.size()));
    Q_EMIT issuesChanged();
    return true;
}

bool IssueLogController::loadDefault() {
    return loadIssues(QUrl{});
}

bool IssueLogController::loadIssues(const QUrl& fileUrl) {
    if (repository_ == nullptr) {
        setStatus(QStringLiteral("无法加载问题记录"), QStringLiteral("repository missing"));
        return false;
    }
    const QString pathText = fileUrl.isLocalFile() ? fileUrl.toLocalFile() : fileUrl.toString();
    const std::filesystem::path target =
        pathText.isEmpty() ? std::filesystem::path{} : std::filesystem::path{fromQString(pathText)};
    const application::IssueRecordIoResult result = repository_->load(target);
    if (!result.ok) {
        // Schema rejection / corrupt JSON: explain, keep the previous in-memory list, and
        // never rewrite the file.
        setStatus(QStringLiteral("加载失败"), toQString(result.error));
        return false;
    }
    issues_.clear();
    issues_.reserve(result.records.size());
    for (const IssueRecord& record : result.records) {
        issues_.push_back(StoredIssue{record, summarize(record)});
    }
    if (!pathText.isEmpty()) {
        documentPath_ = pathText;
    }
    setStatus(QStringLiteral("已加载 %1 条问题记录").arg(issues_.size()));
    Q_EMIT issuesChanged();
    return true;
}

QVariantMap IssueLogController::issueAt(const int index) const {
    QVariantMap map;
    if (index < 0 || index >= static_cast<int>(issues_.size())) {
        return map;
    }
    const StoredIssue& issue = issues_[static_cast<std::size_t>(index)];
    map.insert(QStringLiteral("index"), index);
    map.insert(QStringLiteral("summary"), issue.summary);
    map.insert(QStringLiteral("note"), QString::fromStdString(issue.record.note));
    map.insert(QStringLiteral("kind"),
               issue.record.kind == IssueRecordKind::ImagePair ? QStringLiteral("image-pair")
                                                               : QStringLiteral("video"));
    map.insert(QStringLiteral("hasValidPresentation"), issue.record.hasValidPresentation);
    map.insert(QStringLiteral("schemaVersion"), static_cast<qint64>(issue.record.schemaVersion));
    map.insert(QStringLiteral("screenshotIsDisplayResult"), issue.record.screenshotIsDisplayResult);
    return map;
}

void IssueLogController::clearIssues() {
    issues_.clear();
    setStatus(QStringLiteral("已清空问题记录"));
    Q_EMIT issuesChanged();
}

QVariantMap IssueLogController::performRestore(const StoredIssue& issue) {
    QVariantMap result;
    std::vector<ObservedSourceIdentity> observed;
    observed.reserve(issue.record.sources.size());
    for (const IssueSourceRef& source : issue.record.sources) {
        observed.push_back(observePath(source.path));
    }
    const application::IssueRestoreEvaluation evaluation =
        application::evaluateIssueRestore(issue.record, observed);
    const auto decisionName = [](const IssueRestoreDecision decision) {
        switch (decision) {
        case IssueRestoreDecision::Ready:
            return QStringLiteral("ready");
        case IssueRestoreDecision::RelocationRequired:
            return QStringLiteral("relocation-required");
        case IssueRestoreDecision::NoValidPresentation:
            return QStringLiteral("no-valid-presentation");
        case IssueRestoreDecision::Blocked:
            return QStringLiteral("blocked");
        }
        return QStringLiteral("blocked");
    };
    result.insert(QStringLiteral("decision"), decisionName(evaluation.decision));
    result.insert(QStringLiteral("ok"), evaluation.decision == IssueRestoreDecision::Ready);
    result.insert(QStringLiteral("message"), toQString(evaluation.message));
    result.insert(QStringLiteral("note"), QString::fromStdString(issue.record.note));
    result.insert(QStringLiteral("kind"),
                  issue.record.kind == IssueRecordKind::ImagePair ? QStringLiteral("image-pair")
                                                                  : QStringLiteral("video"));

    QVariantList sourceStatus;
    for (const application::IssueSourceMatchResult& match : evaluation.sources) {
        QString status = QStringLiteral("matched");
        switch (match.match) {
        case application::IssueSourceMatch::Matched:
            status = QStringLiteral("matched");
            break;
        case application::IssueSourceMatch::Missing:
            status = QStringLiteral("missing");
            break;
        case application::IssueSourceMatch::Modified:
            status = QStringLiteral("modified");
            break;
        case application::IssueSourceMatch::IncompleteIdentity:
            status = QStringLiteral("incomplete-identity");
            break;
        }
        sourceStatus.append(QVariantMap{
            {QStringLiteral("path"), toQString(match.path)},
            {QStringLiteral("status"), status},
        });
    }
    result.insert(QStringLiteral("sources"), sourceStatus);

    if (evaluation.decision == IssueRestoreDecision::Ready) {
        QVariantList urls;
        for (const IssueSourceRef& source : issue.record.sources) {
            urls.append(QUrl::fromLocalFile(QString::fromStdString(source.path)));
        }
        result.insert(QStringLiteral("urls"), urls);
        result.insert(QStringLiteral("canonicalSourceIndex"), issue.record.canonicalSourceIndex);
        result.insert(QStringLiteral("viewMode"), issue.record.view.viewMode);
        result.insert(QStringLiteral("differenceEdge"), issue.record.view.differenceEdge);
        result.insert(QStringLiteral("roiEnabled"), issue.record.view.roiEnabled);
        result.insert(QStringLiteral("roiLeft"), issue.record.view.roiLeft);
        result.insert(QStringLiteral("roiTop"), issue.record.view.roiTop);
        result.insert(QStringLiteral("roiRight"), issue.record.view.roiRight);
        result.insert(QStringLiteral("roiBottom"), issue.record.view.roiBottom);
        result.insert(QStringLiteral("zoom"), issue.record.view.zoom);
        result.insert(QStringLiteral("centerX"), issue.record.view.centerX);
        result.insert(QStringLiteral("centerY"), issue.record.view.centerY);
        result.insert(QStringLiteral("compareMode"), issue.record.view.compareMode);
        result.insert(QStringLiteral("panX"), issue.record.view.panX);
        result.insert(QStringLiteral("panY"), issue.record.view.panY);
        result.insert(QStringLiteral("leftPath"), QString::fromStdString(issue.record.leftPath));
        result.insert(QStringLiteral("rightPath"), QString::fromStdString(issue.record.rightPath));
        result.insert(QStringLiteral("leftFolder"),
                      QString::fromStdString(issue.record.leftFolder));
        result.insert(QStringLiteral("rightFolder"),
                      QString::fromStdString(issue.record.rightFolder));
        result.insert(QStringLiteral("rowIndex"), issue.record.rowIndex);
        if (issue.record.kind == IssueRecordKind::Video && issue.record.hasValidPresentation &&
            !issue.record.sources.empty()) {
            const IssueSourceRef& canonical =
                issue.record.sources[canonicalSourcePosition(issue.record)];
            result.insert(QStringLiteral("frame"), static_cast<qint64>(canonical.displayIndex));
        }
        Q_EMIT restoreRequested(result);
        setStatus(QStringLiteral("问题记录可恢复"));
        return result;
    }

    if (evaluation.decision == IssueRestoreDecision::RelocationRequired) {
        setStatus(QStringLiteral("来源已移动或被修改，请先重定位"), toQString(evaluation.message));
        return result;
    }
    if (evaluation.decision == IssueRestoreDecision::NoValidPresentation) {
        setStatus(QStringLiteral("来源一致，但记录中没有有效呈现上下文"),
                  toQString(evaluation.message));
        return result;
    }
    setStatus(QStringLiteral("无法恢复该问题记录"), toQString(evaluation.message));
    return result;
}

QVariantMap IssueLogController::restoreIssue(const int index) {
    if (index < 0 || index >= static_cast<int>(issues_.size())) {
        QVariantMap result;
        result.insert(QStringLiteral("ok"), false);
        result.insert(QStringLiteral("decision"), QStringLiteral("blocked"));
        result.insert(QStringLiteral("message"), QStringLiteral("index-out-of-range"));
        setStatus(QStringLiteral("无法恢复该问题记录"), QStringLiteral("index-out-of-range"));
        return result;
    }
    return performRestore(issues_[static_cast<std::size_t>(index)]);
}

} // namespace dvs::ui
