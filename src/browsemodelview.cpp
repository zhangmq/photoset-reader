#include "browsemodelview.h"

#include "seqwindow.h"
#include "sequence.h"

namespace app {

BrowseModel::BrowseModel(QObject *parent) : QObject(parent) {}

void BrowseModel::setSequence(const Sequence *seq)
{
    m_seq = seq;
    m_needsReset = true;          // 换了来源 ⇒ 上一次的行全部作废
    refresh();
}

void BrowseModel::setCursor(int itemIndex)
{
    if (m_cursor == itemIndex) return;
    m_cursor = itemIndex;
    refresh();
}

void BrowseModel::setNode(int node)
{
    m_node = node;
    refresh();
}

// 这一层 = **一次**向唯一实现取（`fetch::browseLayerOf`：从序列枚举 + 面包屑 + 高亮）
void BrowseModel::refresh()
{
    if (!m_seq) {
        m_node = -1;
        m_chain.clear();
        if (m_rows.count() > 0) m_rows.setRows({});
        m_crumbs.clear();
        m_canGoUp = false;
        m_rangeLabel.clear();
        m_browseRow = -1;
        emit changed();
        return;
    }
    const int prevNode = m_node;
    const fetch::BrowseLayer layer = fetch::browseLayerOf(*m_seq, m_node, m_cursor);
    const bool newLayer = m_needsReset || layer.node != prevNode;
    m_needsReset = false;
    m_node = layer.node;                  // 取回来的是**解析后**的层（陈旧 / 越界 ⇒ 根）
    m_chain = layer.chain;
    // ★ 换层 / 换来源才整表重建；**同一层又长了**走增量（只插入新增的行 / 只刷变化的字段）
    //   —— 否则扫描期间每来一批就把列表推倒重来，滚动位置与高亮全丢（L10 / L14）。
    if (newLayer) m_rows.setRows(layer.rows);
    else          m_rows.patchRows(layer.rows);
    m_crumbs = layer.crumbs;
    m_canGoUp = layer.canGoUp;
    m_rangeLabel = layer.rangeLabel;
    m_browseRow = layer.row;
    emit changed();
}

void BrowseModel::clear()
{
    m_needsReset = true;
    if (m_rows.count() > 0) m_rows.setRows({});
    m_seq = nullptr;
    m_node = -1;
    m_cursor = 0;
    m_chain.clear();
    m_crumbs.clear();
    m_canGoUp = false;
    m_rangeLabel.clear();
    m_browseRow = -1;
    emit changed();
}

// 容器 ⇒ 进入那一层；图 ⇒ 选中（移动那个唯一的游标 —— 归 `UiModel` 执行）
bool BrowseModel::activate(int row)
{
    const ::BrowseModel::Row *r = m_rows.rowAt(row);
    if (!r) return false;
    if (r->nodeIndex >= 0) {
        setNode(r->nodeIndex);
        emit layerChanged(1);
        return true;
    }
    if (r->itemIndex >= 0) {
        emit rowSelected(r->itemIndex);
        return true;
    }
    return false;
}

bool BrowseModel::open(int row)
{
    const ::BrowseModel::Row *r = m_rows.rowAt(row);
    if (!r) return false;
    if (r->nodeIndex >= 0) {                        // 容器：与 activate 同义（进入那一层）
        setNode(r->nodeIndex);
        emit layerChanged(1);
        return true;
    }
    if (r->itemIndex >= 0) {
        emit rowSelected(r->itemIndex);
        emit openRequested(r->itemIndex);
        return true;
    }
    return false;
}

bool BrowseModel::up()
{
    // 祖先链 = 根 → 当前层（含两端）；链上不足两段 ⇒ 已是"打开时的根" ⇒ 到头（界面自己禁用）
    if (m_chain.size() < 2) return false;
    setNode(m_chain.at(m_chain.size() - 2));
    emit layerChanged(0);
    return true;
}

bool BrowseModel::crumb(int index)
{
    if (index < 0 || index >= m_chain.size()) return false;
    if (m_chain.at(index) == m_node) return false;
    setNode(m_chain.at(index));
    emit layerChanged(0);
    return true;
}

bool BrowseModel::isContainerRow(int row) const
{
    const ::BrowseModel::Row *r = m_rows.rowAt(row);
    return r && r->nodeIndex >= 0;
}

} // namespace app
