#!/usr/bin/env bash
# 打 AppImage（便携版）：静态 Qt 的可执行文件 + 第三方动态库闭包 + 桌面入口。
#
#   ./scripts/package-appimage.sh [--deps <前缀>] [--out <输出文件>]
#     默认 deps = .tmp/deps（最小化的 libarchive / libheif，见 scripts/build-minimal-deps.sh）
#
# 依赖：build-static/photoset-reader（见 scripts/build-static-qt.sh）、appimagetool、
#       python3。appimagetool 默认取 .tmp/appimage/appimagetool.AppImage，没有就下载。
#
# 打进包里的库 = 可执行文件的动态依赖闭包，减去：
#   · C/C++ 运行时（glibc / libstdc++ / libgcc）—— 按 AppImage 惯例用系统的
#   · 图形/桌面栈（X11 / xcb / Wayland / GL / fontconfig / freetype / dbus / glib…）
#     —— 换机器上这些必须与本机驱动匹配，打包反而更脆
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEPS=${DEPS:-$ROOT/.tmp/deps}
OUT=${OUT:-$ROOT/dist/photoset-reader-x86_64.AppImage}
BIN=$ROOT/build-static/photoset-reader

while [ $# -gt 0 ]; do
  case "$1" in
    --deps) DEPS=$2; shift 2 ;;
    --out)  OUT=$2;  shift 2 ;;
    *) echo "未知参数: $1"; exit 2 ;;
  esac
done

[ -x "$BIN" ] || { echo "先静态构建：见 scripts/build-static-qt.sh"; exit 1; }

TOOL=${APPIMAGETOOL:-$ROOT/.tmp/appimage/appimagetool.AppImage}
if [ ! -x "$TOOL" ]; then
  mkdir -p "$(dirname "$TOOL")"
  echo "下载 appimagetool…"
  curl -fL -o "$TOOL" \
    https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
  chmod +x "$TOOL"
fi

APPDIR=$ROOT/.tmp/appimage/AppDir
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" \
         "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"
cp "$BIN" "$APPDIR/usr/bin/photoset-reader"

# ── 依赖闭包 ─────────────────────────────────────────────────────────────
python3 - "$APPDIR" "$DEPS" <<'PY'
import os, re, shutil, subprocess, sys
appdir, deps = sys.argv[1], sys.argv[2]
SYS = re.compile(r'^(linux-vdso|ld-linux|libc\.|libm\.|libmvec|libdl\.|libpthread|librt\.|'
                 r'libutil\.|libresolv|libstdc\+\+|libgcc_s)')
GRAPHICS = re.compile(r'^(libGL|libEGL|libOpenGL|libGLX|libGLdispatch|libX11|libXau|libXdmcp|libxcb|'
                      r'libXext|libXrender|libxkbcommon|libwayland|libdrm|libSM|libICE|libuuid|'
                      r'libfontconfig|libfreetype|libharfbuzz|libgraphite2|libdbus-1|libsystemd|'
                      r'libglib-2\.0|libgio|libgobject|libgmodule|libffi|libpcre2|libexpat|'
                      r'libpng16|libbrotli)')
env = dict(os.environ)
if os.path.isdir(deps):
    env['LD_LIBRARY_PATH'] = os.path.join(deps, 'lib')

exe = os.path.join(appdir, 'usr/bin/photoset-reader')
seen, todo = {}, [exe]
while todo:
    p = todo.pop()
    if not os.path.exists(p):
        continue
    out = subprocess.run(['ldd', p], capture_output=True, text=True, env=env).stdout
    for line in out.splitlines():
        m = re.search(r'=>\s+(\S+)', line)
        path = m.group(1) if m else None
        if not path:
            m2 = re.match(r'\s*(\S+\.so[^\s]*)\s+\(', line)
            path = m2.group(1) if m2 else None
        if not path or not os.path.exists(path):
            continue
        name = os.path.basename(path)
        if name in seen:
            continue
        seen[name] = path
        todo.append(path)

n = 0
for name, path in seen.items():
    if SYS.match(name) or GRAPHICS.match(name):
        continue
    dst = os.path.join(appdir, 'usr/lib', name)
    shutil.copy2(os.path.realpath(path), dst)
    subprocess.run(['strip', '--strip-unneeded', dst], capture_output=True)
    n += 1
print(f"  打进 AppImage 的动态库：{n} 个")
PY

# ── AppRun / 桌面入口 / 图标 ─────────────────────────────────────────────
cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE=$(dirname "$(readlink -f "$0")")
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/usr/bin/photoset-reader" "$@"
EOF
chmod +x "$APPDIR/AppRun"

cat > "$APPDIR/photoset-reader.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=photoset-reader
Comment=Image viewer for manga and very large image sets
Exec=photoset-reader %F
Icon=photoset-reader
Terminal=false
Categories=Graphics;Viewer;
MimeType=image/jpeg;image/png;image/webp;image/gif;image/bmp;image/tiff;image/svg+xml;application/zip;application/x-cbz;application/x-cbr;application/x-7z-compressed;
EOF

ICON="$APPDIR/usr/share/icons/hicolor/256x256/apps/photoset-reader.png"
cat > "$ICON.svg" <<'EOF'
<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <rect x="8" y="8" width="240" height="240" rx="44" fill="#18181b"/>
  <rect x="70" y="58" width="98" height="138" rx="10" fill="#e8e8ec"/>
  <rect x="92" y="74" width="100" height="138" rx="10" fill="#a0a0a8" stroke="#18181b" stroke-width="6"/>
</svg>
EOF
# 有 ImageMagick / rsvg 就转成 PNG，否则用 SVG（多数桌面可显示）
if command -v rsvg-convert >/dev/null; then rsvg-convert -w 256 -h 256 "$ICON.svg" -o "$ICON"
elif command -v convert >/dev/null; then convert "$ICON.svg" "$ICON"
fi
cp "$ICON.svg" "$APPDIR/photoset-reader.svg"
cp "$ICON" "$APPDIR/.DirIcon" 2>/dev/null || true

mkdir -p "$(dirname "$OUT")"
cd "$ROOT/.tmp/appimage"
APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 "$TOOL" "$APPDIR" "$OUT" >/dev/null
chmod +x "$OUT"
echo "✓ $OUT  ($(du -h "$OUT" | cut -f1))"
