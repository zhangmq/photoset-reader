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
//     · OSD 要的 EXIF / ICC 也从这里取**头字节**（`headBytes`），不经解码。
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
};

// 整条字节（解码用）。空 = 读不到（容器打不开 / 成员不存在 / 超过内层包上限）
QByteArray readLocation(const Location &loc);

// 头 `maxBytes` 字节（**不解码**；EXIF / ICC 这类信息性数据用它 —— 普通文件只读这一截，
//   包内成员本来就要解出整条，再截断）
QByteArray headBytes(const Location &loc, int maxBytes);

// 条目探测（**一次拿全**，不解码）：头字节 + 容器格式 + 显示尺寸 + 页数 + 字节数
//   ★ 给 OSD 用：`head` 交 `exifmeta`，其余是"图像"那组的行；一次取，不重复开容器
struct Probe {
    QByteArray head;
    QString    format;        // "jpeg" / "png" / …（QImageReader 报的，空 = 读不到）
    QSize      size;          // **显示尺寸**（已按 EXIF 旋转）
    bool       rotated = false;
    int        pages = 1;
    qint64     bytes = -1;    // 文件大小 / 成员大小
};
Probe probe(const Location &loc, int headMax);

// 解码（**唯一实现**）。空图 = 不可读
QImage image(const Location &loc);

// 不解码读像素尺寸（扫描判宽窄用）
QSize imageSize(const Location &loc);

// 一张源图有几页（多页 TIFF 等）；1 = 普通单页图
int pageCount(const QString &path);

// 释放内层包单槽（序列换来源时调用；线程无关 ⇒ 进程内那一份就是它）
void releaseInnerSlot();

}  // namespace imageread
