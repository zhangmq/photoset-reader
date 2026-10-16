#include "testsupport.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QString>
#include <QStringLiteral>
#include <QTimer>
#include <QVariant>
#include <QVector>

#include "browsemodel.h"
#include "browsemodelview.h"      // ★ T2d-1b：`ui.browse`
#include "itemsource.h"
#include "librarycore.h"
#include "librarymodel.h"
#include "librarystore.h"
#include "sequence.h"
#include "uimodel.h"
#include "viewermodel.h"

// ══ 主线接线自检（ib-test-wiring；原先由 `image-browser --selftest-wiring` 跑）══
//   T0 第四批机械搬迁：函数体**一字未改**（含 footer 的 `\n[wireselftest] 全部通过`
//   与 33 项 PASS 口径 —— **不套 `testsupport::Check`**，与前几批的 B 方案一致）。
//   ★ 入口样板（QApplication / 应用名）在 `testsupport::suiteMain`（设计 14 §11）。
using testsupport::waitFor;

static int runWiringSelfTest()
{
    const QString album = QFileInfo(QStringLiteral(".testdata/vol01")).absoluteFilePath();
    if (!QFileInfo(album).isDir())
        return testsupport::skip(QStringLiteral("wireselftest"),
                                 QStringLiteral("素材缺失 %1 —— 未做任何检查（不算通过）").arg(album));

    // 自检用：`PHOTOSET_WIRING_DIR` 可把这次自检的库固定到一个目录（**只为截图核对**：
    //   扫描后的图集库 / 真条目要能留在盘上）；不设则用 QTemporaryDir，跑完即删。
    const QByteArray fixedDir = qgetenv("PHOTOSET_WIRING_DIR");
    QTemporaryDir tmp;
    const QString base = fixedDir.isEmpty() ? tmp.path() : QString::fromLocal8Bit(fixedDir);
    if (base.isEmpty() || !QDir().mkpath(base)) {
        qWarning() << "[wireselftest] 无法建临时目录";
        return 1;
    }
    const QString cfgDir = base + QStringLiteral("/config");
    QDir().mkpath(cfgDir);
    qputenv("PHOTOSET_CONFIG_DIR", cfgDir.toLocal8Bit());          // 设置 + 库都在临时目录
    qputenv("PHOTOSET_LIBRARY_DB", (cfgDir + QStringLiteral("/library.db")).toLocal8Bit());

    // 统一断言器（T0 收尾批：footer 带项数、rc 0/1）；打印格式与旧 lambda 一字不差
    testsupport::Check check(QStringLiteral("wireselftest"));

    library::Store store;
    QString err;
    check(store.open(&err), QStringLiteral("打开临时库（%1）")
                                .arg(err.isEmpty() ? library::Store::databasePath() : err));
    // ★ 设置不再有单例：`load()` 在 `UiModel` 构造里做（设计 14 §9.3）
    app::UiModel ui(&store);

    // ── ⓪ 起点：图集库屏；库列表模型可用；**初始没有 viewer**（没打开来源 ⇒ null）──
    //   用户明确：null 没问题，空 viewer 没用 ⇒ QML 侧靠 `ui.viewer ? ... : 默认值` 守卫
    check(ui.screen() == app::UiModel::Screen::Library, QStringLiteral("初始屏 = 图集库"));
    check(ui.viewerCore() == nullptr, QStringLiteral("初始没有 viewer（ui.viewer == null）"));
    check(ui.browse() != nullptr, QStringLiteral("ui.browse 就绪（浏览界面模型**从 viewer 里搬出来**）"));
    check(ui.library() != nullptr, QStringLiteral("ui.library 就绪（图集库列表来自 SQLite）"));

    // ── ① 添加图集 → 库里有这条集合 + **自动开扫** ──
    const int collectionsBefore = store.collections().size();
    ui.library()->addCollection(album, QStringLiteral("folder"));
    qint64 cid = -1;
    for (const library::Collection &c : store.collections())
        if (c.rootPath == album) cid = c.id;
    check(cid > 0, QStringLiteral("添加图集：库里有这条集合（id=%1）").arg(cid));
    check(store.collections().size() == collectionsBefore + 1,
          QStringLiteral("库里恰好多了 1 条集合（%1 → %2）")
              .arg(collectionsBefore).arg(store.collections().size()));
    //   ★ T2c-2b-2：判定条件从库里的 `scan_state` 列改成 **core 的内存态**（列已删）——
    //     "加入图集 ⇒ 自动重置"：`collectionAdded` → `core->requestReset`。
    const LibraryCore::ResetState rs0 = ui.core() ? ui.core()->resetState(cid)
                                                  : LibraryCore::ResetState::Idle;
    check(cid > 0 && (rs0 == LibraryCore::ResetState::Queued
                      || rs0 == LibraryCore::ResetState::Running),
          QStringLiteral("加入图集 → **自动重置**（core 状态 = Queued/Running，实际 %1）")
              .arg(resetq::resetStateName(rs0)));

    // ── ② 等重置结束（有超时）→ core Done + 图数 > 0 ──
    QElapsedTimer clock;
    clock.start();
    auto stillResetting = [&] {
        if (cid <= 0 || !ui.core()) return false;
        const LibraryCore::ResetState st = ui.core()->resetState(cid);
        return st == LibraryCore::ResetState::Queued || st == LibraryCore::ResetState::Running;
    };
    while (stillResetting() && clock.elapsed() < 60000) {
        QEventLoop loop;                       // 跑事件循环：让 core 的 pump/落库结果送达
        QTimer::singleShot(20, &loop, &QEventLoop::quit);
        loop.exec();
    }
    check(ui.core() && ui.core()->resetState(cid) == LibraryCore::ResetState::Done,
          QStringLiteral("重置结束：core 状态 = Done（实际 %1，耗时 %2 ms）")
              .arg(ui.core() ? resetq::resetStateName(ui.core()->resetState(cid))
                             : QStringLiteral("(无 core)"))
              .arg(clock.elapsed()));
    // ★ v6：图数**现算**（`GROUP BY`），不是列
    const int scannedItems = store.itemCount(cid);
    check(scannedItems > 0, QStringLiteral("扫描结果 itemCount() > 0（实际 %1）").arg(scannedItems));
    {
        int imgs = 0, nodes = 0;
        for (const library::Item &it : store.items(cid))
            (it.kind == QLatin1String("image") ? imgs : nodes)++;
        check(imgs == scannedItems,
              QStringLiteral("item 表：%1 图 + %2 容器行；itemCount() = 图数（%3）")
                  .arg(imgs).arg(nodes).arg(scannedItems));
    }

    // ── ③ 打开集合 → 文件浏览：**真条目**（库查询，逐项同序）──
    check(ui.openCollection(cid), QStringLiteral("openCollection(%1)").arg(cid));
    check(ui.screen() == app::UiModel::Screen::Browse, QStringLiteral("screen = Browse"));
    fetch::ViewerModel *vm = ui.viewerCore();
    check(vm != nullptr, QStringLiteral("打开集合后 viewer 已建"));
    check(waitFor([&] {
              auto *v = ui.viewerCore();
              return v && v->itemCount() == scannedItems
                     && ui.browse()->items()->rowCount() == scannedItems;
          }, 8000),
          QStringLiteral("viewer 条目数与浏览行数 = 库条目数（%1）").arg(scannedItems));
    vm = ui.viewerCore();
    auto rowData = [&](int row, int role) -> QVariant {
        if (!ui.browse()) return {};
        return ui.browse()->items()->index(row, 0).data(role);
    };

    // 首项 name 与**磁盘**一致（顺序也是同一份自然排序：Sequence 的唯一实现）
    Sequence diskSeq;
    diskSeq.buildFromFolder(album, /*recursive=*/true);
    QString firstOnDisk, firstOnDiskAbs;
    for (const Sequence::Item &it : diskSeq.items()) {
        if (it.kind == Sequence::Kind::FolderBoundary) continue;
        firstOnDisk = it.displayName;
        firstOnDiskAbs = it.diskPath();
        break;
    }
    const QString firstName = rowData(0, BrowseModel::NameRole).toString();
    check(!firstOnDisk.isEmpty() && firstName == firstOnDisk,
          QStringLiteral("首行 name 与磁盘一致（界面 '%1' / 磁盘 '%2'）").arg(firstName, firstOnDisk));

    // ── ④ 打开第一行 → 图像查看；viewer **真出图** ──
    check(ui.browse()->open(0), QStringLiteral("ui.browse.open(0) → 图像查看（压栈）"));
    check(ui.screen() == app::UiModel::Screen::Viewer, QStringLiteral("screen = Viewer"));
    check(ui.browse()->isContainerRow(0) == false, QStringLiteral("第 0 行是图（不是容器行）"));
    const bool gotImage = waitFor([&] {
        auto *v = ui.viewerCore();
        return v && v->keyCount() > 0 && !v->imageA().isNull();
    }, 8000);
    const int  gotKeys  = ui.viewerCore() ? ui.viewerCore()->keyCount() : -1;
    const QSize gotSize = ui.viewerCore() ? ui.viewerCore()->imageA().size() : QSize();
    check(gotImage, QStringLiteral("viewer 取到真图（keys %1 / imageA %2x%3）")
                        .arg(gotKeys).arg(gotSize.width()).arg(gotSize.height()));
    const int cursorAfterOpen = ui.viewerCore() ? ui.viewerCore()->cursor() : -1;
    check(cursorAfterOpen == 0, QStringLiteral("游标落在第 0 条（实际 %1）").arg(cursorAfterOpen));

    // ── ⑤ 阅读位置写入磁盘：跳到第 4 条 → 400ms 去抖 → 库里 resumeKey = 该条**label 链的 JSON**，
    //        resume_page = 页号（★ v5：不拼串、不存下标）──
    app::LibraryItemSource src(&store, cid);
    const QString wantKey = Sequence::encodeChain(src.chainAt(4));
    const int wantPage = src.pageAt(4);
    ui.viewerCore()->jump(4);
    const bool resumeSaved = waitFor([&] {
        return store.collection(cid).resumeKey == wantKey
               && store.collection(cid).resumePage == wantPage;
    }, 6000);
    check(resumeSaved,
          QStringLiteral("阅读位置写入磁盘（**label 链 JSON + 页号**）：库里 = '%1' / %2（实际 '%3' / %4）")
              .arg(wantKey).arg(wantPage)
              .arg(store.collection(cid).resumeKey).arg(store.collection(cid).resumePage));
    //   ★ 跨会话还原：链 + 页号 → 沿链命中同一条
    check(src.indexOfChain(Sequence::decodeChain(store.collection(cid).resumeKey),
                           store.collection(cid).resumePage) == 4,
          QStringLiteral("resume 往返：链 + 页号 → 回到第 4 条"));

    // ── ⑥ 切双幅 ⇒ 图集级阅读版面写入磁盘（会话来源不落）──
    ui.viewerCore()->setDoublePage(true);
    const bool layoutSaved = waitFor([&] {
        return store.collection(cid).readingLayout == QLatin1String("double");
    }, 5000);
    check(layoutSaved,
          QStringLiteral("阅读版面写入磁盘：collection.readingLayout = 'double'（实际 '%1'）")
              .arg(store.collection(cid).readingLayout));

    // ── ⑥ back() 逐级退：图像查看 → 文件浏览 ──
    ui.back();
    check(ui.screen() == app::UiModel::Screen::Browse,
          QStringLiteral("back → 文件浏览（实际屏 %1）").arg(int(ui.screen())));

    // ── ⑦ 重新打开该集合：**位置与版面都恢复** ──
    check(ui.openCollection(cid), QStringLiteral("再次 openCollection(%1)").arg(cid));
    const bool restored = waitFor([&] {
        auto *v = ui.viewerCore();
        return v && v->doublePage() && v->cursor() == 4;
    }, 8000);
    check(restored, QStringLiteral("恢复：版面 = 双幅，位置 = 第 4 条（实际 doublePage=%1 cursor=%2）")
                        .arg(ui.viewerCore() && ui.viewerCore()->doublePage()
                                 ? QStringLiteral("true") : QStringLiteral("false"))
                        .arg(ui.viewerCore() ? ui.viewerCore()->cursor() : -1));

    // ── ⑧ back() → 图集库 ──
    check(ui.screen() == app::UiModel::Screen::Browse, QStringLiteral("重开后 screen = Browse"));
    ui.back();
    check(ui.screen() == app::UiModel::Screen::Library, QStringLiteral("back → 图集库"));

    // ── ⑨ 直接打开**一条路径** → 会话来源，且**不产生新的集合记录** ──
    const int collectionsBeforeDirect = store.collections().size();
    const QString resumeBeforeDirect = store.collection(cid).resumeKey;
    ui.openPath(firstOnDiskAbs);
    check(ui.screen() == app::UiModel::Screen::Browse, QStringLiteral("直接打开 → 文件浏览"));
    const bool oneRow = waitFor([&] {
        return ui.browse() && ui.browse()->items()->rowCount() == 1;
    }, 8000);
    const int rowsNow = ui.browse() ? ui.browse()->items()->rowCount() : -1;
    check(oneRow, QStringLiteral("单张图片 → 1 条（实际 %1）").arg(rowsNow));
    check(store.collections().size() == collectionsBeforeDirect,
          QStringLiteral("库里**没有**新增集合记录（%1 条未变）").arg(collectionsBeforeDirect));
    // 会话来源**不写入磁盘**：翻动游标不会写任何集合的 resume
    ui.viewerCore()->jump(0);
    waitFor([] { return false; }, 700);            // 跑 400ms 去抖 + 余量
    check(store.collection(cid).resumeKey == resumeBeforeDirect,
          QStringLiteral("会话来源不写入磁盘：图集 resumeKey 未被改写（'%1'）")
              .arg(store.collection(cid).resumeKey));

    // ── ⑩ 界面要调的入口**都存在**（Q_INVOKABLE）──
    //   QML 里写错方法名不会在加载时报错，只在**点击那一刻**失败 → 这里按元对象签名点一遍。
    //   （对话框只查"存在"，**不调用**：离屏不能弹文件对话框）
    {
        const QStringList needed = {
            QStringLiteral("openCollection(qlonglong)"),
            QStringLiteral("back()"),                    QStringLiteral("goLibrary()"),
            QStringLiteral("collectionIdAt(int)"),
            QStringLiteral("chooseAddCollection()"),     QStringLiteral("chooseOpenFolder()"),
            QStringLiteral("chooseOpenArchive()"),       QStringLiteral("chooseOpenImage()"),
            QStringLiteral("removeCollection(int)"),
            // ★ T2c-2b-2：旧的 startScan/cancelScan 已删；QML 调的是这两个。
            QStringLiteral("requestReset(qlonglong)"),   QStringLiteral("cancelReset(qlonglong)"),
        };
        // ★ T2d-1b：**浏览动作搬到 `ui.browse`**（QML 只调它；行语义只有一处实现）
        const QStringList neededBrowse = {
            QStringLiteral("activate(int)"), QStringLiteral("open(int)"),
            QStringLiteral("up()"),          QStringLiteral("crumb(int)"),
            QStringLiteral("isContainerRow(int)"),
        };
        QStringList missing;
        for (const QString &sig : needed)
            if (ui.metaObject()->indexOfMethod(sig.toUtf8().constData()) < 0)
                missing << QStringLiteral("ui.") + sig;
        for (const QString &sig : neededBrowse)
            if (ui.browse()->metaObject()->indexOfMethod(sig.toUtf8().constData()) < 0)
                missing << QStringLiteral("ui.browse.") + sig;
        check(missing.isEmpty(),
              QStringLiteral("界面要调的 %1 个入口都在 ui / ui.browse 上（缺：%2）")
                  .arg(needed.size() + neededBrowse.size())
                  .arg(missing.isEmpty() ? QStringLiteral("无") : missing.join(QLatin1Char(','))));
    }

    return check.finish();
}

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, &runWiringSelfTest);
}
