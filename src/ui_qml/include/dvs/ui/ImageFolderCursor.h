#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

namespace dvs::ui {

// Directory cursor for single-image review (FLOW3). It remembers which still images live next
// to the file the user opened, so the workspace can step to the previous/next image without
// reopening a dialog.
//
// The cursor is deliberately passive: it never opens anything. Main.qml asks for a URL and
// routes it through the ordinary asynchronous open, which keeps the existing request identity
// rules - a failed decode keeps the current image, and a stale request cannot commit over a
// newer one.
//
// Only the current directory is listed (no recursion), names are ordered with the same natural
// order the video folder list uses, and a directory holding more than kMaximumImages images
// disables browsing instead of offering a silently truncated sequence.
class ImageFolderCursor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString folderPath READ folderPath NOTIFY folderChanged)
    Q_PROPERTY(QString folderName READ folderName NOTIFY folderChanged)
    Q_PROPERTY(QString currentFileName READ currentFileName NOTIFY positionChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY positionChanged)
    Q_PROPERTY(int count READ count NOTIFY folderChanged)
    Q_PROPERTY(bool canGoNext READ canGoNext NOTIFY positionChanged)
    Q_PROPERTY(bool canGoPrevious READ canGoPrevious NOTIFY positionChanged)

public:
    static constexpr int kMaximumImages = 10000;

    explicit ImageFolderCursor(QObject* parent = nullptr);

    // Rescans the file's own directory and places the cursor on that file. A missing file, an
    // unsupported suffix or a folder above kMaximumImages clears the cursor instead of
    // guessing a position, because a wrong index would offer the wrong neighbour.
    Q_INVOKABLE void setCurrentFile(const QString& filePath);
    Q_INVOKABLE void clear();

    [[nodiscard]] Q_INVOKABLE QUrl nextUrl() const;
    [[nodiscard]] Q_INVOKABLE QUrl previousUrl() const;
    [[nodiscard]] Q_INVOKABLE QUrl firstUrl() const;
    [[nodiscard]] Q_INVOKABLE QUrl lastUrl() const;

    [[nodiscard]] QString folderPath() const noexcept;
    [[nodiscard]] QString folderName() const noexcept;
    [[nodiscard]] QString currentFileName() const noexcept;
    // 1-based position for display; 0 when the cursor holds no image.
    [[nodiscard]] int currentIndex() const noexcept;
    [[nodiscard]] int count() const noexcept;
    [[nodiscard]] bool canGoNext() const noexcept;
    [[nodiscard]] bool canGoPrevious() const noexcept;

signals:
    void folderChanged();
    void positionChanged();

private:
    [[nodiscard]] QUrl urlAt(int index) const;
    void resetToEmpty();

    QString folderPath_;
    QString folderName_;
    QList<QUrl> images_;
    QString currentFileName_;
    int index_ = -1;
};

} // namespace dvs::ui
