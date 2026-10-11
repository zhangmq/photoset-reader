#include "settingsbridge.h"
#include "settings.h"
#include "imageprovider.h"

#include <QUrl>
#include <QFileInfo>
#include <QDir>
#include <QDebug>

SettingsBridge::SettingsBridge(ImageProvider *p, QObject *parent)
    : QObject(parent), m_p(p)
{
    rebuild();
}

QVariantList SettingsBridge::dirs() const
{
    QVariantList out;
    const auto list = Settings::instance().dirs();
    for (const Settings::DirEntry &e : list) {
        QVariantMap m;
        m["path"] = e.path;
        m["name"] = QFileInfo(e.path).fileName();
        m["recursive"] = e.recursive;
        // resume 提示：文件名（若有）
        QString hint;
        if (!e.resumeKey.isEmpty()) {
            hint = e.resumeKey.section(QLatin1Char('/'), -1);
            if (!e.resumeArchive.isEmpty())
                hint = e.resumeArchive + QStringLiteral(" · 第 %1 页").arg(e.resumePage + 1);
        }
        m["resumeHint"] = hint;
        out << m;
    }
    return out;
}

int SettingsBridge::slideshowIntervalMs() const
{
    return Settings::instance().slideshowIntervalMs();
}

void SettingsBridge::setSlideshowIntervalMs(int ms)
{
    Settings::instance().setSlideshowIntervalMs(ms);
    emit changed();
}

void SettingsBridge::openPath(const QString &url)
{
    const QString path = QUrl(url).isLocalFile() ? QUrl(url).toLocalFile() : url;
    const QFileInfo fi(path);
    if (!fi.exists()) {
        qWarning().noquote() << QStringLiteral("[bridge] 路径不存在: %1").arg(path);
        return;
    }
    Settings::instance().addOrTouchDir(path, false);
    rebuild();
    m_p->setSource(path, false);
    m_current = Settings::instance().indexOfDir(path);
    emit changed();
}

void SettingsBridge::activateDir(int index)
{
    const auto list = Settings::instance().dirs();
    if (index < 0 || index >= list.size()) return;
    const Settings::DirEntry e = list.at(index);
    m_p->setSource(e.path, e.recursive);
    m_current = index;
    emit changed();
}

void SettingsBridge::removeDir(const QString &path)
{
    Settings::instance().removeDir(path);
    rebuild();
    emit changed();
}

void SettingsBridge::setRecursive(bool recursive)
{
    if (m_current < 0) return;
    const auto list = Settings::instance().dirs();
    if (m_current >= list.size()) return;
    Settings::instance().setRecursive(list.at(m_current).path, recursive);
    m_p->setSource(list.at(m_current).path, recursive);
    emit changed();
}

void SettingsBridge::saveResume(int cursor, const QString &archiveName, int page)
{
    if (m_current < 0) return;
    const auto list = Settings::instance().dirs();
    if (m_current >= list.size()) return;
    const Settings::DirEntry e = list.at(m_current);
    const QString name = m_p->fileNameAt(cursor);
    QString key;
    if (m_p->isArchiveSource() || m_p->insideArchive()) {
        // ★ 压缩包内：e.path 是**压缩包文件本身**，不能拿它当根去算相对路径
        //   （那会拼出 "<rar>/<包内名>" 这种垃圾）。包内直接用**包内名**做键。
        key = name;
        key.replace(QLatin1Char('\\'), QLatin1Char('/'));
        key = key.toCaseFolded();
    } else {
        key = Settings::makeResumeKey(e.path, QDir(e.path).absoluteFilePath(name));
    }
    Settings::instance().updateResume(e.path, key, archiveName, page);
    emit changed();
}

void SettingsBridge::noteSource(const QString &path, bool recursive)
{
    // ★ 必须**登记**（addOrTouchDir），不能只查索引——
    //   否则通过命令行打开的目录在左侧栏里完全无记录（用户报告 #2）。
    Settings::instance().addOrTouchDir(path, recursive);
    m_current = Settings::instance().indexOfDir(path);
    rebuild();
}

int SettingsBridge::resumeIndexOf(const QString &path)
{
    const int di = Settings::instance().indexOfDir(path);
    if (di < 0) return -1;
    const Settings::DirEntry e = Settings::instance().dirs().at(di);
    if (e.resumeKey.isEmpty()) return -1;
    // resume 键 → 绝对路径 → 在当前序列里的索引
    const int n = m_p->count();
    if (m_p->isArchiveSource() || m_p->insideArchive()) {
        // 压缩包内：按键就是包内名
        for (int i = 0; i < n; ++i) {
            QString nm = m_p->fileNameAt(i);
            nm.replace(QLatin1Char('\\'), QLatin1Char('/'));
            if (nm.toCaseFolded() == e.resumeKey) return i;
        }
        return -1;
    }
    const QString abs = Settings::resolveResumeKey(e.path, e.resumeKey);
    if (abs.isEmpty()) return -1;
    for (int i = 0; i < n; ++i) {
        if (QFileInfo(m_p->absPathAt(i)).absoluteFilePath() == abs) return i;
    }
    return -1;
}

void SettingsBridge::rebuild()
{
    // dirs() 是即时读取，这里只需通知
}
