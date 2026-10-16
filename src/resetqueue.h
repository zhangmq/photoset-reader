#pragma once

// ══ 重置队列（设计 14 §9.2 / §9.5）：**串行 + 每图集跨进程锁 + 三种收尾** ══════
//
//   本类只干一件事（不认识界面、不认识设置、**不 include `sequence.h`**）：
//     · **串行**：同时只有一条 running；多条入队**按入队先后**逐个执行（先入先跑）；
//     · **跨进程锁**：轮到**队首**才 `QLockFile::tryLock`（`<库目录>/locks/reset-<cid>.lock`）；
//       拿不到 ⇒ 该集合 `Failed` + 原因"另一实例正在重置该图集"，**不跨进程排队**（§9.5）；
//     · **三种收尾**（`finishCompleted` / `finishStopped` / `finishFailed`，**唯一入口**）：
//         完成     ⇒ 调**落库回调**（`Store::replaceItems` 一个事务原子替换）→ `Done`；
//         主动取消 ⇒ **不落库** → `Stopped`；
//         失败/卡住⇒ **不落库** → `Failed`。
//       三种**都释放锁**并把 running 位让给下一个。
//
//   ★★ **活序列（`Sequence`）由调用方持有** —— 这里只认三个回调（`Hooks`）：
//        `begin`  起一次重置（建活序列 + 开始跑），失败 ⇒ `Failed`；
//        `commit` 完成时的落库（原子替换）；
//        `end`    收尾（三种都调）⇒ 调用方把那条活序列释放掉。
//      ⇒ 不依赖 `Sequence` ⇒ **可以单测**（用例喂假回调即可，见 `tests/selftest_reset.cpp`）。
//
//   ★ 状态是**纯内存**（**不写入磁盘**，设计 §9.5：没有"进行中"的持久状态 ⇒ 自愈彻底消失）；
//     终态（Done / Stopped / Failed）**保留在表里**，只为"查询上一次的结果"。

#include <QHash>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>

class QLockFile;

namespace resetq {

// 重置的对外状态（§9.3 的 `LibraryCore::ResetState` **就是它** —— 枚举只定义这一份，
//   `LibraryCore` 用 `using ResetState = resetq::ResetState` 起别名，不重复定义）
enum class ResetState { Idle, Queued, Running, Done, Stopped, Failed };

// 诊断 / 提示用的名字（日志与用例都走它，别处不许再手抄这几个字符串）
QString resetStateName(ResetState s);

class ResetQueue : public QObject
{
    Q_OBJECT
public:
    // 「活序列」由调用方持有 ⇒ 本类只认这三件事
    struct Hooks {
        std::function<bool(qint64 id, QString *err)> begin;   // 轮到队首且拿到锁之后：起一次重置
        std::function<bool(qint64 id, QString *err)> commit;  // 完成：原子替换落库
        std::function<void(qint64 id)> end;                   // 收尾（三种都调）：释放活序列
    };

    // `libraryDir` = **库目录**（调用方已用 `volumeinfo` 验过是本地固定卷，§9.5）
    ResetQueue(const QString &libraryDir, Hooks hooks, QObject *parent = nullptr);
    ~ResetQueue() override;

    // ── 入队 / 取消 / 查询 ────────────────────────────────────────────────
    // 入队；err 覆盖：`id` 无效 / **已在队列** / **正在重置**。
    // 终态（Done/Stopped/Failed）**可以再次入队**（重跑是正常动作）；入队即清掉上次的原因。
    bool request(qint64 id, QString *err = nullptr);
    // 取消：**排队中** ⇒ 出队 + `Stopped`（不执行、不落库）；
    //   **running** ⇒ 这里返回 false —— 停止请求要动那条活序列（调用方的知识）：
    //   调用方先 `Sequence::requestStop()`，序列跑到 `ScanStoped` 时由 `finishStopped()` 收尾（§9.2）。
    bool cancel(qint64 id);
    ResetState state(qint64 id) const;   // 未知 id ⇒ Idle
    QString    reason(qint64 id) const;  // 失败原因（别的实例持锁 / 落库失败 / 卡住…）
    qint64     runningId() const { return m_running; }
    int        queuedCount() const { return m_queue.size(); }

    // ── 三种收尾（唯一入口；都释放锁 + 让位给下一个）────────────────────────
    // 完成 ⇒ `commit()` 落库；commit 失败 ⇒ Failed（**旧内容不变**，仍不落库）
    bool finishCompleted(qint64 id);
    // 主动取消 / 停止
    void finishStopped(qint64 id);
    // 失败 / 卡住（由调用方判定：落库失败、来源不支持、30 s 无进度…）
    void finishFailed(qint64 id, const QString &why);

    // 提示（FIFO）：跨实例持锁 / 落库失败 …（`LibraryCore::takeNotice()` 取的就是它）
    QString takeNotice();
    // 锁文件路径（§9.5 的**唯一**拼法）：`<库目录>/locks/reset-<cid>.lock`（只读，不碰盘）
    QString lockPathFor(qint64 id) const;

signals:
    void stateChanged(qint64 id, resetq::ResetState state);
    void notice(const QString &text);

private:
    void startNext();                               // 没有 running 时：取队首 → 试锁 → begin
    void finish(qint64 id, ResetState end, const QString &why);
    void setState(qint64 id, ResetState state);
    void releaseLock();
    void pushNotice(const QString &text);

    QString         m_libraryDir;
    Hooks           m_hooks;
    QVector<qint64> m_queue;                        // FIFO：先入先跑
    QHash<qint64, ResetState> m_states;             // 纯内存；终态保留 ⇒ 可查询
    QHash<qint64, QString>    m_reasons;
    qint64          m_running = -1;                 // 同时只有一条（§9.2）
    std::unique_ptr<QLockFile> m_lock;              // 只属于 running 的那条
    QStringList     m_notices;
};

} // namespace resetq

Q_DECLARE_METATYPE(resetq::ResetState)
