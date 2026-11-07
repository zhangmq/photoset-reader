#pragma once

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>
#include <QStringList>

// ══ 解码能力的统一接口（设计 `docs/design/decode-capability.md`）══════════════
//   ★★ **本目录的文件里不出现任何格式名**。「谁能解什么」全部由 provider 在
//      `probe()` 里**运行时问出来**：Qt 插件列表 / 系统编解码器枚举 / 外部程序探测。
//      ⇒ 用户机器上多装一个编解码器包，能力自动出现，**不用改我们一行代码**。
//
//   分工（不要混）：
//     · `imageread`  = 字节从哪来（磁盘 / 包内成员 / 密码 / 内层包单槽）
//     · `decode`     = 字节 → QImage
//     · 缩放 / 缩略图 / 方向 / ICC 一律在外层（这边只管解码）
namespace decode {

//   一次解码的结论 = 产品上的三态 + 密码。**判据只有两个**：
//   "读到字节了吗" 与 "拿到非空 QImage 了吗" —— 与格式、与 provider 都无关。
//   · `Image`       拿到图。**完整也好、残缺也好，一律"有图"**：
//                   JPEG 内容坏掉时解码器照样给非空图（2026-11-06 实测：截断 40% /
//                   随机翻字节都是非空 + `error()` 报 Unknown，所以**残图不需要检测**）。
//   · `Placeholder` 后缀在能力表里，但链上所有 provider 都解不出 ⇒ 画占位图。
//   · `NoBytes`     读不到字节（文件没了 / 容器打不开 / 成员不在）⇒ 缺图。
//   · `Password`    读不到，且原因是密码（`passwordLevel` = 层号）。
enum class Outcome { Image, Placeholder, NoBytes, Password };

//   字节从哪来 —— provider 不碰 `imageread`（避免循环依赖）
struct Source {
    QString    path;                 // 非空 ⇒ 磁盘文件，可按路径交给系统解码器 / 外部程序
    QByteArray bytes;                // 包内成员：整份字节
    bool       isDisk = false;
};

struct Result {
    Outcome outcome = Outcome::NoBytes;
    QImage  image;                   // 仅 `Outcome::Image`
    QSize   originalSize;            // 解码器报的原始尺寸（我们缩放之前）
    QString provider;                // 成功者 id（"qt:jpeg"）—— 进预览缓存键
    int     passwordLevel = -1;
    QString detail;                  // 全失败时各 provider 的原因串（进日志）
};

class Provider
{
public:
    virtual ~Provider() = default;

    virtual QString     name() const = 0;               // 稳定 id（进缓存键）
    //   人可读的**具体实现**：用哪个 .so / .dll / 外部程序来解（进"解码支持"页与 `[cap]` 日志）。
    //   · 内置（Qt 自带那批）：返回空 —— 用户 2026-11-07 定"内置不描述具体组件"；
    //   · 系统编解码器 / dlopen 的库 / 外部程序：返回文件名或路径（多个用 ", " 连接）。
    virtual QString     component() const { return QString(); }
    virtual int         rank() const = 0;               // 小者先试
    virtual void        probe() = 0;                    // 启动一次（幂等）
    virtual bool        available() const = 0;
    virtual QStringList advertisedSuffixes() const = 0; // ★ 只影响"收录"，不影响解码

    //   ★ 唯一解码入口。返回空 QImage = 我失败了，交给下一个 provider。
    //     `page` 是多页图（多页 TIFF）的页号，0 = 首页。
    virtual QImage decode(const Source &src, int page, QString *why) = 0;

    //   可选：不解码问尺寸（扫描 / OSD 用）。拿不到 ⇒ 返回无效尺寸。
    virtual QSize querySize(const Source &src, int page, QString *fmt = nullptr,
                            int *frames = nullptr);

    //   可选：按目标尺寸出图。**默认实现 = `decode()` + 我们自己的缩放**。
    //   ★ 缩略图逻辑永远在这边（谁要多大 / 什么时候要 / 缓存键都是我们的）；
    //     provider 只是被允许"顺便少花点力气"（如 JPEG 的 DCT 缩放）。
    virtual QImage thumbnail(const Source &src, const QSize &target);
};

}  // namespace decode
