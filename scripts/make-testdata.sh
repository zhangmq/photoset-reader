#!/usr/bin/env bash
# 重建自检所需的测试数据（--selftest-flow 依赖 .testdata/edge/ 下的两个样例）
#
#   .testdata/edge/exif6.jpg    原始 800x600 + EXIF Orientation=6（验证自动旋转与尺寸一致性）
#   .testdata/edge/multi.tiff   2 页多页 TIFF（验证按页容器处理与跳页）
#
# 缺这两个文件时，--selftest-flow 会**跳过**对应检查（不会误报通过）。
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .testdata/edge

python3 - <<'PY'
from PIL import Image
import os

# ① EXIF Orientation=6：拍出来的竖图（原始 800x600，需顺时针转 90°）
im = Image.new('RGB', (800, 600), (30, 200, 30))
e = Image.Exif(); e[274] = 6          # 274 = Orientation
im.save('.testdata/edge/exif6.jpg', exif=e)

# ② 多页 TIFF：两页颜色不同（用于证明"真的跳页了"）
a = Image.new('RGB', (800, 600), (10, 10, 200))    # 蓝
b = Image.new('RGB', (800, 600), (200, 200, 10))   # 黄
a.save('.testdata/edge/multi.tiff', save_all=True, append_images=[b])

print("  ✓ exif6.jpg  (800x600, Orientation=6)")
print("  ✓ multi.tiff (2 页：蓝 / 黄)")
PY

echo "✓ 测试数据已重建于 .testdata/edge/"
