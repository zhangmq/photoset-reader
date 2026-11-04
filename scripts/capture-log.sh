#!/usr/bin/env bash
# ══ 单次启动，只留关键日志（排查用）════════════════════════════════════════════
#   ./scripts/capture-log.sh                  # 跟平时一样启动
#   ./scripts/capture-log.sh ~/漫画            # 也可以直接带路径（位置参数）
#
#   只做一件事：开着 debug 跑**一次**应用，把**关键行**写进当前目录的
#   photoset-log-<时间>.log —— 退出后把那个文件发我即可。
#   完整日志不保留（噪音太大）；日志里不会有密码值（代码从不打印密码）。
set -uo pipefail

APP="$(cd "$(dirname "$0")/.." && pwd)/build/photoset-reader"
[[ -x "$APP" ]] || { echo "还没有构建产物：先跑 cmake --build build" >&2; exit 1; }

OUT="photoset-log-$(date +%Y%m%d-%H%M%S).log"
echo "关键日志 → $OUT"

QT_FORCE_STDERR_LOGGING=1 PHOTOSET_LOG=debug "$APP" "$@" 2>&1 \
    | grep --line-buffered -E 'password|rescan|unsupported|progress (restored|saved)|autosave|'\
'favorite|viewer: screen|bad|skipped|error|failed|warning|qml:|dev:' \
    | grep --line-buffered -vE 'No QRhi|MESA|EGL|solid archive' \
    | tee "$OUT"

echo "把 $OUT 发我。"
