#pragma once

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QVector>

// ══ 元数据读取（EXIF + 色彩空间）—— **纯函数，无状态** ══════════════════════════
//   输入 = 一个 `Source`（磁盘文件路径 / 包内成员的整份字节）与 `QImageReader::format()`；
//   输出 = 给 OSD 的行（已过滤结构噪声、已格式化）与色彩空间判词。
//
//   ★ 为什么给"来源"而不是"我们截好的头字节"：
//     - 普通位图（JPEG/PNG/WebP/TIFF）：容器定位只看前 256 KB —— 模块内部自己读；
//     - **HEIC/AVIF（BMFF 容器）**：Exif 块躺在 `mdat` 里，由 `meta` 的 `iinf`/`iloc` 索引指向，
//       不是文件头里的一段 ⇒ 交给 **libheif（LGPL-3.0）**：磁盘文件**按路径**打开（它自己 seek，
//       不读全文），包内成员给整份字节（本来就在内存）。
//
//   ★ 为什么是纯函数：这两个字段是**信息性数据**（AGENTS.md「信息性数据用 getter」），
//     不需要解码、不进任何状态；色彩空间与解码点 `toSrgb()` 用**同一个 ICC 解析器**
//     （`QColorSpace::fromIccProfile`）⇒ 判词与实际转换永远一致。
//
//   ★ EXIF **不是固定字段集**（用户 2026-10-20）：漫画页可能只有 `Software`，
//     照片有几十个标签，PNG 一个都没有 ⇒ 行由数据决定，空就整组不出现。
namespace exifmeta {

struct Source {
    QString    path;      // 磁盘文件：优先（HEIF 交给 libheif 按路径 seek）
    QByteArray bytes;     // 包内成员：整份字节（已在内存）
};

struct Info {
    // 动态行：键 = 中文名（未映射的用 EXIF 原名），值 = libexif 格式化的可读值
    QVector<QPair<QString, QString>> rows;
    int     total = 0;        // 过滤前的条数（`rows.size()` 之外的省略量由它算）
    QString colorSpace;       // 人类可读判词（见 .cpp 的规则表）
};

// `format` = QImageReader 报的格式，仅作嗅探失败时的兜底（可空）
Info read(const Source &src, const QString &format = QString());

}  // namespace exifmeta
