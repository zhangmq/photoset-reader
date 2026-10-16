#pragma once

// ══ bench / 探针（跑完即退）的实现与装配（设计 14 §10）══════════════════════
//   `main.cpp` 只留 `QCommandLineOption` 的声明/解析 + 一次 dispatch；**实现全在这里**。
//   ★ 它们**不是测试**：随 `image-browser` 一起编（不进 `ibcore`、不链 `testsupport`）。
//   ★ 纪律：从 `main.cpp` 搬出来的实现**行为一字不变**（输出文案、退出码都不改）。
//   ★ 底层算法本来就在 `gpubench.cpp` / `gpuscalecheck.cpp` —— 这里只是装配与调用。

#include <QString>

#include "scaler.h"

class ImageProvider;

namespace devbench {

// ── 从 parser 解析出来的请求（**解析留在 `main.cpp`**，这里只承载值）──────
//   ★ 判"给没给"一律用 `xxxSet`（与 `devcli::DevRequest` 同一约定）。
struct Request
{
    bool    thumbsSet = false;      // --bench-thumbs <n>
    int     thumbsN = 0;
    bool    gpuSet = false;         // --bench-gpu <n>
    int     gpuN = 0;
    bool    parallelSet = false;    // --bench-parallel <n>
    int     parallelN = 0;
    bool    gpuProbe = false;       // --gpuscale-probe

    bool    dumpSet = false;        // --dump-scaled <png>（判"给没给"用；决定要不要建来源）
    QString dump;                   // 值（**空串按"没给"处理**，与旧 `!dumpPath.isEmpty()` 同）

    int     index = 0;              // --index <n>（dump 用；内部再 qMax(0, …)）
    bool    boxSet = false;         // --box WxH
    QString box;
    bool    cropSet = false;        // --crop x,y,w,h
    QString crop;
    QString threads;                // --threads <list>
    int     repeats = 1;            // --repeats <n>（内部再 qMax(1, …)）
    bool    noShrink = false;       // --no-shrink
    scaler::Params params;          // 显式 CLI 覆盖后的缩放参数（main.cpp 里算好传进来）

    // 有任何一个"跑完即退"的实验被请求
    bool wanted() const
    {
        return thumbsSet || gpuSet || parallelSet || gpuProbe || !dump.isEmpty();
    }

    // 这些实验**按条目号取图** ⇒ 需要 provider 先建来源（主路径不建，见 main.cpp）
    bool needsProviderSource() const
    {
        return dumpSet || thumbsSet || gpuSet || parallelSet;
    }
};

// 依次跑：gpu → gpu-probe → parallel → thumbs → dump（**与旧 `main.cpp` 同序**）。
//   调用方只在 `wanted()` 为真时调用；返回退出码语义照旧（3/4 是 dump 的解码/存盘失败）。
int runBench(const Request &req, ImageProvider *provider);

} // namespace devbench
