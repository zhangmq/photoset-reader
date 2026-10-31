# 第三方组件与许可

本项目自身按 [MIT](LICENSE) 发布。下面是它**运行时依赖**的第三方组件及其许可。
第三方库一律**动态链接**（`libibcore.a` 是我们自己的代码，不涉及第三方），
没有静态链接任何 LGPL 组件。

## 一览

| 组件 | 用途 | 许可 | 性质 |
|---|---|---|---|
| **Qt 6**（Core / Gui / Quick / Qml / Widgets；`ShaderTools` 只在构建期用） | UI、解码（`QImageReader`）、线程 | **LGPL-3.0-only**（另有 GPL-3.0 与商业授权可选，本项目按 LGPL 使用） | 弱 copyleft |
| **libarchive** | zip/cbz、tar/cbt、7z 读取 | BSD-2-Clause | 宽松 |
| **libexif** | EXIF 标签解析与格式化 | LGPL-2.1 | 弱 copyleft |
| **libheif** | HEIC/AVIF 的 Exif 块与 ICC | LGPL-3.0-or-later | 弱 copyleft |
| **libunrar** | RAR/CBR 读取 | UnRAR 许可（非 OSI，见下） | 有限制但非 copyleft |
| **liblzma**（xz） | 7z 压缩头解压 | public domain / LGPL 部分 | 宽松 |
| **zlib** | PNG `iCCP` 解压 | Zlib | 宽松 |
| **Material Design Icons 字体** | 界面图标 | Apache-2.0（见 `src/assets/fonts/MDI-LICENSE.txt`） | 宽松 |

**没有 GPL / AGPL 组件。** 唯一带 `GPL` 字样的 `libstdc++` / `libgcc_s` 是 GCC 的运行时例外版，
按设计不会传染到使用它的程序。

> 这里只列**我们直接链接**的组件。Qt Quick 自己还会带入 OpenGL（`libglvnd`）、
> Network、DBus 等模块；libarchive 会带入 curl / OpenSSL 一类的传递依赖。
> 它们随发行版的打包方式不同，不改变上面的许可结论。

## 平台相关

| 平台 | 额外依赖 |
|---|---|
| **Linux**（主要支持平台） | glibc（LGPL-2.1-or-later，系统库）、libstdc++ / libgcc_s（GCC 运行时例外）、libglvnd、libxkbcommon、fontconfig / freetype、dbus |
| **Windows**（另一平台） | 系统自带的 Direct3D 11 / DXGI / DirectWrite / GDI；MSVC 运行时（`vcruntime140` / `msvcp140`）或 MinGW 运行时（`libstdc++-6` / `libgcc_s_*-1` / `libwinpthread-1`），打包时随包分发 |

## LGPL 组件的义务（Qt / libexif / libheif）

本项目按 LGPL 的要求做到了：

1. **动态链接**：以上组件都以动态库形式链接（Linux `.so`、Windows `.dll`），
   没有静态链接、没有修改其源码；
2. **可替换**：它们不在本项目仓库里，用户可以用自己编译/发行版提供的同名动态库替换
   （例如 `LD_PRELOAD`，或直接替换同目录下的 `.dll`），程序无需重新编译；
3. **声明**：本文件与 [LICENSE](LICENSE) 一起随源码与二进制分发。

各组件随附的许可全文见其发行包（Arch：`/usr/share/licenses/qt6-base/`、`/usr/share/licenses/libexif/`、
`/usr/share/licenses/libheif/`）。分发二进制时须一并提供这些文本。

## UnRAR

RAR/CBR 支持通过 **libunrar** 提供。其许可要求保留以下原文：

> 2. UnRAR source code may be used in any software to handle
>    RAR archives without limitations free of charge, but cannot be
>    used to develop RAR (WinRAR) compatible archiver and to
>    re-create RAR compression algorithm, which is proprietary.
>    Distribution of modified UnRAR source code in separate form
>    or as a part of other software is permitted, provided that
>    full text of this paragraph, starting from "UnRAR source code"
>    words, is included in license, or in documentation if license
>    is not available, and in source code comments of resulting package.
>
> 3. The UnRAR utility may be freely distributed. It is allowed
>    to distribute UnRAR inside of other software packages.

本项目只用 libunrar **读取** RAR 存档，不实现、不参与任何 RAR 压缩器。
