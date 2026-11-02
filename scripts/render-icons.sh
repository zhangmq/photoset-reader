#!/bin/bash
# 从 assets/icons/photoset-reader.svg 渲染各尺寸 PNG（hicolor / AppImage / 窗口图标都从这里取）。
#
# 为什么 PNG 要进仓库：安装/打包环节不该依赖 rsvg-convert 是否存在
#   （发行版打包机常常只有 Qt 工具）。SVG 是**唯一源**，改完跑一次本脚本刷新 PNG。
#
# 用法： ./scripts/render-icons.sh          # 重新渲染
#        ./scripts/render-icons.sh --check  # 只检查 PNG 是否比 SVG 旧（CI/提交前用）
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SVG="$ROOT/assets/icons/photoset-reader.svg"
SIZES=(16 24 32 48 64 128 256 512)

if [[ "${1:-}" == "--check" ]]; then
    stale=0
    for s in "${SIZES[@]}"; do
        png="$ROOT/assets/icons/photoset-reader-${s}.png"
        if [[ ! -f "$png" ]] || [[ "$SVG" -nt "$png" ]]; then
            echo "✗ 过期或缺失：$(basename "$png")"; stale=1
        fi
    done
    [[ $stale -eq 0 ]] && echo "✓ 图标 PNG 与 SVG 同步"
    exit $stale
fi

command -v rsvg-convert >/dev/null || { echo "需要 rsvg-convert（librsvg）" >&2; exit 1; }
for s in "${SIZES[@]}"; do
    rsvg-convert -w "$s" -h "$s" "$SVG" -o "$ROOT/assets/icons/photoset-reader-${s}.png"
done
echo "✓ 已渲染 ${#SIZES[@]} 个尺寸到 assets/icons/"
