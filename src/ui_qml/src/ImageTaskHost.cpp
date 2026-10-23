#include "dvs/ui/ImageTaskHost.h"

#include "dvs/ui/ImagePairLoader.h"
#include "dvs/ui/ImageReviewController.h"

#include <QFileInfo>
#include <QSize>

#include <algorithm>
#include <utility>

namespace dvs::ui {
namespace {

constexpr int kMaximumImageTasks = 12;
constexpr qint64 kMiB = 1024LL * 1024LL;

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

[[nodiscard]] bool sameIdentityList(const QStringList& first, const QStringList& second) {
    if (first.size() != second.size()) {
        return false;
    }
    for (qsizetype index = 0; index < first.size(); ++index) {
        if (first.at(index).compare(second.at(index), Qt::CaseInsensitive) != 0) {
            return false;
        }
    }
    return true;
}

} // namespace

ImageTaskHost::ImageTaskHost(ImageReviewController* initialController, QObject* parent)
    : ImageTaskHost(initialController, kImagePairWorkingSetBudgetBytes, parent) {}

ImageTaskHost::ImageTaskHost(ImageReviewController* initialController,
                             const qint64 workingSetBudgetBytes,
                             QObject* parent)
    : QObject(parent), initialController_(initialController),
      workingSetBudgetBytes_(workingSetBudgetBytes > 0 ? workingSetBudgetBytes
                                                       : kImagePairWorkingSetBudgetBytes) {}

ImageTaskHost::~ImageTaskHost() = default;

int ImageTaskHost::count() const noexcept {
    return static_cast<int>(tasks_.size());
}

int ImageTaskHost::activeIndex() const noexcept {
    return activeIndex_;
}

ImageReviewController* ImageTaskHost::activeController() const noexcept {
    if (activeIndex_ < 0 || activeIndex_ >= static_cast<int>(tasks_.size())) {
        return nullptr;
    }
    return tasks_[static_cast<std::size_t>(activeIndex_)].controller;
}

QVariantList ImageTaskHost::tabs() const {
    QVariantList result;
    result.reserve(static_cast<qsizetype>(tasks_.size()));
    for (std::size_t index = 0; index < tasks_.size(); ++index) {
        const Task& task = tasks_[index];
        QVariantMap entry;
        entry.insert(QStringLiteral("index"), static_cast<int>(index));
        entry.insert(QStringLiteral("title"), titleFor(task));
        entry.insert(QStringLiteral("detail"), detailFor(task));
        entry.insert(QStringLiteral("loading"), task.loading);
        entry.insert(QStringLiteral("active"), static_cast<int>(index) == activeIndex_);
        entry.insert(QStringLiteral("primaryPath"), task.primaryPath);
        entry.insert(QStringLiteral("secondaryPath"), task.secondaryPath);
        entry.insert(QStringLiteral("pair"), !task.secondaryPath.isEmpty());
        result.push_back(entry);
    }
    return result;
}

qint64 ImageTaskHost::workingSetBytes() const noexcept {
    qint64 total = 0;
    for (const Task& task : tasks_) {
        if (task.controller == nullptr) {
            continue;
        }
        const QSize primary{task.controller->primaryWidth(), task.controller->primaryHeight()};
        if (primary.isEmpty()) {
            continue;
        }
        const QSize secondary{task.controller->secondaryWidth(),
                              task.controller->secondaryHeight()};
        const bool nativeSidecars =
            task.controller->primaryBitDepth() > 8 || task.controller->secondaryBitDepth() > 8;
        if (!secondary.isEmpty()) {
            total += estimatePairWorkingSet(
                         primary, secondary, nativeSidecars, task.controller->diffResampled())
                         .totalBytes;
        } else {
            total +=
                estimateSingleImageWorkingSet(primary.width(), primary.height(), nativeSidecars)
                    .totalBytes;
        }
    }
    return total;
}

ImageReviewController* ImageTaskHost::controllerForToken(const QString& token) const {
    if (token.isEmpty()) {
        return nullptr;
    }
    for (const Task& task : tasks_) {
        if (task.token == token) {
            return task.controller;
        }
    }
    return nullptr;
}

ImageReviewController* ImageTaskHost::ensureActiveController() {
    if (tasks_.empty()) {
        Task task;
        if (!initialInUse_ && initialController_ != nullptr) {
            task.controller = initialController_;
            initialInUse_ = true;
        } else {
            auto controller = std::make_unique<ImageReviewController>();
            task.controller = controller.get();
            task.ownedController = std::move(controller);
        }
        task.token = QStringLiteral("w%1").arg(++nextToken_);
        task.controller->setImageToken(task.token);
        attachController(task.controller);
        task.title = tr("图片标签");
        task.autoCreatedForFolder = true;
        tasks_.push_back(std::move(task));
        emit tabsChanged();
        setActiveIndex(0);
        return tasks_.front().controller;
    }
    if (activeIndex_ < 0) {
        setActiveIndex(0);
    }
    return tasks_[static_cast<std::size_t>(activeIndex_)].controller;
}

int ImageTaskHost::requestOpenPairInActiveTask(const QUrl& primary,
                                               const QUrl& secondary,
                                               const int pairId,
                                               QString* error) {
    ImageReviewController* const controller = ensureActiveController();
    if (controller == nullptr) {
        if (error != nullptr) {
            *error = tr("没有可用的图片标签。");
        }
        return -1;
    }
    if (controller->openPending()) {
        if (error != nullptr) {
            *error = tr("正在打开另一张图片，请稍候。");
        }
        return -1;
    }
    const int requestId = controller->requestOpenPair(primary, secondary, pairId);
    if (requestId <= 0) {
        if (error != nullptr) {
            const QString detail = controller->errorText();
            *error = detail.isEmpty() ? tr("无法打开图片对。") : detail;
        }
        return -1;
    }
    const int index = indexOfController(controller);
    if (index >= 0) {
        Task& task = tasks_[static_cast<std::size_t>(index)];
        task.folderRequestId = requestId;
        task.loading = true;
        refreshTaskMetadata(index);
    }
    return requestId;
}

int ImageTaskHost::requestOpenPrimaryInActiveTask(const QUrl& url,
                                                  const int pairId,
                                                  QString* error) {
    ImageReviewController* const controller = ensureActiveController();
    if (controller == nullptr) {
        if (error != nullptr) {
            *error = tr("没有可用的图片标签。");
        }
        return -1;
    }
    if (controller->openPending()) {
        if (error != nullptr) {
            *error = tr("正在打开另一张图片，请稍候。");
        }
        return -1;
    }
    const int requestId = controller->requestOpenPrimary(url, pairId);
    if (requestId <= 0) {
        if (error != nullptr) {
            const QString detail = controller->errorText();
            *error = detail.isEmpty() ? tr("无法打开图片。") : detail;
        }
        return -1;
    }
    const int index = indexOfController(controller);
    if (index >= 0) {
        Task& task = tasks_[static_cast<std::size_t>(index)];
        task.folderRequestId = requestId;
        task.loading = true;
        refreshTaskMetadata(index);
    }
    return requestId;
}

void ImageTaskHost::cancelOpenRequest(const int requestId) {
    for (int index = static_cast<int>(tasks_.size()) - 1; index >= 0; --index) {
        Task& task = tasks_[static_cast<std::size_t>(index)];
        if (task.pendingRequestId != requestId && task.folderRequestId != requestId) {
            continue;
        }
        task.controller->cancelOpenRequest(requestId);
        task.pendingRequestId = 0;
        task.folderRequestId = 0;
        task.loading = false;
        if (task.autoCreatedForFolder && !task.controller->hasPrimary() &&
            !task.controller->hasSecondary()) {
            removeTask(index, true);
        } else {
            refreshTaskMetadata(index);
        }
    }
}

int ImageTaskHost::openImages(const QVariantList& urls) {
    if (urls.isEmpty() || urls.size() > 2) {
        emit openFailed(tr("一次只能在一个标签里打开一张或两张图片。"));
        return -1;
    }
    QStringList identities;
    identities.reserve(urls.size());
    for (const QVariant& value : urls) {
        identities.push_back(identityForUrl(asUrl(value)));
    }
    for (std::size_t index = 0; index < tasks_.size(); ++index) {
        if (!sameIdentityList(tasks_[index].urlIdentities, identities)) {
            continue;
        }
        if (tasks_[index].loading) {
            tasks_[index].activateWhenLoaded = true;
            return static_cast<int>(index);
        }
        activate(static_cast<int>(index));
        return static_cast<int>(index);
    }
    if (tasks_.size() >= static_cast<std::size_t>(kMaximumImageTasks)) {
        emit openFailed(tr("最多同时打开 %1 个图片标签，请先关闭一个。").arg(kMaximumImageTasks));
        return -1;
    }

    Task task;
    if (!initialInUse_ && initialController_ != nullptr) {
        task.controller = initialController_;
        initialInUse_ = true;
    } else {
        auto controller = std::make_unique<ImageReviewController>();
        task.controller = controller.get();
        task.ownedController = std::move(controller);
    }
    task.token = QStringLiteral("w%1").arg(++nextToken_);
    task.controller->setImageToken(task.token);
    task.urlIdentities = identities;
    task.loading = true;
    task.title = tr("正在打开…");
    attachController(task.controller);
    tasks_.push_back(std::move(task));
    const int index = static_cast<int>(tasks_.size()) - 1;
    emit tabsChanged();

    const QUrl primary = asUrl(urls.at(0));
    const int requestId =
        urls.size() == 1
            ? tasks_[static_cast<std::size_t>(index)].controller->requestOpenPrimary(primary)
            : tasks_[static_cast<std::size_t>(index)].controller->requestOpenPair(
                  primary, asUrl(urls.at(1)));
    if (requestId <= 0) {
        const QString detail = tasks_[static_cast<std::size_t>(index)].controller->errorText();
        removeTask(index, true);
        emit openFailed(detail.isEmpty() ? tr("无法打开图片。") : detail);
        return -1;
    }
    tasks_[static_cast<std::size_t>(index)].pendingRequestId = requestId;
    return index;
}

bool ImageTaskHost::activate(const int index) {
    if (index < 0 || index >= static_cast<int>(tasks_.size())) {
        return false;
    }
    if (tasks_[static_cast<std::size_t>(index)].loading) {
        tasks_[static_cast<std::size_t>(index)].activateWhenLoaded = true;
        return false;
    }
    setActiveIndex(index);
    return true;
}

bool ImageTaskHost::closeAt(const int index) {
    if (index < 0 || index >= static_cast<int>(tasks_.size())) {
        return false;
    }
    removeTask(index, true);
    return true;
}

bool ImageTaskHost::closeActive() {
    if (activeIndex_ < 0 || activeIndex_ >= static_cast<int>(tasks_.size())) {
        return false;
    }
    return closeAt(activeIndex_);
}

void ImageTaskHost::cycle(const int delta) {
    if (tasks_.size() < 2 || delta == 0) {
        return;
    }
    std::vector<int> candidates;
    candidates.reserve(tasks_.size());
    for (std::size_t index = 0; index < tasks_.size(); ++index) {
        if (!tasks_[index].loading) {
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
    const int count = static_cast<int>(candidates.size());
    const int next = ((position + delta) % count + count) % count;
    setActiveIndex(candidates[static_cast<std::size_t>(next)]);
}

int ImageTaskHost::indexOfController(const ImageReviewController* controller) const {
    for (std::size_t index = 0; index < tasks_.size(); ++index) {
        if (tasks_[index].controller == controller) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void ImageTaskHost::attachController(ImageReviewController* const controller) {
    if (controller == nullptr) {
        return;
    }
    // Reused controllers (the composition root's initial instance) must not accumulate a
    // second connection every time they become a task again.
    QObject::disconnect(controller, nullptr, this, nullptr);
    QObject::connect(
        controller,
        &ImageReviewController::openFinished,
        this,
        [this, controller](
            const int requestId, const int pairId, const bool success, const QString& error) {
            handleOpenFinished(controller, requestId, pairId, success, error);
        });
    QObject::connect(controller, &ImageReviewController::stateChanged, this, [this, controller] {
        refreshTaskMetadataFor(controller);
    });
}

void ImageTaskHost::handleOpenFinished(ImageReviewController* const controller,
                                       const int requestId,
                                       const int pairId,
                                       const bool success,
                                       const QString& error) {
    emit controllerOpenFinished(requestId, pairId, success, error);
    const int index = indexOfController(controller);
    if (index < 0) {
        return;
    }
    Task& task = tasks_[static_cast<std::size_t>(index)];
    if (task.pendingRequestId == requestId) {
        task.pendingRequestId = 0;
        task.loading = false;
        if (!success) {
            const QString detail = error.isEmpty() ? tr("无法打开图片。") : error;
            removeTask(index, true);
            emit openFailed(detail);
            return;
        }
        task.loaded = controller->hasPrimary() || controller->hasSecondary();
        refreshTaskMetadata(index);
        const int refreshedIndex = indexOfController(controller);
        if (refreshedIndex < 0) {
            return;
        }
        if (tasks_[static_cast<std::size_t>(refreshedIndex)].loaded && !withinBudget()) {
            const QString detail = tr("图片标签工作集已超过 %1 MiB，请关闭一个标签后再打开。")
                                       .arg(workingSetBudgetBytes_ / kMiB);
            removeTask(refreshedIndex, true);
            emit openFailed(detail);
            return;
        }
        if (!tasks_[static_cast<std::size_t>(refreshedIndex)].loaded) {
            removeTask(refreshedIndex, true);
            emit openFailed(tr("图片没有可显示的像素。"));
            return;
        }
        tasks_[static_cast<std::size_t>(refreshedIndex)].activateWhenLoaded = false;
        setActiveIndex(refreshedIndex);
        emit taskOpened(refreshedIndex);
        return;
    }
    if (task.folderRequestId == requestId) {
        task.folderRequestId = 0;
        task.loading = false;
        if (!success) {
            if (task.autoCreatedForFolder && !controller->hasPrimary() &&
                !controller->hasSecondary()) {
                removeTask(index, true);
            } else {
                refreshTaskMetadata(index);
            }
            return;
        }
        task.loaded = controller->hasPrimary() || controller->hasSecondary();
        task.autoCreatedForFolder = false;
        refreshTaskMetadata(index);
        return;
    }
    refreshTaskMetadata(index);
}

void ImageTaskHost::refreshTaskMetadataFor(const ImageReviewController* const controller) {
    const int index = indexOfController(controller);
    if (index >= 0) {
        refreshTaskMetadata(index);
    }
}

void ImageTaskHost::refreshTaskMetadata(const int index) {
    if (index < 0 || index >= static_cast<int>(tasks_.size())) {
        return;
    }
    Task& task = tasks_[static_cast<std::size_t>(index)];
    if (task.controller == nullptr) {
        return;
    }
    if (task.loading && !task.controller->hasPrimary() && !task.controller->hasSecondary()) {
        const QString title = tr("正在打开…");
        if (task.title != title) {
            task.title = title;
            task.detail.clear();
            emit tabsChanged();
        }
        return;
    }
    const QString primaryPath = task.controller->primaryPath();
    const QString secondaryPath = task.controller->secondaryPath();
    QStringList identities;
    if (!primaryPath.isEmpty()) {
        identities.push_back(QFileInfo{primaryPath}.absoluteFilePath());
        if (!secondaryPath.isEmpty()) {
            identities.push_back(QFileInfo{secondaryPath}.absoluteFilePath());
        }
    }
    const bool changed = task.primaryPath != primaryPath || task.secondaryPath != secondaryPath ||
                         !sameIdentityList(task.urlIdentities, identities);
    task.primaryPath = primaryPath;
    task.secondaryPath = secondaryPath;
    if (!identities.isEmpty()) {
        task.urlIdentities = identities;
    }
    task.title = titleFor(task);
    task.detail = detailFor(task);
    if (changed) {
        emit tabsChanged();
    }
}

QString ImageTaskHost::titleFor(const Task& task) const {
    QString primary = fileNameFor(task.primaryPath);
    QString secondary = fileNameFor(task.secondaryPath);
    if (task.loading && primary.isEmpty()) {
        return tr("正在打开…");
    }
    if (primary.isEmpty() && secondary.isEmpty()) {
        return tr("图片标签");
    }
    if (secondary.isEmpty()) {
        return primary;
    }
    if (primary.isEmpty()) {
        return secondary;
    }
    return QStringLiteral("%1  ↔  %2").arg(primary, secondary);
}

QString ImageTaskHost::detailFor(const Task& task) const {
    QStringList paths;
    if (!task.primaryPath.isEmpty()) {
        paths.push_back(task.primaryPath);
    }
    if (!task.secondaryPath.isEmpty()) {
        paths.push_back(task.secondaryPath);
    }
    return paths.join(QLatin1Char('\n'));
}

void ImageTaskHost::setActiveIndex(const int index) {
    if (index == activeIndex_) {
        return;
    }
    activeIndex_ = index;
    emit tabsChanged();
    emit activeChanged();
}

void ImageTaskHost::removeTask(const int index, const bool notify) {
    if (index < 0 || index >= static_cast<int>(tasks_.size())) {
        return;
    }
    const bool wasActive = index == activeIndex_;
    const int previousActive = activeIndex_;
    Task& task = tasks_[static_cast<std::size_t>(index)];
    if (task.pendingRequestId > 0) {
        task.controller->cancelOpenRequest(task.pendingRequestId);
    }
    if (task.folderRequestId > 0) {
        task.controller->cancelOpenRequest(task.folderRequestId);
    }
    ImageReviewController* const initial =
        task.ownedController == nullptr ? task.controller : nullptr;
    std::unique_ptr<ImageReviewController> owned = std::move(task.ownedController);
    tasks_.erase(tasks_.begin() + index);
    if (initial != nullptr) {
        initial->closeAll();
        initial->setImageToken(QString{});
        if (initial == initialController_) {
            initialInUse_ = false;
        }
    }
    // owned destroys its controller (and its worker pool) when the local unique_ptr leaves.
    if (tasks_.empty()) {
        activeIndex_ = -1;
    } else if (wasActive) {
        activeIndex_ = std::min(index, static_cast<int>(tasks_.size()) - 1);
    } else if (previousActive > index) {
        activeIndex_ = previousActive - 1;
    }
    if (previousActive != activeIndex_) {
        // Rebind the window before any strip model update can run: the closed task may have
        // owned the controller the workspace still pointed at.
        emit activeChanged();
    }
    if (notify) {
        emit tabsChanged();
    }
    if (notify && tasks_.empty()) {
        emit allTasksClosed();
    }
}

bool ImageTaskHost::withinBudget() const {
    if (tasks_.size() <= 1) {
        // One task keeps the same ceiling a single pair had before tabs existed.
        return true;
    }
    return workingSetBytes() <= workingSetBudgetBytes_;
}

} // namespace dvs::ui
