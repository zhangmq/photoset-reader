#include "viewermodel.h"

#include <QDebug>
#include <QMetaObject>

namespace fetch {

// ══ 构造 / 析构 ═════════════════════════════════════════════════════════════
//   `prefetchLength` = 预取长度 L（两个方向的条数相加）。
ViewerModel::ViewerModel(int prefetchLength, QObject *parent)
    : QObject(parent),
      m_length(qMax(2, prefetchLength)),
      m_poster(std::make_shared<Poster>())
{
    m_poster->target = this;
    resetShare();                    // 起始：f = 6（夹到 [1, L−1]）、方向为正
}

//   **不 join 工作线程**（join 会把退出拖住几百毫秒）：**不加锁**地置失效记号 + 置空目标
//   —— 这两个写与在飞结果那条 lambda 里的读**同在主线程** ⇒ 不需要锁、不需要 atomic。
//   ★ 每个 `ViewerModel` 实例有**自己的一份 `Poster`**（由 `shared_ptr` 持有）：析构置完之后，
//     此后所有还在路上的结果**全部丢弃** —— 这是**正常路径**（`ViewerModel` 是短的、
//     随查看建了又毁；长期存在的只有 `preload` 里那个接收者）。
ViewerModel::~ViewerModel()
{
    m_poster->valid  = false;
    m_poster->target = nullptr;
}

// ══ 工作线程：加载并解码 → post 回主线程 ════════════════════════════════════
//   只做两件：`Sequence::image`（唯一解码实现）；解完把结果 post 回主线程。
//   不做取消；解码失败（空图）当成"坏（不可读）"。
//   ★ **不碰 `ViewerModel`、也不读 `valid` / `target`**：对象已销毁时该丢的由主线程那次检查丢掉
//     （多投一次无害）；析构**不 join**，所以这里绝不会有"等对象"的事。
void ViewerModel::preload(const Sequence::Item &imageMeta, int slot)
{
    const Sequence::Item meta  = imageMeta;   // ★ 工作线程只拿到一份条目信息的副本
    const quint64        epoch = m_epoch;     // 记号在这一刻定下（主线程读）
    const std::shared_ptr<Poster> poster = m_poster;

    //   ★ post 的**接收者 = 一个长期存在的主线程对象**（故意不析构 ⇒ 不会先死）：
    //     `ViewerModel` 先死也不影响它，排在事件队列里的结果照样能被投递到。
    //     第一次用到在主线程（`preload()` 就是主线程调的）⇒ 它就住在主线程。
    static QObject *const sink = new QObject;

    std::thread([poster, epoch, slot, meta] {
        const QImage image = Sequence::image(meta);
        //   ★ 工作线程到此为止：只投递、不看任何共享标志。丢掉与否在主线程判。
        QMetaObject::invokeMethod(sink, [poster, epoch, slot, image] {
            //   ★★ **第一步**：这份结果所属的那个 `ViewerModel` 还在不在？
            //     · **不在 ⇒ 把收到的东西整个抛弃** —— 不写状态、不 emit、不判、不派活、
            //       不碰任何槽位（`image` 随之释放）。这是**正常路径**，不是异常情况：
            //       每个 `ViewerModel` 都是短的（随查看建了又毁），长期存在的只有上面那个接收者。
            //     · 在 ⇒ 才继续走"比记号 → 比这个槽还等不等这一条"。
            //   ⚠️ 这里的丢弃**不打日志**（要留痕最多 debug 级，不许刷屏）。
            //   读 `valid` / `target` 与析构的写**同在主线程** ⇒ 无需锁、无需 atomic。
            if (!poster->valid || !poster->target) return;
            poster->target->onLoaded(epoch, slot, image);     // 换来源由 `onLoaded` 比记号丢掉
        }, Qt::QueuedConnection);
    }).detach();
}

// ══ 结果回主线程 ════════════════════════════════════════════════════════════
//   先比记号，再比"这个槽现在还等不等这一条"；任一条不符 ⇒ **直接丢**（白干，不算数）。
//   解码失败（空图）⇒ **坏因写进小表**（不可读），并**释放那个槽**（坏不占槽位额度）。
void ViewerModel::onLoaded(quint64 epoch, int slot, const QImage &image)
{
    if (epoch != m_epoch) return;                        // 换来源记号不符 ⇒ 丢
    if (slot < 0 || slot >= m_slots.size()) return;
    if (m_slots[slot].state != Slot::State::InFlight) return;   // 这个槽已经不等它了 ⇒ 丢

    Slot &s = m_slots[slot];
    if (image.isNull()) {                                // 解码失败 = 坏（不可读）
        markBad(s.item, Bad::Unreadable);
        s.item  = -1;
        s.state = Slot::State::Empty;
        s.image = QImage();
    } else {
        s.image = image;
        s.state = Slot::State::Ready;
    }
    --m_inFlight;
    judge();                                             // 重判一次（里面也会继续派活）
}

// ══ 来源 / 序列长进了 ═══════════════════════════════════════════════════════
//   换来源 ⇒ **换个记号就走**：槽位表 + 坏因表整张换掉、在飞计数归零，回到起始（位置 0）。
void ViewerModel::setSequence(const Sequence *seq)
{
    ++m_epoch;                       // 换来源记号：在飞的回来时记号不符 ⇒ 丢
    m_seq = seq;
    m_slots.clear();
    m_bad.clear();                   // 坏因的生命周期 = 一次查看（换来源一起清）
    m_inFlight = 0;
    m_skipped  = 0;
    m_pos      = 0;
    m_cursor   = 0;
    resetShare();                    // 换来源 ⇒ 回到起始
    emit currentChanged();           // 条目总数变了（QML 的 count / totalPages 跟它）
    judge();
}

//   序列长进了：条目总数可能变了 ⇒ 通知一次，再判一次（可能这一条现在有了 / 能往后走了）。
void ViewerModel::sequenceChanged()
{
    emit currentChanged();
    judge();
}

// ══ 每个动作（stepPage / jump）═════════════════════════════════════════════
//   就三件：改位置 → 当场判 → 备好就上屏、没备好就派活再等。
//   不做排队：一切走主线程正常的 Qt 事件逻辑，一次按键事件处理一次。
//   翻页多这一条判断：**当前要显示的那一条还没解好（既没备好、也不是坏）** ⇒ 这次按键作废——
//   丢弃（不推进位置、不算一次操作），不在之后补上屏。
//   ★ 这一条**当场算**，不看 `loading`：`loading` 只管界面浮层（等图片缓存时是假），
//     拿它来挡按键会让等待期间全放行 ⇒ 一路跳着上屏（跳过一整串）。
void ViewerModel::stepPage(int delta)
{
    if (!m_seq) return judge();
    const int n = m_seq->count();
    if (n <= 0) return judge();
    const int s = slotOf(m_pos);
    const bool ready = (s >= 0 && m_slots[s].state == Slot::State::Ready);
    if (!ready && !isBad(m_pos)) return;   // 当前要显示的那一条还没解好 ⇒ 这次按键丢掉
    act(qBound(0, m_pos + delta, n - 1));
}

void ViewerModel::jump(int imageIndex)
{
    if (!m_seq) return judge();
    const int n = m_seq->count();
    if (n <= 0) return judge();
    act(qBound(0, imageIndex, n - 1));
}

void ViewerModel::act(int target)
{
    if (target != m_pos) {
        observeOperation(target);    // 位置真的变了 ⇒ 才算一次操作
        m_pos = target;
    }
    judge();
}

// ══ 份额规则 ═══════════════════════════════════════════════════════════════
//   同向再走一步 ⇒ `f+1`；反向 ⇒ `f−1`；夹在 `[1, L−1]`。
//   跳转（位置不是相邻一条）⇒ 回到起始（`f = 6`、方向为正）。
void ViewerModel::observeOperation(int to)
{
    const int delta = to - m_pos;
    if (delta == 0) return;                                     // 只是重判 ⇒ 不算一次操作
    if (delta != 1 && delta != -1) return resetShare();         // 跳转 ⇒ 回到起始

    const int dir = (delta > 0) ? +1 : -1;
    if (dir != m_dir) m_dir = dir;                              // 反向 ⇒ 新方向挪到前面
    m_f = qBound(1, m_f + ((dir > 0) ? +1 : -1), m_length - 1); // 前向条数只按方向 ±1
}

void ViewerModel::resetShare()
{
    m_f   = qBound(1, kStartForward, m_length - 1);
    m_dir = +1;
}

// ══ 派活（主线程）══════════════════════════════════════════════════════════
//   按"当前这一条 → 份额那串偏移"的顺序逐条查：坏因表里已有 ⇒ 跳过（**永不重派 = 不重试**）；
//   槽位表里已在等 / 已备好 ⇒ 跳过；元数据已标缺失 ⇒ **只写坏因表，不派解码**；
//   剩下的找一个空槽，记下"这个槽等的是这一条"，在飞计数 +1，起一条工作线程去 `preload`。
//   ★ **当前这一条永远最先派出去**（备图不许拖慢当前这张）。
void ViewerModel::dispatch()
{
    if (!m_seq) return;
    const int n = m_seq->count();
    if (n <= 0) return;

    //   要的条目 = 当前这一条之外的那 L 条（+1..+f 与 −1..−(L−f)）+ 当前这一条；
    //   最近方向那一侧的条目排在前面。
    const int f = m_f;
    const int b = m_length - f;
    QVector<int> want;
    want.reserve(m_length + 1);
    want << m_pos;
    if (m_dir > 0) {
        for (int k = 1; k <= f; ++k) want << m_pos + k;
        for (int k = 1; k <= b; ++k) want << m_pos - k;
    } else {
        for (int k = 1; k <= b; ++k) want << m_pos - k;
        for (int k = 1; k <= f; ++k) want << m_pos + k;
    }

    for (int i : want) {
        if (i < 0 || i >= n) continue;                  // 没有这一条
        if (isBad(i)) continue;                         // 已知是坏的 ⇒ 跳过（**永不重派**）
        if (slotOf(i) >= 0) continue;                   // 已在等 / 已备好 ⇒ 跳过

        const Sequence::Item &it = m_seq->at(i);
        if (it.missing) {                               // 元数据已标缺失 ⇒ 不派解码，只写坏因
            markBad(i, Bad::Missing);
            continue;
        }
        if (m_inFlight >= kMaxInFlight) return;         // 并发上限（写死 4）
        const int s = takeSlot(i);
        if (s < 0) return;                              // 池满且没有可淘汰的（在飞的槽不可淘汰）
        m_slots[s].state = Slot::State::InFlight;
        ++m_inFlight;
        preload(it, s);
    }
}

//   管着这一条的槽（-1 = 没有）。
int ViewerModel::slotOf(int item) const
{
    for (int s = 0; s < m_slots.size(); ++s)
        if (m_slots[s].item == item && m_slots[s].state != Slot::State::Empty) return s;
    return -1;
}

//   找一个槽给这一条：先找空槽；池满（上限 = L + 1）⇒ 丢掉"离当前位置最远、且已备好"的那个槽；
//   找不到（没有可淘汰的备好槽）⇒ -1。
//   ★ 池里只有"在飞 / 备好"两种槽 ⇒ 不会再有"不可淘汰的坏槽"把池堵死（坏因不在池里）。
int ViewerModel::takeSlot(int item)
{
    for (int s = 0; s < m_slots.size(); ++s) {
        if (m_slots[s].state == Slot::State::Empty) {
            m_slots[s].item = item;
            return s;
        }
    }
    if (m_slots.size() < m_length + 1) {
        Slot s;
        s.item = item;
        m_slots << s;
        return m_slots.size() - 1;
    }
    int victim = -1, farthest = -1;
    for (int s = 0; s < m_slots.size(); ++s) {
        if (m_slots[s].state != Slot::State::Ready) continue;
        if (m_slots[s].item == m_pos) continue;      // 当前这一条（距离 0 = 最近）不离手
        const int d = qAbs(m_slots[s].item - m_pos);
        if (d > farthest) { farthest = d; victim = s; }
    }
    if (victim < 0) return -1;
    m_slots[victim].item  = item;
    m_slots[victim].state = Slot::State::Empty;
    m_slots[victim].image = QImage();
    return victim;
}

//   坏因小表（**不占** `L + 1` 的槽位额度）；**两个写点的唯一入口**：
//   元数据已标缺失（`dispatch` 里当场写）/ 解码回来是空图（`onLoaded` 里写）。
bool ViewerModel::isBad(int item) const
{
    return m_bad.contains(item);
}

void ViewerModel::markBad(int item, Bad why)
{
    m_bad.insert(item, why);
}

// ══ 重判：唯一决定上屏的地方 ═══════════════════════════════════════════════
//   · 当前这一条的槽位是"备好"          ⇒ 上屏；
//   · 坏因表里记着当前这一条（**查小表**）⇒ 跳过计数 +1，位置往后挪一条（同方向），再派活再判；
//   · 还没有结论                        ⇒ 等下一个结果回来。
//   每一轮开头都更新一次浮层（`updateLoading()`）⇒ 所有返回路径都更新到，没有早退漏掉。
void ViewerModel::judge()
{
    for (;;) {
        updateLoading();                       // 浮层按当前状态当场算（唯一更新入口）
        if (!m_seq || m_seq->count() <= 0) return;   // 还没有条目（扫描中 = 浮层显示，扫完 = 不显示）
        if (isBad(m_pos)) {                   // 当前这一条坏 ⇒ 查小表（不再看槽的状态）
            ++m_skipped;
            const int next = m_pos + m_dir;   // 往后挪一条（同方向）
            if (next < 0 || next >= m_seq->count()) {
                qInfo().noquote() << QStringLiteral("[viewer] 第 %1 条是坏的，且后面没有了"
                                                    "（累计跳过 %2）")
                                         .arg(m_pos).arg(m_skipped);
                return;
            }
            qInfo().noquote() << QStringLiteral("[viewer] 第 %1 条是坏的 ⇒ 跳过（累计 %2）")
                                     .arg(m_pos).arg(m_skipped);
            observeOperation(next);           // 同方向再走一步
            m_pos = next;
            dispatch();                       // 再派活
            continue;                         // 再判
        }
        const int s = slotOf(m_pos);
        if (s >= 0 && m_slots[s].state == Slot::State::Ready) {
            screen(m_slots[s].image);
            dispatch();                       // 上屏之后接着按份额备图
            return;
        }
        //   还没有结论（没有槽位 / 正在等）⇒ 等下一个结果回来；派活当场给出结论
        //   （缺失 ⇒ 写坏因）就再判一次
        dispatch();
        if (isBad(m_pos)) continue;           // 派活当场写出坏因（缺失）⇒ 再判
        const int again = slotOf(m_pos);
        if (again >= 0 && m_slots[again].state != Slot::State::InFlight) continue;
        return;
    }
}

//   上屏：写 `imageA` / `keyCount` / `cursor`，emit 三个信号，再更新一次浮层。
//   ★ 位置变过、但这一条还没备好时**不清 `imageA`**（不许先显示空图或上一张）。
void ViewerModel::screen(const QImage &image)
{
    m_imageA = image;
    m_imageB = QImage();                      // 单幅：恒空图
    m_keyCount = 1;
    m_cursor = m_pos;
    updateLoading();                          // 上屏之后也更新一次（浮层与新口径无关，只是不漏路径）
    emit imageAChanged();
    emit imageBChanged();
    emit currentChanged();
}

//   loading（界面浮层）的**唯一写点**，当场按当前状态算：
//     还没有序列
//     或（还没扫完 且（还没有条目 或 位置已处在已知末尾））
//   · notReady 那一半：第一批条目到达（count > 0）就不再成立 ⇒ **自动解除**（不等解码）；
//   · 到达边界那一半：下一批把末尾推走、或扫描结束，就不再成立 ⇒ **自动解除**
//     —— 没有任何显式的进入 / 解除状态；
//   · 扫描结束哪怕末尾没变也不再显示（不用再等了）；扫完还没有条目（空目录）⇒ 假；
//   · **正常等图片缓存（当前这条还没解好）⇒ 假**。
void ViewerModel::updateLoading()
{
    bool on = true;                           // 还没有序列
    if (m_seq) {
        on = !m_seq->scanFinished()
             && (m_seq->count() <= 0 || m_pos >= m_seq->count() - 1);
    }
    if (m_loading == on) return;
    m_loading = on;
    emit loadingChanged();
}

} // namespace fetch
