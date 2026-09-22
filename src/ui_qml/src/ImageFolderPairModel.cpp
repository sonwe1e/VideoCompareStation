#include "dvs/ui/ImageFolderPairModel.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QStringList>
#include <QUrl>

#include <algorithm>
#include <map>
#include <utility>

namespace dvs::ui {
namespace {

const QStringList& stillImageSuffixes() {
    static const QStringList kSuffixes = {
        QStringLiteral("png"),
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("bmp"),
        QStringLiteral("gif"),
        QStringLiteral("webp"),
        QStringLiteral("tif"),
        QStringLiteral("tiff"),
        // PNM family (PBM/PGM/PPM/PAM) is a first-class input, not a decode fallback.
        QStringLiteral("pnm"),
        QStringLiteral("ppm"),
        QStringLiteral("pgm"),
        QStringLiteral("pbm"),
        QStringLiteral("pam"),
    };
    return kSuffixes;
}

} // namespace

ImageFolderPairModel::ImageFolderPairModel(QObject* const parent) : QAbstractListModel(parent) {}

void ImageFolderPairModel::setPairOpener(PairOpener opener) {
    opener_ = std::move(opener);
}

void ImageFolderPairModel::setAsyncPairOpener(AsyncPairOpener opener) {
    asyncOpener_ = std::move(opener);
}

void ImageFolderPairModel::setAsyncPairCancel(AsyncPairCancel cancel) {
    asyncCancel_ = std::move(cancel);
}

void ImageFolderPairModel::setSingleSideOpener(SingleSideOpener opener) {
    singleSideOpener_ = std::move(opener);
}

int ImageFolderPairModel::completeCount() const noexcept {
    int complete = 0;
    for (const PairRow& row : rows_) {
        if (row.hasLeft && row.hasRight) {
            ++complete;
        }
    }
    return complete;
}

int ImageFolderPairModel::missingCount() const noexcept {
    int missing = 0;
    for (const PairRow& row : rows_) {
        // Exactly one side exists: the file is missing on the other side.
        if (row.hasLeft != row.hasRight) {
            ++missing;
        }
    }
    return missing;
}

int ImageFolderPairModel::conflictCount() const noexcept {
    int conflicts = 0;
    for (const PairRow& row : rows_) {
        if (row.caseConflict) {
            ++conflicts;
        }
    }
    return conflicts;
}

QString ImageFolderPairModel::leftFolderName() const noexcept {
    return QFileInfo{leftFolderPath_}.fileName();
}

QString ImageFolderPairModel::rightFolderName() const noexcept {
    return QFileInfo{rightFolderPath_}.fileName();
}

QString ImageFolderPairModel::leftFolderPath() const noexcept {
    return leftFolderPath_;
}

QString ImageFolderPairModel::rightFolderPath() const noexcept {
    return rightFolderPath_;
}

int ImageFolderPairModel::pairCount() const noexcept {
    return static_cast<int>(rows_.size());
}

int ImageFolderPairModel::currentPair() const noexcept {
    return currentPair_;
}

void ImageFolderPairModel::setCurrentPair(const int row) {
    if (row == currentPair_) {
        return;
    }
    currentPair_ = row;
    emit currentPairChanged();
}

QString ImageFolderPairModel::errorText() const noexcept {
    return errorText_;
}

bool ImageFolderPairModel::openPending() const noexcept {
    return pendingRequestId_ != 0 || pendingSingleRequestId_ != 0;
}

int ImageFolderPairModel::pendingPair() const noexcept {
    return pendingRequestId_ != 0 ? pendingRow_ : pendingSingleRow_;
}

int ImageFolderPairModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant ImageFolderPairModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 ||
        static_cast<std::size_t>(index.row()) >= rows_.size()) {
        return {};
    }
    const PairRow& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case FileNameRole:
        return row.fileName;
    case LeftPathRole:
        return row.leftUrl;
    case RightPathRole:
        return row.rightUrl;
    case HasLeftRole:
        return row.hasLeft;
    case HasRightRole:
        return row.hasRight;
    case HasBothRole:
        return row.hasLeft && row.hasRight;
    case RowIndexRole:
        return index.row();
    case CaseConflictRole:
        return row.caseConflict;
    case CaseConflictDetailRole:
        return row.caseConflictDetail;
    default:
        return {};
    }
}

QHash<int, QByteArray> ImageFolderPairModel::roleNames() const {
    return {
        {FileNameRole, QByteArrayLiteral("fileName")},
        {LeftPathRole, QByteArrayLiteral("leftUrl")},
        {RightPathRole, QByteArrayLiteral("rightUrl")},
        {HasLeftRole, QByteArrayLiteral("hasLeft")},
        {HasRightRole, QByteArrayLiteral("hasRight")},
        {HasBothRole, QByteArrayLiteral("hasBoth")},
        {RowIndexRole, QByteArrayLiteral("rowIndex")},
        {CaseConflictRole, QByteArrayLiteral("caseConflict")},
        {CaseConflictDetailRole, QByteArrayLiteral("caseConflictDetail")},
    };
}

bool ImageFolderPairModel::isStillImageName(const QString& fileName) {
    return stillImageSuffixes().contains(QFileInfo{fileName}.suffix().toLower());
}

std::vector<ImageFolderPairModel::PairRow>
ImageFolderPairModel::mergePairRows(const FolderSideScan& left, const FolderSideScan& right) {
    // A side holding several names that fold to the same key makes the pairing ambiguous;
    // the row says so and names the file actually used instead of hiding the choice.
    const auto conflictDetailFor = [&left, &right](const QString& folded) {
        QStringList parts;
        const auto describe = [&parts, &folded](const FolderSideScan& side,
                                                const QString& sideLabel) {
            const auto names = side.names.find(folded);
            if (names == side.names.end() || names->second.size() < 2) {
                return;
            }
            parts.push_back(QStringLiteral("%1 侧有多个大小写不同的同名文件：%2（当前使用 %3）")
                                .arg(sideLabel,
                                     names->second.join(QStringLiteral("、")),
                                     names->second.front()));
        };
        describe(left, QStringLiteral("A"));
        describe(right, QStringLiteral("B"));
        return parts.join(QStringLiteral("；"));
    };

    std::vector<PairRow> rows;
    rows.reserve(left.entries.size() + right.entries.size());
    for (const auto& [key, url] : left.entries) {
        PairRow row;
        row.fileName = QFileInfo{url.toLocalFile()}.fileName();
        row.leftUrl = url;
        row.hasLeft = true;
        const auto rightIt = right.entries.find(key);
        if (rightIt != right.entries.end()) {
            row.rightUrl = rightIt->second;
            row.hasRight = true;
        }
        row.caseConflictDetail = conflictDetailFor(key);
        row.caseConflict = !row.caseConflictDetail.isEmpty();
        rows.push_back(std::move(row));
    }
    for (const auto& [key, url] : right.entries) {
        if (left.entries.find(key) != left.entries.end()) {
            continue;
        }
        PairRow row;
        row.fileName = QFileInfo{url.toLocalFile()}.fileName();
        row.rightUrl = url;
        row.hasRight = true;
        row.caseConflictDetail = conflictDetailFor(key);
        row.caseConflict = !row.caseConflictDetail.isEmpty();
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(), [](const PairRow& lhs, const PairRow& rhs) {
        return lhs.fileName.compare(rhs.fileName, Qt::CaseInsensitive) < 0;
    });
    return rows;
}

bool ImageFolderPairModel::loadFolders(const QUrl& left, const QUrl& right) {
    cancelPendingOpen();
    const QString leftPath = left.toLocalFile();
    const QString rightPath = right.toLocalFile();
    const QFileInfo leftInfo{leftPath};
    const QFileInfo rightInfo{rightPath};

    if (leftPath.isEmpty() || rightPath.isEmpty()) {
        errorText_ = tr("Select two folders to compare.");
        beginResetModel();
        rows_.clear();
        endResetModel();
        emit foldersChanged();
        return false;
    }
    if (!leftInfo.isDir() || !rightInfo.isDir()) {
        errorText_ = tr("Both selections must be folders.");
        beginResetModel();
        rows_.clear();
        endResetModel();
        emit foldersChanged();
        return false;
    }
    if (QDir::cleanPath(leftInfo.absoluteFilePath()) ==
        QDir::cleanPath(rightInfo.absoluteFilePath())) {
        errorText_ = tr("Choose two different folders.");
        beginResetModel();
        rows_.clear();
        endResetModel();
        emit foldersChanged();
        return false;
    }

    // Case-insensitive name key so "Shot.PNG" and "shot.png" pair like the drop pipeline
    // treats same-named files. One map pass per side, then a merged sorted row list.
    // Names that fold to the same key within one side are recorded as an explicit
    // conflict (T6): the pairing must not silently pick one of them.
    const auto scanSide = [](const QString& folderPath) {
        FolderSideScan scan;
        const QDir folder{folderPath};
        const QFileInfoList files =
            folder.entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo& file : files) {
            if (!isStillImageName(file.fileName())) {
                continue;
            }
            const QString folded = file.fileName().toCaseFolded();
            scan.entries.emplace(folded, QUrl::fromLocalFile(file.absoluteFilePath()));
            QStringList& names = scan.names[folded];
            if (!names.contains(file.fileName())) {
                names.push_back(file.fileName());
            }
        }
        return scan;
    };

    const FolderSideScan leftScan = scanSide(leftPath);
    const FolderSideScan rightScan = scanSide(rightPath);

    // mergePairRows returns the rows already sorted by case-insensitive file name.
    std::vector<PairRow> nextRows = mergePairRows(leftScan, rightScan);

    errorText_.clear();
    beginResetModel();
    rows_ = std::move(nextRows);
    currentPair_ = -1;
    endResetModel();
    leftFolderPath_ = QDir::cleanPath(leftInfo.absoluteFilePath());
    rightFolderPath_ = QDir::cleanPath(rightInfo.absoluteFilePath());
    emit foldersChanged();
    emit currentPairChanged();
    return true;
}

void ImageFolderPairModel::clear() {
    cancelPendingOpen();
    if (rows_.empty() && leftFolderPath_.isEmpty() && rightFolderPath_.isEmpty() &&
        currentPair_ < 0 && errorText_.isEmpty()) {
        return;
    }
    beginResetModel();
    rows_.clear();
    endResetModel();
    currentPair_ = -1;
    leftFolderPath_.clear();
    rightFolderPath_.clear();
    errorText_.clear();
    emit currentPairChanged();
    emit foldersChanged();
}

bool ImageFolderPairModel::openPairAt(const int row) {
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) {
        return false;
    }
    const PairRow& pair = rows_[static_cast<std::size_t>(row)];
    if (!pair.hasLeft || !pair.hasRight) {
        return false;
    }

    if (asyncOpener_) {
        if (pendingRequestId_ != 0 && pendingRow_ == row) {
            return true;
        }
        cancelPendingOpen();
        QString immediateError;
        const int requestId = asyncOpener_(pair.leftUrl, pair.rightUrl, row, &immediateError);
        if (requestId <= 0) {
            errorText_ =
                immediateError.isEmpty() ? tr("Could not open image pair.") : immediateError;
            emit foldersChanged();
            return false;
        }
        pendingRequestId_ = static_cast<quint64>(requestId);
        pendingRow_ = row;
        errorText_.clear();
        emit pendingPairChanged();
        return true;
    }

    if (!opener_) {
        return false;
    }
    const QString error = opener_(pair.leftUrl, pair.rightUrl, row);
    if (!error.isEmpty()) {
        // The canvas kept its previous committed pair (or the explicit empty state);
        // currentPair_ stays put so selection and canvas identity never diverge.
        errorText_ = error;
        emit foldersChanged();
        return false;
    }
    errorText_.clear();
    if (currentPair_ != row) {
        currentPair_ = row;
        emit currentPairChanged();
    }
    return true;
}

void ImageFolderPairModel::cancelPendingOpen() {
    bool changed = false;
    if (pendingRequestId_ != 0) {
        if (asyncCancel_) {
            asyncCancel_(static_cast<int>(pendingRequestId_));
        }
        pendingRequestId_ = 0;
        pendingRow_ = -1;
        changed = true;
    }
    if (pendingSingleRequestId_ != 0) {
        if (asyncCancel_) {
            asyncCancel_(static_cast<int>(pendingSingleRequestId_));
        }
        pendingSingleRequestId_ = 0;
        pendingSingleRow_ = -1;
        changed = true;
    }
    if (changed) {
        emit pendingPairChanged();
    }
}

bool ImageFolderPairModel::openSingleSideAt(const int row) {
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) {
        return false;
    }
    const PairRow& pair = rows_[static_cast<std::size_t>(row)];
    if (pair.hasLeft == pair.hasRight) {
        return false; // Only one-sided rows open here; complete pairs use openPairAt.
    }
    if (!singleSideOpener_) {
        return false;
    }
    if (pendingSingleRequestId_ != 0 && pendingSingleRow_ == row) {
        return true;
    }
    cancelPendingOpen();
    const QUrl url = pair.hasLeft ? pair.leftUrl : pair.rightUrl;
    QString immediateError;
    const int requestId = singleSideOpener_(url, row, &immediateError);
    if (requestId <= 0) {
        errorText_ = immediateError.isEmpty() ? tr("Could not open image.") : immediateError;
        emit foldersChanged();
        return false;
    }
    pendingSingleRequestId_ = static_cast<quint64>(requestId);
    pendingSingleRow_ = row;
    errorText_.clear();
    emit pendingPairChanged();
    return true;
}

void ImageFolderPairModel::completeSingleSideOpen(const quint64 requestId,
                                                  const bool success,
                                                  QString error) {
    if (requestId == 0 || requestId != pendingSingleRequestId_) {
        return; // A late N/N+1 candidate can never advance N+2's selection.
    }
    const int row = pendingSingleRow_;
    pendingSingleRequestId_ = 0;
    pendingSingleRow_ = -1;

    if (!success) {
        errorText_ = std::move(error);
        if (errorText_.isEmpty()) {
            errorText_ = tr("Could not open image.");
        }
        emit pendingPairChanged();
        emit foldersChanged();
        emit pairOpenFinished(row, false, errorText_);
        return;
    }

    errorText_.clear();
    if (row >= 0 && currentPair_ != row) {
        currentPair_ = row;
        emit currentPairChanged();
    }
    emit pendingPairChanged();
    emit pairOpenFinished(row, true, QString{});
}

QVariantMap ImageFolderPairModel::pairUrlsAt(const int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) {
        return {};
    }
    const PairRow& pair = rows_[static_cast<std::size_t>(row)];
    return QVariantMap{
        {QStringLiteral("row"), row},
        {QStringLiteral("fileName"), pair.fileName},
        {QStringLiteral("leftUrl"), pair.leftUrl},
        {QStringLiteral("rightUrl"), pair.rightUrl},
        {QStringLiteral("hasLeft"), pair.hasLeft},
        {QStringLiteral("hasRight"), pair.hasRight},
        {QStringLiteral("hasBoth"), pair.hasLeft && pair.hasRight},
        {QStringLiteral("caseConflict"), pair.caseConflict},
        {QStringLiteral("caseConflictDetail"), pair.caseConflictDetail},
    };
}

void ImageFolderPairModel::completePairOpen(const quint64 requestId,
                                            const bool success,
                                            QString error) {
    if (requestId == 0 || requestId != pendingRequestId_) {
        return; // A late N/N+1 candidate can never advance N+2's selection.
    }
    const int row = pendingRow_;
    pendingRequestId_ = 0;
    pendingRow_ = -1;

    if (!success) {
        errorText_ = std::move(error);
        if (errorText_.isEmpty()) {
            errorText_ = tr("Could not open image pair.");
        }
        emit pendingPairChanged();
        emit foldersChanged();
        emit pairOpenFinished(row, false, errorText_);
        return;
    }

    errorText_.clear();
    if (row >= 0 && currentPair_ != row) {
        currentPair_ = row;
        emit currentPairChanged();
    }
    emit pendingPairChanged();
    emit pairOpenFinished(row, true, QString{});
}

int ImageFolderPairModel::firstCompleteRow() const noexcept {
    for (std::size_t row = 0U; row < rows_.size(); ++row) {
        if (rows_[row].hasLeft && rows_[row].hasRight) {
            return static_cast<int>(row);
        }
    }
    return -1;
}

int ImageFolderPairModel::stepCompleteRow(const int delta) const {
    if (rows_.empty() || delta == 0) {
        return -1;
    }
    const int count = static_cast<int>(rows_.size());
    const int desiredRow = pendingRequestId_ != 0
                               ? pendingRow_
                               : (pendingSingleRequestId_ != 0 ? pendingSingleRow_ : currentPair_);
    int row = desiredRow >= 0 ? desiredRow : 0;
    for (int step = 0; step < count; ++step) {
        row = ((row + delta) % count + count) % count;
        if (rows_[static_cast<std::size_t>(row)].hasLeft &&
            rows_[static_cast<std::size_t>(row)].hasRight) {
            return row;
        }
    }
    return -1;
}

} // namespace dvs::ui
