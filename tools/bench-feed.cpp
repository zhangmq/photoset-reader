// ══ 独立实验：预取能不能喂饱"60 fps 的消费者"（ib-bench-feed）═════════════════════
//   用户 2026-11-05 的验收口径：
//     **消费者按 60 fps 要下一屏；每次要都能立刻拿到 ⇒ 达标；要等 ⇒ 预取没起到作用。**
//   用户还怀疑：**慢盘上预取越大反而更慢**（在飞的那几张预取把磁盘队列占住，当前屏反而排在后面）。
//   ⇒ 这个工具量的是"消费等待"，不是硬吞吐。
//
//   做法：把 `ViewerModel` 的间隔设成 1000/fps（16 ms ≈ 60 fps 的节奏），让它自己预取 + 推进，
//   再量**每屏实际间隔**：
//     · ≈ 16 ms      ⇒ 消费没等（预取提前备好了）
//     · 明显 > 16 ms  ⇒ 这一屏被迫等 ⇒ 记一次 miss（p95 / max 是最坏的等待）
//   报告：miss 率、p50/p95/max 间隔、等效 fps、总卡顿毫秒。
//
//   ★ **必须冷缓存**：`-flush N` 在每次测量前分配并逐页写 N GB 内存，把系统文件缓存挤掉；
//     否则同一批文件第二遍全是内存带宽（慢盘也能量出 5 GB/s 的假数字）。
//   ★ 走产品同一条流水线（`reconcile()` + 在飞上限 4 + 真解码），只是没有 QML / 渲染。
//   ★ `-L` = **代码里的预取张数**（`m_length`，本屏那 1~2 张不算）；用户口径
//     "预取 N = 本屏 1 + 前面 N−1" ⇒ 对应这里 L = N−1，表里两列都印。
//
//   用法：ib-bench-feed -d <dir> [-L 0,1,2,3,5,6] [-n 300] [-f 60] [-l double] [-flush 20]

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QThread>
#include <QVector>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include "sequence.h"
#include "viewermodel.h"

namespace {

//   把文件缓存挤掉：分配 + 逐页写 + 释放。内存压力下 Windows 会回收 standby 页，
//   下一次读盘就真的落盘了。（用 malloc 不保证 commit，所以这里显式写满。）
void flushCacheGb(int gb)
{
    if (gb <= 0) return;
    const long long total = static_cast<long long>(gb) << 30;
    constexpr long long kChunk = 64LL << 20;
    std::vector<char *> blocks;
    long long done = 0;
    while (done < total) {
        const long long n = (total - done < kChunk) ? (total - done) : kChunk;
        char *p = new (std::nothrow) char[size_t(n)];
        if (!p) break;
        std::memset(p, 0x5a, size_t(n));
        blocks.push_back(p);
        done += n;
    }
    for (char *p : blocks) delete[] p;
}

bool scanAll(Sequence &sq, const QString &dir)
{
    sq.startScan(dir, 20, false);
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 600000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (sq.drain()) continue;
        if (sq.scanFinished()) break;
        QThread::msleep(1);
    }
    while (sq.drain()) { }
    return sq.scanFinished();
}

struct Row {
    int    screens = 0;
    int    misses = 0;
    qint64 stallMs = 0;
    double p50 = 0, p95 = 0, mx = 0, fps = 0;
};

Row runOne(const Sequence &seq, int L, bool doublePage, int wanted, int intervalMs, int timeoutMs)
{
    Row r;
    fetch::ViewerModel vm(L);
    vm.setSequence(&seq);
    vm.setDoublePage(doublePage);
    vm.setIntervalMs(intervalMs);

    QVector<qint64> gaps;
    int lastCursor = vm.cursor();
    QElapsedTimer clock;
    clock.start();
    qint64 lastMs = 0;

    QObject::connect(&vm, &fetch::ViewerModel::currentChanged, &vm, [&] {
        const int cur = vm.cursor();
        if (cur == lastCursor) return;
        const qint64 now = clock.elapsed();
        gaps << (now - lastMs);
        lastMs = now;
        lastCursor = cur;
    });

    vm.setPlaying(true);
    //   ★ 扮演显示器：按 60 Hz 喂帧 —— 换屏只发生在 `framePresented()` 里（每帧最多一屏），
    //     不喂帧就会永远停在第一屏（2026-11-06 起幻灯片是帧驱动的）。
    constexpr int kFrameMs = 16;
    qint64 nextFrame = 0;
    while (clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        if (gaps.size() >= wanted) break;
        if (!vm.playing()) break;              // 到头
        if (clock.elapsed() >= nextFrame) {
            nextFrame = clock.elapsed() + kFrameMs;
            vm.framePresented();
        }
        QThread::msleep(0);
    }
    vm.setPlaying(false);

    r.screens = gaps.size();
    for (qint64 g : gaps) {
        r.stallMs += qMax<qint64>(0, g - intervalMs);
        if (g > intervalMs + intervalMs / 2) ++r.misses;   // 超过 1.5 个节奏才算"等过"
    }
    if (r.screens > 0) {
        QVector<qint64> sorted = gaps;
        std::sort(sorted.begin(), sorted.end());
        r.p50 = sorted.at(sorted.size() / 2);
        const int idx95 = qMin(sorted.size() - 1, int(sorted.size() * 0.95));
        r.p95 = sorted.at(idx95);
        r.mx  = sorted.last();
        const qint64 total = clock.elapsed();
        r.fps = total > 0 ? double(r.screens) * 1000.0 / double(total) : 0;
    }
    return r;
}

}  // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("ib-bench-feed"));

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("Can prefetch feed a 60 fps consumer?"));
    const QCommandLineOption optDir({ "d", "dir" }, "Directory with images", "dir");
    const QCommandLineOption optL({ "L", "lengths" }, "Prefetch counts (code L = ahead images)", "list",
                                   "0,1,2,3,5,6");
    const QCommandLineOption optN({ "n", "screens" }, "Screens to consume", "n", "300");
    const QCommandLineOption optF({ "f", "fps" }, "Consumer fps", "fps", "60");
    const QCommandLineOption optLayout({ "l", "layout" }, "double|single", "layout", "double");
    const QCommandLineOption optFlush({ "flush", "flush-gb" }, "GB of RAM to touch before each run", "gb", "0");
    cli.addOption(optDir);
    cli.addOption(optL);
    cli.addOption(optN);
    cli.addOption(optF);
    cli.addOption(optLayout);
    cli.addOption(optFlush);
    cli.addHelpOption();
    cli.process(app);

    const QString dir = cli.value(optDir);
    if (dir.isEmpty()) {
        std::printf("usage: ib-bench-feed -d <dir> [-L 0,1,2,3,5,6] [-n 300] [-f 60] [-l double] [-flush GB]\n");
        return 2;
    }
    const bool doublePage = (cli.value(optLayout) != QStringLiteral("single"));
    const int wanted = qMax(30, cli.value(optN).toInt());
    const int fps = qBound(1, cli.value(optF).toInt(), 240);
    const int intervalMs = qMax(1, 1000 / fps);
    const int flushGb = qMax(0, cli.value(optFlush).toInt());

    QVector<int> lengths;
    for (const QString &s : cli.value(optL).split(QLatin1Char(','), Qt::SkipEmptyParts))
        lengths << qMax(0, s.trimmed().toInt());

    Sequence seq;
    QElapsedTimer scanClock;
    scanClock.start();
    if (!scanAll(seq, dir) || seq.count() <= 0) {
        std::printf("scan failed or empty: %s\n", qPrintable(dir));
        return 1;
    }
    std::printf("dir    : %s\n", qPrintable(QDir::toNativeSeparators(dir)));
    std::printf("items  : %d   scan %lld ms\n", seq.count(), scanClock.elapsed());
    std::printf("layout : %s   consumer %d fps (cadence %d ms)   screens/run %d   cache flush %d GB\n\n",
                doublePage ? "double" : "single", fps, intervalMs, wanted, flushGb);

    std::printf("%4s %5s %8s %7s %6s %6s %6s %6s %9s\n",
                "L", "N=L+1", "screens", "miss%", "p50", "p95", "max", "fps", "stall ms");
    std::printf("-------------------------------------------------------------------------\n");

    for (int L : lengths) {
        flushCacheGb(flushGb);                 // ★ 每次测量前把文件缓存挤掉（慢盘才量得真）
        const Row r = runOne(seq, L, doublePage, wanted, intervalMs, 300000);
        const double missPct = r.screens > 0 ? 100.0 * r.misses / r.screens : 0;
        std::printf("%4d %5d %8d %7.1f %6.0f %6.0f %6.0f %6.1f %9lld\n",
                    L, L + 1, r.screens, missPct, r.p50, r.p95, r.mx, r.fps, r.stallMs);
        std::fflush(stdout);
    }
    return 0;
}
