# Third-party components and licenses

[中文](THIRD-PARTY.md)

This project itself is released under [MIT](LICENSE). The sections below cover third-party libraries
only; they do not affect the licensing of this project's own code.

**There are two link modes, with different obligations:**

| Mode | Components | Notes |
|---|---|---|
| **Dynamic** | libarchive / libexif / libheif / libunrar / liblzma / zlib | linked at run time against the `.so` / `.dll` provided by the system or distribution; corresponds to LGPL §4(d)(1), the "shared library mechanism" |
| **Static** | **Qt 6.12.0** (Core / Gui / Quick / Qml / Widgets) | AppImage only: built from the `build-static/` output, with Qt compiled in (single ~64 MB binary, no `libQt6` in `ldd`) ⇒ corresponds to LGPL §4(d)(0); see "Statically linked Qt" below |

## Overview

| Component | Used for | Linking | License | Kind |
|---|---|---|---|---|
| **Qt 6** (Core / Gui / Quick / Qml / Widgets; `ShaderTools` is build-time only) | UI, decoding (`QImageReader`), threading | **static** (AppImage) / dynamic (elsewhere) | **LGPL-3.0-only** (GPL-3.0 and commercial licensing are also available; this project uses LGPL) | weak copyleft |
| **libarchive** | zip/cbz, tar/cbt, 7z reading | dynamic | BSD-2-Clause | permissive |
| **libexif** | EXIF tag parsing and formatting | dynamic | LGPL-2.1 | weak copyleft |
| **libheif** | Exif block and ICC for HEIC/AVIF | dynamic | LGPL-3.0-or-later | weak copyleft |
| **libunrar** | RAR/CBR reading | dynamic | UnRAR license (not OSI, see below) | restricted, not copyleft |
| **liblzma** (xz) | 7z header decompression | dynamic | public domain / partly LGPL | permissive |
| **zlib** | PNG `iCCP` decompression | dynamic | Zlib | permissive |
| **Material Design Icons font** | UI icons | compiled into resources | Apache-2.0 (see `src/assets/fonts/MDI-LICENSE.txt`) | permissive |

**No GPL / AGPL components.** Qt is used under **LGPL-3.0** (the bundled `GPL-3.0-only.txt` is the full
text that LGPL §4(b) requires to be included; it does not mean GPL code is used). The only names containing
`GPL` are `libstdc++` / `libgcc_s`, which are covered by the GCC runtime exception.

> Only **directly linked** components are listed. Qt Quick itself pulls in OpenGL (`libglvnd`), Network,
> DBus and similar modules; libarchive pulls in transitive dependencies such as curl / OpenSSL.
> These differ with how a distribution packages them and do not change the conclusions above.

## Platform notes

| Platform | Additional dependencies |
|---|---|
| **Linux** (primary platform) | glibc (LGPL-2.1-or-later, system library), libstdc++ / libgcc_s (GCC runtime exception), libglvnd, libxkbcommon, fontconfig / freetype, dbus |
| **Windows** (second platform) | the system's Direct3D 11 / DXGI / DirectWrite / GDI; MSVC runtime (`vcruntime140` / `msvcp140`) or MinGW runtime (`libstdc++-6` / `libgcc_s_*-1` / `libwinpthread-1`), shipped with the package |

## Dynamically linked LGPL components (libexif / libheif)

1. **Dynamic linking**: linked as dynamic libraries (`.so` on Linux, `.dll` on Windows), with no source
   modifications;
2. **Replaceable**: they are not part of this repository, so users can substitute their own build or the
   one from their distribution (for example `LD_PRELOAD`, or replacing the `.dll` next to the binary);
   the program does not need to be recompiled. This corresponds to LGPL §4(d)(1), the "shared library
   mechanism";
3. **Notice**: this file is distributed with the source and binaries together with [LICENSE](LICENSE).

## Statically linked Qt (AppImage only) — LGPL-3.0 §4

Qt inside the AppImage is **compiled in statically**, so §4(d)(0) applies rather than the "shared library
mechanism". LGPL-3.0 §4 allows a Combined Work to be conveyed under terms of your choice (original text:
*You may convey a Combined Work under terms of your choice …*), provided each of the following is met and
users are not restricted from modifying the library or reverse engineering it for debugging:

| §4 | Requirement | How this project complies |
|---|---|---|
| (a) | **Prominent notice** with each copy that the library is used and that the library and its use are covered by the LGPL | this file ships with every package (`/usr/share/doc/photoset-reader/THIRD-PARTY.md`) |
| (b) | **A copy of the GPL-3.0 and LGPL-3.0 texts** | in the repository under `LICENSES/`; after installation they sit next to `LICENSE` and this file (`GPL-3.0-only.txt` / `LGPL-3.0-only.txt`) |
| (c) | If the program **displays copyright notices at run time**, include the library's copyright notice among them | this program **does not display copyright notices in its UI** ⇒ not applicable |
| (d)(0) | Provide the **Minimal Corresponding Source** plus the **application code in a form suitable for relinking** | both are **source plus build scripts**: ① Qt is **unmodified**, so the Minimal Corresponding Source is upstream Qt 6.12.0 — `scripts/build-static-qt.sh` records the source URLs, module list and every configure flag, enough to rebuild the same static Qt; ② the application source is public under MIT, and the same script relinks the application against a modified Qt into a new executable (GPL-3.0 §6's "Corresponding Source" itself includes the scripts used to control compilation and installation) |
| (e) | Installation Information (**only** when GPL §6 requires it) | §6 applies only to a **User Product** (a consumer device); a desktop AppImage is not one ⇒ not applicable. The relinking steps are still given under (d)(0) |

All license texts are in `LICENSES/` and are installed into `share/doc/photoset-reader/` with every package.

## UnRAR

RAR/CBR support is provided through **libunrar**. Its license requires the following text to be retained:

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

This project uses libunrar **for reading** RAR archives only; it does not implement or take part in any
RAR compressor.
