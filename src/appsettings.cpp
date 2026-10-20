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
    m_layout        = qBound(0, o.value(QStringLiteral("defaultLayout")).toInt(m_layout), 1);
    m_loop          = qBound(0, o.value(QStringLiteral("defaultLoop")).toInt(m_loop), 2);
    // 缩放算法：新键优先；没有就按旧键迁移（旧版是 mode/quality/filter/lobes/... 七个字段）
    if (o.contains(QStringLiteral("scaleAlgorithm"))) {
        m_scaleAlgorithm = o.value(QStringLiteral("scaleAlgorithm")).toString();
    } else {
        const QString oldMode = o.value(QStringLiteral("scalerMode")).toString();
        if (oldMode == QLatin1String("nearest"))
            m_scaleAlgorithm = QStringLiteral("nearest");
        else if (oldMode == QLatin1String("qt-smooth"))
            m_scaleAlgorithm = QStringLiteral("smooth");
        else
            m_scaleAlgorithm = QStringLiteral("lanczos3");   // 其余一律按默认（原来的 balanced/auto）
    }
    m_scanHidden    = o.value(QStringLiteral("scanHidden")).toBool(m_scanHidden);
    m_recurseDepth  = qBound(1, o.value(QStringLiteral("recurseDepth")).toInt(m_recurseDepth), 64);
    m_prefetchWindow = qBound(3, o.value(QStringLiteral("prefetchWindow")).toInt(m_prefetchWindow), 64);
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
    o.insert(QStringLiteral("defaultLayout"), m_layout);
    o.insert(QStringLiteral("defaultLoop"), m_loop);
    o.insert(QStringLiteral("scaleAlgorithm"), m_scaleAlgorithm);
    o.insert(QStringLiteral("scanHidden"), m_scanHidden);
    o.insert(QStringLiteral("recurseDepth"), m_recurseDepth);
    o.insert(QStringLiteral("prefetchWindow"), m_prefetchWindow);
    o.insert(QStringLiteral("prefetchBatch"), m_prefetchBatch);
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
