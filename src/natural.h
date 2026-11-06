#pragma once

#include <QString>
#include <QStringList>

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

// ★ 这两条**不再持有格式表**，只是 `decode::Capabilities` 的转发（2026-11-06）：
//   "能解哪些格式"由 provider 在启动时运行时探测（Qt 插件 / 系统编解码器 / 外部程序），
//   这里只负责"这条路径要不要按图片收"的判断入口，避免 6 个调用点各写一份。
bool isImageSuffix(const QString &suffix);

// 本机能解的图片扩展名列表（扫描与文件对话框过滤器共用同一份，别在别处再抄一遍）
QStringList imageSuffixes();
