#!/usr/bin/env bash
# 打 Linux 便携包：收集依赖闭包 → 重写 RPATH → 自检
#
# 已知体积构成（本机实测，2026-10-10）：
#   非系统库 78 个 / 117.8 MB
#     libicudata      31.6 MB  ← 最大项（37%）
#     Qt6 Gui/Quick/Core/Qml  ~30 MB
#     libcrypto        6.6 MB  ← OpenSSL，纯离线应用用不到
#     icu i18n/uc      6.1 MB
#   ⇒ 若自建 Qt 加 -no-icu -no-openssl，可省约 45 MB（见 docs/research/08）
set -euo pipefail

BIN=build/image-browser
OUT=${1:-dist/image-browser-linux}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

[ -x "$BIN" ] || { echo "先构建：cmake --build build"; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/lib" "$OUT/bin"

# ① 收集依赖闭包（排除系统保证存在的库）
python3 - "$BIN" "$OUT" <<'PY'
import subprocess, os, re, shutil, sys
binpath, out = sys.argv[1], sys.argv[2]
SYS = re.compile(r'^(linux-vdso|ld-linux|libc\.|libm\.|libgcc_s|libstdc\+\+|libdl\.|'
                 r'libpthread|librt\.|libutil\.|libX11|libxcb|libXext|libXrender|'
                 r'libxkbcommon|libGL|libEGL|libOpenGL|libGLX|libdrm|libwayland|'
                 r'libfontconfig|libfreetype|libdbus-1|libsystemd|libudev)')
def deps(p, seen):
    if p in seen or not os.path.exists(p): return
    seen.add(p)
    try: o = subprocess.run(['ldd', p], capture_output=True, text=True).stdout
    except Exception: return
    for line in o.splitlines():
        m = re.search(r'=>\s+(\S+)', line)
        if m: deps(m.group(1), seen)
        else:
            m2 = re.match(r'\s*(\S+\.so[^\s]*)\s+\(', line)
            if m2 and m2.group(1).startswith('/'): deps(m2.group(1), seen)
seen = set(); deps(binpath, seen)
n = 0; tot = 0
for p in sorted(seen):
    b = os.path.basename(p)
    if SYS.match(b): continue
    dst = os.path.join(out, 'lib', b)
    shutil.copy2(os.path.realpath(p), dst)
    n += 1; tot += os.path.getsize(dst)
print(f"  收集 {n} 个库 / {tot/1048576:.1f} MB")
PY

cp "$BIN" "$OUT/bin/"
# Qt 插件（平台 + QML 模块）——Qt 需要
QTQML=$(qmake6 -query QT_INSTALL_QML 2>/dev/null || echo /usr/lib/qt6/qml)
QTPLUG=$(qmake6 -query QT_INSTALL_PLUGINS 2>/dev/null || echo /usr/lib/qt6/plugins)
for m in QtQuick QtQml QtQuick/Controls QtQuick/Layouts QtQuick/Window QtQuick/Templates QtCore; do
  [ -d "$QTQML/$m" ] && mkdir -p "$OUT/lib/qt6/qml/$(dirname "$m")" && cp -r "$QTQML/$m" "$OUT/lib/qt6/qml/$(dirname "$m")/" 2>/dev/null || true
done
for p in platforms imageformats; do
  [ -d "$QTPLUG/$p" ] && mkdir -p "$OUT/lib/qt6/plugins" && cp -r "$QTPLUG/$p" "$OUT/lib/qt6/plugins/" 2>/dev/null || true
done

# ② 重写 RPATH（$ORIGIN/../lib + Qt 子目录）
if command -v patchelf >/dev/null; then
  patchelf --set-rpath '$ORIGIN/../lib:$ORIGIN/../lib/qt6/qml' "$OUT/bin/image-browser"
  for f in "$OUT"/lib/*.so*; do [ -f "$f" ] && patchelf --set-rpath '$ORIGIN' "$f" 2>/dev/null || true; done
  echo "  RPATH 已重写（patchelf）"
else
  echo "  ⚠️ 无 patchelf，未重写 RPATH（可用 LD_LIBRARY_PATH 运行）"
fi

# ③ 自检：不依赖系统 Qt 也能启动
echo "  自检："
PASS=$(QT_FORCE_STDERR_LOGGING=1 QT_PLUGIN_PATH="$OUT/lib/qt6/plugins" \
       QML2_IMPORT_PATH="$OUT/lib/qt6/qml" LD_LIBRARY_PATH="$OUT/lib" \
       QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
       "$OUT/bin/image-browser" --selftest 2>&1 | grep -c PASS || true)
echo "    selftest PASS: $PASS"
[ "$PASS" -ge 9 ] || { echo "  ✗ 打包自检未通过"; exit 1; }

du -sh "$OUT" | sed 's/^/  包体积: /'
echo "✓ 打包完成: $OUT"
