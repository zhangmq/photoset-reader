#include "librarystore.h"

#include "archive.h"           // 来源根是不是容器（唯一实现里要探测压缩包）
#include "sequence.h"          // ★ v5：`itemIndexOfChain` 用**同一条序列**沿链 walk（单一事实源）

#include <QCoreApplication>
#include <QDateTime>
#include <algorithm>
#include <QDebug>
#include <QDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QThread>
#include <QVariant>

namespace library {

// ── 行形态转换（唯一一份，见头文件）────────────────────────────────
Item toItem(const Sequence::Row &r)
{
    Item x;
    x.ord = r.ord;
    x.parentOrd = r.parentOrd;
    x.kind = r.kind;
    x.label = r.label;
    x.pageIndex = r.pageIndex;
    x.size = r.size;
    x.mtime = r.mtime;
    x.width = r.width;
    x.height = r.height;
    x.firstIndex = r.firstIndex;
    x.lastIndex = r.lastIndex;
    return x;
}

Sequence::Row toRow(const Item &r)
{
    Sequence::Row x;
    x.ord = r.ord;
    x.parentOrd = r.parentOrd;
    x.kind = r.kind;
    x.label = r.label;
    x.pageIndex = r.pageIndex;
    x.size = r.size;
    x.mtime = r.mtime;
    x.width = r.width;
    x.height = r.height;
    x.firstIndex = r.firstIndex;
    x.lastIndex = r.lastIndex;
    return x;
}

bool rootIsArchive(const Collection &c)
{
    if (c.kind == Sequence::Kind::Archive) return true;
    return QFileInfo(c.rootPath).isFile() && ArchiveSource::isArchive(c.rootPath);
}

namespace {
// ★ v8（T2c-2b-2）：删除 `scan_state` / `scan_updated_at` / `content_changed` 三列与
//   `action` 表 —— 旧"扫描任务"路径（`ScanController`）已整体作废。
//   版本不符 ⇒ 既有重建路径删库重建（开发期不做迁移，用户 2026-10-13 定案）。
constexpr int kSchemaVersion = 10;

// 绑 TEXT 列：把 Qt 的 **null `QString()`** 规范成空串（否则 NOT NULL 列会拒绝写入）
QVariant textOrEmpty(const QString &s)
{
    return s.isNull() ? QVariant(QString(QLatin1String(""))) : QVariant(s);
}

qint64 nowSecs() { return QDateTime::currentSecsSinceEpoch(); }

// 每个线程一个连接（QtSql/SQLite 的规矩）：连接名带上线程标识
QString connectionNameForCurrentThread()
{
    return QStringLiteral("photoset-library-%1")
            .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16);
}
} // namespace

Store::Store() = default;

Store::~Store()
{
    if (!m_connName.isEmpty()) {
        {
            QSqlDatabase db = QSqlDatabase::database(m_connName, false);
            if (db.isOpen()) db.close();
        }
        QSqlDatabase::removeDatabase(m_connName);
    }
}

QString Store::databasePath()
{
    // 开发期：一个环境变量指定整个配置目录（部署测试才用真路径 ~/.config/photoset-reader）
    const QByteArray dirOverride = qgetenv("PHOTOSET_CONFIG_DIR");
    if (!dirOverride.isEmpty())
        return QString::fromLocal8Bit(dirOverride) + QStringLiteral("/library.db");
    // 自检/测试：也可以只覆盖库文件本身
    const QByteArray override = qgetenv("PHOTOSET_LIBRARY_DB");
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return dir + QStringLiteral("/library.db");
}

void Store::setError(const QString &what)
{
    m_error = what;
    if (!m_quietErrors)
        qWarning().noquote() << QStringLiteral("[library] %1").arg(what);
}

bool Store::exec(const QString &sql)
{
    QSqlQuery q(QSqlDatabase::database(m_connName));
    if (!q.exec(sql)) {
        setError(QStringLiteral("SQL 失败: %1 — %2").arg(q.lastError().text(), sql.left(80)));
        return false;
    }
    return true;
}

bool Store::open(QString *error, bool quiet)
{
    if (m_open) return true;
    m_quietErrors = quiet;                 // 只在打开阶段静音（失败原因照样写进 error）
    const bool ok = openImpl(error);
    m_quietErrors = false;
    return ok;
}

bool Store::openImpl(QString *error)
{
    const QString path = databasePath();
    const QString dir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dir)) {
        setError(QStringLiteral("无法创建目录: %1").arg(dir));
        if (error) *error = m_error;
        return false;
    }

    m_connName = connectionNameForCurrentThread();
    QSqlDatabase db = QSqlDatabase::contains(m_connName)
                        ? QSqlDatabase::database(m_connName, false)
                        : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connName);
    db.setDatabaseName(path);
    if (!db.open()) {
        setError(QStringLiteral("打开库失败: %1").arg(db.lastError().text()));
        if (error) *error = m_error;
        return false;
    }

    // ★ **开发期不做迁移**（用户 2026-10-13 定案）：库版本不符 ⇒ **直接重建库文件**
    //   （删 `library.db` + `-wal` / `-shm` 再建）。v3 那套 `ensureColumn` 迁移**已删**。
    const int version = [&] {
        QSqlQuery q(db);
        if (q.exec(QStringLiteral("PRAGMA user_version")) && q.next()) return q.value(0).toInt();
        return 0;
    }();
    if (version != kSchemaVersion) {
        const QString why = version == 0 ? QStringLiteral("空库/新库")
                                         : QStringLiteral("旧版本 v%1").arg(version);
        qInfo().noquote() << QStringLiteral("[library] 库版本不符（%1，期望 v%2）⇒ 删库重建 %3")
                                 .arg(why).arg(kSchemaVersion).arg(path);
        db.close();
        for (const QString &suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")})
            QFile::remove(path + suffix);
        db.setDatabaseName(path);
        if (!db.open()) {
            setError(QStringLiteral("重建库失败: %1").arg(db.lastError().text()));
            if (error) *error = m_error;
            return false;
        }
    }

    // ★ 缩略图淘汰后要**真的还空间**（设计 10 §八 / 11 §五）。
    //   ⚠️ 必须排在 `journal_mode=WAL` **之前**，也必须排在建表之前：
    //   切 WAL 会先落地库头，之后再设 auto_vacuum 就无效了（实测 sqlite 3.45：返回 0）。
    if (!exec(QStringLiteral("PRAGMA auto_vacuum=INCREMENTAL"))) { if (error) *error = m_error; return false; }

    // 多进程：WAL（多读 + 单写、崩溃安全）+ 写争用等待
    if (!exec(QStringLiteral("PRAGMA journal_mode=WAL"))) { if (error) *error = m_error; return false; }
    if (!exec(QStringLiteral("PRAGMA busy_timeout=5000"))) { if (error) *error = m_error; return false; }
    if (!exec(QStringLiteral("PRAGMA foreign_keys=ON"))) { if (error) *error = m_error; return false; }
    if (!exec(QStringLiteral("PRAGMA synchronous=NORMAL"))) { if (error) *error = m_error; return false; }

    {
        // 表结构见设计文档 §七（集合 / 条目 / 缩略图）
        // ★ v5 的 `item` = **已写入磁盘的序列**：`ord`（节点与叶子共用）+ `parent_ord` + `label`
        //   —— **没有 key / parent_key / rel_path**（写入磁盘不拼任何字符串）。
        const QStringList ddl = {
            // ★ v8：collection **从简** —— 只有身份 / 位置 / 阅读进度 / 版面（见头文件）
            QStringLiteral(R"(
                CREATE TABLE IF NOT EXISTS collection (
                  id             INTEGER PRIMARY KEY,
                  root_path      TEXT UNIQUE NOT NULL,
                  kind           INTEGER NOT NULL,     -- Sequence::Kind 的值
                  title          TEXT,
                  added_at       INTEGER NOT NULL DEFAULT 0,
                  browsed_at     INTEGER NOT NULL DEFAULT 0,
                  resume_key     TEXT,
                  resume_page    INTEGER NOT NULL DEFAULT 0,
                  resume_at      INTEGER NOT NULL DEFAULT 0,
                  reading_layout INTEGER NOT NULL DEFAULT 0   -- app::Layout 的值
                ))"),
            QStringLiteral(R"(
                CREATE TABLE IF NOT EXISTS item (
                  id            INTEGER PRIMARY KEY,
                  collection_id INTEGER NOT NULL REFERENCES collection(id) ON DELETE CASCADE,
                  ord           INTEGER NOT NULL,
                  parent_ord    INTEGER NOT NULL DEFAULT -1,
                  kind          INTEGER NOT NULL,       -- Sequence::Kind 的值
                  label         TEXT NOT NULL DEFAULT '',
                  page_index    INTEGER NOT NULL DEFAULT -1,
                  size          INTEGER NOT NULL DEFAULT 0,
                  mtime         INTEGER NOT NULL DEFAULT 0,
                  width         INTEGER NOT NULL DEFAULT 0,
                  height        INTEGER NOT NULL DEFAULT 0,
                  first_index   INTEGER NOT NULL DEFAULT -1,
                  last_index    INTEGER NOT NULL DEFAULT -1,
                  UNIQUE(collection_id, ord)
                ))"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS item_parent ON item(collection_id, parent_ord)"),
            // ★ v9：找"这张图在哪个容器"要用它（覆盖的容器里 first_index 最大的那个）
            QStringLiteral("CREATE INDEX IF NOT EXISTS item_boundary ON item(collection_id, first_index)"),
            // ★ v8：`action` 表（跨进程 stop）已删 —— 新模型里停止只发生在本进程的内存队列
            // 缩略图缓存（设计 11 §五：进库 + LRU 2000 条，所有档位合计）
            QStringLiteral(R"(
                CREATE TABLE IF NOT EXISTS thumb (
                  collection_id INTEGER,
                  item_key      TEXT NOT NULL,
                  size_class    INTEGER NOT NULL,
                  src_mtime     INTEGER NOT NULL,
                  w             INTEGER,
                  h             INTEGER,
                  bytes         BLOB NOT NULL,
                  used_at       INTEGER NOT NULL,
                  PRIMARY KEY (collection_id, item_key, size_class)
                ))"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS thumb_lru ON thumb(used_at)"),
            QStringLiteral("PRAGMA user_version=10"),
        };
        for (const QString &sql : ddl) {
            if (!exec(sql)) { if (error) *error = m_error; return false; }
        }
    }

    m_open = true;
    if (autoVacuumMode() != 2)
        qInfo().noquote() << QStringLiteral(
            "[library] auto_vacuum 非 INCREMENTAL（既有库需 VACUUM 才生效，按定案不做）");
    qInfo().noquote() << QStringLiteral("[library] 库已打开 %1（schema v%2）").arg(path).arg(kSchemaVersion);
    return true;
}

// ── 变更通知 ─────────────────────────────────────────────────

int Store::dataVersion()
{
    QSqlQuery q(QSqlDatabase::database(m_connName));
    if (q.exec(QStringLiteral("PRAGMA data_version")) && q.next())
        return q.value(0).toInt();
    return -1;
}

QVector<Collection> Store::collections()
{
    QVector<Collection> out;
    QSqlQuery q(QSqlDatabase::database(m_connName));
    const QString sql = QStringLiteral(
        "SELECT id, root_path, kind, title, added_at, browsed_at, resume_key, resume_at,"
        "       reading_layout, resume_page"
        "  FROM collection ORDER BY browsed_at DESC, id DESC");
    if (!q.exec(sql)) { setError(q.lastError().text()); return out; }
    while (q.next()) {
        Collection c;
        c.id = q.value(0).toLongLong();
        c.rootPath = q.value(1).toString();
        c.kind = Sequence::Kind(q.value(2).toInt());
        c.title = q.value(3).toString();
        c.addedAt = q.value(4).toLongLong();
        c.browsedAt = q.value(5).toLongLong();
        c.resumeKey = q.value(6).toString();
        c.resumeAt = q.value(7).toLongLong();
        c.readingLayout = app::Layout(q.value(8).toInt());
        c.resumePage = q.value(9).toInt();
        out << c;
    }
    return out;
}

Collection Store::collection(qint64 id)
{
    for (const Collection &c : collections())
        if (c.id == id) return c;
    return {};
}

// ── 写入 ─────────────────────────────────────────────────────

qint64 Store::addCollection(const QString &rootPath, Sequence::Kind kind)
{
    QSqlQuery q(QSqlDatabase::database(m_connName));
    // 已存在 → 直接返回既有 id（"重复加入同一目录"当作已在库中，文档 §5.2）
    q.prepare(QStringLiteral("SELECT id FROM collection WHERE root_path = ?"));
    q.addBindValue(rootPath);
    if (q.exec() && q.next()) return q.value(0).toLongLong();

    q.prepare(QStringLiteral(
        "INSERT INTO collection (root_path, kind, title, added_at, browsed_at)"
        " VALUES (?, ?, ?, ?, ?)"));           // ★ v8：没有 scan_state 列了
    q.addBindValue(rootPath);
    q.addBindValue(int(kind));                 // ★ 存枚举值（一份表示）
    q.addBindValue(QFileInfo(rootPath).fileName());
    q.addBindValue(nowSecs());
    q.addBindValue(nowSecs());
    if (!q.exec()) { setError(q.lastError().text()); return -1; }
    return q.lastInsertId().toLongLong();
}

bool Store::removeCollection(qint64 id)
{
    QSqlQuery q(QSqlDatabase::database(m_connName));
    q.prepare(QStringLiteral("DELETE FROM collection WHERE id = ?"));   // 条目/任务/动作级联删除
    q.addBindValue(id);
    if (!q.exec()) { setError(q.lastError().text()); return false; }
    return true;
}

bool Store::renameCollection(qint64 id, const QString &title)
{
    QSqlQuery q(QSqlDatabase::database(m_connName));
    q.prepare(QStringLiteral("UPDATE collection SET title = ? WHERE id = ?"));
    q.addBindValue(title);
    q.addBindValue(id);
    if (!q.exec()) { setError(q.lastError().text()); return false; }
    return true;
}

bool Store::touchBrowsed(qint64 id)
{
    // 参与排序 → 算内容变更
    QSqlQuery q(QSqlDatabase::database(m_connName));
    q.prepare(QStringLiteral("UPDATE collection SET browsed_at = ? WHERE id = ?"));
    q.addBindValue(nowSecs());
    q.addBindValue(id);
    if (!q.exec()) { setError(q.lastError().text()); return false; }
    return true;
}

bool Store::setResume(qint64 id, const QString &resumeKey, int page)
{
    // 仅更新不同步（`resume_key` = label 链的 **JSON 数组**，结构化编码，不用分隔符拼串）
    QSqlQuery q(QSqlDatabase::database(m_connName));
    q.prepare(QStringLiteral(
        "UPDATE collection SET resume_key = ?, resume_page = ?, resume_at = ? WHERE id = ?"));
    q.addBindValue(resumeKey);
    q.addBindValue(page);
    q.addBindValue(nowSecs());
    q.addBindValue(id);
    if (!q.exec()) { setError(q.lastError().text()); return false; }
    return true;
}

bool Store::setReadingLayout(qint64 id, app::Layout layout)
{
    // 图集级唯一设置（设计 10 §11.1）
    QSqlQuery q(QSqlDatabase::database(m_connName));
    q.prepare(QStringLiteral("UPDATE collection SET reading_layout = ? WHERE id = ?"));
    q.addBindValue(int(layout));               // ★ 存枚举值（一份表示）
    q.addBindValue(id);
    if (!q.exec()) { setError(q.lastError().text()); return false; }
    return true;
}

// ★ v8：`setContentChanged` / `upsertItems` / `clearItems` **已删** —— 它们只服务
//   旧的"扫描任务"路径（逐批 upsert / 清除在前 / 探针标记），新模型只有 `replaceItems`。

// ★★ 设计 14 §9.1：**原子替换**（图集重置"正常跑完"的那一次落库）。
//   · 一个事务：删旧行 + 插新行 ⇒ 失败就回滚，**旧内容原样保留**
//     （这就是它与旧的"清除在前"路径的根本差别：那个失败 = 空/半截）；
//   · 分块插入：每块一条 multi-row `INSERT ... VALUES (...),(...)` —— 既不是"一行一条"的
//     十万次 prepare/exec，也不是"一条语句装下全部行"的巨型 SQL。
bool Store::replaceItems(qint64 collectionId, const QVector<Item> &rows)
{
    QSqlDatabase db = QSqlDatabase::database(m_connName);
    if (!db.transaction()) { setError(db.lastError().text()); return false; }

    // ① 删该集合的旧行（与插入同一个事务 ⇒ 外面看不到"中间的空表"）
    {
        QSqlQuery del(db);
        del.prepare(QStringLiteral("DELETE FROM item WHERE collection_id = ?"));
        del.addBindValue(collectionId);
        if (!del.exec()) {
            setError(QStringLiteral("替换条目失败（删旧行）：%1").arg(del.lastError().text()));
            db.rollback();
            return false;
        }
    }

    // ② 分块插入新行（块大小见 `kReplaceChunkRows`）
    for (int base = 0; base < rows.size(); base += kReplaceChunkRows) {
        const int n = qMin(kReplaceChunkRows, rows.size() - base);
        QString sql = QStringLiteral(
            "INSERT INTO item (collection_id, ord, parent_ord, kind, label,"
            "                  page_index, size, mtime, width, height,"
            "                  first_index, last_index) VALUES ");
        for (int i = 0; i < n; ++i)
            sql += (i == 0 ? QStringLiteral("(?,?,?,?,?,?,?,?,?,?,?,?)")
                           : QStringLiteral(",(?,?,?,?,?,?,?,?,?,?,?,?)"));
        QSqlQuery q(db);
        q.prepare(sql);
        for (int i = 0; i < n; ++i) {
            const Item &it = rows.at(base + i);
            q.addBindValue(collectionId);
            q.addBindValue(it.ord);
            q.addBindValue(it.parentOrd);
            q.addBindValue(int(it.kind));         // ★ 存枚举值（一份表示）
            q.addBindValue(textOrEmpty(it.label));
            q.addBindValue(it.pageIndex);
            q.addBindValue(it.size);
            q.addBindValue(it.mtime);
            q.addBindValue(it.width);
            q.addBindValue(it.height);
            q.addBindValue(it.firstIndex);
            q.addBindValue(it.lastIndex);
        }
        if (!q.exec()) {
            setError(QStringLiteral("替换条目失败（第 %1 块 / 共 %2 块）：%3")
                         .arg(base / kReplaceChunkRows + 1)
                         .arg((rows.size() + kReplaceChunkRows - 1) / kReplaceChunkRows)
                         .arg(q.lastError().text()));
            db.rollback();               // ★ 原子性：中途失败 ⇒ 旧内容保持（包括已插入的前几块）
            return false;
        }
    }

    if (!db.commit()) { setError(db.lastError().text()); return false; }
    qInfo().noquote() << QStringLiteral("[library] 集合 %1 原子替换：%2 行")
                             .arg(collectionId).arg(rows.size());
    return true;
}

namespace {
// 行读取的**唯一一份**（列顺序 = 下面那条 SELECT 的顺序）
const char *kItemColumns =
    "id, ord, parent_ord, kind, label, page_index, size, mtime, width, height,"
    " first_index, last_index";

Item readItem(const QSqlQuery &q)
{
    Item it;
    it.id         = q.value(0).toLongLong();
    it.ord        = q.value(1).toInt();
    it.parentOrd  = q.value(2).toInt();
    it.kind       = Sequence::Kind(q.value(3).toInt());
    it.label      = q.value(4).toString();
    it.pageIndex  = q.value(5).toInt();
    it.size       = q.value(6).toLongLong();
    it.mtime      = q.value(7).toLongLong();
    it.width      = q.value(8).toInt();
    it.height     = q.value(9).toInt();
    it.firstIndex = q.value(10).toInt();
    it.lastIndex  = q.value(11).toInt();
    return it;
}

// 恢复的行流（见头文件）：forward-only 游标
class RowCursor : public Sequence::RowSource
{
public:
    RowCursor(const QString &connName, qint64 collectionId)
        : m_q(QSqlDatabase::database(connName))
    {
        m_q.setForwardOnly(true);                 // ★ 不缓冲整个结果集
        m_q.prepare(QStringLiteral("SELECT ") + QLatin1String(kItemColumns)
                    + QLatin1String(" FROM item WHERE collection_id = ? ORDER BY ord"));
        m_q.addBindValue(collectionId);
        m_ok = m_q.exec();
    }

    int next(QVector<Sequence::Row> &out, int maxRows) override
    {
        if (!m_ok) return 0;
        int n = 0;
        while (n < maxRows && m_q.next()) {
            out << toRow(readItem(m_q));
            ++n;
        }
        return n;
    }

    bool ok() const { return m_ok; }

private:
    QSqlQuery m_q;
    bool      m_ok = false;
};
} // namespace

std::unique_ptr<Sequence::RowSource> Store::rowSource(qint64 collectionId)
{
    return std::make_unique<RowCursor>(m_connName, collectionId);
}

QVector<Sequence::Row> Store::sequenceRows(qint64 collectionId)
{
    const QVector<Item> rows = items(collectionId);
    QVector<Sequence::Row> out;
    out.reserve(rows.size());
    for (const Item &r : rows) out << toRow(r);
    return out;
}

QVector<Item> Store::items(qint64 collectionId)
{
    QVector<Item> out;
    QSqlQuery q(QSqlDatabase::database(m_connName));
    q.prepare(QStringLiteral("SELECT ") + QLatin1String(kItemColumns)
              + QLatin1String(" FROM item WHERE collection_id = ? ORDER BY ord"));
    q.addBindValue(collectionId);
    if (!q.exec()) { setError(q.lastError().text()); return out; }
    while (q.next()) out << readItem(q);
    return out;
}

// ★ v5：**按链 + 页查**（resume 用）。
//   单一事实源：**走同一条恢复入口**（行流 → 序列），再用序列自己的 `indexOfChain`
//   （沿 label 链 walk 树 + 页号精确命中）⇒ 不再"另建一条序列"那套私有实现。
// ★★ "存档位置（label 链 + 页号）是第几条图"—— **按深度走几条查询**，不做整份遍历。
//   语义与 `Sequence::indexOfChain` 逐条对齐（前 n-1 段 = 容器；末段 A) 直接子图 / B) 子容器按页）。
//   返回 = **图的 index**（0 基：这一行之前有几条图）；-1 = 没有。
int Store::itemIndexOfChain(qint64 collectionId, const QStringList &chain, int page)
{
    if (chain.isEmpty()) return -1;
    QSqlDatabase db = QSqlDatabase::database(m_connName);

    // 容器子行：沿 (parent_ord, label) 下钻（kind <> Image ⇒ folder / archive）
    const auto containerChild = [&](int parentOrd, const QString &label) -> int {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT ord FROM item WHERE collection_id = ? AND parent_ord = ?"
                                 " AND label = ? AND kind <> ? ORDER BY ord LIMIT 1"));
        q.addBindValue(collectionId); q.addBindValue(parentOrd); q.addBindValue(label);
        q.addBindValue(int(Sequence::Kind::Image));
        return (q.exec() && q.next()) ? q.value(0).toInt() : -1;
    };
    // 直接子图（普通图 = 自己的名字；页行 = 空名字 + 页号）
    const auto imageChild = [&](int parentOrd, const QString &label, bool useLabel,
                                int wantPage) -> int {
        QSqlQuery q(db);
        if (useLabel) {
            q.prepare(QStringLiteral(
                "SELECT ord FROM item WHERE collection_id = ? AND parent_ord = ?"
                " AND label = ? AND kind = ?"
                " AND (CASE WHEN page_index < 0 THEN 0 ELSE page_index END) = ?"
                " ORDER BY ord LIMIT 1"));
            q.addBindValue(collectionId); q.addBindValue(parentOrd); q.addBindValue(label);
            q.addBindValue(int(Sequence::Kind::Image)); q.addBindValue(wantPage);
        } else {
            q.prepare(QStringLiteral(
                "SELECT ord FROM item WHERE collection_id = ? AND parent_ord = ?"
                " AND kind = ?"
                " AND (CASE WHEN page_index < 0 THEN 0 ELSE page_index END) = ?"
                " ORDER BY ord LIMIT 1"));
            q.addBindValue(collectionId); q.addBindValue(parentOrd);
            q.addBindValue(int(Sequence::Kind::Image)); q.addBindValue(wantPage);
        }
        return (q.exec() && q.next()) ? q.value(0).toInt() : -1;
    };

    int parentOrd = -1;                                    // 根（合成节点，ord = -1）
    for (int s = 0; s + 1 < chain.size(); ++s) {
        parentOrd = containerChild(parentOrd, chain.at(s));
        if (parentOrd < 0) return -1;
    }
    const QString last = chain.last();

    int ord = imageChild(parentOrd, last, /*useLabel=*/true, page);        // A) 直接子图
    if (ord < 0) {                                                        // B) 子容器（多页文件）
        const int child = containerChild(parentOrd, last);
        if (child < 0) return -1;
        ord = imageChild(child, QString(), /*useLabel=*/false, page);
        if (ord < 0) return -1;
    }

    QSqlQuery q(db);                                       // 图的 index = 这一行之前有几条图
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM item WHERE collection_id = ? AND kind = ?"
                             " AND ord < ?"));
    q.addBindValue(collectionId);
    q.addBindValue(int(Sequence::Kind::Image));
    q.addBindValue(ord);
    if (q.exec() && q.next()) return q.value(0).toInt();
    return -1;
}

// ★★ 卡片封面用的那一行 + 它的读图位置（**不做整份遍历**）：
//   · 候选 = 按 ord 升序、跳过目录、跳过"多页文件的第 > 0 页"；
//   · 候选的**祖先链里出现压缩包 ⇒ 跳过**（包内成员没有盘上绝对路径，当不了封面）；
//   · 命中 ⇒ 只把"祖先链 + 它自己"这几行喂给 `locateRows`（沿链解析的唯一实现）。
bool Store::coverRow(qint64 collectionId, const QString &rootPath, bool rootIsArchive,
                     Sequence::Row *rowOut, imageread::Location *locOut) const
{
    static constexpr int kAncestorGuard = 64;              // 防畸形环（真实目录不会这么深）
    QSqlDatabase db = QSqlDatabase::database(m_connName);

    QSqlQuery q(db);
    q.setForwardOnly(true);
    q.prepare(QStringLiteral("SELECT ") + QLatin1String(kItemColumns)
              + QLatin1String(" FROM item WHERE collection_id = ? AND kind <> ?"
                              " AND (kind <> ? OR page_index <= 0) ORDER BY ord"));
    q.addBindValue(collectionId);
    q.addBindValue(int(Sequence::Kind::Folder));
    q.addBindValue(int(Sequence::Kind::Image));
    if (!q.exec()) return false;

    while (q.next()) {
        const Item cand = readItem(q);
        QVector<Sequence::Row> subset;
        subset << toRow(cand);
        bool insideArchive = false;
        int parentOrd = cand.parentOrd;
        int guard = 0;
        while (parentOrd >= 0 && (guard += 1) <= kAncestorGuard) {
            QSqlQuery a(db);
            a.prepare(QStringLiteral("SELECT ") + QLatin1String(kItemColumns)
                      + QLatin1String(" FROM item WHERE collection_id = ? AND ord = ? LIMIT 1"));
            a.addBindValue(collectionId);
            a.addBindValue(parentOrd);
            if (!a.exec() || !a.next()) { insideArchive = true; break; }   // 祖先缺失 ⇒ 不可用
            const Item anc = readItem(a);
            if (anc.kind == Sequence::Kind::Archive) { insideArchive = true; break; }
            subset << toRow(anc);
            parentOrd = anc.parentOrd;
        }
        if (insideArchive) continue;

        std::sort(subset.begin(), subset.end(),
                  [](const Sequence::Row &x, const Sequence::Row &y) { return x.ord < y.ord; });
        int at = -1;
        for (int i = 0; i < subset.size(); ++i)
            if (subset.at(i).ord == cand.ord) { at = i; break; }
        const QVector<imageread::Location> locs =
            Sequence::locateRows(rootPath, rootIsArchive, subset);
        if (at < 0 || at >= locs.size()) continue;
        if (locs.at(at).diskPath.isEmpty() || !locs.at(at).members.isEmpty()) continue;
        if (rowOut) *rowOut = toRow(cand);
        if (locOut) *locOut = locs.at(at);
        return true;
    }
    return false;
}

// ★ v6：**总数现算**（一条 GROUP BY 覆盖所有集合；界面要的"多少张图"）
QHash<qint64, int> Store::itemCounts()
{
    QHash<qint64, int> out;
    QSqlQuery q(QSqlDatabase::database(m_connName));
    q.prepare(QStringLiteral("SELECT collection_id, COUNT(*) FROM item"
                             " WHERE kind = ? GROUP BY collection_id"));
    q.addBindValue(int(Sequence::Kind::Image));
    if (!q.exec()) {
        setError(q.lastError().text());
        return out;
    }
    while (q.next()) out.insert(q.value(0).toLongLong(), q.value(1).toInt());
    return out;
}

int Store::itemCount(qint64 collectionId)
{
    QSqlQuery q(QSqlDatabase::database(m_connName));
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM item WHERE collection_id = ? AND kind = ?"));
    q.addBindValue(collectionId);
    q.addBindValue(int(Sequence::Kind::Image));
    if (q.exec() && q.next()) return q.value(0).toInt();
    setError(q.lastError().text());
    return -1;
}

// ★ v8：`setScanState` / `touchScanUpdatedAt` / `healStaleScans`（陈旧自愈）与
//   `action` 表（入队 / 认领 / `takeAction`）**已整体删除** —— 新模型里图集没有
//   "扫描任务"，状态只在内存（`resetq::ResetState`），跨进程感知只剩 1 s 的
//   `data_version` 比较（**没有自愈**：不再有"进行中"的持久状态）。

// ── 缩略图缓存（设计 11 §五）─────────────────────────────────
//
//   · 命中即刷新 used_at（LRU 的"用"）
//   · src_mtime 变了 → 视为失效：删掉那行并返回 false（调用方重做）
//   · 写入超限 → 按 used_at 淘汰到 kThumbLimit 条，然后 incremental_vacuum 还空间

namespace {

// "直接打开"的来源在库里存 NULL（设计 11 §五：collection_id 可为 NULL）
QVariant thumbCollectionId(qint64 id) { return id < 0 ? QVariant() : QVariant(id); }

} // namespace

bool Store::thumbGet(qint64 collectionId, const QString &itemKey, int sizeClass,
                     qint64 srcMtime, Thumb *out)
{
    QSqlQuery q(QSqlDatabase::database(m_connName));
    q.prepare(QStringLiteral(
        "SELECT src_mtime, w, h, bytes, used_at FROM thumb"
        " WHERE collection_id IS ? AND item_key = ? AND size_class = ?"));
    q.addBindValue(thumbCollectionId(collectionId));
    q.addBindValue(itemKey);
    q.addBindValue(sizeClass);
    if (!q.exec()) { setError(q.lastError().text()); return false; }
    if (!q.next())
        return false;                                   // 未命中
    const qint64 storedMtime = q.value(0).toLongLong();
    if (storedMtime != srcMtime) {
        // 源变了 → 失效重做（列级删除，不读整行改再写回）
        QSqlQuery d(QSqlDatabase::database(m_connName));
        d.prepare(QStringLiteral(
            "DELETE FROM thumb WHERE collection_id IS ? AND item_key = ? AND size_class = ?"));
        d.addBindValue(thumbCollectionId(collectionId));
        d.addBindValue(itemKey);
        d.addBindValue(sizeClass);
        if (!d.exec()) setError(d.lastError().text());
        return false;
    }
    if (out) {
        out->collectionId = collectionId;
        out->itemKey = itemKey;
        out->sizeClass = sizeClass;
        out->srcMtime = storedMtime;
        out->w = q.value(1).toInt();
        out->h = q.value(2).toInt();
        out->bytes = q.value(3).toByteArray();
        out->usedAt = q.value(4).toLongLong();
    }
    // 命中 → 刷新 used_at（列级 UPDATE：只碰这一列）
    QSqlQuery u(QSqlDatabase::database(m_connName));
    u.prepare(QStringLiteral(
        "UPDATE thumb SET used_at = ? WHERE collection_id IS ? AND item_key = ? AND size_class = ?"));
    u.addBindValue(nowSecs());
    u.addBindValue(thumbCollectionId(collectionId));
    u.addBindValue(itemKey);
    u.addBindValue(sizeClass);
    if (!u.exec()) setError(u.lastError().text());
    return true;
}

int Store::thumbCount()
{
    QSqlQuery q(QSqlDatabase::database(m_connName));
    if (q.exec(QStringLiteral("SELECT COUNT(*) FROM thumb")) && q.next())
        return q.value(0).toInt();
    setError(q.lastError().text());
    return -1;
}

int Store::thumbEvictTo(int limit)
{
    if (limit < 0) limit = kThumbLimit;
    QSqlQuery q(QSqlDatabase::database(m_connName));
    // 只留 used_at 最新的 limit 条（设计 11 §五的那条 SQL）
    q.prepare(QStringLiteral(
        "DELETE FROM thumb WHERE rowid IN ("
        "  SELECT rowid FROM thumb ORDER BY used_at DESC LIMIT -1 OFFSET ?)"));
    q.addBindValue(limit);
    if (!q.exec()) { setError(q.lastError().text()); return 0; }
    const int removed = q.numRowsAffected();
    if (removed > 0)
        exec(QStringLiteral("PRAGMA incremental_vacuum"));   // 真的把空间还回去
    return removed;
}

bool Store::thumbPut(const Thumb &t)
{
    if (t.itemKey.isEmpty()) { setError(QStringLiteral("thumbPut: item_key 为空")); return false; }
    if (t.bytes.isEmpty())   { setError(QStringLiteral("thumbPut: bytes 为空")); return false; }
    if (t.bytes.size() > kThumbMaxBytes) {
        setError(QStringLiteral("thumbPut: BLOB %1 字节 > 上限 %2")
                     .arg(t.bytes.size()).arg(kThumbMaxBytes));
        return false;
    }

    QSqlDatabase db = QSqlDatabase::database(m_connName);
    if (!db.transaction()) { setError(db.lastError().text()); return false; }

    const QVariant cid = thumbCollectionId(t.collectionId);
    const qint64 usedAt = t.usedAt > 0 ? t.usedAt : nowSecs();

    if (t.collectionId < 0) {
        // ⚠️ SQLite 的 UNIQUE/PK 里 NULL 互不相等 → ON CONFLICT 对 NULL 不生效。
        //    "直接打开"这一档因此走 delete + insert（同一事务内，只动这一行）。
        QSqlQuery d(db);
        d.prepare(QStringLiteral(
            "DELETE FROM thumb WHERE collection_id IS NULL AND item_key = ? AND size_class = ?"));
        d.addBindValue(t.itemKey);
        d.addBindValue(t.sizeClass);
        if (!d.exec()) { setError(d.lastError().text()); db.rollback(); return false; }
    }

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO thumb (collection_id, item_key, size_class, src_mtime, w, h, bytes, used_at)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(collection_id, item_key, size_class) DO UPDATE SET"
        "   src_mtime = excluded.src_mtime, w = excluded.w, h = excluded.h,"
        "   bytes = excluded.bytes, used_at = excluded.used_at"));
    q.addBindValue(cid);
    q.addBindValue(t.itemKey);
    q.addBindValue(t.sizeClass);
    q.addBindValue(t.srcMtime);
    q.addBindValue(t.w);
    q.addBindValue(t.h);
    q.addBindValue(t.bytes);
    q.addBindValue(usedAt);
    if (!q.exec()) { setError(q.lastError().text()); db.rollback(); return false; }
    if (!db.commit()) { setError(db.lastError().text()); return false; }

    // 超限才淘汰（不是每次写都扫一遍）
    if (thumbCount() > kThumbLimit)
        thumbEvictTo(kThumbLimit);
    return true;
}

int Store::autoVacuumMode()
{
    QSqlQuery q(QSqlDatabase::database(m_connName));
    if (q.exec(QStringLiteral("PRAGMA auto_vacuum")) && q.next())
        return q.value(0).toInt();
    return -1;
}

} // namespace library
