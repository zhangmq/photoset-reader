#include "testsupport.h"

// ══ 多实例契约（`ib-test-instance`）════════════════════════════════════════
//   为什么需要这一套（2026-11-06）：单实例转发是 `c78f59c`（2026-11-02）里**我自己
//   加进来的**东西（注释里那句"用户选 A"是编造的，用户从来没提过），后果是
//   **带路径的第二次启动也没有自己的窗口**。对看图应用来说"同时开多个窗口"是基本
//   需求，而这类退化**编译、别的套件、CTest 全都发现不了** —— 只有人去点第二次才
//   会看到。所以把契约钉成测试：
//
//   ① 第一个实例先**跑到稳态**（写出 `[ui] viewer:` 行 —— 它排在"建窗口 + 装 IPC 服务端"
//     之后，用它当"第一个实例已经就绪"的信号）；如果只等 `log started` 就会**太早**：
//      那行在解析命令行时就写了，旧实现的 `QLocalServer` 那时还没建 ⇒ 第二个实例连不上
//      ⇒ 它照常开窗口 ⇒ 测试**假绿**（2026-11-06 实测踩到，护栏一度形同虚设）；
//   ② ★ 结构性断言：从外部连 `photoset-reader-ipc` **必须失败**
//      —— 这个产品不提供进程间转发通道。连得上就说明单实例又回来了（与第二个实例的
//      行为无关，不依赖时序，旧实现下必红）；
//   ③ 再起第二个**带路径**的实例 ⇒ 它必须启动、写自己的日志、且不写 `forwarded`；
//   ④ 观察 4 秒，**两个进程都还在**。
//
//   ★ 用 `PHOTOSET_CONFIG_DIR` 把设置指到临时目录：多实例各写各的 settings.json，
//     绝不能碰用户真实配置（这条套件也不例外）。

#include <QDir>
#include <QFile>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>

namespace {

QString readAll(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}

//   旧实现用的固定名字（`PHOTOSET_IPC_NAME`）；这里故意写字面量 —— 一旦哪天又有人
//   想加回"把路径转给已运行实例"，这条断言会直接亮红。
constexpr char kLegacyIpcName[] = "photoset-reader-ipc";

}  // namespace

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, [] {
        testsupport::Check check(QStringLiteral("instance"));

        testsupport::TempFixture tmp;
        if (!tmp.isValid())
            return testsupport::skip(QStringLiteral("instance"),
                                     QStringLiteral("临时目录建不出来"));

        //   ★ 被测二进制与套件**同目录**（CMake 把两者都放在构建根）
        QString app = QCoreApplication::applicationDirPath() + QLatin1Char('/')
                    + QStringLiteral("photoset-reader");
#if defined(Q_OS_WIN)
        app += QStringLiteral(".exe");
#endif
        if (!QFile::exists(app))
            return testsupport::skip(QStringLiteral("instance"),
                                     QStringLiteral("构建根里没有产品二进制（只构建了测试时正常）"));

        //   ── 夹具：一个只有一张图的来源 + 独立的设置目录 ──────────────────
        const QString src = tmp.filePath(QStringLiteral("src"));
        QDir().mkpath(src);
        {
            QImage img(64, 96, QImage::Format_RGB32);
            img.fill(Qt::darkCyan);
            img.save(src + QStringLiteral("/p1.png"), "PNG");
        }
        const QString cfg = tmp.filePath(QStringLiteral("cfg"));
        QDir().mkpath(cfg);

        auto launch = [&](const QString &tag) {
            auto *p = new QProcess;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            //    套件本身跑在 offscreen 上；子进程也一样（没有显示器也能验多实例）
            env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
            env.insert(QStringLiteral("PHOTOSET_CONFIG_DIR"), cfg);
            p->setProcessEnvironment(env);
            p->setProcessChannelMode(QProcess::MergedChannels);
            p->start(app, QStringList{src,
                                      QStringLiteral("--log-level"), QStringLiteral("info"),
                                      QStringLiteral("--log-file"),
                                      tmp.filePath(tag + QStringLiteral(".log"))});
            return p;
        };

        QProcess *a = launch(QStringLiteral("a"));
        const bool aStarted = a->waitForStarted(5000);
        const QString aLog = tmp.filePath(QStringLiteral("a.log"));
        //   ★ 等 `[ui] viewer:` 而**不是** `log started`：后者在解析参数时就写了，
        //     旧实现的 IPC 服务端那时还没建 ⇒ 第二个实例连不上 ⇒ 假绿（见文件头）。
        const bool aReady = testsupport::waitFor(
            [&] { return readAll(aLog).contains(QStringLiteral("[ui] viewer:")); }, 30000);
        check(aStarted && aReady, QStringLiteral("① 第一个实例跑到稳态（出现 `[ui] viewer:` 行）"));

        //   ★★ 结构性断言：产品**不该**提供进程间转发通道（旧实现的痕迹）
        {
            QLocalSocket probe;
            probe.connectToServer(QString::fromLatin1(kLegacyIpcName), QIODevice::WriteOnly);
            const bool connected = probe.waitForConnected(1500);
            if (connected) probe.disconnectFromServer();
            check(!connected,
                  QStringLiteral("① ★ 连得上 `%1` ⇒ 单实例转发又回来了（必须连不上）")
                      .arg(QString::fromLatin1(kLegacyIpcName)));
        }

        //   ★ 顺序就是"用户点第二次"：第一个已经在跑，再起第二个**带路径**的
        QProcess *b = launch(QStringLiteral("b"));
        const bool bStarted = b->waitForStarted(5000);
        const QString bLog = tmp.filePath(QStringLiteral("b.log"));
        const bool bLogged = testsupport::waitFor(
            [&] { return readAll(bLog).contains(QStringLiteral("log started")); }, 20000);
        check(bStarted && bLogged,
              QStringLiteral("① 第二个实例也启动了、也写了自己的日志（没被转发吃掉）"));

        //   观察窗口：两个都得一直活着（旧实现里第二个 `return 0` 之后就不在了）
        testsupport::waitFor([] { return false; }, 4000);
        check(a->state() == QProcess::Running && b->state() == QProcess::Running,
              QStringLiteral("② ★ 两个进程**同时在跑**（a=%1 b=%2；0=NotRunning 1=Starting 2=Running）")
                  .arg(int(a->state())).arg(int(b->state())));

        const QString aText = readAll(aLog);
        const QString bText = readAll(bLog);
        check(!aText.contains(QStringLiteral("forwarded to running instance"))
                  && !bText.contains(QStringLiteral("forwarded to running instance")),
              QStringLiteral("② 两个实例都没有走「转发给已运行实例」那条老路"));

        for (QProcess *p : {a, b}) {
            //   先礼后兵：`terminate()`（Guesture: SIGTERM / WM_CLOSE）让产品走优雅退出，
            //   3 秒不走就 `kill()`，并且**等它真的没了**再 delete —— 否则 Qt 会报
            //   "QProcess: Destroyed while process is still running"（2026-11-06 实测）。
            p->terminate();
            if (!p->waitForFinished(3000)) {
                p->kill();
                p->waitForFinished(3000);
            }
            delete p;
        }
        return check.finish();
    });
}
