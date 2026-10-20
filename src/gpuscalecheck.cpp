#include "gpuscalecheck.h"

#include "gpuscale.h"
#include "headlessgl.h"
#include "rawgldriver.h"
#include "gpuscale.h"

#include <QBuffer>
#include <QDebug>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QtGui/rhi/qrhi.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace gpuscalecheck {

Diff compare(const QImage &aIn, const QImage &bIn)
{
    Diff d;
    if (aIn.isNull() || bIn.isNull() || aIn.size() != bIn.size())
        return d;
    const QImage a = aIn.convertToFormat(QImage::Format_RGB32);
    const QImage b = bIn.convertToFormat(QImage::Format_RGB32);
    qint64 sum = 0, n = 0, obvious = 0;
    int mx = 0;
    for (int y = 0; y < a.height(); ++y) {
        const QRgb *ra = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const QRgb *rb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            const int dr = qAbs(qRed(ra[x]) - qRed(rb[x]));
            const int dg = qAbs(qGreen(ra[x]) - qGreen(rb[x]));
            const int db = qAbs(qBlue(ra[x]) - qBlue(rb[x]));
            const int s = dr + dg + db;
            sum += s;
            n += 3;
            if (s > 6) ++obvious;
            mx = std::max(mx, std::max(dr, std::max(dg, db)));
        }
    }
    d.meanAbs = n ? double(sum) / double(n) : -1;
    d.pctObvious = (a.width() && a.height())
                       ? 100.0 * double(obvious) / double(qint64(a.width()) * qint64(a.height()))
                       : -1;
    d.maxChan = mx;
    d.pixels = qint64(a.width()) * qint64(a.height());
    return d;
}


QString text(const Diff &d)
{
    if (d.meanAbs < 0)
        return QStringLiteral("—");
    return QStringLiteral("%1 / %2% / max%3")
        .arg(d.meanAbs, 0, 'f', 2)
        .arg(d.pctObvious, 0, 'f', 2)
        .arg(d.maxChan);
}

} // namespace gpuscalecheck


int runGpuScaleProbe()
{
    QString err;
    headlessgl::Context *gl = headlessgl::Context::create(&err);
    QRhi *rhi = gl ? headlessgl::createRhi(gl, &err) : nullptr;
    if (rhi) {
        qWarning().noquote() << QStringLiteral("[probe] 本环境有 QRhi，probe 走 raw GL 需要无 QRhi 环境");
        return 2;
    }
    rawgl::Driver *raw = rawgl::Driver::create(&err);
    if (!raw) {
        qWarning().noquote() << QStringLiteral("[probe] 建不出 GL：%1").arg(err);
        return 2;
    }
    qInfo().noquote() << QStringLiteral("[probe] 设备 = %1").arg(raw->rendererInfo());

    // 8×8：每行一个灰度（行号 0..7 → 0,32,64,...,224），再放一个不对称标记
    QImage src(8, 8, QImage::Format_RGB32);
    for (int y = 0; y < 8; ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(src.scanLine(y));
        for (int x = 0; x < 8; ++x) {
            const int v = y * 32 + (x == 0 ? 0 : 0);
            row[x] = qRgb(v, v, v);
        }
    }
    src.setPixel(0, 0, qRgb(255, 0, 0));      // 左上角标记（判朝向）
    src.setPixel(7, 7, qRgb(0, 0, 255));      // 右下角标记

    gpuscale::Params p;   // 默认算法（Lanczos 3）—— 1:1 直通时核退化为恒等，正好验朝向/通道
    const gpuscale::Plan plan = gpuscale::makePlan(src.size(), src.size(), gpuscale::Fit::Original,
                                                   1.0, 0, 0, p);
    QImage gpu;
    if (!raw->render(src, plan, &gpu, &err)) {
        qWarning().noquote() << QStringLiteral("[probe] 渲染失败：%1").arg(err);
        return 3;
    }
    auto dump = [](const char *what, const QImage &img) {
        const QImage c = img.convertToFormat(QImage::Format_RGB32);
        QStringList rows;
        for (int y = 0; y < c.height(); ++y) {
            QStringList cols;
            for (int x = 0; x < c.width(); ++x) {
                const QRgb px = c.pixel(x, y);
                cols << QStringLiteral("%1/%2/%3").arg(qRed(px)).arg(qGreen(px)).arg(qBlue(px));
            }
            rows << cols.join(QLatin1Char(' '));
        }
        qInfo().noquote() << QStringLiteral("[probe] %1（行 0 = 上）:\n%2")
                                 .arg(QLatin1String(what), rows.join(QLatin1String("\n")));
    };
    raw->dumpProgramInfo();
    dump("源图", src);
    dump("GPU 输出", gpu);
    const gpuscalecheck::Diff d = gpuscalecheck::compare(src, gpu);
    qInfo().noquote() << QStringLiteral("[probe] 1:1 直通色差 = %1").arg(gpuscalecheck::text(d));
    delete raw;
    return 0;
}

