#pragma once

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QVector>

// ══ 元数据读取（EXIF + 色彩空间）—— **纯函数，无状态** ══════════════════════════
//   输入 = 一个 `Source`（磁盘文件路径 / 包内成员的整份字节）；
//   输出 = 给 OSD 的行（已过滤结构噪声、已格式化）与色彩空间判词。
//
//   ★ 交给 Exiv2 的是**来源本身**，不是"我们截出来的头"（用户 2026-10-21）：
//     - 磁盘文件给路径 ⇒ 它自己 seek，按 box/段表只读几 KB（实测 14 MB 的 HEIC 读 20 KB、30 µs）；
//     - 内存模式（先读进 QByteArray）才需要整份文件（BMFF 要用 box 尺寸校验全局），
//       所以**任何情况下都不许**把磁盘文件读全再递进去 —— 那是拿两次全读换一次面板。
//     - 包内成员本来就整份在内存里（`imageread` 的内层包单槽），给字节即可。
//
//   ★ 为什么是纯函数：这两个字段是**信息性数据**（AGENTS.md「信息性数据用 getter」），
//     不需要解码、不进任何状态；色彩空间与解码点 `toSrgb()` 用**同一个 ICC 解析器**
//     （`QColorSpace::fromIccProfile`）⇒ 判词与实际转换永远一致。
//
//   ★ EXIF **不是固定字段集**（用户 2026-10-20）：漫画页可能只有 `Software`，
//     照片有几十个标签，PNG 一个都没有 ⇒ 行由数据决定，空就整组不出现。
namespace exifmeta {

struct Source {
    QString    path;      // 磁盘文件：优先（解析器 seek，不读全文）
    QByteArray bytes;     // 包内成员：整份（已在内存，零拷贝交给 Exiv2）
};

struct Info {
    // 动态行：键 = 中文名（未映射的用 EXIF 原名），值 = Exiv2 格式化的可读值
    QVector<QPair<QString, QString>> rows;
    int     total = 0;        // 过滤前的条数（`rows.size()` 之外的省略量由它算）
    QString colorSpace;       // 人类可读判词（见 .cpp 的规则表）
};

// 解析失败（格式不认识 / 数据损坏）⇒ 空行 + 判词按"无 profile"，不抛异常
Info read(const Source &src);

}  // namespace exifmeta
