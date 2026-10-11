#pragma once

#include <QString>
#include <QVector>
#include <QJsonObject>
#include <QVariantMap>
#include <QHash>

// 持久化（设计 §八）。
//
// 存两样：
//   · **已打开目录列表**（顺序 = 用户添加顺序；打开即添加，除非主动移除）
//   · 每个目录的 **resume**：最后条目的相对路径 + 压缩包内页码
//
// resume 键的跨平台规范（Windows 相对路径硬限 MAX_PATH 260，见 08 号调研）：
//   · 分隔符统一归一化为正斜杠
//   · 大小写显式折叠（Windows 文件系统大小写不敏感）
//   · 加长度上限保护；超长时降级
//
// 离线铁律：本地文件，不联网。
class Settings
{
public:
    struct DirEntry {
        QString path;             // 绝对路径（目录或压缩包）
        bool recursive = false;   // 是否遍历子目录
        QString resumeKey;        // 相对路径，正斜杠归一化
        QString resumeArchive;    // 若停在压缩包内：该压缩包的文件名
        int resumePage = 0;       // 压缩包内页码
        qint64 lastUsed = 0;      // 毫秒时间戳
    };

    static Settings &instance();

    void load();
    void save();

    QVector<DirEntry> dirs() const { return m_dirs; }
    int indexOfDir(const QString &path) const;
    void addOrTouchDir(const QString &path, bool recursive);
    void removeDir(const QString &path);
    void setRecursive(const QString &path, bool recursive);
    void updateResume(const QString &path, const QString &resumeKey,
                      const QString &archive, int page);

    // 全局偏好
    int slideshowIntervalMs() const { return m_slideshowMs; }
    void setSlideshowIntervalMs(int ms) { m_slideshowMs = ms; save(); }
    QString quality() const { return m_quality; }
    void setQuality(const QString &q) { m_quality = q; save(); }
    bool advanceWholeDir() const { return m_advanceWholeDir; }
    void setAdvanceWholeDir(bool v) { m_advanceWholeDir = v; save(); }

    // 归一化一个相对路径为 resume 键
    static QString makeResumeKey(const QString &root, const QString &absolute);
    // 从 resume 键还原绝对路径（失败返回空）
    static QString resolveResumeKey(const QString &root, const QString &key);

    QString filePath() const;

private:
    Settings() = default;
    QVector<DirEntry> m_dirs;
    int m_slideshowMs = 300;          // 默认 300ms（设计 §4.6）
    QString m_quality = QStringLiteral("best");
    bool m_advanceWholeDir = true;
};
