#include "progresslistmodel.h"

#include "progressfile.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace {

// 文件名 → 显示名：`<title>-<hash8>.prsave` ⇒ `<title>`
//   （hash 认不出来就整段去掉扩展名退回，不猜）
QString titleFromFileName(const QString &fileName)
{
    QString base = fileName;
    if (base.endsWith(QLatin1String(".prsave"), Qt::CaseInsensitive))
        base.chop(int(qstrlen(".prsave")));
    const int dash = base.lastIndexOf(QLatin1Char('-'));
    if (dash > 0) {
        const QString tail = base.mid(dash + 1);
        if (tail.size() == 8
            && std::all_of(tail.cbegin(), tail.cend(), [](QChar c) {
                   return c.isDigit() || (c.toLower() >= QLatin1Char('a') && c.toLower() <= QLatin1Char('f'));
               }))
            return base.left(dash);
    }
    return base;
}

} // namespace

ProgressListModel::ProgressListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    //   ★ 2026-11-02（用户）：收藏夹**打开时列一次**即可，不监控目录 ——
    //     目录监听会带来"编辑期间列表跳动"和平台差异，收益也就是少按一次刷新。
}

void ProgressListModel::setDirectory(const QString &dir)
{
    const QString v = QDir::cleanPath(dir);
    if (v == m_dir)
        return;
    m_dir = v;
    emit directoryChanged();
    //   ⚠️ 这里**不 reload**：收藏夹目录的列举只在**弹框打开**时做（用户 2026-11-02），
    //      应用启动 / 改设置都不去碰磁盘。
}

void ProgressListModel::setFilter(const QString &text)
{
    if (text == m_filter)
        return;
    m_filter = text;
    emit filterChanged();
    rebuildRows();
}

void ProgressListModel::reload()
{
    //   ★ 只列文件名（不读内容）—— 目录大小只影响这次遍历，不读任何存档内容
    QStringList files;
    if (!m_dir.isEmpty() && QFileInfo(m_dir).isDir()) {
        QDir d(m_dir);
        const QFileInfoList infos = d.entryInfoList(QStringList{ QStringLiteral("*.prsave") },
                                                    QDir::Files, QDir::Name);
        files.reserve(infos.size());
        for (const QFileInfo &fi : infos)
            files << fi.absoluteFilePath();
    }
    m_files = files;
    m_meta.clear();                          // 内容可能变了；缓存只是加速，随时可丢
    m_metaOrder.clear();
    rebuildRows();
}

void ProgressListModel::rebuildRows()
{
    beginResetModel();
    m_rows.clear();
    m_rows.reserve(m_files.size());
    for (const QString &f : m_files) {
        const QString title = titleFromFileName(QFileInfo(f).fileName());
        if (!m_filter.isEmpty() && !title.contains(m_filter, Qt::CaseInsensitive))
            continue;
        m_rows.append(Row{ f, title });
    }
    endResetModel();
    emit countChanged();
}

int ProgressListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_rows.size();
}

QString ProgressListModel::fileAt(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return QString();
    return m_rows.at(row).file;
}

//   懒读：只有界面问到这些 role 才读文件（可见行 ≈ 十几个 ⇒ 首屏只读十几个）
const ProgressListModel::Meta &ProgressListModel::metaFor(const QString &file) const
{
    auto it = m_meta.find(file);
    if (it != m_meta.end())
        return *it;

    //   ★ 只读**元数据段**（不解析 rows）；v1 老文件退回整份读
    Meta m;
    QString err;
    progressfile::Meta pm;
    if (progressfile::readMeta(file, &pm, &err)) {
        m.ok = true;
        m.sourcePath = pm.sourcePath;
        m.done = pm.done;
        m.total = pm.total;
    } else {
        progressfile::Data d;
        if (progressfile::read(file, &d, &err)) {
            m.ok = true;
            m.sourcePath = d.meta.sourcePath;
            m.done = d.meta.done;
            m.total = d.meta.total;
        } else {
            m.error = err.isEmpty() ? QStringLiteral("无法读取") : err;
        }
    }

    // FIFO 32 条（不搞 LRU：滚动方向来回都只会命中最近这些）
    if (m_metaOrder.size() >= 32) {
        const QString oldest = m_metaOrder.takeFirst();
        m_meta.remove(oldest);
    }
    m_metaOrder.append(file);
    auto inserted = m_meta.insert(file, m);
    return *inserted;
}

QVariant ProgressListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row &row = m_rows.at(index.row());

    switch (role) {
    case TitleRole: return row.title;
    case FileRole:  return row.file;
    case SourcePathRole: {
        const Meta &m = metaFor(row.file);
        return m.ok ? m.sourcePath : QString();
    }
    case DoneRole: {
        const Meta &m = metaFor(row.file);
        return m.ok ? m.done : 0;
    }
    case TotalRole: {
        const Meta &m = metaFor(row.file);
        return m.ok ? m.total : 0;
    }
    case RatioRole: {
        const Meta &m = metaFor(row.file);
        return (m.ok && m.total > 0) ? double(m.done) / double(m.total) : 0.0;
    }
    case ErrorRole: {
        const Meta &m = metaFor(row.file);
        return m.ok ? QString() : m.error;
    }
    default: return {};
    }
}

QHash<int, QByteArray> ProgressListModel::roleNames() const
{
    return {
        { TitleRole,      "title" },
        { FileRole,       "file" },
        { SourcePathRole, "sourcePath" },
        { DoneRole,       "done" },
        { TotalRole,      "total" },
        { RatioRole,      "ratio" },
        { ErrorRole,      "errorText" }
    };
}
