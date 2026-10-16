#include "appsettings.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace app {

// ⚠️ 这里**没有** `instance()` —— 设计 14 §9.3：设置**没有单例、没有静态状态**。
//    运行期实例挂在 `UiModel` 上；其他组件需要值一律调用时传参。

void AppSettings::setScalerParams(const QString &mode, const QString &quality, const QString &filter,
                                  int lobes, double b, double c, double antiring)
{
    m_scalerMode = mode;
    m_scalerQuality = quality;
    m_scalerFilter = filter;
    m_scalerLobes = qBound(2, lobes, 4);
    m_scalerMitchellB = b;
    m_scalerMitchellC = c;
    m_scalerAntiring = qBound(0.0, antiring, 1.0);
    save();                                  // 只写一次盘
}

QString AppSettings::filePath() const
{
    // 自检/测试可用环境变量指到临时文件，避免碰用户的真配置
    //   （与 `PHOTOSET_LIBRARY_DB` 同一约定，见 librarystore.cpp）
    const QByteArray override = qgetenv("PHOTOSET_SETTINGS_FILE");
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);
    // 开发期：PHOTOSET_CONFIG_DIR 指定目录；否则用应用名推导的 ~/.config/photoset-reader/
    const QByteArray dirOverride = qgetenv("PHOTOSET_CONFIG_DIR");
    if (!dirOverride.isEmpty())
        return QString::fromLocal8Bit(dirOverride) + QStringLiteral("/settings.json");
    const QByteArray fileOverride = qgetenv("PHOTOSET_SETTINGS_FILE");
    if (!fileOverride.isEmpty())
        return QString::fromLocal8Bit(fileOverride);
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return dir + QStringLiteral("/settings.json");
}

void AppSettings::load()
{
    QFile f(filePath());
    if (!f.exists())
        return;                                  // 首次运行：用默认值
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning().noquote() << QStringLiteral("[settings] 读不了 %1").arg(filePath());
        return;
    }
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    m_themeMode     = o.value(QStringLiteral("themeMode")).toInt(m_themeMode);
    m_intervalMs    = qMax(50, o.value(QStringLiteral("intervalMs")).toInt(m_intervalMs));
    m_fitMode       = o.value(QStringLiteral("defaultFitMode")).toInt(m_fitMode);
    m_scalerQuality = o.value(QStringLiteral("scalerQuality")).toString(m_scalerQuality);
    m_scalerMode    = o.value(QStringLiteral("scalerMode")).toString(m_scalerMode);
    m_scalerFilter  = o.value(QStringLiteral("scalerFilter")).toString(m_scalerFilter);
    m_scalerLobes   = o.value(QStringLiteral("scalerLobes")).toInt(m_scalerLobes);
    m_scalerMitchellB = o.value(QStringLiteral("scalerMitchellB")).toDouble(m_scalerMitchellB);
    m_scalerMitchellC = o.value(QStringLiteral("scalerMitchellC")).toDouble(m_scalerMitchellC);
    m_scalerAntiring  = o.value(QStringLiteral("scalerAntiring")).toDouble(m_scalerAntiring);
    m_scanHidden    = o.value(QStringLiteral("scanHidden")).toBool(m_scanHidden);
    m_recurseDepth  = qBound(1, o.value(QStringLiteral("recurseDepth")).toInt(m_recurseDepth), 64);
    m_prefetchWindow = qBound(3, o.value(QStringLiteral("prefetchWindow")).toInt(m_prefetchWindow), 64);
    m_cacheMb       = qMax(64, o.value(QStringLiteral("cacheMb")).toInt(m_cacheMb));
    m_innerArchiveMaxMb = qBound(64, o.value(QStringLiteral("innerArchiveMaxMb"))
                                         .toInt(m_innerArchiveMaxMb), 8192);
}

void AppSettings::save()
{
    const QString path = filePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QJsonObject o;
    o.insert(QStringLiteral("themeMode"), m_themeMode);
    o.insert(QStringLiteral("intervalMs"), m_intervalMs);
    o.insert(QStringLiteral("defaultFitMode"), m_fitMode);
    o.insert(QStringLiteral("scalerQuality"), m_scalerQuality);
    o.insert(QStringLiteral("scalerMode"), m_scalerMode);
    o.insert(QStringLiteral("scalerFilter"), m_scalerFilter);
    o.insert(QStringLiteral("scalerLobes"), m_scalerLobes);
    o.insert(QStringLiteral("scalerMitchellB"), m_scalerMitchellB);
    o.insert(QStringLiteral("scalerMitchellC"), m_scalerMitchellC);
    o.insert(QStringLiteral("scalerAntiring"), m_scalerAntiring);
    o.insert(QStringLiteral("scanHidden"), m_scanHidden);
    o.insert(QStringLiteral("recurseDepth"), m_recurseDepth);
    o.insert(QStringLiteral("prefetchWindow"), m_prefetchWindow);
    o.insert(QStringLiteral("prefetchBatch"), m_prefetchBatch);
    o.insert(QStringLiteral("cacheMb"), m_cacheMb);
    o.insert(QStringLiteral("innerArchiveMaxMb"), m_innerArchiveMaxMb);

    // 整文件全量写入（最后写的赢）
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning().noquote() << QStringLiteral("[settings] 写不了 %1").arg(path);
        return;
    }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
}

} // namespace app
