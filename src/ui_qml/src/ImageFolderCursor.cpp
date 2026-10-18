#include "dvs/ui/ImageFolderCursor.h"

#include "dvs/application/MediaPaths.h"
#include "dvs/ui/NaturalOrder.h"

#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QStringList>

#include <algorithm>
#include <filesystem>
#include <utility>
#include <vector>

namespace dvs::ui {
namespace {

[[nodiscard]] bool isStillImageName(const QString& fileName) {
    return application::isStillImagePath(std::filesystem::path{fileName.toStdWString()});
}

} // namespace

ImageFolderCursor::ImageFolderCursor(QObject* const parent) : QObject(parent) {}

void ImageFolderCursor::setCurrentFile(const QString& filePath) {
    const QString candidate = filePath.trimmed();
    if (candidate.isEmpty()) {
        clear();
        return;
    }

    const QFileInfo info(candidate);
    if (!info.exists() || !info.isFile() || !isStillImageName(info.fileName())) {
        clear();
        return;
    }

    const QDir directory = info.absoluteDir();
    const QFileInfoList entries =
        directory.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::NoSort);

    std::vector<QUrl> images;
    images.reserve(static_cast<std::size_t>(entries.size()));
    for (const QFileInfo& entry : entries) {
        if (!isStillImageName(entry.fileName())) {
            continue;
        }
        if (static_cast<int>(images.size()) >= kMaximumImages) {
            // A truncated sequence would offer a neighbour that is not the real neighbour.
            clear();
            return;
        }
        images.push_back(QUrl::fromLocalFile(entry.absoluteFilePath()));
    }

    std::sort(images.begin(), images.end(), [](const QUrl& first, const QUrl& second) {
        return naturalLess(first.fileName(), second.fileName());
    });

    const QString target = info.absoluteFilePath();
    int found = -1;
    for (std::size_t i = 0; i < images.size(); ++i) {
        if (QString::compare(images[i].toLocalFile(), target, Qt::CaseInsensitive) == 0) {
            found = static_cast<int>(i);
            break;
        }
    }
    if (found < 0) {
        clear();
        return;
    }

    const QString newFolderPath = directory.absolutePath();
    const QString newFolderName = directory.dirName();
    const QString newName = info.fileName();
    const bool folderMoved = newFolderPath != folderPath_ || newFolderName != folderName_;
    const bool positionMoved = found != index_ || newName != currentFileName_;

    folderPath_ = newFolderPath;
    folderName_ = newFolderName;
    images_.clear();
    for (const QUrl& url : images) {
        images_.push_back(url);
    }
    index_ = found;
    currentFileName_ = newName;

    if (folderMoved) {
        emit folderChanged();
    }
    if (positionMoved) {
        emit positionChanged();
    }
}

void ImageFolderCursor::clear() {
    if (index_ < 0 && images_.isEmpty() && folderPath_.isEmpty()) {
        return;
    }
    resetToEmpty();
    emit folderChanged();
    emit positionChanged();
}

void ImageFolderCursor::resetToEmpty() {
    folderPath_.clear();
    folderName_.clear();
    images_.clear();
    currentFileName_.clear();
    index_ = -1;
}

QUrl ImageFolderCursor::nextUrl() const {
    return urlAt(index_ + 1);
}

QUrl ImageFolderCursor::previousUrl() const {
    return urlAt(index_ - 1);
}

QUrl ImageFolderCursor::firstUrl() const {
    return urlAt(0);
}

QUrl ImageFolderCursor::lastUrl() const {
    return urlAt(static_cast<int>(images_.size()) - 1);
}

QUrl ImageFolderCursor::urlAt(const int index) const {
    if (index < 0 || index >= images_.size()) {
        return {};
    }
    return images_.at(index);
}

QString ImageFolderCursor::folderPath() const noexcept {
    return folderPath_;
}

QString ImageFolderCursor::folderName() const noexcept {
    return folderName_;
}

QString ImageFolderCursor::currentFileName() const noexcept {
    return currentFileName_;
}

int ImageFolderCursor::currentIndex() const noexcept {
    return index_ >= 0 ? index_ + 1 : 0;
}

int ImageFolderCursor::count() const noexcept {
    return static_cast<int>(images_.size());
}

bool ImageFolderCursor::canGoNext() const noexcept {
    return index_ >= 0 && index_ + 1 < images_.size();
}

bool ImageFolderCursor::canGoPrevious() const noexcept {
    return index_ > 0;
}

} // namespace dvs::ui
