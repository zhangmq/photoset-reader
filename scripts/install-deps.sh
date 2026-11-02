#!/usr/bin/env bash
# 安装 photoset-reader 的依赖（按发行版自动识别）。
#
#   ./scripts/install-deps.sh --check     只检查：哪些有、哪些缺（不改系统）
#   ./scripts/install-deps.sh             安装**运行**依赖（系统依赖版跑起来所需）
#   ./scripts/install-deps.sh --build     安装**构建**依赖（从源码编本程序所需）
#
# 支持的发行版：Arch / Debian·Ubuntu / Fedora / openSUSE。
#   · 包名以各发行版**当前稳定版**为准；`--check` 会逐个确认，缺什么一目了然。
#   · libunrar 是唯一不能保证的：Debian 在 non-free、Fedora 在 RPM Fusion、
#     openSUSE 在 Packman；源里没有时会提示「自行编译 libunrar」（见脚本末尾）。
set -euo pipefail

MODE=runtime
ACTION=install
for a in "$@"; do
  case "$a" in
    --build) MODE=build ;;
    --check) ACTION=check ;;
    -y|--yes) ASSUME_YES=1 ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "未知参数: $a"; exit 2 ;;
  esac
done

. /etc/os-release 2>/dev/null || true
DISTRO_ID=${ID:-}
DISTRO_LIKE=${ID_LIKE:-}
DESKTOP=${XDG_CURRENT_DESKTOP:-}

# ── 各发行版的包名 ─────────────────────────────────────────────────────────
# 运行依赖：Qt6（Core/Gui/Quick/Qml/Widgets + imageformats/svg）、libarchive、
#           libexif、libheif、liblzma、zlib、libunrar
case "$DISTRO_ID $DISTRO_LIKE" in
  *arch*)
    RUN_PKGS=(qt6-base qt6-declarative qt6-imageformats qt6-svg
              libarchive libexif libheif xz zlib libunrar)
    BUILD_PKGS=(cmake ninja gcc pkgconf
                qt6-base qt6-declarative qt6-shadertools qt6-imageformats qt6-svg
                libarchive libexif libheif xz zlib libunrar)
    PM=pacman; INSTALL=(sudo pacman -S --needed --noconfirm)
    QUERY=(pacman -Si);;
  *debian*|*ubuntu*)
    RUN_PKGS=(libqt6core6 libqt6gui6 libqt6quick6 libqt6qml6 libqt6widgets6
              qt6-image-formats-plugins qt6-svg-plugins
              libarchive13 libexif12 libheif1 liblzma5 zlib1g
              libunrar5)                       # non-free；源里没有就自己编
    BUILD_PKGS=(cmake ninja-build g++ pkg-config
                qt6-base-dev qt6-declarative-dev qt6-shadertools-dev
                qt6-image-formats-plugins qt6-svg-dev
                libarchive-dev libexif-dev libheif-dev liblzma-dev zlib1g-dev
                libunrar-dev)
    PM=apt; INSTALL=(sudo apt-get install -y)
    QUERY=(apt-cache show);;
  *fedora*|*rhel*)
    RUN_PKGS=(qt6-qtbase qt6-qtdeclarative qt6-qtimageformats qt6-qtsvg
              libarchive libexif libheif xz-libs zlib-ng-compat
              libunrar)                        # RPM Fusion nonfree
    BUILD_PKGS=(cmake ninja-build gcc-c++ pkgconf
                qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qtshadertools-devel
                qt6-qtimageformats qt6-qtsvg
                libarchive-devel libexif-devel libheif-devel xz-devel zlib-ng-compat-devel
                libunrar-devel)
    PM=dnf; INSTALL=(sudo dnf install -y)
    QUERY=(dnf info);;
  *suse*|*opensuse*)
    RUN_PKGS=(libqt6core6 libqt6gui6 libqt6quick6 libqt6qml6 libqt6widgets6
              libqt6imageformats6 qt6-svg
              libarchive13 libexif12 libheif1 liblzma5 libz1
              libunrar)                        # Packman
    BUILD_PKGS=(cmake ninja gcc-c++ pkgconf
                qt6-base-devel qt6-declarative-devel qt6-shadertools-devel
                qt6-imageformats-devel qt6-svg-devel
                libarchive-devel libexif-devel libheif-devel liblzma-devel zlib-devel
                libunrar-devel)
    PM=zypper; INSTALL=(sudo zypper install -y)
    QUERY=(zypper info);;
  *)
    echo "没有为这个发行版（ID='$DISTRO_ID'）准备包名。"
    echo "可以手动安装：Qt6（Core/Gui/Quick/Qml/Widgets + imageformats/svg）、"
    echo "libarchive、libexif、libheif、liblzma、zlib、libunrar。"
    exit 2;;
esac

[ "$MODE" = build ] && PKGS=("${BUILD_PKGS[@]}") || PKGS=("${RUN_PKGS[@]}")

echo "发行版: $DISTRO_ID ${DISTRO_LIKE:+($DISTRO_LIKE)}  包管理器: $PM  模式: $MODE"
echo

# ── 逐个检查可用性 ────────────────────────────────────────────────────────
missing=()
for p in "${PKGS[@]}"; do
  if "${QUERY[@]}" "$p" >/dev/null 2>&1; then
    echo "  ✓ $p"
  else
    echo "  ✗ $p   （源里没有这个包名）"
    missing+=("$p")
  fi
done

if [ "$ACTION" = check ]; then
  echo
  [ ${#missing[@]} -eq 0 ] && { echo "全部可用。"; exit 0; }
  echo "缺 ${#missing[@]} 个。libunrar 的相关说明："
  echo "  · Debian/Ubuntu：non-free 源（libunrar5 / libunrar-dev）"
  echo "  · Fedora：RPM Fusion non-free（libunrar / libunrar-devel）"
  echo "  · openSUSE：Packman（libunrar / libunrar-devel）"
  exit 1
fi

[ ${#missing[@]} -eq 0 ] || { echo; echo "有包名在源里找不到，先按上面的提示处理再重试。"; exit 1; }

# ── 安装 ─────────────────────────────────────────────────────────────────
if [ "${ASSUME_YES:-0}" != 1 ]; then
  read -r -p "即将执行：${INSTALL[*]} ${PKGS[*]}  —— 继续？[y/N] " ans
  [ "$ans" = y ] || [ "$ans" = Y ] || { echo "已取消"; exit 1; }
fi
"${INSTALL[@]}" "${PKGS[@]}"

echo
echo "完成。若是从源码构建，接着执行："
echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release"
echo "  cmake --build build -j\$(nproc)"
echo
echo "如果你在 Debian/Ubuntu 且没有 libunrar：从 https://www.rarlab.com/rar_add.htm 取 unrar 源码，"
echo "用 makefile 编出 libunrar.so 后，把目录加进 CMAKE_PREFIX_PATH 即可（只读用途，许可允许）。"
