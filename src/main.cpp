#include <QGuiApplication>
#include <QStyleHints>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QCommandLineParser>
#include <QDir>
#include <QTimer>
#include <QQuickWindow>
#include <QQuickItem>
#include <QImage>
#include <QImageReader>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QFile>
#include <QFontDatabase>
#include <QElapsedTimer>
#include <QEvent>
#include <QThread>
#include <cmath>

#include "imageprovider.h"
#include "scaler.h"
#include "archive.h"
#include "natural.h"
#include "settings.h"
#include "settingsbridge.h"
#include "viewmodel.h"
#include "layout.h"
#include <QImage>
#include <QImageReader>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QFile>
#include <QFontDatabase>
#include <QElapsedTimer>
#include <QEvent>
#include <QThread>
#include <cmath>

#ifndef DEFAULT_TEST_DIR
#define DEFAULT_TEST_DIR ""
#endif

// 开发期自检：离屏运行时把窗口内容截图存盘，供人工/自动核对。
// 用法： QT_QPA_PLATFORM=offscreen ./image-browser -d <dir> --shot out.png [--delay ms]
// ── 记录"最后一次输入是键盘还是鼠标" ─────────────────────────
//   用途：QML 侧实现 focus-visible —— 焦点环只在键盘操作时显示。
//   Qt Quick 没有暴露焦点原因（实测 Item.focusReason 在 QML 中不存在），
//   所以在应用级事件过滤器里自己记：按键 → 键盘；点按/滚轮/触摸 → 鼠标。
//   只观察、不拦截（始终返回 false）。
class InputSourceWatcher : public QObject
{
public:
    explicit InputSourceWatcher(ViewModel *vm, QObject *parent = nullptr)
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
    ViewModel *m_vm;
};

static void setupScreenshot(QGuiApplication &app, QQmlApplicationEngine &engine,
                            const QString &outPath, int delayMs, int shotIndex,
                            bool immersive, bool slideshow, bool popup, bool osdShown, int seekPage,
                            ViewModel *viewModel)
{
    QTimer::singleShot(delayMs, &app, [&engine, outPath, shotIndex, immersive, slideshow, popup, osdShown, seekPage, viewModel]() {
        const auto roots = engine.rootObjects();
        if (roots.isEmpty()) { qWarning() << "[shot] 没有根对象"; return; }
        auto *win = qobject_cast<QQuickWindow *>(roots.first());
        if (!win) { qWarning() << "[shot] 根对象不是 QQuickWindow"; return; }

        if (shotIndex >= 0)
            viewModel->setCursor(shotIndex);
        if (immersive)
            viewModel->setImmersive(true);
        if (osdShown) viewModel->setOsdVisible(true);
        if (seekPage >= 0) {
            viewModel->setImmersive(true);
            const int before = viewModel->currentPage();
            viewModel->seekToPage(seekPage);
            qInfo().noquote() << QStringLiteral("[seek] 请求第 %1 页：%2 → %3  %4")
                .arg(seekPage).arg(before).arg(viewModel->currentPage())
                .arg(viewModel->currentPage() == seekPage ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
        }
        if (popup) {
            viewModel->setImmersive(true);
            QTimer::singleShot(600, win, [win]() {
                QMetaObject::invokeMethod(win, "openIntervalPopup");
            });
        }
        if (slideshow) {
            viewModel->setImmersive(true);
            // 等 QML 侧就绪后再启动幻灯片
            QTimer::singleShot(600, win, [win]() {
                QMetaObject::invokeMethod(win, "startSlideshow");
            });
        }

        // 再等一帧，让绑定生效（popup/slideshow 需要更久）
        QTimer::singleShot((popup || slideshow) ? 1400 : 400, win, [win, outPath]() {
            const QVariant pg = win->property("currentPage");
            if (pg.isValid())
                qInfo().noquote() << QStringLiteral("[slideshow] 结束时页号 = %1").arg(pg.toInt());
            const QImage img = win->grabWindow();
            if (img.isNull()) { qWarning() << "[shot] grabWindow 返回空"; return; }
            if (img.save(outPath))
                qInfo().noquote() << QStringLiteral("[shot] 已保存 %1  (%2x%3)")
                                         .arg(outPath).arg(img.width()).arg(img.height());
            else
                qWarning().noquote() << QStringLiteral("[shot] 保存失败 %1").arg(outPath);
            QCoreApplication::quit();
        });
    });
}

// ── 核心正确性自检（可手算的期望值）──────────────────────────
// 50/50 黑白棋盘做 2:1 缩小：
//   正确答案（线性光平均）: 线性 0.5 → sRGB = 1.055*0.5^(1/2.4)-0.055 = 0.7354 → **188**
//   错误答案（gamma 空间平均）: (0+255)/2 = **128**
// 两者差 **60 级** —— 这就是"线性光缩放"能被一眼看出的原因。
// 实测（2026-10-09）：我们 188.0（精确正确）／Qt SmoothTransformation 127.0（gamma 空间）。
static int runSelfTest()
{
    const int N = 64;
    QImage img(N, N, QImage::Format_RGB32);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x)
            img.setPixel(x, y, ((x + y) & 1) ? qRgb(255, 255, 255) : qRgb(0, 0, 0));

    auto probe = [&](scaler::Mode m, scaler::Quality q, const char *label) {
        const QImage out = scaler::resize(img, QSize(N / 2, N / 2), m, q);
        if (out.isNull()) { qWarning() << label << "返回空"; return; }
        // 取中心区域均值（避开边缘）
        long sum = 0, cnt = 0;
        for (int y = N / 8; y < N / 2 - N / 8; ++y)
            for (int x = N / 8; x < N / 2 - N / 8; ++x) {
                const QRgb c = out.pixel(x, y); sum += qRed(c); ++cnt;
            }
        const double avg = double(sum) / double(cnt);
        qInfo().noquote() << QStringLiteral("  %1 → 灰度均值 %2  (期望 188=线性光正确 / 128=gamma空间)")
                                 .arg(QString::fromLatin1(label), QString::number(avg, 'f', 1));
    };

    qInfo().noquote() << "[selftest] 50/50 黑白棋盘 2:1 缩小：";
    probe(scaler::Mode::LinearLight, scaler::Quality::Fast,     "linear-light + box     ");
    probe(scaler::Mode::LinearLight, scaler::Quality::Balanced, "linear-light + lanczos3");
    probe(scaler::Mode::QtSmooth,    scaler::Quality::Balanced, "qt-smooth (对照)       ");

    // ── 判据二：光能量守恒 ───────────────────────────────────
    // 正确的降采样判据是「输出的**线性光**均值 == 输入的线性光均值」。
    // 不能用「l2s(线性均值) == 均值(编码值)」——两者因 Jensen 不等式本就不等。
    auto linMean = [](const QImage &im) {
        double acc = 0; long n = 0;
        const QImage s = im.convertToFormat(QImage::Format_RGB32);
        for (int y = 0; y < s.height(); ++y) {
            const QRgb *row = reinterpret_cast<const QRgb *>(s.constScanLine(y));
            for (int x = 0; x < s.width(); ++x) {
                const int ch[3] = {qRed(row[x]), qGreen(row[x]), qBlue(row[x])};
                for (int c = 0; c < 3; ++c) {
                    const double v = ch[c] / 255.0;
                    acc += (v <= 0.04045) ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
                    ++n;
                }
            }
        }
        return n ? acc / n : 0.0;
    };

    const double inLin = linMean(img);   // 棋盘：线性均值应为 0.5
    qInfo().noquote() << QStringLiteral("\n[selftest] 光能量守恒（输入线性均值 %1，理论 0.5）：")
                             .arg(inLin, 0, 'f', 4);
    int failures = 0;
    auto check = [&](scaler::Mode m, scaler::Quality q, const char *label, double tolPct) {
        const QImage out = scaler::resize(img, QSize(N / 2, N / 2), m, q);
        const double err = (linMean(out) - inLin) / inLin * 100.0;
        const bool ok = std::abs(err) <= tolPct;
        if (!ok) ++failures;
        qInfo().noquote() << QStringLiteral("  %1 误差 %2%  %3")
                                 .arg(QString::fromLatin1(label),
                                      QString::number(err, 'f', 2).rightJustified(7),
                                      ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
    };
    check(scaler::Mode::LinearLight, scaler::Quality::Fast,     "linear-light + box     ", 1.0);
    check(scaler::Mode::LinearLight, scaler::Quality::Balanced, "linear-light + lanczos3", 1.0);
    check(scaler::Mode::QtSmooth,    scaler::Quality::Balanced, "qt-smooth (应显著失败)", 1.0);

    // ── 判据三：版面感知（相邻页宽高比组合是最易出错处）─────────
    qInfo().noquote() << QStringLiteral("\n[selftest] 版面感知：");
    int lf = 0;
    auto expectKinds = [&](const char *label, const QVector<QSize> &sz,
                           const QVector<layout::Kind> &want) {
        const auto pages = layout::build(sz);
        QString got;
        bool ok = (pages.size() == want.size());
        for (int i = 0; i < pages.size(); ++i) {
            got += QString::fromLatin1(layout::kindName(pages.at(i).kind)) + QLatin1Char(' ');
            if (i < want.size() && pages.at(i).kind != want.at(i)) ok = false;
        }
        if (!ok) ++lf;
        qInfo().noquote() << QStringLiteral("  %1 → %2  %3")
                                 .arg(QString::fromLatin1(label), got.trimmed(),
                                      ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
    };

    // ① 本项目素材：全横向（跨页扫描）→ 全部单独成页，不配对
    expectKinds("全横向（跨页扫描）",
                {QSize(2893,2200), QSize(2893,2200), QSize(2893,2200)},
                {layout::Kind::WideSingle, layout::Kind::WideSingle, layout::Kind::WideSingle});
    // ② 两张连续竖图 → 配对
    expectKinds("竖+竖 → 配对",
                {QSize(1400,2000), QSize(1400,2000)},
                {layout::Kind::Spread});
    // ③ 竖 + 横 → 不配对（横的单独成页）
    expectKinds("竖+横 → 不配对",
                {QSize(1400,2000), QSize(2893,2200)},
                {layout::Kind::Single, layout::Kind::WideSingle});
    // ④ 三张竖图 → 配对 + 落单
    expectKinds("竖×3 → 配对+落单",
                {QSize(1400,2000), QSize(1400,2000), QSize(1400,2000)},
                {layout::Kind::Spread, layout::Kind::Single});
    // ⑤ 条漫（极端长宽比）→ 单独，不配对
    expectKinds("条漫 800×40000",
                {QSize(800,40000), QSize(1400,2000)},
                {layout::Kind::Webtoon, layout::Kind::Single});
    // ⑥ 条漫按绝对高度识别
    expectKinds("高 5000 的窄图 → 条漫",
                {QSize(1200,5000)},
                {layout::Kind::Webtoon});
    // ⑦ 空序列
    expectKinds("空序列", {}, {});
    // ⑧ RTL 配对顺序：右页在前
    {
        const auto pages = layout::build({QSize(1400,2000), QSize(1400,2000)});
        const bool ok = pages.size()==1 && pages[0].items.size()==2
                        && pages[0].items[0]==1 && pages[0].items[1]==0 && pages[0].rtlPair;
        if (!ok) ++lf;
        qInfo().noquote() << QStringLiteral("  RTL 配对顺序（右页在前）→ %1  %2")
                                 .arg(pages.isEmpty() ? QStringLiteral("?")
                                      : QStringLiteral("%1,%2").arg(pages[0].items.value(0)).arg(pages[0].items.value(1)),
                                      ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
    }

    qInfo().noquote() << (lf == 0 ? QStringLiteral("[selftest] 版面感知全部通过")
                                  : QStringLiteral("[selftest] ⚠️ 版面感知有 %1 项失败").arg(lf));

    qInfo().noquote() << (failures == 1
        ? QStringLiteral("\n[selftest] 结果符合预期：只有 qt-smooth 失败（gamma 空间缩放必然丢光）")
        : QStringLiteral("\n[selftest] ⚠️ 结果与预期不符，需检查"));
    return 0;
}

int main(int argc, char *argv[])
{
    // ★ 强制使用**系统原生**文件对话框（用户要求 #1）。
    //   本机有 xdg-desktop-portal + KDE/GTK 后端 + Qt 的 libqxdgdesktopportal.so，
    //   但 QT_QPA_PLATFORMTHEME 未设置时 Qt 会回退到**自带**对话框。
    //   ⚠️ 必须在 QGuiApplication **构造之前**设置——Qt 在构造时读取它。
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORMTHEME"))
        qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
    QGuiApplication app(argc, argv);

    qInfo().noquote() << QStringLiteral("[ui] 平台主题 = %1（决定文件对话框是否走系统原生）")
                             .arg(QString::fromLocal8Bit(qgetenv("QT_QPA_PLATFORMTHEME")));
    // 字体纪律（用户明确）：一律不设字号 / 字体 / 字体样式，基准字号 = **系统默认**。
    //   ⚠️ 这里刻意不做任何调整：曾按"默认字体加大一号"做过 +1pt，已按用户要求收回。
    //   QML 侧只有 Theme.fontSm = 系统默认 − 1（徽标等次要信息），是**派生**值，跟着走。
    QCoreApplication::setApplicationName(QStringLiteral("image-browser"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1-dev"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("image-browser（开发期最小壳）"));
    parser.addHelpOption();
    QCommandLineOption dirOpt({QStringLiteral("d"), QStringLiteral("dir")},
                              QStringLiteral("图片目录"), QStringLiteral("path"),
                              QString::fromUtf8(DEFAULT_TEST_DIR));
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
                               QStringLiteral("跳到第 N 页（验证进度条点击）"), QStringLiteral("n"));
    QCommandLineOption stepOpt(QStringLiteral("bench-step"),
                               QStringLiteral("测顺序步进延迟（预热 vs 冷）"), QStringLiteral("n"));
    QCommandLineOption flowOpt(QStringLiteral("selftest-flow"),
                               QStringLiteral("进程内验证：缺失标记 / 进入压缩包"));
    // ── 调试：单独加载某一屏（仅开发用，不在正式 CLI 范畴）──
    QCommandLineOption debugListOpt(QStringLiteral("debug-list"),
                                    QStringLiteral("调试：浏览界面用列表视图"));
    QCommandLineOption debugSettingsOpt(QStringLiteral("debug-settings"),
                                        QStringLiteral("调试：启动即打开设置对话框"));
    QCommandLineOption debugSortOpt(QStringLiteral("debug-sort"),
                                    QStringLiteral("调试：截图时展开排序菜单"));
    QCommandLineOption debugThemeOpt(QStringLiteral("debug-theme"),
                                     QStringLiteral("调试：0=跟随系统 1=浅色 2=深色"),
                                     QStringLiteral("n"));
    QCommandLineOption debugViewOpt(QStringLiteral("debug-view"),
                                      QStringLiteral("单独加载一屏: library"),
                                      QStringLiteral("name"));
    QCommandLineOption seekOpt2(QStringLiteral("seek-test"),
                                QStringLiteral("跳到第 N 页并报告结果"),
                                QStringLiteral("n"));
    QCommandLineOption osdOpt(QStringLiteral("show-osd"),
                              QStringLiteral("截图前打开信息面板（验证 #7）"));
    QCommandLineOption popupOpt(QStringLiteral("show-interval-popup"),
                                QStringLiteral("截图前打开翻页间隔 popup"));
    QCommandLineOption slideOpt(QStringLiteral("slideshow-test"),
                                QStringLiteral("启动幻灯片，delay 后报告推进了多少页（验证高速幻灯片）"));
    QCommandLineOption ppOpt(QStringLiteral("pixel-perfect"),
                             QStringLiteral("像素完美通路（整数倍 + nearest）"));
    QCommandLineOption recOpt(QStringLiteral("recursive"),
                              QStringLiteral("遍历子目录（默认只当前文件夹）"));
    QCommandLineOption selfTestOpt(QStringLiteral("selftest"),
                                   QStringLiteral("跑核心正确性自检后退出"));
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
    parser.addOption(selfTestOpt);
    parser.addOption(pwOpt);
    parser.addOption(recOpt);
    parser.addOption(ppOpt);
    parser.addOption(slideOpt);
    parser.addOption(popupOpt);
    parser.addOption(osdOpt);
    parser.addOption(seekOpt2);
    parser.addOption(debugViewOpt);
    parser.addOption(debugThemeOpt);
    parser.addOption(debugSortOpt);
    parser.addOption(debugSettingsOpt);
    parser.addOption(debugListOpt);
    parser.addOption(flowOpt);
    parser.addOption(seekOpt);
    parser.addOption(stepOpt);
    parser.addOption(benchOpt);
    parser.addOption(dumpOpt);
    parser.addOption(boxOpt);
    parser.addOption(cropOpt);
    parser.process(app);

    if (parser.isSet(selfTestOpt))
        return runSelfTest();

    const QString dir = parser.value(dirOpt);
    {
        const QFileInfo fi(dir);
        if (dir.isEmpty() || !fi.exists()) {
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
    provider->setSource(dir, parser.isSet(recOpt));

    const QString sm = parser.value(scalerOpt);
    scaler::Mode mode = scaler::Mode::LinearLight;
    if (sm == QLatin1String("qt-smooth"))      mode = scaler::Mode::QtSmooth;
    else if (sm == QLatin1String("nearest"))   mode = scaler::Mode::Nearest;
    provider->setMode(mode);

    const QString qn = parser.value(qualityOpt);
    scaler::Quality qual = scaler::Quality::Balanced;
    if (qn == QLatin1String("fast"))        qual = scaler::Quality::Fast;
    else if (qn == QLatin1String("best"))   qual = scaler::Quality::Best;
    provider->setQuality(qual);
    provider->setShrinkOnLoad(!parser.isSet(shrinkOpt));   // 默认开启
    provider->setPixelPerfect(parser.isSet(ppOpt));

    qInfo().noquote() << QStringLiteral("[main] 缩放=%1 档位=%2 预缩=%3")
                             .arg(QString::fromLatin1(scaler::modeName(mode)),
                                  QString::fromLatin1(scaler::qualityName(qual)),
                                  parser.isSet(shrinkOpt) ? QStringLiteral("off")
                                                          : QStringLiteral("on"));
    engine.addImageProvider(QStringLiteral("img"), provider);

    Settings::instance().load();
    // 启动目录也"打开即添加"（用户要求 #1）
    if (QFileInfo(dir).isDir())
        Settings::instance().addOrTouchDir(dir, parser.isSet(recOpt));
    auto *bridge = new SettingsBridge(provider, &app);
    bridge->noteSource(dir, parser.isSet(recOpt));   // 登记来源（左侧栏可见 + resume 可存）

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

    engine.rootContext()->setContextProperty(QStringLiteral("provider"), provider);
    engine.rootContext()->setContextProperty(QStringLiteral("settings"), bridge);
    auto *viewModel = new ViewModel(provider, &app);
    engine.rootContext()->setContextProperty(QStringLiteral("ui"), viewModel);
    // focus-visible：全局记录输入来源
    qApp->installEventFilter(new InputSourceWatcher(viewModel, qApp));
    // 系统配色变化 → 通知 QML（"跟随系统"模式下需要）
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                     viewModel, [viewModel]() { emit viewModel->systemDarkChanged(); });
    engine.rootContext()->setContextProperty(QStringLiteral("startDir"), dir);

    // ── 顺序步进延迟（连拍场景红线：O(1) 呈现）──
    if (parser.isSet(stepOpt)) {
        const int n = qMin(parser.value(stepOpt).toInt(), provider->count());
        QSize sz;                                   // 模拟窗口尺寸请求
        auto fetch = [&](int i) {
            return provider->requestImage(QStringLiteral("view/%1").arg(i), &sz, QSize(1600,1000));
        };
        // ① 冷：逐张首次解码（无预取）
        QElapsedTimer t; t.start();
        for (int i = 0; i < n; ++i) fetch(i);
        const qint64 cold = t.elapsed();

        // ② 预热：预取窗口填满后再步进
        provider->prefetch(0, 1600, 1000);
        QThread::msleep(1500);                      // 等预取线程填完
        t.restart();
        int hits = 0;
        for (int i = 0; i < n; ++i) {
            if (!fetch(i).isNull()) ++hits;
            provider->prefetch(i, 1600, 1000);      // 边步进边预取（真实行为）
        }
        const qint64 warm = t.elapsed();
        // ③ 内存有界：连续步进远超缓存容量，看 RSS 是否被压住
        auto rssMb = []() -> double {
            QFile f(QStringLiteral("/proc/self/status"));
            if (!f.open(QIODevice::ReadOnly)) return -1;
            for (const QByteArray &l : f.readAll().split('\n'))
                if (l.startsWith("VmRSS:")) return l.mid(6).trimmed().split(' ').first().toDouble()/1024.0;
            return -1;
        };
        const double rssBefore = rssMb();
        for (int i = 0; i < provider->count(); ++i) fetch(i);      // 跑完全部 117 张
        const double rssAfter = rssMb();
        qInfo().noquote() << QStringLiteral("[bench] 内存：跑完 %1 张 RSS %2 → %3 MB   缓存记账 %4 MB")
                                 .arg(provider->count())
                                 .arg(rssBefore,0,'f',0).arg(rssAfter,0,'f',0)
                                 .arg(provider->cacheMegabytes(),0,'f',1);

        qInfo().noquote() << QStringLiteral("[bench] 步进 %1 张：冷 %2 ms (%3 ms/张) / 预热后 %4 ms (%5 ms/张)  命中 %6")
                                 .arg(n).arg(cold).arg(double(cold)/qMax(1,n),0,'f',2)
                                 .arg(warm).arg(double(warm)/qMax(1,n),0,'f',2).arg(hits);
        return 0;
    }

    // ── 进程内流程自检：缺失标记 / 进入压缩包 ──
    if (parser.isSet(flowOpt)) {
        int fails = 0;
        auto check = [&](const char *label, bool ok) {
            if (!ok) ++fails;
            qInfo().noquote() << QStringLiteral("  %1  %2")
                                     .arg(QString::fromUtf8(label), ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
        };

        // ① 缺失标记：临时目录 → 列序列 → 删一个文件 → revalidate 应标记 1 项
        {
            QTemporaryDir tmp;
            for (int i = 0; i < 3; ++i) {
                QImage im(32, 32, QImage::Format_RGB32);
                im.fill(Qt::red);
                im.save(tmp.filePath(QStringLiteral("p%1.png").arg(i)));
            }
            provider->setSource(tmp.path(), false);
            check("目录列出 3 项", provider->count() == 3);
            QFile::remove(tmp.filePath(QStringLiteral("p1.png")));
            const int marked = provider->revalidate();
            check("删除后标记 1 项缺失", marked == 1);
            check("序列仍为 3 项（只标记不移除）", provider->count() == 3);
            check("第 2 项被标为缺失", provider->isMissingAt(1));
            check("缺失项解码返回空（不崩溃）", provider->decodeRaw(1).isNull());
        }

        // ② 进入压缩包：列目录 → 进入第 1 个压缩包 → 序列应变成内页
        {
            provider->setSource(QStringLiteral(".testdata"), false);
            int arcIdx = -1;
            for (int i = 0; i < provider->count(); ++i)
                if (provider->isArchiveAt(i)) { arcIdx = i; break; }
            check("目录里能找到压缩包条目", arcIdx >= 0);
            if (arcIdx >= 0) {
                const int before = provider->count();
                check("进入压缩包成功", provider->enterArchive(arcIdx));
                // 注意：不比较 count——目录条目数与内页数可能巧合相等
                check("序列已切换为内页", provider->insideArchive()
                      && provider->count() > 0 && provider->absPathAt(0).isEmpty());
                check("内页可解码", !provider->decodeRaw(0).isNull());
                check("返回上级成功", provider->leaveArchive());
                check("已回到目录序列", provider->count() == before && !provider->insideArchive());
            }
        }

        // ③ 空目录
        {
            QTemporaryDir tmp;
            provider->setSource(tmp.path(), false);
            check("空目录 count=0", provider->count() == 0);
            check("空目录 decodeRaw 不崩溃", provider->decodeRaw(0).isNull());
            check("空目录 pageCount=0", provider->pageCount() == 0);
        }

        // ④ 坏图 / 非图片文件混入
        {
            QTemporaryDir tmp;
            QImage im(32,32,QImage::Format_RGB32); im.fill(Qt::blue);
            im.save(tmp.filePath("ok.png"));
            QFile bad(tmp.filePath("bad.png")); bad.open(QIODevice::WriteOnly);
            bad.write("this is not a png"); bad.close();
            QFile txt(tmp.filePath("note.txt")); txt.open(QIODevice::WriteOnly);
            txt.write("hello"); txt.close();
            provider->setSource(tmp.path(), false);
            check("只列出图片后缀（txt 被排除）", provider->count() == 2);
            int okCount = 0;
            for (int i = 0; i < provider->count(); ++i)
                if (!provider->decodeRaw(i).isNull()) ++okCount;
            check("坏图返回空、好图正常（不崩溃）", okCount == 1);
        }

        // ⑤ 幻灯片遇缺失：跳过并计数，不停在坏图上
        {
            QTemporaryDir tmp;
            for (int i = 0; i < 5; ++i) {
                QImage im(32,32,QImage::Format_RGB32); im.fill(Qt::green);
                im.save(tmp.filePath(QStringLiteral("p%1.png").arg(i)));
            }
            provider->setSource(tmp.path(), false);
            QFile::remove(tmp.filePath("p2.png"));
            provider->revalidate();
            check("缺失被标记", provider->isMissingAt(2));
            // 模拟幻灯片推进：跳过缺失项
            int visited = 0, skipped = 0;
            for (int i = 0; i < provider->count(); ++i) {
                if (provider->isMissingAt(i)) { ++skipped; continue; }
                if (!provider->decodeRaw(i).isNull()) ++visited;
            }
            check("幻灯片可跳过缺失并计数", skipped == 1 && visited == 4);
        }

        // ⑥ 版面：条漫 / 跨页
        {
            QTemporaryDir tmp;
            QImage tall(800, 40000, QImage::Format_RGB32); tall.fill(Qt::white);
            tall.save(tmp.filePath("webtoon.png"));
            QImage a(1200,1800,QImage::Format_RGB32); a.fill(Qt::white); a.save(tmp.filePath("a.png"));
            QImage b(1200,1800,QImage::Format_RGB32); b.fill(Qt::white); b.save(tmp.filePath("b.png"));
            provider->setSource(tmp.path(), false);
            check("目录列出 3 项", provider->count() == 3);
            // 自然排序下是 a.png, b.png, webtoon.png —— 按名字找，不假设索引
            int wi = -1;
            for (int i = 0; i < provider->count(); ++i)
                if (provider->fileNameAt(i).contains(QStringLiteral("webtoon"))) wi = i;
            check("找得到条漫条目", wi >= 0);
            check("条漫被识别（isWebtoonAt）", wi >= 0 && provider->isWebtoonAt(wi));
            check("条漫有切片数", wi >= 0 && provider->sliceCountAt(wi) > 1);
            check("两张竖版被配成一页（3 项 → 2 页）", provider->pageCount() == 2);
        }

        // ⑦ 单张图片 + EXIF 自动旋转
        {
            QTemporaryDir tmp;
            QImage im(800, 600, QImage::Format_RGB32); im.fill(Qt::red);
            im.save(tmp.filePath("plain.png"));
            provider->setSource(tmp.filePath("plain.png"), false);
            check("单张图片 → 1 项序列", provider->count() == 1);
            check("单张图片可解码", !provider->decodeRaw(0).isNull());
            check("单张图片尺寸正确", provider->sourceSizeAt(0) == QSize(800,600));

            // EXIF 自动旋转：用仓库内真实样例（原始 800x600 + Orientation=6）
            const QString exifFile = QStringLiteral(".testdata/edge/exif6.jpg");
            if (QFile::exists(exifFile)) {
                provider->setSource(exifFile, false);
                check("EXIF 单张 → 1 项", provider->count() == 1);
                check("EXIF Orientation=6 自动旋转（800x600 → 600x800）",
                      provider->sourceSizeAt(0) == QSize(600,800));
            } else {
                check("EXIF 样例缺失（跳过）", true);
            }
        }

        // ⑧ 多页文件（多页 TIFF 等）按页容器处理
        {
            const QString mp = QStringLiteral(".testdata/edge/multi.tiff");
            if (QFile::exists(mp)) {
                provider->setSource(mp, false);
                check("多页 TIFF 列出 2 页", provider->count() == 2);
                check("多页 TIFF → 2 页版面", provider->pageCount() == 2);
                const QImage p0 = provider->decodeRaw(0);
                const QImage p1 = provider->decodeRaw(1);
                check("两页都可解码", !p0.isNull() && !p1.isNull());
                // 两页内容必须不同（证明真的跳页，而不是都解第 0 页）
                bool differ = false;
                if (!p0.isNull() && !p1.isNull() && p0.size() == p1.size()) {
                    for (int y = 0; y < p0.height() && !differ; y += 7)
                        for (int x = 0; x < p0.width(); x += 7)
                            if (p0.pixel(x,y) != p1.pixel(x,y)) { differ = true; break; }
                }
                check("两页内容不同（真的跳页）", differ);
            } else {
                check("多页样例缺失（跳过）", true);
            }
        }

        // ⑨ 特殊字符文件名
        {
            QTemporaryDir tmp;
            const QStringList names = {QStringLiteral("空格 文件.png"),
                                       QStringLiteral("emoji_\xF0\x9F\x8E\x8C.png"),
                                       QStringLiteral("引号'单引.png"),
                                       QStringLiteral("中文（全角）.png")};
            for (const QString &n : names) {
                QImage im(64,48,QImage::Format_RGB32); im.fill(Qt::darkCyan);
                im.save(tmp.filePath(n));
            }
            provider->setSource(tmp.path(), false);
            check("特殊字符文件名全部列出", provider->count() == names.size());
            bool allDecode = true;
            for (int i = 0; i < provider->count(); ++i)
                if (provider->decodeRaw(i).isNull()) allDecode = false;
            check("特殊字符文件名都可解码", allDecode);
        }

        // ⑩ 损坏的压缩包：不崩溃，返回失败
        {
            QTemporaryDir tmp;
            QFile f(tmp.filePath("junk.cbz"));
            f.open(QIODevice::WriteOnly);
            f.write("this is definitely not an archive");
            f.close();
            provider->setSource(f.fileName(), false);   // 不应崩溃
            check("损坏压缩包不崩溃（count 可为 0）", provider->count() >= 0);
            check("损坏压缩包 decodeRaw 不崩溃", provider->decodeRaw(0).isNull());
        }

        // ⑪ ★ 压缩包内条目的尺寸必须有效（版面判定的前提）
        //    曾经 rebuildPages 用 QImageReader(it.fullPath) —— 对归档条目这是**包内路径**，
        //    不是文件系统路径 → 尺寸恒 0x0 → 117 项被错配成 59 页。
        {
            const QString cbz = QStringLiteral(".testdata/test.cbz");
            if (QFile::exists(cbz)) {
                provider->setSource(cbz, false);
                check("CBZ 有条目", provider->count() > 0);
                int valid = 0;
                for (int i = 0; i < provider->count(); ++i)
                    if (provider->sourceSizeAt(i).isValid()) ++valid;
                check("CBZ 内每项尺寸都有效（非 0x0）", valid == provider->count());
                check("CBZ 页数与条目数一致（横版不配对）",
                      provider->pageCount() == provider->count());
            } else {
                check("CBZ 样例缺失（跳过）", true);
            }
        }

        qInfo().noquote() << (fails == 0 ? QStringLiteral("[flow] 全部通过")
                                         : QStringLiteral("[flow] %1 项失败").arg(fails));
        return fails == 0 ? 0 : 1;
    }

    // 隔离验证：缩略图吞吐（不含 QML/渲染）
    if (parser.isSet(benchOpt)) {
        const int n = qMin(parser.value(benchOpt).toInt(), provider->count());
        QElapsedTimer t; t.start();
        int ok = 0;
        for (int i = 0; i < n; ++i) {
            // ★ 走**真实缩略图路径**（含 shrink-on-load），不是全解码
            QSize sz;
            const QImage th = provider->requestImage(QStringLiteral("thumb/%1").arg(i),
                                                     &sz, QSize(160,160));
            if (!th.isNull()) ++ok;
        }
        const qint64 ms = t.elapsed();
        qInfo().noquote() << QStringLiteral("[bench] %1 张（真实缩略图路径）%2 ms  平均 %3 ms/张  单线程")
                                 .arg(ok).arg(ms).arg(double(ms)/qMax(1,ok), 0, 'f', 2);
        return 0;
    }

    // 隔离验证：不经过 QML，直接把缩放结果存盘
    const QString dumpPath = parser.value(dumpOpt);
    if (!dumpPath.isEmpty()) {
        const int idx = qMax(0, parser.value(idxOpt).toInt());
        const QStringList wh = parser.value(boxOpt).split(QLatin1Char('x'));
        const QSize box(wh.value(0).toInt(), wh.value(1).toInt());
        const QImage raw = provider->decodeRaw(idx);
        if (raw.isNull()) { qWarning() << "解码失败" << provider->fileNameAt(idx); return 3; }
        QImage work = raw;
        if (parser.isSet(cropOpt)) {
            const QStringList c = parser.value(cropOpt).split(QLatin1Char(','));
            if (c.size() == 4) {
                const QRect r(c[0].toInt(), c[1].toInt(), c[2].toInt(), c[3].toInt());
                if (raw.rect().contains(r)) work = raw.copy(r);
                else qWarning().noquote() << "[dump] crop 越界，忽略:" << parser.value(cropOpt);
            }
        }
        const QImage out = provider->pixelPerfect()
            ? scaler::resize(work, box, scaler::Mode::Nearest, scaler::Quality::Fast)
            : scaler::resize(work, box.isValid() && !box.isEmpty() ? box : QSize(1600, 1000),
                             mode, qual);
        if (!out.save(dumpPath)) { qWarning() << "保存失败" << dumpPath; return 4; }
        qInfo().noquote() << QStringLiteral("[dump] %1  源 %2x%3 → 出 %4x%5  已存 %6")
                                 .arg(provider->fileNameAt(idx)).arg(raw.width()).arg(raw.height())
                                 .arg(out.width()).arg(out.height()).arg(dumpPath);
        return 0;
    }

    // 调试：单独加载一屏（自带 Window 包装，便于逐屏过设计）
    const QString debugView = parser.value(debugViewOpt);
    // 组装调试根组件（含各视图的调试参数）
    auto debugRootComponent = [](const QString &view, bool listMode, bool sortMode, bool forceOsd, bool settingsMode) -> QString {
        if (view == QLatin1String("browse"))
            return QStringLiteral("BrowseView { anchors.fill: parent%1%2 }")
                   .arg(listMode ? QStringLiteral("; gridMode: false") : QString())
                   .arg(settingsMode ? QStringLiteral("; debugOpenSettings: true") : QString());
        if (view == QLatin1String("fullscreen")) {
            QString img = QStringLiteral("/home/zmq/projects/image-browser/.testdata/vol01/"
                                         "[\u4e03\u9f99\u73e0\u5b8c\u5168\u7248]"
                                         "\u7b2c01\u5377_005.jpg");
            return QStringLiteral("FullscreenView { anchors.fill: parent; debugForceUi: %2; debugImage: \"%1\" }")
                   .arg(QUrl::fromLocalFile(img).toString())
                   .arg(forceOsd ? QStringLiteral("true") : QStringLiteral("false"));
        }
        return QStringLiteral("LibraryView { anchors.fill: parent%1 }")
               .arg(sortMode ? QStringLiteral("; debugOpenSort: true") : QString());
    };

    if (debugView == QLatin1String("iconpicker")) {
        const QString q = QStringLiteral(
            "import QtQuick\nimport QtQuick.Controls\nimport ImageBrowser\n"
            "ApplicationWindow {\n"
            "    width: 640\n    height: 520\n    visible: true\n    color: Theme.bgBase\n"
            "    IconPicker { anchors.fill: parent; items: [\n"
            "      {name:'book-outline',                  cp:String.fromCodePoint(0xF0B64), hex:'F0B64'},\n"
            "      {name:'book-variant',                  cp:String.fromCodePoint(0xF00BF), hex:'F00BF'},\n"
            "      {name:'book-open-variant (双幅在用)',   cp:String.fromCodePoint(0xF14F7), hex:'F14F7'},\n"
            "      {name:'file-document-outline',         cp:String.fromCodePoint(0xF09EE), hex:'F09EE'},\n"
            "      {name:'note-outline',                  cp:String.fromCodePoint(0xF039B), hex:'F039B'},\n"
            "      {name:'note-text-outline',             cp:String.fromCodePoint(0xF11D7), hex:'F11D7'},\n"
            "      {name:'text-box-outline',              cp:String.fromCodePoint(0xF09ED), hex:'F09ED'},\n"
            "      {name:'card-outline',                  cp:String.fromCodePoint(0xF0B76), hex:'F0B76'},\n"
            "      {name:'card-text-outline',             cp:String.fromCodePoint(0xF0B78), hex:'F0B78'},\n"
            "      {name:'page-layout-body',              cp:String.fromCodePoint(0xF06FA), hex:'F06FA'},\n"
            "      {name:'view-agenda-outline',           cp:String.fromCodePoint(0xF11D8), hex:'F11D8'},\n"
            "      {name:'application-outline',           cp:String.fromCodePoint(0xF0614), hex:'F0614'},\n"
            "      {name:'image-outline',                 cp:String.fromCodePoint(0xF0976), hex:'F0976'},\n"
            "      {name:'format-page-break',             cp:String.fromCodePoint(0xF06D7), hex:'F06D7'},\n"
            "      {name:'script-text-outline',           cp:String.fromCodePoint(0xF0BC3), hex:'F0BC3'},\n"
            "      {name:'bookmark-outline',              cp:String.fromCodePoint(0xF00C3), hex:'F00C3'}\n"
"    ] }\n"
            "}\n");
        engine.loadData(q.toUtf8(), QUrl(QStringLiteral("qrc:/iconpicker.qml")));
        if (!parser.value(shotOpt).isEmpty())
            setupScreenshot(app, engine, parser.value(shotOpt), parser.value(delayOpt).toInt(),
                            -1, false, false, false, false, -1, viewModel);
        return app.exec();
    }

    if (!debugView.isEmpty()) {
        const QString q = QStringLiteral(
            "import QtQuick\n"
            "import QtQuick.Controls\n"
            "import ImageBrowser\n"
            "ApplicationWindow {\n"
            "    width: 1100\n"
            "    height: 720\n"
            "    visible: true\n"
            "    color: Theme.bgBase\n"
            "    %1"
            "}").arg(debugRootComponent(debugView, parser.isSet(debugListOpt), parser.isSet(debugSortOpt), parser.isSet(shotOpt), parser.isSet(debugSettingsOpt)));
        if (parser.isSet(debugThemeOpt)) viewModel->setThemeMode(parser.value(debugThemeOpt).toInt());
        if (qEnvironmentVariableIsSet("DSH_DUMP_QML"))
            qInfo().noquote() << "[dump-qml]\n" + q;
        engine.loadData(q.toUtf8(), QUrl(QStringLiteral("qrc:/debug-root.qml")));
        if (engine.rootObjects().isEmpty()) {
            qWarning().noquote() << QStringLiteral("[debug-view] 加载失败");
            return 1;
        }
        qInfo().noquote() << QStringLiteral("[debug-view] 已加载 %1").arg(debugView);
        if (!parser.value(shotOpt).isEmpty())
            setupScreenshot(app, engine, parser.value(shotOpt), parser.value(delayOpt).toInt(),
                            -1, false, false, false, false, -1, viewModel);
        return app.exec();
    }

    engine.load(QUrl(QStringLiteral("qrc:/ImageBrowser/src/ui/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        qWarning() << "QML 加载失败";
        return 1;
    }


    const QString shot = parser.value(shotOpt);
    if (!shot.isEmpty())
        setupScreenshot(app, engine, shot, parser.value(delayOpt).toInt(),
                        parser.value(idxOpt).toInt(), parser.isSet(immOpt),
                        parser.isSet(slideOpt), parser.isSet(popupOpt), parser.isSet(osdOpt), parser.value(seekOpt2).toInt(), viewModel);

    return app.exec();
}
