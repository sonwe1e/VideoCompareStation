#include "dvs/ui/VideoTaskHost.h"

#include <QFileInfo>
#include <QVariant>

#include <algorithm>

namespace dvs::ui {
namespace {

constexpr int kMaximumVideoTasks = 12;
constexpr int kMaximumSourcesPerTask = 3;

[[nodiscard]] QUrl asUrl(const QVariant& value) {
    if (value.metaType().id() == QMetaType::QUrl) {
        return value.toUrl();
    }
    const QString text = value.toString();
    if (text.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) {
        return QUrl{text};
    }
    return QUrl::fromLocalFile(text);
}

// Matching-phase identity, identical to the still-image host (design §2.1): the normalized
// absolute local path, or the URL text for non-local files.
[[nodiscard]] QString identityForUrl(const QUrl& url) {
    const QString local = url.toLocalFile();
    if (!local.isEmpty()) {
        return QFileInfo{local}.absoluteFilePath();
    }
    return url.toString();
}

[[nodiscard]] QString fileNameFor(const QString& path) {
    if (path.isEmpty()) {
        return {};
    }
    const QString name = QFileInfo{path}.fileName();
    return name.isEmpty() ? path : name;
}

// The last path segment of the file's folder - the only part that stays short enough for a
// tab. Empty for a bare file name with no folder.
[[nodiscard]] QString parentFolderNameFor(const QString& path) {
    if (path.isEmpty()) {
        return {};
    }
    const QString folder = QFileInfo{path}.absolutePath();
    const qsizetype slash = folder.lastIndexOf(QLatin1Char('/'));
    return slash >= 0 ? folder.mid(slash + 1) : folder;
}

// Multiset equality of a task's source identities: order-independent and case-insensitive
// (decision e), so B↔A and A↔B with the same reference source are the same task.
[[nodiscard]] bool sameIdentityMultiset(const QStringList& first, const QStringList& second) {
    if (first.size() != second.size()) {
        return false;
    }
    QStringList left = first;
    QStringList right = second;
    for (QString& identity : left) {
        identity = identity.toLower();
    }
    for (QString& identity : right) {
        identity = identity.toLower();
    }
    left.sort();
    right.sort();
    return left == right;
}

} // namespace

VideoTaskHost::VideoTaskHost(QObject* parent) : QObject(parent) {}

VideoTaskHost::~VideoTaskHost() = default;

int VideoTaskHost::count() const noexcept {
    return static_cast<int>(tasks_.size());
}

int VideoTaskHost::activeIndex() const noexcept {
    return activeIndex_;
}

QVariantList VideoTaskHost::tabs() const {
    QVariantList result;
    result.reserve(static_cast<qsizetype>(tasks_.size()));
    for (std::size_t index = 0; index < tasks_.size(); ++index) {
        const Task& task = tasks_[index];
        QVariantMap entry;
        entry.insert(QStringLiteral("index"), static_cast<int>(index));
        entry.insert(QStringLiteral("title"), titleFor(task));
        entry.insert(QStringLiteral("detail"), detailFor(task));
        entry.insert(QStringLiteral("sourceCount"), static_cast<int>(task.urlIdentities.size()));
        entry.insert(QStringLiteral("comparison"), task.urlIdentities.size() > 1);
        // Session-fact projection for the QML switch skeleton (design §4.3): the slot-order
        // URL list and the recorded reference slot are what "the runtime session still shows
        // this task" is compared against; both follow the record through group evolution.
        QStringList urlTexts;
        urlTexts.reserve(task.urls.size());
        for (const QUrl& url : task.urls) {
            urlTexts.push_back(url.toString());
        }
        entry.insert(QStringLiteral("urls"), urlTexts);
        entry.insert(QStringLiteral("referenceIndex"), task.referenceIndex);
        entry.insert(QStringLiteral("loading"), task.loading);
        entry.insert(QStringLiteral("restoring"), task.restoring);
        entry.insert(QStringLiteral("active"), static_cast<int>(index) == activeIndex_);
        result.push_back(entry);
    }
    return result;
}

int VideoTaskHost::openTask(const QVariantList& urls,
                            const int referenceIndex,
                            const QString& purpose) {
    if (urls.isEmpty() || urls.size() > kMaximumSourcesPerTask) {
        emit openFailed(tr("一次只能在一个标签里打开一到三路视频。"));
        return -1;
    }
    if (referenceIndex < 0 || referenceIndex >= urls.size()) {
        emit openFailed(tr("参考源索引无效。"));
        return -1;
    }
    QList<QUrl> normalized;
    normalized.reserve(urls.size());
    for (const QVariant& value : urls) {
        normalized.push_back(asUrl(value));
    }
    QStringList identities;
    identities.reserve(normalized.size());
    for (const QUrl& url : normalized) {
        identities.push_back(identityForUrl(url));
    }
    const QString referenceIdentity = identities.at(referenceIndex);

    const int existing = indexOfMatchingTask(identities, referenceIdentity);
    if (existing >= 0) {
        Task& task = tasks_[static_cast<std::size_t>(existing)];
        if (task.loading || task.restoring) {
            // The activation waits for the open/restore completion (mirrors the image host).
            task.activateWhenLoaded = true;
        } else {
            // Focus keeps the record and its state snapshot; the recorded slot order wins.
            setActiveIndex(existing);
        }
        return existing;
    }
    if (count() >= kMaximumVideoTasks) {
        emit openFailed(tr("最多同时打开 %1 个视频标签，请先关闭一个。").arg(kMaximumVideoTasks));
        return -1;
    }

    Task task;
    task.urls = normalized;
    task.urlIdentities = identities;
    task.referenceIdentity = referenceIdentity;
    task.referenceIndex = referenceIndex;
    task.loading = true;
    tasks_.push_back(std::move(task));
    const int index = static_cast<int>(tasks_.size()) - 1;
    emit tabsChanged();

    QVariantList requested;
    requested.reserve(urls.size());
    for (const QUrl& url : normalized) {
        requested.push_back(QVariant::fromValue(url));
    }
    emit openRequested(index, requested, referenceIndex, purpose);
    return index;
}

bool VideoTaskHost::commitOpen(const int index, const QStringList& frozenIdentities) {
    if (index < 0 || index >= count()) {
        return false;
    }
    Task& task = tasks_[static_cast<std::size_t>(index)];
    task.frozenIdentities = frozenIdentities;
    if (!task.loading) {
        // An already committed record only refreshes its session facts (group evolution).
        return true;
    }
    task.loading = false;
    task.loaded = true;
    task.activateWhenLoaded = false;
    setActiveIndex(index);
    emit tabsChanged();
    emit taskOpened(index);
    return true;
}

bool VideoTaskHost::failOpen(const int index, const QString& error) {
    if (index < 0 || index >= count()) {
        return false;
    }
    const QString detail = error.isEmpty() ? tr("无法打开视频。") : error;
    if (tasks_[static_cast<std::size_t>(index)].loading) {
        // The open never succeeded: the new record goes away again (design §2.2 step 4).
        removeTask(index, true);
    }
    emit openFailed(detail);
    return true;
}

bool VideoTaskHost::appendSourceToActiveTask(const QUrl& url) {
    if (activeIndex_ < 0 || activeIndex_ >= count()) {
        return false;
    }
    Task& task = tasks_[static_cast<std::size_t>(activeIndex_)];
    if (task.loading || !task.loaded) {
        return false;
    }
    if (static_cast<int>(task.urls.size()) >= kMaximumSourcesPerTask) {
        return false;
    }
    task.urls.push_back(url);
    task.urlIdentities.push_back(identityForUrl(url));
    emit tabsChanged();
    return true;
}

bool VideoTaskHost::removeSourceFromActiveTask(const QString& identity) {
    if (activeIndex_ < 0 || activeIndex_ >= count()) {
        return false;
    }
    Task& task = tasks_[static_cast<std::size_t>(activeIndex_)];
    if (task.loading || !task.loaded || task.urls.size() < 2) {
        return false;
    }
    int slot = -1;
    for (qsizetype index = 0; index < task.urlIdentities.size(); ++index) {
        if (task.urlIdentities.at(index).compare(identity, Qt::CaseInsensitive) == 0) {
            slot = static_cast<int>(index);
            break;
        }
    }
    if (slot < 0) {
        for (qsizetype index = 0; index < task.frozenIdentities.size(); ++index) {
            if (task.frozenIdentities.at(index) == identity) {
                slot = static_cast<int>(index);
                break;
            }
        }
    }
    if (slot < 0 || slot >= static_cast<int>(task.urls.size())) {
        return false;
    }
    task.urls.removeAt(slot);
    task.urlIdentities.removeAt(slot);
    if (slot < static_cast<int>(task.frozenIdentities.size())) {
        task.frozenIdentities.removeAt(slot);
    }
    if (slot == task.referenceIndex) {
        task.referenceIndex = 0;
        task.referenceIdentity = task.urlIdentities.front();
    } else if (slot < task.referenceIndex) {
        --task.referenceIndex;
    }
    emit tabsChanged();
    return true;
}

bool VideoTaskHost::replaceActiveTaskGroup(const QVariantList& urls,
                                           const int referenceIndex,
                                           const bool resetState) {
    if (activeIndex_ < 0 || activeIndex_ >= count()) {
        return false;
    }
    Task& task = tasks_[static_cast<std::size_t>(activeIndex_)];
    if (task.loading || urls.isEmpty() || urls.size() > kMaximumSourcesPerTask) {
        return false;
    }
    if (referenceIndex < 0 || referenceIndex >= urls.size()) {
        return false;
    }
    QList<QUrl> normalized;
    normalized.reserve(urls.size());
    QStringList identities;
    identities.reserve(urls.size());
    for (const QVariant& value : urls) {
        const QUrl url = asUrl(value);
        normalized.push_back(url);
        identities.push_back(identityForUrl(url));
    }
    task.urls = std::move(normalized);
    task.urlIdentities = std::move(identities);
    task.referenceIndex = referenceIndex;
    task.referenceIdentity = task.urlIdentities.at(referenceIndex);
    if (resetState) {
        task.state = {};
    }
    emit tabsChanged();
    return true;
}

bool VideoTaskHost::setTaskReference(const int index, const int referenceIndex) {
    if (index < 0 || index >= count()) {
        return false;
    }
    Task& task = tasks_[static_cast<std::size_t>(index)];
    if (referenceIndex < 0 || referenceIndex >= task.urls.size()) {
        return false;
    }
    task.referenceIndex = referenceIndex;
    task.referenceIdentity = task.urlIdentities.at(referenceIndex);
    emit tabsChanged();
    return true;
}

int VideoTaskHost::indexOfTask(const QVariantList& urls, const int referenceIndex) const {
    if (urls.isEmpty() || referenceIndex < 0 || referenceIndex >= urls.size()) {
        return -1;
    }
    QStringList identities;
    identities.reserve(urls.size());
    for (const QVariant& value : urls) {
        identities.push_back(identityForUrl(asUrl(value)));
    }
    return indexOfMatchingTask(identities, identities.at(referenceIndex));
}

void VideoTaskHost::updateTaskState(const int index, const QVariantMap& state) {
    if (index < 0 || index >= count()) {
        return;
    }
    tasks_[static_cast<std::size_t>(index)].state = state;
}

QVariantMap VideoTaskHost::taskState(const int index) const {
    if (index < 0 || index >= count()) {
        return {};
    }
    return tasks_[static_cast<std::size_t>(index)].state;
}

bool VideoTaskHost::urlsMatch(const QVariantList& left, const QVariantList& right) const {
    if (left.size() != right.size()) {
        return false;
    }
    for (qsizetype index = 0; index < left.size(); ++index) {
        if (left.at(index).toString() != right.at(index).toString()) {
            return false;
        }
    }
    return true;
}

bool VideoTaskHost::setTaskRestoring(const int index, const bool restoring) {
    if (index < 0 || index >= count()) {
        return false;
    }
    Task& task = tasks_[static_cast<std::size_t>(index)];
    if (task.restoring == restoring) {
        return true;
    }
    task.restoring = restoring;
    emit tabsChanged();
    if (!restoring && task.activateWhenLoaded) {
        task.activateWhenLoaded = false;
        setActiveIndex(index);
    }
    return true;
}

bool VideoTaskHost::activate(const int index) {
    if (index < 0 || index >= count()) {
        return false;
    }
    if (tasks_[static_cast<std::size_t>(index)].loading) {
        tasks_[static_cast<std::size_t>(index)].activateWhenLoaded = true;
        return false;
    }
    setActiveIndex(index);
    return true;
}

bool VideoTaskHost::closeAt(const int index) {
    if (index < 0 || index >= count()) {
        return false;
    }
    if (tasks_[static_cast<std::size_t>(index)].loading) {
        return false;
    }
    removeTask(index, true);
    return true;
}

bool VideoTaskHost::closeActive() {
    if (activeIndex_ < 0 || activeIndex_ >= count()) {
        return false;
    }
    return closeAt(activeIndex_);
}

void VideoTaskHost::cycle(const int delta) {
    if (tasks_.size() < 2 || delta == 0) {
        return;
    }
    std::vector<int> candidates;
    candidates.reserve(tasks_.size());
    for (std::size_t index = 0; index < tasks_.size(); ++index) {
        if (!tasks_[index].loading && !tasks_[index].restoring) {
            candidates.push_back(static_cast<int>(index));
        }
    }
    if (candidates.size() < 2) {
        return;
    }
    if (activeIndex_ < 0) {
        setActiveIndex(candidates.front());
        return;
    }
    int position = 0;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (candidates[index] == activeIndex_) {
            position = static_cast<int>(index);
            break;
        }
    }
    const int total = static_cast<int>(candidates.size());
    const int next = ((position + delta) % total + total) % total;
    setActiveIndex(candidates[static_cast<std::size_t>(next)]);
}

int VideoTaskHost::indexOfMatchingTask(const QStringList& identities,
                                       const QString& referenceIdentity) const {
    for (std::size_t index = 0; index < tasks_.size(); ++index) {
        const Task& task = tasks_[index];
        if (!sameIdentityMultiset(task.urlIdentities, identities)) {
            continue;
        }
        if (task.referenceIdentity.compare(referenceIdentity, Qt::CaseInsensitive) == 0) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void VideoTaskHost::setActiveIndex(const int index) {
    if (index == activeIndex_) {
        return;
    }
    activeIndex_ = index;
    emit tabsChanged();
    emit activeChanged();
}

void VideoTaskHost::removeTask(const int index, const bool notify) {
    if (index < 0 || index >= count()) {
        return;
    }
    const bool wasActive = index == activeIndex_;
    const int previousActive = activeIndex_;
    tasks_.erase(tasks_.begin() + index);
    if (tasks_.empty()) {
        activeIndex_ = -1;
    } else if (wasActive) {
        // Prefer the right neighbour; the last tab leaves its left neighbour active.
        activeIndex_ = std::min(index, count() - 1);
    } else if (previousActive > index) {
        activeIndex_ = previousActive - 1;
    }
    if (previousActive != activeIndex_) {
        emit activeChanged();
    }
    if (notify) {
        emit tabsChanged();
    }
    if (notify && tasks_.empty()) {
        emit allTasksClosed();
    }
}

QString VideoTaskHost::titleFor(const Task& task) const {
    if (task.loading) {
        return tr("正在打开…");
    }
    if (task.restoring) {
        return tr("正在恢复…");
    }
    QStringList names;
    names.reserve(task.urlIdentities.size());
    for (const QString& identity : task.urlIdentities) {
        names.push_back(fileNameFor(identity));
    }
    if (names.isEmpty()) {
        return tr("视频标签");
    }
    if (names.size() == 1) {
        return names.front();
    }
    if (names.size() == 2) {
        if (names.at(0) == names.at(1)) {
            // Two materials can share a file name across folders; the bare pair then reads
            // as one file compared against itself. Name the parent folder on both sides so
            // the tab still identifies which two materials the task holds.
            const QString firstFolder = parentFolderNameFor(task.urlIdentities.at(0));
            const QString secondFolder = parentFolderNameFor(task.urlIdentities.at(1));
            if (!firstFolder.isEmpty() && firstFolder != secondFolder) {
                return QStringLiteral("%1 (%2)  ↔  %3 (%4)")
                    .arg(names.at(0), firstFolder, names.at(1), secondFolder);
            }
        }
        return QStringLiteral("%1  ↔  %2").arg(names.at(0), names.at(1));
    }
    return QStringLiteral("%1 + %2").arg(names.at(0)).arg(names.size() - 1);
}

QString VideoTaskHost::detailFor(const Task& task) const {
    return task.urlIdentities.join(QLatin1Char('\n'));
}

} // namespace dvs::ui
