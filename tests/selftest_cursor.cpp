#include "testsupport.h"

#include <QSet>
#include <QString>
#include <QVector>

#include "fetchpipeline.h"
#include "sequence.h"

// ══ 游标模型自检（ib-test-cursor；原先由 `image-browser --selftest-cursor` 跑）═══════
//   ★ 2026-10-12 改造：**分组归序列**（**从头锚定**），调度器只"向序列要元素"。
//     旧"简单双幅（以下一屏起点为基准动态建组）"的用例**已作废并删除** ——
//     语义没了，留着就是假通过。

static int runCursorSelfTest()
{
    using namespace fetch;
    using namespace pipetest;
    testsupport::Check check(QStringLiteral("cursor"));

    // ① 单幅：每组 1 张；前进 +1；prev 对称
    {
        Sequence seq;
        fill(seq, mk(6));
        check(seq.groupAt(0, Mode::Single).members.size() == 1
                  && seq.groupAt(0, Mode::Single).members.first() == 0,
              QStringLiteral("单幅：组 = 1 张（§12.2）"));
        check(seq.advance(0, Mode::Single, Sequence::Dir::Next) == 1,
              QStringLiteral("单幅：前进 +1"));
        check(seq.advance(3, Mode::Single, Sequence::Dir::Prev) == 2,
              QStringLiteral("单幅：prev 对称 −1（不需要额外状态）"));
    }

    // ② 双幅（**从头锚定**）：组 = 2 张；前进 = 本组大小；**每张只出现在一屏**
    {
        Sequence seq;
        fill(seq, mk(6));
        check(seq.groupAt(0, Mode::Double).members.size() == 2,
              QStringLiteral("双幅：组 = (0,1) 两张"));
        check(seq.advance(0, Mode::Double, Sequence::Dir::Next) == 2,
              QStringLiteral("双幅：前进 = **本组大小**（+2）"));
        check(seq.advance(2, Mode::Double, Sequence::Dir::Next) == 4,
              QStringLiteral("双幅：继续 +2（每屏两张）"));
        check(seq.advance(2, Mode::Double, Sequence::Dir::Prev) == 0,
              QStringLiteral("双幅：prev = **上一组的起点**（2 → 0，对称）"));

        QSet<int> seen;
        int sum = 0, dup = 0, cur = 0, screens = 0;
        while (screens++ < 20) {
            const Sequence::Group g = seq.groupAt(cur, Mode::Double);
            if (g.members.isEmpty()) break;
            for (int i : g.members) { if (seen.contains(i)) ++dup; seen.insert(i); }
            sum += g.members.size();
            const int nxt = seq.advance(cur, Mode::Double, Sequence::Dir::Next);
            if (nxt < 0 || nxt == cur) break;
            cur = nxt;
        }
        check(dup == 0 && seen.size() == sum,
              QStringLiteral("双幅：**每张只出现在一屏**（并集 %1 = 各屏之和 %2，重复 %3）")
                  .arg(seen.size()).arg(sum).arg(dup));
        check(seen.size() == 6,
              QStringLiteral("双幅：每张都被排到、一张不丢（%1/6）").arg(seen.size()));
    }

    // ③ 双幅：宽图单独成屏（第一张宽 / 第二张宽 / 末尾只剩一张）
    {
        Sequence s1;
        fill(s1, mk(6, {}, {0}));
        check(s1.groupAt(0, Mode::Double).members.size() == 1,
              QStringLiteral("双幅：**第一张宽** ⇒ 单独成屏（1 张）"));

        Sequence s2;
        fill(s2, mk(6, {}, {1}));
        check(s2.groupAt(0, Mode::Double).members.size() == 1,
              QStringLiteral("双幅：**第一张窄 + 第二张宽 ⇒ 也不配**"));

        Sequence s3;
        fill(s3, mk(5));
        check(s3.groupAt(4, Mode::Double).members.size() == 1,
              QStringLiteral("双幅：**末尾只剩一张 ⇒ 单独 1 张**"));
    }

    // ④ **从头锚定**：落到配对第二张 ⇒ 回到它所属的那一组
    //   （取代旧"从下一屏起点重建组" —— 那条语义已作废）
    {
        Sequence seq;
        fill(seq, mk(8));
        check(seq.anchorOf(1, Mode::Double) == 0,
              QStringLiteral("从头锚定：落到配对第二张 ⇒ **回到那一组**（1 → 0）"));
        check(seq.groupAt(1, Mode::Double).cursor == 0,
              QStringLiteral("从头锚定：从组内第二张要图 ⇒ 给的是同一组（游标 0）"));
        check(seq.advance(0, Mode::Double, Sequence::Dir::Next) == 2
                  && seq.advance(1, Mode::Double, Sequence::Dir::Next) == 2,
              QStringLiteral("从头锚定：**分组与下一屏起点无关**（游标 0 与 1 的下一组都是 2）"));
    }

    // ⑤ 缺失追加进前进量（单幅：跳过 n 个缺失 ⇒ 前进 n+1）
    {
        Sequence seq;
        fill(seq, mk(6, {1, 2, 3}));
        // 跳过发生在 `groupAt`（缺失占位、**按行进方向**跳过）⇒ 断言"最终显示的那一组落在哪"
        check(seq.groupAt(seq.advance(0, Mode::Single, Sequence::Dir::Next),
                          Mode::Single).cursor == 4,
              QStringLiteral("缺失**追加进前进量**：跳过 3 个缺失 ⇒ 显示的那一组落在 4（1+3）"));
        check(seq.groupAt(seq.advance(4, Mode::Single, Sequence::Dir::Prev),
                          Mode::Single, Sequence::Dir::Prev).cursor == 0,
              QStringLiteral("缺失对称：往回同样跳过（4 → 0）"));
    }

    // ⑥ 双幅：缺失**占位**（游标照常按组大小算）；整组缺失 ⇒ 整组跳过
    {
        Sequence seq;
        fill(seq, mk(10, {2, 3}));                // 2、3 缺失 ⇒ 组 (2,3) 整组没了
        const Sequence::Group g = seq.groupAt(2, Mode::Double);
        check(g.cursor == 4 && g.members == QVector<int>({4, 5}),
              QStringLiteral("双幅：整组缺失（2、3）⇒ **整组跳过**，落到下一组（游标 4）"));
        check(seq.advance(0, Mode::Double, Sequence::Dir::Next) == 2,
              QStringLiteral("双幅：缺失**不改变**前进量（仍是本组大小）"));
    }

    // ⑦ 到头 = −1；prev 到底 = −1
    {
        Sequence seq;
        fill(seq, mk(3));
        check(seq.advance(2, Mode::Single, Sequence::Dir::Next) == -1,
              QStringLiteral("到头 ⇒ advance 返回 −1"));
        check(seq.advance(0, Mode::Single, Sequence::Dir::Prev) == -1,
              QStringLiteral("到头（往回）⇒ 返回 −1"));
    }

    // ⑧ **prev 不需要额外状态**（纯函数、幂等）
    {
        Sequence seq;
        fill(seq, mk(8));
        check(seq.advance(4, Mode::Double, Sequence::Dir::Prev)
                  == seq.advance(4, Mode::Double, Sequence::Dir::Prev),
              QStringLiteral("prev 是纯函数（同一游标两次同样结果）⇒ **不需要额外状态**"));
        check(seq.advance(seq.advance(4, Mode::Double, Sequence::Dir::Prev),
                          Mode::Double, Sequence::Dir::Next) == 4,
              QStringLiteral("prev 之后再 next 回到原游标（双幅：上一组起点 → 再 +本组大小）"));
    }

    // ⑨ 身份是 **ord**（不是路径语义）：容器里的名字含特殊字符也能走完全流程
    {
        QVector<Sequence::Item> v;
        for (int i = 0; i < 4; ++i) {
            Sequence::Item it;
            it.kind = Sequence::Kind::Image;
            it.ord = i;
            it.label = QStringLiteral("容器的条目#%1?s=3").arg(i);   // 不像路径、也不是路径
            it.displayName = it.label;
            it.size = QSize(1200, 1800);
            v << it;
        }
        Cache c2;
        Preloader pre2(&c2, decoder({}));
        Sequence seq;
        fill(seq, v);
        Scheduler s(fullWindow(seq, Mode::Single), &c2, &pre2, 7, Mode::Single);
        const auto r = s.fetch(Scheduler::Op::Jump, 0, 1);
        check(!r.keys.isEmpty() && c2.hasImage(r.keys.first()),
              QStringLiteral("key 是**不透明标识**：不像路径的 key 也能走完全流程（不许当路径用）"));
    }

    return check.finish();
}

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, &runCursorSelfTest);
}
