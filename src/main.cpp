#include <QGuiApplication>
#include <QIcon>
#include <QApplication>
#include <QThreadPool>          // 文件对话框用 QFileDialog（QtWidgets）；QApplication ⊂ QGuiApplication
#include <QStyleHints>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QQmlContext>
#include <QCommandLineParser>
#include <QImageReader>
#include <QFileInfo>
#include <QFontDatabase>
#include <QEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSocketNotifier>
#include <QTimer>

#ifdef Q_OS_UNIX
#  include <csignal>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include "logging.h"
#include "qmlregister.h"
#include "archivesource.h"

#include "decode/capabilities.h"
#include "natural.h"
#include "uimodel.h"
#include "previewprovider.h"

#ifndef DEFAULT_TEST_DIR
#define DEFAULT_TEST_DIR ""
#endif

// ── 记录"最后一次输入是键盘还是鼠标" ─────────────────────────
//   用途：QML 侧实现 focus-visible —— 焦点环只在键盘操作时显示。
//   Qt Quick 没有暴露焦点原因（实测 Item.focusReason 在 QML 中不存在），
//   所以在应用级事件过滤器里自己记：按键 → 键盘；点按/滚轮/触摸 → 鼠标。
//   只观察、不拦截（始终返回 false）。
class InputSourceWatcher : public QObject
{
public:
    explicit InputSourceWatcher(app::UiModel *vm, QObject *parent = nullptr)
        : QObject(parent), m_vm(vm) {}

protected:
    bool eventFilter(QObject *, QEvent *e) override
    {
        switch (e->type()) {
        case QEvent::KeyPress:
            m_vm->setInputFromKeyboard(true);
            break;
        case QEvent::MouseButtonPress:
        case QEvent::Wheel:
        case QEvent::TouchBegin:
            m_vm->setInputFromKeyboard(false);
            break;
        default:
            break;                      // 鼠标移动不算（免得只晃一下鼠标焦点环就消失）
        }
        return false;
    }

private:
    app::UiModel *m_vm;
};

#ifdef Q_OS_UNIX
namespace {
//   会话注销 / 关机 / 终端挂断：窗口管理器发 SIGTERM（或 SIGHUP）。
//   默认行为是**立刻死掉**，而"退出前补一次自动保存"是在 `~UiModel` 里做的 ⇒ 会丢掉
//   最多一个保存间隔的进度。这里把信号变成一次**正常退出**（自管道 + QSocketNotifier，
//   信号处理器里只 write 一个字节）。
int g_signalFds[2] = {-1, -1};
void onQuitSignal(int)
{
    const char c = 1;
    ssize_t n = ::write(g_signalFds[1], &c, 1);
    (void)n;
}
} // namespace
#endif

// ── 应用级快捷键：只用 Qt 的**标准键**（QKeySequence），不自创键位 ──────────
//   · 退出：`QKeySequence::Quit` = Ctrl+Q（Linux / Windows 一致）
//   · 全屏 / 最大化 / 最小化 / 关闭：**不由应用绑键** —— 那是窗口管理器的协议与职责，
//     应用只经 QWindow 的窗口状态接入（见下面 windowStateChanged 的同步）。
class StandardShortcutFilter : public QObject
{
public:
    explicit StandardShortcutFilter(QObject *parent = nullptr)
        : QObject(parent), m_quit(QKeySequence::keyBindings(QKeySequence::Quit)) {}

protected:
    bool eventFilter(QObject *, QEvent *e) override
    {
        if (e->type() != QEvent::KeyPress) return false;
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->isAutoRepeat()) return false;
        const QKeySequence seq(ke->keyCombination());
        if (m_quit.contains(seq)) { QCoreApplication::quit(); return true; }
        //   全屏**不在这里处理**：它是窗口管理器的事 —— 应用只经 QWindow 的窗口状态
        //   （`Window.FullScreen` ⇒ EWMH `_NET_WM_STATE_FULLSCREEN`）接入协议，
        //   WM 用它自己的快捷键怎么切，由 WM 决定。
        return false;
    }

private:
    QList<QKeySequence> m_quit;
};

#ifndef PHOTOSET_IPC_NAME
#  define PHOTOSET_IPC_NAME "photoset-reader-ipc"
#endif

//   单实例（用户 2026-11-02 选 A）：**第二次启动把路径转发给已运行的实例并退出**。
//   用 `QLocalServer`（Linux 上就是 Unix domain socket，Windows 上是命名管道）。
namespace {

bool forwardToRunningInstance(const QString &path, bool activateOnly)
{
    QLocalSocket sock;
    sock.connectToServer(QString::fromLatin1(PHOTOSET_IPC_NAME), QIODevice::WriteOnly);
    if (!sock.waitForConnected(300))
        return false;                      // 没有实例在跑
    QByteArray msg = path.toUtf8();
    if (activateOnly)
        msg.clear();                       // 空消息 = 只把已有窗口提到前面
    sock.write(msg);
    sock.write("\n");
    sock.flush();
    sock.waitForBytesWritten(300);
    sock.disconnectFromServer();
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    // ★ 分类日志的默认级别 = warning（`logging.h` 的分类默认值）；用户在设置里改的级别
    //   由 `UiModel` 构造时落地，`--log-level` 可临时覆盖一次（不写盘）。
    iblog::setLevel(iblog::Level::Warning);

    // ★ 文件对话框要**系统原生**（用户要求 #1）。这里要选的是**用哪套实现**：
    //   · KDE 桌面 ⇒ `kde`：plasma-integration 提供 QPlatformFileDialogHelper + KFileWidget，
    //     KIO 列目录、缩略图异步，和 Dolphin 同一套；
    //   · 其它桌面 ⇒ `xdgdesktopportal`：走 XDG Desktop Portal。
    //   ⚠️ 实测（2026-11-01）：在 KDE 上把主题设成 `xdgdesktopportal` 时 Qt 反而**回落到
    //     自带对话框**（QFileSystemModel 逐项取图标/类型）：5000 个文件的目录要 ~1s 才铺满，
    //     之后还在零星增长；换成 `kde` 才是 KFileWidget。
    //   ⚠️ 必须在 QGuiApplication **构造之前**设置——Qt 在构造时读取它。
#ifdef PHOTOSET_STATIC_QT
    //   静态版：系统里的 Qt 插件（plasma-integration 的 `kde` 主题）**加载不了**，
    //   所以用 Qt **自带**的 `xdgdesktopportal` 主题 —— 它经 XDG Desktop Portal 弹
    //   **系统对话框**（KDE 下就是 KFileWidget）。这个插件已静态导入，见 CMakeLists。
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORMTHEME"))
        qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
#elif defined(Q_OS_UNIX)
    //   ★ 这套平台主题只对 Linux 桌面有意义（KDE / portal）；Windows 上没有这些插件，
    //     设了只会多一条 "could not find platform theme" 警告。
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORMTHEME")) {
        const bool kde = qgetenv("XDG_CURRENT_DESKTOP").contains("KDE");
        qputenv("QT_QPA_PLATFORMTHEME", kde ? "kde" : "xdgdesktopportal");
    }
#endif
    // ★ Qt Quick Controls 的**风格钉死成 Basic**（可用 `QT_QUICK_CONTROLS_STYLE` 覆盖）。
    //   我们只用到 Controls 的 `Popup` / `ScrollBar`（其余按钮都是自绘的 `components/Button.qml`），
    //   而 KDE 桌面上 Qt 的默认风格是 Fusion ⇒ 静态版要白白多带一套风格模块。
    //   Basic 是 Qt 的兜底风格，任何桌面都可用。⚠️ 必须在加载 QML 之前调用。
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE"))
        QQuickStyle::setStyle(QStringLiteral("Basic"));
    // QApplication（而非 QGuiApplication）：文件对话框走 `QFileDialog`（**只在 ViewModel 里用**，
    //   QML 不写平台相关代码）。QApplication 是 QGuiApplication 的子类，QML 侧完全一致。
    QApplication app(argc, argv);
    //   窗口图标：X11 下任务栏 / Alt-Tab 用它（Wayland 走 .desktop 的 Icon=）。
    //   资源由 CMake 从 `assets/icons/photoset-reader-256.png` 编进来（同一个 SVG 渲染）。
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/photoset-reader-256.png")));

    qCDebug(lcApp).noquote() << QStringLiteral("[app] platform theme: %1")
                                     .arg(QString::fromLocal8Bit(qgetenv("QT_QPA_PLATFORMTHEME")));
    // 字体纪律（用户明确）：一律不设字号 / 字体 / 字体样式，基准字号 = **系统默认**。
    //   ⚠️ 这里刻意不做任何调整：曾按"默认字体加大一号"做过 +1pt，已按用户要求收回。
    //   QML 侧只有 Theme.fontSm = 系统默认 − 1（徽标等次要信息），是**派生**值，跟着走。
    QCoreApplication::setApplicationName(QStringLiteral("photoset-reader"));
    // 配置与库都落在 ~/.config/photoset-reader/（设计 10 §10.1）
    // 旧目录 ~/.config/image-browser/ 按定案放弃、不迁移
    QCoreApplication::setApplicationVersion(QStringLiteral(PHOTOSET_VERSION));

    QCommandLineParser parser;
    //   CLI 文案**一律英文**（与日志同一口径：命令行/日志不做 i18n，界面才做）
    parser.setApplicationDescription(
        QStringLiteral("Image viewer for manga and very large image sets"));
    parser.addHelpOption();
    //   `--version`：报 程序名 + 版本 + Qt 版本（报 bug 时第一句话就要这个）
    parser.addVersionOption();
    //   ★ 产品 CLI 只有"打开路径 + 全屏"（用户 2026-11-04 定）：测试/验收开关一律不进产品。
    //     路径走**位置参数**（launcher / 文件管理器的 `%F` 就是这么传的），不再有 `-d`。
    QCommandLineOption fullscreenOpt(QStringLiteral("fullscreen"),
                                     QStringLiteral("Start in full screen"));
    parser.addOption(fullscreenOpt);
    parser.addPositionalArgument(QStringLiteral("path"),
                                 QStringLiteral("Image folder, archive or file to open"));
    QCommandLineOption logLevelOpt(QStringLiteral("log-level"),
                                   QStringLiteral("Log level for this run: debug|info|warning|error|off"
                                                  " (overrides the setting; not persisted)"),
                                   QStringLiteral("level"));
    parser.addOption(logLevelOpt);
    QCommandLineOption logFileOpt(QStringLiteral("log-file"),
                                  QStringLiteral("Also append logs to this file"
                                                 " (for bug reports; no console needed)"),
                                  QStringLiteral("file"));
    parser.addOption(logFileOpt);
    parser.process(app);

#ifdef Q_OS_UNIX
    //   只在能建出自管道时装（建不出来就退回默认行为，不挡启动）
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, g_signalFds) == 0) {
        auto *sigNotifier = new QSocketNotifier(g_signalFds[0], QSocketNotifier::Read, &app);
        QObject::connect(sigNotifier, &QSocketNotifier::activated, &app, [sigNotifier]() {
            sigNotifier->setEnabled(false);
            char c = 0;
            ssize_t n = ::read(g_signalFds[0], &c, 1);
            (void)n;
            QCoreApplication::quit();
        });
        std::signal(SIGTERM, onQuitSignal);   // 注销 / 关机
        std::signal(SIGINT, onQuitSignal);    // Ctrl+C
        std::signal(SIGHUP, onQuitSignal);    // 终端挂断
    }
#endif

    //   `--log-file`：**尽早**打开（越早的日志越有用）；打不开只警告，不影响启动。
    if (parser.isSet(logFileOpt)) {
        QString err;
        if (!iblog::setLogFile(parser.value(logFileOpt), &err)) {
            qCWarning(lcApp).noquote()
                << QStringLiteral("[app] cannot write --log-file %1: %2")
                       .arg(parser.value(logFileOpt), err);
        }
    }

    //   `--log-level`：**尽早**落地（启动早期的日志也按它过滤）；`UiModel` 构造会按设置再设一次，
    //   所以构造完要**再覆盖回来**（见下面的 `uiLogOverride`）。
    bool uiLogOverride = false;
    iblog::Level cliLogLevel = iblog::Level::Warning;
    if (parser.isSet(logLevelOpt)) {
        if (iblog::levelFromName(parser.value(logLevelOpt), &cliLogLevel)) {
            uiLogOverride = true;
            iblog::setLevel(cliLogLevel);
        } else {
            qCWarning(lcApp).noquote()
                << QStringLiteral("[app] invalid --log-level, expected debug|info|warning|error|off");
        }
    }

    //   来源 = **一个位置参数** —— 文件管理器按 .desktop 的
    //   `Exec=photoset-reader %F` 启动我们时，路径就是以位置参数传进来的。
    QString dir;
    if (!parser.positionalArguments().isEmpty())
        dir = parser.positionalArguments().first();
    //   `.prsave`（阅读进度文件）：**文件管理器双击 / "用…打开"** 也走位置参数（用户 2026-11-04）。
    const bool isProgress = dir.endsWith(QStringLiteral(".prsave"), Qt::CaseInsensitive);
    const bool hasSource = !dir.isEmpty();
    if (hasSource) {
        const QFileInfo fi(dir);
        if (!fi.exists()) {
            qCWarning(lcApp).noquote() << QStringLiteral("[app] source path does not exist: %1").arg(dir);
            return 2;
        }
        if (isProgress) {
            //   进度文件由 `UiModel::openProgress()` 负责（提前返回，不走来源校验）
        }
        //   ★ **用户显式打开的单文件不做任何格式过滤**（用户 2026-11-06）：
        //     "有义务尝试解码" —— 扩展名什么都不是、甚至没有扩展名，也要进解码链试一次。
        //     原来这里有一道 `!isDir && !isArchive && !isImageSuffix ⇒ return 2` 的闸门，
        //     它让一个 `foo.xyz`（其实是张好图）连打开的机会都没有。
        //     收录（目录扫描 / 压缩包条目）仍然按能力表过滤，那是另一回事 —— 见
        //     `Sequence` 的目录分支与 `ArchiveSource` 的条目过滤。
    }

    // ── 单实例（用户 2026-11-02 选 A）：已有实例 ⇒ 把路径转发过去，自己退出 ──
    if (forwardToRunningInstance(hasSource ? dir : QString(), !hasSource)) {
        qCInfo(lcApp).noquote() << QStringLiteral("[app] forwarded to running instance: %1")
                                    .arg(hasSource ? dir : QStringLiteral("activate only"));
        return 0;
    }

    // 解码分配上限（防解压炸弹，但要容得下我们的合法素材）
    //   素材基线（AGENTS.md）：中画幅最高 ~102 MP → 102M×3B ≈ 306 MB
    //   Qt 默认 256 MB **会把合法的 102 MP 照片当炸弹拒掉**，所以必须调高。
    //   设 1 GB：容得下 102 MP（306 MB）与跨页扫图，仍能拦住 2.5 GB 级的炸弹。
    QImageReader::setAllocationLimit(1024);

    // ── 解码能力探测：**这台机器现在能解什么**（用户 2026-11-06 定）──────────────
    //   唯一信息源是 provider 自己（Qt 插件列表 / 系统编解码器枚举 / 外部程序探测），
    //   代码里没有任何格式字面量。结果进 `[cap]` 日志；白名单与文件对话框都从它取。
    //   ★ 放在这里：日志级别已经生效（`--log-level` 解析过、`iblog::setLevel` 调过），
    //     且早于任何扫描 —— 于是"用户装了编解码器却没生效"一眼就能看出来。
    decode::Capabilities::instance().probe();

    QQmlApplicationEngine engine;

    // ★ 设置的 `load()` 移到 `UiModel` 构造里（设计 14 §9.3：运行期设置挂在它身上）——
    //   旧代码在这里 load 的**时机语义不变**：从这里到 `UiModel` 建出来之间，没有任何人读设置。
    // 命令行 `-d <目录>` 属"直接打开"：走文件系统，不建图集库记录（设计 11 §四）

    // 图标字体：C++ 侧注册（比 QML FontLoader 可靠，无异步竞态）
    // 做法对齐 ~/projects/vsr-player（同一作者）
    {
        const int fid = QFontDatabase::addApplicationFont(
            QStringLiteral(":/assets/fonts/materialdesignicons-webfont.ttf"));
        const QString family = (fid >= 0)
            ? QFontDatabase::applicationFontFamilies(fid).value(0)
            : QString();
        engine.rootContext()->setContextProperty(QStringLiteral("iconFont"), family);
        qCDebug(lcApp).noquote() << QStringLiteral("[app] icon font: %1 (id=%2)")
                                     .arg(family.isEmpty() ? QStringLiteral("failed") : family).arg(fid);
    }

    // ★ 2026-10-31：缩略图 provider **已随浏览 / 图集库删除** —— 界面里没有任何缩略图，
    //   也不需要把它暴露给 QML（AGENTS：QML 禁止引用 `provider.*`）。
    Q_UNUSED(dir);   // 只有 `-d` 用得到它（打开来源）；QML 不再读 startDir

    // GPU / CPU 缩放渲染组件（QML 可用）：显式注册。
    //   ★ 实现移到 `src/qmlregister.cpp`（2026-11-06）：**测试进程**同样要加载
    //     `qrc:/qt/qml/AppUi/src/ui/Main.qml`，以前这两句只写在 `main.cpp` 里 ⇒
    //     套件里 QML 报 "CpuScalerItem is not a type"。产品与套件现在共用同一份。
    //   ★ 必须放在**任何 QML 加载之前**（定稿视图 FullscreenView 的 `GpuScalerItem` 要用它）
    app::registerQmlTypes();

    // ── 全局 VM 装配（放在所有"跑完即退"的路径之后）──────────
    //   ★ **全局只有一个 VM** = `app::UiModel`（QML 里的 `ui`）——viewer（`ui.viewer.*`）由它持有；
    //     它自己就是 QML 侧的类型信息不需要额外注册（只有一个界面之后没有枚举要给 QML 读）。
    auto *ui = new app::UiModel(&app);
    if (uiLogOverride) iblog::setLevel(cliLogLevel);   // CLI 覆盖设置（不写盘）
    //   时间轴悬停预览：provider 只读位置表 + 解码缩略图（位置表归 UiModel ⇒ 换来源也不会悬空）
    engine.addImageProvider(QStringLiteral("preview"), new PreviewProvider(ui->previewStore()));
    //   ★ 换语言：`UiModel` 负责装/摘 `QTranslator`，**界面重算在本行** ——
    //     Qt 6 里 `installTranslator` **不会**让 QML 的 `qsTr()` 绑定自动重算（实测：
    //     对话框文字保持旧语言），必须显式 `QQmlEngine::retranslate()`。
    //     顺序有要求：`languageChanged` 是在新翻译装好**之后**才发的。
    QObject::connect(ui, &app::UiModel::languageChanged, &engine,
                     [&engine]() { engine.retranslate(); });

    engine.rootContext()->setContextProperty(QStringLiteral("ui"), ui);
    // focus-visible：全局记录输入来源
    qApp->installEventFilter(new InputSourceWatcher(ui, qApp));
    // 系统配色变化 → 通知 QML（"跟随系统"模式下需要）
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                     ui, [ui]() { emit ui->systemDarkChanged(); });

    // 命令行 `-d <路径>` = **打开来源**（文件夹 / 压缩包 / 文件同一套）；
    //   `--index N` 定位到第 N 条（不给 = 第 0 条）。只有一个界面 ⇒ 不需要再切屏。
    if (hasSource) {
        if (isProgress) ui->openProgress(dir);
        else            ui->openPath(dir);
    }

    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/AppUi/src/ui/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        qCWarning(lcApp) << QStringLiteral("[app] QML load failed");
        return 1;
    }

    // ── 单实例的服务端：收第二条实例转发来的路径（空串 = 只激活窗口）──
    auto *ipc = new QLocalServer(&app);
    QLocalServer::removeServer(QString::fromLatin1(PHOTOSET_IPC_NAME));   // 清掉上次留下的 socket
    if (!ipc->listen(QString::fromLatin1(PHOTOSET_IPC_NAME))) {
        qCWarning(lcApp).noquote() << QStringLiteral("[app] ipc listen failed: %1").arg(ipc->errorString());
    } else {
        QQuickWindow *rootWin = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QObject::connect(ipc, &QLocalServer::newConnection, ipc, [ipc, ui, rootWin]() {
            while (QLocalSocket *sock = ipc->nextPendingConnection()) {
                if (sock->waitForReadyRead(300)) {
                    const QString path = QString::fromUtf8(sock->readAll()).trimmed();
                    if (!path.isEmpty()) {
                        qCInfo(lcApp).noquote() << QStringLiteral("[app] ipc received path: %1").arg(path);
                        if (path.endsWith(QStringLiteral(".prsave"), Qt::CaseInsensitive))
                            ui->openProgress(path);
                        else
                            ui->openPath(path);
                    }
                }
                if (rootWin) {
                    rootWin->show();
                    rootWin->raise();
                    rootWin->requestActivate();
                }
                sock->disconnectFromServer();
                sock->deleteLater();
            }
        });
    }

    // ── 场景图后端是不是 software？⇒ 告诉 VM（QML 据此把阅读屏切到 CpuScalerItem）──
    //   只能等场景图建好才问得准（`QQuickWindow::rendererInterface()`）；软件后端下
    //   `QQuickRhiItem` 什么都不画 ⇒ 没有这条，阅读屏在 llvmpipe / 强制 software 时是黑的。
    if (QQuickWindow *win = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0))) {
        auto probeBackend = [win, ui]() {
            // ★ 本回调会被调两次（`sceneGraphInitialized` 与下面那次立即探测）⇒ 降级只报一次
            static bool fallbackReported = false;
            if (auto *ri = win->rendererInterface()) {
                const bool sw = (ri->graphicsApi() == QSGRendererInterface::Software);
                if (sw && !fallbackReported) {
                    fallbackReported = true;
                    qCWarning(lcApp).noquote() << QStringLiteral(
                        "[app] scene graph backend is software; reader scaling falls back to Smooth (Qt)");
                }
                ui->setSoftwareBackend(sw);
            }
        };
        win->installEventFilter(new StandardShortcutFilter(win));   // 退出用平台标准键（Ctrl+Q）
        //   全屏是**窗口管理器协议**：WM 改窗口状态（它自己的快捷键 / 最大化按钮）时，
        //   把状态回写到 VM，界面图标与行为跟着走；应用侧只发请求（QML 设 visibility）。
        QObject::connect(win, &QWindow::windowStateChanged, ui, [ui](Qt::WindowState st) {
            ui->setWindowFullscreen((st & Qt::WindowFullScreen) == Qt::WindowFullScreen);
        });
        QObject::connect(win, &QQuickWindow::sceneGraphInitialized, ui, probeBackend);
        probeBackend();          // 已经初始化过的情况（例如窗口已 expose）
    }

    //   `--fullscreen`：窗口已建好、QML 的绑定也接好了 —— 此时改状态会触发 QML 侧同步
    if (parser.isSet(fullscreenOpt))
        ui->setWindowFullscreen(true);

    const int rc = app.exec();
    //   ★ 预览缩略图的批量解码跑在全局线程池里：退出前**有界等待**，避免进程退出时
    //     还有解码在跑（与库的静态析构撞车）。等不到也走，不卡关窗。
    QThreadPool::globalInstance()->waitForDone(300);
    return rc;
}
