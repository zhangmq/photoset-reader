#include "testsupport.h"

#include <QAbstractItemModel>
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringLiteral>
#include <QThread>
#include <QVector>

#include <sys/stat.h>
#include <sys/types.h>

#include "librarycore.h"
#include "librarymodel.h"
#include "librarystore.h"
#include "resetqueue.h"
#include "sequence.h"
#include "uimodel.h"
#include "viewermodel.h"

// ══ `UiModel` ↔ `LibraryCore` 接线自检（ib-test-uicore；T2c-2b-1，设计 14 §9.1/§9.3）══
//   逐条验本批的交付物（父任务书 ①…⑤）：
//     ① **core 接入**：`UiModel` 持 core；`libraryUsable()` 反映 core 的闸门；
//        `openCollection(id)` 之后 `viewSequence()` 是**从磁盘读出的快照**（行数 / 图数与库里一致）。
//     ② **重置走 core**：`requestReset` ⇒ 状态 Queued/Running（**内存态，不落库**）；
//        pump 到完成 ⇒ 库里行被**原子替换**（旧内容不残留 + 新内容正确）+ 状态 Done。
//     ③ **变化驱动**：同一轮连续 pump 两次、没有任何新行 ⇒ **不得**发 `dataChanged` /
//        不得重建视图（用**可观测计数**断言，不靠时间）。
//     ④ **会话生命周期**：`openSession` 后可浏览（`sessionSequence()`）；`closeSession()`
//        后槽位释放；活序列 ≤2；`View == Reset` 是**同一个对象**（别名）。
//     ⑤ **张数口径**：扫描中 `discoveredCount() >= count()`（差 1 的瞬间确实存在）、
//        收尾后相等；`resetProgress` 报的是 `discoveredCount()`。
//     ⑥ **1 s 兜底 tick**（父任务书交付物②的第二条驱动）：生产者**完全不推进**（FIFO 卡死）
//        ⇒ 没有任何唤醒；只有 core 那条既有的 1 s `data_version` 定时器能推进 ⇒
//        判定卡住的条件 / 有界收尾照跑（**不手动 pump** 就能收尾）。
//
//   规则：只碰临时库 / 临时目录（`PHOTOSET_LIBRARY_DB` / `PHOTOSET_SETTINGS_FILE`）。

namespace {

using testsupport::Check;

// 造 `n` 张**窄图**（宽/高 < 1 ⇒ 走"从头锚定 + 1 项前瞻"的分组路径）
QString makeAlbum(const QString &parent, const QString &name, int n)
{
    const QString dir = QDir(parent).filePath(name);
    QDir().mkpath(dir);
    for (int i = 0; i < n; ++i) {
        QImage img(8, 12, QImage::Format_RGB32);
        img.fill(QColor(40 + (i % 200), 80, 120));
        img.save(QDir(dir).filePath(QStringLiteral("p%1.png").arg(i)));
    }
    return dir;
}

library::Item mkRow(int ord, const QString &label)
{
    library::Item it;
    it.ord = ord;
    it.parentOrd = -1;
    it.kind = QStringLiteral("image");
    it.label = label;
    it.size = 100 + ord;
    it.mtime = 1000 + ord;
    it.width = 8;
    it.height = 12;
    return it;
}

// 活序列条数（**去重**：`View == Reset` 时 `viewSequence()` 就是那条活序列 ⇒ 只算 1 条）
int liveSequenceCount(LibraryCore *core, qint64 cid)
{
    QSet<const void *> s;
    if (Sequence *a = core->sessionSequence()) s.insert(a);
    if (Sequence *b = core->viewSequence())    s.insert(b);
    if (Sequence *c = core->resetSequence(cid)) s.insert(c);
    return s.size();
}

qint64 idOf(LibraryCore *core, const QString &root)
{
    for (const library::Collection &c : core->collections())
        if (c.rootPath == root) return c.id;
    return -1;
}

} // namespace

static int runUiCoreSelfTest()
{
    testsupport::TempFixture fx;
    if (!fx.isValid())
        return testsupport::skip(QStringLiteral("uicoretest"), QStringLiteral("临时目录建不出来"));
    fx.useLibraryDb();
    fx.useSettingsFile();

    Check check(QStringLiteral("uicoretest"));

    library::Store store;
    QString err;
    check(store.open(&err), QStringLiteral("打开临时库（%1）")
                                .arg(err.isEmpty() ? library::Store::databasePath() : err));

    // ★ 被验对象：**应用唯一的全局 VM**（构造时自己建 core）
    app::UiModel ui(&store);
    LibraryCore *core = ui.core();
    check(core != nullptr, QStringLiteral("① UiModel 持有 LibraryCore"));
    check(core && core->libraryUsable(),
          QStringLiteral("① libraryUsable() 反映 core 的闸门（临时库在本地固定卷上）"));
    // QML 侧改指的入口必须真的在元对象上（QML 调错名字只在**点击那一刻**失败，加载不报错）
    {
        const QStringList needed = { QStringLiteral("requestReset(qlonglong)"),
                                     QStringLiteral("cancelReset(qlonglong)") };
        QStringList missing;
        for (const QString &sig : needed)
            if (ui.metaObject()->indexOfMethod(sig.toUtf8().constData()) < 0) missing << sig;
        check(missing.isEmpty(),
              QStringLiteral("① QML 要调的 %1 个新入口都在 ui 上（缺：%2）")
                  .arg(needed.size())
                  .arg(missing.isEmpty() ? QStringLiteral("无") : missing.join(QLatin1Char(','))));
    }

    auto rowOf = [&](qint64 id) -> int {
        for (int i = 0; i < ui.library()->rowCount(); ++i)
            if (ui.library()->data(ui.library()->index(i, 0), LibraryModel::IdRole).toLongLong() == id)
                return i;
        return -1;
    };

    // ══ ① core 接入 + 打开图集 = 从磁盘读出的快照 ═══════════════════════════════════
    const QString album1 = makeAlbum(fx.path(), QStringLiteral("a1"), 5);
    check(core->addCollection(album1, QStringLiteral("folder"), &err),
          QStringLiteral("① core 加入图集（err='%1'）").arg(err));
    const qint64 cid1 = idOf(core, album1);
    check(cid1 > 0, QStringLiteral("① 图集 id = %1").arg(cid1));
    check(ui.library()->rowCount() == core->collections().size(),
          QStringLiteral("① 卡片模型与 core 的集合列表同数（%1）").arg(ui.library()->rowCount()));

    check(core->requestReset(cid1, LibraryCore::ResetOptions(), &err),
          QStringLiteral("① 先重置一次把行写入磁盘（err='%1'）").arg(err));
    check(testsupport::waitFor([&] {
              core->pump();
              return core->resetState(cid1) == LibraryCore::ResetState::Done;
          }, 20000), QStringLiteral("① 重置到 Done"));
    check(store.itemCount(cid1) == 5,
          QStringLiteral("① 库里 5 张（实际 %1）").arg(store.itemCount(cid1)));

    check(ui.openCollection(cid1), QStringLiteral("① ui.openCollection(%1)（走 core 快照）").arg(cid1));
    Sequence *vs = core->viewSequence();
    check(vs != nullptr, QStringLiteral("① viewSequence() 非空（core 的查看槽）"));
    if (vs) {
        check(vs->rows().size() == store.items(cid1).size(),
              QStringLiteral("① 快照**行数 = 库行数**（%1 / %2）")
                  .arg(vs->rows().size()).arg(store.items(cid1).size()));
        check(vs->count() == store.itemCount(cid1),
              QStringLiteral("① 快照图数 = 库图数（%1 / %2）")
                  .arg(vs->count()).arg(store.itemCount(cid1)));
    }

    // ★ T2d-1b：`ViewerModel` 不再有 `refresh()`（**结构性消失**，不是行为变了）；
    //   视图刷新入口已随"刷新"整体删除 —— 刷新是重操作，不再提供。
    if (Sequence *vs1 = core->viewSequence()) {
        const int n1 = vs1->count();
        check(ui.viewerCore()->metaObject()->indexOfMethod("refresh()") < 0,
              QStringLiteral("① viewer 上**没有重扫入口**（`refresh()` 已删：引擎不碰序列）"));
        check(vs1->state() == Sequence::State::Scaned && vs1->count() == n1,
              QStringLiteral("① 外部序列：没人能`重扫` core 的快照"
                             "（仍 Scaned / %1 条）").arg(n1));
    }

    // ══ ② 重置走 core：Queued/Running（内存态）⇒ 完成 = 原子替换 ═════════════
    const QString album2 = makeAlbum(fx.path(), QStringLiteral("a2"), 4);
    check(core->addCollection(album2, QStringLiteral("folder"), &err), QStringLiteral("② 加入第二个图集"));
    const qint64 cid2 = idOf(core, album2);
    QVector<library::Item> oldRows;
    for (int i = 0; i < 3; ++i) oldRows << mkRow(i, QStringLiteral("old%1").arg(i));
    // ★ T2c-2b-2：夹具写入改走**唯一写入路径** `replaceItems`（`upsertItems` 已删）
    check(store.replaceItems(cid2, oldRows), QStringLiteral("② 先落 3 行旧内容（replaceItems）"));

    int liveUpdates = 0;
    QObject::connect(ui.library(), &QAbstractItemModel::dataChanged,
                     [&liveUpdates] { ++liveUpdates; });

    check(core->requestReset(cid2, LibraryCore::ResetOptions(), &err),
          QStringLiteral("② requestReset 入队（err='%1'）").arg(err));
    const LibraryCore::ResetState st2 = core->resetState(cid2);
    check(st2 == LibraryCore::ResetState::Queued || st2 == LibraryCore::ResetState::Running,
          QStringLiteral("② 状态 = Queued/Running（内存态，实际 %1）").arg(resetq::resetStateName(st2)));
    // ★ T2c-2b-2：不再有 `scan_state` 列；"进行中不落库"的判定条件改用 **库行内容**：
    //   扫描期间库里还是上一轮的 3 行旧内容（新结果尚未替换）
    check(store.items(cid2).size() == 3,
          QStringLiteral("② **扫描期间不落库**：库里仍是上一轮的 %1 行旧内容")
              .arg(store.items(cid2).size()));
    // ★ 卡片状态/计数 = core 的内存态（**不是**库里的列）：库里是空串，卡片显示"扫描中"
    if (const int r = rowOf(cid2); r >= 0) {
        const QString ss = ui.library()->data(ui.library()->index(r, 0),
                                              LibraryModel::ScanStateRole).toString();
        check(ss == QLatin1String("NotReady") || ss == QLatin1String("ImageFound"),
              QStringLiteral("② 卡片 scanState 从 core 映射（'%1' ⇒ 扫描中），不是库里的空串").arg(ss));
    } else {
        check(false, QStringLiteral("② 卡片里找不到集合 %1").arg(cid2));
    }

    const bool done2 = testsupport::waitFor([&] {
        core->pump();
        return core->resetState(cid2) == LibraryCore::ResetState::Done;
    }, 20000);
    check(done2, QStringLiteral("② 重置到 Done（状态 %1）")
                     .arg(resetq::resetStateName(core->resetState(cid2))));
    {
        const QVector<library::Item> after = store.items(cid2);
        bool noOld = true;
        int imgs = 0;
        for (const library::Item &r : after) {
            if (r.label.startsWith(QLatin1String("old"))) noOld = false;
            if (r.kind == QLatin1String("image")) ++imgs;
        }
        check(noOld && imgs == 4,
              QStringLiteral("② 库里行被**原子替换**（旧内容不残留；%1 行 / %2 张）")
                  .arg(after.size()).arg(imgs));
    }
    check(core->resetSequence(cid2) == nullptr, QStringLiteral("② 收尾释放活序列"));
    check(liveUpdates > 0, QStringLiteral("② 有变化时确实刷了卡片（dataChanged %1 次）").arg(liveUpdates));
    if (const int r = rowOf(cid2); r >= 0) {
        check(ui.library()->data(ui.library()->index(r, 0), LibraryModel::ScanStateRole).toString().isEmpty(),
              QStringLiteral("② Done ⇒ 卡片回到正常态（scanState 空串）"));
        check(ui.library()->data(ui.library()->index(r, 0), LibraryModel::ScannedItemsRole).toInt() == 4,
              QStringLiteral("② 卡片计数 = core 的已到达张数（4）"));
    }

    // ══ ③ 变化驱动：数据没变 ⇒ 不刷（可观测计数）═══════════════════════════
    int viewerChanged = 0;
    QObject::connect(&ui, &app::UiModel::viewerChanged, [&viewerChanged] { ++viewerChanged; });
    const int dcBefore = liveUpdates;
    const int vcBefore = viewerChanged;
    const LibraryCore::Pumped p1 = core->pump();
    const LibraryCore::Pumped p2 = core->pump();       // 同一轮内连续第二次
    check(!p1.sessionChanged && !p1.viewChanged && !p1.resetChanged,
          QStringLiteral("③ 稳定后 pump 无变化（session/view/reset 全 false）"));
    check(!p2.sessionChanged && !p2.viewChanged && !p2.resetChanged,
          QStringLiteral("③ 同一轮连续第二次 pump 仍无变化"));
    check(liveUpdates == dcBefore,
          QStringLiteral("③ 数据没变 ⇒ **不发 dataChanged**（%1 → %2）").arg(dcBefore).arg(liveUpdates));
    check(viewerChanged == vcBefore,
          QStringLiteral("③ 数据没变 ⇒ **不重建视图**（viewerChanged %1 → %2）")
              .arg(vcBefore).arg(viewerChanged));

    // ══ ④ 会话生命周期 + View==Reset 别名 ═══════════════════════════════════
    //   先回库屏释放显示槽（**core 的槽位归 UiModel 管**：直接动 core 前必须让 viewer 先死）
    ui.goLibrary();
    check(ui.viewerCore() == nullptr, QStringLiteral("④ 回库屏 ⇒ 显示槽已释放（viewer == null）"));

    const QString album3 = makeAlbum(fx.path(), QStringLiteral("a3"), 3);
    check(core->openSession(album3, LibraryCore::ResetOptions(), &err),
          QStringLiteral("④ openSession（err='%1'）").arg(err));
    Sequence *sess = core->sessionSequence();
    check(sess != nullptr, QStringLiteral("④ 会话序列在（sessionSequence()）"));
    check(liveSequenceCount(core, cid2) <= 2, QStringLiteral("④ 活序列 ≤2"));
    const bool sessDone = testsupport::waitFor([&] {
        core->pump();
        Sequence *s = core->sessionSequence();
        return s && s->scanFinished();
    }, 10000);
    sess = core->sessionSequence();
    check(sessDone && sess && sess->count() == 3,
          QStringLiteral("④ 会话扫完且**可浏览**（%1 条）").arg(sess ? sess->count() : -1));
    check(sess && !sess->tree().isEmpty(), QStringLiteral("④ 会话有文件树（浏览层级）"));
    core->closeSession();
    check(core->sessionSequence() == nullptr, QStringLiteral("④ closeSession ⇒ 会话槽释放"));
    check(liveSequenceCount(core, cid2) <= 2, QStringLiteral("④ 释放后活序列 ≤2"));

    const QString album4 = makeAlbum(fx.path(), QStringLiteral("a4"), 3);
    check(core->addCollection(album4, QStringLiteral("folder"), &err), QStringLiteral("④ 加入 a4"));
    const qint64 cid4 = idOf(core, album4);
    check(core->requestReset(cid4, LibraryCore::ResetOptions(), &err), QStringLiteral("④ a4 入队重置"));
    check(testsupport::waitFor([&] {
              core->pump();
              return core->resetState(cid4) == LibraryCore::ResetState::Running;
          }, 5000), QStringLiteral("④ a4 进入 Running"));
    check(core->openCollection(cid4, &err), QStringLiteral("④ 打开正在重置的图集"));
    check(core->viewSequence() != nullptr
              && core->viewSequence() == core->resetSequence(cid4),
          QStringLiteral("④ **View == Reset**：`viewSequence()` 就是那条活序列（同一个对象）"));
    check(liveSequenceCount(core, cid4) == 1, QStringLiteral("④ 别名时活序列只算 1 条"));
    core->closeCollectionView();
    core->cancelReset(cid4);
    testsupport::waitFor([&] {
        core->pump();
        const LibraryCore::ResetState s = core->resetState(cid4);
        return s == LibraryCore::ResetState::Stopped || s == LibraryCore::ResetState::Done;
    }, 10000);

    // ══ ⑤ 张数口径：discoveredCount() vs count() ════════════════════════════
    //   夹具设计（**让"差 1"可确定性复现**，不靠时间）：
    //     · 生产者的批大小 = 16 **行**，而"行"里包含**目录节点**（不进条目表）；
    //     · 根下先来一个子目录 ⇒ 第一批 = 1 个节点 + **15 张图** ⇒ `appendItem` 只调 15 次
    //       （**奇数**）⇒ 该批 drain 之后 `m_pending` 还挂着最后一条
    //       ⇒ `count() = discoveredCount() - 1`（这就是"悬着最后一条"）；
    //     · 子目录**够大**（各 600 张）⇒ 第一批 drain 时生产者离终态还远（终态才 `finishAppend`）。
    const QString album5 = makeAlbum(fx.path(), QStringLiteral("a5"), 0);
    makeAlbum(album5, QStringLiteral("d1"), 600);
    makeAlbum(album5, QStringLiteral("d2"), 600);
    check(core->addCollection(album5, QStringLiteral("folder"), &err),
          QStringLiteral("⑤ 加入 a5（2 个子目录 × 600 张）"));
    const qint64 cid5 = idOf(core, album5);

    int  maxGap = 0;
    bool progressBeyondDecided = false;
    bool finalEqual = false;
    int  lastReported = -1;
    const QMetaObject::Connection conn = QObject::connect(
        core, &LibraryCore::resetProgress, [&](qint64 id, int n) {
            if (id != cid5) return;
            lastReported = n;
            Sequence *rs = core->resetSequence(cid5);
            if (!rs) return;
            if (rs->discoveredCount() > rs->count())
                maxGap = qMax(maxGap, rs->discoveredCount() - rs->count());
            if (n > rs->count()) progressBeyondDecided = true;      // 报的**不是** count()
            if (rs->scanFinished()) finalEqual = (rs->discoveredCount() == rs->count());
        });

    check(core->requestReset(cid5, LibraryCore::ResetOptions(), &err),
          QStringLiteral("⑤ requestReset（1200 张）"));
    QElapsedTimer clock5;
    clock5.start();
    bool done5 = false;
    while (!done5 && clock5.elapsed() < 60000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 0);   // 只跑**已排队**的事件
        core->pump();                                                // 直接推（不等定时器/唤醒）
        Sequence *rs = core->resetSequence(cid5);
        if (rs && rs->discoveredCount() > rs->count())
            maxGap = qMax(maxGap, rs->discoveredCount() - rs->count());
        done5 = (core->resetState(cid5) == LibraryCore::ResetState::Done);
        if (!done5) QThread::yieldCurrentThread();
    }
    QObject::disconnect(conn);
    check(done5, QStringLiteral("⑤ 重置到 Done（状态 %1，%2 ms）")
                     .arg(resetq::resetStateName(core->resetState(cid5))).arg(clock5.elapsed()));
    check(maxGap >= 1,
          QStringLiteral("⑤ 扫描中 `discoveredCount() > count()`（**差 %1** 的瞬间确实存在）").arg(maxGap));
    check(progressBeyondDecided,
          QStringLiteral("⑤ `resetProgress` 报的是 **discoveredCount()**（有事件 n > count()）"));
    check(lastReported == 1200, QStringLiteral("⑤ 最终报数 = 1200（实际 %1）").arg(lastReported));
    check(finalEqual, QStringLiteral("⑤ 收尾后 `discoveredCount() == count()`"));
    check(store.itemCount(cid5) == 1200, QStringLiteral("⑤ 库里 1200 张"));

    // ══ ⑥ 1 s 兜底 tick：生产者完全不推进也能收尾 ═══════════════════════════
    qputenv("PHOTOSET_RESET_STALL_MS", "300");      // 只为本组调小（默认 30000 不动）
    const QString fifo = fx.filePath(QStringLiteral("stuck.fifo"));
    check(::mkfifo(fifo.toLocal8Bit().constData(), 0600) == 0,
          QStringLiteral("⑥ 前置：造 FIFO 当来源（生产者会阻塞在不可中断的 open 上）"));
    check(core->addCollection(fifo, QStringLiteral("folder"), &err), QStringLiteral("⑥ 加入 FIFO 图集"));
    const qint64 cid6 = idOf(core, fifo);
    check(core->requestReset(cid6, LibraryCore::ResetOptions(), &err), QStringLiteral("⑥ 入队重置"));
    // ★ **不手动 pump**：卡住的生产者不再发布 ⇒ 没有唤醒钩子；只有 core 那条既有的
    //   1 s `data_version` 定时器会顺带 pump ⇒ 判定卡住的条件 + 有界收尾照跑。
    QElapsedTimer clock;
    clock.start();
    const bool stalled = testsupport::waitFor(
        [&] { return core->resetState(cid6) == LibraryCore::ResetState::Failed; }, 8000);
    check(stalled, QStringLiteral("⑥ 1 s 兜底 tick 把卡住的重置收尾（%1 ms）").arg(clock.elapsed()));
    check(core->resetReason(cid6).contains(QStringLiteral("卡住")),
          QStringLiteral("⑥ 原因 = 卡住（'%1'）").arg(core->resetReason(cid6)));
    check(store.items(cid6).isEmpty(),
          QStringLiteral("⑥ 卡住**不落库**（库里 0 行；FIFO 图集从没被替换过）"));
    qunsetenv("PHOTOSET_RESET_STALL_MS");

    // ══ ⑦ 落库窗口：取消静默 no-op + `resetCommitting` 变化驱动（T2c-2b-2）════════
    //   窗口通常 <1 ms ⇒ 用 `PHOTOSET_COMMIT_DELAY_MS` 注入延迟（验收用的可观测性，
    //   默认 0 ⇒ 生产零影响），让窗口**确定性命中**。
    {
        const QString album7 = makeAlbum(fx.path(), QStringLiteral("a7"), 3);
        check(core->addCollection(album7, QStringLiteral("folder"), &err),
              QStringLiteral("⑦ 加入 a7（3 张）"));
        const qint64 cid7 = idOf(core, album7);
        check(cid7 > 0, QStringLiteral("⑦ a7 id = %1").arg(cid7));

        QVector<library::Item> old7;
        for (int i = 0; i < 2; ++i) old7 << mkRow(i, QStringLiteral("old7_%1").arg(i));
        check(store.replaceItems(cid7, old7), QStringLiteral("⑦ 先落 2 行旧内容"));

        qputenv("PHOTOSET_COMMIT_DELAY_MS", "4000");   // 只为本组（默认 0 不动）

        const int r7 = rowOf(cid7);
        auto role7 = [&](int role) -> QVariant {
            if (r7 < 0) return {};
            return ui.library()->data(ui.library()->index(r7, 0), role);
        };

        check(core->requestReset(cid7, LibraryCore::ResetOptions(), &err),
              QStringLiteral("⑦ 入队重置（err='%1'）").arg(err));
        // 扫描完成 ⇒ core 的 pump 在 drain 内观察到终态 ⇒ 起后台落库（窗口打开）
        const bool committing = testsupport::waitFor([&] {
            core->pump();
            return core->resetCommitting(cid7);
        }, 20000);
        check(committing, QStringLiteral("⑦ **落库窗口打开**（resetCommitting=true）"));
        check(core->resetState(cid7) == LibraryCore::ResetState::Running,
              QStringLiteral("⑦ 窗口内队列仍是 Running（锁继续 held）"));

        // 卡片角色：扫描中 + 落库窗口 ⇒ 取消按钮禁用（QML 的两个输入都齐）
        check(role7(LibraryModel::ResetCommittingRole).toBool(),
              QStringLiteral("⑦ 卡片 `resetCommitting` role = true（界面据此禁用「取消扫描」）"));
        const QString ss7 = role7(LibraryModel::ScanStateRole).toString();
        check(ss7 == QLatin1String("ImageFound") || ss7 == QLatin1String("NotReady"),
              QStringLiteral("⑦ 卡片仍显示「扫描中」（scanState='%1'）").arg(ss7));

        // ── ⑦a 落库窗口内取消：**静默 no-op**（返回 false + 没有任何提示）──
        const int seqBefore = ui.notificationSeq();
        check(!core->cancelReset(cid7),
              QStringLiteral("⑦a 落库窗口内 `cancelReset` 返回 **false**（不骗人）"));
        check(ui.notificationSeq() == seqBefore,
              QStringLiteral("⑦a **静默**：没有新提示（%1 → %2）")
                  .arg(seqBefore).arg(ui.notificationSeq()));
        check(core->takeNotice().isEmpty(),
              QStringLiteral("⑦a core 的提示队列也是空的（不 pushNotice）"));
        check(core->resetState(cid7) == LibraryCore::ResetState::Running
                  && core->resetCommitting(cid7),
              QStringLiteral("⑦a 取消没有打断落库（仍 Running + 窗口仍开）"));

        // ── ⑦b `resetCommitting` 变化驱动（值真变才刷；值没变**不许**刷）──
        // 值没变 ⇒ 不得刷卡片：窗口开着时再发一次 true（信号可调：Qt 的 signals 是 public）
        const int dcBefore = liveUpdates;
        core->resetCommittingChanged(cid7, true);
        check(liveUpdates == dcBefore,
              QStringLiteral("⑦b 值没变（再发一次 true）⇒ **不刷卡片**（dataChanged %1 → %2）")
                  .arg(dcBefore).arg(liveUpdates));

        // 窗口关闭 ⇒ role 回 false；再发一次 false 也不刷
        const bool done7 = testsupport::waitFor([&] {
            core->pump();
            return core->resetState(cid7) == LibraryCore::ResetState::Done;
        }, 20000);
        check(done7, QStringLiteral("⑦b 窗口结束 ⇒ Done"));
        check(!core->resetCommitting(cid7) && !role7(LibraryModel::ResetCommittingRole).toBool(),
              QStringLiteral("⑦b 卡片 `resetCommitting` 回 false（取消按钮恢复）"));
        const int dcAfter = liveUpdates;
        core->resetCommittingChanged(cid7, false);
        check(liveUpdates == dcAfter,
              QStringLiteral("⑦b 值没变（再发一次 false）⇒ 仍**不刷卡片**（%1 → %2）")
                  .arg(dcAfter).arg(liveUpdates));

        // ── ⑦c 库内容 = **新扫描结果**（窗口内那次"取消"没有撤销落库）──
        {
            const QVector<library::Item> after = store.items(cid7);
            bool noOld = true;
            int imgs = 0;
            for (const library::Item &r : after) {
                if (r.label.startsWith(QLatin1String("old7"))) noOld = false;
                if (r.kind == QLatin1String("image")) ++imgs;
            }
            check(noOld && imgs == 3,
                  QStringLiteral("⑦c 库内容 = **新扫描结果**（旧行不残留；%1 张）").arg(imgs));
        }
        qunsetenv("PHOTOSET_COMMIT_DELAY_MS");
    }

    return check.finish();
}

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, &runUiCoreSelfTest);
}
