#include "librarycore.h"

// 设计 14 §9.1（重置 = 起活序列；跑完 ⇒ 原子替换；停止/异常/卡住 ⇒ 不落库）
//        §9.2（两条序列 + 消费者只有一个 + 判定卡住的条件 + **发布钩子唤醒** + **有界收尾**）
//        §9.3（槽位 / pump / 数据出口）
//        §9.4（**判定结束的条件**的实现契约：必须在 drain 内观察终态）
//        §9.5（每图集锁 + 库目录闸门 + 1 s data_version）

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QThread>

#include <atomic>
#include <memory>

#include "volumeinfo.h"

// ══ ★★ 唤醒汇（发布钩子挂进来的地方；§9.2 的"事件驱动"靠它）══════════════════════════
//
//   为什么不让钩子直接捕获 `LibraryCore*` 再 `invokeMethod(this, ...)`：
//   序列可能被 **abandon()**（detach 生产者 + **故意泄漏**）⇒ 它的生产者线程在本类
//   析构之后**仍可能**调钩子 —— 那时 `this` 已经是野指针（UAF）。
//   这里让钩子**只捕获本汇的 `shared_ptr`**（不捕获 core）：
//     · 投递 = `invokeMethod(this /*汇*/, 排队)` ⇒ 生产者在**任意线程**调都安全、不阻塞；
//     · 处理函数在 `~LibraryCore` 里 `detach()` 置空 ⇒ 之后的投递是空操作；
//     · **不需要 moc**（functor 版 `invokeMethod`），汇也不是 core 的子对象（不被一起删）。
class LibraryCore::WakeSink : public QObject
{
public:
    // GUI 线程：装 / 摘处理函数（`~LibraryCore` 摘 ⇒ 不再打到已析构的 core 上）
    void setHandler(std::function<void()> h) { m_handler = std::move(h); }
    void detach() { m_handler = nullptr; }

    // **任意线程**（生产者）：投递一次唤醒 —— 只有"一次原子交换 + 一次 post"，**不阻塞**；
    //   已有一次待处理的唤醒时直接返回（这是**合并**的第一层，第二层在 `LibraryCore::onWake`）。
    void wake()
    {
        if (m_pending.exchange(true, std::memory_order_acq_rel)) return;
        // ⚠️ **先清标志、再跑处理函数**：处理期间新到的唤醒必须能再投一次，
        //   否则"最后一批 + 终态"的唤醒会被吞掉 ⇒ 消费者永远不 pump（丢唤醒）。
        QMetaObject::invokeMethod(this, [this] {
            m_pending.store(false, std::memory_order_relaxed);
            if (m_handler) m_handler();
        }, Qt::QueuedConnection);
    }

private:
    std::atomic<bool>     m_pending{false};
    std::function<void()> m_handler;
};

namespace {

// ★★ 看门狗 / 有界收尾的**唯一阈值来源**（§9.2）：30 s **没有进度更新** ⇒ 收尾 + 释放队首与锁。
//   · 累积型慢不受影响（`Sequence` 一直在 bumpProgress，见 sequence.h 的说明）；
//   · 阈值沿用旧的消费侧看门狗（那道看门狗随旧扫描路径在 T2c-2b-2 删除），本类
//     的 `pump()` 卡住分支与 `reapSlot()` 有界宽限**共用**这一个来源；
//   · 验收要把它临时调小时，用**环境变量覆写**（本仓库既有的测试覆盖风格）：
//     `PHOTOSET_RESET_STALL_MS`（默认值不动）。
constexpr qint64 kStallMs = 30000;

qint64 stallMs()
{
    const QByteArray v = qgetenv("PHOTOSET_RESET_STALL_MS");
    if (v.isEmpty()) return kStallMs;
    bool ok = false;
    const qint64 ms = v.toLongLong(&ok);
    return (ok && ms > 0) ? ms : kStallMs;
}

// ★★ **落库窗口的可观测性注入**（T2c-2b-2，验收用）：落库 worker 里、**事务开始之前**
//   睡一次。默认 0 ⇒ 生产零影响。
//   为什么需要它：落库窗口通常 < 1 ms（10 万行实测 90 ms，典型 < 1 ms）⇒ 不注入延迟就
//   **无法确定性验收**"窗口内取消 = 静默 no-op"与"卡片「取消扫描」禁用"。
//   仓库已有的同类先例：`PHOTOSET_RESET_STALL_MS`。
qint64 commitDelayMs()
{
    const QByteArray v = qgetenv("PHOTOSET_COMMIT_DELAY_MS");
    if (v.isEmpty()) return 0;
    bool ok = false;
    const qint64 ms = v.toLongLong(&ok);
    return (ok && ms > 0) ? ms : 0;
}

// ★ 判定卡住的条件的**唯一实现**（§9.2；看门狗与有界宽限共用）：最后一次进度距今 > `limitMs`。
//   `refMs` = 进度时间戳还没有时的参照时刻；`from <= 0`（未知）⇒ **不判卡**。
bool stalledSince(const Sequence &seq, qint64 refMs, qint64 limitMs)
{
    const qint64 last = seq.lastProgressMs();
    const qint64 from = last > 0 ? last : refMs;
    if (from <= 0) return false;
    return monotonicNowMs() - from > limitMs;
}

QString volumeKindName(volume::Kind k)
{
    switch (k) {
    case volume::Kind::LocalFixed: return QStringLiteral("LocalFixed");
    case volume::Kind::Removable:  return QStringLiteral("Removable");
    case volume::Kind::Network:    return QStringLiteral("Network");
    case volume::Kind::Unknown:    return QStringLiteral("Unknown");
    }
    return QStringLiteral("Unknown");
}

int imageCountOf(const QVector<Sequence::Row> &rows)
{
    int n = 0;
    for (const Sequence::Row &r : rows)
        if (r.kind == Sequence::Kind::Image) ++n;
    return n;
}

} // namespace

LibraryCore::LibraryCore(library::Store *store, QObject *parent)
    : QObject(parent), m_store(store)
{
    qRegisterMetaType<resetq::ResetState>("resetq::ResetState");

    // ── 库目录闸门（§9.5）：**只读挂载表**判断，不 stat / 不 open（volumeinfo 的铁律）──
    //   库目录 = 库文件所在目录。`Store::databasePath()` 是**静态**的 ⇒ 不需要库已打开，
    //   也就保证卷判定排在"任何碰盘"之前（网络卷上连锁文件都不许建）。
    const QString dir = QFileInfo(library::Store::databasePath()).absolutePath();
    const volume::Info info = volume::infoFor(dir);
    // §9.5：不是**本地固定卷**（或 autofs）⇒ 拒绝使用该库（SQLite 官方：WAL 不支持网络 fs；
    //   锁文件同样不可靠）。`autofs` 的 kind 就是 Unknown ⇒ 这一条同时排除它。
    m_volumeOk = info.mounted && !info.autofs && info.kind == volume::Kind::LocalFixed;
    if (!m_volumeOk) {
        m_volumeReason = info.mounted
            ? QStringLiteral("库目录不在本地固定卷（%1：%2）⇒ 拒绝使用该库（锁与 WAL 都不可靠）：%3")
                  .arg(volumeKindName(info.kind), info.fsType, dir)
            : QStringLiteral("库目录所在卷未挂载（%1）⇒ 拒绝使用该库").arg(dir);
        qWarning().noquote() << QStringLiteral("[core] %1").arg(m_volumeReason);
    } else {
        qInfo().noquote() << QStringLiteral("[core] 库目录闸门通过：%1（%2）")
                                 .arg(dir, info.fsType);
    }

    // ── 重置队列（串行 + 每图集锁 + 三种收尾，§9.5）──
    //   活序列由**本类**持有 ⇒ 只把三件事交给队列（可单测，见 resetqueue.h）
    resetq::ResetQueue::Hooks hooks;
    hooks.begin  = [this](qint64 id, QString *err) { return beginReset(id, err); };
    hooks.commit = [this](qint64 id, QString *err) { return commitReset(id, err); };
    hooks.end    = [this](qint64 id) { endReset(id); };
    m_resets = std::make_unique<resetq::ResetQueue>(dir, std::move(hooks), this);
    connect(m_resets.get(), &resetq::ResetQueue::stateChanged, this,
            [this](qint64 id, resetq::ResetState s) { emit resetStateChanged(id, s); });
    connect(m_resets.get(), &resetq::ResetQueue::notice, this,
            [this](const QString &text) { pushNotice(text); });

    // ── 跨进程感知：**只剩这一条职责** —— 1 s 比较 `data_version`（§9.5）──
    m_pollTimer.setInterval(1000);
    connect(&m_pollTimer, &QTimer::timeout, this, &LibraryCore::pollDataVersion);
    m_pollTimer.start();

    // ── ★★ 发布钩子 ⇒ 唤醒（§9.2：**事件驱动**，不引入任何周期定时器）──────────
    //   钩子**只捕获唤醒汇**（shared_ptr），**不捕获 `this`**：被 abandon 的序列会被
    //   故意泄漏 ⇒ 它的生产者线程在本类析构之后仍可能调钩子；`~LibraryCore` 里
    //   `detach()` 之后投递是空操作 ⇒ 绝不会打到已析构的 core 上。
    m_wakeSink = std::make_shared<WakeSink>();
    m_wakeSink->setHandler([this] { onWake(); });      // 只在 GUI 线程被执行（见 WakeSink）
    m_publishHook = [sink = m_wakeSink] { sink->wake(); };
}

// ★★ **退出收尾**（G3，用户 2026-10-15 批准）：由 `UiModel` 接 `aboutToQuit` 调用。
//   为什么必须在这里（而不是只靠析构）：`main.cpp` 把 `UiModel` 挂在 `QApplication` 上
//   ⇒ `~LibraryCore`（原来的唯一 join 点）发生在 `~QApplication` **删除子对象**时，
//   而那会儿 `QCoreApplication::instance()` 已经为空 ⇒ 落库 worker 最后那次事务 / 连接
//   析构用不了 QtSql（实测：`QSqlDatabase requires a QCoreApplication` + `Driver not loaded`
//   ⇒ **那次落库丢了**）。`aboutToQuit` 时 app 实例仍在 ⇒ worker 能正常跑完。
//   · join 落库 worker：**不许丢**（正在跑的原子替换要跑完；有 `PHOTOSET_COMMIT_DELAY_MS`
//     时这会把退出推迟到窗口结束 —— 那是刻意的，见报告）；
//   · 两个槽位：**既有有界宽限**（`reapSlot`；卡住的生产者最多等 `stallMs()`）；
//   · 唤醒汇断开 ⇒ 被 abandon 的生产者之后再调钩子也只是空操作。
//   ⚠️ 已知点：`aboutToQuit` 之后事件循环就要停了 ⇒ worker 排队的 `commitFinished`
//     可能不再被处理 —— **没关系**（事务已提交）；`m_commitInFlight` 保持 true，
//     锁由 `~ResetQueue` 释放。
//   幂等：线程已 join / 槽位已空 / 汇已 detach ⇒ 全部跳过。
void LibraryCore::shutdown()
{
    if (m_commitThread.joinable()) m_commitThread.join();
    reapSlot(m_resetSeq);
    reapSlot(m_session);
    if (m_wakeSink) m_wakeSink->detach();
}

LibraryCore::~LibraryCore()
{
    shutdown();
}

bool LibraryCore::libraryUsable() const
{
    return m_volumeOk && m_store && m_store->isOpen();
}

QString LibraryCore::unusableReason() const
{
    if (!m_volumeOk) return m_volumeReason;
    if (!m_store) return QStringLiteral("库存储未接上");
    if (!m_store->isOpen())
        return QStringLiteral("库未打开：%1").arg(library::Store::databasePath());
    return QString();
}

// ══ 数据出口：图库管理（薄转调）══════════════════════════════════════════════

QVector<library::Collection> LibraryCore::collections() const
{
    return m_store ? m_store->collections() : QVector<library::Collection>();
}

bool LibraryCore::addCollection(const QString &root, Sequence::Kind kind, QString *err)
{
    if (!libraryUsable()) {
        if (err) *err = unusableReason();
        return false;
    }
    const qint64 id = m_store->addCollection(root, kind);
    if (id < 0) {
        if (err) *err = m_store->lastError().isEmpty() ? QStringLiteral("加入图集失败")
                                                       : m_store->lastError();
        return false;
    }
    emit collectionsChanged();     // ★ 自己写库**不 bump data_version** ⇒ 显式通知
    return true;
}

bool LibraryCore::removeCollection(qint64 id, QString *err)
{
    if (!libraryUsable()) {
        if (err) *err = unusableReason();
        return false;
    }
    if (!m_store->removeCollection(id)) {
        if (err) *err = m_store->lastError().isEmpty() ? QStringLiteral("删除图集失败")
                                                       : m_store->lastError();
        return false;
    }
    emit collectionsChanged();
    return true;
}

bool LibraryCore::renameCollection(qint64 id, const QString &title)
{
    if (!libraryUsable()) return false;
    if (!m_store->renameCollection(id, title)) return false;
    emit collectionsChanged();
    return true;
}

bool LibraryCore::setReadingLayout(qint64 id, app::Layout layout)
{
    if (!libraryUsable()) return false;
    if (!m_store->setReadingLayout(id, layout)) return false;
    emit collectionsChanged();     // 版面是图集级设置 ⇒ 卡片会显示它
    return true;
}

bool LibraryCore::setResume(qint64 id, const QString &chainJson, int page)
{
    // 阅读进度：**不是集合列表的一部分**（不 emit collectionsChanged —— 免得卡片白刷）
    if (!libraryUsable()) return false;
    return m_store->setResume(id, chainJson, page);
}

// ══ 重置（§9.1 / §9.5）══════════════════════════════════════════════════════

bool LibraryCore::requestReset(qint64 id, const ResetOptions &opt, QString *err)
{
    if (!libraryUsable()) {
        if (err) *err = unusableReason();     // 闸门：库目录不可用 ⇒ 根本不入队
        return false;
    }
    const library::Collection c = m_store->collection(id);
    if (c.id < 0) {
        if (err) *err = QStringLiteral("图集不存在（%1）").arg(id);
        return false;
    }
    m_resetOptions.insert(id, opt);           // ★ 参数快照（§9.3）
    return m_resets->request(id, err);
}

bool LibraryCore::cancelReset(qint64 id)
{
    // ★★ **落库窗口内不存在"取消"**（设计 14 §9.1；用户 2026-10-15 裁决）：
    //   序列已经跑完（内容已完整），后台 worker 正在原子替换 ⇒ 停止位**没人看**、
    //   落库也**不会被撤销**。旧实现返回 true 却照样落库 = 骗人。
    //   ⇒ **静默 no-op**：返回 false，不 pushNotice、不报错、不做任何用户可见动作
    //     （界面此时已经把「取消扫描」禁用了 —— 见 `resetCommitting()`）。
    if (m_commitInFlight && id == m_resetId) return false;

    // running 的停止请求必须动那条**活序列**（本类持有它）：
    //   置停止 ⇒ 生产者自己退 ⇒ 下一次 pump 在 drain 内观察到 `ScanStoped`
    //   ⇒ `finishStopped()` 收尾 ⇒ **不落库**（§9.1）。
    if (m_resets->state(id) == ResetState::Running && id == m_resetId && m_resetSeq) {
        m_resetSeq->requestStop();
        return true;
    }
    return m_resets->cancel(id);              // 排队中 ⇒ 出队 + Stopped
}

bool LibraryCore::resetCommitting(qint64 id) const
{
    // ★ **唯一一份状态**（不另存）：落库窗口 = `m_commitInFlight` 且就是这条重置。
    return m_commitInFlight && id == m_resetId;
}

LibraryCore::ResetState LibraryCore::resetState(qint64 id) const
{
    return m_resets->state(id);
}

QString LibraryCore::resetReason(qint64 id) const
{
    return m_resets->reason(id);
}

bool LibraryCore::beginReset(qint64 id, QString *err)
{
    const library::Collection c = m_store->collection(id);
    if (c.id < 0) {
        if (err) *err = QStringLiteral("图集不存在（%1）").arg(id);
        return false;
    }
    if (c.rootPath.isEmpty()) {
        if (err) *err = QStringLiteral("图集没有来源路径（%1）").arg(id);
        return false;
    }
    const ResetOptions opt = m_resetOptions.value(id);
    auto seq = std::make_unique<Sequence>();
    installPublishHook(*seq);                 // ★ 唤醒钩子（**必须**在 startScan 之前装）
    // ★ 重置 = **起一条活序列**（与"直接打开文件夹 / 压缩包"**同一条路径**，§9.1）：
    //   序列在**它自己的生产者线程**上扫描；这里只是消费者持有它。
    seq->startScan(c.rootPath, opt.maxDepth, opt.hidden, opt.toScanOptions());
    m_resetSeq = std::move(seq);
    m_resetId = id;
    m_reportedProgress = -1;
    // 正看着这个图集 ⇒ 原来那份"从磁盘读出的快照"多余了（视图现在**别名**到这条活序列，§9.3）：
    //   显式释放，免得同一个图集在内存里留两份行（活序列 ≤2 的账也要算得清）
    if (m_viewId == id) m_viewSeq.reset();
    qInfo().noquote() << QStringLiteral("[core] 集合 %1 开始重置（depth=%2 hidden=%3）")
                             .arg(id).arg(opt.maxDepth).arg(opt.hidden);
    return true;
}

// ★★ **落库已完成**（真正的事务在后台 worker 里，见 `startCommit`）—— 这里只**取走结果**。
//   `ResetQueue` 的契约**一字不改**：返回 false ⇒ 该次重置 Failed 且**不落库**。
//   ⚠️ 故意不写成"永远返回 true"：那样一次误调的 `finishCompleted` 就会**假成功**
//      （库里其实没写）—— 没有结果就必须失败。
bool LibraryCore::commitReset(qint64 id, QString *err)
{
    if (!m_commitDone || m_commitResult.id != id) {
        if (err) *err = QStringLiteral("落库结果不在（%1）").arg(id);
        return false;
    }
    m_commitDone = false;                     // 结果**只消费一次**
    const CommitResult res = m_commitResult;
    if (!res.ok) {
        if (err) *err = res.err.isEmpty() ? QStringLiteral("落库失败") : res.err;
        return false;
    }
    return true;
}

// ★★ **后台落库**（§9.1）：GUI 线程只做两件轻事 —— ① 序列行 → 库行（O(n) 纯 CPU）；
//   ② 起 worker。**重活（十万行写库）在 worker 里**：QtSql/SQLite 连接不能跨线程 ⇒
//   worker **自己开一个** `library::Store`（与旧的写入磁盘线程同款做法）。
//   写完把结果 **post 回 GUI 线程** ⇒ 成功 `finishCompleted`、失败 `finishFailed(理由)`。
//   ⚠️ 落库期间**锁继续 held**：队列那条重置还没收尾（`m_commitInFlight` 挡住重复触发），
//      收尾发生在 `commitFinished`（GUI 线程）。
void LibraryCore::startCommit(qint64 id)
{
    if (m_commitInFlight) return;             // 串行队列保证同一次重置只会来一遍
    if (!m_resetSeq || m_resetId != id) return;

    const QVector<Sequence::Row> &src = m_resetSeq->rows();
    auto rows = std::make_shared<QVector<library::Item>>();
    rows->reserve(src.size());
    for (const Sequence::Row &r : src) rows->push_back(library::toItem(r));
    const int total  = rows->size();
    const int images = imageCountOf(src);

    m_commitInFlight = true;
    m_commitDone = false;
    m_commitResult = CommitResult();
    // ★★ 落库窗口的**变化驱动**出口：界面据此把「取消扫描」禁用（见 resetCommitting()）
    emit resetCommittingChanged(id, true);
    if (m_commitThread.joinable()) m_commitThread.join();   // 上一次早已结束（毫秒级）
    m_commitThread = std::thread([this, id, rows, total, images] {
        CommitResult res;
        res.id = id;
        res.rows = total;
        res.images = images;
        library::Store store;                 // ★ **本线程自己的连接**（QtSql 的规矩）
        QString err;
        if (!store.open(&err)) {
            res.err = QStringLiteral("落库失败：库打不开（%1）").arg(err);
        } else if (!replaceItemsWithDelay(store, id, *rows, &res.err)) {
            // 一个事务原子替换：失败已回滚 ⇒ 库里**旧内容原样保留**
        } else {
            res.ok = true;
        }
        // ★ 结果 post 回 GUI 线程。析构时 `m_commitThread.join()` ⇒ 绝不会打到已析构对象。
        QMetaObject::invokeMethod(this, [this, res] { commitFinished(res); },
                                  Qt::QueuedConnection);
    });
}

// 事务开始前的那一次 `QThread::msleep`（验收注入；默认 0 ⇒ 生产零影响）。
//   单独包一层只是为了把"延迟"与"原子替换"两件事分开，语义仍是一件事。
bool LibraryCore::replaceItemsWithDelay(library::Store &store, qint64 id,
                                        const QVector<library::Item> &rows, QString *err)
{
    const qint64 delay = commitDelayMs();
    if (delay > 0) QThread::msleep(unsigned(delay));
    if (store.replaceItems(id, rows)) return true;
    *err = store.lastError().isEmpty()
               ? QStringLiteral("落库失败（原子替换已回滚）") : store.lastError();
    return false;
}

// GUI 线程：worker 的结果落地（**唯一的落库收尾点**）。
void LibraryCore::commitFinished(const CommitResult &res)
{
    if (!m_commitInFlight) return;            // 迟到 / 重复 ⇒ 忽略
    m_commitInFlight = false;
    m_commitDone = true;                      // `commitReset`（队列的 commit 回调）会取走它
    m_commitResult = res;
    // ★★ 落库窗口结束（**先收窗口再收尾**）：界面恢复「取消扫描」的可用性
    emit resetCommittingChanged(res.id, false);

    const qint64 id = res.id;
    if (m_resets->state(id) != ResetState::Running) {
        // 该次重置已经不在了（理论上不会发生）：只留结果，不动队列
        qWarning().noquote() << QStringLiteral("[core] 落库结果迟到（集合 %1，状态 %2）")
                                    .arg(id).arg(resetq::resetStateName(m_resets->state(id)));
        return;
    }
    if (res.ok) {
        if (m_resets->finishCompleted(id)) {
            pushNotice(QStringLiteral("图集已重置：%1 行（%2 张图）")
                           .arg(res.rows).arg(res.images));
        } else {
            pushNotice(QStringLiteral("重置未落库（库中原内容不变）：%1")
                           .arg(m_resets->reason(id)));
        }
    } else {
        m_resets->finishFailed(id, res.err);  // 失败 ⇒ **不落库** + 释放锁 + 让位
        pushNotice(QStringLiteral("重置未落库（库中原内容不变）：%1").arg(res.err));
    }
}

void LibraryCore::endReset(qint64 id)
{
    // 收尾（完成 / 停止 / 失败**三种都到**）⇒ 释放那条活序列
    if (m_resetId == id) {
        m_resetSeq.reset();
        m_resetId = -1;
        m_reportedProgress = -1;
    }
    // 正看着这个图集 ⇒ 换成"从磁盘读出的只读快照"（查看永远是"库里的行"，§9.3）
    if (m_viewId == id) {
        m_viewSeq.reset();
        QString err;
        buildViewSnapshot(id, &err);
    }
}

// ══ 会话槽 / 查看槽（§9.3：**同一个显示槽** ⇒ 互斥）════════════════════════

void LibraryCore::releaseViewSlot()
{
    m_viewId = -1;
    m_viewSeq.reset();      // View==Reset 时它本来就是空的（别名不动活序列）
}

bool LibraryCore::buildViewSnapshot(qint64 id, QString *err)
{
    if (!m_store || !m_store->isOpen()) {
        if (err) *err = unusableReason();
        return false;
    }
    const library::Collection c = m_store->collection(id);
    if (c.id < 0) {
        if (err) *err = QStringLiteral("图集不存在（%1）").arg(id);
        return false;
    }
    // ★ 恢复的输入 = 库里的行（转换只有一份：`library::toRow`）
    const QVector<Sequence::Row> srows = m_store->sequenceRows(id);

    auto seq = std::make_unique<Sequence>();
    // 已写入磁盘的行 → 一条**静态只读**序列（不再跟随、不再重建，§9.1）
    seq->buildFromRows(c.rootPath, srows, library::rootIsArchive(c));
    m_viewSeq = std::move(seq);
    m_viewId = id;
    return true;
}

bool LibraryCore::openCollection(qint64 id, QString *err)
{
    if (!m_store || !m_store->isOpen()) {
        if (err) *err = unusableReason();
        return false;
    }
    // View 与 Session **互斥**（同一个显示槽，§9.3）：开图集查看 ⇒ 离开会话 ⇒ 停 + 销毁它
    closeSession();

    // ★ `View == Reset` ⇒ **就是那条活序列**（同一个对象，别名；取消"视图镜像"，§9.3）
    if (m_resetId == id && m_resetSeq) {
        m_viewSeq.reset();
        m_viewId = id;
        return true;
    }
    return buildViewSnapshot(id, err);
}

void LibraryCore::closeCollectionView()
{
    releaseViewSlot();
}

Sequence *LibraryCore::viewSequence() const
{
    // ★ 别名规则**每次现算**（不存第二份状态）：正在重置的那条就是查看的那条
    if (m_resetSeq && m_viewId >= 0 && m_viewId == m_resetId) return m_resetSeq.get();
    return m_viewSeq.get();
}

bool LibraryCore::openSession(const QString &path, QString *err)
{
    return openSession(path, ResetOptions(), err);
}

bool LibraryCore::openSession(const QString &path, const ResetOptions &opt, QString *err)
{
    if (path.isEmpty()) {
        if (err) *err = QStringLiteral("打开会话：路径为空");
        return false;
    }
    // View 与 Session 互斥（§9.3）：离开图集查看 ⇒ 释放显示槽（图集是本会话之外的另一条来源）
    releaseViewSlot();
    if (m_session) m_session->requestStop();     // 全局唯一（§9.2）：旧的停 + 销毁
    m_session = std::make_unique<Sequence>();
    installPublishHook(*m_session);              // ★ 唤醒钩子（**必须**在 startScan 之前装）
    m_session->startScan(path, opt.maxDepth, opt.hidden, opt.toScanOptions());
    qInfo().noquote() << QStringLiteral("[core] 会话序列：%1").arg(path);
    return true;
}

void LibraryCore::closeSession()
{
    if (!m_session) return;
    // ★★ **有界收尾**（§9.2，与重置槽**同一份实现**）：置停止位 ⇒ 有界宽限 ⇒
    //   超了就 `abandon()` + **故意泄漏**。源卡在不可中断 IO（FIFO / 网络盘）时，
    //   界面最多被拖 `stallMs()`（默认 30 s），不再是"无超时 join ⇒ 永远僵死"。
    if (!reapSlot(m_session))
        pushNotice(QStringLiteral("会话来源卡住：已放弃（有界宽限 %1 ms 用尽，未 join）")
                       .arg(stallMs()));
}

// ★★ **槽位收尾的唯一实现**（会话槽 / 重置槽共用；§9.2）：
//   · 先 `requestStop()`（复用既有的 cancel 语义 —— **不新造状态**）；
//   · **有界宽限**：只要生产者还在推进（`lastProgressMs()` 在动）就继续等；
//     判定卡住的条件与看门狗**同一来源**（`stalledSince` + `stallMs()`）⇒ 超过阈值就放弃；
//   · 超时 ⇒ `abandon()`（detach，**不 join** —— 它可能永远回不来）+ `release()`
//     （**故意泄漏**：detached 生产者还拿着 `this`，释放就是 UAF）；
//   · 干净 ⇒ `reset()`（`~Sequence` 是**唯一 join 点**）。
//   返回 true = 干净收尾；false = 已放弃（`slot` 已清空，调用方**不能再碰**那个对象）。
bool LibraryCore::reapSlot(std::unique_ptr<Sequence> &slot)
{
    if (!slot) return true;
    Sequence *seq = slot.get();
    const qint64 limit = stallMs();
    const qint64 refMs = monotonicNowMs();   // 进度未知时的参照 ⇒ 至少给满一次宽限
    seq->requestStop();
    while (!seq->scanFinished()) {
        if (stalledSince(*seq, refMs, limit)) {
            seq->abandon();
            slot.release();                  // ★ **故意泄漏**（detached 线程还拿着它）
            return false;
        }
        QThread::msleep(1);                  // 有界：最多 `limit` ms（默认 30 s）
    }
    slot.reset();                            // 干净收尾：join 在 ~Sequence
    return true;
}

// ★ 唤醒钩子：只给**要扫描的**序列装（会话 / 重置槽）；
//   从磁盘读出的只读快照（`buildFromRows`）不扫描 ⇒ 没有批次可发布。
void LibraryCore::installPublishHook(Sequence &seq)
{
    if (m_publishHook) seq.setPublishHook(m_publishHook);
}

// ★ 唤醒（**GUI 线程**：由 WakeSink 排队投递，见文件头）：**合并**到每轮事件循环最多一次。
//   `WakeSink` 已经做过"待处理 ⇒ 不再投递"的第一层合并；这里是第二层：本轮已经排了一次
//   `pump()` 就不再排（钩子每放一批都唤醒，但一轮事件循环只消费一次）。
void LibraryCore::onWake()
{
    if (m_pumpScheduled) return;
    m_pumpScheduled = true;
    QMetaObject::invokeMethod(this, [this] {
        m_pumpScheduled = false;
        const Pumped p = pump();
        emit pumped(p.sessionChanged, p.viewChanged, p.resetChanged);
    }, Qt::QueuedConnection);
}

Sequence *LibraryCore::sessionSequence() const
{
    return m_session.get();
}

Sequence *LibraryCore::resetSequence(qint64 id) const
{
    return (m_resetSeq && m_resetId == id) ? m_resetSeq.get() : nullptr;
}

// ══ 消费（**唯一调用者 = UiModel**；每轮事件循环最多一次）═══════════════════

LibraryCore::Pumped LibraryCore::pump()
{
    Pumped p;

    // ── ① 会话槽：每轮最多 drain 一次（合并，§9.2）──
    if (m_session) p.sessionChanged = m_session->drain();

    // ── ② 重置槽：**判定结束的条件只在 drain 内观察**（§9.4 契约②）──
    //   · 先 `drain()`（它内部就是"先 acquire 读状态、再锁内 swap 队列"，契约①）；
    //   · 然后才读 `state()` —— 读到终态 ⇒ **最后一批已经在本次 swap 里**；
    //   · 收尾前再 drain 一次兜底（契约②末句）。
    if (m_resetSeq) {
        const qint64 id = m_resetId;
        p.resetChanged = m_resetSeq->drain();

        // ★ 数量变化就发（§9.4「计数」；**在收尾之前**，保证最终数量也发得出去）。
        //   ★★ 口径 = **已到达张数**（`discoveredCount()`），**不是** `count()`：后者是
        //      "可显示边界"（从头锚定 + 1 项前瞻 ⇒ 会悬着最后一条）。卡片计数与进度推送
        //      要的是"发现了几张"，与"能不能显示"无关（设计 14 §9.1；两个口径的区别
        //      见 `sequence.h` 的 `count()` / `discoveredCount()` 注释）。
        const int discovered = m_resetSeq->discoveredCount();
        if (discovered != m_reportedProgress) {
            m_reportedProgress = discovered;
            emit resetProgress(id, discovered);
        }

        const Sequence::State s = m_resetSeq->state();
        const bool terminal = (s == Sequence::State::Scaned || s == Sequence::State::ScanStoped);
        if (terminal) {
            m_resetSeq->drain();                       // 兜底（幂等）

            if (m_commitInFlight) {
                // ★★ 落库**已经在后台 worker 里跑**（`startCommit`）：这里什么都不做 ——
                //   **锁继续 held**（§9.1：重置尚未结束），结果由 worker post 回来
                //   （`commitFinished`）再收尾。重复 pump 也不会重复触发。
            } else if (s == Sequence::State::ScanStoped) {
                // 主动停止 / 取消 ⇒ **不落库**（§9.1）
                m_resets->finishStopped(id);
                pushNotice(QStringLiteral("重置已停止（未落库，库中原内容不变）：%1").arg(id));
            } else if (!m_resetSeq->unsupportedReason().isEmpty()) {
                // 来源不支持 / 根不可用：序列"空跑完"（Scaned + 0 条）。
                //   ⚠️ 这不是"干净的 0 条" ⇒ **不许落库**（否则会把旧内容替换成空）
                const QString why = m_resetSeq->unsupportedReason();
                m_resets->finishFailed(id, why);
                pushNotice(QStringLiteral("重置未落库（库中原内容不变）：%1").arg(why));
            } else {
                // ★ 完成 ⇒ **后台落库**（§9.1）：worker 里自己开 Store 连接 + post 结果回来。
                //   成功/失败都在 `commitFinished` 里收尾（GUI 线程）。
                startCommit(id);
            }
            p.resetChanged = true;
        } else if (stalledSince(*m_resetSeq, 0, stallMs())) {
            // ★ 判定卡住的条件在**消费侧**（§9.2）：判定条件与收尾**同一来源**（`stallMs()` / `reapSlot`）。
            const QString why =
                QStringLiteral("重置卡住：超过 %1 s 没有进度（已放弃，未落库）")
                    .arg(stallMs() / 1000.0, 0, 'g', 3);
            // 有界收尾（**与会话槽同一份实现**）：置停止 ⇒ 宽限已用尽 ⇒ 立刻 abandon + 泄漏；
            //   若这期间它其实已经收尾了（`scanFinished`）⇒ 走干净 join，下一轮 pump 处理终态。
            if (!reapSlot(m_resetSeq)) {
                m_resetId = -1;
                m_reportedProgress = -1;
                m_resets->finishFailed(id, why);       // 释放锁 + 让位
                pushNotice(why);
                p.resetChanged = true;
            }
        }
    }

    // ── ③ 查看槽：`View == Reset` 时上面 drain 的**就是同一个对象** ⇒ 不重复 drain ──
    if (m_viewSeq && m_viewSeq.get() != m_resetSeq.get())
        p.viewChanged = m_viewSeq->drain();

    return p;
}

// ══ 跨进程感知（§9.5：只有 1 s 的 data_version 比较 —— **没有自愈**）══════════

void LibraryCore::pollDataVersion()
{
    // ── ★★ 兜底驱动（设计 14 §9.2 的"事件驱动"补充；T2c-2b-1 交付物②）────────────
    //   本定时器**职责不变**（1 s 比较 `data_version`，感知别的实例改了库）——这里只是
    //   **顺带推进一次槽位**，**不新增任何周期定时器**：
    //     `onWake()` 那条路只在生产者**放批 / 写终态**时被唤醒 ⇒ 生产者**完全不推进**
    //     （FIFO 卡死 / 卡在不可中断 IO）时永远不会有人叫醒消费者，判定卡住的条件与收尾就永远
    //     轮不到。⇒ 用这一跳兜底：哪怕一次唤醒都没有，槽位也每秒被推一次。
    //   ⚠️ 只在**真有变化**时才发 `pumped`（变化驱动：没变不许刷界面，见 UiModel）。
    {
        const Pumped p = pump();
        if (p.sessionChanged || p.viewChanged || p.resetChanged)
            emit pumped(p.sessionChanged, p.viewChanged, p.resetChanged);
    }

    if (!m_store || !m_store->isOpen()) return;
    const int v = m_store->dataVersion();
    if (m_lastDataVersion < 0) {       // 首次只记基线（库可能是构造之后才打开的）
        m_lastDataVersion = v;
        return;
    }
    if (v == m_lastDataVersion) return;    // 没变 ⇒ 什么都不做（**没有自愈**）
    m_lastDataVersion = v;
    emit collectionsChanged();             // 别的实例改了库（增删改 / 原子替换完成）
}

// ══ 提示 ════════════════════════════════════════════════════════════════════

void LibraryCore::pushNotice(const QString &text)
{
    if (text.isEmpty()) return;
    m_notices << text;
    emit noticeReady(text);
}

QString LibraryCore::takeNotice()
{
    return m_notices.isEmpty() ? QString() : m_notices.takeFirst();
}
