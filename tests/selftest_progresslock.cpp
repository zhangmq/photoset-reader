#include "testsupport.h"

// ══ 进度存档的独占锁（`ib-test-progresslock`）══════════════════════════════
//   钉住三件事：
//   ① 同一份 `.prsave` 只能被一个 Guard 持有（第二个必须失败、且原因是 in-use）；
//   ② **进程被 kill -9 之后，锁立刻可被回收**（本项目不用"手动删 .lock"）；
//   ③ 路径规范化：`a/x.prsave` 与 `a/../a/x.prsave` 是同一把锁。
//
//   ②的做法：**把本套件自己**再起一个进程（`--hold-lock <path>`），让它拿住锁后睡觉，
//   然后 `QProcess::kill()`（Unix = SIGKILL / Windows = TerminateProcess），
//   再在当前进程里 acquire —— 必须成功。这是"崩溃不锁死"的实证，不是推理。
//
//   ★ 只覆盖锁本身；"只读目录"那条（`PermissionError` → 不加锁）留给真机手工验，
//     因为 Windows 的 ACL 不能用 chmod 表达（不假装覆盖）。

#include "progresslock.h"

#include <QDir>
#include <QFile>
#include <QProcess>

namespace {

QString g_holdLockPath;

int runHolder(const QString &path)
{
    progresslock::Guard guard;
    guard.acquire(path);
    QTextStream(stdout) << (guard.isHeld() ? "locked\n" : "failed\n");
    QTextStream(stdout).flush();
    QThread::sleep(60);                 // 等对方 kill 掉我们
    return 0;
}

QString readAll(const QString &p)
{
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}

}  // namespace

int main(int argc, char *argv[])
{
    //   ★ holder 模式必须在 `suiteMain`（= 建 QApplication）之前分叉：它不需要 GUI。
    for (int i = 1; i + 1 < argc; ++i) {
        if (qstrcmp(argv[i], "--hold-lock") == 0)
            return runHolder(QString::fromLocal8Bit(argv[i + 1]));
    }

    return testsupport::suiteMain(argc, argv, [] {
        testsupport::Check check(QStringLiteral("progresslock"));

        testsupport::TempFixture tmp;
        if (!tmp.isValid())
            return testsupport::skip(QStringLiteral("progresslock"),
                                     QStringLiteral("临时目录建不出来"));

        const QString dir = tmp.filePath(QStringLiteral("d"));
        QDir().mkpath(dir);
        const QString prsave = dir + QStringLiteral("/x.prsave");
        {
            QFile f(prsave);
            f.open(QIODevice::WriteOnly);
            f.write("{}");
        }

        // ── ① 独占 ────────────────────────────────────────────────────
        {
            progresslock::Guard a;
            QString why;
            check(a.acquire(prsave, nullptr, &why),
                  QStringLiteral("① 第一个 Guard 拿到锁（sidecar=%1）")
                      .arg(progresslock::lockPathFor(prsave)));
            check(QFile::exists(progresslock::lockPathFor(prsave)),
                  QStringLiteral("① sidecar 真的建出来了（不是锁 .prsave 本身）"));

            progresslock::Guard b;
            QString why2;
            const bool got = b.acquire(prsave, nullptr, &why2);
            check(!got && why2 == QStringLiteral("in-use"),
                  QStringLiteral("① 第二个 Guard 拿不到（原因=%1）").arg(why2));

            a.release();
            check(b.acquire(prsave, nullptr, &why2),
                  QStringLiteral("① 释放之后可以拿到"));
        }

        // ── ② 规范化：`a/../a/x` 与 `a/x` 是同一把锁 ──────────────────
        {
            progresslock::Guard a;
            a.acquire(prsave);
            progresslock::Guard b;
            const QString same = dir + QStringLiteral("/../d/x.prsave");
            QString why;
            const bool got = b.acquire(same, nullptr, &why);
            check(!got, QStringLiteral("② 经 `..` 的同一路径被同一把锁挡住（原因=%1）").arg(why));
        }

        // ── ③ kill -9 之后锁可回收 ────────────────────────────────────
        {
            //   先把当前进程手里的锁放掉，让 holder 去拿
            const QString holderLog = tmp.filePath(QStringLiteral("holder.log"));
            QFile::remove(holderLog);
            auto *p = new QProcess;
            p->setStandardOutputFile(holderLog);
            p->start(QCoreApplication::applicationFilePath(),
                     QStringList{QStringLiteral("--hold-lock"), prsave});
            const bool started = p->waitForStarted(5000);
            const bool locked = testsupport::waitFor(
                [&] { return readAll(holderLog).contains(QStringLiteral("locked")); }, 10000);
            check(started && locked, QStringLiteral("③ holder 进程拿到了锁"));

            progresslock::Guard probe;
            QString why;
            const bool got = probe.acquire(prsave, nullptr, &why);
            check(!got, QStringLiteral("③ 持有期间别的进程拿不到（原因=%1）").arg(why));

            //   ★ SIGKILL / TerminateProcess：不给它任何清理机会
            p->kill();
            const bool dead = p->waitForFinished(5000);
            delete p;
            check(dead, QStringLiteral("③ holder 被强杀"));

            progresslock::Guard after;
            QString why2;
            const bool recovered = testsupport::waitFor([&] { return after.acquire(prsave, nullptr, &why2); }, 5000);
            check(recovered, QStringLiteral("③ ★ 强杀之后锁**能立刻回收**（崩溃不锁死）"
                                      "（原因=%1，空=拿到了）").arg(why2));
        }

        return check.finish();
    });
}
