#pragma once

#include "imagedecoder.h"

//   外部程序这一路（`rank 90`，**永远最后**）。
//   ★ 存在理由：有些格式**系统里根本没有编解码器**（Windows 上的 JXL / QOI / PSD / XCF /
//     EXR / DDS…），但用户很可能已经装了 ImageMagick / vips。这一路把"用户装了什么外部
//     程序"变成"我们能解什么"，而且**跑在子进程里** —— 它崩了、卡死、吃爆内存都伤不到我们。
//
//   ★ **数据驱动**：程序名与参数模板是**数据**（内置默认 + 用户 `decode-tools.json` 覆盖），
//     二进制里没有格式名。默认 profile 的 `suffixes` 是**空的** ⇒ 它只在链上兜底，
//     不会往文件列表里塞东西（否则 `.txt` 也会被列出来）。
namespace decode {
Provider *makeCliProvider();
}
