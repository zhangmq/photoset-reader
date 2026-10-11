#pragma once

#include <QObject>
#include <QVariantList>

class ImageProvider;

// 把 Settings 暴露给 QML，并把"打开目录"接到 ImageProvider。
// 只做转发，不含业务逻辑（业务在 Settings 与 ImageProvider 里，各一份实现）。
class SettingsBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList dirs READ dirs NOTIFY changed)
    Q_PROPERTY(int currentDirIndex READ currentDirIndex NOTIFY changed)
    Q_PROPERTY(int slideshowIntervalMs READ slideshowIntervalMs
               WRITE setSlideshowIntervalMs NOTIFY changed)
public:
    explicit SettingsBridge(ImageProvider *p, QObject *parent = nullptr);

    QVariantList dirs() const;
    int currentDirIndex() const { return m_current; }
    int slideshowIntervalMs() const;

    // 打开即添加（用户要求 #1）；除非主动移除否则始终保留
    Q_INVOKABLE void openPath(const QString &url);
    Q_INVOKABLE void activateDir(int index);
    Q_INVOKABLE void removeDir(const QString &path);
    Q_INVOKABLE void setRecursive(bool recursive);
    Q_INVOKABLE void setSlideshowIntervalMs(int ms);
    // 记录当前游标（resume）
    Q_INVOKABLE void saveResume(int cursor, const QString &archiveName, int page);
    // 启动时登记当前来源（否则 m_current=-1，saveResume 会早退）
    Q_INVOKABLE void noteSource(const QString &path, bool recursive = false);
    // 把 resume 键解析回条目索引（-1 = 无）
    Q_INVOKABLE int resumeIndexOf(const QString &path);

signals:
    void changed();

private:
    void rebuild();
    ImageProvider *m_p;
    int m_current = -1;
};
