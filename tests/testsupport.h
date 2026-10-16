#pragma once

// ══ 测试脚手架（设计 14 §11）：各套件**共用**的入口 / check / waitFor / 夹具 ══════
//   · 只放**被多个套件复用**的东西；套件自身的行为留在各自的 `selftest_*.cpp`。
//   · 纪律：搬进来的代码**行为一字不变**（输出文案、退出码语义都不许改）。
//   · 依赖方向：这里只依赖 core（sequence / viewermodel），**不碰** uimodel / QML。

#include <QApplication>
#include <QDebug>
#include <QImage>
#include <QSize>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>

#include <functional>
#include <memory>

#include "sequence.h"

namespace testsupport {

// ══ 套件入口：与旧的 `image-browser --selftest-*` 路径同一形态 ══════════════
//   QApplication + 应用名/版本；`argv` 直接透传给 Qt（`-platform offscreen` 等照常可用）。
//   每个套件仍有自己的 `main()`，只是把入口样板收在这里（设计 14 §11）。
int suiteMain(int argc, char *argv[], const std::function<int()> &suite);

// ══ 需要素材目录的套件：从 argv 取 `-d <目录>`（与旧 CLI **同一形状**）════════
//   设计 14 §11："需要夹具的用 `argv`（如 `ib-test-flow -d .testdata/vol01`）"。
//   只认 `-d` / `--dir`；没给 ⇒ 空串（与旧 `QCommandLineOption` 的默认值一致）。
QString dirArg(int argc, char *argv[]);

// ══ 跳过（夹具 / 环境不具备，**一律不算通过**）══════════════════════════════
//   设计 14 §11 T0 收尾批（2026-10-14）：三种退出码各管一件事 ——
//     pass ⇒ 0 / fail ⇒ 1 / skip ⇒ 77（`tests/CMakeLists.txt` 配 `SKIP_RETURN_CODE 77`
//     ⇒ ctest 显示 *Skipped* 而不是 *Passed*）。
//   用法分两层：
//     · **整条套件跳过**（一条检查都没做）⇒ `return testsupport::skip(tag, 原因)`；
//     · 套件内部个别检查跳过 ⇒ 打印 `[tag] 跳过：<原因>` 且**不计入项数**（绝不当 PASS 计）。
constexpr int kSkipExitCode = 77;

// 打印 `[tag] 跳过：<原因>` 并返回 `kSkipExitCode`（统一跳过日志 + 退出码的唯一入口）
inline int skip(const QString &tag, const QString &reason)
{
    qInfo().noquote() << QStringLiteral("[%1] 跳过：%2").arg(tag, reason);
    return kSkipExitCode;
}

// ══ 轮询到条件成立（**不用固定等待**）：收下驱动线程 post 过来的状态 ══════════
//   新 VM 是**异步**的：GUI 只投递请求，状态由**驱动线程 post 回来**（设计 13 §十二 修正）。
//   所以断言前要"收下 post" —— 这是对**可观测行为**的等待，**不是给产品留接口**。
//   `timeoutMs` 只是**上限**（超时即条件不成立 ⇒ FAIL），正常路径远快于它。
bool waitFor(const std::function<bool()> &done, int timeoutMs = 3000);

// ══ 套件断言器：`check(ok, 说明)` ⇒ `[tag] PASS/FAIL 说明`；`finish()` ⇒ 小结 ══
//   小结仍形如 `[tag] 全部通过（N 项）` / `[tag] 有 N 项失败（N 项）`，退出码 0/1 不变。
class Check
{
public:
    explicit Check(const QString &tag) : m_tag(tag) {}

    void operator()(bool ok, const QString &what)
    {
        ++m_checks;
        if (!ok) ++m_failures;
        qInfo().noquote() << QStringLiteral("[%1] %2 %3")
                                 .arg(m_tag)
                                 .arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what);
    }

    int checks() const { return m_checks; }
    int failures() const { return m_failures; }

    int finish() const
    {
        qInfo().noquote() << QStringLiteral("[%1] %2（%3 项）")
                                 .arg(m_tag)
                                 .arg(m_failures == 0
                                          ? QStringLiteral("全部通过")
                                          : QStringLiteral("有 %1 项失败").arg(m_failures))
                                 .arg(m_checks);
        return m_failures == 0 ? 0 : 1;
    }

private:
    QString m_tag;
    int m_checks = 0;
    int m_failures = 0;
};

// ══ 夹具：临时库 / 临时设置（**绝不碰用户真库 / 真配置**）═══════════════════
//   `PHOTOSET_LIBRARY_DB` / `PHOTOSET_SETTINGS_FILE` 指到本次临时目录下的文件。
class TempFixture
{
public:
    bool isValid() const { return m_dir.isValid(); }
    QString path() const { return m_dir.path(); }
    QString filePath(const QString &name) const { return m_dir.filePath(name); }

    QString useLibraryDb(const QString &name = QStringLiteral("library.db"));
    QString useSettingsFile(const QString &name = QStringLiteral("settings.json"));

private:
    QTemporaryDir m_dir;
};

} // namespace testsupport
