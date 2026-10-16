#pragma once

#include <QImage>
#include <QSize>
#include <QString>
#include <QVector>

// 唯一的缩放实现（见 AGENTS.md「依赖策略」）。
//
// 与多数看图器的关键差别：
//   ① **在线性光空间做卷积**（先把 sRGB 转线性，卷积后再转回）
//      —— 非线性空间缩放会让线稿边缘偏暗、漫画网点产生摩尔纹
//   ② 放大与缩小用不同的滤波器支撑（缩小按比例加宽核）
//   ③ 可配置画质档位
//   ④ 提供像素完美通路（整数倍 + 最近邻）
namespace scaler {

enum class Quality {
    Fast,       // 线性光 + box（面积平均）：大比例缩小时足够且很快
    Balanced,   // 线性光 + Lanczos3（默认）
    Best,       // 线性光 + Lanczos3 + anti-ringing（放大时抑制振铃）
};

// 滤波器族（与 Quality 正交：Quality 管"档位/是否 anti-ringing"，Filter 管"用哪个核"）
enum class Filter {
    Auto,       // 按 Quality 走默认：Fast→box，其余→Lanczos(lobes)
    Box,        // 面积平均（支撑 0.5）
    Lanczos,    // Lanczos（支撑 = lobes）
    Mitchell,   // Mitchell–Netravali bicubic（支撑 2.0，B/C 可调）
};

enum class Mode {
    Nearest,    // 像素完美（整数倍时）
    LinearLight,// 我们的实现
    QtSmooth,   // 对照用：Qt 的 SmoothTransformation（非线性）
};

// ── 全部可调参数（**参数才是真相**；预设只是命名组合）──────────
struct Params {
    Mode    mode = Mode::LinearLight;    // 色彩空间 / 通路
    Quality quality = Quality::Balanced; // 档位（Best 时启用 anti-ringing）
    Filter  filter = Filter::Auto;       // 用哪个核
    int     lobes = 3;                   // Lanczos 瓣数（2/3/4）：越大越锐、振铃越多
    double  mitchellB = 1.0 / 3.0;       // Mitchell B/C 参数化
    double  mitchellC = 1.0 / 3.0;       //   B=C=1/3 → Mitchell；B=0,C=1/2 → Catmull-Rom；B=1,C=0 → B 样条
    double  antiring = 1.0;              // 振铃抑制强度 0–1（只在 quality==Best 时生效）
};

// 预设 = 一组参数值的命名组合（**唯一事实源**：UI 与 CLI 都从这里取）
struct Preset {
    const char *key;      // 存进设置的值
    const char *label;    // UI 文案
    const char *desc;     // UI 说明
    Params      params;
};
// 字符串值域 → Params 的**唯一映射**（main.cpp 读配置用它；UI 侧重构后也用它）
Params paramsFromStrings(const QString &mode, const QString &quality, const QString &filter,
                         int lobes, double mitchellB, double mitchellC, double antiring);

// 便捷：只指定 mode/quality（其余取默认）
inline Params params(Mode m, Quality q) { Params p; p.mode = m; p.quality = q; return p; }

const QVector<Preset> &presets();
const Preset *presetByKey(const QString &key);
// 当前参数与哪个预设一致（找不到返回 nullptr → UI 显示"自定义"）
const Preset *presetFor(const Params &p);

// 把 src 缩放到 box 内（保持宽高比），返回结果。失败返回空 QImage。
QImage resize(const QImage &src, const QSize &box, const Params &p);

// 供 UI 显示的可读名称
const char *modeName(Mode m);
const char *qualityName(Quality q);
const char *filterName(Filter f);

} // namespace scaler
