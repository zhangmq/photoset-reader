#include "browsemodel.h"

BrowseModel::BrowseModel(QObject *parent) : QAbstractListModel(parent) {}

int BrowseModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant BrowseModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row &r = m_rows.at(index.row());
    switch (role) {
    case NameRole:      return r.name;
    case SizeTextRole:  return r.sizeText;
    case ResTextRole:   return r.resText;
    case KindRole:      return r.kind;
    case MissingRole:   return r.missing;
    case ThumbRole:     return r.thumb;
    case DepthRole:     return r.depth;
    case ItemIndexRole: return r.itemIndex;
    case NodeIndexRole: return r.nodeIndex;
    default:            return {};
    }
}

QHash<int, QByteArray> BrowseModel::roleNames() const
{
    return {
        { NameRole,      "name" },
        { SizeTextRole,  "sizeText" },
        { ResTextRole,   "resText" },
        { KindRole,      "kind" },
        { MissingRole,   "missing" },
        { ThumbRole,     "thumb" },
        { DepthRole,     "depth" },
        { ItemIndexRole, "itemIndex" },
        { NodeIndexRole, "nodeIndex" },
    };
}

void BrowseModel::setRows(QVector<Row> rows)
{
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
}

const BrowseModel::Row *BrowseModel::rowAt(int i) const
{
    if (i < 0 || i >= m_rows.size()) return nullptr;
    return &m_rows.at(i);
}
