#include "testsupport.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>

#include <cstdio>
#include <memory>

#include "fetchpipeline.h"
#include "seqwindow.h"
#include "sequence.h"
#include "viewermodel.h"

// ══ op→上屏延迟量测（开发期工具：**只打印、不断言、不进 ctest**；T2d-1b）═════════
//   量的是什么：GUI 线程**连续投递 N 个 next** 起，到 GUI 线程**看到第 N 屏的内容**
//   （`ViewerModel::keys()` —— 只有 post 回 GUI 线程才会变）为止的墙钟时间。
//   为什么这么量：`cursor()` 是驱动线程**当场**写的原子量（投递完立刻就变），
//   它不等于"上屏"；`keys()` 只在 GUI 线程被 post 赋值 ⇒ 它就是"上屏"的观测点。
//   N = 16（**T2d-1d-③b 起不再有请求环**：连投的位置只有**最后那一份**会被引擎解）；重复 5 轮，
//   报每轮总耗时与单次平均。
//   ★ T2d-1c-①：**有界窗口（`fetch::Window`）由 `ViewerModel` 自己建**（口径 2：预取半宽 = 7、屏余量 = 16，
//     按"序列变了 / 游标变了 / 自己的跳转"重造）—— 量法与改造前**同一种**
//     （GUI 线程连投 N 个 next ⇒ 第 N 屏上屏的墙钟），只是"重造窗口"从用例的接线搬进了 viewer。
static int runLatency()
{
    using namespace fetch;
    using namespace pipetest;
    using testsupport::waitFor;

    constexpr int kN = 16;         // 每轮投递的 next 数（位置=发布值 ⇒ 引擎只解最后一份）
    constexpr int kReps = 5;       // 重复轮数
    constexpr int kItems = 400;    // 合成就绪序列（已就绪 ⇒ 不涉及扫描/等待）
    constexpr int kHalf = 7;       // 预取半宽（= AppSettings 默认）

    Sequence seq;
    fill(seq, mk(kItems));

    ViewerModel vm(kHalf, Mode::Single, nullptr, decoder({}));
    vm.setSequence(&seq);          // ★ 窗口归 viewer：只给序列（不再有 setWindow / 要窗口接线）
    vm.jump(0);
    if (!waitFor([&] { return !vm.keys().isEmpty(); })) {
        std::printf("[latency] 预热失败\n");
        return 1;
    }

    std::printf("[latency] N=%d reps=%d items=%d（合成就绪序列 / 单幅 / 窗口由 viewer 自建：半宽 %d）\n",
                kN, kReps, kItems, kHalf);
    QVector<double> totals;
    for (int rep = 0; rep < kReps; ++rep) {
        if (vm.cursor() + kN >= kItems) {          // 回到开头，保证每轮都有 N 屏可走
            vm.jump(0);
            waitFor([&] { return vm.keys().value(0) == QLatin1String("0"); });
        }
        const int start = vm.cursor();
        const QString want = QString::number(start + kN);

        QElapsedTimer t;
        t.start();
        for (int i = 0; i < kN; ++i) vm.next();     // 连投 N 个（立即返回）

        bool seen = false;
        while (t.nsecsElapsed() < 5'000'000'000LL) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            if (!vm.keys().isEmpty() && vm.keys().first() == want) { seen = true; break; }
            QThread::usleep(50);
        }
        const double ms = double(t.nsecsElapsed()) / 1e6;
        totals << ms;
        std::printf("[latency] 轮 %d: 首投→第 %d 屏上屏 = %.3f ms（单次平均 %.3f ms）%s\n",
                    rep + 1, kN, ms, ms / kN, seen ? "" : " ⚠ 超时/丢请求");
    }
    std::sort(totals.begin(), totals.end());
    const double med = totals.at(totals.size() / 2);
    std::printf("[latency] 中位数 = %.3f ms（单次平均 %.3f ms）\n", med, med / kN);
    return 0;
}

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, &runLatency);
}
