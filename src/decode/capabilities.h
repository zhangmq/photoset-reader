#pragma once

#include "imagedecoder.h"

#include <QHash>
#include <QList>
#include <QStringList>

// ══ 能力门面（**上层唯一要认识的东西**）════════════════════════════════════
//   · 替换了原来写死的 `natural::imageSuffixes()`：白名单从"代码里的字面量表"
//     变成"运行时问系统"（用户 2026-11-06 定：零编译期格式知识）。
//   · ★★ **探测只在启动时做一次，而且必须显式调**（用户 2026-11-06：
//     "要检查格式支持，应该应用启动时就进行，不需要每次打开文件夹都检查"）。
//     正常入口：`main.cpp`、`tests/testsupport.cpp` 的 `suiteMain()`、`tools/` 的各自 main。
//   · 探完之后**全是纯读**：结果冻结成快照（后缀集合 / owners / provider 列表），
//     没有惰性探测、没有副作用、没有锁 —— 多线程并发读是安全的。
//   · 没探过就被调用 ⇒ 能力表为空 + **一条 warning**（那是代码 bug：调用方漏了 probe，
//     不该由这里替它补，更不该在随便哪个线程上补 —— 见 `provider_wic.cpp` 里那条事故注释）。
namespace decode {

//   给日志 / 将来设置页看的一条"这台机器能解什么"
struct Report {
    QString     name;                 // provider 稳定 id
    bool        available = false;
    QStringList suffixes;
    QString     detail;               // 补充说明（格式数 / 失败原因）
    //   ★ 2026-11-07（用户）：**具体用什么解的** —— `.so` / `.dll` / 外部程序路径。
    //     内置那种（Qt 自带）为空串：用户定"内置不描述具体组件"。
    QString     component;
};

class Capabilities
{
public:
    static Capabilities &instance();

    //   启动探测：跑一遍所有 provider，把结果冻成快照。**幂等**（已探过直接返回）。
    void probe();
    bool probed() const { return m_probed; }

    //   以下全部是**快照读取**（无副作用）
    QStringList suffixes() const;                    // 白名单：去重、排序、策略排除后
    bool        canDecodeSuffix(const QString &suffix) const;
    QHash<QString, QStringList> owners() const;      // 后缀 → 认领它的 provider id

    //   解码：按 rank 升序逐个试，**首个解出非空图即停**（一个格式多个解码器是常态）
    Result decode(const Source &src, int page = 0) const;
    QSize  querySize(const Source &src, int page = 0) const;
    QImage thumbnail(const Source &src, const QSize &target, int page = 0) const;

    QList<Report> report() const;

private:
    Capabilities() = default;
    ~Capabilities();
    Capabilities(const Capabilities &) = delete;
    Capabilities &operator=(const Capabilities &) = delete;

    void warnIfNotProbed() const;                    // 只报一次，指路"调用方漏了 probe"

    bool             m_probed = false;
    QList<Provider *> m_providers;                   // 所有权在本类
    QStringList      m_suffixes;                     // 快照：对外可见的后缀（已排除策略性）
    QHash<QString, QStringList> m_owners;            // 快照：后缀 → provider 列表
    QStringList      m_denied;                       // 快照：被策略排除的（只给日志用）
};

}  // namespace decode
