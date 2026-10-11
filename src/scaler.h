#pragma once

#include <QImage>
#include <QSize>

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

enum class Mode {
    Nearest,    // 像素完美（整数倍时）
    LinearLight,// 我们的实现
    QtSmooth,   // 对照用：Qt 的 SmoothTransformation（非线性）
};

// 把 src 缩放到 box 内（保持宽高比），返回结果。
// 失败返回空 QImage。
QImage resize(const QImage &src, const QSize &box, Mode mode, Quality quality);

// 供 UI 显示的可读名称
const char *modeName(Mode m);
const char *qualityName(Quality q);

} // namespace scaler
