#include "testsupport.h"

#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVector>

#include "progressfile.h"
#include "sequence.h"

// ══ 进度文件（`.prsave`）读写自检（ib-test-progress）════════════════════════════
//   设计 20。覆盖：
//     ① 命名：同一个源 → 同一个文件名（覆盖而不是越存越多）；同名不同目录 → 不同 hash；
//     ② 写 → 读：字段一个不差（title / source / layout / position / progress / rows）；
//     ③ 读回来的行能 **restore 成序列**，且 `indexOfChain` 落回原来的下标；
//     ④ 坏文件 / 不是进度文件 / 版本不认识 ⇒ 拒绝并给一句话原因（不崩）。
//   不碰用户的真收藏夹：全部在临时目录里。
static int runProgressSelfTest()
{
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        qWarning() << "[progressselftest] 无法建临时目录";
        return 1;
    }
    testsupport::Check check(QStringLiteral("progressselftest"));

    // ── 夹具：4 张图 + 一个子目录（顺序稳定：名称序先根遍历）──
    auto mkFile = [](const QString &path) -> bool {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) return false;
        f.write("x");
        return true;
    };
    const QString srcDir = tmp.filePath(QStringLiteral("src"));
    bool fixtureOk = QDir().mkpath(srcDir);
    for (const QString &n : {QStringLiteral("p1.png"), QStringLiteral("p2.png")})
        fixtureOk = mkFile(srcDir + QLatin1Char('/') + n) && fixtureOk;
    fixtureOk = mkFile(srcDir + QStringLiteral("/sub/q1.png")) && fixtureOk;
    fixtureOk = mkFile(srcDir + QStringLiteral("/sub/q2.png")) && fixtureOk;
    check(fixtureOk, QStringLiteral("夹具：源目录写入成功"));

    // ── ① 命名 ──
    const QString title = progressfile::titleOf(srcDir);
    const QString name  = progressfile::fileNameFor(title, srcDir);
    const QString otherDir = tmp.filePath(QStringLiteral("src2"));
    QDir().mkpath(otherDir);
    const QString otherName = progressfile::fileNameFor(progressfile::titleOf(otherDir), otherDir);
    check(name == progressfile::fileNameFor(title, srcDir),
          QStringLiteral("① 同一个源 → 同一个文件名（%1）").arg(name));
    check(name.endsWith(QStringLiteral(".prsave")),
          QStringLiteral("① 扩展名 = .prsave"));
    check(name != otherName,
          QStringLiteral("① 同名不同目录 → 不同 hash（%1 / %2）").arg(name, otherName));

    // ── 扫一遍源，拿行 + 位置 ──
    Sequence seq;
    seq.startScan(srcDir, 5, false);
    const bool scanned = testsupport::waitFor([&] {
        seq.drain();
        return seq.scanFinished();
    }, 60000);
    while (seq.drain()) { }
    check(scanned && seq.count() == 4,
          QStringLiteral("扫描跑完：%1 条（实际 %2）").arg(4).arg(seq.count()));

    const int cursor = 2;                              // 第 3 条（sub/q1.png）
    progressfile::Data d;
    d.meta.savedAt = 111;
    d.meta.title = title;
    d.meta.sourcePath = QFileInfo(srcDir).absoluteFilePath();
    d.meta.sourceKind = Sequence::Kind::Folder;
    d.meta.sourceMtime = QFileInfo(srcDir).lastModified().toSecsSinceEpoch();
    d.meta.hidden = false;
    d.meta.depth = 5;
    d.meta.innerArchiveMaxMb = 512;
    d.meta.layout = 1;                                  // 双幅
    d.meta.chain = seq.chainAt(cursor);
    d.meta.page = seq.pageAt(cursor);
    d.meta.done = cursor + 1;
    d.meta.total = seq.count();
    d.rows = seq.rows();

    const QString file = tmp.filePath(name);
    QString err;
    check(progressfile::write(file, d, &err), QStringLiteral("② 写文件：%1").arg(err));

    // ── ② 读回来，字段逐项比对 ──
    progressfile::Data back;
    check(progressfile::read(file, &back, &err), QStringLiteral("② 读文件：%1").arg(err));
    check(back.meta.version == progressfile::kVersion && back.meta.title == d.meta.title && back.meta.sourcePath == d.meta.sourcePath,
          QStringLiteral("② 头字段一致（title=%1）").arg(back.meta.title));
    check(back.meta.sourceKind == Sequence::Kind::Folder && back.meta.layout == 1,
          QStringLiteral("② 来源种类 / 版面一致（layout=%1）").arg(back.meta.layout));
    check(back.meta.chain == d.meta.chain && back.meta.page == d.meta.page,
          QStringLiteral("② 位置一致（链 %1 页 %2）")
              .arg(back.meta.chain.join(QLatin1Char('/'))).arg(back.meta.page));
    check(back.meta.done == d.meta.done && back.meta.total == d.meta.total,
          QStringLiteral("② 进度快照一致（%1 / %2）").arg(back.meta.done).arg(back.meta.total));
    check(back.rows.size() == d.rows.size(),
          QStringLiteral("② 行数一致（%1 / %2）").arg(back.rows.size()).arg(d.rows.size()));

    bool rowsSame = back.rows.size() == d.rows.size();
    for (int i = 0; rowsSame && i < d.rows.size(); ++i) {
        const Sequence::Row &a = d.rows.at(i);
        const Sequence::Row &b = back.rows.at(i);
        rowsSame = a.ord == b.ord && a.parentOrd == b.parentOrd && a.kind == b.kind
                   && a.label == b.label && a.pageIndex == b.pageIndex
                   && a.firstIndex == b.firstIndex && a.lastIndex == b.lastIndex
                   && a.width == b.width && a.height == b.height;
    }
    check(rowsSame, QStringLiteral("② 每一行的字段都一致（ord/父/种类/名字/页/区间/尺寸）"));

    // ── ③ 读回来的行能 restore，并且定位回原下标 ──
    {
        Sequence restored;
        restored.restore(back.meta.sourcePath, back.rows, back.meta.sourceKind == Sequence::Kind::Archive);
        const int idx = restored.indexOfChain(back.meta.chain, back.meta.page);
        check(restored.count() == seq.count() && idx == cursor,
              QStringLiteral("③ restore 后 %1 条，链/页 落回下标 %2（期望 %3）")
                  .arg(restored.count()).arg(idx).arg(cursor));
        check(restored.chainAt(idx) == back.meta.chain,
              QStringLiteral("③ 落点的链与文件里的一致"));
    }

    // ── ③b 只读元数据段：不解析 rows 也能拿到列表要的字段 ──
    {
        progressfile::Meta m;
        QString e;
        const bool okMeta = progressfile::readMeta(file, &m, &e);
        check(okMeta && m.title == d.meta.title && m.sourcePath == d.meta.sourcePath
                  && m.done == d.meta.done && m.total == d.meta.total
                  && m.chain == d.meta.chain && m.page == d.meta.page
                  && m.layout == d.meta.layout && m.rowCount == d.rows.size(),
              QStringLiteral("③b 只读元数据段：字段齐、行数 %1（%2）").arg(m.rowCount).arg(e));
    }

    // ── ③c v1（单个 JSON 文档）仍能读：不迁移、不报错 ──
    {
        const QString v1file = tmp.filePath(QStringLiteral("v1.prsave"));
        QFile f(v1file);
        f.open(QIODevice::WriteOnly);
        f.write("{\"format\":\"prsave\",\"version\":1,"
                "\"title\":\"old\",\"savedAt\":5,"
                "\"source\":{\"path\":\"" + QFileInfo(srcDir).absoluteFilePath().toUtf8()
                + "\",\"kind\":2},"
                "\"layout\":0,"
                "\"position\":{\"chain\":[\"p1.png\"],\"page\":0},"
                "\"progress\":{\"done\":1,\"total\":2},"
                "\"rows\":[{\"ord\":0,\"kind\":0,\"label\":\"p1.png\"}]}");
        f.close();
        progressfile::Data v1;
        QString e;
        const bool okV1 = progressfile::read(v1file, &v1, &e);
        check(okV1 && v1.meta.version == 1 && v1.meta.title == QLatin1String("old")
                  && v1.rows.size() == 1,
              QStringLiteral("③c v1 旧文件仍能整份读（version=%1，行数 %2，%3）")
                  .arg(v1.meta.version).arg(v1.rows.size()).arg(e));
        //   只读元数据段对 v1 会失败（没有 prsave 头）⇒ 调用方退回整份读
        progressfile::Meta m;
        const bool metaV1 = progressfile::readMeta(v1file, &m, &e);
        check(!metaV1, QStringLiteral("③c v1 没有元数据段 ⇒ readMeta 失败（调用方回退）"));
    }

    // ── ④ 坏文件 / 非进度文件 / 版本不认识 ──
    {
        const QString bad = tmp.filePath(QStringLiteral("bad.prsave"));
        QFile f(bad);
        f.open(QIODevice::WriteOnly);
        f.write("{ 这不是 json");
        f.close();
        progressfile::Data out;
        QString e;
        //   ★ 先调用再判断：`check(cond, msg)` 的两个实参求值顺序未定义，
        //     在参数里调用 read 会把 `e` 读到**上一个**值（写测试时实测踩到）。
        const bool badOk = progressfile::read(bad, &out, &e);
        check(!badOk && !e.isEmpty(),
              QStringLiteral("④ 坏 JSON ⇒ 拒绝并给原因（%1）").arg(e));

        const QString notOurs = tmp.filePath(QStringLiteral("other.prsave"));
        QFile g(notOurs);
        g.open(QIODevice::WriteOnly);
        g.write("{\"format\":\"something\",\"version\":1}");
        g.close();
        const bool notOursOk = progressfile::read(notOurs, &out, &e);
        check(!notOursOk && e.contains(QStringLiteral("不是进度文件")),
              QStringLiteral("④ 不是进度文件 ⇒ 拒绝（%1）").arg(e));

        const QString newer = tmp.filePath(QStringLiteral("v9.prsave"));
        QFile h(newer);
        h.open(QIODevice::WriteOnly);
        h.write("{\"format\":\"prsave\",\"version\":9,\"source\":{\"path\":\"/x\"},"
                "\"rows\":[{\"ord\":0}]}");
        h.close();
        const bool newerOk = progressfile::read(newer, &out, &e);
        check(!newerOk && e.contains(QStringLiteral("版本")),
              QStringLiteral("④ 版本不认识 ⇒ 拒绝（%1）").arg(e));
    }

    return check.finish();
}

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, &runProgressSelfTest);
}
