#include <QGuiApplication>
#include <QApplication>          // 文件对话框用 QFileDialog（QtWidgets）；QApplication ⊂ QGuiApplication
#include <QStyleHints>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QCommandLineParser>
#include <QImageReader>
#include <QFileInfo>
#include <QFontDatabase>
#include <QEvent>

#include "imageprovider.h"
#include "scaler.h"
#include "gpuscaleritem.h"
#include "archive.h"

#include "natural.h"
#include "appsettings.h"
#include "thumburl.h"
#include "librarystore.h"
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
                              QString());   // ★ 默认空：只有**显式传 -d** 才建来源，否则启动即图集库
    QCommandLineOption shotOpt(QStringLiteral("shot"),
                               QStringLiteral("离屏截图到文件后退出（开发期自检用）"),
                               QStringLiteral("png"));
    QCommandLineOption delayOpt(QStringLiteral("delay"),
                                QStringLiteral("截图前等待毫秒（默认 2500）"),
                                QStringLiteral("ms"), QStringLiteral("2500"));
    QCommandLineOption idxOpt(QStringLiteral("index"),
                              QStringLiteral("截图前跳到第 N 张"), QStringLiteral("n"),
                              QStringLiteral("-1"));
    QCommandLineOption immOpt(QStringLiteral("immersive"),
                              QStringLiteral("截图前切到沉浸视图"));
    QCommandLineOption scalerOpt(QStringLiteral("scaler"),
                                 QStringLiteral("缩放路径: linear-light | qt-smooth | nearest"),
                                 QStringLiteral("mode"), QStringLiteral("linear-light"));
    QCommandLineOption qualityOpt(QStringLiteral("quality"),
                                  QStringLiteral("画质档位: fast | balanced | best"),
                                  QStringLiteral("q"), QStringLiteral("balanced"));
    QCommandLineOption pwOpt(QStringLiteral("password"),
                             QStringLiteral("压缩包密码"), QStringLiteral("pw"));
    QCommandLineOption benchOpt(QStringLiteral("bench-thumbs"),
                                QStringLiteral("测 N 张缩略图的解码+缩放耗时（隔离渲染）"),
                                QStringLiteral("n"));
    QCommandLineOption seekOpt(QStringLiteral("seek-test"),
                               QStringLiteral("跳到第 N 页并报告结果（验证进度条点击）"),
                               QStringLiteral("n"), QStringLiteral("-1"));
    // ── 并行度实验（docs/research/14）：解码 / 解码+缩放 / 只缩放，**排除 I/O** ──
    QCommandLineOption parOpt(QStringLiteral("bench-parallel"),
                              QStringLiteral("并行度实验：先把 N 张图的字节读进内存，再测解码/缩放"),
                              QStringLiteral("n"));
    QCommandLineOption threadsOpt(QStringLiteral("threads"),
                                  QStringLiteral("并行度实验的线程数列表（如 1,2,4,6,8）"),
                                  QStringLiteral("list"), QStringLiteral("1,2,4,6,8,14,20"));
    QCommandLineOption presetOpt(QStringLiteral("preset"),
                                 QStringLiteral("缩放预设：manga | photo | sharp | soft | fast"),
                                 QStringLiteral("key"));
    QCommandLineOption lobesOpt(QStringLiteral("lobes"),
                                QStringLiteral("Lanczos 瓣数 2/3/4"), QStringLiteral("n"));
    QCommandLineOption mbOpt(QStringLiteral("mitchell-b"), QStringLiteral("Mitchell B"), QStringLiteral("f"));
    QCommandLineOption mcOpt(QStringLiteral("mitchell-c"), QStringLiteral("Mitchell C"), QStringLiteral("f"));
    QCommandLineOption arOpt(QStringLiteral("antiring"), QStringLiteral("抗振铃强度 0–1"), QStringLiteral("f"));
    QCommandLineOption filterOpt(QStringLiteral("filter"),
                                 QStringLiteral("滤波器族：auto | mitchell（照片）"),
                                 QStringLiteral("name"), QStringLiteral("auto"));
    QCommandLineOption gpuOpt(QStringLiteral("bench-gpu"),
                              QStringLiteral("GPU 缩放实验：上传 + 分离式 Lanczos（线性光）vs CPU 缩放器"),
                              QStringLiteral("n"));
    QCommandLineOption gpuProbeOpt(QStringLiteral("gpuscale-probe"),
                                   QStringLiteral("诊断：8×8 已知图跑 GPU 1:1 直通并逐像素打印（不参与自检）"));
    QCommandLineOption repeatsOpt(QStringLiteral("repeats"),
                                  QStringLiteral("并行度实验每组重复次数（取中位数）"),
                                  QStringLiteral("n"), QStringLiteral("3"));
    // ── 调试：启动即打开设置对话框（仅开发用，不在正式 CLI 范畴）──
    //   ★ 接线（2026-10-14 本批）：值经上下文属性 `devOpenSettings` / `devSettingsTab`
    //     在 **QML 加载之前**注入 `Main.qml`，再由它喂三屏各自的 `debugOpenSettings`
    //     （三屏都**只在当前屏**打开自己的对话框）。默认 false / 0 ⇒ 正常运行时零行为变化。
    //     ⚠️ 必须"加载前"给值：各屏是 `Component.onCompleted` 里开对话框的，加载后再置属性无效。
    //   ★ **必须是裸开关**：`QCommandLineParser` **没有"可选值"**这个概念 —— 带
    //     `valueName` 的选项会**吞掉下一个 token**（实测 `--debug-settings --shot x.png`
    //     把 `--shot` 当成了它的值 ⇒ 截图路径拿不到、进程不退出）。页签因此单列一个选项。
    //   ★ 页签 QML 侧**支持**：`SettingsDialog.tab`（0常规/1图像/2系统/3快捷键），BrowseView 早已这么用。
    QCommandLineOption debugSettingsOpt(QStringLiteral("debug-settings"),
                                        QStringLiteral("调试：启动即打开设置对话框"));
    QCommandLineOption debugSettingsTabOpt(QStringLiteral("debug-settings-tab"),
                                           QStringLiteral("调试：设置对话框开在哪一页"
                                                          "（0常规/1图像/2系统/3快捷键）"),
                                           QStringLiteral("tab"), QStringLiteral("0"));
    // ── 开发期专用（**只为验收截图**；与 --probe-* / --bench-* 同一风格）──
    QCommandLineOption devAddOpt(QStringLiteral("dev-add-collection"),
                                 QStringLiteral("开发：把目录加入（临时）图集库并等扫描结束"),
                                 QStringLiteral("dir"));
    QCommandLineOption devOpenCollOpt(QStringLiteral("dev-open-collection"),
                                      QStringLiteral("开发：打开第 N 个集合（⇒ 文件浏览屏）"),
                                      QStringLiteral("row"));
    QCommandLineOption devOpenPageOpt(QStringLiteral("dev-open-page"),
                                      QStringLiteral("开发：打开浏览第 N 行（⇒ 图像查看屏）"),
                                      QStringLiteral("n"));
    QCommandLineOption devEnterRowOpt(QStringLiteral("dev-enter-row"),
                                      QStringLiteral("开发：进入浏览第 N 行的**容器**（可给逗号分隔的多层，"
                                                     "如 5,0 = 外层包 → 内层包）"),
                                      QStringLiteral("n"));
    QCommandLineOption devNotifyOpt(QStringLiteral("dev-notify"),
                                    QStringLiteral("开发：启动后弹一条**瞬时提示**（验收浮层用）"),
                                    QStringLiteral("text"));
    QCommandLineOption osdOpt(QStringLiteral("show-osd"),
                              QStringLiteral("截图前打开信息面板（验证 #7）"));
    QCommandLineOption popupOpt(QStringLiteral("show-interval-popup"),
                                QStringLiteral("截图前打开翻页间隔 popup"));
    QCommandLineOption ppOpt(QStringLiteral("pixel-perfect"),
                             QStringLiteral("像素完美通路（整数倍 + nearest）"));
    QCommandLineOption recOpt(QStringLiteral("recursive"),
                              QStringLiteral("遍历子目录（默认只当前文件夹）"));
    QCommandLineOption probeArcOpt(QStringLiteral("probe-archive"),
                                   QStringLiteral("临时探针：量压缩包 dataAt(i) 的代价"),
                                   QStringLiteral("路径"));
    QCommandLineOption probeLaOpt(QStringLiteral("probe-libarchive"),
                                  QStringLiteral("libarchive 能力探针（能否随机读）"),
                                  QStringLiteral("路径"));
    // ★ T2c-2b-2：`--scan-run` / `--scan-stop` / `--db-state`（双实例同步验证）**已删** ——
    //   它们建在旧 `ScanController` + `scan_state` 列 + `action` 表之上（整体作废，设计 14 §9.6）。
    QCommandLineOption boxOpt(QStringLiteral("box"),
                              QStringLiteral("缩放目标框 WxH（默认 1600x1000）"),
                              QStringLiteral("WxH"), QStringLiteral("1600x1000"));
    QCommandLineOption cropOpt(QStringLiteral("crop"),
                               QStringLiteral("先裁剪再缩放 x,y,w,h（放大对比用）"),
                               QStringLiteral("rect"));
    QCommandLineOption dumpOpt(QStringLiteral("dump-scaled"),
                               QStringLiteral("把第 N 张按当前缩放设置存盘（隔离验证用）"),
                               QStringLiteral("png"));
    QCommandLineOption shrinkOpt(QStringLiteral("no-shrink"),
                                 QStringLiteral("关闭解码期整数分之一预缩（默认开启；关闭会慢 6 倍）"));
    parser.addOption(dirOpt);
    parser.addOption(shotOpt);
    parser.addOption(delayOpt);
    parser.addOption(idxOpt);
    parser.addOption(immOpt);
    parser.addOption(scalerOpt);
    parser.addOption(qualityOpt);
    parser.addOption(shrinkOpt);
    parser.addOption(probeArcOpt);
    parser.addOption(probeLaOpt);
    parser.addOption(pwOpt);
    parser.addOption(recOpt);
    parser.addOption(ppOpt);
    parser.addOption(popupOpt);
    parser.addOption(osdOpt);
    parser.addOption(seekOpt);
    parser.addOption(devAddOpt);
    parser.addOption(devOpenCollOpt);
    parser.addOption(devOpenPageOpt);
    parser.addOption(devEnterRowOpt);
    parser.addOption(devNotifyOpt);
    parser.addOption(debugSettingsOpt);
    parser.addOption(debugSettingsTabOpt);
    parser.addOption(presetOpt);
    parser.addOption(lobesOpt);
    parser.addOption(mbOpt);
    parser.addOption(mcOpt);
    parser.addOption(arOpt);
    parser.addOption(filterOpt);
    parser.addOption(gpuOpt);
    parser.addOption(gpuProbeOpt);
    parser.addOption(parOpt);
    parser.addOption(threadsOpt);
    parser.addOption(repeatsOpt);
    parser.addOption(benchOpt);
    parser.addOption(dumpOpt);
    parser.addOption(boxOpt);
    parser.addOption(cropOpt);
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

    auto *provider = new ImageProvider;      // engine 接管所有权
    provider->setArchivePassword(parser.value(pwOpt));

    // ★ 缩放参数的**来源是配置文件**（用户 2026-10-12："这些参数不如直接指定一个自定义配置文件"）。
    //   CLI 的这些开关只是 **debug / 实验**用，且**只有显式给出**才覆盖
    //   （踩过的坑：带默认值的选项会被静默当成"用户指定"，把 --preset 覆盖掉）。
    //   ⚠️ 这里**与旧序一致**：此刻还没 `load()`（下面的 `UiModel` 构造时才 load）⇒
    //      provider 拿到的是**默认值**（实测旧代码也是这个行为，本批**不顺手改**）。
    app::AppSettings settings;                 // 运行期设置的家 = `UiModel`；这里只为 provider 参数
    const app::AppSettings &st = settings;
    scaler::Params sp = scaler::paramsFromStrings(st.scalerMode(), st.scalerQuality(),
                                                  st.scalerFilter(), st.scalerLobes(),
                                                  st.scalerMitchellB(), st.scalerMitchellC(),
                                                  st.scalerAntiring());
    if (const scaler::Preset *pre = parser.isSet(presetOpt) ? scaler::presetByKey(parser.value(presetOpt))
                                                            : nullptr)
        sp = pre->params;                                   // 显式 --preset 才打底

    const QString sm = parser.value(scalerOpt);
    scaler::Mode mode = sp.mode;                            // 默认取配置
    if (parser.isSet(scalerOpt)) {                          // 只有显式 --scaler 才覆盖
        if (sm == QLatin1String("qt-smooth"))         mode = scaler::Mode::QtSmooth;
        else if (sm == QLatin1String("nearest"))      mode = scaler::Mode::Nearest;
        else if (sm == QLatin1String("linear-light")) mode = scaler::Mode::LinearLight;
    }

    const QString qn = parser.isSet(qualityOpt) ? parser.value(qualityOpt) : QString();
    scaler::Filter filt = sp.filter;
    if (parser.isSet(filterOpt)) {
        const QString f = parser.value(filterOpt);
        filt = (f == QLatin1String("box"))      ? scaler::Filter::Box
             : (f == QLatin1String("lanczos"))  ? scaler::Filter::Lanczos
             : (f == QLatin1String("mitchell")) ? scaler::Filter::Mitchell
                                                : scaler::Filter::Auto;
    }
    scaler::Quality qual = sp.quality;
    if (qn == QLatin1String("fast"))        qual = scaler::Quality::Fast;
    else if (qn == QLatin1String("best"))   qual = scaler::Quality::Best;
    else if (qn == QLatin1String("balanced")) qual = scaler::Quality::Balanced;
    // 参数才是真相：显式给出的 CLI 单项参数覆盖
    sp.mode = mode;
    sp.quality = qual;
    sp.filter = filt;
    if (parser.isSet(lobesOpt)) sp.lobes = parser.value(lobesOpt).toInt();
    if (parser.isSet(mbOpt))    sp.mitchellB = parser.value(mbOpt).toDouble();
    if (parser.isSet(mcOpt))    sp.mitchellC = parser.value(mcOpt).toDouble();
    if (parser.isSet(arOpt))    sp.antiring = parser.value(arOpt).toDouble();
    provider->setScalerParams(sp);
    provider->setShrinkOnLoad(!parser.isSet(shrinkOpt));   // 默认开启
    provider->setPixelPerfect(parser.isSet(ppOpt));

    qInfo().noquote() << QStringLiteral("[main] 缩放=%1 档位=%2 滤波=%3 预缩=%4")
                             .arg(QString::fromLatin1(scaler::modeName(mode)),
                                  QString::fromLatin1(scaler::qualityName(qual)),
                                  QString::fromLatin1(scaler::filterName(filt)),
                                  parser.isSet(shrinkOpt) ? QStringLiteral("off")
                                                          : QStringLiteral("on"));
    engine.addImageProvider(QLatin1String(kThumbProviderName), provider);

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

    // ⚠️ **不再把 provider 暴露给 QML**（定案：QML 禁止引用 `provider.*`）——
    //     provider 现在**只提供缩略图**（图集卡片封面 / 浏览网格），QML 经 `model.thumb`
    //     属性拿到 `image://img/thumbpath|thumbzip/...` 的 URL，由 Qt 自己向 provider 请求。
    engine.rootContext()->setContextProperty(QStringLiteral("startDir"), dir);

    // GPU 缩放渲染组件（QML 可用）：显式注册。
    //   （不用 QML_ELEMENT：那会让生成的注册文件按 `<gpuscaleritem.h>` 找头文件，
    //     而 src/ 一旦进包含路径，libarchive 的 <archive.h> 就会被我们的 src/archive.h 顶掉）
    //   ★ 必须放在**任何 QML 加载之前**（定稿视图 FullscreenView 的 `GpuScalerItem` 要用它）
    qmlRegisterType<GpuScalerItem>("ImageBrowser", 1, 0, "GpuScalerItem");

    // ── bench / 探针（**跑完即退**）：解析留在本文件，实现与装配在 `src/devbench.cpp` ──
    //   `main.cpp` 只把 `QCommandLineOption` 的取值装进请求结构（判"给没给"用 `xxxSet`）。
    devbench::Request bench;
    bench.thumbsSet   = parser.isSet(benchOpt);
    bench.thumbsN     = parser.value(benchOpt).toInt();
    bench.gpuSet      = parser.isSet(gpuOpt);
    bench.gpuN        = parser.value(gpuOpt).toInt();
    bench.parallelSet = parser.isSet(parOpt);
    bench.parallelN   = parser.value(parOpt).toInt();
    bench.gpuProbe    = parser.isSet(gpuProbeOpt);
    bench.dumpSet     = parser.isSet(dumpOpt);
    bench.dump        = parser.value(dumpOpt);
    bench.index       = parser.value(idxOpt).toInt();
    bench.boxSet      = parser.isSet(boxOpt);
    bench.box         = parser.value(boxOpt);
    bench.cropSet     = parser.isSet(cropOpt);
    bench.crop        = parser.value(cropOpt);
    bench.threads     = parser.value(threadsOpt);
    bench.repeats     = parser.value(repeatsOpt).toInt();
    bench.noShrink    = parser.isSet(shrinkOpt);
    bench.params      = sp;

    // ── provider 的来源：**只建给"跑完即退"的 bench**（它们仍按条目号取图）──
    //   ⚠️ 主路径**不再经 provider 取图**（阅读屏的图由 `ui.viewer` 给，phase B 定案）
    //      ⇒ 不给它扫盘，免得同一目录被扫两遍（大压缩包上很贵）。
    //     主路径的 `-d` 归 `UiModel::openPath`（见下面的装配处；语义照旧：直接打开 ⇒ 文件浏览屏）。
    if (parser.isSet(dirOpt) && bench.needsProviderSource())
        provider->setSource(dir, parser.isSet(recOpt));

    if (bench.wanted())
        return devbench::runBench(bench, provider);

    // ── 全局 VM 装配（放在所有"跑完即退"的路径之后）──────────
    //   库与设置都是**懒装配**：自检 / bench / dump 这些路径不碰用户的真库
    auto *store = new library::Store;                 // 进程生命周期（不随 engine 释放）
    QString libErr;
    if (!store->open(&libErr, /*quiet=*/true)) {
        // 库不可用（如配置目录只读）→ 图集库功能停用，其余照常
        qInfo().noquote() << QStringLiteral("[library] 库不可用，图集库停用: %1").arg(libErr);
    }
    // ★ **全局只有一个 VM** = `app::UiModel`（QML 里的 `ui`）——viewer（`ui.viewer.*`）由它持有。
    //   三屏枚举只在它这里定义一次。
    //   ⚠️ 实测（见报告）：QML **读不到"实例上的枚举"** —— `ui.Library` 是 `undefined`
    //      （`ui.screen` 本身读得到，是 number）。QML 只认**已注册类型名**上的枚举
    //      ⇒ 注册一个**不可创建**的 `UiModel`（只为枚举名；实例仍走上下文属性 `ui`）。
    qmlRegisterUncreatableType<app::UiModel>("ImageBrowser", 1, 0, "UiModel",
                                             QStringLiteral("仅用于枚举；实例经上下文属性 ui 暴露"));
    auto *ui = new app::UiModel(store, &app);
    engine.rootContext()->setContextProperty(QStringLiteral("ui"), ui);
    // focus-visible：全局记录输入来源
    qApp->installEventFilter(new InputSourceWatcher(ui, qApp));
    // 系统配色变化 → 通知 QML（"跟随系统"模式下需要）
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                     ui, [ui]() { emit ui->systemDarkChanged(); });

    // 命令行 `-d <目录>` = **直接打开**（会话来源，不建库记录），语义照旧：
    //   文件浏览屏起步；三屏的后续导航全在 `ui` 上
    if (parser.isSet(dirOpt))
        ui->openPath(dir);

    // ── 开发期专用 CLI（**只为验收截图**；命名照现有 `--probe-*` / `--bench-*` 风格）──
    //   `--dev-add-collection <dir>`   往（临时）库里加一个集合 → 自动开扫 → 等扫完
    //   `--dev-open-collection <row>`  打开第 row 个集合 ⇒ 文件浏览屏（内部会等扫描结束）
    //   `--dev-open-page <n>`          打开**浏览第 n 行** ⇒ 图像查看屏（扁平来源下 = 第 n 条）
    //   `--dev-enter-row <a,b>` / `--dev-notify <text>`
    //   ★ 实现整段在 `src/devcli.cpp`（本文件只解析 + 转发；设计 14 §10）——
    //     这里只把 `QCommandLineOption` 的取值装进请求结构，判"给没给"用 `xxxSet`
    //     （`--dev-notify ""` 这种空值也要照旧执行）。
    devcli::DevRequest dev;
    dev.addSet            = parser.isSet(devAddOpt);
    dev.add               = parser.value(devAddOpt);
    dev.openCollectionSet = parser.isSet(devOpenCollOpt);
    dev.openCollection    = parser.value(devOpenCollOpt);
    dev.openPageSet       = parser.isSet(devOpenPageOpt);
    dev.openPage          = parser.value(devOpenPageOpt);
    dev.enterRowSet       = parser.isSet(devEnterRowOpt);
    dev.enterRow          = parser.value(devEnterRowOpt);
    dev.notifySet         = parser.isSet(devNotifyOpt);
    dev.notify            = parser.value(devNotifyOpt);
    devcli::runDevCommands(dev, ui, store);

    // ── `--debug-settings`（开发期）：**在 QML 加载前**把值注入上下文属性 ──────────
    //   三屏各自的 `debugOpenSettings` / `debugSettingsTab` 由 `Main.qml` 从这里喂
    //   （每屏只在自己的 `Component.onCompleted` 开自己的对话框 —— 时机就在加载期）。
    //   ★ 默认 false / 0 ⇒ 正常运行时**零行为变化**。
    engine.rootContext()->setContextProperty(QStringLiteral("devOpenSettings"),
                                             parser.isSet(debugSettingsOpt));
    engine.rootContext()->setContextProperty(QStringLiteral("devSettingsTab"),
                                             parser.value(debugSettingsTabOpt).toInt());

    engine.load(QUrl(QStringLiteral("qrc:/ImageBrowser/src/ui/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        qWarning() << "QML 加载失败";
        return 1;
    }

    const QString shot = parser.value(shotOpt);
    if (!shot.isEmpty())
        screenshot::setupShot(app, engine, shot, parser.value(delayOpt).toInt(),
                        parser.value(idxOpt).toInt(), parser.isSet(immOpt),
                        parser.isSet(popupOpt), parser.isSet(osdOpt), parser.value(seekOpt).toInt(), ui);

    return app.exec();
}
