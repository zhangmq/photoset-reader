#pragma once

#include "appsettings.h"

#include <QObject>

namespace app {

// ══ 设置对话框的**草稿副本**（用户 2026-11-06 定）══════════════════════════
//   对话框打开时把当前设置**拷一份**进来，所有编辑只改这份拷贝：
//   · 确定 ⇒ `UiModel::applySettingsEdit()` 把拷贝应用成生效设置（并整份写盘）；
//   · 取消 ⇒ 什么都不做（生效设置从头到尾没被碰过，所以没有"回滚"这回事）。
//
//   ★ 为什么不直接读写 `UiModel`：那是**正在生效**的配置，边拖边生效既雾里看花，
//     又让"取消"变得无意义（改了就得想办法还原）。拷贝模型把这两件事分开。
class SettingsDraft : public QObject
{
    Q_OBJECT

    Q_PROPERTY(int themeMode READ themeMode WRITE setThemeMode NOTIFY changed)
    Q_PROPERTY(int slideshowIntervalMs READ slideshowIntervalMs WRITE setSlideshowIntervalMs NOTIFY changed)
    Q_PROPERTY(int fitMode READ fitMode WRITE setFitMode NOTIFY changed)
    Q_PROPERTY(int defaultLayout READ defaultLayout WRITE setDefaultLayout NOTIFY changed)
    Q_PROPERTY(int defaultLoop READ defaultLoop WRITE setDefaultLoop NOTIFY changed)
    Q_PROPERTY(QString scaleAlgorithm READ scaleAlgorithm WRITE setScaleAlgorithm NOTIFY changed)
    Q_PROPERTY(bool scanHidden READ scanHidden WRITE setScanHidden NOTIFY changed)
    Q_PROPERTY(bool previewStrip READ previewStrip WRITE setPreviewStrip NOTIFY changed)
    Q_PROPERTY(QString logLevel READ logLevel WRITE setLogLevel NOTIFY changed)
    Q_PROPERTY(int recurseDepth READ recurseDepth WRITE setRecurseDepth NOTIFY changed)
    Q_PROPERTY(int prefetchWindow READ prefetchWindow WRITE setPrefetchWindow NOTIFY changed)
    Q_PROPERTY(int innerArchiveMaxMb READ innerArchiveMaxMb WRITE setInnerArchiveMaxMb NOTIFY changed)
    Q_PROPERTY(QString favoritesDir READ favoritesDir WRITE setFavoritesDir NOTIFY changed)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY changed)

public:
    explicit SettingsDraft(QObject *parent = nullptr);

    void loadFrom(const AppSettings &s);        // 对话框打开：拿一份拷贝
    AppSettings values() const { return m_settings; }
    Q_INVOKABLE void resetToDefaults();         // 「恢复默认」：只改草稿，仍要点确定

    int themeMode() const { return m_settings.themeMode(); }
    int slideshowIntervalMs() const { return m_settings.intervalMs(); }
    int fitMode() const { return m_settings.defaultFitMode(); }
    int defaultLayout() const { return m_settings.defaultLayout(); }
    int defaultLoop() const { return m_settings.defaultLoop(); }
    QString scaleAlgorithm() const { return m_settings.scaleAlgorithm(); }
    bool scanHidden() const { return m_settings.scanHidden(); }
    bool previewStrip() const { return m_settings.previewStrip(); }
    QString logLevel() const { return m_settings.logLevel(); }
    int recurseDepth() const { return m_settings.recurseDepth(); }
    int prefetchWindow() const { return m_settings.prefetchWindow(); }
    int innerArchiveMaxMb() const { return m_settings.innerArchiveMaxMb(); }
    QString favoritesDir() const { return m_settings.favoritesDir(); }
    QString language() const { return m_settings.language(); }

    void setThemeMode(int v) { m_settings.setThemeMode(v); emit changed(); }
    void setSlideshowIntervalMs(int v) { m_settings.setIntervalMs(v); emit changed(); }
    void setFitMode(int v) { m_settings.setDefaultFitMode(v); emit changed(); }
    void setDefaultLayout(int v) { m_settings.setDefaultLayout(v); emit changed(); }
    void setDefaultLoop(int v) { m_settings.setDefaultLoop(v); emit changed(); }
    void setScaleAlgorithm(const QString &v) { m_settings.setScaleAlgorithm(v); emit changed(); }
    void setScanHidden(bool v) { m_settings.setScanHidden(v); emit changed(); }
    void setPreviewStrip(bool v) { m_settings.setPreviewStrip(v); emit changed(); }
    void setLogLevel(const QString &v) { m_settings.setLogLevel(v); emit changed(); }
    void setRecurseDepth(int v) { m_settings.setRecurseDepth(v); emit changed(); }
    void setPrefetchWindow(int v) { m_settings.setPrefetchWindow(v); emit changed(); }
    void setInnerArchiveMaxMb(int v) { m_settings.setInnerArchiveMaxMb(v); emit changed(); }
    void setFavoritesDir(const QString &v) { m_settings.setFavoritesDir(v); emit changed(); }
    void setLanguage(const QString &v) { m_settings.setLanguage(v); emit changed(); }

signals:
    void changed();

private:
    AppSettings m_settings;
};

}   // namespace app
