#include "gpuscalecheck.h"

#include "gpuscale.h"
#include "headlessgl.h"
#include "rawgldriver.h"
#include "scaler.h"

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

bool pass(const Diff &d)
{
    return d.meanAbs >= 0 && d.meanAbs <= 0.2 && d.pctObvious < 0.005;   // 0.00%
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

namespace {

using gpuscalecheck::Diff;

// ① 首选：让 **Qt/QPA 自己建 GL 上下文**（QRhiGles2InitParams::newFallbackSurface + QRhi::create）
//    —— 这就是 Qt Quick 生产路径的建法（真机有 GPU 时走这条）。
QRhi *createRhiViaQpa(QString *err)
{
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setVersion(4, 5);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    QRhiGles2InitParams params;
    params.format = fmt;
    QOffscreenSurface *surf = QRhiGles2InitParams::newFallbackSurface(fmt);
    params.fallbackSurface = surf;      // QRhi 负责销毁？不：调用方负责，故存起来
    QRhi *rhi = QRhi::create(QRhi::OpenGLES2, &params);
    if (!rhi) {
        delete surf;
        if (err) *err = QStringLiteral("QRhi::create（走 QPA 的 GL 上下文）失败");
        return nullptr;
    }
    // surf 必须活到 QRhi 之后销毁（自检只建一次 → 进程生命周期持有，不做回调）
    static QOffscreenSurface *hold = nullptr;
    delete hold;
    hold = surf;
    return rhi;
}
using gpuscalecheck::compare;
using gpuscalecheck::pass;
using gpuscalecheck::text;

// 读图（**不做 shrink-on-load**：设计 13 §3.1 定案 —— 解码结果按原尺寸）
QImage loadFull(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QImage();
    const QByteArray bytes = f.readAll();
    QBuffer buf;
    buf.setData(bytes);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    reader.setAutoTransform(true);
    return reader.read();
}

struct Row {
    QString what;
    QSize src, dst;
    Diff diff;
    bool ok = false;
    QString note;
};

} // namespace

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

    scaler::Params p = scaler::params(scaler::Mode::LinearLight, scaler::Quality::Balanced);
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

int runGpuScaleCheck(const QStringList &imagePaths, int repeats)
{
    Q_UNUSED(repeats);

    QString err;
    // ① Qt/QPA 自己建（真机的生产路径）→ ② EGL surfaceless 导入 → ③ raw GL 自检驱动
    QRhi *rhi = createRhiViaQpa(&err);
    const char *how = "QRhi（Qt/QPA 建 GL 上下文）";
    headlessgl::Context *gl = nullptr;
    if (!rhi) {
        qInfo().noquote() << QStringLiteral("[gpucheck] QPA 建不出 GL（%1）→ 试 EGL surfaceless 导入").arg(err);
        gl = headlessgl::Context::create(&err);
        rhi = gl ? headlessgl::createRhi(gl, &err) : nullptr;
        if (rhi)
            how = "QRhi（导入 EGL surfaceless 上下文）";
    }
    gpuscale::Pipeline *pipe = nullptr;
    rawgl::Driver *raw = nullptr;
    if (rhi) {
        qInfo().noquote() << QStringLiteral("[gpucheck] 建法 = %1").arg(QLatin1String(how));
        qInfo().noquote() << QStringLiteral("[gpucheck] 后端 = OpenGL(RHI)  设备 = %1")
                                 .arg(rhi->driverInfo().deviceName.isEmpty()
                                          ? QStringLiteral("?")
                                          : QString::fromUtf8(rhi->driverInfo().deviceName));
        qInfo().noquote() << QStringLiteral("[gpucheck] QRhi 属性：Y-up NDC = %1 / Y-up framebuffer = %2")
                                 .arg(rhi->isYUpInNDC() ? QStringLiteral("是") : QStringLiteral("否"),
                                      rhi->isYUpInFramebuffer() ? QStringLiteral("是") : QStringLiteral("否"));
        pipe = new gpuscale::Pipeline(rhi);
        if (!pipe->isValid()) {
            qWarning().noquote() << QStringLiteral("[gpucheck] QRhi 管线不可用：%1").arg(pipe->error());
            return 3;
        }
    } else {
        // 本沙箱/CI：Qt 的 QPA 没有 EGL 平台（offscreen 只有 GLX、minimalegl 要 window surface），
        // 所以 QRhi 建不出来。退到**自检专用的 raw GL 驱动**：同一份 qsb shader + 同一份 Plan/
        // passUniformsFor，只换「谁发这 3 个 draw」。QRhi 资源路径本身必须在有设备节点的机器上验。
        QString e;
        raw = rawgl::Driver::create(&e);
        if (!raw) {
            qWarning().noquote() << QStringLiteral("[gpucheck] 建不出任何 GL 上下文：%1").arg(e);
            qWarning().noquote() << QStringLiteral("[gpucheck] （真机上 Qt Quick 自己会建 RHI；请在有 GPU/设备节点的环境跑这条自检）");
            return 2;
        }
        qInfo().noquote() << QStringLiteral("[gpucheck] 后端 = OpenGL(raw GL 自检驱动)  设备 = %1").arg(raw->rendererInfo());
        qInfo().noquote() << QStringLiteral("[gpucheck] ⚠ 本环境建不出 QRhi（QPA 无 GL / 平台不是 EGL 平台）→ "
                                           "退到 raw GL 自检驱动：验证的是「同一份 qsb shader + 同一份 Plan」"
                                           "的逐像素等价；**QRhi 资源路径本身**需在有 GL 的机器上复跑本自检"
                                           "（那时会自动走 ① QPA 建 QRhi 那条）");
    }

    std::vector<Row> rows;
    int failures = 0;

    // 离屏跑一次：返回回读到的 QImage（RGBA8888）
    auto renderOffscreen = [&](const QImage &src32, const gpuscale::Plan &plan, QImage *out,
                               QString *why) -> bool {
        if (raw)
            return raw->render(src32, plan, out, why);
        pipe->setSourceImage(src32);
        if (pipe->sourceSize() != plan.src) {
            if (why) *why = QStringLiteral("源尺寸不符");
            return false;
        }
        QRhiCommandBuffer *cb = nullptr;
        if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) {
            if (why) *why = QStringLiteral("beginOffscreenFrame 失败");
            return false;
        }
        QRhiReadbackResult rb;
        bool done = false;
        rb.completed = [&done]() { done = true; };
        QRhiResourceUpdateBatch *rub = rhi->nextResourceUpdateBatch();
        // 回读在最后一趟的 endPass 里提交（Qt 文档的离屏示例同款）
        const bool ok = pipe->render(cb, rub, plan, nullptr, &rb);
        if (!ok) {
            rhi->endOffscreenFrame();
            if (why) *why = pipe->error();
            return false;
        }
        rhi->endOffscreenFrame();
        rhi->finish();
        if (!done || rb.data.isEmpty()) {
            if (why) *why = QStringLiteral("回读未完成");
            return false;
        }
        const QSize px = rb.pixelSize.isEmpty() ? plan.dst : rb.pixelSize;
        const qsizetype need = qsizetype(px.width()) * px.height() * 4;
        if (rb.data.size() < need) {
            if (why) *why = QStringLiteral("回读字节数不足（%1 < %2）").arg(rb.data.size()).arg(need);
            return false;
        }
        QImage img(px, QImage::Format_RGBA8888);
        std::memcpy(img.bits(), rb.data.constData(), size_t(need));
        // ★ 回读行序按后端约定：QRhi 文档的离屏示例就是
        //     `if (rhi->isYUpInFramebuffer()) image.flip();`
        //   （GL 的 framebuffer 行 0 在下，回读要翻一次才是自上而下的 QImage）
        if (rhi->isYUpInFramebuffer())
            img = img.mirrored(false, true);
        *out = img;
        return true;
    };

    // 朝向自检：1:1 直通（fit=Original，zoom=1）必须**原样**回来
    auto orientationCheck = [&](const QImage &src32) {
        const scaler::Params p = scaler::params(scaler::Mode::LinearLight, scaler::Quality::Balanced);
        const gpuscale::Plan plan = gpuscale::makePlan(src32.size(), src32.size(), gpuscale::Fit::Original,
                                                       1.0, 0, 0, p);
        QImage gpu;
        QString why;
        if (!renderOffscreen(src32, plan, &gpu, &why)) {
            Row r;
            r.what = QStringLiteral("朝向自检（1:1 直通）");
            r.note = why;
            rows.push_back(r);
            ++failures;
            return;
        }
        struct Cand { const char *name; QImage img; };
        const QVector<Cand> cands = { { "\u539f\u6837", gpu },
                                      { "\u4e0a\u4e0b\u7ffb", gpu.mirrored(false, true) },
                                      { "\u5de6\u53f3\u7ffb", gpu.mirrored(true, false) },
                                      { "\u4e0a\u4e0b+\u5de6\u53f3\u7ffb", gpu.mirrored(true, true) } };
        Diff best;
        QString bestName = QStringLiteral("?");
        for (const Cand &c : cands) {
            const Diff d = compare(src32, c.img);
            if (best.meanAbs < 0 || d.meanAbs < best.meanAbs) {
                best = d;
                bestName = QString::fromUtf8(c.name);
            }
        }
        Row r;
        r.what = QStringLiteral("朝向自检（1:1 直通 vs 源图）");
        r.src = src32.size();
        r.dst = plan.dst;
        r.diff = best;
        r.note = QStringLiteral("最佳朝向 = %1").arg(bestName);
        r.ok = (bestName == QStringLiteral("原样")) && pass(best);
        if (!r.ok)
            ++failures;
        rows.push_back(r);
    };

    int presetCount = 0;
    for (const QString &path : imagePaths) {
        QImage raw = loadFull(path);
        if (raw.isNull()) {
            qWarning().noquote() << QStringLiteral("[gpucheck] 跳过（读不出）：%1").arg(path);
            continue;
        }
        const QString name = QFileInfo(path).fileName();

        // 素材 A：整页（真素材，不预缩）；素材 B：从整页裁 1200×1200（真素材的小图）
        struct Mat { QString label; QImage img; };
        std::vector<Mat> mats;
        mats.push_back({ QStringLiteral("整页 %1x%2").arg(raw.width()).arg(raw.height()), raw });
        {
            const int cw = std::min(1200, raw.width());
            const int ch = std::min(1200, raw.height());
            const int x = (raw.width() - cw) / 2;
            const int y = (raw.height() - ch) / 2;
            if (cw >= 256 && ch >= 256)
                mats.push_back({ QStringLiteral("裁 %1x%2").arg(cw).arg(ch), raw.copy(x, y, cw, ch) });
        }

        if (rows.empty() || rows.back().what.startsWith(QStringLiteral("朝向自检"))) {
            orientationCheck(mats.front().img.convertToFormat(QImage::Format_RGB32));
        }

        for (const Mat &m : mats) {
            const QImage src32 = m.img.convertToFormat(QImage::Format_RGB32);
            // 两个方向：放大（盒 = 源 × 2）、缩小（盒 = 源 × 0.5）
            struct Dir { const char *label; double k; };
            const Dir dirs[2] = { { "\u653e\u5927", 2.0 }, { "\u7f29\u5c0f", 0.5 } };
            for (const Dir &dir : dirs) {
                const QSize box(std::max(16, int(src32.width() * dir.k)),
                                std::max(16, int(src32.height() * dir.k)));
                const QSize target = src32.size().scaled(box, Qt::KeepAspectRatio);
                for (const scaler::Preset &preset : scaler::presets()) {
                    ++presetCount;
                    Row r;
                    r.what = QStringLiteral("%1 · %2 · %3 · %4")
                                 .arg(name, m.label, QString::fromUtf8(dir.label), QLatin1String(preset.key));
                    r.src = src32.size();
                    r.dst = target;

                    const gpuscale::Plan plan = gpuscale::makePlan(src32.size(), box, gpuscale::Fit::Screen,
                                                                   1.0, 0, 0, preset.params);
                    if (plan.full != target) {
                        r.note = QStringLiteral("目标尺寸口径不一致：plan=%1x%2 Qt=%3x%4")
                                     .arg(plan.full.width()).arg(plan.full.height())
                                     .arg(target.width()).arg(target.height());
                        ++failures;
                        rows.push_back(r);
                        continue;
                    }
                    QImage gpu;
                    QString why;
                    if (!renderOffscreen(src32, plan, &gpu, &why)) {
                        r.note = why;
                        ++failures;
                        rows.push_back(r);
                        continue;
                    }
                    const QImage cpu = scaler::resize(src32, box, preset.params);
                    r.diff = compare(cpu, gpu);
                    r.ok = pass(r.diff);
                    if (!r.ok)
                        ++failures;
                    // 可选写入磁盘（供人工目检）：PHOTOSET_GPUCHECK_DUMP=<目录>
                    //   落的是 **GPU 侧真实产物**（raw GL 自检驱动或 QRhi 管线）+ CPU 参照 + 放大 20× 的差异图
                    if (const QByteArray dd = qgetenv("PHOTOSET_GPUCHECK_DUMP"); !dd.isEmpty()) {
                        const QString base = QStringLiteral("%1/%2-%3-%4-%5")
                                                 .arg(QString::fromLocal8Bit(dd))
                                                 .arg(name.section(QLatin1Char('.'), 0, 0))
                                                 .arg(m.label).arg(QString::fromUtf8(dir.label))
                                                 .arg(QLatin1String(preset.key));
                        gpu.save(base + QStringLiteral("-gpu.png"));
                        cpu.save(base + QStringLiteral("-cpu.png"));
                        QImage diffImg(cpu.size(), QImage::Format_RGB32);
                        // ⚠ 必须先落到具名变量：`gpu.convertToFormat(...).constScanLine(y)`
                        //   取的是**临时对象**的行指针，表达式结束即悬垂（实测段错误）
                        const QImage gpu32 = gpu.convertToFormat(QImage::Format_RGB32);
                        for (int y = 0; y < cpu.height(); ++y) {
                            const QRgb *ra = reinterpret_cast<const QRgb *>(cpu.constScanLine(y));
                            const QRgb *rb = reinterpret_cast<const QRgb *>(gpu32.constScanLine(y));
                            QRgb *rd = reinterpret_cast<QRgb *>(diffImg.scanLine(y));
                            for (int x = 0; x < cpu.width(); ++x)
                                rd[x] = qRgb(qMin(255, qAbs(qRed(ra[x]) - qRed(rb[x])) * 20),
                                             qMin(255, qAbs(qGreen(ra[x]) - qGreen(rb[x])) * 20),
                                             qMin(255, qAbs(qBlue(ra[x]) - qBlue(rb[x])) * 20));
                        }
                        diffImg.save(base + QStringLiteral("-diff20.png"));
                        qInfo().noquote() << QStringLiteral("[gpucheck] 已写入磁盘 %1-{gpu,cpu,diff20}.png").arg(base);
                    }
                    rows.push_back(r);
                }
            }

            // pan：输出空间偏移 500,300（视口 800×800），与 CPU 完整结果的对应窗口比
            {
                const scaler::Preset *pManga = scaler::presetByKey(QStringLiteral("manga"));
                const scaler::Preset *pBal = scaler::presetByKey(QStringLiteral("balanced"));
                const scaler::Params ps[2] = { pManga->params, pBal->params };
                const QSize full(2 * src32.width(), 2 * src32.height());
                const QSize vp(800, 800);
                if (full.width() > vp.width() && full.height() > vp.height()) {
                    for (const scaler::Params &p : ps) {
                        Row r;
                        r.what = QStringLiteral("%1 · %2 · pan(500,300) 2×放大 · %3").arg(name, m.label)
                                     .arg(QLatin1String(scaler::presetFor(p) ? scaler::presetFor(p)->key : "custom"));
                        r.src = src32.size();
                        const gpuscale::Plan plan = gpuscale::makePlan(src32.size(), vp, gpuscale::Fit::Screen,
                                                                       1.0, 0, 0, p);
                        // fit=Screen 会装进 800×800；这里要的是 2× 放大后取窗口 → 显式构造
                        gpuscale::Plan panPlan = plan;
                        panPlan.full = full;
                        panPlan.dst = vp;
                        panPlan.offX = 500;
                        panPlan.offY = 300;
                        panPlan.fscaleX = std::max(1.0f, float(src32.width()) / float(full.width()));
                        panPlan.fscaleY = std::max(1.0f, float(src32.height()) / float(full.height()));
                        const float support = (panPlan.filterIdx == 1) ? 0.5f
                                            : (panPlan.filterIdx == 2) ? 2.0f : float(panPlan.lobes);
                        panPlan.radiusX = support * panPlan.fscaleX;
                        panPlan.radiusY = support * panPlan.fscaleY;
                        r.dst = vp;
                        QImage gpu;
                        QString why;
                        if (!renderOffscreen(src32, panPlan, &gpu, &why)) {
                            r.note = why;
                            ++failures;
                            rows.push_back(r);
                            continue;
                        }
                        const QImage cpuFull = scaler::resize(src32, full, p);
                        const QImage cpuWin = cpuFull.copy(500, 300, vp.width(), vp.height());
                        r.diff = compare(cpuWin, gpu);
                        r.ok = pass(r.diff);
                        if (!r.ok)
                            ++failures;
                        rows.push_back(r);
                    }
                }
            }

            // 像素完美：整数倍 + nearest ⇒ 必须与「源像素 2×2 复制」逐位相同
            {
                Row r;
                r.what = QStringLiteral("%1 · %2 · 像素完美 nearest 2×").arg(name, m.label);
                r.src = src32.size();
                const QSize full(2 * src32.width(), 2 * src32.height());
                scaler::Params p;
                p.mode = scaler::Mode::Nearest;
                const gpuscale::Plan plan = gpuscale::makePlan(src32.size(), full, gpuscale::Fit::Original,
                                                               2.0, 0, 0, p);
                r.dst = plan.dst;
                QImage gpu;
                QString why;
                if (!renderOffscreen(src32, plan, &gpu, &why)) {
                    r.note = why;
                    ++failures;
                    rows.push_back(r);
                    continue;
                }
                QImage expect(full, QImage::Format_RGB32);
                for (int y = 0; y < full.height(); ++y) {
                    QRgb *row = reinterpret_cast<QRgb *>(expect.scanLine(y));
                    const QRgb *srow = reinterpret_cast<const QRgb *>(src32.constScanLine(y / 2));
                    for (int x = 0; x < full.width(); ++x)
                        row[x] = srow[x / 2];
                }
                r.diff = compare(expect, gpu);
                r.ok = pass(r.diff);
                if (!r.ok)
                    ++failures;
                rows.push_back(r);
            }
        }
    }

    // ── 报表 ───────────────────────────────────────────────────────
    qInfo().noquote() << QStringLiteral("[gpucheck] 判定条件：平均色差 ≤ 0.2/255 且「明显不同」(三通道合计>6) = 0.00%%");
    qInfo().noquote() << QStringLiteral("[gpucheck] 用例\t源\t出\t色差(平均/明显/max通道)\t结果\t备注");
    for (const Row &r : rows) {
        qInfo().noquote() << QStringLiteral("[gpucheck] %1\t%2x%3\t%4x%5\t%6\t%7\t%8")
                                 .arg(r.what)
                                 .arg(r.src.width()).arg(r.src.height())
                                 .arg(r.dst.width()).arg(r.dst.height())
                                 .arg(text(r.diff))
                                 .arg(r.ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
                                 .arg(r.note);
    }
    int passed = 0;
    for (const Row &r : rows)
        if (r.ok)
            ++passed;
    qInfo().noquote() << QStringLiteral("[gpucheck] 合计 %1 项：PASS %2 / FAIL %3（含 %4 个预设用例）")
                             .arg(rows.size()).arg(passed).arg(rows.size() - passed).arg(presetCount);
    delete pipe;
    delete raw;
    return failures == 0 ? 0 : 1;
}
