#include "devbench.h"

#include <QAtomicInt>
#include <QBuffer>
#include <QByteArray>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QRect>
#include <QRunnable>
#include <QSize>
#include <QStringList>
#include <QThread>
#include <QThreadPool>
#include <QVector>

#include <algorithm>

#ifdef Q_OS_UNIX
#include <sys/resource.h>
#endif

#include "gpubench.h"
#include "gpuscalecheck.h"
#include "imageprovider.h"

// ══ bench / 探针（跑完即退）的实现与装配（设计 14 §10）══════════════════════
//   从 `main.cpp` **整段搬出**，只把 `parser.isSet/value(xxxOpt)` 换成 `req.xxxSet/req.xxx`；
//   **逻辑与文案一字未改**（含退出码 3/4）。

namespace devbench {

// ── 视口盒：默认按本机 2x 屏的物理视口；可用 `--box` 覆盖 ──────────────────
//   ⚠️ `--box` 自带默认值 1600x1000 → 只有**显式给出**时才采纳它（`boxSet`）。
//   （`--bench-gpu` / `--bench-parallel` 两处原本各写一份**完全相同**的解析，合成一个。）
static QSize viewportBox(const Request &req)
{
    QSize box(3000, 1900);                      // 本机 2x 屏的物理视口
    if (req.boxSet) {
        const QStringList wh = req.box.split(QLatin1Char('x'));
        if (wh.size() == 2 && wh[0].toInt() > 0 && wh[1].toInt() > 0)
            box = QSize(wh[0].toInt(), wh[1].toInt());
    }
    return box;
}

// ── GPU 缩放实验（docs/research/12）：上传 + 分离式 Lanczos（线性光）vs CPU 缩放器 ──
static int runGpuBench(const Request &req, ImageProvider *provider)
{
    const int n = qMax(1, req.gpuN);
    const QSize box = viewportBox(req);
    QStringList paths;
    for (int i = 0; i < qMin(n, provider->count()); ++i) {
        const QString p = provider->absPathAt(i);
        if (!p.isEmpty()) paths << p;
    }
    return runGpuScaleBench(paths, box, qMax(1, req.repeats), req.params);
}

// ── 并行度实验（docs/research/14）：解码 / 解码+缩放 / 只缩放，排除 I/O ──
//
//   问题：把 preloader 的"一次一张"改成"一次一批"（多线程）值不值？
//   口径：**先把 N 张图的字节读进内存（不计时）**，再从内存解码 —— 磁盘 I/O 不进计时。
//   三组分开量：
//     A 解码（含 shrink-on-load，生产口径，**不走**我们的缩放器）
//     B 解码 + 缩放到视口盒（= 生产 `decodeFit` 那条路）
//     C 只缩放（输入是 A 的输出，准备阶段不计时）
static int runParallelBench(const Request &req, ImageProvider *provider)
{
    const int n = qMax(1, req.parallelN);
    const QSize box = viewportBox(req);

    QVector<int> threadList;
    for (const QString &s : req.threads.split(QLatin1Char(','), Qt::SkipEmptyParts))
        if (s.toInt() > 0) threadList << s.toInt();
    if (threadList.isEmpty()) threadList << 1;
    const int repeats = qMax(1, req.repeats);

    // ① 素材：把字节读进内存（**不计时**）
    const int total = qMin(n, provider->count());
    QVector<QByteArray> blobs;
    blobs.reserve(total);
    for (int i = 0; i < total; ++i) {
        QFile f(provider->absPathAt(i));
        if (!f.open(QIODevice::ReadOnly)) continue;
        blobs << f.readAll();
    }
    if (blobs.isEmpty()) { qWarning() << "[bench-parallel] 读不到任何素材"; return 4; }

    // ② C 组的输入：A 的输出（同样不计时）。
    //   ⚠️ 只留 **8 张**：大图（24MP+）一张就 20-25 MB，全留会吃掉上 GB 内存。
    //   缩放器不关心输入的身份，8 张不同输入轮流用，对"缩放吞吐"是等价的
    const bool shrink = !req.noShrink;              // 跟随 --no-shrink
    const int poolMax = qMin(blobs.size(), 8);
    QVector<QImage> predecoded;
    predecoded.reserve(poolMax);
    for (int i = 0; i < poolMax; ++i) {
        QBuffer buf; buf.setData(blobs.at(i)); buf.open(QIODevice::ReadOnly);
        QImageReader r(&buf); r.setAutoTransform(true);
        ImageProvider::prepareReader(r, box, shrink);
        predecoded << r.read();
    }

    auto rssMb = []() -> double {
        QFile f(QStringLiteral("/proc/self/status"));
        if (!f.open(QIODevice::ReadOnly)) return -1;
        for (const QByteArray &l : f.readAll().split('\n'))
            if (l.startsWith("VmRSS:")) return l.mid(6).trimmed().split(' ').first().toDouble()/1024.0;
        return -1;
    };
    auto cpuSec = []() -> double {
        rusage ru{};
        if (getrusage(RUSAGE_SELF, &ru) != 0) return -1;
        return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec/1e6
             + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec/1e6;
    };

    // 工作项：从原子计数器领号，直到领完
    struct Task : QRunnable {
        const QVector<QByteArray> *blobs;
        const QVector<QImage> *predecoded;
        QSize box; int mode; QAtomicInt *next; QAtomicInt *done; bool shrink = true;
        scaler::Params prm;
        Task() { setAutoDelete(false); }        // ★ 同一个对象会被 start() 多次
        void run() override {
            forever {
                const int i = next->fetchAndAddRelaxed(1);
                if (i >= blobs->size()) return;
                QImage out;
                if (mode == 2) {                       // C：只缩放（输入轮转）
                    out = scaler::resize(predecoded->at(i % predecoded->size()), box, prm);
                } else {
                    QBuffer buf; buf.setData(blobs->at(i)); buf.open(QIODevice::ReadOnly);
                    QImageReader r(&buf); r.setAutoTransform(true);
                    if (mode == 0) {                   // A：只解码（含 shrink-on-load）
                        ImageProvider::prepareReader(r, box, shrink);
                        out = r.read();
                    } else {                           // B：解码 + 缩放（= 生产路径）
                        out = ImageProvider::decodeScaledWith(
                            r, box, shrink, /*pixelPerfect=*/false, prm);
                    }
                }
                if (!out.isNull()) done->fetchAndAddRelaxed(1);
            }
        }
    };

    const QStringList modeNames = { QStringLiteral("A 解码"),
                                    QStringLiteral("B 解码+缩放"),
                                    QStringLiteral("C 只缩放") };
    qInfo().noquote() << QStringLiteral("[bench-parallel] 素材 %1 张  盒 %2x%3  缩放 %4/%5  CPU 线程 %6  重复 %7 次（取中位数）")
                             .arg(blobs.size()).arg(box.width()).arg(box.height())
                             .arg(QString::fromLatin1(scaler::modeName(req.params.mode)),
                                  QString::fromLatin1(scaler::filterName(req.params.filter)))
                             .arg(QThread::idealThreadCount()).arg(repeats);
    qInfo().noquote() << QStringLiteral("[bench-parallel] 阶段\t线程\t墙钟ms\tms/张\t加速比\t效率\tCPU秒\t%%CPU\tRSS_MB");

    // 热身一轮（插件 / 分配器 / 页缓存），不计入
    { QAtomicInt nx(0), dn(0); Task w; w.blobs=&blobs; w.predecoded=&predecoded; w.box=box;
      w.mode=1; w.next=&nx; w.done=&dn; w.prm=req.params; w.shrink=shrink;
      QThreadPool p; p.setMaxThreadCount(1); p.start(&w); p.waitForDone(); }

    for (int grp = 0; grp < 3; ++grp) {
        double base = -1;                                  // 单线程中位数（算加速比用）
        for (int k : threadList) {
            QVector<double> walls, cpus;
            double rssBefore = rssMb();
            for (int rep = 0; rep < repeats; ++rep) {
                QAtomicInt nx(0), dn(0);
                Task task; task.blobs=&blobs; task.predecoded=&predecoded; task.box=box;
                task.mode=grp; task.next=&nx; task.done=&dn; task.prm=req.params; task.shrink=shrink;
                const double c0 = cpuSec();
                QElapsedTimer t; t.start();
                QThreadPool pool; pool.setMaxThreadCount(k);
                for (int t2 = 0; t2 < k; ++t2) pool.start(&task);
                pool.waitForDone();
                walls << double(t.elapsed());
                cpus  << (cpuSec() - c0);
            }
            std::sort(walls.begin(), walls.end());
            std::sort(cpus.begin(), cpus.end());
            const double wall = walls.at(walls.size()/2);
            const double cpu  = cpus.at(cpus.size()/2);
            if (k == 1) base = wall;
            const double speed = (base > 0 && wall > 0) ? base / wall : 1.0;
            qInfo().noquote() << QStringLiteral("[bench-parallel] %1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9")
                    .arg(modeNames.at(grp)).arg(k)
                    .arg(wall, 0, 'f', 1).arg(wall / blobs.size(), 0, 'f', 2)
                    .arg(speed, 0, 'f', 2).arg(speed / k, 0, 'f', 2)
                    .arg(cpu, 0, 'f', 2).arg(cpu > 0 ? cpu / (wall/1000.0) : 0, 0, 'f', 1)
                    .arg(rssMb() - rssBefore, 0, 'f', 0);
        }
    }
    return 0;
}

// 隔离验证：缩略图吞吐（不含 QML/渲染）
static int runThumbBench(const Request &req, ImageProvider *provider)
{
    const int n = qMin(req.thumbsN, provider->count());
    QElapsedTimer t; t.start();
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        // ★ 走**真实缩略图路径**（含 shrink-on-load），不是全解码
        QSize sz;
        const QImage th = provider->requestImage(QStringLiteral("thumb/%1").arg(i),
                                                 &sz, QSize(160,160));
        if (!th.isNull()) ++ok;
    }
    const qint64 ms = t.elapsed();
    qInfo().noquote() << QStringLiteral("[bench] %1 张（真实缩略图路径）%2 ms  平均 %3 ms/张  单线程")
                             .arg(ok).arg(ms).arg(double(ms)/qMax(1,ok), 0, 'f', 2);
    return 0;
}

// 隔离验证：不经过 QML，直接把缩放结果存盘
static int runDump(const Request &req, ImageProvider *provider)
{
    const QString &dumpPath = req.dump;
    const int idx = qMax(0, req.index);
    const QStringList wh = req.box.split(QLatin1Char('x'));
    const QSize box(wh.value(0).toInt(), wh.value(1).toInt());
    const QImage raw = provider->decodeRaw(idx);
    if (raw.isNull()) { qWarning() << "解码失败" << provider->fileNameAt(idx); return 3; }
    QImage work = raw;
    if (req.cropSet) {
        const QStringList c = req.crop.split(QLatin1Char(','));
        if (c.size() == 4) {
            const QRect r(c[0].toInt(), c[1].toInt(), c[2].toInt(), c[3].toInt());
            if (raw.rect().contains(r)) work = raw.copy(r);
            else qWarning().noquote() << "[dump] crop 越界，忽略:" << req.crop;
        }
    }
    const QImage out = provider->pixelPerfect()
        ? scaler::resize(work, box, scaler::params(scaler::Mode::Nearest, scaler::Quality::Fast))
        : scaler::resize(work, box.isValid() && !box.isEmpty() ? box : QSize(1600, 1000), req.params);
    if (!out.save(dumpPath)) { qWarning() << "保存失败" << dumpPath; return 4; }
    qInfo().noquote() << QStringLiteral("[dump] %1  源 %2x%3 → 出 %4x%5  已存 %6")
                             .arg(provider->fileNameAt(idx)).arg(raw.width()).arg(raw.height())
                             .arg(out.width()).arg(out.height()).arg(dumpPath);
    return 0;
}

// 依次跑：gpu → gpu-probe → parallel → thumbs → dump（**与旧 `main.cpp` 同序**）
int runBench(const Request &req, ImageProvider *provider)
{
    if (req.gpuSet)
        return runGpuBench(req, provider);

    if (req.gpuProbe)
        return runGpuScaleProbe();

    if (req.parallelSet)
        return runParallelBench(req, provider);

    if (req.thumbsSet)
        return runThumbBench(req, provider);

    if (!req.dump.isEmpty())
        return runDump(req, provider);

    return 0;
}

} // namespace devbench
