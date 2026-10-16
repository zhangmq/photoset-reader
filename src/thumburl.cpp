#include "thumburl.h"

#include <QFileInfo>
#include <QUrl>

// 缩略图 URL 的唯一实现（设计 11 §5.2）—— `ImageProvider` 解析它，浏览界面拼它。

static QString prefix()
{
    return QStringLiteral("image://") + QLatin1String(kThumbProviderName) + QLatin1Char('/');
}

QString thumbUrlForFile(const QString &absPath)
{
    return prefix() + QStringLiteral("thumbpath/")
           + thumbPctEncode(QFileInfo(absPath).absoluteFilePath());
}

QString thumbUrlForArchiveMember(const QString &archivePath, const QString &member)
{
    QString id = QStringLiteral("thumbzip/")
                 + thumbPctEncode(QFileInfo(archivePath).absoluteFilePath());
    if (!member.isEmpty()) id += QLatin1Char('/') + thumbPctEncode(member);
    return prefix() + id;
}
