#pragma once

#include <QAbstractListModel>
#include <QUrl>

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace dvs::ui {

// Pairing model for the folder-comparison sidebar. Two selected folders are scanned for
// still images (top level only); files sharing a case-insensitive name form a pair, and
// files present on only one side are listed as singles so the user sees what is missing.
// Only file names are enumerated — images decode when a pair is actually opened, keeping
// hundred-file folders cheap.
class ImageFolderPairModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString leftFolderName READ leftFolderName NOTIFY foldersChanged)
    Q_PROPERTY(QString rightFolderName READ rightFolderName NOTIFY foldersChanged)
    Q_PROPERTY(QString leftFolderPath READ leftFolderPath NOTIFY foldersChanged)
    Q_PROPERTY(QString rightFolderPath READ rightFolderPath NOTIFY foldersChanged)
    Q_PROPERTY(int pairCount READ pairCount NOTIFY foldersChanged)
    Q_PROPERTY(int completeCount READ completeCount NOTIFY foldersChanged)
    Q_PROPERTY(int missingCount READ missingCount NOTIFY foldersChanged)
    Q_PROPERTY(int conflictCount READ conflictCount NOTIFY foldersChanged)
    Q_PROPERTY(int currentPair READ currentPair WRITE setCurrentPair NOTIFY currentPairChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY foldersChanged)
    Q_PROPERTY(bool openPending READ openPending NOTIFY pendingPairChanged)
    Q_PROPERTY(int pendingPair READ pendingPair NOTIFY pendingPairChanged)

public:
    // Synchronous opener used by tests and non-UI embedding. Injected by the composition
    // root so this model never depends on the controller type.
    using PairOpener =
        std::function<QString(const QUrl& primary, const QUrl& secondary, int pairId)>;
    // T4 asynchronous opener: returns a positive request id or <= 0 with an immediate
    // error in *error. Selection must only advance when completePairOpen reports success.
    using AsyncPairOpener =
        std::function<int(const QUrl& primary, const QUrl& secondary, int pairId, QString* error)>;
    // T6 single-side viewer: opens the one existing side of a single-sided row so a
    // missing file can be inspected without a file explorer. Same request/terminal
    // protocol as the pair opener: the selection only advances on a matching success.
    using SingleSideOpener = std::function<int(const QUrl& url, int row, QString* error)>;
    using AsyncPairCancel = std::function<void(int requestId)>;

    struct PairRow final {
        QString fileName;
        QUrl leftUrl;
        QUrl rightUrl;
        bool hasLeft = false;
        bool hasRight = false;
        // T6: one side holds several names that fold to the same key (e.g. Shot.PNG and
        // shot.png). The pairing then has more than one candidate on that side, so the
        // row is listed explicitly with the conflict instead of silently picking one.
        bool caseConflict = false;
        QString caseConflictDetail;
    };

    // One folder side scanned for still images. Names are matched case-insensitively, so
    // the scan also records every distinct name sharing a folded key (T6).
    struct FolderSideScan final {
        // Folded key -> chosen file URL; the first name in scan order wins.
        std::map<QString, QUrl> entries;
        // Folded key -> every distinct file name sharing it, in scan order.
        std::map<QString, QStringList> names;
    };

    enum Role {
        FileNameRole = Qt::UserRole + 1,
        LeftPathRole,
        RightPathRole,
        HasLeftRole,
        HasRightRole,
        HasBothRole,
        RowIndexRole,
        CaseConflictRole,
        CaseConflictDetailRole,
    };
    Q_ENUM(Role)

    explicit ImageFolderPairModel(QObject* parent = nullptr);

    void setPairOpener(PairOpener opener);
    void setAsyncPairOpener(AsyncPairOpener opener);
    void setAsyncPairCancel(AsyncPairCancel cancel);
    void setSingleSideOpener(SingleSideOpener opener);

    [[nodiscard]] QString leftFolderName() const noexcept;
    [[nodiscard]] QString rightFolderName() const noexcept;
    [[nodiscard]] QString leftFolderPath() const noexcept;
    [[nodiscard]] QString rightFolderPath() const noexcept;
    [[nodiscard]] int pairCount() const noexcept;
    [[nodiscard]] int completeCount() const noexcept;
    [[nodiscard]] int missingCount() const noexcept;
    [[nodiscard]] int conflictCount() const noexcept;
    [[nodiscard]] int currentPair() const noexcept;
    void setCurrentPair(int row);
    [[nodiscard]] QString errorText() const noexcept;
    [[nodiscard]] bool openPending() const noexcept;
    [[nodiscard]] int pendingPair() const noexcept;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE bool loadFolders(const QUrl& left, const QUrl& right);
    Q_INVOKABLE void clear();
    Q_INVOKABLE bool openPairAt(int row);
    Q_INVOKABLE void cancelPendingOpen();
    // Opens the single existing side of a one-sided row (T6 missing-file inspection).
    Q_INVOKABLE bool openSingleSideAt(int row);
    // Terminal callback from the single-side opener owner; stale ids are ignored.
    void completeSingleSideOpen(quint64 requestId, bool success, QString error);
    // Row identity map for QML (prefetch wiring and conflict tooltips).
    Q_INVOKABLE QVariantMap pairUrlsAt(int row) const;
    // Terminal callback from the async opener owner. Stale ids are ignored so an older
    // N cannot advance the selection after N+1/N+2 was requested.
    void completePairOpen(quint64 requestId, bool success, QString error);
    Q_INVOKABLE int firstCompleteRow() const noexcept;
    // Steps to the next (delta > 0) or previous (delta < 0) two-sided row, wrapping
    // around; returns the opened row or -1 when no complete pair exists.
    Q_INVOKABLE int stepCompleteRow(int delta) const;
    // Pure merge of two side scans into the sorted row list. Static and filesystem-free
    // so the pairing, ordering and case-conflict semantics stay testable everywhere.
    [[nodiscard]] static std::vector<PairRow> mergePairRows(const FolderSideScan& left,
                                                            const FolderSideScan& right);

signals:
    void foldersChanged();
    void currentPairChanged();
    void pendingPairChanged();
    void pairOpenFinished(int row, bool success, QString error);

private:
    [[nodiscard]] static bool isStillImageName(const QString& fileName);

    PairOpener opener_;
    AsyncPairOpener asyncOpener_;
    AsyncPairCancel asyncCancel_;
    SingleSideOpener singleSideOpener_;
    std::vector<PairRow> rows_;
    QString leftFolderPath_;
    QString rightFolderPath_;
    QString errorText_;
    int currentPair_ = -1;
    quint64 pendingRequestId_ = 0;
    int pendingRow_ = -1;
    quint64 pendingSingleRequestId_ = 0;
    int pendingSingleRow_ = -1;
};

} // namespace dvs::ui
