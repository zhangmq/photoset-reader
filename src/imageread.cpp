#include "imageread.h"

#include "archive.h"

#include <QBuffer>
#include <QColorSpace>
#include <QFile>
#include <QHash>
#include <QImageReader>
#include <QMutex>

namespace imageread {

namespace {

// ── 压缩包句柄：**每线程一个最近用过的**（连续读同一个压缩包时不必反复开）
//   ★ v5：只缓存**最外层**（盘上的那个）；嵌套层用局部 `ArchiveSource::openBytes`
//     （取出来的字节再当容器打开，逐层向下）。
struct ArchiveSlot {
    QString            path;
    ArchiveSource      src;
    QHash<QString, int> index;      // 条目名 → 下标（开一次建一次）
};
thread_local ArchiveSlot t_archive;

ArchiveSource *openCachedArchive(const QString &path)
{
    if (t_archive.path == path && t_archive.src.isOpen()) return &t_archive.src;
    t_archive.src.close();
    t_archive.index.clear();
    t_archive.path.clear();
    // ★ 必须列出**全部**条目：解码一条嵌套条目时要逐层下钻（只列图片会看不到内层容器）
    if (!t_archive.src.open(path, QString(), ArchiveSource::Entries::All)) return nullptr;
    t_archive.path = path;
    for (int i = 0; i < t_archive.src.count(); ++i)
        t_archive.index.insert(t_archive.src.nameAt(i), i);
    return &t_archive.src;
}

int indexOfName(ArchiveSource *a, const QString &name)
{
    for (int i = 0; i < a->count(); ++i)
        if (a->nameAt(i) == name) return i;
    return -1;
}

// ── ★★ v7：**内层包的单槽物化**（用户 2026-10-14 定案）────────────────────
//   物化产物的生命周期 = 该容器在序列里的**区间**（深度优先线性序 ⇒ 一个容器的子项
//   是连续一段）：
//     · 进入区间**物化一次**，区间内反复复用（翻页 / 缩略图 / 预取都走这里）；
//     · **离开区间就释放**（换容器时先 close 旧的 ⇒ 内存丢弃 / RAR 临时文件删除）；
//     · 同一时刻**只有一个内层包被物化**（进程级单槽）；
//     · **不做**确定性命名 / 存在性检查 / LRU / 跨会话保留 —— 一律不做。
//
//   锁：`ArchiveSource::dataAt` 本来就串行（对象内 mutex），所以这里用一把进程级
//   mutex 把"换容器 + 取字节"整体串起来 —— 不新增状态，也不会两个内层包并存。
struct InnerSlot {
    QString       diskPath;      // 最外层盘上容器
    QStringList   chain;         // 到这个内层包**自身**的成员链（空 = 还没物化）
    ArchiveSource src;           // 物化出来的内层包（RAR 时它自己拥有临时文件）
};
QMutex    g_innerMutex;
InnerSlot g_inner;

// ★★ **沿链读字节的唯一实现**（链式解析）：
//   `diskPath` = 最外层盘上容器；`containerChain` = 到那个内层包自身的成员链
//   （空 = 目标就在盘上容器里）；`leaf` = 目标成员名。
//   `innerArchiveLimitBytes` = 「内层压缩包上限」（字节）—— **解压前**从归档条目元数据判定；
//     ★ 设计 14 §9.3：**参数传入**（随 `Location` 带），不读设置。
//   空 = 读不到（容器打不开 / 成员不存在 / 超过内层包上限）。
QByteArray readBytesAt(const QString &diskPath, const QStringList &containerChain,
                       const QString &leaf, qint64 innerArchiveLimitBytes)
{
    const qint64 limit = innerArchiveLimitBytes;
    QMutexLocker lk(&g_innerMutex);

    // ① 目标就在**盘上那个容器**里（没有内层包要物化）
    if (containerChain.isEmpty()) {
        ArchiveSource *outer = openCachedArchive(diskPath);
        if (!outer) return {};
        const int idx = t_archive.index.value(leaf, -1);
        return idx < 0 ? QByteArray() : outer->dataAt(idx);
    }

    // ② 区间内 ⇒ 复用已物化的内层包（翻页 / 缩略图 / 预取反复读都靠这条）
    if (g_inner.src.isOpen() && g_inner.diskPath == diskPath && g_inner.chain == containerChain) {
        const int idx = indexOfName(&g_inner.src, leaf);
        return idx < 0 ? QByteArray() : g_inner.src.dataAt(idx);
    }

    // ③ 换容器 ⇒ **离开区间就释放**（旧的内存 / 临时文件在这里丢掉）
    g_inner.src.close();
    g_inner.chain.clear();
    g_inner.diskPath.clear();

    // ④ 沿链把内层包的字节取出来：最外层用线程本地的盘上句柄，中间层是局部对象
    //    （局部 `ArchiveSource` 出作用域即析构 ⇒ 中间层的临时文件也当场删）
    ArchiveSource *outer = openCachedArchive(diskPath);
    if (!outer) return {};
    QByteArray cur;
    {
        const int idx = t_archive.index.value(containerChain.first(), -1);
        if (idx < 0) return {};
        if (outer->sizeAt(idx) > limit) {
            qWarning().noquote()
                << QStringLiteral("[seq] 内层包超过上限（%1 MB > %2 MB），不展开：%3")
                       .arg(outer->sizeAt(idx) / 1048576.0, 0, 'f', 1)
                       .arg(limit / 1048576)
                       .arg(containerChain.first());
            return {};
        }
        cur = outer->dataAt(idx);
    }
    for (int i = 1; i < containerChain.size(); ++i) {
        ArchiveSource mid;
        if (!mid.openBytes(cur, containerChain.at(i - 1))) return {};
        const int idx = indexOfName(&mid, containerChain.at(i));
        if (idx < 0) return {};
        if (mid.sizeAt(idx) > limit) {
            qWarning().noquote()
                << QStringLiteral("[seq] 内层包超过上限（%1 MB > %2 MB），不展开：%3")
                       .arg(mid.sizeAt(idx) / 1048576.0, 0, 'f', 1)
                       .arg(limit / 1048576)
                       .arg(containerChain.at(i));
            return {};
        }
        cur = mid.dataAt(idx);
    }
    if (cur.isEmpty()) return {};

    // ⑤ 物化：**同一个 openBytes**（RAR 由 ArchiveSource 自己在内部落临时文件）
    if (!g_inner.src.openBytes(cur, containerChain.last())) return {};
    g_inner.diskPath = diskPath;
    g_inner.chain = containerChain;
    const int idx = indexOfName(&g_inner.src, leaf);
    return idx < 0 ? QByteArray() : g_inner.src.dataAt(idx);
}

// ── 正确显示色彩（**唯一一处**）：非 sRGB 的输入按它自己的 ICC 转到 sRGB ─────────
//   ★ 用户 2026-10-20 校正口径：**"不做色彩处理"指的是编辑/调色**（白平衡、饱和度、HDR 调色
//     这些不在本项目），**不是"色彩可以不正确显示"**。文件声明了什么色域，就按它如实呈现。
//   · 只在解码这一处做一次 ⇒ 缓存里存的就是 sRGB，GPU 与 CPU 两条渲染路径看到的是同一批像素；
//   · 没有任何"处理"：不调白平衡、不调饱和、不做风格化；
//   · Qt 因 ICC 头非法而丢弃 profile 的情况（日志 `qt.gui.icc: Invalid ICC illuminant`）
//     无从转换 —— 此时按 sRGB 处理（也就是 Qt 的默认假设）。
QImage toSrgb(const QImage &in)
{
    if (in.isNull()) return in;
    const QColorSpace cs = in.colorSpace();
    if (!cs.isValid() || cs == QColorSpace::SRgb) return in;
    const QImage out = in.convertedToColorSpace(QColorSpace::SRgb);
    return out.isNull() ? in : out;      // 转换失败（Qt 不支持的 profile）⇒ 保持原样
}

class EntryReader
{
public:
    explicit EntryReader(const Location &loc)
    {
        if (loc.members.isEmpty()) {
            m_r.setFileName(loc.diskPath);
        } else {
            // ★ v7：沿链取字节 = `readBytesAt`（**内层包单槽物化**：区间内复用、离开即释放）
            m_bytes = readBytesAt(loc.diskPath, loc.members.mid(0, loc.members.size() - 1),
                                  loc.members.last(), loc.innerArchiveMaxMb * 1024 * 1024);
            if (m_bytes.isEmpty()) return;
            m_buf.setData(m_bytes);
            if (!m_buf.open(QIODevice::ReadOnly)) return;
            m_r.setDevice(&m_buf);
        }
        m_r.setAutoTransform(true);
        if (loc.page > 0) m_r.jumpToImage(loc.page);
        m_ok = true;
    }

    bool ok() const { return m_ok; }
    QSize size() { return m_ok ? m_r.size() : QSize(); }
    QImage read() { return m_ok ? toSrgb(m_r.read()) : QImage(); }

private:
    QByteArray   m_bytes;
    QBuffer      m_buf;
    QImageReader m_r;
    bool         m_ok = false;
};

}  // namespace

QByteArray readLocation(const Location &loc)
{
    if (loc.members.isEmpty()) return {};
    return readBytesAt(loc.diskPath, loc.members.mid(0, loc.members.size() - 1),
                       loc.members.last(), loc.innerArchiveMaxMb * 1024 * 1024);
}

QByteArray headBytes(const Location &loc, int maxBytes)
{
    if (maxBytes <= 0) return {};
    if (loc.members.isEmpty()) {
        QFile f(loc.diskPath);
        if (!f.open(QIODevice::ReadOnly)) return {};
        return f.read(maxBytes);
    }
    const QByteArray all = readLocation(loc);
    return all.size() > maxBytes ? all.left(maxBytes) : all;
}

QImage image(const Location &loc)
{
    EntryReader r(loc);
    return r.read();
}

QSize imageSize(const Location &loc)
{
    EntryReader r(loc);
    return r.size();
}

int pageCount(const QString &path)
{
    QImageReader r(path);
    const int n = r.imageCount();
    return n > 1 ? n : 1;
}

void releaseInnerSlot()
{
    QMutexLocker lk(&g_innerMutex);
    g_inner.src.close();
    g_inner.chain.clear();
    g_inner.diskPath.clear();
}

}  // namespace imageread
