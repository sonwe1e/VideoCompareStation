#include "dvs/ui/ImageFolderPairModel.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
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
    };
    return kSuffixes;
}

} // namespace

ImageFolderPairModel::ImageFolderPairModel(QObject* const parent) : QAbstractListModel(parent) {}

void ImageFolderPairModel::setPairOpener(PairOpener opener) {
    opener_ = std::move(opener);
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
    };
}

bool ImageFolderPairModel::isStillImageName(const QString& fileName) {
    return stillImageSuffixes().contains(QFileInfo{fileName}.suffix().toLower());
}

bool ImageFolderPairModel::loadFolders(const QUrl& left, const QUrl& right) {
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
    const auto scanSide = [](const QString& folderPath) {
        std::map<QString, QUrl> entries;
        const QDir folder{folderPath};
        const QFileInfoList files =
            folder.entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo& file : files) {
            if (!isStillImageName(file.fileName())) {
                continue;
            }
            entries.emplace(file.fileName().toCaseFolded(),
                            QUrl::fromLocalFile(file.absoluteFilePath()));
        }
        return entries;
    };

    const std::map<QString, QUrl> leftEntries = scanSide(leftPath);
    const std::map<QString, QUrl> rightEntries = scanSide(rightPath);

    std::vector<PairRow> nextRows;
    nextRows.reserve(leftEntries.size() + rightEntries.size());
    for (const auto& [key, url] : leftEntries) {
        PairRow row;
        row.fileName = QFileInfo{url.toLocalFile()}.fileName();
        row.leftUrl = url;
        row.hasLeft = true;
        const auto rightIt = rightEntries.find(key);
        if (rightIt != rightEntries.end()) {
            row.rightUrl = rightIt->second;
            row.hasRight = true;
        }
        nextRows.push_back(std::move(row));
    }
    for (const auto& [key, url] : rightEntries) {
        if (leftEntries.find(key) != leftEntries.end()) {
            continue;
        }
        PairRow row;
        row.fileName = QFileInfo{url.toLocalFile()}.fileName();
        row.rightUrl = url;
        row.hasRight = true;
        nextRows.push_back(std::move(row));
    }
    std::sort(nextRows.begin(), nextRows.end(), [](const PairRow& lhs, const PairRow& rhs) {
        return lhs.fileName.compare(rhs.fileName, Qt::CaseInsensitive) < 0;
    });

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

bool ImageFolderPairModel::openPairAt(const int row) {
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size() || !opener_) {
        return false;
    }
    const PairRow& pair = rows_[static_cast<std::size_t>(row)];
    if (!pair.hasLeft || !pair.hasRight) {
        return false;
    }
    opener_(pair.leftUrl, pair.rightUrl);
    return true;
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
    int row = currentPair_ >= 0 ? currentPair_ : 0;
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
