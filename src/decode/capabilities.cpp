#include "capabilities.h"

#include "provider_qt.h"
#include "provider_wic.h"

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
    //   rank 20：Windows 的**系统编解码器**。非 Windows 返回 nullptr ⇒ 这一路不存在。
    //   ★ 它把"用户装了 Raw / HEIF / AV1 扩展"变成"我们能解 arw / heic / avif"，
    //     而我们**不加一行格式代码**（枚举与解码全在参数化调用里）。
    if (Provider *wic = makeWicProvider()) out << wic;
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
            qCDebug(lcDecode).noquote()
                << QStringLiteral("[decode] image %1x%2 by %3 <- %4")
                       .arg(img.width()).arg(img.height()).arg(p->name(),
                                                               src.path.isEmpty() ? QStringLiteral("<member>")
                                                                                  : src.path);
            return out;
        }
        if (!err.isEmpty()) why << QStringLiteral("%1: %2").arg(p->name(), err);
    }
    //   走到这里 = 有字节但没人解得出 ⇒ **占位**（不是缺图）
    out.outcome = Outcome::Placeholder;
    out.detail = why.join(QStringLiteral("; "));
    return out;
}

//   不解码问尺寸：**第一个给出有效尺寸的就用**（与 decode 同一条链，只是问题不同）。
//   ★ 扫描期要对每个文件调一次，所以这里必须便宜：provider 的实现只读头，不解像素。
QSize Capabilities::querySize(const Source &src, int page) const
{
    ensureProbed();
    for (Provider *p : m_providers) {
        if (!p->available()) continue;
        const QSize s = p->querySize(src, page);
        if (s.isValid() && !s.isEmpty()) {
            //   debug 级：排查"扫描尺寸与解码尺寸不一致 / 占位形状怪"时唯一的一手信息
            qCDebug(lcDecode).noquote()
                << QStringLiteral("[decode] size %1x%2 by %3 <- %4")
                       .arg(s.width()).arg(s.height()).arg(p->name(),
                                                           src.path.isEmpty() ? QStringLiteral("<member>")
                                                                              : src.path);
            return s;
        }
    }
    qCDebug(lcDecode).noquote()
        << QStringLiteral("[decode] size unknown <- %1")
               .arg(src.path.isEmpty() ? QStringLiteral("<member>") : src.path);
    return {};
}

//   缩略图：同样链式回退。**"缩略图逻辑"仍然在我们这边**（要多大、什么时候要、缓存键都是
//   调用方的事）；provider 只是被允许少花点力气（默认实现就是 decode() + 我们的缩放）。
QImage Capabilities::thumbnail(const Source &src, const QSize &target, int page) const
{
    ensureProbed();
    if (!target.isValid()) return {};
    for (Provider *p : m_providers) {
        if (!p->available()) continue;
        const QImage img = p->thumbnail(src, target);
        if (!img.isNull()) return img;
    }
    return {};
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
