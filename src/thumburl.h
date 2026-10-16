#pragma once

#include <QString>
#include <QUrl>

// ══ 缩略图 URL（**格式的唯一来源**，设计 11 §5.2）═══════════════════════
//
//   规范形式（`img` = provider 的注册名，见 `kThumbProviderName`）：
//
//     image://img/thumbpath/<percent(绝对路径)>                       普通图片文件
//     image://img/thumbzip/<percent(压缩包绝对路径)>                   包内**第 0 页**
//     image://img/thumbzip/<percent(压缩包绝对路径)>/<percent(成员名)>  包内**指定成员**
//
//   · 路径里的 `/` 必须编码成 `%2F`（`thumbzip` 的分隔符是**第一个未编码的 `/`**）；
//   · **不看内容、不打开压缩包**：解析只 stat 文件 ⇒ 便宜。
//
//   ★ 谁都用这一份：`ImageProvider`（解析）与浏览界面（拼 URL）——
//     避免"格式在两处各写一遍"（算法单一事实源）。
inline constexpr const char *kThumbProviderName = "img";

// 百分号编码 / 解码（URL 里带路径，必须编）
inline QString thumbPctEncode(const QString &s)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(s));
}
inline QString thumbPctDecode(const QString &s)
{
    return QUrl::fromPercentEncoding(s.toUtf8());
}

// 普通图片文件 ⇒ 缩略图 URL
QString thumbUrlForFile(const QString &absPath);
// 压缩包内成员 ⇒ 缩略图 URL（member 为空 = 包内第 0 页）
QString thumbUrlForArchiveMember(const QString &archivePath, const QString &member = QString());
