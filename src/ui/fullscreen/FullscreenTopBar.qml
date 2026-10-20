import QtQuick
import ImageBrowser

// ══ 全屏 · 顶部 OSD（浮层，照抄 vsr-player `TopBar.qml` 的自动隐藏模型）══
//   · 图片占满窗口，本栏**覆盖其上**，不占布局（设计 §3.1）
//   · 背景 = **不透明** `bgBase`（用户 2026-10-11：渐变 → 纯色 → 取消半透明）
//   · **自持 1px 下边框**（用户 2026-10-11；与图集库/浏览的 bar 同一规则）
//   · **隐藏逻辑不用计时器**：可见性 = 鼠标是否在热区内
//       `implicitHeight = barH(48) + osdHotZoneH(40)` —— 栏 + 下方热区
//       **整块一个 HoverHandler**（vsr-player 反面教训：分段判定会在段间留死带，
//        鼠标穿过时"可见→消失→可见"）
//   · 显隐用 `HoverHandler`（只观察 hover）；栏**可见时**另有一层 MouseArea 专门挡事件
//     （用户 2026-10-20：压在栏上不许是拖拽光标、不许把拖动/滚轮穿透给画布）
//   · 浮层隐藏时按钮 `enabled: false`（淡出后仍命中测试会误点）
Item {
    id: root

    property bool   overlaysVisible: true
    property string progressText: ""      // 进度（序号/总数）——**属于顶栏，不属于 OSD**
    property string fileName: ""          // 当前文件名（双幅时两页以 " · " 相连）
    property string relativePath: ""      // 当前文件相对**图集根**的路径（用户 2026-10-11 要求）

    signal libraryRequested()              // home → 图集库
    signal backRequested()                 // 返回 → 浏览
    signal settingsRequested()             // 设置（与其它动作条同一实现）

    readonly property bool mouseInRegion: topHover.hovered
    implicitHeight: Theme.barH + Theme.osdHotZoneH

    HoverHandler { id: topHover }

    Rectangle {
        id: bar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: Theme.barH
        color: Theme.bgBase                  // **不透明**（用户 2026-10-11：能自动显隐就不需要半透明）
        opacity: root.overlaysVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: Theme.durMed; easing.type: Easing.OutCubic } }

        // ★ 挡住事件（用户 2026-10-20）：本栏是浮层，下面是画布的拖拽/缩放手势区。
        //   不挡的话，鼠标压在栏上仍是"拖拽"光标，按下还会拖动画面（事件穿透）。
        //   声明在最前 ⇒ 栏内的按钮（后面声明）仍然在上层可点。
        MouseArea {
            anchors.fill: parent
            visible: root.overlaysVisible
            acceptedButtons: Qt.AllButtons
            cursorShape: Qt.ArrowCursor
            onWheel: (w) => { w.accepted = true }   // 滚轮也不许穿透去缩放
        }

        // 左：返回（回浏览）+ 进度（**不属于 OSD**，随本栏显隐）
        Row {
            anchors.left: parent.left
            anchors.leftMargin: Theme.spaceLg
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spaceMd

            NavBtn {
                anchors.verticalCenter: parent.verticalCenter
                glyph: Theme.icBack
                tooltip: "返回"
                enabled: root.overlaysVisible
                onTriggered: root.backRequested()
            }
            Text {   // 进度：**不属于 OSD**（用户 2026-10-11），只随顶栏显隐
                anchors.verticalCenter: parent.verticalCenter
                visible: root.progressText !== ""
                text: root.progressText
                color: Theme.textPrimary
            }
            Text {   // 当前文件名（长名中间省略，保留首尾辨识度）
                anchors.verticalCenter: parent.verticalCenter
                visible: root.fileName !== ""
                text: root.fileName
                color: Theme.textPrimary
                elide: Text.ElideMiddle
                width: Math.min(implicitWidth, Theme.osdPathMaxW)
            }
            Text {   // 相对图集根的路径（长路径中间省略，保留首尾辨识度）
                anchors.verticalCenter: parent.verticalCenter
                visible: root.relativePath !== ""
                text: root.relativePath
                color: Theme.textPrimary
                elide: Text.ElideMiddle
                width: Math.min(implicitWidth, Theme.osdPathMaxW)
            }
        }

        // 底边框（bar 自持，与图集库/浏览的 bar 同一规则）
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: Theme.hairline
            color: Theme.borderL1
        }

        // 右：主题 + 设置 + home
        Row {
            anchors.right: homeBtn.left
            anchors.rightMargin: Theme.spaceXs
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spaceXs

            NavBtn {   // 主题（三态循环：跟随系统 → 浅色 → 深色）—— 与图集库/浏览动作条同一实现
                anchors.verticalCenter: parent.verticalCenter
                glyph: ui.themeMode === 2 ? Theme.icThemeDark
                     : ui.themeMode === 1 ? Theme.icThemeLight
                     : Theme.icThemeSystem
                tooltip: ui.themeMode === 2 ? "深色"
                       : ui.themeMode === 1 ? "浅色"
                       : "跟随系统"
                enabled: root.overlaysVisible
                onTriggered: ui.cycleThemeMode()
            }
            NavBtn {   // 设置
                anchors.verticalCenter: parent.verticalCenter
                glyph: Theme.icSettings
                tooltip: "设置"
                enabled: root.overlaysVisible
                onTriggered: root.settingsRequested()
            }
        }

        // 右：home（回图集库）
        NavBtn {
            id: homeBtn
            anchors.right: parent.right
            anchors.rightMargin: Theme.spaceLg
            anchors.verticalCenter: parent.verticalCenter
            glyph: Theme.icHome
            tooltip: "图集库"
            enabled: root.overlaysVisible
            onTriggered: root.libraryRequested()
        }
    }
}
