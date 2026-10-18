#pragma once

// ══ 图集库存储（SQLite）════════════════════════════════════════
//   设计：docs/design/10-数据与多进程架构.md
//
//   · 单文件库：`~/.config/photoset-reader/library.db`（WAL → 附带 -wal / -shm）
//   · **多进程**：任意实例都可读写；靠 WAL（多读 + 单写）+ `busy_timeout` 串行化写
//   · **变更通知**：只有一条通道 —— `PRAGMA data_version`（**别的连接**提交才变，自己写不变）
//     ★ v6 定案：**删掉 `revision`** —— 同步只靠 data_version（设计 10 §2.1）
//   · **每线程一个连接**（QtSql 与 SQLite 的共同规矩）：连接名带线程标识
//   · 列级 UPDATE，**绝不"读整行→改→写回"**（文档 §5.3 硬规则 1）

#include <QByteArray>
#include <memory>
#include <QString>
#include <QVector>

#include "appsettings.h"   // ★ app::Layout：阅读版面 = **一份枚举**（库列存它的值）
#include "sequence.h"       // ★ Sequence::Kind：行 / 容器种类 = **一份枚举**

namespace library {

// 集合（一个图集库 = 一个目录或一个压缩包）
//
//   ★★ v6 定案（2026-10-13）：**从简** —— 一个图集只有"身份 + 位置"，
//     没有扫描任务 / 属主 / 租约 / 心跳 / 进度 / 断点这些概念。
//     **删掉**：`scan_owner` / `scan_lease_until` / `scanned_dirs` / `scanned_items` /
//     `scan_cursor` / `scan_error` / `item_count` / `revision`。
//     · `item_count`（界面要"总数"）⇒ 用一条 `GROUP BY` **现算**（`Store::itemCounts`）
//     · `revision` ⇒ 删除；跨进程同步**只有** `data_version` 这一条通道
//   ★★ v8 定案（T2c-2b-2，设计 14 §9.6）：**已写入磁盘的"扫描状态"整体作废** ——
//     新模型里图集没有"扫描任务"：点重置 = `LibraryCore` 起一条**活序列**，
//     跑完 ⇒ 后台一个事务 `replaceItems` **原子替换**该图集的行；停止 / 失败 / 卡住 ⇒
//     **不落库**。⇒ `scan_state` / `scan_updated_at` / `content_changed` 三列与
//     `action` 表（跨进程 stop）**一并删除**：状态只在内存（`resetq::ResetState`），
//     跨进程感知只剩 1 s 的 `data_version` 比较（**没有自愈**）。
struct Collection {
    qint64  id = -1;
    QString rootPath;
    Sequence::Kind kind = Sequence::Kind::Folder;   // ★ 一份枚举（库列存它的值）
    QString title;
    qint64  addedAt = 0;
    qint64  browsedAt = 0;
    qint64  resumeAt = 0;  // 仅更新不同步：进度最后写入时间
    QString resumeKey;     // 仅更新不同步：阅读位置 = **label 链的 JSON 数组**
    int     resumePage = 0;   // 多页文件停在第 k 页（普通图 = 0）
    // ★ v3：图集级唯一设置 = 阅读版面 —— 它影响"阅读进度怎么判读"（设计 10 §11.1）
    app::Layout readingLayout = app::Layout::Single;
};

// 条目（**v5**：库 = **已写入磁盘的序列** —— 身份 = 沿 `parent_ord` 上溯的 label 序列）
//
//   ★ 定案（用户 2026-10-13）：**没有 key / parent_key / rel_path** ——
//     写入磁盘**不拼任何字符串**。唯一约束 = `(collection_id, ord)`。
//   ★ `ord` = **遍历序号**（一个图集内连续，**节点与叶子共用同一个序**）。
//
//   ## 字段语义
//   · `ord`       = 遍历序号（节点与叶子共用；一个集合内唯一）
//   · `parent_ord`= 父行的 `ord`（根级 = -1）
//   · `kind`      = folder（目录节点）| archive（文件即容器：压缩包 / 多页文件）| image（叶子）
//   · `label`     = 它在**容器里的名字**（UI 显示用）；**页行 = 空串**
//   · `pageIndex` = 页行 = 页号；容器行 / 普通图 = -1
//   · `width/height` = 像素尺寸（**文件事实**，写入磁盘；宽窄分组是派生值，不写入磁盘）
//
//   ★ **没有 `missing`**：缺失只在**会话内**标记（跳过/计数），库里没有这个概念（定案）。
//   ★ 读图位置（磁盘路径 + 容器成员链）由 `Sequence::locateRows` **派生**，不写入磁盘。
struct Item {
    qint64  id = -1;
    int     ord = 0;          // 遍历序号（节点与叶子共用）
    int     parentOrd = -1;   // 父行的 ord（根级 = -1）
    Sequence::Kind kind = Sequence::Kind::Image;   // ★ 一份枚举（库列存它的值）
    QString label;            // 容器里的名字（页行 = 空串）
    int     pageIndex = -1;   // 页行的页号；容器行 / 普通图 = -1
    qint64  size = 0;
    qint64  mtime = 0;
    int     width = 0;   // ★ v3：像素宽（0 = 未知）
    int     height = 0;  // ★ v3：像素高（0 = 未知）
    // ★ v9：**边界索引**（只对容器行有意义；图行 = -1）= 该容器**子树**覆盖的**图的 index 区间**。
    //   扫描时算好写盘（`Sequence::Row::firstIndex/lastIndex`）⇒ 恢复时不用再算。
    int     firstIndex = -1;
    int     lastIndex = -1;
};

// ★★ 行形态转换 —— **唯一一份**（用户 2026-10-18："一个概念只留一份"）：
//   库行（`Item`）与序列行（`Sequence::Row`）是**同一个东西的两侧**，字段一一对应。
Item         toItem(const Sequence::Row &r);
Sequence::Row toRow(const Item &r);

// ★ 来源根本身是不是容器（压缩包 / 多页文件）—— **唯一实现**：
//   · 库里记的 `kind = Archive`；或 · 盘上那个路径本身就是压缩包（老库行可能没跟上）。
bool rootIsArchive(const Collection &c);

// 缩略图缓存（设计 11 §五：进库 + 按条数 LRU）
//   · `collection_id` < 0 表示"直接打开"的来源（库里存 NULL）
//   · `src_mtime` 变了即视为失效 → 重做
//   · `used_at` = 0 时由 Store 填当前时间；显式给值供测试构造确定的 LRU 顺序
struct Thumb {
    qint64     collectionId = -1;
    QString    itemKey;
    int        sizeClass = 0;      // 档位（唯一规格 = kThumbSize，见设计 11 §5.1）
    qint64     srcMtime = 0;
    int        w = 0;
    int        h = 0;
    QByteArray bytes;              // 小图（WebP/JPEG），目标 ≤ kThumbMaxBytes
    qint64     usedAt = 0;
};

class Store
{
public:
    Store();
    ~Store();

    // 库文件路径：~/.config/photoset-reader/library.db
    static QString databasePath();

    // 打开（必要时建表）。失败时写 error。
    //   quiet=true：失败**不刷 warning**（启动期"库不可用"由调用方决定怎么报），
    //               只把原因写进 error / lastError()
    bool open(QString *error = nullptr, bool quiet = false);
    bool isOpen() const { return m_open; }
    QString lastError() const { return m_error; }

    // ── 变更通知 ──
    // `PRAGMA data_version`：**别的连接**提交才变；本连接自己写不变（文档 §二）
    int dataVersion();
    // 摘要：所有集合的 (id, root_path, …, reading_layout) —— 轮询第一步，很小
    QVector<Collection> collections();

    Collection collection(qint64 id);

    // ── 写入：集合 ──
    // 已存在则返回既有 id（root_path UNIQUE + ON CONFLICT DO NOTHING）
    qint64 addCollection(const QString &rootPath, Sequence::Kind kind);
    bool   removeCollection(qint64 id);
    bool   renameCollection(qint64 id, const QString &title);
    // 浏览时间：参与"按浏览时间倒序"排序（同步一律走 `data_version`）
    bool   touchBrowsed(qint64 id);
    // 阅读进度：仅更新不同步（`resumeKey` = **label 链的 JSON 数组**；`page` = 页号）
    bool   setResume(qint64 id, const QString &resumeKey, int page = 0);
    // 阅读版面（图集级唯一设置，设计 10 §11.1）
    bool   setReadingLayout(qint64 id, app::Layout layout);

    // ── 写入：条目（**唯一写入路径 = 原子替换**）──────────────────────────────
    // ★★ 设计 14 §9.1：图集重置**正常跑完**时的那一次落库：
    //   · **一个事务**里"删该集合旧行 + 插入新行" ⇒ 别的实例只在 commit 那一刻看到新内容；
    //   · 插入**分块**（每块 ≤ `kReplaceChunkRows` 行，一条 multi-row INSERT = executemany 式批量）
    //     ⇒ 不产生"一条巨型语句"（10 万行 = 多 MB 的 SQL 文本）；
    //   · **中途失败** ⇒ 回滚 + 写 `lastError()`，**旧内容原样保留**。
    //   ★ v8：`upsertItems`（逐批 upsert）与 `clearItems`（清除在前）**已删** ——
    //     它们只服务旧的"扫描任务"路径（边扫边写入磁盘 ⇒ 失败留半截），新模型不再需要。
    bool replaceItems(qint64 collectionId, const QVector<Item> &rows);
    QVector<Item> items(qint64 collectionId);
    // 同一批行的**序列形态**（恢复的输入）—— 转换只有一份（上面的 `toRow`）
    QVector<Sequence::Row> sequenceRows(qint64 collectionId);

    // ★★ 恢复用的**行流**：SQLite 的 **forward-only 游标**（**不把整份结果集读进内存**）。
    //   · 顺序 = `ORDER BY ord`，走 `UNIQUE(collection_id, ord)` 索引（不建临时排序表）；
    //   · 行是 DFS 前序、父行在前 ⇒ **前向流足够**（`Sequence::restore` 边收边并）；
    //   · 游标没走完就销毁 ⇒ 直接收摊（`QSqlQuery` 析构即结束语句）。
    std::unique_ptr<Sequence::RowSource> rowSource(qint64 collectionId);
    // ★ v5：**按链 + 页查**（resume 用）—— 沿 `parent_ord` 建树后沿 label 链 walk + 页号命中。
    //   返回**可读序列**（image 行）里的序号；-1 = 没命中。
    int itemIndexOfChain(qint64 collectionId, const QStringList &chain, int page = 0);
    // ★ v6：**总数现算**（`GROUP BY`；界面要的"多少张图"）——一条查询覆盖所有集合
    QHash<qint64, int> itemCounts();
    int itemCount(qint64 collectionId);

    // ── 缩略图缓存（设计 11 §五）──────────────────────────────
    //   · 有界：**按条数** LRU，上限 kThumbLimit（所有档位合计）
    //   · 单个 BLOB ≤ kThumbMaxBytes（超了拒写，不让它把库撑大）
    //   · 淘汰后 `PRAGMA incremental_vacuum` 真正还空间
    static constexpr int kThumbLimit = 2000;

    // ★ 缩略图规格（设计 11 §5.1，用户 2026-10-11 定案）——**唯一来源**：
    //   别处不许再写 384 / 50 / 64KB 这类字面量。
    //   · 不按 UI 尺寸生成、不做多档：恒为 kThumbSize 的**等比适应**（最长边，**不裁切**）
    //   · WebP 质量 kThumbQuality（q50 经用户目视确认够用）
    //   · kThumbMaxBytes 只是安全网（等比适应实测 384：均值 9.2 KB / 最大 14.8 KB；
    //     Qt 的 WebP 编码器对 384px 漫画扫图实测 22–23 KB，故上限取 64KB）
    static constexpr int kThumbSize     = 384;         // 最长边（等比适应，不裁切）
    static constexpr int kThumbQuality  = 50;          // WebP 编码质量
    static constexpr int kThumbMaxBytes = 64 * 1024;   // 硬上限

    // ★ `replaceItems` 的分块大小：SQLite ≥3.32 的 `SQLITE_MAX_VARIABLE_NUMBER` 默认 32766，
    //   1024 行 × 10 列 = **10240 个占位符** < 上限（本机 sqlite 3.53 实测）⇒ 一条语句安全。
    static constexpr int kReplaceChunkRows = 1024;

    // 命中：填 out 并刷新 used_at；未命中 / src_mtime 变了 → false（变了的那行顺手删掉）
    bool thumbGet(qint64 collectionId, const QString &itemKey, int sizeClass,
                  qint64 srcMtime, Thumb *out);
    // 写入（同键覆盖）+ 超限即 LRU 淘汰 + 还空间
    bool thumbPut(const Thumb &t);
    int  thumbCount();
    // 只保留 used_at 最新的 limit 条；返回删掉的条数
    int  thumbEvictTo(int limit = kThumbLimit);
    // `PRAGMA auto_vacuum` 的实际取值（0 none / 1 full / 2 incremental）——供自检
    int  autoVacuumMode();

private:
    bool exec(const QString &sql);
    bool openImpl(QString *error);
    void setError(const QString &what);

    bool    m_open = false;
    bool    m_quietErrors = false;
    QString m_error;
    QString m_connName;
};

} // namespace library
