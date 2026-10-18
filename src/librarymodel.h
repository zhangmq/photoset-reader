#pragma once

// ══ 图集库列表模型（QAbstractListModel）════════════════════════
//   设计：docs/design/10-数据与多进程架构.md；重置状态：设计 14 §9.1
//
//   · **roles 沿用原占位模型的命名风格**：
//       name / path / addedAt / lastViewedAt / progressValue / progressText / missing / cover
//     `scanState` / `scannedItems`（★ T2c-2b-1：**改内存态，来自 `LibraryCore`**）/
//     `resetCommitting`（★ T2c-2b-2：落库窗口 —— 卡片据此**禁用「取消扫描」**）——
//     卡片上的文案由卡片拼（模型只给这几个 role）。
//   · **变更同步**：轮询 `PRAGMA data_version`（定时 1s + 窗口重新获得焦点时），
//     值变了才重读；**行比较只比真正显示的字段**（★ v6：没有 `revision` 了）；
//     core 自己的写库**不 bump data_version** ⇒ 它显式发 `collectionsChanged()`，
//     本模型接住后 `reload()`（设计 14 §9.5）。
//   · ★★ **状态与计数不是库里的列**（设计 14 §9.1）：
//       状态 = `LibraryCore::resetState(id)`（内存态）；计数 = **已到达张数**
//       （`resetProgress` 推过来的 `Sequence::discoveredCount()`）；
//       落库窗口 = `LibraryCore::resetCommitting(id)`（内存态）。
//       ★ v8：`scan_state` / `scan_updated_at` / `content_changed` 列与陈旧自愈**已删**。

#include "librarystore.h"

#include <QAbstractListModel>
#include <QHash>
#include <QTimer>

class LibraryCore;              // 重置队列 / 内存状态（设计 14 §9.3）—— 只前向声明

class LibraryModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole, PathRole, AddedAtRole, LastViewedAtRole,
        ProgressValueRole, ProgressTextRole, MissingRole, CoverRole, ScanStateRole,
        ScannedItemsRole,     // ★ T2c-2b-1：来自 core 的**已到达张数**（内存态）
        ResetCommittingRole,  // ★ T2c-2b-2：落库窗口（true ⇒ 卡片禁用「取消扫描」）
    };

    // ★ T2c-2b-2：`core` **必需**（图集动作/状态全在 core；旧调试路径的 `core == nullptr`
    //   分支已随旧扫描路径删除）。唯一调用者 = `UiModel`（`tests/` 里没有直接构造它的地方）。
    explicit LibraryModel(library::Store *store, LibraryCore *core, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // 轮询（定时器 + QML 在窗口获得焦点时调 pollNow）；`reload()` = core 显式通知后刷新
    //   ★ v8：**只剩 `data_version` 比较** —— 陈旧自愈（旧检查点）已删。
    Q_INVOKABLE void pollNow();
    // core 显式改了库（增删改：它自己写库不 bump data_version）⇒ 立即重读一次
    Q_INVOKABLE void reload();

    // 界面动作（**走 core 的转发**）
    Q_INVOKABLE void addCollection(const QString &path, Sequence::Kind kind);
    Q_INVOKABLE void removeCollection(int row);
    Q_INVOKABLE void renameCollection(int row, const QString &title);

signals:
    void countChanged();
    void changed();          // 内容有更新（QML 可据此做轻提示）
    // 新集合已入库（**加入成功**才发）—— ViewModel 据此**自动重置**（用户定案：加入图集后自动开扫）
    void collectionAdded(qint64 id);
    // ★ 库写失败等 ⇒ 瞬时提示（接 UiModel::notify；**不写入磁盘**）
    void libraryError(const QString &text);

private:
    struct Row {
        qint64  id = -1;
        QString name, path, addedAt, lastViewedAt, progressText, cover, scanState;
        int     scannedItems = 0;      // ★ T2c-2b-1：core 的已到达张数（无记录时 = 库图数）
        double  progressValue = 0.0;
        bool    missing = false;
        bool    resetCommitting = false;   // ★ T2c-2b-2：落库窗口（卡片禁用取消）
    };

    void refresh(bool force = false);
    static QString formatDate(qint64 secs);
    QString coverFor(const library::Collection &c) const;
    QString progressTextFor(const library::Collection &c, double *value, int itemCount) const;
    // core 状态 → 卡片认得的字面量（**界面文案映射**，QML 零改动，见 .cpp）
    QString scanStateLiteral(qint64 id) const;
    // 单行变化 ⇒ 只发该行的 dataChanged（**变化驱动**：值没变不发）
    void applyLiveState(qint64 id);
    qint64 idByPath(const QString &path) const;

    library::Store *m_store = nullptr;
    LibraryCore    *m_core = nullptr;
    QVector<Row>    m_rows;
    QTimer          m_timer;
    int             m_lastDataVersion = -1;
    // core 推过来的**内存态**（每次数量变化就更新；`scannedItems` 的口径）
    QHash<qint64, int> m_liveCount;
};
