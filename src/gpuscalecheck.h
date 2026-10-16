#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

// ══ GPU 缩放 × CPU 缩放的**逐像素等价性对照**（离屏，`--selftest-gpuscale`）══════
//
// 口径与 12 号调研 / `--bench-gpu` 相同：
//   · 同一张源图（先 convertToFormat(RGB32)）、同一个目标盒、同一组 scaler::Params；
//   · CPU 侧 = `scaler::resize`（**唯一参照实现**），GPU 侧 = 产品那条 QRhi 管线
//     （`gpuscale::Pipeline` + qsb 烘焙的 shader）；
//   · 判定条件：**平均色差 ≤ 0.2/255 且「明显不同」（三通道合计 > 6）的像素 = 0.00%**。
//
// 覆盖：6 个预设 × {放大, 缩小} × {1200×1200 小图, vol01 真页}，外加
//   · pan（输出空间偏移）—— 与 CPU 完整结果的对应窗口比（验证偏移不改变采样）
//   · 像素完美通路（整数倍 + nearest）—— 与「源像素 2×2 复制」逐位比
//   · 朝向自检（1:1 直通必须原样回来）
//
// 返回 0 = 全部通过；非 0 = 有失败项（同时把表打到 stderr）。
int runGpuScaleCheck(const QStringList &imagePaths, int repeats);

// 诊断入口（`--gpuscale-probe`）：8×8 已知图跑 1:1 直通，逐像素打印源 vs GPU 输出。
//   用来快速定位「哪一趟错了」，不参与常规自检。
int runGpuScaleProbe();

namespace gpuscalecheck {

// 与 12 号同一套判定条件
struct Diff {
    double meanAbs = -1;       // 三通道平均绝对差（0–255 尺度）
    double pctObvious = -1;    // 三通道合计 > 6 的像素占比 %
    int    maxChan = -1;       // 单通道最大差
    qint64 pixels = 0;
};

Diff compare(const QImage &a, const QImage &b);
QString text(const Diff &d);
bool pass(const Diff &d);      // 平均 ≤ 0.2 且 明显不同 = 0.00%

} // namespace gpuscalecheck
