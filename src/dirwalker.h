#pragma once

#include <QSet>
#include <QString>
#include <QVector>

// ══ 目录读取器（**唯一实现**：图集库扫描器 与 查看序列 共用）═════════════
//
//   ★ 为什么独立成一份（算法单一事实源）：图集库扫描（写入磁盘）与查看序列（会话内）
//     走的是**同一套**遍历规则；两份实现必然分叉。
//     （2026-10-12 用户定案：序列与图集扫描结果"几乎是同一个东西"，差别只是写不写入磁盘。）
//
//   ★★ 遍历 = **先根遍历（preorder DFS）**，递归由**调用方**完成（2026-10-19 用户定案）：
//     读一层 ⇒ 拿到**一条名次序**（图片 / 压缩包 / 子目录**同序**）⇒ 按这个顺序逐个处理，
//     遇到子目录**当场下钻**、回来接着下一个。
//     **不是**"先把这一层的图全输出，再看下一层"（那等于给同层加了一条"类型优先"的局部序）。
//
//   · 规则（沿用既有定案，未改）：
//     - 只收**图片与压缩包**（其它类型忽略）；
//     - **防环**：已访问目录集合按**规范路径**去重（`ln -s ..` / `ln -s . self` 必须靠它）；
//       深度上限是**第二道**保险，不能只靠深度；
//     - **隐藏项**：总是请求 `QDir::Hidden`，再自己按 `isHidden()` 过滤
//       （不用"点开头"这种 OS 差异）；
//     - **深度上限**：`level > maxDepth` 的目录**不列、不进**（root = 层 0）。
//
//   ⚠️ **压缩包在这里只算一个条目、不展开**（库扫描的语义）。
//      展开压缩包 / 多页文件是**查看序列**的事（`Sequence` 的扫描）。
//
//   ⚠️ 这里是**文件系统**的读取；`entries` 的顺序就是**序列顺序**（名称序 = 自然排序，非字母序）。
struct DirEntry {
    QString name;                  // 单段名（排序键 / 显示名）
    QString path;                  // 绝对路径
    bool    isDir = false;         // 子目录
    bool    isArchive = false;     // 文件是压缩包（图片 ⇒ 两个都 false）
};

struct DirBatch {
    QString dirPath;               // 这一层的绝对路径
    int     level = 0;             // 层数（root = 0）
    QVector<DirEntry> entries;     // ★ **一条名次序**：图片 / 压缩包 / 子目录 同序
};

class DirWalker
{
public:
    // maxDepth：root = 层 0；hidden：是否进隐藏项
    DirWalker(int maxDepth, bool hidden);

    // 读**一层**（把这一层的直接子项按名称序一次给出，含种类）。
    //   false = 这一层**不该进**（读不到 / 断链 / 已访问过 / 超过深度上限）
    //   ⇒ 调用方连容器节点都不建（可观察结果与"它不出现"一致）。
    bool readDir(const QString &path, int level, DirBatch *out);

private:
    int           m_maxDepth = 5;
    bool          m_hidden = false;
    QSet<QString> m_visited;       // 规范路径（防环）
};
