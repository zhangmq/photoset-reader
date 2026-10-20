#include "exifmeta.h"

#include <QColorSpace>
#include <QHash>
#include <QSet>

#include <exiv2/exiv2.hpp>

#include <algorithm>

// ══ 分层：Exiv2 负责"容器 + 标签 + 格式化 + ICC"，我们只留"怎么说给人听" ═══════
//   · 容器（JPEG 段 / PNG 块 / WebP 块 / TIFF IFD / HEIF·AVIF·CR3·JP2·JXL 的 box 树）
//     一律交给 Exiv2 —— 手写 216 行只认得前四种，HEIC 的 EXIF 组整块消失（用户 2026-10-21）；
//   · 中文名、优先级、结构噪声过滤、上限 —— Exiv2 只给英文 key，这层是我们自己的；
//   · 色彩空间判词与解码点 `toSrgb()` 共用 `QColorSpace::fromIccProfile` ⇒ 说的 = 做的。
namespace exifmeta {

namespace {

// ══ 标签 → 中文名 / 排序权重 ═════════════════════════════════════════════════
//   键用 Exiv2 的稳定 key（`Exif.Image.Make` / `Exif.Photo.DateTimeOriginal`）
struct TagSpec {
    const char *key;
    const char *label;
    int         prio;      // 越小越靠前；未列出 = 按文件原序排在后面
};

const TagSpec kSpecs[] = {
    { "Exif.Image.Make",                  "相机厂商",  1 },
    { "Exif.Image.Model",                 "机型",      2 },
    { "Exif.Photo.LensModel",             "镜头",      3 },
    { "Exif.Photo.DateTimeOriginal",      "拍摄时间",  4 },
    { "Exif.Photo.ExposureTime",          "曝光",      5 },
    { "Exif.Photo.FNumber",               "光圈",      6 },
    { "Exif.Photo.ISOSpeedRatings",       "ISO",       7 },
    { "Exif.Photo.FocalLength",           "焦距",      8 },
    { "Exif.Photo.FocalLengthIn35mmFilm", "等效焦距",  9 },
    { "Exif.Photo.ExposureBiasValue",     "曝光补偿", 10 },
    { "Exif.Photo.MaxApertureValue",      "最大光圈", 11 },
    { "Exif.Photo.MeteringMode",          "测光",     12 },
    { "Exif.Photo.Flash",                 "闪光灯",   13 },
    { "Exif.Photo.LightSource",           "光源",     14 },
    { "Exif.Photo.WhiteBalance",          "白平衡",   15 },
    { "Exif.Photo.ExposureProgram",       "曝光程序", 16 },
    { "Exif.Image.DateTime",              "修改时间", 17 },
    { "Exif.Image.Software",              "软件",     20 },
    { "Exif.Image.Artist",                "作者",     21 },
    { "Exif.Image.Copyright",             "版权",     22 },
    { "Exif.Image.ImageDescription",      "描述",     23 },
    { "Exif.Photo.LensMake",              "镜头厂商", 24 },
    { "Exif.Photo.SubjectDistance",       "拍摄距离", 25 },
    { "Exif.Photo.DigitalZoomRatio",      "数码变焦", 26 },
    { "Exif.Photo.SceneCaptureType",      "场景",     27 },
    { "Exif.Photo.Saturation",            "饱和度",   28 },
    { "Exif.Photo.Contrast",              "对比度",   29 },
    { "Exif.Photo.Sharpness",             "锐度",     30 },
};

const QHash<QString, const TagSpec *> &specMap()
{
    static const QHash<QString, const TagSpec *> m = [] {
        QHash<QString, const TagSpec *> h;
        for (const TagSpec &s : kSpecs) h.insert(QString::fromLatin1(s.key), &s);
        return h;
    }();
    return m;
}

// 结构 / 常量类标签：对"看图的人"没有信息量，且会盖掉真正生效的结论
bool isNoise(const QString &key)
{
    if (key.startsWith(QLatin1String("Exif.Thumbnail."))) return true;   // 缩略图 IFD 整组
    static const QSet<QString> kNoise = {
        QStringLiteral("Exif.Photo.ExifVersion"),
        QStringLiteral("Exif.Photo.FlashpixVersion"),
        QStringLiteral("Exif.Photo.ComponentsConfiguration"),
        QStringLiteral("Exif.Photo.MakerNote"),
        QStringLiteral("Exif.Photo.UserComment"),
        QStringLiteral("Exif.Photo.ColorSpace"),      // 常写 Uncalibrated，与真正生效的 ICC 冲突
        QStringLiteral("Exif.Photo.InterColorProfile"),
        QStringLiteral("Exif.Photo.PixelXDimension"), // 像素尺寸已在"分辨率"行
        QStringLiteral("Exif.Photo.PixelYDimension"),
        QStringLiteral("Exif.Image.XResolution"),     // DPI 与屏幕显示无关
        QStringLiteral("Exif.Image.YResolution"),
        QStringLiteral("Exif.Image.ResolutionUnit"),
        QStringLiteral("Exif.Image.Orientation"),     // 效果已体现在"分辨率"行
        QStringLiteral("Exif.Image.InterColorProfile"),
        QStringLiteral("Exif.Image.ExifTag"),         // 只是 IFD 偏移
        QStringLiteral("Exif.Image.GPSTag"),          // 同上（Exiv2 比 libexif 多暴露指针类标签）
        QStringLiteral("Exif.Image.InteroperabilityTag"),
        QStringLiteral("Exif.GPSInfo.GPSVersionID"),
        QStringLiteral("Exif.GPSInfo.GPSLatitudeRef"),    // 只是方向标记，值已在经纬度里
        QStringLiteral("Exif.GPSInfo.GPSLongitudeRef"),
        QStringLiteral("Exif.GPSInfo.GPSAltitudeRef"),
    };
    return kNoise.contains(key);
}

// GPS 有独立的中文名（词条与其它 IFD 无关，按 Exiv2 的完整 key 区分）
QString gpsLabel(const QString &key)
{
    if (key == QLatin1String("Exif.GPSInfo.GPSLatitude"))  return QStringLiteral("纬度");
    if (key == QLatin1String("Exif.GPSInfo.GPSLongitude")) return QStringLiteral("经度");
    if (key == QLatin1String("Exif.GPSInfo.GPSAltitude"))  return QStringLiteral("海拔");
    if (key == QLatin1String("Exif.GPSInfo.GPSTimeStamp")) return QStringLiteral("GPS 时间");
    if (key == QLatin1String("Exif.GPSInfo.GPSDateStamp")) return QStringLiteral("GPS 日期");
    return {};
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

struct Row { QString key; QString value; int prio; int seq; };

}  // namespace

Info read(const Source &src)
{
    // Exiv2 默认把坏文件的 Warning 写 stderr；本项目要求"跑起来除了 qml: 没有多余输出"。
    // 解析失败本来也只体现为"这组行不出现"，所以静音，不丢信息。
    static const bool kQuiet = [] {
        Exiv2::LogMsg::setLevel(Exiv2::LogMsg::mute);
        return true;
    }();
    Q_UNUSED(kQuiet)

    Info info;
    info.colorSpace = QStringLiteral("sRGB（无 profile）");

    Exiv2::Image::UniquePtr img;
    try {
        if (!src.path.isEmpty()) {
            // ★ 磁盘文件走**路径**：Exiv2 自己 seek，只读元数据那一截（14 MB 的 HEIC 读 20 KB）
            const QByteArray path = src.path.toUtf8();
            img = Exiv2::ImageFactory::open(path.constData());
        } else if (!src.bytes.isEmpty()) {
            // 包内成员：整份已在内存（`Probe::memberBytes`），零拷贝借用
            img = Exiv2::ImageFactory::open(
                reinterpret_cast<const Exiv2::byte *>(src.bytes.constData()),
                size_t(src.bytes.size()));
        }
        if (!img) return info;
        img->readMetadata();
    } catch (const Exiv2::Error &) {
        return info;      // 格式不认识 / 数据损坏：整组不出现，判词按无 profile
    }

    QByteArray icc;
    if (img->iccProfileDefined()) {
        const Exiv2::DataBuf &buf = img->iccProfile();
        icc = QByteArray(reinterpret_cast<const char *>(buf.c_data()), int(buf.size()));
    }
    info.colorSpace = colorSpaceText(icc, !icc.isEmpty());

    QVector<Row> rows;
    int seq = 0;
    for (const Exiv2::Exifdatum &d : img->exifData()) {
        const QString key = QString::fromLatin1(d.key().c_str());
        if (isNoise(key)) continue;
        QString value;
        try {
            value = QString::fromStdString(d.print()).trimmed();
        } catch (const Exiv2::Error &) {
            continue;                       // 个别标签的类型不被支持 ⇒ 只跳过它
        }
        if (value.isEmpty()) continue;

        QString label;
        int prio = 1000;
        if (const TagSpec *s = specMap().value(key, nullptr)) {
            label = QString::fromUtf8(s->label);
            prio = s->prio;
        } else {
            label = gpsLabel(key);
            if (label.isEmpty()) label = QString::fromStdString(d.tagLabel());
            if (label.isEmpty()) label = QString::fromStdString(d.tagName());
        }
        if (label.isEmpty() || label.startsWith(QLatin1String("0x"))) continue;   // 词典外的裸 tag
        rows.push_back({ label, value, prio, seq++ });
    }

    // 常用项按固定优先级在前，其余保持文件原序
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.prio != b.prio) return a.prio < b.prio;
        return a.seq < b.seq;
    });

    info.total = rows.size();
    constexpr int kMaxRows = 24;          // 单幅最多 24 行（超出补"… 其余 N 条"）
    for (const Row &r : rows) {
        if (info.rows.size() >= kMaxRows) break;
        info.rows.append(qMakePair(r.key, r.value));
    }
    return info;
}

}  // namespace exifmeta
