import QtQuick
import QtQuick.Controls
import ImageBrowser

// ══ 应用入口 ══════════════════════════════════════════════════
//   图集库 = 应用的第一屏（用户 2026-10-11 的界面名：图集库 / 文件浏览 / 图像查看）
//   ⚠️ 旧入口（`src/qml/Main.qml`）已于 2026-10-11 弃用，移入 `.archive/`，不再参与构建
//   ⚠️ 根对象必须是 Window：否则 QQmlApplicationEngine 拿不到窗口（显示与截图都会失败，
//      实测报 "[shot] 根对象不是 QQuickWindow"）
ApplicationWindow {
    width: 1500
    height: 950
    visible: true
    color: Theme.bgBase
    title: "photoset-reader"

    LibraryView {
        anchors.fill: parent
    }
}
