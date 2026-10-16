#include "resetqueue.h"

// 设计 14 §9.2（串行两条序列）/ §9.5（每图集锁 + 库目录闸门）。

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>

namespace resetq {

QString resetStateName(ResetState s)
{
    switch (s) {
    case ResetState::Idle:    return QStringLiteral("Idle");
    case ResetState::Queued:  return QStringLiteral("Queued");
    case ResetState::Running: return QStringLiteral("Running");
    case ResetState::Done:    return QStringLiteral("Done");
    case ResetState::Stopped: return QStringLiteral("Stopped");
    case ResetState::Failed:  return QStringLiteral("Failed");
    }
    return QStringLiteral("Idle");
}

ResetQueue::ResetQueue(const QString &libraryDir, Hooks hooks, QObject *parent)
    : QObject(parent), m_libraryDir(libraryDir), m_hooks(std::move(hooks))
{
    // 信号带枚举 ⇒ 跨线程（queued）连接也要能用（本批是同线程直连，但注册是无害的）
    qRegisterMetaType<resetq::ResetState>("resetq::ResetState");
}

ResetQueue::~ResetQueue() = default;

QString ResetQueue::lockPathFor(qint64 id) const
{
    // §9.5 的唯一拼法：**每图集一个锁文件**
    return QStringLiteral("%1/locks/reset-%2.lock").arg(m_libraryDir).arg(id);
}

ResetState ResetQueue::state(qint64 id) const
{
    return m_states.value(id, ResetState::Idle);
}

QString ResetQueue::reason(qint64 id) const
{
    return m_reasons.value(id);
}

void ResetQueue::setState(qint64 id, ResetState state)
{
    if (m_states.value(id, ResetState::Idle) == state) return;
    m_states.insert(id, state);
    emit stateChanged(id, state);
}

void ResetQueue::pushNotice(const QString &text)
{
    if (text.isEmpty()) return;
    m_notices << text;
    emit notice(text);
}

QString ResetQueue::takeNotice()
{
    return m_notices.isEmpty() ? QString() : m_notices.takeFirst();
}

void ResetQueue::releaseLock()
{
    if (!m_lock) return;
    // ★ 三种收尾都走这里（§9.5："锁属进程，不属序列" ⇒ 卡住被放弃时也要显式释放）
    m_lock->unlock();
    m_lock.reset();
}

bool ResetQueue::request(qint64 id, QString *err)
{
    if (id < 0) {
        if (err) *err = QStringLiteral("集合 id 无效");
        return false;
    }
    const ResetState st = state(id);
    if (st == ResetState::Queued) {
        if (err) *err = QStringLiteral("该图集已在重置队列里");
        return false;
    }
    if (st == ResetState::Running) {
        if (err) *err = QStringLiteral("该图集正在重置");
        return false;
    }
    m_reasons.remove(id);                 // 重跑：清掉上一次的原因
    setState(id, ResetState::Queued);     // 终态可以直接再次入队
    m_queue.append(id);
    startNext();                          // 空闲就立刻起（observable = 立刻 Running）
    return true;
}

bool ResetQueue::cancel(qint64 id)
{
    if (state(id) != ResetState::Queued) return false;
    m_queue.removeAll(id);                // 排队中 ⇒ 出队，**不执行、不落库**
    setState(id, ResetState::Stopped);
    return true;
}

// 轮到一个集合：**先试锁，再 begin**（§9.5："轮到队首才尝试"）
void ResetQueue::startNext()
{
    if (m_running >= 0) return;           // 同时只有一条 running（§9.2）

    while (!m_queue.isEmpty()) {
        const qint64 id = m_queue.takeFirst();
        const QString lockPath = lockPathFor(id);
        // 锁目录按需建（只在**真的轮到它**时才碰库目录 —— 卷闸门已由调用方在构造时验过）
        if (!QDir().mkpath(QFileInfo(lockPath).absolutePath())) {
            const QString why = QStringLiteral("无法创建锁目录：%1")
                                    .arg(QFileInfo(lockPath).absolutePath());
            m_reasons.insert(id, why);
            setState(id, ResetState::Failed);
            pushNotice(why);
            continue;
        }

        auto lock = std::make_unique<QLockFile>(lockPath);
        if (!lock->tryLock(0)) {
            // ★ 拿不到 ⇒ **不跨进程排队**（§9.5）：该集合直接 Failed + 提示，让给下一个
            const QString why = QStringLiteral("另一实例正在重置该图集（%1）").arg(id);
            m_reasons.insert(id, why);
            setState(id, ResetState::Failed);
            pushNotice(why);
            continue;
        }

        QString err;
        if (!m_hooks.begin || !m_hooks.begin(id, &err)) {
            lock->unlock();               // begin 失败 ⇒ 锁立刻还回去（不占着）
            const QString why = err.isEmpty()
                                    ? QStringLiteral("无法开始重置（%1）").arg(id) : err;
            m_reasons.insert(id, why);
            setState(id, ResetState::Failed);
            pushNotice(why);
            if (m_hooks.end) m_hooks.end(id);
            continue;
        }

        m_lock = std::move(lock);
        m_running = id;
        setState(id, ResetState::Running);
        return;
    }
}

void ResetQueue::finish(qint64 id, ResetState end, const QString &why)
{
    if (id != m_running) return;          // 只有 running 才有收尾（重复调用 = 无效）
    releaseLock();                        // §9.5：三种收尾都释放锁
    m_running = -1;
    if (!why.isEmpty()) m_reasons.insert(id, why);
    // 顺序：end（释放活序列）→ setState（通知外界终态）→ startNext（让位）。
    //   end 先于 setState：监听 stateChanged 的人看到终态时，那条活序列已经收好了。
    if (m_hooks.end) m_hooks.end(id);
    setState(id, end);
    startNext();                          // 把 running 位让给下一个（队首才试锁）
}

bool ResetQueue::finishCompleted(qint64 id)
{
    if (id != m_running) return false;
    QString err;
    const bool ok = (m_hooks.commit && m_hooks.commit(id, &err));
    finish(id, ok ? ResetState::Done : ResetState::Failed,
           ok ? QString()
              : (err.isEmpty() ? QStringLiteral("落库失败（旧内容保持不变）") : err));
    return ok;
}

void ResetQueue::finishStopped(qint64 id)
{
    if (id != m_running) return;
    qInfo().noquote() << QStringLiteral("[reset] 集合 %1 主动停止 ⇒ **不落库**").arg(id);
    finish(id, ResetState::Stopped, QString());
}

void ResetQueue::finishFailed(qint64 id, const QString &why)
{
    if (id != m_running) return;
    qWarning().noquote() << QStringLiteral("[reset] 集合 %1 失败 ⇒ **不落库**：%2").arg(id).arg(why);
    finish(id, ResetState::Failed, why);
}

} // namespace resetq
