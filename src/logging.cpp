#include "logging.h"

#include <QByteArray>
#include <QLoggingCategory>
#include <QMutex>
#include <QSet>
#include <QString>
#include <QStringLiteral>

// ══ 分类的定义（名字见 logging.h 的声明）═════════════════════════════════════
//   ★ 第三个参数 = **该分类的默认最低级别**：即使没有人调过 `iblog::init()`
//     （例如测试套件直接链 `ibcore`、不经过 `main()`），debug 也不会漏到默认输出里。
Q_LOGGING_CATEGORY(lcApp, "ib.app", QtWarningMsg)
Q_LOGGING_CATEGORY(lcUi, "ib.ui", QtWarningMsg)
Q_LOGGING_CATEGORY(lcScan, "ib.scan", QtWarningMsg)
Q_LOGGING_CATEGORY(lcDecode, "ib.decode", QtWarningMsg)
Q_LOGGING_CATEGORY(lcGpu, "ib.gpu", QtWarningMsg)

namespace iblog {

bool levelFromName(const QString &name, Level *out)
{
    const QString n = name.trimmed().toLower();
    Level l;
    if (n == "debug") l = Level::Debug;
    else if (n == "info") l = Level::Info;
    else if (n == "warning") l = Level::Warning;
    else if (n == "error") l = Level::Error;
    else if (n == "off") l = Level::Off;
    else return false;
    if (out) *out = l;
    return true;
}

QString levelName(Level l)
{
    switch (l) {
    case Level::Debug: return QStringLiteral("debug");
    case Level::Info: return QStringLiteral("info");
    case Level::Warning: return QStringLiteral("warning");
    case Level::Error: return QStringLiteral("error");
    case Level::Off: return QStringLiteral("off");
    }
    return QStringLiteral("warning");
}

namespace {
Level g_level = Level::Warning;
}

Level level() { return g_level; }

void setLevel(Level level)
{
    g_level = level;

    //   ★ `setFilterRules()` 是**全量替换**：五个级别一律显式写清，避免沿用上一次的残留规则；
    //     规则只作用于 `ib.*`，Qt 自己的日志（`qml:` / `qt.*`）保持原有默认，照旧可见。
    //   ★ 规则里的级别名是 Qt 的**那一套**：`debug` / `info` / `warning` / **`critical`** ——
    //     `PHOTOSET_LOG=error` 对应的是规则里的 `critical`（写 `error` 会被 Qt 判成畸形规则）。
    //   ★ `off` 用「整分类关闭」（不下发级别）——它是唯一能把该分类**所有**级别都关掉的写法。
    switch (level) {
    case Level::Debug:
        QLoggingCategory::setFilterRules(QStringLiteral(
            "ib.*.debug=true\nib.*.info=true\nib.*.warning=true\nib.*.critical=true"));
        break;
    case Level::Info:
        QLoggingCategory::setFilterRules(QStringLiteral(
            "ib.*.debug=false\nib.*.info=true\nib.*.warning=true\nib.*.critical=true"));
        break;
    case Level::Warning:
        QLoggingCategory::setFilterRules(QStringLiteral(
            "ib.*.debug=false\nib.*.info=false\nib.*.warning=true\nib.*.critical=true"));
        break;
    case Level::Error:
        QLoggingCategory::setFilterRules(QStringLiteral(
            "ib.*.debug=false\nib.*.info=false\nib.*.warning=false\nib.*.critical=true"));
        break;
    case Level::Off:
        QLoggingCategory::setFilterRules(QStringLiteral("ib.*=false"));
        break;
    }

    //   生效结果用 **debug** 级回显：默认输出里不多占一行，调到 debug 时能确认它生效了
    qCDebug(lcApp).noquote()
        << QStringLiteral("[app] log level = %1").arg(iblog::levelName(level));
}

void init() { setLevel(g_level); }

// ══ 限流（见 logging.h）：一个 key 只放行第一次 ═══════════════════════════════
//   ★ 锁里**只有集合查找/插入**：不做 I/O、不调日志、不等待 —— 与 logMessage 无关，
//     不会和 Qt 自己的日志锁形成嵌套。
bool firstTime(const QString &key)
{
    static QMutex mutex;
    static QSet<QString> seen;
    const QMutexLocker locker(&mutex);
    if (seen.contains(key))
        return false;
    seen.insert(key);
    return true;
}

} // namespace iblog
