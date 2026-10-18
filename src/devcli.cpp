#include "devcli.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QString>
#include <QStringLiteral>
#include <QThread>
#include <QTimer>

#include <functional>

#include <archive.h>
#include <archive_entry.h>

#include "archive.h"
#include "browsemodel.h"
#include "librarycore.h"
#include "librarymodel.h"
#include "resetqueue.h"
#include "librarystore.h"
#include "sequence.h"
#include "sevenzcheck.h"
#include "browsemodelview.h"
#include "uimodel.h"

// ══ 开发/验收用 CLI 的实现（设计 14 §10）：从 `main.cpp` **整段搬出**，行为一字不变 ══
//   `main.cpp` 里只剩 `QCommandLineOption` 的声明/解析 + 一次转发调用。

namespace devcli {

// ★ 刻意本地化（2026-10-14 dev 工具收拾批复核）：下面自带的 `waitFor` 与
//   `testsupport` 里的同名物**重复是故意的** —— dev CLI 随应用编，而应用
//   **不链 `testsupport`**（设计 14 §11）；不为一个小工具把测试脚手架拉进产品二进制
//   （保持"应用库 = 只是应用"）。

// ── 轮询到条件成立：与 `testsupport::waitFor` **同一实现** ──────────────────
//   ⚠️ 这里必须自带一份：dev CLI 随应用编，而应用**不链 `testsupport`**（设计 14 §11）。
static bool waitFor(const std::function<bool()> &done, int timeoutMs = 3000)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (done()) return true;
        QThread::msleep(1);
    }
    QCoreApplication::processEvents();
    return done();
}

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

// ★ T2c-2b-2：旧的"双实例同步验证"三个模式（`--scan-run` / `--scan-stop` / `--db-state`）
//   **已删** —— 它们建在 `ScanController` + `scan_state` 列 + `action` 表之上，
//   而这套机制已整体作废（设计 14 §9.6）：新模型里状态只在内存、停止只发生在本进程。

// ── `--dev-*` 全块：从 `main.cpp` 整段搬出（`parser.isSet/value(xxxOpt)` 换成
//    `req.xxxSet` / `req.xxx`）────────────────────────────────────────────
//   ★ T2c-2b-2：两处"等扫描结束"的判定条件从 `collection.scan_state` 列改成 **core 的状态**
//     （列已删）；其余逻辑与文案未改。
void runDevCommands(const DevRequest &req, app::UiModel *ui, library::Store *store)
{
    if (!req.any())
        return;                                  // 等价于旧 `if (parser.isSet(...) || ...)`
    // 先把在扫的都等完（集合可能是上一次运行留下的）。
    //   ★ T2c-2b-2：`scan_state` 列已删 ⇒ 判定条件改用 **core 的重置状态**（内存态）：
    //     "还在扫" = 状态 Queued/Running **且**活序列还没写完终态。
    //     ⚠️ **不等落库窗口**（`scanFinished()` 之后就是窗口）：落库期间卡片仍是
    //        「扫描中」（`resetState == Running`），但内容已定 —— 旧行为等的是"扫描结束"，
    //        这里保持同一语义（旧代码等的也是"状态离开 NotReady/ImageFound"）。
    auto resetBusy = [ui]() -> bool {
        if (!ui->core() || !ui->core()->libraryUsable()) return false;
        for (const library::Collection &c : ui->core()->collections()) {
            const auto st = ui->core()->resetState(c.id);
            if (st == LibraryCore::ResetState::Queued) return true;
            if (st != LibraryCore::ResetState::Running) continue;
            Sequence *s = ui->core()->resetSequence(c.id);
            if (s && !s->scanFinished()) return true;
        }
        return false;
    };
    QElapsedTimer devClock;
    devClock.start();
    while (devClock.elapsed() < 180000) {
        if (!resetBusy()) break;
        QEventLoop loop;
        QTimer::singleShot(20, &loop, &QEventLoop::quit);
        loop.exec();
    }
    if (req.addSet) {
        const QString d = req.add;
        if (!store->isOpen()) {
            qWarning().noquote() << QStringLiteral("[dev] 库不可用，--dev-add-collection 无效");
        } else {
            for (const library::Collection &c : store->collections())
                if (c.rootPath == QFileInfo(d).absoluteFilePath())
                    qInfo().noquote() << QStringLiteral("[dev] 集合已存在 id=%1（会重扫）").arg(c.id);
            ui->library()->addCollection(d, Sequence::Kind::Folder);
        }
        // 等这次自动重置"扫完"（有超时；语义同旧代码：等状态离开扫描中）
        devClock.restart();
        while (devClock.elapsed() < 180000) {
            if (!resetBusy()) break;
            QEventLoop loop;
            QTimer::singleShot(20, &loop, &QEventLoop::quit);
            loop.exec();
        }
        for (const library::Collection &c : store->collections())
            qInfo().noquote() << QStringLiteral("[dev] 集合 %1  %2  %3 条  状态=%4")
                                     .arg(c.id).arg(c.rootPath)
                                     .arg(store->itemCount(c.id))
                                     .arg(ui->core()
                                              ? resetq::resetStateName(
                                                    ui->core()->resetState(c.id))
                                              : QStringLiteral("(无 core)"));
    }
    if (req.openCollectionSet) {
        const int row = qMax(0, req.openCollection.toInt());
        const qint64 cid = ui->collectionIdAt(row);
        // ★ T2c-2b-2：**落库窗口内不许打开图集**（UiModel 会拒绝 —— 那时 core 的查看槽
        //   别名到活序列，挂 viewer 会形成两个消费者）。上一段只等到"扫描结束"，窗口可能
        //   还开着（无延迟时通常 <1 ms）⇒ 这里等到窗口关闭，去掉那次竞态。
        if (ui->core() && cid > 0 && ui->core()->resetCommitting(cid)) {
            QElapsedTimer w;
            w.start();
            while (ui->core()->resetCommitting(cid) && w.elapsed() < 30000) {
                QEventLoop loop;
                QTimer::singleShot(10, &loop, &QEventLoop::quit);
                loop.exec();
            }
        }
        qInfo().noquote() << QStringLiteral("[dev] 打开集合 row=%1 id=%2").arg(row).arg(cid);
        if (!ui->openCollection(cid))
            qWarning().noquote() << QStringLiteral("[dev] openCollection 失败（row=%1）").arg(row);
    }
    if (req.enterRowSet || req.openPageSet) {
        // 浏览行是**驱动线程 post 回来的**（异步）⇒ 不等它，`openRow(row)` 会找不到
        //   那一行（实测：`openRow(6) 失败`）。这里等一会儿（只为开发期 CLI）。
        // ★ T2d-1b：浏览行归 `ui.browse`（**换层是同步的**，这里的等待只是兜底）
        if (!waitFor([&] {
                auto *b = ui->browse();
                return b && b->items()->rowCount() > 0;
            }, 15000))
            qWarning().noquote() << QStringLiteral("[dev] ⚠ 等不到浏览行（15s）");
    }
    if (req.enterRowSet) {
        // 支持**多层**（逗号分隔）：每层进入后都要等这一层真的换过来（进入是异步的）
        const QStringList parts = req.enterRow
                                      .split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &p : parts) {
            const int n = qMax(0, p.trimmed().toInt());
            if (!ui->viewerCore()) {
                qWarning().noquote() << QStringLiteral(
                    "[dev] --dev-enter-row 需要先 --dev-open-collection（没有 viewer）");
                break;
            }
            // 进入前记下当前层的"签名"（行数 + 首行名）：**换层是异步的**，
            //   只等 rowCount>0 会拿**旧层**的行号继续进入（实测：第 7 行进入后
            //   立刻 enterRow(0) 用的是旧层的 p0.png ⇒ 失败）。
            auto layerSig = [&]() -> QString {
                auto *b = ui->browse();
                if (!b) return QString();
                const int n = b->items()->rowCount();
                const QString first = n > 0
                    ? b->items()->index(0, 0).data(::BrowseModel::NameRole).toString()
                    : QString();
                return QStringLiteral("%1|%2").arg(n).arg(first);
            };
            const QString before = layerSig();
            if (!(ui->browse() && ui->browse()->activate(n))) {
                qWarning().noquote() << QStringLiteral("[dev] enterRow(%1) 失败").arg(n);
                break;
            }
            waitFor([&] { return layerSig() != before; }, 8000);
            qInfo().noquote() << QStringLiteral("[dev] 已进入第 %1 行的容器（留在浏览屏）").arg(n);
        }
    }
    if (req.notifySet) {
        const QString text = req.notify;
        QTimer::singleShot(300, ui, [ui, text] { ui->notify(text); });
        qInfo().noquote() << QStringLiteral("[dev] 瞬时提示：%1").arg(text);
    }
    if (req.openPageSet) {
        const int n = qMax(0, req.openPage.toInt());
        if (!ui->viewerCore()) {
            qWarning().noquote() << QStringLiteral(
                "[dev] --dev-open-page 需要先 --dev-open-collection（没有 viewer）");
        } else if (!(ui->browse() && ui->browse()->open(n))) {
            qWarning().noquote() << QStringLiteral("[dev] openRow(%1) 失败").arg(n);
        } else {
            qInfo().noquote() << QStringLiteral("[dev] 打开第 %1 行 → 图像查看屏").arg(n);
        }
    }
    qInfo().noquote() << QStringLiteral("[dev] 屏 = %1（0=图集库 1=文件浏览 2=图像查看）")
                             .arg(int(ui->screen()));
}

} // namespace devcli
