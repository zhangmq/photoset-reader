#include "volumeinfo.h"

#include <QtGlobal>          // ★ Q_OS_* 宏（平台分支靠它；别指望 QString 间接带进来）

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QStringList>

#if defined(Q_OS_WIN)
#  include <QtCore/qt_windows.h>
#elif defined(Q_OS_MACOS)
#  include <sys/mount.h>
#  include <sys/param.h>
#endif

namespace volume {

namespace {

// 路径是否落在该挂载点下（要求边界是 '/'；挂载点为根时恒真）
bool covers(const QString &mountPoint, const QString &path)
{
    if (mountPoint == QLatin1String("/")) return true;
    if (path == mountPoint) return true;
    return path.startsWith(mountPoint) && path.size() > mountPoint.size()
           && path.at(mountPoint.size()) == QLatin1Char('/');
}

// **最长挂载点前缀匹配**（唯一实现）
//   ⚠️ 必须按最长匹配：同一设备可以有多个挂载点（本机实测 `/` 与 `/home` 都是
//      `/dev/nvme1n1p2`），按设备判会错；叠加挂载也靠最长匹配取最内层。
//   ⚠️ **同一路径上可能同时有 `autofs` 条目和真实挂载条目**（systemd automount：
//      本机 `/mnt/router-samba` 就同时有 `autofs|systemd-1` 与 `cifs|//192.168.50.1/seagate`）。
//      谁是第一条取决于内核表的顺序 ⇒ **不许按顺序定**：最长层里只要有 autofs，
//      就按 autofs 处理（**绝不触碰** —— 这也是唯一安全的结论）。
Info pick(const QVector<MountEntry> &entries, const QString &path)
{
    int bestLen = -1;
    bool anyAutofs = false;
    const MountEntry *best = nullptr;
    for (const MountEntry &e : entries) {
        if (!covers(e.mountPoint, path)) continue;
        const int len = e.mountPoint.size();
        if (len < bestLen) continue;
        if (len > bestLen) { bestLen = len; best = nullptr; anyAutofs = false; }
        if (e.autofs) anyAutofs = true;
        if (!best) best = &e;
    }
    Info out;
    if (!best) return out;
    out.mounted    = true;
    out.mountPoint = best->mountPoint;
    out.fsType     = best->fsType;
    out.autofs     = anyAutofs;
    out.kind       = anyAutofs ? Kind::Unknown : best->kind;
    return out;
}

// 归一化：绝对化 + 去重复斜杠 / `.`（**不解析符号链接** —— 那要 stat）
QString normalize(const QString &path)
{
    if (path.isEmpty()) return path;
    const QString abs = QDir::isAbsolutePath(path) ? path : QDir::current().absoluteFilePath(path);
    return QDir::cleanPath(abs);
}

#if defined(Q_OS_LINUX)

// 内核会把挂载点里的 空格 / TAB / 反斜杠 / 换行 转义成三位八进制（`\040` …）
QString unescapeMountPath(const QByteArray &raw)
{
    QByteArray out;
    out.reserve(raw.size());
    for (int i = 0; i < raw.size(); ++i) {
        if (raw.at(i) == '\\' && i + 3 < raw.size()) {
            bool ok = true;
            int c = 0;
            for (int k = 1; k <= 3; ++k) {
                const char d = raw.at(i + k);
                if (d < '0' || d > '7') { ok = false; break; }
                c = (c << 3) | (d - '0');
            }
            if (ok) { out.append(char(c)); i += 3; continue; }
        }
        out.append(raw.at(i));
    }
    return QString::fromLocal8Bit(out);
}

// ── 网络文件系统类型（**唯一维护处**；兜底再看"挂载源形如 host:/path 或 //host/share"）──
const QStringList &networkFsTypes()
{
    static const QStringList kList = {
        QStringLiteral("nfs"),      QStringLiteral("nfs4"),     QStringLiteral("cifs"),
        QStringLiteral("smb3"),     QStringLiteral("smbfs"),    QStringLiteral("afs"),
        QStringLiteral("9p"),       QStringLiteral("ceph"),     QStringLiteral("glusterfs"),
        QStringLiteral("davfs"),    QStringLiteral("ncpfs"),    QStringLiteral("ceph-fuse"),
        QStringLiteral("fuse.sshfs"), QStringLiteral("fuse.rclone"), QStringLiteral("fuse.smbnetfs"),
        QStringLiteral("fuse.davfs"), QStringLiteral("fuse.gvfsd-fuse"),
    };
    return kList;
}

bool looksNetworkSource(const QString &source)
{
    if (source.startsWith(QStringLiteral("//"))) return true;              // cifs: //server/share
    const int colon = source.indexOf(QLatin1Char(':'));
    if (colon > 0 && !source.startsWith(QLatin1Char('/'))) return true;    // nfs: server:/export
    return false;
}

// 可移除介质（**启发式**：宁可误判成可移除 —— 代价只是"启动时不探，打开时再探"）
//   精确判定条件是读 `/sys/block/<dev>/removable`（也是纯元数据），本期未做
bool looksRemovable(const QString &fsType, const QString &source, const QString &mountPoint)
{
    if (fsType == QLatin1String("iso9660") || fsType == QLatin1String("udf"))
        return true;                                                       // 光驱
    // ⚠️ `/dev/sd*` 包含内置 SATA 盘 ⇒ 可能把内置盘误判为可移除（安全侧：少探一次）
    static const QStringList kDevPrefixes = {
        QStringLiteral("/dev/sd"), QStringLiteral("/dev/sr"), QStringLiteral("/dev/mmcblk"),
        QStringLiteral("/dev/fd"),
    };
    for (const QString &p : kDevPrefixes)
        if (source.startsWith(p)) return true;
    if (mountPoint.startsWith(QStringLiteral("/run/media/"))
        || mountPoint.startsWith(QStringLiteral("/media/")))
        return true;
    return false;
}

#elif defined(Q_OS_WIN)

MountEntry entryForPath(const QString &path, bool *ok)
{
    // Windows 没有"一次读全表"的元数据接口 ⇒ 按需把**这一个路径**映射到挂载根
    //   （`GetVolumePathName` 就是 Qt 的 `QStorageInfo` 内部用的同一招，纯路径计算）
    MountEntry e;
    *ok = false;
    if (path.startsWith(QStringLiteral("//"))) {          // UNC：\\server\share
        e.mountPoint = path;
        e.kind = Kind::Network;
        *ok = true;
        return e;
    }
    wchar_t buf[MAX_PATH];
    if (!::GetVolumePathNameW(reinterpret_cast<const wchar_t *>(path.utf16()), buf, MAX_PATH))
        return e;                                          // 没有卷挂在这个路径上
    QString root = QDir::fromNativeSeparators(QString::fromWCharArray(buf));
    while (root.size() > 1 && root.endsWith(QLatin1Char('/')))   // 去掉尾部 '/'（`C:/` → `C:`）
        root.chop(1);
    e.mountPoint = root;
    const QString native = QDir::toNativeSeparators(root + QLatin1Char('/'));   // GetDriveType 要根 + '\'
    switch (::GetDriveTypeW(reinterpret_cast<const wchar_t *>(native.utf16()))) {
    case DRIVE_REMOTE:      e.kind = Kind::Network;    break;
    case DRIVE_REMOVABLE:
    case DRIVE_CDROM:       e.kind = Kind::Removable;  break;
    case DRIVE_FIXED:
    case DRIVE_RAMDISK:     e.kind = Kind::LocalFixed; break;
    case DRIVE_NO_ROOT_DIR: return e;                  // 该根下没有卷 ⇒ 未挂载
    default:                e.kind = Kind::Unknown;    break;
    }
    *ok = true;
    return e;
}

#endif

} // namespace

bool isNetworkFsType(const QString &fsType)
{
#if defined(Q_OS_LINUX)
    return networkFsTypes().contains(fsType);
#else
    Q_UNUSED(fsType);
    return false;
#endif
}

QVector<MountEntry> mountTable()
{
#if defined(Q_OS_LINUX)
    QVector<MountEntry> out;
    QFile f(QStringLiteral("/proc/self/mountinfo"));
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning().noquote() << QStringLiteral("[volume] 读不到 /proc/self/mountinfo：%1")
                                    .arg(f.errorString());
        return out;
    }
    // ⚠️ 不能用 `atEnd()`：procfs 文件 **stat 出来的 size 是 0** ⇒ `atEnd()` 一开始就为真
    //    （实测踩到：整个挂载表读成 0 条）。`readAll()` 会一直读到 read() 返回 0，是对的。
    //    这个文件是内核现生成的，几十 KB 量级；加一道上限只是防御。
    const QByteArray all = f.readAll();
    if (all.size() > 8 * 1024 * 1024) return out;
    const QList<QByteArray> rawLines = all.split('\n');
    for (const QByteArray &line : rawLines) {
        if (line.isEmpty()) continue;
        // 格式：mntid parent maj:min fsroot mountpoint mountopts [optional…] - fsType source superopts
        const int sep = line.indexOf(QByteArrayLiteral(" - "));
        if (sep < 0) continue;
        const QList<QByteArray> head = line.left(sep).split(' ');
        if (head.size() < 6) continue;
        const QList<QByteArray> tail = line.mid(sep + 3).split(' ');
        if (tail.size() < 2) continue;

        MountEntry e;
        e.mountPoint = unescapeMountPath(head.at(4));
        e.fsType     = QString::fromLatin1(tail.at(0));
        e.source     = QString::fromLatin1(tail.at(1));
        if (e.fsType == QLatin1String("autofs")) {
            e.autofs = true;
            e.kind   = Kind::Unknown;          // 触发后可能是任何东西 ⇒ 绝不碰
        } else if (isNetworkFsType(e.fsType) || looksNetworkSource(e.source)) {
            e.kind = Kind::Network;
        } else if (looksRemovable(e.fsType, e.source, e.mountPoint)) {
            e.kind = Kind::Removable;
        } else {
            e.kind = Kind::LocalFixed;
        }
        out << e;
    }
    return out;
#elif defined(Q_OS_MACOS)
    QVector<MountEntry> out;
    // MNT_NOWAIT：只回答"手上已有的信息"，**不向各文件系统请求更新**、不会被无法响应的 fs 阻塞
    const int count = ::getfsstat(nullptr, 0, MNT_NOWAIT);
    if (count <= 0) return out;
    QVector<struct statfs> buf(count);
    const int n = ::getfsstat(buf.data(), count * int(sizeof(struct statfs)), MNT_NOWAIT);
    for (int i = 0; i < n; ++i) {
        MountEntry e;
        e.mountPoint = QString::fromUtf8(buf.at(i).f_mntonname);
        e.fsType     = QString::fromUtf8(buf.at(i).f_fstypename);
        e.source     = QString::fromUtf8(buf.at(i).f_mntfromname);
        // Apple QA NW09 的官方判定条件：本地卷 = `f_flags & MNT_LOCAL`
        //   ⚠️ 可移除（U 盘）在 macOS 上也满足 MNT_LOCAL ⇒ 这里**区分不出来**；
        //      再往下就要 DiskArbitration（键的语义 Apple 没写文档，且已知雷电盘判不准）⇒ 不做。
        e.kind = (buf.at(i).f_flags & MNT_LOCAL) ? Kind::LocalFixed : Kind::Network;
        out << e;
    }
    return out;
#else
    return {};   // Windows：没有"一次读全表"的元数据接口（走 infoFor 的单路径分支）
#endif
}

Info infoFor(const QString &absPath)
{
    const QString path = normalize(absPath);
    if (path.isEmpty()) return {};

#if defined(Q_OS_WIN)
    bool ok = false;
    const MountEntry e = entryForPath(path, &ok);
    if (!ok) return {};
    return pick(QVector<MountEntry> { e }, path);
#elif defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    return pick(mountTable(), path);
#else
    Info out;
    out.mounted = true;      // 不猜"未挂载"（会把整个库标成离线）⇒ 未知类型天然"启动不探"
    return out;
#endif
}

} // namespace volume
