#include "testsupport.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "imageprovider.h"

// ══ 进程内流程自检（ib-test-flow；原先由 `image-browser -d <目录> --selftest-flow` 跑）══
//   缺失标记 / 进入压缩包。**纯逐字搬迁**：输出格式（`  LABEL  PASS`）与 `[flow]` footer
//   保持原样，不套 `testsupport::Check`。
//   ★ 旧路径里 `provider` 是 `main` 里 `new` 出来、由 QML engine 接管的；这里栈上持有，
//     被测代码路径一模一样（`decodeRaw` / `setSource` 不受 scaler 参数影响）。
static int runFlowSelfTest()
{
    ImageProvider providerObj;
    ImageProvider *provider = &providerObj;
    int fails = 0, checks = 0, skips = 0;
    auto check = [&](const char *label, bool ok) {
        ++checks;
        if (!ok) ++fails;
        qInfo().noquote() << QStringLiteral("  %1  %2")
                                 .arg(QString::fromUtf8(label), ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
    };

    // ① 缺失标记：临时目录 → 列序列 → 删一个文件 → revalidate 应标记 1 项
    {
        QTemporaryDir tmp;
        for (int i = 0; i < 3; ++i) {
            QImage im(32, 32, QImage::Format_RGB32);
            im.fill(Qt::red);
            im.save(tmp.filePath(QStringLiteral("p%1.png").arg(i)));
        }
        provider->setSource(tmp.path(), false);
        check("目录列出 3 项", provider->count() == 3);
        QFile::remove(tmp.filePath(QStringLiteral("p1.png")));
        const int marked = provider->revalidate();
        check("删除后标记 1 项缺失", marked == 1);
        check("序列仍为 3 项（只标记不移除）", provider->count() == 3);
        check("第 2 项被标为缺失", provider->isMissingAt(1));
        check("缺失项解码返回空（不崩溃）", provider->decodeRaw(1).isNull());
    }

    // ② 进入压缩包：列目录 → 进入第 1 个压缩包 → 序列应变成内页
    {
        provider->setSource(QStringLiteral(".testdata"), false);
        int arcIdx = -1;
        for (int i = 0; i < provider->count(); ++i)
            if (provider->isArchiveAt(i)) { arcIdx = i; break; }
        check("目录里能找到压缩包条目", arcIdx >= 0);
        if (arcIdx >= 0) {
            const int before = provider->count();
            check("进入压缩包成功", provider->enterArchive(arcIdx));
            // 注意：不比较 count——目录条目数与内页数可能巧合相等
            check("序列已切换为内页", provider->insideArchive()
                  && provider->count() > 0 && provider->absPathAt(0).isEmpty());
            check("内页可解码", !provider->decodeRaw(0).isNull());
            check("返回上级成功", provider->leaveArchive());
            check("已回到目录序列", provider->count() == before && !provider->insideArchive());
        }
    }

    // ③ 空目录
    {
        QTemporaryDir tmp;
        provider->setSource(tmp.path(), false);
        check("空目录 count=0", provider->count() == 0);
        check("空目录 decodeRaw 不崩溃", provider->decodeRaw(0).isNull());
        check("空目录 pageCount=0", provider->pageCount() == 0);
    }

    // ④ 坏图 / 非图片文件混入
    {
        QTemporaryDir tmp;
        QImage im(32,32,QImage::Format_RGB32); im.fill(Qt::blue);
        im.save(tmp.filePath("ok.png"));
        QFile bad(tmp.filePath("bad.png")); bad.open(QIODevice::WriteOnly);
        bad.write("this is not a png"); bad.close();
        QFile txt(tmp.filePath("note.txt")); txt.open(QIODevice::WriteOnly);
        txt.write("hello"); txt.close();
        provider->setSource(tmp.path(), false);
        check("只列出图片后缀（txt 被排除）", provider->count() == 2);
        int okCount = 0;
        for (int i = 0; i < provider->count(); ++i)
            if (!provider->decodeRaw(i).isNull()) ++okCount;
        check("坏图返回空、好图正常（不崩溃）", okCount == 1);
    }

    // ⑤ 幻灯片遇缺失：跳过并计数，不停在坏图上
    {
        QTemporaryDir tmp;
        for (int i = 0; i < 5; ++i) {
            QImage im(32,32,QImage::Format_RGB32); im.fill(Qt::green);
            im.save(tmp.filePath(QStringLiteral("p%1.png").arg(i)));
        }
        provider->setSource(tmp.path(), false);
        QFile::remove(tmp.filePath("p2.png"));
        provider->revalidate();
        check("缺失被标记", provider->isMissingAt(2));
        // 模拟幻灯片推进：跳过缺失项
        int visited = 0, skipped = 0;
        for (int i = 0; i < provider->count(); ++i) {
            if (provider->isMissingAt(i)) { ++skipped; continue; }
            if (!provider->decodeRaw(i).isNull()) ++visited;
        }
        check("幻灯片可跳过缺失并计数", skipped == 1 && visited == 4);
    }

    // ⑥ 版面：条漫 / 跨页
    {
        QTemporaryDir tmp;
        QImage tall(800, 40000, QImage::Format_RGB32); tall.fill(Qt::white);
        tall.save(tmp.filePath("webtoon.png"));
        QImage a(1200,1800,QImage::Format_RGB32); a.fill(Qt::white); a.save(tmp.filePath("a.png"));
        QImage b(1200,1800,QImage::Format_RGB32); b.fill(Qt::white); b.save(tmp.filePath("b.png"));
        provider->setSource(tmp.path(), false);
        check("目录列出 3 项", provider->count() == 3);
        // 自然排序下是 a.png, b.png, webtoon.png —— 按名字找，不假设索引
        int wi = -1;
        for (int i = 0; i < provider->count(); ++i)
            if (provider->fileNameAt(i).contains(QStringLiteral("webtoon"))) wi = i;
        check("找得到条漫条目", wi >= 0);
        check("条漫被识别（isWebtoonAt）", wi >= 0 && provider->isWebtoonAt(wi));
        check("条漫有切片数", wi >= 0 && provider->sliceCountAt(wi) > 1);
        check("两张竖版被配成一页（3 项 → 2 页）", provider->pageCount() == 2);
    }

    // ⑦ 单张图片 + EXIF 自动旋转
    {
        QTemporaryDir tmp;
        QImage im(800, 600, QImage::Format_RGB32); im.fill(Qt::red);
        im.save(tmp.filePath("plain.png"));
        provider->setSource(tmp.filePath("plain.png"), false);
        check("单张图片 → 1 项序列", provider->count() == 1);
        check("单张图片可解码", !provider->decodeRaw(0).isNull());
        check("单张图片尺寸正确", provider->sourceSizeAt(0) == QSize(800,600));

        // EXIF 自动旋转：用仓库内真实样例（原始 800x600 + Orientation=6）
        const QString exifFile = QStringLiteral(".testdata/edge/exif6.jpg");
        if (QFile::exists(exifFile)) {
            provider->setSource(exifFile, false);
            check("EXIF 单张 → 1 项", provider->count() == 1);
            check("EXIF Orientation=6 自动旋转（800x600 → 600x800）",
                  provider->sourceSizeAt(0) == QSize(600,800));
        } else {
            // ★ 夹具缺失 = **跳过**，不是通过（T0 收尾批修掉"跳过当 PASS"）
            ++skips;
            qInfo().noquote() << QStringLiteral(
                "[flow] 跳过：EXIF 样例缺失 %1 —— 未做任何检查（不算通过）").arg(exifFile);
        }
    }

    // ⑧ 多页文件（多页 TIFF 等）按页容器处理
    {
        const QString mp = QStringLiteral(".testdata/edge/multi.tiff");
        if (QFile::exists(mp)) {
            provider->setSource(mp, false);
            check("多页 TIFF 列出 2 页", provider->count() == 2);
            check("多页 TIFF → 2 页版面", provider->pageCount() == 2);
            const QImage p0 = provider->decodeRaw(0);
            const QImage p1 = provider->decodeRaw(1);
            check("两页都可解码", !p0.isNull() && !p1.isNull());
            // 两页内容必须不同（证明真的跳页，而不是都解第 0 页）
            bool differ = false;
            if (!p0.isNull() && !p1.isNull() && p0.size() == p1.size()) {
                for (int y = 0; y < p0.height() && !differ; y += 7)
                    for (int x = 0; x < p0.width(); x += 7)
                        if (p0.pixel(x,y) != p1.pixel(x,y)) { differ = true; break; }
            }
            check("两页内容不同（真的跳页）", differ);
        } else {
            ++skips;
            qInfo().noquote() << QStringLiteral(
                "[flow] 跳过：多页样例缺失 %1 —— 未做任何检查（不算通过）").arg(mp);
        }
    }

    // ⑨ 特殊字符文件名
    {
        QTemporaryDir tmp;
        const QStringList names = {QStringLiteral("空格 文件.png"),
                                   QStringLiteral("emoji_\xF0\x9F\x8E\x8C.png"),
                                   QStringLiteral("引号'单引.png"),
                                   QStringLiteral("中文（全角）.png")};
        for (const QString &n : names) {
            QImage im(64,48,QImage::Format_RGB32); im.fill(Qt::darkCyan);
            im.save(tmp.filePath(n));
        }
        provider->setSource(tmp.path(), false);
        check("特殊字符文件名全部列出", provider->count() == names.size());
        bool allDecode = true;
        for (int i = 0; i < provider->count(); ++i)
            if (provider->decodeRaw(i).isNull()) allDecode = false;
        check("特殊字符文件名都可解码", allDecode);
    }

    // ⑩ 损坏的压缩包：不崩溃，返回失败
    {
        QTemporaryDir tmp;
        QFile f(tmp.filePath("junk.cbz"));
        f.open(QIODevice::WriteOnly);
        f.write("this is definitely not an archive");
        f.close();
        provider->setSource(f.fileName(), false);   // 不应崩溃
        check("损坏压缩包不崩溃（count 可为 0）", provider->count() >= 0);
        check("损坏压缩包 decodeRaw 不崩溃", provider->decodeRaw(0).isNull());
    }

    // ⑪ ★ 压缩包内条目的尺寸必须有效（版面判定的前提）
    //    曾经 rebuildPages 用 QImageReader(包内路径) —— 对归档条目那**不是**文件系统路径，
    //    不是文件系统路径 → 尺寸恒 0x0 → 117 项被错配成 59 页。
    {
        const QString cbz = QStringLiteral(".testdata/test.cbz");
        if (QFile::exists(cbz)) {
            provider->setSource(cbz, false);
            check("CBZ 有条目", provider->count() > 0);
            int valid = 0;
            for (int i = 0; i < provider->count(); ++i)
                if (provider->sourceSizeAt(i).isValid()) ++valid;
            check("CBZ 内每项尺寸都有效（非 0x0）", valid == provider->count());
            check("CBZ 页数与条目数一致（横版不配对）",
                  provider->pageCount() == provider->count());
        } else {
            ++skips;
            qInfo().noquote() << QStringLiteral(
                "[flow] 跳过：CBZ 样例缺失 %1 —— 未做任何检查（不算通过）").arg(cbz);
        }
    }

    // footer 与其它套件同一形状（T0 收尾批）：带项数；跳过单列（**不算通过**）
    qInfo().noquote() << (fails == 0
        ? (skips == 0
               ? QStringLiteral("[flow] 全部通过（%1 项）").arg(checks)
               : QStringLiteral("[flow] 全部通过（%1 项，跳过 %2 项）").arg(checks).arg(skips))
        : (skips == 0
               ? QStringLiteral("[flow] 有 %1 项失败（%2 项）").arg(fails).arg(checks)
               : QStringLiteral("[flow] 有 %1 项失败（%2 项，跳过 %3 项）")
                     .arg(fails).arg(checks).arg(skips)));
    return fails == 0 ? 0 : 1;
}

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, &runFlowSelfTest);
}
