#include "testsupport.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QIODevice>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVector>

#include <unistd.h>

#include "librarycore.h"
#include "librarystore.h"
#include "resetqueue.h"
#include "sequence.h"

// ══ 目录枚举规则自检（ib-test-scan）══════════════════════════════════════════
//   ★ T2c-2b-2：旧套件（88 项）建在 `ScanController` + 写入磁盘 `scan_state` + `action` 表 +
//     认领 / 代次 / 心跳 / 陈旧自愈之上 —— 那套机制**整体作废**（设计 14 §9.6）。
//     这里只留 **`Sequence` 的目录枚举规则**（与旧路径无关的那些），并改走新模型：
//     图集动作 = `LibraryCore::requestReset`（跑完 ⇒ 后台 `Store::replaceItems` 原子替换）。
//   覆盖：
//     ① 一次遍历顺序（深度优先 + 同级自然排序）、条目数、容器节点数、`note.txt` 不算条目；
//     ② `scanHidden=true`：隐藏目录 / 隐藏文件（`isHidden()` 判定）进来；
//     ③ `recurseDepth` 边界（含 `recurseDepth=1` 只扫根 + 一层）；
//     ④ 符号链接环：条目有界、扫描在合理时间内结束；
//     ⑤ 打不开的压缩包（假 zip）不产出条目；
//     ⑥ **重扫后的行**：上一轮见到、这一轮不在树里的条目**被删除**（重置 + `replaceItems`）；
//     ⑦ 根目录无图 ⇒ 照常结束 + 第一批从子目录拿到 + 最终条数正确；
//        根不可用 ⇒ 空跑完 `Scaned` + 0 条 + 原因只走提示 / 日志（**不写入磁盘**）。
//   规则：只碰临时库 / 临时目录（`PHOTOSET_LIBRARY_DB` / `PHOTOSET_SETTINGS_FILE`）。
static int runScanSelfTest()
{
    using testsupport::waitFor;
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        qWarning() << "[scanselftest] 无法建临时目录";
        return 1;
    }

    const QString dbPath = QStringLiteral("/tmp/photoset-scanselftest.db");
    for (const QString &suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")})
        QFile::remove(dbPath + suffix);
    qputenv("PHOTOSET_LIBRARY_DB", dbPath.toLocal8Bit());
    const QString settingsPath = QStringLiteral("/tmp/photoset-scanselftest-settings.json");
    QFile::remove(settingsPath);
    qputenv("PHOTOSET_SETTINGS_FILE", settingsPath.toLocal8Bit());

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

    library::Store store;
    QString err;
    check(store.open(&err), QStringLiteral("打开临时库：%1").arg(err.isEmpty() ? dbPath : err));
    check(fixtureOk, QStringLiteral("夹具：临时目录树（含符号链接环）写入成功"));

    LibraryCore core(&store);

    auto opts = [](int depth, bool hidden) {
        LibraryCore::ResetOptions o;
        o.maxDepth = depth;
        o.hidden   = hidden;
        return o;
    };
    // 重置到 Done（**跑完 = 后台原子替换完成**；失败 / 卡住的路径本套件单测）
    auto runReset = [&](qint64 cid, int depth, bool hidden, int timeoutMs = 60000) {
        QString e;
        if (!core.requestReset(cid, opts(depth, hidden), &e)) {
            qWarning().noquote() << QStringLiteral("[scanselftest] requestReset 被拒：%1").arg(e);
            return false;
        }
        return waitFor([&] {
            core.pump();
            return core.resetState(cid) == LibraryCore::ResetState::Done;
        }, timeoutMs);
    };

    // ★ v5：库行里**没有路径** ⇒ 断言用的"相对路径"由 `Sequence::locateRows` **派生**
    //   （盘上的行 = 相对图集根的路径；包内的行 = 最外层容器名!/成员链）
    auto seqRowsOf = [&store](qint64 cid) {
        QVector<Sequence::Row> rs;
        for (const library::Item &it : store.items(cid)) {
            Sequence::Row x;
            x.ord = it.ord; x.parentOrd = it.parentOrd; x.kind = it.kind; x.label = it.label;
            x.pageIndex = it.pageIndex; x.size = it.size; x.mtime = it.mtime;
            x.width = it.width; x.height = it.height;
            rs << x;
        }
        return rs;
    };
    auto pathsOf = [&store, &seqRowsOf](qint64 cid) {
        const library::Collection c = store.collection(cid);
        const QVector<library::Item> rows = store.items(cid);
        const QVector<Sequence::Location> locs =
            Sequence::locateRows(c.rootPath, c.kind == Sequence::Kind::Archive, seqRowsOf(cid));
        QStringList out;
        for (int i = 0; i < rows.size() && i < locs.size(); ++i) {
            if (rows.at(i).kind != Sequence::Kind::Image) continue;
            const Sequence::Location &l = locs.at(i);
            if (l.members.isEmpty()) out << QDir(c.rootPath).relativeFilePath(l.diskPath);
            else out << (QFileInfo(l.diskPath).fileName() + QStringLiteral("!/")
                         + l.members.join(QStringLiteral("!/")));
        }
        return out;
    };
    auto nodeCountOf = [&store](qint64 cid) {
        int n = 0;
        for (const library::Item &it : store.items(cid))
            if (it.kind != Sequence::Kind::Image) ++n;
        return n;
    };
    // 按**派生的相对路径**取那个 image 行（找不到 ⇒ `id < 0` 的哨兵；枚举没有"空值"）
    auto rowOf = [&store, &pathsOf](qint64 cid, const QString &rel) {
        const QStringList paths = pathsOf(cid);
        const QVector<library::Item> rows = store.items(cid);
        int k = 0;
        for (const library::Item &it : rows) {
            if (it.kind != Sequence::Kind::Image) continue;
            if (k < paths.size() && paths.at(k) == rel) return it;
            ++k;
        }
        return library::Item{};
    };

    // ══ ① 小树 S：条目数 / 一次遍历顺序 / 容器节点 / 符号链接环 / 隐藏 / 深度 / 假 zip ══
    const qint64 cidS = store.addCollection(rootS, Sequence::Kind::Folder);
    check(cidS > 0, QStringLiteral("加入集合（小树 S）"));
    QElapsedTimer clock;
    clock.start();
    check(runReset(cidS, 5, false), QStringLiteral("① 重置跑完（符号链接环没挂死：等待有超时）"));
    const qint64 scanMs = clock.elapsed();
    check(scanMs < 5000, QStringLiteral("① 符号链接环：整体扫描在合理时间内结束（%1 ms）").arg(scanMs));

    QStringList ks = pathsOf(cidS);
    //   ★ 2026-10-19 用户定案：遍历 = **名称序先根遍历**（preorder DFS）—— 同层按名称排、
    //     **图片与子目录同序**，遇到子目录**当场下钻**（不是"先出完这一层的图再看下一层"）。
    const QStringList expectedS = {
        QStringLiteral("deep/l1/l2/l3/deep.png"), QStringLiteral("loop/li.png"),
        QStringLiteral("p1.png"), QStringLiteral("p2.png"), QStringLiteral("p10.png"),
        QStringLiteral("sub/q1.png"), QStringLiteral("sub/q2.png"),
    };
    check(ks.size() == 7 && nodeCountOf(cidS) == 8,
          QStringLiteral("① 条目数 = 7、容器节点 = 8（实际 %1 / %2）")
              .arg(ks.size()).arg(nodeCountOf(cidS)));
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

    // ══ ② scanHidden=true：隐藏项被扫进来 ══════════════════════
    const bool hiddenDone = runReset(cidS, 5, true);
    ks = pathsOf(cidS);
    check(hiddenDone && ks.size() == 9 && ks.contains(QStringLiteral(".hidden/h.png"))
              && ks.contains(QStringLiteral(".hiddenfile.png")),
          QStringLiteral("② scanHidden=true：隐藏目录与隐藏文件都按 isHidden() 判定进来"
                         "（条目 %1）").arg(ks.size()));

    // ══ ③ recurseDepth=1：只扫根 + 一层；上一轮的深层行被**替换掉** ═════════════
    const bool depth1Done = runReset(cidS, 1, false);
    ks = pathsOf(cidS);
    check(depth1Done && ks.size() == 6
              && !ks.contains(QStringLiteral("deep/l1/l2/l3/deep.png")),
          QStringLiteral("③ recurseDepth=1：本轮只扫根 + 一层（%1 条，一层以外不进去）")
              .arg(ks.size()));
    // ★ v8：重置 = **原子替换**（`replaceItems`）⇒ 没见到的旧行被删掉（库里没有 missing 概念）
    check(rowOf(cidS, QStringLiteral("deep/l1/l2/l3/deep.png")).id < 0,
          QStringLiteral("③ 上一轮见到的深层条目（不在本棵树里）**被删除**（replaceItems）"));
    check(store.items(cidS).size() == 10 && nodeCountOf(cidS) == 4,
          QStringLiteral("③ 本轮 6 条图 + 4 个目录节点 = 10 行（实际 %1 / %2）")
              .arg(store.items(cidS).size()).arg(nodeCountOf(cidS)));

    // ══ ④ 树 M：note.txt 不算条目；删文件重扫 ⇒ 消失的行被删，其余正常 ═══════════
    const qint64 cidM = store.addCollection(rootM, Sequence::Kind::Folder);
    const bool mFirst = (cidM > 0) && runReset(cidM, 5, false);
    const int  mCount1 = pathsOf(cidM).size();
    check(mFirst && mCount1 == 3,
          QStringLiteral("④ 扫描 M 跑完：条目数 = 3（note.txt 不算条目，实际 %1）").arg(mCount1));
    const bool removed = QFile::remove(rootM + QStringLiteral("/sub/m3.png"));
    const bool mAgain = removed && runReset(cidM, 5, false);
    const int  mCount2 = pathsOf(cidM).size();
    check(mAgain && mCount2 == 2
              && rowOf(cidM, QStringLiteral("sub/m3.png")).id < 0
              && rowOf(cidM, QStringLiteral("m1.png")).id >= 0,
          QStringLiteral("④ 删掉 sub/m3.png 后重扫：消失的条目被**删除**（replaceItems 原子替换），"
                         "其余仍正常（%1 条）").arg(mCount2));

    // ══ ⑤ 根目录无图：照常结束 + **第一批从子目录拿到**（扫完前）+ 最终条数正确 ══
    const qint64 cidN = store.addCollection(rootN, Sequence::Kind::Folder);
    int  firstProgress = -1;
    bool firstBeforeFinish = false;
    {
        const QMetaObject::Connection c5 = QObject::connect(
            &core, &LibraryCore::resetProgress, [&](qint64 id, int n) {
                if (id != cidN || firstProgress > 0) return;
                if (n > 0) {
                    firstProgress = n;
                    Sequence *rs = core.resetSequence(cidN);
                    firstBeforeFinish = (rs != nullptr && !rs->scanFinished());
                }
            });
        check(cidN > 0 && runReset(cidN, 5, false),
              QStringLiteral("⑤ 加入集合（根目录无图，子目录 %1 张）且扫描照常结束").arg(innerN));
        QObject::disconnect(c5);
    }
    check(firstBeforeFinish && firstProgress > 0 && firstProgress < innerN,
          QStringLiteral("⑤ **第一批从子目录拿到**：扫完前已发现 %1 张（< %2）")
              .arg(firstProgress).arg(innerN));
    check(pathsOf(cidN).size() == innerN && nodeCountOf(cidN) == 1,
          QStringLiteral("⑤ 最终 %1 条图 + 1 个目录节点（note.txt 不算，实际 %2 / %3）")
              .arg(innerN).arg(pathsOf(cidN).size()).arg(nodeCountOf(cidN)));

    // ══ ⑥ 根不可用：空跑完 Scaned + 0 条 + 原因只走提示 / 日志（**不写入磁盘**）═══════
    const QString rootGone = tmp.filePath(QStringLiteral("scanGone"));   // 刻意不创建
    const qint64 cidGone = store.addCollection(rootGone, Sequence::Kind::Folder);
    {
        QString e;
        check(cidGone > 0 && core.requestReset(cidGone, opts(5, false), &e),
              QStringLiteral("⑥ 根目录不存在也**不再被拒**：照常入队（err='%1'）").arg(e));
        const bool failed = waitFor([&] {
            core.pump();
            return core.resetState(cidGone) == LibraryCore::ResetState::Failed;
        }, 30000);
        check(failed, QStringLiteral("⑥ 空跑完 ⇒ Failed（状态 %1）")
                          .arg(resetq::resetStateName(core.resetState(cidGone))));
        bool sawNotice = false;
        for (QString n = core.takeNotice(); !n.isEmpty(); n = core.takeNotice())
            if (n.contains(QStringLiteral("未落库"))) sawNotice = true;
        check(core.resetReason(cidGone).contains(rootGone) && sawNotice,
              QStringLiteral("⑥ 原因只走**提示 / 日志**（不写入磁盘）：%1").arg(core.resetReason(cidGone)));
        check(store.items(cidGone).isEmpty(),
              QStringLiteral("⑥ **不写入磁盘**：0 条（根不可用不算一个「干净的 0 条」）"));
    }

    return check.finish();
}

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, &runScanSelfTest);
}
