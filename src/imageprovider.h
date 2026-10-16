#pragma once

#include <QQuickImageProvider>
#include <QImage>
#include <QImageReader>
#include <QStringList>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QThreadPool>
#include <QSet>
#include <atomic>

#include "scaler.h"
#include "archive.h"
#include "sequence.h"
#include "layout.h"
#include "librarystore.h"

// 唯一的解码 + 缩放 + 缓存 + 预取入口（见 AGENTS.md「依赖策略」）。
//
// 分层：
//   解码   → QImageReader（唯一实现）
//   缩放   → scaler::（线性光 + Lanczos3 + anti-ringing，唯一实现）
//   压缩包 → ArchiveSource（一个接口，两个后端）
//   序列   → Sequence（一级：文件夹条目；二级：压缩包内页）
//   缓存   → 字节有界 LRU（进程内，不写入磁盘）
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
    // ★ 接缝（设计 11 §七 第 5 条）：**按显式条目列表**建序列 ——
    //   图集库的条目以 SQLite 为真相（含 missing 行），解码层不该自己再扫一次盘：
    //   扫盘会跳过已消失的文件、也没有递归深度 / 隐藏项规则 → 解码索引会与库行错位，
    //   "打开第 N 行"就会显示错的那张图。
    //   entries 的 kind / displayName / fullPath / folderLabel / folderDepth 由调用方给出；
    //   序列构建仍只有一份实现（`Sequence::buildFromEntries`，扫描器用的是同一个）。
    void setSourceItems(const QString &rootPath, const QVector<Sequence::Item> &entries);
    Q_INVOKABLE void setArchivePassword(const QString &pw) { m_archivePassword = pw; }

    // 缩略图**写入磁盘**开关（用户 2026-10-12：管线 = 先全部生成（进程内），**只有图集要存**）：
    //   开 = 生成的缩略图写进 `thumb` 表；关 = 只走**进程内 LRU**（会话来源不写入磁盘）。
    void setPersistThumbs(bool on) { m_persistThumbs = on; }
    bool persistThumbs() const { return m_persistThumbs; }

    // 进入/离开压缩包（两级序列）
    Q_INVOKABLE bool enterArchive(int index);
    Q_INVOKABLE bool leaveArchive();
    Q_INVOKABLE bool insideArchive() const { return m_nested; }

    // 刷新（重建序列 + 标记缺失）。**只标记，不移除。**
    Q_INVOKABLE int refresh();
    Q_INVOKABLE int revalidate();

    // 当前序列的**快照**（设计 11 §四：ViewModel 用它建文件系统来源的 ItemSource）。
    // 不改变任何状态；序列构建的唯一实现仍在 ImageProvider/Sequence 里。
    Sequence currentSequence() const;

    // ── 版面（阅读版面：单幅 / 双幅）────────────────────────
    //   ★ 单幅/双幅**不是视图的开关**，而是"页"的定义：
    //       双幅 → 连续竖图**配对**（一页两个条目）→ 视图并排画两张
    //       单幅 → 不配对（一页恒一个条目）→ 视图画一张
    //     若只在视图里藏掉第二张，配对出来的**后一个条目会永远看不到**
    //     （翻页按页走，会整条跳过）。所以必须在这一层决定。
    //   传入的 `pairPortrait` 直接进 `layout::Options`（判定唯一实现仍在 layout）。
    void setPairPortrait(bool on);

    // ── 单张图的「解码 + 缩放到盒」核心（**唯一实现**）────────────
    //   `decodeFit`（生产路径）与并行度实验（`--bench-parallel`）**共用这两步**，
    //   否则实验量到的就不是生产路径（算法单一事实源）。
    //   `reader` 由调用方按来源准备好（文件路径 / 内存缓冲 / 多页跳页 / autoTransform）。
    //
    //   ① 按需设置 shrink-on-load（整数分之一预缩，**只作降采样预处理**），返回等比适应到盒的目标尺寸
    static QSize prepareReader(QImageReader &reader, const QSize &box, bool shrinkOnLoad);
    //   ② 解码 + 走我们自己的缩放层到盒（shrink → read → scaler::resize）
    static QImage decodeScaledWith(QImageReader &reader, const QSize &box,
                                   bool shrinkOnLoad, bool pixelPerfect,
                                   const scaler::Params &params);

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
    void setScalerParams(const scaler::Params &p) { m_scaler = p; }
    const scaler::Params &scalerParams() const { return m_scaler; }
    void setMode(scaler::Mode m) { m_mode = m; }
    void setQuality(scaler::Quality q) { m_quality = q; }
    void setShrinkOnLoad(bool on) { m_shrinkOnLoad = on; }
    Q_INVOKABLE QString modeName() const { return QString::fromLatin1(scaler::modeName(m_mode)); }
    Q_INVOKABLE QString qualityName() const { return QString::fromLatin1(scaler::qualityName(m_quality)); }
    Q_INVOKABLE QString cacheStats() const;

    QImage decodeRaw(int index);

    // 内存缓存硬上限（MB）——设置项 `cacheMb` 落到这里（默认 512）
    void setCacheBudgetMb(int mb);

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

    // ── 缩略图（设计 11 §5.1，唯一规格）────────────────────
    //   thumb 的入库档位（size_class）：**恒为 kThumbSize**（等比适应盒的边长），
    //   与 requestedSize 无关 —— 缓存键唯一、命中率不被 UI 尺寸左右
    static int thumbSizeClass(const QSize &requestedSize = QSize());
    //   把缩略图编成入库 BLOB 的**唯一实现**：WebP，质量 = kThumbQuality
    static QByteArray encodeThumbBlob(const QImage &img);

    // ── 按路径取封面 / 缩略图（设计 11 §5.2）────────────────
    //   为什么需要：卡片封面可能是"集合的第一个条目 = 压缩包"，而扫描**不展开**
    //   压缩包（重 I/O），包内第 0 页没有"当前序列序号"可取。所以按**源路径**取，
    //   并在**第一次请求时才**打开压缩包（之后走 thumb 表缓存）。
    //
    //   id 方案（**编码的唯一实现**，别处不要手写字符串）：
    //     thumbpath/<percent(绝对路径)>                        普通图片文件
    //     thumbzip/<percent(压缩包绝对路径)>                    包内第 0 页
    //     thumbzip/<percent(压缩包绝对路径)>/<percent(成员名)>   包内指定成员
    //   构造器返回的是 **id**（不含 `image://img/` 前缀）；QML/C++ 拼成 URL。
    //   解析容忍"未编码的绝对路径"（逐段找真正存在的压缩包前缀），但规范形式是百分号编码。
    static QString thumbPathId(const QString &absPath);
    static QString thumbZipId(const QString &archivePath, const QString &member = QString());
    static bool    isPathThumbId(const QString &id);
    //   该 id 在 `thumb` 表里的 item_key（规范串，人类可读）：
    //     `path:<绝对路径>` / `zip:<压缩包绝对路径>!/<成员名>`（未指定成员 → 成员留空）
    static QString thumbItemKey(const QString &id);

    //   按需取图：命中（内存 LRU → thumb 表）直接返回；未命中才读文件/打开压缩包，
    //   按 kThumbSize 规格（等比适应、不裁切）生成并写库。失败一律返回空图，不重试。
    QImage pathThumb(const QString &id, QSize *outSize = nullptr);

    //   自检用计数：为封面**真正打开压缩包**的次数 / 命中 thumb 表缓存的次数
    qint64 coverArchiveOpens() const { return m_coverArchiveOpens.load(std::memory_order_relaxed); }
    qint64 coverDbHits() const { return m_coverDbHits.load(std::memory_order_relaxed); }
    qint64 coverGenerated() const { return m_coverGenerated.load(std::memory_order_relaxed); }

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
    bool m_persistThumbs = true;      // 缩略图写入磁盘（**只有图集**才开；会话来源关）
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
    bool m_pairPortrait = true;             // 竖图是否配对（= 阅读版面"双幅"；默认与 layout::Options 一致）

    static constexpr int kSliceHeight = 1280;      // 条漫切片高度（业界惯例）
    QImage sliceAt(int index, int n);              // 取第 n 片（内部按需解码+切）
    mutable QHash<QString, QImage> m_sliceCache;   // key = "index:n"

    qint64 m_hits = 0;
    qint64 m_misses = 0;

    // 封面路径的可观测计数（自检用；可能被图像加载线程并发写）
    std::atomic<qint64> m_coverArchiveOpens{0};
    std::atomic<qint64> m_coverDbHits{0};
    std::atomic<qint64> m_coverGenerated{0};

    QThreadPool m_pool;
    QSet<QString> m_inFlight;
    mutable QMutex m_poolMutex;
    qint64 m_prefetched = 0;
    qint64 m_prefetchDropped = 0;
    int m_generation = 0;
    int m_lastCursor = -1000;
    QString m_lastError;
    bool m_needPassword = false;

    scaler::Mode m_mode = scaler::Mode::LinearLight;   // 兼容旧调用（setMode/setQuality）
    scaler::Params m_scaler;
    scaler::Quality m_quality = scaler::Quality::Balanced;
    bool m_shrinkOnLoad = true;
    bool m_pixelPerfect = false;   // 像素完美：整数倍 + nearest   // 解码期整数分之一预缩：留余量，最终缩放仍是我们的层

    friend class PrefetchTask;
};
