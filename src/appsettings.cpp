#include "appsettings.h"
#include "logging.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace app {

// ⚠️ 这里**没有** `instance()` —— 设计 14 §9.3：设置**没有单例、没有静态状态**。
//    运行期实例挂在 `UiModel` 上；其他组件需要值一律调用时传参。

QString AppSettings::filePath() const
{
    //   路径一律走标准位置（`~/.config/photoset-reader/`）；测试隔离用标准的
    //   `XDG_CONFIG_HOME`（`QStandardPaths` 自己认），产品不再有私有 env 后门（用户 2026-11-04）。
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return dir + QStringLiteral("/settings.json");
}

//   解析错误 → 稳定英文 token（不用 `errorString()`：那是 Qt 的本地化文案，日志要英文）
static const char *jsonErrorName(QJsonParseError::ParseError e)
{
    switch (e) {
    case QJsonParseError::NoError:            return "no_error";
    case QJsonParseError::UnterminatedObject: return "unterminated_object";
    case QJsonParseError::MissingNameSeparator: return "missing_name_separator";
    case QJsonParseError::UnterminatedArray:  return "unterminated_array";
    case QJsonParseError::MissingValueSeparator: return "missing_value_separator";
    case QJsonParseError::IllegalValue:       return "illegal_value";
    case QJsonParseError::TerminationByNumber: return "termination_by_number";
    case QJsonParseError::IllegalNumber:      return "illegal_number";
    case QJsonParseError::IllegalEscapeSequence: return "illegal_escape_sequence";
    case QJsonParseError::IllegalUTF8String:  return "illegal_utf8_string";
    case QJsonParseError::UnterminatedString: return "unterminated_string";
    case QJsonParseError::MissingObject:      return "missing_object";
    case QJsonParseError::DeepNesting:        return "deep_nesting";
    case QJsonParseError::DocumentTooLarge:   return "document_too_large";
    case QJsonParseError::GarbageAtEnd:       return "garbage_at_end";
    }
    return "unknown";
}

void AppSettings::load()
{
    const QString path = filePath();
    QFile f(path);
    if (!f.exists())
        return;                                  // 首次运行：用默认值
    if (!f.open(QIODevice::ReadOnly)) {
        qCWarning(lcApp).noquote() << QStringLiteral("[app] settings read failed: %1").arg(path);
        return;
    }
    const QByteArray raw = f.readAll();
    f.close();

    //   ★ 坏文件容错：JSON 解析失败 ⇒ 备份成 `.broken`（保留现场给用户/我们看），再用默认值。
    //     不这样做的话，用户唯一的症状是"设置全没了"，而且下次保存会把坏文件直接覆盖掉。
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        const QString broken = path + QStringLiteral(".broken");
        QFile::remove(broken);
        const bool kept = QFile::rename(path, broken);
        qCWarning(lcApp).noquote()
            << QStringLiteral("[app] settings are not valid JSON (error=%1, offset=%2); "
                              "backup kept: %3 (%4)")
                   .arg(jsonErrorName(perr.error))
                   .arg(perr.offset)
                   .arg(broken)
                   .arg(kept ? QStringLiteral("ok") : QStringLiteral("rename failed"));
        return;                                  // 用默认值继续跑
    }
    const QJsonObject o = doc.object();

    //   格式版本：缺 = v0（老文件，下面的 key 都按"缺就用默认"读）；比我们新 ⇒ 只警告
    const int schema = o.value(QStringLiteral("schemaVersion")).toInt(0);
    if (schema > kSchemaVersion) {
        qCWarning(lcApp).noquote()
            << QStringLiteral("[app] settings schema v%1 is newer than v%2; unknown keys are ignored")
                   .arg(schema)
                   .arg(kSchemaVersion);
    }
    m_themeMode     = o.value(QStringLiteral("themeMode")).toInt(m_themeMode);
    //   ★ 下限走**唯一事实源**（曾经在这里硬写 50 ⇒ 存了 0 读回来变 50）
    m_intervalMs    = qBound(kMinSlideshowIntervalMs,
                             o.value(QStringLiteral("intervalMs")).toInt(m_intervalMs),
                             kMaxSlideshowIntervalMs);
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
    m_previewStrip  = o.value(QStringLiteral("previewStrip")).toBool(m_previewStrip);
    m_logLevel      = o.value(QStringLiteral("logLevel")).toString(m_logLevel);
    m_recurseDepth  = qBound(1, o.value(QStringLiteral("recurseDepth")).toInt(m_recurseDepth), 64);
    m_prefetchWindow = qBound(3, o.value(QStringLiteral("prefetchWindow")).toInt(m_prefetchWindow), 64);
    m_innerArchiveMaxMb = qBound(64, o.value(QStringLiteral("innerArchiveMaxMb"))
                                         .toInt(m_innerArchiveMaxMb), 8192);
    m_favoritesDir  = o.value(QStringLiteral("favoritesDir")).toString();
    m_language      = o.value(QStringLiteral("language")).toString();
}

void AppSettings::save()
{
    const QString path = filePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QJsonObject o;
    o.insert(QStringLiteral("schemaVersion"), kSchemaVersion);
    o.insert(QStringLiteral("themeMode"), m_themeMode);
    o.insert(QStringLiteral("intervalMs"), m_intervalMs);
    o.insert(QStringLiteral("defaultFitMode"), m_fitMode);
    o.insert(QStringLiteral("defaultLayout"), m_layout);
    o.insert(QStringLiteral("defaultLoop"), m_loop);
    o.insert(QStringLiteral("scaleAlgorithm"), m_scaleAlgorithm);
    o.insert(QStringLiteral("scanHidden"), m_scanHidden);
    o.insert(QStringLiteral("previewStrip"), m_previewStrip);
    o.insert(QStringLiteral("logLevel"), m_logLevel);
    o.insert(QStringLiteral("recurseDepth"), m_recurseDepth);
    o.insert(QStringLiteral("prefetchWindow"), m_prefetchWindow);
    o.insert(QStringLiteral("innerArchiveMaxMb"), m_innerArchiveMaxMb);
    o.insert(QStringLiteral("favoritesDir"), m_favoritesDir);
    o.insert(QStringLiteral("language"), m_language);

    //   整文件全量写入（最后写的赢），但**原子**写：`QSaveFile` 先写临时文件再 rename
    //   —— 进程被 kill / 断电时不会留下半截 JSON（那种文件下次启动只能当"坏文件"处理）。
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        qCWarning(lcApp).noquote() << QStringLiteral("[app] settings write failed: %1").arg(path);
        return;
    }
    const QByteArray data = QJsonDocument(o).toJson(QJsonDocument::Indented);
    if (f.write(data) != data.size() || !f.commit()) {
        qCWarning(lcApp).noquote() << QStringLiteral("[app] settings write failed: %1").arg(path);
    }
}

} // namespace app
