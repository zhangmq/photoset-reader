#pragma once

#include <QFileInfo>
#include <QSet>
#include <QString>
#include <QVector>

// ══ 目录遍历器（**唯一实现**：图集库扫描器 与 查看序列 共用）═════════════
//
//   ★ 为什么独立成一份（算法单一事实源）：图集库扫描（写入磁盘）与查看序列（会话内）
//     走的是**同一套**遍历规则；两份实现必然分叉。
//     （2026-10-12 用户定案：序列与图集扫描结果"几乎是同一个东西"，差别只是写不写入磁盘。）
//
//   规则（沿用既有定案，未改）：
//   · **逐目录**产出（不是一个根目录一次 I/O 全扫完）⇒ 调用方可以"每批"提交 / 查取消；
//   · **深度优先**：同级目录**自然排序**后**反着入栈**，弹出即自然升序（设计 10 §六）；
//   · **防环**：已访问目录集合按**规范路径**去重（第一道，`ln -s ..` 这类必须靠它）；
//     深度上限是**第二道**保险，不能只靠深度；
//   · **隐藏项**：总是请求 `QDir::Hidden`，再自己按 `isHidden()` 过滤
//     （不用"点开头"这种 OS 差异）；
//   · 只收**图片与压缩包**；其它类型忽略。
//
//   ⚠️ **压缩包在这里只算一个条目、不展开**（库扫描的语义）。
//      展开压缩包 / 多页文件是**查看序列**的事（`Sequence` 的扫描）。
//
//   ⚠️ 这里是**文件系统**的遍历；`files` 里的顺序就是**序列顺序**（自然排序，非字母序）。
struct DirBatch {
    QString            dirPath;    // 绝对路径
    QString            relFolder;  // 相对根（根 = 空串）
    int                level = 0;  // 层数（root = 0）
    QVector<QFileInfo> files;      // 自然排序后的**图片 / 压缩包**
    QVector<QFileInfo> subs;       // 自然排序后的**子目录**（同一批给出，供"文件树索引"用）
};

class DirWalker
{
public:
    // maxDepth：root = 层 0；hidden：是否进入隐藏项
    DirWalker(QString root, int maxDepth, bool hidden);

    // 取下一个目录批次。false = 走完了。
    bool next(DirBatch *out);

private:
    struct Pending {
        QString path;
        QString rel;
        int     level = 0;
    };

    QVector<Pending> m_stack;
    QSet<QString>    m_visited;    // 规范路径（防环）
    int              m_maxDepth = 5;
    bool             m_hidden = false;
};
