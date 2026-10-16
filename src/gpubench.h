#pragma once

#include <QSize>
#include <QStringList>

#include "scaler.h"

// ══ GPU 缩放实验（docs/research/12）════════════════════════════
//   问题：GPU 做「上传 + 缩放」到底要多久？与 CPU 缩放器（~160-195 ms/张）比差多少？
//   口径：**解码仍在 CPU**（GPU 只替换缩放那一段）。流水线结构与 CPU 版同构：
//     上传 → ①线性化 → ②水平 Lanczos → ③垂直 Lanczos + 回 sRGB → 回读
//   回读只在实验里做（生产不需要，直接上屏）。
//
//   返回 0 = 成功；非 0 = 失败（并把原因打到 stderr）。
int runGpuScaleBench(const QStringList &imagePaths, const QSize &box, int repeats,
                     const scaler::Params &params);
