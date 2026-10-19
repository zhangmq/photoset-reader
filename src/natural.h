#pragma once

#include <QString>

// 自然排序（设计 §2.7）。**唯一实现**，目录与压缩包共用。
//   · 数字段按**数值**比较（2 < 10），前导零同值
//   · 非数字段按代码单元、大小写不敏感
//   · **不用 localeCompare**（跨平台顺序必须一致）
int naturalCompare(const QString &a, const QString &b);

// ★ 同层排序键（**唯一实现**：文件系统遍历 与 压缩包内遍历共用）：
//   **子目录按 `名字 + "/"` 比较** —— 于是 `A.jpg` 排在 `A/` **前面**
//   （用户 2026-10-19 的图集形态：上层那张封面图紧挨在它对应文件夹之前）。
//   若按纯名字比，`A` 是 `A.jpg` 的前缀 ⇒ 目录会排在封面之前（封面落到那一支的最后）。
QString nameSortKey(const QString &name, bool isDir);

// 是否是我们认得的图片扩展名（唯一实现，供目录与压缩包共用）
bool isImageSuffix(const QString &suffix);
