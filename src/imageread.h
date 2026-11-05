#pragma once

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>
#include <QStringList>

// ══ 条目读取（**唯一实现**：QImageReader）══════════════════════════════════
//   「图在哪里、怎么读」全是本模块的知识（用户 2026-10-21 定案）：
//     · `Sequence` **只调用**本模块拿尺寸 / 页数，自己**不碰 `QImageReader` / `QImage`**；
//     · 查看屏的解码（`fetch::ViewerModel` 的工作线程）同样只调这里；
//     · OSD 的 EXIF / ICC 不走这里截头字节：磁盘文件把**路径**交给解析器（HEIF 由 libheif
//       自己 seek），只有包内成员才把整份字节递过去（`Probe::memberBytes`）—— 见 exifmeta.h。
//
//   容器解析 = 「沿链读字节」：从盘上最外层的压缩包逐层下钻（详见 imageread.cpp 的注释）；
//   内层包有**进程级单槽物化**，翻页 / 预取 / 缩略图反复读都复用它。
namespace imageread {

// 内层压缩包上限的默认值（MB）—— 与 `AppSettings` 的默认一致（512）
inline constexpr qint64 kDefaultInnerArchiveMaxMb = 512;

// ★★ 条目的**读取位置**（沿链解析）
//   · `diskPath` = 链上最外层容器的绝对路径；无容器 = 图片本身的路径
//   · `members`  = 容器成员链（长度 = 容器层数）；空 = 直接在盘上读
//   · `innerArchiveMaxMb` = 读这条时用的内层包上限（**随条目带**：读取方法拿不到序列实例）
struct Location {
    QString     diskPath;
    QStringList members;
    int         page = 0;
    qint64      innerArchiveMaxMb = kDefaultInnerArchiveMaxMb;
    //   ★ **每层容器的密码**（2026-11-03 加密压缩包）：与 `members` 对齐 ——
    //     `passwords[0]` = 最外层容器（`diskPath`），`passwords[i]` = 第 i 层嵌套容器
    //     （即 `members[i-1]`）；最后一项（叶子本身）没用。
    //     空串 = 该层不需要密码。**随条目带**（与 `innerArchiveMaxMb` 同一个先例）。
    QStringList passwords;
};

// 整条字节（解码用）。空 = 读不到（容器打不开 / 成员不存在 / 超过内层包上限）
//   ★ `passwordLevel`（可选，2026-11-03）：读不到且原因是**要密码**时，带出层号
//     （0 = 最外层磁盘容器，i = 第 i 层嵌套容器）；-1 = 不是密码原因。
QByteArray readLocation(const Location &loc, int *passwordLevel = nullptr);

// 条目探测（**一次拿全**，不解码）：容器格式 + 显示尺寸 + 页数 + 字节数
//   ★ 给 OSD 用：磁盘文件不必取字节（解析器自己 seek）；包内成员交出整份（已在内存）
struct Probe {
    QByteArray memberBytes;   // 包内成员：整份字节（磁盘文件为空）—— 交给 `exifmeta`
    QString    format;        // "jpeg" / "png" / …（QImageReader 报的，空 = 读不到）
    QSize      size;          // **显示尺寸**（已按 EXIF 旋转）
    bool       rotated = false;
    int        pages = 1;
    qint64     bytes = -1;    // 文件大小 / 成员大小
};
Probe probe(const Location &loc);

// ★ 解码（**唯一实现**：QImageReader + 沿链读字节）。返回值把"读不到"分成三类，
//   供查看屏决定「缺 / 等密码 / 画占位图」：
//   · `passwordLevel >= 0`            ⇒ 要密码（层号；0 = 最外层）
//   · `hasBytes == false`             ⇒ **没有字节可读**（文件不在 / 容器打不开 / 成员不存在）⇒ 缺
//   · `hasBytes == true && 图是空的`  ⇒ 字节在、解码器给不出图（截断 / 坏编码）
//     ⇒ 由调用方画占位图（**不算缺**：不许跳屏、不许重试）
//   ★ 以前只返回 `QImage` ⇒ 上面两种"没图"分不开，只能一律当"坏"记下来并跳屏。
struct Loaded {
    QImage image;
    int    passwordLevel = -1;
    bool   hasBytes      = false;
};
Loaded load(const Location &loc);

// 不解码读像素尺寸（扫描判宽窄用）
QSize imageSize(const Location &loc);
//   ★ 带出"密码层号"的版本（扫描期读图头失败 ⇒ 立刻要密码，设计 22 §5.1）
QSize imageSize(const Location &loc, int *passwordLevel);
//   ★ 缩略图用途：按目标尺寸**降采样解码**（`QImageReader::setScaledSize`，JPEG 走 shrink-on-load）。
//     只给时间轴悬停预览这种"小图"用；主显示路径仍解码全图再走我们的缩放层（格式纪律）。
//     返回空 = 读不到（容器打不开 / 成员不存在 / 密码不对）。
QImage imageScaled(const Location &loc, const QSize &target);

// 一张源图有几页（多页 TIFF 等）；1 = 普通单页图
int pageCount(const QString &path);

// 释放内层包单槽（序列换来源时调用；线程无关 ⇒ 进程内那一份就是它）
void releaseInnerSlot();

}  // namespace imageread
