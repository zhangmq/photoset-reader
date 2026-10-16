#include "itemsource.h"

#include "archive.h"
#include "librarystore.h"

#include <QFileInfo>

namespace app {

// ── 实现：图集库里的集合（库行 = 已写入磁盘的序列；这里只读，**不碰盘**）──────

LibraryItemSource::LibraryItemSource(library::Store *store, qint64 collectionId)
    : m_store(store), m_cid(collectionId)
{
    if (m_store) {
        const library::Collection c = m_store->collection(m_cid);
        m_rootPath = c.rootPath;
        m_title = c.title;
        // 来源根本身就是压缩包？⇒ 包内成员没有"祖先 archive 行"（根不是一行），
        //   `Sequence::locateRows` 靠这个标志把最外层容器指到来源根
        m_rootIsArchive = c.kind == QLatin1String("archive")
                          || (QFileInfo(m_rootPath).isFile()
                              && ArchiveSource::isArchive(m_rootPath));
    }
    reload();
}

void LibraryItemSource::reload()
{
    m_rows.clear();
    if (!m_store) return;

    // 库行（`library::Item`）→ 序列行（`Sequence::Row`）：字段一一对应
    const QVector<library::Item> rows = m_store->items(m_cid);
    m_rows.reserve(rows.size());
    for (const library::Item &r : rows) {
        Sequence::Row row;
        row.ord = r.ord;
        row.parentOrd = r.parentOrd;
        row.kind = r.kind;
        row.label = r.label;
        row.pageIndex = r.pageIndex;
        row.size = r.size;
        row.mtime = r.mtime;
        row.width = r.width;
        row.height = r.height;
        m_rows << row;
    }
    // 身份链 / 序号都问**同一条序列**（唯一的沿链解析与树 walk 实现）
    m_seq.clear();
    m_seq.buildFromRows(m_rootPath, m_rows, m_rootIsArchive, sourceLabel());
}

QString LibraryItemSource::sourceLabel() const
{
    if (!m_title.isEmpty()) return m_title;
    return QFileInfo(m_rootPath).fileName();
}

// ── 图集级阅读版面 ↔ 序列模式 ──
Sequence::Mode layoutToMode(const QString &layout)
{
    return layout == QLatin1String("double") ? Sequence::Mode::Double : Sequence::Mode::Single;
}

QString modeToLayout(Sequence::Mode m)
{
    return m == Sequence::Mode::Double ? QStringLiteral("double") : QStringLiteral("single");
}

} // namespace app
