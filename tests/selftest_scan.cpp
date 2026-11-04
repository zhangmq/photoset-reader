#include "testsupport.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>

//   `symlink()` 只有 POSIX 有（见下面的 `Q_OS_UNIX` 分支），头文件同理
#ifdef Q_OS_UNIX
#  include <unistd.h>
#endif

#include "sequence.h"

// ══ 目录枚举规则自检（ib-test-scan）══════════════════════════════════════════
//   ★ 2026-10-31：随「浏览 / 图集库」删除，这套件改成**只驱动 `Sequence`**
//     （以前要经 `LibraryCore::requestReset` 落库、再从库里读回行；库没了）。
//     覆盖的那些规则没有变，仍是「扫描该怎么走」这一层：
//     ① 一次遍历顺序（先根 DFS + 同级自然排序）、条目数、容器数、`note.txt` 不算条目；
//     ② `scanHidden=true`：隐藏目录 / 隐藏文件（`isHidden()` 判定）进来；
//     ③ `recurseDepth` 边界（`recurseDepth=1` 只扫根 + 一层）；
//     ④ 符号链接环：条目有界、扫描在合理时间内结束；
//     ⑤ 打不开的压缩包（假 zip）不产出条目；
//     ⑥ 根目录无图 ⇒ 照常结束 + 第一批从子目录拿到 + 最终条数正确；
//     ⑦ 根不可用 ⇒ 空跑完 `Scaned` + 0 条 + 原因只走 `unsupportedReason()`。
//   规则：只碰临时目录，**不写任何库 / 设置**。
static int runScanSelfTest()
{
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        qWarning() << "[scanselftest] 无法建临时目录";
        return 1;
    }

    testsupport::Check check(QStringLiteral("scanselftest"));

    // ── 夹具工具（内容无所谓：扫描**不解码**，只按扩展名列条目）──
    auto mkFile = [](const QString &path) -> bool {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) return false;
        f.write("x");
        return true;
    };
    auto mkLink = [](const QString &target, const QString &linkPath) -> bool {
#ifdef Q_OS_UNIX
        QDir().mkpath(QFileInfo(linkPath).absolutePath());
        return ::symlink(target.toLocal8Bit().constData(),
                         linkPath.toLocal8Bit().constData()) == 0;
#else
        Q_UNUSED(target); Q_UNUSED(linkPath);
        return false;
#endif
    };

    // ── 小树 S：根 / 子目录 / 隐藏 / 深层 / 符号链接环 / 假压缩包 ──
    const QString rootS = tmp.filePath(QStringLiteral("scanS"));
    bool fixtureOk = QDir().mkpath(rootS);
    for (const QString &n : {QStringLiteral("p2.png"), QStringLiteral("p10.png"),
                             QStringLiteral("p1.png"), QStringLiteral(".hiddenfile.png")})
        fixtureOk = mkFile(rootS + QLatin1Char('/') + n) && fixtureOk;
    fixtureOk = mkFile(rootS + QStringLiteral("/sub/q1.png")) && fixtureOk;
    fixtureOk = mkFile(rootS + QStringLiteral("/sub/q2.png")) && fixtureOk;
    fixtureOk = mkFile(rootS + QStringLiteral("/.hidden/h.png")) && fixtureOk;
    fixtureOk = mkFile(rootS + QStringLiteral("/deep/l1/l2/l3/deep.png")) && fixtureOk;   // 层 4
    fixtureOk = mkFile(rootS + QStringLiteral("/deep/l1/l2/l3/l4/l5/tooDeep.png")) && fixtureOk; // 层 6
    fixtureOk = mkFile(rootS + QStringLiteral("/loop/li.png")) && fixtureOk;
    fixtureOk = mkFile(rootS + QStringLiteral("/zip/vol01.cbz")) && fixtureOk;             // 假压缩包
    // ★ 符号链接环：`back` 指回祖先（`ln -s ../../a` 的形态）、`self` 自指
    fixtureOk = mkLink(QStringLiteral(".."), rootS + QStringLiteral("/loop/back")) && fixtureOk;
    fixtureOk = mkLink(QStringLiteral("."), rootS + QStringLiteral("/loop/self")) && fixtureOk;

    // ── 小树 M：m1 / m2 / sub/m3 + 一个非图片文件 ──
    const QString rootM = tmp.filePath(QStringLiteral("scanM"));
    fixtureOk = QDir().mkpath(rootM) && fixtureOk;
    fixtureOk = mkFile(rootM + QStringLiteral("/m1.png")) && fixtureOk;
    fixtureOk = mkFile(rootM + QStringLiteral("/m2.png")) && fixtureOk;
    fixtureOk = mkFile(rootM + QStringLiteral("/sub/m3.png")) && fixtureOk;
    fixtureOk = mkFile(rootM + QStringLiteral("/note.txt")) && fixtureOk;

    // ── 小树 N：**根目录一张图都没有**，图都在子目录（40 张 ⇒ 第一批在扫完前到达）──
    const QString rootN = tmp.filePath(QStringLiteral("scanN"));
    fixtureOk = QDir().mkpath(rootN + QStringLiteral("/inner")) && fixtureOk;
    fixtureOk = mkFile(rootN + QStringLiteral("/note.txt")) && fixtureOk;
    const int innerN = 40;
    for (int i = 0; i < innerN; ++i)
        fixtureOk = mkFile(rootN + QStringLiteral("/inner/in%1.png").arg(i, 3, 10, QLatin1Char('0')))
                    && fixtureOk;

    check(fixtureOk, QStringLiteral("夹具：临时目录树（含符号链接环）写入成功"));

    // ── 扫一遍（**唯一入口 = `Sequence::startScan`**；本套件不碰库）──
    auto runScan = [](Sequence &sq, const QString &path, int depth, bool hidden,
                      int timeoutMs = 60000) -> bool {
        sq.startScan(path, depth, hidden);
        const bool done = testsupport::waitFor([&] {
            sq.drain();
            return sq.scanFinished();
        }, timeoutMs);
        while (sq.drain()) { }         // 收尾那一批（放批与写终态可能挨着）
        return done;
    };
    // 条目表里的**图的相对路径**（盘上 = 相对根；包内 = 最外层容器!/成员链）
    auto pathsOf = [](const Sequence &sq) {
        QStringList out;
        const QVector<Sequence::Item> &items = sq.items();
        for (const Sequence::Item &it : items) {
            if (it.kind != Sequence::Kind::Image) continue;
            if (it.loc.members.isEmpty())
                out << QDir(sq.rootPath()).relativeFilePath(it.loc.diskPath);
            else
                out << (QFileInfo(it.loc.diskPath).fileName() + QStringLiteral("!/")
                        + it.loc.members.join(QStringLiteral("!/")));
        }
        return out;
    };

    // ══ ① 小树 S：条目数 / 一次遍历顺序 / 容器 / 符号链接环 / 隐藏 / 深度 / 假 zip ══
    {
        Sequence sq;
        QElapsedTimer clock;
        clock.start();
        check(runScan(sq, rootS, 5, false),
              QStringLiteral("① 扫描跑完（符号链接环没挂死：等待有超时）"));
        check(clock.elapsed() < 5000,
              QStringLiteral("① 符号链接环：整体扫描在合理时间内结束（%1 ms）").arg(clock.elapsed()));

        const QStringList ks = pathsOf(sq);
        //   ★ 2026-10-19 用户定案：遍历 = **名称序先根遍历**（preorder DFS）—— 同层按名称排、
        //     **图片与子目录同序**，遇到子目录**当场下钻**（不是"先出完这一层的图再看下一层"）。
        const QStringList expectedS = {
            QStringLiteral("deep/l1/l2/l3/deep.png"), QStringLiteral("loop/li.png"),
            QStringLiteral("p1.png"), QStringLiteral("p2.png"), QStringLiteral("p10.png"),
            QStringLiteral("sub/q1.png"), QStringLiteral("sub/q2.png"),
        };
        check(ks.size() == 7 && sq.containers().size() == 6,
              QStringLiteral("① 条目数 = 7、容器（不含根 / 空容器）= 6（实际 %1 / %2）")
                  .arg(ks.size()).arg(sq.containers().size()));
        check(ks == expectedS,
              QStringLiteral("① 顺序 = **名称序先根遍历**（同层同序 + 遇到目录当场下钻）：%1")
                  .arg(ks.join(QLatin1Char(','))));
        check(!ks.contains(QStringLiteral(".hidden/h.png"))
                  && !ks.contains(QStringLiteral(".hiddenfile.png")),
              QStringLiteral("① 隐藏目录 / 隐藏文件默认不扫（scanHidden=false）"));
        check(ks.contains(QStringLiteral("deep/l1/l2/l3/deep.png")),
              QStringLiteral("① recurseDepth=5 内的目录照常进去"));
        check(!ks.contains(QStringLiteral("deep/l1/l2/l3/l4/l5/tooDeep.png")),
              QStringLiteral("① 超过 recurseDepth 的目录不进去（层 6 不在结果里）"));
        check(ks.count(QStringLiteral("loop/li.png")) == 1,
              QStringLiteral("① 符号链接环：条目有界（loop/li.png 只算一次，back/self 没展开）"));
        check(!ks.contains(QStringLiteral("zip/vol01.cbz")),
              QStringLiteral("① **打不开**的压缩包（夹具是假 zip）不产出条目；目录节点照旧"));
    }

    // ══ ② scanHidden=true：隐藏项被扫进来 ══════════════════════
    {
        Sequence sq;
        const bool done = runScan(sq, rootS, 5, true);
        const QStringList ks = pathsOf(sq);
        check(done && ks.size() == 9 && sq.containers().size() == 7
                  && ks.contains(QStringLiteral(".hidden/h.png"))
                  && ks.contains(QStringLiteral(".hiddenfile.png")),
              QStringLiteral("② scanHidden=true：隐藏目录与隐藏文件都按 isHidden() 判定进来"
                             "（条目 %1 / 容器 %2）").arg(ks.size()).arg(sq.containers().size()));
    }

    // ══ ③ recurseDepth=1：只扫根 + 一层 ═══════════════════════════════
    {
        Sequence sq;
        const bool done = runScan(sq, rootS, 1, false);
        const QStringList ks = pathsOf(sq);
        check(done && ks.size() == 6
                  && !ks.contains(QStringLiteral("deep/l1/l2/l3/deep.png")),
              QStringLiteral("③ recurseDepth=1：只扫根 + 一层（%1 条，一层以外不进去）")
                  .arg(ks.size()));
        check(sq.containers().size() == 2,
              QStringLiteral("③ 容器只剩 loop / sub 两个（实际 %1）").arg(sq.containers().size()));
    }

    // ══ ④ 树 M：note.txt 不算条目；删文件重扫 ⇒ 条目少一条 ═══════════════
    {
        Sequence sq;
        const bool first = runScan(sq, rootM, 5, false);
        const int count1 = pathsOf(sq).size();
        check(first && count1 == 3,
              QStringLiteral("④ 扫描 M 跑完：条目数 = 3（note.txt 不算条目，实际 %1）").arg(count1));

        const bool removed = QFile::remove(rootM + QStringLiteral("/sub/m3.png"));
        Sequence again;
        const bool second = removed && runScan(again, rootM, 5, false);
        const QStringList ks = pathsOf(again);
        //   ★ `sub` 变空 ⇒ 它**不再是元素**（空容器不算）⇒ 容器数从 1 变成 0
        check(second && ks.size() == 2 && again.containers().size() == 0,
              QStringLiteral("④ 删掉 sub/m3.png 后重扫：条目 = 2、容器 = 0（空容器不算；实际 %1 / %2）")
                  .arg(ks.size()).arg(again.containers().size()));
        check(ks == QStringList({QStringLiteral("m1.png"), QStringLiteral("m2.png")}),
              QStringLiteral("④ 剩下的条目仍是 m1 / m2：%1").arg(ks.join(QLatin1Char(','))));
    }

    // ══ ⑤ 根目录无图：照常结束 + **第一批从子目录拿到**（扫完前）+ 最终条数正确 ══
    {
        Sequence sq;
        int  firstProgress = -1;
        bool firstBeforeFinish = false;
        sq.startScan(rootN, 5, false);
        QElapsedTimer clock;
        clock.start();
        while (!sq.scanFinished() && clock.elapsed() < 60000) {
            sq.drain();
            if (firstProgress < 0 && sq.discoveredCount() > 0 && !sq.scanFinished()) {
                firstProgress = sq.discoveredCount();
                firstBeforeFinish = true;
            }
            QThread::msleep(2);
        }
        while (sq.drain()) { }
        check(sq.scanFinished() && sq.state() == Sequence::State::Scaned,
              QStringLiteral("⑤ 加入来源（根目录无图，子目录 %1 张）且扫描照常结束").arg(innerN));
        //   ★ 这条是**时序**断言：只有在"扫描还没结束"时观察到部分发现才算验证过。
        //     机器够快（Windows runner 只建 40 个小文件）时，第一轮 drain 之前扫描就已经
        //     结束 ⇒ 这个性质**观测不到**，此时报"跳过"而不是失败（2026-11-05，Windows CI 实测）。
        if (!firstBeforeFinish) {
            qInfo().noquote()
                << QStringLiteral("[scanselftest] 跳过 ⑤ 增量发现：扫描在第一次 drain 前就结束，无法观测");
        } else {
            check(firstProgress > 0 && firstProgress < innerN,
                  QStringLiteral("⑤ **第一批从子目录拿到**：扫完前已发现 %1 张（< %2）")
                      .arg(firstProgress).arg(innerN));
        }
        const QStringList ks = pathsOf(sq);
        check(ks.size() == innerN && sq.containers().size() == 1,
              QStringLiteral("⑤ 最终 %1 条图 + 1 个容器（note.txt 不算，实际 %2 / %3）")
                  .arg(innerN).arg(ks.size()).arg(sq.containers().size()));
    }

    // ══ ⑥ 根不可用：空跑完 Scaned + 0 条 + 原因只走 `unsupportedReason()` ═══════
    {
        const QString rootGone = tmp.filePath(QStringLiteral("scanGone"));   // 刻意不创建
        Sequence sq;
        const bool done = runScan(sq, rootGone, 5, false, 30000);
        check(done && sq.state() == Sequence::State::Scaned && sq.items().isEmpty(),
              QStringLiteral("⑥ 根不存在 ⇒ 空跑完 Scaned + 0 条（状态 %1 / 条目 %2）")
                  .arg(int(sq.state())).arg(sq.items().size()));
        check(sq.unsupportedReason().contains(rootGone),
              QStringLiteral("⑥ 原因只走瞬时通道（不写盘）：%1").arg(sq.unsupportedReason()));
    }

    return check.finish();
}

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, &runScanSelfTest);
}
