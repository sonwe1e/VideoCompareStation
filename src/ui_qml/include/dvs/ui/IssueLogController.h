#pragma once

#include "dvs/application/IssueRecord.h"

#include <QUrl>
#include <QVariantMap>

#include <vector>

namespace dvs::ui {

class ImageFolderPairModel;
class ReviewController;
class ReviewPreferencesController;

// T7 read-only issue log: manual capture, manual save/load, and identity-gated restore.
// Restore never opens a moved/modified file as if it were the recorded revision.
class IssueLogController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY issuesChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY statusChanged)
    Q_PROPERTY(QString documentPath READ documentPath NOTIFY issuesChanged)

public:
    explicit IssueLogController(application::IIssueRecordRepository* repository,
                                QObject* parent = nullptr);
    ~IssueLogController() override;

    void setReviewController(ReviewController* controller) noexcept;
    void setPreferences(ReviewPreferencesController* preferences) noexcept;
    void setFolderModel(ImageFolderPairModel* model) noexcept;
    // ImageReviewController / ComparisonSurface live in the graphics adapter.
    void setImageController(QObject* imageController) noexcept;
    void setVideoSurface(QObject* videoSurface) noexcept;
    void setWorkspaceMode(const QString& mode) noexcept;
    void setDefaultDocumentPath(const QUrl& path) noexcept;

    [[nodiscard]] int count() const noexcept;
    [[nodiscard]] QString statusText() const noexcept;
    [[nodiscard]] QString lastError() const noexcept;
    [[nodiscard]] QString documentPath() const noexcept;

    Q_INVOKABLE bool captureCurrentIssue(const QString& note);
    Q_INVOKABLE bool saveDefault();
    Q_INVOKABLE bool saveIssues(const QUrl& fileUrl);
    Q_INVOKABLE bool loadDefault();
    Q_INVOKABLE bool loadIssues(const QUrl& fileUrl);
    Q_INVOKABLE QVariantMap restoreIssue(int index);
    Q_INVOKABLE QVariantMap issueAt(int index) const;
    Q_INVOKABLE void clearIssues();

Q_SIGNALS:
    void issuesChanged();
    void statusChanged();
    // Ready restore: workspace should switch to this media/view context.
    void restoreRequested(const QVariantMap payload);

private:
    struct StoredIssue final {
        application::IssueRecord record;
        QString summary;
    };

    void setStatus(QString text, QString error = {});
    [[nodiscard]] application::ObservedSourceIdentity observePath(const std::string& path) const;
    [[nodiscard]] bool captureVideoIssue(StoredIssue& issue, const QString& note);
    [[nodiscard]] bool captureImageIssue(StoredIssue& issue, const QString& note);
    [[nodiscard]] QVariantMap performRestore(const StoredIssue& issue);
    [[nodiscard]] static QString summarize(const application::IssueRecord& record);

    application::IIssueRecordRepository* repository_ = nullptr;
    ReviewController* review_ = nullptr;
    ReviewPreferencesController* preferences_ = nullptr;
    ImageFolderPairModel* folderModel_ = nullptr;
    QObject* imageController_ = nullptr;
    QObject* videoSurface_ = nullptr;
    QString workspaceMode_ = QStringLiteral("video");
    QString documentPath_;
    std::vector<StoredIssue> issues_;
    QString statusText_;
    QString lastError_;
};

} // namespace dvs::ui
