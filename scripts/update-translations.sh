#!/bin/bash
# 更新翻译文件（.ts）：一条命令跑完 lupdate + 完整性检查。
#
# ⚠️ 为什么要有这个脚本：lupdate 只认**你显式给它的文件**。漏掉某个文件里的
#   `tr()` / `qsTr()` → 对应条目变成 `type="vanished"` → lrelease **不编进 .qm**
#   → 运行时查不到 → 静默回退源串（英文）。
#   （同样的坑在 vsr-player 上踩过：FrucBackend.h 没进 lupdate 输入 ⇒ 11 条 vanished。）
#
# 用法：
#   ./scripts/update-translations.sh          # 更新 .ts（之后手工补译文）
#   ./scripts/update-translations.sh --check  # 检查：源码里每个 UI 字面量都在 .ts 里，
#                                             #       且中文没有 unfinished
#
# 加一门新语言：把语言代码加进下面的 LANGS，然后跑一次本脚本，
#   得到 `i18n/photoset-reader_<代码>.ts`；翻译完直接 `cmake --build build`
#   （CMake 里 GLOB 了 i18n/*.ts，lrelease 会把 .qm 编进资源）。
#   **C++/QML 代码零改动** —— 运行期按语言代码去 `:/i18n/` 找 .qm。

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

# Qt 工具：优先 PATH，其次发行版的 Qt6 bin（Arch 是 /usr/lib/qt6/bin）
find_tool() {
    local name="$1" ; local env="$2"
    if [ -n "${!env:-}" ] && [ -x "${!env}" ]; then echo "${!env}"; return; fi
    if command -v "$name" >/dev/null 2>&1; then command -v "$name"; return; fi
    for c in /usr/lib/qt6/bin/$name /usr/lib/x86_64-linux-gnu/qt6/bin/$name \
             /usr/lib64/qt6/bin/$name /opt/Qt/6.*/gcc_64/bin/$name; do
        [ -x "$c" ] && { echo "$c"; return; }
    done
    echo "找不到 $name（可用 $env 指定）" >&2
    exit 1
}
LUPDATE="$(find_tool lupdate LUPDATE)"
LCONVERT="$(find_tool lconvert LCONVERT)"

LANGS=(zh_CN)
APP=photoset-reader
I18N="$PROJECT_ROOT/i18n"

# ── lupdate 的输入清单 ───────────────────────────────────────────────
# 新增**带界面文案**的文件时必须加到这里（QML 的 context 按文件/组件名，不按目录）。
INPUTS=(
    "$PROJECT_ROOT"/src/ui/*.qml
    "$PROJECT_ROOT"/src/ui/components/*.qml
    "$PROJECT_ROOT"/src/ui/fullscreen/*.qml
    "$PROJECT_ROOT"/src/uimodel.cpp
    "$PROJECT_ROOT"/src/viewermodel.cpp
    "$PROJECT_ROOT"/src/sequence.cpp
    "$PROJECT_ROOT"/src/exifmeta.cpp
)

# 源码里的界面字面量：tr("…") / qsTr("…")
# 这检查针对"字面量压根没进 .ts"（漏文件 / tr(变量) 没标记），比数 vanished 更准。
missing_literals() {
    local ts="$1"
    python3 - "$ts" "${INPUTS[@]}" <<'PYEOF'
import re, sys
ts, files = sys.argv[1], sys.argv[2:]
have = set(re.findall(r'<source>(.*?)</source>', open(ts, encoding='utf-8').read(), re.S))
pat = re.compile(r'(?<![A-Za-z0-9_])(?:qsTr|tr)\(\s*"((?:[^"\\]|\\.)*)"\s*\)')
missing, total = [], 0
for f in files:
    src = open(f, encoding='utf-8').read()
    for m in pat.finditer(src):
        lit = m.group(1)
        if not lit:
            continue
        # 源码里是转义写法（\n / \"），.ts 里存的是真实字符 ⇒ 先还原再比对
        lit = (lit.replace('\\\\', '\x00')
                  .replace('\\n', '\n')
                  .replace('\\t', '\t')
                  .replace('\\"', '"')
                  .replace('\x00', '\\'))
        total += 1
        if lit not in have:
            missing.append(f"{f}: {lit!r}")
print(f"  源码字面量 {total} 个，缺条目 {len(missing)} 个")
for x in missing:
    print("    ✗", x)
sys.exit(1 if missing else 0)
PYEOF
}

if [[ "${1:-}" == "--check" ]]; then
    echo "翻译完整性检查："
    for lang in "${LANGS[@]}"; do
        ts="$I18N/${APP}_${lang}.ts"
        printf "  %s: vanished=%s unfinished=%s\n" "$(basename "$ts")" \
            "$(grep -c 'type="vanished"' "$ts" || true)" \
            "$(grep -c 'type="unfinished"' "$ts" || true)"
    done
    if ! missing_literals "$I18N/${APP}_${LANGS[0]}.ts"; then
        echo "✗ 有界面字面量没进 .ts：lrelease 不会编进 .qm，运行时会显示源串（英文）。" >&2
        exit 1
    fi
    # 中文是唯一验收标准（其它语言允许 unfinished → 回退英文源串）
    if grep -q 'type="unfinished"' "$I18N/${APP}_zh_CN.ts" 2>/dev/null; then
        echo "✗ zh_CN 存在 unfinished 条目（运行时会回退英文）" >&2
        exit 1
    fi
    echo "✓ zh_CN 完整"
    exit 0
fi

mkdir -p "$I18N"
TS_ARGS=()
for lang in "${LANGS[@]}"; do TS_ARGS+=("$I18N/${APP}_${lang}.ts"); done

"$LUPDATE" -no-obsolete "${INPUTS[@]}" -ts "${TS_ARGS[@]}"

echo
echo "更新完成：${TS_ARGS[*]}"
echo "补完译文后：cmake --build build && ./scripts/update-translations.sh --check"
