#pragma once

// ══ 浏览界面的**界面模型**（QML 里的 `ui.browse`；T2d-1b 从 viewer 里搬出来）════════
//
//   ★★ 用户定案（2026-10-15，设计 14 §9.2/§9.8）：三个界面各有一个 model，**并列挂在 `ui`
//     这一级**（照 `ui.library` 的样子）：`ui.library`（LibraryModel，已有不动）/
//     **`ui.browse`（本类）** / **`ui.viewer`（ViewerModel）**。
//
//   ★ **浏览只依赖"序列的树 + 缩略图 provider"**：行 / 面包屑 / 能否上一级 / 范围标签 /
//     高亮**全部从序列取**（`fetch::browseLayerOf`，**唯一实现**）——
//     **不依赖取图 / 预取 / 缓存 / `ui.viewer`**（设计 12 §8.9 口径 7：
//     浏览行不再随窗口）。序列由 `UiModel` 给（它持有序列）；当前条目下标（游标）
//     也由 `UiModel` 告知（游标一变就重算高亮行）。
//   ★ **动作**：换层（进入 / 上一级 / 面包屑）**纯浏览**，只改本类的层下标 + 重算这一层；
//     "选中 / 打开某条图"是**动作**，经 `rowSelected` / `openRequested` 交给 `UiModel`
//     （它才认识 viewer 与抢占屏）。
//
//   ⚠️ 名字说明：本类 = `app::BrowseModel`（QObject，QML 的 `ui.browse`）；
//      行模型 = **全局命名空间的 `::BrowseModel`**（`QAbstractListModel`，见 browsemodel.h）。
//      两者都在 `uimodel.cpp` 可见 ⇒ 文件里对行模型一律写 `::BrowseModel`。
//
//   界面（`src/ui/browse/**`，**定稿**）只认下面这几个名字：
//     items（当前这一层的子项）/ crumbs [{label, locked}] / canGoUp / rangeLabel / browseRow

#include "browsemodel.h"          // ::BrowseModel（行模型）

#include <QAbstractItemModel>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVector>

class Sequence;

namespace app {

class BrowseModel : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QAbstractItemModel *items READ items NOTIFY changed)
    Q_PROPERTY(QVariantList crumbs READ crumbs NOTIFY changed)
    Q_PROPERTY(bool canGoUp READ canGoUp NOTIFY changed)
    Q_PROPERTY(QString rangeLabel READ rangeLabel NOTIFY changed)
    //   `browseRow` = **进度**落在哪一行（**派生**自"当前条目"，不另存状态）
    Q_PROPERTY(int browseRow READ browseRow NOTIFY changed)
    //   `selectedRow` = **本屏选中**的那一行（用户单击；-1 = 没选中）。
    //     ★ 与 `browseRow` 是两件事：选中**不改变任何阅读状态**（存档台账 L24 / L25）。
    Q_PROPERTY(int selectedRow READ selectedRow NOTIFY selectedChanged)
    //   过滤词（空 = 不过滤）：**只筛当前这一层**；序列不动（用户 2026-10-24）
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)

public:
    explicit BrowseModel(QObject *parent = nullptr);

    // ── 数据来源（**GUI 线程**；都由 `UiModel` 给）──────────────────────────
    //   序列（**非拥有**；nullptr = 没有来源）。换来源 = `clear()` 之后再设新的。
    //   设完立刻重算这一层（层下标为 -1 = 根）。
    void setSequence(const Sequence *seq);
    //   当前条目的下标（= viewer 的游标）—— 只影响**高亮行**；变了就重算。
    void setCursor(int itemIndex);
    //   换层：设层下标 + 立刻重算（`activate` / `up` / `crumb` 内部都走它）。
    //   -1 = 根 / 整条序列（无树来源）。
    void setNode(int node);
    //   按"当前序列 + 当前层 + 当前条目"重算这一层（序列长了 / 收尾了 ⇒ `UiModel` 调它）。
    void refresh();
    // 换来源 / 回库屏：清空（行 / 面包屑 / 高亮 / 序列）—— 免得留着上一条来源的层
    void clear();
    // 单击一行 = **只选中**（不碰 viewer、不改进度）；越界 / 换层后自动清掉
    Q_INVOKABLE void selectRow(int row);
    // 把"当前层"对到**进度所在的那一层**（从 viewer 退回浏览时用：不回到文件夹根）
    void revealProgress();
    // 面包屑下拉：直接进同层的另一个容器（等价于 `setNode`，只是给 QML 一个入口）
    Q_INVOKABLE void goNode(int node) { setNode(node); }
    void setFilter(const QString &text);

    int  node() const { return m_node; }

    // 界面直接绑的那几个
    QAbstractItemModel *items() { return &m_rows; }
    QVariantList crumbs() const { return m_crumbs; }
    bool canGoUp() const { return m_canGoUp; }
    QString rangeLabel() const { return m_rangeLabel; }
    int  browseRow() const { return m_browseRow; }
    int  selectedRow() const { return m_selectedRow; }
    QString filter() const { return m_filter; }

    // ── 动作（QML 调；语义见下）────────────────────────────────────────
    //   容器 ⇒ 进入那一层（留在浏览屏）；图 ⇒ 移动那个唯一的游标（= 设置进度）
    Q_INVOKABLE bool activate(int row);
    //   打开那条图（图 ⇒ 选中 + 切查看屏；容器 ⇒ 与 `activate` 同义）
    Q_INVOKABLE bool open(int row);
    Q_INVOKABLE bool up();                // [上一级]（到头 ⇒ 什么都不做，返回 false）
    Q_INVOKABLE bool crumb(int index);    // 跳到某一层（首段 = 锁死的根）
    //   第 `row` 行是不是**容器**（可进入）—— QML 用它决定"双击 = 进入 还是 打开"
    Q_INVOKABLE bool isContainerRow(int row) const;

Q_SIGNALS:
    void changed();                       // 这一层（行 / 面包屑 / 高亮）变了
    void selectedChanged();               // **本屏选中**换了（与进度无关）
    void filterChanged();                 // 过滤词变了
    void rowSelected(int itemIndex);      // 图行被选中 ⇒ `UiModel` 让 viewer 跳过去
    void openRequested(int itemIndex);    // 打开那条图 ⇒ `UiModel` 切到查看屏
    //   换层（`reason`：0 = 上一级 / 面包屑；1 = 进入容器）
    //   ⇒ `UiModel` 做"进入容器时把游标落到它的第一条图"（浏览行本身不经过任何有界窗口）
    void layerChanged(int reason);

private:
    ::BrowseModel m_rows;                              // 行模型（对象恒定：QML 绑定不换对象）
    const Sequence *m_seq = nullptr;                   // 序列（非拥有；归 `UiModel` / core）
    int m_node = -1;             // 当前层（-1 = 根 / 整条序列）
    bool m_needsReset = false;   // 换来源 / 清空 ⇒ 下一次 refresh 必须整表重建
    int m_cursor = 0;                                  // 当前条目下标（进度行用）
    int m_selectedRow = -1;                            // 本屏选中（用户单击；-1 = 没选中）
    QString m_filter;                                  // 过滤词（只作用于当前这一层）
    QVector<int> m_chain;                              // 根 → 当前层的节点下标（`up` / `crumb` 用）
    QVariantList m_crumbs;
    bool m_canGoUp = false;
    QString m_rangeLabel;
    int m_browseRow = -1;
};

} // namespace app
