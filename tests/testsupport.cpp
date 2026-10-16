#include "testsupport.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QThread>

namespace testsupport {

// ── 套件入口：旧的 `image-browser --selftest-*` 在 dispatch 之前做的事，一模一样 ──
int suiteMain(int argc, char *argv[], const std::function<int()> &suite)
{
    // ★ 强制系统原生文件对话框（用户要求 #1）；必须在 QGuiApplication 构造之前设置
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORMTHEME"))
        qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktoopportal");
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("photoset-reader"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1-dev"));
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
    const QString p = m_dir.filePath(name);
    qputenv("PHOTOSET_SETTINGS_FILE", p.toLocal8Bit());
    return p;
}

} // namespace testsupport
