#include "testsupport.h"

#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <functional>

#include "appsettings.h"
#include "sequence.h"
#include "viewermodel.h"   // `src/` 在 include 路径上（testsupport 给的）

// ══ 阅读屏自检（ib-test-viewer）════════════════════════════════════════════
//   ★ 2026-11-05：这套件的**唯一目的**是钉住那个 bug ——
//     「双幅 / 间隔 0ms / 长图集 ⇒ 画面卡住不再翻页」。
//     机制：前端每重判一次就重挂 0ms 闹钟 ⇒ `onBeat` 永远轮不到。
//     ⇒ 断言方式 = **播到最后一屏并自己停播**（旧实现会卡在中途，永远到不了）。
//
//   为什么能自动化：`ViewerModel` 自己就是一个 QObject，解码线程把结果 post 回主线程的
//   事件队列 ⇒ 这里只要"转事件循环"就能推进，不需要窗口、不需要人点播放。
//   夹具 = 临时目录里现造的 PNG（**不解码库里的真素材**，也不写任何设置 / 进度文件）。
//
//   用例：
//     ① 双幅 / 0ms / L=8 ⇒ 播到底（旧实现卡死）
//     ② 单幅 / 0ms / L=0 ⇒ 播到底（L=0 合法：只备本屏）
//     ③ 截断图（有字节、解不出）⇒ 显示占位图、**不跳屏**
//     ④ 缺（文件删了、元数据标 missing）⇒ 该屏自动跳过、不卡住
namespace {

//   等条件成立（上限 = timeoutMs；每轮都转一把事件循环）。超时 = 条件不成立。
//   ★ 解码结果 / 幻灯片定时器都靠事件循环推进（`QCoreApplication::processEvents` 就够，
//     不需要真窗口 —— 这也是这套件能 headless 跑的原因）。
bool waitUntil(const std::function<bool()> &done, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        if (done()) return true;
        QThread::msleep(1);
    }
    return done();
}

//   造 n 张竖版小图（40×60 ⇒ 双幅会两两配对；真 PNG，扫描要读尺寸）
bool makeImages(const QString &dir, int n, const QSize &size = QSize(40, 60))
{
    if (!QDir().mkpath(dir)) return false;
    for (int i = 0; i < n; ++i) {
        QImage img(size, QImage::Format_RGB32);
        img.fill(QColor::fromHsv((i * 37) % 360, 160, 200));
        if (!img.save(QStringLiteral("%1/p%2.png").arg(dir).arg(i, 3, 10, QLatin1Char('0')), "PNG"))
            return false;
    }
    return true;
}

//   截断图夹具：**只把 IDAT 的数据区涂坏**（长度 / 结构都留着）⇒
//   头里的 IHDR 照旧读得到尺寸（扫描不会把它当"缺"），而像素数据解不出来。
//   这正是"有字节、坏编码"那一类：必须画占位图，不许当"缺"跳过去。
bool makeBrokenPng(const QString &goodPath, const QString &outPath)
{
    QFile good(goodPath);
    if (!good.open(QIODevice::ReadOnly)) return false;
    QByteArray bytes = good.readAll();
    good.close();
    const int idat = bytes.indexOf("IDAT");
    if (idat < 4 || idat + 12 >= bytes.size()) return false;
    const int payload = idat + 4;                 // "IDAT" 之后紧跟数据区（长度在类型之前）
    for (int k = 0; k < 12; ++k) bytes[payload + k] = char(0xAB);
    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly)) return false;
    out.write(bytes);
    out.close();
    return true;
}

bool scanAll(Sequence &sq, const QString &root)
{
    sq.startScan(root, 5, false);
    const bool done = waitUntil([&] {
        sq.drain();
        return sq.scanFinished();
    }, 120000);
    while (sq.drain()) { }
    return done;
}

//   播一次的统计
struct PlayResult {
    int    advanced = 0;      // `cursor` 变了几次（= 真翻了几屏）
    int    reached  = -1;     // 最后的 `cursor`
    bool   stopped  = false;  // 自己停播了（到末尾）
    qint64 ms       = 0;      // 从开播到停播（或超时）用了多久
};

//   从当前位置开播，跑到"自己停播"或超时。
PlayResult play(fetch::ViewerModel &vm, int timeoutMs)
{
    PlayResult r;
    int last = vm.cursor();
    QObject::connect(&vm, &fetch::ViewerModel::currentChanged, &vm, [&] {
        const int c = vm.cursor();
        if (c != last) {
            ++r.advanced;
            last = c;
        }
    });
    QObject::connect(&vm, &fetch::ViewerModel::playingChanged, &vm, [&] {
        if (!vm.playing()) r.stopped = true;
    });
    QElapsedTimer t;
    t.start();
    vm.setPlaying(true);
    waitUntil([&] { return r.stopped; }, timeoutMs);
    r.ms = t.elapsed();
    r.reached = vm.cursor();
    return r;
}

int runViewerSelfTest(const QString &realDir)
{
    testsupport::Check check(QStringLiteral("viewerselftest"));

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        qWarning() << "[viewerselftest] 无法建临时目录";
        return 1;
    }

    // ── 夹具 A：40 张竖版小图（双幅 20 屏 / 单幅 40 屏）──
    const QString rootA = tmp.filePath(QStringLiteral("pages"));
    check(makeImages(rootA, 40), QStringLiteral("夹具：40 张 PNG 写入成功"));
    {
        Sequence sq;
        const bool okA = scanAll(sq, rootA);
        check(okA && sq.count() == 40,
              QStringLiteral("夹具：扫描到 40 条（实际 %1）").arg(sq.count()));

        // ══ ① 双幅 / 0ms / L=8：旧实现卡死的那一条 ══
        {
            fetch::ViewerModel vm(8);
            vm.setSequence(&sq);
            vm.setDoublePage(true);
            vm.setIntervalMs(0);
            const PlayResult r = play(vm, 20000);
            const int lastStart = sq.pairStart(sq.count() - 1);
            check(r.stopped,
                  QStringLiteral("① 双幅 / 0ms / L=8：播到末尾自己停播（推进 %1 屏、用时 %2 ms）")
                      .arg(r.advanced).arg(r.ms));
            check(r.reached == lastStart,
                  QStringLiteral("① 双幅：停在最后一屏（cursor=%1，期望 %2）")
                      .arg(r.reached).arg(lastStart));
            check(r.advanced >= 19,
                  QStringLiteral("① 双幅：真的翻了 19 屏以上（实际 %1）").arg(r.advanced));
        }

        // ══ ② 单幅 / 0ms / L=0：预取深度 0 也得照常播 ══
        {
            fetch::ViewerModel vm(0);
            vm.setSequence(&sq);
            vm.setIntervalMs(0);
            const PlayResult r = play(vm, 20000);
            const int lastStart = sq.count() - 1;
            check(r.stopped,
                  QStringLiteral("② 单幅 / 0ms / L=0：播到末尾自己停播（推进 %1 屏、用时 %2 ms）")
                      .arg(r.advanced).arg(r.ms));
            check(r.reached == lastStart,
                  QStringLiteral("② 单幅 / L=0：停在最后一屏（cursor=%1，期望 %2）")
                      .arg(r.reached).arg(lastStart));
        }

        // ══ ③ 截断图：有字节、解不出 ⇒ 占位图，**不跳屏** ══
        {
            const QString rootB = tmp.filePath(QStringLiteral("broken"));
            check(makeImages(rootB, 12), QStringLiteral("③ 夹具：12 张 PNG 写入成功"));
            check(makeBrokenPng(QStringLiteral("%1/p005.png").arg(rootB),
                                QStringLiteral("%1/p006.png").arg(rootB)),
                  QStringLiteral("③ 夹具：截断图写入成功（IDAT 数据区被涂坏、结构仍完整）"));

            Sequence sqB;
            const bool okB = scanAll(sqB, rootB);
            check(okB && sqB.count() == 12,
                  QStringLiteral("③ 夹具：扫描到 12 条（实际 %1）").arg(sqB.count()));
            if (sqB.count() == 12) {
                check(sqB.at(6).size.isValid() && !sqB.at(6).missing,
                      QStringLiteral("③ 夹具：截断图的尺寸仍读得到（否则测的是「缺」，不是残图）"));
                fetch::ViewerModel vm(8);
                vm.setSequence(&sqB);
                vm.setIntervalMs(0);
                vm.jump(6);
                const bool gotImage = waitUntil([&] { return !vm.imageA().isNull(); }, 5000);
                check(gotImage, QStringLiteral("③ 截断图：显示出一张图（残图或占位图）"));
                check(vm.cursor() == 6,
                      QStringLiteral("③ 截断图：**没有跳屏**（cursor=%1，期望 6）").arg(vm.cursor()));
                check(vm.imageA().size() == sqB.at(6).size,
                      QStringLiteral("③ 截断图：图尺寸 = 序列里的原图尺寸（%1×%2）")
                          .arg(vm.imageA().width()).arg(vm.imageA().height()));
            }
        }

        // ══ ④ "缺"：文件删了 + 元数据标 missing ⇒ 该屏跳过、不卡住 ══
        {
            const QString rootC = tmp.filePath(QStringLiteral("missing"));
            check(makeImages(rootC, 12), QStringLiteral("④ 夹具：12 张 PNG 写入成功"));
            Sequence sqC;
            const bool okC = scanAll(sqC, rootC);
            check(okC && sqC.count() == 12,
                  QStringLiteral("④ 夹具：扫描到 12 条（实际 %1）").arg(sqC.count()));
            QFile::remove(QStringLiteral("%1/p005.png").arg(rootC));
            const int marked = sqC.markMissing();
            check(marked == 1, QStringLiteral("④ 夹具：markMissing 标出 1 条（实际 %1）").arg(marked));
            fetch::ViewerModel vm(8);
            vm.setSequence(&sqC);
            vm.setIntervalMs(0);
            vm.jump(5);
            const bool moved = waitUntil([&] { return vm.cursor() == 6; }, 5000);
            check(moved, QStringLiteral("④ 缺：该屏自动跳过、落到下一屏（cursor=%1，期望 6）")
                             .arg(vm.cursor()));
            //   落到下一屏后还要等它自己的图（跳屏只是改位置，解码是异步的）
            const bool gotImage = waitUntil([&] { return !vm.imageA().isNull(); }, 5000);
            check(gotImage, QStringLiteral("④ 缺：跳过去之后有新的一屏图"));
        }
    }

    // ══ ⑤ 真素材（`-d <目录>`）：可选，用真实图集跑同一件事 ══
    //   ★ 只读素材：不写进度文件、不改设置。用途 = 手工验收（第 9 节清单）。
    if (!realDir.isEmpty()) {
        Sequence sq;
        QElapsedTimer scanClock;
        scanClock.start();
        const bool scanned = scanAll(sq, realDir);
        check(scanned, QStringLiteral("⑤ 真素材：扫描跑完（%1 条，用时 %2 ms）")
                           .arg(sq.count()).arg(scanClock.elapsed()));
        if (scanned && sq.count() > 100) {
            for (int layout = 0; layout < 2; ++layout) {
                fetch::ViewerModel vm(8);
                vm.setSequence(&sq);
                vm.setDoublePage(layout == 1);
                vm.setIntervalMs(0);
                //   只播 10 秒：这里要看的是"**一直在动**"，不是"播到末尾"（真图集有上万条）
                PlayResult r;
                int last = vm.cursor();
                QObject::connect(&vm, &fetch::ViewerModel::currentChanged, &vm, [&] {
                    if (vm.cursor() != last) {
                        ++r.advanced;
                        last = vm.cursor();
                    }
                });
                QElapsedTimer t;
                t.start();
                vm.setPlaying(true);
                const int half = 5000;
                waitUntil([&] { return false; }, half);     // 前一半
                const int firstHalf = r.advanced;
                waitUntil([&] { return false; }, half);     // 后一半
                r.ms = t.elapsed();
                r.reached = vm.cursor();
                vm.setPlaying(false);
                const QString mode = (layout == 1) ? QStringLiteral("双幅") : QStringLiteral("单幅");
                check(r.advanced > 20,
                      QStringLiteral("⑤ 真素材 / %1 / 0ms：10 秒内推进 %2 屏（不再冻结）")
                          .arg(mode).arg(r.advanced));
                check(r.advanced - firstHalf > 0,
                      QStringLiteral("⑤ 真素材 / %1：后半程仍在推进（后半 %2 屏）")
                          .arg(mode).arg(r.advanced - firstHalf));
            }
        } else if (scanned) {
            qInfo().noquote() << QStringLiteral("[viewerselftest] 跳过⑤：素材太少（%1 条）")
                                     .arg(sq.count());
        }
    }

    // ══ ⑥ 设置：预取张数（= viewer 的 `L`）的落盘与范围 ══
    //   ★ 用户 2026-11-05 报的 bug：设置页选 0，重启后是 3、运行时也不生效 ——
    //     根因是 `UiModel::setPrefetchWindow` 里**自己又夹了一次** `qBound(3, …)`，
    //     和 `AppSettings` 的 `qBound(0, …)` 不一致（选 0 被夹成 3；3 早就存过 ⇒
    //     "没变就 return" 直接命中 ⇒ 既没落盘、也没通知 viewer）。
    //   ⇒ 这里钉住"边界只有一处（AppSettings）、改完立刻落盘、读回来还是那个值"。
    //   ★ 隔离用 Qt 自己的 test mode（`QStandardPaths::setTestModeEnabled`）：
    //     产品里没有设置文件 env 后门（用户 2026-11-04 定），而 `XDG_CONFIG_HOME`
    //     在 Windows 上不生效 ⇒ 绝不能让测试写到用户的真设置里。
    {
        QStandardPaths::setTestModeEnabled(true);
        const QString settingsPath =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
            + QStringLiteral("/settings.json");
        QFile::remove(settingsPath);
        qInfo().noquote() << QStringLiteral("[viewerselftest] ⑥ 设置文件（测试隔离）：%1")
                                 .arg(QDir::toNativeSeparators(settingsPath));
        {
            app::AppSettings s;
            s.load();
            check(s.prefetchWindow() == 8,
                  QStringLiteral("⑥ 设置：默认预取 8（实际 %1）").arg(s.prefetchWindow()));
            s.setPrefetchWindow(0);
            check(s.prefetchWindow() == 0,
                  QStringLiteral("⑥ 设置：选 0 **不被夹成 3**（实际 %1）").arg(s.prefetchWindow()));
            s.setPrefetchWindow(-5);
            check(s.prefetchWindow() == 0,
                  QStringLiteral("⑥ 设置：下限 0（实际 %1）").arg(s.prefetchWindow()));
            s.setPrefetchWindow(999);
            check(s.prefetchWindow() == 64,
                  QStringLiteral("⑥ 设置：上限 64（实际 %1）").arg(s.prefetchWindow()));
            s.setPrefetchWindow(12);          // 留一个"普通值"给下一实例读
        }
        {
            app::AppSettings again;           // 新实例 = 重新读盘
            again.load();
            check(again.prefetchWindow() == 12,
                  QStringLiteral("⑥ 设置：改完**立刻落盘**（新实例读回 %1，期望 12）")
                      .arg(again.prefetchWindow()));
        }
        QFile::remove(settingsPath);
    }

    return check.finish();
}

}  // namespace

int main(int argc, char *argv[])
{
    const QString dir = testsupport::dirArg(argc, argv);
    return testsupport::suiteMain(argc, argv, [dir] { return runViewerSelfTest(dir); });
}
