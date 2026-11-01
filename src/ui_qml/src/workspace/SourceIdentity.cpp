#include "dvs/ui/SourceIdentity.h"

#include <QDir>
#include <QFileInfo>

namespace dvs::ui {

QString composeSourceIdentity(const QString& path,
                              const std::int64_t byteSize,
                              const std::int64_t modifiedMs) {
    return QStringLiteral("%1|%2|%3")
        .arg(QDir::cleanPath(path).toCaseFolded())
        .arg(byteSize)
        .arg(modifiedMs);
}

QString canonicalSourceIdentity(const QUrl& source) {
    if (!source.isLocalFile()) {
        return source.toString(QUrl::FullyEncoded).toCaseFolded();
    }

    const QFileInfo file{source.toLocalFile()};
    QString path = file.canonicalFilePath();
    if (path.isEmpty()) {
        path = file.absoluteFilePath();
    }
    return composeSourceIdentity(path,
                                 file.exists() ? file.size() : -1,
                                 file.exists() ? file.lastModified().toMSecsSinceEpoch() : -1);
}

} // namespace dvs::ui
