#include "layout.h"

namespace layout {

const char *kindName(Kind k)
{
    switch (k) {
    case Kind::Single:     return "single";
    case Kind::Spread:     return "spread";
    case Kind::WideSingle: return "wide";
    case Kind::Webtoon:    return "webtoon";
    }
    return "?";
}

Kind classify(const QSize &sz, const Options &opt)
{
    if (!sz.isValid() || sz.isEmpty()) return Kind::Single;
    const double w = sz.width(), h = sz.height();

    // 条漫优先：极端长宽比 或 绝对高度过大
    if (h > w * opt.webtoonRatio || h >= opt.webtoonMinHeight)
        return Kind::Webtoon;

    if (w > h * opt.wideRatio) return Kind::WideSingle;   // 宽图（多为跨页扫描）
    return Kind::Single;
}

QVector<Page> build(const QVector<QSize> &sizes, const Options &opt)
{
    QVector<Page> pages;
    pages.reserve(sizes.size());

    int i = 0;
    const int n = sizes.size();
    while (i < n) {
        const Kind k = classify(sizes.at(i), opt);

        if (k == Kind::Webtoon) {
            Page p; p.kind = Kind::Webtoon; p.items << i; pages << p;
            ++i;
            continue;
        }
        if (k == Kind::WideSingle) {
            Page p; p.kind = Kind::WideSingle; p.items << i; pages << p;
            ++i;
            continue;
        }

        // 竖图：尝试与下一张配对（下一张也必须是竖图）
        if (opt.pairPortrait && i + 1 < n) {
            const Kind k2 = classify(sizes.at(i + 1), opt);
            if (k2 == Kind::Single) {
                Page p;
                p.kind = Kind::Spread;
                p.rtlPair = opt.rtl;
                // RTL：右页在前 → 先显示后一张（右边那张）
                if (opt.rtl) { p.items << i + 1 << i; }
                else         { p.items << i << i + 1; }
                pages << p;
                i += 2;
                continue;
            }
        }

        Page p; p.kind = Kind::Single; p.items << i; pages << p;
        ++i;
    }
    return pages;
}

} // namespace layout
