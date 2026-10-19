#pragma once

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QPair>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <thread>

// ★★ **单调时钟（ms）—— 唯一实现**（算法单一事实源）：
//   `steady_clock` 不是墙钟 ⇒ 系统时间跳变不影响判活。语义 = 原旧扫描路径里那份。
//   两个用途共用同一个时间原点，所以必须只有这一处实现：
//     ① **生产者**（`Sequence` 的内部扫描线程）写"进度时间戳"（`Sequence::bumpProgress`）；
//     ② **消费侧**看门狗（`LibraryCore::pump` / `reapSlot`）读它做差，判"超过 30 s 没有进度更新"。
inline qint64 monotonicNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ══ 序列（设计：**会话内的"这一批图"**）═════════════════════════════════
//
//   序列 = 一个**动态抽象**：持有"这一批图"的信息，按**方法调用**回答
//   "从游标开始的那一组是什么 / 新游标在哪"。**调度器不计算** —— 分组、前进量、
//   缺失处理都是序列的知识（2026-10-12 用户定案）。
//
// ## 两个来源（**同一份实现**，差别只有"写不写入磁盘"）
//   · **文件系统**：打开文件夹 / 压缩包 / 单张图 / **未扫完的图集**（规则完全一致）
//     ⇒ 会话内存在，**应用一关就消失**，不保存；
//   · **图集**（已扫完）：同一份东西，只是**写入磁盘**（DB 里那份 = 已写入磁盘的序列）。
//
// ## ★★ v5 定案（2026-10-13 用户定案）：**条目身份 = 沿 parent_ord 上溯的 label 序列**
//   · 表的行 = `Row`：`ord / parent_ord / label / kind(folder|archive|image) / page_index / …`
//     —— **没有** key / rel_path 这类拼出来的字符串；写入磁盘**不拼任何字符串**；
//   · `ord` 在**一个集合内唯一**（节点与叶子共用同一个遍历序，DFS 前序）；
//   · `parent_ord` = 父行的 ord（根级 = -1）；身份 = 从根往下走的一串 label（+ 页号）；
//   · 页行：`label` 为**空串**，`page_index` = 页号；容器行 / 普通图：`page_index` = -1；
//   · 恢复（`restore`）= `ORDER BY ord` 读行 → 按 ord 建树 + append 叶子（**边收边并**）
//     （与**直接扫描**同一套：扫描线程产出的也是 `Row`）。
//
// ## 读图 = **沿链解析**（v5 的真活）
//   · `Item` 携带：**磁盘路径**（链上**最外层容器**的绝对路径；无容器 = 图片本身）
//     + **容器成员链**（`QStringList`，长度 = 容器层数）+ `pageIndex`；
//   · 解码：从磁盘路径开 `ArchiveSource` → 逐个取成员（取出来的字节再当容器打开，
//     逐层向下）→ 最内层字节交 `QImageReader`（`pageIndex > 0` 时 `jumpToImage`）；
//   · ⇒ **目录层级只是磁盘路径的一部分**（不产生额外解析成本）；
//   · ⇒ 嵌套压缩包**不再解到临时文件**，库来源也能读（扫描期也没有临时文件了）。
//
// ## 状态（信息完备度；**单调**）
//   `notReady` → `imageFound` → `scaned` / `scanStoped`
//   · `imageFound` 的**进入条件** = 第一批"**条目 + 分组都准确**" ⇒ 这就是**允许查看**的门槛；
//   · `scaned` = 计数完备 + 分组完备 ⇒ 无边界；
//   · `scanStoped` = **用户终止扫描**（处理规则与 `scaned` **完全相同**，只是状态不同）。
//
// ## 从头锚定（★ 这是"简单双幅被取消"的原因）
//   **所有来源都走一趟**（压缩包哪怕贵）⇒ 尺寸随条目一起到 ⇒ 分组**边收边从头锚定**：
//     走一遍：`i` 窄 且 `i+1` 窄 ⇒ `(i, i+1)` 一组，下一个起点 `i+2`；否则 `i` 单独，下一个起点 `i+1`。
//   · 这是对 index 空间的一个**划分** ⇒ `advance` / `anchorOf` **零状态、完全对称**；
//   · 只有**最后一个已知条目**要等下一个才能定（**1 项前瞻**）。
//
// ## 游标 = "当前屏的起点"
//   · `next` ⇒ 下一组的起点；`prev` ⇒ 上一组的起点（`anchorOf(cursor-1)`）—— **完全对称**；
//   · `jump(i)` ⇒ **`i` 落在的那一组**的起点。
//
// ## 遍历（与图集库扫描**共用一份** `DirWalker`；压缩包等同于文件夹）
//   · 深度优先 + 同级**自然排序**（`naturalCompare`）；目录里**文件在前、子目录在后**；
//   · **压缩包 / 多页文件 = 容器**，在它的自然序位置**当场展开**；
//   · 嵌套容器：**取出来的字节再当容器打开**（`ArchiveSource::openBytes`）；
//   · 序列要的是**图** ⇒ 容器本身**不进**条目表（容器只进**文件树索引**）。
//
// ## 线程（**一把锁 + 无界批队列**；2026-10-14 定案，§9.4）
//   · **扫描全程在子线程**；扫描线程只**产出批次 `Batch`**（行 + 根描述），
//     锁内 `push_back` 进**无界**队列 —— O(1)、**永不阻塞**（没有"满了"这回事）；
//   · **消费线程**（= VM 的驱动线程）`drain()` 锁内 O(1) `swap` 走整队，出锁再处理，
//     在这里建树 / 定组 / 增长数量；⇒ 序列的数据**只被消费线程碰**；
//   · 状态是原子：**生产者写终态（release）**，消费者只在 `NotReady` 时 `CAS(→ImageFound)`
//     ⇒ **终态不可被覆盖**；"扫描结束"不再走队列（**没有结束专用格**）。
//   · ★★ **"卡住"的发现与处置都在消费侧**（`LibraryCore::pump` 的卡住分支 /
//     `reapSlot()` 的有界收尾；主线程不参与）：生产者写进度时间戳（`bumpProgress`），
//     消费侧判"30 s 没有进度更新" ⇒ `requestStop()`（有效时生产者自己退）+
//     `abandon()`（**detach，不 join**，对象不再释放）。
class Sequence
{
public:
    // ★★ 行 / 容器的种类 = **一份枚举**（用户 2026-10-18）：库里 `item.kind` / `collection.kind`
    //   存的就是它的**值**；多页文件也算 `Archive`。取值显式写死（不许重排）。
    enum class Kind { Image = 0, Archive = 1, Folder = 2 };

    // 信息完备度（单调）
    enum class State { NotReady, ImageFound, Scaned, ScanStoped };

    //   ★ **序列不带"版面"这个概念**（用户 2026-10-18）：单幅 / 双幅是**设置**
    //     （`app::Layout`，落在 `appsettings.h`），序列这边只有**配对表**这份**数据**
    //     （谁跟谁配成一对，由尺寸**从头锚定**算出来）。

    // ★★ 设计 14 §9.3：**序列不读设置** —— 扫描要的参数由调用方在这一组里传入。
    //   默认值 = 旧设置项的默认值（`recurseDepth=5` / `scanHidden=false` 是 `startScan`
    //   既有形参的默认；`innerArchiveMaxMb` 这里默认 `512`，与 `AppSettings` 一致）。
    //   §7 以后会往这里加字段（enabled / total / cachePath），所以名字取通用的 `ScanOptions`。
    static constexpr qint64 kDefaultInnerArchiveMaxMb = 512;
    // 恢复时"一批吃几行"（约 2048 × 138 B ≈ 280 KB；只影响流式批大小，不改语义）
    static constexpr int kRestoreBatchRows = 2048;
    struct ScanOptions {
        // ⚠️ 用**显式构造函数**给默认值（不用默认成员初始值）：`ScanOptions()` 是外层类
        //    成员函数的默认实参，GCC 不允许在那里依赖嵌套类的默认成员初始值。
        qint64 innerArchiveMaxMb;                               // 内层压缩包上限（MB）
        ScanOptions(qint64 mb = kDefaultInnerArchiveMaxMb) : innerArchiveMaxMb(mb) {}
    };

    // ★★ v5：**已写入磁盘的那一行**（库表 `item` 与扫描输出**同一个形态**）
    //   · `ord`       = 遍历序号（一个集合内唯一；节点与叶子共用）
    //   · `parentOrd` = 父行的 ord（根级 = -1）
    //   · `label`     = 它在**容器里的名字**（页行 = 空串）
    //   · `kind`      = folder | archive | image
    //   · `pageIndex` = 页行 = 页号；容器行 / 普通图 = -1
    //   · `firstIndex`/`lastIndex` = ★★ **边界索引**（只对容器行有意义；图行 = -1）
    struct Row {
        int     ord = 0;
        int     parentOrd = -1;
        Kind    kind = Kind::Image;   // ★ 一份枚举（库里存它的值）
        QString label;
        int     pageIndex = -1;
        qint64  size = 0;             // 盘上文件的事实（包内成员 = 0）
        qint64  mtime = 0;
        int     width = 0;
        int     height = 0;
        // ★★ 这个容器**子树**（含子容器）覆盖的**图的 index 区间**，闭区间 `[firstIndex, lastIndex]`：
        //   · "图的 index" = 恢复后 `m_items` 的下标（**容器不占这个空间**）；
        //   · **扫描时算好、随行写盘** ⇒ 恢复时不用再算（用户 2026-10-18 定案）；
        //   · 找"这张图在哪个容器" = 覆盖它的容器里 `firstIndex` **最大**（并列取 `ord` 更大的）。
        int     firstIndex = -1;
        int     lastIndex = -1;
    };

    // ★★ v5：条目携带的**读取位置**（沿链解析）
    //   · `diskPath` = 链上最外层容器的绝对路径；无容器 = 图片本身的路径
    //   · `members`  = 容器成员链（长度 = 容器层数）；空 = 直接在盘上读
    //   · `innerArchiveMaxMb` = 读这条时用的内层包上限（**随条目带**，因为
    //     解码入口 `image()/imageSize()` 是静态的、拿不到序列实例 ⇒ 参数只能随位置传）
    struct Location {
        QString     diskPath;
        QStringList members;
        int         page = 0;
        qint64      innerArchiveMaxMb = kDefaultInnerArchiveMaxMb;
    };

    struct Item {
        Kind kind = Kind::Image;
        QString label;           // 它在容器里的名字（页行 = 空串）
        QString displayName;     // 显示名（文件名 / 页行 = "file [k/N]"）
        Location loc;            // ★ 读取位置（磁盘路径 + 容器成员链 + 页号）
        QString folderLabel;     // 所属容器（显示用）
        int folderDepth = 0;
        bool missing = false;    // 惰性发现：已消失 / 读不到（**只在会话内**标记）
        QSize size;              // 不解码读到的像素尺寸（判宽窄用；无效 = 未知）
        int ord = -1;            // ★ 遍历序号（写入磁盘时的排序依据）
        int parentNode = -1;     // 所属树节点（**派生**，不写入磁盘；根级 = -1）

        int     pageIndex() const { return loc.page; }
        QString diskPath() const { return loc.diskPath; }
        const QStringList &members() const { return loc.members; }
        // ★ **缓存键 = `ord`**（一个序列内唯一；页行也有自己的 ord）——
        //   身份不再是拼出来的字符串（v5 定案）。
        QString cacheKey() const { return QString::number(ord); }
    };

    // 文件树索引（浏览界面的层级 = 它）
    //   ★ v5：节点只有 `label` + `parent`（身份 = 沿 label 链走）——没有 key / path。
    struct TreeNode {
        //   ★ 节点种类用**同一个** `Sequence::Kind`（`Folder` / `Archive`）——
        //     多页文件也算 `Archive`；不再另立一套 `TreeNode::Kind`（用户 2026-10-18）。
        Kind    kind = Kind::Folder;
        QString label;           // 它在容器里的名字
        int     parent = -1;     // 父节点下标（-1 = 根）
        int     level = 0;
        int     ord = -1;        // 遍历序号（与叶子共用）
        int     firstItem = 0;   // 它的**直接**条目区间 [firstItem, +itemCount)
        int     itemCount = 0;
    };

    // 一次要图的答复（**序列算，调用方只收下**）
    //   ★ **成员 = 本屏的位置**（顺序即显示顺序）——**不看 missing**：
    //     "这一格上的条目能不能显示"由缓存判（设计 12 §8.9 四），序列只管位置。
    struct Group {
        QVector<int> members;    // 本对的位置（双幅 2 个 / 宽图或末尾 1 个）
        int cursor = -1;         // 本对起点；-1 = 没有
    };

    Sequence();
    ~Sequence();

    Sequence(const Sequence &other);
    Sequence &operator=(const Sequence &other);

    // ── ① 会话内扫描（查看序列）──────────────────────────────────
    //   `opt` = 扫描参数（**调用方传**，设计 14 §9.3：序列不读设置）；
    //   带默认值 ⇒ 既有调用点（`startScan(path, 5, false)`）一字不改。
    void startScan(const QString &path, int maxDepth = 5, bool hidden = false,
                   const ScanOptions &opt = ScanOptions());
    void cancelScan();

    // ★★ **停止请求**（"卡住"处置的第一步，由消费侧看门狗调）：置**既有的 cancel 语义**
    //   （`m_cancel`）—— **不新造状态**。生产者每个阻塞点返回后第一件事就查它。
    void requestStop() { m_cancel.store(true, std::memory_order_relaxed); }

    // ── ★★ **发布钩子**（设计 14 §9.2：消费者的**事件驱动唤醒**，不引入周期定时器）────
    //   生产者线程在**每次放批之后**、以及**写终态之后**各同步调用一次。
    //   · **未设置 ⇒ 零开销**：只有一次空 `std::function` 判断，不分配、不取锁；
    //   · **不改变扫描语义**：钩子抛的异常在这里被吞掉（正常钩子**不应该抛**，这是兜底）；
    //   · **绝不许阻塞生产者**：钩子是**生产者线程同步调用**的 ⇒ 它必须自己保证
    //     "只投递、不等待"（`LibraryCore` 用的就是跨线程 `Qt::QueuedConnection` 投递）；
    //   · **在队列锁之外调用**（实现保证）⇒ 钩子里跑多久都不会拖住消费者的 `drain()`；
    //   · **重入约定**：钩子不得在本调用里销毁 / 重启本序列；
    //   · 必须在 `startScan()` **之前**设置（此后固定 —— 生产者线程不再读改它）。
    void setPublishHook(std::function<void()> hook);

    // ★★ **进度**（**生产者写、任意线程可读**）：内部扫描线程在推进时写单调时间戳 ——
    //   · 两个枚举循环内部每 N 条（见 `ArchiveSource::kProgressEvery`）；
    //   · 每次逐条读图头（`imageSize()`）返回后；
    //   · 每批**放批之前**；
    //   · 扫描线程启动时（`startScan` 创建线程那一刻）。
    //   消费侧看门狗（`LibraryCore::pump` / `reapSlot`）据此判"超过 30 s **没有进度更新**"
    //   ⇒ **累积型慢不受影响**（它一直在推进），只有真卡住才不动。
    qint64 lastProgressMs() const { return m_lastProgressMs.load(std::memory_order_relaxed); }

    // ★★ **放弃**（看门狗判"卡住"之后的第二步；在 `requestStop()` 之后调）：
    //   · 置"已放弃"+ 置停止标志；
    //   · **detach 生产者线程**（**不 join** —— 它可能永远回不来，join 就又卡死）；
    //   · 此后**所有 join 点**（`~Sequence` / `cancelScan` / `startScan` / `clear` / 赋值）
    //     都跳过 join；
    //   ⚠️ **调用方必须"不再释放"本对象**（detached 线程还拿着 `this` ⇒ 销毁 = UAF）：
    //      调用方负责**故意泄漏**它（见 `LibraryCore::reapSlot` 的 `slot.release()`）。
    void abandon();
    bool isAbandoned() const { return m_abandoned.load(std::memory_order_relaxed); }

    // ★★ **生产者线程计数**（T2c-2b-2）：线程入口 `+1` / 出口 `−1` —— **所有退出路径都减**
    //   （正常结束、被取消、被 `abandon()` 之后才退出、以及 `scanThreadMain` 抛异常的路径，
    //   实现用 RAII 守卫保证）。
    //   用途：验收"干净收尾 ⇒ 不泄漏生产者线程"与"被放弃的生产者**真的还活着**"——
    //   这两条判定条件**跨平台**（不依赖 `/proc/self/status`）。
    int producerThreadCount() const { return m_producerThreads.load(std::memory_order_relaxed); }

    bool drain();                            // **消费线程**：收下已产出的批次；true = 有变化
    //   ★ T2d-1b：`waitForIndex` **已删** —— 它只服务"引擎在序列上等某个下标"那条路，
    //     而引擎现在**不碰序列**（只吃有界窗口 —— `fetch::Window`；等窗口是 `ViewerModel` 的事）。

    State state() const { return m_state.load(std::memory_order_relaxed); }
    QString unsupportedReason() const { return m_unsupported; }
    // ★ v7：**瞬时提示**（扫描线程产出 —— 例如"内层压缩包超过上限，未展开"）。
    //   读取方 = GUI 侧轮询（`UiModel`）⇒ 一律走 `UiModel::notify`；**不写入磁盘、不是失败**。
    QString noticeText() const;
    bool  scanFinished() const
    {
        const State s = state();
        return s == State::Scaned || s == State::ScanStoped;
    }

    // ── ② 查询（**配对表**；**从头锚定**，序列自己算）──────────────────
    //   ★ 全部**不带版面**（单幅 / 双幅由持有该设置的那一层选）：这里只回答"配对"这份数据。
    //   · `pairAt(i)`      = i 落在的那一对（本对的位置串 + 起点）；
    //   · `pairStart(i)`   = i 所在那一对的起点；
    //   · `pairSize(i)`    = 这一对几张（1 或 2）；
    //   · `nextPairStart(i)` / `prevPairStart(i)` = 下一对 / 上一对的起点（到头 = -1）。
    Group pairAt(int index) const;
    int   pairStart(int index) const;
    int   pairSize(int index) const;
    int   nextPairStart(int index) const;
    int   prevPairStart(int index) const;

    // ── ②b 容器（文件树节点）查询 ──
    int containerOf(int index) const;
    int nextContainerFirst(int index) const;
    int prevContainerFirst(int index) const;
    //   ★★ **子树记录**（L41；语义的**唯一事实源**就是这两行）：
    //     · `subtreeFirst(node)` = 该节点子树里**第一条真图**的下标；**没有图 ⇒ -1**；
    //     · `subtreeCount(node)` = 该节点子树里**真图的条数**；没有 ⇒ 0。
    //   "真图" = **条目表（`m_items`）里的一条**（容器只进树、不进条目表）。
    //   · 记录 = **并入条目那一刻沿"它所属节点 → 根"维护**（见 `appendItem`）——
    //     代价 = **树深**，不是节点数；**没有"收尾时全量重算"、也没有"查询时重算"**；
    //   · ⇒ 这两个是**纯查表只读**（读方法**零写入**：没有 `mutable`、没有脏标志）；
    //   · **"没有第一条"用 -1 表示，且不参与取最小** —— 空节点**不会**把父节点的
    //     "第一条"拉成 0（旧递归就是栽在这：`min(0, …)` 把空容器污染进了所有祖先）；
    //   · **越界**（`node < 0 || node >= tree().size()`）⇒ **0**（既有边界约定，不动）。
    int subtreeFirst(int node) const;
    int subtreeCount(int node) const;
    //   ★★ **是不是元素**（L40 的**唯一实现**；口径 = 下面三条）：
    //     · **图片**：发现即可见（它自己就是一条）—— 本方法只回答**节点**；
    //     · **目录 / 压缩包**：**自己那一支扫完那一刻**才成为元素，并且**要求子树里至少有一条图**；
    //     · ⇒ 空目录 / 无图压缩包 / 不受支持的压缩包（solid / 纯流式）**永远不是元素**；
    //       "子目录全空 ⇒ 自己也不出现"由**同一条判定条件自动成立**，**不需要自底向上剪**。
    //   · **节点不物理删除**（删掉会让树的节点下标位移 ⇒ 把消费方手里"当前在哪一层"的
    //     裸下标指错）：节点留在结构里，但**枚举接口一律过这个判定条件**（**浏览行** /
    //     `fetch::Window::childrenOf`）；而 `fetch::Window::ancestorsOf` 给的是**祖先链**
    //     （层不是元素）⇒ **不过**判定条件（见设计 12 §8.7.1）。
    //   ⚠️ 未扫完的节点**不是元素**（⇒ 不出现在任何枚举里），但它的**记录是"当前已知值"** ——
    //      供 `containerOf` / `prev·nextContainerFirst` 这类**内部查询**用（扫描期间 OSD 就能用）。
    bool isElement(int node) const;
    QPair<int, int> containerRange(int index) const;
    // ★★ **这一条所在容器的子树区间** = {第一条图的 index, 图条数}，**O(1)**：
    //   条目并入时就记下了它**直接所属**的那一层容器（`Item::parentNode`），
    //   该节点的子树区间是并入时沿祖先链维护的 ⇒ 两者都是查表。
    //   ⇒ 循环（文件夹循环）的区间用它，**不搜全树**；直接挂在根上 ⇒ 整条序列。
    QPair<int, int> ownerRange(int index) const;

    // ── ②c **身份链**（v5 resume 用）─────────────────────────────
    //   链 = 从根往下、每层容器在它容器里的名字；**页行不含自己的名字**（label 为空）
    QStringList chainAt(int index) const;              // 越界 ⇒ 空
    int         pageAt(int index) const;               // 越界 ⇒ 0
    //   沿链 walk 树 + 页号精确命中；-1 = 没有
    int indexOfChain(const QStringList &chain, int page) const;
    //   链的**结构化编码**（JSON 数组；**不用分隔符拼串**）
    static QString encodeChain(const QStringList &chain);
    static QStringList decodeChain(const QString &encoded);

    // ── ③ 条目读取（**唯一实现**：QImageReader；预载线程也用它解码）──
    static QSize  imageSize(const Item &it);
    static QImage image(const Item &it);

    // ── ④ 只读 ──────────────────────────────────────────────────
    //   ★★ **两个数量口径，别混用**（设计 14 §9.1「计数用已到达张数」）：
    //     · `count()`           = **可显示边界**（`m_decided`）—— 分组是**从头锚定 + 1 项前瞻**，
    //       所以最后一条已被读到的条目在下一个条目到达之前**还不能定组** ⇒ `count()` 会
    //       **悬着最后一条**（扫描进行中 `count() <= discoveredCount()`，收尾后相等）。
    //       它是**查看/浏览**用的边界：用到达数会让用户跳到未定组的条目、随后画面跳形。
    //     · `discoveredCount()` = **已到达图片数**（`m_items.size()`）—— 卡片"已发现 N 张"、
    //       进度推送要用它（数量一变就报，与分组是否定下来无关）。
    int count() const { return m_decided; }
    int discoveredCount() const { return m_items.size(); }
    const Item &at(int i) const { return m_items.at(i); }
    const QVector<Item> &items() const { return m_items; }
    bool isEmpty() const { return m_items.isEmpty(); }
    bool isNarrow(int index) const;                    // ★ 宽窄的唯一对外接口
    QString rootPath() const { return m_root; }
    bool isRecursive() const { return m_recursive; }
    bool isArchivePages() const { return m_archivePages; }
    QString archivePath() const { return m_root; }
    const QVector<TreeNode> &tree() const { return m_tree; }

    //   ★ 扫描 / 写入磁盘用：**全部行**（节点 + 叶子，按 ord 升序）—— 库扫描器就是落它
    const QVector<Row> &rows() const { return m_rows; }

    // ── ⑤ 静态构建（库扫描器 / 旧路径用，行为不变）──────────────
    void buildFromFolder(const QString &dir, bool recursive);
    void buildFromSingleFile(const QString &filePath);
    void buildFromMultiPageFile(const QString &filePath, int pageCount);
    void buildFromNames(const QString &archivePath, const QStringList &names);
    void buildFromEntries(const QString &rootPath, const QVector<Item> &entries);

    // ★★ **行的来源**（恢复的输入）：只有"下一批"这一个方法（用户 2026-10-18 的设计）。
    //   · 行是 **DFS 前序、父行必在子行前** ⇒ **前向流足够**（不回退、不随机访问）；
    //   · 恢复**边收边并入** ⇒ 不要求把整份行读进内存（库那条用 SQLite 的 forward-only
    //     游标，见 `library::Store::rowSource`）。
    class RowSource
    {
    public:
        virtual ~RowSource() = default;
        // 把最多 `maxRows` 行**追加**到 `out`；返回这一批实际给了几行；0 = 没有更多了。
        virtual int next(QVector<Row> &out, int maxRows) = 0;
    };

    // ── ⑤b **恢复**（行 → 序列）：**唯一的"从磁盘回到内存"入口** ──────────────
    //   · `src` = 行来源（前向流，见上）；`rootIsArchive` = 来源根本身是容器
    //     （压缩包 / 多页文件）；`rootLabel` 显示用；`opt` = 扫描参数（读包内条目时用）；
    //   · ★ **本方法不碰数据库、不碰盘** —— 行从哪来是调用方的事。
    void restore(const QString &rootPath, RowSource &src, bool rootIsArchive = false,
                 const QString &rootLabel = QString(), const ScanOptions &opt = ScanOptions());
    //   便利重载：手里已经有一份行（测试 / 内存里那份）——同一段实现，只是包成一个流。
    void restore(const QString &rootPath, const QVector<Row> &rows, bool rootIsArchive = false,
                 const QString &rootLabel = QString(), const ScanOptions &opt = ScanOptions());

    // ── ⑤c **行的读取位置**（沿链解析的**唯一实现**；库探针 / 封面 / 恢复共用）──
    //   `rows` 必须按 ord 升序、父行在前（DFS 前序）。返回与 rows 等长的位置表。
    static QVector<Location> locateRows(const QString &rootPath, bool rootIsArchive,
                                        const QVector<Row> &rows);

    void clear();
    void sortNaturally();
    int  markMissing();

private:
    void appendItem(const Item &it);
    void finishAppend();                     // 扫描结束：把"待定的最后一条"定下来 + 全部扫完
    void resetGrouping();
    void resetRows();                        // 清空行 / 树 / 记录（startScan / clear / restore）
    // 合成根节点（0 号）：`hasTree=false` = 单张图（没有树）⇒ 只记"根 = -1"
    void initRoot(bool hasTree, Kind root, const QString &rootLabel);
    void applyRows(const QVector<Row> &rows);                // 行 → 树 + 叶子（消费线程）
    void finalizePageRowNames();             // 页行（label 空）的显示名（用**直接** itemCount）
    // ★★ **扫完 + 子树记录**（L40/L41；见公开的 `subtreeFirst/Count` 与 `isElement` 注释）：
    //   · `appendItem` 沿"它所属节点 → 根"维护 `m_subFirst/m_subCount` —— **唯一的写点**；
    //   · 行按**前序 DFS** 并入（父行在子行之前）⇒ 一个节点"还开着"当且仅当它是最近一行
    //     所属节点的**祖先（或它自己）**；新行一来，与它不再共享链的打开节点就**扫完**
    //     （`syncOpenChain`；`m_openStack` = 打开的链，`m_nodeClosed` = 已扫完）；
    //   · 行流结束（扫描收尾 / 库快照构建收尾）⇒ `closeAllOpenNodes`。
    int  addTreeNode(const TreeNode &n);     // 建节点：记录 / 扫完标志 / 并行数组一起长出来
    void syncOpenChain(int parentNode);      // 扫完：不在 `parentNode` 祖先链上的打开节点
    void closeAllOpenNodes();                // 扫完：行流结束
    // ★★ 容器扫完那一刻，把它的**图的 index 区间**写回它那一行（`Row::firstIndex/lastIndex`）：
    //   · 区间 = 子树（含子容器）覆盖的图 index —— `m_subFirst/m_subCount` 早已沿祖先链维护好；
    //   · 只对**行流路径**有意义（`m_rows` 由 `applyRows` 填；静态构建器没有行 ⇒ 直接跳过）；
    //   · 这就是"**扫描产生所有信息**"里那份**边界索引**，随行写盘 ⇒ 恢复零计算。
    void writeBoundaryRange(int node);
    void resetNodeRecords();                 // 清记录（重排 ⇒ 重新沿链累加）
    int  parentNodeOfItem(int index) const;
    int  childNodeWithLabel(int parent, const QString &label) const;

    void scanThreadMain(QString path, int maxDepth, bool hidden);
    // `progress` = 进度钩子（`ArchiveSource::ProgressFn` 的同一形态：true = 继续）——
    //   支持性判定里也要**枚举**容器（7z 解析 / 压缩包探测），枚举期间同样要汇报进度。
    QString checkSupported(const QString &path, const std::function<bool()> &progress);
    void setNotice(const QString &text);         // ★ v7：扫描线程写瞬时提示（互斥保护）

    // ★ **唯一的 join 点**：已放弃 ⇒ **跳过**（detached 的生产者线程 join 不了，join 就又卡死）
    void joinWorker();
    // ★ **生产者**写进度（见 `lastProgressMs()`）；**唯一的写点**
    void bumpProgress() { m_lastProgressMs.store(monotonicNowMs(), std::memory_order_relaxed); }
    // ★★ **发布**（生产者的**唯一调用点**，见 `setPublishHook`）：未设置 ⇒ 立刻返回；
    //   异常吞掉；**必须在队列锁之外调**（放批之后 / 写终态之后各一次）。
    void publish();
    // ★★ **写终态的唯一入口**（生产者）：release 存终态 + `publish()`。
    //   ⚠️ 生产者的每个终止路径都必须走它（否则消费者等不到唤醒 ⇒ 最后一批/终态没人收）。
    void writeTerminal(State s)
    {
        m_state.store(s, std::memory_order_release);
        publish();
    }
    // ★ 生产者**每个阻塞点返回后**第一件事查它（停止请求 / 已放弃）
    bool stopRequested() const
    {
        return m_cancel.load(std::memory_order_relaxed)
            || m_abandoned.load(std::memory_order_relaxed);
    }

    // ★ 内层包上限（MB → 字节）—— **解压前**判定的唯一实现（参数来自 `m_scanOptions`）
    qint64 innerArchiveLimitBytes() const
    {
        return m_scanOptions.innerArchiveMaxMb * 1024 * 1024;
    }

    // ── 交接：**一把锁 + 无界批队列**（扫描线程 → 消费线程）──
    //   `Batch` **不可变**：一旦入队，生产者不再碰它（消费者出锁后再处理）。
    struct Batch {
        QVector<Row> rows;
        // 根描述（只有第一批发）：`rootSet` = 这一批带根描述；`rootHasTree=false` = 单张图（没有树）
        bool         rootSet     = false;
        bool         rootHasTree = false;
        Kind         rootKind    = Kind::Folder;
        QString      rootLabel;
    };

    QVector<Item>     m_items;
    QVector<TreeNode> m_tree;
    // ★★ **子树记录**（`m_tree` 的**并行数组**；**只由 `appendItem` 沿祖先链写**）：
    //   `m_subFirst[i]` = 含全部后代的**第一条真图**下标（**-1 = 还没有图**）；
    //   `m_subCount[i]` = 含全部后代的**真图条数**。
    //   ★ **不是 `mutable`、没有"脏"标志**：`subtreeFirst/Count` 是纯查表只读
    //     （读方法零写入 —— 这是硬性判定条件；写入只发生在"改树 / 并条目"那一步）。
    QVector<int>  m_subFirst;
    QVector<int>  m_subCount;
    // ★★ **扫完**（L40；同为 `m_tree` 的并行数组）：
    //   `m_nodeClosed[i]` = 节点 i 那一支**已扫完**（后续再无属于它子树的条目）；
    //   `m_openStack` = 当前"打开"的**前序链**（根 → 最近一行所属节点）—— 只被并入路径碰。
    QVector<char> m_nodeClosed;
    QVector<int>  m_openStack;
    QVector<int>  m_chainBuf;                // `syncOpenChain` 的复用缓冲（不参与值语义）
    QVector<Row>      m_rows;
    // ★★ 扫描参数（**调用方经 `startScan` / `restore` 传入** —— 序列不读设置）
    ScanOptions       m_scanOptions;
    QHash<int, int>   m_rowIndexByOrd;       // 行 ord → 行下标（**沿链解析**用）
    QHash<int, int>   m_nodeIndexByOrd;      // 行 ord → 树节点下标（含 -1 → 根）
    bool              m_rootInit = false;    // 根节点描述已到（或本来就没有树）
    // 扫描路径：尺寸读不到 ⇒ 标 missing（**只在会话内**）；库行路径：尺寸未知 ≠ 缺失
    bool              m_markMissingBySize = true;

    // 从头锚定分组
    QVector<int> m_gStart;
    QVector<int> m_gSize;
    int m_pending = -1;
    int m_decided = 0;

    QString m_unsupported;
    // ★ v7：瞬时提示（扫描线程写 / GUI 侧读 ⇒ 互斥；**不写入磁盘**）
    mutable QMutex m_noticeMutex;
    QString        m_notice;
    QString m_root;
    bool m_recursive = false;
    bool m_archivePages = false;
    bool m_rootIsArchive = false;

    // ── 扫描侧 ──
    QMutex                m_qMutex;          // ★ **只保护队列本身**（临界区只有 O(1) 的 push / swap）
    std::deque<Batch>     m_q;               // ★ **无界**；生产者**永不阻塞**
    std::atomic<State>    m_state{State::NotReady};
    bool                  m_finished = false; // 消费者私有：`finishAppend` 只做一次
    std::atomic<bool>     m_cancel{false};
    // ★★ 进度时间戳（单调 ms；**生产者写、任意线程可读**）—— 见 lastProgressMs()
    std::atomic<qint64>   m_lastProgressMs{0};
    // ★★ 已放弃（看门狗 `abandon()` 置）—— 所有 join 点看它
    std::atomic<bool>     m_abandoned{false};
    // ★★ 生产者线程计数（见 `producerThreadCount()`）：线程入口 +1 / 出口 −1（RAII 守卫）
    std::atomic<int>      m_producerThreads{0};
    std::thread           m_worker;
    // ★★ 发布钩子（见 `setPublishHook`）：**startScan 之前**设好，之后生产者只读 ⇒ 无锁。
    //   声明在最后：它与扫描线程同生共死（析构时线程已被 join / detach）。
    std::function<void()> m_publishHook;
};
