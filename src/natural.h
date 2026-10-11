#pragma once

#include <QString>

// 自然排序（设计 §2.7）。**唯一实现**，目录与压缩包共用。
//   · 数字段按**数值**比较（2 < 10），前导零同值
//   · 非数字段按代码单元、大小写不敏感
//   · **不用 localeCompare**（跨平台顺序必须一致）
int naturalCompare(const QString &a, const QString &b);

// 是否是我们认得的图片扩展名（唯一实现，供目录与压缩包共用）
bool isImageSuffix(const QString &suffix);
