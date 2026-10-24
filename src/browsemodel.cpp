#include "browsemodel.h"

BrowseModel::BrowseModel(QObject *parent) : QAbstractListModel(parent) {}

int BrowseModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

// ★ QML 边界的那一份字符串（`image` / `archive` / `folder`）—— **唯一生成点**：
//   它只是把 `Sequence::Kind` 投影成 QML 认的三个词，**不是第二份概念定义**。
QString BrowseModel::kindText(Sequence::Kind k)
{
    switch (k) {
    case Sequence::Kind::Archive: return QStringLiteral("archive");
    case Sequence::Kind::Folder:  return QStringLiteral("folder");
    case Sequence::Kind::Image:   break;
    }
    return QStringLiteral("image");
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
    case KindRole:      return BrowseModel::kindText(r.kind);
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

namespace {
// 两行之间**哪些列**变了（返回空 = 完全一样）
QVector<int> changedRoles(const BrowseModel::Row &a, const BrowseModel::Row &b)
{
    QVector<int> roles;
    if (a.name != b.name)            roles << BrowseModel::NameRole;
    if (a.sizeText != b.sizeText)    roles << BrowseModel::SizeTextRole;
    if (a.resText != b.resText)      roles << BrowseModel::ResTextRole;
    if (a.kind != b.kind)            roles << BrowseModel::KindRole;
    if (a.missing != b.missing)      roles << BrowseModel::MissingRole;
    if (a.thumb != b.thumb)          roles << BrowseModel::ThumbRole;
    if (a.depth != b.depth)          roles << BrowseModel::DepthRole;
    if (a.itemIndex != b.itemIndex)  roles << BrowseModel::ItemIndexRole;
    if (a.nodeIndex != b.nodeIndex)  roles << BrowseModel::NodeIndexRole;
    return roles;
}
}  // namespace

//  ★★ 同一个层又长了一批（扫描中）⇒ **只发增量**，不重建整表。
//     `beginResetModel/endResetModel` 的语义是"旧数据全失效 + 视图 reset"（滚动位置与当前项一起丢），
//     用它表达"多了 16 行"就是扫描期间没法浏览的原因（实测：contentY 从 3000 变 0）。
//   · 纯插入（尾部 / 中间）⇒ `beginInsertRows`，只插新增的那几行；
//   · 长度不变、key 全对齐 ⇒ 只可能是字段变（例：文件夹的"N 张"随内层解析增长）⇒ `dataChanged`；
//   · 其余（换层 / 换来源 / 缩短 / 重排）⇒ 整表重建（由调用方用 `setRows`，这里兜底）。
void BrowseModel::patchRows(QVector<Row> rows)
{
    const QVector<Row> old = m_rows;                 // 隐式共享，不复制元素
    const int oldN = old.size();
    const int newN = rows.size();

    int p = 0;                                        // 公共前缀长度
    while (p < oldN && p < newN && old.at(p).key == rows.at(p).key) ++p;

    if (oldN == newN && p == oldN) {                  // 行数没变、key 全对齐 ⇒ 只比字段
        m_rows = std::move(rows);
        emitFieldDiff(old, m_rows, 0, 0, oldN);
        return;
    }

    const int k = newN - oldN;
    bool pureInsert = false;
    int at = p;
    if (k > 0) {
        if (p == oldN) {                              // 尾部插入
            pureInsert = true;
            at = oldN;
        } else {                                      // 中间插入：旧行整体后移 k，顺序不变
            pureInsert = true;
            for (int i = p; i < oldN; ++i)
                if (old.at(i).key != rows.at(i + k).key) { pureInsert = false; break; }
            at = p;
        }
    }
    if (!pureInsert) {                                // 其余形态：整表重建
        beginResetModel();
        m_rows = std::move(rows);
        endResetModel();
        return;
    }

    beginInsertRows(QModelIndex(), at, at + k - 1);
    m_rows = std::move(rows);
    endInsertRows();

    emitFieldDiff(old, m_rows, 0, 0, at);             // 前缀：旧/新一一对应
    emitFieldDiff(old, m_rows, at, at + k, oldN - at);   // 尾部：整体后移了 k
}

void BrowseModel::emitFieldDiff(const QVector<Row> &old, const QVector<Row> &cur,
                                int oldFrom, int curFrom, int count)
{
    int i = 0;
    while (i < count) {
        QVector<int> roles = changedRoles(old.at(oldFrom + i), cur.at(curFrom + i));
        if (roles.isEmpty()) { ++i; continue; }
        int j = i + 1;                                // 连续变化的行并成一段
        while (j < count) {
            const QVector<int> r = changedRoles(old.at(oldFrom + j), cur.at(curFrom + j));
            if (r.isEmpty()) break;
            for (int role : r) if (!roles.contains(role)) roles << role;
            ++j;
        }
        emit dataChanged(index(curFrom + i), index(curFrom + j - 1), roles);
        i = j;
    }
}

const BrowseModel::Row *BrowseModel::rowAt(int i) const
{
    if (i < 0 || i >= m_rows.size()) return nullptr;
    return &m_rows.at(i);
}
