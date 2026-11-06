#pragma once

#include "imagedecoder.h"

#include <QHash>
#include <QList>
#include <QStringList>

// ══ 能力门面（**上层唯一要认识的东西**）════════════════════════════════════
//   · 替换了原来写死的 `natural::imageSuffixes()`：白名单从"代码里的字面量表"
//     变成"运行时问系统"（用户 2026-11-06 定：零编译期格式知识）。
//   · 启动时 `probe()` 一次，结果进日志；上层只问 `suffixes()` / `decode()`。
//   · **惰性探测**：任何一次 `suffixes()` / `decode()` / `report()` 都会自动先探一次
//     ⇒ 调用顺序无关（测试、工具不必记得手动 probe）。
namespace decode {

//   给日志 / 将来设置页看的一条"这台机器能解什么"
struct Report {
    QString     name;                 // provider 稳定 id
    bool        available = false;
    QStringList suffixes;
    QString     detail;               // 补充说明（格式数 / 外部程序路径 / 失败原因）
};

class Capabilities
{
public:
    static Capabilities &instance();

    //   启动探测（幂等）。`outcome` 见 `Report`。
    void probe();
    bool probed() const { return m_probed; }

    //   收录依据：所有**可用** provider 的广告后缀并集（小写、排序、去重）
    QStringList suffixes() const;
    bool        canDecodeSuffix(const QString &suffix) const;

    //   后缀 → 认领它的 provider id 列表（重叠很正常，调试用）
    QHash<QString, QStringList> owners() const;

    //   解码：按 rank 升序逐个试，**首个解出非空图即停**（一个格式多个解码器是常态）
    Result decode(const Source &src, int page = 0) const;

    //   不解码问尺寸：同样按 rank 逐个问，**第一个给出有效尺寸的就用**。
    //   ★ 这是"能力表敢列出来"的配套：列了就必须给出尺寸/像素，不能只列不解。
    QSize querySize(const Source &src, int page = 0) const;

    //   按目标尺寸出图（预览条用）：按 rank 逐个试，先成功的算。
    //   provider 可以自己优化（如 JPEG 的 DCT 缩放），默认实现是解码后我们自己的缩放。
    QImage thumbnail(const Source &src, const QSize &target, int page = 0) const;

    QList<Report> report() const;

private:
    Capabilities() = default;
    ~Capabilities();
    Capabilities(const Capabilities &) = delete;
    Capabilities &operator=(const Capabilities &) = delete;

    void ensureProbed() const;                       // 惰性探测（mutable 语义）

    mutable bool             m_probed = false;
    mutable QList<Provider *> m_providers;           // 所有权在本类
};

}  // namespace decode
