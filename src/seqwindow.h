#pragma once

// ══ 浏览的一层（**直接从序列取**）════════════════════════════════════════════
//   设计 12 §8.9 口径 7：浏览属于浏览场景 ⇒ 它的该层行 / 面包屑 / 能否上一级 / 范围标签 /
//   高亮由 `app::BrowseModel`（QML 的 `ui.browse`）每次需要时从这里取一次。
//
//   ★ **算法只有这一份**：从序列枚举这一层 + 面包屑 + 高亮。
//   ★ 高亮语义：当前层里含"当前条目"的那一行（先按 `itemIndex == cursor` 命中，
//     再按 `nodeIndex` 的子树范围命中）。
//   ★ 行的枚举**只含元素**（L40）：空目录 / 无图压缩包 / 尚未扫完的目录都不出现 ——
//     判定条件只有 `Sequence::isElement()` 一份（这里只调它）。

#include "browsemodel.h"
#include "sequence.h"

#include <QString>
#include <QVariantList>
#include <QVector>

namespace fetch {

struct BrowseLayer {
    int node = -1;                     // 解析后的层（-1 = 无树 ⇒ 整条序列当一层）
    QVector<BrowseModel::Row> rows;    // 该层的行：本层的**图**（自然序）→ 本层的**子容器**
    QVector<int> chain;                // 根 → 当前层的节点下标（面包屑 / 上一级用它；无树 ⇒ 空）
    QVariantList crumbs;               // [{label, locked, siblings:[{label,node}]}]（首段 = 锁死的根）
    bool canGoUp = false;
    QString rangeLabel;
    int row = -1;                      // 含"当前条目"的那一行（-1 = 这一层没有它）
};

//   `node` = 想取的层（-1 = 根；越界 / 陈旧的层下标与"没定层"同义 ⇒ 落到根）。
//   `cursor` = 当前条目下标（**由调用方给**：浏览模型持有它，它不读 viewer）。
//   `filter` = 过滤词（空 = 不过滤）：**只筛当前这一层**，按每行自己的名字匹配（大小写不敏感）。
BrowseLayer browseLayerOf(const Sequence &seq, int node, int cursor,
                          const QString &filter = QString());

} // namespace fetch
