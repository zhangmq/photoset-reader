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

// ══ 版面 / 屏 ══════════════════════════════════════════════════════════════
//   版面 = `app::Layout`（一份枚举）：**单幅** ⇒ 一屏就是这一条；**双幅** ⇒ 一屏是"这一对"。
//   双幅那一对的位置由**序列的配对表**给（`pairStart` / `pairAt`），本类不自己配对、不算张数。
// 这一条所在那一屏的起点（双幅下落在配对第二张 ⇒ 回到它所属的那一对）。
int ViewerModel::screenStart(int item) const
{
    if (!m_seq) return 0;
    if (m_layout == app::Layout::Single) return qBound(0, item, qMax(0, m_seq->count() - 1));
    const int s = m_seq->pairStart(item);
    return (s < 0) ? 0 : s;
}

// 本屏的位置（单幅 1 条 / 双幅 = 本对，宽图或末尾也是 1 条）——**照抄序列的配对表**，不挑不滤。
QVector<int> ViewerModel::screenItems(int start) const
{
    if (!m_seq) return QVector<int>();
    if (m_layout == app::Layout::Single) return QVector<int>{ qBound(0, start, qMax(0, m_seq->count() - 1)) };
    return m_seq->pairAt(start).members;
}

// ★★ **换一屏 = 唯一的"换屏算术"**（`stepPage` 与"整屏坏 ⇒ 跳过"共用这一处）：
//   先问序列要"相邻的那一屏"；到头（序列尽头 **或** 目标 index 出了本次循环的区间）
//   再按循环模式回绕。判定**只看 index**（用户 2026-10-18："不需要按屏讨论问题，
//   当前 index 是谁就以谁为准"）。
int ViewerModel::nextScreen(int start, int dir) const
{
    if (!m_seq) return -1;
    const int n = m_seq->count();
    if (n <= 0) return -1;

    int next = (m_layout == app::Layout::Double)
                   ? ((dir > 0) ? m_seq->nextPairStart(start) : m_seq->prevPairStart(start))
                   : ((dir > 0) ? ((start + 1 < n) ? start + 1 : -1)
                                : ((start - 1 >= 0) ? start - 1 : -1));
    //   文件夹循环：目标出了区间也算到头
    if (next >= 0 && !inLoopScope(next)) next = -1;
    if (next >= 0) return next;
    if (m_loop == Loop::None) return -1;

    const int target = (m_loop == Loop::All)
                           ? ((dir > 0) ? 0 : lastScreenStart())
                           : wrapTarget(dir);
    if (target < 0 || screenStart(target) == start) return -1;   // 回绕到同一屏 ⇒ 当到头
    return target;
}

//   ★ 循环的区间 = **当前那一条所在容器**的子树区间（O(1)，序列的 `ownerRange` 给）。
QPair<int, int> ViewerModel::loopScope() const
{
    const int n = m_seq ? m_seq->count() : 0;
    if (!m_seq || n <= 0) return { 0, 0 };
    return m_seq->ownerRange(qBound(0, m_pos, n - 1));
}

bool ViewerModel::inLoopScope(int item) const
{
    if (m_loop != Loop::Folder) return true;        // 不循环 / 全部循环：整个序列都是区间
    const QPair<int, int> r = loopScope();
    return item >= r.first && item < r.first + r.second;
}

int ViewerModel::lastScreenStart() const
{
    const int n = m_seq ? m_seq->count() : 0;
    if (n <= 0) return -1;
    return screenStart(n - 1);
}

//   文件夹循环的回绕目标 = **区间里那一条**（第一条 / 最后一条）。
//   ★ 回的是**条目**而不是屏起点：跨边界那一屏的起点可能落在区间外，
//     若把 m_pos 落在那里，下一次"当前 index 所在容器"就会算到隔壁文件夹去。
//     屏照旧由它派生（`screenStart(m_pos)`），区间外那一格留空。
int ViewerModel::wrapTarget(int dir) const
{
    const QPair<int, int> r = loopScope();
    if (r.second <= 0) return -1;
    return (dir > 0) ? r.first : (r.first + r.second - 1);
}

// ══ 上一个 / 下一个文件夹（**规则在视图**：序列只给事实 + 两个序）══════════
//
//   候选 = `Sequence::containers()`（根不在里面）；**包含当前 index 的容器距离是负数/零**
//   ⇒ 排除（你正躺在它里面）⇒ "在深层"与"在上层"候选集合本来就不同（看当前状态）。
//      · 下一个 = `first_index > 当前 index` 里**最近**的（first 最小）；
//      · 上一个 = `last_index < 当前 index` 里**最近**的（last 最大）；
//      · 距离并列（同一个 first / 同一个 last ⇒ 嵌套链）⇒ 取**最上层**（`level` 最小）
//        —— 两个序都把并列的**最上层排在块首**，所以取块首就是它；
//      · 落点 = 那个容器的**第一条图**；名字 = 那个容器**自己**的 label。
//   两个序都在序列里**建好**（容器收尾那一刻）⇒ 这里只做二分，O(log 容器数)。
int ViewerModel::adjacentFolderNode(int dir) const
{
    if (!m_seq) return -1;
    const int n = m_seq->count();
    if (n <= 0) return -1;
    const int cur = qBound(0, m_pos, n - 1);
    const QVector<Sequence::Container> &cs = m_seq->containers();

    if (dir > 0) {
        const QVector<int> &idx = m_seq->containersByFirst();
        const auto it = std::upper_bound(idx.cbegin(), idx.cend(), cur,
                                         [&cs](int value, int ci) { return value < cs.at(ci).first; });
        if (it == idx.cend()) return -1;
        return cs.at(*it).node;                    // 并列块的**块首** = 最上层
    }
    const QVector<int> &idx = m_seq->containersByLast();
    const auto it = std::lower_bound(idx.cbegin(), idx.cend(), cur,
                                     [&cs](int ci, int value) { return cs.at(ci).last < value; });
    if (it == idx.cbegin()) return -1;
    int pos = int(it - idx.cbegin()) - 1;          // 最后一个 last < cur
    const int key = cs.at(idx.at(pos)).last;
    while (pos > 0 && cs.at(idx.at(pos - 1)).last == key) --pos;   // 回到并列块的块首
    return cs.at(idx.at(pos)).node;
}

//   全部循环的回绕目标：第一个 / 最后一个容器（块首同样是最上层）。
int ViewerModel::edgeFolderNode(int dir) const
{
    if (!m_seq) return -1;
    const QVector<Sequence::Container> &cs = m_seq->containers();
    const QVector<int> &idx = (dir > 0) ? m_seq->containersByFirst() : m_seq->containersByLast();
    if (idx.isEmpty()) return -1;
    return cs.at(dir > 0 ? idx.first() : idx.last()).node;
}

QString ViewerModel::folderNameOfNode(int node) const
{
    if (!m_seq || node < 0 || node >= m_seq->tree().size()) return QString();
    return m_seq->tree().at(node).label;            // 容器**自己**那一层的名字（单段）
}

QString ViewerModel::prevFolderName() const
{
    // ★ **文件夹内循环 ⇒ 两个按钮禁用置灰**（用户 2026-10-19）：既然只看这一个文件夹，
    //   就没有"上一个 / 下一个文件夹"可切（以前这里会拿当前文件夹名顶上、
    //   还会跳到区间外）。区间内回绕照旧由 ← → / ⏮⏭（换屏）承担。
    if (!m_seq || m_seq->count() <= 0 || m_loop == Loop::Folder) return QString();
    const int node = adjacentFolderNode(-1);
    if (node >= 0) return folderNameOfNode(node);
    if (m_loop == Loop::All) return folderNameOfNode(edgeFolderNode(-1));
    return QString();
}

QString ViewerModel::nextFolderName() const
{
    if (!m_seq || m_seq->count() <= 0 || m_loop == Loop::Folder) return QString();
    const int node = adjacentFolderNode(1);
    if (node >= 0) return folderNameOfNode(node);
    if (m_loop == Loop::All) return folderNameOfNode(edgeFolderNode(1));
    return QString();
}

//   跳相邻容器的第一条（到头按循环模式；`delta` = ±1）
void ViewerModel::stepFolder(int delta)
{
    if (!m_seq || m_seq->count() <= 0) return judge();
    if (m_loop == Loop::Folder) return judge();     // ★ 文件夹内循环：不许切文件夹
    int node = adjacentFolderNode(delta);
    if (node < 0) {
        if (m_loop == Loop::All) node = edgeFolderNode(delta);
        else return judge();                        // 不循环：到头（按钮本来就是灰的）
    }
    if (node < 0) return judge();
    const int target = m_seq->subtreeFirst(node);   // 落点 = 那个容器的第一条图
    if (target < 0) return judge();
    act(target, 0);
}

//   换循环模式：会话级、不写盘；**只重判一次**（不重置份额）。
void ViewerModel::setLoopMode(int mode)
{
    const Loop want = (mode == int(Loop::All))    ? Loop::All
                    : (mode == int(Loop::Folder)) ? Loop::Folder
                                                  : Loop::None;
    if (m_loop == want) return;
    m_loop = want;
    emit loopModeChanged();
    judge();
}

// 这一条在不在当前屏上（淘汰槽位时不离手）。
bool ViewerModel::onScreen(int item) const
{
    const QVector<int> items = screenItems(screenStart(m_pos));
    return items.contains(item);
}

// ══ 每个动作（stepPage / jump / 换版面）═════════════════════════════════════
//   就三件：改位置 → 当场判 → 就位就上屏、没就位就派活再等。
//   不做排队：一切走主线程正常的 Qt 事件逻辑，一次按键事件处理一次。
//   翻页多这一条判断：**本屏还有位置没结论（既没备好、也不是坏）** ⇒ 这次按键作废——
//   丢弃（不换屏、不算一次操作），不在之后补上屏。**坏算"有结论"** ⇒ 那时放行。
//   ★ 这一条**当场算**，不看 `loading`：`loading` 只管界面浮层（等图片缓存时是假），
//     拿它来挡按键会让等待期间全放行 ⇒ 一路跳着上屏（跳过一整串）。
void ViewerModel::stepPage(int delta)
{
    if (!m_seq) return judge();
    if (m_seq->count() <= 0) return judge();
    const int start = screenStart(m_pos);

    bool settled = false, anyReady = false;
    scanScreen(screenItems(start), settled, anyReady);
    if (!settled) return;                     // 本屏还没结论 ⇒ 这次按键丢掉

    const int dir = (delta > 0) ? +1 : -1;
    const int next = nextScreen(start, dir);      // 到头 ⇒ -1（位置不动）
    if (next < 0) return judge();
    act(next, dir);
}

void ViewerModel::jump(int imageIndex)
{
    if (!m_seq) return judge();
    const int n = m_seq->count();
    if (n <= 0) return judge();
    act(qBound(0, imageIndex, n - 1), 0);     // 跳转 ⇒ 份额回到起始
}

//   换版面：**保留当前位置**，屏由序列按新版面重算（不重新分组）⇒ 再判一次。
//   预取份额回到起始（`f = 6`、方向为正）。
void ViewerModel::setDoublePage(bool on)
{
    const app::Layout want = on ? app::Layout::Double : app::Layout::Single;
    if (m_layout == want) return;
    m_layout = want;
    resetShare();
    emit doublePageChanged();
    judge();
}

//   `stepDir` = 这次是"换一屏"（±1）；0 = 跳转 / 换版面。
void ViewerModel::act(int target, int stepDir)
{
    if (target != m_pos) {
        if (stepDir == 0) resetShare();       // 跳转 / 换版面 ⇒ 回到起始
        else observeOperation(stepDir);       // 位置真的变了 ⇒ 才算一次操作
        m_pos = target;
    }
    judge();
}

// ══ 份额规则 ═══════════════════════════════════════════════════════════════
//   同向再换一屏 ⇒ `f+1`；反向 ⇒ `f−1`；夹在 `[1, L−1]`。
void ViewerModel::observeOperation(int dir)
{
    if (dir != m_dir) m_dir = dir;                              // 反向 ⇒ 新方向挪到前面
    m_f = qBound(1, m_f + ((dir > 0) ? +1 : -1), m_length - 1); // 前向条数只按方向 ±1
}

void ViewerModel::resetShare()
{
    m_f   = qBound(1, kStartForward, m_length - 1);
    m_dir = +1;
}

// ══ 派活（主线程）══════════════════════════════════════════════════════════
//   按"本屏位置 → 份额那串偏移"的顺序逐条查：坏因表里已有 ⇒ 跳过（**永不重派 = 不重试**）；
//   槽位表里已在等 / 已备好 ⇒ 跳过；元数据已标缺失 ⇒ **只写坏因表，不派解码**；
//   剩下的找一个空槽，记下"这个槽等的是这一条"，在飞计数 +1，起一条工作线程去 `preload`。
//   ★ **本屏的位置永远最先派出去**（备图不许拖慢当前这一屏）。
void ViewerModel::dispatch()
{
    if (!m_seq) return;
    const int n = m_seq->count();
    if (n <= 0) return;

    //   要的条目 = **本屏的位置** + 顺着"下一屏 / 上一屏"各走 f / b 屏（最近方向那侧在前）。
    //   ★★ **不按下标加减硬算**（用户 2026-10-15 定的 L38）：每一步都过 `nextScreen`
    //      ⇒ 开了循环就**跟着绕回开头**（到末尾附近时，开头的条目也在队列里）。
    const QVector<int> items = screenItems(screenStart(m_pos));
    if (items.isEmpty()) return;
    const int start = items.first();

    const int f = m_f;
    const int b = m_length - f;
    QVector<int> want;
    want.reserve(m_length + 2);
    want << items;                                   // 本屏（1 或 2 个位置）
    const auto walk = [&](int dir, int steps) {
        int cur = start;
        for (int k = 0; k < steps; ++k) {
            cur = nextScreen(cur, dir);
            if (cur < 0) return;
            const QVector<int> its = screenItems(cur);
            if (its.isEmpty()) return;
            want << its;
        }
    };
    if (m_dir > 0) { walk(+1, f); walk(-1, b); }
    else           { walk(-1, b); walk(+1, f); }

    for (int i : want) {
        if (i < 0 || i >= n) continue;                  // 没有这一条
        if (!inLoopScope(i)) continue;                  // ★ 区间外 ⇒ 不派活（留空，不白解码）
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

//   这一条备好时的那张图（坏 / 在飞 / 没槽 ⇒ 空图）。
QImage ViewerModel::imageOf(int item) const
{
    if (item < 0) return QImage();
    if (!inLoopScope(item)) return QImage();         // ★ 区间外 ⇒ **留空**（不是坏，只是本次不显示）
    const int s = slotOf(item);
    if (s < 0 || m_slots[s].state != Slot::State::Ready) return QImage();
    return m_slots[s].image;
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
//   ★ **本屏的哪些条不离手**（双幅 2 条都算）。
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
        if (onScreen(m_slots[s].item)) continue;     // 当前屏的那几条不离手
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
//   · 本屏每个位置都有结论（备好 / 坏）**且至少一个备好** ⇒ 上屏（坏的位置留空图）；
//   · 本屏**整屏都坏**（单幅 = 那一条坏）  ⇒ 跳过计数 +1，位置挪到下一屏（同方向），再派活再判；
//   · 还有位置没结论                      ⇒ 等下一个结果回来。
//   每一轮开头都更新一次浮层（`updateLoading()`）⇒ 所有返回路径都更新到，没有早退漏掉。
void ViewerModel::judge()
{
    const int origin = screenStart(m_pos);     // 出发那一屏（跳过坏屏时"绕一圈回到它就停"）
    for (;;) {
        updateLoading();                       // 浮层按当前状态当场算（唯一更新入口）
        if (!m_seq || m_seq->count() <= 0) return;   // 还没有条目（扫描中 = 浮层显示，扫完 = 不显示）

        const int start = screenStart(m_pos);
        const QVector<int> items = screenItems(start);
        if (items.isEmpty()) return;

        bool settled = false, anyReady = false;
        scanScreen(items, settled, anyReady);
        if (settled) {
            if (anyReady) {
                screen(start, items);
                dispatch();                    // 上屏之后接着按份额备图
                return;
            }
            ++m_skipped;                       // 整屏坏 ⇒ 跳过（按当前方向）
            const int next = nextScreen(start, m_dir);      // 跳过也按当前方向换屏
            if (next < 0 || next == origin) {               // 到头 / 绕一圈回到出发那屏 ⇒ 停
                qInfo().noquote() << QStringLiteral("[viewer] 本屏（起点 %1）是坏的，且后面没有了"
                                                    "（累计跳过 %2）")
                                         .arg(start).arg(m_skipped);
                return;
            }
            qInfo().noquote() << QStringLiteral("[viewer] 本屏（起点 %1）是坏的 ⇒ 跳过（累计 %2）")
                                     .arg(start).arg(m_skipped);
            observeOperation(m_dir);           // 同方向再换一屏
            m_pos = next;
            dispatch();                        // 再派活
            continue;                          // 再判
        }
        //   还没结论（没有槽位 / 正在等）⇒ 等下一个结果回来；派活当场给出结论
        //   （缺失 ⇒ 写坏因）就再判一次
        dispatch();
        bool settled2 = false, anyReady2 = false;
        scanScreen(items, settled2, anyReady2);
        if (settled2) continue;                // 派活当场写出坏因（缺失）⇒ 再判
        return;
    }
}

//   本屏每个位置的状态：`settled` = 都有结论（备好 / 坏）；`anyReady` = 至少一个备好。
void ViewerModel::scanScreen(const QVector<int> &items, bool &settled, bool &anyReady) const
{
    settled  = true;
    anyReady = false;
    for (int i : items) {
        if (!inLoopScope(i)) continue;                   // 区间外：留空 = 有结论
        if (isBad(i)) continue;                          // 坏 = 有结论
        const int s = slotOf(i);
        if (s >= 0 && m_slots[s].state == Slot::State::Ready) { anyReady = true; continue; }
        settled = false;                                 // 既没备好、也不是坏 ⇒ 还在等
    }
}

//   上屏：按**位置**写 `imageA` / `imageB`（备好 ⇒ 那张图；坏 / 没有第二个位置 ⇒ 空图），
//   写 `keyCount`（本屏位置数）与 `cursor`（本屏起点），emit 信号，再更新一次浮层。
//   ★ 位置变过、但这一屏还没就位时**不清 `imageA` / `imageB`**（不许先显示空图或上一张）。
void ViewerModel::screen(int start, const QVector<int> &items)
{
    m_imageA = imageOf(items.value(0, -1));
    m_imageB = imageOf(items.value(1, -1));
    m_keyCount = items.size();
    m_cursor = start;
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
