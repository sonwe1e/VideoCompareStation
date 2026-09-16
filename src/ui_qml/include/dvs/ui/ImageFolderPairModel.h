#pragma once

#include <QAbstractListModel>
#include <QUrl>

#include <functional>
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
    Q_PROPERTY(int currentPair READ currentPair WRITE setCurrentPair NOTIFY currentPairChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY foldersChanged)

public:
    // Opens a pair's images in the still-image review controller. Injected by the
    // composition root so this model never depends on the controller type. Returns an
    // empty string on success or the failure reason; the model only advances currentPair
    // when the opener reports success, so list selection and canvas identity stay locked.
    using PairOpener =
        std::function<QString(const QUrl& primary, const QUrl& secondary, int pairId)>;

    struct PairRow final {
        QString fileName;
        QUrl leftUrl;
        QUrl rightUrl;
        bool hasLeft = false;
        bool hasRight = false;
    };

    enum Role {
        FileNameRole = Qt::UserRole + 1,
        LeftPathRole,
        RightPathRole,
        HasLeftRole,
        HasRightRole,
        HasBothRole,
        RowIndexRole,
    };
    Q_ENUM(Role)

    explicit ImageFolderPairModel(QObject* parent = nullptr);

    void setPairOpener(PairOpener opener);

    [[nodiscard]] QString leftFolderName() const noexcept;
    [[nodiscard]] QString rightFolderName() const noexcept;
    [[nodiscard]] QString leftFolderPath() const noexcept;
    [[nodiscard]] QString rightFolderPath() const noexcept;
    [[nodiscard]] int pairCount() const noexcept;
    [[nodiscard]] int currentPair() const noexcept;
    void setCurrentPair(int row);
    [[nodiscard]] QString errorText() const noexcept;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE bool loadFolders(const QUrl& left, const QUrl& right);
    Q_INVOKABLE bool openPairAt(int row);
    Q_INVOKABLE int firstCompleteRow() const noexcept;
    // Steps to the next (delta > 0) or previous (delta < 0) two-sided row, wrapping
    // around; returns the opened row or -1 when no complete pair exists.
    Q_INVOKABLE int stepCompleteRow(int delta) const;

signals:
    void foldersChanged();
    void currentPairChanged();

private:
    [[nodiscard]] static bool isStillImageName(const QString& fileName);

    PairOpener opener_;
    std::vector<PairRow> rows_;
    QString leftFolderPath_;
    QString rightFolderPath_;
    QString errorText_;
    int currentPair_ = -1;
};

} // namespace dvs::ui
