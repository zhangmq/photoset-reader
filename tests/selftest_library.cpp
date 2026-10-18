#include "testsupport.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QSize>
#include <QString>
#include <QTemporaryDir>
#include <QVector>

#include "imageprovider.h"
#include "librarystore.h"

// ══ 图集库存储自检（ib-test-library；原先由 `image-browser --selftest-library` 跑）══
//   用**临时库**（PHOTOSET_LIBRARY_DB），不碰用户的真库。
//   覆盖：建表 / 集合增删改 / 条目**原子替换**（`replaceItems`：空集 / 子集 / 超集 /
//         分块边界 / 失败回滚 / 字段往返 / 跨集合隔离）/
//         data_version 语义（自己写不变）
static int runLibrarySelfTest()
{
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        qWarning() << "[libselftest] 无法建临时目录";
        return 1;
    }
    const QString dbPath = QStringLiteral("/tmp/photoset-libselftest.db");
    QFile::remove(dbPath);
    qputenv("PHOTOSET_LIBRARY_DB", dbPath.toLocal8Bit());

    library::Store store;
    QString err;
    testsupport::Check check(QStringLiteral("libselftest"));

    check(store.open(&err), QStringLiteral("打开库（建表）"));

    // data_version：自己写**不变**（文档 §二 的核心语义）
    const int v0 = store.dataVersion();
    const qint64 cid = store.addCollection(tmp.path() + "/album", Sequence::Kind::Folder);
    check(cid > 0, QStringLiteral("加入集合"));
    check(store.addCollection(tmp.path() + "/album", Sequence::Kind::Folder) == cid,
          QStringLiteral("重复加入同一目录 → 返回既有 id"));
    check(store.dataVersion() == v0, QStringLiteral("自己写库 → data_version 不变"));

    check(store.renameCollection(cid, QStringLiteral("改名后")), QStringLiteral("重命名"));
    check(store.collection(cid).title == QStringLiteral("改名后"), QStringLiteral("重命名生效"));
    // ★ v6：**没有 revision 了** —— 跨进程同步只有 `data_version` 一条通道
    const int dv0 = store.dataVersion();
    // ★ v5：resume = **label 链的 JSON 数组** + 页号（不再拼串）
    const QString resumeJson = Sequence::encodeChain({ QStringLiteral("edge"), QStringLiteral("p1.png") });
    check(store.setResume(cid, resumeJson, 0), QStringLiteral("写进度（label 链 JSON + 页号）"));
    check(store.collection(cid).resumeKey == resumeJson && store.collection(cid).resumePage == 0,
          QStringLiteral("进度往返（链 + 页号）"));
    check(store.touchBrowsed(cid), QStringLiteral("写浏览时间（参与排序）"));
    check(store.dataVersion() == dv0, QStringLiteral("自己写库：data_version **不变**（同步只认别人的提交）"));
    // 条目（★ v5：库 = 一棵树 + 条目；`ord` = 遍历序号，节点与叶子共用；
    //   **没有 key / rel_path** —— 身份 = 沿 parent_ord 上溯的 label 序列）
    //   ★ v8：**唯一写入路径 = `Store::replaceItems`（一个事务原子替换）**；
    //     `upsertItems` / `clearItems` 已随旧"扫描任务"路径删除。
    auto mkItem = [](int ord, int parentOrd, Sequence::Kind kind, const QString &label,
                     int page = -1) {
        library::Item it;
        it.ord = ord;
        it.parentOrd = parentOrd;
        it.kind = kind;
        it.label = label;
        it.pageIndex = page;
        return it;
    };
    QVector<library::Item> items;
    items << mkItem(0, -1, Sequence::Kind::Folder, QStringLiteral("edge"));
    for (int i = 0; i < 3; ++i)                           // 节点与叶子共用同一个序
        items << mkItem(i + 1, 0, Sequence::Kind::Image, QStringLiteral("p%1.png").arg(i));

    // ── ① 原子替换（空集 → 4 行）：唯一的写入路径 ──
    check(store.replaceItems(cid, items), QStringLiteral("① 替换条目：空集 → 4 行（唯一写入路径）"));
    check(store.items(cid).size() == 4, QStringLiteral("① 4 行 = 1 目录节点 + 3 图"));
    // ★ v3：像素尺寸写入磁盘（**文件事实**；分组是派生值 ⇒ 不写入磁盘）
    items[1].width = 1200; items[1].height = 1800;      // 窄（漫画页）—— items[0] 是目录行
    items[2].width = 2400; items[2].height = 1600;      // 宽（跨页）
    check(store.replaceItems(cid, items), QStringLiteral("① 替换条目：写回像素尺寸"));
    {
        const QVector<library::Item> sized = store.items(cid);
        //   行 0 = 目录节点；行 1..3 = p0/p1/p2
        check(sized.size() == 4 && sized[1].width == 1200 && sized[1].height == 1800
                  && sized[2].width == 2400 && sized[2].height == 1600 && sized[3].width == 0,
              QStringLiteral("① 像素宽/高往返（未知 = 0）"));
    }
    check(store.collection(cid).readingLayout == app::Layout::Single,
          QStringLiteral("① 新图集默认阅读版面 = 单幅"));
    check(store.setReadingLayout(cid, app::Layout::Double), QStringLiteral("① 写阅读版面"));
    check(store.collection(cid).readingLayout == app::Layout::Double,
          QStringLiteral("① 阅读版面写入磁盘（图集级唯一设置）"));
    // ★ v6：**没有 item_count 列** —— "多少张图"用 `GROUP BY` 现算
    check(store.itemCount(cid) == 3, QStringLiteral("① itemCount() = **可读序列** 3 行（不含容器节点）"));
    check(store.itemCounts().value(cid, -1) == 3, QStringLiteral("① itemCounts() 一条 GROUP BY 覆盖所有集合"));
    // ★ v5：**按链 + 页查**（沿 label 链 walk 树 + 页号精确命中）→ 可读序列里的序号
    check(store.itemIndexOfChain(cid, { QStringLiteral("edge"), QStringLiteral("p1.png") }, 0) == 1,
          QStringLiteral("① label 链 → 序号（进度用；只数 image 行）"));
    check(store.itemIndexOfChain(cid, { QStringLiteral("edge"), QStringLiteral("nope.png") }, 0) == -1,
          QStringLiteral("① 链里有一层对不上 ⇒ -1"));

    // ── ② 同一份再替换：不累积（**不是** upsert 合并）──
    check(store.replaceItems(cid, items), QStringLiteral("② 同一份再替换（幂等）"));
    check(store.items(cid).size() == 4, QStringLiteral("② 行数不变（不产生重复行）"));

    // ── ③ 边界：替换成**空集** ⇒ 旧行一条不剩（再替换回来）──
    check(store.replaceItems(cid, {}), QStringLiteral("③ 替换成**空集**（边界）"));
    check(store.items(cid).isEmpty() && store.itemCount(cid) == 0,
          QStringLiteral("③ 空集替换后 0 行（旧内容全部清掉）"));
    check(store.replaceItems(cid, items) && store.items(cid).size() == 4,
          QStringLiteral("③ 再替换回 4 行（空集不是终态）"));

    // ── ④ 子集 / 超集 ──
    check(store.replaceItems(cid, items.mid(0, 3)) && store.items(cid).size() == 3,
          QStringLiteral("④ 子集替换：4 行 → 3 行（旧行不残留）"));
    check(store.replaceItems(cid, items) && store.items(cid).size() == 4,
          QStringLiteral("④ 超集替换：3 行 → 4 行"));

    // ── ⑤ 字段往返：archive 容器 / 多页页行 / 尺寸 / 时间 ──
    {
        QVector<library::Item> multi;
        multi << mkItem(0, -1, Sequence::Kind::Archive, QStringLiteral("vol01.cbz"));
        multi[0].size = 12345;
        multi[0].mtime = 67890;
        for (int p = 0; p < 2; ++p) {
            //   页行 = **label 空串**（这里刻意用 **null `QString()`**，验 NOT NULL 列的规范化）
            multi << mkItem(p + 1, 0, Sequence::Kind::Image, QString(), p);
            multi.last().size = 100 + p;
            multi.last().mtime = 200 + p;
            multi.last().width = 800;
            multi.last().height = 1200;
        }
        check(store.replaceItems(cid, multi), QStringLiteral("⑤ 替换成多页容器 3 行"));
        const QVector<library::Item> got = store.items(cid);
        check(got.size() == 3 && got[0].kind == Sequence::Kind::Archive
                  && got[0].label == QLatin1String("vol01.cbz") && got[0].size == 12345
                  && got[0].mtime == 67890,
              QStringLiteral("⑤ 容器行字段往返（kind/label/size/mtime）"));
        check(got[1].pageIndex == 0 && got[2].pageIndex == 1
                  && got[1].label.isEmpty() && got[2].label.isEmpty(),
              QStringLiteral("⑤ 页行往返：label = 空串（**null 被规范成空串**）、page_index = 页号"));
        check(got[1].width == 800 && got[1].height == 1200 && got[1].size == 100
                  && got[1].mtime == 200,
              QStringLiteral("⑤ 页行尺寸 / 时间往返"));
        check(store.itemCount(cid) == 2,
              QStringLiteral("⑤ itemCount() 只数 image 行（archive 容器不计）"));
        check(store.itemIndexOfChain(cid, { QStringLiteral("vol01.cbz") }, 1) == 1,
              QStringLiteral("⑤ 链 + 页号命中第 2 页（替换后的行也可查）"));
    }

    // ── ⑥ 乱序输入 ⇒ 读回仍按 `ord` 升序 ──
    {
        QVector<library::Item> shuffled;
        shuffled << items[2] << items[0] << items[3] << items[1];   // ord = 2,0,3,1
        check(store.replaceItems(cid, shuffled), QStringLiteral("⑥ 乱序输入替换"));
        const QVector<library::Item> got = store.items(cid);
        bool ordered = got.size() == 4;
        for (int i = 1; ordered && i < got.size(); ++i) ordered = got[i - 1].ord < got[i].ord;
        check(ordered, QStringLiteral("⑥ 读回按 `ord` 升序（%1 行）").arg(got.size()));
    }

    // ── ⑦ 失败 ⇒ 回滚 + 旧内容原样保留 ──
    {
        QVector<library::Item> bad = items;
        bad[2].ord = bad[1].ord;                     // 同一段里撞 UNIQUE(collection_id, ord)
        check(!store.replaceItems(cid, bad), QStringLiteral("⑦ 同段内 ord 重复 ⇒ 替换失败（false）"));
        check(!store.lastError().isEmpty(),
              QStringLiteral("⑦ 失败写了 lastError：%1").arg(store.lastError()));
        const QVector<library::Item> after = store.items(cid);
        check(after.size() == 4 && after.first().label == QLatin1String("edge"),
              QStringLiteral("⑦ 失败后**旧内容原样保留**（%1 行）").arg(after.size()));
        check(store.replaceItems(cid, items) && store.items(cid).size() == 4,
              QStringLiteral("⑦ 失败之后库仍可用（再替换成立）"));
    }

    // ── ⑧ 分块边界（`kReplaceChunkRows`）：恰好 1 块 / 刚过 1 块 ⇒ 多块也无巨型语句 ──
    const int chunk = library::Store::kReplaceChunkRows;
    {
        QVector<library::Item> exact;
        for (int i = 0; i < chunk; ++i)
            exact << mkItem(i, -1, Sequence::Kind::Image, QStringLiteral("e%1").arg(i));
        check(store.replaceItems(cid, exact),
              QStringLiteral("⑧ 恰好 %1 行（= 1 块）替换成功").arg(chunk));
        check(store.items(cid).size() == chunk,
              QStringLiteral("⑧ 1 块边界：行数正确（%1）").arg(store.items(cid).size()));
    }
    {
        QVector<library::Item> over;
        for (int i = 0; i < chunk + 1; ++i)
            over << mkItem(i, -1, Sequence::Kind::Image, QStringLiteral("o%1").arg(i));
        check(store.replaceItems(cid, over),
              QStringLiteral("⑧ %1 行（= 2 块）替换成功").arg(chunk + 1));
        const QVector<library::Item> got = store.items(cid);
        check(got.size() == chunk + 1 && got.first().label == QLatin1String("o0")
                  && got.last().label == QStringLiteral("o%1").arg(chunk),
              QStringLiteral("⑧ 跨块替换：新旧行数正确 + 首尾有序（%1）").arg(got.size()));
    }
    //   跨块失败：把**第二块**里的一行 ord 撞到**第一块** ⇒ 第一块已 INSERT 也必须整段回滚
    {
        QVector<library::Item> bad;
        for (int i = 0; i < chunk + 1; ++i)
            bad << mkItem(i, -1, Sequence::Kind::Image, QStringLiteral("x%1").arg(i));
        bad[chunk].ord = bad[3].ord;                 // 第 2 块撞第 1 块
        check(!store.replaceItems(cid, bad),
              QStringLiteral("⑧ 跨块失败（第二块撞第一块）⇒ false"));
        const QVector<library::Item> after = store.items(cid);
        check(after.size() == chunk + 1 && after.first().label == QLatin1String("o0")
                  && after.last().label == QStringLiteral("o%1").arg(chunk),
              QStringLiteral("⑧ 跨块失败后**旧内容原样保留**（%1 行，不是空/半截）").arg(after.size()));
    }

    // ── ⑨ 替换只影响该集合 + 不存在集合 ⇒ 失败（外键）──
    const qint64 cid2 = store.addCollection(tmp.path() + QStringLiteral("/album2"),
                                            Sequence::Kind::Folder);
    check(cid2 > 0, QStringLiteral("⑨ 加入第二个集合（%1）").arg(cid2));
    check(store.replaceItems(cid2, { mkItem(0, -1, Sequence::Kind::Image,
                                            QStringLiteral("other")) }),
          QStringLiteral("⑨ 第二个集合落 1 行"));
    check(store.replaceItems(cid, items), QStringLiteral("⑨ 再替换第一个集合"));
    check(store.items(cid2).size() == 1
              && store.items(cid2).first().label == QLatin1String("other"),
          QStringLiteral("⑨ 替换第一个集合**不影响**第二个（它仍是 1 行）"));
    check(store.itemCounts().value(cid, -1) == 3 && store.itemCounts().value(cid2, -1) == 1,
          QStringLiteral("⑨ itemCounts() 覆盖两个集合（3 / 1）"));
    check(!store.replaceItems(999999, { mkItem(0, -1, Sequence::Kind::Image,
                                               QStringLiteral("ghost")) }),
          QStringLiteral("⑨ 替换**不存在的集合** ⇒ 失败（外键约束，不写孤儿行）"));

    // ── ⑩ 自己写库不 bump `data_version`（跨进程同步只认别人的提交）──
    {
        const int dv1 = store.dataVersion();
        check(store.replaceItems(cid, items), QStringLiteral("⑩ 再替换一次（前置）"));
        check(store.dataVersion() == dv1,
              QStringLiteral("⑩ replaceItems **自己写库** ⇒ data_version 不变"));
    }

    // ── ⑪ 同 ord 但 kind 不同 ⇒ **被替换**（不是 upsert 合并）──
    {
        QVector<library::Item> one;
        one << mkItem(1, -1, Sequence::Kind::Folder, QStringLiteral("now-a-folder"));
        check(store.replaceItems(cid, one), QStringLiteral("⑪ 用同 ord、不同 kind 的 1 行替换"));
        const QVector<library::Item> got = store.items(cid);
        check(got.size() == 1 && got[0].kind == Sequence::Kind::Folder
                  && got[0].label == QLatin1String("now-a-folder"),
              QStringLiteral("⑪ 旧 image 行被**替换**掉（旧模型 upsert 会合并成 2 行）"));
        check(store.itemCount(cid) == 0, QStringLiteral("⑪ 全被换成 folder ⇒ 图数 0"));
    }
    check(store.replaceItems(cid, items) && store.items(cid).size() == 4,
          QStringLiteral("⑪ 收尾：替换回 4 行（供缩略图/级联段使用）"));

    // ── 缩略图缓存（设计 11 §五：进库 + 按条数 LRU）──────────────
    check(store.autoVacuumMode() == 2, QStringLiteral("打开库时 auto_vacuum=INCREMENTAL"));
    auto blob = [](int n) { return QByteArray(n, 'x'); };

    library::Thumb t;
    t.collectionId = cid;
    t.itemKey = QStringLiteral("edge/p0.png");
    t.sizeClass = 128;
    t.srcMtime = 111;
    t.w = 128;
    t.h = 96;
    t.bytes = blob(6 * 1024);
    t.usedAt = 100;

    check(store.thumbPut(t), QStringLiteral("写入缩略图"));
    library::Thumb got;
    check(store.thumbGet(cid, QStringLiteral("edge/p0.png"), 128, 111, &got),
          QStringLiteral("取图命中"));
    check(got.bytes == t.bytes && got.w == 128 && got.h == 96,
          QStringLiteral("命中内容一致（w/h/bytes）"));
    check(!store.thumbGet(cid, QStringLiteral("edge/p0.png"), 128, 222, nullptr),
          QStringLiteral("src_mtime 变了 → 未命中（重做）"));
    check(store.thumbCount() == 0, QStringLiteral("失效那行已删掉"));
    check(!store.thumbGet(cid, QStringLiteral("没有这张"), 128, 111, nullptr),
          QStringLiteral("未知 key → 未命中"));

    check(store.thumbPut(t), QStringLiteral("重新写入"));
    t.srcMtime = 222;
    t.bytes = blob(1024);
    check(store.thumbPut(t), QStringLiteral("同键覆盖写入"));
    check(store.thumbCount() == 1, QStringLiteral("同键只有一行（不累积）"));
    check(store.thumbGet(cid, QStringLiteral("edge/p0.png"), 128, 222, &got)
              && got.bytes.size() == 1024,
          QStringLiteral("覆盖后取到新内容"));

    library::Thumb t2 = t;
    t2.sizeClass = 256;
    check(store.thumbPut(t2), QStringLiteral("另一档位写入"));
    check(store.thumbCount() == 2, QStringLiteral("条数是**所有档位合计**"));

    library::Thumb big = t;
    big.itemKey = QStringLiteral("too-big");
    big.bytes = blob(library::Store::kThumbMaxBytes + 1);
    check(!store.thumbPut(big), QStringLiteral("BLOB 超过 %1KB → 拒写")
                                    .arg(library::Store::kThumbMaxBytes / 1024));

    // 直接打开的来源：collection_id 为 NULL（设计 11 §五）
    library::Thumb direct = t;
    direct.collectionId = -1;
    direct.itemKey = QStringLiteral("/tmp/direct.png");
    check(store.thumbPut(direct), QStringLiteral("写「直接打开」的缩略图（collection_id=NULL）"));
    check(store.thumbGet(-1, QStringLiteral("/tmp/direct.png"), 128, 222, &got),
          QStringLiteral("NULL 集合也能命中"));
    check(store.thumbPut(direct), QStringLiteral("NULL 集合重复写"));
    check(store.thumbCount() == 3, QStringLiteral("NULL 集合不重复累积"));

    // ── 缩略图规格（设计 11 §5.1，用户 2026-10-11 定案）──────────
    //   规格：**等比适应**到最长边 kThumbSize（384），保持源图宽高比、**不裁切**；
    //   WebP q=50；BLOB ≤ kThumbMaxBytes（64KB）。
    //   "恒用 kThumbSize、忽略 requestedSize、单档位"是 requestImage/thumbSizeClass
    //   的**代码结构**保证的，不在这里重复断言 —— 只证明几何与上限这两条事实。
    const QString thumbDir = tmp.path() + QStringLiteral("/thumbsrc");
    QDir().mkpath(thumbDir);
    QImage sample(800, 600, QImage::Format_RGB32);     // **非方形**源图：裁切会得 384×384
    for (int y = 0; y < sample.height(); ++y)
        for (int x = 0; x < sample.width(); ++x)
            sample.setPixel(x, y, qRgb((x * 255) / sample.width(),
                                       (y * 255) / sample.height(),
                                       ((x + y) * 255) / (sample.width() + sample.height())));
    // 写入磁盘只是**夹具**：写失败会让下面的几何断言直接失败（0×0），不单独占一条
    if (!sample.save(thumbDir + QStringLiteral("/src.png"), "PNG"))
        qWarning().noquote() << QStringLiteral("[libselftest] 夹具写盘失败: %1/%2")
                                    .arg(thumbDir, QStringLiteral("src.png"));

    //   ★ 2026-10-19：provider 已**只服务按路径的缩略图**（`thumbpath/` / `thumbzip/`）——
    //     按序号的 `thumb/<n>` 随"provider 自建序列"一起删了。
    ImageProvider provider;
    QSize thumbSize;
    // 请求尺寸 168（≠ kThumbSize）：返回图必须仍是 384 级 + 源图宽高比
    //   裁切 → 384×384；按请求尺寸 → 168 级。两种错法都过不了这一条。
    const QImage th = provider.requestImage(
        ImageProvider::thumbPathId(thumbDir + QStringLiteral("/src.png")), &thumbSize, QSize(168, 168));
    const int longest = qMax(th.width(), th.height());
    check(!th.isNull()
              && longest == library::Store::kThumbSize
              && th.width() * sample.height() == th.height() * sample.width()
              && thumbSize == th.size(),
          QStringLiteral("固定尺寸 + 不裁切：%1×%2 源图，请求 168 → %3×%4"
                         "（最长边 = kThumbSize = %5，宽高比与源一致）")
              .arg(sample.width()).arg(sample.height())
              .arg(th.width()).arg(th.height()).arg(library::Store::kThumbSize));

    // 按条数 LRU 淘汰（上限 2000，所有档位合计）
    int manyWrites = 0;
    for (int i = 0; i < library::Store::kThumbLimit + 5; ++i) {
        library::Thumb x;
        x.collectionId = cid;
        x.itemKey = QStringLiteral("many/%1").arg(i);
        x.sizeClass = 128;
        x.srcMtime = 1;
        x.bytes = blob(64);
        x.usedAt = 1000 + i;                       // 构造确定的 LRU 顺序
        if (!store.thumbPut(x)) break;
        ++manyWrites;
    }
    check(manyWrites == library::Store::kThumbLimit + 5,
          QStringLiteral("批量写入 %1 条").arg(manyWrites));
    check(store.thumbCount() <= library::Store::kThumbLimit,
          QStringLiteral("LRU 淘汰后条数 %1 ≤ %2")
              .arg(store.thumbCount()).arg(library::Store::kThumbLimit));
    check(!store.thumbGet(cid, QStringLiteral("many/0"), 128, 1, nullptr),
          QStringLiteral("最久未用的已被淘汰"));
    check(store.thumbGet(cid, QStringLiteral("many/%1").arg(library::Store::kThumbLimit + 4),
                         128, 1, nullptr),
          QStringLiteral("最新写入的仍在"));

    // 移除集合（级联）
    check(store.removeCollection(cid), QStringLiteral("移除集合"));
    check(store.items(cid).isEmpty(), QStringLiteral("条目级联删除"));

    return check.finish();
}

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, &runLibrarySelfTest);
}
