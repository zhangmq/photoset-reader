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

inline double lanczos3(double x)
{
    if (x <= -kLanczosSupport || x >= kLanczosSupport) return 0.0;
    return sinc(x) * sinc(x / kLanczosSupport);
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
std::vector<Taps> buildTaps(int srcN, int dstN, Quality q)
{
    std::vector<Taps> out;
    out.resize(size_t(dstN));   // 注意：写成 out(size_t(dstN)) 会被解析成函数声明
    const double scale = double(srcN) / double(dstN);
    const double fscale = std::max(1.0, scale);
    const double radius = (q == Quality::Fast ? 0.5 : kLanczosSupport) * fscale;

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
            if (q == Quality::Fast) {
                w = 1.0;                                   // box（面积平均）
            } else {
                w = lanczos3((lo + k - center) / fscale);
            }
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
void resampleH(const FImg &in, int outW, Quality q, FImg &out,
               std::vector<float> *mn = nullptr, std::vector<float> *mx = nullptr)
{
    const auto taps = buildTaps(in.w, outW, q);
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
void resampleV(const FImg &in, int outH, Quality q, FImg &out,
               const std::vector<float> *mn = nullptr,
               const std::vector<float> *mx = nullptr, bool antiRing = false)
{
    const auto taps = buildTaps(in.h, outH, q);
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
                if (antiRing && mn && mx) v = std::clamp(v, lo[c], hi[c]);
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

QImage resize(const QImage &src, const QSize &box, Mode mode, Quality quality)
{
    if (src.isNull()) return QImage();
    const QSize target = fitInside(src.size(), box);
    if (!target.isValid() || target.isEmpty()) return src;
    if (target == src.size() && mode != Mode::QtSmooth) return src;

    switch (mode) {
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

    // 大比例缩小时先做 box 预缩，再 Lanczos 收尾：更快且更少振铃
    FImg h;
    std::vector<float> mn, mx;
    const bool wantAntiRing = (quality == Quality::Best);

    if (quality == Quality::Best) {
        resampleH(lin, target.width(), quality, h, &mn, &mx);
    } else {
        resampleH(lin, target.width(), quality, h);
    }

    FImg v;
    if (quality == Quality::Best)
        resampleV(h, target.height(), quality, v, &mn, &mx, wantAntiRing);
    else
        resampleV(h, target.height(), quality, v);

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
