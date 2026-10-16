#include "testsupport.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariantMap>
#include <QVector>

#include "browsemodel.h"
#include "seqwindow.h"
#include "sequence.h"
#include "thumburl.h"

// ══ 发布物自检（`ib-test-seqwindow`；T2d-1a）══════════════════════════════════
//   被测：`fetch::Window` + `fetch::WindowBuilder`（`src/seqwindow.{h,cpp}`）。
//
//   ★ **硬性判定条件**：窗口的 `screenCount / screenOf / screenFirst / screenItems / anchorOf /
//     advance / groupAt` 与**直接用 `Sequence` 算出来的**逐条一致 —— 算法只有 `Sequence`
//     一份，窗口只是"把结果取下来"。
//   ★ 另验：有界 + 越界行为、live 增量、browse 一层、OSD 标量、版本与等价、阴性对照。
//   ★ **不许用 `ViewerModel`**（T2d-1b 才改它）：期望值一律由 `Sequence` 直接算或写明常量。
static int runSeqWindowSelfTest()
{
    using namespace fetch;
    testsupport::Check check(QStringLiteral("seqwindow"));

    // ── 夹具 ────────────────────────────────────────────────────────────────
    // ① 合成序列（**无树**）：尺寸自己摆（宽图单独成屏）
    auto synthItems = [](int n, const QSet<int> &wide, const QSet<int> &missing) {
        QVector<Sequence::Item> v;
        for (int i = 0; i < n; ++i) {
            Sequence::Item it;
            it.kind = Sequence::Kind::Image;
            it.ord = i;
            it.label = QStringLiteral("k%1").arg(i);
            it.displayName = it.label;
            it.loc.diskPath = QStringLiteral("/synth/%1.png").arg(i);
            it.size = wide.contains(i) ? QSize(3000, 1200) : QSize(1200, 1800);
            it.missing = missing.contains(i);
            v << it;
        }
        return v;
    };
    auto fillSynth = [&synthItems](Sequence &seq, int n, const QSet<int> &wide = {},
                                   const QSet<int> &missing = {}) {
        seq.buildFromEntries(QStringLiteral("/synth"), synthItems(n, wide, missing));
    };

    // ② 小树（目录 / 压缩包 / 图片混合）—— 行按 DFS 前序（与扫描产出同一形状）
    //   条目下标：0=a.png 1=z.png 2=p1 3=p2 4=p3(宽) 5=s1 6=b1 7=b2
    //   节点下标：0=vol01(根) 1=photos 2=sub 3=book.cbz
    auto fillTree = [](Sequence &seq) {
        auto row = [](int ord, int parentOrd, const QString &kind, const QString &label, int w,
                      int h) {
            Sequence::Row r;
            r.ord = ord;
            r.parentOrd = parentOrd;
            r.kind = kind;
            r.label = label;
            r.pageIndex = -1;
            r.width = w;
            r.height = h;
            return r;
        };
        QVector<Sequence::Row> rows;
        rows << row(0, -1, QStringLiteral("image"), QStringLiteral("a.png"), 1200, 1800);
        rows << row(1, -1, QStringLiteral("image"), QStringLiteral("z.png"), 1200, 1800);
        rows << row(2, -1, QStringLiteral("folder"), QStringLiteral("photos"), 0, 0);
        rows << row(3, 2, QStringLiteral("image"), QStringLiteral("p1.png"), 1200, 1800);
        rows << row(4, 2, QStringLiteral("image"), QStringLiteral("p2.png"), 1200, 1800);
        rows << row(5, 2, QStringLiteral("image"), QStringLiteral("p3.png"), 3000, 1200);
        rows << row(6, 2, QStringLiteral("folder"), QStringLiteral("sub"), 0, 0);
        rows << row(7, 6, QStringLiteral("image"), QStringLiteral("s1.png"), 1200, 1800);
        rows << row(8, -1, QStringLiteral("archive"), QStringLiteral("book.cbz"), 0, 0);
        rows << row(9, 8, QStringLiteral("image"), QStringLiteral("b1.png"), 800, 1200);
        rows << row(10, 8, QStringLiteral("image"), QStringLiteral("b2.png"), 800, 1200);
        seq.buildFromRows(QStringLiteral("/synth/vol01"), rows, false, QStringLiteral("vol01"));
    };

    // 期望值：屏起点表（**直接用 Sequence 算**，与窗口对照）
    auto expectedStarts = [](const Sequence &seq, Sequence::Mode m, int lo, int hi) {
        QVector<int> out;
        for (int i = lo; i < hi; ++i) {
            const int s = seq.anchorOf(i, m);
            if (s < 0) continue;
            if (out.isEmpty() || out.last() != s) out << s;
        }
        return out;
    };
    auto sameGroup = [](const Sequence::Group &a, const Sequence::Group &b) {
        return a.cursor == b.cursor && a.members == b.members;
    };

    // 期望值：OSD（**直接用 Sequence 算** —— 与 `ViewerModel::buildScreenInfo` 同一口径）
    struct Osd {
        int totalPages = 0;
        int currentPage = -1;
        QString relativePath, fileLabel, sizeLabel, pageKindLabel, folderLabel;
        QString prevFolderLabel, nextFolderLabel;
    };
    auto expectedOsd = [](const Sequence &seq, int cursor, Sequence::Mode mode) {
        Osd o;
        o.totalPages = seq.screenCount(mode);
        o.currentPage = seq.screenOf(cursor, mode);
        const int n = seq.count();
        if (n <= 0) return o;
        const int i = qBound(0, cursor, n - 1);
        const Sequence::Item &it = seq.at(i);
        o.fileLabel = it.displayName.isEmpty() ? it.label : it.displayName;
        o.sizeLabel = it.size.isValid()
                          ? QStringLiteral("%1×%2").arg(it.size.width()).arg(it.size.height())
                          : QString();
        o.pageKindLabel = (it.kind == Sequence::Kind::Archive) ? QStringLiteral("压缩包")
                          : (it.pageIndex() > 0 || !it.members().isEmpty())
                              ? QStringLiteral("容器内页")
                              : QStringLiteral("图片");
        o.folderLabel = it.folderLabel;
        o.relativePath = it.members().isEmpty()
                             ? it.diskPath()
                             : QFileInfo(it.diskPath()).fileName() + QStringLiteral("!/")
                                   + it.members().join(QStringLiteral("!/"));
        const int pn = seq.prevContainerFirst(i);
        const int nn = seq.nextContainerFirst(i);
        if (pn >= 0 && pn < n) {
            const int node = seq.containerOf(pn);
            o.prevFolderLabel = (node >= 0 && node < seq.tree().size()) ? seq.tree().at(node).label
                                                                        : QString();
        }
        if (nn >= 0 && nn < n) {
            const int node = seq.containerOf(nn);
            o.nextFolderLabel = (node >= 0 && node < seq.tree().size()) ? seq.tree().at(node).label
                                                                        : QString();
        }
        return o;
    };

    // 临时库 / 临时设置（**绝不碰真库**）；本套件不写入磁盘，只是守住纪律
    testsupport::TempFixture fx;
    if (fx.isValid()) {
        fx.useLibraryDb();
        fx.useSettingsFile();
    }

    WindowBuilder builder;

    // ══ ① 不许分叉算法：窗口 vs 直接用 Sequence（单幅 + 双幅，含宽图/窄图配对）════
    {
        Sequence seq;
        const QSet<int> wide{ 4, 9, 17 };
        const int n = 24;
        fillSynth(seq, n, wide);

        for (int mi = 0; mi < 2; ++mi) {
            const Sequence::Mode mode = (mi == 0) ? Sequence::Mode::Single
                                                  : Sequence::Mode::Double;
            const QString tag = (mi == 0) ? QStringLiteral("单幅") : QStringLiteral("双幅");
            NavState nav;
            nav.cursor = 10;
            nav.mode = mode;
            auto w = builder.build(seq, nav);

            check(w->count() == seq.count() && w->scanFinished() == seq.scanFinished(),
                  QStringLiteral("① %1：count/finished 与序列一致").arg(tag));
            check(w->screenCount() == seq.screenCount(mode),
                  QStringLiteral("① %1：屏数 %2 = Sequence 的 %3")
                      .arg(tag).arg(w->screenCount()).arg(seq.screenCount(mode)));
            check(w->itemsLo() == 0 && w->itemsHi() == n,
                  QStringLiteral("① %1：余量足够 ⇒ 覆盖全序列 [%2,%3)")
                      .arg(tag).arg(w->itemsLo()).arg(w->itemsHi()));

            bool ofOk = true, ancOk = true, advOk = true, scrOk = true, grpOk = true;
            for (int i = w->itemsLo(); i < w->itemsHi(); ++i) {
                if (w->screenOf(i) != seq.screenOf(i, mode)) ofOk = false;
                if (w->anchorOf(i) != seq.anchorOf(i, mode)) ancOk = false;
                for (int d = 0; d < 2; ++d) {
                    const Sequence::Dir dir = (d == 0) ? Sequence::Dir::Next
                                                       : Sequence::Dir::Prev;
                    if (w->advance(i, dir) != seq.advance(i, mode, dir)) advOk = false;
                }
            }
            const QVector<int> starts = expectedStarts(seq, mode, w->itemsLo(), w->itemsHi());
            const int loK = seq.screenOf(w->itemsLo(), mode);
            for (int k = loK; k < loK + starts.size(); ++k) {
                if (w->screenFirst(k) != starts.at(k - loK)) scrOk = false;
                QVector<int> exp;
                const int from = starts.at(k - loK);
                const int to = (k - loK + 1 < starts.size()) ? starts.at(k - loK + 1) : n;
                for (int i = from; i < to; ++i) exp << i;
                if (w->screenItems(k) != exp) scrOk = false;
                for (int d = 0; d < 2; ++d) {
                    const Sequence::Dir dir = (d == 0) ? Sequence::Dir::Next
                                                       : Sequence::Dir::Prev;
                    if (!sameGroup(w->groupAt(from, mode, dir), seq.groupAt(from, mode, dir)))
                        grpOk = false;
                }
            }
            check(ofOk, QStringLiteral("① %1：screenOf 逐条一致（%2 条）").arg(tag).arg(n));
            check(ancOk, QStringLiteral("① %1：anchorOf 逐条一致").arg(tag));
            check(advOk, QStringLiteral("① %1：advance 两个方向逐条一致").arg(tag));
            check(scrOk, QStringLiteral("① %1：screenFirst/screenItems 逐屏一致（%2 屏）")
                             .arg(tag).arg(starts.size()));
            check(grpOk, QStringLiteral("① %1：groupAt（Next/Prev）逐屏一致").arg(tag));
            check(w->nextScreenFirst(w->itemsLo())
                          == seq.advance(w->itemsLo(), mode, Sequence::Dir::Next)
                      && w->prevScreenFirst(w->itemsHi() - 1)
                             == seq.advance(w->itemsHi() - 1, mode, Sequence::Dir::Prev),
                  QStringLiteral("① %1：nextScreenFirst/prevScreenFirst = advance").arg(tag));
        }

        // 窄图配对跨容器边界（双幅下 s1(5) 与 b1(6) 同屏）—— 窗口照抄序列的分组
        {
            Sequence tree;
            fillTree(tree);
            NavState nav;
            nav.cursor = 5;
            nav.mode = Sequence::Mode::Double;
            auto w = builder.build(tree, nav);
            const Sequence::Group g = tree.groupAt(5, Sequence::Mode::Double, Sequence::Dir::Next);
            check(w->anchorOf(6) == tree.anchorOf(6, Sequence::Mode::Double) && g.members.size() == 2,
                  QStringLiteral("① 双幅：s1+b1 配对成屏（anchor %1 = Sequence 的 %2）")
                      .arg(w->anchorOf(6)).arg(tree.anchorOf(6, Sequence::Mode::Double)));
            check(sameGroup(w->groupAt(5, Sequence::Mode::Double, Sequence::Dir::Next), g),
                  QStringLiteral("① 双幅：跨容器配对的组构成与 Sequence 一致"));
        }
    }

    // ══ ② 窗口有界 + 越界行为 ═══════════════════════════════════════════════
    {
        Sequence seq;
        fillSynth(seq, 2000, QSet<int>{ 1000 });        // 2000 条（其中一条宽图）
        NavState nav;
        nav.cursor = 1000;
        nav.mode = Sequence::Mode::Double;
        nav.screenMargin = 2;                           // 2 屏余量
        nav.prefetchHalf = 3;                           // 3 条预取半宽
        auto w = builder.build(seq, nav);

        check(w->count() == 2000, QStringLiteral("② count = 2000（序列的真实总数）"));
        check(w->windowItemCount() == w->itemsHi() - w->itemsLo(),
              QStringLiteral("② 窗口条目数 = itemsHi-itemsLo"));
        check(w->windowItemCount() < 64,
              QStringLiteral("② 有界：%1 条 << 2000（不是全量镜像）").arg(w->windowItemCount()));

        const int lo = w->itemsLo();
        const int hi = w->itemsHi();
        check(w->coversItem(1000) && w->itemAt(1000) != nullptr,
              QStringLiteral("② 游标在覆盖内 ⇒ itemAt 有值"));
        check(lo > 0 && hi < 2000, QStringLiteral("② 覆盖是**真子区间** [%1,%2)").arg(lo).arg(hi));
        check(w->itemAt(lo - 1) == nullptr && !w->coversItem(lo - 1),
              QStringLiteral("② 左边界外 ⇒ nullptr（不扩窗）"));
        check(w->itemAt(hi) == nullptr && !w->coversItem(hi),
              QStringLiteral("② 右边界外 ⇒ nullptr（不扩窗）"));
        check(w->itemAt(1500) == nullptr && !w->coversItem(1500),
              QStringLiteral("② 远处下标 ⇒ nullptr（绝不偷偷扩窗）"));
        check(w->itemAt(lo) != nullptr && w->itemAt(hi - 1) != nullptr,
              QStringLiteral("② 边界内首末都有值"));

        const Sequence::Item *a = w->itemAt(lo);
        const Sequence::Item &sa = seq.at(lo);
        check(a->cacheKey() == sa.cacheKey() && a->loc.diskPath == sa.loc.diskPath
                  && a->size == sa.size && a->displayName == sa.displayName && a->ord == sa.ord,
              QStringLiteral("② 条目是 Item 的副本（cacheKey/loc/尺寸/身份都在）"));
        check(w->itemAt(lo) == a, QStringLiteral("② 同一不可变窗口里指针稳定（构造后只读）"));

        const int curScreen = w->screenOf(1000);
        check(curScreen >= 0 && w->coversScreen(curScreen),
              QStringLiteral("② 当前屏（第 %1 屏）在覆盖内").arg(curScreen));
        check(!w->coversScreen(curScreen + 20) && w->screenFirst(curScreen + 20) == -1,
              QStringLiteral("② 远处屏 ⇒ coversScreen 假 + screenFirst -1"));
        check(w->screenOf(hi + 50) == -1 && w->anchorOf(hi + 50) == -1,
              QStringLiteral("② 覆盖外 ⇒ screenOf/anchorOf = -1（如实说「不知道」）"));

        BuildOpts wider;
        wider.prefetchHalf = 7;
        wider.screenMargin = 8;
        auto w2 = builder.build(seq, nav, wider);
        check(w2->windowItemCount() > w->windowItemCount()
                  && w2->windowItemCount() < 2000 / 10,
              QStringLiteral("② 窗口随 screenMargin/prefetchHalf 变宽（%1 → %2 条，仍有界）")
                  .arg(w->windowItemCount()).arg(w2->windowItemCount()));

        NavState nav0 = nav;
        nav0.cursor = 0;
        nav0.screenMargin = 0;
        nav0.prefetchHalf = 0;
        auto w0 = builder.build(seq, nav0);
        check(w0->windowItemCount() <= 2,
              QStringLiteral("② 余量 0 ⇒ 只覆盖当前屏（%1 条）").arg(w0->windowItemCount()));
    }

    // ══ ③ live 增量（真实目录：startScan + drain 推进到结束）══════════════════
    {
        QTemporaryDir dir;
        check(dir.isValid(), QStringLiteral("③ 临时目录建好"));
        const QString root = dir.path();
        QDir(root).mkpath(QStringLiteral("sub"));
        auto mkPng = [](const QString &path, int w, int h) {
            QImage img(w, h, QImage::Format_RGB32);
            img.fill(Qt::red);
            return img.save(path, "PNG");
        };
        bool wrote = true;
        wrote = mkPng(root + QStringLiteral("/a.png"), 8, 12) && wrote;
        wrote = mkPng(root + QStringLiteral("/b.png"), 8, 12) && wrote;
        wrote = mkPng(root + QStringLiteral("/c.png"), 30, 8) && wrote;
        wrote = mkPng(root + QStringLiteral("/sub/d.png"), 8, 12) && wrote;
        wrote = mkPng(root + QStringLiteral("/sub/e.png"), 8, 12) && wrote;
        for (int i = 0; i < 150; ++i)                    // 批量：让"扫描中途"是常态
            wrote = mkPng(root + QStringLiteral("/bulk%1.png").arg(i), 8, 12) && wrote;
        check(wrote, QStringLiteral("③ 造了 155 张真实 PNG（含子目录 + 宽图 + 批量）"));

        //   未开始扫描的序列 ⇒ count 0 / finished false（**确定性**的 false 一侧）
        {
            Sequence fresh;
            NavState nav;
            nav.cursor = 0;
            auto w = builder.build(fresh, nav);
            check(w->count() == 0 && !w->scanFinished() && w->itemsHi() == 0,
                  QStringLiteral("③ 未开始 ⇒ count 0 / finished false / 覆盖空"));
            check(w->itemAt(0) == nullptr && w->screenCount() == 0,
                  QStringLiteral("③ 空序列：itemAt nullptr / 屏数 0"));
        }

        Sequence seq;
        NavState nav;
        nav.cursor = 0;
        nav.mode = Sequence::Mode::Double;
        seq.startScan(root, 5, false);

        //   扫描进行中（**消费线程还没 drain**）建的窗口必须如实反映当时的状态
        auto wMid = builder.build(seq, nav);
        check(wMid->count() == 0 && wMid->itemsHi() == 0,
              QStringLiteral("③ 扫描中途（未 drain）：窗口 count 0 / 覆盖空"));

        //   推进到结束（**消费线程 = 本线程**）；每一轮都重建窗口并与序列当时的状态对账
        int builds = 0, partial = 0;
        bool consistent = true;
        for (int guard = 0; guard < 20000 && !seq.scanFinished(); ++guard) {
            seq.drain();
            auto w = builder.build(seq, nav);
            ++builds;
            if (seq.count() > 0 && !seq.scanFinished()) ++partial;
            if (w->count() != seq.count() || w->itemsLo() < 0 || w->itemsHi() > seq.count()
                || w->itemsHi() < w->itemsLo()
                || (w->count() > 0 && w->itemsHi() <= w->itemsLo()))
                consistent = false;
            QThread::msleep(1);
        }
        const bool done = testsupport::waitFor([&] { seq.drain(); return seq.scanFinished(); },
                                               15000);
        check(done, QStringLiteral("③ 真实目录扫描收尾（drain 到终态）"));
        check(consistent,
              QStringLiteral("③ 扫描中途重建 %1 次（其中「有内容但未收尾」%2 次）—— 每次都和序列当时的 count 一致")
                  .arg(builds).arg(partial));

        auto w1 = builder.build(seq, nav);
        check(w1->count() == 155, QStringLiteral("③ 重建后 count = %1（期望 155）").arg(w1->count()));
        check(w1->scanFinished(), QStringLiteral("③ 重建后 finished = true"));
        check(w1->itemsHi() >= wMid->itemsHi() && w1->itemsHi() > 0,
              QStringLiteral("③ 覆盖范围随扫描增长（%1 → %2）")
                  .arg(wMid->itemsHi()).arg(w1->itemsHi()));
        QSet<QString> names;
        for (int i = w1->itemsLo(); i < w1->itemsHi(); ++i)
            names.insert(w1->itemAt(i)->displayName);
        check(names.contains(QStringLiteral("a.png")) && names.contains(QStringLiteral("b.png")),
              QStringLiteral("③ 覆盖里有真实条目（%1 个名字）").arg(names.size()));
        bool keyOk = true;
        for (int i = w1->itemsLo(); i < w1->itemsHi(); ++i)
            if (w1->itemAt(i)->cacheKey() != seq.at(i).cacheKey()) keyOk = false;
        check(keyOk, QStringLiteral("③ 覆盖内每个条目 cacheKey = ord（与序列同一身份）"));
    }

    // ══ ④ 浏览一层（**从序列取**；T2d-1c-① 起**不再跟着有界窗口（`fetch::Window`）**）════════════════════════
    //   被测 = `fetch::browseLayerOf(seq, node, cursor)`：该层的行 + 面包屑 + 能否上一级 +
    //   范围标签 + 高亮行（**唯一实现**；`app::BrowseModel` = QML 的 `ui.browse` 直接吃它）。
    {
        Sequence seq;
        fillTree(seq);
        check(seq.count() == 8 && seq.tree().size() == 4,
              QStringLiteral("④ 夹具：8 条图 + 4 个节点（根/photos/sub/book.cbz）"));

        // 根层（node = -1 ⇒ 解析成根）
        {
            const BrowseLayer L = browseLayerOf(seq, -1, 0);
            const QVector<BrowseModel::Row> &rows = L.rows;
            check(L.node == 0 && rows.size() == 4,
                  QStringLiteral("④ 根层：解析到节点 0，4 行（2 图 + 2 容器）"));
            bool ok = rows.size() == 4
                      && rows.at(0).kind == QStringLiteral("image")
                      && rows.at(0).name == QStringLiteral("a.png") && rows.at(0).itemIndex == 0
                      && rows.at(1).name == QStringLiteral("z.png") && rows.at(1).itemIndex == 1
                      && rows.at(2).kind == QStringLiteral("folder")
                      && rows.at(2).name == QStringLiteral("photos") && rows.at(2).nodeIndex == 1
                      && rows.at(2).subtreeFirst == 2 && rows.at(2).subtreeCount == 4
                      && rows.at(2).sizeText == QStringLiteral("4 张")
                      && rows.at(3).kind == QStringLiteral("archive")
                      && rows.at(3).name == QStringLiteral("book.cbz") && rows.at(3).nodeIndex == 3
                      && rows.at(3).subtreeFirst == 6 && rows.at(3).subtreeCount == 2;
            check(ok, QStringLiteral("④ 根层：行字段逐项正确（图行 + 容器行子树区间/张数）"));
            check(rows.at(0).resText == QStringLiteral("1200×1800")
                      && rows.at(0).thumb == thumbUrlForFile(QStringLiteral("/synth/vol01/a.png"))
                      && rows.at(0).sizeText.isEmpty(),
                  QStringLiteral("④ 图行：resText/thumb/（不存在的文件不报字节数）"));
            check(rows.at(3).thumb
                      == thumbUrlForArchiveMember(QStringLiteral("/synth/vol01/book.cbz"),
                                                  QStringLiteral("b1.png")),
                  QStringLiteral("④ 压缩包容器行：封面 = 包内第 0 页（thumbzip + 成员）"));
            check(L.crumbs.size() == 1
                      && L.crumbs.at(0).toMap().value(QStringLiteral("label")).toString()
                             == QStringLiteral("vol01")
                      && L.crumbs.at(0).toMap().value(QStringLiteral("locked")).toBool(),
                  QStringLiteral("④ 根层面包屑 = [{vol01, locked}]"));
            check(!L.canGoUp && L.rangeLabel == QStringLiteral("vol01"),
                  QStringLiteral("④ 根层：canGoUp 假 / rangeLabel = vol01"));
            check(L.row == 0, QStringLiteral("④ 高亮行 = 0（当前条目 0 = a.png 那行）"));
            check(L.chain == QVector<int>({ 0 }),
                  QStringLiteral("④ 根层祖先链 = {0}（面包屑 / 上一级都用它）"));
        }

        // 进一层：photos（节点 1）
        {
            const BrowseLayer L = browseLayerOf(seq, 1, 2);
            const QVector<BrowseModel::Row> &rows = L.rows;
            bool ok = rows.size() == 4 && rows.at(0).name == QStringLiteral("p1.png")
                      && rows.at(0).itemIndex == 2 && rows.at(2).resText == QStringLiteral("3000×1200")
                      && rows.at(3).kind == QStringLiteral("folder")
                      && rows.at(3).name == QStringLiteral("sub") && rows.at(3).nodeIndex == 2
                      && rows.at(3).subtreeCount == 1;
            check(ok, QStringLiteral("④ 进 photos：3 图 + 子目录 sub（顺序 = 自然序）"));
            check(L.crumbs.size() == 2
                      && L.crumbs.at(0).toMap().value(QStringLiteral("locked")).toBool()
                      && !L.crumbs.at(1).toMap().value(QStringLiteral("locked")).toBool()
                      && L.crumbs.at(1).toMap().value(QStringLiteral("label")).toString()
                             == QStringLiteral("photos"),
                  QStringLiteral("④ 进 photos：面包屑 [{vol01,锁},{photos,不锁}]"));
            check(L.canGoUp && L.rangeLabel == QStringLiteral("photos"),
                  QStringLiteral("④ 进 photos：canGoUp 真 / rangeLabel = photos"));
            check(L.row == 0, QStringLiteral("④ 进 photos：高亮行指向含当前条目那一行"));
            check(L.chain == QVector<int>({ 0, 1 }),
                  QStringLiteral("④ 进 photos：祖先链 = {根 0, photos 1}（上一级 = chain[-2]）"));

            // 容器查询仍住在**有界窗口**上（`loopTarget` / 文件夹循环 / 顶栏要用它）：与 Sequence 同口径
            NavState nav;
            nav.cursor = 2;
            nav.mode = Sequence::Mode::Single;
            auto w = builder.build(seq, nav);
            check(w->containerOf(2) == 1 && w->containerRange(2) == QPair<int, int>(2, 4)
                      && w->prevContainerFirst(2) == 0 && w->nextContainerFirst(2) == 5,
                  QStringLiteral("④ 容器查询（窗口）：containerOf/Range/prev/next（与 Sequence 同口径）"));
            check(w->nodeInfo(1) && w->nodeInfo(1)->label == QStringLiteral("photos")
                      && w->nodeInfo(1)->parent == 0 && w->nodeInfo(1)->firstItem == 2
                      && w->nodeInfo(1)->itemCount == 3 && w->subtreeFirst(1) == 2
                      && w->subtreeCount(1) == 4,
                  QStringLiteral("④ 节点信息（覆盖内）：label/parent/firstItem/itemCount + subtree"));
            check(seq.tree().at(3).kind == Sequence::TreeNode::Kind::Archive
                      && seq.tree().at(3).firstItem == 6 && seq.tree().at(3).itemCount == 2,
                  QStringLiteral("④ 压缩包节点：kind = Archive（浏览里显示 archive；"
                                 "树的事实问 Sequence）"));
        }

        // 再进一层：sub（节点 2），然后**退回**根
        {
            const BrowseLayer L2 = browseLayerOf(seq, 2, 5);
            check(L2.rows.size() == 1 && L2.rows.at(0).name == QStringLiteral("s1.png")
                      && L2.rows.at(0).itemIndex == 5 && L2.crumbs.size() == 3
                      && L2.canGoUp && L2.rangeLabel == QStringLiteral("sub")
                      && L2.chain == QVector<int>({ 0, 1, 2 }),
                  QStringLiteral("④ 进 sub：1 行 + 3 段面包屑 + 祖先链 {0,1,2}"));

            const BrowseLayer back = browseLayerOf(seq, 0, 5);
            check(back.node == 0 && back.rows.size() == 4 && back.crumbs.size() == 1
                      && !back.canGoUp,
                  QStringLiteral("④ 退回根：行 / 面包屑 / canGoUp 复原"));
        }

        // 浏览层与条目覆盖**互相独立**：游标在最内层（6）、浏览层还在 photos ⇒ 行仍是整层
        {
            NavState nav;
            nav.cursor = 6;
            nav.screenMargin = 0;
            nav.prefetchHalf = 0;
            auto w = builder.build(seq, nav);
            const BrowseLayer L = browseLayerOf(seq, 1, 6);
            check(L.rows.size() == 4 && w->coversItem(6) && !w->coversItem(2),
                  QStringLiteral("④ 浏览层完整、条目覆盖仍然有界（互不影响）"));
            check(L.row == -1,
                  QStringLiteral("④ 游标(6)不在 photos 这一层 ⇒ 高亮行 = -1"));
        }

        // 高亮行语义**逐字不变**：先按 `itemIndex == cursor` 命中，再按节点子树范围命中
        {
            const BrowseLayer byNode = browseLayerOf(seq, -1, 6);   // 6 = book.cbz 里第一张
            check(byNode.row == 3 && byNode.rows.at(3).nodeIndex == 3,
                  QStringLiteral("④ 高亮按**子树范围**命中：游标 6 落在 book.cbz 那一行"));
        }

        // 陈旧 / 越界的层下标 ⇒ 与"没定层"同义（落到根）
        {
            const BrowseLayer L = browseLayerOf(seq, 999, 0);
            check(L.node == 0 && L.rows.size() == 4 && !L.canGoUp,
                  QStringLiteral("④ 越界的层下标 ⇒ 落到根（不夹取、不崩）"));
        }
    }

    // ══ ⑤ OSD 标量（含多页文件）══════════════════════════════════════════════
    {
        Sequence seq;
        fillTree(seq);
        const int cursors[] = { 0, 2, 4, 5, 6, 7 };
        bool allOk = true;
        for (int cursor : cursors) {
            NavState nav;
            nav.cursor = cursor;
            nav.mode = Sequence::Mode::Double;
            auto w = builder.build(seq, nav);
            const Osd e = expectedOsd(seq, cursor, nav.mode);
            if (w->totalPages() != e.totalPages || w->currentPage() != e.currentPage
                || w->relativePath() != e.relativePath || w->fileLabel() != e.fileLabel
                || w->sizeLabel() != e.sizeLabel || w->pageKindLabel() != e.pageKindLabel
                || w->folderLabel() != e.folderLabel || w->prevFolderLabel() != e.prevFolderLabel
                || w->nextFolderLabel() != e.nextFolderLabel)
                allOk = false;
        }
        check(allOk, QStringLiteral("⑤ 6 个游标 × 9 个 OSD 标量 = 直接用 Sequence 算的一致"));
        {
            NavState nav;
            nav.cursor = 6;                        // b1.png（包内成员）
            auto w = builder.build(seq, nav);
            check(w->relativePath() == QStringLiteral("book.cbz!/b1.png")
                      && w->pageKindLabel() == QStringLiteral("容器内页")
                      && w->folderLabel() == QStringLiteral("book.cbz")
                      && w->prevFolderLabel() == QStringLiteral("sub")
                      && w->nextFolderLabel().isEmpty(),
                  QStringLiteral("⑤ 包内条目：relativePath=book.cbz!/b1.png、上一夹=sub、下一夹=空"));
            check(w->totalPages() == seq.screenCount(Sequence::Mode::Single)
                      && w->screenCount(Sequence::Mode::Double)
                             == seq.screenCount(Sequence::Mode::Double)
                      && w->currentPage() == seq.screenOf(6, Sequence::Mode::Single),
                  QStringLiteral("⑤ 屏数/屏号：两种 mode 的表都在（当前 mode = 单幅）"));
        }
        {
            QTemporaryDir dir;
            const QString png = dir.filePath(QStringLiteral("x.png"));
            QImage page(8, 12, QImage::Format_RGB32);
            page.fill(Qt::blue);
            page.save(png, "PNG");
            Sequence mp;
            mp.buildFromMultiPageFile(png, 3);
            NavState nav;
            nav.cursor = 1;
            auto w = builder.build(mp, nav);
            check(w->count() == 3 && w->totalPages() == 3 && w->currentPage() == 1,
                  QStringLiteral("⑤ 多页文件：3 页 / 3 屏 / 当前第 1 屏"));
            check(w->fileLabel() == QStringLiteral("x.png [2/3]")
                      && w->pageKindLabel() == QStringLiteral("容器内页")
                      && w->sizeLabel().isEmpty() && w->relativePath() == png,
                  QStringLiteral("⑤ 多页文件：fileLabel=[2/3] / 容器内页 / 路径 = 文件本身"));
            NavState p0 = nav;
            p0.cursor = 0;
            auto w0 = builder.build(mp, p0);
            check(w0->pageKindLabel() == QStringLiteral("图片"),
                  QStringLiteral("⑤ 多页文件第 0 页 ⇒ 仍算「图片」（与今天同口径）"));
            const BrowseLayer L0 = browseLayerOf(mp, -1, 0);
            check(L0.node == -1 && L0.rows.size() == 3 && L0.chain.isEmpty()
                      && L0.rangeLabel == QStringLiteral("本次打开"),
                  QStringLiteral("⑤ 无树来源：整条序列当一层（3 行）/ 无祖先链 / rangeLabel=本次打开"));
        }
    }

    // ══ ⑥ 版本与等价（同一 NavState 重建两次）════════════════════════════════
    {
        Sequence seq;
        fillSynth(seq, 20, QSet<int>{ 6 });
        NavState nav;
        nav.cursor = 7;
        nav.mode = Sequence::Mode::Double;
        auto a = builder.build(seq, nav);
        auto b = builder.build(seq, nav);

        const BrowseLayer la = browseLayerOf(seq, -1, nav.cursor);
        const BrowseLayer lb = browseLayerOf(seq, -1, nav.cursor);
        bool same = a->count() == b->count() && a->itemsLo() == b->itemsLo()
                    && a->itemsHi() == b->itemsHi() && a->screenCount() == b->screenCount()
                    && la.node == lb.node && la.rows.size() == lb.rows.size()
                    && la.crumbs == lb.crumbs && la.row == lb.row
                    && a->totalPages() == b->totalPages()
                    && a->currentPage() == b->currentPage()
                    && a->relativePath() == b->relativePath() && a->fileLabel() == b->fileLabel()
                    && a->sizeLabel() == b->sizeLabel()
                    && a->pageKindLabel() == b->pageKindLabel()
                    && a->folderLabel() == b->folderLabel();
        for (int i = a->itemsLo(); same && i < a->itemsHi(); ++i) {
            const Sequence::Item *x = a->itemAt(i);
            const Sequence::Item *y = b->itemAt(i);
            if (!x || !y || x->cacheKey() != y->cacheKey() || x->loc.diskPath != y->loc.diskPath
                || x->size != y->size)
                same = false;
        }
        for (int k = a->screenOf(a->itemsLo()); same && k <= a->screenOf(a->itemsHi() - 1); ++k)
            if (a->screenFirst(k) != b->screenFirst(k) || a->screenItems(k) != b->screenItems(k))
                same = false;
        check(same, QStringLiteral("⑥ 同一 NavState 重建两次 ⇒ 内容等价"));
        check(b->version() > a->version() && a->version() > 0,
              QStringLiteral("⑥ 版本单调增（%1 → %2）").arg(a->version()).arg(b->version()));
        check(builder.version() == b->version(),
              QStringLiteral("⑥ builder 的版本计数 = 最后一次 build 的版本"));

        // 序列变了 ⇒ 窗口内容跟着变（不是缓存 / 陈旧）
        fillSynth(seq, 5);
        auto c = builder.build(seq, nav);
        check(c->count() == 5 && c->itemsHi() <= 5 && c->version() > b->version(),
              QStringLiteral("⑥ 序列换了 ⇒ 新窗口（count %1 / version %2）")
                  .arg(c->count()).arg(c->version()));
    }

    // ══ ⑦ 阴性对照：不够宽的窗口必须被判"不够" ═══════════════════════════════
    {
        Sequence seq;
        fillSynth(seq, 40, QSet<int>{ 12 });
        NavState nav;
        nav.cursor = 0;
        nav.mode = Sequence::Mode::Double;
        nav.screenMargin = 0;
        nav.prefetchHalf = 0;
        auto w = builder.build(seq, nav);

        check(w->itemsLo() == 0 && w->itemsHi() == 2 && w->windowItemCount() == 2,
              QStringLiteral("⑦ 余量 0：只覆盖当前屏 [0,2)"));
        check(w->coversScreen(0) && !w->coversScreen(1) && w->screenFirst(1) == -1,
              QStringLiteral("⑦ 远处屏 ⇒ coversScreen 假 / screenFirst -1（阴性对照）"));
        check(!w->coversGroupAt(6, Sequence::Mode::Double, Sequence::Dir::Next)
                  && w->groupAt(6, Sequence::Mode::Double, Sequence::Dir::Next).members.isEmpty()
                  && w->groupAt(6, Sequence::Mode::Double, Sequence::Dir::Next).cursor == -1,
              QStringLiteral("⑦ 覆盖外的 groupAt ⇒ 空组 + coversGroupAt 假"));
        check(w->itemAt(6) == nullptr && w->screenOf(6) == -1 && w->anchorOf(6) == -1,
              QStringLiteral("⑦ 覆盖外 ⇒ itemAt nullptr / screenOf / anchorOf = -1"));
        check(!w->coversGroupAt(38, Sequence::Mode::Double, Sequence::Dir::Prev),
              QStringLiteral("⑦ 覆盖外（向后）也算不够"));

        NavState far = nav;
        far.cursor = 30;
        far.screenMargin = 1;
        far.prefetchHalf = 1;
        auto wf = builder.build(seq, far);
        check(!wf->coversItem(0) && wf->coversItem(30) && wf->screenOf(0) == -1,
              QStringLiteral("⑦ 另一个窗口对别处同样判不够（不是「什么都能答」）"));

        // 标题字段：窗口的可显示边界 = 序列的 count（静态序列上 = 已发现数）
        check(w->count() == seq.count() && w->count() == seq.discoveredCount(),
              QStringLiteral("⑦ count = 序列的可显示边界（不是别处另算的数）"));

        // 缺失条目：屏划分照抄，组的**缺失跳过**也照抄（逐屏与 Sequence 比对）。
        //   ⚠️ 缺失里刻意放**一整屏都缺失**的一对（4、5 同屏且都缺）—— 这时 `groupAt` 的
        //   Next/Prev 会各自向不同方向跳，方向处理写错就会露馅。
        Sequence mseq;
        fillSynth(mseq, 8, QSet<int>{}, QSet<int>{ 1, 4, 5 });
        NavState mnav;
        mnav.cursor = 0;
        mnav.mode = Sequence::Mode::Double;
        auto mw = builder.build(mseq, mnav);
        bool grpSame = true;
        int dirDiffers = 0;
        for (int k = mw->screenOf(mw->itemsLo()); k <= mw->screenOf(mw->itemsHi() - 1); ++k) {
            const int s = mw->screenFirst(k);
            const Sequence::Group gn = mseq.groupAt(s, Sequence::Mode::Double, Sequence::Dir::Next);
            const Sequence::Group gp = mseq.groupAt(s, Sequence::Mode::Double, Sequence::Dir::Prev);
            if (!sameGroup(gn, gp)) ++dirDiffers;
            for (int d = 0; d < 2; ++d) {
                const Sequence::Dir dir = (d == 0) ? Sequence::Dir::Next : Sequence::Dir::Prev;
                if (!sameGroup(mw->groupAt(s, Sequence::Mode::Double, dir),
                               mseq.groupAt(s, Sequence::Mode::Double, dir)))
                    grpSame = false;
            }
        }
        check(grpSame, QStringLiteral("⑦ 缺失条目：groupAt 的跳过行为与 Sequence 逐屏一致"));
        check(dirDiffers > 0,
              QStringLiteral("⑦ 夹具里有 %1 屏「整屏缺失」⇒ Next/Prev 答案不同（变异测试能抓到的地方）")
                  .arg(dirDiffers));
        check(mw->screenItems(0).size() == 2
                  && !mw->groupAt(0, Sequence::Mode::Double, Sequence::Dir::Next).members.contains(1),
              QStringLiteral("⑦ 屏划分含 missing（2 条），组构成按 missing 过滤"));
    }

    // ══ ⑧ 容器查询逐条一致（**含空目录 / 同起点并列 / 无直接条目的中间目录**）══════
    //   为什么单列一节：builder 对 `containerOf/prevContainerFirst` 做了**按 `parentNode` 分组**
    //   的省调用优化（同一最内层节点的条目答案相同）—— 这里用一棵"刁钻"的树把
    //   四个容器查询**逐条目**与 `Sequence` 对照，证明优化没有改变答案。
    {
        //   节点：0=root 1=emptyA(空) 2=tie(无直接条目，只有子目录) 3=tieChild 4=d1 5=d2 6=emptyB(空)
        //   条目：0=t1 1=t2 2=d1a 3=d1b 4=r1 5=d2a 6=r2
        //   ⚠️ 2/3/0 三个节点的 subtreeFirst 都是 0（并列）；空目录（1 / 6）的 subtreeFirst 是 **-1**。
        Sequence tri;
        {
            auto row = [](int ord, int parentOrd, const QString &kind, const QString &label, int w,
                          int h) {
                Sequence::Row r;
                r.ord = ord;
                r.parentOrd = parentOrd;
                r.kind = kind;
                r.label = label;
                r.pageIndex = -1;
                r.width = w;
                r.height = h;
                return r;
            };
            QVector<Sequence::Row> rows;
            rows << row(0, -1, QStringLiteral("folder"), QStringLiteral("emptyA"), 0, 0);
            rows << row(1, -1, QStringLiteral("folder"), QStringLiteral("tie"), 0, 0);
            rows << row(2, 1, QStringLiteral("folder"), QStringLiteral("tieChild"), 0, 0);
            rows << row(3, 2, QStringLiteral("image"), QStringLiteral("t1.png"), 1200, 1800);
            rows << row(4, 2, QStringLiteral("image"), QStringLiteral("t2.png"), 1200, 1800);
            rows << row(5, -1, QStringLiteral("folder"), QStringLiteral("d1"), 0, 0);
            rows << row(6, 5, QStringLiteral("image"), QStringLiteral("d1a.png"), 1200, 1800);
            rows << row(7, 5, QStringLiteral("image"), QStringLiteral("d1b.png"), 1200, 1800);
            rows << row(8, -1, QStringLiteral("image"), QStringLiteral("r1.png"), 1200, 1800);
            rows << row(9, -1, QStringLiteral("folder"), QStringLiteral("d2"), 0, 0);
            rows << row(10, 9, QStringLiteral("image"), QStringLiteral("d2a.png"), 1200, 1800);
            rows << row(11, -1, QStringLiteral("image"), QStringLiteral("r2.png"), 1200, 1800);
            rows << row(12, -1, QStringLiteral("folder"), QStringLiteral("emptyB"), 0, 0);
            tri.buildFromRows(QStringLiteral("/synth/tricky"), rows, false, QStringLiteral("tricky"));
        }
        check(tri.count() == 7 && tri.tree().size() == 7,
              QStringLiteral("⑧ 刁钻树：7 条图 + 7 个节点（含 2 个空目录 + 1 个无直接条目的中间目录）"));

        NavState nav;
        nav.cursor = 0;
        nav.mode = Sequence::Mode::Double;
        auto w = builder.build(tri, nav);
        bool coOk = true, crOk = true, pvOk = true, nxOk = true;
        for (int i = w->itemsLo(); i < w->itemsHi(); ++i) {
            if (w->containerOf(i) != tri.containerOf(i)) coOk = false;
            if (w->containerRange(i) != tri.containerRange(i)) crOk = false;
            if (w->prevContainerFirst(i) != tri.prevContainerFirst(i)) pvOk = false;
            if (w->nextContainerFirst(i) != tri.nextContainerFirst(i)) nxOk = false;
        }
        check(coOk, QStringLiteral("⑧ containerOf 逐条 = Sequence（含空目录 / 并列起点）"));
        check(crOk, QStringLiteral("⑧ containerRange 逐条 = Sequence"));
        check(pvOk, QStringLiteral("⑧ prevContainerFirst 逐条 = Sequence（按 parentNode 分组优化后仍精确）"));
        check(nxOk, QStringLiteral("⑧ nextContainerFirst 逐条 = Sequence"));

        //   并列起点的具体形状：t1 的 parentNode 是 tieChild(3)，但 containerOf 取**并列里
        //   节点下标最小的**（root 0）—— 分组键（parentNode）与答案（containerOf）确实不同，
        //   这正是"分组必须仍然调 Sequence 取答案"的理由。
        check(tri.at(0).parentNode == 3 && tri.containerOf(0) == 0 && w->containerOf(0) == 0,
              QStringLiteral("⑧ 并列起点：parentNode(3) ≠ containerOf(0)，窗口答案 = Sequence 答案"));
        //   ★ T2d-1c-①：空目录（节点 1）**不在任何覆盖条目的容器链上** ⇒ 它根本不进窗口
        //     （窗口只装覆盖条目的容器链 + 根）。它的记录 / 元素判定条件问 `Sequence`；而
        //     "它不出现在任何枚举里"由 `browseLayerOf` 那一段（④/⑨）与 childrenOf 一起证。
        check(!w->coversNode(1) && tri.subtreeFirst(1) == -1 && tri.subtreeCount(1) == 0
                  && !tri.isElement(1),
              QStringLiteral("⑧ 空目录节点：不进窗口（不在容器链上）；记录 **−1** / count 0 / 不是元素"));
        check(w->nodeInfo(2) && w->nodeInfo(2)->itemCount == 0 && w->nodeInfo(2)->subtreeCount == 2
                  && w->subtreeFirst(2) == 0 && w->nodeInfo(2)->element,
              QStringLiteral("⑧ 无直接条目的中间目录：itemCount 0 但 subtreeCount 2（是元素）"));
        check(w->ancestorsOf(3) == QVector<int>({ 0, 2, 3 })
                  && w->childrenOf(2) == QVector<int>({ 3 })
                  && w->childrenOf(0) == QVector<int>({ 2, 4, 5 })
                  && w->childrenOf(1).isEmpty() && w->childrenOf(6).isEmpty(),
              QStringLiteral("⑧ 祖先链 / 子项按**树顺序**，且**空目录（1 / 6）不出现在子项里**"));

        //   同一棵树：把游标移到中段、缩小余量，覆盖 != 全序列也要逐条一致
        NavState mid;
        mid.cursor = 3;
        mid.screenMargin = 0;
        mid.prefetchHalf = 1;
        auto w2 = builder.build(tri, mid);
        bool coOk2 = true, nxOk2 = true;
        for (int i = w2->itemsLo(); i < w2->itemsHi(); ++i) {
            if (w2->containerOf(i) != tri.containerOf(i)) coOk2 = false;
            if (w2->nextContainerFirst(i) != tri.nextContainerFirst(i)) nxOk2 = false;
        }
        check(coOk2 && nxOk2 && w2->itemsLo() > 0 && w2->itemsHi() < tri.count(),
              QStringLiteral("⑧ 部分覆盖（[%1,%2)）下容器查询仍逐条一致")
                  .arg(w2->itemsLo()).arg(w2->itemsHi()));
    }

    // ══ ⑨ **元素可见性**（L40）：空容器 / 无图容器**不出现在任何枚举里** ═══════════
    //   形状（库快照来源 `buildFromRows` —— 与扫描走**同一份**收尾代码路径）：
    //     · 空目录（根级）；
    //     · **子目录全空 ⇒ 自己也不出现**的链（chain → inner，两层都是空的）；
    //     · 只有空子目录的目录（emptyParent → emptyChild）；
    //     · **无图压缩包**（noimg.cbz：一个 image 行都没有）；
    //     · 对照组：有图压缩包（withimg.cbz）、有图目录（dirWithImg）+ 根级两张图。
    //   口径：**「是不是元素」只有 `Sequence::isElement()` 一份实现**，枚举处只调它 ⇒
    //    **浏览行（`browseLayerOf`）/ `Window::childrenOf` / `Window::ancestorsOf`** 都必须过它。
    {
        Sequence seq;
        {
            auto row = [](int ord, int parentOrd, const QString &kind, const QString &label, int w,
                          int h) {
                Sequence::Row r;
                r.ord = ord;
                r.parentOrd = parentOrd;
                r.kind = kind;
                r.label = label;
                r.pageIndex = -1;
                r.width = w;
                r.height = h;
                return r;
            };
            QVector<Sequence::Row> rows;
            rows << row(0, -1, QStringLiteral("image"), QStringLiteral("r0.png"), 1200, 1800);
            rows << row(1, -1, QStringLiteral("image"), QStringLiteral("r1.png"), 1200, 1800);
            rows << row(2, -1, QStringLiteral("folder"), QStringLiteral("empty"), 0, 0);
            rows << row(3, -1, QStringLiteral("folder"), QStringLiteral("chain"), 0, 0);
            rows << row(4, 3, QStringLiteral("folder"), QStringLiteral("inner"), 0, 0);
            rows << row(5, -1, QStringLiteral("folder"), QStringLiteral("emptyParent"), 0, 0);
            rows << row(6, 5, QStringLiteral("folder"), QStringLiteral("emptyChild"), 0, 0);
            rows << row(7, -1, QStringLiteral("archive"), QStringLiteral("noimg.cbz"), 0, 0);
            rows << row(8, -1, QStringLiteral("archive"), QStringLiteral("withimg.cbz"), 0, 0);
            rows << row(9, 8, QStringLiteral("image"), QStringLiteral("w1.png"), 800, 1200);
            rows << row(10, -1, QStringLiteral("folder"), QStringLiteral("dirWithImg"), 0, 0);
            rows << row(11, 10, QStringLiteral("image"), QStringLiteral("d1.png"), 1200, 1800);
            rows << row(12, 10, QStringLiteral("image"), QStringLiteral("d2.png"), 1200, 1800);
            seq.buildFromRows(QStringLiteral("/synth/vis"), rows, false, QStringLiteral("vis"));
        }
        //   树节点：0=root 1=empty 2=chain 3=inner 4=emptyParent 5=emptyChild
        //          6=noimg.cbz 7=withimg.cbz 8=dirWithImg；条目 0=r0 1=r1 2=w1 3=d1 4=d2
        check(seq.tree().size() == 9 && seq.count() == 5,
              QStringLiteral("⑨ 夹具：9 个节点（**一个都没被物理删除**）/ 5 条图"));
        bool elemOk = true;
        for (int i = 0; i < 9; ++i)
            if (seq.isElement(i) != (i == 0 || i == 7 || i == 8)) elemOk = false;
        check(elemOk && seq.isElement(0) && seq.isElement(7) && seq.isElement(8),
              QStringLiteral("⑨ isElement：只有 root(0) / withimg.cbz(7) / dirWithImg(8) 是元素"));
        check(!seq.isElement(1) && !seq.isElement(2) && !seq.isElement(3)
                  && !seq.isElement(4) && !seq.isElement(5) && !seq.isElement(6),
              QStringLiteral("⑨ 空目录 / 子目录全空的链 / 无图压缩包：**全都不是元素**"));
        check(seq.subtreeFirst(6) == -1 && seq.subtreeCount(6) == 0
                  && seq.subtreeFirst(3) == -1,
              QStringLiteral("⑨ 无图容器 / 空子目录的记录 = −1 / 0（不污染祖先）"));

        NavState nav;
        nav.cursor = 0;
        nav.mode = Sequence::Mode::Single;
        auto w = builder.build(seq, nav);

        // ① 浏览行（**从序列取**）：这一层只有 2 张图 + 2 个**有图**容器
        const QVector<BrowseModel::Row> &rows = browseLayerOf(seq, -1, 0).rows;
        bool namesOk = rows.size() == 4;
        const QStringList want{ QStringLiteral("r0.png"), QStringLiteral("r1.png"),
                                QStringLiteral("withimg.cbz"), QStringLiteral("dirWithImg") };
        for (int i = 0; i < rows.size() && namesOk; ++i)
            if (rows.at(i).name != want.at(i)) namesOk = false;
        check(namesOk, QStringLiteral("⑨ 根层浏览行 = 2 图 + 2 个有图容器（实得 %1 行：%2）")
                           .arg(rows.size())
                           .arg(rows.isEmpty() ? QString() : rows.at(0).name));
        bool noInvisibleRow = true;
        for (const BrowseModel::Row &r : rows)
            if (r.nodeIndex >= 0 && !seq.isElement(r.nodeIndex)) noInvisibleRow = false;
        check(noInvisibleRow, QStringLiteral("⑨ 浏览行里**每一行节点都是元素**（空容器一个都没有）"));

        // ② 子节点：空目录 / 空链 / 无图压缩包都不在 childrenOf 里
        check(w->childrenOf(0) == QVector<int>({ 7, 8 }),
              QStringLiteral("⑨ childrenOf(root) = {withimg.cbz, dirWithImg}（实得 %1 个）")
                  .arg(w->childrenOf(0).size()));
        check(w->childrenOf(2).isEmpty() && w->childrenOf(4).isEmpty(),
              QStringLiteral("⑨ 「子目录全空 ⇒ 自己也不出现」：chain / emptyParent 的子项为空"));
        //   空目录节点本身**还在树里**（没被物理删除 ⇒ 标号不位移），但枚举拿不到它
        //   ★ T2d-1c-①：它留在树里（标号不位移），但**任何枚举都拿不到它** ——
        //     `childrenOf` 里没有它（L40 的 element 判定条件），`browseLayerOf` 的行里也没有它
        //     （同一份判定条件）；窗口**只装覆盖条目的容器链** ⇒ 它也不在窗口的表里。
        check(seq.tree().at(1).label == QStringLiteral("empty")
                  && !w->childrenOf(0).contains(1) && !w->coversNode(1)
                  && browseLayerOf(seq, -1, 0).rows.size() == 4,
              QStringLiteral("⑨ 节点留在结构里（标号不位移）但对外枚举不到（childrenOf / 浏览行 / 窗口的表）"));

        // ③ 祖先链 = **祖先链**（新口径，设计 12 §8.7.1）：**不过「元素」判定条件** ——
        //    空目录 / 子目录全空的链 / 无图压缩包**照样出现在链里**（它只是"层"，不是元素）。
        //    为什么改：`childrenOf` / browse 行枚举的是**元素**（必须过滤空容器），但"根 → node
        //    的祖先链"是导航/扫完的**结构** —— 扫描期间根自己还没扫完（不是元素），若也过滤，
        //    链就断了头。旧断言 `ancestorsOf(1) == {0}` 是按旧口径写的 ⇒ 这里**按新口径改对**，
        //    不是放宽：`childrenOf` 的过滤断言（上面 ②）一条没动。
        //   ★ T2d-1c-①：窗口里的节点表 = 覆盖条目的容器链（+根）⇒ 只有链上的节点有祖先链
        //     （8 = dirWithImg 在链上；1 / 2 / 6 是"空容器"，本来就不在任何覆盖条目的链上）。
        //     "祖先链不过元素判定条件"这条口径现在由 `browseLayerOf(...).chain` 验（见 ④ 与 viewseq ⑰）。
        check(w->ancestorsOf(8) == QVector<int>({ 0, 8 })
                  && w->ancestorsOf(7) == QVector<int>({ 0, 7 })
                  && w->ancestorsOf(1).isEmpty() && w->ancestorsOf(2).isEmpty()
                  && w->ancestorsOf(6).isEmpty()
                  && w->childrenOf(0) == QVector<int>({ 7, 8 }),
              QStringLiteral("⑨ 祖先链（链上的节点 = {根 → 自己}）；空容器不在窗口里 ⇒ 拿不到"
                             "，而 childrenOf 仍**只含元素**"));
        //   边界：**不在窗口**的节点 ⇒ 仍然空（不夹取、不扩窗）。节点 3 = inner 在树里，但它
        //   既不是被覆盖条目的祖先、也不是 browse 当前层的直接子节点 ⇒ 窗口没装它。
        check(!w->coversNode(3) && w->ancestorsOf(3).isEmpty() && w->ancestorsOf(999).isEmpty(),
              QStringLiteral("⑨ 不在窗口的节点（inner=3 / 越界 999）⇒ ancestorsOf 空"));

        // ④ 容器查询不受影响：整条快照里 `containerOf` 只会命中**有图的容器**（= 元素）
        bool contOk = true;
        for (int i = 0; i < seq.count(); ++i) {
            const int node = seq.containerOf(i);
            if (node >= 0 && !seq.isElement(node)) contOk = false;
        }
        check(contOk && seq.containerRange(2) == QPair<int, int>(2, 1)
                  && seq.containerRange(3) == QPair<int, int>(3, 2),
              QStringLiteral("⑨ 容器查询只落在元素上（w1 = [2,+1) / d1 = [3,+2)）"));
    }

    // ══ ⑩ 按屏 OSD（**覆盖范围内每一屏的起点**各一份）══════════════════════════
    //   参照实现 = 上面的 `expectedOsd`（**直接从 `Sequence` 按 `ViewerModel::buildScreenInfo`
    //   的语义**算九个字段 —— 与 `src/viewermodel.cpp` 的那份实现同一口径，但这里是**独立一份**，
    //   被测的是窗口的 `screenOsd(k)`）。形状：单幅/双幅（宽图改变配对）、文件夹与压缩包混排
    //   的树、多页容器（无树）、无树来源（整条序列当一层）、游标在首屏/末屏/中间，
    //   以及**覆盖不足**的边界（问覆盖外的屏 ⇒ 必须如实返回"没有"）。
    {
        int screens = 0, fields = 0;
        QString firstBad;
        auto osdDiff = [](const Window::ScreenOsd &o, const Osd &e) {
            QStringList d;
            if (o.totalPages != e.totalPages) d << QStringLiteral("totalPages");
            if (o.currentPage != e.currentPage) d << QStringLiteral("currentPage");
            if (o.relativePath != e.relativePath) d << QStringLiteral("relativePath");
            if (o.fileLabel != e.fileLabel) d << QStringLiteral("fileLabel");
            if (o.sizeLabel != e.sizeLabel) d << QStringLiteral("sizeLabel");
            if (o.pageKindLabel != e.pageKindLabel) d << QStringLiteral("pageKindLabel");
            if (o.folderLabel != e.folderLabel) d << QStringLiteral("folderLabel");
            if (o.prevFolderLabel != e.prevFolderLabel) d << QStringLiteral("prevFolderLabel");
            if (o.nextFolderLabel != e.nextFolderLabel) d << QStringLiteral("nextFolderLabel");
            return d.join(QLatin1Char(','));
        };
        //   一个窗口：覆盖内**每一屏的起点**逐屏×9 字段比对
        auto checkWindow = [&](const Sequence &seq, const NavState &nav, const QString &tag) {
            const std::shared_ptr<const Window> w = builder.build(seq, nav);
            const Sequence::Mode mode = nav.mode;
            const QVector<int> starts = expectedStarts(seq, mode, w->itemsLo(), w->itemsHi());
            const int before = screens;
            const int loK = seq.screenOf(w->itemsLo(), mode);
            for (int k = loK; k < loK + starts.size(); ++k) {
                const Window::ScreenOsd *o = w->screenOsd(k);
                if (!o) {
                    if (firstBad.isEmpty())
                        firstBad = QStringLiteral("%1：屏 %2 在覆盖内却返回 nullptr").arg(tag).arg(k);
                    continue;
                }
                ++screens;
                fields += 9;
                const Osd e = expectedOsd(seq, starts.at(k - loK), mode);
                const QString diff = osdDiff(*o, e);
                if (!diff.isEmpty() && firstBad.isEmpty())
                    firstBad = QStringLiteral("%1：屏 %2（起点 %3）字段 %4 不一致")
                                   .arg(tag).arg(k).arg(starts.at(k - loK)).arg(diff);
            }
            check(screens > before || starts.isEmpty(),
                  QStringLiteral("⑩ %1：覆盖内每一屏都查到了按屏 OSD（%2 屏）")
                      .arg(tag).arg(starts.size()));
        };
        //   ★ **内部一致性**（任务要求的那条）：游标**落在屏起点**时，`screenOsd(currentPage)`
        //   的九个字段必须 == 窗口原有的九个标量（一份实现、两处调用）。对**整条序列的每一屏
        //   起点**各建一个窗口验一遍。
        //   ⚠️ 游标落在**屏中**（例如双幅的第二张）时两者**本就不该相等**：老九个标量是
        //      `ViewerModel::buildScreenInfo(cursor)` 的口径 = **那个条目**的字段；按屏 OSD 是
        //      **该屏起点**的字段。这条边界在下面单独验（见"屏中游标"）。
        auto navEqualsWindow = [](const std::shared_ptr<const Window> &w) {
            const Window::ScreenOsd *o = w->screenOsd(w->currentPage());
            return o != nullptr && o->totalPages == w->totalPages()
                   && o->currentPage == w->currentPage() && o->relativePath == w->relativePath()
                   && o->fileLabel == w->fileLabel() && o->sizeLabel == w->sizeLabel()
                   && o->pageKindLabel == w->pageKindLabel() && o->folderLabel == w->folderLabel()
                   && o->prevFolderLabel == w->prevFolderLabel()
                   && o->nextFolderLabel == w->nextFolderLabel();
        };
        auto checkNavConsistent = [&](const Sequence &seq, Sequence::Mode mode, const QString &tag) {
            const QVector<int> all = expectedStarts(seq, mode, 0, seq.count());
            bool ok = !all.isEmpty();
            for (int s : all) {                       // s = 屏起点
                NavState nav;
                nav.cursor = s;
                nav.mode = mode;
                auto w = builder.build(seq, nav);
                if (!navEqualsWindow(w)) ok = false;
            }
            check(ok, QStringLiteral("⑩ %1：每一屏起点作游标 ⇒ nav 九个标量 == 该屏按屏 OSD（%2 屏）")
                          .arg(tag).arg(all.size()));
        };

        {   // 单幅 / 双幅：24 条（3 张宽图单独成屏）+ 游标首/中/尾
            Sequence seq;
            fillSynth(seq, 24, QSet<int>{ 4, 9, 17 });
            for (int mi = 0; mi < 2; ++mi) {
                const Sequence::Mode mode = (mi == 0) ? Sequence::Mode::Single
                                                      : Sequence::Mode::Double;
                const QString tag = (mi == 0) ? QStringLiteral("合成单幅") : QStringLiteral("合成双幅");
                const int cursors[] = { 0, 10, 23 };
                for (int cursor : cursors) {
                    NavState nav;
                    nav.cursor = cursor;
                    nav.mode = mode;
                    checkWindow(seq, nav, QStringLiteral("%1 cursor=%2").arg(tag).arg(cursor));
                }
                checkNavConsistent(seq, mode, tag);
            }
        }
        {   // 文件夹 + 压缩包混排的树（8 条：根级图 / photos→sub / book.cbz）：首/中/尾
            Sequence seq;
            fillTree(seq);
            const int cursors[] = { 0, 3, 7 };
            for (int cursor : cursors) {
                NavState nav;
                nav.cursor = cursor;
                nav.mode = Sequence::Mode::Double;
                checkWindow(seq, nav, QStringLiteral("树 cursor=%1").arg(cursor));
            }
            checkNavConsistent(seq, Sequence::Mode::Double, QStringLiteral("树双幅"));
            checkNavConsistent(seq, Sequence::Mode::Single, QStringLiteral("树单幅"));

            //   ★ 边界（文档 + 阴性对照）：游标落在**屏中**时，老九个标量 = **游标那个条目**的
            //   字段，按屏 OSD = **屏起点**的字段 ⇒ 两者不同；而按屏 OSD 仍等于起点的参照实现。
            const int mid = 3;                        // p2.png：双幅下与 p1 同屏（起点 2）
            const int start = seq.anchorOf(mid, Sequence::Mode::Double);
            NavState nav;
            nav.cursor = mid;
            nav.mode = Sequence::Mode::Double;
            auto w = builder.build(seq, nav);
            const Window::ScreenOsd *o = w->screenOsd(w->currentPage());
            const Osd e = expectedOsd(seq, start, Sequence::Mode::Double);
            check(start != mid && o != nullptr && o->fileLabel != w->fileLabel()
                      && osdDiff(*o, e).isEmpty()
                      && w->fileLabel() == expectedOsd(seq, mid, nav.mode).fileLabel,
                  QStringLiteral("⑩ 屏中游标(%1)：nav 标量 = 游标条目的(`%2`) ≠ 按屏 OSD(`%3`)；"
                                 "按屏 OSD 仍 = 屏起点 %4 的参照")
                      .arg(mid).arg(w->fileLabel()).arg(o ? o->fileLabel : QString()).arg(start));
        }
        {   // 多页容器（无树来源：整条序列当一层）
            QTemporaryDir dir;
            const QString png = dir.filePath(QStringLiteral("q.png"));
            QImage page(8, 12, QImage::Format_RGB32);
            page.fill(Qt::green);
            page.save(png, "PNG");
            Sequence mp;
            mp.buildFromMultiPageFile(png, 4);
            NavState nav;
            nav.cursor = 1;
            checkWindow(mp, nav, QStringLiteral("多页容器 cursor=1（中）"));
            nav.cursor = 3;
            checkWindow(mp, nav, QStringLiteral("多页容器 cursor=3（末屏）"));
            checkNavConsistent(mp, Sequence::Mode::Single, QStringLiteral("多页容器"));
        }
        {   // **覆盖不足**的边界：覆盖外的屏号必须如实返回"没有"
            Sequence seq;
            fillSynth(seq, 2000, QSet<int>{ 1000 });
            NavState nav;
            nav.cursor = 1000;
            nav.mode = Sequence::Mode::Double;
            nav.screenMargin = 2;
            nav.prefetchHalf = 3;
            auto w = builder.build(seq, nav);
            const int kIn = w->screenOf(1000);
            const int kOutHi = w->screenOf(w->itemsHi() - 1) + 20;
            const int kOutLo = w->screenOf(w->itemsLo()) - 20;
            check(kIn >= 0 && w->coversScreen(kIn) && w->screenOsd(kIn) != nullptr,
                  QStringLiteral("⑩ 覆盖内（第 %1 屏）⇒ 有按屏 OSD").arg(kIn));
            check(w->screenOsd(kOutHi) == nullptr && w->screenOsd(kOutLo) == nullptr
                      && w->screenOsd(-1) == nullptr && w->screenOsd(999999) == nullptr,
                  QStringLiteral("⑩ **覆盖外 ⇒ nullptr**（上界 %1 / 下界 %2 / -1 / 极远）")
                      .arg(kOutHi).arg(kOutLo));
            const Window::ScreenOsd *o = w->screenOsd(kIn);
            const Osd e = expectedOsd(seq, seq.anchorOf(1000, nav.mode), nav.mode);
            check(o && osdDiff(*o, e).isEmpty(),
                  QStringLiteral("⑩ 覆盖内的那一屏也逐字段 = 直接算的（不因覆盖窄而失真）"));
        }

        check(firstBad.isEmpty(),
              QStringLiteral("⑩ 按屏 OSD 逐屏×9 字段 = 直接算的（%1 屏 / %2 字段）%3")
                  .arg(screens).arg(fields).arg(firstBad));
        check(screens >= 60 && fields == screens * 9,
              QStringLiteral("⑩ 覆盖形状足够（%1 屏 × 9 = %2 字段）").arg(screens).arg(fields));
    }

    // ══ ⑪ 回绕目标 `loopTarget`（三种 loopMode × 前/后 × 头/中/尾）══════════════
    //   参照 = 直接用 `Sequence` 按 `ViewerModel::loopTarget(bool)` 的语义算（那份是事实源）。
    {
        auto expectedLoop = [](const Sequence &seq, int loopMode, int cursorRaw, bool fwd) -> int {
            if (loopMode == 0) return -1;
            const int n = seq.count();
            if (n <= 0) return -1;
            const int cur = qBound(0, cursorRaw, n - 1);
            if (loopMode == 1) return fwd ? 0 : qMax(0, n - 1);
            const QPair<int, int> r = seq.containerRange(cur);
            if (r.second <= 0) return -1;
            return fwd ? r.first : qMax(r.first, r.first + r.second - 1);
        };
        int cases = 0;
        QString bad;
        auto cmpLoop = [&](const Sequence &seq, int loopMode, int cursor, const QString &tag) {
            NavState nav;
            nav.cursor = cursor;
            nav.mode = Sequence::Mode::Double;
            nav.loopMode = loopMode;
            auto w = builder.build(seq, nav);
            for (int f = 0; f < 2; ++f) {
                const bool fwd = (f == 0);
                const int got = w->loopTarget(fwd);
                const int exp = expectedLoop(seq, loopMode, cursor, fwd);
                ++cases;
                if (got != exp && bad.isEmpty())
                    bad = QStringLiteral("%1（loopMode=%2 cursor=%3 %4）：得 %5 期望 %6")
                              .arg(tag).arg(loopMode).arg(cursor)
                              .arg(fwd ? QStringLiteral("前") : QStringLiteral("后"))
                              .arg(got).arg(exp);
            }
            if (w->loopMode() != loopMode && bad.isEmpty())
                bad = QStringLiteral("%1：loopMode 没原样带走").arg(tag);
        };

        {   // 有树（文件夹 + 压缩包混排）：头 / 中（photos 内）/ 尾（book.cbz 内）
            Sequence seq;
            fillTree(seq);
            const int n = seq.count();
            for (int lm = 0; lm <= 2; ++lm)
                for (int cursor : { 0, 3, n - 1 })
                    cmpLoop(seq, lm, cursor, QStringLiteral("树"));

            // 具体语义（可读的期望值，不只依赖上面的通用比对）
            NavState nav;
            nav.cursor = 3;                       // p1.png ∈ photos（子树 [2, +4)）
            nav.mode = Sequence::Mode::Double;
            nav.loopMode = 2;
            auto w = builder.build(seq, nav);
            check(w->loopTarget(true) == 2 && w->loopTarget(false) == 5,
                  QStringLiteral("⑪ 文件夹循环 cursor=3(photos) ⇒ 前 %1（期望 2）/ 后 %2（期望 5）")
                      .arg(w->loopTarget(true)).arg(w->loopTarget(false)));
            NavState nb = nav;
            nb.cursor = 7;                        // b2.png ∈ book.cbz（子树 [6, +2)）
            auto wb = builder.build(seq, nb);
            check(wb->loopTarget(true) == 6 && wb->loopTarget(false) == 7,
                  QStringLiteral("⑪ 文件夹循环 cursor=7(book.cbz) ⇒ 前 %1（期望 6）/ 后 %2（期望 7）")
                      .arg(wb->loopTarget(true)).arg(wb->loopTarget(false)));
            NavState na = nav;
            na.loopMode = 1;
            auto wa = builder.build(seq, na);
            check(wa->loopTarget(true) == 0 && wa->loopTarget(false) == n - 1,
                  QStringLiteral("⑪ 全部循环 cursor=3 ⇒ 0 / %1").arg(n - 1));
        }
        {   // 无树来源（`containerRange` = 整条序列 [0, n)）
            Sequence seq;
            fillSynth(seq, 40, QSet<int>{ 12 });
            const int n = seq.count();
            for (int lm = 0; lm <= 2; ++lm)
                for (int cursor : { 0, n / 2, n - 1 })
                    cmpLoop(seq, lm, cursor, QStringLiteral("无树"));
        }
        {   // 空序列 ⇒ 一律 -1
            Sequence empty;
            NavState nav;
            nav.loopMode = 2;
            auto w = builder.build(empty, nav);
            check(w->loopTarget(true) == -1 && w->loopTarget(false) == -1,
                  QStringLiteral("⑪ 空序列 ⇒ loopTarget 前/后都是 -1"));
        }
        check(bad.isEmpty() && cases >= 36,
              QStringLiteral("⑪ loopTarget 逐条一致（%1 次比对：3 种 loopMode × 前/后 × 头/中/尾）%2")
                  .arg(cases).arg(bad));
    }

    // ══ ⑫ 扫描中间态：`ancestorsOf` 的链**包含根**（未扫完也不过"元素"判定条件）════════
    //   为什么单列：静态夹具（⑨）验的是"空容器在链里"；这里验的是**扫描进行中**——根自己
    //   那一刻还没扫完（不是元素），旧口径会把链断头。
    {
        QTemporaryDir dir;
        check(dir.isValid(), QStringLiteral("⑫ 临时目录建好"));
        const QString root = dir.path();
        QDir(root).mkpath(QStringLiteral("sub"));
        int made = 0;
        for (int i = 0; i < 120; ++i) {
            QImage img(8, 12, QImage::Format_RGB32);
            img.fill(Qt::red);
            if (img.save(root + QStringLiteral("/sub/p%1.png").arg(i), "PNG")) ++made;
        }
        for (int i = 0; i < 8; ++i) {
            QImage img(8, 12, QImage::Format_RGB32);
            img.fill(Qt::blue);
            if (img.save(root + QStringLiteral("/r%1.png").arg(i), "PNG")) ++made;
        }
        check(made == 128, QStringLiteral("⑫ 造了 %1 张真实 PNG（子目录 120 + 根 8）").arg(made));

        Sequence seq;
        NavState nav;
        nav.cursor = 0;
        seq.startScan(root, 5, false);
        bool caught = false;
        int rootIdx = -1, probe = -1;
        QVector<int> chain;
        bool probeIsElement = true;
        for (int spin = 0; spin < 200000 && !seq.scanFinished(); ++spin) {
            seq.drain();
            if (seq.scanFinished()) break;
            if (seq.count() > 0 && !seq.tree().isEmpty()) {
                auto w = builder.build(seq, nav);
                rootIdx = w->rootNode();
                if (rootIdx >= 0 && w->coversNode(rootIdx)) {
                    // 挑一个**在窗口里**的节点探测，优先挑"还不是元素"的那个（新口径的关键）
                    probe = rootIdx;
                    for (int x = 0; x < seq.tree().size(); ++x)
                        if (w->coversNode(x) && !seq.isElement(x)) { probe = x; break; }
                    chain = w->ancestorsOf(probe);
                    probeIsElement = seq.isElement(probe);
                    caught = true;
                    break;
                }
            }
            QThread::msleep(1);
        }
        check(caught && !chain.isEmpty() && chain.first() == rootIdx && chain.last() == probe
                  && chain.contains(rootIdx) && !probeIsElement,
              QStringLiteral("⑫ 扫描中间态（未收尾）：链包含根、**非元素层不被过滤**"
                             "（根 %1 / 探测 %2 / 链长 %3）")
                  .arg(rootIdx).arg(probe).arg(chain.size()));
    }

    return check.finish();
}

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, &runSeqWindowSelfTest);
}
