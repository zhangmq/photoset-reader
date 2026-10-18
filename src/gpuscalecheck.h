#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

// ══ GPU 缩放诊断（`--gpuscale-probe`）══════════════════════════════════
//   ★ 2026-10-19：那份"GPU × CPU 逐像素等价性对照"（`runGpuScaleCheck`）**已删** ——
//     它当初是为"把 CPU 缩放算法移植到 GPU"做的一次性校验，移植已完成、
//     且它唯一的调用者（`ib-test-gpuscale`）也一起删了。**这里只剩诊断入口。**
//
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

} // namespace gpuscalecheck
