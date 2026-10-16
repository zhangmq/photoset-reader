#include "archive.h"
#include "natural.h"

#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <QMutexLocker>
#include <QTemporaryFile>
#include <algorithm>
#include <cstring>

#include <archive.h>
#include <archive_entry.h>

#define _UNIX                     // unrar 的 dll.hpp 在 Unix 上需要它来定义 HANDLE/LPARAM 等
#include <unrar/dll.hpp>

namespace {

constexpr int kBlockSize = 10240;

QString archiveError(struct archive *a)
{
    const char *e = archive_error_string(a);
    return e ? QString::fromUtf8(e) : QStringLiteral("(无错误信息)");
}

// ★ v7：RAR 的**魔数**（内容判定，不看扩展名 —— 上层零分派的第一步）。
//   RAR4 = "Rar!\x1a\x07\x00"；RAR5 = "Rar!\x1a\x07\x01\x00"。
bool looksLikeRar(const QByteArray &b)
{
    static const char kRar4[] = { 'R', 'a', 'r', '!', '\x1a', '\x07', '\x00' };
    static const char kRar5[] = { 'R', 'a', 'r', '!', '\x1a', '\x07', '\x01', '\x00' };
    if (b.size() < 7) return false;
    if (memcmp(b.constData(), kRar4, sizeof(kRar4)) == 0) return true;
    return b.size() >= 8 && memcmp(b.constData(), kRar5, sizeof(kRar5)) == 0;
}

// ── libarchive 的 RAII ────────────────────────────────────────
struct ArcReader {
    struct archive *a = archive_read_new();
    ArcReader()
    {
        archive_read_support_format_all(a);
        archive_read_support_filter_all(a);
    }
    ~ArcReader() { if (a) archive_read_free(a); }
    bool open(const QString &path, const QString &password)
    {
        if (!password.isEmpty())
            archive_read_add_passphrase(a, password.toUtf8().constData());
        const QByteArray p = QFileInfo(path).absoluteFilePath().toUtf8();
        return archive_read_open_filename(a, p.constData(), kBlockSize) == ARCHIVE_OK;
    }
    // ★ v5：内存容器（嵌套压缩包）——`buff` 必须活到 reader 析构（调用方持有 bytes）
    bool openMemory(const QByteArray &bytes, const QString &password)
    {
        if (!password.isEmpty())
            archive_read_add_passphrase(a, password.toUtf8().constData());
        return archive_read_open_memory(a, bytes.constData(), size_t(bytes.size())) == ARCHIVE_OK;
    }
};

// ── libunrar 的回调上下文与 RAII ─────────────────────────────
struct UnrarCtx {
    QByteArray *out = nullptr;      // 非空 = 正在解出该条目
    qint64 expected = 0;
};

int CALLBACK unrarCallback(UINT msg, LPARAM userData, LPARAM p1, LPARAM p2)
{
    auto *ctx = reinterpret_cast<UnrarCtx *>(userData);
    switch (msg) {
    case UCM_PROCESSDATA:
        if (ctx && ctx->out)
            ctx->out->append(reinterpret_cast<const char *>(p1), int(p2));
        return 0;                                   // 继续
    case UCM_LARGEDICT:
        return 1;                                   // 1 = 允许大字典（256MB 归档必需）
    default:
        return 0;                                   // 密码已由 RARSetPassword 提供
    }
}

struct UnrarReader {
    HANDLE h = nullptr;
    QByteArray pathUtf8;
    UnrarCtx ctx;
    unsigned int openResult = 0;
    unsigned int flags = 0;              // 打开时返回的归档标志（含 ROADF_SOLID）

    bool open(const QString &path, const QString &password, unsigned int mode)
    {
        pathUtf8 = QFileInfo(path).absoluteFilePath().toUtf8();
        RAROpenArchiveDataEx data;
        memset(&data, 0, sizeof(data));
        data.ArcName = pathUtf8.data();
        data.OpenMode = mode;
        data.Callback = unrarCallback;
        data.UserData = reinterpret_cast<LPARAM>(&ctx);
        h = RAROpenArchiveEx(&data);
        if (!h || data.OpenResult != ERAR_SUCCESS) {
            if (h) { RARCloseArchive(h); h = nullptr; }
            openResult = data.OpenResult;
            return false;
        }
        flags = data.Flags;
        if (!password.isEmpty()) {
            const QByteArray pw = password.toUtf8();
            RARSetPassword(h, const_cast<char *>(pw.constData()));
        }
        return true;
    }
    ~UnrarReader() { if (h) RARCloseArchive(h); }
};

QString unrarErrorText(unsigned int code)
{
    switch (code) {
    case ERAR_SUCCESS:           return QStringLiteral("成功");
    case ERAR_END_ARCHIVE:       return QStringLiteral("归档结束");
    case ERAR_NO_MEMORY:         return QStringLiteral("内存不足");
    case ERAR_BAD_DATA:          return QStringLiteral("数据损坏");
    case ERAR_BAD_ARCHIVE:       return QStringLiteral("不是有效归档");
    case ERAR_UNKNOWN_FORMAT:    return QStringLiteral("未知格式");
    case ERAR_EOPEN:             return QStringLiteral("打开失败");
    case ERAR_MISSING_PASSWORD:  return QStringLiteral("缺少密码");
    case ERAR_BAD_PASSWORD:      return QStringLiteral("密码错误");
    case ERAR_LARGE_DICT:        return QStringLiteral("字典过大（被拒绝）");
    default:                     return QStringLiteral("错误码 %1").arg(code);
    }
}

} // namespace

ArchiveSource::~ArchiveSource()
{
    close();
}

bool ArchiveSource::isArchive(const QString &path)
{
    static const QSet<QString> exts = {
        QStringLiteral("zip"), QStringLiteral("cbz"), QStringLiteral("rar"),
        QStringLiteral("cbr"), QStringLiteral("7z"),  QStringLiteral("cb7"),
        QStringLiteral("tar"), QStringLiteral("cbt"), QStringLiteral("gz"),
        QStringLiteral("bz2"), QStringLiteral("xz"),  QStringLiteral("zst"),
    };
    return exts.contains(QFileInfo(path).suffix().toLower());
}

// ─────────────────────────────────────────────────────────────
// 打开：RAR 走 libunrar；其余走 libarchive（失败再回退 libunrar）
// ─────────────────────────────────────────────────────────────
bool ArchiveSource::open(const QString &path, const QString &password, Entries entries,
                         ProgressFn progress)
{
    QMutexLocker lock(&m_mutex);
    close();
    m_password = password;

    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool isRar = (suffix == QLatin1String("rar") || suffix == QLatin1String("cbr"));

    if (isRar) {
        if (!openUnrar(path, password, entries, progress)) return false;
    } else {
        if (!openLibarchive(path, password, entries, progress)) {
            // 枚举被取消（不是"打不开"）⇒ 不要再回退 / 不要再报错
            if (progress && !progress()) return false;
            qInfo().noquote() << QStringLiteral("[archive] libarchive 失败，回退 libunrar");
            if (!openUnrar(path, password, entries, progress)) return false;
        }
    }

    if (m_names.isEmpty()) {
        qWarning().noquote() << QStringLiteral("[archive] 无可用条目: %1").arg(path);
        close();
        return false;
    }

    m_path = QFileInfo(path).absoluteFilePath();
    m_reopenPath = m_path;                       // 顺序游标回退时重开的就是它
    qInfo().noquote() << QStringLiteral("[archive] %1  后端=%2  格式=%3  图片数=%4  加密=%5")
                             .arg(QFileInfo(path).fileName(),
                                  m_backend == Backend::Unrar ? QStringLiteral("libunrar")
                                                              : QStringLiteral("libarchive"),
                                  m_format)
                             .arg(m_names.size())
                             .arg(m_encrypted ? QStringLiteral("是") : QStringLiteral("否"));
    qInfo().noquote() << QStringLiteral("           首=%1").arg(m_names.first());
    qInfo().noquote() << QStringLiteral("           末=%1").arg(m_names.last());
    return true;
}

int ArchiveSource::countUnlocked() const
{
    return m_names.size();
}

// ★ v7：从内存字节打开（嵌套压缩包）。**两个后端由本类自己分派 —— 上层零分派**：
//   · RAR（按**魔数**判定，不看扩展名）⇒ 落临时文件 + `openUnrar`（libunrar 只能按路径开）；
//   · 其余 ⇒ libarchive 的 `archive_read_open_memory`（不写入磁盘）。
bool ArchiveSource::openBytes(const QByteArray &bytes, const QString &label,
                              const QString &password, Entries entries, ProgressFn progress)
{
    QMutexLocker lock(&m_mutex);
    close();
    if (bytes.isEmpty()) return false;
    m_password = password;
    if (looksLikeRar(bytes))
        return openUnrarBytes(bytes, label, password, entries, progress);

    m_memory = true;
    m_bytes = bytes;                       // 自己持有一份（`archive_read_open_memory` 只借指针）
    if (!openLibarchiveBytes(m_bytes, password, entries, progress)) {
        close();
        return false;
    }
    m_path = label;
    qInfo().noquote() << QStringLiteral("[archive] 内存容器 %1  格式=%2  条目数=%3")
                             .arg(label.isEmpty() ? QStringLiteral("(无标签)") : label,
                                  m_format)
                             .arg(m_names.size());
    return true;
}

// ── libarchive 后端 ──────────────────────────────────────────
//   列出条目的**唯一实现**：文件 / 内存两种来源只差 ArcReader 怎么打开。
static bool listLibarchive(ArcReader &r, ArchiveSource::Entries entries, QStringList *names,
                           QVector<qint64> *sizes, QVector<int> *arcIndex, QString *format,
                           bool *encrypted, const ArchiveSource::ProgressFn &progress)
{
    struct archive_entry *entry = nullptr;
    int idx = 0, rc = ARCHIVE_OK;
    while ((rc = archive_read_next_header(r.a, &entry)) == ARCHIVE_OK) {
        // ★★ **枚举期间也在推进**：每 `kProgressEvery` 条汇报一次（生产者 = 扫描线程）。
        //   返回 false ⇒ 立刻停止枚举（取消 / 已放弃），**不再碰任何状态**。
        if (++idx % ArchiveSource::kProgressEvery == 0 && progress && !progress()) {
            qInfo().noquote()
                << QStringLiteral("[archive/libarchive] 枚举被取消（已读 %1 条）").arg(idx);
            return false;
        }
        const char *raw = archive_entry_pathname(entry);
        if (raw) {
            const QString name = QString::fromUtf8(raw);
            const bool keep = (entries == ArchiveSource::Entries::All)
                                  ? !name.endsWith(QLatin1Char('/'))
                                  : (!name.endsWith(QLatin1Char('/'))
                                     && isImageSuffix(QFileInfo(name).suffix()));
            if (keep) {
                *names << name;
                *sizes << qint64(archive_entry_size(entry));
                *arcIndex << idx - 1;
            }
        }
        archive_read_data_skip(r.a);
    }
    if (names->isEmpty()) {
        qWarning().noquote() << QStringLiteral("[archive/libarchive] 无可用条目 (rc=%1 %2)")
                                    .arg(rc).arg(archiveError(r.a));
        return false;
    }
    const char *fn = archive_format_name(r.a);
    *format = fn ? QString::fromUtf8(fn) : QStringLiteral("?");
    *encrypted = archive_read_has_encrypted_entries(r.a) == 1;
    return true;
}

bool ArchiveSource::openLibarchive(const QString &path, const QString &password, Entries entries,
                                   const ProgressFn &progress)
{
    ArcReader r;
    if (!r.open(path, password)) {
        qWarning().noquote() << QStringLiteral("[archive/libarchive] 打开失败: %1 (%2)")
                                    .arg(path, archiveError(r.a));
        return false;
    }
    if (!listLibarchive(r, entries, &m_names, &m_sizes, &m_archiveOrder, &m_format, &m_encrypted,
                        progress))
        return false;
    m_backend = Backend::Libarchive;
    sortNames();
    return true;
}

bool ArchiveSource::openLibarchiveBytes(const QByteArray &bytes, const QString &password,
                                        Entries entries, const ProgressFn &progress)
{
    ArcReader r;
    if (!r.openMemory(bytes, password)) {
        qWarning().noquote() << QStringLiteral("[archive/libarchive] 内存打开失败 (%1)")
                                    .arg(archiveError(r.a));
        return false;
    }
    if (!listLibarchive(r, entries, &m_names, &m_sizes, &m_archiveOrder, &m_format, &m_encrypted,
                        progress))
        return false;
    m_backend = Backend::Libarchive;
    sortNames();
    return true;
}

// ── libunrar 后端 ────────────────────────────────────────────
bool ArchiveSource::openUnrar(const QString &path, const QString &password, Entries entries,
                              const ProgressFn &progress)
{
    UnrarReader r;
    if (!r.open(path, password, RAR_OM_LIST)) {
        qWarning().noquote() << QStringLiteral("[archive/libunrar] 打开失败: %1")
                                    .arg(unrarErrorText(r.openResult));
        return false;
    }

    QVector<QString> names;
    QVector<qint64> sizes;
    QVector<int> arcIndex;
    RARHeaderDataEx hdr;
    int idx = 0;
    bool anyEncrypted = false;
    int rc = 0;
    while (true) {
        // ★ 必须先清零：CmtBuf/CmtBufSize 是**输入输出**字段，
        //   未初始化会让库拿垃圾指针去写（实测段错误）
        memset(&hdr, 0, sizeof(hdr));
        rc = RARReadHeaderEx(r.h, &hdr);
        if (rc != 0) break;
        // ★★ **枚举期间也在推进**：每 `kProgressEvery` 条汇报一次（生产者 = 扫描线程）。
        //   返回 false ⇒ 立刻停止枚举（取消 / 已放弃），**不再碰任何状态**。
        if (++idx % ArchiveSource::kProgressEvery == 0 && progress && !progress()) {
            qInfo().noquote()
                << QStringLiteral("[archive/libunrar] 枚举被取消（已读 %1 条）").arg(idx);
            return false;
        }
        // ★ 用宽字符文件名（中文必需）
        const QString name = QString::fromWCharArray(hdr.FileNameW);
        const qint64 size = (qint64(hdr.UnpSizeHigh) << 32) | qint64(hdr.UnpSize);
        const bool keep = (entries == Entries::All)
                              ? !(hdr.Flags & RHDF_DIRECTORY)
                              : (!(hdr.Flags & RHDF_DIRECTORY)
                                 && isImageSuffix(QFileInfo(name).suffix()));
        if (keep) {
            names << name;
            sizes << size;
            arcIndex << idx - 1;
        }
        if (hdr.Flags & RHDF_ENCRYPTED) anyEncrypted = true;
        RARProcessFile(r.h, RAR_SKIP, nullptr, nullptr);
    }
    if (names.isEmpty()) {
        qWarning().noquote() << QStringLiteral("[archive/libunrar] 无图片条目 (rc=%1 %2)")
                                    .arg(rc).arg(unrarErrorText(unsigned(rc)));
        return false;
    }

    m_names = names;
    m_sizes = sizes;
    m_archiveOrder = arcIndex;
    m_format = QStringLiteral("RAR (libunrar)");
    m_encrypted = anyEncrypted;
    // ★ RAR 的 solid 标记（用户 2026-10-13 定的原则：**能随机读的才支持**）
    m_solid = (r.flags & ROADF_SOLID) != 0;
    if (m_solid)
        qInfo().noquote() << QStringLiteral("[archive/libunrar] %1 是 **solid**（不能随机读）")
                                 .arg(QFileInfo(path).fileName());
    m_backend = Backend::Unrar;
    sortNames();
    return true;
}

// ★ v7：RAR 的**内存通路** —— libunrar 没有内存打开 API，所以这里自己落一个临时文件，
//   再用 `RAROpenArchiveEx(路径)` 打开。临时文件由 `m_temp`（本对象）拥有：
//   唯一名字由 QTemporaryFile 生成（**不做确定性命名 / 存在性检查**），
//   `close()` / 析构即删除。写完 `close()` 让句柄落下 —— 别的库（libunrar）要按路径开它。
bool ArchiveSource::openUnrarBytes(const QByteArray &bytes, const QString &label,
                                   const QString &password, Entries entries,
                                   const ProgressFn &progress)
{
    m_temp = new QTemporaryFile(QDir::tempPath() + QStringLiteral("/ib-wire-inner-XXXXXX.rar"));
    if (!m_temp->open()) {
        qWarning().noquote() << QStringLiteral("[archive/libunrar] 内存容器落临时文件失败: %1")
                                    .arg(m_temp->errorString());
        delete m_temp; m_temp = nullptr;
        return false;
    }
    const QString tempPath = m_temp->fileName();
    if (m_temp->write(bytes) != bytes.size()) {
        qWarning().noquote() << QStringLiteral("[archive/libunrar] 临时文件写入不完整: %1")
                                    .arg(tempPath);
        delete m_temp; m_temp = nullptr;         // 析构即删
        return false;
    }
    m_temp->close();

    if (!openUnrar(tempPath, password, entries, progress)) {
        delete m_temp; m_temp = nullptr;         // 打不开 ⇒ 立刻删掉（不留垃圾）
        return false;
    }
    m_reopenPath = tempPath;                     // ★ 顺序游标回退时重开的是**临时文件**
    m_path = label;                              // 对外只认 label（临时文件名不出现在日志/路径里）
    qInfo().noquote() << QStringLiteral("[archive] 内存容器(RAR) %1  条目数=%2  临时文件=%3")
                             .arg(label.isEmpty() ? QStringLiteral("(无标签)") : label)
                             .arg(m_names.size())
                             .arg(QFileInfo(tempPath).fileName());
    return true;
}

// 自然排序（归档内顺序可能是乱的——本项目的测试 RAR 就是）
void ArchiveSource::sortNames()
{
    const int n = m_names.size();
    QVector<int> order(n);
    for (int i = 0; i < n; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [this](int x, int y) {
        return naturalCompare(m_names[x], m_names[y]) < 0;
    });
    QStringList sn;
    QVector<qint64> ss;
    QVector<int> so;
    sn.reserve(n); ss.reserve(n); so.reserve(n);
    for (int i : order) { sn << m_names[i]; ss << m_sizes[i]; so << m_archiveOrder[i]; }
    m_names = sn; m_sizes = ss; m_archiveOrder = so;
}

void ArchiveSource::close()
{
    clearCursors();                              // 顺序游标一起清（reader 句柄在析构里关）
    delete m_temp;                               // ★ v7：RAR 内存通路的临时文件（删除即删文件）
    m_temp = nullptr;
    m_solid = false;
    m_path.clear();
    m_reopenPath.clear();
    m_bytes.clear();
    m_memory = false;
    m_names.clear();
    m_sizes.clear();
    m_archiveOrder.clear();
    m_format.clear();
    m_encrypted = false;
    m_backend = Backend::None;
}

QString ArchiveSource::nameAt(int index) const
{
    QMutexLocker lock(&m_mutex);
    if (index < 0 || index >= m_names.size()) return QString();
    return m_names.at(index);
}

qint64 ArchiveSource::sizeAt(int index) const
{
    QMutexLocker lock(&m_mutex);
    if (index < 0 || index >= m_sizes.size()) return 0;
    return m_sizes.at(index);
}

QByteArray ArchiveSource::dataAt(int index) const
{
    QMutexLocker lock(&m_mutex);
    if (index < 0 || index >= m_names.size()) return {};
    return m_backend == Backend::Unrar ? dataUnrar(index) : dataLibarchive(index);
}

// 顺序游标：扫描是 0,1,2… **顺序**读的 ⇒ 复用同一个 reader 往前走，
//   不必每条都"重开 + 从头走到 i"（那对每个格式都是白花的钱 —— 用户 2026-10-13）
struct ArcSeq {
    ArcReader r;
    int pos = 0;                       // 已读过头的条目数（= 下一个要读的归档序号）
};
struct UnrarSeq {
    UnrarReader r;
    int pos = 0;
};

void ArchiveSource::clearCursors()
{
    delete static_cast<ArcSeq *>(m_seqArc);
    m_seqArc = nullptr;
    delete static_cast<UnrarSeq *>(m_seqUnrar);
    m_seqUnrar = nullptr;
}

QByteArray ArchiveSource::dataLibarchive(int index) const
{
    const int want = m_archiveOrder.at(index);
    const qint64 wantSize = m_sizes.at(index);

    auto *seq = static_cast<ArcSeq *>(m_seqArc);
    if (!seq || want < seq->pos) {                  // 没开 / 要回退 ⇒ 重开（回退只能重来）
        delete seq;
        seq = new ArcSeq;
        const bool ok = m_memory ? seq->r.openMemory(m_bytes, m_password)
                                 : seq->r.open(m_path, m_password);
        if (!ok) {
            qWarning().noquote() << QStringLiteral("[archive/libarchive] 打开失败: %1")
                                        .arg(archiveError(seq->r.a));
            delete seq;
            m_seqArc = nullptr;
            return {};
        }
        m_seqArc = seq;
    }
    struct archive_entry *entry = nullptr;
    int rc = ARCHIVE_OK;
    while (seq->pos <= want) {
        rc = archive_read_next_header(seq->r.a, &entry);
        if (rc != ARCHIVE_OK) break;
        const int i = seq->pos;
        if (i == want) {
            // 注意：archive_read_data 允许返回少于请求的字节数，**必须循环读满**
            QByteArray out;
            out.resize(int(wantSize));
            qint64 total = 0;
            bool bad = false;
            while (total < wantSize) {
                const la_ssize_t got = archive_read_data(seq->r.a, out.data() + total,
                                                         size_t(wantSize - total));
                if (got < 0) {
                    qWarning().noquote() << QStringLiteral("[archive/libarchive] 解出 %1 出错: %2")
                                                .arg(m_names.at(index), archiveError(seq->r.a));
                    bad = true;
                    break;
                }
                if (got == 0) break;
                total += got;
            }
            ++seq->pos;                             // 游标前进（下一条接着读）
            if (bad || total != wantSize) {
                if (!bad)
                    qWarning().noquote() << QStringLiteral("[archive/libarchive] 解出 %1 不完整: %2/%3")
                                                .arg(m_names.at(index)).arg(total).arg(wantSize);
                delete seq;                         // 出错 ⇒ 游标作废（下次重开）
                m_seqArc = nullptr;
                return {};
            }
            return out;
        }
        archive_read_data_skip(seq->r.a);
        ++seq->pos;
    }
    qWarning().noquote() << QStringLiteral("[archive/libarchive] 未找到条目 %1 (rc=%2)")
                                .arg(m_names.at(index)).arg(rc);
    delete seq;
    m_seqArc = nullptr;
    return {};
}

QByteArray ArchiveSource::dataUnrar(int index) const
{
    const int want = m_archiveOrder.at(index);
    const qint64 wantSize = m_sizes.at(index);

    auto *seq = static_cast<UnrarSeq *>(m_seqUnrar);
    if (!seq || want < seq->pos) {
        delete seq;
        seq = new UnrarSeq;
        if (!seq->r.open(m_reopenPath, m_password, RAR_OM_EXTRACT)) {
            qWarning().noquote() << QStringLiteral("[archive/libunrar] 打开失败: %1")
                                        .arg(unrarErrorText(seq->r.openResult));
            delete seq;
            m_seqUnrar = nullptr;
            return {};
        }
        m_seqUnrar = seq;
    }

    QByteArray out;
    out.reserve(int(wantSize));
    seq->r.ctx.out = &out;
    seq->r.ctx.expected = wantSize;

    RARHeaderDataEx hdr;
    int rc = 0;
    while (seq->pos <= want) {
        memset(&hdr, 0, sizeof(hdr));     // 同上：必须先清零
        rc = RARReadHeaderEx(seq->r.h, &hdr);
        if (rc != 0) break;
        const int i = seq->pos;
        if (i == want) {
            // RAR_TEST 会经回调把解压数据交给我们（不写入磁盘）
            const int pr = RARProcessFile(seq->r.h, RAR_TEST, nullptr, nullptr);
            ++seq->pos;                             // 游标前进
            if (pr != ERAR_SUCCESS) {
                qWarning().noquote() << QStringLiteral("[archive/libunrar] 解出 %1 失败: %2")
                                            .arg(m_names.at(index), unrarErrorText(unsigned(pr)));
                delete seq;
                m_seqUnrar = nullptr;
                return {};
            }
            if (out.size() != wantSize) {
                qWarning().noquote() << QStringLiteral("[archive/libunrar] 解出 %1 不完整: %2/%3")
                                            .arg(m_names.at(index)).arg(out.size()).arg(wantSize);
                delete seq;
                m_seqUnrar = nullptr;
                return {};
            }
            return out;
        }
        RARProcessFile(seq->r.h, RAR_SKIP, nullptr, nullptr);
        ++seq->pos;
    }
    qWarning().noquote() << QStringLiteral("[archive/libunrar] 未找到条目 %1 (rc=%2)")
                                .arg(m_names.at(index)).arg(rc);
    delete seq;
    m_seqUnrar = nullptr;
    return {};
}
