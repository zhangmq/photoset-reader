#pragma once

#include <QAbstractListModel>

#include "sequence.h"     // ★ Sequence::Kind：行种类 = **一份枚举**
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>

// ══ 浏览界面的**子项模型**（层级浏览：一次只装"当前这一层"）══════════════
//
//   ★ 数据源 = **序列的文件树索引**（`Sequence::tree()`），**不查真实文件系统**
//     （2026-10-12 用户定案："浏览界面不再关心真实文件夹情况，完全以文件树索引为准"）。
//   ★ role 名沿用界面既有词表（`src/ui/browse/**` 是定稿 UI，直接用）：
//       name / sizeText / resText / kind / missing / thumb / depth
//   ★ `kind` = `Sequence::Kind`（**一份枚举**，多页文件也是"容器"⇒ 归 `Archive`）；
//     QML 拿到的是它的**字符串**（`image` / `archive` / `folder`），那份字符串**只在一个地方生成**
//     （`BrowseModel::kindText`，QML 边界的投影，不是第二份概念定义）。
//
//   行 = **当前节点的一个子项**：
//     · 子**容器**（目录 / 压缩包 / 多页文件）⇒ `nodeIndex >= 0`（可以"进入"）
//     · 子**图**（序列里的一条）⇒ `itemIndex >= 0`（可以"选中/打开"）
class BrowseModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        SizeTextRole,
        ResTextRole,
        KindRole,
        MissingRole,
        ThumbRole,
        DepthRole,
        ItemIndexRole,     // >= 0 ⇒ 序列里的图（-1 = 容器行）
        NodeIndexRole,     // >= 0 ⇒ 文件树里的容器（-1 = 图行）
    };

    struct Row {
        QString name;
        QString sizeText;
        QString resText;
        Sequence::Kind kind = Sequence::Kind::Image;   // ★ 一份枚举（QML 只认三个字符串）
        QString thumb;                  // 缩略图 URL（暂空 ⇒ 卡片显示占位）
        bool    missing = false;
        int     depth = 0;
        int     itemIndex = -1;
        int     nodeIndex = -1;
        // 容器行的**子树**区间（含自己 + 后代；用于"高亮哪一行"的派生；不是 QML role）
        int     subtreeFirst = 0;
        int     subtreeCount = 0;
    };

    // ★ QML 边界的那一份字符串（`image` / `archive` / `folder`）—— **唯一生成点**：
    //   它只是把 `Sequence::Kind` 投影成 QML 认的三个词，**不是第二份概念定义**。
    static QString kindText(Sequence::Kind k);

    explicit BrowseModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // 换一批行（**本对象线程**调用：驱动线程算好 → post 回来 → 在这里落地）
    void setRows(QVector<Row> rows);

    const Row *rowAt(int i) const;      // 越界返回 nullptr
    int count() const { return m_rows.size(); }

private:
    QVector<Row> m_rows;
};
