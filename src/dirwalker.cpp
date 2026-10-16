#include "dirwalker.h"

#include "archive.h"
#include "natural.h"

#include <QDir>

#include <algorithm>

// 从**旧的扫描路径**的匿名命名空间**原样搬出来**（规则一字未改），
// 让图集重置与查看序列共用同一份遍历（算法单一事实源）。

DirWalker::DirWalker(QString root, int maxDepth, bool hidden)
{
    m_maxDepth = qMax(0, maxDepth);
    m_hidden = hidden;
    m_stack.append(Pending{ std::move(root), QString(), 0 });
}

bool DirWalker::next(DirBatch *out)
{
    while (!m_stack.isEmpty()) {
        const Pending p = m_stack.takeLast();          // 栈 → 深度优先

        // ★ 防环（第一道）：已访问目录集合，按**规范路径**去重。
        //   `ln -s ../../a` 指回祖先 / `ln -s . self` 自指 → 这里被挡掉；
        //   深度上限只是第二道保险，**不能只靠深度**。
        const QString canon = QFileInfo(p.path).canonicalFilePath();
        if (canon.isEmpty()) continue;                 // 断链 / 读不到
        if (m_visited.contains(canon)) continue;
        m_visited.insert(canon);

        out->dirPath = p.path;
        out->relFolder = p.rel;
        out->level = p.level;
        out->files.clear();
        out->subs.clear();

        QDir d(p.path);

        // ── 子目录：总是请求 Hidden，再自己按 isHidden() 过滤（不用"点开头"这种 OS 差异）──
        const QFileInfoList dirs = d.entryInfoList(
            QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDir::NoSort);
        QVector<QFileInfo> subs;
        subs.reserve(dirs.size());
        for (const QFileInfo &fi : dirs) {
            if (!m_hidden && fi.isHidden()) continue;  // 隐藏目录默认不进去
            if (p.level + 1 > m_maxDepth) continue;    // ★ 防环（第二道）：深度上限
            subs.append(fi);
        }
        // 同级目录自然排序 → 深度优先 = 最终顺序（设计 10 §六）
        std::sort(subs.begin(), subs.end(), [](const QFileInfo &a, const QFileInfo &b) {
            return naturalCompare(a.fileName(), b.fileName()) < 0;
        });
        // 反着入栈：弹出即自然升序
        for (auto it = subs.crbegin(); it != subs.crend(); ++it) {
            const QString rel = p.rel.isEmpty()
                ? it->fileName()
                : p.rel + QLatin1Char('/') + it->fileName();
            m_stack.append(Pending{ it->absoluteFilePath(), rel, p.level + 1 });
        }
        out->subs = subs;

        // ── 文件：只收图片与压缩包（压缩包只算一个条目，**不打开、不展开**）──
        const QFileInfoList files = d.entryInfoList(
            QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDir::NoSort);
        for (const QFileInfo &fi : files) {
            if (!m_hidden && fi.isHidden()) continue;
            if (!isImageSuffix(fi.suffix())
                && !ArchiveSource::isArchive(fi.absoluteFilePath()))
                continue;
            out->files.append(fi);
        }
        std::sort(out->files.begin(), out->files.end(),
                  [](const QFileInfo &a, const QFileInfo &b) {
                      return naturalCompare(a.fileName(), b.fileName()) < 0;
                  });
        return true;
    }
    return false;
}
