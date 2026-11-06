#include "viewermodel.h"
#include "logging.h"

#include "exifmeta.h"
#include "gpuscale.h"
#include "placeholder.h"

#include <QCoreApplication>
#include <QDebug>
#include <QColor>
#include <QPainter>
#include <QPen>
#include <QVariantMap>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <chrono>

namespace fetch {

//   ★ 生产的时间源 = **单调时钟**（`steady_clock`）：系统时间跳变不影响节拍；
//     自检可以 `setNowFn` 注入虚拟时钟（节奏精确验证，不靠真实 sleep）。
static qint64 steadyNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ══ 构造 / 析构 ═════════════════════════════════════════════════════════════
//   `prefetchLength` = 预取长度 L（按**图**计；0 合法 = 只备本屏）。
ViewerModel::ViewerModel(int prefetchLength, QObject *parent)
    : QObject(parent),
      m_length(qMax(0, prefetchLength)),
      m_poster(std::make_shared<Poster>())
{
    m_poster->target = this;
    m_dir = +1;                      // 起始方向：前向
    m_now = steadyNowMs;
    //   验收用（`--log-level debug`）：预取深度最终取自设置，值不对时这一行就能看出来
    qCDebug(lcUi).noquote() << QStringLiteral("[ui] viewer: prefetch window L=%1").arg(m_length);
    //   ★ 单发闹钟：只负责"到点叫醒"。**不是节拍器** —— 节奏由单调时钟算
    //     （`到期 = 请求时刻 + 间隔`），所以默认精度够用（Qt 定时器只会晚到、不会早到）。
    m_beat = new QTimer(this);
    m_beat->setSingleShot(true);
    m_beat->setTimerType(Qt::CoarseTimer);
    connect(m_beat, &QTimer::timeout, this, &ViewerModel::onBeat);
    //   ★ 每秒体检（用户自己跑 5 秒对比预取 0/8 时看这一行）：只打日志，不参与任何调度。
    m_stat = new QTimer(this);
    m_stat->setInterval(1000);
    m_stat->setTimerType(Qt::CoarseTimer);
    connect(m_stat, &QTimer::timeout, this, &ViewerModel::onStatTick);
    m_stat->start();
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

// ══ 占位图（2026-11-05）══════════════════════════════════════════════════════
//   有字节、但解码器给不出图 ⇒ 画占位图。**实现搬到了 `placeholder.h`**（2026-11-06）：
//   预览条也要给坏图画占位（按 90px 直接画，绝不"画 1080 再缩"），两处必须同一份几何。
//   尺寸规则也变了（用户定）：有原图尺寸 ⇒ 等比装进 1920×1080；没有 ⇒ 1080×1080 方图。
//   ★ 只读入参、不碰 `this` ⇒ 仍在**工作线程**里调（GUI 线程不做像素活）。

// ══ 工作线程：加载并解码 → post 回主线程 ════════════════════════════════════
//   只做三件：`imageread::load`（唯一解码实现）；**解码失败但字节在 ⇒ 画占位图**；
//   解完把结果 post 回主线程。
//   ★ **不碰 `ViewerModel`、也不读 `valid` / `target`**：对象已销毁时该丢的由主线程那次检查丢掉
//     （多投一次无害）；析构**不 join**，所以这里绝不会有"等对象"的事。
void ViewerModel::preload(const Sequence::Item &imageMeta, int item)
{
    const Sequence::Item meta  = imageMeta;   // ★ 工作线程只拿到一份条目信息的副本
    const quint64        epoch = m_epoch;     // 记号在这一刻定下（主线程读）
    const std::shared_ptr<Poster> poster = m_poster;

    //   ★ post 的**接收者 = 一个长期存在的主线程对象**（故意不析构 ⇒ 不会先死）：
    //     `ViewerModel` 先死也不影响它，排在事件队列里的结果照样能被投递到。
    //     第一次用到在主线程（`preload()` 就是主线程调的）⇒ 它就住在主线程。
    static QObject *const sink = new QObject;

    //   这次解码**手里有没有密码**（嵌套时密码可能只在某一层，不能只看第 0 项）
    const bool passwordUsed = std::any_of(meta.loc.passwords.cbegin(), meta.loc.passwords.cend(),
                                          [](const QString &pw) { return !pw.isEmpty(); });
    std::thread([poster, epoch, item, meta, passwordUsed] {
        int passwordLevel = -1;
        const imageread::Loaded r = imageread::load(meta.loc);
        QImage image = r.image;
        passwordLevel = r.passwordLevel;
        //   ★ 有字节、解不出图、也不是密码问题 ⇒ **占位图**（结论 = "这一条有图"）。
        //     只在这里做（工作线程）：GUI 线程不该为一张占位图刷几百万像素。
        if (image.isNull() && passwordLevel < 0 && r.hasBytes) image = placeholder::forScreen(meta.size);
        //   ★ 工作线程到此为止：只投递、不看任何共享标志。丢掉与否在主线程判。
        QMetaObject::invokeMethod(sink, [poster, epoch, item, image, passwordLevel, passwordUsed] {
            //   ★★ **第一步**：这份结果所属的那个 `ViewerModel` 还在不在？
            //     · **不在 ⇒ 把收到的东西整个抛弃** —— 不写状态、不 emit、不判、不派活、
            //       不碰任何条目（`image` 随之释放）。这是**正常路径**，不是异常情况：
            //       每个 `ViewerModel` 都是短的（随查看建了又毁），长期存在的只有上面那个接收者。
            //     · 在 ⇒ 才继续走"比记号 → 比条目表里还要不要这一条"。
            //   ⚠️ 这里的丢弃**不打日志**（要留痕最多 debug 级，不许刷屏）。
            //   读 `valid` / `target` 与析构的写**同在主线程** ⇒ 无需锁、无需 atomic。
            if (!poster->valid || !poster->target) return;
            poster->target->onLoaded(epoch, item, image, passwordLevel, passwordUsed);
        }, Qt::QueuedConnection);
    }).detach();
}

//   设置里改「预取窗口」：**即时生效**，不重建 viewmodel、不重扫。
//   ★ `0` 合法（用户 2026-11-05：预取深度 0 = 只备本屏，前端行为不许变）。
void ViewerModel::setPrefetchWindow(int prefetchLength)
{
    const int v = qMax(0, prefetchLength);
    if (v == m_length) return;
    //   验收用（`--log-level debug`）：改设置时能看到它真的传到了 viewer
    qCInfo(lcUi).noquote()
        << QStringLiteral("[ui] viewer: prefetch window L=%1 -> %2").arg(m_length).arg(v);
    m_length = v;
    evaluateScreen();
    reconcile();                 // 按新窗口重拍（多要的照派；少要的丢掉）
}

// ══ 结果回主线程 ════════════════════════════════════════════════════════════
//   先比记号，再比"条目表里还要不要这一条"；任一条不符 ⇒ **直接丢**（白干，不算数）。
//   ★ 三种"没图"分开处理（2026-11-05）：**等密码 / 缺 / 占位图**（占位图已在工作线程画好，
//     到这里就是一张普通图）。
void ViewerModel::onLoaded(quint64 epoch, int item, const QImage &image, int passwordLevel,
                           bool passwordUsed)
{

    if (epoch != m_epoch) return;                        // 换来源记号不符 ⇒ 丢
    if (m_inFlight > 0) --m_inFlight;                    // 那条线程已经跑完（无论要不要）

    auto it = m_entries.find(item);
    if (it == m_entries.end() || it->state != Entry::State::InFlight) {
        reconcile();                                     // 已经不想要了 ⇒ 白干
        return;
    }

    if (!image.isNull()) {
        it->image = image;
        it->state = Entry::State::Ready;
        ++m_statReady;                                   // 每秒体检：这一秒多解出一张
    } else {
        //   这次解码是在**用户输入密码之前**发起的（结果回来晚）：条目现在已经有密码了
        //   ⇒ 丢掉这个结果，别误报"要密码"、也别把它记成任何结论（等 retry 的新结果）。
        const bool nowHasPw = m_seq && item >= 0 && item < m_seq->count()
                              && !m_seq->at(item).loc.passwords.value(passwordLevel).isEmpty();
        if (passwordLevel >= 0 && !passwordUsed && nowHasPw) {
            m_entries.erase(it);
        } else if (passwordLevel >= 0) {
            //   ★ **要密码**：记结论（不能记成"缺" —— 那会被**永久跳过**），带上**层号**
            //     （弹框要靠它算"是哪个容器"）。这里**不弹框**：预取照常往后取（不暂停，
            //     2026-11-05 用户定），弹框由前端在"**本屏还有没解决的等密码格**"时发
            //     （`maybeAskPassword` / 设计 22 §5.3）。
            m_entries.erase(it);
            markNeedsPassword(item, passwordLevel);
        } else {
            //   走到这里只剩一种：**没有字节可读**（文件没了 / 容器打不开 / 成员不在）
            //   —— 解码失败但有字节的那种，工作线程已经换成占位图了。
            m_entries.erase(it);
            markMissing(item);
        }
    }
    notifyFrontend();                                    // 只通知（前端自己判定）
    reconcile();                                         // 接着按新的 desired 派活
}

// ══ 来源 / 序列长进了 ═══════════════════════════════════════════════════════
//   换来源 ⇒ **换个记号就走**：条目表 + 两张结论表整张换掉、在飞计数归零，回到起始（位置 0）。
void ViewerModel::setSequence(const Sequence *seq)
{
    ++m_epoch;                       // 换来源记号：在飞的回来时记号不符 ⇒ 丢
    m_seq = seq;
    m_entries.clear();
    m_missing.clear();               // 结论的生命周期 = 一次查看（换来源一起清）
    m_needsPassword.clear();
    //   ★ 预览也换代（2026-11-06）：键里带代号 ⇒ 上一个来源的缩略图/结论一律不再命中。
    ++m_previewGen;
    m_previewFailed.clear();
    m_promptedItem = -1;
    m_inFlight = 0;
    m_skipped  = 0;
    clearAnnounced();                // 新来源 ⇒ 没有"已宣布的屏"
    //   ★ 初始位置由上层在**装序列之前**给（`setStartPosition`）：恢复进度/从第 N 条开始
    //     都是"初始化时就在那儿"，不是装载完再跳一次（用户 2026-11-04）。
    const int n = m_seq ? m_seq->count() : 0;
    m_pos      = (n > 0) ? qBound(0, m_startIndex, n - 1) : 0;
    m_cursor   = m_pos;
    m_startIndex = 0;                // 一次性：之后换序列默认还是第 0 条
    m_dir = +1;                      // 换来源 ⇒ 方向回前向
    emit currentChanged();           // 条目总数变了（QML 的 count / totalPages 跟它）
    evaluateScreen();
    reconcile();
}

//   序列长进了：条目总数可能变了 ⇒ 通知一次，再判一次（可能这一条现在有了 / 能往后走了）。
void ViewerModel::sequenceChanged()
{
    emit currentChanged();
    evaluateScreen();
    reconcile();
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

QString ViewerModel::currentName() const
{
    return m_screenName;
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
    if (!m_seq || m_seq->count() <= 0) { evaluateScreen(); return; }
    if (m_loop == Loop::Folder) { evaluateScreen(); return; }   // ★ 文件夹内循环：不许切文件夹
    if (m_playing) return foldOrRun(Want::Folder, (delta > 0) ? +1 : -1);
    int node = adjacentFolderNode(delta);
    if (node < 0) {
        if (m_loop == Loop::All) node = edgeFolderNode(delta);
        else { evaluateScreen(); return; }              // 不循环：到头（按钮本来就是灰的）
    }
    if (node < 0) { evaluateScreen(); return; }
    const int target = m_seq->subtreeFirst(node);       // 落点 = 那个容器的第一条图
    if (target < 0) { evaluateScreen(); return; }
    act(target, 0);
}

//   换循环模式：会话级、不写盘；**只重判一次**（不动方向）。
void ViewerModel::setLoopMode(int mode)
{
    const Loop want = (mode == int(Loop::All))    ? Loop::All
                    : (mode == int(Loop::Folder)) ? Loop::Folder
                                                  : Loop::None;
    if (m_loop == want) return;
    m_loop = want;
    emit loopModeChanged();
    evaluateScreen();
    reconcile();
}

// ══ 幻灯片（规则见设计 13 §九·补·二）═══════════════════════════════════════
//
//   两种等待**必须分开**：
//     · **等闹钟**：本屏已就位，节拍在计时 —— 能响应；
//     · **等缓存**：本屏还没上屏 —— 不能响应。
//   节奏：`到期 = 请求时刻 + 间隔`（**间隔是下限**，准备耗时吃掉间隔）⇒ 不堆积、不跳页。
//   推进等待位 = **一个 bool**：到点但还在等缓存 ⇒ 置位、**且不挂闹钟**（忙碌期不存在闹钟）。
//   折叠器 = **一个槽**：等缓存期间的进度事件按类型覆盖成一条最终意图，上屏那一刻执行一次；
//   任何事件进折叠器 ⇒ **立刻清掉推进等待位**（用户意图优先）。

bool ViewerModel::waitingForCache() const
{
    if (!m_seq || m_seq->count() <= 0) return true;
    return !screenSettled();            // 本屏还没结论（既没备好、也不是缺 / 等密码）
}

void ViewerModel::cancelBeat()
{
    if (m_beat) m_beat->stop();
    m_beatArmed = false;
}

//   挂闹钟：剩余按**请求时刻**算（不是"上次到期"）⇒ 耗时自然吃掉间隔。
//   ★ 等缓存期间**不挂**（不变量）；只在“宣布新屏”那一刻会话进来。
//   ★★ **幂等**（2026-11-05，0ms 卡死的根治点）：同一 deadline 且闹钟还在跑 ⇒ 直接返回。
//      以前这里每次都 `start(remain)`，间隔 0 时等于把一个 0ms 单发定时器无限重启，
//      `onBeat` 永远轮不到 ⇒ 画面冻结。
void ViewerModel::armBeat()
{
    if (!m_playing || !m_beat) return;
    if (!screenSettled()) return;                   // 等缓存：不挂（宣布时会再来）
    const qint64 deadline = m_reqAt + m_intervalMs;
    const int remain = int(qMax<qint64>(0, deadline - nowMs()));
    if (m_beatArmed && m_beatDeadline == deadline && m_beat->isActive()) return;
    m_beatArmed    = true;
    m_beatDeadline = deadline;
    m_beat->start(remain);
}

//   每秒体检（**只打日志**，不参与调度）：用户自己跑 5 秒对比预取 0 / 8 时看这一行。
//   `ready` = 这秒解出几张、`screens` = 这秒上几屏、`started` = 这秒派了几条、
//   `inflight` = 现在还在飞、`pending` = 本屏还差几张。空闲不打，避免刷屏。
void ViewerModel::onStatTick()
{
    const int ready = m_statReady, screens = m_statScreens, started = m_statStarted;
    m_statReady = m_statScreens = m_statStarted = 0;

    int pending = 0;
    if (m_seq) {
        for (int i : screenItems(screenStart(m_pos))) {
            if (!inLoopScope(i)) continue;
            if (isMissing(i)) continue;
            if (waitsPassword(i) && !passwordDeclined(i)) continue;
            if (entryReady(i)) continue;
            ++pending;
        }
    }
    if (ready == 0 && screens == 0 && started == 0 && m_inFlight == 0 && pending == 0) return;

    qCInfo(lcUi).noquote()
        << QStringLiteral("[ui] viewer: 1s ready=%1 screens=%2 started=%3 inflight=%4 pending=%5 L=%6")
               .arg(ready).arg(screens).arg(started).arg(m_inFlight).arg(pending).arg(m_length);
}

//   闹钟到点 = **间隔满足**，但**不在这里换屏**（2026-11-06 用户定）：
//   只置 `m_beatElapsed`，真正的换屏在 `framePresented()` 里（等下一帧呈现）。
//   ⇒ 节奏 = 刷新率与间隔共同决定；慢解码时就地等待，不补播、不跳帧。
void ViewerModel::onBeat()
{
    m_beatArmed = false;                // 单发闹钟已经到点（下一次宣布再挂）
    if (!m_playing) return;
    m_beatElapsed = true;
}

//   ★★ **显示时钟**：每呈现一帧调一次（QML 的 `FrameAnimation`）。幻灯片的换屏唯一入口。
//   规则：① 当前这一屏必须先被画过一帧（`m_screenPainted`）才允许换掉它；
//        ② 间隔要满足（`m_beatElapsed`）；③ 下一屏要有结论（`screenSettled`，否则等着）。
//   ⇒ 每帧最多换一屏 ⇒ 0 ms 时 = 每帧一屏（≈刷新率），任何一屏都不会被跳过。
void ViewerModel::framePresented()
{
    if (!m_playing) return;
    if (!m_screenPainted) { m_screenPainted = true; return; }   // 这一帧就是把新屏画出来
    if (!m_beatElapsed) return;                                 // 间隔还没到
    if (!screenSettled()) return;                               // 下一屏还没就绪：等就绪再换
    m_beatElapsed = false;
    const int next = nextScreen(screenStart(m_pos), +1);
    if (next < 0) return stopPlaying(tr("End reached · slideshow stopped"));
    //   ★ 这一段里 announce 出来的新屏**就是本帧画的那一屏**（`m_inFrame`）⇒ 不必再多等一帧。
    m_inFrame = true;
    requestScreen(next, +1);
    m_inFrame = false;
}

//   宣布新屏那一刻（**唯一调用点 = `announce()`**）：① 折叠器 ② 推进等待位 ③ 挂节拍。
void ViewerModel::onScreenUp()
{
    if (!m_playing) return;
    if (m_want != Want::None) {         // ① 用户意图优先
        const Want w = m_want;
        const int arg = m_wantArg;
        m_want = Want::None;
        m_wantArg = 0;
        return runWanted(w, arg, /*folded=*/true);
    }
    armBeat();                          // ② 挂间隔闹钟（换屏本身等下一帧，见 `framePresented()`）
}

//   进度事件的统一入口（只在播放态调用）。
void ViewerModel::foldOrRun(Want w, int arg)
{
    if (waitingForCache()) {            // 等缓存 ⇒ 折叠器（一个槽，后到覆盖先到）
        m_want = w;
        m_wantArg = arg;
        m_beatElapsed = false;          // ★ 用户意图进来 ⇒ 幻灯片这次推进作废
        return;
    }
    cancelBeat();                       // 等闹钟 ⇒ 取消旧闹钟 → 执行 → 从这一刻重挂
    runWanted(w, arg, /*folded=*/false);
    armBeat();                          // 动作没换屏（例如到头）时兜住节拍；换屏那条已在 onScreenUp 挂过
}

void ViewerModel::runWanted(Want w, int arg, bool folded)
{
    if (!m_seq || m_seq->count() <= 0) return;
    switch (w) {
    case Want::Step: {
        const int next = nextScreen(screenStart(m_pos), arg);
        if (next < 0) {                 // 到头：折起来的那一次给个瞬时提示
            if (folded) emit notice(tr("End of list"));
            return;
        }
        return requestScreen(next, arg);
    }
    case Want::Jump:
        return requestScreen(qBound(0, arg, m_seq->count() - 1), 0);
    case Want::Folder: {
        const int node = adjacentFolderNode(arg);
        if (node < 0) return;
        const int target = m_seq->subtreeFirst(node);
        if (target < 0) return;
        return requestScreen(target, 0);
    }
    case Want::None:
        return;
    }
}

//   换屏 + 记下**请求时刻**（计时起点 = 这次"成为当前屏"的动作被请求的时刻）。
void ViewerModel::requestScreen(int target, int stepDir)
{
    m_reqAt = nowMs();
    act(target, stepDir);               // 就位 ⇒ judge → screen（里面的 onScreenUp 会挂闹钟）
}

void ViewerModel::stopPlaying(const QString &noticeText)
{
    if (!m_playing) return;
    m_playing = false;
    cancelBeat();
    m_want = Want::None;
    m_wantArg = 0;
    m_beatElapsed = false;
    emit playingChanged();
    if (!noticeText.isEmpty()) emit notice(noticeText);
}

void ViewerModel::setPlaying(bool on)
{
    if (!on) {
        //   ★ 任何一次 `setPlaying(false)` 都**先清闹钟**（不管当时是不是在播放）
        //     —— 不依赖"闹钟只在播放态存在"这条不变量。
        cancelBeat();
        if (m_playing) stopPlaying(QString());
        return;
    }
    if (m_playing) return;
    if (!m_seq || m_seq->count() <= 0) return;
    //   已经在最后一屏（不循环 / 区间到头）⇒ **不回到开头**，只给"到头"的瞬时提示
    if (nextScreen(screenStart(m_pos), +1) < 0) {
        emit notice(tr("End of list"));
        return;
    }
    m_playing = true;
    m_want = Want::None;
    m_wantArg = 0;
    m_beatElapsed = false;
    m_screenPainted = true;             // 开播这一刻屏幕上那一屏已经画过了
    m_reqAt = nowMs();
    emit playingChanged();
    armBeat();
}

void ViewerModel::togglePlaying()
{
    setPlaying(!m_playing);
}

//   改间隔：**立即生效** —— 取消旧闹钟，按当时请求时刻用新间隔重算。
void ViewerModel::setIntervalMs(int ms)
{
    const int want = qMax(app::kMinSlideshowIntervalMs, ms);
    if (m_intervalMs == want) return;
    m_intervalMs = want;
    emit intervalChanged();
    if (m_playing) {
        cancelBeat();
        armBeat();
    }
}

// ══ 视图态（viewer 领域）：fit 只是命令，状态只有 zoom + pan ════════════════
//   几何全部走 `gpuscale`（`zoomForFit` / `panForAnchoredZoom` / `makePlan`），本类只持有状态。
QImage ViewerModel::geometryImage() const
{
    return !m_imageA.isNull() ? m_imageA : m_imageB;
}

void ViewerModel::setFitMode(int m)
{
    m = qBound(0, m, 3);
    if (m_fitMode != m) {
        m_fitMode = m;
        emit fitModeChanged();
    }
    applyFit();          // 点 fit = 命令：算一次 zoom、pan 归 0
}

void ViewerModel::setZoom(qreal v)
{
    if (!(v > 0) || qFuzzyCompare(m_zoom, v)) return;
    m_zoom = v;
    emit zoomChanged();
}

void ViewerModel::setPanX(qreal v)
{
    if (qFuzzyCompare(m_panX, v)) return;
    m_panX = v;
    emit panChanged();
}

void ViewerModel::setPanY(qreal v)
{
    if (qFuzzyCompare(m_panY, v)) return;
    m_panY = v;
    emit panChanged();
}

void ViewerModel::setZoomLocked(bool on)
{
    if (m_zoomLocked == on) return;
    m_zoomLocked = on;
    emit zoomLockedChanged();
}

void ViewerModel::setOsdVisible(bool on)
{
    if (m_osdVisible == on) return;
    m_osdVisible = on;
    emit osdVisibleChanged();
}

void ViewerModel::setScaleAlgorithm(const QString &key)
{
    // 键串 → 枚举 → 规范化键（唯一映射在 gpuscale）
    const QString k = QLatin1String(gpuscale::algorithmKey(gpuscale::algorithmFromKey(key)));
    if (m_scaleAlgorithm == k) return;
    m_scaleAlgorithm = k;
    emit scaleAlgorithmChanged();
}

void ViewerModel::setViewport(qreal logicalW, qreal logicalH, qreal dpr)
{
    if (!(dpr > 0)) dpr = 1.0;
    const QSize vp(qMax(1, qRound(logicalW * dpr)), qMax(1, qRound(logicalH * dpr)));
    if (vp == m_viewport && qFuzzyCompare(dpr, m_dpr)) return;
    //   ★ dpr 变了（窗口被拖到另一块屏）：`zoom` / `pan` 都是**设备像素**口径，
    //     按 dpr 的比例一起缩放 ⇒ **逻辑倍率不变**（同一张图在 1x / 2x 屏上看起来一样大）。
    //     不这么做的话，从 1x 屏拖到 2x 屏，图会缩成一半（"画面变了"）。
    const qreal oldDpr = m_dpr;
    if (!qFuzzyCompare(oldDpr, dpr) && !m_viewport.isEmpty()) {
        const qreal k = dpr / oldDpr;
        const qreal z0 = m_zoom;
        m_zoom   = qBound(qreal(0.02), m_zoom * k, qreal(64.0));
        m_panX  *= k;
        m_panY  *= k;
        //   验收用（PHOTOSET_LOG=debug）：跨 dpi 显示器时这一行能证明"视口交接收到了什么"
        qCDebug(lcUi).noquote()
            << QStringLiteral("[ui] viewport: %1x%2 dpr=%3 (was %4); zoom %5 -> %6")
                   .arg(vp.width()).arg(vp.height()).arg(dpr).arg(oldDpr)
                   .arg(z0).arg(m_zoom);
        emit zoomChanged();
        emit panChanged();
    }
    m_viewport = vp;
    m_dpr = dpr;
    // 视口变了（窗口/版面/dpr）：还没自由缩放、也没锁定 ⇒ 按当前 fit 重算
    if (!m_freeZoom && !m_zoomLocked) applyFit();
    //   ★ **自由缩放/锁定时也要把 pan 夹回新范围**：渲染侧（`gpuscale::makePlan`）会自己夹，
    //     但 VM 里若留着越界的 pan，拖拽第一次就会把"差量"一次性补出来 = **画面跳变**
    //     （用户 2026-11-03 报的"放大后切换窗口再拖，画面会跳"就是这个）。
    clampPan();
}

void ViewerModel::applyFit()
{
    const QImage img = geometryImage();
    if (img.isNull() || m_viewport.isEmpty()) return;
    const qreal z = gpuscale::zoomForFit(img.size(), m_viewport, gpuscale::Fit(m_fitMode));
    m_freeZoom = false;
    setZoom(z);
    setPanX(0.0);
    setPanY(0.0);
}

//   把 pan 夹回"当前 zoom × 视口"允许的范围内。
//   `panBy` 里也夹，但那是**拖拽时**才夹；窗口/dpr 变化后不夹的话，VM 状态与渲染结果不一致
//   （渲染侧 makePlan 会夹），下一次拖拽就会跳。
//   ⚠️ 单位：`m_zoom` 与 `m_viewport` 都是**设备像素** ⇒ 这里的 slack 也是设备像素。
void ViewerModel::clampPan()
{
    const QImage img = geometryImage();
    if (img.isNull() || m_viewport.isEmpty()) return;
    const QSize full(qRound(img.width() * m_zoom), qRound(img.height() * m_zoom));
    const qreal slackX = qMax(0, full.width() - m_viewport.width());
    const qreal slackY = qMax(0, full.height() - m_viewport.height());
    setPanX(qBound(qreal(0), m_panX, slackX));
    setPanY(qBound(qreal(0), m_panY, slackY));
}

void ViewerModel::panBy(qreal dx, qreal dy)
{
    const QImage img = geometryImage();
    if (img.isNull() || m_viewport.isEmpty()) return;
    const QSize full(qRound(img.width() * m_zoom), qRound(img.height() * m_zoom));
    const qreal slackX = qMax(0, full.width() - m_viewport.width());
    const qreal slackY = qMax(0, full.height() - m_viewport.height());
    // 先把旧 pan 夹回当前范围，再叠加本次位移 ⇒ 不会"空走一段"
    const qreal baseX = qBound(qreal(0), m_panX, slackX);
    const qreal baseY = qBound(qreal(0), m_panY, slackY);
    // 内容跟着指针走 ⇒ pan 减小。★ pan 不取消 fit（适应高度/宽度/原始都要平移余量）
    const qreal nx = qBound(qreal(0), baseX - dx * m_dpr, slackX);
    const qreal ny = qBound(qreal(0), baseY - dy * m_dpr, slackY);
    setPanX(nx);
    setPanY(ny);
}

void ViewerModel::zoomAtPoint(qreal factor, qreal anchorX, qreal anchorY)
{
    if (!(factor > 0)) return;
    const QImage img = geometryImage();
    if (img.isNull() || m_viewport.isEmpty()) return;
    const qreal oldZoom = m_zoom;
    const qreal newZoom = qBound(qreal(0.02), oldZoom * factor, qreal(64.0));
    if (qFuzzyCompare(newZoom, oldZoom)) return;
    const QPointF np = gpuscale::panForAnchoredZoom(img.size(), m_viewport, oldZoom, newZoom,
                                                    m_panX, m_panY,
                                                    anchorX * m_dpr, anchorY * m_dpr);
    m_freeZoom = true;              // 只有 **zoom** 变化才让 fit 让位
    setZoom(newZoom);
    setPanX(np.x());
    setPanY(np.y());
}

//   绝对倍率缩放：直接给 zoom（不是乘系数），pan 归零 ⇒ 居中；并让 fit 让位
//   （`m_freeZoom = true`，否则下一次视口变化会被 applyFit 覆盖回去）。
//   Chrome 的缩放档位（110% / 125% / 150% …）。以**当前 zoom × 100** 找相邻档，
//   而不是按倍率连乘 —— 这样从 43%（fit）按一次“放大”会落到 50%，符合预期。
void ViewerModel::zoomStep(int dir)
{
    static const qreal steps[] = {0.25, 0.33, 0.50, 0.67, 0.75, 0.80, 0.90, 1.00,
                                  1.10, 1.25, 1.50, 1.75, 2.00, 2.50, 3.00, 4.00, 5.00};
    const int n = int(sizeof(steps) / sizeof(steps[0]));
    const qreal cur = m_zoom;
    //   ★ 到头就是**到头**（不动），不许"倒着弹回最近一档"：
    //     旧实现在 cur < 25%（fit 到比最小档还小）按"缩小"会跳回 25% —— 缩小反而变大；
    //     cur > 500% 按"放大"会跳到 500% —— 放大反而变小。
    if (dir > 0) {
        for (int i = 0; i < n; ++i)
            if (steps[i] > cur * 1.001) return zoomToRatio(steps[i]);
    } else {
        for (int i = n - 1; i >= 0; --i)
            if (steps[i] < cur * 0.999) return zoomToRatio(steps[i]);
    }
}

void ViewerModel::zoomToRatio(qreal ratio)
{
    if (!(ratio > 0)) return;
    const QImage img = geometryImage();
    if (img.isNull() || m_viewport.isEmpty()) return;
    m_freeZoom = true;
    setZoom(qBound(qreal(0.02), ratio, qreal(64.0)));
    setPanX(0);
    setPanY(0);
    clampPan();
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
    if (!m_seq || m_seq->count() <= 0) { evaluateScreen(); return; }
    const int dir = (delta > 0) ? +1 : -1;

    //   ★ 幻灯片：走"等闹钟 ⇒ 立刻执行 / 等缓存 ⇒ 进折叠器"（设计 13 §九·补·二 21–24）。
    if (m_playing) return foldOrRun(Want::Step, dir);

    //   ── 不播放：维持现状（本屏还没结论 ⇒ 这次按键丢掉）──
    if (!screenSettled()) { evaluateScreen(); return; }   // 还没结论 ⇒ 这次按键丢掉

    const int start = screenStart(m_pos);
    const int next = nextScreen(start, dir);      // 到头 ⇒ -1（位置不动）
    if (next < 0) { evaluateScreen(); return; }
    act(next, dir);
}

void ViewerModel::setStartPosition(int imageIndex)
{
    m_startIndex = qMax(0, imageIndex);
}

void ViewerModel::jump(int imageIndex)
{
    if (!m_seq) { evaluateScreen(); return; }
    const int n = m_seq->count();
    if (n <= 0) { evaluateScreen(); return; }
    if (m_playing) return foldOrRun(Want::Jump, qBound(0, imageIndex, n - 1));
    act(qBound(0, imageIndex, n - 1), 0);     // 跳转 ⇒ 方向回前向
}

//   换版面：**保留当前位置**，屏由序列按新版面重算（不重新分组）⇒ 再判一次。
//   ★ 换版面**不改方向**（用户 2026-11-05 定）：预取份额只跟"往哪边翻"有关。
void ViewerModel::setDoublePage(bool on)
{
    const app::Layout want = on ? app::Layout::Double : app::Layout::Single;
    if (m_layout == want) return;
    m_layout = want;
    clearAnnounced();                 // 屏换了 ⇒ 必须重新宣布（否则节拍不会动）
    emit doublePageChanged();
    //   ★ 幻灯片：换版面**不停播**（设计 13 §九·补·二 28）：取消旧闹钟，按"这一刻"重新计时；
    //     宣布那一刻会重挂（等缓存期间不挂）。
    if (m_playing) {
        cancelBeat();
        m_reqAt = nowMs();
    }
    evaluateScreen();
    reconcile();
}

//   `stepDir` = 这次是"换一屏"（±1）；0 = 跳转 / 换版面。
void ViewerModel::act(int target, int stepDir)
{
    if (target != m_pos) {
        //   ★ 方向只看这一次动作：换屏取那一次的方向；跳转 / 换版面（`stepDir == 0`）回前向。
        m_dir = (stepDir >= 0) ? +1 : -1;
        m_pos = target;
        clearAnnounced();             // 换位置 ⇒ 新屏还没宣布
    }
    evaluateScreen();
    reconcile();
}

// ══ 后台线：预取（`reconcile`）══════════════════════════════════════════════

//   解码并发 = **当前屏未满足的图数 + 预取张数**，再夹到硬件并发（2026-11-05 用户定）。
//   ★ 「未满足」= 本屏还没拿到图、且不是缺 / 不是等密码的那些格（真正要解的那几张）。
//     所以它是**实时值**：本屏越接近齐，这条线就越低（本屏齐了 ⇒ 只剩预取在跑）。
//   ★ 为什么不是固定 4：慢盘实测 1/4/8 线程 = 18/51/121 页每秒，双幅 60 fps 要约 8 并发；
//     预取调大就该多给几条解码，调小（默认 1）就只给本屏那 1~2 条 + 1 条预取。
//   ★ 上限取 `QThread::idealThreadCount()`（逻辑核）：超过它只会互相抢 CPU。
int ViewerModel::inFlightLimit() const
{
    int pending = 0;
    if (m_seq) {
        for (int i : screenItems(screenStart(m_pos))) {
            if (!inLoopScope(i)) continue;
            if (isMissing(i)) continue;
            if (waitsPassword(i) && !passwordDeclined(i)) continue;   // 等密码：解不了，不算
            if (entryReady(i)) continue;
            ++pending;
        }
    }
    const int cores = qMax(1, QThread::idealThreadCount());
    return qBound(1, pending + m_length, cores);
}

//   每拍重算一次"现在该有哪些条目"，然后派缺失的、丢不要的。**幂等**，没有历史包袱。
//
//     desired = 本屏（pinned：必取，**不占** L）
//            ++ 行进方向 ahead 张      （ahead = L − insurance）
//            ++ 反方向   insurance 张  （insurance = L >= 4 ? max(1, L/4) : 0）
//
//   顺序 = 派活顺序（pinned 最前 ⇒ 并发满时永远先派本屏）。
//   ★ 只认"图"，不认"屏"：双幅的 `L` **不**×2，额度按张数算。
//   ★ **不做"预取 2 + 钉住 2"这类配额**（用户 2026-11-05 明确否掉）：并发上限只是资源上限，
//     靠"每完成一条就重拍"保证 pinned 永远排最前。
void ViewerModel::reconcile()
{
    if (!m_seq) { m_entries.clear(); return; }
    const int n = m_seq->count();
    if (n <= 0) { m_entries.clear(); return; }

    const int start = screenStart(m_pos);
    const QVector<int> members = screenItems(start);

    QVector<int> order;                 // desired 的顺序（= 派活顺序）
    QSet<int>    want;                  // desired 的集合（= 判"还要不要"）
    bool learned = false;               // 本次有没有新写出"缺"

    const auto take = [&](int i) -> bool {
        if (i < 0 || i >= n) return false;
        if (!inLoopScope(i)) return false;               // 区间外：本次不显示，也不白解码
        if (want.contains(i)) return false;
        if (m_missing.contains(i)) return false;         // 已知缺 ⇒ 永不重派
        if (m_needsPassword.contains(i)) return false;   // 等密码 ⇒ 不重派（拿到密码会清）
        const Sequence::Item &it = m_seq->at(i);
        if (it.missing) {                                // 元数据已标缺失 ⇒ 只写结论，不派解码
            m_missing.insert(i);
            learned = true;
            return false;
        }
        want.insert(i);
        order << i;
        return true;
    };

    //   ① 本屏那 1~2 条：**必取**（不占 L —— 这就是 pinned 的独立槽位）
    for (int i : members) take(i);

    //   ② 行进方向 ahead 张 → 反方向 insurance 张（都按"图"数；走屏不改计数口径）
    const int insurance = (m_length >= 4) ? qMax(1, m_length / 4) : 0;
    const auto walk = [&](int dir, int budget) {
        int cur = start;
        for (int guard = 0; budget > 0 && guard < 256; ++guard) {
            const int next = nextScreen(cur, dir);
            if (next < 0) return;
            const QVector<int> its = screenItems(next);
            if (its.isEmpty()) return;
            for (int i : its) {
                if (budget <= 0) return;
                if (take(i)) --budget;      // 只数**真的要取**的图（缺 / 区间外不占额度）
            }
            cur = next;
        }
    };
    walk(m_dir, m_length - insurance);
    walk(-m_dir, insurance);

    //   ③ 角色：属于当前屏的才是 pinned（独立槽位；其余都是 lookahead，占 L）
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
        it->pinned = members.contains(it.key());

    //   ④ 丢：已经不要的。**备好的立刻释放**；在飞的等它回来再丢（线程不能取消，
    //      而它回来那一刻正好借 `onLoaded` 重拍一次）
    for (auto it = m_entries.begin(); it != m_entries.end(); ) {
        if (!want.contains(it.key()) && it->state == Entry::State::Ready)
            it = m_entries.erase(it);
        else
            ++it;
    }

    //   ⑤ 派：按 desired 顺序（pinned 在最前）；受"并发上限"与"lookahead 额度"两条约束
    //   ★ 并发上限 = 本屏图数 + 预取张数，再夹到核数（见 `inFlightLimit()`）。
    const int inFlightLimit = this->inFlightLimit();
    int lookahead = 0;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
        if (!it->pinned) ++lookahead;
    for (int item : order) {
        if (m_entries.contains(item)) continue;              // 在飞 / 已备好
        const bool pinned = members.contains(item);
        if (!pinned && lookahead >= m_length) break;          // 额度用完（L = 0 ⇒ 一条不派）
        if (m_inFlight >= inFlightLimit) break;               // 资源上限：下一次重拍再派
        Entry e;
        e.state  = Entry::State::InFlight;
        e.pinned = pinned;
        m_entries.insert(item, e);
        ++m_inFlight;
        ++m_statStarted;                                 // 每秒体检：这一秒派出去一条
        if (!pinned) ++lookahead;
        preload(m_seq->at(item), item);
    }

    if (learned) notifyFrontend();          // 新写出"缺" ⇒ 前端可能能跳屏了
}

//   后台完成一条后的**唯一通知**（投递到 GUI 线程、**合并去抖**）：
//   ★ 它只说"有东西变了"，**不做任何前端判定** —— 这就是这次重构的分界线。
void ViewerModel::notifyFrontend()
{
    if (m_notifyPending) return;            // 同一轮事件里多次完成 ⇒ 只判一次
    m_notifyPending = true;
    QMetaObject::invokeMethod(this, [this] {
        m_notifyPending = false;
        evaluateScreen();
    }, Qt::QueuedConnection);
}

// ══ 条目表的小工具（后台线写、前端只读）════════════════════════════════════
bool ViewerModel::entryReady(int item) const
{
    const auto it = m_entries.constFind(item);
    return it != m_entries.constEnd() && it->state == Entry::State::Ready;
}

//   这一条备好时的那张图（缺 / 等密码 / 在飞 / 区间外 ⇒ 空图）。
QImage ViewerModel::imageOf(int item) const
{
    if (item < 0) return QImage();
    if (!inLoopScope(item)) return QImage();    // ★ 区间外 ⇒ **留空**（不是缺，只是本次不显示）
    const auto it = m_entries.constFind(item);
    if (it == m_entries.constEnd() || it->state != Entry::State::Ready) return QImage();
    return it->image;
}

//   "缺"：**不占额度、不重试**（写进小表就不再派活；这一条本次查看的命就到这儿）。
void ViewerModel::markMissing(int item)
{
    m_needsPassword.remove(item);
    m_missing.insert(item);
}

void ViewerModel::markNeedsPassword(int item, int level)
{
    m_missing.remove(item);
    m_needsPassword.insert(item, level);
}

// ══ 时间轴悬停预览（用户 2026-11-03 / 2026-11-06 定稿）══════════════════════════
//   ★★ 分工 —— 这一节存在的理由就是"别再让组件自己调度"：
//     · **前端**（QML `FullscreenBottomBar`）= **唯一的调度者 + 唯一的显示决定者**。
//       它持有"指针指在哪一条"这个事实，在**两个事件**（进度变化 / 空闲通知）上跑同一段判定：
//       算当前位置要哪几张（`previewStrip`）⇒ 缺就 `previewEnsure`（幂等）⇒ `ready` 就换显示，
//       不 ready **保持上一份**（绝不显示半条 / 过期几何）。
//     · **本类**（组件）= 缓存 + pending + **一条"我空闲了"的通知**：
//       不知道"当前位置"、不比较新旧、不采样、**没有队列**。
//     · "忙"在前端只是一个布尔（派了活、还没收到 `previewIdle`）；前端是唯一的派活者 ⇒ 精确。
//   ★ 为什么没有批次号（2026-11-06 删掉 `m_previewBatch` / `m_previewBatchKeys` /
//     `previewBatchReady`）：批次号唯一的用途是让**组件**判断"我这批还用得上吗" ——
//     那正是把调度塞回组件里。通知不带批次 ⇒ 前端每次都按**当时**的位置重算，
//     "最新优先"是自然结果（旧请求的活照跑完、落缓存，只是没人再等它）。
//   ★ 用户报的 bug（2026-11-06）："停在某一位置时会先显示一个非当前状态的预览，再跳到正确的"。
//     成因两条，都在前端：① 请求被 600 ms 节流推迟（几何一直是旧的）；
//     ② 显示跟着"请求"走而不是跟着"就绪"走（`0e4f563` 把 2026-11-04 的就绪门控拆了）。

//   缩略图的缓存键 = **代 + 解码高度 + 条目下标**：换条高是另一张缩略图；
//   换来源后"代"变了 ⇒ 同序号也不会命中上一个来源的图（2026-11-06）。
QString ViewerModel::previewKeyOf(int heightPx, int index) const
{
    return QStringLiteral("%1-%2-%3").arg(m_previewGen).arg(heightPx).arg(index);
}

//   以 `center` 为中心左右交替取，直到铺满 `maxWidthPx`（最多 40 张）。
//   ★ 纯几何：`previewStrip()`（要哪几张 → 给界面）与 `previewEnsure()`（派哪几张）共用，
//     两边的 key 集合必须一模一样，否则"算出来的"和"去解的"会对不上。
QVector<int> ViewerModel::previewNeighbors(int center, int heightPx, int maxWidthPx, int gapPx) const
{
    QVector<int> idx;
    const int n = m_seq ? m_seq->count() : 0;
    if (n <= 0 || heightPx <= 0 || maxWidthPx <= 0) return idx;

    center = qBound(0, center, n - 1);
    const auto widthOf = [this, heightPx](int i) { return previewItemWidth(i, heightPx); };

    const int half = maxWidthPx / 2 + heightPx;
    const int gap = qMax(0, gapPx);        // ★ 间距由调用方给（与 QML 的 Row spacing 同一个值）
    idx << center;
    int total = widthOf(center);
    int leftW = 0, rightW = 0;
    int li = center - 1, ri = center + 1;
    const auto takeLeft = [&] { if (li < 0) return false; idx << li; total += widthOf(li) + gap;
                                leftW += widthOf(li) + gap; --li; return true; };
    const auto takeRight = [&] { if (ri >= n) return false; idx << ri; total += widthOf(ri) + gap;
                                 rightW += widthOf(ri) + gap; ++ri; return true; };
    while ((leftW < half || rightW < half) && idx.size() < 40) {
        bool added = false;
        if (leftW < half)  added = takeLeft()  || added;
        if (rightW < half) added = takeRight() || added;
        if (!added) break;
    }
    while (total < maxWidthPx && idx.size() < 40) {
        bool added = takeLeft();
        added = takeRight() || added;
        if (!added) break;
    }
    std::sort(idx.begin(), idx.end());
    return idx;
}

//   时间轴悬停预览：一次算完 **条目表 + 整条偏移 + 居中的那一页 + 就绪与否**
//   ★ **不发活**：`ready=false` 时前端自己去调 `previewEnsure()`（而且只在它认为组件空闲时）。
//   ★ **不返回"框的坐标"**（用户 2026-11-06 定）：定位框的位置与宽度一律由前端
//     **按 `index`（这批居中那一页）/ `hoverIndex`（指针在条里指着那一页）现算** ——
//     `items` 每条都带 `index` / `x` / `w`，配上这里的 `stripX` 就够算了。
//     以前返回 `focusX` / `focusW`（= 居中那一页的绝对 x 和宽）是多余的**快照**：
//     条一隐藏、快照被清掉就变成兜底值 0，下次出现时定位框会从 0 "飞"过来。
QVariantMap ViewerModel::previewStrip(int center, int heightPx, int maxWidthPx, int gapPx)
{
    QVariantMap result;
    result.insert(QStringLiteral("items"), QVariantList());
    result.insert(QStringLiteral("stripX"), 0);
    result.insert(QStringLiteral("index"), -1);        // 这批居中那一页（没有 = -1）
    result.insert(QStringLiteral("ready"), false);
    if (!m_seq || m_seq->count() <= 0 || heightPx <= 0 || maxWidthPx <= 0 || !m_previewStore)
        return result;

    const QVector<int> idx = previewNeighbors(center, heightPx, maxWidthPx, gapPx);
    if (idx.isEmpty()) return result;

    center = qBound(0, center, m_seq->count() - 1);
    const auto widthOf = [this, heightPx](int i) { return previewItemWidth(i, heightPx); };
    const int gap = qMax(0, gapPx);

    //   整条该在哪（Row 的唯一来源）：
    //   `wanted` = 让居中那一页落在窗口中线所需的位置；两端凑不够图时钳在 [maxWidth-rowW, 0]
    //   ⇒ 定位框的 x 由前端算 `stripX + item(index).x`，中线情形自然就是"窗口宽/2 − 框宽/2"。
    int leftSum = 0, rowW = -gap;
    for (int i : idx) rowW += widthOf(i) + gap;
    for (int i : idx) { if (i == center) break; leftSum += widthOf(i) + gap; }
    const int w = widthOf(center);
    const double wanted = maxWidthPx / 2.0 - (leftSum + w / 2.0);
    const double stripX = qMin(0.0, qMax(double(maxWidthPx - rowW), wanted));
    result.insert(QStringLiteral("stripX"), int(qRound(stripX)));   // 整条该在哪
    result.insert(QStringLiteral("index"), center);                 // 居中那一页
    //   ★ 框**不夹取**（用户 2026-11-04）：边缘处的图可能被窗口裁掉一截，框必须跟着那张图的
    //     真实位置走（超出部分由容器的 clip 自然裁掉），否则会对不齐 —— 前端用同一份数据算，
    //     天然满足。

    QHash<QString, PreviewStore::Entry> entries;
    QVariantList items;
    int xAcc = 0;                       // 该条在"条内"的左偏移（数据，供定位框按 hoverIndex 取用）
    bool ready = true;
    for (int i : idx) {
        const int iw = widthOf(i);
        const int ix = xAcc;
        xAcc += iw + gap;
        const QString key = previewKeyOf(heightPx, i);
        PreviewStore::Entry e;
        e.loc    = m_seq->at(i).loc;
        e.target = QSize(iw * 2, heightPx * 2);
        entries.insert(key, e);
        if (!previewSettled(key)) ready = false;    // 在飞 / 还没解 ⇒ 这一批**还不能显示**

        QVariantMap m;
        m.insert(QStringLiteral("index"), i);
        m.insert(QStringLiteral("x"), ix);        // 条内左偏移（**数据**，不是渲染位置）
        m.insert(QStringLiteral("w"), iw);
        m.insert(QStringLiteral("h"), heightPx);
        m.insert(QStringLiteral("url"), QStringLiteral("image://preview/") + key);
        items << m;
    }
    {
        QMutexLocker lk(&m_previewStore->mutex);
        //   表的规模上限；**先清后插** —— 以前是插完再清，会把刚放进去的这一批也抹掉，
        //   然后 provider 找不到位置 ⇒ 那一格报 "Failed to get image from provider"。
        if (m_previewStore->entries.size() + entries.size() > 512) m_previewStore->entries.clear();
        for (auto it = entries.constBegin(); it != entries.constEnd(); ++it)
            m_previewStore->entries.insert(it.key(), it.value());
    }
    result.insert(QStringLiteral("items"), items);
    result.insert(QStringLiteral("ready"), ready);
    return result;
}

//   批量并行解码：一批里的缩略图在**线程池**里并行解出来；**同一时刻只跑一批**
void ViewerModel::startPreviewBatch(const QVector<PreviewJob> &jobs)
{
    if (jobs.isEmpty() || !m_previewStore) return;
    m_previewBusy = true;
    for (const PreviewJob &j : jobs) m_previewInFlight.insert(j.key);

    const auto store = m_previewStore;
    if (!m_previewWatcher) {
        m_previewWatcher = new QFutureWatcher<QString>(this);
        //   ★ 唯一的通知入口（用户 2026-11-06）：跑完 = **组件回到空闲**。
        //     不带批次号 ⇒ 前端按"此刻"的位置重算：位置变了就是新需求，没变就把剩下的补上。
        //     ★ 也**不在这里继续起下一批**（以前那套"采样当前位置再起一批"就是组件在调度）。
        QObject::connect(m_previewWatcher, &QFutureWatcherBase::finished, this, [this] {
            m_previewBusy = false;
            m_previewInFlight.clear();                     // 同一时刻只有一批 ⇒ 全清
            const QList<QString> failed = m_previewWatcher->future().results();
            for (const QString &k : failed)
                if (!k.isEmpty()) m_previewFailed.insert(k);   // 解不出来：算已了结、永不重派
            emit previewIdle();
        });
    }
    m_previewWatcher->setFuture(QtConcurrent::mapped(jobs, [store](const PreviewJob &j) -> QString {
        bool hasBytes = false;
        QImage img = imageread::imageScaled(j.loc, j.target, &hasBytes);
        //   ★ 坏图（有字节、解不出）在条上**也要有个东西**（用户 2026-11-06：不能空）：
        //     按 `j.target` **直接画**占位（绝不"画 1080 再缩到 90px"）。
        //     缺图（读不到字节）才是真空位 —— 那种什么都不画。
        if (img.isNull() && hasBytes) img = placeholder::draw(j.target);
        if (img.isNull()) return j.key;                    // 真的没有 ⇒ 报给主线程记一笔
        QMutexLocker lk(&store->mutex);                    // 锁只护内存读写，锁里不解码
        if (store->images.size() >= 512) store->images.clear();   // **先清后插**（别抹掉刚写进去的）
        store->images.insert(j.key, img);
        return QString();
    }));
}

void ViewerModel::shutdownPreviewWork()
{
    //   不再派新活；在飞的那几个回来时照旧落缓存 + 发一次 `previewIdle`（这时已经没人在听了）。
    m_previewStopped = true;
}

//   "这一张有结论了吗" = **解出来了 ∨ 判了失败**（在飞**不算**）
//   ★ 失败必须算"有结论"：否则一张坏图能让整批永远不就绪 ⇒ 预览条永远停在上一次的内容；
//     而且"未就绪 ⇒ 再派"会绕成死循环（派 → 失败 → 未就绪 → 再派）。
bool ViewerModel::previewSettled(const QString &key) const
{
    if (key.isEmpty() || !m_previewStore) return false;
    if (m_previewFailed.contains(key)) return true;
    QMutexLocker lk(&m_previewStore->mutex);
    return m_previewStore->images.contains(key);
}

//   派活（用户 2026-11-06）：**只派此刻还缺的**，而且**忙的时候一概不派**。
//   ★ 幂等：已缓存 / 已判失败 / 正在解 都跳过 ⇒ 前端调多少次都不会重复解。
//   ★ 返回 false 的两种含义对前端是一样的："现在没有活能干，等通知 / 等下一次事件"。
bool ViewerModel::previewEnsure(int center, int heightPx, int maxWidthPx, int gapPx)
{
    if (!m_seq || !m_previewStore || m_previewStopped) return false;
    if (m_previewBusy) return false;                   // ★ 组件忙 ⇒ 不派（同一个时刻只有一批）
    const QVector<int> idx = previewNeighbors(center, heightPx, maxWidthPx, gapPx);
    if (idx.isEmpty()) return false;

    QVector<PreviewJob> jobs;
    jobs.reserve(idx.size());
    for (int i : idx) {
        const QString key = previewKeyOf(heightPx, i);
        if (previewSettled(key) || m_previewInFlight.contains(key)) continue;
        PreviewJob j;
        j.key    = key;
        j.loc    = m_seq->at(i).loc;
        j.target = QSize(previewItemWidth(i, heightPx) * 2, heightPx * 2);
        jobs.append(j);
    }
    if (jobs.isEmpty()) return false;
    startPreviewBatch(jobs);
    return true;
}

int ViewerModel::previewItemWidth(int index, int heightPx) const
{
    if (!m_seq || index < 0 || index >= m_seq->count() || heightPx <= 0) return 0;
    const QSize s = m_seq->at(index).size;
    const qreal ratio = (s.isValid() && s.height() > 0) ? qreal(s.width()) / s.height() : 0.7;
    return qMax(24, int(qRound(heightPx * ratio)));
}

// ══ 前端线（重判）：唯一决定「显示 / 宣布 / 跳屏 / 问密码」的地方 ══════════
//   · 这一屏**已经宣布过** ⇒ 只补图（**不许**再宣布、再挂节拍）；
//   · 还有位置没结论 ⇒ 先把已就绪的格显示出来（双幅可以先出一格），不宣布；
//   · 有结论但一条图都没有：等密码 ⇒ 停在原地问；否则整屏都缺 ⇒ 同方向跳屏；
//   · 全都有结论且至少一张图 ⇒ 显示 + 宣布（**节拍只在这一刻挂**）。
//   以前这里叫 `judge()`，且"上屏"就等于"重新挂节拍" ⇒ 0ms 间隔下闹钟被反复重启。
//
//   ★ 加密压缩包（设计 22 §5.3）：用户提交密码后，清掉"等密码"的结论再判一次（重新派解码）。
//     **只有"提交密码"会调 `retryNeedingPassword()`** —— 不是自动重试。
void ViewerModel::retryNeedingPassword()
{
    //   ★ 同一个容器往往**不止一条**因为它失败（预取/本屏几条都在等同一个包）⇒
    //     拿到密码要**全部清掉**重新取，否则只重取"触发弹框的那一条"，
    //     本屏另一条仍然停在等密码 ⇒ 画面空白，翻页才出来（用户 2026-11-03 实测）。
    //     别的容器里的条目也会被重取一次：密码不对就再次失败、再次追问（符合"继续弹框即可"）。
    m_needsPassword.clear();
    m_promptedItem = -1;                  // 拿到密码了 ⇒ 以后（密码错时）可以再问
    evaluateScreen();
    reconcile();
}

//   用户**取消**了密码输入：`Sequence::declinePassword(key)` 之后那一格从"等密码"变成"缺"
//   （`passwordDeclined()` 判的），前端得重判一次才能跳屏 / 挂节拍。
//   ★ 不重取、不清别的结论：取消 ≠ 密码错，没有"再试一次"的意思。
void ViewerModel::passwordStateChanged()
{
    evaluateScreen();
    reconcile();
}

void ViewerModel::evaluateScreen()
{
    updateLoading();                      // 浮层按当前状态当场算（唯一更新入口）
    if (!m_seq || m_seq->count() <= 0) return;

    const int origin = screenStart(m_pos);    // 跳屏绕一圈回到它就停
    for (int guard = 0; guard < 4096; ++guard) {
        const int start = screenStart(m_pos);
        const QVector<int> items = screenItems(start);
        if (items.isEmpty()) return;

        //   ① 已经宣布过这一屏 ⇒ 只补图（幂等的 armBeat 兜住"播放态但还没挂闹钟"那一瞬间）
        if (isAnnounced(start, items)) {
            updateDisplay(start, items);
            armBeat();
            return;
        }

        //   ② 数三种格：等密码 / 没结论 / 有图（"用户已取消密码"的按"缺"算，不进等密码）
        int pending = 0, ready = 0, needsPw = 0;
        for (int i : items) {
            if (waitsPassword(i) && !passwordDeclined(i)) { ++needsPw; continue; }  // 不是"没结论"，是等用户
            if (cellOf(i) == Cell::Pending) ++pending;
            else if (cellOf(i) == Cell::Ready) ++ready;
        }
        //   ★★ 等密码 ⇒ **先问、不挂节拍、也不渲染**（2026-11-05 用户定：见下面 pending 那段）。
        if (needsPw > 0) {
            maybeAskPassword(items);
            return;
        }
        if (pending > 0) {                // ★ 还有位置没结论 ⇒ 不渲染、不宣布、不挂节拍
            //   ★★ **双幅必须两幅都到稳定态才渲染一次**（2026-11-05 用户定）：
            //     以前"先出已就绪的一格"会在一屏里**刷两次**（一格到、两格到各一次），
            //     渲染压力翻倍；而且**两次提交挤在同一帧里，后一次（＝右半）会被刷新率吃掉**
            //     ⇒ 视觉上就是"左边在动、右边不动"。
            //   ⇒ 未稳定时**什么都不动**：画面上还是上一屏，页码也不跳（老口径一并回来了）。
            return;
        }
        if (ready > 0) {                  // 全屏稳定且有图 ⇒ **一次**渲染 + 宣布（挂节拍）
            updateDisplay(start, items);
            announce(start, items);
            return;
        }

        //   ③ 整屏都缺 ⇒ 跳屏（同方向）。★ 不走 `act()`：跳屏不改方向、也不算一次"换屏操作"
        const int next = nextScreen(start, m_dir);
        if (next < 0 || next == origin) {     // 到头 / 绕一圈回到出发那屏 ⇒ 停
            qCWarning(lcUi).noquote()
                << QStringLiteral("[ui] viewer: screen at %1 is missing and no further screen "
                                  "exists (skipped %2)").arg(start).arg(m_skipped);
            if (m_playing) stopPlaying(tr("End reached · slideshow stopped"));
            return;
        }
        ++m_skipped;
        qCDebug(lcUi).noquote()
            << QStringLiteral("[ui] viewer: screen at %1 is missing, skipping (total skipped %2)")
                   .arg(start).arg(m_skipped);
        m_pos = next;
        clearAnnounced();
        reconcile();                      // 派新屏的活（可能当场写出新的"缺"）
    }
    qCWarning(lcUi) << QStringLiteral("[ui] viewer: evaluateScreen hit the skip guard");
}

//   宣布这一屏：记下屏键 + 走"宣布那一刻"（折叠器 / 推进等待位 / 挂节拍）。
//   ★ 唯一写点。同一屏重复判到这儿也只会在 `isAnnounced()` 处被挡住。
void ViewerModel::announce(int start, const QVector<int> &items)
{
    m_announcedStart = start;
    m_announcedItems = items;
    //   在 `framePresented()` 里换的屏：本帧就画出来；由"数据就绪"触发的换屏：等下一帧才算画过。
    m_screenPainted = m_inFrame;
    ++m_statScreens;                                     // 每秒体检：这一秒上新屏的屏数
    onScreenUp();
}

bool ViewerModel::isAnnounced(int start, const QVector<int> &items) const
{
    return m_announcedStart == start && m_announcedItems == items;
}

void ViewerModel::clearAnnounced()
{
    m_announcedStart = -1;
    m_announcedItems.clear();
}

//   本屏每一格的**前端口径**：
//     · 区间外（文件夹循环跨边界那一格）⇒ 缺（不显示、也不等它）；
//     · 已知缺 ⇒ 缺；有图（真图 / 占位图）⇒ 有图；
//     · **等密码 → 还没结论**（★ 2026-11-05 用户纠正）：它不是"缺"，也不能算"有结论"——
//       图还没拿到，用户也没答，节拍**不许**挂（否则会带着一个永远空着的半页往前翻）。
//       用户取消密码之后才降级成"缺"（那时才允许跳屏 / 挂节拍）。
ViewerModel::Cell ViewerModel::cellOf(int item) const
{
    if (item < 0) return Cell::Missing;
    if (!inLoopScope(item)) return Cell::Missing;
    if (isMissing(item)) return Cell::Missing;
    if (waitsPassword(item) && !passwordDeclined(item)) return Cell::Pending;
    if (waitsPassword(item)) return Cell::Missing;     // 用户取消过 ⇒ 当缺（跳屏）
    if (entryReady(item)) return Cell::Ready;
    return Cell::Pending;
}

//   本屏每个位置都有结论（有图 / 缺）= **不在等缓存**（节拍能不能挂看它）。
//   ★★ **只看本屏**（`screenItems(screenStart(m_pos))`）：预取窗口 `L` 是**后台**的事，
//     从来不是"上屏/挂钟"的条件 —— 翻一屏只等这一屏那一两格（2026-11-05 用户强调）。
//   ★ "等密码（用户还没取消）"算 **没结论** ⇒ 不挂钟（它既不是缺、也不该被翻过去）。
bool ViewerModel::screenSettled() const
{
    if (!m_seq || m_seq->count() <= 0) return false;
    const QVector<int> items = screenItems(screenStart(m_pos));
    for (int i : items)
        if (cellOf(i) == Cell::Pending) return false;
    return true;
}

//   本屏真需要密码才问：挑本屏第一条"等密码"，同一个条目只问一次。
//   ★ 为什么不在解码失败时直接弹：预取会让**别的卷/别的包**的条目也在飞，
//     那边缺密码就把框弹出来 ⇒ 打开一份多卷漫画会连着弹好几个（用户 2026-11-03 实测）。
//   ★★ 判据是"**本屏还有没解决的等密码格**"，**不是**"本屏一条图都没有"
//      （2026-11-05 用户纠正）：双幅里另一半有图、这一半等密码也要问，
//      否则那半页会永远空着却没人通知你。
void ViewerModel::maybeAskPassword(const QVector<int> &items)
{
    for (int i : items) {
        if (!waitsPassword(i)) continue;
        if (passwordDeclined(i)) continue;     // 用户已经取消过 ⇒ 不再问
        if (m_promptedItem == i) return;       // 这一条已经问过（不重复问）
        m_promptedItem = i;
        emit passwordRequired(i, m_needsPassword.value(i, 0));
        return;
    }
}

//   本屏有"等密码"拿不到的图（派生）：界面据此显示"无密码，无法查看"的占位
bool ViewerModel::screenNeedsPassword() const
{
    if (!m_seq || m_seq->count() <= 0) return false;
    return screenWaitsPassword(screenItems(screenStart(m_pos)));
}

//   本屏有没有"要密码"的条目（有就**不跳屏**：停在原地等用户输密码，见设计 22 §5.3）
bool ViewerModel::screenWaitsPassword(const QVector<int> &items) const
{
    for (int i : items)
        if (waitsPassword(i) && !passwordDeclined(i)) return true;
    return false;
}

//   这一条的容器**本次会话已经被用户取消过密码** ⇒ 当"缺"处理。
//   ★ 不这么做的话：用户取消后这一屏永远"等密码" ⇒ 幻灯片停在这一页不动（假死）。
bool ViewerModel::passwordDeclined(int item) const
{
    if (!m_seq || item < 0 || item >= m_seq->count()) return false;
    if (!waitsPassword(item)) return false;
    const Sequence::Item &it = m_seq->at(item);
    const int level = qMax(0, m_needsPassword.value(item, 0));
    const QString key = Sequence::containerKey(it.loc.diskPath, it.loc.members.mid(0, level));
    return m_seq->isPasswordDeclined(key);
}

//   上屏：按**位置**写 `imageA` / `imageB`（有图 ⇒ 那张图；缺 ⇒ 空图），
//   写 `keyCount`（本屏位置数）、`cursor`（本屏起点）、文件名，**变了才** emit。
//   ★★ **一屏只调一次**（2026-11-05 用户定）：调用点只剩"全屏稳定、准备宣布"那一处 ——
//      双幅里绝不在"只到一半"时先刷一次（一屏两次渲染；两次提交挤同一帧，
//      **后一次＝右半**会被刷新率吃掉 ⇒ 看着像"左边更快"）。
//   ★ 还没稳定时**根本不动画面**（不清 `imageA` / `imageB`、不跳页码）：见 `evaluateScreen()`。
//   ★ 按幅判变化、只发变了的那一幅（别让没变的那半白重传一次纹理）。
void ViewerModel::updateDisplay(int start, const QVector<int> &items)
{
    const QImage a = imageOf(items.value(0, -1));
    const QImage b = imageOf(items.value(1, -1));
    //   ★ **按幅判变化、按幅 emit**（2026-11-05 用户报"双幅时第一幅比第二幅刷得勤"）：
    //     以前只要任一格变了就两个信号都发 ⇒ 双幅里第二页晚到时，第一页（没变）也被重新
    //     赋值一次（QML 那边等于重传纹理 / 重画），看起来就是"第一幅一直在刷"。
    const bool aChanged = a.cacheKey() != m_imageA.cacheKey();
    const bool bChanged = b.cacheKey() != m_imageB.cacheKey();
    const bool imageChanged = aChanged || bChanged;
    const bool screenChanged = (m_cursor != start) || (m_keyCount != items.size());

    // 顶栏文件名：按**这一屏真实的条目**取（配对可能跳过缺条目，不能拿 cursor+1 推）
    QString name;
    if (m_seq) {
        const auto nameOf = [this](int idx) {
            return (idx >= 0 && idx < m_seq->count()) ? m_seq->at(idx).displayName : QString();
        };
        name = nameOf(items.value(0, -1));
        const QString bn = nameOf(items.value(1, -1));
        if (!bn.isEmpty()) name += QStringLiteral(" · ") + bn;
    }
    const QString path = relativePathOf(items.value(0, -1));
    const bool metaChanged = (name != m_screenName) || (path != m_screenPath);

    m_imageA     = a;
    m_imageB     = b;
    m_screenName = name;
    m_screenPath = path;
    m_keyCount   = items.size();
    m_cursor     = start;
    updateLoading();                          // 上屏之后也更新一次（浮层与其他状态无关）
    if (aChanged) emit imageAChanged();        // ★ 只发**变了的那一幅**（见上）
    if (bChanged) emit imageBChanged();
    if (!imageChanged && !screenChanged && !metaChanged) return;
    emit currentChanged();
    // 换画面 ⇒ 视图态复位（未锁定时按当前 fit 重算 zoom；锁定 ⇒ 保留 zoom/pan）
    if (!m_zoomLocked && (imageChanged || screenChanged)) applyFit();
}

// ══ OSD 信息行（**按需读取**，无状态；用户 2026-10-21 定案）═════════════════
//   · OSD / EXIF 不归 `Sequence`（它只描述序列），也不做 VM 状态 ⇒ 界面要时才读；
//   · 一次读 = 一次条目探测（格式 + 尺寸 + 页数，**不解码**）+ 元数据解析；
//   · 元数据不截头字节：磁盘文件把**路径**交给解析器（它自己 seek），
//     包内成员给整份字节（本来就在内存）—— 详见 exifmeta.h。
namespace {
const QString kSecFile  = QCoreApplication::translate("fetch::ViewerModel", "File");
const QString kSecImage = QCoreApplication::translate("fetch::ViewerModel", "Image");
const QString kSecExif  = QStringLiteral("EXIF");

QString humanBytes(qint64 n)
{
    if (n < 0) return {};
    if (n < 1024) return QStringLiteral("%1 B").arg(n);
    if (n < 1024 * 1024) return QStringLiteral("%1 KB").arg(n / 1024.0, 0, 'f', 1);
    if (n < qint64(1024) * 1024 * 1024) return QStringLiteral("%1 MB").arg(n / 1048576.0, 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(n / 1073741824.0, 0, 'f', 2);
}
}  // namespace

//   相对图集根的路径：`文件夹/…/压缩包!/包内成员`（容器层数取自 `loc.members`）
QString ViewerModel::relativePathOf(int idx) const
{
    if (!m_seq || idx < 0 || idx >= m_seq->count()) return {};
    const QStringList chain = m_seq->chainAt(idx);
    if (chain.isEmpty()) return {};
    const int depth = qMax(0, m_seq->at(idx).members().size() - 1);   // 容器层数
    const int boundary = chain.size() - depth;                        // 第一个"包内"元素
    QString out;
    for (int i = 0; i < chain.size(); ++i) {
        if (i > 0) out += (i == boundary) ? QStringLiteral("!/") : QStringLiteral("/");
        out += chain.at(i);
    }
    return out;
}

QVariantList ViewerModel::osdColumn(int idx)
{
    QVariantList rows;
    if (!m_seq || idx < 0 || idx >= m_seq->count()) return rows;
    const Sequence::Item &it = m_seq->at(idx);
    const auto add = [&rows](const QString &k, const QString &v, const QString &section) {
        if (k.isEmpty() || v.isEmpty()) return;      // 没有的行不出现（不显示 "—"）
        QVariantMap m;
        m.insert(QStringLiteral("k"), k);
        m.insert(QStringLiteral("v"), v);
        m.insert(QStringLiteral("section"), section);
        rows.append(m);
    };

    // ── 文件（序列 + 上层给的上下文，不需要读盘）──
    add(tr("Index"), QStringLiteral("%1 / %2").arg(idx + 1).arg(totalPages()), kSecFile);
    add(tr("File"), it.displayName, kSecFile);
    const QString tail = relativePathOf(idx);
    if (tail != it.displayName) add(tr("Path"), tail, kSecFile);   // 根目录下与文件名相同 ⇒ 不重复
    add(tr("Folder"), it.folderLabel, kSecFile);
    //   ★ 2026-10-31：图集没了 ⇒ 这一行（图集标题）一并删掉

    // ── 图像（一次探测，不解码）──
    const imageread::Probe pb = imageread::probe(it.loc);
    if (pb.size.isValid()) {
        const QString dims = QStringLiteral("%1×%2").arg(pb.size.width()).arg(pb.size.height());
        add(tr("Resolution"), pb.rotated ? dims + tr(" (rotated by EXIF)") : dims,
            kSecImage);
    }
    QString kind = pb.format.toUpper();
    if (kind.isEmpty()) kind = tr("Unknown");
    if (pb.pages > 1) kind += QStringLiteral(" %1/%2").arg(it.loc.page + 1).arg(pb.pages);
    add(tr("Type"), kind, kSecImage);
    add(tr("Size"), humanBytes(pb.bytes), kSecImage);

    // ── EXIF + 色彩空间（磁盘给路径、成员给整份字节；HEIC/AVIF 走 libheif）──
    exifmeta::Source src;
    if (it.loc.members.isEmpty()) src.path = it.loc.diskPath;
    else                          src.bytes = pb.memberBytes;
    const exifmeta::Info info = exifmeta::read(src, pb.format);
    add(tr("Color space"), info.colorSpace, kSecImage);
    for (const auto &row : info.rows) add(row.first, row.second, kSecExif);
    if (info.total > info.rows.size())
        add(tr("Other"),
            tr("… %1 more").arg(info.total - info.rows.size()), kSecExif);
    return rows;
}

//   一次调用 = 当前这一屏所有可见位置各一列（双幅两列）
QVariantList ViewerModel::osdRows()
{
    QVariantList cols;
    if (!m_seq || m_seq->count() <= 0) return cols;
    const QVector<int> items = screenItems(screenStart(m_pos));
    for (int idx : items) {
        const QVariantList col = osdColumn(idx);
        if (col.isEmpty()) continue;
        QVariantMap column;
        column.insert(QStringLiteral("rows"), col);
        cols.append(column);
    }
    return cols;
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
