#pragma once

// ══ 图库 / 序列抽象（设计 14 §9.3）：**与三个界面无关**的核心模型 ══════════════
//
//   · **不认识界面**：没有 `Screen` 枚举、没有 QML、**不 include** `uimodel.h` / `appsettings.h`
//     （T2a 起设置没有单例 ⇒ 扫描参数一律**调用方传**，见 `ResetOptions`）；
//   · 拥有**三个槽位**（`Session` / `View` / `Reset`），**活序列 ≤ 2**：
//       `Session`（0/1，全局唯一）与 `View`（0/1，从磁盘读出的只读快照）**互斥**（同一个显示槽）；
//       `Reset`（0/1）独立存活（退出浏览/查看不销毁）；
//       **View 与 Reset 同 id ⇒ 是同一个 `Sequence` 对象**（别名，取消"视图镜像"，§9.3）；
//     ⇒ 可达组合只有 `Reset+View` / `Reset+Session` / `View==Reset`（1 条）/ 单条；
//   · `pump()` 是**唯一消费入口**（调用者 = UiModel，**合并到每轮事件循环一次**）：
//       各槽位 `drain()` 一次；**在 drain 内观察到终态**才算"重置跑完"（§9.4 契约①②）；
//   · 落库走 `Store::replaceItems`（一个事务原子替换，§9.1）；
//     串行 + 每图集跨进程锁走 `ResetQueue`（§9.5）；
//     跨进程感知只有 1 s 的 `data_version` 比较（**没有自愈** —— 不再有"进行中"的持久状态）。
//
//   ⚠️ 唤醒汇的**唯一目的**是"跨线程唤醒 GUI 线程 + 合并"（每轮事件循环最多 `pump()` 一次）
//      ⇒ **不需要任何周期定时器**（§9.2）。本类只有 1 s 的 `data_version` 定时器
//      （它的职责是感知别的实例改了库；顺带兜底推进槽位，见 .cpp）。
//   ★ T2c-2b-2：`ScanController` / `libraryprobe` 已删 —— 图集动作**全部经本类**：
//     `requestReset` / `cancelReset` / `openCollection` / `openSession`；落库走
//     `Store::replaceItems`（唯一写入路径）；停止 / 失败 / 卡住 ⇒ **不落库**。

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include <functional>
#include <memory>
#include <thread>

#include "librarystore.h"
#include "resetqueue.h"
#include "sequence.h"

class LibraryCore : public QObject
{
    Q_OBJECT
public:
    // §9.3 的状态枚举**只定义一份**（在 `resetqueue.h`）—— 这里只是别名，
    //   `LibraryCore::ResetState` 这个用法与设计里写的一字不差。
    using ResetState = resetq::ResetState;

    // 一次重置的参数 = **点按那一刻的快照**（§9.3）。core 不读设置（T2a 起没有单例）。
    //   ★ 偏离 §9.3（说明理由）：设计里只写了 `maxDepth` / `hidden`，但 `Sequence` 还要
    //     内层包上限（§7），而 core 又不许读设置 ⇒ 只能由调用方一起传进来。
    struct ResetOptions {
        int    maxDepth = 5;          // 与 `AppSettings::recurseDepth` 默认一致
        bool   hidden = false;        // 与 `AppSettings::scanHidden` 默认一致
        qint64 innerArchiveMaxMb = Sequence::kDefaultInnerArchiveMaxMb;   // §7

        // 转成序列要的那组参数（**唯一转换点**）
        Sequence::ScanOptions toScanOptions() const
        {
            return Sequence::ScanOptions{ innerArchiveMaxMb };
        }
    };

    // `store` 由调用方打开（本类**不碰数据库生命周期**）；库目录取 `Store::databasePath()`。
    explicit LibraryCore(library::Store *store, QObject *parent = nullptr);
    // ★★ **退出收尾入口**（G3，用户 2026-10-15 批准）：`UiModel` 把它接到
    //   `QCoreApplication::aboutToQuit` —— 那时 `QCoreApplication::instance()` 还在
    //   （`~QApplication` 之前），落库 worker 里的 QtSql 仍能工作。
    //   · **join 落库 worker**：正在跑的那次原子替换要**跑完**，不许丢；
    //   · 两个槽位走既有 `reapSlot()` 的**有界宽限**（卡住的生产者最多等 `stallMs()`）；
    //   · 唤醒汇断开（此后任何投递都不再碰本对象）。
    //   ⚠️ 已知点：`aboutToQuit` 之后事件循环就要停了 ⇒ worker 排队的 `commitFinished`
    //     可能不再被处理 —— **没关系**（事务已提交）；`m_commitInFlight` 保持 true，
    //     锁由 `~ResetQueue` 释放。
    //   **幂等**：`~LibraryCore` 可以安全地再调一遍（不可 join 的线程 / 已空的槽位都跳过）。
    void shutdown();
    // ★ 析构 = **收尾点**：`shutdown()`（两个槽位**有界收尾** + 落库 worker join +
    //   唤醒汇断开 ⇒ 卡死的生产者不会把退出拖住，也不留下悬挂线程 / 悬挂 this）。
    ~LibraryCore() override;

    // ── 库目录闸门（§9.5）：**本地固定卷** + 库可用 ────────────────────────────
    bool    libraryUsable() const;
    QString unusableReason() const;

    // ── 图库管理（薄转调；**自己写库不 bump data_version** ⇒ 成功后显式 collectionsChanged）──
    QVector<library::Collection> collections() const;
    bool addCollection(const QString &root, Sequence::Kind kind, QString *err);
    bool removeCollection(qint64 id, QString *err);
    bool renameCollection(qint64 id, const QString &title);
    bool setReadingLayout(qint64 id, app::Layout layout);
    bool setResume(qint64 id, const QString &chainJson, int page);

    // ── 重置（串行队列 + 每图集锁，§9.5）─────────────────────────────────────
    // err 覆盖：库不可用 / 图集不存在 / 已在队列 / 正在重置。
    // ⚠️ "别的实例持锁"**不是**同步 err（锁只在**轮到队首**时才试，§9.5）⇒
    //    它表现为该集合 `state == Failed` + `resetReason()` / 提示。
    bool requestReset(qint64 id, const ResetOptions &opt, QString *err);
    bool cancelReset(qint64 id);              // 停止 ⇒ **不落库**（§9.1）
    ResetState resetState(qint64 id) const;
    // ★★ **落库窗口**（设计 14 §9.1；用户 2026-10-15 裁决）：
    //   序列跑完 ⇒ `startCommit()` 在后台 worker 里原子替换；这期间**锁继续 held**、
    //   扫描**已经结束**（内容已完整）⇒ **不存在"取消"这个动作**：`cancelReset()` 必须
    //   返回 false 且**静默 no-op**（不提示、不报错、不做任何用户可见动作）。
    //   界面据此把「取消扫描」**禁用**（`CollectionCard.qml`；文案不动）。
    //   ⚠️ 这就是唯一一份状态（`m_commitInFlight`）—— 不许另存一份。
    bool resetCommitting(qint64 id) const;
    // ★ 偏离 §9.3（说明理由）：失败原因没有别的稳定出口 —— `takeNotice()` 是瞬时的
    //   （谁先取走谁得），界面要回答"这个图集上次为什么没重置"就得有它。
    QString resetReason(qint64 id) const;

    // ── 会话（全局唯一）与查看槽（§9.3：**同一个显示槽** ⇒ 互斥）──────────────
    bool openSession(const QString &path, QString *err);
    // ★ 偏离 §9.3（说明理由）：core 不读设置 ⇒ 会话扫描参数也只能调用方传；
    //   两参版是它的默认值包装（默认值 = 旧设置的默认值）。
    bool openSession(const QString &path, const ResetOptions &opt, QString *err);
    void closeSession();                       // 退出浏览/查看 ⇒ 停 + 销毁会话序列（§9.2）
    bool openCollection(qint64 id, QString *err);   // 磁盘上的行 → 只读快照（正在重置 ⇒ 别名）
    void closeCollectionView();                // 离开图集查看 ⇒ 释放显示槽（§9.3）
    Sequence *viewSequence() const;            // 正在重置的那条 ⇒ 返回**那条活序列**（别名）

    // ── 消费：**唯一调用者 = UiModel**，合并到每轮事件循环一次（§9.3）─────────
    struct Pumped { bool sessionChanged = false, viewChanged = false, resetChanged = false; };
    Pumped pump();

    Sequence *sessionSequence() const;
    Sequence *resetSequence(qint64 id) const;
    QString   takeNotice();

signals:
    // 增删改 / **别的实例改了库**（内部 1 s 比较 `data_version`，§9.5）
    void collectionsChanged();
    void resetStateChanged(qint64 id, LibraryCore::ResetState state);
    void resetProgress(qint64 id, int discovered);   // ★ 每次数量变化就发（卡片计数，§9.4）
    // ★★ 落库窗口的**变化驱动**出口（T2c-2b-2）：`startCommit()` 置真后、`commitFinished()`
    //   收尾时各发一次（id = 那个图集）。`LibraryModel` 接它更新 `resetCommitting` role
    //   （**值真变才 dataChanged**）。界面据此禁用「取消扫描」。
    void resetCommittingChanged(qint64 id, bool committing);
    void noticeReady(const QString &text);
    // ★★ **事件驱动的心跳**（§9.2）：生产者放一批 / 写终态 ⇒ 唤醒钩子 ⇒ 跨线程 post 到 GUI 线程
    //   ⇒ **合并到每轮事件循环最多 `pump()` 一次**（**不需要任何周期定时器**）。
    //   三个 bool = 那次 `pump()` 的 `Pumped`（T2c-2b 的 `UiModel` 接它刷新视图即可）。
    void pumped(bool sessionChanged, bool viewChanged, bool resetChanged);

private:
    // ★★ 唤醒汇（定义在 .cpp）：**发布钩子只捕获它**（shared_ptr），**不捕获 `this`**。
    //   理由：序列可能被 `abandon()`（detach + 故意泄漏）⇒ 生产者线程在本类析构之后
    //   仍可能调钩子。钩子只投递到汇；`~LibraryCore` 里 `detach()` 后投递变成空操作
    //   ⇒ 绝不会把调用打到已析构的 core 上（也绝不需要 moc：用 functor 版 invokeMethod）。
    class WakeSink;

    void pollDataVersion();                    // 1 s：只比较 data_version（**没有自愈**）
    bool beginReset(qint64 id, QString *err);  // ResetQueue 的 begin 回调（建活序列 + 起跑）
    bool commitReset(qint64 id, QString *err); // ResetQueue 的 commit 回调（**取走后台落库结果**）
    void endReset(qint64 id);                  // ResetQueue 的 end 回调（释放活序列 / 换快照）
    bool buildViewSnapshot(qint64 id, QString *err);
    void releaseViewSlot();
    void pushNotice(const QString &text);

    // ── ★★ 钩子链路（§9.2：事件驱动，**没有周期定时器**）────────────────────
    void installPublishHook(Sequence &seq);    // 只给**要扫描的**序列装（会话 / 重置槽）
    void onWake();                             // GUI 线程：**合并**（每轮最多一次）

    // ── ★★ 槽位收尾的**唯一实现**（会话槽 / 重置槽共用，§9.2）───────────────
    //   置停止位 ⇒ 有界宽限 ⇒ 超了就 `abandon()` + **故意泄漏**。
    //   返回 true = 干净收尾（已析构 / join）；false = 已放弃（对象泄漏，`slot` 已清空）。
    bool reapSlot(std::unique_ptr<Sequence> &slot);

    // ── ★★ 后台落库（§9.1：worker 里**自己开一个** Store 连接，写完 post 回 GUI）──
    struct CommitResult {
        qint64  id = -1;
        bool    ok = false;
        QString err;
        int     rows = 0;
        int     images = 0;
    };
    void startCommit(qint64 id);               // GUI 线程：快照行 + 起 worker
    void commitFinished(const CommitResult &res);   // GUI 线程：结果 ⇒ 队列的完成/失败收尾
    // worker 线程：**事务开始之前**的一次延迟注入（`PHOTOSET_COMMIT_DELAY_MS`，默认 0）
    //   + 原子替换。返回 false ⇒ 已回滚，`err` 带原因（旧内容原样保留）。
    static bool replaceItemsWithDelay(library::Store &store, qint64 id,
                                      const QVector<library::Item> &rows, QString *err);

    library::Store *m_store = nullptr;

    // ── 槽位（**活序列 ≤ 2**，§9.3）──
    std::unique_ptr<Sequence> m_session;       // Session（0/1）
    std::unique_ptr<Sequence> m_viewSeq;       // View 的从磁盘读出的只读快照（别名时不用它）
    qint64 m_viewId = -1;
    std::unique_ptr<Sequence> m_resetSeq;      // Reset（0/1）：正在重置的那条**活序列**
    qint64 m_resetId = -1;
    QHash<qint64, ResetOptions> m_resetOptions;   // 点按那一刻的参数快照
    int m_reportedProgress = -1;                  // 上次发出去的 discovered

    // ── 唤醒（§9.2）：钩子捕获 `m_publishHook`；`m_wakeSink` 由钩子与这里共同持有 ──
    std::shared_ptr<WakeSink> m_wakeSink;
    std::function<void()>     m_publishHook;
    bool                      m_pumpScheduled = false;   // 合并：本轮是否已排了一次 pump

    // ── 后台落库（§9.1）──
    std::thread  m_commitThread;
    bool         m_commitInFlight = false;     // 落库期间**锁继续 held**（队列保持 Running）
    bool         m_commitDone = false;         // worker 结果已 post 回来（GUI 线程读写）
    CommitResult m_commitResult;

    std::unique_ptr<resetq::ResetQueue> m_resets; // 串行 + 每图集锁 + 三种收尾
    QTimer  m_pollTimer;                          // 1 s：只做 data_version 比较
    int     m_lastDataVersion = -1;
    QStringList m_notices;

    // 库目录闸门（构造时算一次；`Store` 可能后开 ⇒ `libraryUsable()` 里再问一次 isOpen）
    bool    m_volumeOk = false;
    QString m_volumeReason;
};
