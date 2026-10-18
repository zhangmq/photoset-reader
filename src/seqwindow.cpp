#include "seqwindow.h"

#include "thumburl.h"

#include <QFileInfo>
#include <QLocale>
#include <QVariantMap>

namespace fetch {

// 浏览的一层：**唯一一份实现**（`app::BrowseModel` 每次换层 / 序列变了 / 游标变了就向它取一次）。
//   高亮语义：先按 `itemIndex == cursor` 命中，再按 `nodeIndex` 的子树范围命中。
BrowseLayer browseLayerOf(const Sequence &seq, int node, int cursor)
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
    for (int x = bnode; x >= 0 && x < tree.size(); x = tree.at(x).parent) out.chain.prepend(x);
    for (int i = 0; i < out.chain.size(); ++i)
        out.crumbs.append(QVariantMap{
            { QStringLiteral("label"), tree.at(out.chain.at(i)).label },
            { QStringLiteral("locked"), i == 0 } });

    out.canGoUp = (bnode >= 0 && tree.at(bnode).parent >= 0);
    out.rangeLabel = (bnode >= 0 && !tree.at(bnode).label.isEmpty())
                         ? tree.at(bnode).label
                         : QStringLiteral("本次打开");

    auto addImage = [&](int itemIndex) {
        const Sequence::Item &it = seq.at(itemIndex);
        BrowseModel::Row r;
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
        r.name = n.label;
        r.kind = n.kind;                       // 节点种类 = 同一个枚举
        r.nodeIndex = nodeIndex;
        r.subtreeFirst = seq.subtreeFirst(nodeIndex);
        r.subtreeCount = seq.subtreeCount(nodeIndex);
        r.sizeText = QStringLiteral("%1 张").arg(r.subtreeCount);
        //   压缩包容器：封面 = **包内第 0 页**（`thumbzip` 不带成员就是第 0 页，设计 11 §5.2）；
        //   文件夹没有"封面"这回事 ⇒ 保持空（界面显示文件夹字形）。
        if (n.kind == Sequence::Kind::Archive && n.itemCount > 0) {
            const Sequence::Item &first = seq.at(n.firstItem);
            if (first.loc.members.isEmpty())
                r.thumb = thumbUrlForFile(first.loc.diskPath);
            else if (first.loc.members.size() == 1)
                r.thumb = thumbUrlForArchiveMember(first.loc.diskPath, first.loc.members.first());
        }
        out.rows << r;
    };

    if (bnode < 0) {                                  // 没有树（单张图等）⇒ 整条序列当一层
        for (int i = 0; i < seq.count(); ++i) addImage(i);
    } else {
        const Sequence::TreeNode &n = tree.at(bnode);
        for (int i = n.firstItem; i < n.firstItem + n.itemCount && i < seq.count(); ++i)
            addImage(i);                              // ① 本层的**图**（自然序）
        // ② 本层的**子容器**（扫描顺序 = 自然序）—— ★ **只有元素才成为行**（L40）：
        //    空目录 / 无图压缩包 / 不受支持的压缩包 / **自己那一支还没扫完**的目录都不出现
        //    （判定条件只有 `Sequence::isElement()` 一份实现；这里只调它，不另写一遍）。
        for (int i = 0; i < tree.size(); ++i)
            if (tree.at(i).parent == bnode && seq.isElement(i)) addNode(i);
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
