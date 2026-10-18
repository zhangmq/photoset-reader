#include "testsupport.h"

#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLockFile>
#include <QSet>
#include <QSqlDatabase>
#include <QString>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>

#include <atomic>
#include <thread>
#include <sys/stat.h>
#include <sys/types.h>

#include "librarycore.h"
#include "librarystore.h"
#include "resetqueue.h"
#include "volumeinfo.h"

// ══ 重置模型自检（ib-test-reset；设计 14 §9.1 / §9.3 / §9.5）══════════════════
//   本批（T2c-1）交付的三个东西逐条验：
//     ① `Store::replaceItems` 原子替换（成功 / **中途失败 ⇒ 旧内容原样保留** / 分块跨边界）
//     ② `ResetQueue` 串行（连入 3 个 ⇒ 逐个 running，不并发；排队中取消 ⇒ 不执行）
//     ③ 跨进程锁（手工占住 `reset-<cid>.lock` ⇒ Failed + 明确原因；释放后可再入队）
//     ④ 三种收尾（完成 ⇒ 落库回调被调 + Done；取消 / 失败 ⇒ **回调不被调用** + 状态正确）
//     ⑤ 库目录闸门（非本地固定卷 ⇒ `libraryUsable()==false` + **原因是卷闸门**）
//     ⑥ LibraryCore 端到端 + 槽位别名（View==Reset 是**同一个对象**；View/Session 互斥）
//
//   T2c-2a 补的三处"设计里缺的结构"逐条验：
//     ⑦ **发布钩子**：每次放批都调（计数 = 批数）/ 未装钩子行为一致 / **钩子在队列锁之外**
//     ⑧ **会话槽有界收尾**：FIFO 真卡死 ⇒ 阈值调小后 `closeSession()` 有界返回 + abandon
//        （对照：正常来源走干净 join，没有多出来的线程）
//     ⑨ **落库后台化**：2 万行 ⇒ `replaceItems` 在**另一个线程**执行 + 不阻塞 GUI；
//        注入"worker 端库打不开" ⇒ Failed + 旧内容不变 + 锁释放
//
//   规则：**只碰临时库 / 临时目录**（`PHOTOSET_LIBRARY_DB`），绝不碰用户真库。

namespace {

using testsupport::Check;

// 本进程的线程数（`/proc/self/status`）：证明"被放弃的生产者线程**真的还活着**"
//   —— detach + 故意泄漏的直接证据（干净收尾时线程数回到基线）。
//   ⚠️ procfs 的 `size()` 是 0 ⇒ **不能用 `atEnd()`**（Qt 会当成立刻结束）；按行读到空为止。
int threadCount()
{
    QFile f(QStringLiteral("/proc/self/status"));
    if (!f.open(QIODevice::ReadOnly)) return -1;
    QByteArray line;
    while (!(line = f.readLine()).isEmpty()) {
        if (line.startsWith("Threads:"))
            return line.mid(8).trimmed().toInt();
    }
    return -1;
}

// 库连接名（QtSql 的规矩：**每线程一个连接**；`Store` 的连接名带线程标识，见 librarystore.cpp）
QStringList libraryConnections()
{
    QStringList out;
    for (const QString &n : QSqlDatabase::connectionNames())
        if (n.startsWith(QLatin1String("photoset-library-"))) out << n;
    return out;
}

// 某图集的跨进程锁文件路径（§9.5 的唯一拼法；库目录取当前 `PHOTOSET_LIBRARY_DB`）
QString testLockPath(qint64 cid)
{
    const QString dir = QFileInfo(library::Store::databasePath()).absolutePath();
    return QStringLiteral("%1/locks/reset-%2.lock").arg(dir).arg(cid);
}

// **不跑事件循环**的等待（要用例亲手控制"什么时候才 post 结果回来"时用：
//   `waitFor` 会 `processEvents` ⇒ 唤醒钩子排的 pump / 落库结果都会被提前消费）。
bool waitNoEvents(const std::function<bool()> &done, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (done()) return true;
        QThread::msleep(1);
    }
    return done();
}


// 库文件路径覆盖（每组自己设；组内一切 Store/LibraryCore 都必须在这之后构造）
void useLibraryDbIn(const QString &dir)
{
    qputenv("PHOTOSET_LIBRARY_DB",
            QDir(dir).filePath(QStringLiteral("library.db")).toLocal8Bit());
}

library::Item mkRow(int ord, const QString &label, Sequence::Kind kind = Sequence::Kind::Image)
{
    library::Item it;
    it.ord = ord;
    it.parentOrd = -1;
    it.kind = kind;
    it.label = label;
    it.size = 100 + ord;
    it.mtime = 1000 + ord;
    it.width = 8;
    it.height = 12;
    return it;
}

// 假回调（**不依赖 Sequence** ⇒ 队列本身可单测）：只记账，可注入"某集合落库失败"
struct Fake {
    QVector<QString> log;
    int begins = 0, commits = 0, ends = 0;
    QSet<qint64> failCommit;
};

resetq::ResetQueue::Hooks fakeHooks(Fake &f)
{
    resetq::ResetQueue::Hooks h;
    h.begin = [&f](qint64 id, QString *) {
        ++f.begins;
        f.log.append(QStringLiteral("begin%1").arg(id));
        return true;
    };
    h.commit = [&f](qint64 id, QString *err) {
        ++f.commits;
        f.log.append(QStringLiteral("commit%1").arg(id));
        if (f.failCommit.contains(id)) {
            if (err) *err = QStringLiteral("落库失败（用例注入）");
            return false;
        }
        return true;
    };
    h.end = [&f](qint64 id) {
        ++f.ends;
        f.log.append(QStringLiteral("end%1").arg(id));
    };
    return h;
}

// ── ① replaceItems 原子性 ─────────────────────────────────────────────────
void groupReplaceAtomic(Check &check)
{
    QTemporaryDir tmp;
    useLibraryDbIn(tmp.path());
    library::Store store;
    QString err;
    if (!store.open(&err)) {
        check(false, QStringLiteral("① 打开临时库（%1）").arg(err));
        return;
    }
    const qint64 cid = store.addCollection(tmp.filePath(QStringLiteral("album")),
                                           Sequence::Kind::Folder);

    QVector<library::Item> oldRows;
    for (int i = 0; i < 3; ++i) oldRows << mkRow(i, QStringLiteral("old%1").arg(i));
    check(store.replaceItems(cid, oldRows), QStringLiteral("① 先落 3 行旧内容（唯一写入路径 = replaceItems）"));

    // 新行 1100 条 ⇒ **跨过 `kReplaceChunkRows`（1024）的分块边界**（2 块）
    QVector<library::Item> fresh;
    for (int i = 0; i < 1100; ++i) fresh << mkRow(i, QStringLiteral("new%1").arg(i));
    check(store.replaceItems(cid, fresh), QStringLiteral("① 原子替换 1100 行（分块 ≥2 块，无巨型语句）"));
    {
        const QVector<library::Item> got = store.items(cid);
        bool noOld = true;
        for (const library::Item &r : got)
            if (r.label.startsWith(QLatin1String("old"))) noOld = false;
        check(got.size() == 1100 && noOld
                  && got.first().label == QLatin1String("new0")
                  && got.last().label == QLatin1String("new1099"),
              QStringLiteral("① 旧行全消失 + 新行齐全（%1 行）").arg(got.size()));
    }

    // 故意让**第二块**失败：`ord` 与第一块里的一行撞 UNIQUE(collection_id, ord)
    //   ⇒ 第一块已经 INSERT 进事务，第二块失败必须把第一块一起回滚
    QVector<library::Item> bad;
    for (int i = 0; i < 1100; ++i) bad << mkRow(i, QStringLiteral("bad%1").arg(i));
    bad[1050].ord = bad[300].ord;                       // 1050 在第二块，300 在第一块
    check(!store.replaceItems(cid, bad), QStringLiteral("① 中途（第二块）失败 ⇒ 返回 false"));
    check(!store.lastError().isEmpty(),
          QStringLiteral("① 失败写了 lastError：%1").arg(store.lastError()));
    {
        const QVector<library::Item> after = store.items(cid);
        const bool intact = after.size() == 1100
                            && after.first().label == QLatin1String("new0")
                            && after.last().label == QLatin1String("new1099");
        check(intact, QStringLiteral("① 失败后**旧内容原样保留**（%1 行；不是空/半截）").arg(after.size()));
    }
    // 失败不能把连接/事务搞坏：还能再替换一次
    check(store.replaceItems(cid, oldRows) && store.items(cid).size() == 3
              && store.items(cid).first().label == QLatin1String("old0"),
          QStringLiteral("① 失败之后库仍可用（再次替换成立）"));
}

// ── ② 队列串行 ───────────────────────────────────────────────────────────
void groupSerial(Check &check)
{
    QTemporaryDir tmp;
    Fake f;
    resetq::ResetQueue q(tmp.path(), fakeHooks(f));
    QString err;

    check(q.request(1, &err), QStringLiteral("② 入队 1（空闲 ⇒ 立刻起跑）"));
    check(q.state(1) == resetq::ResetState::Running && q.runningId() == 1,
          QStringLiteral("② 1 = Running，runningId = 1"));
    check(q.request(2, &err) && q.request(3, &err), QStringLiteral("② 入队 2、3"));
    check(q.state(2) == resetq::ResetState::Queued && q.state(3) == resetq::ResetState::Queued,
          QStringLiteral("② 2、3 = Queued（**同时只有一条 running**）"));
    check(f.begins == 1, QStringLiteral("② 只有 1 起了活序列（begins=%1）").arg(f.begins));
    const bool dup = q.request(2, &err);          // ⚠️ 先求值再拼文案（实参求值顺序不确定）
    check(!dup && err.contains(QStringLiteral("已在重置队列")),
          QStringLiteral("② 重复入队被拒：%1").arg(err));

    check(q.cancel(3), QStringLiteral("② 取消排队中的 3"));
    check(q.state(3) == resetq::ResetState::Stopped, QStringLiteral("② 3 = Stopped"));

    check(q.finishCompleted(1), QStringLiteral("② 1 完成（落库回调被调）"));
    check(q.state(1) == resetq::ResetState::Done && f.commits == 1 && f.ends == 1,
          QStringLiteral("② 1 = Done，commit/end 各 1 次"));
    check(q.runningId() == 2 && q.state(2) == resetq::ResetState::Running && f.begins == 2,
          QStringLiteral("② 收尾后**让位给下一个**（2 起跑）"));
    check(!f.log.contains(QStringLiteral("begin3")),
          QStringLiteral("② 被取消的 3 **从未执行**"));
    check(f.log.indexOf(QStringLiteral("end1")) < f.log.indexOf(QStringLiteral("begin2")),
          QStringLiteral("② 串行次序：1 收尾之后 2 才起（%1）").arg(f.log.join(QLatin1Char(','))));

    q.finishStopped(2);
    check(q.state(2) == resetq::ResetState::Stopped && f.commits == 1 && q.runningId() == -1,
          QStringLiteral("② 停止 ⇒ 状态 Stopped、**不落库**、running 位让出"));

    check(q.request(1, &err) && q.state(1) == resetq::ResetState::Running,
          QStringLiteral("② 终态可以**再次入队**（重跑是正常动作）"));
    q.finishFailed(1, QStringLiteral("用例收尾"));
    check(q.state(1) == resetq::ResetState::Failed
              && q.reason(1) == QStringLiteral("用例收尾"),
          QStringLiteral("② Failed 带原因"));
}

// ── ③ 跨进程锁 ───────────────────────────────────────────────────────────
void groupLock(Check &check)
{
    QTemporaryDir tmp;
    const qint64 cid = 7;
    QDir().mkpath(tmp.path() + QStringLiteral("/locks"));

    Fake f;
    resetq::ResetQueue q(tmp.path(), fakeHooks(f));
    const QString lockPath = q.lockPathFor(cid);
    check(lockPath == tmp.path() + QStringLiteral("/locks/reset-7.lock"),
          QStringLiteral("③ 锁路径 = <库目录>/locks/reset-<cid>.lock（%1）").arg(lockPath));

    QLockFile held(lockPath);                       // 模拟"另一实例"
    check(held.tryLock(0), QStringLiteral("③ 用例先手工占住锁（前置）"));

    QString err;
    check(q.request(cid, &err), QStringLiteral("③ 占着锁的情况下入队"));
    check(q.state(cid) == resetq::ResetState::Failed,
          QStringLiteral("③ 拿不到锁 ⇒ **Failed**（不跨进程排队）"));
    check(q.reason(cid).contains(QStringLiteral("另一实例正在重置该图集")),
          QStringLiteral("③ 原因明确：%1").arg(q.reason(cid)));
    check(q.takeNotice().contains(QStringLiteral("另一实例")),
          QStringLiteral("③ 有用户提示（takeNotice）"));
    check(f.begins == 0, QStringLiteral("③ 没有起活序列（begins=%1）").arg(f.begins));

    held.unlock();
    check(q.request(cid, &err), QStringLiteral("③ 锁释放后**可以再入队**"));
    check(q.state(cid) == resetq::ResetState::Running && f.begins == 1,
          QStringLiteral("③ 这次拿到了锁（Running + begin 1 次）"));
    q.finishFailed(cid, QStringLiteral("用例收尾"));
    check(q.state(cid) == resetq::ResetState::Failed && !QFile::exists(lockPath),
          QStringLiteral("③ 失败收尾也**释放锁**（锁文件已删）"));
}

// ── ④ 三种收尾 ───────────────────────────────────────────────────────────
void groupEndings(Check &check)
{
    QTemporaryDir tmp;
    Fake f;
    resetq::ResetQueue q(tmp.path(), fakeHooks(f));

    QString err;
    check(q.request(11, &err) && q.state(11) == resetq::ResetState::Running,
          QStringLiteral("④ 入队 11"));
    check(QFile::exists(q.lockPathFor(11)), QStringLiteral("④ running 期间**持锁**（锁文件存在）"));
    check(q.finishCompleted(11), QStringLiteral("④ 完成 ⇒ 落库回调返回 true"));
    check(q.state(11) == resetq::ResetState::Done && f.commits == 1 && f.ends == 1,
          QStringLiteral("④ 完成：Done + commit/end 各 1 次"));
    check(!QFile::exists(q.lockPathFor(11)), QStringLiteral("④ 完成也释放锁"));

    q.request(12, &err);
    q.finishStopped(12);
    check(q.state(12) == resetq::ResetState::Stopped && f.commits == 1 && f.ends == 2,
          QStringLiteral("④ 主动取消：Stopped、**落库回调不被调用**（commits 仍 1）"));

    q.request(13, &err);
    q.finishFailed(13, QStringLiteral("卡住（用例注入）"));
    check(q.state(13) == resetq::ResetState::Failed && f.commits == 1 && f.ends == 3
              && q.reason(13).contains(QStringLiteral("卡住")),
          QStringLiteral("④ 失败/卡住：Failed、**不落库**、原因保留"));

    // 落库自己失败 ⇒ 也必须是 Failed（不落库），而且 commit 确实被调过（区别于"没调"）
    Fake f2;
    f2.failCommit.insert(14);
    resetq::ResetQueue q2(tmp.path() + QStringLiteral("/q2"), fakeHooks(f2));
    q2.request(14, &err);
    check(!q2.finishCompleted(14), QStringLiteral("④ 落库失败 ⇒ finishCompleted 返回 false"));
    check(q2.state(14) == resetq::ResetState::Failed && f2.commits == 1
              && q2.reason(14).contains(QStringLiteral("落库失败")),
          QStringLiteral("④ 落库失败：Failed + 原因（commits 确实 1 次）"));
}

// ── ⑤ 库目录闸门 ─────────────────────────────────────────────────────────
void groupVolumeGate(Check &check)
{
    // 正向对照：本地固定卷上的临时库 ⇒ 可用（否则"恒返回 false"也能骗过负向检查）
    QTemporaryDir tmp;
    useLibraryDbIn(tmp.path());
    library::Store okStore;
    QString err;
    if (!okStore.open(&err)) {
        check(false, QStringLiteral("⑤ 打开临时库（%1）").arg(err));
        return;
    }
    {
        LibraryCore okCore(&okStore);
        check(okCore.libraryUsable() && okCore.unusableReason().isEmpty(),
              QStringLiteral("⑤ 对照：本地固定卷 + 库已打开 ⇒ libraryUsable()"));
    }

    // 找一个**真实存在**的非本地固定卷（网络 / autofs）
    QString mount, fsType;
    for (const volume::MountEntry &e : volume::mountTable()) {
        if (e.kind == volume::Kind::Network || e.autofs) {
            mount = e.mountPoint;
            fsType = e.fsType;
            break;
        }
    }
    if (mount.isEmpty()) {
        // 个别检查跳过（**不计入项数**，整条套件照常 pass/fail —— 见 tests/CMakeLists.txt 的约定）
        qInfo().noquote() << QStringLiteral(
            "[resettest] 跳过：本机挂载表里没有网络 / autofs 卷 ⇒ 库目录闸门无法实测");
        return;
    }

    useLibraryDbIn(mount + QStringLiteral("/photoset-gate-probe"));
    library::Store netStore;                 // **故意不 open**：一个字节都不碰那个卷
    LibraryCore netCore(&netStore);
    check(!netCore.libraryUsable(),
          QStringLiteral("⑤ 非本地固定卷（%1，%2）⇒ libraryUsable()==false").arg(mount, fsType));
    const QString why = netCore.unusableReason();
    check(why.contains(QStringLiteral("本地固定卷")),
          QStringLiteral("⑤ 原因来自**卷闸门**（不是「库未打开」）：%1").arg(why));
    if (!fsType.isEmpty())
        check(why.contains(fsType), QStringLiteral("⑤ 原因里带真实 fs 类型（%1）").arg(fsType));

    QString rerr;
    const bool blocked = netCore.requestReset(1, LibraryCore::ResetOptions(), &rerr);
    check(!blocked && !rerr.isEmpty(),
          QStringLiteral("⑤ 闸门也挡在重置入口：%1").arg(rerr));
}

// ── ⑥ LibraryCore 端到端 + 槽位别名 ──────────────────────────────────────
void groupCoreEndToEnd(Check &check)
{
    QTemporaryDir tmp;
    useLibraryDbIn(tmp.path());

    // 真实素材：临时目录里 3 张真 PNG（不依赖 .testdata）
    const QString album = tmp.filePath(QStringLiteral("album"));
    QDir().mkpath(album);
    for (int i = 0; i < 3; ++i) {
        QImage img(8, 12, QImage::Format_RGB32);
        img.fill(QColor(40 + i * 20, 80, 120));
        if (!img.save(QDir(album).filePath(QStringLiteral("p%1.png").arg(i)))) {
            check(false, QStringLiteral("⑥ 造夹具图片失败"));
            return;
        }
    }

    library::Store store;
    QString err;
    if (!store.open(&err)) {
        check(false, QStringLiteral("⑥ 打开临时库（%1）").arg(err));
        return;
    }
    LibraryCore core(&store);
    check(core.libraryUsable(), QStringLiteral("⑥ 库目录闸门通过"));

    check(core.addCollection(album, Sequence::Kind::Folder, &err),
          QStringLiteral("⑥ addCollection 转调成功"));
    const QVector<library::Collection> cs = core.collections();
    check(cs.size() == 1, QStringLiteral("⑥ collections() 里有 1 个（%1）").arg(cs.size()));
    const qint64 cid = cs.isEmpty() ? -1 : cs.first().id;

    check(core.requestReset(cid, LibraryCore::ResetOptions(), &err),
          QStringLiteral("⑥ 请求重置（串行队列空闲 ⇒ 立刻起活序列）"));
    check(core.resetState(cid) == LibraryCore::ResetState::Running,
          QStringLiteral("⑥ 请求后立刻 Running"));
    check(core.resetSequence(cid) != nullptr, QStringLiteral("⑥ resetSequence(id) 非空"));

    // ★ 槽位别名：还没 pump（没观察到终态）⇒ Reset 那条**就是** View 那条
    check(core.openCollection(cid, &err), QStringLiteral("⑥ openCollection"));
    check(core.viewSequence() != nullptr && core.viewSequence() == core.resetSequence(cid),
          QStringLiteral("⑥ **View == Reset ⇒ 同一个 Sequence 对象**（别名，不建副本）"));

    // View 与 Session 互斥（同一个显示槽）
    check(core.openSession(album, &err) && core.sessionSequence() != nullptr,
          QStringLiteral("⑥ openSession"));
    check(core.viewSequence() == nullptr,
          QStringLiteral("⑥ 开会话 ⇒ 释放查看槽（View 与 Session 互斥）"));
    core.closeSession();
    check(core.sessionSequence() == nullptr, QStringLiteral("⑥ closeSession 销毁会话序列"));
    // 再看这个**正在重置**的图集 ⇒ 又是别名（View==Reset）
    check(core.openCollection(cid, &err) && core.viewSequence() == core.resetSequence(cid),
          QStringLiteral("⑥ 重新 openCollection ⇒ 仍是那条活序列的别名"));

    // 排队取消（串行）：第二个集合在第一个 running 期间只能 Queued，取消 ⇒ 不执行、不落库
    const QString album2 = tmp.filePath(QStringLiteral("album2"));
    QDir().mkpath(album2);
    QImage img(8, 12, QImage::Format_RGB32);
    img.fill(Qt::green);
    img.save(QDir(album2).filePath(QStringLiteral("q0.png")));
    check(core.addCollection(album2, Sequence::Kind::Folder, &err), QStringLiteral("⑥ 加第二个图集"));
    qint64 cid2 = -1;
    for (const library::Collection &c : core.collections())
        if (c.rootPath.endsWith(QStringLiteral("album2"))) cid2 = c.id;
    check(cid2 > 0, QStringLiteral("⑥ 第二个图集 id=%1").arg(cid2));

    check(core.requestReset(cid2, LibraryCore::ResetOptions(), &err), QStringLiteral("⑥ 入队第二个"));
    check(core.resetState(cid2) == LibraryCore::ResetState::Queued,
          QStringLiteral("⑥ 第二个 = Queued（**不并发**）"));
    const bool dup2 = core.requestReset(cid2, LibraryCore::ResetOptions(), &err);
    check(!dup2 && err.contains(QStringLiteral("已在重置队列")),
          QStringLiteral("⑥ 重复入队被拒：%1").arg(err));
    check(core.cancelReset(cid2) && core.resetState(cid2) == LibraryCore::ResetState::Stopped,
          QStringLiteral("⑥ 取消排队中的第二个 ⇒ Stopped"));

    // 跑完第一个：pump 直到 Done（**判定结束的条件在 pump 的 drain 内观察**）
    const bool done = testsupport::waitFor([&core, cid] {
        core.pump();
        return core.resetState(cid) == LibraryCore::ResetState::Done;
    }, 15000);
    check(done, QStringLiteral("⑥ 重置跑完 ⇒ Done（状态：%1）")
                    .arg(resetq::resetStateName(core.resetState(cid))));

    {
        const QVector<library::Item> rows = store.items(cid);
        int images = 0;
        for (const library::Item &r : rows)
            if (r.kind == Sequence::Kind::Image) ++images;
        check(images == 3 && store.itemCount(cid) == 3,
              QStringLiteral("⑥ **原子替换真的落库**：%1 张图 / itemCount=%2")
                  .arg(images).arg(store.itemCount(cid)));
        check(store.items(cid2).isEmpty(),
              QStringLiteral("⑥ 被取消的第二个**没有落库**（0 行）"));
    }
    check(core.resetSequence(cid) == nullptr,
          QStringLiteral("⑥ 收尾释放了活序列（resetSequence == nullptr）"));
    check(core.viewSequence() != nullptr && core.viewSequence() != core.resetSequence(cid),
          QStringLiteral("⑥ 收尾后 View 换成**从磁盘读出的快照**（不再是活序列）"));
    const QString notice = core.takeNotice();
    check(!notice.isEmpty(), QStringLiteral("⑥ 有用户提示：%1").arg(notice));

    // 队列让位之后，第二个图集（已被取消）可以重新入队并跑完
    check(core.requestReset(cid2, LibraryCore::ResetOptions(), &err)
              && core.resetState(cid2) == LibraryCore::ResetState::Running,
          QStringLiteral("⑥ 队首让位 ⇒ 第二个可以重新入队并起跑"));
    const bool done2 = testsupport::waitFor([&core, cid2] {
        core.pump();
        return core.resetState(cid2) == LibraryCore::ResetState::Done;
    }, 15000);
    check(done2 && store.itemCount(cid2) == 1,
          QStringLiteral("⑥ 第二个跑完 ⇒ Done + 落库 1 张（itemCount=%1）").arg(store.itemCount(cid2)));
}

// ══ ⑦ 发布钩子（T2c-2a 缺口①）═══════════════════════════════════════════════
//   测什么：`Sequence::setPublishHook` 的**唯一调用口径** —— 生产者每放一批调一次、
//   写终态再调一次；未装钩子时行为一字不变；钩子在**队列锁之外**跑（不拖住 drain）。
//   怎么保证非空洞：见每组注释里的"变异测试验证"。
void groupPublishHook(Check &check)
{
    QTemporaryDir tmp;
    const QString album = tmp.filePath(QStringLiteral("album"));
    QDir().mkpath(album);
    const int n = 40;                                   // 40 行 ⇒ 3 个满批 + 尾批
    for (int i = 0; i < n; ++i) {
        QImage img(8, 12, QImage::Format_RGB32);
        img.fill(QColor(30 + i, 90, 140));
        if (!img.save(QDir(album).filePath(
                QStringLiteral("p%1.png").arg(i, 3, 10, QLatin1Char('0'))))) {
            check(false, QStringLiteral("⑦ 造夹具图片失败"));
            return;
        }
    }
    // 调用口径 = `Sequence::scanThreadMain`（**结构性推导**，不是抄实现里的数字）：
    //   · 放批：首片（空行 + 根描述）1 次 + 每**满 16 行** 1 次 + 末尾 `flush(true)` 1 次；
    //   · 写终态之后 1 次（消费者要靠它才知道"终态到了"）。
    const int rowBatches  = (n + 15) / 16;      // 40 行 ⇒ 3（16 + 16 + 8）
    const int batchPosts  = 1 + rowBatches;     // 4
    const int expectCalls = batchPosts + 1;     // 5（放批 4 + 终态 1）

    const Qt::HANDLE callerTid = QThread::currentThreadId();

    // ── ⑦a 装了钩子：每次放批都被调用，且**在生产者线程**调 ──
    std::atomic<int>  calls{0};
    std::atomic<bool> offThread{false};
    Sequence hooked;
    hooked.setPublishHook([&] {
        if (QThread::currentThreadId() != callerTid) offThread.store(true);
        calls.fetch_add(1, std::memory_order_relaxed);
    });
    const qint64 t0 = monotonicNowMs();
    hooked.startScan(album, 5, false);
    const bool fin1 = testsupport::waitFor([&] { hooked.drain(); return hooked.scanFinished(); }, 15000);
    hooked.drain();
    const qint64 tHookMs = monotonicNowMs() - t0;
    check(fin1 && hooked.count() == n && hooked.state() == Sequence::State::Scaned,
          QStringLiteral("⑦ 装钩子的扫描正常跑完（%1 项，状态 Scaned）").arg(hooked.count()));
    check(calls.load() == expectCalls,
          QStringLiteral("⑦ **每次放批 + 写终态都被调用**：%1 次 = 放批 %2（首片 + %3 个行批）+ 终态 1")
              .arg(calls.load()).arg(batchPosts).arg(rowBatches));
    check(offThread.load(),
          QStringLiteral("⑦ 钩子由**生产者线程**调（调用者线程 id 不同 ⇒ 必须跨线程投递）"));

    // ── ⑦b 未装钩子：同一夹具下 count()/状态/行 完全一致 ──
    Sequence plain;
    const qint64 t1 = monotonicNowMs();
    plain.startScan(album, 5, false);
    const bool fin2 = testsupport::waitFor([&] { plain.drain(); return plain.scanFinished(); }, 15000);
    plain.drain();
    const qint64 tPlainMs = monotonicNowMs() - t1;
    bool sameRows = (plain.rows().size() == hooked.rows().size());
    for (int i = 0; sameRows && i < plain.rows().size(); ++i)
        sameRows = (plain.rows().at(i).label == hooked.rows().at(i).label
                    && plain.rows().at(i).kind == hooked.rows().at(i).kind);
    check(fin2 && plain.count() == hooked.count() && plain.state() == hooked.state(),
          QStringLiteral("⑦ 未装钩子：count/状态与装了**完全一致**（%1 项）").arg(plain.count()));
    check(sameRows, QStringLiteral("⑦ 未装钩子：行也一致（%1 行）").arg(plain.rows().size()));
    // 生产者总耗时的量级检查（钩子体是"计数"这种零成本操作 ⇒ 只该是噪声）
    check(tHookMs < tPlainMs * 4 + 500,
          QStringLiteral("⑦ 生产者耗时同量级（装 %1 ms vs 未装 %2 ms）").arg(tHookMs).arg(tPlainMs));

    // ── ⑦c 钩子在**队列锁之外**跑（硬证据）──
    //   钩子第一次被调时睡 300 ms：消费者的 `drain()` **必须**照常立刻返回。
    //   ⚠️ 变异测试验证：把 `post()` 里的 `publish()` 挪进 `QMutexLocker` 内，这一项必 FAIL。
    Sequence slow;
    std::atomic<bool> inHook{false};
    slow.setPublishHook([&] {
        bool first = false;
        if (!inHook.exchange(true)) first = true;
        if (first) QThread::msleep(300);          // 只在第一次放批时"卡"钩子
    });
    slow.startScan(album, 5, false);
    const bool entered = testsupport::waitFor([&] { return inHook.load(); }, 5000);
    check(entered, QStringLiteral("⑦ 前置：生产者已进入钩子（正在睡 300 ms）"));
    const qint64 td0 = monotonicNowMs();
    slow.drain();                                  // 消费者：**不许**被钩子拖住
    const qint64 drainMs = monotonicNowMs() - td0;
    check(drainMs < 100,
          QStringLiteral("⑦ 钩子睡着时 `drain()` 仍只要 %1 ms（<< 300 ms）⇒ 钩子在**队列锁之外**")
              .arg(drainMs));
    const bool fin3 = testsupport::waitFor([&] { slow.drain(); return slow.scanFinished(); }, 15000);
    check(fin3 && slow.count() == n, QStringLiteral("⑦ ⑦c 的扫描仍跑完（%1 项）").arg(slow.count()));
}

// ══ ⑧ 会话槽的有界收尾（T2c-2a 缺口②）══════════════════════════════════════
//   测什么：`closeSession()` 从"无超时 join"变成"置停止位 ⇒ 有界宽限 ⇒ 超了 abandon + 泄漏"。
//   怎么保证非空洞：FIFO 当来源根制造**真卡死**（生产者阻塞在 open(fifo) 上，永远不会
//   bumpProgress）；阈值用 `PHOTOSET_RESET_STALL_MS` 调成 300 ms ⇒ 实测耗时必须
//   **有界**（远小于默认 30 s）且落在宽限附近；对象 `isAbandoned()==true`（**没被销毁**）
//   + `/proc/self/status` 线程数 +1（detached 生产者真的还活着）。对照：正常来源走干净
//   join ⇒ 快、无"放弃"提示、线程数回基线。
void groupBoundedClose(Check &check)
{
    QTemporaryDir tmp;
    useLibraryDbIn(tmp.path());
    library::Store store;
    QString err;
    if (!store.open(&err)) {
        check(false, QStringLiteral("⑧ 打开临时库（%1）").arg(err));
        return;
    }
    const QString album = tmp.filePath(QStringLiteral("album"));
    QDir().mkpath(album);
    for (int i = 0; i < 3; ++i) {
        QImage img(8, 12, QImage::Format_RGB32);
        img.fill(QColor(40 + i * 20, 80, 120));
        img.save(QDir(album).filePath(QStringLiteral("p%1.png").arg(i)));
    }
    // ★ 只为**本组**把"卡住"阈值调小（默认 30000 不动 —— 本仓库既有的测试覆盖风格）
    qputenv("PHOTOSET_RESET_STALL_MS", "300");

    // ── ⑧a 对照：正常来源 ⇒ 干净 join（快 + 无放弃提示 + 生产者计数回 0）──
    //   ★ T2c-2b-2：判定条件从 `/proc/self/status` 线程数改成 **`Sequence::producerThreadCount()`**
    //     （跨平台；procfs 在部分环境不可用 ⇒ `-1 == -1` 会假通过、`-1 > -1` 会假失败）。
    const int threads0 = threadCount();               // **只作辅助日志**，不作判定条件
    {
        LibraryCore core(&store);
        check(core.openSession(album, &err), QStringLiteral("⑧a 对照：打开正常会话"));
        const bool done = testsupport::waitFor([&] {
            core.pump();
            Sequence *s = core.sessionSequence();
            return s && s->scanFinished();
        }, 5000);
        check(done, QStringLiteral("⑧a 对照：正常来源扫描跑完"));
        const qint64 t0 = monotonicNowMs();
        core.closeSession();
        const qint64 ms = monotonicNowMs() - t0;
        check(core.sessionSequence() == nullptr, QStringLiteral("⑧a 对照：会话槽已清空"));
        check(ms < 150, QStringLiteral("⑧a 对照：`closeSession()` %1 ms（**远小于 300 ms 宽限**）"
                                       "⇒ 走的是**干净 join**").arg(ms));
        check(!core.takeNotice().contains(QStringLiteral("放弃")),
              QStringLiteral("⑧a 对照：**没有**走 abandon（无「已放弃」提示）"));
        check(core.libraryUsable(), QStringLiteral("⑧a 对照：库仍可用"));
    }
    //   干净收尾 ⇒ 生产者计数必须回 0（对象已销毁，用一条本地序列拿同一份证据）
    {
        Sequence clean;
        clean.startScan(album, 5, false);
        check(testsupport::waitFor([&] { clean.drain(); return clean.scanFinished(); }, 5000),
              QStringLiteral("⑧a 对照：本地序列也跑完（前置）"));
        clean.requestStop();
        clean.cancelScan();                            // 唯一的 join 点（join ⇒ 计数已 −1）
        check(clean.producerThreadCount() == 0,
              QStringLiteral("⑧a 对照：干净 join 后生产者线程计数 = %1（应为 0 ⇒ 无泄漏）")
                  .arg(clean.producerThreadCount()));
        check(!clean.isAbandoned(),
              QStringLiteral("⑧a 对照：这条对照序列**没有**被放弃（走的是干净 join）"));
    }
    qInfo().noquote() << QStringLiteral(
        "[resettest] 辅助（不作判定条件）：procfs 线程数 基线 %1 → ⑧a 后 %2")
        .arg(threads0).arg(threadCount());

    // ── ⑧b FIFO 真卡死 ⇒ 有界返回 + abandon（对象泄漏是预期的）──
    const QString fifo = tmp.filePath(QStringLiteral("stuck.fifo"));
    check(::mkfifo(fifo.toLocal8Bit().constData(), 0600) == 0,
          QStringLiteral("⑧b 前置：造 FIFO 当来源根"));
    {
        LibraryCore core(&store);
        check(core.openSession(fifo, &err), QStringLiteral("⑧b openSession(FIFO 路径)"));
        Sequence *stuck = core.sessionSequence();
        check(stuck != nullptr, QStringLiteral("⑧b 会话序列在"));
        QThread::msleep(30);                        // 让它真的进到 open(fifo) 里
        check(stuck->scanFinished() == false,
              QStringLiteral("⑧b 前置：生产者**没有**结束（阻塞在不可中断的 open(fifo) 上）"));
        const qint64 t0 = monotonicNowMs();
        core.closeSession();
        const qint64 ms = monotonicNowMs() - t0;
        qInfo().noquote() << QStringLiteral("[resettest] 实测：closeSession(FIFO, 阈值 300 ms) = %1 ms")
                                 .arg(ms);
        // ① 有界返回（不是无超时 join ⇒ 更不是 30 s）；② 也不能是"立刻放弃"（宽限真的等了）
        check(ms >= 150 && ms < 1500,
              QStringLiteral("⑧b `closeSession()` **有界**返回：%1 ms（阈值 300 ms ⇒ 等有界宽限）")
                  .arg(ms));
        check(core.sessionSequence() == nullptr, QStringLiteral("⑧b 槽已清空"));
        check(stuck->isAbandoned(),
              QStringLiteral("⑧b 那条序列 `isAbandoned()==true`（**对象仍在** = 故意泄漏，未被销毁）"));
        check(core.takeNotice().contains(QStringLiteral("放弃")),
              QStringLiteral("⑧b 有「已放弃」提示"));
        check(core.libraryUsable(), QStringLiteral("⑧b 库仍可用（卡死没连累库）"));
        qInfo().noquote() << QStringLiteral(
            "[resettest] 辅助（不作判定条件）：procfs 线程数 基线 %1 → ⑧b 后 %2")
            .arg(threads0).arg(threadCount());
        // ★ T2c-2b-2：**跨平台**判定条件 = 被放弃序列自己的生产者计数仍为 1
        //   （detach + 故意泄漏 ⇒ 线程真的还在）；procfs 只作辅助日志。
        check(stuck->producerThreadCount() == 1,
              QStringLiteral("⑧b 被放弃序列的生产者线程计数 = %1（应为 1）"
                             "⇒ detached 生产者**真的还活着**（泄漏是预期的）")
                  .arg(stuck->producerThreadCount()));
        // ③ 界面没被拖死：随后新会话照常开、照常跑完
        check(core.openSession(album, &err), QStringLiteral("⑧b 放弃之后：新会话仍可开"));
        const bool ok = testsupport::waitFor([&] {
            core.pump();
            Sequence *s = core.sessionSequence();
            return s && s->scanFinished();
        }, 5000);
        check(ok && core.sessionSequence() != nullptr && core.sessionSequence()->count() == 3,
              QStringLiteral("⑧b 放弃之后：新会话照常跑完（%1 项）")
                  .arg(core.sessionSequence() ? core.sessionSequence()->count() : -1));
        core.closeSession();
        check(core.sessionSequence() == nullptr, QStringLiteral("⑧b 新会话照常收尾"));
    }
    qunsetenv("PHOTOSET_RESET_STALL_MS");           // 恢复默认（别的组 / 别的用例不受影响）
}

// ══ ⑨ 落库后台化（T2c-2a 缺口③）════════════════════════════════════════════
//   测什么：`commitReset` 不再同步 `replaceItems`，而是 worker 线程**自己开一个**
//   `library::Store` 写库、写完把结果 post 回 GUI 线程（成功 ⇒ Done；失败 ⇒ Failed）。
//   怎么保证非空洞：
//     · 结构性证据 = worker 的**库连接名带另一个线程的标识**（QtSql 每线程一个连接；
//       `Store::openImpl` 用 `QThread::currentThreadId()` 拼连接名）⇒ `replaceItems`
//       确实在另一个线程执行，不是"GUI 线程里快得看不出来"；
//     · 反证同步 = 起落库的那次 `pump()` 返回后**不跑事件循环**，状态必须仍是 Running；
//     · 失败注入 = 把库路径指到"父级是普通文件"的位置 ⇒ worker 端 `Store::open` 必失败
//       （前置用例先自证"这个路径必然打不开"）⇒ Failed + **旧内容不变** + 锁释放。
void groupBackgroundCommit(Check &check)
{
    QTemporaryDir tmp;
    useLibraryDbIn(tmp.path());
    library::Store store;
    QString err;
    if (!store.open(&err)) {
        check(false, QStringLiteral("⑨ 打开临时库（%1）").arg(err));
        return;
    }

    // ── 夹具：2 万个**子目录** ⇒ 2 万行（造得便宜；写库真的要花时间，才观察得到"后台"）──
    const QString big = tmp.filePath(QStringLiteral("big"));
    QDir().mkpath(big);
    const int kRows = 20000;
    for (int i = 0; i < kRows; ++i)
        QDir().mkdir(QDir(big).filePath(QStringLiteral("d%1").arg(i, 5, 10, QLatin1Char('0'))));
    check(QDir(big).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() == kRows,
          QStringLiteral("⑨ 前置：造出 %1 个子目录（= %1 行）").arg(kRows));

    LibraryCore core(&store);
    check(core.addCollection(big, Sequence::Kind::Folder, &err), QStringLiteral("⑨ addCollection"));
    const qint64 cid = core.collections().isEmpty() ? -1 : core.collections().first().id;
    check(cid > 0, QStringLiteral("⑨ 图集 id=%1").arg(cid));

    check(core.requestReset(cid, LibraryCore::ResetOptions(), &err),
          QStringLiteral("⑨ 请求重置"));
    Sequence *rs = core.resetSequence(cid);
    check(rs != nullptr, QStringLiteral("⑨ 活序列在"));
    // 扫描完成（**不跑事件循环** ⇒ 唤醒钩子排的 pump 不会替我们提前起落库）
    const bool scanned = waitNoEvents([&] { return rs->scanFinished(); }, 60000);
    check(scanned, QStringLiteral("⑨ 生产者已写终态（扫描跑完）"));

    const QStringList connsBefore = libraryConnections();
    check(!connsBefore.isEmpty(),
          QStringLiteral("⑨ 前置：GUI 线程已有库连接（%1）").arg(connsBefore.join(QLatin1Char(','))));

    // ── ⑨a 起后台落库：那次 pump() 必须立刻返回，且状态仍 Running（锁继续 held）──
    const qint64 tp0 = monotonicNowMs();
    core.pump();
    const qint64 pumpMs = monotonicNowMs() - tp0;
    check(core.resetSequence(cid) != nullptr
              && core.resetSequence(cid)->rows().size() == kRows,
          QStringLiteral("⑨ §9.4 契约：drain 内观察到终态 ⇒ **最后一批已并入**，全量行齐（%1）")
              .arg(core.resetSequence(cid) ? core.resetSequence(cid)->rows().size() : -1));
    check(core.resetState(cid) == LibraryCore::ResetState::Running,
          QStringLiteral("⑨ 起落库后**不跑事件循环** ⇒ 状态仍 Running（**不是同步落库**）"));
    check(QFile::exists(testLockPath(cid)),
          QStringLiteral("⑨ 落库期间**锁仍 held**（重置尚未结束，§9.1）"));
    check(pumpMs < 2000,
          QStringLiteral("⑨ 启动后台落库的那次 `pump()` 只花 %1 ms（%2 行的写库不在这里）")
              .arg(pumpMs).arg(kRows));

    // ★ 结构性证据：worker 在**另一个线程**里开了自己的库连接（`replaceItems` 就在那个线程）。
    //   ⚠️ 这一段也**不跑事件循环**（否则落库结果会被提前投递，"不同步"的反证就废了）。
    QString foreign;
    const bool sawForeign = waitNoEvents([&] {
        for (const QString &n : libraryConnections())
            if (!connsBefore.contains(n)) { foreign = n; return true; }
        return false;
    }, 5000);
    qInfo().noquote() << QStringLiteral(
        "[resettest] 实测：pump() 启动落库 %1 ms；worker 库连接 = %2")
        .arg(pumpMs).arg(foreign.isEmpty() ? QStringLiteral("(没抓到)") : foreign);
    check(sawForeign,
          QStringLiteral("⑨ `replaceItems` 在**另一个线程**（worker 的库连接 %1；GUI 的是 %2）")
              .arg(foreign.isEmpty() ? QStringLiteral("(没抓到)") : foreign)
              .arg(connsBefore.join(QLatin1Char(','))));

    // 不跑事件循环 500 ms：结果回不来 ⇒ 后台线程与 GUI 线程**解耦**（结果走 post）
    QThread::msleep(500);
    check(core.resetState(cid) == LibraryCore::ResetState::Running,
          QStringLiteral("⑨ 500 ms 不跑事件循环 ⇒ 结果送不回来（结果**跨线程 post** 回 GUI）"));

    const bool done = testsupport::waitFor([&] {
        core.pump();
        return core.resetState(cid) == LibraryCore::ResetState::Done;
    }, 60000);
    check(done, QStringLiteral("⑨ 结果 post 回来 ⇒ Done（状态 %1）")
                    .arg(resetq::resetStateName(core.resetState(cid))));
    check(store.items(cid).size() == kRows,
          QStringLiteral("⑨ 落库行数正确：%1（期望 %2；节点行也算 —— 夹具是 2 万个子目录）")
              .arg(store.items(cid).size()).arg(kRows));
    check(core.resetSequence(cid) == nullptr, QStringLiteral("⑨ 收尾释放了活序列"));
    const QString notice = core.takeNotice();
    check(notice.contains(QStringLiteral("已重置")),
          QStringLiteral("⑨ 成功提示：%1").arg(notice));

    // ── ⑨b 注入"worker 端库打不开"：Failed + 旧内容不变 + 锁释放 ──
    const QString album2 = tmp.filePath(QStringLiteral("album2"));
    QDir().mkpath(album2);
    for (int i = 0; i < 2; ++i) {
        QImage img(8, 12, QImage::Format_RGB32);
        img.fill(QColor(200, 60 + i * 40, 60));
        img.save(QDir(album2).filePath(QStringLiteral("q%1.png").arg(i)));
    }
    check(core.addCollection(album2, Sequence::Kind::Folder, &err), QStringLiteral("⑨b 加第二个图集"));
    qint64 cid2 = -1;
    for (const library::Collection &c : core.collections())
        if (c.rootPath.endsWith(QStringLiteral("album2"))) cid2 = c.id;
    check(cid2 > 0, QStringLiteral("⑨b 第二个图集 id=%1").arg(cid2));

    // "旧内容" = 手工落 2 行（**不经过重置** ⇒ 与本次重置必然不同）
    QVector<library::Item> oldRows;
    oldRows << mkRow(0, QStringLiteral("old0")) << mkRow(1, QStringLiteral("old1"));
    check(store.replaceItems(cid2, oldRows) && store.itemCount(cid2) == 2,
          QStringLiteral("⑨b 旧内容已到位（2 行；replaceItems）"));

    check(core.requestReset(cid2, LibraryCore::ResetOptions(), &err),
          QStringLiteral("⑨b 请求重置第二个"));
    Sequence *rs2 = core.resetSequence(cid2);
    check(rs2 && waitNoEvents([&] { return rs2->scanFinished(); }, 15000),
          QStringLiteral("⑨b 第二个生产者已写终态（扫描跑完）"));

    // 注入：库路径指向"父级是普通文件"的位置 ⇒ worker 里 `Store::open` 必然失败
    const QString brokenParent = tmp.filePath(QStringLiteral("broken"));
    {
        QFile f(brokenParent);
        check(f.open(QIODevice::WriteOnly) && f.write("not a dir") > 0,
              QStringLiteral("⑨b 前置：造一个**普通文件**当「目录」"));
    }
    const QString brokenDb = brokenParent + QStringLiteral("/sub/library.db");
    const QByteArray goodDb = qgetenv("PHOTOSET_LIBRARY_DB");
    qputenv("PHOTOSET_LIBRARY_DB", brokenDb.toLocal8Bit());
    {
        // 自证注入是真的：同一份 `Store::open` 代码在这个路径上必失败。
        //   ⚠️ 必须在**另一个线程**里试：QtSql 的连接名按线程取（同一线程里会**复用**现有连接，
        //      那样 probe 会把真库的连接改名/关掉 —— 实测踩过）。
        std::atomic<bool> opened{false};
        QString perr;
        std::thread probe([&] {
            library::Store s;
            opened.store(s.open(&perr));
        });
        probe.join();
        check(!opened.load(),
              QStringLiteral("⑨b 前置：注入的库路径**必然打不开**（%1）").arg(perr));
    }

    core.pump();                                    // 观察到终态 ⇒ 起后台落库（worker 会失败）
    const bool failed = testsupport::waitFor([&] {
        core.pump();
        return core.resetState(cid2) != LibraryCore::ResetState::Running;
    }, 30000);
    qputenv("PHOTOSET_LIBRARY_DB", goodDb);         // 立刻恢复：后面还要用真库
    check(failed && core.resetState(cid2) == LibraryCore::ResetState::Failed,
          QStringLiteral("⑨b worker 打不开库 ⇒ **Failed**（状态 %1）")
              .arg(resetq::resetStateName(core.resetState(cid2))));
    check(core.resetReason(cid2).contains(QStringLiteral("库打不开")),
          QStringLiteral("⑨b 失败原因来自 worker 的落库：%1").arg(core.resetReason(cid2)));
    {
        const QVector<library::Item> after = store.items(cid2);
        check(after.size() == 2 && after.first().label == QLatin1String("old0"),
              QStringLiteral("⑨b **旧内容原样保留**（%1 行；没被替换成新扫描结果）").arg(after.size()));
    }
    check(!QFile::exists(testLockPath(cid2)),
          QStringLiteral("⑨b 失败收尾也**释放锁**（锁文件已删）"));
    check(core.resetSequence(cid2) == nullptr, QStringLiteral("⑨b 活序列已释放"));
    check(core.libraryUsable(), QStringLiteral("⑨b 库仍可用（失败没连累库）"));
    // 恢复之后：同一图集可以重跑，并且**这次真的落库**
    check(core.requestReset(cid2, LibraryCore::ResetOptions(), &err),
          QStringLiteral("⑨b 注入恢复后可以重跑"));
    const bool done2 = testsupport::waitFor([&] {
        core.pump();
        return core.resetState(cid2) == LibraryCore::ResetState::Done;
    }, 30000);
    check(done2 && store.itemCount(cid2) == 2,
          QStringLiteral("⑨b 重跑跑完 ⇒ Done + 落库 2 行（itemCount=%1）").arg(store.itemCount(cid2)));
    check(!QFile::exists(testLockPath(cid2)),
          QStringLiteral("⑨b 重跑也释放锁"));
}

// ══ ⑩ 落库窗口：取消静默 no-op + `PHOTOSET_COMMIT_DELAY_MS`（T2c-2b-2）════════
//   测什么：① 窗口通常 <1 ms ⇒ 注入延迟让窗口**确定性命中**（验收可复现）；
//           ② 窗口内 `cancelReset` = **静默 no-op**（返回 false、不 pushNotice、不打断落库）；
//           ③ 窗口结束后库内容 = **新扫描结果**（那次"取消"没有撤销落库）。
void groupCommitWindow(Check &check)
{
    QTemporaryDir tmp;
    useLibraryDbIn(tmp.path());
    library::Store store;
    QString err;
    if (!store.open(&err)) {
        check(false, QStringLiteral("⑩ 打开临时库（%1）").arg(err));
        return;
    }
    const QString album = tmp.filePath(QStringLiteral("album"));
    QDir().mkpath(album);
    for (int i = 0; i < 3; ++i) {
        QImage img(8, 12, QImage::Format_RGB32);
        img.fill(QColor(40 + i * 20, 80, 120));
        img.save(QDir(album).filePath(QStringLiteral("p%1.png").arg(i)));
    }
    LibraryCore core(&store);
    check(core.addCollection(album, Sequence::Kind::Folder, &err),
          QStringLiteral("⑩ addCollection（err='%1'）").arg(err));
    const qint64 cid = core.collections().isEmpty() ? -1 : core.collections().first().id;
    check(cid > 0, QStringLiteral("⑩ 图集 id=%1").arg(cid));

    QVector<library::Item> oldRows;
    oldRows << mkRow(0, QStringLiteral("old0")) << mkRow(1, QStringLiteral("old1"));
    check(store.replaceItems(cid, oldRows) && store.itemCount(cid) == 2,
          QStringLiteral("⑩ 旧内容已到位（2 行）"));

    // ★ 只为**本组**注入落库延迟（默认 0 ⇒ 生产零影响）
    qputenv("PHOTOSET_COMMIT_DELAY_MS", "1500");
    check(core.requestReset(cid, LibraryCore::ResetOptions(), &err),
          QStringLiteral("⑩ 请求重置（err='%1'）").arg(err));

    // 窗口起：**不跑事件循环**（结果不会提前 post 回来）——只手动 pump
    const qint64 t0 = monotonicNowMs();
    const bool committing = waitNoEvents([&] {
        core.pump();
        return core.resetCommitting(cid);
    }, 20000);
    check(committing, QStringLiteral("⑩ **落库窗口打开**（resetCommitting=true）"));
    check(core.resetState(cid) == LibraryCore::ResetState::Running,
          QStringLiteral("⑩ 窗口内队列仍 Running（锁继续 held）"));
    check(QFile::exists(testLockPath(cid)),
          QStringLiteral("⑩ 落库期间锁仍 held（重置尚未结束）"));

    // ── 落库窗口内取消：**静默 no-op** ──
    check(core.takeNotice().isEmpty(), QStringLiteral("⑩ 前置：窗口内还没有任何提示"));
    check(!core.cancelReset(cid),
          QStringLiteral("⑩ **落库窗口内 cancelReset 返回 false**（骗人的 true 没了）"));
    check(core.takeNotice().isEmpty(),
          QStringLiteral("⑩ **静默 no-op**：不 pushNotice（提示队列仍空）"));
    check(core.resetState(cid) == LibraryCore::ResetState::Running && core.resetCommitting(cid),
          QStringLiteral("⑩ 那次取消没有打断落库（仍 Running + 窗口仍开）"));

    // 窗口结束 ⇒ Done + 库 = 新扫描结果
    const bool done = testsupport::waitFor([&] {
        core.pump();
        return core.resetState(cid) == LibraryCore::ResetState::Done;
    }, 20000);
    const qint64 windowMs = monotonicNowMs() - t0;
    check(done, QStringLiteral("⑩ 落库结束 ⇒ Done（状态 %1）")
                    .arg(resetq::resetStateName(core.resetState(cid))));
    check(windowMs >= 1000,
          QStringLiteral("⑩ `PHOTOSET_COMMIT_DELAY_MS` 生效：窗口实测 %1 ms（注入 1500）")
              .arg(windowMs));
    check(!core.resetCommitting(cid), QStringLiteral("⑩ 窗口关闭（resetCommitting=false）"));
    {
        const QVector<library::Item> after = store.items(cid);
        bool noOld = true;
        int imgs = 0;
        for (const library::Item &r : after) {
            if (r.label.startsWith(QLatin1String("old"))) noOld = false;
            if (r.kind == Sequence::Kind::Image) ++imgs;
        }
        check(noOld && imgs == 3,
              QStringLiteral("⑩ 库内容 = **新扫描结果**（旧行不残留；%1 张）").arg(imgs));
    }
    qunsetenv("PHOTOSET_COMMIT_DELAY_MS");
}

} // namespace

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, [] {
        // 库路径只认 `PHOTOSET_LIBRARY_DB`（别的套件同款）；把可能盖过它的变量清掉
        qunsetenv("PHOTOSET_CONFIG_DIR");

        testsupport::Check check(QStringLiteral("resettest"));
        groupReplaceAtomic(check);
        groupSerial(check);
        groupLock(check);
        groupEndings(check);
        groupVolumeGate(check);
        groupCoreEndToEnd(check);
        groupPublishHook(check);
        groupBoundedClose(check);
        groupBackgroundCommit(check);
        groupCommitWindow(check);
        return check.finish();
    });
}
