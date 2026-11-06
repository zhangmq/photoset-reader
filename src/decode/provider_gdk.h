#pragma once

#include "imagedecoder.h"

//   Linux 的 **gdk-pixbuf**（GNOME 栈）这一路（`rank 20`）。
//   ★ 为什么需要它：Qt 插件那条路只覆盖"装了 kimageformats 的机器"（KDE 系）；
//     GNOME / 非 KDE 桌面上 `kimg_*` 可能一个都没有，能解 AVIF/JXL/HEIF 的往往是
//     gdk-pixbuf 的 loader（libheif / libjxl / librsvg 提供）。
//   ★ **全部 symbol 用 `dlopen` 取**：不加编译期依赖（pkg-config 里不需要 gdk-pixbuf），
//     库不在 ⇒ 这一路直接不存在。
//   ★ 非 Linux 平台返回 `nullptr`。
namespace decode {
Provider *makeGdkProvider();
}
