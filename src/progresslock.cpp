#include "progresslock.h"

#include "logging.h"

#include <QDir>
#include <QFileInfo>
#include <QLockFile>

namespace progresslock {

QString lockPathFor(const QString &prsavePath)
{
    //   规范化（解符号链接 / 统一大小写）；文件还不存在（另存）⇒ 父目录 + 文件名
    QString canonical = QFileInfo(prsavePath).canonicalFilePath();
    if (canonical.isEmpty()) {
        const QFileInfo fi(prsavePath);
        QString dir = QFileInfo(fi.absolutePath()).canonicalFilePath();
        if (dir.isEmpty()) dir = fi.absolutePath();
        canonical = QDir(dir).filePath(fi.fileName());
    }
    return canonical + QStringLiteral(".lock");
}

Guard::~Guard()
{
    release();
}

bool Guard::acquire(const QString &prsavePath, QString *holder, QString *reasonKey)
{
    const QString sidecar = lockPathFor(prsavePath);
    if (m_lock && sidecar == lockPathFor(m_prsave))
        return true;                       // 同一把锁重复 acquire（同一进程内）

    release();

    //   ★ 2026-11-07：**目标目录还不存在时必须先建它**（收藏夹第一次使用时就是这样）。
    //     以前直接 `tryLock()`：`QLockFile` 建不出锁文件 ⇒ `reasonKey` 落到 "lock-error"，
    //     上层把"目录没建"误当成"只读/被占用"，`saveProgressToFavorites()` 直接失败
    //     （Linux CI 上就是这样红的）。`mkpath` 对已存在的目录是无操作。
    if (!QDir().mkpath(QFileInfo(sidecar).absolutePath())) {
        if (holder) *holder = QString();
        if (reasonKey) *reasonKey = QStringLiteral("readonly-dir");
        qCInfo(lcUi).noquote()
            << QStringLiteral("[ui] progress lock failed (cannot create directory): %1")
                   .arg(QFileInfo(sidecar).absolutePath());
        return false;
    }

    auto lock = std::make_unique<QLockFile>(sidecar);
    lock->setStaleLockTime(0);          // ★ 见头文件：默认 30 s 会把活锁抢走

    if (lock->tryLock(0)) {
        m_lock = std::move(lock);
        m_prsave = prsavePath;
        qCDebug(lcUi).noquote() << QStringLiteral("[ui] progress lock acquired: %1").arg(sidecar);
        return true;
    }

    //   失败：区分"被人占着"与"建不出锁文件"
    const QLockFile::LockError err = lock->error();
    qint64 pid = 0;
    QString host;
    QString app;
    lock->getLockInfo(&pid, &host, &app);

    if (holder) {
        *holder = pid > 0 ? QStringLiteral("%1@%2").arg(pid).arg(host.isEmpty() ? QStringLiteral("?") : host)
                          : QString();
    }
    if (reasonKey) {
        if (err == QLockFile::PermissionError) {
            //   目录只读 / 没有写权限：不加锁，允许读（调用方转只读模式）
            *reasonKey = QStringLiteral("readonly-dir");
        } else if (err == QLockFile::LockFailedError) {
            *reasonKey = QStringLiteral("in-use");
        } else {
            *reasonKey = QStringLiteral("lock-error");
        }
    }
    qCInfo(lcUi).noquote()
        << QStringLiteral("[ui] progress lock failed (%1): %2 holder=%3")
               .arg(err == QLockFile::PermissionError ? QStringLiteral("permission")
                    : err == QLockFile::LockFailedError ? QStringLiteral("in-use")
                                                        : QStringLiteral("other"))
               .arg(sidecar, holder != nullptr ? *holder : QString());
    return false;
}

void Guard::release()
{
    if (!m_lock)
        return;
    m_lock->unlock();                      // 删掉 sidecar
    qCDebug(lcUi).noquote() << QStringLiteral("[ui] progress lock released: %1")
                                   .arg(lockPathFor(m_prsave));
    m_lock.reset();
    m_prsave.clear();
}

}   // namespace progresslock
