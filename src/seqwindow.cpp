#include "seqwindow.h"

#include "thumburl.h"

#include <QFileInfo>
#include <QLocale>
#include <QVariantMap>

#include <algorithm>

namespace fetch {

// 浏览的一层：**唯一一份实现**（`app::BrowseModel` 每次换层 / 序列变了 / 游标变了就向它取一次）。
//   高亮语义：先按 `itemIndex == cursor` 命中，再按 `nodeIndex` 的子树范围命中。
BrowseLayer browseLayerOf(const Sequence &seq, int node, int cursor, const QString &filter)
{
    BrowseLayer out;
    const QVector<Sequence::TreeNode> &tree = seq.tree();
    int bnode = node;
    if (bnode >= tree.size()) bnode = -1;        // 陈旧 / 越界的层下标 ⇒ 与"没定层"同义
    if (bnode < 0) {
        for (int i = 0; i < tree.size(); ++i)
            if (tree.at(i).parent < 0) { bnode = i; break; }
    }
    out.node = bnode;

    // 面包屑：从根到当前层（首段 = **锁死的根**，见 Breadcrumb 契约 [{label, locked}]）
    //   ★ 首段是"打开时的根"这个**路径**（用户 2026-10-24），不是根节点名
    for (int x = bnode; x >= 0 && x < tree.size(); x = tree.at(x).parent) out.chain.prepend(x);
    for (int i = 0; i < out.chain.size(); ++i) {
        QString label;
        if (i == 0) {
            label = seq.rootPath();
            if (label.isEmpty()) label = tree.at(out.chain.at(i)).label;
        } else {
            label = tree.at(out.chain.at(i)).label;
        }
        // ★ 同层其他容器（供面包屑下拉，用户 2026-10-24）：= 父节点的**元素**子容器，按 ord 排。
        //   根段没有兄弟（root 之上没有层）。
        QVariantList siblings;
        if (i > 0) {
            const int parent = tree.at(out.chain.at(i)).parent;
            for (int k = 0; k < tree.size(); ++k)
                if (tree.at(k).parent == parent && seq.isElement(k))
                    siblings << QVariantMap{ { QStringLiteral("label"), tree.at(k).label },
                                             { QStringLiteral("node"), k },
                                             { QStringLiteral("ord"), tree.at(k).ord } };
            std::sort(siblings.begin(), siblings.end(),
                      [](const QVariant &a, const QVariant &b) {
                          return a.toMap().value(QStringLiteral("ord")).toInt()
                               < b.toMap().value(QStringLiteral("ord")).toInt();
                      });
        }
        out.crumbs.append(QVariantMap{
            { QStringLiteral("label"), label },
            { QStringLiteral("locked"), i == 0 },
            { QStringLiteral("node"), out.chain.at(i) },
            { QStringLiteral("siblings"), siblings } });
    }

    out.canGoUp = (bnode >= 0 && tree.at(bnode).parent >= 0);
    out.rangeLabel = (bnode >= 0 && !tree.at(bnode).label.isEmpty())
                         ? tree.at(bnode).label
                         : QStringLiteral("本次打开");

    auto addImage = [&](int itemIndex) {
        const Sequence::Item &it = seq.at(itemIndex);
        BrowseModel::Row r;
        // ★ 元素路径（key）：容器链 + 自己的名字；**页行的 label 是空串**（页号由 `loc.page` 表达）
        //   ⇒ 同一多页文件的每一页路径相同，必须补页号才能区分（sequence.cpp:1209-1214）
        r.key = Sequence::encodeChain(seq.chainAt(itemIndex));
        if (it.label.isEmpty()) r.key += QStringLiteral("#%1").arg(it.loc.page);
        r.name = it.displayName.isEmpty() ? it.label : it.displayName;
        r.kind = Sequence::Kind::Image;
        r.missing = it.missing;
        r.itemIndex = itemIndex;
        r.resText = it.size.isValid()
                        ? QStringLiteral("%1×%2").arg(it.size.width()).arg(it.size.height())
                        : QString();
        const QFileInfo fi(it.loc.diskPath);
        if (it.loc.members.isEmpty() && fi.exists())
            r.sizeText = QLocale().formattedDataSize(fi.size());   // 字节数（压缩包内条目不报）
        //   缩略图：普通文件走 thumbpath；压缩包内成员走 thumbzip（**格式的唯一来源**）；
        //   嵌套容器（成员链 > 1）provider 走不通 ⇒ 不给缩略图（回落到字形）。
        if (it.loc.members.isEmpty())
            r.thumb = thumbUrlForFile(it.loc.diskPath);
        else if (it.loc.members.size() == 1)
            r.thumb = thumbUrlForArchiveMember(it.loc.diskPath, it.loc.members.first());
        out.rows << r;
    };
    auto addNode = [&](int nodeIndex) {
        const Sequence::TreeNode &n = tree.at(nodeIndex);
        BrowseModel::Row r;
        // ★ 元素路径（key）：与 `Sequence::chainAt` 同一套写法（沿 parent 上溯、**不含根**）
        QStringList chain;
        for (int x = nodeIndex; x >= 0 && x < tree.size() && tree.at(x).parent >= 0;
             x = tree.at(x).parent)
            chain.prepend(tree.at(x).label);
        r.key = Sequence::encodeChain(chain);
        r.name = n.label;
        r.kind = n.kind;                       // 节点种类 = 同一个枚举
        r.nodeIndex = nodeIndex;
        r.subtreeFirst = seq.subtreeFirst(nodeIndex);
        r.subtreeCount = seq.subtreeCount(nodeIndex);
        r.sizeText = QStringLiteral("%1 张").arg(r.subtreeCount);
        //   压缩包容器：封面 = **包内第 0 页**（`thumbzip` 不带成员就是第 0 页，设计 11 §5.2）；
        //   文件夹没有"封面"这回事 ⇒ 保持空（界面显示文件夹字形）。
        if (n.kind == Sequence::Kind::Archive && n.itemCount > 0) {
            QVector<int> own;                          // 直接条目 = 链（不是连续区间）
            seq.directItems(nodeIndex, &own);
            if (!own.isEmpty()) {
                const Sequence::Item &first = seq.at(own.first());
                if (first.loc.members.isEmpty())
                    r.thumb = thumbUrlForFile(first.loc.diskPath);
                else if (first.loc.members.size() == 1)
                    r.thumb = thumbUrlForArchiveMember(first.loc.diskPath,
                                                       first.loc.members.first());
            }
        }
        out.rows << r;
    };

    if (bnode < 0) {                                  // 没有树（单张图等）⇒ 整条序列当一层
        for (int i = 0; i < seq.count(); ++i) addImage(i);
    } else {
        // ★★ 这一层的行 = **自己的条目 + 直接子容器**，按 `ord`（= 序列顺序 = 先根遍历序）排。
        //   ⇒ 图集常见的"上层放对应文件夹的封面图"就落在它那个文件夹的**前一行**。
        //   ⚠️ **不能**再假设"自己的图是连续区间"：先根遍历下它们被子容器切开。
        struct LayerRow {
            int ord = -1;
            int itemIndex = -1;                        // ≥ 0 = 自己的条目
            int nodeIndex = -1;                        // ≥ 0 = 直接子容器
        };
        QVector<LayerRow> merged;
        QVector<int> own;
        seq.directItems(bnode, &own);
        for (int k : own) merged << LayerRow{ seq.at(k).ord, k, -1 };
        // 子容器—— ★ **只有元素才成为行**（L40）：空目录 / 无图压缩包 / 不受支持的压缩包 /
        //   **自己那一支还没扫完**的目录都不出现（判定条件只有 `Sequence::isElement()`
        //   一份实现；这里只调它，不另写一遍）。
        for (int i = 0; i < tree.size(); ++i)
            if (tree.at(i).parent == bnode && seq.isElement(i))
                merged << LayerRow{ tree.at(i).ord, -1, i };
        std::stable_sort(merged.begin(), merged.end(),
                         [](const LayerRow &a, const LayerRow &b) { return a.ord < b.ord; });
        for (const LayerRow &lr : merged) {
            if (lr.itemIndex >= 0) addImage(lr.itemIndex);
            else                   addNode(lr.nodeIndex);
        }
    }

    // ★ 过滤（用户 2026-10-24）：**只筛当前这一层**，按每行自己的名字匹配；序列本身不动。
    //   容器行也参与（否则一过滤文件夹全没了、没法往下点）。
    const QString needle = filter.trimmed();
    if (!needle.isEmpty()) {
        QVector<BrowseModel::Row> kept;
        kept.reserve(out.rows.size());
        for (const BrowseModel::Row &r : out.rows)
            if (r.name.contains(needle, Qt::CaseInsensitive)) kept << r;
        out.rows = kept;
    }

    // 高亮行 = 当前层里含"当前条目"的那一行（**派生**；唯一一处）
    for (int i = 0; i < out.rows.size(); ++i) {
        const BrowseModel::Row &r = out.rows.at(i);
        if (r.itemIndex == cursor) { out.row = i; break; }
        if (r.nodeIndex >= 0 && r.subtreeCount > 0
            && cursor >= r.subtreeFirst && cursor < r.subtreeFirst + r.subtreeCount) {
            out.row = i;
            break;
        }
    }
    return out;
}

} // namespace fetch
