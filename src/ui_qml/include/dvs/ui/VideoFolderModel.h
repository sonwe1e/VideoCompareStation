#pragma once

#include <QAbstractListModel>
#include <QStringList>
#include <QUrl>
#include <QVariantList>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class QTimer;

namespace dvs::ui {

class ReviewController;
class ReviewShellController;
class ReviewPreferencesController;

// A lightweight, top-level folder browser. Enumeration never probes/decodes video and never
// touches the GUI thread. The shell owns media commands; only its matching successful terminal
// may start playback. Current selection is projected from committed sources, not from a click.
class VideoFolderModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QUrl folderUrl READ folderUrl NOTIFY stateChanged)
    Q_PROPERTY(QString folderName READ folderName NOTIFY stateChanged)
    Q_PROPERTY(QString folderPath READ folderPath NOTIFY stateChanged)
    Q_PROPERTY(int fileCount READ fileCount NOTIFY stateChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(int currentRow READ currentRow NOTIFY stateChanged)
    Q_PROPERTY(int pendingRow READ pendingRow NOTIFY stateChanged)
    Q_PROPERTY(bool openPending READ openPending NOTIFY stateChanged)
    Q_PROPERTY(QVariantList recentFiles READ recentFiles NOTIFY recentFilesChanged)
    Q_PROPERTY(QUrl currentUrl READ currentUrl NOTIFY stateChanged)
    Q_PROPERTY(int recentCurrentRow READ recentCurrentRow NOTIFY stateChanged)

public:
    struct Dependencies final {
        using Task = std::function<void()>;
        std::function<void(Task)> schedule;
        std::function<qulonglong(const QUrl&)> openVideo;
        std::function<void(qulonglong)> cancelOpen;
        std::function<bool()> play;
        std::size_t maximumFiles = 100'000U;
    };

    enum Role { FileNameRole = Qt::UserRole + 1, FileUrlRole };
    Q_ENUM(Role)

    explicit VideoFolderModel(QObject* parent = nullptr);
    explicit VideoFolderModel(Dependencies dependencies, QObject* parent = nullptr);
    ~VideoFolderModel() override;

    [[nodiscard]] QUrl folderUrl() const;
    [[nodiscard]] QString folderName() const;
    [[nodiscard]] QString folderPath() const;
    [[nodiscard]] int fileCount() const noexcept;
    [[nodiscard]] bool scanning() const noexcept;
    [[nodiscard]] QString errorText() const;
    [[nodiscard]] int currentRow() const noexcept;
    [[nodiscard]] int pendingRow() const noexcept;
    [[nodiscard]] bool openPending() const noexcept;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] QVariantList recentFiles() const;
    [[nodiscard]] QUrl currentUrl() const;
    [[nodiscard]] int recentCurrentRow() const;
    void attachPreferences(ReviewPreferencesController& preferences);
    void attachPlayback(ReviewController& controller, ReviewShellController& shell);
    Q_INVOKABLE bool loadFolder(const QUrl& folder);
    Q_INVOKABLE bool refreshFolder();
    Q_INVOKABLE void cancelScan();
    Q_INVOKABLE void clear();
    Q_INVOKABLE bool openAt(int row);
    Q_INVOKABLE bool openRecent(int row);
    Q_INVOKABLE bool step(int delta);
    Q_INVOKABLE void cancelPendingOpen();
    void synchronizeSources(const QVariantList& sources);
    void completeOpen(qulonglong intentId, bool success, const QString& error);
    void recordCommittedVideo();

signals:
    void stateChanged();
    void recentFilesChanged();
    void currentFileOpened();

private:
    struct FileEntry final {
        QString name;
        QUrl url;
    };
    struct ScanResult;
    struct Inbox;
    static ScanResult scan(const QUrl& folder,
                           std::uint64_t generation,
                           const std::shared_ptr<std::atomic_bool>& canceled,
                           std::size_t maximumFiles);
    void drainScan();
    bool startScan(const QUrl& folder);
    void followCurrentFolder();
    bool openUrl(const QUrl& url, int row);
    void synchronizeRecentFiles(const QStringList& files);
    [[nodiscard]] int rowForUrl(const QUrl& url) const;

    Dependencies dependencies_;
    std::shared_ptr<Inbox> inbox_;
    std::shared_ptr<std::atomic_bool> scanCanceled_;
    QTimer* scanTimer_ = nullptr;
    std::uint64_t scanGeneration_ = 0;
    std::vector<FileEntry> files_;
    QUrl folderUrl_;
    QUrl scanningFolderUrl_;
    QStringList recentUrls_;
    std::function<void(const QUrl&)> rememberVideo_;
    QString folderName_;
    QString errorText_;
    QString folderErrorText_;
    bool scanning_ = false;
    QUrl currentUrl_;
    int currentRow_ = -1;
    QUrl pendingUrl_;
    int pendingRow_ = -1;
    qulonglong pendingIntentId_ = 0;
};

} // namespace dvs::ui
