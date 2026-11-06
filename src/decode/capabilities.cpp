#include "capabilities.h"

#include "provider_cli.h"
#include "provider_gdk.h"
#include "provider_qt.h"
#include "provider_wic.h"

#include "../logging.h"

#include <QLoggingCategory>
#include <QSet>

namespace decode {
namespace {

//   ★★ **产品策略性排除**（不是能力声明，**只做减法**）：用户 2026-11-06 定 ——
//     **PDF 不做**："首先我们目标是看图，PDF 以什么形式接进来都别扭 / 有更合适的工具"。
//     有些发行版的 Qt 插件（`libqpdf.so`）能解 PDF，能力探测会把 `pdf` 广告进来；
//     但一个"没有页语义、只有第一页"的东西混在图集里比不支持更糟 ⇒ 在这里减掉。
//     ⚠️ 减掉的只是**收录与显示**：用户显式打开一个 PDF 仍会进解码链（那是他的选择）。
bool excludedSuffix(const QString &suffix)
{
    static const QSet<QString> denied = {QStringLiteral("pdf"), QStringLiteral("pdd")};
    return denied.contains(suffix);
}

//   注册表：**按平台**装配（不是按格式）。新增一个来源 = 这里多一行，
//   `imagedecoder.h` / 上层调用点一个字都不用改。
QList<Provider *> makeProviders()
{
    QList<Provider *> out;
    out << makeQtProvider();          // rank 10：Qt 插件（含 Linux 发行版的 kimageformats）
    //   rank 20：系统编解码器。非对应平台返回 nullptr（或 available()==false）。
    if (Provider *wic = makeWicProvider()) out << wic;   // Windows
    if (Provider *gdk = makeGdkProvider()) out << gdk;   // Linux（gdk-pixbuf）
    //   rank 90：外部程序，**永远最后**（能自己解就不起子进程）。
    //   默认 profile 的 suffixes 为空 ⇒ 只兜底、不往文件列表里塞东西。
    out << makeCliProvider();
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

//   没探过就被用 ⇒ 空表 + 一条 warning。**不替调用方探测**：
//   探测会建 COM / 起子进程 / dlopen，必须发生在**启动时那个确定的线程**上
//   （WIC 那次把 GUI 线程钉成 MTA、导致"打开文件夹卡死"，就是从这条捷径来的）。
void Capabilities::warnIfNotProbed() const
{
    if (m_probed) return;
    static bool warned = false;
    if (warned) return;
    warned = true;
    qCWarning(lcDecode).noquote()
        << QStringLiteral("[cap] 能力表还没探测就被用了（能力表按空处理）—— 调用方漏了 "
                          "`Capabilities::instance().probe()`；它只该在启动时调一次");
}

void Capabilities::probe()
{
    if (m_probed) return;              // 幂等

    m_providers = makeProviders();
    for (Provider *p : m_providers) p->probe();

    //   ── 把结果冻成快照（之后全部是纯读）──────────────────────────────
    QSet<QString> unionOf;
    QSet<QString> denied;
    for (const Provider *p : m_providers) {
        if (!p->available()) continue;
        for (const QString &s : p->advertisedSuffixes()) {
            if (excludedSuffix(s)) {
                denied.insert(s);
                continue;
            }
            unionOf.insert(s);
            m_owners[s] << p->name();
        }
    }
    m_suffixes = QStringList(unionOf.cbegin(), unionOf.cend());
    m_suffixes.sort();
    m_denied = QStringList(denied.cbegin(), denied.cend());
    m_denied.sort();

    //   `[cap]` 报告：**这台机器到底能解什么**，一条 provider 一行 + 一行总计。
    //   ★ info 级：只在启动时几行，不许刷屏。
    for (const Provider *p : m_providers) {
        const QStringList sfx = p->advertisedSuffixes();
        qCInfo(lcDecode).noquote()
            << QStringLiteral("[cap] %1%2: %3 suffixes%4")
                   .arg(p->name(), -5)
                   .arg(p->available() ? QString() : QStringLiteral(" (unavailable)"))
                   .arg(sfx.size())
                   .arg(sfx.isEmpty() ? QString()
                                      : QStringLiteral(" (%1)").arg(sfx.join(QLatin1Char(' '))));
    }
    //   ★ 把"被策略排除的"写出来：否则看日志的人会以为"这台机器解不了 pdf"，
    //     而不是"我们不做 pdf"。
    qCInfo(lcDecode).noquote()
        << QStringLiteral("[cap] total : %1 suffixes from %2 providers%3")
               .arg(m_suffixes.size()).arg(m_providers.size())
               .arg(m_denied.isEmpty() ? QString()
                                       : QStringLiteral(" (excluded by policy: %1)")
                                             .arg(m_denied.join(QLatin1Char(' '))));

    m_probed = true;                   // **最后**才置位
}

QStringList Capabilities::suffixes() const
{
    warnIfNotProbed();
    return m_suffixes;
}

bool Capabilities::canDecodeSuffix(const QString &suffix) const
{
    warnIfNotProbed();
    if (suffix.isEmpty()) return false;
    return m_suffixes.contains(suffix.trimmed().toLower());
}

QHash<QString, QStringList> Capabilities::owners() const
{
    warnIfNotProbed();
    return m_owners;
}

//   ★ 链式回退：provider 之间按 rank 升序，**首个解出非空图即停**。
//     "一个格式多个解码器"是常态（Windows 上一个 ARW 可能被好几个 WIC 包认领），
//     而且"声明支持某后缀"≠"真能解出图"，所以只能逐个试、不能选一个用到底。
Result Capabilities::decode(const Source &src, int page) const
{
    warnIfNotProbed();

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
    warnIfNotProbed();
    for (Provider *p : m_providers) {
        if (!p->available()) continue;
        const QSize s = p->querySize(src, page);
        if (s.isValid() && !s.isEmpty()) {
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
    warnIfNotProbed();
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
    warnIfNotProbed();
    QList<Report> out;
    for (const Provider *p : m_providers) {
        Report r;
        r.name = p->name();
        r.available = p->available();
        for (const QString &s : p->advertisedSuffixes())
            if (!excludedSuffix(s)) r.suffixes << s;
        r.detail = QStringLiteral("%1 suffixes").arg(r.suffixes.size());
        out << r;
    }
    return out;
}

}  // namespace decode
