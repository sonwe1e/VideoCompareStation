#include "dvs/ui/VideoFolderModel.h"

#include "RecentVideoFiles.h"

#include "dvs/application/MediaPaths.h"
#include "dvs/ui/ReviewController.h"
#include "dvs/ui/ReviewPreferencesController.h"
#include "dvs/ui/ReviewShellController.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace dvs::ui {
namespace {

constexpr std::size_t kMaximumFolderFiles = 100'000U;

struct FolderExecutor final {
    std::mutex mutex;
    std::optional<VideoFolderModel::Dependencies::Task> next;
    bool running = false;
};

// One detached worker and one latest queued job per browser, including slow filesystem calls.
// No QObject is retained and no GUI-thread join is needed when the window closes.
[[nodiscard]] auto folderScheduler() {
    const auto executor = std::make_shared<FolderExecutor>();
    return [executor](VideoFolderModel::Dependencies::Task task) {
        const std::lock_guard lock{executor->mutex};
        executor->next = std::move(task);
        if (executor->running) {
            return;
        }
        executor->running = true;
        try {
            std::thread([executor] {
                for (;;) {
                    std::optional<VideoFolderModel::Dependencies::Task> next;
                    {
                        const std::lock_guard guard{executor->mutex};
                        next = std::exchange(executor->next, std::nullopt);
                        if (!next) {
                            executor->running = false;
                            return;
                        }
                    }
                    (*next)();
                }
            }).detach();
        } catch (const std::exception&) {
            executor->running = false;
            executor->next.reset();
            throw;
        }
    };
}

// Compare arbitrary-length decimal runs without integer conversion, locale/ICU dependencies
// or overflow. Equal numeric spellings use the original name as a deterministic tie-breaker.
[[nodiscard]] bool naturalLess(const QString& first, const QString& second) {
    const QString a = first.toCaseFolded();
    const QString b = second.toCaseFolded();
    const auto digit = [](const QChar c) { return c >= u'0' && c <= u'9'; };
    qsizetype i = 0;
    qsizetype j = 0;
    while (i < a.size() && j < b.size()) {
        if (digit(a[i]) && digit(b[j])) {
            qsizetype endA = i;
            qsizetype endB = j;
            while (endA < a.size() && digit(a[endA])) {
                ++endA;
            }
            while (endB < b.size() && digit(b[endB])) {
                ++endB;
            }
            while (i < endA && a[i] == u'0') {
                ++i;
            }
            while (j < endB && b[j] == u'0') {
                ++j;
            }
            if (endA - i != endB - j) {
                return endA - i < endB - j;
            }
            while (i < endA) {
                if (a[i] != b[j]) {
                    return a[i] < b[j];
                }
                ++i;
                ++j;
            }
            i = endA;
            j = endB;
        } else {
            if (a[i] != b[j]) {
                return a[i] < b[j];
            }
            ++i;
            ++j;
        }
    }
    if (i < a.size() || j < b.size()) {
        return i == a.size();
    }
    return first < second;
}

} // namespace

struct VideoFolderModel::ScanResult final {
    std::uint64_t generation = 0;
    QUrl folder;
    QString name;
    std::vector<FileEntry> files;
    QString error;
};

struct VideoFolderModel::Inbox final {
    std::mutex mutex;
    bool closed = false;
    std::optional<ScanResult> result;
};

VideoFolderModel::VideoFolderModel(QObject* const parent)
    : VideoFolderModel(Dependencies{}, parent) {}

VideoFolderModel::VideoFolderModel(Dependencies dependencies, QObject* const parent)
    : QAbstractListModel(parent), dependencies_(std::move(dependencies)),
      inbox_(std::make_shared<Inbox>()), scanTimer_(new QTimer(this)) {
    dependencies_.maximumFiles =
        std::clamp(dependencies_.maximumFiles, std::size_t{1}, kMaximumFolderFiles);
    if (!dependencies_.schedule) {
        dependencies_.schedule = folderScheduler();
    }
    scanTimer_->setInterval(20);
    QObject::connect(scanTimer_, &QTimer::timeout, this, [this] { drainScan(); });
}

void VideoFolderModel::attachPreferences(ReviewPreferencesController& preferences) {
    const QPointer<ReviewPreferencesController> guarded{&preferences};
    rememberVideo_ = [guarded](const QUrl& url) {
        if (guarded) {
            guarded->rememberVideoFile(url);
        }
    };
    QObject::connect(&preferences,
                     &ReviewPreferencesController::recentVideoFilesChanged,
                     this,
                     [this, &preferences] {
                         synchronizeRecentFiles(preferences.recentVideoFiles());
                     });
    synchronizeRecentFiles(preferences.recentVideoFiles());
}

void VideoFolderModel::attachPlayback(ReviewController& controller, ReviewShellController& shell) {
    dependencies_.openVideo = [&shell](const QUrl& url) { return shell.openVideo(url); };
    dependencies_.cancelOpen = [&shell](const qulonglong id) { shell.cancelQueuedIntent(id); };
    dependencies_.play = [&controller] { return controller.play(); };
    QObject::connect(&shell, &ReviewShellController::stateChanged, this, [this, &shell] {
        synchronizeSources(shell.activeSources());
    });
    QObject::connect(
        &shell,
        &ReviewShellController::intentFinished,
        this,
        [this](const qulonglong id, const int kind, const int outcome, const QString& error) {
            completeOpen(id, outcome == 0, error);
            if (outcome == 0 && kind == ReviewShellController::OpenSourcesIntent) {
                recordCommittedVideo();
            }
        });
    QObject::connect(
        &shell,
        &ReviewShellController::intentEvent,
        this,
        [this](const qulonglong id, const int status, const int, const int, const int) {
            // A newer File-menu/Explorer/source intent also supersedes browser autoplay.
            if (id != pendingIntentId_ &&
                (status == ReviewShellController::RunningStatus ||
                 status == ReviewShellController::QueuedStatus)) {
                cancelPendingOpen();
                // A new accepted ordinary open supersedes the previous browser error too.
                // Do this on acceptance, not an older intent's eventual success terminal.
                if (!errorText_.isEmpty()) {
                    errorText_.clear();
                    Q_EMIT stateChanged();
                }
            }
            if (status == ReviewShellController::CanceledStatus ||
                status == ReviewShellController::ReplacedStatus ||
                status == ReviewShellController::RejectedStatus) {
                completeOpen(id, false, tr("打开请求已取消或被替换。"));
            }
        });
    synchronizeSources(shell.activeSources());
}

VideoFolderModel::~VideoFolderModel() {
    if (scanCanceled_) {
        scanCanceled_->store(true);
    }
    // Workers own only their token and mailbox, never this QObject. Destruction does not join
    // filesystem work (a disconnected/mapped drive may take time to return).
    const std::lock_guard lock{inbox_->mutex};
    inbox_->closed = true;
}

QUrl VideoFolderModel::folderUrl() const {
    return folderUrl_;
}
QString VideoFolderModel::folderName() const {
    return folderName_;
}
QString VideoFolderModel::folderPath() const {
    return folderUrl_.toLocalFile();
}
int VideoFolderModel::fileCount() const noexcept {
    return static_cast<int>(files_.size());
}
bool VideoFolderModel::scanning() const noexcept {
    return scanning_;
}
QString VideoFolderModel::errorText() const {
    return errorText_.isEmpty() ? folderErrorText_ : errorText_;
}
int VideoFolderModel::currentRow() const noexcept {
    return currentRow_;
}
int VideoFolderModel::pendingRow() const noexcept {
    return pendingRow_;
}
bool VideoFolderModel::openPending() const noexcept {
    return pendingIntentId_ != 0;
}
int VideoFolderModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : fileCount();
}
QVariant VideoFolderModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= fileCount()) {
        return {};
    }
    const auto& file = files_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case FileNameRole:
        return file.name;
    case FileUrlRole:
        return file.url;
    default:
        return {};
    }
}
QHash<int, QByteArray> VideoFolderModel::roleNames() const {
    return {{FileNameRole, "fileName"}, {FileUrlRole, "fileUrl"}};
}

VideoFolderModel::ScanResult
VideoFolderModel::scan(const QUrl& folder,
                       const std::uint64_t generation,
                       const std::shared_ptr<std::atomic_bool>& canceled,
                       const std::size_t maximumFiles) {
    ScanResult result{.generation = generation, .folder = folder};
    if (canceled->load()) {
        return result;
    }
    const QDir directory{folder.toLocalFile()};
    if (!directory.exists() || !QFileInfo{directory.absolutePath()}.isReadable()) {
        result.error = tr("无法读取该文件夹，请检查路径或权限。");
        return result;
    }
    result.folder = QUrl::fromLocalFile(directory.absolutePath());
    result.name = directory.dirName().isEmpty() ? directory.absolutePath() : directory.dirName();
    QDirIterator iterator{directory.absolutePath(),
                          QDir::Files | QDir::Readable | QDir::NoSymLinks | QDir::NoDotAndDotDot};
    while (!canceled->load() && iterator.hasNext()) {
        iterator.next();
        const QString name = iterator.fileName();
        // Only the suffix enters the extension classifier; Unicode paths stay in QUrl. Use
        // native Windows wide characters even for unsupported Unicode extensions.
        const auto suffix = std::filesystem::path{
            (QStringLiteral("video.") + QFileInfo{name}.suffix()).toStdWString()};
        if (!application::isVideoPath(suffix)) {
            continue;
        }
        if (result.files.size() >= maximumFiles) {
            result.files.clear();
            result.error = tr("视频文件过多，请选择较小的文件夹。");
            return result;
        }
        result.files.push_back({name, QUrl::fromLocalFile(iterator.filePath())});
    }
    if (!canceled->load()) {
        std::sort(result.files.begin(), result.files.end(), [](const auto& a, const auto& b) {
            return naturalLess(a.name, b.name);
        });
    }
    return result;
}

bool VideoFolderModel::loadFolder(const QUrl& folder) {
    cancelPendingOpen();
    errorText_.clear();
    return startScan(folder);
}

bool VideoFolderModel::startScan(const QUrl& folder) {
    cancelScan();
    const QString path = folder.toLocalFile();
    if (!folder.isLocalFile() || !folder.host().isEmpty() || !QDir::isAbsolutePath(path) ||
        path.startsWith(QStringLiteral("//")) || path.startsWith(QStringLiteral("\\\\"))) {
        folderErrorText_ = tr("请选择本地文件夹。");
        Q_EMIT stateChanged();
        return false;
    }
    folderErrorText_.clear();
    scanning_ = true;
    scanningFolderUrl_ = folder;
    const auto canceled = std::make_shared<std::atomic_bool>(false);
    scanCanceled_ = canceled;
    const auto inbox = inbox_;
    const auto generation = scanGeneration_;
    const auto limit = dependencies_.maximumFiles;
    try {
        dependencies_.schedule([folder, generation, limit, canceled, inbox] {
            ScanResult result;
            try {
                result = scan(folder, generation, canceled, limit);
            } catch (const std::exception&) {
                result.generation = generation;
                result.error = tr("读取文件夹失败。");
            }
            const std::lock_guard lock{inbox->mutex};
            if (!inbox->closed && !canceled->load()) {
                inbox->result = std::move(result);
            }
        });
    } catch (const std::exception&) {
        canceled->store(true);
        scanning_ = false;
        scanningFolderUrl_.clear();
        folderErrorText_ = tr("无法启动文件夹读取。");
        Q_EMIT stateChanged();
        return false;
    }
    scanTimer_->start();
    Q_EMIT stateChanged();
    return true;
}

void VideoFolderModel::drainScan() {
    std::optional<ScanResult> result;
    {
        const std::lock_guard lock{inbox_->mutex};
        result = std::exchange(inbox_->result, std::nullopt);
    }
    if (!result || result->generation != scanGeneration_) {
        return;
    }
    scanning_ = false;
    scanningFolderUrl_.clear();
    scanTimer_->stop();
    folderErrorText_ = result->error;
    if (result->error.isEmpty()) {
        beginResetModel();
        files_ = std::move(result->files);
        folderUrl_ = result->folder;
        folderName_ = result->name;
        currentRow_ = rowForUrl(currentUrl_);
        pendingRow_ = rowForUrl(pendingUrl_);
        endResetModel();
    }
    Q_EMIT stateChanged();
}

bool VideoFolderModel::refreshFolder() {
    return !folderUrl_.isEmpty() && loadFolder(folderUrl_);
}
void VideoFolderModel::cancelScan() {
    const bool wasScanning = scanning_;
    scanningFolderUrl_.clear();
    ++scanGeneration_;
    if (scanCanceled_) {
        scanCanceled_->store(true);
    }
    scanTimer_->stop();
    scanning_ = false;
    // Remove a previously completed reply before the next worker starts. Stale workers cannot
    // overwrite it afterwards because cancellation and publishing share the mailbox fence.
    {
        const std::lock_guard lock{inbox_->mutex};
        inbox_->result.reset();
    }
    if (wasScanning) {
        Q_EMIT stateChanged();
    }
}
void VideoFolderModel::clear() {
    cancelScan();
    cancelPendingOpen();
    beginResetModel();
    files_.clear();
    folderUrl_.clear();
    folderName_.clear();
    errorText_.clear();
    folderErrorText_.clear();
    currentRow_ = -1;
    endResetModel();
    Q_EMIT stateChanged();
}

int VideoFolderModel::rowForUrl(const QUrl& url) const {
    if (url.isEmpty()) {
        return -1;
    }
    const auto found = std::find_if(
        files_.begin(), files_.end(), [&](const auto& file) {
            return detail::sameVideoUrl(file.url, url);
        });
    return found == files_.end() ? -1 : static_cast<int>(found - files_.begin());
}
void VideoFolderModel::synchronizeSources(const QVariantList& sources) {
    const QUrl current =
        sources.size() == 1 ? detail::localVideoUrl(sources.front().toUrl()) : QUrl{};
    if (current == currentUrl_) {
        return;
    }
    currentUrl_ = current;
    currentRow_ = rowForUrl(currentUrl_);
    followCurrentFolder();
    Q_EMIT stateChanged();
}

void VideoFolderModel::followCurrentFolder() {
    if (!currentUrl_.isEmpty()) {
        const auto folder =
            QUrl::fromLocalFile(QFileInfo{currentUrl_.toLocalFile()}.absolutePath());
        if (folder != scanningFolderUrl_ &&
            (scanning_ || folder != folderUrl_ || currentRow_ < 0)) {
            // A committed A may arrive while newer B is queued. Following A is enumeration
            // only: never cancel B's intent or suppress a matching browser autoplay terminal.
            startScan(folder);
        }
    }
}

QUrl VideoFolderModel::currentUrl() const {
    return currentUrl_;
}

int VideoFolderModel::recentCurrentRow() const {
    if (currentUrl_.isEmpty()) {
        return -1;
    }
    for (qsizetype index = 0; index < recentUrls_.size(); ++index) {
        if (detail::sameVideoUrl(QUrl{recentUrls_[index]}, currentUrl_)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

QVariantList VideoFolderModel::recentFiles() const {
    QVariantList result;
    result.reserve(recentUrls_.size());
    for (const QString& value : recentUrls_) {
        const QUrl url{value};
        result.push_back(
            QVariantMap{{QStringLiteral("fileName"), QFileInfo{url.toLocalFile()}.fileName()},
                        {QStringLiteral("fileUrl"), url}});
    }
    return result;
}

void VideoFolderModel::synchronizeRecentFiles(const QStringList& files) {
    const auto recent = detail::mergeRecentVideoFiles(files, {});
    if (recent == recentUrls_) {
        return;
    }
    recentUrls_ = recent;
    Q_EMIT recentFilesChanged();
    Q_EMIT stateChanged();
}

void VideoFolderModel::recordCommittedVideo() {
    if (currentUrl_.isEmpty()) {
        return;
    }
    followCurrentFolder();
    synchronizeRecentFiles(detail::mergeRecentVideoFiles(
        {currentUrl_.toString(QUrl::FullyEncoded)}, recentUrls_));
    if (rememberVideo_) {
        rememberVideo_(currentUrl_);
    }
    Q_EMIT currentFileOpened();
}

bool VideoFolderModel::openRecent(const int row) {
    if (row < 0 || row >= recentUrls_.size()) {
        return false;
    }
    const QUrl url{recentUrls_[row]};
    return openUrl(url, rowForUrl(url));
}

bool VideoFolderModel::openFile(const QUrl& url) {
    return openUrl(url, rowForUrl(url));
}

bool VideoFolderModel::openAt(const int row) {
    if (scanning_ || row < 0 || row >= fileCount() || !dependencies_.openVideo) {
        return false;
    }
    return openUrl(files_[static_cast<std::size_t>(row)].url, row);
}

bool VideoFolderModel::openUrl(const QUrl& url, const int row) {
    if (!dependencies_.openVideo || detail::localVideoUrl(url).isEmpty()) {
        return false;
    }
    cancelPendingOpen();
    const auto intent = dependencies_.openVideo(url);
    if (intent == 0) {
        errorText_ = tr("无法提交视频打开请求。");
        Q_EMIT stateChanged();
        return false;
    }
    pendingUrl_ = url;
    pendingRow_ = row;
    pendingIntentId_ = intent;
    errorText_.clear();
    Q_EMIT stateChanged();
    return true;
}

bool VideoFolderModel::step(const int delta) {
    if (delta == 0 || files_.empty()) {
        return false;
    }
    const int anchor = openPending() ? pendingRow_ : currentRow_;
    const int target =
        anchor < 0 ? (delta > 0 ? 0 : fileCount() - 1) : anchor + (delta > 0 ? 1 : -1);
    return openAt(target); // No wrap or implicit autoplay at EOF.
}
void VideoFolderModel::cancelPendingOpen() {
    const auto intent = std::exchange(pendingIntentId_, 0);
    pendingRow_ = -1;
    pendingUrl_.clear();
    if (intent != 0 && dependencies_.cancelOpen) {
        dependencies_.cancelOpen(intent);
    }
    if (intent != 0) {
        Q_EMIT stateChanged();
    }
}
void VideoFolderModel::completeOpen(const qulonglong intentId,
                                    const bool success,
                                    const QString& error) {
    if (intentId == 0 || intentId != pendingIntentId_) {
        return;
    }
    const QUrl requested = pendingUrl_;
    pendingIntentId_ = 0;
    pendingRow_ = -1;
    pendingUrl_.clear();
    if (!success || currentUrl_.isEmpty() ||
        !detail::sameVideoUrl(currentUrl_, detail::localVideoUrl(requested))) {
        errorText_ = error.isEmpty() ? tr("无法打开该视频，原视频保持不变。") : error;
    } else if (!dependencies_.play || !dependencies_.play()) {
        errorText_ = tr("视频已打开，但未能开始播放。");
    } else {
        errorText_.clear();
    }
    Q_EMIT stateChanged();
}

} // namespace dvs::ui
