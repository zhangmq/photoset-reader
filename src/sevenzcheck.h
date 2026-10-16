#pragma once

#include <QString>

// ══ 7z/cb7：能不能**随机读**（= 是不是非 solid）═══════════════════════════
//
//   用户 2026-10-13 定的原则：**一切可以随机读的压缩包都支持**；
//   solid 的"不属于图库压缩包"（它无法随机读取）⇒ 不支持。
//
//   为什么需要自己解析：libarchive **不暴露**块结构，实测 `archive_seek_data`
//   对所有格式都返回 -25（连 tar 都是）⇒ 拿不到"能不能随机读"这个信息。
//
//   判定依据（7z 格式）：**一个"文件夹"(folder/块) 里装了多个文件 ⇒ solid** ✗。
//     · 非 solid（每个文件各自一个块）⇒ 跳过一条只需走过头部 ⇒ 可随机读 ✓
//     · 实测差 30 倍：非 solid 全扫 117 条 1.8s ✓ / solid 48.9s ✗
//
//   ★ 拿不准就返回 **false**（按"不能随机读"处理）—— 宁可少支持，不要卡死用户。
bool sevenZipIsRandomAccessible(const QString &path, QString *why);
