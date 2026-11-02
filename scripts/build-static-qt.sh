#!/usr/bin/env bash
# 构建**最小化的静态 Qt**（给"静态便携版"用），并装到指定前缀。
#
#   ./scripts/build-static-qt.sh [源码目录] [安装前缀]
#     默认：.tmp/qt-src  →  .tmp/qt-static
#
# 产物：一个**静态**的 Qt（qtbase + qtshadertools + qtdeclarative），之后这样建应用：
#
#   cmake -S . -B build-static -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_PREFIX_PATH="$PREFIX" \
#     -DCMAKE_CXX_FLAGS="-ffunction-sections -fdata-sections" \
#     -DCMAKE_EXE_LINKER_FLAGS="-Wl,--gc-sections"
#   cmake --build build-static --target photoset-reader --parallel
#
# 实测（2026-11-02，20 核）：Qt 约 15 分钟，占 ~19 GB（源码 + 构建 + 安装）；
# 我们的应用静态链接后单文件 61.8 MB，zstd-19 压缩 17.5 MB。
#
# 说明（为什么是这些参数）：
#   -no-icu        Qt 官方 Linux 构建强依赖 ICU，我们完全用不到（省 37.7 MB）
#   -no-openssl    我们离线，不用网络（省 ~9.5 MB）
#   -no-glib       不需要 GLib 事件循环
#   -Os + -fno-asynchronous-unwind-tables  体积取向（-O3 换 -Os 省得不多，但白省）
#   -dbus-linked   **要** QtDBus：静态版的文件对话框走 XDG Desktop Portal（见 main.cpp）
#   -no-feature-vulkan  我们的 GPU 路径走 OpenGL/QRhi，不用 Vulkan
#   -static -gc-binaries -ltcg  官方"体积优化"三件套（见 Qt 文档 Building Optimized Qt）
#   -DINPUT_libmd4c=qt          **必须**：用系统 libmd4c(1.x) 会让 Qt 的 static_assert 失败
#                               （md4c 的 flag 位与 Qt 期望的不一致）——用 Qt 自带那份
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:-$ROOT/.tmp/qt-src}
PREFIX=${2:-$ROOT/.tmp/qt-static}
QT_VERSION=6.12.0
MODULES=(qtbase qtshadertools qtdeclarative qtwayland)
JOBS=${JOBS:-$(nproc)}

mkdir -p "$SRC"

# ── ① 下载 + 解包（已存在就跳过）────────────────────────────────────────────
for m in "${MODULES[@]}"; do
  tarball="$SRC/$m.tar.xz"
  [ -f "$tarball" ] || curl -fL -o "$tarball" \
    "https://download.qt.io/official_releases/qt/6.12/${QT_VERSION}/submodules/$m-everywhere-src-${QT_VERSION}.tar.xz"
  [ -d "$SRC/$m-everywhere-src-${QT_VERSION}" ] || tar -xf "$tarball" -C "$SRC"
done

# ── ② qtbase：配置 → 构建 → 安装 ───────────────────────────────────────────
cd "$SRC/qtbase-everywhere-src-${QT_VERSION}"
[ -f CMakeCache.txt ] || ./configure \
  -static -release \
  -prefix "$PREFIX" \
  -gc-binaries -ltcg \
  -no-icu -no-openssl -no-glib -dbus-linked -no-feature-vulkan \
  -nomake examples -nomake tests -nomake benchmarks \
  -- -DINPUT_libmd4c=qt \
     -DCMAKE_C_FLAGS_RELEASE="-Os -DNDEBUG -fno-asynchronous-unwind-tables" \
     -DCMAKE_CXX_FLAGS_RELEASE="-Os -DNDEBUG -fno-asynchronous-unwind-tables"
cmake --build . --target clean >/dev/null 2>&1 || true   # 配置变过 ⇒ 别复用旧对象
cmake --build . --parallel "$JOBS"
cmake --install .

# ── ③ 其余模块（用 Qt 自带的 qt-configure-module 接上同一个前缀）───────────
for m in qtshadertools qtdeclarative qtwayland; do
  build="$SRC/$m-build"
  rm -rf "$build" && mkdir -p "$build" && cd "$build"
  "$PREFIX/bin/qt-configure-module" "$SRC/$m-everywhere-src-${QT_VERSION}"
  cmake --build . --parallel "$JOBS"
  cmake --install .
done

echo
echo "✓ 静态 Qt 已装到: $PREFIX"
echo "  体积: $(du -sh "$PREFIX" | cut -f1)"
echo "  建应用见本脚本头部注释。"
