#include "sequence.h"
#include "natural.h"
#include "archive.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QMap>
#include <QDebug>
#include <algorithm>

void Sequence::clear()
{
    m_items.clear();
    m_root.clear();
    m_recursive = false;
    m_archivePages = false;
}

void Sequence::sortNaturally()
{
    std::sort(m_items.begin(), m_items.end(), [](const Item &a, const Item &b) {
        return naturalCompare(a.displayName, b.displayName) < 0;
    });
}

// 子目录遍历：按文件夹分组，**组内自然排序**，组间按文件夹路径排序，
// 每组前插入一个 FolderBoundary（UI 当分节标题，不当条目）。
void Sequence::buildFromFolder(const QString &dir, bool recursive)
{
    clear();
    m_root = QFileInfo(dir).absoluteFilePath();
    m_recursive = recursive;

    // folderPath → 条目
    QMap<QString, QVector<Item>> groups;

    auto collect = [&](const QString &folder) {
        QDir d(folder);
        const QFileInfoList entries = d.entryInfoList(QDir::Files, QDir::NoSort);
        for (const QFileInfo &fi : entries) {
            Item it;
            it.displayName = fi.fileName();
            it.fullPath = fi.absoluteFilePath();
            it.folderLabel = (folder == m_root) ? QString() : d.dirName();
            it.folderDepth = 0;
            if (isImageSuffix(fi.suffix())) {
                it.kind = Kind::Image;
            } else if (ArchiveSource::isArchive(fi.absoluteFilePath())) {
                it.kind = Kind::Archive;      // ★ 压缩包算**一个条目**
            } else {
                continue;                     // 其它类型忽略
            }
            groups[folder] << it;
        }
    };

    collect(m_root);

    if (recursive) {
        QDirIterator it(m_root, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString sub = it.next();
            collect(sub);
        }
    }

    // 根目录在最前，其余按路径自然排序
    QStringList folders = groups.keys();
    std::sort(folders.begin(), folders.end(), [this](const QString &a, const QString &b) {
        if (a == m_root) return true;
        if (b == m_root) return false;
        return naturalCompare(a, b) < 0;
    });

    for (const QString &folder : folders) {
        QVector<Item> &v = groups[folder];
        std::sort(v.begin(), v.end(), [](const Item &a, const Item &b) {
            return naturalCompare(a.displayName, b.displayName) < 0;
        });

        // 子目录模式下，非根文件夹前插入边界标记
        if (recursive && folder != m_root) {
            Item b;
            b.kind = Kind::FolderBoundary;
            b.displayName = QDir(folder).dirName();
            b.fullPath = folder;
            b.folderLabel = QDir(m_root).relativeFilePath(folder);
            b.folderDepth = QDir(m_root).relativeFilePath(folder).count(QLatin1Char('/'));
            m_items << b;
        }
        for (Item &x : v) {
            if (recursive && folder != m_root)
                x.folderDepth = QDir(m_root).relativeFilePath(folder).count(QLatin1Char('/'));
            m_items << x;
        }
    }
}

void Sequence::buildFromSingleFile(const QString &filePath)
{
    clear();
    const QFileInfo fi(filePath);
    m_root = fi.absolutePath();                // 根是所在目录
    m_archivePages = false;                    // ★ 关键：不是归档内页
    Item it;
    it.displayName = fi.fileName();
    it.fullPath = fi.absoluteFilePath();       // 文件系统路径
    it.kind = Kind::Image;
    m_items << it;
}

void Sequence::buildFromMultiPageFile(const QString &filePath, int pageCount)
{
    clear();
    const QFileInfo fi(filePath);
    m_root = fi.absolutePath();
    m_archivePages = false;                    // 走普通文件系统路径
    for (int i = 0; i < pageCount; ++i) {
        Item it;
        it.displayName = QStringLiteral("%1 [%2/%3]").arg(fi.fileName()).arg(i+1).arg(pageCount);
        it.fullPath = fi.absoluteFilePath();
        it.kind = Kind::Image;
        it.pageIndex = i;
        m_items << it;
    }
}

void Sequence::buildFromNames(const QString &archivePath, const QStringList &names)
{
    clear();
    m_root = QFileInfo(archivePath).absoluteFilePath();
    m_archivePages = true;

    for (const QString &n : names) {
        Item it;
        it.displayName = QFileInfo(n).fileName();
        it.fullPath = n;                       // 归档内路径
        it.kind = Kind::Image;
        m_items << it;
    }
    sortNaturally();                           // ★ 归档内顺序可能是乱的
}

// 只标记，不移除——移除会让索引位移，破坏位置记忆（AGENTS.md 红线）
int Sequence::markMissing()
{
    if (m_archivePages) return 0;              // 归档内条目不做文件系统校验
    int n = 0;
    for (Item &it : m_items) {
        if (it.kind == Kind::FolderBoundary) continue;
        const bool exists = QFileInfo::exists(it.fullPath);
        if (!exists && !it.missing) { it.missing = true; ++n; }
        else if (exists && it.missing) { it.missing = false; }
    }
    return n;
}
