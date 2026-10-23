#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>

#include <cstdint>
#include <memory>
#include <vector>

namespace dvs::ui {

class ImageReviewController;

// Owns the still-image review tasks shown as tabs in one window. Each task is an independent
// ImageReviewController, so switching tabs keeps the pair, decode buffers, difference cache and
// observation state of every open task. The host is the single owner of task lifetime, the
// active-task index, and the token that keeps Qt's image cache from serving one tab's pixels to
// another. It never reads or writes image pixels itself.
class ImageTaskHost final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY tabsChanged)
    Q_PROPERTY(int activeIndex READ activeIndex NOTIFY activeChanged)
    Q_PROPERTY(ImageReviewController* activeController READ activeController NOTIFY activeChanged)
    Q_PROPERTY(QVariantList tabs READ tabs NOTIFY tabsChanged)

public:
    explicit ImageTaskHost(ImageReviewController* initialController, QObject* parent = nullptr);
    ImageTaskHost(ImageReviewController* initialController,
                  qint64 workingSetBudgetBytes,
                  QObject* parent = nullptr);
    ~ImageTaskHost() override;

    ImageTaskHost(const ImageTaskHost&) = delete;
    ImageTaskHost& operator=(const ImageTaskHost&) = delete;
    ImageTaskHost(ImageTaskHost&&) = delete;
    ImageTaskHost& operator=(ImageTaskHost&&) = delete;

    [[nodiscard]] int count() const noexcept;
    [[nodiscard]] int activeIndex() const noexcept;
    [[nodiscard]] ImageReviewController* activeController() const noexcept;
    [[nodiscard]] QVariantList tabs() const;
    [[nodiscard]] qint64 workingSetBytes() const noexcept;

    // Provider routing: a task's token maps to its live controller. An empty or unknown token
    // yields nullptr; ReviewImageProvider must not fall back to another task's pixels.
    [[nodiscard]] ImageReviewController* controllerForToken(const QString& token) const;

    // Used by the folder-model adapters: open into the active task, creating an empty one only
    // when no image task exists. The auto-created task is removed again if the open fails
    // before it ever carried content.
    [[nodiscard]] ImageReviewController* ensureActiveController();
    [[nodiscard]] int requestOpenPairInActiveTask(const QUrl& primary,
                                                  const QUrl& secondary,
                                                  int pairId,
                                                  QString* error);
    [[nodiscard]] int requestOpenPrimaryInActiveTask(const QUrl& url, int pairId, QString* error);
    void cancelOpenRequest(int requestId);

    // QML entry points. openImages is the "open in a new tab" path used by the file dialog,
    // drops and external opens; duplicate material activates the existing tab instead of
    // loading it twice.
    Q_INVOKABLE int openImages(const QVariantList& urls);
    Q_INVOKABLE bool activate(int index);
    Q_INVOKABLE bool closeAt(int index);
    Q_INVOKABLE bool closeActive();
    Q_INVOKABLE void cycle(int delta);

signals:
    void tabsChanged();
    void activeChanged();
    // A new tab finished loading and became active. The host has already committed the tab
    // state; the window still has to adopt the workspace, remember the paths and end staging.
    void taskOpened(int index);
    void openFailed(const QString& error);
    void allTasksClosed();
    // Forwarded for every task controller so the folder session still receives the terminal
    // of an in-place folder open.
    void controllerOpenFinished(int requestId, int pairId, bool success, const QString& error);

private:
    struct Task {
        ImageReviewController* controller = nullptr;
        std::unique_ptr<ImageReviewController> ownedController;
        QString token;
        QStringList urlIdentities;
        QString title;
        QString detail;
        QString primaryPath;
        QString secondaryPath;
        bool loading = false;
        bool loaded = false;
        bool autoCreatedForFolder = false;
        bool activateWhenLoaded = false;
        int pendingRequestId = 0;
        int folderRequestId = 0;
    };

    [[nodiscard]] int indexOfController(const ImageReviewController* controller) const;
    void attachController(ImageReviewController* controller);
    void handleOpenFinished(ImageReviewController* controller,
                            int requestId,
                            int pairId,
                            bool success,
                            const QString& error);
    void refreshTaskMetadata(int index);
    void refreshTaskMetadataFor(const ImageReviewController* controller);
    void setActiveIndex(int index);
    void removeTask(int index, bool notify);
    [[nodiscard]] bool withinBudget() const;
    [[nodiscard]] QString titleFor(const Task& task) const;
    [[nodiscard]] QString detailFor(const Task& task) const;

    ImageReviewController* initialController_ = nullptr;
    bool initialInUse_ = false;
    std::vector<Task> tasks_;
    int activeIndex_ = -1;
    std::uint64_t nextToken_ = 0;
    qint64 workingSetBudgetBytes_ = 0;
};

} // namespace dvs::ui
