#!/usr/bin/env bash
# 把「桌面集成」装到当前用户（**不需要 root**）：.desktop + 全套图标 + 缓存刷新。
#
# 用法：
#   ./scripts/install-desktop.sh                     # Exec 用 PATH 里的 photoset-reader
#   ./scripts/install-desktop.sh --exec ./build/photoset-reader   # 指到开发构建
#   ./scripts/install-desktop.sh --uninstall         # 卸掉这三个东西
#
# 想让它成为图片/漫画的默认打开方式（**可选**，不改系统级关联）：
#   xdg-mime default photoset-reader.desktop \
#       image/jpeg image/png image/webp image/gif image/bmp image/tiff image/svg+xml \
#       image/avif image/jxl image/qoi image/tga image/x-icon image/x-portable-anymap \
#       application/vnd.comicbook+zip application/vnd.comicbook-rar \
#       application/x-cb7 application/x-cbt application/zip application/vnd.rar \
#       application/x-7z-compressed application/x-tar
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DATA=${XDG_DATA_HOME:-$HOME/.local/share}
APP=photoset-reader
EXEC=$(command -v $APP || echo "/usr/local/bin/$APP")
UNINSTALL=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --exec)      EXEC=$2; shift 2 ;;
        --uninstall) UNINSTALL=1; shift ;;
        -h|--help)   sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "未知参数：$1" >&2; exit 2 ;;
    esac
done

DESKTOP="$DATA/applications/$APP.desktop"
ICONS="$DATA/icons/hicolor"

if [[ $UNINSTALL -eq 1 ]]; then
    rm -f "$DESKTOP"
    for s in 16 24 32 48 64 128 256 512; do rm -f "$ICONS/${s}x${s}/apps/$APP.png"; done
    rm -f "$ICONS/scalable/apps/$APP.svg"
    echo "✓ 已移除桌面集成（MIME 默认关联请用 xdg-mime default 改回）"
    exit 0
fi

# `--exec` 给了相对路径 ⇒ 转成绝对路径（.desktop 里的 Exec 不接受相对）
case "$EXEC" in
    /*) ;;
    *)  EXEC=$(cd "$(dirname "$EXEC")" && pwd)/$(basename "$EXEC") ;;
esac
[[ -x "$EXEC" ]] || echo "⚠️  $EXEC 不存在或不可执行（先构建，或用 --exec 指定）" >&2

mkdir -p "$DATA/applications"
sed "s|^Exec=.*|Exec=$EXEC %F|" "$ROOT/assets/$APP.desktop" > "$DESKTOP"
for s in 16 24 32 48 64 128 256 512; do
    mkdir -p "$ICONS/${s}x${s}/apps"
    cp "$ROOT/assets/icons/$APP-$s.png" "$ICONS/${s}x${s}/apps/$APP.png"
done
mkdir -p "$ICONS/scalable/apps"
cp "$ROOT/assets/icons/$APP.svg" "$ICONS/scalable/apps/$APP.svg"

# 让桌面立刻看见（有哪个跑哪个；都不在也不影响）
command -v update-desktop-database >/dev/null && update-desktop-database "$DATA/applications" >/dev/null 2>&1 || true
command -v gtk-update-icon-cache   >/dev/null && gtk-update-icon-cache -f -t "$ICONS" >/dev/null 2>&1 || true
command -v kbuildsycoca6          >/dev/null && kbuildsycoca6 >/dev/null 2>&1 || true

echo "✓ 已安装：$DESKTOP"
echo "  Exec = $EXEC %F"
echo "  图标 = $ICONS/{16..512}x…/apps/$APP.png + scalable/apps/$APP.svg"
echo
#   ★ 2026-11-07（用户）：**不做任何自动关联**。这里只把"怎么关联"写清楚，
#     由用户自己决定要不要执行 —— 安装过程绝不替用户改默认程序。
echo "本应用已出现在文件管理器的「打开方式」里；**没有**替你把任何类型设为默认。"
echo "想让它成为默认（可选，由你决定）："
echo "  xdg-mime default $APP.desktop image/jpeg image/png image/webp application/vnd.comicbook+zip"
echo "  xdg-mime default $APP.desktop application/x-photoset-progress   # .prsave 进度文件"
