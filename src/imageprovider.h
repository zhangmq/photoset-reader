#pragma once

#include <QQuickImageProvider>
#include <QImage>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QString>
#include <atomic>

// ══ 缩略图 provider（**只服务缩略图**）════════════════════════════════════
//   ★ 2026-10-19：这里**只剩缩略图能力**。此前那一整块"按条目号取图 + 自建序列 +
//     预取 + 页面 / 条漫切片 + CPU 缩放参数"**已删** —— 阅读屏的图由
//     `fetch::ViewerModel` / `imageread::image` 直接给 QML（phase B 定案），
//     provider 只按**路径**出封面 / 缩略图（URL 拼法见 `src/thumburl.{h,cpp}`）。
//
//   取图 id（**编码的唯一实现**，别处不要手写字符串）：
//     thumbpath/<percent(绝对路径)>                        普通图片文件
//     thumbzip/<percent(压缩包绝对路径)>                    包内第 0 页
//     thumbzip/<percent(压缩包绝对路径)>/<percent(成员名)>   包内指定成员
//   解析容忍"未编码的绝对路径"（逐段找真正存在的压缩包前缀），规范形式是百分号编码。
class ImageProvider : public QQuickImageProvider
{
    Q_OBJECT
public:
    ImageProvider();
    ~ImageProvider() override;

    Q_INVOKABLE void setArchivePassword(const QString &pw) { m_archivePassword = pw; }

    // 缩略图**写入磁盘**开关（用户 2026-10-12：管线 = 先全部生成（进程内），**只有图集要存**）：
    //   开 = 生成的缩略图写进 `thumb` 表；关 = 只走**进程内 LRU**（会话来源不写入磁盘）。
    void setPersistThumbs(bool on) { m_persistThumbs = on; }
    bool persistThumbs() const { return m_persistThumbs; }

    // 解码期整数分之一预缩（默认开）：缩略图靠它把解码代价压下来
    void setShrinkOnLoad(bool on) { m_shrinkOnLoad = on; }

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

    // ── 缩略图规格（设计 11 §5.1，唯一来源：library::Store 的三个常量）────
    //   thumb 的入库档位（size_class）：**恒为 kThumbSize**，与 requestedSize 无关
    //   —— 缓存键唯一、命中率不被 UI 尺寸左右
    static int thumbSizeClass(const QSize &requestedSize = QSize());
    //   把缩略图编成入库 BLOB 的**唯一实现**：WebP，质量 = kThumbQuality
    static QByteArray encodeThumbBlob(const QImage &img);

    // ── 按路径取封面 / 缩略图（设计 11 §5.2）────────────────
    //   为什么需要：卡片封面可能是"集合的第一个条目 = 压缩包"，而扫描**不展开**
    //   压缩包（重 I/O），包内第 0 页没有"当前序列序号"可取 ⇒ 按**源路径**取，
    //   并在**第一次请求时才**打开压缩包（之后走 thumb 表缓存）。
    static QString thumbPathId(const QString &absPath);
    static QString thumbZipId(const QString &archivePath, const QString &member = QString());
    static bool    isPathThumbId(const QString &id);
    //   该 id 在 `thumb` 表里的 item_key（规范串，人类可读）：
    //     `path:<绝对路径>` / `zip:<压缩包绝对路径>!/<成员名>`（未指定成员 → 成员留空）
    static QString thumbItemKey(const QString &id);

    //   按需取图：命中（内存 LRU → thumb 表）直接返回；未命中才读文件 / 打开压缩包，
    //   按 kThumbSize 规格（等比适应、不裁切、Qt 平滑缩放）生成并写库。失败一律空图、不重试。
    QImage pathThumb(const QString &id, QSize *outSize = nullptr);

    // 自检用计数：为封面**真正打开压缩包**的次数 / 命中 thumb 表缓存的次数 / 生成次数
    qint64 coverArchiveOpens() const { return m_coverArchiveOpens.load(std::memory_order_relaxed); }
    qint64 coverDbHits() const { return m_coverDbHits.load(std::memory_order_relaxed); }
    qint64 coverGenerated() const { return m_coverGenerated.load(std::memory_order_relaxed); }

private:
    QImage cacheGet(const QString &key);
    void cachePut(const QString &key, const QImage &img);

    mutable QMutex m_mutex;
    bool m_persistThumbs = true;      // 缩略图写入磁盘（**只有图集**才开；会话来源关）
    bool m_shrinkOnLoad = true;       // 解码期整数分之一预缩（留余量；最终是 Qt 平滑缩放）
    QString m_archivePassword;        // 按路径取"压缩包内第 0 页"时用

    QHash<QString, QImage> m_cache;   // 进程内 LRU（按字节上限淘汰）
    QList<QString> m_lru;
    qint64 m_cacheBytes = 0;
    qint64 m_cacheBudget = 512LL * 1024 * 1024;   // 512 MB 硬上限

    // 封面路径的可观测计数（自检用；可能被图像加载线程并发写）
    std::atomic<qint64> m_coverArchiveOpens{0};
    std::atomic<qint64> m_coverDbHits{0};
    std::atomic<qint64> m_coverGenerated{0};
};
