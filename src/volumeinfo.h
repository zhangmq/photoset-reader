#pragma once

// ══ 卷状态：**只读挂载表**判断"某个路径所在的卷现在是什么状态" ══════════════
//   设计：docs/design/10-数据与多进程架构.md §十一；调研：docs/research/13-卷状态与挂载判断-三平台调研.md
//
//   ★ 铁律：**判断动作本身不许碰卷** —— 不 stat / 不 open / 不 statfs 目标路径。
//     理由（三条都是真会发生的）：
//       · Linux `autofs`（systemd automount）：任何 stat **触发自动挂载**，可能阻塞在网络上
//       · 断线的网络盘（nfs/cifs）：stat/statfs 阻塞数十秒（Qt 自己在 Windows 后端注释 "may hang"）
//       · 休眠的机械盘：stat 会把它唤醒（噪音 + 耗时）
//
//   ⇒ **不能用 `QStorageInfo`**（已按本机头文件 + Qt 源码核实）：它没有卷类型 API，
//     且构造时就 `statfs` 目标卷（Linux `statfs64` / macOS `::statfs` /
//     Windows `GetVolumeInformation` + `GetDiskFreeSpaceEx`）。
//
//   三平台实现（各只有一份，`#ifdef` 分开；类型清单只在本文件里维护）：
//     · Linux   ：`/proc/self/mountinfo` —— `fsType` + 挂载源 + 最长挂载点前缀
//     · macOS   ：`getfsstat(MNT_NOWAIT)` —— `f_fstypename` + `f_flags & MNT_LOCAL`
//     · Windows ：`GetVolumePathName` + `GetDriveType`（UNC 直接就是网络）

#include <QString>
#include <QVector>

namespace volume {

enum class Kind {
    LocalFixed,   // 本地固定盘 ⇒ 启动时可以探（可中断 + 超时）
    Removable,    // 可移除介质（U 盘 / 光驱 / 存储卡）⇒ **启动不探**，打开时才探
    Network,      // 网络卷（nfs/cifs/smb/9p/davfs…）⇒ **启动不探**，打开时才探
    Unknown,      // 判不出来（含 Linux `autofs`）⇒ 按"不可探"处理
};

// 挂载表里的一条（只读元数据）
struct MountEntry {
    QString mountPoint;
    QString fsType;      // 平台原始 fs 类型名（诊断用）
    QString source;      // 设备 / 网络源（诊断用）
    Kind    kind = Kind::Unknown;
    bool    autofs = false;   // Linux：`autofs` ⇒ **绝对不碰**（碰了就触发挂载）
};

struct Info {
    bool    mounted = false;      // 路径是否落在**已挂载**的卷上（false ⇒ 未挂载 / 离线）
    Kind    kind = Kind::Unknown;
    QString mountPoint;           // 命中的挂载点（最长前缀匹配的结果）
    QString fsType;               // 命中的 fs 类型名（诊断用）
    bool    autofs = false;

    // 启动一致性探针是否允许碰这个卷（定案：本地固定盘才在启动时探）
    bool probeAtStartup() const { return mounted && !autofs && kind == Kind::LocalFixed; }
};

// 当前挂载表（**只读元数据，不碰任何卷**）；Linux / macOS 有实现，Windows 走单路径分支（返回空）
QVector<MountEntry> mountTable();

// 传入**绝对路径**（**不要求存在**——存在性检查才是碰盘，这里不做）；只读挂载表
Info infoFor(const QString &absPath);

// 平台判定条件：这个 fs 类型是不是网络卷（Linux 的清单是**唯一维护处**）
bool isNetworkFsType(const QString &fsType);

} // namespace volume
