#!/usr/bin/env bash
# 构建**最小化**的第三方库（给便携版 AppImage 用）：
#   · libarchive：关掉 libxml2 / curl / openssl / 测试工具 ⇒ 不再拖进 ICU(31 MB)/OpenSSL
#   ★ 2026-11-07：**libheif 已从这里删掉** —— 现在 HEIC/AVIF 的 Exif/ICC 是运行期
#     `dlopen` 取（`src/heifmeta.cpp`），不再是编译期依赖，编它纯属浪费。
#
#   ./scripts/build-minimal-deps.sh [安装前缀]     默认 .tmp/deps
#
# 版本取**本机发行版同款**（libarchive 3.8.11）；换版本时改下面的变量即可。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PREFIX=${1:-$ROOT/.tmp/deps}
SRC=$ROOT/.tmp/deps-src
BUILD=$ROOT/.tmp/deps-build
JOBS=${JOBS:-$(nproc)}
LIBARCHIVE_VER=3.8.11

mkdir -p "$SRC" "$BUILD"

fetch() {  # $1=文件 $2=URL
  [ -f "$SRC/$1" ] || curl -fL -o "$SRC/$1" "$2"
}

fetch "libarchive-$LIBARCHIVE_VER.tar.xz" \
  "https://github.com/libarchive/libarchive/releases/download/v$LIBARCHIVE_VER/libarchive-$LIBARCHIVE_VER.tar.xz"
[ -d "$SRC/libarchive-$LIBARCHIVE_VER" ] || tar -xf "$SRC/libarchive-$LIBARCHIVE_VER.tar.xz" -C "$SRC"

# ── libarchive ───────────────────────────────────────────────────────────
cmake -S "$SRC/libarchive-$LIBARCHIVE_VER" -B "$BUILD/libarchive" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DBUILD_SHARED_LIBS=ON \
  -DENABLE_LIBXML2=OFF -DENABLE_EXPAT=OFF -DENABLE_CURL=OFF -DENABLE_OPENSSL=OFF \
  -DENABLE_TEST=OFF -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF \
  -DCMAKE_C_FLAGS="-Os -fno-asynchronous-unwind-tables"
cmake --build "$BUILD/libarchive" --parallel "$JOBS"
cmake --install "$BUILD/libarchive"

echo
echo "✓ 最小化依赖已装到: $PREFIX"
echo "  之后用这个前缀静态构建："
echo "    PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig cmake -S . -B build-static …"
