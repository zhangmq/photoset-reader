#include "capabilities.h"

#include "provider_qt.h"

#include "../logging.h"

#include <QLoggingCategory>
#include <QSet>

namespace decode {
namespace {

//   注册表：**按平台**装配（不是按格式）。新增一个来源 = 这里多一行，
//   `imagedecoder.h` / 上层调用点一个字都不用改。
QList<Provider *> makeProviders()
{
    QList<Provider *> out;
    out << makeQtProvider();          // rank 10：Qt 插件（含 Linux 发行版的 kimageformats）
#ifdef Q_OS_WIN
    //   阶段 B：WIC（系统已注册的全部解码器）
#endif
#ifdef Q_OS_LINUX
    //   阶段 B：gdk-pixbuf（dlopen，GNOME 栈的 loader）
#endif
    //   阶段 C：外部程序（数据驱动 profile）
    return out;
}

}  // namespace

Capabilities &Capabilities::instance()
{
    static Capabilities caps;
    return caps;
}

Capabilities::~Capabilities()
{
    qDeleteAll(m_providers);
    m_providers.clear();
}

//   惰性探测：**第一次用到才探**，之后进程内不再探
//   （用户 2026-11-06 确定要在启动时检查 ⇒ `main.cpp` 会显式调一次，这里只是兜底）
void Capabilities::ensureProbed() const
{
    if (m_probed) return;
    m_probed = true;

    m_providers = makeProviders();
    for (Provider *p : m_providers) p->probe();

    //   `[cap]` 报告：**这台机器到底能解什么**，一条 provider 一行。
    //   ★ info 级：只在启动时几行，不许刷屏。
    int total = 0;
    QSet<QString> unionOf;
    for (const Provider *p : m_providers) {
        const QStringList sfx = p->advertisedSuffixes();
        for (const QString &s : sfx) unionOf.insert(s);
        qCInfo(lcDecode).noquote()
            << QStringLiteral("[cap] %1%2: %3 suffixes%4")
                   .arg(p->name(), -5)
                   .arg(p->available() ? QString() : QStringLiteral(" (unavailable)"))
                   .arg(sfx.size())
                   .arg(sfx.isEmpty() ? QString()
                                      : QStringLiteral(" (%1)").arg(sfx.join(QLatin1Char(' '))));
        if (p->available()) total += sfx.size();
    }
    qCInfo(lcDecode).noquote()
        << QStringLiteral("[cap] total : %1 suffixes from %2 providers")
               .arg(unionOf.size()).arg(m_providers.size());
}

void Capabilities::probe()
{
    ensureProbed();
}

QStringList Capabilities::suffixes() const
{
    ensureProbed();
    QStringList out;
    for (const Provider *p : m_providers) {
        if (!p->available()) continue;
        for (const QString &s : p->advertisedSuffixes()) out << s;
    }
    out.removeDuplicates();
    out.sort();
    return out;
}

bool Capabilities::canDecodeSuffix(const QString &suffix) const
{
    ensureProbed();
    const QString s = suffix.trimmed().toLower();
    if (s.isEmpty()) return false;
    for (const Provider *p : m_providers) {
        if (!p->available()) continue;
        if (p->advertisedSuffixes().contains(s)) return true;
    }
    return false;
}

QHash<QString, QStringList> Capabilities::owners() const
{
    ensureProbed();
    QHash<QString, QStringList> out;
    for (const Provider *p : m_providers) {
        if (!p->available()) continue;
        for (const QString &s : p->advertisedSuffixes()) out[s] << p->name();
    }
    return out;
}

//   ★ 链式回退：provider 之间按 rank 升序，**首个解出非空图即停**。
//     "一个格式多个解码器"是常态（Windows 上一个 ARW 可能被好几个 WIC 包认领），
//     而且"声明支持某后缀"≠"真能解出图"，所以只能逐个试、不能选一个用到底。
Result Capabilities::decode(const Source &src, int page) const
{
    ensureProbed();

    Result out;
    QStringList why;
    for (Provider *p : m_providers) {
        if (!p->available()) continue;
        QString err;
        const QImage img = p->decode(src, page, &err);
        if (!img.isNull()) {
            out.outcome = Outcome::Image;
            out.image = img;
            out.originalSize = img.size();     // 解码器没报时以像素为准
            out.provider = p->name();
            return out;
        }
        if (!err.isEmpty()) why << QStringLiteral("%1: %2").arg(p->name(), err);
    }
    //   走到这里 = 有字节但没人解得出 ⇒ **占位**（不是缺图）
    out.outcome = Outcome::Placeholder;
    out.detail = why.join(QStringLiteral("; "));
    return out;
}

QList<Report> Capabilities::report() const
{
    ensureProbed();
    QList<Report> out;
    for (const Provider *p : m_providers) {
        Report r;
        r.name = p->name();
        r.available = p->available();
        r.suffixes = p->advertisedSuffixes();
        r.detail = QStringLiteral("%1 suffixes").arg(r.suffixes.size());
        out << r;
    }
    return out;
}

}  // namespace decode
