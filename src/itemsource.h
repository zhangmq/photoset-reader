#pragma once

// ══ 条目来源：**库行 → 序列** 的接缝（v5 定案，2026-10-13）══════════════
//
//   ★ 定案：**图集 = 已写入磁盘的序列** —— 库扫描 = 跑一次序列扫描，并把"这棵序列的
//     行（节点 + 叶子）"写入磁盘；`LibraryItemSource` 就是"把已写入磁盘的那棵序列重新读出来"的那一层
//     （**不碰盘**：磁盘路径 / 容器成员链由 `Sequence::locateRows` 派生）。
//   ★ 条目**身份 = 沿 `parent_ord` 上溯的 label 序列**（页行不含自己的名字，用页号区分）。
//     **没有** key / rel_path 这类拼出来的字符串，也**没有**"包内身份"专用列。
//   ★ 缺失**不写入磁盘**：库里没有 missing 概念 ⇒ 这里也不报 missing（会话内标记归 `Sequence`）。
//   ★ 文件系统来源（直接打开：文件夹 / 压缩包 / 单图）**不经过这里** ——
//     它没有库行，走 `Sequence::startScan`（同一份扫描实现）。

#include "sequence.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace library { class Store; }

namespace app {

class ItemSource
{
public:
    virtual ~ItemSource() = default;

    // 可读序列（`kind=image` 的行，按 `ord`）：游标 / 进度 / 位置写入磁盘都用它
    virtual int         count() const = 0;
    virtual QStringList chainAt(int index) const = 0;              // 越界 ⇒ 空
    virtual int         pageAt(int index) const = 0;               // 越界 ⇒ 0
    // 沿 label 链 walk 树 + 页号精确命中（-1 = 没有）
    virtual int         indexOfChain(const QStringList &chain, int page) const = 0;

    virtual QString sourcePath() const = 0;                      // 来源根 = collection.root_path
    virtual QString sourceLabel() const = 0;
    virtual bool    rootIsArchive() const = 0;                   // 来源根本身是压缩包

    // **已写入磁盘的那些行**（容器节点也在里面）—— `Sequence::buildFromRows` 的唯一输入
    virtual QVector<Sequence::Row> sequenceRows() const = 0;
};

// ── 实现：图集库里的集合 —— 查 SQLite（`item` 表，按 `ord`）──
class LibraryItemSource : public ItemSource
{
public:
    LibraryItemSource(library::Store *store, qint64 collectionId);

    void reload();

    int         count() const override { return m_seq.count(); }
    QStringList chainAt(int index) const override { return m_seq.chainAt(index); }
    int         pageAt(int index) const override { return m_seq.pageAt(index); }
    int         indexOfChain(const QStringList &chain, int page) const override
    {
        return m_seq.indexOfChain(chain, page);
    }
    QString sourcePath() const override { return m_rootPath; }
    QString sourceLabel() const override;
    bool    rootIsArchive() const override { return m_rootIsArchive; }
    QVector<Sequence::Row> sequenceRows() const override { return m_rows; }

private:
    library::Store *m_store = nullptr;
    qint64  m_cid = -1;
    QString m_rootPath;
    QString m_title;
    bool    m_rootIsArchive = false;
    QVector<Sequence::Row> m_rows;      // 全部行（容器节点 + 叶子，按 ord）
    Sequence m_seq;                     // 由 m_rows 建（**不碰盘**）：身份链 / 序号都问它
};

// ── 图集级阅读版面（`collection.reading_layout`）↔ 序列模式（**唯一映射**）──
//   定案（设计 10 §11.1 / AGENTS.md）：图集级只有"阅读版面"这一项设置；默认单幅
Sequence::Mode layoutToMode(const QString &layout);   // "double" ⇒ Double，其余 ⇒ Single
QString        modeToLayout(Sequence::Mode m);        // Double ⇒ "double"，否则 "single"

} // namespace app
