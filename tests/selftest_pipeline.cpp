#include "testsupport.h"

#include <QElapsedTimer>
#include <QImage>
#include <QImageReader>
#include <QSet>
#include <QString>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>

#include "fetchpipeline.h"
#include "sequence.h"

// ══ 管线自检（ib-test-pipeline；原先由 `image-browser --selftest-pipeline` 跑）════════
//   合成序列，新游标模型。

static int runFetchPipelineSelfTest()
{
    using namespace fetch;
    using namespace pipetest;
    testsupport::Check check(QStringLiteral("pipe"));

    // ① fetch **阻塞且必然拿到好图**（冷缓存下直接返回；不返回空/未就绪）
    {
        Cache c;
        Preloader pre(&c, decoder({}));
        Sequence seq; fill(seq, mk(10));
        Scheduler s(fullWindow(seq, Mode::Single), &c, &pre, 7, Mode::Single);
        const auto r = s.fetch(Scheduler::Op::Jump, 0, 0);
        check(!r.keys.isEmpty(), QStringLiteral("fetch **一定拿到好图**：冷缓存下也直接返回（不返回空）"));
        check(allGood(c, r.keys), QStringLiteral("fetch 返回的 keys **必然是缓存里的好图**"));
        check(r.cursor == 0, QStringLiteral("单幅 · 进入=jump：新游标 = 0（游标 = 当前所显示那一屏的起点）"));
    }

    // ② **未命中就等**：返回时已经就绪（内部把本组跑完才返回）
    {
        Cache c;
        auto st = std::make_shared<Stats>();
        Preloader pre(&c, decoder({}, st, 5));            // 每张 5ms，制造"要等"的场景
        Sequence seq; fill(seq, mk(6));
        Scheduler s(fullWindow(seq, Mode::Single), &c, &pre, 7, Mode::Single);
        const auto r = s.fetch(Scheduler::Op::Jump, 0, 0);
        check(st->calls.load() >= 1, QStringLiteral("未命中就等：内部驱动了解码（调用了 %1 次）").arg(st->calls.load()));
        check(allGood(c, r.keys), QStringLiteral("未命中就等：返回时本组已经就绪"));
    }

    // ③ 坏条目（不可读）：**如实报回**（key + 状态 = 坏），并且**不重试**
    //   ★ T2d-1d-①：调度器**不再替上层"跳过坏图"** —— 它把本屏各条目的 key + 状态报回去，
    //     由上层决定「显示 / 等待 / 跳过」（单幅跳过 / 双幅留空位）。
    {
        Cache c;
        auto st = std::make_shared<Stats>();
        QSet<QString> bad{QStringLiteral("k0")};
        Preloader pre(&c, decoder(bad, st));
        Sequence seq; fill(seq, mk(6));
        Scheduler s(fullWindow(seq, Mode::Single), &c, &pre, 7, Mode::Single);
        const QString badKey = seq.at(0).cacheKey();            // ★ 缓存键 = ord
        const auto r = s.fetch(Scheduler::Op::Jump, 0, 0);      // 下一屏起点 k0 是坏的
        check(r.keys.size() == 1 && r.states.size() == 1 && r.keys.first() == badKey
                  && r.states.first() == Cache::State::Bad,
              QStringLiteral("坏条目：**如实报回**（key = 那一条 / 状态 = 坏）"));
        check(c.isBad(badKey) && c.badReason(badKey) == Cache::BadReason::Unreadable,
              QStringLiteral("坏条目：缓存里留的是「坏·不可读」（打不开 / 解码失败）"));
        check(c.count() == 0 && c.badCount() == 1,
              QStringLiteral("坏条目：**不占缓存长度**（备好 %1 张 / 坏 %2 条）")
                  .arg(c.count()).arg(c.badCount()));

        const int before = st->calls.load();
        for (int i = 0; i < 5; ++i) s.fetch(Scheduler::Op::Jump, 0, 0);      // 反复要这一屏
        check(st->calls.load() == before,
              QStringLiteral("坏条目：**不重试**（反复要这一屏也不再解码它：%1 → %2）")
                  .arg(before).arg(st->calls.load()));
    }

    // ④ 缓存键 = 图（同一张两次要图 ⇒ 不重复解码）
    {
        Cache c;
        auto st = std::make_shared<Stats>();
        Preloader pre(&c, decoder({}, st));
        Sequence seq; fill(seq, mk(8));
        Scheduler s(fullWindow(seq, Mode::Single), &c, &pre, 7, Mode::Single);
        s.fetch(Scheduler::Op::Jump, 0, 3);
        const int n1 = st->calls.load();
        s.fetch(Scheduler::Op::Jump, 0, 3);
        s.fetch(Scheduler::Op::Jump, 0, 3);
        check(st->calls.load() == n1,
              QStringLiteral("缓存键 = 图：同一张反复要 ⇒ **不重复解码**（%1 → %2）").arg(n1).arg(st->calls.load()));
    }

    // ⑤ 淘汰 = **掉出窗口**（retain）
    {
        Cache c;
        Preloader pre(&c, decoder({}));
        Sequence seq; fill(seq, mk(40));
        Scheduler s(fullWindow(seq, Mode::Single), &c, &pre, 5, Mode::Single);        // 窗口 5
        s.fetch(Scheduler::Op::Jump, 0, 0);
        const int n0 = c.count();
        for (int i = 0; i < 12; ++i) s.fetch(Scheduler::Op::Next, i);
        check(c.count() <= 5, QStringLiteral("淘汰 = 掉出窗口：缓存只留窗口内的（%1 张 ≤ 窗口 5）").arg(c.count()));
        check(!c.hasImage(QStringLiteral("k0")), QStringLiteral("淘汰 = 掉出窗口：早就离开窗口的 k0 已被丢弃"));
        check(n0 > 0, QStringLiteral("淘汰前缓存里有图（%1 张）").arg(n0));
    }

    // ⑥ 批 = 1 退化成串行；批 = 4 真并发
    {
        {
            Cache c;
            auto st = std::make_shared<Stats>();
            Preloader pre(&c, decoder({}, st, 6));
            pre.setBatch(1);
            Sequence seq; fill(seq, mk(6));
            Scheduler s(fullWindow(seq, Mode::Single), &c, &pre, 7, Mode::Single);
            s.fetch(Scheduler::Op::Jump, 0, 0);
            check(st->peak.load() == 1, QStringLiteral("批 = 1：**退化成串行**（峰值并发 1）"));
        }
        {
            Cache c;
            auto st = std::make_shared<Stats>();
            Preloader pre(&c, decoder({}, st, 6));
            pre.setBatch(4);
            Sequence seq; fill(seq, mk(6));
            Scheduler s(fullWindow(seq, Mode::Single), &c, &pre, 7, Mode::Single);
            s.fetch(Scheduler::Op::Jump, 0, 0);
            check(st->peak.load() >= 2 && st->peak.load() <= 4,
                  QStringLiteral("批 = 4：**真的并发解码**（峰值 %1 ∈ [2,4]）").arg(st->peak.load()));
        }
    }

    // ⑦ 边完成边发布（当前屏不必等批里最慢那张）
    {
        Cache c;
        QElapsedTimer t;
        qint64 firstAt = -1, total = -1;
        Preloader pre(&c, [](const Sequence::Item &it) -> QImage {
            QThread::msleep(it.label == QLatin1String("k0") ? 5 : 60);   // 第 0 张快、其余慢
            QImage img(8, 8, QImage::Format_RGB32);
            img.fill(Qt::red);
            return img;
        });
        pre.setBatch(4);
        pre.setPublishHook([&](const QString &k) { if (firstAt < 0) firstAt = t.elapsed(); });
        const auto v = mk(4);
        for (const Sequence::Item &it : v) pre.request(it);
        t.start();
        pre.step();
        total = t.elapsed();
        check(firstAt >= 0 && firstAt * 2 < total,
              QStringLiteral("边完成边发布：第 0 张 %1 ms 就发布 vs 整批 %2 ms").arg(firstAt).arg(total));
    }

    // ⑧ 双幅 · fetch 级：从头锚定、每张只出现在一屏
    {
        Cache c;
        Preloader pre(&c, decoder({}));
        Sequence seq; fill(seq, mk(8));
        Scheduler s(fullWindow(seq, Mode::Double), &c, &pre, 7, Mode::Double);
        const auto a = s.fetch(Scheduler::Op::Jump, 0, 0);
        check(a.keys.size() == 2 && allGood(c, a.keys), QStringLiteral("双幅：每屏 2 张，且都是好图"));
        const auto b = s.fetch(Scheduler::Op::Next, a.cursor);
        check(b.keys.size() == 2 && allGood(c, b.keys), QStringLiteral("双幅：下一屏也是 2 张好图"));
        check(a.keys.last() != b.keys.first(),
              QStringLiteral("双幅：**每张只出现在一屏** —— 上一屏的第二张不再出现在下一屏"));
        check(b.cursor == 2, QStringLiteral("双幅：新游标 = 游标 + 本组大小（= 2）"));
    }

    // ⑨ 双幅 · fetch 级：相连两屏不重叠（每张只出现在一屏）
    {
        Cache c;
        auto st = std::make_shared<Stats>();
        Preloader pre(&c, decoder({}, st));
        Sequence seq; fill(seq, mk(8));
        Scheduler s(fullWindow(seq, Mode::Double), &c, &pre, 7, Mode::Double);
        const auto a = s.fetch(Scheduler::Op::Jump, 0, 0);
        const auto b = s.fetch(Scheduler::Op::Next, a.cursor);
        check(a.keys.size() == 2 && b.keys.size() == 2, QStringLiteral("双幅：每屏 2 张"));
        check(a.keys.last() != b.keys.first(),
              QStringLiteral("双幅：**不重叠** —— 上一屏的第二张**不再**出现在下一屏"));
        check(b.cursor == 2, QStringLiteral("双幅：新游标 = 游标 + 本组大小（= 2）"));
    }

    // ⑩ 真实解码（证明 DecodeFn 是真的：QImageReader + 真文件）
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("t.png"));
        QImage src(24, 16, QImage::Format_RGB32);
        src.fill(Qt::green);
        const bool saved = src.save(path);
        Cache c;
        Preloader pre(&c, [&path](const Sequence::Item &) -> QImage { QImageReader r(path); return r.read(); });
        Sequence::Item real;
        real.kind = Sequence::Kind::Image;
        real.ord = 0;
        real.label = QStringLiteral("real");
        real.displayName = real.label;
        real.size = QSize(24, 16);
        real.loc.diskPath = path;          // ★ v5：读取位置 = 磁盘路径 + 成员链
        
        Sequence seq; fill(seq, QVector<Sequence::Item>{ real });
        Scheduler s(fullWindow(seq, Mode::Single), &c, &pre, 7, Mode::Single);
        const auto r = s.fetch(Scheduler::Op::Jump, 0, 0);
        const QImage got = r.keys.isEmpty() ? QImage() : c.get(r.keys.first());
        check(saved && !got.isNull() && got.size() == QSize(24, 16),
              QStringLiteral("真实解码：preloader 用 QImageReader 解出真图（%1x%2）").arg(got.width()).arg(got.height()));
    }

    // ⑪ ★★ **覆盖不足 vs 真到头**（T2d-1c-① 的判定条件；两者在**行为上**必须分开）：
    //    · 要的位置不在有界窗口（`fetch::Window`）的覆盖范围内（`notCovered` = 答不出来）⇒ 答复**空 keys**，
    //      调用方这一轮什么都不做、等一份更新的窗口（**不许**当成"到头" —— 那会让 loading 常亮、
    //      也会让循环模式在"还没扫到"时错误回绕）；
    //    · 真到头（覆盖内、序列已扫完、真的没有下一组）⇒ 空 keys 且 `notCovered` **假**。
    //    调度器**从不请求任何东西**（引擎纯消费）。
    {
        Cache c;
        Preloader pre(&c, decoder({}));
        Sequence seq; fill(seq, mk(40));

        //   窄窗口：窗口中心 0、只覆盖它附近（预取半宽 1 / 屏余量 1）⇒ 目标 30 在覆盖外
        Scheduler s(windowFor(seq, Mode::Single, 0, 1, 1), &c, &pre, 7, Mode::Single);
        const auto far = s.fetch(Scheduler::Op::Jump, 0, 30);
        check(far.notCovered && far.keys.isEmpty(),
              QStringLiteral("⑪ 覆盖不足：远跳（30）⇒ 「答不出来」（空 keys + notCovered，不请求）"));
        const auto outside = s.fetch(Scheduler::Op::Next, 30);
        check(outside.notCovered && outside.keys.isEmpty(),
              QStringLiteral("⑪ 覆盖不足：连\"现在在哪\"都不在窗口里 ⇒ 同样答不出来"));

        //   真到头：覆盖**整条**序列（扫完）⇒ 最后一屏再 next = 空 keys，但**不是**"覆盖不足"
        Scheduler s2(fullWindow(seq, Mode::Single), &c, &pre, 7, Mode::Single);
        const auto end = s2.fetch(Scheduler::Op::Next, 39);
        check(!end.notCovered && end.keys.isEmpty(),
              QStringLiteral("⑪ 真到头：覆盖内最后一屏再 next ⇒ 空 keys 且**不是** notCovered"));

        //   还没扫到（count = 0、`!scanFinished`）⇒ 同样是"不知道"（等下一份窗口）
        Sequence notReady;
        Scheduler s3(fullWindow(notReady, Mode::Single), &c, &pre, 7, Mode::Single);
        const auto notYet = s3.fetch(Scheduler::Op::Jump, 0, 0);
        check(notYet.notCovered && notYet.keys.isEmpty(),
              QStringLiteral("⑪ 序列还没到（目标超出当前 count + 扫描没结束）⇒ notCovered"));

        //   还没有有界窗口 ⇒ 一切都是"不知道"（调用方等一份窗口，而不是当到头）
        Scheduler s4(nullptr, &c, &pre, 7, Mode::Single);
        const auto noWin = s4.fetch(Scheduler::Op::Jump, 0, 0);
        check(noWin.notCovered && noWin.keys.isEmpty(),
              QStringLiteral("⑪ 还没有有界窗口 ⇒ notCovered（什么都不做，等更新的一份）"));
    }

    // ⑫ ★★ **预取队列的前向条数 / 后向条数规则**（设计 12 §8.9「五」1–3）：
    //    状态只有两个值 —— `f` = 前向条数（起始 6、限 [1, L−1]）、`d` = 最近方向（无方向 = 前）；
    //    队列 = **最近方向那一侧 f 条 + 另一侧 L−f 条**（长度恒 L）；
    //    跳转 / 换版面 / 换来源 ⇒ 回到起始。
    //    ⚠️ 变异测试（故意把代码改错，看用例能不能发现）：前向条数改成"按同向/反向 ±1"（一跳 5 条、不是逐步）⇒ 本条**必须失败**。
    {
        Cache c;
        Preloader pre(&c, decoder({}));
        Sequence seq; fill(seq, mk(400));
        Scheduler s(fullWindow(seq, Mode::Single), &c, &pre, /*window=*/8, Mode::Single);
        auto fOf = [](const QVector<int> &q) {
            int n = 0;
            for (int x : q) if (x > 0) ++n;
            return n;
        };
        auto dump = [](const QVector<int> &q) {
            QStringList l;
            for (int x : q) l << QString::number(x);
            return l.join(QLatin1Char(','));
        };
        const auto q0 = s.prefetchOffsets();

        s.fetch(Scheduler::Op::Jump, 0, 100);          // 跳转 ⇒ 回到起始
        const auto q1 = s.prefetchOffsets();
        check(q1 == QVector<int>({ 1, 2, 3, 4, 5, 6, -1, -2 }) && q1.size() == 8,
              QStringLiteral("⑫ 起始（跳转后）：f = 6 ⇒ [+1..+6, −1, −2]，长度恒 8（%1）")
                  .arg(dump(q1)));

        s.fetch(Scheduler::Op::Jump, 0, 101);          // 向前一步 ⇒ f = 7
        const auto q2 = s.prefetchOffsets();
        check(fOf(q2) == 7 && q2.size() == 8 && q2.last() == -1,
              QStringLiteral("⑫ 持续向前：f = 7 ⇒ [+1..+7, −1]（%1）").arg(dump(q2)));

        s.fetch(Scheduler::Op::Jump, 0, 102);          // 再向前 ⇒ f 封顶（不越界）
        check(fOf(s.prefetchOffsets()) == 7,
              QStringLiteral("⑫ f **上限 7**（再向前不涨）：%1").arg(dump(s.prefetchOffsets())));

        s.fetch(Scheduler::Op::Jump, 0, 101);          // 反向一次 ⇒ 排序翻转 + f = 6
        const auto q3 = s.prefetchOffsets();
        check(q3.mid(0, 2) == QVector<int>({ -1, -2 }) && fOf(q3) == 6 && q3.size() == 8,
              QStringLiteral("⑫ **反向一次**：新方向挪到前面 + f = 6 ⇒ [−1,−2,+1..+6]（%1）")
                  .arg(dump(q3)));

        s.fetch(Scheduler::Op::Jump, 0, 100);          // 再反向 ⇒ f = 5
        const auto q4 = s.prefetchOffsets();
        check(q4.mid(0, 3) == QVector<int>({ -1, -2, -3 }) && fOf(q4) == 5,
              QStringLiteral("⑫ **再反向**：f = 5 ⇒ [−1,−2,−3,+1..+5]（%1）").arg(dump(q4)));

        for (int i = 0; i < 8; ++i) s.fetch(Scheduler::Op::Jump, 0, 99 - i);  // 持续向后 ⇒ f 落到 1
        const auto q5 = s.prefetchOffsets();
        check(fOf(q5) == 1 && q5.first() == -1 && q5.last() == +1 && q5.size() == 8,
              QStringLiteral("⑫ 持续向后：f = 1（**下限**）⇒ [−1..−7, +1]（%1）").arg(dump(q5)));

        s.setMode(Mode::Single);                        // 换版面（同版面也走"回到起始"）
        const auto q6 = s.prefetchOffsets();
        check(q6 == QVector<int>({ 1, 2, 3, 4, 5, 6, -1, -2 }),
              QStringLiteral("⑫ **换版面 ⇒ 回到起始**（f = 6、d = 前）：%1").arg(dump(q6)));
        check(q0 == q6, QStringLiteral("⑫ 起始队列可复现（跳转 / 换版面两条路一致）"));
    }

    // ⑬ ★★ **坏不占缓存长度、不挡队列后面的、不重试**（设计 12 §8.9「三」「五」）
    //    · **元数据已标"扫描期缺失" ⇒ 直接写"坏（缺失）"、不必真去开文件**；
    //    · 坏的那一格**不占长度**：队列后面的偏移照样要取到（预取长度 L 的预算不被它吃掉）。
    //    ⚠️ 变异测试（坏当"尽头 / 占长度" ⇒ 队列在坏条目处截断）⇒ 后面的偏移取不到 ⇒ 本条**必须失败**。
    {
        Cache c;
        QVector<QString> order;
        auto dec = [&](const Sequence::Item &it) -> QImage {
            order << it.label;
            QImage img(8, 8, QImage::Format_RGB32);
            img.fill(Qt::red);
            return img;
        };
        Sequence seq; fill(seq, mk(40, QSet<int>{ 22 }));   // ★ 第 22 条：扫描期已标"缺失"
        Preloader pre(&c, dec);
        //   起始队列 = [+1..+6, −1, −2]（L = 8）⇒ 本屏 20 之后取 21..26 / 19 / 18
        Scheduler s(fullWindow(seq, Mode::Single), &c, &pre, /*window=*/8, Mode::Single);
        order.clear();
        s.fetch(Scheduler::Op::Jump, 0, 20);
        while (pre.pendingKeyCount() > 0) pre.step();
        const QString missingKey = seq.at(22).cacheKey();   // ★ 缓存键 = ord
        check(order.contains(QStringLiteral("k25")) && order.contains(QStringLiteral("k19"))
                  && order.contains(QStringLiteral("k18")),
              QStringLiteral("⑬ 坏**不挡队列后面的**：8 个偏移都取到（%1）")
                  .arg(order.join(QLatin1Char(','))));
        check(!order.contains(QStringLiteral("k22")) && c.isBad(missingKey)
                  && c.badReason(missingKey) == Cache::BadReason::Missing,
              QStringLiteral("⑬ 扫描期缺失 ⇒ **直接写「坏·缺失」、不开文件**（解码过 %1 次）")
                  .arg(order.count(QStringLiteral("k22"))));
        check(c.count() == 8 && c.badCount() == 1,
              QStringLiteral("⑬ 缓存长度**只数备好的**（备好 %1 / 坏 %2；预取长度 L = 8）")
                  .arg(c.count()).arg(c.badCount()));
        const int before = order.size();
        for (int i = 0; i < 3; ++i) s.fetch(Scheduler::Op::Jump, 0, 20);
        while (pre.pendingKeyCount() > 0) pre.step();
        check(order.size() == before,
              QStringLiteral("⑬ **不重试**、备好的也不重复取（解码次数 %1 → %2）")
                  .arg(before).arg(order.size()));
    }

    // ⑭ ★★ **坏·不可读 ⇒ 不重试**（设计 12 §8.9「三」：坏的在本次查看生命周期内一直留着，
    //    队列再指到它 ⇒ 跳过、不再去取）。这里**单线程驱动**（自己 step），解码次数可确定地数。
    //    ⚠️ 变异测试（引擎不挡坏条目：只挡"备好"的）⇒ 回看同一屏会再解它一次 ⇒ 本条**必须失败**。
    {
        Cache c;
        QVector<QString> order;
        const QSet<QString> bad{ QStringLiteral("k22") };
        auto dec = [&](const Sequence::Item &it) -> QImage {
            order << it.label;
            if (bad.contains(it.label)) return QImage();          // 打不开 / 解码失败
            QImage img(8, 8, QImage::Format_RGB32);
            img.fill(Qt::red);
            return img;
        };
        Sequence seq; fill(seq, mk(40));
        Preloader pre(&c, dec);
        Scheduler s(fullWindow(seq, Mode::Single), &c, &pre, 7, Mode::Single);
        s.fetch(Scheduler::Op::Jump, 0, 20);
        while (pre.pendingKeyCount() > 0) pre.step();
        check(c.isBad(seq.at(22).cacheKey())
                  && c.badReason(seq.at(22).cacheKey()) == Cache::BadReason::Unreadable,
              QStringLiteral("⑭ 解码失败 ⇒ 写「坏·不可读」"));
        const int before = order.size();
        const int badBefore = order.count(QStringLiteral("k22"));
        for (int i = 0; i < 3; ++i) {
            s.fetch(Scheduler::Op::Jump, 0, 20);                  // 回看同一屏
            while (pre.pendingKeyCount() > 0) pre.step();
        }
        check(order.size() == before && order.count(QStringLiteral("k22")) == badBefore,
              QStringLiteral("⑭ 坏条目**不重试**（3 次回看：解码次数 %1 → %2，坏条目 %3 次）")
                  .arg(before).arg(order.size()).arg(order.count(QStringLiteral("k22"))));
    }

    // ⑮ ★★ **越界由 provider（有界窗口）按模式回答**（设计 12 §8.9「五」5）：
    //     不循环 ⇒ 越界即"没有这一条"⇒ 丢；**全部 / 文件夹循环 ⇒ 给回绕后的那一条**。
    //     ⇒ 循环模式下预取**也跟着回绕**（末尾用"全部循环"时，另一端 0,1,2… 也要预热）。
    //     ⚠️ 变异测试（访问器改成"越界一律 -1"）⇒ ①② 必须失败。
    {
        //   ① 全部循环 + 站在末尾 ⇒ 队列把另一端 0,1,2… 也备上
        Cache c;
        QVector<QString> order;
        auto dec = [&](const Sequence::Item &it) -> QImage {
            order << it.label;
            QImage img(8, 8, QImage::Format_RGB32);
            img.fill(Qt::red);
            return img;
        };
        Sequence seq; fill(seq, mk(20));
        Preloader pre(&c, dec);
        Scheduler s1(fullWindow(seq, Mode::Single, 0, /*loopMode=*/1), &c, &pre, 7,
                     Mode::Single);
        order.clear();
        s1.fetch(Scheduler::Op::Jump, 0, 19);                 // 站在末条（= 末屏）
        while (pre.pendingKeyCount() > 0) pre.step();
        check(order.contains(QStringLiteral("k0")) && order.contains(QStringLiteral("k1"))
                  && order.contains(QStringLiteral("k2")),
              QStringLiteral("⑮① **全部循环** + 末尾 ⇒ 预取越过末尾备到另一端（%1）")
                  .arg(order.join(QLatin1Char(','))));

        //   ② 文件夹循环 + 站在文件夹末条 ⇒ 在**该段**里绕（备到段起点那几条；不越出该段）
        Sequence tree;
        {
            auto row = [](int ord, int parentOrd, const QString &kind, const QString &label) {
                Sequence::Row r;
                r.ord = ord; r.parentOrd = parentOrd; r.kind = kind; r.label = label;
                r.pageIndex = -1; r.width = 1200; r.height = 1800;
                return r;
            };
            QVector<Sequence::Row> rows;
            for (int i = 0; i < 60; ++i)
                rows << row(i, -1, QStringLiteral("image"), QStringLiteral("r%1").arg(i));
            rows << row(60, -1, QStringLiteral("folder"), QStringLiteral("A"));
            for (int i = 0; i < 3; ++i)
                rows << row(61 + i, 60, QStringLiteral("image"), QStringLiteral("a%1").arg(i));
            rows << row(64, -1, QStringLiteral("folder"), QStringLiteral("B"));
            for (int i = 0; i < 20; ++i)      // B 段够长（63..82）⇒ 末尾在 +3 之外才绕
                rows << row(65 + i, 64, QStringLiteral("image"), QStringLiteral("b%1").arg(i));
            tree.buildFromRows(QStringLiteral("/synth/loopfolder"), rows, false,
                               QStringLiteral("loopfolder"));
        }
        Cache c2;
        QVector<QString> order2;
        auto dec2 = [&](const Sequence::Item &it) -> QImage {
            order2 << it.label;
            QImage img(8, 8, QImage::Format_RGB32);
            img.fill(Qt::red);
            return img;
        };
        Preloader pre2(&c2, dec2);
        Scheduler s2(fullWindow(tree, Mode::Single, 0, /*loopMode=*/2), &c2, &pre2, 7,
                     Mode::Single);
        //   B 段 = 63..82（20 条）；站在 80（段的末三条之前）⇒ 队列 [+1…+5] 里
        //   +1/+2 = b17(81)/b18(82) 还在段内，**+3 越出段末 ⇒ 回到段起 b0(63)**、+4/+5 ⇒ b1/b2。
        order2.clear();
        s2.fetch(Scheduler::Op::Jump, 0, 80);
        while (pre2.pendingKeyCount() > 0) pre2.step();
        check(order2.contains(QStringLiteral("b0")) && order2.contains(QStringLiteral("b2"))
                  && !order2.contains(QStringLiteral("a2"))
                  && !order2.contains(QStringLiteral("r59")),
              QStringLiteral("⑮② **文件夹循环**：越出段末 ⇒ 回到段起（%1）")
                  .arg(order2.join(QLatin1Char(','))));

        //   ③ 不循环 ⇒ 越界依旧**丢**（不许顺手把另一端也备上）
        Cache c3;
        QVector<QString> order3;
        auto dec3 = [&](const Sequence::Item &it) -> QImage {
            order3 << it.label;
            QImage img(8, 8, QImage::Format_RGB32);
            img.fill(Qt::red);
            return img;
        };
        Preloader pre3(&c3, dec3);
        Scheduler s3(fullWindow(tree, Mode::Single, 0, /*loopMode=*/0), &c3, &pre3, 7,
                     Mode::Single);
        order3.clear();
        s3.fetch(Scheduler::Op::Jump, 0, 80);                 // 同一位置：不循环 ⇒ 越界丢
        while (pre3.pendingKeyCount() > 0) pre3.step();
        check(!order3.contains(QStringLiteral("r0")) && !order3.contains(QStringLiteral("r1")),
              QStringLiteral("⑮③ **不循环** ⇒ 越界丢（不备另一端：%1）")
                  .arg(order3.join(QLatin1Char(','))));
    }

    return check.finish();
}

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, &runFetchPipelineSelfTest);
}
