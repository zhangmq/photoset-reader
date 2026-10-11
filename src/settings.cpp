#include "settings.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDateTime>
#include <QDebug>

namespace {
constexpr int kMaxKeyLen = 200;      // resume 键长度上限（Windows 相对路径硬限）
}

Settings &Settings::instance()
{
    static Settings s;
    return s;
}

QString Settings::filePath() const
{
    // 允许用环境变量覆盖（开发/沙箱用；生产走标准位置）
    const QByteArray override = qgetenv("IMAGE_BROWSER_STATE");
    if (!override.isEmpty()) return QString::fromLocal8Bit(override);

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/state.json");
}

void Settings::load()
{
    QFile f(filePath());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();

    m_dirs.clear();
    for (const QJsonValue &v : root.value(QStringLiteral("dirs")).toArray()) {
        const QJsonObject o = v.toObject();
        DirEntry e;
        e.path = o.value(QStringLiteral("path")).toString();
        e.recursive = o.value(QStringLiteral("recursive")).toBool();
        e.resumeKey = o.value(QStringLiteral("resumeKey")).toString();
        e.resumeArchive = o.value(QStringLiteral("resumeArchive")).toString();
        e.resumePage = o.value(QStringLiteral("resumePage")).toInt();
        e.lastUsed = qint64(o.value(QStringLiteral("lastUsed")).toDouble());
        if (!e.path.isEmpty()) m_dirs << e;
    }
    m_slideshowMs = root.value(QStringLiteral("slideshowMs")).toInt(300);
    m_quality = root.value(QStringLiteral("quality")).toString(QStringLiteral("best"));
    m_advanceWholeDir = root.value(QStringLiteral("advanceWholeDir")).toBool(true);

    qInfo().noquote() << QStringLiteral("[settings] 载入 %1 个目录  (%2)")
                             .arg(m_dirs.size()).arg(filePath());
}

void Settings::save()
{
    QJsonArray arr;
    for (const DirEntry &e : m_dirs) {
        QJsonObject o;
        o[QStringLiteral("path")] = e.path;
        o[QStringLiteral("recursive")] = e.recursive;
        o[QStringLiteral("resumeKey")] = e.resumeKey;
        o[QStringLiteral("resumeArchive")] = e.resumeArchive;
        o[QStringLiteral("resumePage")] = e.resumePage;
        o[QStringLiteral("lastUsed")] = double(e.lastUsed);
        arr.append(o);
    }
    QJsonObject root;
    root[QStringLiteral("dirs")] = arr;
    root[QStringLiteral("slideshowMs")] = m_slideshowMs;
    root[QStringLiteral("quality")] = m_quality;
    root[QStringLiteral("advanceWholeDir")] = m_advanceWholeDir;


    QFile f(filePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning().noquote() << QStringLiteral("[settings] 无法写入 %1").arg(filePath());
        return;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

int Settings::indexOfDir(const QString &path) const
{
    const QString canon = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < m_dirs.size(); ++i)
        if (m_dirs[i].path == canon) return i;
    return -1;
}

void Settings::addOrTouchDir(const QString &path, bool recursive)
{
    const QString canon = QFileInfo(path).absoluteFilePath();
    const int i = indexOfDir(canon);
    if (i >= 0) {
        m_dirs[i].recursive = recursive;
        m_dirs[i].lastUsed = QDateTime::currentMSecsSinceEpoch();
    } else {
        DirEntry e;
        e.path = canon;
        e.recursive = recursive;
        e.lastUsed = QDateTime::currentMSecsSinceEpoch();
        m_dirs << e;
    }
    save();
}

void Settings::removeDir(const QString &path)
{
    const int i = indexOfDir(path);
    if (i < 0) return;
    m_dirs.remove(i);
    save();
}

void Settings::setRecursive(const QString &path, bool recursive)
{
    const int i = indexOfDir(path);
    if (i < 0) return;
    m_dirs[i].recursive = recursive;
    save();
}

void Settings::updateResume(const QString &path, const QString &resumeKey,
                            const QString &archive, int page)
{
    const int i = indexOfDir(path);
    if (i < 0) return;
    m_dirs[i].resumeKey = resumeKey;
    m_dirs[i].resumeArchive = archive;
    m_dirs[i].resumePage = page;
    m_dirs[i].lastUsed = QDateTime::currentMSecsSinceEpoch();
    save();
}

// 归一化：正斜杠 + 大小写折叠（Windows 文件系统大小写不敏感）+ 长度上限
QString Settings::makeResumeKey(const QString &root, const QString &absolute)
{
    QString rel = QDir(root).relativeFilePath(absolute);
    rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
    rel = rel.toCaseFolded();
    if (rel.size() > kMaxKeyLen) {
        // 超长降级：只保留文件名 + 上级目录名
        const QStringList parts = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.size() >= 2)
            rel = parts.at(parts.size() - 2) + QLatin1Char('/') + parts.last();
        else if (!parts.isEmpty())
            rel = parts.last();
    }
    return rel;
}

QString Settings::resolveResumeKey(const QString &root, const QString &key)
{
    if (key.isEmpty()) return QString();
    // 先按原样
    QString p = QDir(root).absoluteFilePath(key);
    if (QFileInfo::exists(p)) return p;
    // 大小写折叠后逐段匹配（Linux 区分大小写，Windows 不区分）
    const QStringList parts = key.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString cur = root;
    for (const QString &seg : parts) {
        QDir d(cur);
        const QStringList entries = d.entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::NoSort);
        QString found;
        for (const QString &e : entries)
            if (e.toCaseFolded() == seg) { found = e; break; }
        if (found.isEmpty()) return QString();
        cur = d.absoluteFilePath(found);
    }
    return QFileInfo::exists(cur) ? cur : QString();
}
