#pragma once

#include <QVector>
#include <QSize>

// 版面感知（设计 §2.5 / §2.6）。**判定逻辑，不是渲染**——独立成模块，可单测。
//
// 规则（相邻页宽高比组合是最易出错处）：
//   · **宽图**（宽 > 高 × 1.2）→ 单独成页（本项目素材多为跨页扫描，本身就宽）
//   · **两张连续竖图**（高 > 宽）→ 可配成一对（跨页）
//   · **RTL 页序**（日式漫画右到左）：配对时右页在前
//   · 条漫（极端长宽比）不参与配对
namespace layout {

enum class Kind {
    Single,       // 单页
    Spread,       // 两张竖图配成跨页
    WideSingle,   // 宽图单独成页
    Webtoon,      // 极端长宽比，单独滚动
};

struct Page {
    QVector<int> items;      // 1 或 2 个序列索引
    Kind kind = Kind::Single;
    bool rtlPair = false;    // 配对时是否右页在前
};

struct Options {
    bool pairPortrait = true;    // 是否把连续竖图配对
    bool rtl = true;             // 日式右到左
    double wideRatio = 1.2;      // 宽/高 超过此值算"宽图"
    double webtoonRatio = 4.0;   // 高/宽 超过此值算"条漫"
    int webtoonMinHeight = 4000; // 或高度超过此值
};

// 判断单个尺寸的类型
Kind classify(const QSize &sz, const Options &opt = {});

// 由尺寸序列构建页面序列
QVector<Page> build(const QVector<QSize> &sizes, const Options &opt = {});

// 供 UI 显示的短标签（用于单测与诊断）
const char *kindName(Kind k);

} // namespace layout
