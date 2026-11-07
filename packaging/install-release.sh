#!/usr/bin/env bash
# ══ photoset-reader 发布包安装脚本（**系统依赖版**：Qt 与第三方库用系统已装的）════════
#   ./install.sh                 # 装到 ~/.local（不需要 root）
#   ./install.sh --prefix /usr/local   # 系统级（需要 sudo）
#   ./install.sh --uninstall           # 按同一前缀卸载（只删本包文件，不动配置/收藏夹）
#
#   做的事：检查运行期依赖 → 复制文件 → 重写 .desktop 的 Exec 为绝对路径 →
#   `update-desktop-database` / `update-mime-database`（只让类型**可被识别**）→
#   **不设任何默认程序**（图片 / 文件夹 / 压缩包 / `.prsave` 都不改，详见下面的 2026-11-07 注）。
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
PREFIX=${PREFIX:-$HOME/.local}
UNINSTALL=0
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix)    PREFIX=$2; shift 2 ;;
    --uninstall) UNINSTALL=1; shift ;;
    -h|--help)   sed -n '2,9p' "$0"; exit 0 ;;
    *) echo "未知参数：$1" >&2; exit 2 ;;
  esac
done

DESKTOP_NAME=photoset-reader.desktop
MIME_NAME=photoset-reader.xml

if [ "$UNINSTALL" -eq 1 ]; then
  rm -f  "$PREFIX/bin/photoset-reader" \
         "$PREFIX/share/applications/$DESKTOP_NAME" \
         "$PREFIX/share/mime/packages/$MIME_NAME"
  rm -rf "$PREFIX/share/doc/photoset-reader"
  for d in "$PREFIX"/share/icons/hicolor/*/apps; do rm -f "$d/photoset-reader.png"; done
  rm -f "$PREFIX/share/icons/hicolor/scalable/apps/photoset-reader.svg"
  command -v update-desktop-database >/dev/null && update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true
  command -v update-mime-database   >/dev/null && update-mime-database   "$PREFIX/share/mime"         2>/dev/null || true
  echo "已卸载（配置与收藏夹保留在 ~/.config/photoset-reader/）"
  exit 0
fi

echo "== 检查运行期依赖 =="
if command -v ldd >/dev/null; then
  MISSING=$(ldd "$HERE/bin/photoset-reader" 2>/dev/null | awk '/not found/{print $1}' | sort -u || true)
  if [ -n "$MISSING" ]; then
    echo "缺少运行期库：" >&2
    echo "$MISSING" | sed 's/^/  - /' >&2
    echo >&2
    echo "这是**系统依赖版**：请先用发行版的包管理器装好 Qt 6 与第三方库，例如：" >&2
    case "$(sed -n 's/^ID=//p' /etc/os-release 2>/dev/null | head -1)" in
      arch|endeavouros|manjaro) echo "  sudo pacman -S qt6-base qt6-declarative qt6-shadertools libarchive libexif libheif libunrar zlib liblzma" >&2 ;;
      debian|ubuntu)            echo "  sudo apt install qt6-base-dev qt6-declarative-dev qt6-shadertools-dev libarchive13 libexif12 libheif1 libunrar5 zlib1g liblzma5" >&2 ;;
      fedora|rhel|centos)       echo "  sudo dnf install qt6-qtbase qt6-qtdeclarative qt6-qtshadertools libarchive libexif libheif unrar-free zlib xz-libs" >&2 ;;
      *)                        echo "  详见项目 README 的依赖列表" >&2 ;;
    esac
    exit 1
  fi
  echo "  OK"
else
  echo "  （没有 ldd，跳过检查）"
fi

echo "== 安装到 $PREFIX =="
mkdir -p "$PREFIX/bin" "$PREFIX/share/applications" "$PREFIX/share/mime/packages" \
         "$PREFIX/share/icons/hicolor" "$PREFIX/share/doc/photoset-reader"
install -m755 "$HERE/bin/photoset-reader" "$PREFIX/bin/photoset-reader"
install -m644 "$HERE/share/applications/$DESKTOP_NAME" "$PREFIX/share/applications/$DESKTOP_NAME"
install -m644 "$HERE/share/mime/packages/$MIME_NAME"  "$PREFIX/share/mime/packages/$MIME_NAME"
install -m644 "$HERE/share/doc/photoset-reader/"*     "$PREFIX/share/doc/photoset-reader/" 2>/dev/null || true
cp -a "$HERE/share/icons/hicolor/." "$PREFIX/share/icons/hicolor/"

#   `.desktop` 的 Exec 用**绝对路径**（launcher 不继承 shell 的 PATH）
sed -i "s|^Exec=.*|Exec=$PREFIX/bin/photoset-reader %F|" "$PREFIX/share/applications/$DESKTOP_NAME"

command -v update-desktop-database >/dev/null && update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true
command -v update-mime-database   >/dev/null && update-mime-database   "$PREFIX/share/mime"         2>/dev/null || true
#   ★ 2026-11-07（用户）：**不做任何自动关联**（连 `.prsave` 也不）。
#     上面的 `update-mime-database` 只是让 `application/x-photoset-progress` 这个**类型**
#     能被识别，不属于"把某类型交给某程序"；默认程序一律交给用户自己选。

echo
echo "✓ 完成。启动：$PREFIX/bin/photoset-reader <文件夹|压缩包|图片>"
case ":$PATH:" in
  *":$PREFIX/bin:"*) ;;
  *) echo "提示：$PREFIX/bin 不在 PATH 里（把这行加进 shell 配置）："
     echo "  export PATH=\"$PREFIX/bin:\$PATH\"" ;;
esac
