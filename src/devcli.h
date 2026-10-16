#pragma once

// ══ 开发/验收用 CLI 的实现（设计 14 §10）════════════════════════════════════
//   `main.cpp` 只留启动 + `QCommandLineOption` 的声明/解析；**实现全在这里**。
//   ★ 它们**不是测试**：随 `image-browser` 一起编（不进 `ibcore`、不链 `testsupport`）。
//   ★ 纪律：从 `main.cpp` 搬出来的实现**行为一字不变**（输出文案、退出码都不改）。

#include <QString>

namespace library { class Store; }
namespace app { class UiModel; }

namespace devcli {

// ── `--dev-*`：从 parser 解析出来的请求（**解析留在 `main.cpp`**，这里只承载值）──
//   ★ 用 `xxxSet` 而不是"值非空"判"给没给"：`--dev-notify ""` 这种空值也要照旧执行。
struct DevRequest
{
    bool    addSet = false;            QString add;            // --dev-add-collection <dir>
    bool    openCollectionSet = false; QString openCollection; // --dev-open-collection <row>
    bool    openPageSet = false;       QString openPage;       // --dev-open-page <n>
    bool    enterRowSet = false;       QString enterRow;       // --dev-enter-row <a,b>
    bool    notifySet = false;         QString notify;         // --dev-notify <text>

    bool any() const
    {
        return addSet || openCollectionSet || openPageSet
               || enterRowSet || notifySet;
    }
};

// `--dev-*` 全块（含开头"先把在扫的等完"）：无任何选项时**立即返回**（与旧 `if (...)` 等价）
void runDevCommands(const DevRequest &req, app::UiModel *ui, library::Store *store);

// ── 探针 / 基准 CLI（跑完即退；退出码语义照旧）──
//   ★ T2c-2b-2：旧的 `--scan-run` / `--scan-stop` / `--db-state`（双实例同步验证，
//     基于 `ScanController` + `scan_state` 列 + `action` 表）**随旧扫描路径一起删除**。
int runLibarchiveProbe(const QString &path);
int runArchiveProbe(const QString &path);

} // namespace devcli
