#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QByteArray>
#include <QSize>
#include <QMutex>

// 压缩包读取（**一个接口，两个后端**，按格式分工）：
//   · ZIP / CBZ / 7z / tar / ...  →  libarchive（BSD-2）
//   · RAR / CBR（含 RAR5 / 大字典 / AES 加密）→  libunrar（官方参考实现）
//   理由见 AGENTS.md「依赖策略」：libarchive 硬限 RAR5 字典 ≤64MB 且未实现 RAR5 AES，
//   实测本项目素材（-md=256m + AES）两者都不满足；unarr 只有 RAR4（issue #3 仍 open）。
//
// 设计要点（见 AGENTS.md「依赖策略」与设计 §2.6 第 5 点）：
//   · 压缩包在**目录层级上算一个条目**，内部页构成第二级序列
//   · 列目录便宜（读中央目录 / 文件头）
//   · **随机访问不保证**：solid 归档（如本项目的测试 RAR5）只能顺序解压，
//     所以取第 N 页可能要从头扫。v1 先做正确，性能优化后置
//   · 所有 I/O 容忍失败：返回空，不崩溃
class ArchiveSource
{
public:
    // 按扩展名判断是否压缩包
    static bool isArchive(const QString &path);

    ~ArchiveSource();

    // 打开并列出条目。password 为空表示不加密。
    bool open(const QString &path, const QString &password = QString());
    void close();

    bool isOpen() const { return !m_path.isEmpty(); }
    QString path() const { return m_path; }

    int count() const { return m_names.size(); }
    QString nameAt(int index) const;
    qint64 sizeAt(int index) const;

    // 解出第 index 个条目的原始字节。失败返回空 QByteArray。
    QByteArray dataAt(int index) const;

    // 诊断信息
    QString formatName() const { return m_format; }
    bool isEncrypted() const { return m_encrypted; }

private:
    enum class Backend { None, Libarchive, Unrar };

    bool openLibarchive(const QString &path, const QString &password);
    bool openUnrar(const QString &path, const QString &password);
    int countUnlocked() const;
    void sortNames();
    QByteArray dataLibarchive(int index) const;
    QByteArray dataUnrar(int index) const;

    mutable QMutex m_mutex;
    Backend m_backend = Backend::None;

    QString m_path;
    QString m_password;
    QStringList m_names;          // 已按自然排序
    QVector<qint64> m_sizes;
    QVector<int> m_archiveOrder;  // 排序后第 i 项在归档中的原始序号
    QString m_format;
    bool m_encrypted = false;
};
