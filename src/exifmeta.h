#pragma once

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QVector>

// ══ 元数据读取（EXIF + 色彩空间）—— **纯函数，无状态** ══════════════════════════
//   输入 = 条目的**头字节**（`imageread::headBytes`，最多 256 KB）与 `QImageReader::format()`；
//   输出 = 给 OSD 的行（已过滤结构噪声、已格式化）与色彩空间判词。
//
//   ★ 为什么只吃字节：这两个字段是**信息性数据**（AGENTS.md「信息性数据用 getter」），
//     不需要解码、不进任何状态；色彩空间与解码点 `toSrgb()` 用**同一个 ICC 解析器**
//     （`QColorSpace::fromIccProfile`）⇒ 判词与实际转换永远一致。
//
//   ★ EXIF **不是固定字段集**（用户 2026-10-20）：漫画页可能只有 `Software`，
//     照片有几十个标签，PNG 一个都没有 ⇒ 行由数据决定，空就整组不出现。
namespace exifmeta {

struct Info {
    // 动态行：键 = 中文名（未映射的用 EXIF 原名），值 = libexif 格式化的可读值
    QVector<QPair<QString, QString>> rows;
    int     total = 0;        // 过滤前的条数（`rows.size()` 之外的省略量由它算）
    QString colorSpace;       // 人类可读判词（见 .cpp 的规则表）
};

// `head` = 前若干 KB；`format` = QImageReader 报的格式（可空，模块自己也会嗅探容器）
Info read(const QByteArray &head, const QString &format);

}  // namespace exifmeta
