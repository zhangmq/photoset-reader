// ══ ib-bench-scale：CPU 缩放的**延迟**实测 ═══════════════════════════════════
//   背景（2026-11-07 用户）：软件渲染路径目前只有 Qt 内置平滑（`CpuScalerItem` 用
//   `QPainter::drawImage` + `SmoothPixmapTransform`），Nearest/Bicubic/Lanczos2/3
//   只存在 GPU shader 里。想评估"软件路径也上高级算法"是否可行 ⇒ 先量延迟。
//
//   这个工具**自己实现**可分离卷积（bicubic = Catmull-Rom a=-0.5，与 GPU kernel 一致；
//   lanczos2/3 = sinc 窗），再和 Qt 的平滑做对照。**只是实验**：不进产品、不进测试。
//
//   用法：
//     ib-bench-scale [--iters N] [--size WxH] [--out WxH]
//   默认：8 次取中位数，输入 1200x1700（漫画页典型），输出 1080x1530（1080P 级别等比）。
//
//   输出全英文走 stdout（避免 GBK 终端乱码）。

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

// ── 采样核 ────────────────────────────────────────────────────────────────
double cubicWeight(double x)                 // Catmull-Rom（a = -0.5，与 GPU 一致）
{
    x = std::fabs(x);
    const double a = -0.5;
    if (x <= 1.0) return ((a + 2) * x - (a + 3)) * x * x + 1;
    if (x < 2.0)  return (((x - 5) * x + 8) * x - 4) * a;
    return 0.0;
}

double sinc(double x)
{
    if (std::fabs(x) < 1e-8) return 1.0;
    const double px = M_PI * x;
    return std::sin(px) / px;
}

double lanczosWeight(double x, int lobes)
{
    x = std::fabs(x);
    if (x >= lobes) return 0.0;
    return sinc(x) * sinc(x / lobes);
}

struct Kernel
{
    const char *name = "";
    double (*fn)(double) = nullptr;
    int     radius = 0;                      // 单侧抽头数（不含中心）
};

//   预计算**权重表**：每个输出像素的 (首抽头, 权重列表)，避免内层重复算 sinc。
struct Tap { int first; std::vector<double> w; };

std::vector<Tap> buildTaps(int inSize, int outSize, double (*fn)(double), int radius)
{
    std::vector<Tap> taps(static_cast<size_t>(outSize));   // 注意：写成 `taps(size_t(n))` 会被当成函数声明
    const double ratio = double(inSize) / double(outSize);
    const double scale = std::max(1.0, ratio);        // 缩小 ⇒ 核要变宽（抗锯齿）
    for (int o = 0; o < outSize; ++o) {
        const double center = (o + 0.5) * ratio - 0.5;
        const int    r      = int(std::ceil(radius * scale));
        int first = int(std::floor(center)) - r + 1;
        Tap t;
        t.first = first;
        t.w.resize(size_t(2 * r));
        double sum = 0;
        for (int i = 0; i < 2 * r; ++i) {
            const int   s = first + i;
            const double d = (s - center) / scale;
            const double w = fn(d);
            t.w[size_t(i)] = w;
            if (s >= 0 && s < inSize) sum += w;
        }
        if (sum != 0)
            for (double &w : t.w) w /= sum;
        taps[size_t(o)] = std::move(t);
    }
    return taps;
}

QImage resampleSeparable(const QImage &src, const QSize &dst,
                         double (*fn)(double), int radius)
{
    const QImage in = src.convertToFormat(QImage::Format_ARGB32);
    const int iw = in.width(), ih = in.height();
    const int ow = dst.width(), oh = dst.height();

    const auto tapsX = buildTaps(iw, ow, fn, radius);
    const auto tapsY = buildTaps(ih, oh, fn, radius);

    //   横向：iw×ih → ow×ih（float RGBA）
    std::vector<float> mid(size_t(ow) * size_t(ih) * 4);
    for (int y = 0; y < ih; ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(in.constScanLine(y));
        for (int x = 0; x < ow; ++x) {
            const Tap &t = tapsX[size_t(x)];
            double r = 0, g = 0, b = 0, a = 0;
            for (size_t i = 0; i < t.w.size(); ++i) {
                const int s = t.first + int(i);
                if (s < 0 || s >= iw) continue;
                const double w = t.w[i];
                const QRgb p = line[s];
                r += w * qRed(p); g += w * qGreen(p); b += w * qBlue(p); a += w * qAlpha(p);
            }
            float *out = &mid[(size_t(y) * size_t(ow) + size_t(x)) * 4];
            out[0] = float(r); out[1] = float(g); out[2] = float(b); out[3] = float(a);
        }
    }
    //   纵向：ow×ih → ow×oh
    QImage out(ow, oh, QImage::Format_ARGB32);
    for (int y = 0; y < oh; ++y) {
        const Tap &t = tapsY[size_t(y)];
        auto *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < ow; ++x) {
            double r = 0, g = 0, b = 0, a = 0;
            for (size_t i = 0; i < t.w.size(); ++i) {
                const int s = t.first + int(i);
                if (s < 0 || s >= ih) continue;
                const double w = t.w[i];
                const float *p = &mid[(size_t(s) * size_t(ow) + size_t(x)) * 4];
                r += w * p[0]; g += w * p[1]; b += w * p[2]; a += w * p[3];
            }
            line[x] = qRgba(int(std::lround(std::clamp(r, 0.0, 255.0))),
                            int(std::lround(std::clamp(g, 0.0, 255.0))),
                            int(std::lround(std::clamp(b, 0.0, 255.0))),
                            int(std::lround(std::clamp(a, 0.0, 255.0))));
        }
    }
    return out;
}

QImage resampleNearest(const QImage &src, const QSize &dst)
{
    return src.scaled(dst, Qt::IgnoreAspectRatio, Qt::FastTransformation);
}

double median(std::vector<double> v)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

QSize parseSize(const QString &s, const QSize &fallback)
{
    const QStringList parts = s.split(QLatin1Char('x'));
    if (parts.size() != 2) return fallback;
    bool ok1 = false, ok2 = false;
    const int w = parts[0].toInt(&ok1), h = parts[1].toInt(&ok2);
    return (ok1 && ok2 && w > 0 && h > 0) ? QSize(w, h) : fallback;
}

}  // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    int iters = 8;
    QSize inSize(1200, 1700);
    QSize outSize(1080, 1530);

    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--iters") && i + 1 < args.size())
            iters = std::clamp(args[++i].toInt(), 1, 500);
        else if (args[i] == QLatin1String("--size") && i + 1 < args.size())
            inSize = parseSize(args[++i], inSize);
        else if (args[i] == QLatin1String("--out") && i + 1 < args.size())
            outSize = parseSize(args[++i], outSize);
    }

    //   造一张有梯度 + 噪声的图（缩放质量/速度都对内容略敏感，用梯度更接近真实扫描页）
    QImage src(inSize, QImage::Format_ARGB32);
    for (int y = 0; y < inSize.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(src.scanLine(y));
        for (int x = 0; x < inSize.width(); ++x) {
            const int r = (x * 255) / inSize.width();
            const int g = (y * 255) / inSize.height();
            const int b = ((x / 16 + y / 16) % 2) ? 200 : 40;
            line[x] = qRgba(r, g, b, 255);
        }
    }

    std::printf("input %dx%d (%.1f MP) -> output %dx%d (%.1f MP), iters=%d, single thread\n",
                inSize.width(), inSize.height(), inSize.width() * inSize.height() / 1e6,
                outSize.width(), outSize.height(), outSize.width() * outSize.height() / 1e6,
                iters);

    struct Case { const char *name; int kind; double (*fn)(double); int radius; };
    //   kind: 0 = Qt 内置平滑（现状）／1 = nearest／2 = 卷积核
    const Case cases[] = {
        { "Smooth (Qt, current)", 0, nullptr, 0 },
        { "Nearest",              1, nullptr, 0 },
        { "Bicubic (Catmull-Rom)", 2, cubicWeight, 2 },
        { "Lanczos 2",            2, [](double x) { return lanczosWeight(x, 2); }, 2 },
        { "Lanczos 3",            2, [](double x) { return lanczosWeight(x, 3); }, 3 },
    };

    std::printf("%-24s %10s %10s\n", "algorithm", "median ms", "MP/s");
    for (const Case &c : cases) {
        std::vector<double> samples;
        samples.reserve(size_t(iters));
        for (int it = 0; it < iters; ++it) {
            QElapsedTimer t;
            t.start();
            QImage out;
            if (c.kind == 0)
                out = src.scaled(outSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            else if (c.kind == 1)
                out = resampleNearest(src, outSize);
            else
                out = resampleSeparable(src, outSize, c.fn, c.radius);
            samples.push_back(double(t.nsecsElapsed()) / 1e6);
            Q_UNUSED(out);
        }
        const double ms = median(samples);
        const double mp = (outSize.width() * double(outSize.height()) / 1e6) / (ms / 1000.0);
        std::printf("%-24s %10.2f %10.1f\n", c.name, ms, mp);
    }
    return 0;
}
