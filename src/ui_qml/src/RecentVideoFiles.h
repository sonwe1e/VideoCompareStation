#pragma once

#include "dvs/application/MediaPaths.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QUrl>

namespace dvs::ui::detail {

constexpr qsizetype kMaximumRecentVideoFiles = 50;

// Pure path validation only. Existence/probing stays off the GUI thread; stale history is
// allowed, and an attempted open reports the normal transactional media error.
inline QUrl localVideoUrl(const QUrl& url) {
    const QString path = url.toLocalFile();
    if (!url.isLocalFile() || !url.host().isEmpty() || url.hasQuery() || url.hasFragment() ||
        !QDir::isAbsolutePath(path) || path.startsWith(QStringLiteral("//")) ||
        path.startsWith(QStringLiteral("\\\\"))) {
        return {};
    }
    const QString suffix = QFileInfo{path}.suffix().toLower();
    // Feed only an ASCII extension to the shared classifier. Unknown Unicode suffixes cannot
    // name any of the supported video types, and must not require filesystem transcoding.
    for (const QChar character : suffix) {
        if (character.unicode() > 127U) {
            return {};
        }
    }
    const auto file = std::filesystem::path{("video." + suffix).toStdString()};
    return application::isVideoPath(file) ? QUrl::fromLocalFile(QDir::cleanPath(path)) : QUrl{};
}

inline bool sameVideoUrl(const QUrl& first, const QUrl& second) {
#ifdef Q_OS_WIN
    return first.toLocalFile().compare(second.toLocalFile(), Qt::CaseInsensitive) == 0;
#else
    return first == second;
#endif
}

// Both inputs are newest first. Newly opened files take precedence over a late settings load.
inline QStringList mergeRecentVideoFiles(const QStringList& newer, const QStringList& older) {
    QStringList result;
    const auto append = [&result](const QStringList& files) {
        for (const QString& value : files) {
            const QUrl url = localVideoUrl(QUrl{value});
            if (url.isEmpty()) {
                continue;
            }
            bool duplicate = false;
            for (const QString& existing : result) {
                if (sameVideoUrl(QUrl{existing}, url)) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                result.push_back(url.toString(QUrl::FullyEncoded));
            }
            if (result.size() == kMaximumRecentVideoFiles) {
                break;
            }
        }
    };
    append(newer);
    if (result.size() < kMaximumRecentVideoFiles) {
        append(older);
    }
    return result;
}

} // namespace dvs::ui::detail
