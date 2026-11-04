# 第三方组件与许可

[English](THIRD-PARTY.en.md)

本项目自身按 [MIT](LICENSE) 发布；下面只涉及第三方库，不影响本项目代码的许可。

**链接形态分两种，义务不同：**

| 形态 | 组件 | 说明 |
|---|---|---|
| **动态链接** | libarchive / libexif / libheif / libunrar / liblzma / zlib | 运行时用系统或发行版提供的 `.so` / `.dll`，对应 LGPL §4(d)(1)「共享库机制」 |
| **静态链接** | **Qt 6.12.0**（Core / Gui / Quick / Qml / Widgets） | 仅 AppImage：由 `build-static/` 的产物打成，Qt 静态编入（单文件约 64 MB，`ldd` 里没有 `libQt6`）⇒ 对应 LGPL §4(d)(0)，义务见「静态链接的 Qt」一节 |

## 一览

| 组件 | 用途 | 链接 | 许可 | 性质 |
|---|---|---|---|---|
| **Qt 6**（Core / Gui / Quick / Qml / Widgets；`ShaderTools` 只在构建期用） | UI、解码（`QImageReader`）、线程 | **静态**（AppImage）/ 动态（其余） | **LGPL-3.0-only**（另有 GPL-3.0 与商业授权可选，本项目按 LGPL 使用） | 弱 copyleft |
| **libarchive** | zip/cbz、tar/cbt、7z 读取 | 动态 | BSD-2-Clause | 宽松 |
| **libexif** | EXIF 标签解析与格式化 | 动态 | LGPL-2.1 | 弱 copyleft |
| **libheif** | HEIC/AVIF 的 Exif 块与 ICC | 动态 | LGPL-3.0-or-later | 弱 copyleft |
| **libunrar** | RAR/CBR 读取 | 动态 | UnRAR 许可（非 OSI，见下） | 有限制但非 copyleft |
| **liblzma**（xz） | 7z 压缩头解压 | 动态 | public domain / LGPL 部分 | 宽松 |
| **zlib** | PNG `iCCP` 解压 | 动态 | Zlib | 宽松 |
| **Material Design Icons 字体** | 界面图标 | 编进资源 | Apache-2.0（见 `src/assets/fonts/MDI-LICENSE.txt`） | 宽松 |

**没有 GPL / AGPL 组件。** Qt 按 **LGPL-3.0** 使用（随包的 `GPL-3.0-only.txt` 是 LGPL §4(b) 要求
一并附上的全文，不代表用到 GPL 代码）。唯一带 `GPL` 字样的 `libstdc++` / `libgcc_s` 是 GCC 运行时例外版。

> 这里只列**我们直接链接**的组件。Qt Quick 自己还会带入 OpenGL（`libglvnd`）、
> Network、DBus 等模块；libarchive 会带入 curl / OpenSSL 一类的传递依赖。
> 它们随发行版的打包方式不同，不改变上面的许可结论。

## 平台相关

| 平台 | 额外依赖 |
|---|---|
| **Linux**（主要支持平台） | glibc（LGPL-2.1-or-later，系统库）、libstdc++ / libgcc_s（GCC 运行时例外）、libglvnd、libxkbcommon、fontconfig / freetype、dbus |
| **Windows**（另一平台） | 系统自带的 Direct3D 11 / DXGI / DirectWrite / GDI；MSVC 运行时（`vcruntime140` / `msvcp140`）或 MinGW 运行时（`libstdc++-6` / `libgcc_s_*-1` / `libwinpthread-1`），打包时随包分发 |

## 动态链接的 LGPL 组件（libexif / libheif）

1. **动态链接**：以动态库形式链接（Linux `.so`、Windows `.dll`），未修改源码；
2. **可替换**：它们不在本项目仓库里，用户可以用自己编译/发行版提供的同名动态库替换
   （例如 `LD_PRELOAD`，或直接替换同目录下的 `.dll`），程序无需重新编译；
   对应 LGPL §4(d)(1) 的「共享库机制」；
3. **声明**：本文件与 [LICENSE](LICENSE) 一起随源码与二进制分发。

## 静态链接的 Qt（仅 AppImage）—— LGPL-3.0 §4

AppImage 里的 Qt 是**静态编入**的，所以走 §4(d)(0) 而不是「共享库机制」。LGPL-3.0 §4 允许
Combined Work 用你选定的条款发布（原文：*You may convey a Combined Work under terms of your choice …*），
前提是同时满足下列各条，并且**不限制用户修改库、为调试反向工程**。逐条对应：

| §4 | 要求 | 本项目如何满足 |
|---|---|---|
| (a) | 每份副本**显著声明**用了这个库、且库与其使用受 LGPL 约束 | 本文件随每个包分发（`/usr/share/doc/photoset-reader/THIRD-PARTY.md`） |
| (b) | **随附 GPL-3.0 与 LGPL-3.0 全文** | 仓库里在 `LICENSES/`；安装后与 `LICENSE`、本文件同目录（`GPL-3.0-only.txt` / `LGPL-3.0-only.txt`） |
| (c) | 若程序**运行期显示版权声明**，其中要含库的版权声明 | 本程序**不在界面里显示版权声明** ⇒ 不适用 |
| (d)(0) | 提供 **Minimal Corresponding Source** + **可重新链接的应用代码** | 两者都是**源码 + 构建脚本**：① Qt **未作任何修改**，Minimal Corresponding Source 就是上游 Qt 6.12.0 —— `scripts/build-static-qt.sh` 记录了源码地址、模块清单与全部 configure 参数，可据此重建同一份静态 Qt；② 应用侧源码按 MIT 全部公开，同一脚本即可把应用与改过的 Qt 重新链接成新的可执行文件（GPL-3.0 §6 的 "Corresponding Source" 本身就包含控制编译与安装的脚本） |
| (e) | Installation Information（**仅**当 GPL §6 要求时） | §6 只针对 **User Product（消费类设备）**，桌面 AppImage 不属于 ⇒ 不适用；重链接步骤仍按 (d)(0) 给出 |

全部许可全文在 `LICENSES/`，并随每个包安装到 `share/doc/photoset-reader/`。

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
