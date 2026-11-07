#pragma once

// ══ 收藏夹列表（`.prsave` 那份目录）════════════════════════════════════════
//   设计 20 §7 / §8。要点：
//     · **打开列表只列文件名**（不读内容）；`title` 从文件名反推（`<title>-<hash8>.prsave`）；
//     · 行内容（源路径 / N/M / 百分比）**按需读**：只有 `data()` 被问到时才读文件，带一个 32 条的小缓存；
//     · 过滤只按**名称**（文件名里的 title），不读内容；
//     · 目录用 `QFileSystemWatcher` 监控**一层**：变了就重列（300 ms 合并）。
//   ★ 单一事实源 = 磁盘上的文件：这里**不存任何状态**，删文件 = 删记录。

#include <QAbstractListModel>
#include <QFileSystemWatcher>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTimer>

class ProgressListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        TitleRole = Qt::UserRole + 1,
        FileRole,
        SourcePathRole,
        DoneRole,
        TotalRole,
        RatioRole,      // done / total（0–1；读不到 = 0）
        ErrorRole       // 非空 = 这个文件读不了（行里显示"无法读取"）
    };

    explicit ProgressListModel(QObject *parent = nullptr);

    int      rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_PROPERTY(int     count     READ count     NOTIFY countChanged)
    Q_PROPERTY(QString filter    READ filter    WRITE setFilter    NOTIFY filterChanged)
    Q_PROPERTY(QString directory READ directory                 NOTIFY directoryChanged)

    int     count() const { return m_rows.size(); }
    QString filter() const { return m_filter; }
    Q_INVOKABLE void setFilter(const QString &text);
    QString directory() const { return m_dir; }
    void    setDirectory(const QString &dir);

    Q_INVOKABLE void    reload();
    Q_INVOKABLE QString fileAt(int row) const;

Q_SIGNALS:
    void countChanged();
    void filterChanged();
    void directoryChanged();

private:
    struct Row {
        QString file;      // 绝对路径
        QString title;     // 文件名反推的显示名
    };
    struct Meta {
        bool    ok = false;
        QString sourcePath;
        int     done = 0;
        int     total = 0;
        QString error;
    };

    //   ★ `data()` 是 const ⇒ 缓存与读文件都放在 mutable 里（只在 GUI 线程碰）
    const Meta &metaFor(const QString &file) const;
    void        rebuildRows();
    void        scheduleReload();

    QString          m_dir;
    QString            m_filter;
    QStringList        m_files;      // 全部 `.prsave`（已排序）
    QVector<Row>       m_rows;       // 过滤后的行


    mutable QHash<QString, Meta> m_meta;      // 懒读缓存（FIFO 32 条）
    mutable QList<QString>       m_metaOrder;
};
