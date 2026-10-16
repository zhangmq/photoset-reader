#include "testsupport.h"

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QObject>
#include <QSemaphore>
#include <QString>
#include <QThread>

#include "browsemodelview.h"      // ★ T2d-1b：`ui.browse`（浏览界面模型）
#include "fetchpipeline.h"
#include "seqwindow.h"
#include "sequence.h"
#include "viewermodel.h"

// ══ viewer 核心自检（ib-test-vmcore；原先由 `image-browser --selftest-vmcore` 跑）══
//   ★ 只验**状态语义**：`{ 游标, keys }` 只收下不计算、keys 不可由游标派生、next/prev 对称、
//     模式 = 会话级。**幻灯片（状态 + 节奏）在 `--selftest-slideshow`**（注入虚拟时间源）。
//   ★ 断言方式 = **投递 + 轮询到条件成立 + 超时**（`waitFor`），不用固定等待。
//   ★★ T2d-1c-①：**有界窗口（`fetch::Window`）由 viewer 自己建**（它手里既有序列、又有导航状态）⇒ 用例只把序列
//     交给它（`setSequence`），不再有 `setWindow` / `windowNeeded`；
//     **引擎纯消费**：覆盖不足 / 序列还没到 ⇒ 什么都不做（不做无谓的重试），等一份更新的窗口再重试暂缓的
//     那一步（⑧/⑧b/⑨/⑪ 就是这条的正面与反面）。
//     浏览那一段用**新家** `app::BrowseModel`（= QML 的 `ui.browse`）——它只吃"序列 + 当前条目"，
//     **不经过有界窗口**（口径 7）。
static int runVmCoreSelfTest()
{
    using namespace fetch;
    using namespace pipetest;
    using testsupport::waitFor;
    testsupport::Check check(QStringLiteral("vmcore"));

    // ① 状态 = { 游标, 当前上屏的 keys }；进入 = jump
    {
        Sequence seq; fill(seq, mk(10));
        ViewerModel vm(7, Mode::Single, nullptr, decoder({}));
        vm.setSequence(&seq);
        vm.jump(0);
        const bool up = waitFor([&] { return vm.keyCount() > 0; });
        check(up, QStringLiteral("状态：上屏 keys = %1 张").arg(vm.keyCount()));
        check(vm.keys().size() == 1, QStringLiteral("状态：单幅 ⇒ 上屏正好 1 张"));
        check(vm.cursor() == 0, QStringLiteral("状态：游标 = 调度器给的新游标（当前屏起点 0）"));
    }

    // ② **游标只收下，不计算**：换一个"前进 +2"的模式，游标就按调度器给的走
    {
        Sequence seq; fill(seq, mk(10));
        ViewerModel vm(7, Mode::Double, nullptr, decoder({}));
        vm.setSequence(&seq);
        vm.jump(0);
        check(waitFor([&] { return vm.cursor() == 0 && vm.keyCount() > 0; }),
              QStringLiteral("游标**只收下**：双幅下新游标 = 0（VM 没算过）"));
        vm.next();
        const bool stepped = waitFor([&] { return vm.cursor() == 2; });
        check(stepped, QStringLiteral("步长由调度器决定：双幅再 next ⇒ %1（+本组大小）").arg(vm.cursor()));
    }

    // ③ keys **不可由游标派生**：同一游标、不同模式 ⇒ 不同 keys
    {
        Sequence seq;
        fill(seq, mk(10));
        ViewerModel a(7, Mode::Single, nullptr, decoder({}));
        ViewerModel b(7, Mode::Double, nullptr, decoder({}));
        a.setSequence(&seq);
        b.setSequence(&seq);
        a.jump(0);
        b.jump(0);
        check(waitFor([&] { return a.keyCount() > 0 && b.keyCount() > 0; }),
              QStringLiteral("③ 两边都上了屏"));
        check(a.cursor() == b.cursor() && a.keys().size() == 1 && b.keys().size() == 2,
              QStringLiteral("keys **不可由游标派生**：同一个下一屏起点，单幅 1 张 / 双幅 2 张"));
    }

    // ④ next / prev 方向正确（prev 回到上一屏）
    {
        Sequence seq; fill(seq, mk(10));
        ViewerModel vm(7, Mode::Single, nullptr, decoder({}));
        vm.setSequence(&seq);
        vm.jump(3);
        check(waitFor([&] { return vm.keyCount() > 0; }), QStringLiteral("④ 进入：有一屏"));
        const QVector<QString> k3 = vm.keys();          // ★ 必须是**进入之后**的当前屏
        vm.next();
        check(waitFor([&] { return vm.keys() != k3; }), QStringLiteral("next：上屏变了"));
        vm.prev();
        check(waitFor([&] { return vm.keys() == k3; }),
              QStringLiteral("prev：回到上一屏（keys 一致，游标对称）"));
    }

    // ⑤ 模式 = 会话级（VM 持有并转发；**不写设置**）
    //    ⚠️ T2d-1c-①：版面变了 ⇒ 屏表也变了 ⇒ **窗口由本类自己换**（`setDoublePage` 内部
    //      重造一份按新版面的窗口）。转发是否真的生效由**行为**证明（一屏 2 张）。
    {
        Sequence seq; fill(seq, mk(10));
        ViewerModel vm(7, Mode::Single, nullptr, decoder({}));
        vm.setSequence(&seq);
        vm.setMode(Mode::Double);
        check(vm.mode() == Mode::Double && vm.doublePage(),
              QStringLiteral("模式 = **会话级**：VM 持有（doublePage = true）"));
        vm.jump(0);
        const bool two = waitFor([&] { return vm.keys().size() == 2; });
        check(two, QStringLiteral("切到双幅后，下一屏起点所在的那一屏 = 2 张"));
    }

    // ⑥ 浏览（**新家 `ui.browse`**）**只枚举元素**（L40）：
    //    空目录 / 无图压缩包 / 「子目录全空 ⇒ 自己也不出现」的链**都不出现**；
    //    判定条件只有 `Sequence::isElement()` 一份实现（这里验的是**枚举处过了它**）。
    {
        Sequence seq;
        {
            auto row = [](int ord, int parentOrd, const QString &kind, const QString &label,
                          int w, int h) {
                Sequence::Row r;
                r.ord = ord;
                r.parentOrd = parentOrd;
                r.kind = kind;
                r.label = label;
                r.pageIndex = -1;
                r.width = w;
                r.height = h;
                return r;
            };
            QVector<Sequence::Row> rows;
            rows << row(0, -1, QStringLiteral("image"), QStringLiteral("r0.png"), 1200, 1800);
            rows << row(1, -1, QStringLiteral("image"), QStringLiteral("r1.png"), 1200, 1800);
            rows << row(2, -1, QStringLiteral("folder"), QStringLiteral("empty"), 0, 0);
            rows << row(3, -1, QStringLiteral("folder"), QStringLiteral("chain"), 0, 0);
            rows << row(4, 3, QStringLiteral("folder"), QStringLiteral("inner"), 0, 0);
            rows << row(5, -1, QStringLiteral("folder"), QStringLiteral("emptyParent"), 0, 0);
            rows << row(6, 5, QStringLiteral("folder"), QStringLiteral("emptyChild"), 0, 0);
            rows << row(7, -1, QStringLiteral("archive"), QStringLiteral("noimg.cbz"), 0, 0);
            rows << row(8, -1, QStringLiteral("archive"), QStringLiteral("withimg.cbz"), 0, 0);
            rows << row(9, 8, QStringLiteral("image"), QStringLiteral("w1.png"), 800, 1200);
            rows << row(10, -1, QStringLiteral("folder"), QStringLiteral("dirWithImg"), 0, 0);
            rows << row(11, 10, QStringLiteral("image"), QStringLiteral("d1.png"), 1200, 1800);
            rows << row(12, 10, QStringLiteral("image"), QStringLiteral("d2.png"), 1200, 1800);
            seq.buildFromRows(QStringLiteral("/synth/vis"), rows, false, QStringLiteral("vis"));
        }
        check(seq.tree().size() == 9 && seq.count() == 5,
              QStringLiteral("⑥ 夹具：9 个节点全在树里 / 5 条图；元素只有 root(0) + withimg(7) + dirWithImg(8)"));

        // ★ `ui.browse`（T2d-1c-①）：**只吃"序列 + 当前条目"**，换层自己重算 ——
        //   不经过有界窗口、不经过 viewer（这一条就是"浏览行不再随窗口"的行为凭据）。
        app::BrowseModel browse;
        browse.setSequence(&seq);
        browse.setCursor(0);

        const int rootRows = browse.items()->rowCount();
        check(rootRows == 4, QStringLiteral("⑥ 根层浏览行 = 2 图 + 2 个有图容器（实得 %1 行）")
                                 .arg(rootRows));
        const auto nameAt = [&](int r) {
            return browse.items()->index(r, 0).data(::BrowseModel::NameRole).toString();
        };
        const auto nodeAt = [&](int r) {
            return browse.items()->index(r, 0).data(::BrowseModel::NodeIndexRole).toInt();
        };
        check(nameAt(0) == QStringLiteral("r0.png") && nameAt(1) == QStringLiteral("r1.png")
                  && nameAt(2) == QStringLiteral("withimg.cbz")
                  && nameAt(3) == QStringLiteral("dirWithImg"),
              QStringLiteral("⑥ 行序 = 图（自然序）→ 有图容器；空目录 / 无图压缩包**一个都没有**"));
        bool onlyElements = true;
        for (int r = 0; r < browse.items()->rowCount(); ++r) {
            const int ni = nodeAt(r);
            if (ni >= 0 && !seq.isElement(ni)) onlyElements = false;
        }
        check(onlyElements, QStringLiteral("⑥ 每一行的 `nodeIndex` 都是元素"));

        // 进 dirWithImg（第 3 行）⇒ 只剩它那两张；空容器根本进不去（从不出现）
        browse.activate(3);
        const int subRows = browse.items()->rowCount();
        check(subRows == 2 && browse.canGoUp(),
              QStringLiteral("⑥ 进 dirWithImg ⇒ 2 行 + 可上一级（实得 %1 行）").arg(subRows));
        check(nameAt(0) == QStringLiteral("d1.png") && nameAt(1) == QStringLiteral("d2.png"),
              QStringLiteral("⑥ 层内两张图（自然序）"));
    }

    // ⑦ ★ D1：按住方向键 —— 第一次按下 = **一次普通 step**；按住 ⇒ **常量节奏**继续推；
    //    松手 ⇒ **立刻停**（未执行的推进被清掉，队列里不再堆积）。用真实时钟（节奏 33ms）。
    //   ⚠️ 断言一律"**先求值、再拼文案**"（C++ 的实参求值次序不定；写在 `check()` 里的
    //      `.arg(vm.cursor())` 可能在 `waitFor` 之前求值 ⇒ 文案会印出**过期读数**）。
    {
        Sequence seq; fill(seq, mk(40));
        ViewerModel vm(7, Mode::Single, nullptr, decoder({}));
        vm.setSequence(&seq);
        vm.jump(0);
        const bool entered7 = waitFor([&] { return vm.keyCount() > 0; });
        check(entered7, QStringLiteral("⑦ D1：进入有一屏（keys %1）").arg(vm.keyCount()));

        // 单击（按下即松开）：**必须真的前进一步**，而且不会自己继续推
        const int c0 = vm.cursor();
        vm.stepPage(1);                                // QML 的"第一次按下"就调这个
        const bool clicked = waitFor([&] { return vm.cursor() == c0 + 1; });
        const int cClick = vm.cursor();
        check(clicked, QStringLiteral("⑦ 单击（按下即松开）⇒ 前进一步（%1 → %2）").arg(c0).arg(cClick));
        waitFor([] { return false; }, 200);            // 等 6 个多节奏周期
        const int cIdle = vm.cursor();
        check(cIdle == c0 + 1,
              QStringLiteral("⑦ 单击之后**不会自己继续推**（游标 %1，期望 %2）").arg(cIdle).arg(c0 + 1));

        // 按住：常量节奏推进（33ms/次 ⇒ 200ms 里至少 3 屏）
        const int c1 = vm.cursor();
        vm.pushStart(+1);
        const bool pushed = waitFor([&] { return vm.cursor() >= c1 + 3; }, 2000);
        const int cPush = vm.cursor();
        check(pushed, QStringLiteral("⑦ 按住 ⇒ 按常量节奏继续推（%1 → %2）").arg(c1).arg(cPush));

        // 松手：**立刻停**（容差 1 屏 = 停止信号与正在进行的那一次推进的天然竞争）
        vm.pushStop();
        const int c2 = vm.cursor();
        waitFor([] { return false; }, 250);            // 7 个节奏周期
        const int cAfter = vm.cursor();
        check(cAfter <= c2 + 1,
              QStringLiteral("⑦ 松手 ⇒ **立刻停**（未执行的被清掉：%1 → %2，容差 1）")
                  .arg(c2).arg(cAfter));
    }

    // ⑧ ★★ T2d-1c-①：**远跳 ⇒ 本类（GUI 线程）自己造一份覆盖得到的窗口**，引擎直接跳过去。
    //    ⚠️ 这条盯的就是"窗口各归各位 + 引擎纯消费"：**引擎从头到尾没有"要窗口"这回事**
    //      （`windowNeeded` / `requestWindow` / `waitForWindow` 整条协议已删 —— 这里直接
    //      用"**类上没有那个信号**"钉死它）。把"远跳先重造窗口"这段删掉 ⇒ 本条**必须失败**。
    {
        Sequence seq; fill(seq, mk(400));
        ViewerModel vm(7, Mode::Single, nullptr, decoder({}));
        check(vm.metaObject()->indexOfSignal("windowNeeded(int,int,int)") < 0,
              QStringLiteral("⑧ 引擎**没有**「要窗口」信号（`windowNeeded` 已整体删除）"));
        vm.setSequence(&seq);
        vm.jump(300);                     // 远目标：远超一份窗口的覆盖（窗口中心 ±16 屏）
        const bool landed = waitFor([&] { return vm.cursor() == 300 && vm.keyCount() > 0; });
        const int cur8 = vm.cursor(), keys8 = vm.keyCount();
        check(landed, QStringLiteral("⑧ 远跳 ⇒ **真的跳过去**（游标 %1 / keys %2）")
                          .arg(cur8).arg(keys8));
        check(vm.currentPage() == 300 && vm.itemCount() == 400,
              QStringLiteral("⑧ 新窗口的 OSD 跟上：屏号 %1（期望 300）/ 总数 %2（期望 400）")
                  .arg(vm.currentPage()).arg(vm.itemCount()));
        check(!vm.loading(),
              QStringLiteral("⑧ 跳完 ⇒ 不再 loading（= 要显示的那一屏**已经备好**；%1）")
                  .arg(vm.loading()));
    }

    // ⑧b ★★ **上一 / 下一文件夹的远目标**（口径 1 + 1c-①）：目标由**本类用手里的窗口**算，
    //     目标不在原窗口覆盖内 ⇒ 本类**自己重造一份覆盖得到的窗口**再投 `OpJump` ——
    //     **引擎没有为它发过任何请求**（`windowNeeded` 类上就不存在，见 ⑧）。
    {
        // 夹具：根下 60 张散图 → 目录 A（3 张）→ 目录 B（3 张）
        //   条目 0..59 = 散图；60..62 = A 里三张；63..65 = B 里三张
        //   节点 0 = 根 / 1 = A / 2 = B ⇒ `nextContainerFirst(0)` = 60（**远在窗口覆盖之外**）
        Sequence seq;
        {
            auto row = [](int ord, int parentOrd, const QString &kind, const QString &label) {
                Sequence::Row r;
                r.ord = ord;
                r.parentOrd = parentOrd;
                r.kind = kind;
                r.label = label;
                r.pageIndex = -1;
                r.width = 1200;
                r.height = 1800;
                return r;
            };
            QVector<Sequence::Row> rows;
            for (int i = 0; i < 60; ++i)
                rows << row(i, -1, QStringLiteral("image"), QStringLiteral("r%1.png").arg(i));
            rows << row(60, -1, QStringLiteral("folder"), QStringLiteral("A"));
            for (int i = 0; i < 3; ++i)
                rows << row(61 + i, 60, QStringLiteral("image"), QStringLiteral("a%1.png").arg(i));
            rows << row(64, -1, QStringLiteral("folder"), QStringLiteral("B"));
            for (int i = 0; i < 3; ++i)
                rows << row(65 + i, 64, QStringLiteral("image"), QStringLiteral("b%1.png").arg(i));
            seq.buildFromRows(QStringLiteral("/synth/folders"), rows, false,
                              QStringLiteral("folders"));
        }
        check(seq.count() == 66 && seq.nextContainerFirst(0) == 60
                  && seq.nextContainerFirst(60) == 63 && seq.prevContainerFirst(63) == 60,
              QStringLiteral("⑧b 夹具：66 条 / 容器起点 60(A) 与 63(B)（远间距）"));

        ViewerModel vm(7, Mode::Single, nullptr, decoder({}));
        vm.setSequence(&seq);
        vm.jump(0);
        check(waitFor([&] { return vm.cursor() == 0 && vm.keyCount() > 0; }),
              QStringLiteral("⑧b 先站到第 0 条（窗口里）"));

        vm.nextArchive();                 // 下一文件夹：目标 60 —— **原窗口（±16 屏）之外**
        const bool to60 = waitFor([&] { return vm.cursor() == 60 && vm.keyCount() > 0; });
        const int curB1 = vm.cursor();
        check(to60, QStringLiteral("⑧b 下一文件夹的**远目标** ⇒ 跳到 60（实得 %1）").arg(curB1));
        check(!vm.loading(),
              QStringLiteral("⑧b 远目标落地 ⇒ 不再 loading（这一屏已备好；引擎没发过请求）"));

        vm.nextArchive();                 // 再下一个：目标 63（B 的第一条）
        check(waitFor([&] { return vm.cursor() == 63 && vm.keyCount() > 0; }),
              QStringLiteral("⑧b 再下一文件夹 ⇒ 63（实得 %1）").arg(vm.cursor()));

        vm.prevArchive();                 // 上一个：回到 A 的第一条 60
        check(waitFor([&] { return vm.cursor() == 60 && vm.keyCount() > 0; }),
              QStringLiteral("⑧b 上一文件夹 ⇒ 回到 60（实得 %1）").arg(vm.cursor()));

        // ★ **文件夹循环**（loopMode = 2）到头：回绕到**当前容器**的第一条
        //   （目标 = `Window::loopTarget`，唯一实现；回绕同样由 GUI 线程造窗口）
        vm.setLoopMode(2);
        vm.jump(65);                      // 最后一个条目 = B 的最后一张
        check(waitFor([&] { return vm.cursor() == 65 && vm.keyCount() > 0; }),
              QStringLiteral("⑧b 站到末条（B 的最后一张 = 65）"));
        vm.next();
        check(waitFor([&] { return vm.cursor() == 63; }),
              QStringLiteral("⑧b **文件夹循环**：末屏 next ⇒ 回绕到 B 的第一条 63（实得 %1）")
                  .arg(vm.cursor()));
        vm.setLoopMode(0);
    }

    // ⑨ ★ 目标**还没扫到**（`!scanFinished()`，目标超出 count）⇒ **什么都不做、维持现状**
    //     （置「正在等推进」+ loading），等**更新的窗口**到了再**重试暂缓的那一步** ⇒ 跳过去。
    //   ⚠️ 用**默认构造**的序列当"还没扫到"（`NotReady` + `scanFinished()==false` + count 0）：
    //      "半扫描的活序列"在单测里不可复现；引擎看的正是 `!scanFinished()` 这个旗标
    //      （不是 count 的具体数值）。"扫到了"= 序列长了 + `UiModel` 调 `sequenceChanged()`
    //      （同一个入口，用例直接调）。
    //   ⚠️ 把"暂缓的那一步在新窗口到达后重试"删掉（暂缓即丢弃）⇒ 本条**必须失败**。
    {
        Sequence seq;                              // NotReady：还没条目、扫描没结束
        ViewerModel vm(7, Mode::Single, nullptr, decoder({}));
        vm.setSequence(&seq);
        vm.jump(300);
        const bool waiting = waitFor([&] { return vm.loading(); });
        check(waiting,
              QStringLiteral("⑨ 还没扫到 ⇒ loading（= 要显示的那一屏还没备好；判定条件 = 本类）"));
        const bool stillNothing = !waitFor([&] { return vm.cursor() != 0 || vm.keyCount() > 0; }, 250);
        check(stillNothing, QStringLiteral("⑨ **不做无谓的重试**：维持现状（游标 %1 / keys %2）")
                                .arg(vm.cursor()).arg(vm.keyCount()));

        // "扫到了" = 序列长了 ⇒ `UiModel` 调 `sequenceChanged()`（窗口中心仍是那个暂缓的远目标）
        fill(seq, mk(400));
        vm.sequenceChanged();
        const bool landed9 = waitFor([&] { return vm.cursor() == 300 && vm.keyCount() > 0; });
        const int cur9 = vm.cursor(), keys9 = vm.keyCount();
        check(landed9, QStringLiteral("⑨ 新窗口到了 ⇒ **跳过去**（游标 %1 / keys %2）")
                           .arg(cur9).arg(keys9));
        check(waitFor([&] { return !vm.loading(); }),
              QStringLiteral("⑨ 不再等（这一屏备好了 ⇒ loading 清掉）"));
    }

    // ⑩ ★★ 按屏 OSD **真的被用上**：走到**另一屏** ⇒ OSD 九个字段必须是**那一屏**的
    //    （而不是"窗口中心那一屏"的）。⚠️ 把 `buildScreenInfo` 里"按屏 OSD"那一段旁路掉 ⇒ 本条**必须失败**。
    {
        Sequence seq;
        {   // 造一棵"两话"的树（`folderLabel` / `relativePath` 才有区分度）
            auto row = [](int ord, int parentOrd, const QString &kind, const QString &label,
                          int w, int h) {
                Sequence::Row r;
                r.ord = ord; r.parentOrd = parentOrd; r.kind = kind; r.label = label;
                r.pageIndex = -1; r.width = w; r.height = h;
                return r;
            };
            QVector<Sequence::Row> rows;
            rows << row(0, -1, QStringLiteral("image"), QStringLiteral("a0.png"), 1200, 1800);
            rows << row(1, -1, QStringLiteral("image"), QStringLiteral("a1.png"), 1200, 1800);
            rows << row(2, -1, QStringLiteral("folder"), QStringLiteral("第01话"), 0, 0);
            rows << row(3, 2, QStringLiteral("image"), QStringLiteral("b0.png"), 3000, 1200);
            rows << row(4, 2, QStringLiteral("image"), QStringLiteral("b1.png"), 3000, 1200);
            rows << row(5, -1, QStringLiteral("folder"), QStringLiteral("第02话"), 0, 0);
            rows << row(6, 5, QStringLiteral("image"), QStringLiteral("c0.png"), 1200, 1800);
            seq.buildFromRows(QStringLiteral("/synth/osd"), rows, false, QStringLiteral("osd"));
        }
        // 条目序：a0(0) a1(1) b0(2) b1(3) c0(4) ⇒ 5 条 / 单幅 5 屏；第 3 屏 = b1.png（第01话）
        //   参照（夹具自证区分力）：单独造一份窗口，取第 0 / 第 3 屏的按屏 OSD
        const auto pack = windowFor(seq, Mode::Single, 0, 16, 16);
        const Window::ScreenOsd *o0 = pack->screenOsd(pack->screenOf(0));
        const Window::ScreenOsd *o3 = pack->screenOsd(pack->screenOf(3));
        const bool distinguishable = o0 && o3 && o0->fileLabel != o3->fileLabel
                                     && o0->currentPage != o3->currentPage
                                     && pack->screenFirst(pack->screenOf(3)) == 3;
        check(distinguishable,
              QStringLiteral("⑩ 夹具自证区分力：窗口覆盖第 3 屏，第 0 / 第 3 屏的 OSD **不同**"
                             "（%1@%2 vs %3@%4）")
                  .arg(o0 ? o0->fileLabel : QString(), o0 ? QString::number(o0->currentPage) : QString(),
                       o3 ? o3->fileLabel : QString(), o3 ? QString::number(o3->currentPage) : QString()));

        ViewerModel vm(7, Mode::Single, nullptr, decoder({}));
        vm.setSequence(&seq);
        vm.jump(0);
        const bool first = waitFor([&] { return vm.cursor() == 0 && vm.keyCount() > 0; });
        check(first, QStringLiteral("⑩ 先上第 0 屏"));
        check(vm.fileLabel() == QStringLiteral("a0.png") && vm.currentPage() == 0,
              QStringLiteral("⑩ 第 0 屏的 OSD（%1 @ 屏 %2）").arg(vm.fileLabel()).arg(vm.currentPage()));

        vm.jump(3);                                   // ★ 走到**另一屏**
        const bool third = waitFor([&] { return vm.cursor() == 3 && vm.keyCount() > 0; });
        const int cur10 = vm.cursor();
        check(third, QStringLiteral("⑩ 走到第 3 屏（游标 %1）").arg(cur10));
        check(vm.keyCount() == 1 && vm.itemCount() == 5,
              QStringLiteral("⑩ 单幅 ⇒ 本屏 1 张 / 条目总数 5（实得 %1 / %2）")
                  .arg(vm.keyCount()).arg(vm.itemCount()));
        const QString f10 = vm.fileLabel(), s10 = vm.sizeLabel(), d10 = vm.folderLabel();
        const QString p10 = vm.relativePath();
        const int page10 = vm.currentPage(), pages10 = vm.totalPages();
        check(f10 == QStringLiteral("b1.png"),
              QStringLiteral("⑩ fileLabel 跟着游标（%1，期望 b1.png）").arg(f10));
        check(s10 == QStringLiteral("3000×1200"),
              QStringLiteral("⑩ sizeLabel 跟着游标（%1，期望 3000×1200）").arg(s10));
        check(d10 == QStringLiteral("第01话"),
              QStringLiteral("⑩ folderLabel 跟着游标（%1，期望 第01话）").arg(d10));
        check(page10 == 3 && pages10 == 5,
              QStringLiteral("⑩ 屏号 / 屏数（%1 / %2，期望 3 / 5）").arg(page10).arg(pages10));
        check(!p10.isEmpty() && o3 && p10 == o3->relativePath,
              QStringLiteral("⑩ relativePath = 第 3 屏那一份（%1）").arg(p10));
    }

    // ⑪ ★ **没有可显示的数据时就等着、不做无用功**；序列长进来 ⇒ 重新判一次并上屏
    //    （设计 12 §8.9「四」：要显示的那一屏还有"未备好" ⇒ 等待；收到"缓存变了"再判一次）
    {
        auto st = std::make_shared<Stats>();
        ViewerModel vm(7, Mode::Single, nullptr, decoder({}, st));
        Sequence seq;                            // NotReady：一条都还没有
        vm.setSequence(&seq);                    // 生产顺序：先给序列
        vm.jump(0);                              // 再"要第一屏"（引擎答不出来 ⇒ 等）
        const bool quiet = !waitFor([&] { return st->calls.load() > 0 || vm.keyCount() > 0; }, 250);
        check(quiet, QStringLiteral("⑪ 还没有可显示的数据 ⇒ **不做无谓的重试**（解码 %1 次 / keys %2）")
                         .arg(st->calls.load()).arg(vm.keyCount()));
        check(vm.loading(), QStringLiteral("⑪ 要显示的那一屏还没备好 ⇒ loading = true"));
        fill(seq, mk(20));                       // 序列长进来（= UiModel 收到通知 ⇒ sequenceChanged）
        vm.sequenceChanged();
        check(waitFor([&] { return vm.keyCount() > 0; }),
              QStringLiteral("⑪ 缓存变了 ⇒ **重新判一次并上屏**（keys %1）").arg(vm.keyCount()));
        check(st->calls.load() > 0 && !vm.loading(),
              QStringLiteral("⑪ 上屏后 ⇒ 真的解码过（%1 次）且不再 loading").arg(st->calls.load()));
    }

    // ⑫ ★ 循环模式**到头回绕仍要生效**（口径：到头"什么都不做"与"覆盖不足"必须分开）：
    //    到头 ⇒ 停下并留在当前屏（**不 loading**）；循环模式该回绕就回绕
    //    （回绕目标 = `Window::loopTarget`，唯一实现；本类先造一份**覆盖得到目标**的窗口再投 `OpJump`）。
    {
        Sequence seq; fill(seq, mk(10));
        ViewerModel vm(7, Mode::Single, nullptr, decoder({}));
        vm.setSequence(&seq);
        vm.setLoopMode(1);                          // 全部循环
        vm.jump(9);                                 // 末屏
        check(waitFor([&] { return vm.cursor() == 9 && vm.keyCount() > 0; }),
              QStringLiteral("⑫ 先站到最后一屏（%1）").arg(vm.cursor()));
        vm.next();                                  // 到头 ⇒ 该回绕到 0
        const bool wrapped = waitFor([&] { return vm.cursor() == 0 && vm.keyCount() > 0; });
        const int cur12 = vm.cursor();
        check(wrapped, QStringLiteral("⑫ **全部循环**：末屏 next ⇒ 回绕到第 0 屏（实得 %1）").arg(cur12));
        check(!vm.loading(),
              QStringLiteral("⑫ 回绕**不是**「等推进」：回绕后这一屏已备好 ⇒ 不 loading（%1）")
                  .arg(vm.loading()));
        vm.prev();                                  // 第 0 屏 prev ⇒ 回绕到末屏
        check(waitFor([&] { return vm.cursor() == 9; }),
              QStringLiteral("⑫ 首屏 prev ⇒ 回绕到末屏（实得 %1）").arg(vm.cursor()));

        //   **不循环**（loopMode = 0）⇒ 到头**停下并留在当前屏**（什么都不做，也不 loading）
        vm.setLoopMode(0);
        const int keep = vm.cursor();
        const QVector<QString> keepKeys = vm.keys();
        vm.next();
        check(!waitFor([&] { return vm.loading(); }, 200) && vm.cursor() == keep
                  && vm.keys() == keepKeys,
              QStringLiteral("⑫ 不循环 ⇒ 到头**停下并留在当前屏**（游标 %1 / 不 loading）")
                  .arg(vm.cursor()));
    }

    // ⑬ ★★ **单幅：含"坏"的那一屏自动跳过**（设计 12 §8.9「四」2）——
    //    坏 = 缺失 / 不可读（这里用"解码失败 ⇒ 坏·不可读"）；**手动浏览也一样**。
    //    ⚠️ 变异测试（故意把代码改错，看用例能不能发现）：① 把"坏"当"未备好" ⇒ 变成等待（游标不动 + loading）；② 单幅不跳过
    //    ⇒ 显示那一屏（游标停在 3）⇒ 本条都**必须失败**。
    {
        Sequence seq; fill(seq, mk(6));
        QHash<QString, int> calls;                 // 每条解码了几次（验"不重试"）
        const QSet<QString> bad{ QStringLiteral("k3") };
        auto dec = [&](const Sequence::Item &it) -> QImage {
            ++calls[it.label];
            if (bad.contains(it.label)) return QImage();          // 打不开 / 解码失败
            QImage img(8, 8, QImage::Format_RGB32);
            img.fill(Qt::red);
            return img;
        };
        ViewerModel vm(7, Mode::Single, nullptr, dec);
        vm.setSequence(&seq);
        vm.jump(3);                                // 直接落到坏条目那一屏
        check(waitFor([&] { return vm.cursor() == 4 && vm.keyCount() > 0; }),
              QStringLiteral("⑬ 单幅：这一屏含坏条目 ⇒ **自动跳过**（游标 3 ⇒ %1）")
                  .arg(vm.cursor()));
        check(!vm.loading() && vm.skippedCount() == 1,
              QStringLiteral("⑬ 跳过 ⇒ 不 loading（%1）、跳过计数 +1（%2）")
                  .arg(vm.loading()).arg(vm.skippedCount()));
        check(vm.keys().size() == 1 && !vm.imageA().isNull() && vm.cursor() == 4,
              QStringLiteral("⑬ 落到的那一屏是**备好的图**（游标 %1 / keys %2 / 有图 %3）")
                  .arg(vm.cursor()).arg(vm.keys().size()).arg(!vm.imageA().isNull()));
        //   **坏的状态一直留着**：再回到那一屏 ⇒ 照样**跳过**（不弹回坏图、也不卡在等待）。
        //   （"不再去取"的**精确解码次数**由引擎级用例保证 —— 那里单线程驱动、可确定地数；
        //     这里引擎那条线在后台写入缓存，精确计数会随调度波动。）
        vm.jump(3);
        check(waitFor([&] { return vm.cursor() == 4 && vm.skippedCount() == 2; }),
              QStringLiteral("⑬ 坏状态**一直留着**：回看同一屏仍然跳过（游标 %1 / 累计跳过 %2）")
                  .arg(vm.cursor()).arg(vm.skippedCount()));
    }

    // ⑭ ★★ **双幅：含"坏"的那一屏"保留原本空位"**（另一页照常显示；不等、不跳）——
    //    设计 12 §8.9「四」2 的括注：**双幅但两页全坏 ⇒ 也跳过**（AI 推断，待用户确认）。
    //    ⚠️ 变异测试：双幅没留空位（只把好的那一条报上去）⇒ keys 只有 1 条 ⇒ 本条**必须失败**。
    {
        Sequence seq; fill(seq, mk(6));            // 双幅配对：(0,1) (2,3) (4,5)
        const QSet<QString> oneBad{ QStringLiteral("k2") };
        auto dec1 = [&](const Sequence::Item &it) -> QImage {
            if (oneBad.contains(it.label)) return QImage();
            QImage img(8, 8, QImage::Format_RGB32);
            img.fill(Qt::red);
            return img;
        };
        ViewerModel vm(7, Mode::Double, nullptr, dec1);
        vm.setSequence(&seq);
        vm.jump(2);
        check(waitFor([&] { return vm.cursor() == 2 && vm.keyCount() == 2; }),
              QStringLiteral("⑭ 双幅一好一坏 ⇒ **显示这一屏且保留原本空位**（游标 %1 / keys %2）")
                  .arg(vm.cursor()).arg(vm.keyCount()));
        check(vm.imageA().isNull() && !vm.imageB().isNull(),
              QStringLiteral("⑭ 坏的那一页留空位（imageA 空 %1）/ 另一页照常显示（有图 %2）")
                  .arg(vm.imageA().isNull()).arg(!vm.imageB().isNull()));
        check(!vm.loading(), QStringLiteral("⑭ 双幅留空位 ⇒ **不等、不跳**（loading %1）")
                                 .arg(vm.loading()));

        Sequence seq2; fill(seq2, mk(6));
        const QSet<QString> bothBad{ QStringLiteral("k2"), QStringLiteral("k3") };
        auto dec2 = [&](const Sequence::Item &it) -> QImage {
            if (bothBad.contains(it.label)) return QImage();
            QImage img(8, 8, QImage::Format_RGB32);
            img.fill(Qt::red);
            return img;
        };
        ViewerModel vm2(7, Mode::Double, nullptr, dec2);
        vm2.setSequence(&seq2);
        vm2.jump(2);
        check(waitFor([&] { return vm2.cursor() == 4 && vm2.keyCount() == 2; }),
              QStringLiteral("⑭ 双幅两页全坏 ⇒ **跳过**（游标 2 ⇒ %1）").arg(vm2.cursor()));
    }

    // ⑮ ★★ **等待 / 唤醒（四类事件）**（14 号调研 §7.1）：没活干时驱动线程**阻塞在唯一的
    //    等待点**上（不是轮询、没有切片），只有"事件"能叫醒它 —— ① 位置变（发布「位置与操作」）
    //    ② 换窗口 / 换版面（同一份发布里） ③ 到点（时间前进 + 起播） ④ 停机。
    //    **注入 `WaitFn`** ⇒ 全部确定性、不靠真实 sleep 卡时间。
    //    ⚠️ 变异测试：**发布后不唤醒** / 换窗口不唤醒 / 停机不参与同一个 signal ⇒ 本条**必须失败**。
    {
        struct VNow {
            std::atomic<qint64> t{0};
            ViewerModel::NowFn fn() { return [this] { return t.load(std::memory_order_relaxed); }; }
        };
        //   注入的等待：**真 park 在同一条信号上**（不是轮询）；deadline < 0 ⇒ **无限等**（只等事件）。
        //   故意**不设真实上限** ⇒ 少了唤醒就真的不动（断言超时 ⇒ 失败），不会偷偷靠"兜底节拍"过关。
        int parks = 0;
        auto parkWait = [&parks](qint64, QSemaphore &wake) {
            ++parks;
            wake.acquire();
        };

        VNow vn;
        Sequence seq; fill(seq, mk(20));
        ViewerModel vm(8, Mode::Single, nullptr, decoder({}), vn.fn(), parkWait);
        vm.setSequence(&seq);
        vm.jump(0);                                    // 生产顺序：装好来源就"要第一屏"
        check(waitFor([&] { return vm.keyCount() > 0 && vm.cursor() == 0; }),
              QStringLiteral("⑮ 起手：第一屏上屏（游标 %1）").arg(vm.cursor()));
        check(parks > 0,
              QStringLiteral("⑮ 驱动线程**进入过等待**（park %1 次）—— 是阻塞，不是轮询")
                  .arg(parks));

        //   ① **位置变** ⇒ 发布「位置与操作」+ 叫醒 ⇒ 确定下一屏起点
        vm.jump(7);
        check(waitFor([&] { return vm.cursor() == 7 && vm.keyCount() > 0; }),
              QStringLiteral("⑮① **位置变**：jump(7) ⇒ 落在第 7 条（游标 %1）").arg(vm.cursor()));

        //   ③ **到点**（时间前进）⇒ 显式唤醒 ⇒ 放映推进一屏
        vm.setIntervalMs(100);
        vm.jump(0);
        check(waitFor([&] { return vm.cursor() == 0 && vm.keyCount() > 0; }),
              QStringLiteral("⑮③ 回到第 0 屏"));
        vm.startSlideshow();
        check(waitFor([&] { return vm.cursor() == 1; }),
              QStringLiteral("⑮③ 起播先走一屏（游标 %1）").arg(vm.cursor()));
        vn.t.fetch_add(100);                           // ★ 时间前进
        vm.wakeDriver();                               // ★ 阻塞化之后："时间前进"也是一个事件
        check(waitFor([&] { return vm.cursor() == 2; }),
              QStringLiteral("⑮③ **到点**：时间前进 + 显式唤醒 ⇒ 推进一屏（游标 %1）")
                  .arg(vm.cursor()));
        vm.stopSlideshow();
        check(waitFor([&] { return !vm.slideshowRunning(); }),
              QStringLiteral("⑮③ **停播**（状态位清掉 + 叫醒）"));
    }
    //   ② **窗口 / 版面变**（同一份发布）：目标先"答不出来"（序列还没扫到）⇒ 换窗口 + 唤醒 ⇒ 重新解出、落地。
    {
        struct VNow {
            std::atomic<qint64> t{0};
            ViewerModel::NowFn fn() { return [this] { return t.load(std::memory_order_relaxed); }; }
        };
        VNow vn;
        Sequence seq;                                  // NotReady：一条都还没有
        ViewerModel vm(8, Mode::Single, nullptr, decoder({}), vn.fn(),
                       [](qint64, QSemaphore &wake) { wake.acquire(); });
        vm.setSequence(&seq);
        vm.jump(30);                                   // 目标超出当前 count + 扫描没结束 ⇒ 答不出来
        check(!waitFor([&] { return vm.keyCount() > 0; }, 200),
              QStringLiteral("⑮② 还没扫到 ⇒ 没有可显示的一屏（keys %1）").arg(vm.keyCount()));
        fill(seq, mk(60));                             // 序列长进来（= UiModel 收到通知）
        vm.sequenceChanged();                          // **换窗口**（version 单调增）+ 唤醒
        check(waitFor([&] { return vm.cursor() == 30 && vm.keyCount() > 0; }),
              QStringLiteral("⑮② **窗口变**：换窗口 + 唤醒 ⇒ 落到第 30 条（游标 %1）").arg(vm.cursor()));
    }
    //   ④ **停机**：正在等的时候拆 viewer ⇒ **立刻**回来（停机参与**同一个** signal）。
    //     这里的注入等待**带真实上限**（500ms）：少了停机唤醒也只是"慢一拍" ⇒ 判红而不会挂死。
    {
        Sequence seq; fill(seq, mk(400));
        struct VNow {
            std::atomic<qint64> t{0};
            ViewerModel::NowFn fn() { return [this] { return t.load(std::memory_order_relaxed); }; }
        };
        VNow vn;
        auto* vm = new ViewerModel(8, Mode::Single, nullptr, decoder({}), vn.fn(),
                                   [](qint64, QSemaphore &wake) {
                                       wake.tryAcquire(1, QDeadlineTimer(500));
                                   });
        vm->setSequence(&seq);
        vm->setIntervalMs(5000);                       // 长间隔 ⇒ 驱动"正在等"
        vm->jump(0);
        check(waitFor([&] { return vm->keyCount() > 0; }), QStringLiteral("⑮④ 进入有一屏"));
        vm->startSlideshow();
        check(waitFor([&] { return vm->slideshowRunning(); }), QStringLiteral("⑮④ 在播（驱动在长等）"));
        QElapsedTimer t;
        t.start();
        delete vm;                                     // ~ViewerModel：置停机 + 唤醒 + join
        const qint64 took = t.elapsed();
        check(took < 100,
              QStringLiteral("⑮④ **停机**：正在等时拆 viewer ⇒ 立刻回来（%1 ms）").arg(took));
    }

    // ⑯ ★★ **未备好 ⇒ 等待（一个像素都不许先上屏）** —— 设计 12 §8.9「四」2：
    //    上层读缓存自己判：要显示的那一屏里还有**未备好**的 ⇒ loading、**不上屏**；备好了才显示。
    //    现场怎么造（**确定性**，不靠 sleep 卡时间）：把"预取那一条"与"要跳过去那一条"的解码
    //    都卡在闸门上 ——
    //      · 驱动线程卡在**预取那一条**上 ⇒ 此时 `jump(100)` 不会有人替它解码；
    //      · 放行预取那一条 ⇒ 它**写入缓存**并投"缓存变了" ⇒ 上层判一次：这一刻"100"还卡在
    //        闸门上（**未备好**）⇒ 必须**等待**（不上屏）；
    //      · 再放行 ⇒ "100"备好 ⇒ 那条通知到了才上屏（且上屏时图已经在手）。
    //    ⚠️ 变异测试（把"未备好"当"已备好" ⇒ 该等待却显示）⇒ 本条**必须失败**。
    {
        Sequence seq; fill(seq, mk(200));
        QSemaphore gate;                               // 解码卡在闸门上：由用例逐条放行
        std::atomic<int> stuck{0};                      // 已卡进闸门的解码条数（夹具自证）
        auto dec = [&](const Sequence::Item &it) -> QImage {
            if (it.ord == 1 || it.ord == 100) { stuck.fetch_add(1); gate.acquire(); }
            QImage img(8, 8, QImage::Format_RGB32);
            img.fill(Qt::red);
            return img;
        };
        ViewerModel vm(7, Mode::Single, nullptr, dec);
        vm.setSequence(&seq);
        vm.jump(0);
        check(waitFor([&] { return vm.keyCount() > 0 && !vm.imageA().isNull(); }),
              QStringLiteral("⑯ 先站到第 0 屏（有图）"));
        check(waitFor([&] { return stuck.load() > 0; }, 1000),
              QStringLiteral("⑯ 夹具：驱动线程卡在预取那一条的解码上（卡住 %1 条）")
                  .arg(stuck.load()));
        const QVector<QString> keys0 = vm.keys();
        vm.jump(100);                                  // ★ 要跳过去的那一条还卡在闸门上（未备好）
        gate.release(1);                               // 放行预取那一条 ⇒ 缓存变了 ⇒ 上层判一次
        check(!waitFor([&] { return vm.cursor() == 100; }, 300) && vm.keys() == keys0 && vm.loading(),
              QStringLiteral("⑯ 未备好 ⇒ **等待**：位置不动 / 还是第 0 屏 / loading（游标 %1 / keys %2）")
                  .arg(vm.cursor()).arg(vm.keys().join(QLatin1Char(','))));
        gate.release(1);                               // 放行"100" ⇒ 备好 ⇒ 该上屏了
        check(waitFor([&] { return vm.cursor() == 100 && vm.keyCount() > 0; })
                  && !vm.imageA().isNull(),
              QStringLiteral("⑯ 备好 ⇒ **显示**（游标 %1 / keys %2 / 有图 %3）")
                  .arg(vm.cursor()).arg(vm.keyCount()).arg(!vm.imageA().isNull()));
        check(!vm.loading(), QStringLiteral("⑯ 显示之后 ⇒ loading 清掉（%1）").arg(vm.loading()));
    }

    // ⑰ ★★ **"缓存变了"那一条通知：一个在飞就不再投（合并）** —— 设计 12 §8.9「二」：
    //    向上**只有一条、没有内容**的通知。这里**故意不跑事件循环**（那条通知一直"在飞"）
    //    ⇒ 引擎这一段时间里产生的**每一次**"缓存变了"都必须被合并掉。
    //    ⚠️ 变异测试（每次变更都投一条）⇒ 真投出的条数远大于 1 ⇒ 本条**必须失败**。
    {
        Sequence seq; fill(seq, mk(400));
        std::atomic<int> posts{0};
        ViewerModel vm(8, Mode::Single, nullptr, decoder({}));
        vm.setNotifyHook([&] { posts.fetch_add(1); });   // 只记"真投出去"的那些
        vm.setSequence(&seq);
        vm.jump(0);
        //   ★ 不 processEvents、不 waitFor：让那一条通知一直留在事件队列里（"在飞"）
        QThread::msleep(300);
        const int n = posts.load();
        check(n >= 1, QStringLiteral("⑰ 引擎确实投过那条通知（真投出 %1 条 ≥ 1）").arg(n));
        check(n == 1, QStringLiteral("⑰ **一个在飞就不再投**：真投出 %1 条（期望 1）").arg(n));
        //   让事件循环跑起来 ⇒ 照常判一次并上屏（顺带证明那一条通知是活的）
        check(waitFor([&] { return vm.keyCount() > 0 && !vm.imageA().isNull(); }),
              QStringLiteral("⑰ 通知到了 ⇒ 读最新状态判一次并上屏（keys %1）").arg(vm.keyCount()));
    }

    // ⑱ ★★ T2d-1d-③b：**「位置与操作」是同一份发布** —— 引擎看的那一份里，位置与版面
    //    永远同属上层当时要的那一次操作：
    //     ① **"正在等某一屏"期间切版面** ⇒ 引擎**按新版面重解那一屏**（位置没变也要重解）
    //        ⇒ 那一屏落地、**不停在 loading**（③a 报的边角："切版面不重发请求 ⇒ 停在 loading"）；
    //     ② **发出去的每一份「位置与操作」都必须自洽**（位置 / 最近方向 / 版面 / 循环模式 / 窗口
    //        同属一次发布）：**撕裂**（半份带着旧版面）的次数必须是 **0**。
    //    为什么用两个钩子（与 `setNotifyHook` / `Preloader::setPublishHook` 同一条纪律 ——
    //    "没有别的可观测手段"）：**发出去的那一份**只有 `setPublishHook` 看得见；
    //    **引擎实际用的那一份**只有 `setResolveHook` 看得见。
    //    ⚠️ 变异测试：① 把"位置"与"版面 / 窗口"**分两次发布** ⇒ ②**必须失败**
    //      （先发出去的那一份带着旧版面）；② 切版面**不重解**（改造前那套"只在位置还没解出来
    //      时才重解"）⇒ ①**必须失败**。
    {
        Sequence seq; fill(seq, mk(200));
        QSemaphore gate;                               // 卡住"要显示的那一屏"的解码 ⇒ 上层正在等
        std::atomic<bool> gated{false};
        std::atomic<int>  stuck{0};                    // 已卡进闸门的解码条数（夹具自证）
        auto dec = [&](const Sequence::Item &it) -> QImage {
            if (it.ord == 100 && gated.load(std::memory_order_relaxed)) {
                stuck.fetch_add(1, std::memory_order_relaxed);
                gate.acquire();
            }
            QImage img(8, 8, QImage::Format_RGB32);
            img.fill(Qt::red);
            return img;
        };
        //   验证用：① 每一份**发出去**的发布值是否与上层当时的状态自洽（半份 = 撕裂）；
        //            ② 引擎每次"按一份发布值解那一屏"用的版面
        std::atomic<bool> expectDouble{false};
        std::atomic<int>  resolvedDouble{0}, tornResolve{0}, tornPublish{0}, halfPublish{0};
        ViewerModel vm(8, Mode::Single, nullptr, dec);
        vm.setResolveHook([&](int, Mode m) {
            const bool dbl = (m == Mode::Double);
            if (dbl) resolvedDouble.fetch_add(1, std::memory_order_relaxed);
            if (expectDouble.load(std::memory_order_relaxed) != dbl)
                tornResolve.fetch_add(1, std::memory_order_relaxed);
        });
        vm.setPublishHook([&](const NavPublish &p) {
            //   ① 发布值里的版面必须 == 上层**此刻**的版面（分两次发布会先发一份旧版面的）
            if ((p.mode == Mode::Double) != vm.doublePage())
                tornPublish.fetch_add(1, std::memory_order_relaxed);
            //   ② 位置 / 循环模式 / 窗口也必须同属这一份（窗口的版面 == 发布值里的版面）
            if (p.win && p.win->mode() != p.mode)
                halfPublish.fetch_add(1, std::memory_order_relaxed);
        });
        vm.setSequence(&seq);
        gated.store(true, std::memory_order_relaxed);   // 先卡住：这一屏要"还没备好"
        vm.jump(100);
        //   夹具自证：引擎**已经**在解这一屏（卡在闸门上）—— `loading` 由 GUI 线程当场置，
        //   只有 `stuck` 能证明驱动线程真的进了这一次解（否则"引擎还没开始解"会被误当成撕裂）
        //   ⚠️ 断言一律"**先求值、再拼文案**"（实参求值次序不定）
        const bool waiting18 = waitFor([&] { return vm.loading() && stuck.load() > 0; }, 1000);
        const int  stuck18   = stuck.load();
        const int  cur18     = vm.cursor();
        check(waiting18 && cur18 != 100,
              QStringLiteral("⑱ 正在等第 100 屏（loading = %1 / 卡住 %2 条 / 游标仍是 %3）")
                  .arg(vm.loading()).arg(stuck18).arg(cur18));

        expectDouble.store(true, std::memory_order_relaxed);
        vm.setDoublePage(true);                         // ★ 等这一屏的**期间**切版面
        gate.release(1);                                // 放行 ⇒ 该按**新版面**重解并显示
        const bool landed = waitFor([&] { return vm.keyCount() == 2 && !vm.loading(); });
        const int rd = resolvedDouble.load(std::memory_order_relaxed);
        const int nt = tornResolve.load(std::memory_order_relaxed);
        const int np = tornPublish.load(std::memory_order_relaxed);
        const int nh = halfPublish.load(std::memory_order_relaxed);
        check(landed, QStringLiteral("⑱① 切版面 ⇒ 那一屏按**新版面**解出来并显示"
                                    "（游标 %1 / keys %2 张 / loading %3）")
                          .arg(vm.cursor()).arg(vm.keyCount()).arg(vm.loading()));
        check(rd > 0, QStringLiteral("⑱① 引擎**重解**了那一屏（按新版面解过 %1 次；"
                                     "改造前位置已解过 ⇒ 不再重解）").arg(rd));
        check(np == 0 && nh == 0,
              QStringLiteral("⑱② **位置与版面 / 窗口打在同一份发布里**：发出去的半份 %1 次 / "
                             "窗口版面与发布值不一致 %2 次（都期望 0 —— 撕裂）").arg(np).arg(nh));
        check(nt == 0, QStringLiteral("⑱② 引擎**没有**用旧版面解过任何一次（%1 次，期望 0）").arg(nt));
    }

    // ⑲ ★★ **位置是"状态"，不是"队列"**（T2d-1d-③b）：引擎只看**最新那一份**「位置与操作」
    //    —— 连投多次（驱动线程这一段时间读不到）**只解最后那一份**。
    //    改造前：位置走请求环，每一条都会被逐条解出来（中间屏会依次上屏）。
    //    ⚠️ 变异测试（位置改回"逐条投递"）⇒ 解过的那几屏会 > 1 次 ⇒ 本条**必须失败**。
    {
        Sequence seq; fill(seq, mk(200));
        QSemaphore gate;                               // 卡住预取那一条 ⇒ 驱动线程这一段时间读不到发布值
        std::atomic<bool> gated{true};
        std::atomic<int>  stuck{0};
        auto dec = [&](const Sequence::Item &it) -> QImage {
            if (it.ord == 1 && gated.load(std::memory_order_relaxed)) {
                stuck.fetch_add(1, std::memory_order_relaxed);
                gate.acquire();
            }
            QImage img(8, 8, QImage::Format_RGB32);
            img.fill(Qt::red);
            return img;
        };
        std::atomic<int> resolves{0};
        ViewerModel vm(8, Mode::Single, nullptr, dec);
        vm.setResolveHook([&](int, Mode) { resolves.fetch_add(1, std::memory_order_relaxed); });
        vm.setSequence(&seq);
        vm.jump(0);
        check(waitFor([&] { return vm.cursor() == 0 && vm.keyCount() > 0; }),
              QStringLiteral("⑲ 先站到第 0 屏"));
        check(waitFor([&] { return stuck.load() > 0; }, 1000),
              QStringLiteral("⑲ 夹具：驱动线程卡在预取那一条上（卡住 %1 条）").arg(stuck.load()));
        const int before19 = resolves.load(std::memory_order_relaxed);
        vm.next();                                     // 连投三次（都是"位置"这一份的新版本）
        vm.next();
        vm.next();
        gate.release(1);                               // 放行 ⇒ 驱动只该看到**最新那一份**
        const bool landed19 = waitFor([&] { return vm.cursor() == 3 && vm.keyCount() > 0; });
        const int n19 = resolves.load(std::memory_order_relaxed) - before19;
        check(landed19, QStringLiteral("⑲ 连投三次 ⇒ 落到最后一屏（游标 %1 / keys %2）")
                            .arg(vm.cursor()).arg(vm.keyCount()));
        check(n19 == 1, QStringLiteral("⑲ **只解最新那一份**：引擎这一段时间解了 %1 次"
                                       "（期望 1 —— 位置是状态、不是队列）").arg(n19));
    }

    return check.finish();
}

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, &runVmCoreSelfTest);
}
