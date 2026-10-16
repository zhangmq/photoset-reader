#include "imageprovider.h"

#include "thumburl.h"
#include "natural.h"
#include "layout.h"

#include <QDir>
#include <QFileInfo>
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
    // 预取并发度：解码+缩放是 CPU 密集，留余量给 UI 与主线程
    m_pool.setMaxThreadCount(qMax(2, QThread::idealThreadCount() / 2));
    m_pool.setExpiryTimeout(30000);
}

ImageProvider::~ImageProvider()
{
    m_pool.waitForDone();       // 任务持有 this，必须等它们结束
    coverStoreReleaseCurrentThread();   // 别把 SQLite 连接留到 QCoreApplication 之后
}

// ─────────────────────────────────────────────────────────────
// 来源：目录 / 压缩包 / 进入压缩包条目
// ─────────────────────────────────────────────────────────────
void ImageProvider::setSource(const QString &path, bool recursive, const QString &archivePassword)
{
    QMutexLocker lock(&m_mutex);
    if (!archivePassword.isEmpty()) m_archivePassword = archivePassword;

    m_cache.clear(); m_lru.clear(); m_cacheBytes = 0;
    m_sizeCache.clear();
    m_archive.close();
    m_nested = false;
    m_sourceIsArchive = false;

    const QFileInfo fi(path);
    if (fi.isDir()) {
        m_sequence.buildFromFolder(path, recursive);
        qInfo().noquote() << QStringLiteral("[source] 目录 %1  条目 %2  递归 %3")
                                 .arg(path).arg(m_sequence.count())
                                 .arg(recursive ? QStringLiteral("是") : QStringLiteral("否"));
    } else if (fi.isFile() && ArchiveSource::isArchive(path)) {
        lock.unlock();
        const bool ok = m_archive.open(path, m_archivePassword);
        lock.relock();
        if (ok) {
            m_sourceIsArchive = true;
            QStringList names;
            names.reserve(m_archive.count());
            for (int i = 0; i < m_archive.count(); ++i) names << m_archive.nameAt(i);
            m_sequence.buildFromNames(path, names);
        }
    } else if (fi.isFile() && isImageSuffix(fi.suffix())) {
        // 单张图片：包成 1 项序列（"打开一张图"是最基本的操作）
        // 多页文件（多页 TIFF 等）按**页容器**处理：列出每一页
        QImageReader probe(fi.absoluteFilePath());
        const int pages = probe.imageCount();
        if (pages > 1) {
            m_sequence.buildFromMultiPageFile(fi.absoluteFilePath(), pages);
            qInfo().noquote() << QStringLiteral("[source] 多页文件 %1  页数 %2")
                                     .arg(fi.fileName()).arg(pages);
        } else {
            m_sequence.buildFromSingleFile(fi.absoluteFilePath());
        }
    } else {
        qWarning().noquote() << QStringLiteral("[source] 既不是目录、压缩包，也不是图片: %1").arg(path);
    }

    lock.unlock();
    rebuildPages();
    lock.relock();
    if (m_sequence.count() > 0) {
        qInfo().noquote() << QStringLiteral("         首=%1").arg(m_sequence.at(0).displayName);
        qInfo().noquote() << QStringLiteral("         末=%1")
                                 .arg(m_sequence.at(m_sequence.count() - 1).displayName);
        qInfo().noquote() << QStringLiteral("         版面 %1 页（条目 %2）")
                                 .arg(m_pages.size()).arg(m_sequence.count());
    }
}

// ★ 接缝（设计 11 §七 第 5 条）：用**显式条目列表**建序列（图集库的库行）
void ImageProvider::setSourceItems(const QString &rootPath, const QVector<Sequence::Item> &entries)
{
    QMutexLocker lock(&m_mutex);
    m_cache.clear(); m_lru.clear(); m_cacheBytes = 0;
    m_sizeCache.clear();
    m_archive.close();
    m_nested = false;
    m_sourceIsArchive = false;
    m_sequence.buildFromEntries(rootPath, entries);

    lock.unlock();
    rebuildPages();
    lock.relock();
    qInfo().noquote() << QStringLiteral("[source] 库条目 %1  条（根 %2）")
                             .arg(m_sequence.count()).arg(rootPath);
    if (m_sequence.count() > 0)
        qInfo().noquote() << QStringLiteral("         首=%1").arg(m_sequence.at(0).displayName);
}

bool ImageProvider::enterArchive(int index)
{
    QMutexLocker lock(&m_mutex);
    if (index < 0 || index >= m_sequence.count()) return false;
    const Sequence::Item it = m_sequence.at(index);
    if (it.kind != Sequence::Kind::Archive) return false;

    // 记住现场
    m_parentPath = m_sequence.rootPath();
    m_parentRecursive = m_sequence.isRecursive();
    m_parentCursor = index;

    lock.unlock();
    const bool ok = m_archive.open(it.loc.diskPath, m_archivePassword);
    lock.relock();
    if (!ok) {
        // 加密归档 + 无密码/密码错 → 让 UI 弹密码框
        const bool enc = m_archive.isEncrypted();
        m_needPassword = enc;
        m_lastError = enc ? QStringLiteral("需要密码") : QStringLiteral("无法打开压缩包");
        qWarning().noquote() << QStringLiteral("[source] 进入压缩包失败: %1 (%2)")
                                    .arg(it.loc.diskPath, m_lastError);
        return false;
    }
    m_needPassword = false;
    m_lastError.clear();

    QStringList names;
    names.reserve(m_archive.count());
    for (int i = 0; i < m_archive.count(); ++i) names << m_archive.nameAt(i);
    m_sequence.buildFromNames(it.loc.diskPath, names);
    m_nested = true;
    m_sizeCache.clear();
    qInfo().noquote() << QStringLiteral("[source] 进入压缩包 %1  内页 %2")
                             .arg(it.displayName).arg(m_sequence.count());
    return true;
}

bool ImageProvider::leaveArchive()
{
    QMutexLocker lock(&m_mutex);
    if (!m_nested) return false;
    m_archive.close();
    m_nested = false;
    m_sizeCache.clear();
    lock.unlock();
    setSource(m_parentPath, m_parentRecursive);
    qInfo().noquote() << QStringLiteral("[source] 返回上级 %1").arg(m_parentPath);
    return true;
}

int ImageProvider::refresh()
{
    QMutexLocker lock(&m_mutex);
    const QString root = m_sequence.rootPath();
    const bool rec = m_sequence.isRecursive();
    const bool wasArchive = m_sequence.isArchivePages();
    lock.unlock();

    if (wasArchive) return 0;                 // 归档内页不重建
    setSource(root, rec);
    return 0;
}

int ImageProvider::revalidate()
{
    QMutexLocker lock(&m_mutex);
    if (m_sequence.isArchivePages()) return 0;
    return m_sequence.markMissing();          // 只标记，不移除
}

Sequence ImageProvider::currentSequence() const
{
    QMutexLocker lock(&m_mutex);
    return m_sequence;                        // 快照（ViewModel 建 ItemSource 用）
}

// ─────────────────────────────────────────────────────────────
// 条目元信息
// ─────────────────────────────────────────────────────────────
int ImageProvider::countUnlocked() const { return m_sequence.count(); }

int ImageProvider::count() const
{
    QMutexLocker lock(&m_mutex);
    return countUnlocked();
}

QString ImageProvider::fileNameAtUnlocked(int index) const
{
    if (index < 0 || index >= m_sequence.count()) return QString();
    return m_sequence.at(index).displayName;
}

QString ImageProvider::fileNameAt(int index) const
{
    QMutexLocker lock(&m_mutex);
    return fileNameAtUnlocked(index);
}

bool ImageProvider::isArchiveAt(int index) const
{
    QMutexLocker lock(&m_mutex);
    if (index < 0 || index >= m_sequence.count()) return false;
    return m_sequence.at(index).kind == Sequence::Kind::Archive;
}

bool ImageProvider::isBoundaryAt(int index) const
{
    QMutexLocker lock(&m_mutex);
    if (index < 0 || index >= m_sequence.count()) return false;
    return m_sequence.at(index).kind == Sequence::Kind::FolderBoundary;
}

QString ImageProvider::folderLabelAt(int index) const
{
    QMutexLocker lock(&m_mutex);
    if (index < 0 || index >= m_sequence.count()) return QString();
    return m_sequence.at(index).folderLabel;
}

int ImageProvider::folderDepthAt(int index) const
{
    QMutexLocker lock(&m_mutex);
    if (index < 0 || index >= m_sequence.count()) return 0;
    return m_sequence.at(index).folderDepth;
}

bool ImageProvider::isMissingAt(int index) const
{
    QMutexLocker lock(&m_mutex);
    if (index < 0 || index >= m_sequence.count()) return false;
    return m_sequence.at(index).missing;
}

QString ImageProvider::relativePathAt(int index) const
{
    QMutexLocker lock(&m_mutex);
    if (index < 0 || index >= m_sequence.count()) return QString();
    const Sequence::Item &it = m_sequence.at(index);
    if (m_sequence.isArchivePages()) {
        // 压缩包内文件：用**约定的显示格式** `A!/B`（设计 03 §六："显示/日志 join 成 A!/B 只为人眼"）
        //   A = 容器（压缩包）相对图集根的路径，B = 包内路径
        return QFileInfo(m_sequence.rootPath()).fileName() + QStringLiteral("!/")
               + it.loc.members.join(QStringLiteral("!/"));
    }
    const QString root = m_sequence.rootPath();
    if (root.isEmpty())
        return it.displayName;
    return QDir(root).relativeFilePath(it.loc.diskPath);
}

QString ImageProvider::sourceLabel() const
{
    QMutexLocker lock(&m_mutex);
    return QFileInfo(m_sequence.rootPath()).fileName();
}

QString ImageProvider::sourcePath() const
{
    QMutexLocker lock(&m_mutex);
    return m_sequence.rootPath();
}

double ImageProvider::cacheMegabytes() const
{
    QMutexLocker lock(&m_mutex);
    return m_cacheBytes / 1048576.0;
}

int ImageProvider::pageIndexOf(int index) const
{
    QMutexLocker lock(&m_mutex);
    if (index < 0 || index >= m_sequence.count()) return 0;
    return m_sequence.at(index).pageIndex();
}

QString ImageProvider::absPathAt(int index) const
{
    QMutexLocker lock(&m_mutex);
    if (index < 0 || index >= m_sequence.count()) return QString();
    const Sequence::Item &it = m_sequence.at(index);
    if (m_sequence.isArchivePages() || !it.loc.members.isEmpty()) return QString();
    return it.loc.diskPath;
}

QString ImageProvider::backendLabel() const
{
    QMutexLocker lock(&m_mutex);
    if (!m_sequence.isArchivePages()) return QStringLiteral("文件夹");
    return m_archive.formatName();
}

QString ImageProvider::cacheStats() const
{
    QMutexLocker lock(&m_mutex);
    return QStringLiteral("缓存 %1 张 / %2 MB · 命中 %3 · 未命中 %4")
        .arg(m_cache.size())
        .arg(m_cacheBytes / 1048576.0, 0, 'f', 1)
        .arg(m_hits).arg(m_misses);
}

// ─────────────────────────────────────────────────────────────
// 取数据：目录模式给路径，压缩包模式解出字节
// ─────────────────────────────────────────────────────────────
bool ImageProvider::prepare(int index, QString &pathOut, QByteArray &bytesOut) const
{
    QMutexLocker lock(&m_mutex);
    if (index < 0 || index >= m_sequence.count()) return false;
    const Sequence::Item &it = m_sequence.at(index);

    if (it.kind == Sequence::Kind::FolderBoundary) return false;
    if (it.missing) return false;                              // 缺失条目直接失败，不重试

    // ★ 用户要求 #3：压缩包条目用**内页第一张**作图标
    if (it.kind == Sequence::Kind::Archive) {
        const QString path = it.loc.diskPath;
        const QString pw = m_archivePassword;
        lock.unlock();
        // 串行化：大字典归档（本项目素材 256MB）同时开多个会爆内存
        QMutexLocker probeLock(&archiveOpenMutex());
        ArchiveSource probe;                                   // 独立实例，不干扰当前序列
        if (!probe.open(path, pw) || probe.count() == 0) return false;
        const int first = firstImageMemberIndex(probe);        // "第 0 页"的唯一判定
        if (first < 0) return false;
        bytesOut = probe.dataAt(first);
        return !bytesOut.isEmpty();
    }

    if (!m_sequence.isArchivePages()) {
        pathOut = it.loc.diskPath;
        return true;
    }
    lock.unlock();
    bytesOut = m_archive.dataAt(index);
    return !bytesOut.isEmpty();
}

// ★ 尺寸的**唯一实现**（算法单一事实源）：
//   · prepare() 同时覆盖"文件系统路径"与"压缩包内数据"两种情况
//   · 必须与 decodeRaw 一致地应用 EXIF 变换（read() 会转，size() 不会）
//   · 版面判定、源尺寸展示、缓存预热全部走这里，禁止各自再写一套
QSize ImageProvider::probeSize(int index) const
{
    QByteArray bytes;
    QString path;
    if (!prepare(index, path, bytes)) return QSize();

    QBuffer buf;
    QImageReader reader;
    if (!path.isEmpty()) {
        reader.setFileName(path);
    } else {
        buf.setData(bytes);
        buf.open(QIODevice::ReadOnly);
        reader.setDevice(&buf);
    }
    if (const int pg = pageIndexOf(index); pg > 0) reader.jumpToImage(pg);

    QSize sz = reader.size();
    switch (reader.transformation()) {
    case QImageIOHandler::TransformationRotate90:
    case QImageIOHandler::TransformationRotate270:
    case QImageIOHandler::TransformationMirrorAndRotate90:
    case QImageIOHandler::TransformationFlipAndRotate90:
        sz.transpose();
        break;
    default:
        break;
    }
    return sz;
}

QSize ImageProvider::sourceSizeAt(int index) const
{
    QMutexLocker lock(&m_mutex);
    if (index < 0 || index >= m_sequence.count()) return QSize();
    const Sequence::Item &it = m_sequence.at(index);
    if (it.kind != Sequence::Kind::Image || it.missing) return QSize();

    auto cit = m_sizeCache.constFind(index);
    if (cit != m_sizeCache.constEnd()) return cit.value();

    lock.unlock();
    const QSize sz = probeSize(index);          // ★ 唯一实现
    lock.relock();
    if (sz.isValid()) m_sizeCache.insert(index, sz);
    return sz;
}

// ─────────────────────────────────────────────────────────────
// 缓存（字节有界 LRU）
// ─────────────────────────────────────────────────────────────
QImage ImageProvider::cacheGet(const QString &key)
{
    auto it = m_cache.find(key);
    if (it == m_cache.end()) return QImage();
    m_lru.removeOne(key);
    m_lru.append(key);
    ++m_hits;
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
// 解码 + 缩放（**唯一实现**）
// ─────────────────────────────────────────────────────────────
QImage ImageProvider::decodeFit(int index, const QSize &box)
{
    QString path;
    QByteArray bytes;
    if (!prepare(index, path, bytes)) {
        qWarning().noquote() << QStringLiteral("[decode] 取数据失败: #%1").arg(index);
        return QImage();
    }

    QBuffer buf;
    QImageReader reader;
    if (!path.isEmpty()) {
        reader.setFileName(path);
    } else {
        buf.setData(bytes);
        buf.open(QIODevice::ReadOnly);
        reader.setDevice(&buf);
    }
    // 多页文件（多页 TIFF 等）：跳到本条目对应的页
    if (const int pg = pageIndexOf(index); pg > 0) reader.jumpToImage(pg);
    reader.setAutoTransform(true);

    // 目标框：**等比适应**（KeepAspectRatio）到 box，**不裁切** ——
    //   缩略图也是这条路径（box = kThumbSize 见方，最终是"最长边 = kThumbSize"）
    if (prepareReader(reader, box, m_shrinkOnLoad).isEmpty()) {
        qWarning().noquote() << QStringLiteral("[decode] 无法读取尺寸: %1 (%2)")
                                    .arg(fileNameAtUnlocked(index), reader.errorString());
        return QImage();
    }

    QImage img = decodeScaledWith(reader, box, m_shrinkOnLoad, m_pixelPerfect, m_scaler);
    if (img.isNull()) {
        qWarning().noquote() << QStringLiteral("[decode] 解码失败(%1): %2")
                                    .arg(reader.errorString(), fileNameAtUnlocked(index));
        return QImage();
    }
    return img;
}

// ── 单张图的「解码 + 缩放到盒」核心（唯一实现；见头文件）──────────
QSize ImageProvider::prepareReader(QImageReader &reader, const QSize &box, bool shrinkOnLoad)
{
    const QSize src = reader.size();
    if (!src.isValid() || src.isEmpty()) return QSize();

    const QSize target = src.scaled(box, Qt::KeepAspectRatio);

    // 可选的降采样预处理（只在整数分之一且留有余量时启用）
    if (shrinkOnLoad && target.isValid() && target.width() > 0) {
        int denom = 1;
        while (denom * 2 <= 8 && src.width() / (denom * 2) >= target.width())
            denom *= 2;
        if (denom > 1)
            reader.setScaledSize(QSize(src.width() / denom, src.height() / denom));
    }
    return target;
}

QImage ImageProvider::decodeScaledWith(QImageReader &reader, const QSize &box,
                                       bool shrinkOnLoad, bool pixelPerfect,
                                       const scaler::Params &params)
{
    if (prepareReader(reader, box, shrinkOnLoad).isEmpty()) return QImage();
    const QImage img = reader.read();
    if (img.isNull()) return QImage();

    // 像素完美通路优先（整数倍 + nearest）
    if (pixelPerfect) {
        scaler::Params np;
        np.mode = scaler::Mode::Nearest;
        np.quality = scaler::Quality::Fast;
        return scaler::resize(img, box, np);
    }

    return scaler::resize(img, box, params);
}

QImage ImageProvider::decodeRaw(int index)
{
    QString path;
    QByteArray bytes;
    if (!prepare(index, path, bytes)) return QImage();

    QBuffer buf;
    QImageReader reader;
    if (!path.isEmpty()) {
        reader.setFileName(path);
    } else {
        buf.setData(bytes);
        buf.open(QIODevice::ReadOnly);
        reader.setDevice(&buf);
    }
    // 多页文件（多页 TIFF 等）：跳到本条目对应的页
    if (const int pg = pageIndexOf(index); pg > 0) reader.jumpToImage(pg);
    reader.setAutoTransform(true);
    return reader.read();
}

void ImageProvider::setCacheBudgetMb(int mb)
{
    QMutexLocker lock(&m_mutex);
    m_cacheBudget = qMax(64LL, qint64(mb)) * 1024 * 1024;      // 下限 64MB（与 Settings 一致）
    // 预算调小了：立刻按 LRU 收到新上限之内（同一份淘汰逻辑）
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
// 版面：由条目尺寸构建"页"序列（跨页算一页）
// ─────────────────────────────────────────────────────────────
void ImageProvider::setPairPortrait(bool on)
{
    {
        QMutexLocker lock(&m_mutex);
        qInfo().noquote() << QStringLiteral("[probe] provider::setPairPortrait(%1) 当前=%2 条目=%3")
                                 .arg(on).arg(m_pairPortrait).arg(m_sequence.count());
        if (m_pairPortrait == on) {
            qInfo().noquote() << QStringLiteral("[probe] provider::setPairPortrait 提前返回（值没变）");
            return;
        }
        m_pairPortrait = on;
    }
    // 版面变了 → 立刻重建（"页"的条数、每页含几个条目都变了）。
    //   游标是**条目身份**，不用改：`pageOfItem(cursor)` 会重新算出所在页
    rebuildPages();
}

void ImageProvider::rebuildPages()
{
    // ★ 必须走 sourceSizeAt（唯一实现）——它同时覆盖"文件系统"与"压缩包内"两种情况。
    //   曾经这里直接 QImageReader(包内路径)：对压缩包条目而言那不是文件系统路径
    //   → 尺寸恒为无效 0x0 → 版面判定全错
    //   （实测：117 项的真实 RAR 被错配成 59 页，而同样尺寸的目录是 117 页）。
    QVector<QSize> sizes;
    sizes.reserve(m_sequence.count());
    for (int i = 0; i < m_sequence.count(); ++i)
        sizes << sourceSizeAt(i);
    // 判定规则**只有一份**（layout::build）；这里只提供"要不要配对"这一个输入
    layout::Options opt;
    opt.pairPortrait = m_pairPortrait;
    m_pages = layout::build(sizes, opt);
    qInfo().noquote() << QStringLiteral("[probe] provider::rebuildPages → %1 页（条目 %2，pairPortrait=%3）")
                             .arg(m_pages.size()).arg(m_sequence.count()).arg(m_pairPortrait);
}

int ImageProvider::pageCount() const
{
    QMutexLocker lock(&m_mutex);
    return m_pages.size();
}

int ImageProvider::pageOfItem(int itemIndex) const
{
    QMutexLocker lock(&m_mutex);
    for (int p = 0; p < m_pages.size(); ++p)
        if (m_pages.at(p).items.contains(itemIndex)) return p;
    return 0;
}

QString ImageProvider::pageKindAt(int page) const
{
    QMutexLocker lock(&m_mutex);
    if (page < 0 || page >= m_pages.size()) return QString();
    return QString::fromLatin1(layout::kindName(m_pages.at(page).kind));
}

QVariantList ImageProvider::pageItemsAt(int page) const
{
    QMutexLocker lock(&m_mutex);
    QVariantList out;
    if (page < 0 || page >= m_pages.size()) return out;
    for (int i : m_pages.at(page).items) out << i;
    return out;
}

QString ImageProvider::pageLabelAt(int page) const
{
    QMutexLocker lock(&m_mutex);
    if (page < 0 || page >= m_pages.size()) return QString();
    const auto &pg = m_pages.at(page);
    if (pg.items.isEmpty()) return QString();
    if (pg.items.size() == 1) return m_sequence.at(pg.items.first()).displayName;
    return m_sequence.at(pg.items.at(0)).displayName + QStringLiteral(" + ")
         + m_sequence.at(pg.items.at(1)).displayName;
}

bool ImageProvider::pageMissingAt(int page) const
{
    QMutexLocker lock(&m_mutex);
    if (page < 0 || page >= m_pages.size()) return false;
    for (int i : m_pages.at(page).items)
        if (!m_sequence.at(i).missing) return false;
    return true;
}

QSize ImageProvider::pageSourceSizeAt(int page) const
{
    QMutexLocker lock(&m_mutex);
    if (page < 0 || page >= m_pages.size()) return QSize();
    const auto &pg = m_pages.at(page);
    if (pg.items.isEmpty()) return QSize();
    lock.unlock();
    return sourceSizeAt(pg.items.first());
}

// ─────────────────────────────────────────────────────────────
// 条漫切片（设计 §3.7）：一次性切，派生数据缓存，不是运行时金字塔
// ─────────────────────────────────────────────────────────────
bool ImageProvider::isWebtoonAt(int index) const
{
    QMutexLocker lock(&m_mutex);
    if (index < 0 || index >= m_sequence.count()) return false;
    if (m_sequence.at(index).kind != Sequence::Kind::Image) return false;
    lock.unlock();
    const QSize sz = sourceSizeAt(index);
    return layout::classify(sz) == layout::Kind::Webtoon;
}

int ImageProvider::sliceCountAt(int index) const
{
    QMutexLocker lock(&m_mutex);
    if (index < 0 || index >= m_sequence.count()) return 0;
    lock.unlock();
    const QSize sz = sourceSizeAt(index);
    if (!sz.isValid() || sz.height() <= 0) return 0;
    return (sz.height() + kSliceHeight - 1) / kSliceHeight;
}

QImage ImageProvider::sliceAt(int index, int n)
{
    const QString key = QString::number(index) + QLatin1Char(':') + QString::number(n);
    {
        QMutexLocker lock(&m_mutex);
        auto it = m_sliceCache.constFind(key);
        if (it != m_sliceCache.constEnd()) return it.value();
    }

    // 解码整张（条漫的一次性代价），切出第 n 片
    const QImage full = decodeRaw(index);
    if (full.isNull()) return QImage();

    const int y0 = n * kSliceHeight;
    if (y0 >= full.height()) return QImage();
    const int h = qMin(kSliceHeight, full.height() - y0);
    QImage slice = full.copy(0, y0, full.width(), h);

    {
        QMutexLocker lock(&m_mutex);
        // 切片缓存也受总预算约束（简单起见按片数上限）
        if (m_sliceCache.size() > 256) m_sliceCache.clear();
        m_sliceCache.insert(key, slice);
    }
    return slice;
}

// ─────────────────────────────────────────────────────────────
// requestImage：**只服务缩略图**（图集卡片封面 / 浏览网格的缩略图）
//   ★ 定案（phase B）：本 provider **不再提供"页面取图"**（旧的 `view/<条目号>` /
//     `slice/<条目号>/<片号>` 取图 URL 已删）—— 阅读屏的图由 viewer
//     （`fetch::ViewerModel` / `Sequence::image`）直接给 QML，不走 image provider。
//     provider 只留**缩略图能力**，保留 `src/thumburl.{h,cpp}` 那条（按路径取封面）。
// ─────────────────────────────────────────────────────────────
QImage ImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    const int slash = id.indexOf(QLatin1Char('/'));

    // 按路径的封面 / 缩略图（设计 11 §5.2）：id = "thumbpath/…" 或 "thumbzip/…"
    //   —— 与"当前序列的序号"无关，所以任意压缩包的第 0 页也取得到
    if (isPathThumbId(id))
        return pathThumb(id, size);

    const QString kind = slash > 0 ? id.left(slash) : QString();
    if (kind != QLatin1String("thumb")) {
        qWarning().noquote() << QStringLiteral("[thumb] 不认识的取图 id（只支持缩略图）: %1").arg(id);
        if (size) *size = QSize();
        return QImage();
    }

    const int index = id.mid(slash + 1).toInt();

    // ★ 缩略图恒用 kThumbSize（等比适应盒），**忽略 requestedSize**（设计 11 §5.1）：
    //   缓存键唯一、命中率不被 UI 尺寸左右；盒是正方形但**不裁切** ——
    //   最终尺寸由 decodeFit 按源图宽高比适应，最长边 = kThumbSize
    const QSize box(thumbSizeClass(requestedSize), thumbSizeClass(requestedSize));
    QString key;
    {
        QMutexLocker lock(&m_mutex);
        if (index < 0 || index >= countUnlocked()) {
            if (size) *size = QSize();
            return QImage();
        }
        key = makeKey(index, box);
        QImage hit = cacheGet(key);
        if (!hit.isNull()) {
            if (size) *size = hit.size();
            return hit;
        }
        ++m_misses;
    }

    QImage img = decodeFit(index, box);
    if (!img.isNull()) {
        QMutexLocker lock(&m_mutex);
        cachePut(key, img);
    }
    if (size) *size = img.size();
    return img;
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

    // ④ 解码（唯一实现 QImageReader）+ 缩放（唯一实现 scaler::）
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

    bool shrink = false, pp = false;
    scaler::Params sp;
    {
        QMutexLocker lock(&m_mutex);
        shrink = m_shrinkOnLoad;
        pp = m_pixelPerfect;
        sp = m_scaler;
    }
    if (shrink && target.isValid() && target.width() > 0) {
        int denom = 1;
        while (denom * 2 <= 8 && srcSize.width() / (denom * 2) >= target.width())
            denom *= 2;
        if (denom > 1)
            reader.setScaledSize(QSize(srcSize.width() / denom, srcSize.height() / denom));
    }

    const QImage raw = reader.read();
    if (raw.isNull()) {
        qWarning().noquote() << QStringLiteral("[cover] 解码失败(%1): %2")
                                    .arg(reader.errorString(), cs.itemKey);
        return miss();
    }
    if (pp) {
        sp.mode = scaler::Mode::Nearest;
        sp.quality = scaler::Quality::Fast;
    }
    const QImage out = scaler::resize(raw, box, sp);
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

// ─────────────────────────────────────────────────────────────
// 预取
// ─────────────────────────────────────────────────────────────
class PrefetchTask : public QRunnable
{
public:
    PrefetchTask(ImageProvider *p, int index, QSize box, QString key, int gen)
        : m_p(p), m_index(index), m_box(box), m_key(std::move(key)), m_gen(gen)
    { setAutoDelete(true); }

    void run() override
    {
        if (!m_p) return;
        {
            QMutexLocker lock(&m_p->m_poolMutex);
            if (m_gen < m_p->m_generation) {
                ++m_p->m_prefetchDropped; m_p->m_inFlight.remove(m_key); return;
            }
        }
        QImage img = m_p->decodeFit(m_index, m_box);
        {
            QMutexLocker lock(&m_p->m_poolMutex);
            m_p->m_inFlight.remove(m_key);
            if (m_gen < m_p->m_generation) { ++m_p->m_prefetchDropped; return; }
        }
        if (img.isNull()) return;
        QMutexLocker lock(&m_p->m_mutex);
        m_p->cachePut(m_key, img);
        ++m_p->m_prefetched;
    }

private:
    ImageProvider *m_p;
    int m_index;
    QSize m_box;
    QString m_key;
    int m_gen;
};

QString ImageProvider::makeKey(int index, const QSize &box) const
{
    return (m_sequence.isArchivePages() ? QLatin1String("arc:") : QLatin1String("dir:"))
           + QString::number(index) + QLatin1Char('@')
           + QString::number(box.width()) + QLatin1Char('x') + QString::number(box.height());
}

void ImageProvider::setPixelPerfect(bool on)
{
    {
        QMutexLocker lock(&m_mutex);
        if (m_pixelPerfect == on) return;
        m_pixelPerfect = on;
        m_cache.clear(); m_lru.clear(); m_cacheBytes = 0;   // 缩放路径变了，缓存作废
    }
    qInfo().noquote() << QStringLiteral("[scaler] 像素完美 %1")
                             .arg(on ? QStringLiteral("开（整数倍 nearest）") : QStringLiteral("关（线性光插值）"));
}

bool ImageProvider::atIntegerScale(int index, int w, int h) const
{
    QMutexLocker lock(&m_mutex);
    if (index < 0 || index >= countUnlocked()) return false;
    const Sequence::Item &it = m_sequence.at(index);
    if (it.kind != Sequence::Kind::Image) return false;
    lock.unlock();
    const QSize src = sourceSizeAt(index);
    if (!src.isValid() || w <= 0 || h <= 0) return false;
    const QSize fit = src.scaled(QSize(w, h), Qt::KeepAspectRatio);
    if (fit.width() <= 0) return false;
    const double s = double(src.width()) / double(fit.width());
    return qAbs(s - qRound(s)) < 0.02 && s >= 1.0;      // 整数倍（含 1:1）
}

QString ImageProvider::prefetchStats() const
{
    QMutexLocker lock(&m_poolMutex);
    return QStringLiteral("预取 %1 · 放弃 %2 · 在飞 %3")
        .arg(m_prefetched).arg(m_prefetchDropped).arg(m_inFlight.size());
}

void ImageProvider::prefetch(int cursor, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    const QSize box(w, h);

    int n = 0;
    {
        QMutexLocker lock(&m_mutex);
        n = countUnlocked();
    }
    if (n <= 0) return;

    // 前进 8 / 后退 3（设计红线：前进 5 / 后退 2 是下限）
    constexpr int kAhead = 8, kBehind = 3;
    QVector<int> order;
    order.reserve(kAhead + kBehind);
    for (int d = 1; d <= kAhead; ++d)  if (cursor + d < n) order << cursor + d;
    for (int d = 1; d <= kBehind; ++d) if (cursor - d >= 0) order << cursor - d;

    int gen = 0;
    {
        QMutexLocker lock(&m_poolMutex);
        if (qAbs(cursor - m_lastCursor) >= 4) ++m_generation;
        m_lastCursor = cursor;
        gen = m_generation;
    }

    for (int idx : order) {
        QString key;
        {
            QMutexLocker lock(&m_mutex);
            if (m_sequence.at(idx).kind != Sequence::Kind::Image) continue;   // 跳过压缩包/边界
            if (m_sequence.at(idx).missing) continue;                          // 缺失不重试
            key = makeKey(idx, box);
            if (!cacheGet(key).isNull()) continue;
        }
        {
            QMutexLocker lock(&m_poolMutex);
            if (m_inFlight.contains(key)) continue;
            m_inFlight.insert(key);
        }
        m_pool.start(new PrefetchTask(this, idx, box, key, gen));
    }
}
