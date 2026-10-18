#include "testsupport.h"

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QIODevice>
#include <QPair>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>

#include "librarycore.h"
#include "librarystore.h"
#include "sequence.h"

// ══ 图集来源 → 查看序列 自检（ib-test-collection；原先由 `image-browser --selftest-collection` 跑）══
//   设计 10 §11.1 / 设计 11 §四。验证"图集来源接进新 VM"的每一环，且**全部不碰盘**：
//     ① 库行 → 序列（唯一入口 `Sequence::restore`）
//     ② 像素尺寸来自库 ⇒ **素材删掉后**宽窄分组照样成立（证明没读盘）
//     ③ 图集级阅读版面 ↔ 序列模式（唯一映射）
//     ④ resume 存 key ⇒ `indexOfKey(key)` 拿回下标
static int runCollectionSelfTest()
{
    testsupport::TempFixture tmp;
    tmp.useLibraryDb(QStringLiteral("coll.db"));
    // ★ 本用例不再改设置（内层包上限**调用时传参**，见 ⑬）；设置文件仍指到临时位置，
    //   保证任何间接读设置的地方都**绝不碰用户的真配置**
    tmp.useSettingsFile(QStringLiteral("settings.json"));

    testsupport::Check check(QStringLiteral("colltest"));

    // ── 夹具：3 张窄（600×900）+ 1 张宽（1600×900）+ **一个含图的压缩包** ──
    //   ★ 压缩包**自己造**（ustar tar；不依赖 `.testdata`，那里是 gitignore 的本地素材）：
    //     同构验收必须真的含压缩包（定案：库 = 已写入磁盘的序列，压缩包内的页也要在）。
    const QString album = tmp.filePath(QStringLiteral("album"));
    QDir().mkpath(album);
    auto pngBytes = [](const QSize &sz, int tone) {
        QImage img(sz, QImage::Format_RGB32);
        img.fill(QColor(30 + tone * 20, 80, 140));
        QByteArray bytes;
        QBuffer buf(&bytes);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        return bytes;
    };
    for (int i = 0; i < 4; ++i) {
        const QSize sz = (i == 3) ? QSize(1600, 900) : QSize(600, 900);
        QImage img(sz, QImage::Format_RGB32);
        img.fill(QColor(30 + i * 20, 80, 140));
        if (!img.save(album + QStringLiteral("/p%1.png").arg(i))) {
            qWarning() << "[colltest] 造素材失败";
            return 1;
        }
    }
    // 一个 ustar tar（**能随机读**的容器；名字短，内容 = 两张真 PNG）
    auto writeTar = [](const QString &path,
                       const QVector<QPair<QString, QByteArray>> &entries) -> bool {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) return false;
        for (const auto &e : entries) {
            QByteArray h(512, '\0');
            const QByteArray name = e.first.toUtf8();
            memcpy(h.data(), name.constData(), size_t(qMin<int>(100, name.size())));
            auto putOctal = [&](int off, int len, qint64 v) {
                const QByteArray o = QByteArray::number(v, 8).rightJustified(len - 1, '0');
                memcpy(h.data() + off, o.constData(), size_t(len - 1));
                h[off + len - 1] = ' ';
            };
            putOctal(100, 8, 0644);                     // mode
            putOctal(108, 8, 0);                        // uid
            putOctal(116, 8, 0);                        // gid
            putOctal(124, 12, e.second.size());         // size
            putOctal(136, 12, 0);                       // mtime
            h[156] = '0';                               // typeflag = 普通文件
            memcpy(h.data() + 257, "ustar", 5);         // magic
            h[262] = '0'; h[263] = '0';                 // version "00"
            memset(h.data() + 148, ' ', 8);             // 校验和先填空格
            unsigned sum = 0;
            for (char c : h) sum += static_cast<unsigned char>(c);
            const QByteArray cs = QByteArray::number(sum, 8).rightJustified(6, '0');
            memcpy(h.data() + 148, cs.constData(), 6);
            h[154] = '\0'; h[155] = ' ';
            if (f.write(h) != 512) return false;
            if (f.write(e.second) != e.second.size()) return false;
            const int pad = (512 - (e.second.size() % 512)) % 512;
            if (pad > 0) { QByteArray z(pad, '\0'); f.write(z); }
        }
        QByteArray end(1024, '\0');
        f.write(end);
        return true;
    };
    check(writeTar(album + QStringLiteral("/book.tar"),
                   { { QStringLiteral("y0.png"), pngBytes(QSize(600, 900), 1) },
                     { QStringLiteral("y1.png"), pngBytes(QSize(600, 900), 2) } }),
          QStringLiteral("夹具：造一个含 2 张图的压缩包（book.tar）"));
    // ★ 嵌套压缩包：`outer.tar` 里装着 `inner.tar`，包里还有两张图
    //   （v5 验收点：**不解到临时文件**，取出来的字节再当容器打开）
    {
        QByteArray inner;
        {
            QTemporaryDir d;
            const QString ip = d.filePath(QStringLiteral("inner.tar"));
            const bool ok = d.isValid() && writeTar(ip,
                { { QStringLiteral("n0.png"), pngBytes(QSize(600, 900), 3) },
                  { QStringLiteral("n1.png"), pngBytes(QSize(600, 900), 4) } });
            check(ok, QStringLiteral("夹具：造内层压缩包（inner.tar，含 2 张图）"));
            QFile f(ip);
            if (f.open(QIODevice::ReadOnly)) inner = f.readAll();
        }
        check(!inner.isEmpty()
                  && writeTar(album + QStringLiteral("/outer.tar"),
                              { { QStringLiteral("inner.tar"), inner } }),
              QStringLiteral("夹具：造外层压缩包（outer.tar 里装 inner.tar）"));
    }
    // ★★ v7：**嵌套 RAR** 夹具 —— `deep.tar!/inner.rar!/n*.png`
    //   libunrar 只能按路径打开 ⇒ `ArchiveSource::openBytes` 对 RAR 后端**自己落临时文件**。
    //   本机只有 unrar（只读）没有 rar（压缩器）⇒ 这里按 **RAR4 store** 格式直写：
    //     marker + MAIN_HEAD(0x73) + 每个文件一个 FILE_HEAD(0x74)，HEAD_CRC = 头体 CRC32 低 16 位。
    auto writeRar = [](const QString &path,
                       const QVector<QPair<QString, QByteArray>> &entries) -> bool {
        auto crc32of = [](const QByteArray &d) {
            quint32 crc = 0xFFFFFFFFu;
            for (unsigned char ch : d) {
                crc ^= ch;
                for (int k = 0; k < 8; ++k)
                    crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
            }
            return ~crc;
        };
        auto le16 = [](QByteArray &b, quint16 v) {
            b.append(char(v & 0xFF)); b.append(char((v >> 8) & 0xFF));
        };
        auto le32 = [](QByteArray &b, quint32 v) {
            for (int i = 0; i < 4; ++i) b.append(char((v >> (8 * i)) & 0xFF));
        };
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) return false;
        if (f.write("Rar!\x1a\x07\x00", 7) != 7) return false;
        {   // MAIN_HEAD：13 字节
            QByteArray body;
            body.append(char(0x73)); le16(body, 0); le16(body, 13); le16(body, 0); le32(body, 0);
            QByteArray head; le16(head, quint16(crc32of(body) & 0xFFFF)); head.append(body);
            if (f.write(head) != head.size()) return false;
        }
        for (const auto &e : entries) {                       // FILE_HEAD（store）
            const QByteArray name = e.first.toUtf8();
            QByteArray body;
            body.append(char(0x74)); le16(body, 0x8000);       // LONG_BLOCK（数据跟在头后）
            le16(body, quint16(32 + name.size()));
            le32(body, quint32(e.second.size()));              // PACK_SIZE
            le32(body, quint32(e.second.size()));              // UNP_SIZE
            body.append(char(3));                              // HOST_OS = Unix
            le32(body, crc32of(e.second));                     // FILE_CRC（未压缩数据）
            le32(body, 0x4A5B0000);                            // FTIME（任意 DOS 时间）
            body.append(char(20)); body.append(char(0x30));    // UNP_VER / METHOD = store
            le16(body, quint16(name.size()));
            le32(body, 0x20);                                  // ATTR = archive
            body.append(name);
            QByteArray head; le16(head, quint16(crc32of(body) & 0xFFFF)); head.append(body);
            if (f.write(head) != head.size()) return false;
            if (f.write(e.second) != e.second.size()) return false;
        }
        return true;
    };
    {
        QByteArray innerRar;
        {
            QTemporaryDir d;
            const QString rp = d.filePath(QStringLiteral("inner.rar"));
            const bool ok = d.isValid() && writeRar(rp,
                { { QStringLiteral("r0.png"), pngBytes(QSize(600, 900), 5) },
                  { QStringLiteral("r1.png"), pngBytes(QSize(600, 900), 6) } });
            check(ok, QStringLiteral("夹具：造内层 RAR（inner.rar，含 2 张图）"));
            QFile f(rp);
            if (f.open(QIODevice::ReadOnly)) innerRar = f.readAll();
        }
        check(!innerRar.isEmpty()
                  && writeTar(album + QStringLiteral("/deep.tar"),
                              { { QStringLiteral("inner.rar"), innerRar } }),
              QStringLiteral("夹具：造外层压缩包（deep.tar 里装 **RAR**）"));
    }

    library::Store store;
    QString err;
    if (!store.open(&err)) { qWarning().noquote() << err; return 1; }

    // ── (a) **直接扫描**同一来源（会话序列；扫描线程 → 消费线程）──
    Sequence direct;
    direct.startScan(album, 4, false);
    for (int guard = 0; guard < 1200 && !direct.scanFinished(); ++guard) {
        direct.drain();
        QThread::msleep(5);
    }
    direct.drain();

    // ── (b) 该目录**入图集（重置）**→ 用库行重建出来的序列 ──
    //   ★ T2c-2b-2：旧 `ScanController` 夹具改成 **`LibraryCore::requestReset` + pump 到 Done**
    //     （与产品同一条路径：跑完 ⇒ 后台 `replaceItems` 原子替换）。
    const qint64 cid = store.addCollection(album, Sequence::Kind::Folder);
    {
        LibraryCore core(&store);
        QString rerr;
        check(core.requestReset(cid, LibraryCore::ResetOptions(), &rerr),
              QStringLiteral("库重置入队（err='%1'）").arg(rerr));
        check(testsupport::waitFor([&] {
                  core.pump();
                  return core.resetState(cid) == LibraryCore::ResetState::Done;
              }, 30000), QStringLiteral("库重置结束（Done）"));
        check(store.items(cid).size() >= 6,
              QStringLiteral("库里有 %1 行（树 + 条目）").arg(store.items(cid).size()));
    }
    // ★ 2026-10-18：`LibraryItemSource` 那一层已删 —— 这里直接用**库里的行**恢复
    //   （与"打开图集"同一条路；`src` 就是那条恢复出来的序列）
    const library::Collection coll = store.collection(cid);
    const QString collLabel = coll.title.isEmpty() ? QFileInfo(coll.rootPath).fileName()
                                                   : coll.title;
    Sequence src;
    src.restore(coll.rootPath, store.sequenceRows(cid), library::rootIsArchive(coll),
                      collLabel);
    const Sequence &rebuilt = src;

    // ── ★★ 同构验收（本次核心）：(a) 与 (b) **逐项相等** ──
    //   ★ v5 的比较面：`ord / parent_ord / label / page_index / kind` + 叶子顺序 +
    //     节点数 + 双幅分组结果（`groupAt` 序列）—— **没有 key / rel_path 这类拼串**
    auto dumpSeq = [](const Sequence &s) {
        QStringList out;
        out << QStringLiteral("leaves=%1").arg(s.count());
        for (int i = 0; i < s.count(); ++i) {
            const Sequence::Item &it = s.at(i);
            out << QStringLiteral("L%1 ord=%2 label=%3 page=%4 kind=%5 parentNode=%6")
                       .arg(i).arg(it.ord).arg(it.label, QString::number(it.pageIndex()))
                       .arg(QStringLiteral("image")).arg(it.parentNode);
        }
        const QVector<Sequence::TreeNode> &t = s.tree();
        out << QStringLiteral("nodes=%1").arg(t.size());
        for (int i = 0; i < t.size(); ++i) {
            const Sequence::TreeNode &n = t.at(i);
            const QString kind = (n.kind == Sequence::Kind::Folder) ? QStringLiteral("folder")
                                                                    : QStringLiteral("archive");
            out << QStringLiteral("N%1 kind=%2 label=%3 parent=%4 ord=%5 first=%6 count=%7")
                       .arg(i).arg(kind, n.label).arg(n.parent).arg(n.ord)
                       .arg(n.firstItem).arg(n.itemCount);
        }
        // 双幅分组结果：从头到尾 groupAt 一遍（每组的成员下标）
        QStringList groups;
        for (int c = 0; c >= 0 && c < s.count(); ) {
            const Sequence::Group g = s.pairAt(c);
            if (g.cursor < 0 || g.members.isEmpty()) break;
            QStringList m;
            for (int x : g.members) m << QString::number(x);
            groups << QStringLiteral("(%1)").arg(m.join(QLatin1Char(',')));
            const int nxt = s.nextPairStart(c);
            if (nxt < 0) break;
            c = nxt;
        }
        out << QStringLiteral("double=%1").arg(groups.join(QLatin1Char(' ')));
        return out.join(QLatin1Char('\n'));
    };
    const QString dumpA = dumpSeq(direct);
    const QString dumpB = dumpSeq(rebuilt);
    if (dumpA != dumpB) {
        qInfo().noquote() << QStringLiteral("[colltest] (a) 直接扫描:\n%1").arg(dumpA);
        qInfo().noquote() << QStringLiteral("[colltest] (b) 库行重建:\n%1").arg(dumpB);
    } else {
        qInfo().noquote() << QStringLiteral("[colltest] 同构 dump:\n%1").arg(dumpA);
    }
    check(dumpA == dumpB,
          QStringLiteral("★★ 同构：直接扫描(…) == 库行重建（叶子 ord/label/page_index/kind + "
                         "节点 kind/label/parent/ord/直接条目区间 + 叶子顺序 + 双幅分组）"));
    check(direct.count() == rebuilt.count() && direct.count() >= 8,
          QStringLiteral("叶子数相等且含**嵌套压缩包内的页**（%1 条）").arg(direct.count()));
    check(direct.tree().size() == rebuilt.tree().size() && direct.tree().size() >= 3,
          QStringLiteral("节点数相等（%1 个：根 + book.tar + outer.tar + inner.tar）")
              .arg(direct.tree().size()));
    {
        bool hasTarNode = false, hasOuter = false, hasInner = false;
        bool hasDeep = false, hasInnerRar = false;              // ★ v7：`deep.tar!/inner.rar!`
        for (const Sequence::TreeNode &n : rebuilt.tree()) {
            if (n.kind != Sequence::Kind::Archive) continue;
            if (n.label == QLatin1String("book.tar")) hasTarNode = true;
            if (n.label == QLatin1String("outer.tar")) hasOuter = true;
            if (n.label == QLatin1String("inner.tar")) hasInner = true;
            if (n.label == QLatin1String("deep.tar")) hasDeep = true;
            if (n.label == QLatin1String("inner.rar")) hasInnerRar = true;
        }
        check(hasTarNode && hasOuter && hasInner && hasDeep && hasInnerRar,
              QStringLiteral("压缩包内的页在两边都存在，且容器节点 = book.tar / outer.tar / "
                             "inner.tar / deep.tar / inner.rar（嵌套 RAR）"));
    }
    // ★★ v5 验收点：**嵌套压缩包能真的解出图**（取出来的字节再当容器打开；不落临时文件）
    //   ⚠️ v7 起夹具里还有 `deep.tar!/inner.rar!/*`（另一种嵌套）⇒ 这里按**链首**精确挑
    //      `inner.tar` 那一支（原来是"第一个 member ≥ 2 的条目"，会挑到 RAR 那支）。
    {
        int nested = -1;
        for (int i = 0; i < rebuilt.count(); ++i)
            if (rebuilt.at(i).members().size() >= 2
                && rebuilt.at(i).members().first() == QLatin1String("inner.tar")) {
                nested = i;
                break;
            }
        check(nested >= 0, QStringLiteral("库行重建里能找到 member 链长度 ≥ 2 的嵌套条目"));
        if (nested >= 0) {
            const Sequence::Item &it = rebuilt.at(nested);
            check(!it.diskPath().isEmpty() && QFileInfo(it.diskPath()).exists(),
                  QStringLiteral("嵌套条目的磁盘路径 = 最外层容器（%1）")
                      .arg(QFileInfo(it.diskPath()).fileName()));
            check(it.members().size() == 2 && it.members().first() == QLatin1String("inner.tar"),
                  QStringLiteral("成员链 = [inner.tar, %1]").arg(it.members().value(1)));
            const QImage img = Sequence::image(it);
            check(!img.isNull() && img.width() > 0 && img.height() > 0,
                  QStringLiteral("★ **嵌套压缩包内的图真的解出来了**（%1×%2）")
                      .arg(img.width()).arg(img.height()));
        }
    }
    // ★★ v7 验收点：**嵌套 RAR 也能真的解出图**
    //   （libunrar 只能按路径打开 ⇒ `ArchiveSource::openBytes` 对 RAR 后端自己落临时文件，
    //    上层代码一个字都没改 —— 零分派）
    {
        int rarNested = -1;
        for (int i = 0; i < rebuilt.count(); ++i)
            if (rebuilt.at(i).members().size() == 2
                && rebuilt.at(i).members().first() == QLatin1String("inner.rar")) {
                rarNested = i;
                break;
            }
        check(rarNested >= 0,
              QStringLiteral("库行重建里能找到 `deep.tar!/inner.rar!/*` 的嵌套条目"));
        if (rarNested >= 0) {
            const Sequence::Item &it = rebuilt.at(rarNested);
            const QImage img = Sequence::image(it);
            check(!img.isNull() && img.width() > 0 && img.height() > 0,
                  QStringLiteral("★ **嵌套 RAR 内的图真的解出来了**（%1×%2）—— openBytes 内部落临时文件")
                      .arg(img.width()).arg(img.height()));
            // 同一个内层 RAR 的下一张 ⇒ **区间内复用同一份物化产物**（不重新落临时文件）
            if (rarNested + 1 < rebuilt.count()
                && rebuilt.at(rarNested + 1).members().value(0) == QLatin1String("inner.rar")) {
                const QImage img2 = Sequence::image(rebuilt.at(rarNested + 1));
                check(!img2.isNull(),
                      QStringLiteral("★ 同一内层 RAR 的下一张也解出来了（区间内复用同一份物化产物）"));
            }
        }
    }

    // ── ★ 把素材删掉：接下来的分组必须**只靠库里的尺寸**（碰盘就必然失效）──
    check(QDir(album).removeRecursively(), QStringLiteral("删掉素材目录（此后不许碰盘）"));
    {
        Sequence noDisk;
        noDisk.restore(coll.rootPath, store.sequenceRows(cid), library::rootIsArchive(coll),
                             collLabel);
        check(noDisk.count() == rebuilt.count(),
              QStringLiteral("① 库行 → 序列条目 %1 条（素材已删）").arg(noDisk.count()));
        int wide = -1, narrowCount = 0;
        for (int i = 0; i < noDisk.count(); ++i) {
            if (noDisk.isNarrow(i)) ++narrowCount; else wide = i;
        }
        check(wide >= 0 && narrowCount == noDisk.count() - 1,
              QStringLiteral("② 宽窄判定来自库里的像素尺寸（素材已删）：%1 窄 / 1 宽")
                  .arg(narrowCount));
        // ③ 双幅分组：窄+窄 ⇒ 一屏；宽图自己一屏
        const Sequence::Group g0 = noDisk.pairAt(0);
        check(g0.members == QVector<int>({ 0, 1 }) && g0.cursor == 0,
              QStringLiteral("③ 双幅：(0,1) 一屏"));
        if (wide >= 0) {
            const Sequence::Group gw = noDisk.pairAt(wide);
            check(gw.members == QVector<int>({ wide }) && gw.cursor == wide,
                  QStringLiteral("③ 双幅：宽图（%1）自己一屏").arg(wide));
        }
        // 单幅（一屏一条）是**视图那一层**的事（`ViewerModel` 按版面选），这里只查配对表本身
        check(noDisk.pairSize(0) == 2 && noDisk.pairStart(1) == 0,
              QStringLiteral("③ 配对表：第 0 条与第 1 条成对（落在第二张 ⇒ 回到那一对）"));
    }

    // ④ resume（**label 链 + 页号**，不是下标、不是拼串）
    //   ★ 2026-10-18：阅读版面的"字符串 ↔ 模式"翻译函数已删 —— 版面就是 `app::Layout`
    //     一份枚举（库里存它的值），这里不再有可测的映射。
    check(src.indexOfChain({ QStringLiteral("p2.png") }, 0) == 2
              && src.indexOfChain({ QStringLiteral("p0.png") }, 0) == 0
              && src.indexOfChain({ QStringLiteral("nope.png") }, 0) == -1,
          QStringLiteral("④ resume 存 label 链 ⇒ 沿链 walk 拿回下标（找不到 = -1）"));
    //   链是**结构化编码**（JSON 数组）：编解码往返 + 嵌套链也能命中
    {
        const QStringList chain = src.chainAt(src.indexOfChain({ QStringLiteral("p1.png") }, 0));
        check(chain == QStringList({ QStringLiteral("p1.png") }),
              QStringLiteral("④ chainAt → [p1.png]（实得 %1）").arg(chain.join(QLatin1Char('/'))));
        const QString enc = Sequence::encodeChain(chain);
        check(Sequence::decodeChain(enc) == chain && enc.startsWith(QLatin1Char('[')),
              QStringLiteral("④ label 链的**结构化编码**（JSON 数组）：%1").arg(enc));
        const int nestedIdx = src.indexOfChain(
            { QStringLiteral("outer.tar"), QStringLiteral("inner.tar"), QStringLiteral("n0.png") }, 0);
        check(nestedIdx >= 0 && src.chainAt(nestedIdx)
                                    == QStringList({ QStringLiteral("outer.tar"),
                                                     QStringLiteral("inner.tar"),
                                                     QStringLiteral("n0.png") }),
              QStringLiteral("④ 嵌套链（outer.tar/inner.tar/n0.png）沿链命中（下标 %1）").arg(nestedIdx));
        const int pageIdx = src.indexOfChain({ QStringLiteral("p2.png") }, 1);   // 页号对不上
        check(pageIdx == -1, QStringLiteral("④ 页号精确命中：普通图 + 页号 1 ⇒ -1"));
    }

    // ★★ v7 验收点：**内层压缩包上限**（用户 2026-10-14 定案）
    //   · 判定在**解压前**：从归档条目元数据取成员大小（不读数据）
    //   · 超限 ⇒ 那个内层包**不展开**（不出现 / 读不了）+ **瞬时提示**（notify，不是失败）
    //   夹具：外层 tar 里那个内层 tar 在**头里被声明成 65 MiB**（尾部补零 —— tar 允许），
    //   于是不需要真的造 65 MiB 的压缩包也能命中"解压前判定"这条路径。
    {
        const QString limitDir = tmp.filePath(QStringLiteral("limitdir"));
        QDir().mkpath(limitDir);
        QByteArray innerSmall;
        {
            QTemporaryDir d;
            const QString ip = d.filePath(QStringLiteral("inner2.tar"));
            const bool ok = d.isValid() && writeTar(ip,
                { { QStringLiteral("q0.png"), pngBytes(QSize(600, 900), 7) },
                  { QStringLiteral("q1.png"), pngBytes(QSize(600, 900), 8) } });
            check(ok, QStringLiteral("夹具：造「上限」用例的内层 tar（真实内容很小）"));
            QFile f(ip);
            if (f.open(QIODevice::ReadOnly)) innerSmall = f.readAll();
        }
        const qint64 big = 65LL * 1024 * 1024;                   // > 下限 64 MiB
        QByteArray payload = innerSmall;
        payload.resize(int(big));                                // 尾部补零
        check(writeTar(limitDir + QStringLiteral("/outer.tar"),
                       { { QStringLiteral("inner.tar"), payload } }),
              QStringLiteral("夹具：外层头里把内层声明成 %1 MiB").arg(big >> 20));

        // ★ 扫描参数**调用时传**（设计 14 §9.3：序列不读设置）—— 64 MB = 设置范围的下限
        Sequence limited;
        limited.startScan(limitDir, 3, false, Sequence::ScanOptions{ 64 });
        for (int guard = 0; guard < 1200 && !limited.scanFinished(); ++guard) {
            limited.drain();
            QThread::msleep(5);
        }
        limited.drain();
        check(limited.state() == Sequence::State::Scaned,
              QStringLiteral("超限**不是失败**：序列照常扫完（Scaned）"));
        check(limited.count() == 0,
              QStringLiteral("超限 ⇒ 该内层包**不展开**（0 条；它不出现）"));
        check(limited.noticeText().contains(QStringLiteral("超过上限")),
              QStringLiteral("超限 ⇒ **瞬时提示**（走 notify，不是失败）：%1")
                  .arg(limited.noticeText()));

        Sequence full;                                          // 阴性对照：不传 ⇒ 默认上限
        full.startScan(limitDir, 3, false);
        for (int guard = 0; guard < 1200 && !full.scanFinished(); ++guard) {
            full.drain();
            QThread::msleep(5);
        }
        full.drain();
        check(full.count() == 2 && full.noticeText().isEmpty(),
              QStringLiteral("阴性对照：默认上限（512 MB）下同一个夹具正常展开（%1 条、无提示）")
                  .arg(full.count()));
    }

    return check.finish();
}

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, &runCollectionSelfTest);
}
