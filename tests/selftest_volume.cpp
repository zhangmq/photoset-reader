#include "testsupport.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include "volumeinfo.h"

// ══ 卷状态自检（ib-test-volume；原先由 `image-browser --selftest-volume` 跑）══════
//   设计 10 §十一；调研 13。用**本机真实挂载表**验证匹配规则（**全程不碰任何卷**）：
//     ① 每个挂载点命中自己   ② 最长挂载点前缀匹配（同一设备可有多个挂载点）
//     ③ 不要求路径存在（存在性检查才是碰盘）   ④ 只有"本地固定盘"启动可探
static int runVolumeSelfTest()
{
    testsupport::Check check(QStringLiteral("volumetest"));

    const QVector<volume::MountEntry> table = volume::mountTable();
    qInfo().noquote() << QStringLiteral("[volumetest] 本机挂载点 %1 条（只读元数据）")
                             .arg(table.size());
    check(!table.isEmpty(), QStringLiteral("读到挂载表"));

    static const char *kKindName[] = { "LocalFixed", "Removable", "Network", "Unknown" };
    QHash<QString, int> byKind;
    for (const volume::MountEntry &e : table)
        byKind[QString::fromLatin1(kKindName[int(e.kind)])] += 1;
    QString summary;
    for (auto it = byKind.cbegin(); it != byKind.cend(); ++it)
        summary += QStringLiteral("%1=%2 ").arg(it.key()).arg(it.value());
    qInfo().noquote() << QStringLiteral("[volumetest] 类型分布：%1").arg(summary.trimmed());

    // ① 每条挂载点自己必须命中自己
    {
        bool allSelf = true;
        QStringList seen;
        for (const volume::MountEntry &e : table) {
            if (seen.contains(e.mountPoint)) continue;        // 叠加挂载可能有重复挂载点
            seen << e.mountPoint;
            const volume::Info i = volume::infoFor(e.mountPoint);
            if (!i.mounted || i.mountPoint != e.mountPoint) allSelf = false;
        }
        check(allSelf, QStringLiteral("① 每个挂载点命中自己"));
    }

    // ② 最长前缀：A 严格位于 B 之下 ⇒ A 内的路径必须命中 A（不能命中 B）
    {
        int tested = 0;
        bool ok = true;
        for (const volume::MountEntry &a : table) {
            if (a.mountPoint == QLatin1String("/")) continue;
            for (const volume::MountEntry &b : table) {
                if (b.mountPoint == a.mountPoint) continue;
                const bool nested = (b.mountPoint == QLatin1String("/"))
                                    || a.mountPoint.startsWith(b.mountPoint + QLatin1Char('/'));
                if (!nested) continue;
                const volume::Info i = volume::infoFor(a.mountPoint + QStringLiteral("/x"));
                if (!i.mounted || i.mountPoint != a.mountPoint) ok = false;
                ++tested;
                break;
            }
        }
        check(tested > 0 && ok,
              QStringLiteral("② 最长挂载点前缀匹配（嵌套挂载点 %1 处）").arg(tested));
    }

    // ③ 不要求路径存在
    check(volume::infoFor(QStringLiteral("/photoset-does-not-exist-xyz")).mounted,
          QStringLiteral("③ 不存在的路径照样能判卷状态（没碰盘）"));

    // ④ 分层判定条件：autofs / 网络 / 可移除 / 未知 ⇒ 启动不探；只有本地固定盘启动可探
    {
        bool consistent = true;
        int probeable = 0, autofs = 0;
        for (const volume::MountEntry &e : table) {
            const volume::Info i = volume::infoFor(e.mountPoint);
            const bool expect = i.mounted && !i.autofs && i.kind == volume::Kind::LocalFixed;
            if (i.probeAtStartup() != expect) consistent = false;
            if (i.probeAtStartup()) ++probeable;
            if (e.autofs) {
                ++autofs;
                qInfo().noquote() << QStringLiteral("[volumetest]   autofs（绝不触碰）：%1")
                                         .arg(e.mountPoint);
            }
        }
        check(consistent, QStringLiteral("④ 启动可探 = 已挂载 且 非 autofs 且 本地固定"));
        qInfo().noquote() << QStringLiteral("[volumetest] 启动可探 %1 条 / autofs %2 条")
                                 .arg(probeable).arg(autofs);
    }

#if defined(Q_OS_LINUX)
    check(volume::isNetworkFsType(QStringLiteral("nfs4"))
              && volume::isNetworkFsType(QStringLiteral("cifs"))
              && !volume::isNetworkFsType(QStringLiteral("btrfs")),
          QStringLiteral("⑤ 网络 fs 类型判定条件（nfs4 / cifs 是，btrfs 不是）"));

    // ⑥ 同一路径同时有 autofs 条目和真实挂载条目（systemd automount）⇒ 必须按 autofs
    //    （**不许看内核表的顺序**；本机 `/mnt/router-samba` = autofs|systemd-1 + cifs|//…）
    {
        QHash<QString, int> atMount;
        for (const volume::MountEntry &e : table) atMount[e.mountPoint] += 1;
        int tested = 0;
        bool ok = true;
        for (const volume::MountEntry &e : table) {
            if (!e.autofs || atMount.value(e.mountPoint) < 2) continue;
            ++tested;
            const volume::Info i = volume::infoFor(e.mountPoint + QStringLiteral("/x"));
            if (!i.autofs || i.probeAtStartup()) ok = false;
        }
        if (tested > 0)
            check(ok, QStringLiteral("⑥ autofs 与真实挂载同路径 ⇒ 按 autofs（%1 处）").arg(tested));
        else
            qInfo().noquote() << QStringLiteral("[volumetest] 跳过：⑥ 本机没有 automount 同路径样例");
    }
#endif

    return check.finish();
}

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, &runVolumeSelfTest);
}
