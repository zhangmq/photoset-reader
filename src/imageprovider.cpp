#include "imageprovider.h"
#include "natural.h"
#include "layout.h"

#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QImageIOHandler>
#include <QBuffer>
#include <QThread>
#include <QDebug>
#include <algorithm>

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
    const bool ok = m_archive.open(it.fullPath, m_archivePassword);
    lock.relock();
    if (!ok) {
        // 加密归档 + 无密码/密码错 → 让 UI 弹密码框
        const bool enc = m_archive.isEncrypted();
        m_needPassword = enc;
        m_lastError = enc ? QStringLiteral("需要密码") : QStringLiteral("无法打开压缩包");
        qWarning().noquote() << QStringLiteral("[source] 进入压缩包失败: %1 (%2)")
                                    .arg(it.fullPath, m_lastError);
        return false;
    }
    m_needPassword = false;
    m_lastError.clear();

    QStringList names;
    names.reserve(m_archive.count());
    for (int i = 0; i < m_archive.count(); ++i) names << m_archive.nameAt(i);
    m_sequence.buildFromNames(it.fullPath, names);
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
        return QFileInfo(m_sequence.rootPath()).fileName() + QStringLiteral("!/") + it.fullPath;
    }
    const QString root = m_sequence.rootPath();
    if (root.isEmpty())
        return it.displayName;
    return QDir(root).relativeFilePath(it.fullPath);
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
    return m_sequence.at(index).pageIndex;
}

QString ImageProvider::absPathAt(int index) const
{
    QMutexLocker lock(&m_mutex);
    if (index < 0 || index >= m_sequence.count()) return QString();
    if (m_sequence.isArchivePages()) return QString();
    return m_sequence.at(index).fullPath;
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
        const QString path = it.fullPath;
        const QString pw = m_archivePassword;
        lock.unlock();
        // 串行化：大字典归档（本项目素材 256MB）同时开多个会爆内存
        static QMutex probeMutex;
        QMutexLocker probeLock(&probeMutex);
        ArchiveSource probe;                                   // 独立实例，不干扰当前序列
        if (!probe.open(path, pw) || probe.count() == 0) return false;
        bytesOut = probe.dataAt(0);
        return !bytesOut.isEmpty();
    }

    if (!m_sequence.isArchivePages()) {
        pathOut = it.fullPath;
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

    const QSize src = reader.size();
    if (!src.isValid() || src.isEmpty()) {
        qWarning().noquote() << QStringLiteral("[decode] 无法读取尺寸: %1 (%2)")
                                    .arg(fileNameAtUnlocked(index), reader.errorString());
        return QImage();
    }

    const QSize target = src.scaled(box, Qt::KeepAspectRatio);

    // 可选的降采样预处理（只在整数分之一且留有余量时启用）
    if (m_shrinkOnLoad && target.isValid() && target.width() > 0) {
        int denom = 1;
        while (denom * 2 <= 8 && src.width() / (denom * 2) >= target.width())
            denom *= 2;
        if (denom > 1)
            reader.setScaledSize(QSize(src.width() / denom, src.height() / denom));
    }

    QImage img = reader.read();
    if (img.isNull()) {
        qWarning().noquote() << QStringLiteral("[decode] 解码失败(%1): %2")
                                    .arg(reader.errorString(), fileNameAtUnlocked(index));
        return QImage();
    }

    // 像素完美通路优先（整数倍 + nearest）
    if (m_pixelPerfect)
        return scaler::resize(img, box, scaler::Mode::Nearest, scaler::Quality::Fast);

    return scaler::resize(img, box, m_mode, m_quality);
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

// ─────────────────────────────────────────────────────────────
// 版面：由条目尺寸构建"页"序列（跨页算一页）
// ─────────────────────────────────────────────────────────────
void ImageProvider::rebuildPages()
{
    // ★ 必须走 sourceSizeAt（唯一实现）——它同时覆盖"文件系统"与"压缩包内"两种情况。
    //   曾经这里直接 QImageReader(it.fullPath)：对压缩包条目而言 fullPath 是**包内路径**，
    //   不是文件系统路径 → 尺寸恒为无效 0x0 → 版面判定全错
    //   （实测：117 项的真实 RAR 被错配成 59 页，而同样尺寸的目录是 117 页）。
    QVector<QSize> sizes;
    sizes.reserve(m_sequence.count());
    for (int i = 0; i < m_sequence.count(); ++i)
        sizes << sourceSizeAt(i);
    m_pages = layout::build(sizes);
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
// requestImage：当前张走同步路径（不等待预取）
// ─────────────────────────────────────────────────────────────
QImage ImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    const int slash = id.indexOf(QLatin1Char('/'));
    const QString kind = slash > 0 ? id.left(slash) : QStringLiteral("full");

    // 条漫切片：id = "slice/<index>/<n>"
    if (kind == QLatin1String("slice")) {
        const QString rest = id.mid(slash + 1);
        const int s2 = rest.indexOf(QLatin1Char('/'));
        const int idx = (s2 > 0 ? rest.left(s2) : rest).toInt();
        const int n = (s2 > 0 ? rest.mid(s2 + 1) : QStringLiteral("0")).toInt();
        const QImage s = sliceAt(idx, n);
        if (size) *size = s.size();
        return s;
    }

    const int index = (slash > 0 ? id.mid(slash + 1) : id).toInt();

    QSize box;
    if (kind == QLatin1String("thumb")) {
        const int side = (requestedSize.isValid() && requestedSize.width() > 0)
                             ? qMax(requestedSize.width(), requestedSize.height())
                             : 200;
        box = QSize(side, side);
    } else {
        box = (requestedSize.isValid() && !requestedSize.isEmpty()) ? requestedSize
                                                                   : QSize(3840, 2160);
    }

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
