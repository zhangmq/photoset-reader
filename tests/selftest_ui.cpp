#include "testsupport.h"

// ══ UI 契约（`ib-test-ui`）══════════════════════════════════════════════════
//   为什么需要这一套（2026-11-06）：解码/渲染都有测试，**QML 一行覆盖都没有**，
//   于是"`RectangularShadow` 不是类型"、"tooltip 在缩放窗口里压住按钮并抢指针"这类
//   纯 UI 事故只能靠人眼发现。
//
//   这里钉两条**产品契约**：
//   ① `Main.qml` 能在**没有显示器**的环境里加载，且**零 QML 警告**
//      （import 写错 / 类型不存在 / 属性绑定失败 都会在这里红）；
//   ② `HintBubble` 的两个硬要求：
//      · `popupType == Popup.Item` —— 不是独立表面 ⇒ **结构上不可能抢指针**
//        （默认的 Window 在 Wayland 上是 xdg_popup，带 grab，会让底栏悬停抖动死循环）；
//      · 气泡落在目标**上方**、**不与目标矩形相交**、间距为正（只缩不放、不压按钮）。
//        这条正是 DPI/缩放事故的回归：旧实现混用 `win.width` 与 `mapToItem(contentItem)`
//        两个坐标空间，在 dpr≠1 的窗口里把气泡算到按钮上。
//
//   `QT_SCALE_FACTOR` 由 ctest 的 ENVIRONMENT 轮流注入（1.0 / 1.35），
//   ⇒ 同一个二进制会把"缩放 1"和"缩放 1.35"两条都跑一遍。

#include "uimodel.h"

#include <QEventLoop>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QDir>
#include <QTest>
#include <QUrl>
#include <viewermodel.h>
#include <QQuickWindow>
#include <QTimer>

namespace {

//   ★ `QQmlApplicationEngine` 没有"累积警告"接口（Qt 6 移除了 `QQmlEngine::warnings`），
//     所以自己抓：只统计 **load 期间** 的 warning/critical。消息照旧转发给原来的处理器，
//     日志（`--log-file`）不受影响。
int             g_loadWarnings = 0;
QtMessageHandler g_prevHandler = nullptr;

void captureWarnings(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    //   ★ 环境噪声**不算** QML 缺陷（2026-11-06 实测）：Windows 依赖树里的 Qt 没带
    //     `lib/fonts` 目录 ⇒ 一碰字体就报 "QFontDatabase: Cannot find font directory"。
    //     它跟"我们的 QML 写错了"毫无关系，Linux 上（fontconfig）也不会出现。
    const bool envNoise = msg.contains(QStringLiteral("QFontDatabase"));
    if (!envNoise && (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg))
        ++g_loadWarnings;
    if (g_prevHandler) g_prevHandler(type, ctx, msg);
}

void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

//   把气泡挂在 (x,y)、尺寸 targetW×targetH 的容器里，等它弹出，返回它的矩形（相对容器）
struct BubbleProbe
{
    bool     visible = false;
    int      type = -1;
    qreal    dpr = 0;
    int      windowsBefore = -1;   // 打开气泡**之前**进程里的顶层窗口数
    int      windowsAfter = -1;    // 打开之后
    QRectF   rect;
};

BubbleProbe probeBubble(QQmlEngine *engine, qreal scale)
{
    BubbleProbe out;
    QQuickWindow win;
    win.resize(int(800 * scale), int(600 * scale));
    win.show();
    //   ★ 记录真实的 dpr：ctest 用 `QT_SCALE_FACTOR=1.35` 跑第二遍，**必须证明这个
    //     环境变量真的生效** —— 否则"1.35 也过了"只是空话（offscreen 平台若忽略它，
    //     这条会红，我们立刻知道高 DPI 其实没被覆盖）。
    out.dpr = win.devicePixelRatio();

    QQmlComponent comp(engine, QUrl(QStringLiteral(
        "qrc:/qt/qml/AppUi/src/ui/components/HintBubble.qml")));
    QObject *obj = comp.create();
    if (!obj) return out;
    auto *bubble = qobject_cast<QQuickItem *>(obj);
    if (!bubble) return out;

    bubble->setParentItem(win.contentItem());
    //   模拟底栏里的一个 action 按钮：靠近窗口**下沿**，正是"放不下就翻上方"的场景
    const qreal w = 40 * scale;
    const qreal h = 40 * scale;
    bubble->setX(300 * scale);
    bubble->setY(win.height() - h - 10 * scale);
    bubble->setWidth(w);
    bubble->setHeight(h);
    bubble->setProperty("text", QStringLiteral("probe"));
    //   ★ 数 `allWindows()` 而不是 `topLevelWindows()`：`QQuickPopupWindow` 是**子窗口**
    //     （构造时给了 parent），不会进 topLevelWindows —— 用后者这条断言就是空的。
    out.windowsBefore = QGuiApplication::allWindows().size();
    bubble->setProperty("hovered", true);

    spin(1200);           // 500ms 延迟 + 余量

    out.visible = bubble->property("bubbleVisible").toBool();
    out.type = bubble->property("bubbleType").toInt();
    out.rect = bubble->property("bubbleRect").toRectF();
    //   ★ "没有抢指针"的**运行时证明**（2026-11-06）：
    //     Qt 只在 `usePopupWindow()` 为真时才 new `QQuickPopupWindow`
    //     （qquickpopup.cpp:1207 `adjustPopupItemParentAndWindow()`），而窗口 =
    //     Wayland surface = xdg_popup = 合成器给的隐式 grab。所以"气泡打开后
    //     **顶层窗口数没变**"等价于"没有多出表面"，也就等价于"没有 grab 的通路"。
    //     旧实现（Popup.Window）这一条会红：打开后多一个 QQuickPopupWindow。
    out.windowsAfter = QGuiApplication::allWindows().size();
    return out;
}

}  // namespace

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, [] {
        testsupport::Check check(QStringLiteral("uitest"));

        testsupport::TempFixture tmp;
        if (!tmp.isValid()) {
            return testsupport::skip(QStringLiteral("uitest"),
                                     QStringLiteral("临时目录建不出来"));
        }
        tmp.useSettingsFile();          // 绝不碰用户真配置

        //   ── ① Main.qml 加载（无显示器）+ 零警告 ───────────────────────────
        app::UiModel ui;
        const int fontId = QFontDatabase::addApplicationFont(
            QStringLiteral(":/assets/fonts/materialdesignicons-webfont.ttf"));
        const QString fontFamily = QFontDatabase::applicationFontFamilies(fontId).value(0);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("iconFont"), fontFamily);
        engine.rootContext()->setContextProperty(QStringLiteral("ui"), &ui);

        g_loadWarnings = 0;
        g_prevHandler = qInstallMessageHandler(captureWarnings);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/AppUi/src/ui/Main.qml")));
        qInstallMessageHandler(g_prevHandler);
        g_prevHandler = nullptr;

        check(!engine.rootObjects().isEmpty(),
              QStringLiteral("① Main.qml 在离屏环境里加载成功"));
        check(g_loadWarnings == 0,
              QStringLiteral("① 加载零 QML 警告（实际 %1 条，见日志）").arg(g_loadWarnings));
        check(qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)) != nullptr,
              QStringLiteral("① 根对象是 QQuickWindow"));

        //   ── ② HintBubble：不抢指针（Popup.Item）+ 位置在目标上方 ──────────
        const BubbleProbe b = probeBubble(&engine, 1.0);
        //   ★ 前提检查（避免"高 DPI 其实没被测到"的假绿）：环境给了 `QT_SCALE_FACTOR`
        //     就必须真的看到 dpr > 1；没给就必须是 1。
        const QString wantScale = qEnvironmentVariable("QT_SCALE_FACTOR");
        check(b.dpr > 0 && (wantScale.isEmpty() ? qFuzzyCompare(b.dpr, 1.0) : b.dpr > 1.1),
              QStringLiteral("② 缩放环境真的生效（QT_SCALE_FACTOR=%1，dpr=%2）")
                  .arg(wantScale.isEmpty() ? QStringLiteral("(未设)") : wantScale).arg(b.dpr));
        check(b.visible, QStringLiteral("② 悬停 500ms 后气泡真的弹出来"));
        //   Popup.Item == 0：不是独立表面 ⇒ 没有 grab ⇒ 不可能抢指针
        check(b.type == 0,
              QStringLiteral("② ★ popupType 必须是 Item（实际 %1；Window/Native 会抢指针）")
                  .arg(b.type));
        //   ★ 上面那条只是"我们写了 Item"，这一条才是 Qt 真的没建窗口：
        //     窗口数变多 ⇒ 多了一个 Wayland surface ⇒ 那颗炸弹又回来了。
        check(b.windowsBefore > 0 && b.windowsAfter == b.windowsBefore,
              QStringLiteral("② ★ 打开气泡**没有多出窗口**（前 %1 / 后 %2；多出即有新表面 = 有 grab 风险）")
                  .arg(b.windowsBefore).arg(b.windowsAfter));        //   容器坐标里 y=0 是目标顶边 ⇒ "在上方"等价于 bottom <= 0，且不能远得离谱
        check(b.visible && b.rect.bottom() <= 0.5,
              QStringLiteral("② ★ 气泡在目标**上方**（bottom=%1）").arg(b.rect.bottom()));
        check(b.visible && b.rect.bottom() > -200,
              QStringLiteral("② 气泡没被算到天上（bottom=%1）").arg(b.rect.bottom()));
        check(b.visible && !b.rect.intersects(QRectF(0, 0, 40, 40)),
              QStringLiteral("② ★ 气泡**不与目标矩形相交**（rect=%1,%2 %3x%4）")
                  .arg(b.rect.x()).arg(b.rect.y()).arg(b.rect.width()).arg(b.rect.height()));

        //   ── ③ 键盘：**合成按键必须真的驱动界面**（2026-11-07 用户报"快捷键全部失效"）──
        //   为什么放这里：`FullscreenView` 的 `Keys.onPressed` 依赖 `activeFocus`；
        //   只要焦点在哪一环断掉，**所有**快捷键一起死。这条用进程内按键把它钉住，
        //   不依赖桌面/窗口管理器（真机窗口没前台时 SendKeys 也不可靠）。
        {
            QQuickWindow *win = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
            const QString src = tmp.filePath(QStringLiteral("keys"));
            QDir().mkpath(src);
            for (int i = 0; i < 3; ++i) {
                QImage img(400, 600, QImage::Format_RGB32);
                img.fill(QColor(40 + i * 60, 90, 160));
                img.save(src + QStringLiteral("/k%1.png").arg(i + 1), "PNG");
            }
            ui.openPath(QUrl::fromLocalFile(src));
            auto viewer = [&]() { return qobject_cast<fetch::ViewerModel *>(ui.viewer()); };
            const bool opened = testsupport::waitFor(
                [&] { return viewer() != nullptr && viewer()->totalPages() >= 3; }, 20000);
            testsupport::waitFor([]{ return false; }, 500);      // 让第一帧落定（焦点链建好）
            check(opened, QStringLiteral("③ 夹具：三张图打开成功"));

            if (opened && win && viewer()) {
                win->requestActivate();
                const bool activated = testsupport::waitFor([&] { return win->isActive(); }, 5000);
                check(activated, QStringLiteral("③ 窗口成为活动窗口（键盘焦点的前提）"));

                //   ★ A 系统的两条闸门分开验（2026-11-07）：
                //     ①**规则**：焦点为空/在视图子树里 ⇒ `enabled`；焦点在面板里 ⇒ 不 enabled；
                //     ②**动作**：`run(key)` 真的驱动 UiModel。
                //     不在这里断言"合成按键能触发 Shortcut"—— 离屏平台插件下 `QTest::keyClick`
                //     不经过 Qt 的 shortcut map（实测不触发），把它写进断言只会变成假覆盖；
                //     真实键盘路径由真机手工/后续 L2 输入工具覆盖。
                QObject *sc = win->findChild<QObject *>(QStringLiteral("appShortcuts"));
                check(sc != nullptr, QStringLiteral("③ 找到 AppShortcuts（A 系统）"));

                if (sc) {
                    check(sc->property("active").toBool(),
                          QStringLiteral("③ ★ A 在「窗口 active、焦点不占键盘」时启用"));

                    const int before = viewer()->currentPage();
                    const bool ran = QMetaObject::invokeMethod(sc, "run",
                                                              Q_ARG(QVariant, int(Qt::Key_Right)));
                    const bool moved = testsupport::waitFor(
                        [&] { return viewer()->currentPage() > before; }, 3000);
                    check(ran && moved,
                          QStringLiteral("③ ★ → 的动作真的翻页（%1 → %2）")
                              .arg(before).arg(viewer()->currentPage()));

                    const bool playingBefore = viewer()->playing();
                    QMetaObject::invokeMethod(sc, "run", Q_ARG(QVariant, int(Qt::Key_Space)));
                    check(testsupport::waitFor([&] { return viewer()->playing() != playingBefore; }, 3000),
                          QStringLiteral("③ ★ 空格的动作真的切换播放/暂停"));

                    //   把焦点交给面板（这里用设置对话框当代表）：A 必须整体让路
                    QObject *settings = win->findChild<QObject *>(QStringLiteral("settingsDialog"));
                    if (settings) {
                        const bool openedDlg = QMetaObject::invokeMethod(settings, "open");
                        testsupport::waitFor([&] { return settings->property("opened").toBool(); }, 3000);
                        check(openedDlg && !sc->property("active").toBool(),
                              QStringLiteral("③ ★ 面板打开（焦点被它拿走）⇒ A 整体禁用"));
                        QMetaObject::invokeMethod(settings, "close");
                        check(testsupport::waitFor([&] { return sc->property("active").toBool(); }, 3000),
                              QStringLiteral("③ ★ 面板关闭 ⇒ A 恢复"));
                    }
                }
            }
        }

        //   ── ④ 面板分类契约（2026-11-07）────────────────────────────────
        //   ★ 断言**整窗所有 Popup 都是 `popupType: Item`**（不点名任何面板）：
        //     独立表面（Window/Native）会带隐式 grab，并让键盘焦点跑到另一个 surface 上
        //     —— 那正是"快捷键/焦点"这一串问题的来源。
        {
            QQuickWindow *win = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
            int popups = 0;
            int notItem = 0;
            if (win) {
                const QList<QObject *> all = win->findChildren<QObject *>();
                for (QObject *o : all) {
                    const QMetaObject *mo = o->metaObject();
                    if (mo->indexOfProperty("popupType") < 0)
                        continue;                       // 不是 Popup
                    ++popups;
                    if (o->property("popupType").toInt() != 0) {
                        ++notItem;
                        qInfo().noquote() << QStringLiteral("[uitest]   非 Item 型的 popup: %1")
                                                 .arg(QString::fromLatin1(mo->className()));
                    }
                }
            }
            check(popups >= 6 && notItem == 0,
                  QStringLiteral("④ ★ 窗口内 %1 个 Popup 全是 Item 型（0 = 都新建了表面）")
                      .arg(notItem));
        }

        return check.finish();
    });
}
