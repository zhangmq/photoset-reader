#pragma once

#include "imagedecoder.h"

//   Windows 的**系统编解码器**这一路（`rank 20`）。
//   ★ 它不"实现"任何格式：`probe()` 枚举 `IWICImagingFactory` 里**系统已注册的全部解码器**，
//     把每个编解码器自己声明的扩展名收集起来 ⇒ 用户装了"Raw 图像扩展 / HEIF 扩展 /
//     AV1 视频扩展"之后，`arw` / `heic` / `avif` 会自动出现在能力表里，**我们不加一行格式代码**。
//   ★ 非 Windows 平台返回 `nullptr`（这一路不存在），调用方不注册。
namespace decode {
Provider *makeWicProvider();
}
