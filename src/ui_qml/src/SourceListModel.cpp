#include "dvs/ui/SourceListModel.h"

#include <QByteArray>
#include <QHash>
#include <QModelIndex>
#include <QVariant>

#include <algorithm>
#include <utility>

namespace dvs::ui {

SourceListModel::SourceListModel(QObject* const parent) : QAbstractListModel(parent) {}

int SourceListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant SourceListModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 ||
        static_cast<std::size_t>(index.row()) >= rows_.size()) {
        return {};
    }
    const SourceListRow& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case SourceIdRole:
        return QVariant::fromValue<qulonglong>(row.sourceId);
    case SourceIdentityRole:
        return row.sourceIdentity;
    case RoleRole:
        return row.role;
    case FilenameRole:
        return row.filename;
    case ParentLabelRole:
        return row.parentLabel;
    case FullPathRole:
        return row.fullPath;
    case ErrorRole:
        return row.errorKey;
    case CurrentSourceFrameRole:
        return row.currentSourceFrame.has_value() ? QVariant{*row.currentSourceFrame} : QVariant{};
    case MatchKindRole:
        return row.matchKind;
    case ConfidenceRole:
        return row.confidence;
    case MissingRole:
        return row.missing;
    case MissingReasonRole:
        return row.missingReason;
    case ManualOffsetRole:
        return row.manualOffset;
    case ChangedOnDiskRole:
        return row.changedOnDisk;
    default:
        return {};
    }
}

QHash<int, QByteArray> SourceListModel::roleNames() const {
    return {
        {SourceIdRole, QByteArrayLiteral("sourceId")},
        {SourceIdentityRole, QByteArrayLiteral("sourceIdentity")},
        {RoleRole, QByteArrayLiteral("role")},
        {FilenameRole, QByteArrayLiteral("filename")},
        {ParentLabelRole, QByteArrayLiteral("parentLabel")},
        {FullPathRole, QByteArrayLiteral("fullPath")},
        {ErrorRole, QByteArrayLiteral("errorKey")},
        {CurrentSourceFrameRole, QByteArrayLiteral("currentSourceFrame")},
        {MatchKindRole, QByteArrayLiteral("matchKind")},
        {ConfidenceRole, QByteArrayLiteral("confidence")},
        {MissingRole, QByteArrayLiteral("missing")},
        {MissingReasonRole, QByteArrayLiteral("missingReason")},
        {ManualOffsetRole, QByteArrayLiteral("manualOffset")},
        {ChangedOnDiskRole, QByteArrayLiteral("changedOnDisk")},
    };
}

void SourceListModel::setRows(std::vector<SourceListRow> rows) {
    if (rows_ == rows) {
        return;
    }

    const bool identityChanged =
        rows_.size() != rows.size() ||
        !std::equal(
            rows_.begin(), rows_.end(), rows.begin(), [](const auto& left, const auto& right) {
                return left.sourceId == right.sourceId;
            });
    if (identityChanged) {
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
        return;
    }

    QList<int> changedRoles;
    const auto addRole = [&changedRoles](const int role) {
        if (!changedRoles.contains(role)) {
            changedRoles.push_back(role);
        }
    };
    for (std::size_t row = 0U; row < rows_.size(); ++row) {
        const SourceListRow& before = rows_[row];
        const SourceListRow& after = rows[row];
        if (before.sourceIdentity != after.sourceIdentity) {
            addRole(SourceIdentityRole);
        }
        if (before.role != after.role) {
            addRole(RoleRole);
        }
        if (before.filename != after.filename) {
            addRole(FilenameRole);
        }
        if (before.parentLabel != after.parentLabel) {
            addRole(ParentLabelRole);
        }
        if (before.fullPath != after.fullPath) {
            addRole(FullPathRole);
        }
        if (before.errorKey != after.errorKey) {
            addRole(ErrorRole);
        }
        if (before.currentSourceFrame != after.currentSourceFrame) {
            addRole(CurrentSourceFrameRole);
        }
        if (before.matchKind != after.matchKind) {
            addRole(MatchKindRole);
        }
        if (before.confidence != after.confidence) {
            addRole(ConfidenceRole);
        }
        if (before.missing != after.missing) {
            addRole(MissingRole);
        }
        if (before.missingReason != after.missingReason) {
            addRole(MissingReasonRole);
        }
        if (before.manualOffset != after.manualOffset) {
            addRole(ManualOffsetRole);
        }
        if (before.changedOnDisk != after.changedOnDisk) {
            addRole(ChangedOnDiskRole);
        }
    }
    rows_ = std::move(rows);
    if (!changedRoles.isEmpty() && !rows_.empty()) {
        Q_EMIT dataChanged(
            index(0, 0), index(static_cast<int>(rows_.size() - 1U), 0), changedRoles);
    }
}

} // namespace dvs::ui
