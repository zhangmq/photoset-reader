#include "testsupport.h"

#include <QBuffer>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QIODevice>
#include <QSize>
#include <QString>
#include <QTemporaryDir>

#include "archive.h"
#include "imageprovider.h"
#include "librarystore.h"

// ══ 封面（压缩包按需展开）自检（ib-test-cover；原先由 `image-browser --selftest-cover` 跑）══
//   设计 11 §5.2。用**临时库**（PHOTOSET_CONFIG_DIR + PHOTOSET_LIBRARY_DB），
//   绝不碰用户的真库。验证：
//     ① 按路径请求压缩包封面 → 拿到图、最长边 = kThumbSize、宽高比与包内第 0 页一致（不裁切）
//     ② 生成后确实写进 `thumb` 表（size_class = kThumbSize）
//     ③ 第二次请求（**新实例**，内存 LRU 为空）命中库缓存，**不重新打开压缩包**
//     ④ 路径 / 成员不存在 → 空图、不崩
//     ⑤ thumbpath（普通图片文件）同样按规格出图，不裁切
static int runCoverSelfTest()
{
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        qWarning() << "[coverselftest] 无法建临时目录";
        return 1;
    }
    const QString cfgDir = tmp.path() + QStringLiteral("/config");
    const QString dbPath = tmp.path() + QStringLiteral("/library.db");
    QDir().mkpath(cfgDir);
    qputenv("PHOTOSET_CONFIG_DIR", cfgDir.toLocal8Bit());
    qputenv("PHOTOSET_LIBRARY_DB", dbPath.toLocal8Bit());

    testsupport::Check check(QStringLiteral("coverselftest"));

    // 真实压缩包缺失 → **整条套件跳过**（未做任何检查；rc=77，不算通过）
    const QString cbz = QFileInfo(QStringLiteral(".testdata/test.cbz")).absoluteFilePath();
    if (!QFile::exists(cbz))
        return testsupport::skip(QStringLiteral("coverselftest"),
                                 QStringLiteral("样例缺失 %1 —— 未做任何检查（不算通过）").arg(cbz));

    // 库（与 provider 的封面缓存同线程 → 共用同一条 SQLite 连接）
    library::Store store;
    QString err;
    check(store.open(&err), QStringLiteral("打开临时库（%1）").arg(library::Store::databasePath()));
    const int thumbsBefore = store.thumbCount();

    // 参考：包内第 0 页是谁、多大（这是"不裁切"的判定条件）
    QString member;
    QSize refSize;
    {
        ArchiveSource as;
        const bool opened = as.open(cbz);
        check(opened, QStringLiteral("打开 .testdata/test.cbz"));
        if (opened && as.count() > 0) {
            member = as.nameAt(0);
            const QByteArray b = as.dataAt(0);
            QBuffer buf;
            buf.setData(b);
            buf.open(QIODevice::ReadOnly);
            QImageReader r(&buf);
            r.setAutoTransform(true);
            refSize = r.size();
        }
    }
    check(!member.isEmpty() && refSize.isValid(),
          QStringLiteral("取到包内第 0 页：%1（%2×%3）")
              .arg(member).arg(refSize.width()).arg(refSize.height()));

    const QString id = ImageProvider::thumbZipId(cbz, member);
    qInfo().noquote() << QStringLiteral("[coverselftest] id = %1").arg(id);
    qInfo().noquote() << QStringLiteral("[coverselftest] item_key = %1")
                             .arg(ImageProvider::thumbItemKey(id));

    // ① 第一次：必须打开压缩包、出图
    ImageProvider p1;
    QSize sz1;
    const QImage c1 = p1.requestImage(id, &sz1, QSize(128, 128));
    check(!c1.isNull(), QStringLiteral("请求压缩包封面 → 拿到图 %1×%2").arg(c1.width()).arg(c1.height()));
    check(qMax(c1.width(), c1.height()) == library::Store::kThumbSize,
          QStringLiteral("最长边 == kThumbSize（%1）").arg(library::Store::kThumbSize));
    if (!c1.isNull() && refSize.isValid() && refSize.height() > 0) {
        const double got = double(c1.width()) / double(c1.height());
        const double ref = double(refSize.width()) / double(refSize.height());
        const double rel = qAbs(got - ref) / ref;
        check(rel < 0.005,
              QStringLiteral("宽高比与包内第 0 页一致（不裁切）：%1 vs %2（相对差 %3）")
                  .arg(got, 0, 'f', 4).arg(ref, 0, 'f', 4).arg(rel, 0, 'f', 5));
    } else {
        check(false, QStringLiteral("宽高比与包内第 0 页一致（不裁切）—— 前置数据缺失"));
    }
    check(p1.coverArchiveOpens() == 1,
          QStringLiteral("第一次确实打开了压缩包（可观测计数 = %1）").arg(p1.coverArchiveOpens()));

    // ② 写进 thumb 表（档位 = kThumbSize，键 = item_key + 源 mtime）
    const qint64 mtime = QFileInfo(cbz).lastModified().toSecsSinceEpoch();
    check(store.thumbCount() == thumbsBefore + 1,
          QStringLiteral("thumb 表多了一行（%1 → %2）").arg(thumbsBefore).arg(store.thumbCount()));
    library::Thumb row;
    const bool gotRow = store.thumbGet(-1, ImageProvider::thumbItemKey(id),
                                       library::Store::kThumbSize, mtime, &row);
    check(gotRow, QStringLiteral("能按 (item_key, kThumbSize, 源 mtime) 取到该行"));
    check(gotRow && row.sizeClass == library::Store::kThumbSize,
          QStringLiteral("size_class == kThumbSize（%1）").arg(row.sizeClass));
    check(gotRow && row.bytes.size() > 0 && row.bytes.size() <= library::Store::kThumbMaxBytes,
          QStringLiteral("入库 BLOB %1 字节 ≤ 上限 %2")
              .arg(row.bytes.size()).arg(library::Store::kThumbMaxBytes));
    check(gotRow && row.w == c1.width() && row.h == c1.height(),
          QStringLiteral("库里 w/h 与返回的图一致（%1×%2）").arg(row.w).arg(row.h));

    // ③ 第二次：**新实例**（内存 LRU 为空）→ 只能走 thumb 表；不得重开压缩包
    ImageProvider p2;
    QSize sz2;
    const QImage c2 = p2.requestImage(id, &sz2, QSize(320, 320));
    check(!c2.isNull() && c2.size() == c1.size(),
          QStringLiteral("第二次请求（新实例）拿到同样尺寸的图 %1×%2").arg(c2.width()).arg(c2.height()));
    check(p2.coverArchiveOpens() == 0,
          QStringLiteral("第二次**没有**重新打开压缩包（打开计数 = %1）").arg(p2.coverArchiveOpens()));
    check(p2.coverDbHits() == 1,
          QStringLiteral("第二次命中 thumb 表缓存（命中计数 = %1）").arg(p2.coverDbHits()));
    check(store.thumbCount() == thumbsBefore + 1,
          QStringLiteral("第二次没有新增行（仍 %1 行）").arg(store.thumbCount()));

    // ④ 失败容忍：路径不存在 / 成员不存在
    ImageProvider p3;
    QSize sz3;
    const QString missingPathId =
        ImageProvider::thumbPathId(tmp.path() + QStringLiteral("/nope-xyz.png"));
    check(p3.requestImage(missingPathId, &sz3, QSize()).isNull(),
          QStringLiteral("路径不存在 → 空图、不崩"));
    const QString missingMemberId = ImageProvider::thumbZipId(cbz, QStringLiteral("no-such-member-zzz.png"));
    check(p3.requestImage(missingMemberId, &sz3, QSize()).isNull(),
          QStringLiteral("包内成员不存在 → 空图、不崩"));
    check(store.thumbCount() == thumbsBefore + 1,
          QStringLiteral("失败没有往 thumb 表写东西"));

    // ④b 卡片要用的形式：**不指定成员** = 包内第 0 页（从"集合第一个条目是压缩包"直接可拼）
    {
        const QString id0 = ImageProvider::thumbZipId(cbz);
        check(ImageProvider::thumbItemKey(id0).endsWith(QStringLiteral("!/")),
              QStringLiteral("不指定成员的 item_key 以 '!/' 结尾（成员留空）"));
        ImageProvider p4;
        QSize s4;
        const QImage c4 = p4.requestImage(id0, &s4, QSize());
        check(!c4.isNull() && c4.size() == c1.size(),
              QStringLiteral("不指定成员 → 取包内第 0 页，尺寸与显式指定一致（%1×%2）")
                  .arg(c4.width()).arg(c4.height()));
        check(store.thumbCount() == thumbsBefore + 2,
              QStringLiteral("该形式也进了 thumb 表（%1 行）").arg(store.thumbCount()));
    }

    // ⑤ 普通图片文件走同一入口（thumbpath），同样等比不裁切
    const QString png = tmp.path() + QStringLiteral("/plain.png");
    {
        QImage im(200, 100, QImage::Format_RGB32);
        im.fill(Qt::darkCyan);
        im.save(png);
    }
    QSize sz5;
    const QImage c5 = p3.requestImage(ImageProvider::thumbPathId(png), &sz5, QSize(64, 64));
    check(!c5.isNull() && qMax(c5.width(), c5.height()) == library::Store::kThumbSize,
          QStringLiteral("thumbpath 普通图片出图，最长边 == kThumbSize（%1×%2）")
              .arg(c5.width()).arg(c5.height()));
    check(!c5.isNull() && qAbs(double(c5.width()) / c5.height() - 2.0) < 0.01,
          QStringLiteral("thumbpath 等比（2:1）不裁切"));

    return check.finish();
}

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, &runCoverSelfTest);
}
