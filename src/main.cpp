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

#include "cpuscaleritem.h"
#include "logging.h"
#include "gpuscaleritem.h"
#include "archive.h"

#include "natural.h"
#include "uimodel.h"
#include "previewprovider.h"
#include "screenshot.h"
#include "devcli.h"
#include "devbench.h"

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
    // ★ 分类日志的**最早**初始化：本文件下面第一条诊断日志之前就要落地
    //   （`PHOTOSET_LOG` 决定 `ib.*` 的最低级别；默认 info）。
    iblog::init();

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
#else
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
    QCommandLineOption dirOpt({QStringLiteral("d"), QStringLiteral("dir")},
                              QStringLiteral("Image folder, archive or file"), QStringLiteral("path"),
                              QString());   // ★ 默认空：只有**显式传 -d** 才打开来源，否则窗口空白
    QCommandLineOption shotOpt(QStringLiteral("shot"),
                               QStringLiteral("Render one frame to this PNG and exit (self-check)"),
                               QStringLiteral("png"));
    QCommandLineOption delayOpt(QStringLiteral("delay"),
                                QStringLiteral("Delay before the screenshot in ms (default 2500)"),
                                QStringLiteral("ms"), QStringLiteral("2500"));
    QCommandLineOption idxOpt(QStringLiteral("index"),
                              QStringLiteral("Jump to item N when opening the source"),
                              QStringLiteral("n"), QStringLiteral("-1"));
    QCommandLineOption seekOpt(QStringLiteral("seek-test"),
                               QStringLiteral("Jump to page N and report the result"),
                               QStringLiteral("n"), QStringLiteral("-1"));
    QCommandLineOption gpuProbeOpt(QStringLiteral("gpuscale-probe"),
                                   QStringLiteral("Diagnostic: push a known 8x8 image through the GPU 1:1 pass and print pixels"));
    // 开发期：**强制走 CPU 缩放**（= 假装软件后端）——在有 GPU 的机器上验收降级画面用
    QCommandLineOption forceCpuOpt(QStringLiteral("force-cpu-scale"),
                                   QStringLiteral("Debug: force CPU scaling (Smooth (Qt))"));
    // ── 调试：启动即打开设置对话框（仅开发用，不在正式 CLI 范畴）──
    //   ★ 接线（2026-10-14 本批）：值经上下文属性 `devOpenSettings` / `devSettingsTab`
    //     在 **QML 加载之前**注入 `Main.qml`，再由它喂两屏各自的 `debugOpenSettings`
    //     （两屏都**只在当前屏**打开自己的对话框）。默认 false / 0 ⇒ 正常运行时零行为变化。
    //     ⚠️ 必须"加载前"给值：各屏是 `Component.onCompleted` 里开对话框的，加载后再置属性无效。
    //   ★ **必须是裸开关**：`QCommandLineParser` **没有"可选值"**这个概念 —— 带
    //     `valueName` 的选项会**吞掉下一个 token**（实测 `--debug-settings --shot x.png`
    //     把 `--shot` 当成了它的值 ⇒ 截图路径拿不到、进程不退出）。页签因此单列一个选项。
    //   ★ 页签 QML 侧**支持**：`SettingsDialog.tab`（0常规/1图像/2系统/3快捷键）。
    QCommandLineOption debugSettingsOpt(QStringLiteral("debug-settings"),
                                        QStringLiteral("Debug: open the settings dialog on startup"));
    QCommandLineOption debugSettingsTabOpt(QStringLiteral("debug-settings-tab"),
                                           QStringLiteral("Debug: which settings tab to open"
                                                          "（0常规/1图像/2系统/3快捷键）"),
                                           QStringLiteral("tab"), QStringLiteral("0"));
    // ── 验收用（截图 / 临时探针；不参与正常使用）──
    QCommandLineOption probeArcOpt(QStringLiteral("probe-archive"),
                                   QStringLiteral("Probe: measure the cost of archive dataAt(i)"),
                                   QStringLiteral("path"));
    QCommandLineOption probeLaOpt(QStringLiteral("probe-libarchive"),
                                  QStringLiteral("Probe libarchive for random access support"),
                                  QStringLiteral("path"));
    QCommandLineOption osdOpt(QStringLiteral("show-osd"),
                              QStringLiteral("Open the info panel before the screenshot"));
    QCommandLineOption popupOpt(QStringLiteral("show-interval-popup"),
                                QStringLiteral("Open the slideshow-interval popup before the screenshot"));
    // ── 开发期专用（**只为验收截图**）──
    QCommandLineOption devContextMenuOpt(QStringLiteral("dev-context-menu"),
                                         QStringLiteral("Debug: open the context menu on startup (screenshots)"));
    QCommandLineOption devSaveFavOpt(QStringLiteral("dev-save-favorites"),
                                     QStringLiteral("Debug: save progress to favorites shortly after opening"));
    QCommandLineOption devOpenProgressOpt(QStringLiteral("dev-open-progress"),
                                          QStringLiteral("Debug: open this progress file on startup"),
                                          QStringLiteral("path"));
    QCommandLineOption devCancelScanOpt(QStringLiteral("dev-cancel-scan"),
                                        QStringLiteral("Debug: cancel the scan after opening (incomplete state)"));
    QCommandLineOption devRescanOpt(QStringLiteral("dev-rescan"),
                                    QStringLiteral("Debug: rescan once after opening"));
    QCommandLineOption devDialogOpt(QStringLiteral("dev-dialog"),
                                    QStringLiteral("Debug: open a specific dialog on startup"
                                                   "（scan-not-done / cancel-scan / unfavorite）"),
                                    QStringLiteral("name"));
    QCommandLineOption benchVerOpt(QStringLiteral("bench-slideshow"),
                                   QStringLiteral("Measure slideshow throughput at this interval (ms)"),
                                   QStringLiteral("ms"));
    QCommandLineOption benchSecOpt(QStringLiteral("bench-seconds"),
                                   QStringLiteral("Measurement window for --bench-slideshow (default 5)"),
                                   QStringLiteral("s"), QStringLiteral("5"));
    QCommandLineOption devNotifyOpt(QStringLiteral("dev-notify"),
                                    QStringLiteral("Debug: show a toast on startup"),
                                    QStringLiteral("text"));
    parser.addOption(dirOpt);
    parser.addOption(shotOpt);
    parser.addOption(delayOpt);
    parser.addOption(idxOpt);
    parser.addOption(probeArcOpt);
    parser.addOption(probeLaOpt);
    parser.addOption(popupOpt);
    parser.addOption(osdOpt);
    parser.addOption(seekOpt);
    parser.addOption(devNotifyOpt);
    parser.addOption(benchVerOpt);
    parser.addOption(benchSecOpt);
    parser.addOption(devContextMenuOpt);
    parser.addOption(devSaveFavOpt);
    parser.addOption(devOpenProgressOpt);
    parser.addOption(devCancelScanOpt);
    parser.addOption(devRescanOpt);
    parser.addOption(devDialogOpt);
    parser.addOption(debugSettingsOpt);
    parser.addOption(debugSettingsTabOpt);
    parser.addOption(gpuProbeOpt);
    parser.addOption(forceCpuOpt);
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

    if (parser.isSet(probeArcOpt))
        return devcli::runArchiveProbe(parser.value(probeArcOpt));
    if (parser.isSet(probeLaOpt))
        return devcli::runLibarchiveProbe(parser.value(probeLaOpt));

    //   来源 = `-d <路径>`，或**一个位置参数** —— 文件管理器按 .desktop 的
    //   `Exec=photoset-reader %F` 启动我们时，路径就是以位置参数传进来的。
    QString dir = parser.value(dirOpt);
    if (dir.isEmpty() && !parser.positionalArguments().isEmpty())
        dir = parser.positionalArguments().first();
    const bool hasSource = !dir.isEmpty();
    if (hasSource) {
        const QFileInfo fi(dir);
        if (!fi.exists()) {
            qCWarning(lcApp).noquote() << QStringLiteral("[app] source path does not exist: %1").arg(dir);
            return 2;
        }
        if (!fi.isDir() && !ArchiveSource::isArchive(dir) && !isImageSuffix(fi.suffix())) {
            qCWarning(lcApp).noquote() << QStringLiteral("[app] source is neither folder, archive nor image: %1").arg(dir);
            return 2;
        }
    }

    // ── 单实例（用户 2026-11-02 选 A）：已有实例 ⇒ 把路径转发过去，自己退出 ──
    //   开发自检可用 `PHOTOSET_NO_SINGLE_INSTANCE=1` 关掉（要同时跑两个实例截图时用）。
    if (qEnvironmentVariableIsEmpty("PHOTOSET_NO_SINGLE_INSTANCE")) {
        if (forwardToRunningInstance(hasSource ? dir : QString(), !hasSource)) {
            qCInfo(lcApp).noquote() << QStringLiteral("[app] forwarded to running instance: %1")
                                        .arg(hasSource ? dir : QStringLiteral("activate only"));
            return 0;
        }
    }

    // 解码分配上限（防解压炸弹，但要容得下我们的合法素材）
    //   素材基线（AGENTS.md）：中画幅最高 ~102 MP → 102M×3B ≈ 306 MB
    //   Qt 默认 256 MB **会把合法的 102 MP 照片当炸弹拒掉**，所以必须调高。
    //   设 1 GB：容得下 102 MP（306 MB）与跨页扫图，仍能拦住 2.5 GB 级的炸弹。
    QImageReader::setAllocationLimit(1024);

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

    // GPU 缩放渲染组件（QML 可用）：显式注册。
    //   （不用 QML_ELEMENT：那会让生成的注册文件按 `<gpuscaleritem.h>` 找头文件，
    //     而 src/ 一旦进包含路径，libarchive 的 <archive.h> 就会被我们的 src/archive.h 顶掉）
    //   ★ 必须放在**任何 QML 加载之前**（定稿视图 FullscreenView 的 `GpuScalerItem` 要用它）
    qmlRegisterType<GpuScalerItem>("AppUi", 1, 0, "GpuScalerItem");
    //   CPU 缩放项：**Smooth (Qt) 这一项**与**软件后端降级**都用它（设计 16 §五）
    qmlRegisterType<CpuScalerItem>("AppUi", 1, 0, "CpuScalerItem");

    // ── bench / 探针（**跑完即退**）：解析留在本文件，实现与装配在 `src/devbench.cpp` ──
    //   `main.cpp` 只把 `QCommandLineOption` 的取值装进请求结构（判"给没给"用 `xxxSet`）。
    devbench::Request bench;
    bench.gpuProbe = parser.isSet(gpuProbeOpt);

    // ── bench / 探针跑完即退（不建 VM、不碰设置）──
    if (bench.wanted())
        return devbench::runBench(bench);

    // ── 全局 VM 装配（放在所有"跑完即退"的路径之后）──────────
    //   ★ **全局只有一个 VM** = `app::UiModel`（QML 里的 `ui`）——viewer（`ui.viewer.*`）由它持有；
    //     它自己就是 QML 侧的类型信息不需要额外注册（只有一个界面之后没有枚举要给 QML 读）。
    auto *ui = new app::UiModel(&app);
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
    if (hasSource)
        ui->openPath(dir, parser.value(idxOpt).toInt());

    // ── 开发期专用 CLI（**只为验收截图**）──
    //   `--dev-notify <text>`：启动后弹一条瞬时提示（验收浮层用）
    //   ★ 实现整段在 `src/devcli.cpp`（本文件只解析 + 转发）
    devcli::DevRequest dev;
    dev.notifySet = parser.isSet(devNotifyOpt);
    dev.notify    = parser.value(devNotifyOpt);
    devcli::runDevCommands(dev, ui);

    // ── `--debug-settings`（开发期）：**在 QML 加载前**把值注入上下文属性 ──────────
    //   两屏各自的 `debugOpenSettings` / `debugSettingsTab` 由 `Main.qml` 从这里喂
    //   （每屏只在自己的 `Component.onCompleted` 开自己的对话框 —— 时机就在加载期）。
    //   ★ 默认 false / 0 ⇒ 正常运行时**零行为变化**。
    engine.rootContext()->setContextProperty(QStringLiteral("devOpenSettings"),
                                             parser.isSet(debugSettingsOpt));
    engine.rootContext()->setContextProperty(QStringLiteral("devSettingsTab"),
                                             parser.value(debugSettingsTabOpt).toInt());
    //   ★ `--show-interval-popup`（截图自检用）：同样默认 false ⇒ 正常运行时零行为变化；
    //     由阅读屏自己在可见时把"切换速度"菜单打开（旧实现调的是已删除的旧 QML 方法）。
    engine.rootContext()->setContextProperty(QStringLiteral("devShowIntervalPopup"),
                                             parser.isSet(popupOpt));
    engine.rootContext()->setContextProperty(QStringLiteral("devContextMenu"),
                                             parser.isSet(devContextMenuOpt));
    engine.rootContext()->setContextProperty(QStringLiteral("devDialog"),
                                             parser.value(devDialogOpt));
    if (parser.isSet(devOpenProgressOpt)) {
        QTimer::singleShot(600, ui, [ui, p = parser.value(devOpenProgressOpt)] {
            qCInfo(lcApp).noquote() << QStringLiteral("[app] dev: open progress file: %1")
                                        .arg(ui->openProgress(p) ? QStringLiteral("ok")
                                                                 : QStringLiteral("failed"));
        });
    }
    if (parser.isSet(devRescanOpt)) {
        const int at = qEnvironmentVariableIntValue("PHOTOSET_DEV_RESCAN_MS");
        QTimer::singleShot(at > 0 ? at : 800, ui, [ui] {
            qCInfo(lcApp).noquote() << QStringLiteral("[app] dev: rescan requested");
            ui->rescanSource();
        });
    }
    if (parser.isSet(devCancelScanOpt)) {
        const int at = qEnvironmentVariableIntValue("PHOTOSET_DEV_CANCEL_MS");
        QTimer::singleShot(at > 0 ? at : 300, ui, [ui] {
            ui->cancelScan();
            qCInfo(lcApp).noquote() << QStringLiteral("[app] dev: scan stop requested");
        });
    }
    if (parser.isSet(devSaveFavOpt)) {
        QTimer::singleShot(1200, ui, [ui] {
            qCInfo(lcApp).noquote() << QStringLiteral("[app] dev: save progress to favorites: %1")
                                        .arg(ui->saveProgressToFavorites(false) ? QStringLiteral("ok")
                                                                           : QStringLiteral("failed"));
        });
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
        if (parser.isSet(forceCpuOpt)) {
            qCInfo(lcApp).noquote()
                << QStringLiteral("[app] --force-cpu-scale: using software backend (CPU scaling)");
            ui->setSoftwareBackend(true);
        }
    }

    if (parser.isSet(benchVerOpt))
        devcli::runSlideshowBench(ui, parser.value(benchVerOpt).toInt(),
                                  qMax(1, parser.value(benchSecOpt).toInt()));

    const QString shot = parser.value(shotOpt);
    if (!shot.isEmpty())
        screenshot::setupShot(app, engine, shot, parser.value(delayOpt).toInt(),
                        parser.value(idxOpt).toInt(),
                        parser.isSet(popupOpt), parser.isSet(osdOpt), parser.value(seekOpt).toInt(), ui);

    const int rc = app.exec();
    //   ★ 预览缩略图的批量解码跑在全局线程池里：退出前**有界等待**，避免进程退出时
    //     还有解码在跑（与库的静态析构撞车）。等不到也走，不卡关窗。
    QThreadPool::globalInstance()->waitForDone(300);
    return rc;
}
