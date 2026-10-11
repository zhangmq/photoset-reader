#pragma once

#include <QQuickImageProvider>
#include <QImage>
#include <QStringList>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QThreadPool>
#include <QSet>

#include "scaler.h"
#include "archive.h"
#include "sequence.h"
#include "layout.h"

// 唯一的解码 + 缩放 + 缓存 + 预取入口（见 AGENTS.md「依赖策略」）。
//
// 分层：
//   解码   → QImageReader（唯一实现）
//   缩放   → scaler::（线性光 + Lanczos3 + anti-ringing，唯一实现）
//   压缩包 → ArchiveSource（一个接口，两个后端）
//   序列   → Sequence（一级：文件夹条目；二级：压缩包内页）
//   缓存   → 字节有界 LRU（进程内，不落盘）
//   预取   → 异步解码邻域，当前张不等待
//
// 所有 I/O 容忍失败：解码失败返回空图，条目有缺失标记，不崩溃。
class ImageProvider : public QQuickImageProvider
{
    Q_OBJECT
public:
    ImageProvider();
    ~ImageProvider() override;

    // ── 来源 ──────────────────────────────────────────────
    // 目录 → 列条目（图片 + 压缩包，压缩包算一个条目）
    // 压缩包 → 列内页
    void setSource(const QString &path, bool recursive = false,
                   const QString &archivePassword = QString());
    Q_INVOKABLE void setArchivePassword(const QString &pw) { m_archivePassword = pw; }

    // 进入/离开压缩包（两级序列）
    Q_INVOKABLE bool enterArchive(int index);
    Q_INVOKABLE bool leaveArchive();
    Q_INVOKABLE bool insideArchive() const { return m_nested; }

    // 刷新（重建序列 + 标记缺失）。**只标记，不移除。**
    Q_INVOKABLE int refresh();
    Q_INVOKABLE int revalidate();

    // ── 条目元信息（供 QML）────────────────────────────────
    Q_INVOKABLE int count() const;
    Q_INVOKABLE QString fileNameAt(int index) const;
    Q_INVOKABLE QSize sourceSizeAt(int index) const;
    Q_INVOKABLE bool isArchiveAt(int index) const;
    Q_INVOKABLE bool isBoundaryAt(int index) const;
    Q_INVOKABLE QString folderLabelAt(int index) const;
    // 当前条目相对于"图集根"（sourcePath）的路径；归档内路径本身就是相对路径
    QString relativePathAt(int index) const;
    Q_INVOKABLE int folderDepthAt(int index) const;
    Q_INVOKABLE bool isMissingAt(int index) const;
    Q_INVOKABLE QString sourceLabel() const;
    Q_INVOKABLE QString sourcePath() const;
    Q_INVOKABLE QString absPathAt(int index) const;   // 条目绝对路径（resume 用）
    double cacheMegabytes() const;
    int pageIndexOf(int index) const;   // 多页文件内的页号（单页恒 0）                    // 缓存记账（诊断内存上限用）
    Q_INVOKABLE bool isArchiveSource() const { return m_sourceIsArchive; }
    Q_INVOKABLE QString backendLabel() const;
    Q_INVOKABLE QString lastError() const { return m_lastError; }
    Q_INVOKABLE bool needPassword() const { return m_needPassword; }

    // ── 缩放路径（可切换，用于画质对比）───────────────────
    void setMode(scaler::Mode m) { m_mode = m; }
    void setQuality(scaler::Quality q) { m_quality = q; }
    void setShrinkOnLoad(bool on) { m_shrinkOnLoad = on; }
    Q_INVOKABLE QString modeName() const { return QString::fromLatin1(scaler::modeName(m_mode)); }
    Q_INVOKABLE QString qualityName() const { return QString::fromLatin1(scaler::qualityName(m_quality)); }
    Q_INVOKABLE QString cacheStats() const;

    QImage decodeRaw(int index);

    // 预取：游标移动时由 UI 调用
    Q_INVOKABLE void prefetch(int cursor, int w, int h);
    Q_INVOKABLE QString prefetchStats() const;

    // 像素完美通路（AGENTS.md 缩放立场 #4）：
    // 整数倍缩放时用 nearest，且允许用户关掉一切插值。
    Q_INVOKABLE void setPixelPerfect(bool on);
    Q_INVOKABLE bool pixelPerfect() const { return m_pixelPerfect; }
    // 当前张相对屏幕的缩放倍数是否接近整数（供 UI 提示）
    Q_INVOKABLE bool atIntegerScale(int index, int w, int h) const;

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

    // ── 条漫（设计 §3.7）────────────────────────────────
    // 极端长宽比的图**一次性切片**（每片 ≤1280px 高），作为派生数据缓存；
    // 不是运行时金字塔——切完就是普通顺序图片。
    // ── 版面（沉浸视图按"页"，跨页算一页；浏览视图仍按文件）──
    Q_INVOKABLE int pageCount() const;
    Q_INVOKABLE int pageOfItem(int itemIndex) const;      // 文件索引 → 页索引
    Q_INVOKABLE QString pageKindAt(int page) const;       // single/spread/wide/webtoon
    Q_INVOKABLE QVariantList pageItemsAt(int page) const; // 该页包含的文件索引
    Q_INVOKABLE QString pageLabelAt(int page) const;
    Q_INVOKABLE bool pageMissingAt(int page) const;
    Q_INVOKABLE QSize pageSourceSizeAt(int page) const;

    Q_INVOKABLE bool isWebtoonAt(int index) const;
    Q_INVOKABLE int sliceCountAt(int index) const;
    Q_INVOKABLE int sliceHeight() const { return kSliceHeight; }

private:
    int countUnlocked() const;
    QString fileNameAtUnlocked(int index) const;
    bool prepare(int index, QString &pathOut, QByteArray &bytesOut) const;
    QSize probeSize(int index) const;
    QImage decodeFit(int index, const QSize &box);
    QString makeKey(int index, const QSize &box) const;
    void rebuildPages();

    QImage cacheGet(const QString &key);
    void cachePut(const QString &key, const QImage &img);

    mutable QMutex m_mutex;
    Sequence m_sequence;
    ArchiveSource m_archive;                   // 压缩包内页模式
    QString m_archivePassword;
    bool m_sourceIsArchive = false;            // 来源本身是压缩包
    bool m_nested = false;                     // 已进入某个压缩包条目

    QString m_parentPath;                      // 返回现场
    bool m_parentRecursive = false;
    int m_parentCursor = 0;

    QHash<QString, QImage> m_cache;
    QList<QString> m_lru;
    qint64 m_cacheBytes = 0;
    qint64 m_cacheBudget = 512LL * 1024 * 1024;   // 512 MB 硬上限
    mutable QHash<int, QSize> m_sizeCache;
    QVector<layout::Page> m_pages;          // 版面（由条目尺寸构建）

    static constexpr int kSliceHeight = 1280;      // 条漫切片高度（业界惯例）
    QImage sliceAt(int index, int n);              // 取第 n 片（内部按需解码+切）
    mutable QHash<QString, QImage> m_sliceCache;   // key = "index:n"

    qint64 m_hits = 0;
    qint64 m_misses = 0;

    QThreadPool m_pool;
    QSet<QString> m_inFlight;
    mutable QMutex m_poolMutex;
    qint64 m_prefetched = 0;
    qint64 m_prefetchDropped = 0;
    int m_generation = 0;
    int m_lastCursor = -1000;
    QString m_lastError;
    bool m_needPassword = false;

    scaler::Mode m_mode = scaler::Mode::LinearLight;
    scaler::Quality m_quality = scaler::Quality::Balanced;
    bool m_shrinkOnLoad = true;
    bool m_pixelPerfect = false;   // 像素完美：整数倍 + nearest   // 解码期整数分之一预缩：留余量，最终缩放仍是我们的层

    friend class PrefetchTask;
};
