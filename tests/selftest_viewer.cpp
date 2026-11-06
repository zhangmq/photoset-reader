#include "testsupport.h"

//   ★ 占位图的目标尺寸规则是**产品口径**（等比装进 1920×1080 / 1080×1080 方图），
//     测试直接引用实现，避免在测试里再抄一份常量。
#include "placeholder.h"

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
#include "uimodel.h"
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
//     ⑦ 预览条：冷缓存 `ready=false` / 空闲才派活 / 一批一条通知 / 坏图算已了结
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
    //   ★ 显示时钟：换屏只发生在 `framePresented()` 里（每帧最多一屏）⇒ 这里扮演显示器，
    //     每转一轮喂一帧。不喂的话播放永远停在第一屏（这正是"每张都看到"的保证）。
    while (t.elapsed() < timeoutMs && !r.stopped) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        vm.framePresented();
        QThread::msleep(0);
    }
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
            //   ★ 顺便量一下**两幅各自的刷新次数**（用户 2026-11-05：双幅时第一幅明显比第二幅
            //     刷得勤）。模型层应当对称：同一屏的 A / B 要么一起更新，要么各更新一次。
            int updA = 0, updB = 0;
            QObject::connect(&vm, &fetch::ViewerModel::imageAChanged, &vm, [&] { ++updA; });
            QObject::connect(&vm, &fetch::ViewerModel::imageBChanged, &vm, [&] { ++updB; });
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
            //   ★ 一屏**只渲染一次**（2026-11-05 用户定：双幅两幅都稳定才渲染 + 宣告）⇒
            //     两幅的更新次数都应当**正好等于屏数**（以前"先出一格"会让晚到的那幅多刷一次）。
            const int screens = r.advanced + 1;
            check(updA == screens && updB == screens,
                  QStringLiteral("① 双幅：每屏只渲染一次（A=%1 / B=%2，屏数=%3）")
                      .arg(updA).arg(updB).arg(screens));
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
                //   ★ 占位图尺寸规则（用户 2026-11-06 定）：**有原图尺寸 ⇒ 等比装进
                //     1920×1080**（"1080P 级别"），所以这里**不再等于原图尺寸**。
                const QSize expectPh = placeholder::screenSize(sqB.at(6).size);
                check(vm.imageA().size() == expectPh,
                      QStringLiteral("③ 截断图：占位图尺寸 = 等比装进 1920×1080（%1×%2，期望 %3×%4）")
                          .arg(vm.imageA().width()).arg(vm.imageA().height())
                          .arg(expectPh.width()).arg(expectPh.height()));
                //   尺寸未知 ⇒ 1080×1080 方图（方图靠 `narrowSize(invalid)==false`
                //   天然独占一屏，不会跟邻图配对显示）
                check(placeholder::screenSize(QSize()) == QSize(1080, 1080),
                      QStringLiteral("③ 占位图：拿不到尺寸 ⇒ 1080×1080 方图"));
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

        // ══ ⑦ 预览条：前端调度 / 空闲门控 / 失败算已了结（用户 2026-11-06 定的模型）══
        //   ★ 这三条以前是"组件自己调度"（批次号 + 跑完采样最新），坏处是组件得判断
        //     "我这批还用得上吗" —— 那是前端的知识。现在：组件只回一个 `ready` 和一条
        //     "我空闲了"的通知，**派不派活由前端决定**。
        //     这里钉住 C++ 侧的三条契约（QML 侧的判定在 `FullscreenBottomBar.decidePreview`）。
        {
            const QString rootD = tmp.filePath(QStringLiteral("preview"));
            check(makeImages(rootD, 8), QStringLiteral("⑦ 夹具：8 张 PNG 写入成功"));
            check(makeBrokenPng(QStringLiteral("%1/p003.png").arg(rootD),
                                QStringLiteral("%1/p004.png").arg(rootD)),
                  QStringLiteral("⑦ 夹具：第 4 张写成截断图（有字节、解不出）"));
            Sequence sqD;
            const bool okD = scanAll(sqD, rootD);
            check(okD && sqD.count() == 8,
                  QStringLiteral("⑦ 夹具：扫描到 8 条（实际 %1）").arg(sqD.count()));
            if (sqD.count() == 8) {
                auto store = std::make_shared<PreviewStore>();
                fetch::ViewerModel vm(1);
                vm.setPreviewStore(store);
                vm.setSequence(&sqD);
                int idle = 0;
                QObject::connect(&vm, &fetch::ViewerModel::previewIdle, &vm,
                                 [&idle] { ++idle; });

                const int H = 100, W = 600, G = 6;
                const QVariantMap cold = vm.previewStrip(4, H, W, G);
                check(cold.value(QStringLiteral("items")).toList().size() >= 5,
                      QStringLiteral("⑦ 预览条：条目 + 定位框几何一次算完（%1 张）")
                          .arg(cold.value(QStringLiteral("items")).toList().size()));
                check(!cold.value(QStringLiteral("ready")).toBool(),
                      QStringLiteral("⑦ 冷缓存：ready=false（前端据此**不换**显示）"));
                check(vm.previewEnsure(4, H, W, G),
                      QStringLiteral("⑦ 空闲 ⇒ 派活（`previewEnsure` 返回 true）"));
                check(!vm.previewEnsure(4, H, W, G),
                      QStringLiteral("⑦ 忙 ⇒ 不再派（只有空闲才发，不排队）"));
                check(waitUntil([&] { return idle > 0; }, 30000),
                      QStringLiteral("⑦ 一批跑完 ⇒ 发 `previewIdle`（组件回到空闲）"));
                const QVariantMap warm = vm.previewStrip(4, H, W, G);
                check(warm.value(QStringLiteral("ready")).toBool(),
                      QStringLiteral("⑦ 解完 ⇒ ready=true（可以显示）"));
                check(!vm.previewEnsure(4, H, W, G),
                      QStringLiteral("⑦ 全部命中 ⇒ 不再派活"));
                //   ★ 截断图（解不出）必须算"已了结"：否则这一批**永远不就绪** ⇒ 预览条永远
                //     停在上一次的内容；而且"未就绪 ⇒ 再派"会绕成死循环（派 → 失败 → 再派）。
                check(idle == 1,
                      QStringLiteral("⑦ 坏图算已了结：只发一次通知、没有反复重派（实际 %1 次）")
                          .arg(idle));
                check(store->images.size() == 7,
                      QStringLiteral("⑦ 缓存里 7 张（8 张里 1 张解不出，实际 %1）")
                          .arg(store->images.size()));
            }
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
                QElapsedTimer pump;
                pump.start();
                while (pump.elapsed() < half) {             // 前一半（每轮喂一帧）
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
                    vm.framePresented();
                    QThread::msleep(0);
                }
                const int firstHalf = r.advanced;
                pump.restart();
                while (pump.elapsed() < half) {             // 后一半
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
                    vm.framePresented();
                    QThread::msleep(0);
                }
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
            check(s.prefetchWindow() == 1,
                  QStringLiteral("⑥ 设置：默认预取 1（实际 %1）").arg(s.prefetchWindow()));
            s.setPrefetchWindow(0);
            check(s.prefetchWindow() == 0,
                  QStringLiteral("⑥ 设置：选 0 **不被夹成 3**（实际 %1）").arg(s.prefetchWindow()));
            s.setPrefetchWindow(-5);
            check(s.prefetchWindow() == 0,
                  QStringLiteral("⑥ 设置：下限 0（实际 %1）").arg(s.prefetchWindow()));
            s.setPrefetchWindow(999);
            check(s.prefetchWindow() == 8,
                  QStringLiteral("⑥ 设置：上限 8（实际 %1）").arg(s.prefetchWindow()));
            s.setPrefetchWindow(6);           // 留一个"普通值"给下一实例读（0..8 之内）
        }
        {
            app::AppSettings again;           // 新实例 = 重新读盘
            again.load();
            check(again.prefetchWindow() == 6,
                  QStringLiteral("⑥ 设置：改完**立刻落盘**（新实例读回 %1，期望 6）")
                      .arg(again.prefetchWindow()));
        }
        {
            //   ★ 走**产品那条路**：QML 里 `ui.prefetchWindow = v` 等价于调这个 setter。
            //     用户报的"改成 8 也变回 3"就出在这条路上（重复夹取 + 提前 return）。
            app::UiModel ui;
            ui.setPrefetchWindow(8);
            check(ui.prefetchWindow() == 8,
                  QStringLiteral("⑥ 设置：UiModel 收到 8（实际 %1）").arg(ui.prefetchWindow()));
            app::AppSettings fresh;
            fresh.load();
            check(fresh.prefetchWindow() == 8,
                  QStringLiteral("⑥ 设置：经 UiModel 写完后**文件里也是 8**（实际 %1）")
                      .arg(fresh.prefetchWindow()));
            //   ★ 「恢复全部默认设置」（2026-11-05 用户要求）：刚才把预取改成了 8，
            //     重置之后必须回到默认值 1，而且**立刻落盘**。
            ui.resetSettingsToDefaults();
            check(ui.prefetchWindow() == 1,
                  QStringLiteral("⑥ 设置：重置后预取回到默认 1（实际 %1）").arg(ui.prefetchWindow()));
            app::AppSettings afterReset;
            afterReset.load();
            check(afterReset.prefetchWindow() == 1 && afterReset.intervalMs() == 1000,
                  QStringLiteral("⑥ 设置：重置**落盘**（预取 %1，间隔 %2 ms）")
                      .arg(afterReset.prefetchWindow()).arg(afterReset.intervalMs()));
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
