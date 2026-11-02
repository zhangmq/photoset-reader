#!/usr/bin/env bash
# 构建**最小化**的第三方库（给便携版 AppImage 用）：
#   · libarchive：关掉 libxml2 / curl / openssl / 测试工具 ⇒ 不再拖进 ICU(31 MB)/OpenSSL
#   · libheif：关掉**所有**编解码插件（我们只拿它读 AVIF/HEIC 的元数据；HEIC 不支持）
#                 ⇒ 不再拖进 x265/x264/aom/openh264/dav1d
#
#   ./scripts/build-minimal-deps.sh [安装前缀]     默认 .tmp/deps
#
# 版本取**本机发行版同款**（libarchive 3.8.11 / libheif 1.23.7）；换版本时改下面的变量即可。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PREFIX=${1:-$ROOT/.tmp/deps}
SRC=$ROOT/.tmp/deps-src
BUILD=$ROOT/.tmp/deps-build
JOBS=${JOBS:-$(nproc)}
LIBARCHIVE_VER=3.8.11
LIBHEIF_VER=1.23.7

mkdir -p "$SRC" "$BUILD"

fetch() {  # $1=文件 $2=URL
  [ -f "$SRC/$1" ] || curl -fL -o "$SRC/$1" "$2"
}

fetch "libarchive-$LIBARCHIVE_VER.tar.xz" \
  "https://github.com/libarchive/libarchive/releases/download/v$LIBARCHIVE_VER/libarchive-$LIBARCHIVE_VER.tar.xz"
fetch "libheif-$LIBHEIF_VER.tar.gz" \
  "https://github.com/strukturag/libheif/releases/download/v$LIBHEIF_VER/libheif-$LIBHEIF_VER.tar.gz"
[ -d "$SRC/libarchive-$LIBARCHIVE_VER" ] || tar -xf "$SRC/libarchive-$LIBARCHIVE_VER.tar.xz" -C "$SRC"
[ -d "$SRC/libheif-$LIBHEIF_VER" ] || tar -xf "$SRC/libheif-$LIBHEIF_VER.tar.gz" -C "$SRC"

# ── libarchive ───────────────────────────────────────────────────────────
cmake -S "$SRC/libarchive-$LIBARCHIVE_VER" -B "$BUILD/libarchive" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DBUILD_SHARED_LIBS=ON \
  -DENABLE_LIBXML2=OFF -DENABLE_EXPAT=OFF -DENABLE_CURL=OFF -DENABLE_OPENSSL=OFF \
  -DENABLE_TEST=OFF -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF \
  -DCMAKE_C_FLAGS="-Os -fno-asynchronous-unwind-tables"
cmake --build "$BUILD/libarchive" --parallel "$JOBS"
cmake --install "$BUILD/libarchive"

# ── libheif（全部编解码器关闭）────────────────────────────────────────────
cmake -S "$SRC/libheif-$LIBHEIF_VER" -B "$BUILD/libheif" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DBUILD_SHARED_LIBS=ON \
  -DWITH_LIBDE265=OFF -DWITH_X265=OFF -DWITH_KVAZAAR=OFF -DWITH_UVG266=OFF \
  -DWITH_VVDEC=OFF -DWITH_VVENC=OFF -DWITH_X264=OFF -DWITH_OpenH264_DECODER=OFF \
  -DWITH_DAV1D=OFF -DWITH_AOM_DECODER=OFF -DWITH_AOM_ENCODER=OFF -DWITH_SvtEnc=OFF \
  -DWITH_RAV1E=OFF -DWITH_JPEG_DECODER=OFF -DWITH_JPEG_ENCODER=OFF \
  -DWITH_OpenJPEG_DECODER=OFF -DWITH_OpenJPEG_ENCODER=OFF -DWITH_LIBSHARPYUV=OFF \
  -DWITH_UNCOMPRESSED_CODEC=OFF -DWITH_EXAMPLES=OFF -DWITH_GDK_PIXBUF=OFF \
  -DCMAKE_C_FLAGS="-Os" -DCMAKE_CXX_FLAGS="-Os"
cmake --build "$BUILD/libheif" --parallel "$JOBS"
cmake --install "$BUILD/libheif"

echo
echo "✓ 最小化依赖已装到: $PREFIX"
echo "  之后用这个前缀静态构建："
echo "    PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig cmake -S . -B build-static …"
