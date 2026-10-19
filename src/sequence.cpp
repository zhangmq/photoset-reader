#include "sequence.h"

#include "archive.h"
#include "dirwalker.h"
#include "natural.h"
#include "sevenzcheck.h"

#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QMap>
#include <QSet>
#include <QThread>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

namespace {

// ── 压缩包句柄：**每线程一个最近用过的**（连续读同一个压缩包时不必反复开）──
//   ★ v5：只缓存**最外层**（盘上的那个）；嵌套层用局部 `ArchiveSource::openBytes`
//     （取出来的字节再当容器打开，逐层向下）。
struct ArchiveSlot {
    QString            path;
    ArchiveSource      src;
    QHash<QString, int> index;      // 条目名 → 下标（开一次建一次）
};
thread_local ArchiveSlot t_archive;

ArchiveSource *openCachedArchive(const QString &path)
{
    if (t_archive.path == path && t_archive.src.isOpen()) return &t_archive.src;
    t_archive.src.close();
    t_archive.index.clear();
    t_archive.path.clear();
    // ★ 必须列出**全部**条目：解码一条嵌套条目时要逐层下钻（只列图片会看不到内层容器）
    if (!t_archive.src.open(path, QString(), ArchiveSource::Entries::All)) return nullptr;
    t_archive.path = path;
    for (int i = 0; i < t_archive.src.count(); ++i)
        t_archive.index.insert(t_archive.src.nameAt(i), i);
    return &t_archive.src;
}

int indexOfName(ArchiveSource *a, const QString &name)
{
    for (int i = 0; i < a->count(); ++i)
        if (a->nameAt(i) == name) return i;
    return -1;
}

// ── ★★ v7：**内层包的单槽物化**（用户 2026-10-14 定案）────────────────────
//   物化产物的生命周期 = 该容器在序列里的**区间**（深度优先线性序 ⇒ 一个容器的子项
//   是连续一段）：
//     · 进入区间**物化一次**，区间内反复复用（翻页 / 缩略图 / 预取都走这里）；
//     · **离开区间就释放**（换容器时先 close 旧的 ⇒ 内存丢弃 / RAR 临时文件删除）；
//     · 同一时刻**只有一个内层包被物化**（进程级单槽）；
//     · **不做**确定性命名 / 存在性检查 / LRU / 跨会话保留 —— 一律不做。
//
//   锁：`ArchiveSource::dataAt` 本来就串行（对象内 mutex），所以这里用一把进程级
//   mutex 把"换容器 + 取字节"整体串起来 —— 不新增状态，也不会两个内层包并存。
struct InnerSlot {
    QString       diskPath;      // 最外层盘上容器
    QStringList   chain;         // 到这个内层包**自身**的成员链（空 = 还没物化）
    ArchiveSource src;           // 物化出来的内层包（RAR 时它自己拥有临时文件）
};
QMutex    g_innerMutex;
InnerSlot g_inner;

// 释放单槽（序列换来源 / 退出时调；线程无关 ⇒ 进程内那一份就是它）
void releaseInnerSlot()
{
    QMutexLocker lk(&g_innerMutex);
    g_inner.src.close();
    g_inner.chain.clear();
    g_inner.diskPath.clear();
}

// ★★ **沿链读字节的唯一实现**（链式解析）：
//   `diskPath` = 最外层盘上容器；`containerChain` = 到那个内层包自身的成员链
//   （空 = 目标就在盘上容器里）；`leaf` = 目标成员名。
//   `innerArchiveLimitBytes` = 「内层压缩包上限」（字节）—— **解压前**从归档条目元数据判定；
//     ★ 设计 14 §9.3：**参数传入**（随 `Location` 带），序列不读设置。
//   空 = 读不到（容器打不开 / 成员不存在 / 超过内层包上限）。
QByteArray readBytesAt(const QString &diskPath, const QStringList &containerChain,
                       const QString &leaf, qint64 innerArchiveLimitBytes)
{
    const qint64 limit = innerArchiveLimitBytes;
    QMutexLocker lk(&g_innerMutex);

    // ① 目标就在**盘上那个容器**里（没有内层包要物化）
    if (containerChain.isEmpty()) {
        ArchiveSource *outer = openCachedArchive(diskPath);
        if (!outer) return {};
        const int idx = t_archive.index.value(leaf, -1);
        return idx < 0 ? QByteArray() : outer->dataAt(idx);
    }

    // ② 区间内 ⇒ 复用已物化的内层包（翻页 / 缩略图 / 预取反复读都靠这条）
    if (g_inner.src.isOpen() && g_inner.diskPath == diskPath && g_inner.chain == containerChain) {
        const int idx = indexOfName(&g_inner.src, leaf);
        return idx < 0 ? QByteArray() : g_inner.src.dataAt(idx);
    }

    // ③ 换容器 ⇒ **离开区间就释放**（旧的内存 / 临时文件在这里丢掉）
    g_inner.src.close();
    g_inner.chain.clear();
    g_inner.diskPath.clear();

    // ④ 沿链把内层包的字节取出来：最外层用线程本地的盘上句柄，中间层是局部对象
    //    （局部 `ArchiveSource` 出作用域即析构 ⇒ 中间层的临时文件也当场删）
    ArchiveSource *outer = openCachedArchive(diskPath);
    if (!outer) return {};
    QByteArray cur;
    {
        const int idx = t_archive.index.value(containerChain.first(), -1);
        if (idx < 0) return {};
        if (outer->sizeAt(idx) > limit) {
            qWarning().noquote()
                << QStringLiteral("[seq] 内层包超过上限（%1 MB > %2 MB），不展开：%3")
                       .arg(outer->sizeAt(idx) / 1048576.0, 0, 'f', 1)
                       .arg(limit / 1048576)
                       .arg(containerChain.first());
            return {};
        }
        cur = outer->dataAt(idx);
    }
    for (int i = 1; i < containerChain.size(); ++i) {
        ArchiveSource mid;
        if (!mid.openBytes(cur, containerChain.at(i - 1))) return {};
        const int idx = indexOfName(&mid, containerChain.at(i));
        if (idx < 0) return {};
        if (mid.sizeAt(idx) > limit) {
            qWarning().noquote()
                << QStringLiteral("[seq] 内层包超过上限（%1 MB > %2 MB），不展开：%3")
                       .arg(mid.sizeAt(idx) / 1048576.0, 0, 'f', 1)
                       .arg(limit / 1048576)
                       .arg(containerChain.at(i));
            return {};
        }
        cur = mid.dataAt(idx);
    }
    if (cur.isEmpty()) return {};

    // ⑤ 物化：**同一个 openBytes**（RAR 由 ArchiveSource 自己在内部落临时文件）
    if (!g_inner.src.openBytes(cur, containerChain.last())) return {};
    g_inner.diskPath = diskPath;
    g_inner.chain = containerChain;
    const int idx = indexOfName(&g_inner.src, leaf);
    return idx < 0 ? QByteArray() : g_inner.src.dataAt(idx);
}

// 一个 `Location` 的读取：`members` = [c1, …, cn, leaf]（空 = 盘上文件，不走这里）
QByteArray readLocation(const Sequence::Location &loc)
{
    if (loc.members.isEmpty()) return {};
    const QStringList containerChain = loc.members.mid(0, loc.members.size() - 1);
    return readBytesAt(loc.diskPath, containerChain, loc.members.last(),
                       loc.innerArchiveMaxMb * 1024 * 1024);
}

// ── 把条目打开成 `QImageReader` 的**唯一实现**（读尺寸 / 解码都走它）──
class EntryReader
{
public:
    explicit EntryReader(const Sequence::Item &it)
    {
        if (it.loc.members.isEmpty()) {
            m_r.setFileName(it.loc.diskPath);
        } else {
            // ★ v7：沿链取字节 = `readBytesAt`（**内层包单槽物化**：区间内复用、离开即释放）
            m_bytes = readLocation(it.loc);
            if (m_bytes.isEmpty()) return;
            m_buf.setData(m_bytes);
            if (!m_buf.open(QIODevice::ReadOnly)) return;
            m_r.setDevice(&m_buf);
        }
        m_r.setAutoTransform(true);
        if (it.loc.page > 0) m_r.jumpToImage(it.loc.page);
        m_ok = true;
    }

    bool ok() const { return m_ok; }
    QSize size() { return m_ok ? m_r.size() : QSize(); }
    QImage read() { return m_ok ? m_r.read() : QImage(); }

private:
    QByteArray   m_bytes;
    QBuffer      m_buf;
    QImageReader m_r;
    bool         m_ok = false;
};

// 一张源图有几页（多页 TIFF 等）；1 = 普通单页图
int pageCountOf(const QString &path)
{
    QImageReader r(path);
    const int n = r.imageCount();
    return n > 1 ? n : 1;
}

QString parentRelOf(const QString &rel)
{
    const int slash = rel.lastIndexOf(QLatin1Char('/'));
    return (slash < 0) ? QString() : rel.left(slash);
}

void sortNatural(QStringList &v)
{
    std::sort(v.begin(), v.end(),
              [](const QString &a, const QString &b) { return naturalCompare(a, b) < 0; });
}

QString joinRel(const QString &a, const QString &b)
{
    if (a.isEmpty()) return b;
    if (b.isEmpty()) return a;
    return a + QLatin1Char('/') + b;
}

// ★★ v5 **行 → 读取位置**（沿链解析的**唯一实现**）
//   · 祖先链 = 沿 `parentOrd` 上溯（DFS 前序 ⇒ 父行一定在前面）；
//   · 第一个**容器行**（非 folder）= 最外层容器 ⇒ 它就是磁盘路径；
//   · 之后每遇到一个容器行，就把"当前层累积的路径"作为**一个成员**推入链；
//   · 叶子（image）的名字最后推入（页行 label 为空 ⇒ 不推；页号由 `pageIndex` 表达）。
//   · `innerArchiveMaxMb` = **调用方传**的内层包上限（随 `Location` 带走；序列不读设置）
Sequence::Location locateOne(const QString &rootPath, bool rootIsArchive,
                             const QHash<int, int> &ordToRow, const QVector<Sequence::Row> &rows,
                             int i, qint64 innerArchiveMaxMb = Sequence::kDefaultInnerArchiveMaxMb)
{
    Sequence::Location loc;
    loc.innerArchiveMaxMb = innerArchiveMaxMb;
    if (i < 0 || i >= rows.size()) return loc;

    QVector<int> chain;
    for (int k = i; k >= 0; k = ordToRow.value(rows.at(k).parentOrd, -1))
        chain.prepend(k);

    int firstContainer = -1;
    for (int c = 0; c < chain.size(); ++c)
        if (rows.at(chain.at(c)).kind != Sequence::Kind::Folder) { firstContainer = c; break; }

    QString cur;      // 当前层里累积的路径段
    int start = 0;
    if (rootIsArchive) {
        loc.diskPath = rootPath;                 // 来源根本身就是容器
        start = 0;
    } else if (firstContainer >= 0) {
        for (int c = 0; c < firstContainer; ++c)
            cur = joinRel(cur, rows.at(chain.at(c)).label);
        loc.diskPath = joinRel(rootPath, joinRel(cur, rows.at(chain.at(firstContainer)).label));
        cur.clear();
        start = firstContainer + 1;
    } else {
        for (int c = 0; c < chain.size(); ++c)
            cur = joinRel(cur, rows.at(chain.at(c)).label);
        loc.diskPath = joinRel(rootPath, cur);
        loc.page = qMax(0, rows.at(i).pageIndex);
        return loc;
    }

    for (int c = start; c < chain.size(); ++c) {
        const Sequence::Row &r = rows.at(chain.at(c));
        cur = joinRel(cur, r.label);
        if (r.kind == Sequence::Kind::Folder) continue;
        if (r.kind == Sequence::Kind::Image) continue;       // 叶子：最后统一推入
        if (!cur.isEmpty()) { loc.members << cur; cur.clear(); }   // 嵌套容器：下钻一层
    }
    if (!cur.isEmpty()) loc.members << cur;
    loc.page = qMax(0, rows.at(i).pageIndex);
    return loc;
}

} // namespace

// ══ 构造 / 析构 ══════════════════════════════════════════════════════
Sequence::Sequence() = default;

// 数据快照拷贝（**不含扫描机制**）—— 记录 / 扫完标志 / 打开链**一起按值拷**（不是派生量：
//   它们由"并入顺序"决定，拷完必须与源逐值一致，**不能**留给访问器去重算）。
Sequence::Sequence(const Sequence &o)
    : m_items(o.m_items), m_tree(o.m_tree),
      m_subFirst(o.m_subFirst), m_subCount(o.m_subCount),
      m_nodeClosed(o.m_nodeClosed), m_openStack(o.m_openStack),
      m_rows(o.m_rows),
      m_scanOptions(o.m_scanOptions),
      m_rowIndexByOrd(o.m_rowIndexByOrd), m_nodeIndexByOrd(o.m_nodeIndexByOrd),
      m_rootInit(o.m_rootInit), m_markMissingBySize(o.m_markMissingBySize),
      m_gStart(o.m_gStart), m_gSize(o.m_gSize), m_pending(o.m_pending), m_decided(o.m_decided),
      m_unsupported(o.m_unsupported), m_root(o.m_root), m_recursive(o.m_recursive),
      m_archivePages(o.m_archivePages), m_rootIsArchive(o.m_rootIsArchive),
      m_state(o.m_state.load(std::memory_order_relaxed))
{
}

Sequence &Sequence::operator=(const Sequence &o)
{
    if (this == &o) return *this;
    joinWorker();                            // ★ 已放弃 ⇒ 跳过（见 joinWorker）
    m_items = o.m_items;
    m_tree = o.m_tree;
    // ★ 记录 / 扫完标志 / 打开链都是**并入顺序的产物**（不是可重算的派生量）⇒ 一起按值拷，
    //   否则新对象读到的记录会是空的（读方法零写入 ⇒ 没有任何"补齐"的机会）。
    m_subFirst = o.m_subFirst;
    m_subCount = o.m_subCount;
    m_nodeClosed = o.m_nodeClosed;
    m_openStack = o.m_openStack;
    m_chainBuf.clear();
    m_rows = o.m_rows;
    m_scanOptions = o.m_scanOptions;
    m_rowIndexByOrd = o.m_rowIndexByOrd;
    m_nodeIndexByOrd = o.m_nodeIndexByOrd;
    m_rootInit = o.m_rootInit;
    m_markMissingBySize = o.m_markMissingBySize;
    m_gStart = o.m_gStart;
    m_gSize = o.m_gSize;
    m_pending = o.m_pending;
    m_decided = o.m_decided;
    m_unsupported = o.m_unsupported;
    m_root = o.m_root;
    m_recursive = o.m_recursive;
    m_archivePages = o.m_archivePages;
    m_rootIsArchive = o.m_rootIsArchive;
    m_state.store(o.m_state.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return *this;
}

// ★ **唯一的 join 点**：已放弃 ⇒ **跳过 join**（detached 的生产者线程可能永远回不来，
//   join 就又卡死 —— 那正是"放弃"要避免的事）。**不新增状态**：只读 `m_abandoned`。
void Sequence::joinWorker()
{
    if (m_abandoned.load(std::memory_order_relaxed)) return;
    if (!m_worker.joinable()) return;
    m_cancel.store(true, std::memory_order_relaxed);
    m_worker.join();
}

// ★★ **放弃**（消费侧看门狗专用；在 `requestStop()` 之后调）：
//   · 置停止标志（复用既有 cancel 语义）+ 置"已放弃"；
//   · **detach 生产者线程**（不 join）；
//   · ⚠️ 此后本对象**只能泄漏** —— detached 线程还拿着 `this`，销毁就是 UAF。
//     调用方（`LibraryCore::reapSlot`）负责 `release()`，**不释放**它。
void Sequence::abandon()
{
    m_cancel.store(true, std::memory_order_relaxed);
    m_abandoned.store(true, std::memory_order_relaxed);
    if (m_worker.joinable()) m_worker.detach();
}

void Sequence::setPublishHook(std::function<void()> hook)
{
    // ⚠️ 约定：必须在 `startScan()` **之前**调（生产者启动后固定 ⇒ 无锁读安全）。
    //   误在扫描中调 ⇒ 与生产者读 `m_publishHook` 竞争（调用方责任，见 sequence.h）。
    m_publishHook = std::move(hook);
}

// ★★ **发布**（生产者的唯一调用点）：见 `setPublishHook` 的四条约定。
//   · 未设置 ⇒ 一次空 function 判断就返回（**零开销**）；
//   · 异常兜底吞掉（钩子抛异常**不许**越过这里改变扫描语义）；
//   · **调用点在队列锁之外**（`post()` / `writeTerminal()` 都保证）⇒
//     钩子里跑多久都不会拖住消费者的 `drain()`。
void Sequence::publish()
{
    if (!m_publishHook) return;
    try {
        m_publishHook();
    } catch (...) {
        // 正确的钩子不抛；真抛了也只是"这次没唤醒"，不能让生产者线程死掉。
    }
}

Sequence::~Sequence()
{
    // ★ 已放弃 ⇒ 既不 join（会卡死）也不该走到这里（对象必须被泄漏，见 abandon()）
    joinWorker();
}

// ══ 定组（**从头锚定**，边收边定）═════════════════════════════════════

void Sequence::resetGrouping()
{
    m_gStart.clear();
    m_gSize.clear();
    m_pending = -1;
    m_decided = 0;
}

// 宽窄判定条件（**唯一实现**）：宽 > 高 × 1.2 ⇒ 宽图（单独成屏）；尺寸未知 ⇒ 当作不窄
static bool narrowSize(const QSize &s)
{
    if (!s.isValid() || s.isEmpty()) return false;
    return double(s.width()) <= double(s.height()) * 1.2;
}

bool Sequence::isNarrow(int index) const
{
    if (index < 0 || index >= m_items.size()) return false;
    return narrowSize(m_items.at(index).size);
}

void Sequence::appendItem(const Item &it)
{
    m_items << it;
    m_ownNext << -1;                         // ★ 直接条目的链：新条目先当链尾
    m_gStart << 0;
    m_gSize  << 0;
    const int k = m_items.size() - 1;

    // ★★ **记录 = 并入条目那一刻沿"它所属节点 → 根"维护**（L41）—— **唯一的写点**：
    //   · **张数 +1**（含全部后代的 `m_subCount`）；
    //   · **第一条取更小**；`-1` = "还没有第一条"，**不参与取最小** ⇒
    //     一个还没有任何图的节点**不会**把父节点的"第一条"拉成 0（旧递归的毛病，见 L41）；
    //   · **代价 = 树深**，不是节点数；没有"收尾时全量重算"、也没有"查询时重算"；
    //   · `firstItem/itemCount` = 该节点**直接**产出的图（子容器的另算 ⇒ 分节不重叠）：
    //     只在**它所属的那个节点**上长（祖先拿到的是含后代的那两个数）；
    //   · ★ 直接条目**不是连续区间**（先根遍历会被子容器切开）⇒ 顺手把它们串成链
    //     （`firstItem` = 链头、`lastOwn` = 链尾、`m_ownNext` = 下一条）。
    const int pn = it.parentNode;
    if (pn >= 0 && pn < m_tree.size()) {
        TreeNode &owner = m_tree[pn];
        if (owner.itemCount <= 0) owner.firstItem = k;   // 该节点的第一条**直接**条目
        if (owner.lastOwn >= 0 && owner.lastOwn < m_ownNext.size())
            m_ownNext[owner.lastOwn] = k;                // 接在链尾后面
        owner.lastOwn = k;
        owner.itemCount += 1;
        for (int x = pn; x >= 0 && x < m_tree.size(); x = m_tree.at(x).parent) {
            if (m_subFirst.at(x) < 0 || k < m_subFirst.at(x)) m_subFirst[x] = k;
            m_subCount[x] += 1;
        }
    }

    if (m_pending < 0) {
        m_pending = k;
        m_decided = k;
        return;
    }

    const bool pair = narrowSize(m_items.at(m_pending).size) && narrowSize(m_items.at(k).size);
    if (pair) {
        m_gStart[m_pending] = m_pending;
        m_gSize[m_pending]  = 2;
        m_gStart[k]         = m_pending;
        m_gSize[k]          = 0;
        m_pending = -1;
        m_decided = m_items.size();
    } else {
        m_gStart[m_pending] = m_pending;
        m_gSize[m_pending]  = 1;
        m_pending = k;
        m_decided = k;
    }
}

void Sequence::finishAppend()
{
    if (m_pending >= 0) {
        m_gStart[m_pending] = m_pending;
        m_gSize[m_pending]  = 1;
        m_pending = -1;
    }
    m_decided = m_items.size();
    // ★ 行流结束 ⇒ **剩下的打开节点全部扫完**（扫描到底是 Scaned / ScanStoped 都算；
    //   库行恢复（`restore`）走的也是这里 —— 两条来源共用同一份收尾代码路径）。
    closeAllOpenNodes();
    finalizePageRowNames();
    m_state.store(State::Scaned, std::memory_order_relaxed);
}

// ── 建节点：记录 / 扫完标志（与 `m_tree` 等长的并行数组）**一起长出来** ──
int Sequence::addTreeNode(const TreeNode &n)
{
    m_tree << n;
    m_subFirst << -1;            // -1 = "还没有第一条"（不参与取最小；见 subtreeFirst）
    m_subCount << 0;
    m_nodeClosed << 0;           // 新节点那一支当然还没扫完
    return m_tree.size() - 1;
}

// ★★ **扫完**（L40）：行是**前序 DFS** 并入的（父行在子行之前）⇒ 节点 N"还开着"当且仅当
//   它是"最近一行所属节点"的**祖先（或它自己）**。新行属于 `parentNode` 的子树 ⇒ 打开的
//   链就是 `parentNode` 的祖先链；与它**不再共享的那些**节点从此再无属于自己子树的条目 ⇒ 扫完。
//   · 实现 = 取两条链的**最长公共前缀**，把多出来的后缀逐个标成"已扫完"；
//   · 代价 = O(树深)（`m_chainBuf` 复用，不分配）；
//   · 数据若不是前序（父行在后）也**不会崩**：公共前缀之后一律扫完，行为是"尽力而为"。
void Sequence::syncOpenChain(int parentNode)
{
    m_chainBuf.clear();
    for (int x = parentNode; x >= 0 && x < m_tree.size()
                           && m_chainBuf.size() <= m_tree.size();     // 防畸形环
         x = m_tree.at(x).parent)
        m_chainBuf << x;
    std::reverse(m_chainBuf.begin(), m_chainBuf.end());               // 根 → parentNode
    int keep = 0;
    while (keep < m_chainBuf.size() && keep < m_openStack.size()
           && m_chainBuf.at(keep) == m_openStack.at(keep))
        ++keep;
    for (int i = keep; i < m_openStack.size(); ++i) {
        m_nodeClosed[m_openStack.at(i)] = 1;
        writeBoundaryRange(m_openStack.at(i));          // ★ 扫完 ⇒ 它的图 index 区间已定
    }
    m_openStack = m_chainBuf;
}

// 行流结束 ⇒ 打开链上剩下的节点**全部扫完**（"收尾"是唯一一次批量扫完）
void Sequence::closeAllOpenNodes()
{
    for (int x : m_openStack) {
        if (x >= 0 && x < m_nodeClosed.size()) m_nodeClosed[x] = 1;
        writeBoundaryRange(x);
    }
    m_openStack.clear();
}

// ★★ 把容器**子树覆盖的图的 index 区间**写回它那一行（`Row::firstIndex/lastIndex`）。
//   · `m_subFirst/m_subCount` = 并入条目时沿祖先链维护的**子树记录**（这里是只读）；
//   · 只在**行流路径**上写：静态构建器（`buildFromFolder` 那几条）没有 `m_rows`，
//     `m_rowIndexByOrd` 里查不到 ⇒ 直接返回（那种路径本来也不落库）；
//   · 空容器（子树里没有图）不写（它也不会成为元素，见 `isElement`）。
void Sequence::writeBoundaryRange(int node)
{
    if (node < 0 || node >= m_tree.size()) return;
    if (node >= m_subFirst.size() || node >= m_subCount.size()) return;
    const int first = m_subFirst.at(node);
    const int cnt   = m_subCount.at(node);
    if (first < 0 || cnt <= 0) return;
    const auto it = m_rowIndexByOrd.constFind(m_tree.at(node).ord);
    if (it == m_rowIndexByOrd.constEnd()) return;
    if (*it < 0 || *it >= m_rows.size()) return;
    Row &r = m_rows[*it];
    r.firstIndex = first;
    r.lastIndex  = first + cnt - 1;
    //   ★ 顺手进**容器区间表 + 两个序**：那个"运行期的索引"就在**容器收尾这一刻**建好
    //     （不在查询时重建）。代价 = 每个容器 O(k) 的搬移（k = 文件夹 / 压缩包个数）。
    Container c;
    c.node  = node;
    c.first = first;
    c.last  = r.lastIndex;
    c.level = m_tree.at(node).level;
    m_containers << c;
    indexContainer(m_containers.size() - 1);
}

//   把一个容器插进两个序（各自按 (key, level) 升序；并列 ⇒ **最上层在前**）。
void Sequence::indexContainer(int ci)
{
    const auto beforeFirst = [this](int lhs, int rhs) {
        const Container &a = m_containers.at(lhs);
        const Container &b = m_containers.at(rhs);
        if (a.first != b.first) return a.first < b.first;
        if (a.level != b.level) return a.level < b.level;
        return a.last > b.last;
    };
    const auto beforeLast = [this](int lhs, int rhs) {
        const Container &a = m_containers.at(lhs);
        const Container &b = m_containers.at(rhs);
        if (a.last != b.last) return a.last < b.last;
        if (a.level != b.level) return a.level < b.level;
        return a.first > b.first;
    };
    m_byFirst.insert(std::upper_bound(m_byFirst.cbegin(), m_byFirst.cend(), ci, beforeFirst), ci);
    m_byLast.insert(std::upper_bound(m_byLast.cbegin(), m_byLast.cend(), ci, beforeLast), ci);
}

// 清记录（**只有"重排同一批条目"才用**：`sortNaturally` 会把 `m_items` 整个重并一遍 ⇒
//   必须先把记录清零，否则沿链累加会**重复计数**）。扫完标志 / 打开链不属于记录，不动。
void Sequence::resetNodeRecords()
{
    m_containers.clear();                    // 区间表 + 两个序是"并入序的产物" ⇒ 重排要重建
    m_byFirst.clear();
    m_byLast.clear();
    for (TreeNode &n : m_tree) { n.firstItem = 0; n.lastOwn = -1; n.itemCount = 0; }
    for (int i = 0; i < m_subFirst.size(); ++i) m_subFirst[i] = -1;
    for (int i = 0; i < m_subCount.size(); ++i) m_subCount[i] = 0;
}

// ── 页行显示名（label 为空 ⇒ "父容器名 [页/总页数]"）；区间本身**已经在并入时维护好** ──
void Sequence::finalizePageRowNames()
{
    for (int i = 0; i < m_items.size(); ++i) {
        Item &it = m_items[i];
        if (!it.label.isEmpty() || it.parentNode < 0 || it.parentNode >= m_tree.size()) continue;
        const TreeNode &pn = m_tree.at(it.parentNode);
        if (pn.itemCount <= 0) continue;
        it.displayName = QStringLiteral("%1 [%2/%3]").arg(pn.label)
                             .arg(it.loc.page + 1).arg(pn.itemCount);
    }
}

// ══ 查询（配对表）════════════════════════════════════════════════════
//   ★ 全部**不带版面**：单幅 / 双幅是设置（`app::Layout`），由持有它的那一层去选；
//     序列这里只回答**配对**这份数据（从头锚定算出来的原生划分）。

int Sequence::pairStart(int index) const
{
    if (m_decided <= 0) return -1;
    const int i = qBound(0, index, m_decided - 1);
    return qBound(0, m_gStart.value(i, i), m_decided - 1);
}

int Sequence::pairSize(int index) const
{
    if (m_decided <= 0) return 0;
    const int start = pairStart(index);
    if (start < 0) return 0;
    return qBound(1, m_gSize.value(start, 1), m_decided - start);
}

int Sequence::nextPairStart(int index) const
{
    if (m_decided <= 0) return -1;
    const int start = pairStart(index);
    if (start < 0) return -1;
    const int next = start + pairSize(start);       // 前进量 = 本对张数（1 或 2）
    return (next < m_decided) ? next : -1;
}

int Sequence::prevPairStart(int index) const
{
    if (m_decided <= 0) return -1;
    const int start = pairStart(index);
    if (start < 0) return -1;
    const int before = start - 1;
    if (before < 0) return -1;
    return pairStart(before);                      // 上一对的起点（完全对称）
}

//   本对 = [本对起点, +本对张数)——**从头锚定**算出来的原生划分。
//   ★ **不给 missing 留任何处理**：这一对给的是**完整的位置**（双幅 2 个 / 宽图或末尾 1 个），
//     "哪一格拿不出图"由缓存那条线判（设计 12 §8.9 四：双幅留空位、其余跳过）⇒
//     序列说位置，缓存说好坏，两边不重复管同一件事。
Sequence::Group Sequence::pairAt(int index) const
{
    Group g;
    if (m_decided <= 0) return g;

    const int start = pairStart(index);
    if (start < 0) return g;

    const int n = pairSize(start);
    g.cursor = start;
    for (int i = start; i < start + n && i < m_decided; ++i) g.members << i;
    return g;
}

// ══ 容器查询（文件树）══════════════════════════════════════════════════

// ★★ 子树记录：**纯查表只读**（口径的单一事实源 = `sequence.h` 上这两行的注释）。
//   值由 `appendItem` 沿"它所属节点 → 根"维护 ⇒ **这里不写任何东西**
//   （没有 `mutable`、没有"脏了就重算"、没有遍历）—— 读方法零写入。
//   越界 ⇒ 0（既有边界约定，不动；返回 0 与"节点没有图"的 -1 **不是一回事**）。
int Sequence::subtreeFirst(int node) const
{
    if (node < 0 || node >= m_tree.size()) return 0;
    return m_subFirst.at(node);
}

int Sequence::subtreeCount(int node) const
{
    if (node < 0 || node >= m_tree.size()) return 0;
    return m_subCount.at(node);
}

// ★★ **是不是元素**（L40 的**唯一实现**；口径见 `sequence.h`）：
//   `已扫完` **且** `子树里至少有一条图`。空目录 / 无图压缩包 / 不受支持的压缩包
//   （它们要么没有图、要么根本没进树）一律为假 ⇒ 不出现在任何枚举里。
bool Sequence::isElement(int node) const
{
    if (node < 0 || node >= m_tree.size()) return false;
    return m_nodeClosed.at(node) != 0 && m_subCount.at(node) > 0;
}

// ★★ **这一条所在容器的子树区间** = {第一条图的 index, 图条数}，**O(1)**：
//   条目并入时就记下了它**直接所属**的那一层容器（`Item::parentNode`），该节点的子树区间
//   是并入时沿祖先链维护的 ⇒ 两者都是查表。⇒ 循环（文件夹循环）的区间也用它，**不搜全树**。
QPair<int, int> Sequence::ownerRange(int index) const
{
    if (index < 0 || index >= m_items.size()) return { 0, count() };
    const int node = m_items.at(index).parentNode;
    if (node < 0 || node >= m_tree.size()) return { 0, count() };
    const int first = subtreeFirst(node);
    const int cnt   = subtreeCount(node);
    if (first < 0 || cnt <= 0) return { 0, count() };     // 记录还没长出来 ⇒ 当作整条序列
    return { first, cnt };
}

// ══ 直接条目（**不是连续区间**，见头文件）══════════════════════════════
void Sequence::directItems(int node, QVector<int> *out) const
{
    out->clear();
    if (node < 0 || node >= m_tree.size()) {         // 没有树（单张图 / 多页文件根）⇒ 整条序列
        out->reserve(m_items.size());
        for (int k = 0; k < m_items.size(); ++k) out->append(k);
        return;
    }
    const TreeNode &n = m_tree.at(node);
    if (n.itemCount <= 0) return;
    out->reserve(n.itemCount);
    for (int k = n.firstItem; k >= 0 && k < m_items.size() && k < m_ownNext.size();
         k = m_ownNext.at(k)) {
        out->append(k);
        if (out->size() >= n.itemCount) break;       // 链长 = 直接条目数（防御）
    }
}

// ══ 身份链（v5 resume）════════════════════════════════════════════════

int Sequence::parentNodeOfItem(int index) const
{
    if (index < 0 || index >= m_items.size()) return -1;
    return m_items.at(index).parentNode;
}

int Sequence::childNodeWithLabel(int parent, const QString &label) const
{
    for (int i = 0; i < m_tree.size(); ++i) {
        const TreeNode &n = m_tree.at(i);
        const bool child = (parent < 0) ? (n.parent < 0) : (n.parent == parent);
        if (child && n.label == label) return i;
    }
    return -1;
}

QStringList Sequence::chainAt(int index) const
{
    QStringList out;
    if (index < 0 || index >= m_items.size()) return out;
    const int node = parentNodeOfItem(index);
    for (int n = node; n >= 0 && n < m_tree.size() && m_tree.at(n).parent >= 0;
         n = m_tree.at(n).parent)
        out.prepend(m_tree.at(n).label);
    if (!m_items.at(index).label.isEmpty()) out << m_items.at(index).label;
    return out;
}

int Sequence::pageAt(int index) const
{
    if (index < 0 || index >= m_items.size()) return 0;
    return m_items.at(index).loc.page;
}

int Sequence::indexOfChain(const QStringList &chain, int page) const
{
    if (chain.isEmpty()) return -1;
    int cur = -1;
    for (int i = 0; i < m_tree.size(); ++i)
        if (m_tree.at(i).parent < 0) { cur = i; break; }

    for (int s = 0; s + 1 < chain.size(); ++s) {
        const int child = childNodeWithLabel(cur, chain.at(s));
        if (child < 0) return -1;
        cur = child;
    }
    const QString last = chain.last();

    auto directLeaf = [&](int node) -> int {
        QVector<int> own;
        directItems(node, &own);                 // ★ 直接条目 = 链，不是连续区间
        for (int k : own)
            if (m_items.at(k).label == last && m_items.at(k).loc.page == page) return k;
        return -1;
    };

    // A) 直接子图（普通图 = 自己的名字；页行 = 空名字 + 页号）
    if (const int k = directLeaf(cur); k >= 0) return k;
    // B) 子容器（多页文件 / 压缩包）：它的**直接**子项里按页号命中
    const int child = childNodeWithLabel(cur, last);
    if (child >= 0) {
        QVector<int> own;
        directItems(child, &own);
        for (int k : own)
            if (m_items.at(k).loc.page == page) return k;
    }
    return -1;
}

QString Sequence::encodeChain(const QStringList &chain)
{
    QJsonArray arr;
    for (const QString &s : chain) arr.append(s);
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QStringList Sequence::decodeChain(const QString &encoded)
{
    QStringList out;
    if (encoded.trimmed().isEmpty()) return out;
    const QJsonDocument doc = QJsonDocument::fromJson(encoded.toUtf8());
    if (!doc.isArray()) return out;
    for (const QJsonValue &v : doc.array()) out << v.toString();
    return out;
}

// ══ 条目读取（**唯一实现**；解码线程也用）══════════════════════════════

QSize Sequence::imageSize(const Item &it)
{
    EntryReader r(it);
    return r.size();
}

QImage Sequence::image(const Item &it)
{
    EntryReader r(it);
    return r.read();
}

// ══ 行 → 位置（静态入口；库探针 / 封面 / 恢复共用）══════════════════

QVector<Sequence::Location> Sequence::locateRows(const QString &rootPath, bool rootIsArchive,
                                                  const QVector<Row> &rows)
{
    QHash<int, int> ordToRow;
    ordToRow.reserve(rows.size() * 2);
    for (int i = 0; i < rows.size(); ++i) ordToRow.insert(rows.at(i).ord, i);
    QVector<Location> out;
    out.reserve(rows.size());
    for (int i = 0; i < rows.size(); ++i)
        out << locateOne(rootPath, rootIsArchive, ordToRow, rows, i);
    return out;
}

// ══ 交接：**一把锁 + 无界批队列**（扫描线程 → 消费线程）═════════════════

// **消费线程**：收下已产出的批次（在这里建树 / 定组 / 增长数量 / 推进状态）
bool Sequence::drain()
{
    // ★ **状态先读（acquire）、队列后取** —— 次序是正确性的一部分：
    //   生产者"放完最后一批 → release 写终态"。消费者只有在**取队之前**读到终态，
    //   才保证"最后一批的 push"（同一把锁）**已经 happens-before** 这次 `swap` ⇒ 不会漏批。
    //   反过来（先 swap 后读）会出现："终态先被看到 + 最后一批还留在队里 + `finishAppend`
    //   已经跑完" ⇒ 那一批在收尾之后才并入，分组被定死（末尾两条窄图不再配对）。
    const State s = m_state.load(std::memory_order_acquire);
    const bool terminal = (s == State::Scaned || s == State::ScanStoped);

    std::deque<Batch> local;
    {
        QMutexLocker lk(&m_qMutex);
        local.swap(m_q);                     // O(1)：整队拿走，**出锁再处理**
    }
    bool changed = !local.empty();
    for (Batch &b : local) {
        if (b.rootSet && !m_rootInit) {      // 根描述到了才有树 / 或无树
            initRoot(b.rootHasTree, b.rootKind, b.rootLabel);
            m_rootInit = true;
        }
        if (!b.rows.isEmpty()) applyRows(b.rows);   // 传 const 引用，不拷贝
    }

    if (terminal && !m_finished) {           // "扫描结束"不再走队列（没有结束专用格）
        m_finished = true;
        finishAppend();                      // 它自己会写 Scaned（静态构建路径的唯一入口）
        // ★ 守住**生产者写的终态**：`ScanStoped` 不能被 `finishAppend()` 的 Scaned 顶掉
        //   （旧形态是"结束格的值最后写一次"，这里等价）。
        if (s != State::Scaned) m_state.store(s, std::memory_order_release);
        changed = true;
    }
    if (!terminal && m_decided > 0) {        // ★ ImageFound：**只在 NotReady 时 CAS**
        State exp = State::NotReady;         //   ⇒ 终态不可被覆盖
        m_state.compare_exchange_strong(exp, State::ImageFound);
    }
    return changed;
}

void Sequence::cancelScan()
{
    joinWorker();                            // ★ 已放弃 ⇒ 跳过（见 joinWorker）
    if (m_state.load(std::memory_order_relaxed) != State::Scaned)
        m_state.store(State::ScanStoped, std::memory_order_release);
}

// ══ 行 → 树 + 叶子（**唯一入口**）═════════════════════════════════════

// ★ v7：瞬时提示（扫描线程写 / GUI 侧读；互斥保护，**不写入磁盘**）
QString Sequence::noticeText() const
{
    QMutexLocker lk(&m_noticeMutex);
    return m_notice;
}

void Sequence::setNotice(const QString &text)
{
    QMutexLocker lk(&m_noticeMutex);
    m_notice = text;
}

void Sequence::resetRows()
{
    releaseInnerSlot();                  // ★ v7：序列换了 ⇒ 物化的内层包立刻释放
    m_items.clear();
    m_ownNext.clear();
    m_tree.clear();
    m_subFirst.clear();                  // ★ 记录 / 扫完标志：随树一起清（不留脏值）
    m_subCount.clear();
    m_nodeClosed.clear();
    m_openStack.clear();
    m_chainBuf.clear();
    m_rows.clear();
    m_rowIndexByOrd.clear();
    m_nodeIndexByOrd.clear();
    m_rootInit = false;
    m_markMissingBySize = false;
    resetGrouping();
}

void Sequence::initRoot(bool hasTree, Kind root, const QString &rootLabel)
{
    if (!hasTree) {                           // 没有树（单张图）
        m_openStack.clear();                  // 没有节点 ⇒ 也没有任何"打开"的节点
        m_nodeIndexByOrd.insert(-1, -1);
        return;
    }
    TreeNode rootNode;
    rootNode.kind = root;                     // 根也是容器（文件夹 / 压缩包 / 多页文件）
    rootNode.label = rootLabel;
    rootNode.parent = -1;
    rootNode.level = 0;
    rootNode.ord = -1;
    const int ri = addTreeNode(rootNode);
    //   ★ 根也是容器（它的区间 = 整条序列），但它**不进容器区间表**（`containers()`）：
    //     它不是"文件夹"，而且区间覆盖整条序列 ⇒ 永远不可能是"上/下一个文件夹"的候选
    //     （用户 2026-10-19）。

    // ★ 根是**第一行所属节点的祖先**（其实是它自己）⇒ 它就是那条打开链的链首。
    //   根到扫描结束才扫完（L40：根也是目录 / 压缩包，按同一条判定条件走）。
    m_openStack.clear();
    m_openStack << ri;
    m_nodeIndexByOrd.insert(-1, 0);
}

void Sequence::applyRows(const QVector<Row> &rows)
{
    // ★ 行是**前序 DFS**（父行在子行之前）：每来一行，先按它所属的节点**扫完**那些
    //   与它不再共享祖先链的打开节点（L40），再建节点 / 并条目（记录在这里沿链维护，L41）。
    for (const Row &r : rows) {
        m_rows << r;
        const int rowIndex = m_rows.size() - 1;
        m_rowIndexByOrd.insert(r.ord, rowIndex);

        if (r.kind != Kind::Image) {
            const int p = m_nodeIndexByOrd.value(r.parentOrd, m_nodeIndexByOrd.value(-1, -1));
            syncOpenChain(p);                     // ★ 先扫完：新节点不在它们子树里了
            TreeNode n;
            n.kind = (r.kind == Kind::Folder) ? Kind::Folder : Kind::Archive;
            n.label = r.label;
            n.ord = r.ord;
            n.parent = p;
            n.level = (p >= 0 && p < m_tree.size()) ? m_tree.at(p).level + 1 : 0;
            const int ni = addTreeNode(n);
            m_openStack << ni;                    // 新节点那一支现在"开着"
            m_nodeIndexByOrd.insert(r.ord, ni);
            continue;
        }

        Item it;
        it.kind = Kind::Image;
        it.label = r.label;
        it.displayName = r.label;
        it.ord = r.ord;
        it.loc = locateOne(m_root, m_rootIsArchive, m_rowIndexByOrd, m_rows, rowIndex,
                           m_scanOptions.innerArchiveMaxMb);
        it.size = QSize(r.width, r.height);
        it.missing = m_markMissingBySize && !it.size.isValid();
        const int pn = m_nodeIndexByOrd.value(r.parentOrd, m_nodeIndexByOrd.value(-1, -1));
        it.parentNode = pn;
        if (pn >= 0 && pn < m_tree.size()) {
            it.folderLabel = m_tree.at(pn).label;
            it.folderDepth = m_tree.at(pn).level;
        }
        syncOpenChain(pn);                        // ★ 先扫完（图不改打开链的深度）
        appendItem(it);                           // ★ 记录在这里沿祖先链 +1
    }
}

// ══ 恢复（行 → 序列；**唯一的"从磁盘回到内存"入口**）══════════════════════
//   与扫描路**共用同一段并入代码**（`applyRows` / `finishAppend`）——区别只是行从哪来：
//   扫描是生产者产批，恢复是"流"喂批 ⇒ 两条路不会分叉。
namespace {
// 手里已经有一份行时的便利流（恢复的"内存"那一档）
class VectorRowSource : public Sequence::RowSource
{
public:
    explicit VectorRowSource(const QVector<Sequence::Row> &rows) : m_rows(rows) {}
    int next(QVector<Sequence::Row> &out, int maxRows) override
    {
        const int left = m_rows.size() - m_pos;
        const int n = qBound(0, maxRows, left);
        for (int i = 0; i < n; ++i) out << m_rows.at(m_pos + i);
        m_pos += n;
        return n;
    }
private:
    const QVector<Sequence::Row> &m_rows;
    int m_pos = 0;
};
} // namespace

void Sequence::restore(const QString &rootPath, RowSource &src, bool rootIsArchive,
                       const QString &rootLabel, const ScanOptions &opt)
{
    clear();
    m_scanOptions = opt;                      // ★ 扫描参数**调用方传**（序列不读设置）
    m_root = QFileInfo(rootPath).absoluteFilePath();
    m_recursive = true;
    m_archivePages = rootIsArchive;
    m_rootIsArchive = rootIsArchive;
    m_markMissingBySize = false;              // 库行路径：尺寸未知 ≠ 缺失（库里没有 missing）
    initRoot(true, rootIsArchive ? Kind::Archive : Kind::Folder,
             rootLabel.isEmpty() ? QFileInfo(m_root).fileName() : rootLabel);
    m_rootInit = true;

    //   边收边并：一批一批地并入 ⇒ 峰值内存 = "一个批"，不再要求整份行
    QVector<Row> batch;
    batch.reserve(kRestoreBatchRows);
    for (;;) {
        batch.clear();
        const int n = src.next(batch, kRestoreBatchRows);
        if (n <= 0) break;
        applyRows(batch);
    }
    finishAppend();
}

void Sequence::restore(const QString &rootPath, const QVector<Row> &rows, bool rootIsArchive,
                       const QString &rootLabel, const ScanOptions &opt)
{
    VectorRowSource src(rows);
    restore(rootPath, src, rootIsArchive, rootLabel, opt);
}

// ══ 会话内扫描 ════════════════════════════════════════════════════════

void Sequence::startScan(const QString &path, int maxDepth, bool hidden, const ScanOptions &opt)
{
    joinWorker();                             // ★ 打开新的来源 ⇒ 旧的扫描直接取消（已放弃 ⇒ 跳过）

    resetRows();
    m_scanOptions = opt;                      // ★ 扫描参数**调用方传**（序列不读设置）
    m_markMissingBySize = true;               // 扫描路径：尺寸读不到 ⇒ 标 missing
    m_root = QFileInfo(path).absoluteFilePath();
    m_recursive = true;
    m_archivePages = false;
    m_rootIsArchive = false;
    m_state.store(State::NotReady, std::memory_order_relaxed);
    // ★ 换来源：**把队列与"已收尾"一起重置**（旧成员 m_wHead / m_wTail 的重置由它取代）
    {
        QMutexLocker lk(&m_qMutex);
        m_q.clear();
    }
    m_finished = false;
    m_cancel.store(false, std::memory_order_relaxed);
    m_abandoned.store(false, std::memory_order_relaxed);
    m_unsupported.clear();
    setNotice(QString());

    // ★★ **扫描线程启动时就算一次进度**（"创建线程那一刻"）—— 否则看门狗会把
    //    "刚建好还没跑"看成"卡了 30 s"。
    bumpProgress();
    m_worker = std::thread([this, path, maxDepth, hidden] {
        // ★★ **生产者线程计数**（T2c-2b-2）：入口 +1、出口 −1 —— RAII 守卫保证
        //   **所有退出路径都减**（正常结束 / 被取消 / 被 abandon 之后才退出 / 抛异常展开）。
        std::atomic<int> *counter = &m_producerThreads;
        counter->fetch_add(1, std::memory_order_relaxed);
        struct CountGuard {
            std::atomic<int> *p;
            ~CountGuard() { p->fetch_sub(1, std::memory_order_relaxed); }
        } guard{ counter };
        scanThreadMain(path, maxDepth, hidden);
    });
}

// **扫描线程**：支持策略（用户 2026-10-13）：**能随机读的才支持**。
QString Sequence::checkSupported(const QString &path, const std::function<bool()> &progress)
{
    m_unsupported.clear();
    // ★ v7：**根不可用** ⇒ 序列**空跑完**（Scaned + 0 条）—— 失败**不写特例状态**，
    //   原因只走瞬时提示 + 日志（上层 `scanFailed` → `UiModel::notify`）。
    {
        const QFileInfo root(path);
        if (!root.exists() || !root.isReadable()) {
            m_unsupported = QStringLiteral("根不可用：%1").arg(path);
            return m_unsupported;
        }
    }
    const QString suf = QFileInfo(path).suffix().toLower();
    if (suf == QLatin1String("gz") || suf == QLatin1String("bz2")
        || suf == QLatin1String("xz") || suf == QLatin1String("zst")) {
        m_unsupported = QStringLiteral("流式压缩（.%1）不能随机读，请先解压成文件夹").arg(suf);
    } else if (suf == QLatin1String("7z") || suf == QLatin1String("cb7")) {
        QString why;
        if (!sevenZipIsRandomAccessible(path, &why))
            m_unsupported = QStringLiteral("7z 不支持随机读（%1），请先解压成文件夹").arg(why);
    } else if (suf == QLatin1String("rar") || suf == QLatin1String("cbr")) {
        ArchiveSource probe;
        // ★ 枚举期间也要汇报进度（这两个探针在大包上很贵）
        if (probe.open(path, QString(), ArchiveSource::Entries::ImagesOnly, progress)
            && probe.isSolid())
            m_unsupported = QStringLiteral("RAR 是 solid，不能随机读，请先解压成文件夹");
    }
    // ★ v5：容器**打不开**也算不支持（"该容器不出现 + 瞬时提示"，Phase 3 接 notification）。
    //   在这里（扫描线程最开头）判定 ⇒ 不跨线程改这个字符串。
    if (m_unsupported.isEmpty() && ArchiveSource::isArchive(path)) {
        ArchiveSource probe;
        if (!probe.open(path, QString(), ArchiveSource::Entries::All, progress))
            m_unsupported = QStringLiteral("压缩包打不开：%1").arg(QFileInfo(path).fileName());
    }
    return m_unsupported;
}

void Sequence::scanThreadMain(QString path, int maxDepth, bool hidden)
{
    // ★★ **生产者进度钩子**（唯一实现）：交给 `ArchiveSource::open*` —— 容器**枚举过程中**
    //   每 `kProgressEvery` 条调一次。做两件事：
    //     ① 写进度时间戳（"枚举期间也在推进"）；
    //     ② 给调用方一个"停"的机会：返回 **false ⇒ 立刻停止枚举**（取消 / 已放弃）。
    const auto progressFn = [this]() -> bool {
        bumpProgress();
        return !stopRequested();
    };

    if (!checkSupported(path, progressFn).isEmpty()) {
        // ★ 是被**取消**（不是真"不支持"）⇒ 不记"不支持"日志、原因也清掉
        if (stopRequested()) {
            m_unsupported.clear();
            writeTerminal(State::ScanStoped);    // ★ 写终态 + 唤醒消费者（唯一入口）
            return;
        }
        qInfo().noquote() << QStringLiteral("[seq] 不支持：%1 —— %2").arg(path, m_unsupported);
        writeTerminal(State::Scaned);
        return;
    }
    // ★ 阻塞调用（7z 解析 / 压缩包枚举）返回后：立刻查停止 —— 看到就走，不碰任何状态
    if (stopRequested()) {
        writeTerminal(State::ScanStoped);
        return;
    }

    const QFileInfo rootInfo(path);
    const bool rootIsArchive   = !rootInfo.isDir() && ArchiveSource::isArchive(path);
    const bool rootIsMultiPage = !rootInfo.isDir() && !rootIsArchive
                                 && pageCountOf(path) > 1;
    m_rootIsArchive = rootIsArchive || rootIsMultiPage;    // "来源根本身就是容器"
    // ★ `pageCountOf` 是**阻塞调用**（对 FIFO 之类的路径会永远不返回）⇒ 返回后第一件事查停止
    if (stopRequested()) {
        writeTerminal(State::ScanStoped);
        return;
    }


    QVector<Row>      batch;
    QVector<Row>      localRows;                 // 定位用（与消费线程**同一份派生**）
    QHash<int, int>   localIdx;
    int               ordSeq = 0;

    // ★ **唯一的入队点**（生产者）：锁内 O(1)、**永不阻塞**（队列无界 ⇒ 不存在"满了"）；
    //   ★★ 钩子在**锁外**调（§9.2：唤醒消费者；钩子不许阻塞生产者，也不许拖住 drain）。
    auto post = [this](Batch &&b) {
        {
            QMutexLocker lk(&m_qMutex);
            m_q.push_back(std::move(b));
        }
        publish();                           // ★ 每次放批之后
    };

    auto flush = [&](bool force) {
        if (batch.isEmpty()) return;
        if (force || batch.size() >= 16) {
            bumpProgress();                      // ★ 每批放批**之前**写进度
            Batch b;
            b.rows = std::move(batch);
            post(std::move(b));
        }
    };

    // ★ 行的唯一产出点：领 ord → 入本地表 → 图像行**当场读尺寸**（扫描线程，不碰 UI）
    auto emitRow = [&](Row r) -> int {
        r.ord = ordSeq++;
        localRows << r;
        localIdx.insert(r.ord, localRows.size() - 1);
        if (r.kind == Kind::Image) {
            const Location loc = locateOne(m_root, rootIsArchive, localIdx, localRows,
                                           localRows.size() - 1,
                                           m_scanOptions.innerArchiveMaxMb);
            Item probe;
            probe.loc = loc;
            const QSize sz = imageSize(probe);
            bumpProgress();                      // ★ 逐条读图头（`imageSize()`）返回后写进度
            if (stopRequested()) return r.ord;   // ★ 看到停止立刻退出，不再碰任何状态
            Row &back = localRows.last();
            if (sz.isValid()) { back.width = sz.width(); back.height = sz.height(); }
            else              { back.width = 0; back.height = 0; }
            batch << back;
        } else {
            batch << r;
        }
        flush(false);
        return r.ord;
    };
    auto emitNode = [&](Kind kind, const QString &label, int parentOrd) -> int {
        Row r;
        r.kind = kind;
        r.label = label;
        r.parentOrd = parentOrd;
        r.pageIndex = -1;
        return emitRow(r);
    };
    auto emitImage = [&](const QString &label, int parentOrd, int pageIndex,
                         const QString &diskPath, const QStringList &members) -> int {
        Row r;
        r.kind = Kind::Image;
        r.label = label;
        r.parentOrd = parentOrd;
        r.pageIndex = pageIndex;
        if (members.isEmpty()) {                 // 盘上的文件：顺手记事实（库探针用）
            const QFileInfo fi(diskPath);
            if (fi.exists()) {
                r.size = fi.size();
                r.mtime = fi.lastModified().toSecsSinceEpoch();
            }
        }
        return emitRow(r);
    };

    // ── 压缩包 / 多页文件 = 容器：当场展开（嵌套：**取出来的字节再当容器打开**）──
    //   `cMembers` = 到这个容器**自己**的成员链（相对最外层 `diskPath`）
    std::function<void(ArchiveSource &, const QStringList &, int, int)> expandArchive;
    expandArchive = [&](ArchiveSource &a, const QStringList &cMembers, int selfOrd, int level) {
        if (stopRequested()) return;

        // ★★ 与文件系统**同一套**遍历规则（先根遍历）：本层条目 = **一条名次序**
        //   （图片 / 嵌套容器 / 子目录同序），遇到子目录或嵌套容器**当场下钻**。
        struct ArcEntry {
            QString name;      // 容器内完整路径（成员的 key）
            QString leaf;      // 单段名（排序键 / 显示名）
            int     kind = 0;  // 0 = 图，1 = 嵌套容器，2 = 子目录
        };
        QHash<QString, QVector<ArcEntry>> dirs;
        QHash<QString, int>               nameIndex;
        QSet<QString>                     knownSubs;       // 子目录只登记一次
        for (int i = 0; i < a.count(); ++i) {
            const QString name = a.nameAt(i);
            nameIndex.insert(name, i);
            const int slash = name.lastIndexOf(QLatin1Char('/'));
            const QString dir  = (slash < 0) ? QString() : name.left(slash);
            const QString leaf = (slash < 0) ? name : name.mid(slash + 1);
            ArcEntry e;
            e.name = name;
            e.leaf = leaf;
            if (isImageSuffix(QFileInfo(leaf).suffix())) {
                e.kind = 0;
                dirs[dir] << e;
            } else if (ArchiveSource::isArchive(leaf)) {
                e.kind = 1;
                dirs[dir] << e;
            }
            QString d = dir;
            while (!d.isEmpty()) {
                const QString up = parentRelOf(d);
                if (!knownSubs.contains(d)) {
                    knownSubs.insert(d);
                    ArcEntry s;
                    s.name = d;
                    const int sl = d.lastIndexOf(QLatin1Char('/'));
                    s.leaf = (sl < 0) ? d : d.mid(sl + 1);
                    s.kind = 2;
                    dirs[up] << s;
                }
                d = up;
            }
        }

        std::function<void(const QString &, int)> walk;
        walk = [&](const QString &dir, int parentOrd) {
            if (stopRequested()) return;
            QVector<ArcEntry> &list = dirs[dir];
            std::sort(list.begin(), list.end(),                  // ★ 一条名次序
                      [](const ArcEntry &x, const ArcEntry &y) {
                          return naturalCompare(nameSortKey(x.leaf, x.kind == 2),
                                                nameSortKey(y.leaf, y.kind == 2)) < 0;
                      });
            for (const ArcEntry &e : list) {
                if (stopRequested()) return;
                if (e.kind == 2) {                               // 子目录：当场下钻
                    const int nodeOrd = emitNode(Kind::Folder, e.leaf, parentOrd);
                    walk(e.name, nodeOrd);
                    continue;
                }
                if (e.kind == 0) {                               // 图
                    QStringList members = cMembers;
                    members << e.name;                           // 容器内完整路径
                    emitImage(e.leaf, parentOrd, -1, m_root, members);
                    continue;
                }
                // 嵌套容器
                if (level + 1 > maxDepth) continue;              // 深度上限：不展开
                const int idx = nameIndex.value(e.name, -1);
                if (idx < 0) continue;
                // ★ v7：**内层压缩包上限** —— 判定在**解压前**（从归档条目元数据取成员大小）。
                //   超限 ⇒ 这个内层包**不展开**（不出现 / 读不了）+ 瞬时提示
                //   （`UiModel::notify`，**不是失败**）。
                const qint64 limit = innerArchiveLimitBytes();
                if (a.sizeAt(idx) > limit) {
                    const QString why =
                        QStringLiteral("内层压缩包 %1 超过上限（%2 MB > %3 MB），已跳过不展开")
                            .arg(e.leaf)
                            .arg(a.sizeAt(idx) / 1048576.0, 0, 'f', 1)
                            .arg(limit / 1048576);
                    qInfo().noquote() << QStringLiteral("[seq] %1").arg(why);
                    setNotice(why);
                    continue;
                }
                // ★ **取成员字节**是最贵的阻塞调用（可能要解压）⇒ 返回后第一件事查停止
                const QByteArray bytes = a.dataAt(idx);
                bumpProgress();                              // ★ 取字节返回后写进度
                if (stopRequested()) return;
                if (bytes.isEmpty()) continue;
                ArchiveSource inner;
                // ★ 内层容器的**枚举**同样要汇报进度（它也可能有几万条）
                if (!inner.openBytes(bytes, e.leaf, QString(),
                                     ArchiveSource::Entries::All, progressFn)) {
                    if (stopRequested()) return;             // 取消 ⇒ 不是"打不开"，不记日志
                    qWarning().noquote()
                        << QStringLiteral("[seq] 嵌套容器打不开（该容器不出现）：%1").arg(e.name);
                    continue;
                }
                QStringList members = cMembers;
                members << e.name;
                const int nodeOrd = emitNode(Kind::Archive, e.leaf, parentOrd);
                expandArchive(inner, members, nodeOrd, level + 1);
            }
        };
        walk(QString(), selfOrd);
    };

    // 根节点描述（第一片就送；消费线程据此合成 0 号节点）
    //   · 目录            ⇒ Dir 根
    //   · 压缩包 / 多页文件 ⇒ Archive 根（**文件本身就是那个容器**；页/成员挂在根上）
    //   · 单张图          ⇒ 没有树（`m_root` 改成所在目录 ⇒ 盘上路径仍可派生）
    const bool rootIsDir = rootInfo.isDir();
    const bool rootHasTree = rootIsDir || rootIsArchive || rootIsMultiPage;
    const Kind rootKind    = rootIsDir ? Kind::Folder : Kind::Archive;
    if (!rootHasTree) m_root = rootInfo.absolutePath();
    bumpProgress();                              // ★ 每批放批**之前**写进度
    {                                            // 第一片：**空行 + 根描述**（今天那次 pushBatch 就是它）
        Batch b;
        b.rootHasTree = rootHasTree;
        b.rootKind    = rootKind;
        b.rootLabel   = rootHasTree ? rootInfo.fileName() : QString();
        b.rootSet     = true;
        post(std::move(b));
    }
    if (stopRequested()) {                       // 首片之后也一样：看到停就走
        writeTerminal(State::ScanStoped);
        return;
    }

    if (rootInfo.isDir()) {
        // ★★ 遍历 = **先根遍历（preorder DFS）**：读一层 ⇒ 按**名称序**逐个处理条目
        //   （图片 / 压缩包 / 子目录**同序**），遇到子目录**当场下钻**、回来接着下一个。
        //   ⇒ 容器**自己**的图在整条序列里**不必连续**（会被它的子容器切开）。
        DirWalker w(maxDepth, hidden);
        //   `label` = 这个目录**自己的**节点行名字（**根没有行** ⇒ 空串表示不发节点）
        std::function<void(const QString &, int, int, const QString &)> walk;
        walk = [&](const QString &dir, int level, int parentOrd, const QString &label) {
            if (stopRequested()) return;
            DirBatch b;
            if (!w.readDir(dir, level, &b)) return;            // 读不到 / 防环 / 超深 ⇒ 连节点都不建
            int nodeOrd = parentOrd;
            if (!label.isNull())                               // ★ **读成功才建节点**
                nodeOrd = emitNode(Kind::Folder, label, parentOrd);
            for (const DirEntry &e : b.entries) {
                if (stopRequested()) return;
                if (e.isDir) {                                 // 子目录：当场下钻
                    walk(e.path, level + 1, nodeOrd, e.name);
                    continue;
                }
                if (e.isArchive) {                             // 压缩包：在场展开
                    ArchiveSource a;
                    // ★ 必须列出**全部**条目（要看见"压缩包里的压缩包"）；
                    //   打不开 ⇒ 该容器不出现（+ 日志）
                    if (!a.open(e.path, QString(), ArchiveSource::Entries::All, progressFn)) {
                        if (stopRequested()) return;           // 取消 ⇒ 不是"打不开"，不记日志
                        qWarning().noquote()
                            << QStringLiteral("[seq] 压缩包打不开（该容器不出现）：%1").arg(e.path);
                        continue;
                    }
                    const int arcOrd = emitNode(Kind::Archive, e.name, nodeOrd);
                    expandArchive(a, QStringList(), arcOrd, 0);
                    continue;
                }
                const int pages = pageCountOf(e.path);         // 图片 / 多页文件
                if (stopRequested()) return;                   // ★ 阻塞调用返回后第一件事
                if (pages > 1) {                               // 多页文件 = 容器（页 = 叶子）
                    const int mp = emitNode(Kind::Archive, e.name, nodeOrd);
                    for (int p = 0; p < pages; ++p) {
                        if (stopRequested()) return;
                        emitImage(QString(), mp, p, e.path, QStringList());
                    }
                    continue;
                }
                emitImage(e.name, nodeOrd, -1, e.path, QStringList());
            }
        };
        walk(path, 0, -1, QString());                // 根**没有节点行**（根描述那一片负责）
    } else if (rootIsArchive) {
        ArchiveSource a;
        if (!a.open(path, QString(), ArchiveSource::Entries::All, progressFn)) {
            if (stopRequested()) {                   // ★ 是取消（不是"打不开"）⇒ 不记日志、不写 Scaned
                writeTerminal(State::ScanStoped);
                return;
            }
            qWarning().noquote() << QStringLiteral("[seq] 压缩包打不开（来源根）：%1").arg(path);
            writeTerminal(State::Scaned);
            return;
        }
        expandArchive(a, QStringList(), -1, 0);
    } else if (rootIsMultiPage) {
        // 多页文件 = **根容器自己**（没有额外的节点行）：页 = 叶子，挂在根上
        const int pages = pageCountOf(path);
        if (stopRequested()) {                       // ★ 阻塞调用返回后第一件事
            writeTerminal(State::ScanStoped);
            return;
        }
        for (int p = 0; p < pages; ++p) {
            if (stopRequested()) break;
            emitImage(QString(), -1, p, path, QStringList());
        }
    } else {
        // 单张图 ⇒ 没有树，整条序列当一层
        emitImage(rootInfo.fileName(), -1, -1, rootInfo.absoluteFilePath(), QStringList());
    }

    flush(true);
    // ★ 结束 = **写终态**（release ⇒ `unsupportedReason()` 等结束字段对消费者可见）+
    //   **唤醒消费者**（§9.2 的发布钩子）；不再有"结束专用格"，也不再走队列。
    writeTerminal(stopRequested() ? State::ScanStoped : State::Scaned);
}

// ══ 静态构建（库扫描器 / 旧路径）═══════════════════════════════════════

void Sequence::clear()
{
    joinWorker();                            // ★ 已放弃 ⇒ 跳过（见 joinWorker）
    resetRows();
    m_root.clear();
    m_recursive = false;
    m_archivePages = false;
    m_rootIsArchive = false;
    m_unsupported.clear();
    setNotice(QString());
    m_state.store(State::NotReady, std::memory_order_relaxed);
    // ★ 换来源：**把队列与"已收尾"一起重置**（旧成员 m_wHead / m_wTail 的重置由它取代）
    {
        QMutexLocker lk(&m_qMutex);
        m_q.clear();
    }
    m_finished = false;
}

void Sequence::sortNaturally()
{
    std::sort(m_items.begin(), m_items.end(), [](const Item &a, const Item &b) {
        return naturalCompare(a.displayName, b.displayName) < 0;
    });
    QVector<Item> keep = m_items;
    m_items.clear();
    m_ownNext.clear();               // ★ 直接条目的链与 `m_items` 等长 ⇒ 一起清（重并时重建）
    resetGrouping();
    resetNodeRecords();          // ★ 记录是"并入顺序的产物" ⇒ 重并之前先清零（否则重复计数）
    for (const Item &it : keep) appendItem(it);
    finishAppend();
}

void Sequence::buildFromFolder(const QString &dir, bool recursive)
{
    clear();
    m_root = QFileInfo(dir).absoluteFilePath();
    m_recursive = recursive;
    m_rootIsArchive = false;
    m_markMissingBySize = false;
    initRoot(true, Kind::Folder, QFileInfo(m_root).fileName());
    m_rootInit = true;

    QMap<QString, QVector<Item>> groups;
    auto collect = [&](const QString &folder) {
        QDir d(folder);
        const QFileInfoList entries = d.entryInfoList(QDir::Files, QDir::NoSort);
        for (const QFileInfo &fi : entries) {
            Item it;
            it.displayName = fi.fileName();
            it.label = fi.fileName();
            it.loc.diskPath = fi.absoluteFilePath();
            it.folderLabel = (folder == m_root) ? QString() : d.dirName();
            it.folderDepth = 0;
            if (isImageSuffix(fi.suffix())) {
                it.kind = Kind::Image;
            } else if (ArchiveSource::isArchive(fi.absoluteFilePath())) {
                it.kind = Kind::Archive;
            } else {
                continue;
            }
            groups[folder] << it;
        }
    };
    collect(m_root);
    if (recursive) {
        QDirIterator it(m_root, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) collect(it.next());
    }

    QStringList folders = groups.keys();
    std::sort(folders.begin(), folders.end(), [this](const QString &a, const QString &b) {
        if (a == m_root) return true;
        if (b == m_root) return false;
        return naturalCompare(a, b) < 0;
    });

    QVector<Item> out;
    for (const QString &folder : folders) {
        QVector<Item> &v = groups[folder];
        std::sort(v.begin(), v.end(), [](const Item &a, const Item &b) {
            return naturalCompare(a.displayName, b.displayName) < 0;
        });
        for (Item &x : v) {
            if (recursive && folder != m_root)
                x.folderDepth = QDir(m_root).relativeFilePath(folder).count(QLatin1Char('/'));
            out << x;
        }
    }
    for (int i = 0; i < out.size(); ++i) { out[i].ord = i; appendItem(out.at(i)); }
    finishAppend();
}

void Sequence::buildFromSingleFile(const QString &filePath)
{
    clear();
    const QFileInfo fi(filePath);
    m_root = fi.absolutePath();
    m_archivePages = false;
    m_markMissingBySize = false;
    Item it;
    it.displayName = fi.fileName();
    it.label = fi.fileName();
    it.loc.diskPath = fi.absoluteFilePath();
    it.kind = Kind::Image;
    it.ord = 0;
    appendItem(it);
    finishAppend();
}

void Sequence::buildFromMultiPageFile(const QString &filePath, int pageCount)
{
    clear();
    const QFileInfo fi(filePath);
    m_root = fi.absolutePath();
    m_archivePages = false;
    m_markMissingBySize = false;
    for (int i = 0; i < pageCount; ++i) {
        Item it;
        it.displayName = QStringLiteral("%1 [%2/%3]").arg(fi.fileName()).arg(i + 1).arg(pageCount);
        it.loc.diskPath = fi.absoluteFilePath();
        it.loc.page = i;
        it.kind = Kind::Image;
        it.ord = i;
        appendItem(it);
    }
    finishAppend();
}

void Sequence::buildFromNames(const QString &archivePath, const QStringList &names)
{
    clear();
    m_root = QFileInfo(archivePath).absoluteFilePath();
    m_archivePages = true;
    m_rootIsArchive = true;
    m_markMissingBySize = false;
    initRoot(true, Kind::Archive, QFileInfo(m_root).fileName());
    m_rootInit = true;

    QStringList sorted = names;
    sortNatural(sorted);
    int ord = 0;
    for (const QString &n : sorted) {
        Item it;
        it.displayName = QFileInfo(n).fileName();
        it.label = QFileInfo(n).fileName();
        it.loc.diskPath = m_root;
        it.loc.members = QStringList{ n };
        it.kind = Kind::Image;
        it.ord = ord++;
        it.parentNode = 0;
        appendItem(it);
    }
    finishAppend();
}

void Sequence::buildFromEntries(const QString &rootPath, const QVector<Item> &entries)
{
    clear();
    m_root = QFileInfo(rootPath).absoluteFilePath();
    m_markMissingBySize = false;
    for (const Item &it : entries) appendItem(it);
    finishAppend();
}

// 只标记，不移除——移除会让索引位移（AGENTS.md 红线）
int Sequence::markMissing()
{
    if (m_archivePages) return 0;
    int n = 0;
    for (Item &it : m_items) {
        if (!it.loc.members.isEmpty()) continue;      // 包内条目不做文件系统校验
        const bool exists = QFileInfo::exists(it.loc.diskPath);
        if (!exists && !it.missing) { it.missing = true; ++n; }
        else if (exists && it.missing) { it.missing = false; }
    }
    return n;
}
