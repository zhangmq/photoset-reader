// ══ 独立实验：读盘 / 解码 / 读+解码 的真实吞吐（ib-bench-io）═════════════════════
//   用户 2026-11-05：想弄清楚"预取到底能不能提速"，要在慢盘（H:）和快盘（M:）上量真吞吐，
//   并且**必须避开系统文件缓存**（同一批文件读第二遍就全是内存带宽，量不到磁盘）。
//
//   ★ 绕开页缓存的办法：Win32 `FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN`
//     （读缓冲按 1 MB 对齐）。所以这里**不用 `QFile` 读盘**，而是自己拿对齐缓冲读。
//   ★ 三个阶段（每个阶段都跑够 `-t` 秒，不是"一遍就报数"）：
//       ① read  ：顺序读盘、不解码                → 磁盘顺序带宽（MB/s）
//       ② decode：字节已在内存里，只解码（单线程） → 解码吞吐上限（pages/s）
//       ③ both  ：读盘 + 解码，并发 1/3/4/6       → 真实流水线（预取就是这个）
//   ★ 为什么要 1/3/4/6：按用户口径 **预取 N = 本屏 1 + 前面 N−1 张** ⇒ N=1 只有本屏、
//     N=3 是 2 张预取、N=6 是 5 张预取。产品在飞上限写死 4（`kMaxInFlight`）⇒ **6 不会比 4 快**。
//   ★ 报告单页 p50/p95/max：预取能"藏"的就是这段延迟（首屏、跳页、卡顿的观感来源）。
//
//   用法：
//     ib-bench-io -d "M:\..\.local\test\db" -n 150 -c 1,3,4,6 -t 20
//     ib-bench-io -d "H:\psr-bench"          -n 150 -c 1,3,4,6 -t 20

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QVector>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <malloc.h>
#endif

namespace {

constexpr int kBlock = 1 << 20;              // 1 MB（NO_BUFFERING 要求扇区对齐）

//   ★ 绕页缓存读整个文件到内存（真磁盘）。失败（文件尾不足一块之类）也把已读到的返回。
bool readNoBuffer(const QString &path, QByteArray *out)
{
    out->clear();
    out->reserve(int(QFileInfo(path).size()));
#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(path);
    HANDLE h = ::CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    void *raw = ::_aligned_malloc(kBlock, 4096);
    if (!raw) { ::CloseHandle(h); return false; }
    const qint64 size = QFileInfo(path).size();
    for (;;) {
        DWORD got = 0;
        if (!::ReadFile(h, raw, DWORD(kBlock), &got, nullptr) || got == 0) break;
        out->append(static_cast<const char *>(raw), int(got));
        if (out->size() >= size) break;
    }
    ::_aligned_free(raw);
    ::CloseHandle(h);
#else
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    *out = f.readAll();
#endif
    return !out->isEmpty();
}

//   只解码（数据已在内存）：与产品同一条 `QImageReader`（自动 EXIF 旋转）
bool decodeBytes(const QByteArray &bytes, double *ms)
{
    QElapsedTimer one;
    one.start();
    QBuffer buf;
    buf.setData(bytes);
    if (!buf.open(QIODevice::ReadOnly)) return false;
    QImageReader r(&buf);
    r.setAutoTransform(true);
    const QImage img = r.read();
    if (ms) *ms = one.nsecsElapsed() / 1e6;
    return !img.isNull();
}

struct Stats {
    double p50 = 0, p95 = 0, mx = 0;
    QString line() const
    {
        return QStringLiteral("%1/%2/%3").arg(p50, 0, 'f', 1).arg(p95, 0, 'f', 1).arg(mx, 0, 'f', 1);
    }
};

Stats statsOf(std::vector<double> v)
{
    Stats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.p50 = v[v.size() / 2];
    s.p95 = v[std::min(v.size() - 1, size_t(v.size() * 0.95))];
    s.mx  = v.back();
    return s;
}

QVector<QString> listImages(const QString &dir, int want)
{
    QVector<QString> out;
    QDir root(dir);
    const QStringList names = root.entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &n : names) {
        const QString p = root.filePath(n);
        if (QImageReader(p).canRead()) out << p;
        if (out.size() >= want) return out;
    }
    for (const QString &d : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        out += listImages(root.filePath(d), want - out.size());
        if (out.size() >= want) break;
    }
    return out;
}

}  // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("ib-bench-io"));

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("Read / decode / pipeline throughput (bypasses OS cache)"));
    const QCommandLineOption optDir({ "d", "dir" }, "Directory with images", "dir");
    const QCommandLineOption optN({ "n", "count" }, "How many images in the working set", "n", "150");
    const QCommandLineOption optC({ "c", "concurrency" }, "Concurrency list", "list", "1,3,4,6");
    const QCommandLineOption optT({ "t", "seconds" }, "Min seconds per phase", "t", "20");
    cli.addOption(optDir);
    cli.addOption(optN);
    cli.addOption(optC);
    cli.addOption(optT);
    cli.addHelpOption();
    cli.process(app);

    const QString dir = cli.value(optDir);
    const int want = qMax(4, cli.value(optN).toInt());
    const int minSec = qMax(1, cli.value(optT).toInt());
    QVector<int> conc;
    for (const QString &s : cli.value(optC).split(QLatin1Char(','), Qt::SkipEmptyParts))
        conc << qBound(1, s.trimmed().toInt(), 32);
    if (dir.isEmpty()) {
        std::printf("usage: ib-bench-io -d <dir> [-n 150] [-c 1,3,4,6] [-t 20]\n");
        return 2;
    }

    const QVector<QString> files = listImages(dir, want);
    if (files.isEmpty()) {
        std::printf("no readable images in %s\n", qPrintable(dir));
        return 1;
    }
    qint64 bytesTotal = 0;
    for (const QString &p : files) bytesTotal += QFileInfo(p).size();
    const double setMb = double(bytesTotal) / 1048576.0;

    std::printf("dir   : %s\n", qPrintable(QDir::toNativeSeparators(dir)));
    std::printf("files : %d   working set %.1f MB   avg %.0f KB/file\n",
                files.size(), setMb, files.isEmpty() ? 0.0 : double(bytesTotal) / files.size() / 1024.0);
    std::printf("cache : BYPASSED (FILE_FLAG_NO_BUFFERING)\n");
    std::printf("phase : >= %d s each\n\n", minSec);

    // ── ① 顺序读盘（NO_BUFFERING，单线程；跑够 minSec 秒）──
    {
        QElapsedTimer t;
        t.start();
        qint64 got = 0;
        int passes = 0;
        QByteArray sink;
        while (t.elapsed() < minSec * 1000LL || passes < 1) {
            for (const QString &p : files) {
                if (readNoBuffer(p, &sink)) got += sink.size();
            }
            ++passes;
        }
        const double sec = t.nsecsElapsed() / 1e9;
        std::printf("[read   ] 1 thread          : %7.1f MB/s   (%d passes, %.1f s, %.1f MB)\n",
                    got / 1048576.0 / sec, passes, sec, got / 1048576.0);
    }

    // ── ② 只解码（先把整批读进内存，再计时纯解码）──
    {
        std::vector<QByteArray> raw;
        raw.reserve(files.size());
        for (const QString &p : files) {
            QByteArray b;
            if (readNoBuffer(p, &b)) raw.push_back(std::move(b));
        }
        std::vector<double> lat;
        QElapsedTimer t;
        t.start();
        int ok = 0, passes = 0;
        while (t.elapsed() < minSec * 1000LL || passes < 1) {
            for (const QByteArray &b : raw) {
                double ms = 0;
                if (decodeBytes(b, &ms)) ++ok;
                if (passes == 0) lat.push_back(ms);
            }
            ++passes;
        }
        const double sec = t.nsecsElapsed() / 1e9;
        const Stats s = statsOf(lat);
        std::printf("[decode ] 1 thread          : %7.1f pages/s (%d passes, p50/p95/max %s ms)\n",
                    ok / sec, passes, qPrintable(s.line()));
    }

    // ── ③ 读 + 解码（NO_BUFFERING 读，并发 N）──
    for (int th : conc) {
        QElapsedTimer t;
        t.start();
        std::atomic<qint64> done{ 0 };
        std::atomic<int> ok{ 0 }, failed{ 0 };
        std::mutex mu;
        std::vector<double> lat;                 // 只收第一遍的延迟（避免缓存差异）
        int passes = 0;
        std::vector<std::thread> pool;
        pool.reserve(th);
        for (int k = 0; k < th; ++k) {
            pool.emplace_back([&, k] {
                std::vector<double> mine;
                QByteArray buf;
                while (t.elapsed() < minSec * 1000LL || done.load() < files.size()) {
                    for (const QString &p : files) {
                        if (!readNoBuffer(p, &buf)) { failed.fetch_add(1); continue; }
                        double ms = 0;
                        if (decodeBytes(buf, &ms)) ok.fetch_add(1);
                        else failed.fetch_add(1);
                        done.fetch_add(1);
                        if (k == 0 && passes == 0) mine.push_back(ms);
                        if (t.elapsed() >= minSec * 1000LL && done.load() >= files.size()) break;
                    }
                    if (k == 0) ++passes;
                    if (t.elapsed() >= minSec * 1000LL) break;
                }
                std::lock_guard<std::mutex> lk(mu);
                lat.insert(lat.end(), mine.begin(), mine.end());
            });
        }
        for (auto &x : pool) x.join();
        const double sec = t.nsecsElapsed() / 1e9;
        const double pages = double(ok.load()) + double(failed.load());
        const double mbRead = pages * (setMb / files.size());
        std::printf("[both   ] %d thread%-9s: %7.1f pages/s  %6.1f MB/s  p50/p95/max %s ms  (ok=%d fail=%d)\n",
                    th, th == 1 ? "" : "s", pages / sec, mbRead / sec,
                    qPrintable(statsOf(lat).line()), ok.load(), failed.load());
        std::fflush(stdout);
    }
    return 0;
}
