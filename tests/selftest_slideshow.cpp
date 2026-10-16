#include "testsupport.h"

#include <QDeadlineTimer>
#include <QImage>
#include <QSemaphore>
#include <QSet>
#include <QString>
#include <QThread>
#include <QVector>

#include "fetchpipeline.h"
#include "sequence.h"
#include "viewermodel.h"

// ══ 幻灯片自检（ib-test-slideshow；原先由 `image-browser --selftest-slideshow` 跑）═════
//   ★ 幻灯片 = **一个状态**（`playing`），**没有 Slideshow 这个对象**（用户 2026-10-12 定案）
//     ⇒ 本套件打的是 `ViewerModel` 的**可观测行为**（起播/停播/推进），节奏由**注入的时间源**精确验。
//   ★ 虚拟时间源**只由用例推进**（原子；驱动线程只读）⇒ 节奏可控、**不靠真实 sleep 卡时间**：
//     用例不推进虚拟时间，幻灯片就**永远在"等剩余时间"**，于是"没到期就不推进"是确定性的。
//   ★ 断言方式 = **投递 + 轮询到条件成立 + 超时**（`waitFor`）。

static int runSlideshowSelfTest()
{
    using namespace fetch;
    using namespace pipetest;
    using testsupport::waitFor;
    testsupport::Check check(QStringLiteral("slide"));

    // 虚拟时间源：**只由用例推进**（驱动线程只读）
    struct VNow {
        std::atomic<qint64> t{0};
        ViewerModel::NowFn fn() { return [this] { return t.load(std::memory_order_relaxed); }; }
    };
    //   ★ 14 号调研 §6.2 A：**等待也注入**（与 `NowFn` / `DecodeFn` 同一条纪律）——
    //     不真睡：按"注入域的剩余"给一个真实上限（50ms），到点/事件都由**判定条件（虚拟时钟）**决定；
    //     "时间前进"由用例**显式唤醒**（`vm.wakeDriver()`，见下面 :71/:74 等处）。
    auto waitFn = [](VNow &vn) {
        return [&vn](qint64 deadlineMs, QSemaphore &wake) {
            if (deadlineMs < 0) { wake.acquire(); return; }        // 没有到期事项 ⇒ 只等事件
            const qint64 r = deadlineMs - vn.t.load();
            if (r <= 0) return;                                    // 已经到点 ⇒ 立刻重判
            wake.tryAcquire(1, QDeadlineTimer(qMin<qint64>(r, 50)));
        };
    };

    // ① 起播/停播 = **一个状态**（不是对象）；间隔下限
    {
        Sequence seq; fill(seq, mk(10));
        VNow vn;
        // ★ T2d-1c-①：有界窗口（`fetch::Window`）由 viewer 自己建 ⇒ 用例只把序列交给它；假解码器经构造参数注入
        ViewerModel vm(7, Mode::Single, nullptr, decoder({}), vn.fn(), waitFn(vn));
        vm.setSequence(&seq);
        vm.setIntervalMs(10);
        check(vm.intervalMs() == 50, QStringLiteral("间隔下限：设 10ms ⇒ 实际 %1ms").arg(vm.intervalMs()));
        vm.setIntervalMs(100);
        vm.jump(0);
        const bool entered = waitFor([&] { return vm.keyCount() > 0; });
        check(entered, QStringLiteral("进入：先有一屏（%1 张）").arg(vm.keyCount()));
        vm.startSlideshow();
        check(waitFor([&] { return vm.slideshowRunning(); }),
              QStringLiteral("起播 = 置位 ⇒ running = true（**没有 Slideshow 这个对象**）"));
        vm.stopSlideshow();
        check(waitFor([&] { return !vm.slideshowRunning(); }),
              QStringLiteral("停播 = 清位 ⇒ running = false"));
    }

    // ② **间隔是下限**：本屏开始 + 间隔 之前**绝不**推进（虚拟时间冻结 ⇒ 确定性）
    {
        Sequence seq; fill(seq, mk(10));
        VNow vn;
        ViewerModel vm(7, Mode::Single, nullptr, decoder({}), vn.fn(), waitFn(vn));
        vm.setSequence(&seq);
        vm.setIntervalMs(100);
        vm.jump(0);
        check(waitFor([&] { return vm.keyCount() > 0; }), QStringLiteral("② 进入：有一屏"));
        const int c0 = vm.cursor();
        vm.startSlideshow();
        const bool went = waitFor([&] { return vm.cursor() > c0; });
        check(went, QStringLiteral("② 起播先走一屏（= 循环体先要图；游标 %1 → %2）")
                        .arg(c0).arg(vm.cursor()));
        const int c1 = vm.cursor();
        check(!waitFor([&] { return vm.cursor() > c1; }, 150),
              QStringLiteral("② **间隔是下限**：虚拟时间停在 0、间隔 100ms ⇒ **不推进**"));
        vn.t.store(99);                                // ★ 时间前进 ⇒ 显式唤醒（§6.4）
        vm.wakeDriver();
        check(!waitFor([&] { return vm.cursor() > c1; }, 150),
              QStringLiteral("② 还差 1ms ⇒ **仍不推进**"));
        vn.t.store(100);                               // ★ 时间前进 ⇒ 显式唤醒（§6.4）
        vm.wakeDriver();
        const bool due = waitFor([&] { return vm.cursor() > c1; });
        check(due, QStringLiteral("② 到点 ⇒ 推进下一屏（游标 → %1）").arg(vm.cursor()));
    }

    // ③ **不堆积**：耗时 > 间隔 ⇒ **不额外等待**（用例一次都不推进虚拟时间，它自己就往前走）
    {
        VNow vn;
        auto slow = [&vn](const Sequence::Item &) -> QImage {     // 每页解码"耗时" 120ms（> 间隔 50ms）
            vn.t.fetch_add(120, std::memory_order_relaxed);
            QImage img(8, 8, QImage::Format_RGB32);
            img.fill(Qt::red);
            return img;
        };
        //   ⚠️ 夹具用**够长**的序列（40 条）：引擎那条线会**提前写入缓存**，序列太短时
        //      "队列能把后面全备好" ⇒ 虚拟时钟（只有解码才前进）就停住 —— 那是**夹具**的
        //      模型限制，不是产品的"额外等待"。够长 ⇒ 写入缓存一直有新东西解 ⇒ 时钟一直走。
        Sequence seq; fill(seq, mk(40));
        ViewerModel vm(7, Mode::Single, nullptr, slow, vn.fn(), waitFn(vn));
        vm.setSequence(&seq);
        vm.setIntervalMs(50);
        vm.jump(0);
        check(waitFor([&] { return vm.keyCount() > 0; }), QStringLiteral("③ 进入：有一屏"));
        vm.startSlideshow();
        // ★ 用例**一次都没有**推进虚拟时间：唯一让时间前进的就是解码本身（每页 120ms > 间隔 50ms）
        //   ⇒ 只要它往前走了，就证明"耗时吃掉了间隔"（**没有**额外等一个间隔）。
        const bool ran = waitFor([&] { return vm.cursor() >= 5; });
        check(ran, QStringLiteral("③ **不堆积**：耗时 120ms > 间隔 50ms ⇒ 无需额外等待，"
                                  "一路推进到游标 %1（用例没推进过虚拟时间）").arg(vm.cursor()));
    }

    // ④ **不跳页**：到期几次就正好推进几屏（一次一屏，没有跳过）
    {
        Sequence seq; fill(seq, mk(10));
        VNow vn;
        ViewerModel vm(7, Mode::Single, nullptr, decoder({}), vn.fn(), waitFn(vn));
        vm.setSequence(&seq);
        vm.setIntervalMs(100);
        vm.jump(0);
        check(waitFor([&] { return vm.keyCount() > 0; }), QStringLiteral("④ 进入：有一屏"));
        vm.startSlideshow();
        check(waitFor([&] { return vm.cursor() == 1; }),
              QStringLiteral("④ 起播 ⇒ 正好推进到游标 1"));
        bool ok = true;
        for (int i = 2; i <= 4; ++i) {
            vn.t.fetch_add(100);                        // 正好过一个间隔
            vm.wakeDriver();                            // ★ 时间前进 ⇒ 显式唤醒
            if (!waitFor([&] { return vm.cursor() == i; })) { ok = false; break; }
        }
        check(ok, QStringLiteral("④ **不跳页**：3 次到期 ⇒ 游标正好 1→2→3→4（每次 +1，实得 %1）")
                      .arg(vm.cursor()));
        vm.stopSlideshow();
        const bool stopped4 = waitFor([&] { return !vm.slideshowRunning(); });
        check(stopped4, QStringLiteral("④ 停播 ⇒ running=false"));
    }

    // ⑤ **等待期间位置照常生效**：虚拟时间冻结（永远不到期）时按 next，立刻生效
    //    （T2d-1d-③b 起不再有请求环：next 就是"改位置 + 发布 + 叫醒"，驱动醒来照样收下）
    {
        Sequence seq; fill(seq, mk(10));
        VNow vn;
        ViewerModel vm(7, Mode::Single, nullptr, decoder({}), vn.fn(), waitFn(vn));
        vm.setSequence(&seq);
        vm.setIntervalMs(1000);                        // 间隔很大 + 虚拟时间冻结 ⇒ 永远在等
        vm.jump(0);
        check(waitFor([&] { return vm.keyCount() > 0; }), QStringLiteral("⑤ 进入：有一屏"));
        vm.startSlideshow();
        check(waitFor([&] { return vm.cursor() == 1; }), QStringLiteral("⑤ 起播 ⇒ 游标 1"));
        const int c1 = vm.cursor();
        vm.next();                                     // ★ 用户在"等剩余时间"期间主动翻页
        const bool drained = waitFor([&] { return vm.cursor() > c1; }, 500);
        check(drained, QStringLiteral("⑤ **等待期间位置照常生效**：按 next ⇒ 立刻推进（游标 %1 → %2）")
                           .arg(c1).arg(vm.cursor()));
        vm.stopSlideshow();
        const bool stopped5 = waitFor([&] { return !vm.slideshowRunning(); });
        check(stopped5, QStringLiteral("⑤ 停播 ⇒ running=false"));
    }

    // ⑥ **可被主动打破**：等待中停播 ⇒ 立刻清位，且**停在当前屏**；停播后不再自动推进
    {
        Sequence seq; fill(seq, mk(10));
        VNow vn;
        ViewerModel vm(7, Mode::Single, nullptr, decoder({}), vn.fn(), waitFn(vn));
        vm.setSequence(&seq);
        vm.setIntervalMs(1000);
        vm.jump(0);
        check(waitFor([&] { return vm.keyCount() > 0; }), QStringLiteral("⑥ 进入：有一屏"));
        vm.startSlideshow();
        check(waitFor([&] { return vm.cursor() == 1; }), QStringLiteral("⑥ 起播 ⇒ 游标 1"));
        const QVector<QString> keep = vm.keys();
        const int cKeep = vm.cursor();
        vm.stopSlideshow();                            // ★ 在"等剩余时间"期间打破
        const bool broke = waitFor([&] { return !vm.slideshowRunning(); }, 500);
        check(broke, QStringLiteral("⑥ **可被主动打破**：等待中停播 ⇒ 清位（≤ 一片）"));
        check(!keep.isEmpty() && vm.keys() == keep,
              QStringLiteral("⑥ 打破后：**停在当前屏**（keys 不变，没被清空）"));
        vn.t.fetch_add(100000);                        // 时间推很远：若还在播就会立刻推进
        vm.wakeDriver();                               // ★ 时间前进 ⇒ 显式唤醒
        const bool moved = waitFor([&] { return vm.cursor() != cKeep; }, 150);
        check(!moved, QStringLiteral("⑥ 打破后：**不再自动推进**（游标停在 %1）").arg(cKeep));
    }

    // ⑦ **只有"向前"**：每次推进游标正好 +1（没有向后、没有跳页语义）
    {
        Sequence seq; fill(seq, mk(10));
        VNow vn;
        ViewerModel vm(7, Mode::Single, nullptr, decoder({}), vn.fn(), waitFn(vn));
        vm.setSequence(&seq);
        vm.setIntervalMs(50);
        vm.jump(2);
        check(waitFor([&] { return vm.keyCount() > 0; }), QStringLiteral("⑦ 进入：有一屏"));
        const int c0 = vm.cursor();
        vm.startSlideshow();
        QVector<int> seen;
        if (waitFor([&] { return vm.cursor() == c0 + 1; })) seen << vm.cursor();
        for (int i = 2; i <= 4; ++i) {
            vn.t.fetch_add(50);
            vm.wakeDriver();                            // ★ 时间前进 ⇒ 显式唤醒
            if (waitFor([&] { return vm.cursor() == c0 + i; })) seen << vm.cursor();
        }
        check(seen == QVector<int>({c0 + 1, c0 + 2, c0 + 3, c0 + 4}),
              QStringLiteral("⑦ **只有向前**：从游标 %1 起每次 +1（实得 %2 次推进）")
                  .arg(c0).arg(seen.size()));
        vm.stopSlideshow();
        const bool stopped7 = waitFor([&] { return !vm.slideshowRunning(); });
        check(stopped7, QStringLiteral("⑦ 停播 ⇒ running=false"));
    }

    // ⑧ 到头 ⇒ **自动停播**并**留在最后一屏**
    {
        Sequence seq; fill(seq, mk(3));                // 只有 3 张
        VNow vn;
        ViewerModel vm(7, Mode::Single, nullptr, decoder({}), vn.fn(), waitFn(vn));
        vm.setSequence(&seq);
        vm.setIntervalMs(50);
        vm.jump(0);
        const bool entered8 = waitFor([&] { return vm.keyCount() > 0; });
        check(entered8, QStringLiteral("⑧ 进入：有一屏（游标 0）"));
        vm.startSlideshow();
        const bool playing8 = waitFor([&] { return vm.slideshowRunning(); });
        check(playing8, QStringLiteral("⑧ 起播 ⇒ running=true（先确认在播，再谈「到头自动停」）"));
        const bool first8 = waitFor([&] { return vm.cursor() == 1; });
        check(first8, QStringLiteral("⑧ 起播先走一屏（游标 0 → 1）"));
        vn.t.fetch_add(50);                              // 到期 ⇒ 第 2 屏（最后一屏）
        vm.wakeDriver();                                 // ★ 时间前进 ⇒ 显式唤醒
        const bool second8 = waitFor([&] { return vm.cursor() == 2; });
        check(second8, QStringLiteral("⑧ 推进到最后一屏（游标 2）"));
        vn.t.fetch_add(50);                              // 再到期 ⇒ 后面没有了
        vm.wakeDriver();                                 // ★ 时间前进 ⇒ 显式唤醒
        const bool ended8 = waitFor([&] { return !vm.slideshowRunning(); });
        check(ended8, QStringLiteral("⑧ 到头（最后一屏之后）⇒ **自动停播**"));
        check(vm.cursor() == 2 && vm.keyCount() == 1,
              QStringLiteral("⑧ 到头 ⇒ **留在最后一屏**（游标 %1、keys %2 张）")
                  .arg(vm.cursor()).arg(vm.keyCount()));
        check(!vm.loading(), QStringLiteral("⑧ 到头 ≠ loading（要显示的那一屏已经备好；%1）")
                                 .arg(vm.loading()));
    }

    // ⑨ ★ **长序列**（两端不在同一份有界窗口内）+ 放映走到头 ⇒ **回绕到另一端**、并**继续放映**
    //    （设计 12 §8.9「四」2 的到头分支；这条是上一轮欠的硬性判定条件）
    //    ⚠️ 把"到头回绕"那段删掉 ⇒ 本条**必须失败**（游标会停在最后一屏）。
    {
        Sequence seq; fill(seq, mk(400));          // 400 条：第 0 与第 399 条不可能同一份窗口
        VNow vn;
        ViewerModel vm(7, Mode::Single, nullptr, decoder({}), vn.fn(), waitFn(vn));
        vm.setSequence(&seq);
        vm.setIntervalMs(100);
        vm.setLoopMode(1);                         // 全部循环
        vm.jump(399);                              // 直接站到最后一屏
        check(waitFor([&] { return vm.cursor() == 399 && vm.keyCount() > 0; }),
              QStringLiteral("⑨ 先站到最后一屏（游标 %1）").arg(vm.cursor()));
        vm.startSlideshow();
        const bool wrapped = waitFor([&] { return vm.cursor() == 0 && vm.keyCount() > 0; });
        const int cur9 = vm.cursor();
        check(wrapped, QStringLiteral("⑨ **长序列**放映到头 ⇒ 回绕到另一端第 0 屏（游标 %1）")
                           .arg(cur9));
        check(vm.slideshowRunning(),
              QStringLiteral("⑨ 回绕之后**继续放映**（running = %1）").arg(vm.slideshowRunning()));
        vn.t.fetch_add(1000);                      // 过一个间隔 ⇒ 应继续往前走
        vm.wakeDriver();                           // ★ 时间前进 ⇒ 显式唤醒
        check(waitFor([&] { return vm.cursor() == 1; }),
              QStringLiteral("⑨ 回绕后仍按节奏前进（游标 %1）").arg(vm.cursor()));
        vm.stopSlideshow();
        check(waitFor([&] { return !vm.slideshowRunning(); }), QStringLiteral("⑨ 收尾：停播"));
    }

    return check.finish();
}

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, &runSlideshowSelfTest);
}
