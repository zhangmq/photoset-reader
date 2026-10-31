#include <QGuiApplication>
#include <QApplication>          // 文件对话框用 QFileDialog（QtWidgets）；QApplication ⊂ QGuiApplication
#include <QStyleHints>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QQmlContext>
#include <QCommandLineParser>
#include <QImageReader>
#include <QFileInfo>
#include <QFontDatabase>
#include <QEvent>

#include "cpuscaleritem.h"
#include "gpuscaleritem.h"
#include "archive.h"

#include "natural.h"
#include "uimodel.h"
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

int main(int argc, char *argv[])
{
    // ★ 强制使用**系统原生**文件对话框（用户要求 #1）。
    //   本机有 xdg-desktop-portal + KDE/GTK 后端 + Qt 的 libqxdgdesktopportal.so，
    //   但 QT_QPA_PLATFORMTHEME 未设置时 Qt 会回退到**自带**对话框。
    //   ⚠️ 必须在 QGuiApplication **构造之前**设置——Qt 在构造时读取它。
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORMTHEME"))
        qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
    // QApplication（而非 QGuiApplication）：文件对话框走 `QFileDialog`（**只在 ViewModel 里用**，
    //   QML 不写平台相关代码）。QApplication 是 QGuiApplication 的子类，QML 侧完全一致。
    QApplication app(argc, argv);

    qInfo().noquote() << QStringLiteral("[ui] 平台主题 = %1（决定文件对话框是否走系统原生）")
                             .arg(QString::fromLocal8Bit(qgetenv("QT_QPA_PLATFORMTHEME")));
    // 字体纪律（用户明确）：一律不设字号 / 字体 / 字体样式，基准字号 = **系统默认**。
    //   ⚠️ 这里刻意不做任何调整：曾按"默认字体加大一号"做过 +1pt，已按用户要求收回。
    //   QML 侧只有 Theme.fontSm = 系统默认 − 1（徽标等次要信息），是**派生**值，跟着走。
    QCoreApplication::setApplicationName(QStringLiteral("photoset-reader"));
    // 配置与库都落在 ~/.config/photoset-reader/（设计 10 §10.1）
    // 旧目录 ~/.config/image-browser/ 按定案放弃、不迁移
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1-dev"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("image-browser（开发期最小壳）"));
    parser.addHelpOption();
    QCommandLineOption dirOpt({QStringLiteral("d"), QStringLiteral("dir")},
                              QStringLiteral("图片目录"), QStringLiteral("path"),
                              QString());   // ★ 默认空：只有**显式传 -d** 才打开来源，否则窗口空白
    QCommandLineOption shotOpt(QStringLiteral("shot"),
                               QStringLiteral("离屏截图到文件后退出（开发期自检用）"),
                               QStringLiteral("png"));
    QCommandLineOption delayOpt(QStringLiteral("delay"),
                                QStringLiteral("截图前等待毫秒（默认 2500）"),
                                QStringLiteral("ms"), QStringLiteral("2500"));
    QCommandLineOption idxOpt(QStringLiteral("index"),
                              QStringLiteral("打开来源时定位到第 N 条（截图前也会跳到这里）"),
                              QStringLiteral("n"), QStringLiteral("-1"));
    QCommandLineOption pwOpt(QStringLiteral("password"),
                             QStringLiteral("压缩包密码"), QStringLiteral("pw"));
    QCommandLineOption seekOpt(QStringLiteral("seek-test"),
                               QStringLiteral("跳到第 N 页并报告结果（验证进度条点击）"),
                               QStringLiteral("n"), QStringLiteral("-1"));
    QCommandLineOption gpuProbeOpt(QStringLiteral("gpuscale-probe"),
                                   QStringLiteral("诊断：8×8 已知图跑 GPU 1:1 直通并逐像素打印（不参与自检）"));
    // 开发期：**强制走 CPU 缩放**（= 假装软件后端）——在有 GPU 的机器上验收降级画面用
    QCommandLineOption forceCpuOpt(QStringLiteral("force-cpu-scale"),
                                   QStringLiteral("调试：强制走 CPU 缩放（Smooth (Qt)），用于验收软件后端画面"));
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
                                        QStringLiteral("调试：启动即打开设置对话框"));
    QCommandLineOption debugSettingsTabOpt(QStringLiteral("debug-settings-tab"),
                                           QStringLiteral("调试：设置对话框开在哪一页"
                                                          "（0常规/1图像/2系统/3快捷键）"),
                                           QStringLiteral("tab"), QStringLiteral("0"));
    // ── 验收用（截图 / 临时探针；不参与正常使用）──
    QCommandLineOption probeArcOpt(QStringLiteral("probe-archive"),
                                   QStringLiteral("临时探针：量压缩包 dataAt(i) 的代价"),
                                   QStringLiteral("路径"));
    QCommandLineOption probeLaOpt(QStringLiteral("probe-libarchive"),
                                  QStringLiteral("libarchive 能力探针（能否随机读）"),
                                  QStringLiteral("路径"));
    QCommandLineOption osdOpt(QStringLiteral("show-osd"),
                              QStringLiteral("截图前打开信息面板（验证 #7）"));
    QCommandLineOption popupOpt(QStringLiteral("show-interval-popup"),
                                QStringLiteral("截图前打开翻页间隔 popup"));
    // ── 开发期专用（**只为验收截图**）──
    QCommandLineOption devNotifyOpt(QStringLiteral("dev-notify"),
                                    QStringLiteral("开发：启动后弹一条**瞬时提示**（验收浮层用）"),
                                    QStringLiteral("text"));
    parser.addOption(dirOpt);
    parser.addOption(shotOpt);
    parser.addOption(delayOpt);
    parser.addOption(idxOpt);
    parser.addOption(probeArcOpt);
    parser.addOption(probeLaOpt);
    parser.addOption(pwOpt);
    parser.addOption(popupOpt);
    parser.addOption(osdOpt);
    parser.addOption(seekOpt);
    parser.addOption(devNotifyOpt);
    parser.addOption(debugSettingsOpt);
    parser.addOption(debugSettingsTabOpt);
    parser.addOption(gpuProbeOpt);
    parser.addOption(forceCpuOpt);
    parser.process(app);

    if (parser.isSet(probeArcOpt))
        return devcli::runArchiveProbe(parser.value(probeArcOpt));
    if (parser.isSet(probeLaOpt))
        return devcli::runLibarchiveProbe(parser.value(probeLaOpt));

    const QString dir = parser.value(dirOpt);
    if (parser.isSet(dirOpt)) {
        const QFileInfo fi(dir);
        if (!fi.exists()) {
            qWarning().noquote() << QStringLiteral("路径不存在: %1").arg(dir);
            return 2;
        }
        if (!fi.isDir() && !ArchiveSource::isArchive(dir) && !isImageSuffix(fi.suffix())) {
            qWarning().noquote() << QStringLiteral("既不是目录、压缩包，也不是图片: %1").arg(dir);
            return 2;
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
        qInfo().noquote() << QStringLiteral("[font] 图标字体: %1 (id=%2)")
                                 .arg(family.isEmpty() ? QStringLiteral("失败") : family).arg(fid);
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
    engine.rootContext()->setContextProperty(QStringLiteral("ui"), ui);
    // focus-visible：全局记录输入来源
    qApp->installEventFilter(new InputSourceWatcher(ui, qApp));
    // 系统配色变化 → 通知 QML（"跟随系统"模式下需要）
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                     ui, [ui]() { emit ui->systemDarkChanged(); });

    // 命令行 `-d <路径>` = **打开来源**（文件夹 / 压缩包 / 文件同一套）；
    //   `--index N` 定位到第 N 条（不给 = 第 0 条）。只有一个界面 ⇒ 不需要再切屏。
    if (parser.isSet(dirOpt))
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

    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/AppUi/src/ui/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        qWarning() << "QML 加载失败";
        return 1;
    }

    // ── 场景图后端是不是 software？⇒ 告诉 VM（QML 据此把阅读屏切到 CpuScalerItem）──
    //   只能等场景图建好才问得准（`QQuickWindow::rendererInterface()`）；软件后端下
    //   `QQuickRhiItem` 什么都不画 ⇒ 没有这条，阅读屏在 llvmpipe / 强制 software 时是黑的。
    if (QQuickWindow *win = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0))) {
        auto probeBackend = [win, ui]() {
            if (auto *ri = win->rendererInterface()) {
                const bool sw = (ri->graphicsApi() == QSGRendererInterface::Software);
                if (sw)
                    qInfo().noquote() << QStringLiteral(
                        "[scale] 场景图后端 = software ⇒ 阅读屏缩放降级为 Smooth (Qt)");
                ui->setSoftwareBackend(sw);
            }
        };
        QObject::connect(win, &QQuickWindow::sceneGraphInitialized, ui, probeBackend);
        probeBackend();          // 已经初始化过的情况（例如窗口已 expose）
        if (parser.isSet(forceCpuOpt)) {
            qInfo().noquote() << QStringLiteral("[scale] --force-cpu-scale ⇒ 按软件后端处理（走 CPU 缩放）");
            ui->setSoftwareBackend(true);
        }
    }

    const QString shot = parser.value(shotOpt);
    if (!shot.isEmpty())
        screenshot::setupShot(app, engine, shot, parser.value(delayOpt).toInt(),
                        parser.value(idxOpt).toInt(),
                        parser.isSet(popupOpt), parser.isSet(osdOpt), parser.value(seekOpt).toInt(), ui);

    return app.exec();
}
