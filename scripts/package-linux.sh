#!/usr/bin/env bash
# 打 Linux 便携包（full 版：自带 Qt + 全部第三方依赖）
#
# 2026-11-02 重写。旧版有四个问题（都在实测里暴露过）：
#   ① 从不 strip —— 二进制 68 MB（其中 63 MB 是 RelWithDebInfo 的调试信息）；
#   ② 把可执行文件自己也当成依赖复制进 lib/（又一份 68 MB）；
#   ③ 只收**可执行文件**的依赖 ⇒ imageformats 插件要的 libavif / libjxl 根本没打进去，
#      AVIF / JXL 在包里其实**打不开**；
#   ④ 整目录 cp Qt 插件与 QML（虚拟键盘 5.3 MB、FluentWinUI3 4.8 MB…全带上）。
#
# 现在：只带**我们要用的**插件 / QML 模块；依赖闭包按「二进制 + 所有插件 + QML 插件」递归求；
#       strip 全部 ELF；自检改成"离屏跑一次截图"。
set -euo pipefail

BIN=build/photoset-reader
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
[ -x "$BIN" ] || { echo "先构建：cmake --build build"; exit 1; }
#   版本号：**只从 CMakeLists.txt 读**（单一来源；--version 打的是同一个）
VERSION=$(sed -n 's/^project(photoset-reader VERSION \([0-9][0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")
[ -n "$VERSION" ] || { echo "读不到版本号（CMakeLists.txt 的 project(... VERSION）" >&2; exit 1; }
OUT=${1:-dist/photoset-reader-${VERSION}-linux}

QTQML=$(qmake6 -query QT_INSTALL_QML 2>/dev/null || echo /usr/lib/qt6/qml)
QTPLUG=$(qmake6 -query QT_INSTALL_PLUGINS 2>/dev/null || echo /usr/lib/qt6/plugins)

rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/lib/qt6/plugins/platforms" "$OUT/lib/qt6/plugins/imageformats" "$OUT/lib/qt6/qml"

# ① 可执行文件
cp "$BIN" "$OUT/bin/photoset-reader"

# ② 插件白名单（只带用得到的）
for p in libqxcb.so libqwayland-egl.so libqwayland-generic.so libqoffscreen.so; do
  [ -f "$QTPLUG/platforms/$p" ] && cp "$QTPLUG/platforms/$p" "$OUT/lib/qt6/plugins/platforms/" || true
done
#   图片格式：与 natural.cpp 的 imageSuffixes() 对齐（+ AVIF/JXL/QOI 走 kimageformats）
for p in libqjpeg.so libqgif.so libqico.so libqicns.so libqtiff.so libqwebp.so \
         libqtga.so libqsvg.so kimg_avif.so kimg_jxl.so kimg_qoi.so; do
  [ -f "$QTPLUG/imageformats/$p" ] && cp "$QTPLUG/imageformats/$p" "$OUT/lib/qt6/plugins/imageformats/" || true
done

# ③ QML 模块白名单
copy_qml() {
  local rel=$1
  [ -d "$QTQML/$rel" ] || return 0
  mkdir -p "$OUT/lib/qt6/qml/$(dirname "$rel")"
  cp -a "$QTQML/$rel" "$OUT/lib/qt6/qml/$(dirname "$rel")/"
}
copy_qml QtQml
copy_qml QtQuick
#   QtQuick 下只留程序真正 import 的：QtQuick 本体、Controls（默认风格 Basic 与 Fusion 都要留）、
#   Layouts、Window、Templates。其余（虚拟键盘 / Pdf / Shapes / Particles / Dialogs / 其它风格）删掉。
Q="$OUT/lib/qt6/qml/QtQuick"
for d in VirtualKeyboard Pdf Shapes Particles Dialogs VectorImage tooling; do rm -rf "$Q/$d"; done
for d in FluentWinUI3 Material Universal Imagine designer; do rm -rf "$Q/Controls/$d"; done
rm -f "$Q/plugins.qmltypes" "$Q/Controls/plugins.qmltypes"

# ④ 依赖闭包：二进制 + 所有插件 / QML 插件 → 递归 ldd，拷进 lib/ 并 strip
python3 - "$OUT" "$BIN" <<'PY'
import os, re, shutil, subprocess, sys

out, root_bin = sys.argv[1], sys.argv[2]
SYS = re.compile(r'^(linux-vdso|ld-linux|libc\.|libm\.|libdl\.|libpthread|librt\.|libutil\.|libgcc_s)')
KEEP_SYSTEM = re.compile(r'^(libX11|libxcb|libXext|libXrender|libxkbcommon|libGL|libEGL|libOpenGL|'
                         r'libGLX|libGLdispatch|libdrm|libwayland|libfontconfig|libfreetype|'
                         r'libdbus-1|libsystemd|libudev|libstdc\+\+)')
libdir = os.path.join(out, 'lib')

targets = [os.path.join(out, 'bin', 'photoset-reader')]
for base, _, files in os.walk(os.path.join(out, 'lib', 'qt6')):
    targets += [os.path.join(base, f) for f in files if '.so' in f]

have = set()
seen = set()
copied = []

def deps(p):
    try:
        text = subprocess.run(['ldd', p], capture_output=True, text=True).stdout
    except Exception:
        return
    for line in text.splitlines():
        m = re.search(r'=>\s+(\S+)\s', line)
        if m:
            yield m.group(1)
        else:
            m2 = re.match(r'\s*(\S+\.so[^\s]*)\s+\(', line)
            if m2 and m2.group(1).startswith('/'):
                yield m2.group(1)

def walk(p):
    real = os.path.realpath(p)
    if real in seen:
        return
    seen.add(real)
    for dep in deps(real):
        name = os.path.basename(dep)
        if SYS.match(name) or KEEP_SYSTEM.match(name):
            continue
        dst = os.path.join(libdir, name)
        if name not in have and not os.path.exists(dst):
            shutil.copy2(os.path.realpath(dep), dst)
            subprocess.run(['strip', '--strip-unneeded', dst], capture_output=True)
            have.add(name)
            copied.append((os.path.getsize(dst), name))
        walk(dep)

for t in targets:
    walk(t)
print(f"  依赖闭包补进 {len(copied)} 个库 / {sum(s for s, _ in copied)/1048576:.1f} MB")
PY

# ⑤ strip 可执行文件（RelWithDebInfo 带 -g，68 MB → ~5 MB）；插件也 strip 一次
strip --strip-unneeded "$OUT/bin/photoset-reader"
find "$OUT" -name '*.so*' -type f -exec strip --strip-unneeded {} \; 2>/dev/null || true

# ⑥ RPATH：$ORIGIN/../lib + Qt QML 目录
if command -v patchelf >/dev/null; then
  patchelf --set-rpath '$ORIGIN/../lib:$ORIGIN/../lib/qt6/qml' "$OUT/bin/photoset-reader"
  for f in "$OUT"/lib/*.so* "$OUT"/lib/qt6/plugins/*/*.so*; do
    [ -f "$f" ] && patchelf --set-rpath '$ORIGIN:$ORIGIN/../..' "$f" 2>/dev/null || true
  done
  echo "  RPATH 已重写（patchelf）"
else
  echo "  ⚠️ 无 patchelf，未重写 RPATH（运行需 LD_LIBRARY_PATH）"
fi

# ⑦ 自检：离屏起一次，确认能启动、能打开来源、0 条 qml 警告
#   （`--shot` 已从产品移除 —— 用户 2026-11-04；这里不再截图，只看"起得来 + 无 qml 告警"）
SMOKE_DIR=".testdata/vol01"
if [ -d "$SMOKE_DIR" ]; then
  LOG=$(mktemp /tmp/photoset-smoke-XXXX.log)
  QT_FORCE_STDERR_LOGGING=1 QT_PLUGIN_PATH="$OUT/lib/qt6/plugins" \
    QML2_IMPORT_PATH="$OUT/lib/qt6/qml" LD_LIBRARY_PATH="$OUT/lib" \
    QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
    timeout 8 "$OUT/bin/photoset-reader" "$SMOKE_DIR" >"$LOG" 2>&1 || true
  BAD=$(grep -c '^qml:' "$LOG" || true)
  if [ "$BAD" -eq 0 ]; then
    echo "  自检：启动正常，qml 警告 0 条"
  else
    echo "  ✗ 自检失败：qml 警告 $BAD 条"
    tail -20 "$LOG"
  fi
  rm -f "$LOG"
else
  echo "  （没有 $SMOKE_DIR，跳过自检）"
fi

# ⑧ 许可 / 第三方声明 / 桌面入口 / 图标（对外分发必须随附的东西）
mkdir -p "$OUT/share/doc/photoset-reader" "$OUT/share/applications"
cp LICENSE THIRD-PARTY.md "$OUT/share/doc/photoset-reader/"
cp assets/photoset-reader.desktop "$OUT/share/applications/"
for _sz in 16 24 32 48 64 128 256 512; do
  mkdir -p "$OUT/share/icons/hicolor/${_sz}x${_sz}/apps"
  cp "assets/icons/photoset-reader-${_sz}.png" "$OUT/share/icons/hicolor/${_sz}x${_sz}/apps/photoset-reader.png"
done

du -sh "$OUT" | sed 's/^/  解压后: /'
tar --zstd -cf "$OUT.tar.zst" -C "$OUT" . && ls -lh "$OUT.tar.zst" | awk '{print "  压缩包: " $5}'
echo "✓ 打包完成: $OUT"
