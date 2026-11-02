#pragma once

// ══ 开发 / 验收用的 CLI（设计 14 §10）════════════════════════════════════════
//   `main.cpp` 只留启动 + `QCommandLineOption` 的声明/解析；**实现全在这里**。
//   ★ 它们**不是测试**：随 `photoset-reader` 一起编（不进 `ibcore`、不链 `testsupport`）。
//
//   2026-10-31：随「浏览 / 图集库」删除，`--dev-add-collection` / `--dev-open-collection` /
//   `--dev-open-page` / `--dev-enter-row` 一并删掉（它们全部建在那两个界面上）；
//   只剩 `--dev-notify`（验收浮层）与两个探针（压缩包 / libarchive 能力）。

#include <QString>

namespace app { class UiModel; }

namespace devcli {

// ── `--dev-*`：从 parser 解析出来的请求（**解析留在 `main.cpp`**，这里只承载值）──
//   ★ 用 `xxxSet` 而不是"值非空"判"给没给"：`--dev-notify ""` 这种空值也要照旧执行。
struct DevRequest
{
    bool    notifySet = false;         QString notify;         // --dev-notify <text>

    bool any() const { return notifySet; }
};

// `--dev-*` 全块：无任何选项时**立即返回**
void runDevCommands(const DevRequest &req, app::UiModel *ui);

// ── 探针 CLI（跑完即退；退出码语义照旧）──
int runLibarchiveProbe(const QString &path);
int runArchiveProbe(const QString &path);

// ── 幻灯片吞吐量测（`--bench-slideshow <ms> --bench-seconds <s>`）────────────────
//   量的是"间隔到底被什么卡住"：把间隔设成 N ms、跑 S 秒，统计真正翻了多少屏、
//   每屏间隔的 min/median/max。跑完打一行摘要并退出。
//   ★ 只用于验收/调参：`--bench-*` 不给时完全不参与运行。
void runSlideshowBench(app::UiModel *ui, int intervalMs, int seconds);

} // namespace devcli
