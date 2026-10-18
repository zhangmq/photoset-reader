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

#include <memory>
#include <thread>

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
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(int cursor READ cursor WRITE setCursor NOTIFY currentChanged)  // 写 = 跳转；= 本屏起点
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

    QImage imageA() const { return m_imageA; }
    QImage imageB() const { return m_imageB; }      // 单幅 / 坏的空位：空图
    int  keyCount() const { return m_keyCount; }
    bool doublePage() const { return m_layout == app::Layout::Double; }
    void setDoublePage(bool on);                    // 换版面（保留当前位置，屏重算）
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
    void loadingChanged();

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
    //   只做两件：加载并解码（生产实现 = `Sequence::image`）；解完把结果 post 回主线程。
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
    bool   m_loading  = true;                // 界面浮层（只有 `updateLoading()` 会写）

    int m_f   = kStartForward;               // 前向条数
    int m_dir = +1;                          // 最近方向（+1 / −1）
};

} // namespace fetch
