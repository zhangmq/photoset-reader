#include "archive.h"
#include "natural.h"

#include <QFileInfo>
#include <QDebug>
#include <QMutexLocker>
#include <algorithm>

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
bool ArchiveSource::open(const QString &path, const QString &password)
{
    QMutexLocker lock(&m_mutex);
    close();
    m_password = password;

    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool isRar = (suffix == QLatin1String("rar") || suffix == QLatin1String("cbr"));

    if (isRar) {
        if (!openUnrar(path, password)) return false;
    } else {
        if (!openLibarchive(path, password)) {
            qInfo().noquote() << QStringLiteral("[archive] libarchive 失败，回退 libunrar");
            if (!openUnrar(path, password)) return false;
        }
    }

    if (m_names.isEmpty()) {
        qWarning().noquote() << QStringLiteral("[archive] 无可用图片条目: %1").arg(path);
        close();
        return false;
    }

    m_path = QFileInfo(path).absoluteFilePath();
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

// ── libarchive 后端 ──────────────────────────────────────────
bool ArchiveSource::openLibarchive(const QString &path, const QString &password)
{
    ArcReader r;
    if (!r.open(path, password)) {
        qWarning().noquote() << QStringLiteral("[archive/libarchive] 打开失败: %1 (%2)")
                                    .arg(path, archiveError(r.a));
        return false;
    }

    struct archive_entry *entry = nullptr;
    QVector<QString> names;
    QVector<qint64> sizes;
    QVector<int> arcIndex;
    int idx = 0, rc = ARCHIVE_OK;
    while ((rc = archive_read_next_header(r.a, &entry)) == ARCHIVE_OK) {
        const char *raw = archive_entry_pathname(entry);
        if (raw) {
            const QString name = QString::fromUtf8(raw);
            if (!name.endsWith(QLatin1Char('/')) && isImageSuffix(QFileInfo(name).suffix())) {
                names << name;
                sizes << qint64(archive_entry_size(entry));
                arcIndex << idx;
            }
        }
        archive_read_data_skip(r.a);
        ++idx;
    }
    if (names.isEmpty()) {
        qWarning().noquote() << QStringLiteral("[archive/libarchive] 无图片条目 (rc=%1 %2)")
                                    .arg(rc).arg(archiveError(r.a));
        return false;
    }

    m_names = names;
    m_sizes = sizes;
    m_archiveOrder = arcIndex;
    const char *fn = archive_format_name(r.a);
    m_format = fn ? QString::fromUtf8(fn) : QStringLiteral("?");
    m_encrypted = archive_read_has_encrypted_entries(r.a) == 1;
    m_backend = Backend::Libarchive;
    sortNames();
    return true;
}

// ── libunrar 后端 ────────────────────────────────────────────
bool ArchiveSource::openUnrar(const QString &path, const QString &password)
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
        // ★ 用宽字符文件名（中文必需）
        const QString name = QString::fromWCharArray(hdr.FileNameW);
        const qint64 size = (qint64(hdr.UnpSizeHigh) << 32) | qint64(hdr.UnpSize);
        if (!(hdr.Flags & RHDF_DIRECTORY) && isImageSuffix(QFileInfo(name).suffix())) {
            names << name;
            sizes << size;
            arcIndex << idx;
        }
        if (hdr.Flags & RHDF_ENCRYPTED) anyEncrypted = true;
        RARProcessFile(r.h, RAR_SKIP, nullptr, nullptr);
        ++idx;
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
    m_backend = Backend::Unrar;
    sortNames();
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
    m_path.clear();
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

QByteArray ArchiveSource::dataLibarchive(int index) const
{
    const int want = m_archiveOrder.at(index);
    const qint64 wantSize = m_sizes.at(index);

    ArcReader r;
    if (!r.open(m_path, m_password)) {
        qWarning().noquote() << QStringLiteral("[archive/libarchive] 重开失败: %1")
                                    .arg(archiveError(r.a));
        return {};
    }
    struct archive_entry *entry = nullptr;
    int i = 0, rc = ARCHIVE_OK;
    while ((rc = archive_read_next_header(r.a, &entry)) == ARCHIVE_OK) {
        if (i == want) {
            // 注意：archive_read_data 允许返回少于请求的字节数，**必须循环读满**
            QByteArray out;
            out.resize(int(wantSize));
            qint64 total = 0;
            while (total < wantSize) {
                const la_ssize_t got = archive_read_data(r.a, out.data() + total,
                                                         size_t(wantSize - total));
                if (got < 0) {
                    qWarning().noquote() << QStringLiteral("[archive/libarchive] 解出 %1 出错: %2")
                                                .arg(m_names.at(index), archiveError(r.a));
                    return {};
                }
                if (got == 0) break;
                total += got;
            }
            if (total != wantSize) {
                qWarning().noquote() << QStringLiteral("[archive/libarchive] 解出 %1 不完整: %2/%3")
                                            .arg(m_names.at(index)).arg(total).arg(wantSize);
                return {};
            }
            return out;
        }
        archive_read_data_skip(r.a);
        ++i;
    }
    qWarning().noquote() << QStringLiteral("[archive/libarchive] 未找到条目 %1 (rc=%2)")
                                .arg(m_names.at(index)).arg(rc);
    return {};
}

QByteArray ArchiveSource::dataUnrar(int index) const
{
    const int want = m_archiveOrder.at(index);
    const qint64 wantSize = m_sizes.at(index);

    UnrarReader r;
    if (!r.open(m_path, m_password, RAR_OM_EXTRACT)) {
        qWarning().noquote() << QStringLiteral("[archive/libunrar] 重开失败: %1")
                                    .arg(unrarErrorText(r.openResult));
        return {};
    }

    QByteArray out;
    out.reserve(int(wantSize));
    r.ctx.out = &out;
    r.ctx.expected = wantSize;

    RARHeaderDataEx hdr;
    int i = 0, rc = 0;
    while (true) {
        memset(&hdr, 0, sizeof(hdr));     // 同上：必须先清零
        rc = RARReadHeaderEx(r.h, &hdr);
        if (rc != 0) break;
        if (i == want) {
            // RAR_TEST 会经回调把解压数据交给我们（不落盘）
            const int pr = RARProcessFile(r.h, RAR_TEST, nullptr, nullptr);
            if (pr != ERAR_SUCCESS) {
                qWarning().noquote() << QStringLiteral("[archive/libunrar] 解出 %1 失败: %2")
                                            .arg(m_names.at(index), unrarErrorText(unsigned(pr)));
                return {};
            }
            if (out.size() != wantSize) {
                qWarning().noquote() << QStringLiteral("[archive/libunrar] 解出 %1 不完整: %2/%3")
                                            .arg(m_names.at(index)).arg(out.size()).arg(wantSize);
                return {};
            }
            return out;
        }
        RARProcessFile(r.h, RAR_SKIP, nullptr, nullptr);
        ++i;
    }
    qWarning().noquote() << QStringLiteral("[archive/libunrar] 未找到条目 %1 (rc=%2)")
                                .arg(m_names.at(index)).arg(rc);
    return {};
}
