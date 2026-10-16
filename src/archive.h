#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QByteArray>
#include <QSize>
#include <QMutex>

#include <functional>

class QTemporaryFile;          // RAR 的 openBytes 落临时文件用（定义在 archive.cpp）

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
//
// ★★ v7 定案（2026-10-14）：**对外只有一套 API** —— `open` / `openBytes` / `count` /
//   `nameAt` / `sizeAt` / `dataAt` / `close`。**上层不许有任何"如果是 RAR 就换条路"的
//   分支**：libunrar 只能按路径打开这件事是**本类的内部细节** —— `openBytes`（内存字节）
//   对 RAR 后端自己落一个临时文件，再用 `RAROpenArchiveEx(路径)` 打开；该临时文件
//   **由本对象拥有**，`close()` / 析构时删除。（不要确定性命名、不要存在性检查。）
class ArchiveSource
{
public:
    // 按扩展名判断是否压缩包
    static bool isArchive(const QString &path);

    ~ArchiveSource();

    // 列出哪些条目：
    //   · `ImagesOnly`（**默认，既有行为**）——只保留**图片**条目（`isImageSuffix`）；
    //   · `All` —— 列出**全部**条目。
    //     ★ 查看序列要"钻"嵌套压缩包（压缩包里的压缩包）⇒ 必须能看见非图片条目。
    enum class Entries { ImagesOnly, All };

    // ★★ **进度回调**（可选；**生产者** = `Sequence` 的内部扫描线程用它汇报
    //   "枚举期间也在推进"）：容器**枚举**（我们自己的逐条循环）**每 `kProgressEvery` 条**
    //   调一次。返回值：`true` = 继续；`false` = **立刻停止枚举**（取消 / 已放弃）。
    //   ⚠️ 只在"我们自己的循环里"调 —— **阻塞在单次 I/O 里时不会调**；
    //      那种真卡住只能靠消费侧看门狗 `Sequence::abandon()`（见 `LibraryCore::reapSlot`）。
    static constexpr int kProgressEvery = 32;    // ★ 唯一来源：两个后端的枚举循环共用
    using ProgressFn = std::function<bool()>;

    // 打开并列出条目。password 为空表示不加密。
    //   `progress` 只应传**扫描线程**的钩子；解码 / 缩略图路径不传（它们不是"这次扫描的进度"）。
    bool open(const QString &path, const QString &password = QString(),
              Entries entries = Entries::ImagesOnly, ProgressFn progress = {});
    // ★ v5：从**内存字节**打开容器（嵌套压缩包用 —— 取出来的字节再当容器打开）。
    //   ★ v7：**两个后端都支持**（对外零分派）：
    //     · RAR 签名 ⇒ 内部自己落一个**临时文件**，再用 `RAROpenArchiveEx(路径)` 打开
    //       （libunrar 没有内存打开 API）——临时文件由本对象拥有，`close()` 时删除；
    //     · 其余 ⇒ libarchive 的 `archive_read_open_memory`（不写入磁盘）。
    //   `label` 只用于日志（可空）。
    bool openBytes(const QByteArray &bytes, const QString &label = QString(),
                   const QString &password = QString(),
                   Entries entries = Entries::ImagesOnly, ProgressFn progress = {});
    void close();

    bool isOpen() const { return m_memory || !m_path.isEmpty(); }
    QString path() const { return m_path; }

    int count() const { return m_names.size(); }
    QString nameAt(int index) const;
    qint64 sizeAt(int index) const;

    // 解出第 index 个条目的原始字节。失败返回空 QByteArray。
    QByteArray dataAt(int index) const;

    // 诊断信息
    QString formatName() const { return m_format; }
    bool isEncrypted() const { return m_encrypted; }

    // **能不能随机读**：RAR 由打开时的 ROADF_SOLID 判定（solid ⇒ 不能随机读 ✗）；
    //   其它后端这里恒 false（7z 由 sevenzcheck 单独判；zip/tar 恒可随机 ✓）
    bool isSolid() const { return m_solid; }

private:
    enum class Backend { None, Libarchive, Unrar };

    bool openLibarchive(const QString &path, const QString &password, Entries entries,
                        const ProgressFn &progress);
    bool openLibarchiveBytes(const QByteArray &bytes, const QString &password, Entries entries,
                             const ProgressFn &progress);
    bool openUnrar(const QString &path, const QString &password, Entries entries,
                   const ProgressFn &progress);
    // ★ v7：RAR 的内存通路 —— 落临时文件（唯一命名由 QTemporaryFile 生成），
    //   由本对象拥有；返回 false = 写不了。
    bool openUnrarBytes(const QByteArray &bytes, const QString &label,
                        const QString &password, Entries entries, const ProgressFn &progress);
    void clearCursors();                 // 清顺序游标（定义在 archive.cpp 的结构体之后）
    int countUnlocked() const;
    void sortNames();
    QByteArray dataLibarchive(int index) const;
    QByteArray dataUnrar(int index) const;

    mutable QMutex m_mutex;
    Backend m_backend = Backend::None;

    QString m_path;
    // ★ v7：顺序游标回退时**真正要重开的文件**：
    //   · `open(path)`      ⇒ 就是那个盘上路径；
    //   · `openBytes` 的 RAR ⇒ **临时文件路径**（`m_path` 这时只是 label）；
    //   · `openBytes` 的 libarchive ⇒ 空（用 `m_bytes` 走内存）。
    QString m_reopenPath;
    QByteArray m_bytes;                  // ★ v5：内存容器（openBytes）的字节；m_memory 为真时有效
    bool m_memory = false;
    QString m_password;
    QStringList m_names;          // 已按自然排序
    QVector<qint64> m_sizes;
    QVector<int> m_archiveOrder;  // 排序后第 i 项在归档中的原始序号
    QString m_format;
    bool m_encrypted = false;
    bool m_solid = false;                // RAR solid ⇒ 不能随机读（见 isSolid）
    // ★ v7：RAR 的 openBytes 落的临时文件（**本对象拥有**，close()/析构即删）。
    //   裸指针：QTemporaryFile 只前向声明（析构在 archive.cpp 里做，那里类型完整）。
    QTemporaryFile *m_temp = nullptr;
    // 顺序游标（**复用 reader 往前走**；只在回退时重开）—— 见 archive.cpp 的 ArcSeq/UnrarSeq
    mutable void *m_seqArc = nullptr;
    mutable void *m_seqUnrar = nullptr;
};
