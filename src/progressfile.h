#pragma once

// ══ 进度文件（`.prsave`）：**保存 / 打开进度的唯一实现** ═══════════════════════
//   设计：[20-进度文件与收藏夹](../docs/design/20-进度文件与收藏夹.md)
//
//   · **一个源一个文件**：收藏夹里 = `<title>-<hash8>.prsave`（`hash8` = 源规范路径的
//     SHA-256 前 8 位）⇒ 同一个源再保存**覆盖同一个文件**，不会越存越多；
//   · 外部进度文件 = 同样内容，由用户选路径（不进收藏夹、不被列出）；
//   · **文件就是记录**：没有数据库、没有索引文件；移除 = 删文件；
//   · 写盘用 `QSaveFile`（临时文件 + 原子替换）：半截文件不会被读到。
//
//   ## 文件结构（2026-11-01 定案，v2）
//
//     prsave 2                       ← 魔数 + 版本（第一行）
//     savedAt 1761234567
//     title vol01
//     source /a/vol01
//     kind folder
//     mtime 1761234000
//     scan 0 5 512                   ← hidden depth innerArchiveMaxMb
//     layout 0
//     chain ["vol01","005.jpg"]      ← 位置：label 链（一行 JSON 数组）+ 页号
//     page 0
//     progress 61 117                ← 只给列表显示；恢复以 chain/page 为准
//     rows 117                       ← 行数（诊断用）
//     ---                            ← 元数据段结束
//     [ {…}, {…} ]                   ← 其余 = rows（JSON 数组，与 `Sequence::Row` 一一对应）
//
//   ★ **为什么元数据段是纯文本而不是 JSON**（用户 2026-11-01）：
//     列表要显示 N/M 时**只读元数据段**（前 8 KB / 十几行），不必解析整份 rows；
//     Qt 的 `QJsonDocument` **只能整份解析**（没有流式 API）⇒ 用"固定格式的行"最省。
//     `chain` 那种列表值仍用**一行紧凑 JSON**（JSON 会把换行转义成 `\n`，不会破坏分行）。
//   ★ 文本值里的 `\` / 换行写成 `\\` / `\n`（见 escapeText），所以**物理换行永远只来自我们的分行**。
//   ★ 兼容 v1（2026-10-31 那版：单个 JSON 文档）：读的时候先看第一行是不是 `prsave`，
//     不是就当 v1 整份解析 —— 不需要迁移。

#include <QString>
#include <QStringList>
#include <QVector>

#include "sequence.h"

namespace progressfile {

// 文件扩展名（对话框过滤器 / 识别都用这一个常量）
inline constexpr const char *kSuffix = "prsave";
inline constexpr int kVersion = 2;

// 元数据段（列表只用这一截；恢复也只从这里取"来源 / 位置 / 版面"）
struct Meta {
    int     version = kVersion;
    qint64  savedAt = 0;

    QString        title;                  // 列表显示用（源的名字）
    QString        sourcePath;             // 源**绝对路径**
    //   ★ 加密压缩包（2026-11-03）：**源本身**是加密容器时的密码（明文）。
    //     嵌套容器的密码在各自的 `Sequence::Row::password` 上（根容器没有行）。
    QString        sourcePassword;
    Sequence::Kind sourceKind = Sequence::Kind::Folder;
    qint64         sourceMtime = 0;        // 保存那一刻（诊断用；**不做自动重扫判定**）

    bool   hidden = false;                 // 当时的扫描参数（诊断用；恢复不重扫）
    int    depth = 5;
    qint64 innerArchiveMaxMb = 512;

    int layout = 0;                        // 阅读版面（0 单幅 / 1 双幅）

    // 位置 = **label 链 + 页号**（跨会话稳定；恢复时 `Sequence::indexOfChain`）
    QStringList chain;
    int         page = 0;

    // 只给列表显示用的快照（"第 N 条 / 共 M 条"）；恢复**以 chain/page 为准**
    int done = 0;
    int total = 0;

    int rowCount = 0;                      // 行数（诊断用）
};

struct Data {
    Meta                   meta;
    QVector<Sequence::Row> rows;
};

// ── 命名 ────────────────────────────────────────────────────────────────
QString hash8(const QString &sourcePath);          // 规范路径的短哈希
QString titleOf(const QString &sourcePath);        // 文件夹名 / 压缩包文件名
QString fileNameFor(const QString &title, const QString &sourcePath);
bool    isProgressFile(const QString &path);

// ── 读写 ────────────────────────────────────────────────────────────────
bool write(const QString &filePath, const Data &d, QString *err);
//   整份读：元数据段 + rows
bool read(const QString &filePath, Data *out, QString *err);
//   只读元数据段（收藏夹列表用）：**不解析 rows**；v1 文件会返回失败（调用方退回 `read`）
bool readMeta(const QString &filePath, Meta *out, QString *err);

} // namespace progressfile
