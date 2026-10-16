#include "scaler.h"

#include <QDebug>
#include <algorithm>
#include <cmath>
#include <vector>

namespace scaler {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kLanczosSupport = 3.0;

// ── sRGB ↔ 线性光 ────────────────────────────────────────────
// 正向用 256 项 LUT（8bit 输入精确）；反向用 4096 项 LUT + 线性插值。
struct Lut {
    float toLinear[256];
    unsigned char toSrgb[4097];

    Lut()
    {
        for (int i = 0; i < 256; ++i) {
            const double c = i / 255.0;
            toLinear[i] = float(c <= 0.04045 ? c / 12.92
                                             : std::pow((c + 0.055) / 1.055, 2.4));
        }
        for (int i = 0; i <= 4096; ++i) {
            const double lin = i / 4096.0;
            const double c = lin <= 0.0031308 ? lin * 12.92
                                              : 1.055 * std::pow(lin, 1.0 / 2.4) - 0.055;
            toSrgb[i] = (unsigned char)std::lround(std::clamp(c, 0.0, 1.0) * 255.0);
        }
    }

    inline unsigned char encode(float lin) const
    {
        if (lin <= 0.0f) return 0;
        if (lin >= 1.0f) return 255;
        return toSrgb[int(lin * 4096.0f)];
    }
};

const Lut &lut()
{
    static const Lut L;
    return L;
}

inline double sinc(double x)
{
    if (x == 0.0) return 1.0;
    const double px = kPi * x;
    return std::sin(px) / px;
}

// Mitchell–Netravali bicubic（B = C = 1/3），支撑 2.0
inline double mitchell(double x)
{
    const double a = std::abs(x);
    if (a < 1.0) return (7.0 * a * a * a - 12.0 * a * a + 16.0 / 3.0) / 6.0;
    if (a < 2.0) return (-7.0 / 3.0 * a * a * a + 12.0 * a * a - 20.0 * a + 32.0 / 3.0) / 6.0;
    return 0.0;
}

inline double lanczosN(double x, int lobes)
{
    const double a = double(lobes);
    if (x <= -a || x >= a) return 0.0;
    return sinc(x) * sinc(x / a);
}

// Mitchell–Netravali 通用式（B/C 参数化）
inline double mitchellBC(double x, double B, double C)
{
    const double a = std::abs(x);
    if (a < 1.0) {
        return ((12.0 - 9.0 * B - 6.0 * C) * a * a * a
                + (-18.0 + 12.0 * B + 6.0 * C) * a * a
                + (6.0 - 2.0 * B)) / 6.0;
    }
    if (a < 2.0) {
        return ((-B - 6.0 * C) * a * a * a
                + (6.0 * B + 30.0 * C) * a * a
                + (-12.0 * B - 48.0 * C) * a
                + (8.0 * B + 24.0 * C)) / 6.0;
    }
    return 0.0;
}

// ── 浮点图像（交错 RGB；单通道时只填 R）─────────────────────
struct FImg {
    int w = 0, h = 0;
    std::vector<float> p;   // w*h*3
    bool gray = false;
};

// 一维抽头
struct Taps {
    int start = 0;
    int count = 0;
    std::vector<float> w;
};

// 缩小要按比例加宽核（足支撑，避免走样/摩尔纹）；放大用原支撑
std::vector<Taps> buildTaps(int srcN, int dstN, const Params &p)
{
    std::vector<Taps> out;
    out.resize(size_t(dstN));   // 注意：写成 out(size_t(dstN)) 会被解析成函数声明
    const double scale = double(srcN) / double(dstN);
    const double fscale = std::max(1.0, scale);

    // 核的选择：显式 filter 优先；Auto 时按档位（Fast → box，其余 → Lanczos(lobes)）
    Filter f = p.filter;
    if (f == Filter::Auto) f = (p.quality == Quality::Fast) ? Filter::Box : Filter::Lanczos;
    const int lobes = qBound(2, p.lobes, 4);
    const double support = (f == Filter::Box) ? 0.5
                         : (f == Filter::Mitchell) ? 2.0
                                                   : double(lobes);
    const double radius = support * fscale;

    for (int i = 0; i < dstN; ++i) {
        const double center = (i + 0.5) * scale - 0.5;
        int lo = int(std::floor(center - radius + 0.5));
        int hi = int(std::ceil(center + radius - 0.5));
        lo = std::max(lo, 0);
        hi = std::min(hi, srcN - 1);
        if (hi < lo) hi = lo;

        Taps t;
        t.start = lo;
        t.count = hi - lo + 1;
        t.w.resize(size_t(t.count));
        double sum = 0.0;
        for (int k = 0; k < t.count; ++k) {
            double w;
            const double u = (lo + k - center) / fscale;
            if (f == Filter::Mitchell)    w = mitchellBC(u, p.mitchellB, p.mitchellC);
            else if (f == Filter::Box)    w = 1.0;          // 面积平均
            else                          w = lanczosN(u, lobes);
            t.w[size_t(k)] = float(w);
            sum += w;
        }
        if (std::abs(sum) > 1e-12)
            for (auto &w : t.w) w = float(w / sum);
        out[size_t(i)] = std::move(t);
    }
    return out;
}

// 水平重采样。wantMinMax 时同时产出每行的 min/max（供 anti-ringing 用）。
void resampleH(const FImg &in, int outW, const Params &p, FImg &out,
               std::vector<float> *mn = nullptr, std::vector<float> *mx = nullptr)
{
    const auto taps = buildTaps(in.w, outW, p);
    out.w = outW;
    out.h = in.h;
    out.gray = in.gray;
    out.p.assign(size_t(outW) * size_t(in.h) * 3, 0.0f);
    if (mn) mn->assign(size_t(outW) * size_t(in.h) * 3, 0.0f);
    if (mx) mx->assign(size_t(outW) * size_t(in.h) * 3, 0.0f);

    for (int y = 0; y < in.h; ++y) {
        const float *src = &in.p[size_t(y) * size_t(in.w) * 3];
        float *dst = &out.p[size_t(y) * size_t(outW) * 3];
        float *dmn = mn ? &(*mn)[size_t(y) * size_t(outW) * 3] : nullptr;
        float *dmx = mx ? &(*mx)[size_t(y) * size_t(outW) * 3] : nullptr;

        for (int x = 0; x < outW; ++x) {
            const Taps &t = taps[size_t(x)];
            float acc[3] = {0, 0, 0};
            float lo[3] = {1e30f, 1e30f, 1e30f};
            float hi[3] = {-1e30f, -1e30f, -1e30f};
            for (int k = 0; k < t.count; ++k) {
                const float *s = &src[size_t(t.start + k) * 3];
                const float w = t.w[size_t(k)];
                for (int c = 0; c < 3; ++c) {
                    const float v = s[c];
                    acc[c] += v * w;
                    lo[c] = std::min(lo[c], v);
                    hi[c] = std::max(hi[c], v);
                }
            }
            for (int c = 0; c < 3; ++c) {
                dst[x * 3 + c] = acc[c];
                if (dmn) { dmn[x * 3 + c] = lo[c]; dmx[x * 3 + c] = hi[c]; }
            }
        }
    }
}

// 垂直重采样。antiRing 时把结果夹到源邻域 min/max（抑制 Lanczos 负瓣振铃）。
void resampleV(const FImg &in, int outH, const Params &p, FImg &out,
               const std::vector<float> *mn = nullptr,
               const std::vector<float> *mx = nullptr, bool antiRing = false)
{
    const auto taps = buildTaps(in.h, outH, p);
    out.w = in.w;
    out.h = outH;
    out.gray = in.gray;
    out.p.assign(size_t(in.w) * size_t(outH) * 3, 0.0f);

    std::vector<float> vmn, vmx;
    if (antiRing && mn && mx) {
        vmn.assign(size_t(in.w) * size_t(outH) * 3, 0.0f);
        vmx.assign(size_t(in.w) * size_t(outH) * 3, 0.0f);
    }

    for (int y = 0; y < outH; ++y) {
        const Taps &t = taps[size_t(y)];
        for (int x = 0; x < in.w; ++x) {
            float acc[3] = {0, 0, 0};
            float lo[3] = {1e30f, 1e30f, 1e30f};
            float hi[3] = {-1e30f, -1e30f, -1e30f};
            for (int k = 0; k < t.count; ++k) {
                const size_t off = (size_t(t.start + k) * size_t(in.w) + size_t(x)) * 3;
                const float w = t.w[size_t(k)];
                for (int c = 0; c < 3; ++c) {
                    const float v = in.p[off + size_t(c)];
                    acc[c] += v * w;
                    if (antiRing && mn && mx) {
                        lo[c] = std::min(lo[c], (*mn)[off + size_t(c)]);
                        hi[c] = std::max(hi[c], (*mx)[off + size_t(c)]);
                    }
                }
            }
            const size_t doff = (size_t(y) * size_t(in.w) + size_t(x)) * 3;
            for (int c = 0; c < 3; ++c) {
                float v = acc[c];
                if (antiRing && mn && mx) {
                    const float clamped = std::clamp(v, lo[c], hi[c]);
                    v = float(v + (clamped - v) * p.antiring);   // 强度 0 = 关，1 = 全钳
                }
                out.p[doff + size_t(c)] = v;
            }
        }
    }
}

FImg toLinear(const QImage &img, bool &wasGray)
{
    FImg f;
    wasGray = (img.format() == QImage::Format_Grayscale8);
    const QImage src = wasGray ? img : img.convertToFormat(QImage::Format_RGB32);
    f.w = src.width();
    f.h = src.height();
    f.gray = wasGray;
    f.p.assign(size_t(f.w) * size_t(f.h) * 3, 0.0f);
    const Lut &L = lut();

    for (int y = 0; y < f.h; ++y) {
        const uchar *line = src.constScanLine(y);
        float *dst = &f.p[size_t(y) * size_t(f.w) * 3];
        if (wasGray) {
            for (int x = 0; x < f.w; ++x) {
                const float v = L.toLinear[line[x]];
                dst[x * 3] = dst[x * 3 + 1] = dst[x * 3 + 2] = v;
            }
        } else {
            const QRgb *row = reinterpret_cast<const QRgb *>(line);
            for (int x = 0; x < f.w; ++x) {
                dst[x * 3]     = L.toLinear[qRed(row[x])];
                dst[x * 3 + 1] = L.toLinear[qGreen(row[x])];
                dst[x * 3 + 2] = L.toLinear[qBlue(row[x])];
            }
        }
    }
    return f;
}

QImage toSrgb(const FImg &f, bool gray)
{
    const Lut &L = lut();
    if (gray) {
        QImage out(f.w, f.h, QImage::Format_Grayscale8);
        for (int y = 0; y < f.h; ++y) {
            uchar *line = out.scanLine(y);
            const float *src = &f.p[size_t(y) * size_t(f.w) * 3];
            for (int x = 0; x < f.w; ++x) line[x] = L.encode(src[x * 3]);
        }
        return out;
    }
    QImage out(f.w, f.h, QImage::Format_RGB32);
    for (int y = 0; y < f.h; ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(out.scanLine(y));
        const float *src = &f.p[size_t(y) * size_t(f.w) * 3];
        for (int x = 0; x < f.w; ++x) {
            row[x] = qRgb(L.encode(src[x * 3]), L.encode(src[x * 3 + 1]),
                          L.encode(src[x * 3 + 2]));
        }
    }
    return out;
}

QSize fitInside(const QSize &src, const QSize &box)
{
    if (!box.isValid() || box.isEmpty()) return src;
    return src.scaled(box, Qt::KeepAspectRatio);
}

} // namespace

QImage resize(const QImage &src, const QSize &box, const Params &p)
{
    if (src.isNull()) return QImage();
    const QSize target = fitInside(src.size(), box);
    if (!target.isValid() || target.isEmpty()) return src;
    if (target == src.size() && p.mode != Mode::QtSmooth) return src;

    switch (p.mode) {
    case Mode::QtSmooth:
        // 对照路径：Qt 的缩放（**非线性空间**）—— 仅用于对比验证
        return src.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    case Mode::Nearest:
        return src.scaled(target, Qt::KeepAspectRatio, Qt::FastTransformation);

    case Mode::LinearLight:
        break;
    }

    bool gray = false;
    const FImg lin = toLinear(src, gray);

    FImg h;
    std::vector<float> mn, mx;
    const bool wantAntiRing = (p.quality == Quality::Best && p.antiring > 0.0);

    if (p.quality == Quality::Best) {
        resampleH(lin, target.width(), p, h, &mn, &mx);
    } else {
        resampleH(lin, target.width(), p, h);
    }

    FImg v;
    if (p.quality == Quality::Best)
        resampleV(h, target.height(), p, v, &mn, &mx, wantAntiRing);
    else
        resampleV(h, target.height(), p, v);

    return toSrgb(v, gray);
}

const char *modeName(Mode m)
{
    switch (m) {
    case Mode::Nearest:     return "nearest";
    case Mode::QtSmooth:    return "qt-smooth";
    case Mode::LinearLight: return "linear-light";
    }
    return "?";
}

const char *filterName(Filter f)
{
    switch (f) {
    case Filter::Auto:     return "auto";
    case Filter::Box:      return "box";
    case Filter::Lanczos:  return "lanczos";
    case Filter::Mitchell: return "mitchell";
    }
    return "?";
}

// ── 预设（唯一事实源）────────────────────────────────────────
Params paramsFromStrings(const QString &mode, const QString &quality, const QString &filter,
                         int lobes, double mitchellB, double mitchellC, double antiring)
{
    Params p;
    p.mode = (mode == QLatin1String("nearest"))  ? Mode::Nearest
           : (mode == QLatin1String("qt-smooth")) ? Mode::QtSmooth
                                                  : Mode::LinearLight;
    p.quality = (quality == QLatin1String("fast")) ? Quality::Fast
              : (quality == QLatin1String("best")) ? Quality::Best
                                                   : Quality::Balanced;
    p.filter = (filter == QLatin1String("box"))      ? Filter::Box
             : (filter == QLatin1String("lanczos"))  ? Filter::Lanczos
             : (filter == QLatin1String("mitchell")) ? Filter::Mitchell
                                                     : Filter::Auto;
    p.lobes = qBound(2, lobes, 4);
    p.mitchellB = mitchellB;
    p.mitchellC = mitchellC;
    p.antiring = qBound(0.0, antiring, 1.0);
    return p;
}

const QVector<Preset> &presets()
{
    static const QVector<Preset> P = [] {
        QVector<Preset> v;
        {   // 平衡：**出厂默认**（= 旧默认档位：线性光 + Lanczos3，行为不变）
            Preset x{ "balanced", "平衡（默认）", "线性光 + Lanczos3；通用，行为与旧默认一致", {} };
            v << x;
        }
        {   // 漫画：线稿要锐、网点不能起摩尔纹；放大用抗振铃压住晕
            Preset x{ "manga", "漫画（+抗振铃）", "线稿锐利、网点不起摩尔纹、放大不振铃", {} };
            x.params.filter = Filter::Lanczos;
            x.params.lobes = 3;
            x.params.quality = Quality::Best;
            x.params.antiring = 1.0;
            v << x;
        }
        {   // 照片：Mitchell 振铃远比 Lanczos 小，观感自然
            Preset x{ "photo", "照片", "振铃小、观感自然（Mitchell bicubic）", {} };
            x.params.filter = Filter::Mitchell;
            x.params.mitchellB = 1.0 / 3.0;
            x.params.mitchellC = 1.0 / 3.0;
            x.params.quality = Quality::Balanced;
            v << x;
        }
        {   // 锐利：Lanczos4 更锐，代价是振铃更多
            Preset x{ "sharp", "锐利", "Lanczos4：更锐，振铃更多", {} };
            x.params.filter = Filter::Lanczos;
            x.params.lobes = 4;
            x.params.quality = Quality::Best;
            x.params.antiring = 1.0;
            v << x;
        }
        {   // 柔和：B 样条（B=1,C=0）——无振铃、最柔
            Preset x{ "soft", "柔和", "B 样条：无振铃、最柔", {} };
            x.params.filter = Filter::Mitchell;
            x.params.mitchellB = 1.0;
            x.params.mitchellC = 0.0;
            x.params.quality = Quality::Balanced;
            v << x;
        }
        {   // 锐利（Catmull-Rom）：B=0,C=1/2 —— 比 Mitchell 锐，轻微振铃
            Preset x{ "catmull", "锐利（Catmull-Rom）", "B=0,C=1/2：比 Mitchell 锐、轻微振铃", {} };
            x.params.filter = Filter::Mitchell;
            x.params.mitchellB = 0.0;
            x.params.mitchellC = 0.5;
            x.params.quality = Quality::Balanced;
            v << x;
        }
        {   // 快速：面积平均
            Preset x{ "fast", "快速（面积平均）", "大比例缩小时够用且最快", {} };
            x.params.filter = Filter::Box;
            x.params.quality = Quality::Fast;
            v << x;
        }
        return v;
    }();
    return P;
}

const Preset *presetByKey(const QString &key)
{
    for (const Preset &p : presets())
        if (key == QLatin1String(p.key)) return &p;
    return nullptr;
}

const Preset *presetFor(const Params &q)
{
    for (const Preset &p : presets()) {
        const Params &a = p.params;
        if (a.mode == q.mode && a.quality == q.quality && a.filter == q.filter
            && a.lobes == q.lobes
            && std::abs(a.mitchellB - q.mitchellB) < 1e-9
            && std::abs(a.mitchellC - q.mitchellC) < 1e-9
            && std::abs(a.antiring - q.antiring) < 1e-9)
            return &p;
    }
    return nullptr;
}

const char *qualityName(Quality q)
{
    switch (q) {
    case Quality::Fast:     return "fast";
    case Quality::Balanced: return "balanced";
    case Quality::Best:     return "best";
    }
    return "?";
}

} // namespace scaler
