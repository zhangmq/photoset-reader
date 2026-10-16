#include "testsupport.h"

#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QSet>
#include <QSize>
#include <QString>
#include <QThread>
#include <QVector>

#include "fetchpipeline.h"
#include "sequence.h"

// ══ 取图 / 预载管线 · **真实序列**验证（ib-test-pipeline-seq；原先由
//    `image-browser -d <目录> --selftest-pipeline-seq` 跑）══════
//   用例与素材无关：宽窄/张数**按真实尺寸自算**，不写死"全横版"这类假设。
static int runFetchPipelineSeqSelfTest(const QString &dir)
{
    using namespace fetch;
    using namespace pipetest;
    testsupport::Check check(QStringLiteral("seq"));

    // ★ 序列 = **查看序列的真实扫描**（文件夹 → 深度优先 + 自然排序 + **一趟读尺寸**）
    Sequence seq;
    seq.startScan(dir, 5, false);
    {
        QElapsedTimer t;
        t.start();
        while (!seq.scanFinished() && t.elapsed() < 300000) {
            seq.drain();
            QThread::msleep(2);
        }
        seq.drain();
    }
    if (seq.count() < 20) {
        // 夹具不具备（没给 `-d` / 目录里图太少）⇒ **整条套件跳过**：rc=77，不是通过
        return testsupport::skip(QStringLiteral("seq"),
                                 QStringLiteral("%1 里可读图片太少（%2）").arg(dir).arg(seq.count()));
    }
    check(seq.state() == Sequence::State::Scaned,
          QStringLiteral("真实序列：扫描到 Scaned（%1 项）").arg(seq.count()));

    const int total = seq.count();
    int withSize = 0;
    for (int i = 0; i < total; ++i) if (seq.at(i).size.isValid()) ++withSize;
    check(withSize == total, QStringLiteral("不解码读尺寸：全部 %1 项都有源尺寸（判宽窄不需要解码）").arg(withSize));

    auto st = std::make_shared<Stats>();
    auto decode = [&st](const Sequence::Item &it) -> QImage {
        st->calls.fetch_add(1);
        return Sequence::image(it);              // ★ **唯一解码实现**
    };

    // ① 单幅 · 进入 = jump：冷缓存也必然拿到好图
    {
        Cache c;
        Preloader pre(&c, decode);
        Scheduler s(fullWindow(seq, Mode::Single), &c, &pre, 7, Mode::Single);
        const auto r = s.fetch(Scheduler::Op::Jump, 0, 0);
        check(!r.keys.isEmpty() && allGood(c, r.keys),
              QStringLiteral("单幅 · 进入=jump：冷缓存下也直接拿到好图（不返回空）"));
        check(r.keys.size() == 1, QStringLiteral("单幅：一屏 1 张"));
    }

    // ② 单幅 · 整卷翻完：**每一步都拿到好图**（不出现"解码中"）
    {
        Cache c;
        Preloader pre(&c, decode);
        pre.setBatch(4);
        Scheduler s(fullWindow(seq, Mode::Single), &c, &pre, 7, Mode::Single);
        auto r = s.fetch(Scheduler::Op::Jump, 0, 0);
        int steps = 0, good = 0;
        while (steps < total && !r.keys.isEmpty()) {
            if (allGood(c, r.keys)) ++good;
            ++steps;
            const int cur = r.cursor;
            r = s.fetch(Scheduler::Op::Next, cur);
            if (r.keys.isEmpty()) break;
        }
        check(steps >= total - 1, QStringLiteral("整卷：翻完 %1 步（共 %2 项）").arg(steps).arg(total));
        check(good == steps, QStringLiteral("整卷：**每一步都拿到好图**（%1/%2）—— 不出现「解码中」").arg(good).arg(steps));
    }

    // ③ 双幅（简单）：每屏张数与**真实尺寸**一致（宽→1 / 窄→2）
    {
        Cache c;
        Preloader pre(&c, decode);
        Scheduler s(fullWindow(seq, Mode::Double), &c, &pre, 7, Mode::Double);
        int pairs = 0, singles = 0, mismatch = 0, dup = 0, sum = 0;
        QSet<int> seen;
        int cur = 0, screens = 0;
        while (screens++ < 20) {                       // ★ 走**真实屏序列**（不是 groupAt(i)）
            const Sequence::Group g = seq.groupAt(cur, Mode::Double);
            if (g.members.isEmpty()) break;
            // 期望 = 规则本身（**从头锚定**）：组起点窄 **且 下一条也窄** ⇒ 2 张，否则 1 张
            auto narrow = [&seq](int i) {
                if (i < 0 || i >= seq.count()) return false;
                const QSize q = seq.at(i).size;
                if (!q.isValid() || q.isEmpty()) return false;
                return double(q.width()) <= double(q.height()) * 1.2;
            };
            const int first = g.members.first();
            const int expect = (narrow(first) && narrow(first + 1)) ? 2 : 1;
            if (g.members.size() != expect) ++mismatch;
            for (int i : g.members) { if (seen.contains(i)) ++dup; seen.insert(i); }
            sum += g.members.size();
            (g.members.size() == 2 ? pairs : singles)++;
            const int nxt = seq.advance(cur, Mode::Double, Sequence::Dir::Next);
            if (nxt < 0 || nxt == cur) break;
            cur = nxt;
        }
        check(mismatch == 0, QStringLiteral("双幅（从头锚定）：每屏张数与**真实尺寸**一致（宽→1 / 窄→2）"));
        check(pairs + singles > 0, QStringLiteral("双幅：%1 屏配对成两张、%2 屏单独（由素材决定）").arg(pairs).arg(singles));
        check(dup == 0 && seen.size() == sum,
              QStringLiteral("双幅：**每张只出现在一屏**（并集 %1 = 各屏之和 %2）").arg(seen.size()).arg(sum));
    }

    // ④ （原"严格双幅按序列头对齐"的用例已删 ✗）
    //    双幅现在**只有一条规则**（从头锚定，由序列算），"每张只出现在一屏"已由 ③ 覆盖。

    // ⑤ 缺失 / 不可读：**整组缺失整组跳过**，上层永远拿到好的；坏 key 只解 1 次（不重试）
    {
        Cache c;
        Sequence::Item bad;
        bad.kind = Sequence::Kind::Image;
        bad.ord = 100000;                                    // ★ 缓存键 = ord（与既有条目不撞）
        bad.label = QStringLiteral("__不存在的文件__.png");
        bad.displayName = bad.label;
        bad.loc.diskPath = QDir(dir).filePath(bad.label);     // 真实解码必然失败
        bad.size = QSize(1200, 1800);
        QVector<Sequence::Item> v;
        for (int i = 0; i < seq.count(); ++i) v << seq.at(i);
        v.insert(1, bad);
        Sequence badSeq;
        badSeq.buildFromEntries(dir, v);
        auto st2 = std::make_shared<Stats>();
        Preloader pre(&c, [&st2](const Sequence::Item &it) {
            st2->calls.fetch_add(1);
            return Sequence::image(it);
        });
        Scheduler s(fullWindow(badSeq, Mode::Single), &c, &pre, 7, Mode::Single);
        const auto r = s.fetch(Scheduler::Op::Jump, 0, 1);   // ★ 直接落在那个坏条目上
        //   ★ T2d-1d-①：调度器**不再替上层跳过坏图** —— 如实报回（key + 状态 = 坏），
        //     由上层决定「显示 / 等待 / 跳过」（单幅跳过 / 双幅留空位）。
        check(r.keys.size() == 1 && r.states.size() == 1
                  && r.keys.first() == bad.cacheKey()
                  && r.states.first() == Cache::State::Bad,
              QStringLiteral("不可读：**如实报回**（key = 那一条 / 状态 = 坏）"));
        check(c.isBad(bad.cacheKey()) && c.badReason(bad.cacheKey()) == Cache::BadReason::Unreadable,
              QStringLiteral("不可读：缓存里写的是「坏·不可读」（打不开 / 解码失败）"));
        const int before = st2->calls.load();
        for (int i = 0; i < 4; ++i) s.fetch(Scheduler::Op::Jump, 0, 1);   // 反复要这一屏
        check(st2->calls.load() == before,
              QStringLiteral("不可读：**不重试**（后续 4 次要图新增 %1 次解码）")
                  .arg(st2->calls.load() - before));
    }

    // ⑥ 缓存键 = 图：同一张反复要 ⇒ 不重复解码
    {
        Cache c;
        auto st3 = std::make_shared<Stats>();
        Preloader pre(&c, [&st3](const Sequence::Item &it) {
            st3->calls.fetch_add(1);
            return Sequence::image(it);
        });
        Scheduler s(fullWindow(seq, Mode::Single), &c, &pre, 7, Mode::Single);
        s.fetch(Scheduler::Op::Jump, 0, 5);
        const int n1 = st3->calls.load();
        s.fetch(Scheduler::Op::Jump, 0, 5);
        s.fetch(Scheduler::Op::Jump, 0, 5);
        check(st3->calls.load() == n1,
              QStringLiteral("缓存键 = 图：同一张反复要 ⇒ **不重复解码**（%1 → %2）").arg(n1).arg(st3->calls.load()));
    }

    // ⑦ 淘汰 = 掉出窗口
    {
        Cache c;
        Preloader pre(&c, decode);
        Scheduler s(fullWindow(seq, Mode::Single), &c, &pre, 5, Mode::Single);
        auto r = s.fetch(Scheduler::Op::Jump, 0, 0);
        for (int i = 0; i < 12 && !r.keys.isEmpty(); ++i)
            r = s.fetch(Scheduler::Op::Next, r.cursor);
        check(c.count() <= 5, QStringLiteral("淘汰 = 掉出窗口：缓存只留窗口内的（%1 张）").arg(c.count()));
    }

    // ⑧ 批 4：真实文件也并行
    {
        Cache c;
        auto st4 = std::make_shared<Stats>();
        Preloader pre(&c, [&st4](const Sequence::Item &it) {
            st4->calls.fetch_add(1);
            const int cur = st4->inflight.fetch_add(1) + 1;
            for (int prev = st4->peak.load(); cur > prev && !st4->peak.compare_exchange_weak(prev, cur); )
                ;
            const QImage img = Sequence::image(it);
            st4->inflight.fetch_sub(1);
            return img;
        });
        pre.setBatch(4);
        Scheduler s(fullWindow(seq, Mode::Single), &c, &pre, 7, Mode::Single);
        s.fetch(Scheduler::Op::Jump, 0, 0);
        check(st4->peak.load() >= 2, QStringLiteral("批 = 4：**真实文件也并行**（峰值并发 %1）").arg(st4->peak.load()));
    }

    return check.finish();
}

int main(int argc, char *argv[])
{
    // `-d <目录>` 由 argv 透传（与旧 CLI 同一形状；设计 14 §11）
    const QString dir = testsupport::dirArg(argc, argv);
    return testsupport::suiteMain(argc, argv, [&] { return runFetchPipelineSeqSelfTest(dir); });
}
