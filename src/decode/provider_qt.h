#pragma once

#include "imagedecoder.h"

//   Qt 图像插件这一路（`rank 10`，永远可用）。
//   ★ 它"能解什么"完全由 `QImageReader::supportedImageFormats()` 运行时决定 ——
//     我们自己一个后缀都不写：Windows 上就是随包发的 9 个插件，
//     Linux 上发行版装的 `kimageformats`（raw / psd / xcf / avif / jxl / exr …）
//     会自动出现在能力表里。
namespace decode {
Provider *makeQtProvider();
}
