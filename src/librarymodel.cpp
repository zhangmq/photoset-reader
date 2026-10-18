#include "librarymodel.h"

#include "archive.h"
#include "imageprovider.h"      // 封面 id 的**唯一编码处**（thumbZipId / thumbPathId）
#include "librarycore.h"        // ★ T2c-2b-1：状态/计数的**内存态**来源（设计 14 §9.1/§9.3）
#include "sequence.h"           // ★ v5：位置派生（磁盘路径 + 容器成员链）

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUrl>
#include <QVariant>


LibraryModel::LibraryModel(library::Store *store, LibraryCore *core, QObject *parent)
    : QAbstractListModel(parent), m_store(store), m_core(core)
{
    // 变更通知：② 定时轮询 1 s（文档 §二）；① 窗口重新获得焦点时 QML 会再调 `pollNow`
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &LibraryModel::pollNow);
    m_timer.start();

    // ★★ core 的**内存态**推送（设计 14 §9.1 / §9.4）——**变化驱动**：
    //   每次数量变化发 `resetProgress`、状态变化发 `resetStateChanged`；
    //   只更新那一行、只在该行的显示值真的变了才 `dataChanged`（没变不刷）。
    {
        connect(m_core, &LibraryCore::resetProgress, this, [this](qint64 id, int discovered) {
            if (m_liveCount.value(id, -1) == discovered) return;   // 值没变 ⇒ 不发
            m_liveCount.insert(id, discovered);
            applyLiveState(id);
        });
        connect(m_core, &LibraryCore::resetStateChanged, this,
                [this](qint64 id, LibraryCore::ResetState state) {
                    // 新一轮重置入队 ⇒ 计数**从 0 重新计**（别把上一轮的数留在卡片上）
                    if (state == LibraryCore::ResetState::Queued) m_liveCount.insert(id, 0);
                    applyLiveState(id);
                    // 完成 ⇒ core 的落库 worker 已经把行**原子替换**过了 ⇒ 立即重读
                    //   （封面 / 阅读进度 / 图数都跟着变；失败/停止**不落库** ⇒ 不用重读）
                    if (state == LibraryCore::ResetState::Done) reload();
                });
        // ★★ 落库窗口（T2c-2b-2）：起落各一次 ⇒ 更新 `resetCommitting` role
        //   （同样走"值真变才 dataChanged"）
        connect(m_core, &LibraryCore::resetCommittingChanged, this,
                [this](qint64 id, bool) { applyLiveState(id); });
        // core 自己写库**不 bump data_version** ⇒ 它显式通知（增删改 / 版面）
        connect(m_core, &LibraryCore::collectionsChanged, this, &LibraryModel::reload);
    }

    refresh(true);
}

int LibraryModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant LibraryModel::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row &r = m_rows.at(index.row());
    switch (role) {
    case IdRole:              return QVariant::fromValue(r.id);
    case NameRole:            return r.name;
    case PathRole:            return r.path;
    case AddedAtRole:         return r.addedAt;
    case LastViewedAtRole:    return r.lastViewedAt;
    case ProgressValueRole:   return r.progressValue;
    case ProgressTextRole:    return r.progressText;
    case MissingRole:         return r.missing;
    case CoverRole:           return r.cover;
    case ScanStateRole:       return r.scanState;
    case ScannedItemsRole:    return r.scannedItems;
    case ResetCommittingRole: return r.resetCommitting;
    default:                  return {};
    }
}

QHash<int, QByteArray> LibraryModel::roleNames() const
{
    return {
        { IdRole,            "collectionId" },
        { NameRole,          "name" },
        { PathRole,          "path" },
        { AddedAtRole,       "addedAt" },
        { LastViewedAtRole,  "lastViewedAt" },
        { ProgressValueRole, "progressValue" },
        { ProgressTextRole,  "progressText" },
        { MissingRole,       "missing" },
        { CoverRole,         "cover" },
        { ScanStateRole,     "scanState" },
        { ScannedItemsRole,  "scannedItems" },
        { ResetCommittingRole, "resetCommitting" },
    };
}

QString LibraryModel::formatDate(qint64 secs)
{
    if (secs <= 0) return QStringLiteral("—");
    return QDateTime::fromSecsSinceEpoch(secs).toString(QStringLiteral("yyyy-MM-dd"));
}

// ══ ★★ core 的**内存状态** → 卡片认得的字面量（界面文案映射，设计 14 §9.1）════════
//   卡片（`src/ui/library/CollectionCard.qml`）只认这三个字面量：
//     "NotReady" / "ImageFound" ⇒ 「扫描中 · 已发现 N」+ 按钮变「取消扫描」
//     "ScanStoped"             ⇒ 「已停止 · N 张」
//     ""                       ⇒ 正常态（不显示扫描信息）
//   ⇒ 这里把 core 的 `ResetState` 映射进去，**QML 一行不改**（改动最小）；
//     不再有"进行中"落库 —— 这份状态**只活在内存**（`ResetQueue`：
//     Idle/Queued/Running/Done/Stopped/Failed）。
//   失败态（Failed）映射成 ""：失败只走**瞬时提示**（既有口径：卡片上没有失败态，
//   原因另可经 `LibraryCore::resetReason(id)` 取）。
QString LibraryModel::scanStateLiteral(qint64 id) const
{
    if (!m_core) return QString();
    switch (m_core->resetState(id)) {
    case LibraryCore::ResetState::Queued:  return QStringLiteral("NotReady");    // 排队中
    case LibraryCore::ResetState::Running: return QStringLiteral("ImageFound");  // 跑起来了
    case LibraryCore::ResetState::Stopped: return QStringLiteral("ScanStoped");  // 主动停止
    case LibraryCore::ResetState::Idle:
    case LibraryCore::ResetState::Done:
    case LibraryCore::ResetState::Failed:  return QString();
    }
    return QString();
}

// 单行（`id >= 0`）或全表（`id < 0`）按 **core 的内存态**重算 scanState / scannedItems /
//   resetCommitting，**只在该行显示值真的变了才 `dataChanged`**（变化驱动：没变不刷，设计 14 §9.4）。
//   `scannedItems` 的口径 = **已到达张数**（`resetProgress` 推过来的
//   `Sequence::discoveredCount()`）；没有本次重置记录的行 ⇒ 用上次 refresh 的库图数。
void LibraryModel::applyLiveState(qint64 id)
{
    bool any = false;
    for (int i = 0; i < m_rows.size(); ++i) {
        Row &r = m_rows[i];
        if (id >= 0 && r.id != id) continue;
        const QString st = scanStateLiteral(r.id);
        const int n = m_liveCount.contains(r.id) ? m_liveCount.value(r.id) : r.scannedItems;
        const bool committing = m_core && m_core->resetCommitting(r.id);
        if (r.scanState == st && r.scannedItems == n && r.resetCommitting == committing) continue;
        r.scanState = st;
        r.scannedItems = n;
        r.resetCommitting = committing;
        const QModelIndex idx = index(i);
        emit dataChanged(idx, idx);
        any = true;
    }
    if (any) emit changed();
}

qint64 LibraryModel::idByPath(const QString &path) const
{
    if (!m_store || !m_store->isOpen()) return -1;
    for (const library::Collection &c : m_store->collections())
        if (c.rootPath == path) return c.id;
    return -1;
}

void LibraryModel::reload()
{
    refresh(false);
}

// 封面 = **第一个能出图的条目**（按 `ord`）：
//   · 目录节点 → 跳过（目录不是封面）
//   · 图片     → 该文件的本地 URL
//   · 压缩包   → **包内第 0 页**（设计 11 §5.2）：扫描**展开了**压缩包（v4：压缩包成员
//     也是库行），所以封面按**容器自身的源路径**取（`ImageProvider::thumbZipId` 是
//     id 编码的**唯一实现**，别处不要手写百分号编码）；provider 第一次请求才真的打开包。
namespace {

// 库行 → 序列行（同一个形态；位置派生用）
} // namespace

QString LibraryModel::coverFor(const library::Collection &c) const
{
    // ★ 2026-10-19：不再"每张卡片全量拉行 + 全量定位" —— 只问"封面那一行 + 它的读图位置"
    //   （`Store::coverRow`：按 ord 找第一个**盘上**的非目录行，祖先链里没有压缩包）。
    Sequence::Row row;
    Sequence::Location loc;
    if (!m_store->coverRow(c.id, c.rootPath, library::rootIsArchive(c), &row, &loc)) return {};
    if (loc.diskPath.isEmpty() || !loc.members.isEmpty()) return {};
    if (row.kind == Sequence::Kind::Archive)
        return QStringLiteral("image://img/") + ImageProvider::thumbZipId(loc.diskPath);
    if (row.kind == Sequence::Kind::Image && loc.page <= 0)     // 普通图 / 多页的第 0 页
        return QStringLiteral("image://img/") + ImageProvider::thumbPathId(loc.diskPath);
    return {};
}

// 阅读进度文案 —— **只管阅读进度**。
//   ⚠️ 扫描状态的文案不在这里：卡片读 `scanState` / `scannedItems` 自己拼
//   （「扫描中 · 已发现 N」等，见 06 §3.5）。两处都写会分叉（§1.2 单一事实源）。
QString LibraryModel::progressTextFor(const library::Collection &c, double *value,
                                      int itemCount) const
{
    if (itemCount <= 0) {
        if (value) *value = 0.0;
        return QStringLiteral("—");
    }
    const int idx = m_store->itemIndexOfChain(c.id, Sequence::decodeChain(c.resumeKey),
                                             c.resumePage);
    if (value) *value = idx > 0 ? double(idx) / double(itemCount) : 0.0;
    const int seen = idx > 0 ? idx : 0;
    return QStringLiteral("%1 / %2").arg(seen).arg(itemCount);
}

void LibraryModel::pollNow()
{
    if (!m_store || !m_store->isOpen())
        return;
    // ★ v8：**只剩 `data_version` 比较** —— 陈旧自愈（旧检查点 ①窗口获得焦点 ②定时 1 s）
    //   已随 `scan_state` / `scan_updated_at` 列一起删除。
    //   `data_version` 只在**别的连接**提交时才变（自己写不变）→ 无变化就什么都不做。
    const int v = m_store->dataVersion();
    if (v == m_lastDataVersion)
        return;
    m_lastDataVersion = v;
    refresh();
}

void LibraryModel::refresh(bool force)
{
    if (!m_store || !m_store->isOpen())
        return;

    const QVector<library::Collection> cols = m_store->collections();
    // ★ v6：图数**现算**（一条 GROUP BY 覆盖所有集合；没有 item_count 列了）
    const QHash<qint64, int> counts = m_store->itemCounts();

    // 逐行比对**卡片上真正显示的东西**，没变的行不动（避免整表重置导致界面抖动）。
    //   ★ v6：**没有 `revision` 了** —— 同步只有 `data_version` 一条通道，
    //   所以这里只比"实际显示的字段"（状态 / 进度 / 图数 …）。
    auto sameRow = [](const Row &a, const Row &b) {
        return a.id == b.id
            && a.name == b.name && a.path == b.path
            && a.addedAt == b.addedAt && a.lastViewedAt == b.lastViewedAt
            && a.progressText == b.progressText && a.progressValue == b.progressValue
            && a.missing == b.missing && a.cover == b.cover
            && a.scanState == b.scanState && a.scannedItems == b.scannedItems
            && a.resetCommitting == b.resetCommitting;
    };

    QVector<Row> next;
    next.reserve(cols.size());
    for (const library::Collection &c : cols) {
        Row r;
        r.id = c.id;
        r.name = c.title.isEmpty() ? QFileInfo(c.rootPath).fileName() : c.title;
        r.path = c.rootPath;
        r.addedAt = formatDate(c.addedAt);
        r.lastViewedAt = formatDate(c.browsedAt);
        // ★★ T2c-2b-1：状态**从 core 的内存态映射**（库里没有状态列）
        r.scanState = scanStateLiteral(c.id);
        // ★★ 计数 = core 推过来的**已到达张数**（本次重置有记录时）；否则 = 库里现算的图数
        r.scannedItems = m_liveCount.contains(c.id) ? m_liveCount.value(c.id)
                                                    : counts.value(c.id, 0);
        // ★★ T2c-2b-2：落库窗口（初值；起落变化由 `resetCommittingChanged` 推）
        r.resetCommitting = m_core && m_core->resetCommitting(c.id);
        r.cover = coverFor(c);
        //   阅读进度文案**保持现状口径**：分母 = 库里的图数（`counts`），不是发现数
        r.progressText = progressTextFor(c, &r.progressValue, counts.value(c.id, 0));
        r.missing = !QFileInfo::exists(c.rootPath);
        next << r;
    }

    // 集合没了的 id ⇒ 顺手丢掉它的内存计数（不留悬挂项）
    for (auto it = m_liveCount.begin(); it != m_liveCount.end();) {
        bool found = false;
        for (const Row &r : next) if (r.id == it.key()) { found = true; break; }
        if (found) ++it;
        else       it = m_liveCount.erase(it);
    }

    if (force || next.size() != m_rows.size()) {
        beginResetModel();
        m_rows = next;
        endResetModel();
        emit countChanged();
        emit changed();
        return;
    }

    bool any = false;
    for (int i = 0; i < next.size(); ++i) {
        if (!sameRow(next.at(i), m_rows.at(i))) {
            m_rows[i] = next.at(i);
            const QModelIndex idx = index(i);
            emit dataChanged(idx, idx);
            any = true;
        }
    }
    if (any)
        emit changed();
}

void LibraryModel::addCollection(const QString &path, Sequence::Kind kind)
{
    // ★ T2c-2b-1：增删改**走 core 的转发**（core 自己写库不 bump data_version ⇒
    //   成功后它显式发 `collectionsChanged()`，上面那个连接会 `reload()`）。
    //   ★ T2c-2b-2：`m_core == nullptr` 的旧调试分支已删（core 必需）。
    QString err;
    if (!m_core->addCollection(path, kind, &err)) {
        const QString why = QStringLiteral("加入图集失败：%1")
                                .arg(err.isEmpty() ? QStringLiteral("未知原因") : err);
        qWarning().noquote() << QStringLiteral("[library] %1").arg(why);
        emit libraryError(why);
        reload();
        return;
    }
    const qint64 id = idByPath(path);        // core 已经让我们 reload 过了
    if (id <= 0) reload();                   // 兜底（极端情况下同步通知没跑到）
    // 加入成功 → 通知上层（ViewModel）**自动重置**（用户定案：加入图集后自动开扫）
    if (id > 0) emit collectionAdded(id);
}

void LibraryModel::removeCollection(int row)
{
    if (row < 0 || row >= m_rows.size()) return;
    const qint64 id = m_rows.at(row).id;
    QString err;
    if (!m_core->removeCollection(id, &err))
        emit libraryError(QStringLiteral("移除图集失败：%1")
                              .arg(err.isEmpty() ? QStringLiteral("未知原因") : err));
    // collectionsChanged ⇒ reload
}

void LibraryModel::renameCollection(int row, const QString &title)
{
    if (row < 0 || row >= m_rows.size()) return;
    const qint64 id = m_rows.at(row).id;
    if (!m_core->renameCollection(id, title))
        emit libraryError(QStringLiteral("重命名失败"));
    // collectionsChanged ⇒ reload
}
