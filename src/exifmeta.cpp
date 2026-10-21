#include "exifmeta.h"

#include <QColorSpace>
#include <QFile>
#include <QMap>

#include <libexif/exif-data.h>
#include <libheif/heif.h>

#include <zlib.h>

#include <algorithm>

namespace exifmeta {

namespace {

// OSD 只读这么多就够定位容器里的 EXIF/ICC；HEIC/AVIF 例外（走 libheif 的路径/整份）
constexpr int kHeadBytes = 256 * 1024;

// ══ 容器嗅探（不信扩展名，只认字节）═════════════════════════════════════════
enum class Container { Unknown, Jpeg, Png, WebP, Tiff, Heif };

// HEIF/AVIF = ISO BMFF（box 树）：`ftyp` 在偏移 4，品牌写在它里面
bool isHeifBrand(const QByteArray &h)
{
    static const char *kBrands[] = { "heic", "heix", "heim", "heis", "hevc", "hevx",
                                     "mif1", "msf1", "avif", "avis" };
    if (h.size() >= 12 && h.mid(4, 4) == "ftyp") {
        for (const char *b : kBrands) {
            if (h.mid(8, 4) == b) return true;                 // major brand
            for (int p = 16; p + 4 <= h.size(); p += 4)        // compatible brands
                if (h.mid(p, 4) == b) return true;
        }
        return false;
    }
    return false;
}

Container sniff(const QByteArray &h)
{
    if (h.size() >= 2 && (quint8)h[0] == 0xFF && (quint8)h[1] == 0xD8) return Container::Jpeg;
    if (h.size() >= 8 && h.startsWith("\x89PNG\r\n\x1a\n")) return Container::Png;
    if (h.size() >= 12 && h.startsWith("RIFF") && h.mid(8, 4) == "WEBP") return Container::WebP;
    if (h.size() >= 4 && (h.startsWith("II*\0") || h.startsWith("MM\0*"))) return Container::Tiff;
    if (isHeifBrand(h)) return Container::Heif;
    return Container::Unknown;
}

quint32 be32(const QByteArray &b, int p)
{
    if (p < 0 || p + 4 > b.size()) return 0;
    return (quint32(quint8(b[p])) << 24) | (quint32(quint8(b[p + 1])) << 16)
         | (quint32(quint8(b[p + 2])) << 8) | quint32(quint8(b[p + 3]));
}

quint32 le32(const QByteArray &b, int p)
{
    if (p < 0 || p + 4 > b.size()) return 0;
    return quint32(quint8(b[p])) | (quint32(quint8(b[p + 1])) << 8)
         | (quint32(quint8(b[p + 2])) << 16) | (quint32(quint8(b[p + 3])) << 24);
}

quint16 be16(const QByteArray &b, int p)
{
    if (p < 0 || p + 2 > b.size()) return 0;
    return quint16((quint16(quint8(b[p])) << 8) | quint8(b[p + 1]));
}

// ── JPEG：扫段直到 SOS；APP1 "Exif\0\0" = EXIF 块，APP2 "ICC_PROFILE\0" = ICC 分段 ──
struct JpegParts {
    QByteArray exif;    // TIFF 头开始的那一段
    QByteArray icc;     // 按序号拼好的完整 profile（可能为空）
    bool       hasIcc = false;
};

JpegParts scanJpeg(const QByteArray &h)
{
    JpegParts out;
    int pos = 2;                                  // 跳过 SOI
    QMap<int, QByteArray> iccSegs;
    int iccCount = 0;
    while (pos + 4 <= h.size()) {
        if ((quint8)h[pos] != 0xFF) break;
        const quint8 marker = (quint8)h[pos + 1];
        if (marker == 0xFF) { ++pos; continue; }
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            pos += 2;
            continue;
        }
        if (marker == 0xDA) break;                // SOS：后面是压缩数据
        const int len = be16(h, pos + 2);
        if (len < 2) break;
        const int body = pos + 4;
        const int bodyLen = qMin(len - 2, h.size() - body);
        if (bodyLen > 0) {
            const QByteArray blob = h.mid(body, bodyLen);
            // ★ libexif 的 `exif_data_load_data()` 要的是**带 `Exif\0\0` 前缀**的整块
            //   （实测：去掉前缀就 0 条）—— 这里保留原样，统一格式在 `read()` 里补齐
            if (marker == 0xE1 && blob.startsWith("Exif\0\0") && out.exif.isEmpty())
                out.exif = blob;
            else if (marker == 0xE2 && blob.startsWith("ICC_PROFILE\0") && blob.size() > 14) {
                const int seq = quint8(blob[12]);
                iccCount = quint8(blob[13]);
                iccSegs.insert(seq, blob.mid(14));
                out.hasIcc = true;
            }
        }
        pos = body + (len - 2);
    }
    if (iccCount > 0 && iccSegs.size() == iccCount) {
        for (int i = 1; i <= iccCount; ++i) {
            if (!iccSegs.contains(i)) { out.icc.clear(); break; }
            out.icc += iccSegs.value(i);
        }
    }
    return out;
}

// ── PNG：eXIf（未压缩的 TIFF 块）/ iCCP（profile 名 + 方法 + zlib 流）──
QByteArray inflateZlib(const QByteArray &in)
{
    if (in.isEmpty()) return {};
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return {};
    QByteArray out;
    out.resize(qMax<int>(in.size() * 3, 4096));
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
    zs.avail_in = uInt(in.size());
    int rc = Z_OK;
    while (true) {
        if (out.size() - int(zs.total_out) < 256) out.resize(out.size() * 2);
        zs.next_out = reinterpret_cast<Bytef *>(out.data()) + zs.total_out;
        zs.avail_out = uInt(out.size() - zs.total_out);
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK) { out.clear(); break; }
    }
    inflateEnd(&zs);
    if (out.isEmpty()) return {};
    out.resize(int(zs.total_out));
    return out;
}

void scanPng(const QByteArray &h, JpegParts *out)
{
    int pos = 8;
    while (pos + 8 <= h.size()) {
        const quint32 len = be32(h, pos);
        const QByteArray type = h.mid(pos + 4, 4);
        const int data = pos + 8;
        const int avail = qMin<int>(len, h.size() - data);
        if (type == "eXIf" && out->exif.isEmpty()) {
            out->exif = h.mid(data, avail);
        } else if (type == "iCCP") {
            const QByteArray blob = h.mid(data, avail);
            const int nameEnd = blob.indexOf('\0');
            if (nameEnd > 0 && nameEnd + 2 <= blob.size()) {
                const QByteArray inflated = inflateZlib(blob.mid(nameEnd + 2));
                if (!inflated.isEmpty()) { out->icc = inflated; out->hasIcc = true; }
                else if (blob.size() > nameEnd + 2) out->hasIcc = true;   // 有但解不开
            }
        } else if (type == "IEND") {
            break;
        }
        pos = data + int(len) + 4;                // 数据 + CRC
        if (len == 0) pos = data + 4;
    }
}

// ── WebP：RIFF 分块（EXIF / ICCP 都是裸块）──
void scanWebp(const QByteArray &h, JpegParts *out)
{
    int pos = 12;
    while (pos + 8 <= h.size()) {
        const QByteArray fourcc = h.mid(pos, 4);
        const quint32 len = le32(h, pos + 4);
        const int data = pos + 8;
        const int avail = qMin<int>(len, h.size() - data);
        if (fourcc == "EXIF") {
            QByteArray blob = h.mid(data, avail);
            if (blob.startsWith("Exif\0\0")) blob = blob.mid(6);
            if (out->exif.isEmpty()) out->exif = blob;
        } else if (fourcc == "ICCP") {
            out->icc = h.mid(data, avail);
            out->hasIcc = true;
        }
        pos = data + int(len) + (int(len) & 1);
    }
}

// ── TIFF：文件本身就是 EXIF 块；ICC 在 tag 0x8773 ──
QByteArray tiffIcc(const QByteArray &head)
{
    // 只解 IFD0 里 0x8773 这一条：够用，且不依赖 libexif 的 TIFF 走法
    const bool little = head.startsWith("II");
    auto rd16 = [&](int p) {
        return little ? quint16(quint8(head[p]) | (quint8(head[p + 1]) << 8)) : be16(head, p);
    };
    auto rd32 = [&](int p) {
        return little ? le32(head, p) : be32(head, p);
    };
    const int ifd0 = int(rd32(4));
    if (ifd0 <= 0 || ifd0 + 2 > head.size()) return {};
    const int n = rd16(ifd0);
    for (int i = 0; i < n; ++i) {
        const int e = ifd0 + 2 + i * 12;
        if (e + 12 > head.size()) break;
        if (rd16(e) != 0x8773) continue;
        const quint32 count = rd32(e + 4);
        const int off = int(rd32(e + 8));
        if (off <= 0 || off >= head.size()) return {};
        return head.mid(off, qMin<int>(int(count), head.size() - off));
    }
    return {};
}

// ══ EXIF 标签 → 中文名 / 排序权重 / 噪声过滤 ═════════════════════════════════
struct TagSpec {
    int     tag;
    QString label;
    int     prio;      // 越小越靠前；0 = 不参与优先（按文件原序）
};

const TagSpec kSpecs[] = {
    { EXIF_TAG_MAKE,                 QStringLiteral("相机厂商"), 1 },
    { EXIF_TAG_MODEL,                QStringLiteral("机型"),     2 },
    { EXIF_TAG_LENS_MODEL,           QStringLiteral("镜头"),     3 },
    { EXIF_TAG_DATE_TIME_ORIGINAL,    QStringLiteral("拍摄时间"), 4 },
    { EXIF_TAG_EXPOSURE_TIME,        QStringLiteral("曝光"),     5 },
    { EXIF_TAG_FNUMBER,              QStringLiteral("光圈"),     6 },
    { EXIF_TAG_ISO_SPEED_RATINGS,    QStringLiteral("ISO"),      7 },
    { EXIF_TAG_FOCAL_LENGTH,         QStringLiteral("焦距"),     8 },
    { EXIF_TAG_FOCAL_LENGTH_IN_35MM_FILM, QStringLiteral("等效焦距"), 9 },
    { EXIF_TAG_EXPOSURE_BIAS_VALUE,  QStringLiteral("曝光补偿"), 10 },
    { EXIF_TAG_MAX_APERTURE_VALUE,   QStringLiteral("最大光圈"), 11 },
    { EXIF_TAG_METERING_MODE,        QStringLiteral("测光"),     12 },
    { EXIF_TAG_FLASH,                QStringLiteral("闪光灯"),   13 },
    { EXIF_TAG_LIGHT_SOURCE,         QStringLiteral("光源"),     14 },
    { EXIF_TAG_WHITE_BALANCE,        QStringLiteral("白平衡"),   15 },
    { EXIF_TAG_EXPOSURE_PROGRAM,     QStringLiteral("曝光程序"), 16 },
    { EXIF_TAG_SOFTWARE,             QStringLiteral("软件"),     20 },
    { EXIF_TAG_ARTIST,               QStringLiteral("作者"),     21 },
    { EXIF_TAG_COPYRIGHT,            QStringLiteral("版权"),     22 },
    { EXIF_TAG_IMAGE_DESCRIPTION,    QStringLiteral("描述"),     23 },
    { EXIF_TAG_DATE_TIME,            QStringLiteral("修改时间"), 17 },
    { EXIF_TAG_LENS_MAKE,            QStringLiteral("镜头厂商"), 24 },
    { EXIF_TAG_SUBJECT_DISTANCE,     QStringLiteral("拍摄距离"), 25 },
    { EXIF_TAG_DIGITAL_ZOOM_RATIO,   QStringLiteral("数码变焦"), 26 },
    { EXIF_TAG_SCENE_CAPTURE_TYPE,   QStringLiteral("场景"),     27 },
    { EXIF_TAG_SATURATION,           QStringLiteral("饱和度"),   28 },
    { EXIF_TAG_CONTRAST,             QStringLiteral("对比度"),   29 },
    { EXIF_TAG_SHARPNESS,            QStringLiteral("锐度"),     30 },
};

const TagSpec *specFor(int tag)
{
    for (const TagSpec &s : kSpecs)
        if (s.tag == tag) return &s;
    return nullptr;
}

// 结构 / 常量类标签：对"看图的人"没有信息量，且会盖掉真正生效的结论
bool isNoise(int tag)
{
    switch (tag) {
    case EXIF_TAG_EXIF_VERSION:
    case EXIF_TAG_FLASH_PIX_VERSION:
    case EXIF_TAG_COMPONENTS_CONFIGURATION:
    case EXIF_TAG_MAKER_NOTE:
    case EXIF_TAG_USER_COMMENT:
    case EXIF_TAG_X_RESOLUTION:
    case EXIF_TAG_Y_RESOLUTION:
    case EXIF_TAG_RESOLUTION_UNIT:
    case EXIF_TAG_COLOR_SPACE:      // 常写 Uncalibrated，与真正生效的 ICC 冲突
    case EXIF_TAG_ORIENTATION:      // 效果已体现在"分辨率"行
    case EXIF_TAG_INTER_COLOR_PROFILE:   // ICC 本体，不作为行显示
    case EXIF_TAG_PIXEL_X_DIMENSION:     // 像素尺寸已在"分辨率"行
    case EXIF_TAG_PIXEL_Y_DIMENSION:
        return true;
    default:
        return false;
    }
}

// GPS 有独立的中文名（tag 号与 IFD0 重号，必须按 IFD 区分）
QString gpsLabel(int tag)
{
    switch (tag) {
    case EXIF_TAG_GPS_LATITUDE:   return QStringLiteral("纬度");
    case EXIF_TAG_GPS_LONGITUDE:  return QStringLiteral("经度");
    case EXIF_TAG_GPS_ALTITUDE:   return QStringLiteral("海拔");
    case EXIF_TAG_GPS_TIME_STAMP: return QStringLiteral("GPS 时间");
    case EXIF_TAG_GPS_DATE_STAMP: return QStringLiteral("GPS 日期");
    default:                      return {};
    }
}

struct Row { QString key; QString value; int prio; int seq; int ifd; };

QVector<Row> exifRowsFromBlock(const QByteArray &block)
{
    QVector<Row> out;
    if (block.size() < 8) return out;
    ExifData *d = exif_data_new();
    if (!d) return out;
    exif_data_load_data(d, reinterpret_cast<const unsigned char *>(block.constData()),
                        uint(block.size()));
    static const ExifIfd kIfds[] = { EXIF_IFD_0, EXIF_IFD_EXIF, EXIF_IFD_GPS };
    int seq = 0;
    for (ExifIfd ifd : kIfds) {
        ExifContent *c = d->ifd[ifd];
        if (!c) continue;
        for (unsigned i = 0; i < c->count; ++i) {
            ExifEntry *e = c->entries[i];
            if (!e || isNoise(int(e->tag))) continue;
            char buf[512] = { 0 };
            exif_entry_get_value(e, buf, sizeof buf);
            const QString value = QString::fromUtf8(buf).trimmed();
            if (value.isEmpty()) continue;
            if (ifd == EXIF_IFD_GPS && int(e->tag) <= EXIF_TAG_GPS_LONGITUDE_REF)
                continue;                        // *_REF 只是方向标记，值已在经纬度里
            QString key = (ifd == EXIF_IFD_GPS) ? gpsLabel(int(e->tag)) : QString();
            int prio = 1000;
            if (key.isEmpty()) {
                if (const TagSpec *s = specFor(int(e->tag))) { key = s->label; prio = s->prio; }
                else key = QString::fromUtf8(exif_tag_get_name_in_ifd(e->tag, ifd));
            }
            if (key.isEmpty()) continue;
            out.push_back({ key, value, prio, seq++, int(ifd) });
        }
    }
    exif_data_unref(d);
    // 常用项按固定优先级在前，其余保持文件原序
    std::stable_sort(out.begin(), out.end(), [](const Row &a, const Row &b) {
        if (a.prio != b.prio) return a.prio < b.prio;
        return a.seq < b.seq;
    });
    return out;
}

// ══ 色彩空间判词（与解码点 `toSrgb()` 同一个解析器）══════════════════════════
QString colorSpaceText(const QByteArray &icc, bool hasIcc)
{
    if (!hasIcc || icc.isEmpty()) return QStringLiteral("sRGB（无 profile）");
    const QColorSpace cs = QColorSpace::fromIccProfile(icc);
    if (!cs.isValid())
        return QStringLiteral("ICC 无法解析（按 sRGB 显示）");
    if (cs == QColorSpace::SRgb) return QStringLiteral("sRGB");
    QString desc = cs.description().trimmed();
    if (desc.isEmpty()) desc = QStringLiteral("ICC");
    return desc + QStringLiteral(" → 已转 sRGB");
}

// ── HEIF/AVIF：Exif 块与 ICC 都在 BMFF 的 box 表里，交给 libheif ──────────────
//   ★ 磁盘文件按**路径**打开（libheif 自己 seek，不读全文）；包内成员给整份字节
//   ★ Exif 载荷的前 4 字节是"TIFF 头在本块内的偏移"（HEIF 规范）；跳过它才是 `Exif\0\0…`
void readHeif(const Source &src, JpegParts *out)
{
    heif_context *ctx = heif_context_alloc();
    if (!ctx) return;
    const QByteArray path = src.path.toUtf8();
    const heif_error err = !src.path.isEmpty()
        ? heif_context_read_from_file(ctx, path.constData(), nullptr)
        : heif_context_read_from_memory_without_copy(
              ctx, src.bytes.constData(), size_t(src.bytes.size()), nullptr);
    if (err.code == heif_error_Ok) {
        heif_image_handle *handle = nullptr;
        if (heif_context_get_primary_image_handle(ctx, &handle).code == heif_error_Ok && handle) {
            heif_item_id id = 0;
            if (heif_image_handle_get_list_of_metadata_block_IDs(handle, "Exif", &id, 1) > 0) {
                const size_t size = heif_image_handle_get_metadata_size(handle, id);
                if (size > 4) {
                    QByteArray blob;
                    blob.resize(int(size));
                    if (heif_image_handle_get_metadata(handle, id, blob.data()).code == heif_error_Ok)
                        out->exif = blob.mid(4);
                }
            }
            const size_t iccSize = heif_image_handle_get_raw_color_profile_size(handle);
            if (iccSize > 0) {
                QByteArray icc;
                icc.resize(int(iccSize));
                if (heif_image_handle_get_raw_color_profile(handle, icc.data()).code == heif_error_Ok) {
                    out->icc = icc;
                    out->hasIcc = true;
                }
            }
            heif_image_handle_release(handle);
        }
    }
    heif_context_free(ctx);
}

}  // namespace

Info read(const Source &src, const QString &format)
{
    Info info;
    // 容器嗅探只看头部；包内成员的整份已在内存，直接从它切
    QByteArray head;
    if (!src.path.isEmpty()) {
        QFile f(src.path);
        if (f.open(QIODevice::ReadOnly)) head = f.read(kHeadBytes);
    } else {
        head = src.bytes.left(kHeadBytes);
    }

    const Container c = sniff(head);
    const QString fmt = format.toLower();

    JpegParts parts;
    QByteArray icc;
    bool hasIcc = false;
    switch (c) {
    case Container::Jpeg: parts = scanJpeg(head); break;
    case Container::Png:  scanPng(head, &parts);  break;
    case Container::WebP: scanWebp(head, &parts); break;
    case Container::Tiff: parts.exif = head; icc = tiffIcc(head); hasIcc = !icc.isEmpty(); break;
    case Container::Heif: readHeif(src, &parts); break;          // ★ BMFF：交给 libheif
    case Container::Unknown: break;
    }
    if (icc.isEmpty()) icc = parts.icc;
    hasIcc = hasIcc || parts.hasIcc;

    // 嗅探失败时退回扩展名提示（例如截断的头），交给 libexif 自己判断
    if (parts.exif.isEmpty() && !head.isEmpty() && fmt == QStringLiteral("tiff"))
        parts.exif = head;

    // libexif 只认带 `Exif\0\0` 前缀的那一种块（PNG eXIf / WebP EXIF / TIFF 都是裸 TIFF）
    QByteArray exifBlock = parts.exif;
    if (!exifBlock.isEmpty() && !exifBlock.startsWith("Exif\0\0"))
        exifBlock.prepend("Exif\0\0", 6);

    info.colorSpace = colorSpaceText(icc, hasIcc);

    const QVector<Row> rows = exifRowsFromBlock(exifBlock);
    info.total = rows.size();
    constexpr int kMaxRows = 24;      // 单幅最多 24 行（超出补"… 其余 N 条"）
    for (const Row &r : rows) {
        if (info.rows.size() >= kMaxRows) break;
        info.rows.append(qMakePair(r.key, r.value));
    }
    return info;
}

}  // namespace exifmeta
