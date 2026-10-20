#include "testsupport.h"

#include <QBuffer>
#include <QColorSpace>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

#include "exifmeta.h"

// ══ 元数据（EXIF + 色彩空间）自检（ib-test-meta）══════════════════════════════
//   设计 17：`exifmeta` 是**纯函数**（来源 → 行 + 色彩空间判词），不碰解码、不进状态。
//   这里用 `.testdata` 的真实素材 + 自己造的 EXIF 块，验证：
//     ① 漫画页（JPEG + sRGB ICC，走**路径**源）：色彩空间 = `sRGB`，动态行里有 `软件`，
//        且**结构噪声**（Orientation / DPI / PixelXDimension）被过滤掉；
//     ② 自己造的 EXIF 块（IFD0 的 Make / Model / DateTime，走**字节**源）解析成中文键；
//     ③ 无 ICC 的 JPEG：`sRGB（无 profile）`，且全是噪声 ⇒ 0 行；
//     ④ PNG：没有 EXIF ⇒ 0 行；
//     ⑤ Display P3 的 PNG：判词 = `Display P3 → 已转 sRGB`（与解码点同一个解析器）；
//     ⑥ **HEIC**（BMFF 容器）：路径源与字节源给出同样的行 —— 这条以前整组不出现
//        （手写容器扫描只认 JPEG/PNG/WebP/TIFF），用户 2026-10-21 定案换 Exiv2。
namespace {

bool hasRow(const exifmeta::Info &info, const QString &key, const QString &value = QString())
{
    for (const auto &row : info.rows) {
        if (row.first != key) continue;
        if (value.isEmpty() || row.second == value) return true;
    }
    return false;
}

exifmeta::Info readPath(const QString &path)
{
    exifmeta::Source src;
    src.path = path;
    return exifmeta::read(src);
}

exifmeta::Info readBytes(const QByteArray &bytes)
{
    exifmeta::Source src;
    src.bytes = bytes;
    return exifmeta::read(src);
}

// ── 自己造一个最小 EXIF 块（TIFF little-endian，IFD0 三条 ASCII）──
QByteArray makeExifBlock()
{
    QByteArray tiff;
    const auto put8  = [&tiff](int v) { tiff.append(char(v & 0xFF)); };
    const auto put16 = [&tiff, &put8](int v) { put8(v); put8(v >> 8); };
    const auto put32 = [&tiff, &put16](int v) { put16(v); put16(v >> 16); };

    tiff.append("II", 2);
    put16(42);                                  // TIFF magic
    put32(8);                                   // IFD0 偏移

    struct Entry { quint16 tag; QByteArray value; };
    const QVector<Entry> entries = {
        { 0x010F, QByteArray("TestCam\0", 8) },              // Make（含结尾 NUL）
        { 0x0110, QByteArray("Model X\0", 8) },              // Model
        { 0x0132, QByteArray("2026:10:21 01:00:00\0", 20) }, // DateTime
    };
    put16(entries.size());
    const int valueArea = 8 + 2 + entries.size() * 12 + 4;
    int offset = valueArea;
    QByteArray tail;
    for (const Entry &e : entries) {
        const QByteArray val = e.value;
        put16(e.tag);
        put16(2);                               // ASCII
        put32(val.size());                      // 计数（含结尾 NUL）
        if (val.size() <= 4) {
            QByteArray inlineVal = val;
            inlineVal.resize(4, '\0');
            tiff.append(inlineVal);
        } else {
            put32(offset);
            offset += val.size();
            tail.append(val);
        }
    }
    put32(0);                                   // 没有下一个 IFD
    tiff.append(tail);
    return tiff;
}

// ── 把 EXIF 块插进一张真实 JPEG（SOI 之后一个 APP1），得到"有 EXIF 的照片" ──
QByteArray jpegWithExif(const QString &baseJpeg)
{
    QFile f(baseJpeg);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray src = f.readAll();
    if (src.size() < 4 || !src.startsWith("\xFF\xD8")) return {};
    const QByteArray block = makeExifBlock();
    const QByteArray payload = QByteArray("Exif\0\0", 6) + block;
    const int segLen = payload.size() + 2;                     // 长度字段含它自己
    QByteArray app1;
    app1.append(char(0xFF));
    app1.append(char(0xE1));
    app1.append(char((segLen >> 8) & 0xFF));
    app1.append(char(segLen & 0xFF));
    app1.append(payload);
    return src.left(2) + app1 + src.mid(2);
}

}  // namespace

static int runMetaSelfTest()
{
    testsupport::Check check(QStringLiteral("metaselftest"));

    const QString manga = QStringLiteral(".testdata/vol01/[七龙珠完全版]第01卷_000.jpg");
    const QString exifRot = QStringLiteral(".testdata/edge/exif6.jpg");
    const QString plainPng = QStringLiteral(".testdata/edge/p0.png");
    const QString heic = QStringLiteral(".testdata/edge/exif.heic");
    if (!QFileInfo::exists(manga) || !QFileInfo::exists(exifRot) || !QFileInfo::exists(plainPng))
        return testsupport::skip(QStringLiteral("metaselftest"),
                                 QStringLiteral("样例缺失（.testdata/vol01 或 edge）—— 未做任何检查"));

    // ① 漫画页：sRGB ICC + Software，噪声被过滤
    {
        const exifmeta::Info info = readPath(manga);
        check(info.colorSpace == QStringLiteral("sRGB"),
              QStringLiteral("漫画页色彩空间 = sRGB（实际：%1）").arg(info.colorSpace));
        check(hasRow(info, QStringLiteral("软件"), QStringLiteral("Adobe Photoshop CS2 Windows")),
              QStringLiteral("漫画页动态行含「软件」"));
        check(!hasRow(info, QStringLiteral("Orientation"))
                  && !hasRow(info, QStringLiteral("XResolution"))
                  && !hasRow(info, QStringLiteral("PixelXDimension"))
                  && !hasRow(info, QStringLiteral("0x0112")),
              QStringLiteral("结构噪声（Orientation / DPI / 像素尺寸）不出现"));
        check(info.rows.size() < 10, QStringLiteral("漫画页行数受控（%1 行）").arg(info.rows.size()));
    }

    // ② 自己造的 EXIF：Make / Model / DateTime 解析成中文键（字节源）
    {
        const QByteArray jpeg = jpegWithExif(exifRot);
        check(!jpeg.isEmpty(), QStringLiteral("造出带 EXIF 的 JPEG（%1 字节）").arg(jpeg.size()));
        const exifmeta::Info info = readBytes(jpeg);
        check(hasRow(info, QStringLiteral("相机厂商"), QStringLiteral("TestCam")),
              QStringLiteral("Make → 「相机厂商」"));
        check(hasRow(info, QStringLiteral("机型"), QStringLiteral("Model X")),
              QStringLiteral("Model → 「机型」"));
        check(hasRow(info, QStringLiteral("修改时间"), QStringLiteral("2026:10:21 01:00:00")),
              QStringLiteral("DateTime → 「修改时间」"));
    }

    // ③ 无 ICC 的 JPEG：全是噪声 ⇒ 0 行
    {
        const exifmeta::Info info = readPath(exifRot);
        check(info.colorSpace == QStringLiteral("sRGB（无 profile）"),
              QStringLiteral("无 ICC ⇒ 「sRGB（无 profile）」（实际：%1）").arg(info.colorSpace));
        check(info.rows.isEmpty(), QStringLiteral("只有噪声标签 ⇒ 0 行（实际：%1）").arg(info.rows.size()));
    }

    // ④ PNG 无 EXIF
    {
        const exifmeta::Info info = readPath(plainPng);
        check(info.rows.isEmpty(), QStringLiteral("PNG 没有 EXIF ⇒ 0 行"));
    }

    // ⑤ Display P3：判词与解码点一致
    {
        QTemporaryDir tmp;
        const QString p3 = tmp.filePath(QStringLiteral("p3.png"));
        QImage img(16, 16, QImage::Format_RGB32);
        img.fill(QColor(128, 64, 32));
        img.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
        check(img.save(p3), QStringLiteral("生成 Display P3 的 PNG"));
        const exifmeta::Info info = readPath(p3);
        check(info.colorSpace == QStringLiteral("Display P3 → 已转 sRGB"),
              QStringLiteral("P3 判词（实际：%1）").arg(info.colorSpace));
    }

    // ⑥ HEIC（BMFF 容器）：路径源能用，且与字节源结果一致
    if (QFileInfo::exists(heic)) {
        const exifmeta::Info byPath = readPath(heic);
        check(hasRow(byPath, QStringLiteral("相机厂商"), QStringLiteral("TestCam")),
              QStringLiteral("HEIC 走路径源：Make → 「相机厂商」"));
        check(hasRow(byPath, QStringLiteral("机型"), QStringLiteral("X100V")),
              QStringLiteral("HEIC：Model → 「机型」"));
        QFile f(heic);
        const QByteArray bytes = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        const exifmeta::Info byBytes = readBytes(bytes);
        check(byBytes.rows == byPath.rows,
              QStringLiteral("HEIC：路径源与字节源结果一致（%1 / %2 行）")
                  .arg(byPath.rows.size()).arg(byBytes.rows.size()));
    } else {
        check(true, QStringLiteral("HEIC 样例缺失（.testdata/edge/exif.heic）—— 跳过"));
    }

    return check.finish();
}

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, [] { return runMetaSelfTest(); });
}
