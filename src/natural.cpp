#include "natural.h"

#include <QSet>

int naturalCompare(const QString &a, const QString &b)
{
    int i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].isDigit() && b[j].isDigit()) {
            int i2 = i, j2 = j;
            while (i2 < a.size() && a[i2].isDigit()) ++i2;
            while (j2 < b.size() && b[j2].isDigit()) ++j2;
            // 跳过前导零（007 与 7 同值）
            int i3 = i, j3 = j;
            while (i3 < i2 - 1 && a[i3] == QLatin1Char('0')) ++i3;
            while (j3 < j2 - 1 && b[j3] == QLatin1Char('0')) ++j3;
            const int la = i2 - i3, lb = j2 - j3;
            if (la != lb) return la < lb ? -1 : 1;
            for (int k = 0; k < la; ++k)
                if (a[i3 + k] != b[j3 + k]) return a[i3 + k] < b[j3 + k] ? -1 : 1;
            i = i2; j = j2;
        } else {
            const QChar ca = a[i].toLower(), cb = b[j].toLower();
            if (ca != cb) return ca < cb ? -1 : 1;
            ++i; ++j;
        }
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

bool isImageSuffix(const QString &suffix)
{
    static const QSet<QString> exts = {
        QStringLiteral("jpg"),  QStringLiteral("jpeg"), QStringLiteral("png"),
        QStringLiteral("gif"),  QStringLiteral("bmp"),  QStringLiteral("webp"),
        QStringLiteral("tif"),  QStringLiteral("tiff"), QStringLiteral("ico"),
        QStringLiteral("tga"),  QStringLiteral("svg"),  QStringLiteral("avif"),
        QStringLiteral("jxl"),  QStringLiteral("heic"), QStringLiteral("heif"),
        QStringLiteral("qoi"),  QStringLiteral("pnm"),  QStringLiteral("pgm"),
        QStringLiteral("ppm"),  QStringLiteral("pbm"),
    };
    return exts.contains(suffix.toLower());
}

QString nameSortKey(const QString &name, bool isDir)
{
    return isDir ? name + QLatin1Char('/') : name;
}
