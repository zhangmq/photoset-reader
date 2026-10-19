#include "dirwalker.h"

#include "archive.h"
#include "natural.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>

// 从**旧的扫描路径**的匿名命名空间**原样搬出来**（规则一字未改），
// 让图集重置与查看序列共用同一份遍历（算法单一事实源）。

DirWalker::DirWalker(int maxDepth, bool hidden)
    : m_maxDepth(qMax(0, maxDepth)), m_hidden(hidden)
{
}

bool DirWalker::readDir(const QString &path, int level, DirBatch *out)
{
    if (level > m_maxDepth) return false;              // ★ 深度上限：这一层不列、不进

    // ★ 防环（第一道）：已访问目录集合，按**规范路径**去重。
    //   `ln -s ../../a` 指回祖先 / `ln -s . self` 自指 → 这里被挡掉；
    //   深度上限只是第二道保险，**不能只靠深度**。
    const QString canon = QFileInfo(path).canonicalFilePath();
    if (canon.isEmpty()) return false;                 // 断链 / 读不到
    if (m_visited.contains(canon)) return false;
    m_visited.insert(canon);

    out->dirPath = path;
    out->level = level;
    out->entries.clear();

    QDir d(path);

    // ── 子目录：总是请求 Hidden，再自己按 isHidden() 过滤（不用"点开头"这种 OS 差异）──
    const QFileInfoList dirs = d.entryInfoList(
        QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDir::NoSort);
    for (const QFileInfo &fi : dirs) {
        if (!m_hidden && fi.isHidden()) continue;      // 隐藏目录默认不进去
        if (level + 1 > m_maxDepth) continue;          // 下一层进不去 ⇒ 也不列它
        DirEntry e;
        e.name = fi.fileName();
        e.path = fi.absoluteFilePath();
        e.isDir = true;
        out->entries << e;
    }

    // ── 文件：只收图片与压缩包（压缩包只算一个条目，**不打开、不展开**）──
    const QFileInfoList files = d.entryInfoList(
        QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDir::NoSort);
    for (const QFileInfo &fi : files) {
        if (!m_hidden && fi.isHidden()) continue;
        const bool isArchive = ArchiveSource::isArchive(fi.absoluteFilePath());
        if (!isImageSuffix(fi.suffix()) && !isArchive) continue;
        DirEntry e;
        e.name = fi.fileName();
        e.path = fi.absoluteFilePath();
        e.isArchive = isArchive;
        out->entries << e;
    }

    // ★★ **一条名次序**（图片 / 压缩包 / 子目录同序）—— 遍历顺序 = 这个顺序 +
    //    遇到子目录当场下钻（先根遍历，见头文件）。
    //    ⚠️ 子目录按 `名字 + "/"` 比（`nameSortKey`）⇒ `A.jpg` 在 `A/` **前面**
    //      （图集常见形态：上层那张封面图紧挨它对应文件夹之前）。
    std::sort(out->entries.begin(), out->entries.end(),
              [](const DirEntry &a, const DirEntry &b) {
                  return naturalCompare(nameSortKey(a.name, a.isDir),
                                        nameSortKey(b.name, b.isDir)) < 0;
              });
    return true;
}
