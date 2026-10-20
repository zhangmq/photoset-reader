#include "imageprovider.h"

#include "archive.h"
#include "natural.h"        // isImageSuffix（解析封面 id 用）           // 压缩包（按路径取"包内第 0 页"）
#include "librarystore.h"      // 缩略图规格 / thumb 表 / 封面用的库连接
#include "thumburl.h"

#include <QDir>
#include <QFileInfo>
#include <QColorSpace>
#include <QImageReader>
#include <QImageWriter>
#include <QImageIOHandler>
#include <QBuffer>
#include <QThread>
#include <QThreadStorage>
#include <QUrl>
#include <QDebug>
#include <algorithm>
#include <memory>

// ─────────────────────────────────────────────────────────────
// 文件内私有设施
// ─────────────────────────────────────────────────────────────
namespace {

// 归档打开串行化：大字典归档（本项目素材 256MB）同时开多个会爆内存。
// `prepare()`（压缩包条目取第一张）与封面路径（pathThumb）共用**同一把锁**。
QMutex &archiveOpenMutex()
{
    static QMutex m;
    return m;
}

// "包内第 0 页"的**唯一判定**：优先第一个**图片后缀**的成员（跳过 ComicInfo.xml /
// __MACOSX 之类的非图片条目），都没有才退回归档内的第 0 项。找不到返回 -1。
int firstImageMemberIndex(const ArchiveSource &a)
{
    for (int i = 0; i < a.count(); ++i)
        if (isImageSuffix(QFileInfo(a.nameAt(i)).suffix())) return i;
    return a.count() > 0 ? 0 : -1;
}

// 缩略图库（`thumb` 表）的连接：**每线程一个**（QtSql 的规矩：连接不能跨线程；
// QQuickImageProvider::requestImage 可能跑在 Qt 的图像加载线程上）。
// 打不开就退化成"只在内存里"，主流程不受影响（`tried` 保证失败**不重试**）。
struct CoverStoreSlot {
    bool tried = false;
    std::unique_ptr<library::Store> store;      // 本线程的连接（线程退出即析构）
};

QThreadStorage<CoverStoreSlot *> &coverStores()
{
    static QThreadStorage<CoverStoreSlot *> tls;
    return tls;
}

library::Store *coverStore()
{
    CoverStoreSlot *slot = coverStores().localData();
    if (!slot) {
        slot = new CoverStoreSlot;
        coverStores().setLocalData(slot);
    }
    if (!slot->tried) {
        slot->tried = true;
        auto s = std::make_unique<library::Store>();
        QString err;
        if (!s->open(&err, /*quiet=*/true)) {
            qWarning().noquote()
                << QStringLiteral("[cover] 缩略图库不可用（封面不写入磁盘缓存）: %1").arg(err);
        } else {
            slot->store = std::move(s);
        }
    }
    return slot->store.get();
}

// 释放**本线程**的缓存连接：别把 SQLite 连接留到 QCoreApplication 之后才关
//（那时 QSqlDatabase 会报 "requires a QCoreApplication"）。下次请求会重新打开。
void coverStoreReleaseCurrentThread()
{
    if (!coverStores().hasLocalData()) return;
    CoverStoreSlot *slot = coverStores().localData();
    if (!slot) return;
    slot->store.reset();
    slot->tried = false;
}

// 百分号编解码 = `thumburl.h` 里的**唯一实现**（URL 格式也只有那一份）
inline QString pctEncode(const QString &s) { return thumbPctEncode(s); }
inline QString pctDecode(const QString &s) { return thumbPctDecode(s); }

// 封面源：一个普通文件，或压缩包里的一个成员
struct CoverSource {
    bool    valid = false;
    bool    zip = false;
    QString filePath;      // zip=false：绝对路径
    QString archivePath;   // zip=true：绝对路径
    QString member;        // zip=true：成员名（空 = 包内第 0 页）
    qint64  mtime = 0;     // 源 mtime（普通文件本身 / 压缩包文件）
    QString itemKey;       // thumb 表的 item_key
};

// 解析 id（设计 11 §5.2）。**不看内容、不打开压缩包**：只 stat 文件。
CoverSource parseCoverId(const QString &id)
{
    CoverSource cs;
    const int slash = id.indexOf(QLatin1Char('/'));
    if (slash <= 0) return cs;
    const QString kind = id.left(slash);
    const QString rest = id.mid(slash + 1);
    if (rest.isEmpty()) return cs;

    if (kind == QLatin1String("thumbpath")) {
        // 规范形式是百分号编码；容错：解码后不存在就按原样再试一次
        QString p = pctDecode(rest);
        if (!QFileInfo::exists(p) && QFileInfo::exists(rest)) p = rest;
        const QFileInfo fi(p);
        if (!fi.isFile()) return cs;
        cs.filePath = fi.absoluteFilePath();
        cs.mtime = fi.lastModified().toSecsSinceEpoch();
        cs.itemKey = QStringLiteral("path:") + cs.filePath;
        cs.valid = true;
        return cs;
    }

    if (kind == QLatin1String("thumbzip")) {
        // 分隔符是第一个**未编码**的 '/'：规范形式下路径里的 '/' 必是 %2F，
        // 所以先按第一个 '/' 切；不成立（调用方塞了未编码的绝对路径）再从左往右
        // 逐段找"真正存在的压缩包"。成员为空 = 取包内第 0 页。
        QVector<int> splits;
        splits << rest.indexOf(QLatin1Char('/'));
        for (int i = 0; i < rest.size(); ++i)
            if (rest.at(i) == QLatin1Char('/')) splits << i;
        splits << rest.size();                       // 无成员的形式
        for (int at : splits) {
            if (at < 0) continue;
            const QString rawArchive = rest.left(at);
            if (rawArchive.isEmpty()) continue;
            QString archive = pctDecode(rawArchive);
            if (!QFileInfo::exists(archive) && QFileInfo::exists(rawArchive))
                archive = rawArchive;
            const QFileInfo fi(archive);
            if (!fi.isFile() || !ArchiveSource::isArchive(archive)) continue;
            cs.zip = true;
            cs.archivePath = fi.absoluteFilePath();
            cs.member = (at < rest.size()) ? pctDecode(rest.mid(at + 1)) : QString();
            cs.mtime = fi.lastModified().toSecsSinceEpoch();
            cs.itemKey = QStringLiteral("zip:") + cs.archivePath + QStringLiteral("!/") + cs.member;
            cs.valid = true;
            return cs;
        }
    }
    return cs;
}

} // namespace

// ─────────────────────────────────────────────────────────────
// 构造 / 析构
// ─────────────────────────────────────────────────────────────
ImageProvider::ImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

ImageProvider::~ImageProvider()
{
    coverStoreReleaseCurrentThread();   // 别把 SQLite 连接留到 QCoreApplication 之后
}

QImage ImageProvider::cacheGet(const QString &key)
{
    auto it = m_cache.find(key);
    if (it == m_cache.end()) return QImage();
    m_lru.removeOne(key);
    m_lru.append(key);
    return it.value();
}

void ImageProvider::cachePut(const QString &key, const QImage &img)
{
    if (img.isNull()) return;
    const qint64 bytes = qint64(img.sizeInBytes());
    if (bytes > m_cacheBudget) return;

    if (m_cache.contains(key)) {
        m_cacheBytes -= qint64(m_cache.value(key).sizeInBytes());
        m_lru.removeOne(key);
    }
    m_cache.insert(key, img);
    m_lru.append(key);
    m_cacheBytes += bytes;

    while (m_cacheBytes > m_cacheBudget && !m_lru.isEmpty()) {
        const QString victim = m_lru.takeFirst();
        auto vit = m_cache.find(victim);
        if (vit != m_cache.end()) {
            m_cacheBytes -= qint64(vit.value().sizeInBytes());
            m_cache.erase(vit);
        }
    }
}

// ─────────────────────────────────────────────────────────────
// requestImage：**只服务缩略图**（图集卡片封面 / 浏览网格的缩略图）
//   ★ 定案（phase B）：本 provider **不再提供"页面取图"**（旧的 `view/<条目号>` /
//     `slice/<条目号>/<片号>` 取图 URL 已删）—— 阅读屏的图由 viewer
//     （`fetch::ViewerModel` / `imageread::image`）直接给 QML，不走 image provider。
//     provider 只留**缩略图能力**，保留 `src/thumburl.{h,cpp}` 那条（按路径取封面）。
// ─────────────────────────────────────────────────────────────
QImage ImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    Q_UNUSED(requestedSize);
    // ★ **只认"按路径"的缩略图 id**（`thumbpath/` / `thumbzip/`）。
    //   按序号的 `thumb/<n>` 随"provider 自建序列"一起删了（阅读屏不走 provider）。
    if (!isPathThumbId(id)) {
        qWarning().noquote()
            << QStringLiteral("[thumb] 不认识的取图 id（只支持 thumbpath / thumbzip）: %1").arg(id);
        if (size) *size = QSize();
        return QImage();
    }
    return pathThumb(id, size);
}

// ─────────────────────────────────────────────────────────────
// 缩略图规格（设计 11 §5.1，唯一来源：library::Store 的三个常量）
// ─────────────────────────────────────────────────────────────
int ImageProvider::thumbSizeClass(const QSize &requestedSize)
{
    Q_UNUSED(requestedSize);        // ★ 不按 UI 尺寸生成：恒为 kThumbSize
    return library::Store::kThumbSize;
}

QByteArray ImageProvider::encodeThumbBlob(const QImage &img)
{
    if (img.isNull()) return QByteArray();
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    QImageWriter writer(&buf, QByteArrayLiteral("webp"));
    writer.setQuality(library::Store::kThumbQuality);   // ★ q=50（设计 11 §5.1）
    if (!writer.write(img)) {
        qWarning().noquote() << QStringLiteral("[thumb] WebP 编码失败: %1")
                                    .arg(writer.errorString());
        return QByteArray();
    }
    return out;
}

// ─────────────────────────────────────────────────────────────
// 按路径取封面 / 缩略图（设计 11 §5.2）
//   · id 方案见头文件注释；编码只在这里实现
//   · 压缩包**只在缓存未命中时**才打开，且走 firstImageMemberIndex()（唯一判定）
//   · 规格 = kThumbSize 等比适应（不裁切）+ kThumbQuality，常量只在 librarystore.h
//   · 一切失败都返回空图（不崩、不重试、不写负缓存）
// ─────────────────────────────────────────────────────────────
QString ImageProvider::thumbPathId(const QString &absPath)
{
    // id（不含 provider 前缀）= 共享 URL 去掉 "image://img/"
    const QString url = thumbUrlForFile(absPath);
    return url.mid(url.indexOf(QLatin1Char('/'), 8) + 1);
}

QString ImageProvider::thumbZipId(const QString &archivePath, const QString &member)
{
    const QString url = thumbUrlForArchiveMember(archivePath, member);
    return url.mid(url.indexOf(QLatin1Char('/'), 8) + 1);
}

bool ImageProvider::isPathThumbId(const QString &id)
{
    return id.startsWith(QLatin1String("thumbpath/"))
        || id.startsWith(QLatin1String("thumbzip/"));
}

QString ImageProvider::thumbItemKey(const QString &id)
{
    return parseCoverId(id).itemKey;
}

QImage ImageProvider::pathThumb(const QString &id, QSize *outSize)
{
    const auto miss = [outSize]() {
        if (outSize) *outSize = QSize();
        return QImage();
    };

    const CoverSource cs = parseCoverId(id);
    if (!cs.valid) {
        qWarning().noquote() << QStringLiteral("[cover] id 无法解析或源不存在: %1").arg(id);
        return miss();
    }

    const QString memKey = QStringLiteral("pthumb:") + id;

    // ① 进程内 LRU（同一进程里的重复请求不再碰库/盘）
    QString archivePassword;
    {
        QMutexLocker lock(&m_mutex);
        archivePassword = m_archivePassword;
        const QImage hit = cacheGet(memKey);
        if (!hit.isNull()) {
            if (outSize) *outSize = hit.size();
            return hit;
        }
    }

    library::Store *st = coverStore();

    // ② thumb 表：键 = (item_key, kThumbSize 档位, 源 mtime)
    if (st) {
        library::Thumb row;
        if (st->thumbGet(-1, cs.itemKey, library::Store::kThumbSize, cs.mtime, &row)) {
            const QImage cached = QImage::fromData(row.bytes, "webp");
            if (!cached.isNull()) {
                m_coverDbHits.fetch_add(1, std::memory_order_relaxed);
                QMutexLocker lock(&m_mutex);
                cachePut(memKey, cached);
                if (outSize) *outSize = cached.size();
                return cached;
            }
            qWarning().noquote()
                << QStringLiteral("[cover] thumb 表里的 BLOB 解不开（当作未命中）: %1")
                       .arg(cs.itemKey);
        }
    }

    // ③ 未命中 → 取源数据。压缩包**到这里才打开**（重 I/O 只发生一次）
    QByteArray bytes;
    if (cs.zip) {
        m_coverArchiveOpens.fetch_add(1, std::memory_order_relaxed);
        QMutexLocker openLock(&archiveOpenMutex());
        ArchiveSource src;
        if (!src.open(cs.archivePath, archivePassword)) {
            qWarning().noquote() << QStringLiteral("[cover] 打不开压缩包: %1").arg(cs.archivePath);
            return miss();
        }
        int idx = -1;
        if (cs.member.isEmpty()) {
            idx = firstImageMemberIndex(src);            // 未指定成员 = 包内第 0 页
        } else {
            for (int i = 0; i < src.count(); ++i)
                if (src.nameAt(i) == cs.member) { idx = i; break; }
            if (idx < 0)                                  // 大小写不敏感兜底
                for (int i = 0; i < src.count(); ++i)
                    if (src.nameAt(i).compare(cs.member, Qt::CaseInsensitive) == 0) { idx = i; break; }
        }
        if (idx < 0) {
            qWarning().noquote() << QStringLiteral("[cover] 包内成员不存在: %1!/%2")
                                        .arg(cs.archivePath, cs.member);
            return miss();
        }
        bytes = src.dataAt(idx);
        if (bytes.isEmpty()) {
            qWarning().noquote() << QStringLiteral("[cover] 成员解出为空: %1!/%2")
                                        .arg(cs.archivePath, src.nameAt(idx));
            return miss();
        }
    }

    // ④ 解码（`QImageReader`，唯一实现）+ 缩放（**Qt 平滑**）
    //   ★ 2026-10-19（用户定案）：缩略图**不要画质**（"能看就行"）⇒ 不再走线性光那条
    //     CPU 缩放那条（已退休），改用 Qt 的平滑缩放（Qt 的 `SmoothTransformation` 那条路）——
    //     "省一小段 CPU、零新耦合"。线性光那条仍然服务**阅读画质**（在 GPU 上，`GpuScalerItem`）。
    QBuffer buf;
    QImageReader reader;
    if (cs.zip) {
        buf.setData(bytes);
        buf.open(QIODevice::ReadOnly);
        reader.setDevice(&buf);
    } else {
        reader.setFileName(cs.filePath);
    }
    reader.setAutoTransform(true);

    const QSize srcSize = reader.size();
    if (!srcSize.isValid() || srcSize.isEmpty()) {
        qWarning().noquote() << QStringLiteral("[cover] 读不到尺寸: %1").arg(cs.itemKey);
        return miss();
    }

    const QSize box(library::Store::kThumbSize, library::Store::kThumbSize);   // 等比适应、不裁切
    const QSize target = srcSize.scaled(box, Qt::KeepAspectRatio);

    bool shrink = false;
    {
        QMutexLocker lock(&m_mutex);
        shrink = m_shrinkOnLoad;
    }
    if (shrink && target.isValid() && target.width() > 0) {
        int denom = 1;
        while (denom * 2 <= 8 && srcSize.width() / (denom * 2) >= target.width())
            denom *= 2;
        if (denom > 1)
            reader.setScaledSize(QSize(srcSize.width() / denom, srcSize.height() / denom));
    }

    QImage raw = reader.read();
    // 缩略图同样要"色彩正确"（成本低：缩略图很小；只在带非 sRGB profile 时转一次）
    if (!raw.isNull() && raw.colorSpace().isValid() && raw.colorSpace() != QColorSpace::SRgb) {
        const QImage conv = raw.convertedToColorSpace(QColorSpace::SRgb);
        if (!conv.isNull())
            raw = conv;
    }
    if (raw.isNull()) {
        qWarning().noquote() << QStringLiteral("[cover] 解码失败(%1): %2")
                                    .arg(reader.errorString(), cs.itemKey);
        return miss();
    }
    //   等比适应、不裁切；缩略图不参与画质档位/pixel-perfect
    const QImage out = raw.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (out.isNull()) return miss();

    // ⑤ 写 thumb 表（失败只影响"下次是否重算"，不影响本次出图）
    //   ★ 用户 2026-10-12：缩略图是**一条管线** —— 先**全部生成**（进程内 LRU），
    //     然后**只有图集要存** ⇒ 会话来源（文件夹/压缩包）不写入磁盘。
    if (st && m_persistThumbs) {
        const QByteArray blob = encodeThumbBlob(out);
        if (blob.isEmpty()) {
            qWarning().noquote() << QStringLiteral("[cover] 缩略图编码失败，不入库: %1").arg(cs.itemKey);
        } else if (blob.size() > library::Store::kThumbMaxBytes) {
            qWarning().noquote() << QStringLiteral("[cover] 缩略图 %1 字节 > 上限 %2，不入库: %3")
                                        .arg(blob.size()).arg(library::Store::kThumbMaxBytes)
                                        .arg(cs.itemKey);
        } else {
            library::Thumb row;
            row.collectionId = -1;                       // "直接打开"这一档（库里存 NULL）
            row.itemKey = cs.itemKey;
            row.sizeClass = library::Store::kThumbSize;
            row.srcMtime = cs.mtime;
            row.w = out.width();
            row.h = out.height();
            row.bytes = blob;                            // usedAt=0 → Store 填当前时间
            st->thumbPut(row);
        }
    }

    m_coverGenerated.fetch_add(1, std::memory_order_relaxed);
    {
        QMutexLocker lock(&m_mutex);
        cachePut(memKey, out);
    }
    if (outSize) *outSize = out.size();
    return out;
}
