#include "devcli.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QString>
#include <QStringLiteral>
#include <QThread>
#include <QTimer>

#include <archive.h>
#include <archive_entry.h>

#include "archive.h"
#include "sequence.h"
#include "sevenzcheck.h"
#include "uimodel.h"

// ══ 开发/验收用 CLI 的实现（设计 14 §10）：从 `main.cpp` **整段搬出** ══
//   `main.cpp` 里只剩 `QCommandLineOption` 的声明/解析 + 一次转发调用。
//
//   2026-10-31：「浏览 / 图集库」删除 ⇒ `--dev-add-collection` / `--dev-open-collection` /
//   `--dev-open-page` / `--dev-enter-row` 与它们的等待逻辑一起删掉；只留探针与 `--dev-notify`。

namespace devcli {

// libarchive 能力探针：格式支不支持**随机读**（能不能一行判定 solid）
int runLibarchiveProbe(const QString &path)
{
    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, path.toUtf8().constData(), 65536) != ARCHIVE_OK) {
        qWarning().noquote() << QStringLiteral("[probe-la] 打不开：%1").arg(path);
        archive_read_free(a);
        return 1;
    }
    qInfo().noquote() << QStringLiteral("[probe-la] %1").arg(QFileInfo(path).fileName());

    // 走两条，再试 seek（**能 seek = 能随机读**）
    struct archive_entry *e = nullptr;
    int n = 0;
    while (archive_read_next_header(a, &e) == ARCHIVE_OK) {
        if (++n >= 2) break;
        archive_read_data_skip(a);
    }
    const la_int64_t rEnd = archive_seek_data(a, 0, SEEK_END);
    const la_int64_t rSet = archive_seek_data(a, 0, SEEK_SET);
    qInfo().noquote() << QStringLiteral("[probe-la]   seek_data(END) → %1   seek_data(SET,0) → %2   ⇒ %3")
                             .arg(rEnd).arg(rSet)
                             .arg(rEnd >= 0 ? QStringLiteral("能随机读 ✓") : QStringLiteral("不能随机读 ✗"));
    archive_read_free(a);
    return 0;
}

// ══ 临时探针（--probe-archive <路径>）：量"从头跳过 n"的真实损耗 ══════════
int runArchiveProbe(const QString &path)
{
    using namespace fetch;
    ArchiveSource a;
    if (!a.open(path, QString(), ArchiveSource::Entries::All)) {
        qWarning().noquote() << QStringLiteral("[probe-arc] 打不开：%1").arg(path);
        return 1;
    }
    {   // 7z/cb7：先报"能不能随机读"的判定
        const QString suf = QFileInfo(path).suffix().toLower();
        if (suf == QLatin1String("7z") || suf == QLatin1String("cb7")) {
            QString why;
            const bool ok = sevenZipIsRandomAccessible(path, &why);
            qInfo().noquote() << QStringLiteral("[probe-arc] 7z 判定：%1（%2）")
                                     .arg(ok ? QStringLiteral("能随机读 ✓ 支持")
                                             : QStringLiteral("不能随机读 ✗ 不支持"))
                                     .arg(why);
        }
    }
    qint64 totalBytes = 0;
    for (int i = 0; i < a.count(); ++i) totalBytes += a.sizeAt(i);
    qInfo().noquote() << QStringLiteral("[probe-arc] 条目 %1  归档 %2 MB  解压后共 %3 MB  格式 %4")
                             .arg(a.count())
                             .arg(QFileInfo(path).size() / 1048576.0, 0, 'f', 1)
                             .arg(totalBytes / 1048576.0, 0, 'f', 1)
                             .arg(a.formatName());

    // 前缀字节（第 i 条之前的总字节数）= solid 归档"定位到第 i 条"必须解压的量
    QVector<qint64> prefix(a.count() + 1, 0);
    for (int i = 0; i < a.count(); ++i) prefix[i + 1] = prefix[i] + a.sizeAt(i);

    const QVector<int> idx { 0, 1, 5, 20, 50, 100, 200, 500, 1000, a.count() / 2,
                             a.count() - 1 };
    for (int i : idx) {
        if (i < 0 || i >= a.count()) continue;
        QElapsedTimer t;
        t.start();
        const QByteArray d = a.dataAt(i);
        const qint64 ms = qMax<qint64>(1, t.elapsed());
        qInfo().noquote() << QStringLiteral("[probe-arc] dataAt(%1)\t%2 ms\t前缀 %3 MB\t吞吐 %4 MB/s")
                                 .arg(i).arg(t.elapsed())
                                 .arg(prefix[i] / 1048576.0, 0, 'f', 1)
                                 .arg(prefix[i] / 1048576.0 / (ms / 1000.0), 0, 'f', 0);
        if (t.elapsed() > 60000) {
            qInfo().noquote() << QStringLiteral("[probe-arc] 单次已超 60s，停止采样");
            break;
        }
    }

    // 顺序读前 N 条（模拟"扫描读尺寸"的现状）
    const int seqN = qMin(60, a.count());
    QElapsedTimer t;
    t.start();
    qint64 bytes = 0;
    for (int i = 0; i < seqN; ++i) {
        bytes += a.dataAt(i).size();
        if (t.elapsed() > 60000) { qInfo().noquote() << QStringLiteral("[probe-arc] 顺序读 60s 到第 %1 条"); break; }
    }
    {   // 顺带：真扫一遍这个包（验证"归档内子目录"修好后能不能全扫到）
        Sequence sq;
        QElapsedTimer st;
        st.start();
        sq.startScan(path, 5, false);
        while (!sq.scanFinished() && st.elapsed() < 300000) {
            sq.drain();
            QThread::msleep(5);
        }
        sq.drain();
        qInfo().noquote() << QStringLiteral("[probe-arc] 真实扫描：%1 条  状态 %2  用时 %3 ms")
                                 .arg(sq.count())
                                 .arg(sq.state() == Sequence::State::Scaned
                                          ? QStringLiteral("scanned")
                                          : QStringLiteral("stopped/other"))
                                 .arg(st.elapsed());
    }
    qInfo().noquote() << QStringLiteral("[probe-arc] 顺序前 %1 条\t%2 ms\t%3 MB\t每条 %4 ms")
                             .arg(seqN).arg(t.elapsed()).arg(bytes / 1048576.0, 0, 'f', 1)
                             .arg(double(t.elapsed()) / seqN, 0, 'f', 1);
    return 0;
}

// ── `--dev-*` 的执行入口 ───────────────────────────────────────────────────
void runDevCommands(const DevRequest &req, app::UiModel *ui)
{
    if (!req.any())
        return;
    if (req.notifySet) {
        const QString text = req.notify;
        QTimer::singleShot(300, ui, [ui, text] { ui->notify(text); });
        qInfo().noquote() << QStringLiteral("[dev] 瞬时提示：%1").arg(text);
    }
}

} // namespace devcli
