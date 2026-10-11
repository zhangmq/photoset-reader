#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QSize>

// 序列（设计 §1.2 / §2.5 / §2.6）。
//
// 两级：
//   · **一级**：文件夹里的条目——图片文件、**压缩包（算一个条目）**、子文件夹边界
//   · **二级**：进入压缩包后的内页序列
//
// 规则（AGENTS.md「两个场景的技术红线」）：
//   · **自然排序，且必须自己实现**（与 locale 无关）
//   · 子目录遍历要记录**文件夹边界**（UI 上作为分节，不是条目）
//   · 条目有**三态**：正常 / 缺失 / 不可读——序列是快照，文件系统是真相
class Sequence
{
public:
    enum class Kind { Image, Archive, FolderBoundary };

    struct Item {
        Kind kind = Kind::Image;
        QString displayName;     // 显示名（文件名）
        QString fullPath;        // 绝对路径（图片或压缩包）
        QString folderLabel;     // 所属文件夹（子目录模式下的边界标注）
        int folderDepth = 0;
        bool missing = false;    // 惰性发现：已消失
        int pageIndex = 0;       // 多页文件（多页 TIFF 等）内的页号；单页恒为 0
    };

    // 从文件夹构建。recursive = 是否遍历子目录（含边界标记）。
    // 只收图片与压缩包；其它类型忽略。
    void buildFromFolder(const QString &dir, bool recursive);
    // 单个图片文件：1 项序列，**走普通文件系统路径**（不是归档内页）
    void buildFromSingleFile(const QString &filePath);
    // 多页文件（多页 TIFF 等）：同一路径 + N 个页号，**页容器语义**
    void buildFromMultiPageFile(const QString &filePath, int pageCount);

    // 从一个显式列表构建（压缩包内页：由 ArchiveSource 提供名字）
    void buildFromNames(const QString &archivePath, const QStringList &names);

    void clear();
    bool isEmpty() const { return m_items.isEmpty(); }
    int count() const { return m_items.size(); }
    const Item &at(int i) const { return m_items.at(i); }
    const QVector<Item> &items() const { return m_items; }

    // 排序后重建（自然排序）。buildFrom* 内部已调用，暴露供测试。
    void sortNaturally();

    // 惰性校验：把已消失的条目标记为 missing（**只标记，不移除**）
    int markMissing();

    QString rootPath() const { return m_root; }
    bool isRecursive() const { return m_recursive; }

    // 压缩包模式
    bool isArchivePages() const { return m_archivePages; }
    QString archivePath() const { return m_root; }

private:
    QVector<Item> m_items;
    QString m_root;
    bool m_recursive = false;
    bool m_archivePages = false;
};
