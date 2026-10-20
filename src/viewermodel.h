#pragma once

// ══ viewer：阅读界面的模型（位置 / 预取 / 上屏）══════════════════════════════
//
//   ★ 本类**整个住在主线程**：来源 / 位置 / 槽位表 / 上屏内容只有主线程碰；
//     解码在工作线程上做，解完只 post 回主线程。**没有任何两侧都能写的东西。**
//
//   ## 术语
//     · **槽位**：一个"正在等某一条 / 已经备好某一条"的位置。池上限 = `L + 1`。
//     · **条目**：序列里的一条（`Sequence::Item`）。
//     · **屏**：序列按**版面**分好的**位置集合**（单幅 1 条 / 双幅 2 条；宽图、末尾剩一张
//       ⇒ 1 条）。**位置是序列给的**（`groupAt`），本类不算分组、不做 ±屏 的算术。
//     · **预取**：在当前屏之外，按份额规则提前把条目取好。
//     · **上屏**：把本屏各位置的图解出来放进 `imageA` / `imageB`（坏的位置留空图）。
//     · **loading（界面浮层）**：还没扫完 **且**（还没有条目 **或** 位置已处在已知末尾）；
//       还没有序列也算 —— 也就是「序列还没就绪 / 扫描中到达边界」。
//       ★ **正常等图片缓存（当前这一屏还没就位）不算**（用户 2026-10-17 口径）。
//     · **位置**：现在要显示的是哪一条（条目下标）；**屏 = 位置所在的那一屏**。
//     · **坏**：这一条拿不出图（**缺失** = 元数据已标 / **不可读** = 解码出来是空图）。
//       ★ **坏不占长度**：坏因另存一张小表（条目 → 坏因），**不占** `L + 1` 的槽位额度。
//
//   ## 份额规则（要哪些条目）
//     · 一次操作 = **位置真的变了**；只是重判不算。
//     · `f` = 前向条数，起始 6、限 `[1, L−1]`（`L` = 预取长度，构造参数）。
//       同向再走一屏 ⇒ `f+1`；反向 ⇒ `f−1`；夹在 `[1, L−1]`。
//     · 最近方向那一侧的条目排在前面。
//     · 要的条目 = **本屏的位置**（永远最先）+ 本屏之外的那 `L` 条
//       （从**屏外第一条**起数：前向 `+1..+f`、后向 `−1..−(L−f)`）。
//       跳转 / **换版面** / 换来源 ⇒ 回到起始（`f = 6`、方向为正）。
//
//   ## 上屏
//     · **重判是唯一决定上屏的地方**：本屏每个位置都**有结论**（备好 / 坏）⇒ 上屏 ——
//       备好的位置画出来，坏的位置**留空图**（双幅留空位）；
//       本屏**整屏都坏**（单幅就是那一条坏）⇒ 跳过计数 +1、位置挪到下一屏（同方向）、再派活再判；
//       还有位置没结论 ⇒ 等下一个结果回来。
//     · 位置一变**不清掉 `imageA` / `imageB`**（不许先显示空图或上一张）；等图片缓存期间画面上
//       还是上一屏，`loading` 为假（浮层不显示）。
//
//   ## 版面（单幅 / 双幅）
//     · **换版面 = 换个开关**：保留**当前位置**，屏由序列重算（`anchorOf`）⇒ 不重新分组。
//     · `stepPage(±1)` = **换一屏**：前进量交给序列（`advance`），本类收下游标。
//     · 按键那条判断按**整屏**算：本屏还有位置没结论（既没备好、也不是坏）⇒ 这次按键丢掉。
//       **坏算"有结论"** ⇒ 那时按键照常放行（否则"坏到末尾"会卡在末尾出不来）。
//
//   ## 退出查看 / 换来源
//     · **不 join 工作线程**（join 会把退出拖住几百毫秒）：换个记号就走；
//       在飞的回来时记号不符就被丢掉，既不写错槽位也不影响下一次进入。
//     · 析构 = **记号 + 标记失效**（见 .cpp 的 `Poster`）。

#include <QHash>
#include <QImage>
#include <QObject>
#include <QVector>

#include <functional>
#include <memory>
#include <thread>

class QTimer;

#include "appsettings.h"   // ★ app::Layout：阅读版面 = 一份枚举（本类只"持有并投影"，不定义）
#include "sequence.h"

namespace fetch {

class ViewerModel : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QImage imageA READ imageA NOTIFY currentChanged)
    Q_PROPERTY(QImage imageB READ imageB NOTIFY currentChanged)            // 单幅 / 空位：空图
    Q_PROPERTY(int keyCount READ keyCount NOTIFY currentChanged)           // 本屏位置数（1 或 2），无屏 = 0
    //   ★ QML 边界的那一份**投影**（界面只要"是不是双幅"这一个布尔）：
    //     概念本身 = `app::Layout`（`appsettings.h`）＝ 库里 `reading_layout` 存的那份。
    Q_PROPERTY(bool doublePage READ doublePage WRITE setDoublePage NOTIFY doublePageChanged)  // 写 = 换版面
    //   循环（到头怎么办）＝ 会话级、不写盘：0 不循环 / 1 全部循环 / 2 文件夹循环
    //   （**值就是界面的菜单下标**，一份表示）
    Q_PROPERTY(int loopMode READ loopMode WRITE setLoopMode NOTIFY loopModeChanged)
    //   上一 / 下一文件夹的**目标名字**（空 ⇒ 界面把按钮灰掉）
    Q_PROPERTY(QString prevFolderName READ prevFolderName NOTIFY currentChanged)
    Q_PROPERTY(QString nextFolderName READ nextFolderName NOTIFY currentChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    //   当前这一屏的**文件名**（双幅两页时用 " · " 连起来；多页文件是 "file [k/N]"）——
    //   顶栏显示用，派生值，不改任何状态
    Q_PROPERTY(QString currentName READ currentName NOTIFY currentChanged)
    //   当前这一屏第一幅**相对图集根的路径**（顶栏用；与 OSD 的"路径"行同一份实现）
    Q_PROPERTY(QString relativePath READ relativePath NOTIFY currentChanged)
    //   ★ 幻灯片（13 §九·补·二）：`playing` **只是一个状态**（没有幻灯片对象）
    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(int cursor READ cursor WRITE setCursor NOTIFY currentChanged)  // 写 = 跳转；= 本屏起点

    // ── 阅读视图态（**viewer 领域**，2026-10-20 定案）────────────────────────
    //   随 viewer 生灭；**初值由上层从全局设置下发**（`UiModel::applyViewerDefaults`）。
    //   UI（底栏）上的调整只改这里，**不回写**全局默认；只有设置对话框才落盘。
    //   单位：zoom = 设备像素/源像素；pan = 设备像素；视口也是设备像素（QML 报逻辑尺寸 + dpr）。
    Q_PROPERTY(int   fitMode READ fitMode WRITE setFitMode NOTIFY fitModeChanged)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY zoomChanged)
    Q_PROPERTY(qreal panX READ panX WRITE setPanX NOTIFY panChanged)
    Q_PROPERTY(qreal panY READ panY WRITE setPanY NOTIFY panChanged)
    Q_PROPERTY(bool  zoomLocked READ zoomLocked WRITE setZoomLocked NOTIFY zoomLockedChanged)
    Q_PROPERTY(bool  osdVisible READ osdVisible WRITE setOsdVisible NOTIFY osdVisibleChanged)
    Q_PROPERTY(QString scaleAlgorithm READ scaleAlgorithm WRITE setScaleAlgorithm
                                    NOTIFY scaleAlgorithmChanged)
    Q_PROPERTY(int   intervalMs READ intervalMs WRITE setIntervalMs NOTIFY intervalChanged)
    Q_PROPERTY(int count READ itemCount NOTIFY currentChanged)             // 条目总数
    Q_PROPERTY(int totalPages READ totalPages NOTIFY currentChanged)       // = count（口径不变：按条目）
    Q_PROPERTY(int currentPage READ currentPage NOTIFY currentChanged)     // = cursor（本屏起点）

public:
    explicit ViewerModel(int prefetchLength, QObject *parent = nullptr);
    ~ViewerModel() override;

    ViewerModel(const ViewerModel &) = delete;
    ViewerModel &operator=(const ViewerModel &) = delete;

    void setSequence(const Sequence *seq);   // 来源（非拥有）
    void sequenceChanged();                  // 序列长进了
    Q_INVOKABLE void stepPage(int delta);    // ±1 屏（前进量由序列按版面算）
    Q_INVOKABLE void jump(int imageIndex);   // 跳到某一条（时间轴拖 / 浏览里选中）
    Q_INVOKABLE void stepFolder(int delta);  // ★ 给 QML 的同一件事（`delta` = ±1）
    Q_INVOKABLE void togglePlaying();        // ★ 幻灯片：播放 ⇄ 暂停（`Space` / ⏯）

    // ── 幻灯片（规则见设计 13 §九·补·二）────────────────────────────────
    //   ★ 没有幻灯片对象：只有 `playing` 一个状态 + 一个**单发闹钟**（只报时）+
    //     一个**推进等待位**（一个 bool）+ 一个**折叠器**（等缓存期间的进度事件，一个槽）。
    //   ★ 节奏一律由**单调时钟**算：`到期 = 请求时刻 + 间隔`（间隔是下限，准备耗时吃掉间隔）。
    bool playing() const { return m_playing; }
    void setPlaying(bool on);
    int  intervalMs() const { return m_intervalMs; }
    void setIntervalMs(int ms);                              // 由上层从设置下发
    using NowFn = std::function<qint64()>;                   // 时间源（自检注入虚拟时钟）
    void setNowFn(NowFn fn) { m_now = std::move(fn); }

    // ── 视图态（几何只有一处：zoom/pan + fit 命令，见 gpuscale.h）──────────
    int   fitMode() const { return m_fitMode; }
    void  setFitMode(int m);              // fit 是**命令**：算一次 zoom（pan=0）
    qreal zoom() const { return m_zoom; }
    void  setZoom(qreal v);
    qreal panX() const { return m_panX; }
    void  setPanX(qreal v);
    qreal panY() const { return m_panY; }
    void  setPanY(qreal v);
    bool  zoomLocked() const { return m_zoomLocked; }
    void  setZoomLocked(bool on);
    bool  osdVisible() const { return m_osdVisible; }
    void  setOsdVisible(bool on);
    QString scaleAlgorithm() const { return m_scaleAlgorithm; }
    void  setScaleAlgorithm(const QString &key);   // 键串（nearest|smooth|bicubic|lanczos2|lanczos3）
    Q_INVOKABLE void setViewport(qreal logicalW, qreal logicalH, qreal dpr);
    Q_INVOKABLE void applyFit();
    Q_INVOKABLE void panBy(qreal dx, qreal dy);
    Q_INVOKABLE void zoomAtPoint(qreal factor, qreal anchorX, qreal anchorY);

    // ── OSD 信息行（**按需读取**，用户 2026-10-21 定案）─────────────────────
    //   ★ 不是属性、没有 NOTIFY、不进任何成员：EXIF / 尺寸 / 色彩空间是**信息性数据**
    //     （AGENTS.md「信息性数据用 getter，不进 VM 状态」），界面要看时才读一次。
    //   返回"列"：单幅 1 列、双幅 2 列；每列 = 该幅的行（`{k, v, section}`；
    //   EXIF 行**有才出现** —— 不同图片标签集不同，绝不固化）。
    Q_INVOKABLE QVariantList osdRows();
    //   图集标题（属于"这一屏的上下文"，不是 OSD 数据）：由上层建 viewer / 进图集时下发
    void setCollectionLabel(const QString &label) { m_collectionLabel = label; }

    QImage imageA() const { return m_imageA; }
    QImage imageB() const { return m_imageB; }      // 单幅 / 坏的空位：空图
    int  keyCount() const { return m_keyCount; }
    bool doublePage() const { return m_layout == app::Layout::Double; }
    void setDoublePage(bool on);                    // 换版面（保留当前位置，屏重算）
    int  loopMode() const { return int(m_loop); }
    void setLoopMode(int mode);                     // 写 = 换循环模式（不写盘；进沉浸视图回默认）
    QString currentName() const;
    QString relativePath() const { return m_screenPath; }
    QString prevFolderName() const;
    QString nextFolderName() const;
    bool loading() const { return m_loading; }
    int  cursor() const { return m_cursor; }        // 上屏那一屏的起点（写 = 跳转）
    void setCursor(int index) { jump(index); }
    int  itemCount() const { return m_seq ? m_seq->count() : 0; }
    int  totalPages() const { return itemCount(); } // 口径不变：按条目
    int  currentPage() const { return m_cursor; }

Q_SIGNALS:
    void currentChanged();
    void imageAChanged();
    void imageBChanged();
    void doublePageChanged();
    void fitModeChanged();
    void zoomChanged();
    void panChanged();
    void zoomLockedChanged();
    void osdVisibleChanged();
    void scaleAlgorithmChanged();
    void intervalChanged();
    void loopModeChanged();
    void loadingChanged();
    void playingChanged();
    //   瞬时提示（幻灯片到头）。上层（`UiModel`）转发到 `UiModel::notify`。
    void notice(const QString &text);

private:
    //   槽位 = { 它在等哪一条 / 状态 / 备好时那张图 }。空槽 = `item < 0`。
    //   ★ 槽位池里**只有两种状态：在飞 / 备好** —— "坏"不在这里（见 `m_bad`）。
    struct Slot {
        enum class State { Empty, InFlight, Ready };
        int    item  = -1;
        State  state = State::Empty;
        QImage image;                       // 只有"备好"才有
    };

    //   坏因：这一条拿不出图的原因。**只有两种**，对应两个已知写点（见 .cpp）。
    enum class Bad {
        Missing,      // 元数据已标缺失（`Item::missing`）⇒ 不派解码
        Unreadable,   // 解码回来是空图 ⇒ 不可读
    };

    //   ★ **post 交接点没有锁**：工作线程**完全不碰 `ViewerModel`**，也**不读**下面两个字段
    //     —— 它只解码 + 把结果 post 给一个**长期存在的主线程对象**（见 .cpp 的 `preload`）。
    //     `valid` / `target` 的读（那条 lambda）与析构的写**同在主线程** ⇒ 不需要锁、不需要 atomic。
    //   ★ 每个实例有**自己的一份**：`ViewerModel` 是短的（随查看建了又毁）⇒ 销毁置空之后，
    //     还在路上的结果**全部丢弃**（正常路径，不是异常情况）。
    //   `Poster` 由 `shared_ptr` 持有 ⇒ 工作线程在结果排队期间用它也不会悬空。
    struct Poster {
        bool          valid  = true;
        ViewerModel  *target = nullptr;   // 析构置空 ⇒ 在飞结果不再碰本对象
    };

    // ── 工作线程 ────────────────────────────────────────────────────────
    //   只做两件：加载并解码（生产实现 = `imageread::image`）；解完把结果 post 回主线程。
    //   ★ **不做取消**；解码失败（空图）当成"坏（不可读）"。
    void preload(const Sequence::Item &imageMeta, int slot);

    // ── 结果回主线程 ────────────────────────────────────────────────────
    //   先比记号，再比"这个槽现在还等不等这一条"；任一条不符 ⇒ 直接丢（白干，不算数）。
    void onLoaded(quint64 epoch, int slot, const QImage &image);

    // ── 每个动作（stepPage / jump / 换版面）：改位置 → 当场判 → 就位就上屏 / 没就位就派活再等
    //   `stepDir` = 这次是"换一屏"（+1 / −1）；0 = 跳转或换版面（份额回到起始）。
    void act(int target, int stepDir);
    //   按"本屏位置 → 份额那串偏移"的顺序逐条派活（**本屏永远最先**）。
    void dispatch();
    //   唯一决定上屏的地方。
    void judge();
    void screen(int start, const QVector<int> &items);
    //   浮层的**唯一更新入口**：按当前状态当场算「该不该显示」（见类头 loading 那条）。
    void updateLoading();
    //   本屏每个位置的状态：`settled` = 都有结论（备好 / 坏）；`anyReady` = 至少一个备好。
    void scanScreen(const QVector<int> &items, bool &settled, bool &anyReady) const;
    //   份额规则：位置真的变了才算一次操作；`dir` = 换屏的方向（±1）。
    void observeOperation(int dir);
    void resetShare();

    // ── 版面 / 屏 ────────────────────────────────────────────────────────
    //   本屏 = 单幅的"这一条" / 双幅的"这一对"（**屏是视图的说法**，由版面选）；
    //   双幅那一对的位置由序列的**配对表**给（`pairStart` / `pairAt`），本类不自己算。
    int  screenStart(int item) const;
    QVector<int> screenItems(int start) const;
    // ── 循环（到头怎么办）────────────────────────────────────────────────
    //   三个值：**不循环**（到头就是到头）/ **全部循环**（末尾回开头）/ **文件夹循环**
    //   （只在**当前这一条所在容器**的区间里环绕）。
    //   ★ 循环**归本类**（`Sequence` 只描述序列）；判定**只看当前 index**（不按屏讨论）。
    enum class Loop { None = 0, All = 1, Folder = 2 };
    QPair<int, int> loopScope() const;          // 当前 index 所在容器的区间（O(1)，序列给）
    bool inLoopScope(int item) const;           // 这一条在不在本次循环的区间里
    int  lastScreenStart() const;               // 最后一屏的起点
    int  wrapTarget(int dir) const;             // 文件夹循环的回绕目标（区间两端所在那屏）
    // ★★ **上一个 / 下一个文件夹**（用户 2026-10-19 定案；**规则在本类**，序列只给事实表）：
    //   · 候选 = 序列的容器区间表（根不在里面）；**包含当前 index 的容器距离 ≤ 0 ⇒ 排除**
    //     （你正躺在它里面 —— 所以"在深层"与"在上层"候选集合不同，这是**看当前状态**的判定）；
    //   · 下一个 = `first_index > 当前 index` 里**最近**的（first 最小的）；
    //     上一个 = `last_index < 当前 index` 里**最近**的（last 最大的）；
    //   · **距离并列**（同一个 first / 同一个 last ⇒ 只可能是嵌套链）⇒ 取**最上层**那个
    //     （`level` 最小的）—— 名字与落点都取它；
    //   · 落点 = 那个容器的**第一条图**（双幅由配对表锚定到含它的那一屏）；
    //   · 名字 = 那个容器**自己**的 `label`（单段，不取落点那条图的 `folderLabel`）。
    //   返回**容器节点下标**；-1 = 到头（不循环 / 区间外）。
    //   ★ **文件夹内循环（`Loop::Folder`）⇒ 两个按钮禁用置灰、不许切文件夹**
    //     （用户 2026-10-19；以前到头会拿当前文件夹名顶上、还会跳到区间外）。
    int  adjacentFolderNode(int dir) const;     // dir > 0 = 下一个，< 0 = 上一个（二分）
    int  edgeFolderNode(int dir) const;         // 全部循环的回绕：第一个 / 最后一个容器
    QString folderNameOfNode(int node) const;   // 容器自己的名字（单段）
    //   换一屏：**单幅** = 相邻一条；**双幅** = 问序列的配对表（+本对张数）。到头 ⇒ -1。
    //   ⇒ "±本对张数"的算术在序列的配对表里，本类只做"这次用一条还是用一对"的选择。
    int  nextScreen(int start, int dir) const;
    bool onScreen(int item) const;             // 这一条在不在当前屏上（淘汰时不离手）
    int  slotOf(int item) const;      // 管着这一条的槽（-1 = 没有）
    QImage imageOf(int item) const;   // 这一条备好时的那张图（坏 / 在飞 ⇒ 空图）
    int  takeSlot(int item);          // 找一个槽给它（池满 ⇒ 淘汰；找不到 ⇒ -1）
    bool isBad(int item) const;       // 坏因表里记着这一条吗（**不占**槽位额度）
    void markBad(int item, Bad why);  // 把坏因写进小表（两个写点的唯一入口）

    static constexpr int kMaxInFlight = 4;   // 并发上限（在飞计数上限；写死）
    static constexpr int kStartForward = 6;  // `f` 的起始值

    const int m_length;                      // 预取长度 L（构造参数）
    const Sequence *m_seq = nullptr;         // 来源（非拥有；只有主线程读）
    std::shared_ptr<Poster> m_poster;        // 工作线程 post 回来的落点（见上）

    // ── 状态（全在主线程）────────────────────────────────────────────────
    QVector<Slot> m_slots;                   // 一张槽位表（池上限 = L + 1）
    //   坏因小表：条目 → 坏因。**不占** `L + 1` 的额度；**同一次查看内一直保留**
    //   （这就是"坏不重试"的依据）；换来源（`setSequence`）时清空。
    QHash<int, Bad> m_bad;
    int     m_inFlight = 0;                  // 在飞计数
    int     m_skipped  = 0;                  // 跳过计数
    quint64 m_epoch    = 0;                  // 换来源记号（单调递增）

    int    m_pos    = 0;                     // 位置：用户指到的那一条（屏 = 它所在的那一屏）
    int    m_cursor = 0;                     // 上屏那一屏的起点（只在 `screen` 里写）
    QImage m_imageA, m_imageB;               // 上屏内容（坏的位置 / 没有第二个位置 = 空图）
    int    m_keyCount = 0;                   // 本屏位置数（1 或 2），无屏 = 0
    app::Layout m_layout = app::Layout::Single;   // ★ 版面 = 一份枚举（默认单幅）
    Loop   m_loop = Loop::None;                   // ★ 循环模式（会话级；进沉浸视图回默认）
    bool   m_loading  = true;                // 界面浮层（只有 `updateLoading()` 会写）

    int m_f   = kStartForward;               // 前向条数
    int m_dir = +1;                          // 最近方向（+1 / −1）

    // ── 幻灯片（13 §九·补·二）────────────────────────────────────────────
    //   两种等待必须分开：**等闹钟**（本屏已就位，能响应）/ **等缓存**（本屏还没上屏，不能响应）。
    //   进度事件（翻页 / 跳转 / 上一·下一文件夹）在等闹钟时**立刻执行**，
    //   在等缓存时进**折叠器**（一个槽，后到覆盖先到，上屏那一刻执行一次）。
    enum class Want { None, Step, Jump, Folder };   // 折叠器里的那条"最终意图"
    bool    waitingForCache() const;          // 现算：本屏还没结论（= 等缓存）
    void    onBeat();                         // 闹钟到点（按**当时**状态计算）
    void    onScreenUp();                     // 上屏那一刻（在 `screen()` 里调）
    void    foldOrRun(Want w, int arg);       // 进度事件的统一入口（播放态）
    void    runWanted(Want w, int arg, bool folded);
    void    requestScreen(int target, int stepDir);   // 换屏 + 记下"请求时刻"
    void    armBeat();                        // 挂/重挂闹钟（等缓存时不挂）
    void    cancelBeat();
    void    stopPlaying(const QString &noticeText);   // 停播（到头）
    qint64  nowMs() const { return m_now ? m_now() : 0; }

    bool     m_playing = false;               // ★ 只是一个状态（没有幻灯片对象）
    int      m_intervalMs = 300;              // 间隔（初值由上层从全局默认下发；UI 改动不回写）

    // ── 视图态（viewer 领域）──
    QImage geometryImage() const;             // 双幅取有图的那一张
    int    m_fitMode = 1;                     // 0 原始 / 1 适应屏幕 / 2 适应宽度 / 3 适应高度
    qreal  m_zoom = 1.0, m_panX = 0.0, m_panY = 0.0;
    QSize  m_viewport;                        // 设备像素
    qreal  m_dpr = 1.0;
    bool   m_freeZoom = false;                // zoom 被用户改过 ⇒ fit 不再接管（pan 不算）
    bool   m_zoomLocked = false;
    bool   m_osdVisible = false;
    QString m_scaleAlgorithm = QStringLiteral("lanczos3");
    QString m_screenName;                     // 当前这一屏的文件名（上屏时按真实条目算）
    QString m_screenPath;                     // 当前这一屏的路径（同上）
    QString m_collectionLabel;                // 图集标题（会话来源 = 空）
    QVariantList osdColumn(int itemIndex);    // OSD 的一列（按需读；见 osdRows）
    QString      relativePathOf(int itemIndex) const;   // 相对图集根的路径（`A!/B`）
    QTimer  *m_beat = nullptr;                // 单发闹钟（只负责"到点叫醒"，不参与节奏）
    NowFn    m_now;                           // 单调时钟（可注入；生产在构造里装）
    qint64   m_reqAt = 0;                     // 上一次"成为当前屏"的动作**被请求**的时刻
    bool     m_pendingAdvance = false;        // 推进等待位（一个 bool；忙碌期只有一个）
    Want     m_want = Want::None;             // 折叠器（一个槽）
    int      m_wantArg = 0;
};

} // namespace fetch
