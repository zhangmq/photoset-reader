#include "iconpicker.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QString>
#include <QStyleHints>
#include <QUrl>

#include "uimodel.h"

// ══ 图标挑选工具 `ib-iconpicker`（开发用；设计 14 §11.1）════════════════════
//   原 `image-browser --debug-view iconpicker` 模式的独立目标版：
//   ★ 候选图标表（QML 字符串）与应用 CLI 里的**一字不差**（只搬不改），
//     QML 页仍是 `src/ui/debug/IconPicker.qml`（模块 `ImageBrowser`）。
//   ★ `Theme.qml` 要看 `ui.themeMode` / `ui.systemDark` ⇒ 这里按**调试视图的老接法**
//     建一个不落库的 `UiModel`（`store = nullptr`）挂成上下文属性 `ui`（与已删的
//     调试页同一处语义；`GpuScalerItem` 那类没用到就不注册）。
//   ★ 不是测试：不进 `tests/`、不挂 `add_test`。

int runIconPicker(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("photoset-reader"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1-dev"));

    // ★ 设置的 `load()` 在 `UiModel` 构造里（设计 14 §9.3：运行期设置挂在它身上）——
    //   与旧代码那次单例 `load()` 在这里的时机等价（QML 加载前已完成）。
    QQmlApplicationEngine engine;

    // 调试视图的老接法：**不建库**（store = nullptr）—— 不碰用户的真库
    auto *ui = new app::UiModel(nullptr, &app);
    engine.rootContext()->setContextProperty(QStringLiteral("ui"), ui);
    // 系统配色变化 → 通知 QML（"跟随系统"模式下需要）；与主路径同一接线
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                     ui, [ui]() { emit ui->systemDarkChanged(); });

    // 图标字体：与 `main.cpp` 同一接线（QML 侧经 `iconFont` 取族名，比 FontLoader 可靠）
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
    if (engine.rootObjects().isEmpty()) {
        qWarning().noquote() << QStringLiteral("[iconpicker] QML 加载失败");
        return 1;
    }
    qInfo().noquote() << QStringLiteral("[iconpicker] 已加载 src/ui/debug/IconPicker.qml");
    return app.exec();
}

int main(int argc, char *argv[])
{
    return runIconPicker(argc, argv);
}
