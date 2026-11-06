#include "testsupport.h"

#include "decode/capabilities.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QThread>

namespace testsupport {

// ── 套件入口：旧的 `photoset-reader --selftest-*` 在 dispatch 之前做的事，一模一样 ──
int suiteMain(int argc, char *argv[], const std::function<int()> &suite)
{
    // ★ 强制系统原生文件对话框（用户要求 #1）；必须在 QGuiApplication 构造之前设置
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORMTHEME"))
        qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktoopportal");
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("photoset-reader"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1-dev"));
    //   ★ 能力探测 = **套件的"启动时"**：产品里由 `main.cpp` 做，套件没有那条路径，
    //     所以在这里显式做一次（生产代码里**不再有惰性探测**，见 capabilities.h 的注释）。
    decode::Capabilities::instance().probe();
    return suite();
}

// ── 需要素材目录的套件：从 argv 取 `-d <目录>`（旧 CLI 同一形状）──
QString dirArg(int argc, char *argv[])
{
    for (int i = 1; i + 1 < argc; ++i) {
        const char *a = argv[i];
        if (qstrcmp(a, "-d") == 0 || qstrcmp(a, "--dir") == 0)
            return QString::fromLocal8Bit(argv[i + 1]);
    }
    return QString();
}

bool waitFor(const std::function<bool()> &done, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (done()) return true;
        QThread::msleep(1);
    }
    QCoreApplication::processEvents();
    return done();
}

QString TempFixture::useLibraryDb(const QString &name)
{
    const QString p = m_dir.filePath(name);
    qputenv("PHOTOSET_LIBRARY_DB", p.toLocal8Bit());
    return p;
}

QString TempFixture::useSettingsFile(const QString &name)
{
    //   用**标准** XDG 配置目录做隔离（产品里不再有私有的设置文件 env 后门）
    qputenv("XDG_CONFIG_HOME", m_dir.path().toLocal8Bit());
    const QString dir = m_dir.filePath(QStringLiteral("photoset-reader"));
    QDir().mkpath(dir);
    return dir + QStringLiteral("/") + name;
}

} // namespace testsupport
