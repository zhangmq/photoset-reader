#include "browsemodelview.h"

#include "seqwindow.h"
#include "sequence.h"

namespace app {

BrowseModel::BrowseModel(QObject *parent) : QObject(parent) {}

// 行**只在内容真的变了**时才重设（否则每换一屏 / 每动一次游标都重置模型 =
// 界面重新建卡片，白费）
static bool sameRows(const QVector<::BrowseModel::Row> &a, const ::BrowseModel &b)
{
    if (a.size() != b.count()) return false;
    for (int i = 0; i < a.size(); ++i) {
        const ::BrowseModel::Row *r = b.rowAt(i);
        if (!r) return false;
        const ::BrowseModel::Row &x = a.at(i);
        if (r->name != x.name || r->kind != x.kind || r->missing != x.missing
            || r->sizeText != x.sizeText || r->resText != x.resText || r->thumb != x.thumb
            || r->itemIndex != x.itemIndex || r->nodeIndex != x.nodeIndex)
            return false;
    }
    return true;
}

void BrowseModel::setSequence(const Sequence *seq)
{
    m_seq = seq;
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
    const fetch::BrowseLayer layer = fetch::browseLayerOf(*m_seq, m_node, m_cursor);
    m_node = layer.node;                  // 取回来的是**解析后**的层（陈旧 / 越界 ⇒ 根）
    m_chain = layer.chain;
    if (!sameRows(layer.rows, m_rows)) m_rows.setRows(layer.rows);
    m_crumbs = layer.crumbs;
    m_canGoUp = layer.canGoUp;
    m_rangeLabel = layer.rangeLabel;
    m_browseRow = layer.row;
    emit changed();
}

void BrowseModel::clear()
{
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
