import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import ImageBrowser

// ══ 设置对话框外壳（只做三件事：头部 / 左导航 / 按页签显示哪个页文件）══
//   设计：docs/design/09-设置界面设计.md
//   ★★ **一个页签一个文件**（用户 2026-10-11 要求"手写但结构化"）：
//        SettingsGeneralPage / SettingsImagePage / SettingsSystemPage / SettingsShortcutsPage
//      → 改一个页签**物理上碰不到别的页签**（之前全写在一个 Column 里，切片改代码
//        曾把"常规页收尾 + 图像页开头"一起删掉，导致图像设置跑进常规）
//      → 本文件只负责布局，**不放任何设置项**
//   ★ 设置项的结构只定义一次：`SettingsRow.qml`（左「标题+描述」、右控件垂直居中、行底 hairline）
//   ★ 当前阶段：**单纯 UI**，各页的值只存在页面自己的属性里，不接 ViewModel
//
//   样式来源：DSH 真实设置弹框 `dsh-client-ui-settings-general/lib/client.js`
//     · 面板 `.panel{width:800px; height:min(800px,100vh - 48px); border-radius:radius-panel(28);
//                     background:bg-layer-2}`
//     · 左导航 `.nav{width:188px}`、头部 `.header{height:54px}`、关闭 `.close{28×28; radius-sm}`
//     ⚠️ 本项目差异：头部高 `barH`(48)、关闭用标准 `IconAction`(36)
//   键盘：Esc 关闭；Tab 在导航与字段间移动（**导航不响应 ←/→** —— 用户 2026-10-11）
Popup {
    id: root

    signal finished()

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    padding: 0
    width: Math.min(Theme.settingsW, parent ? parent.width - Theme.spaceXl * 2 : Theme.settingsW)
    height: Math.min(Theme.settingsH, parent ? parent.height - Theme.spaceXl * 2 : Theme.settingsH)

    Overlay.modal: Rectangle { color: Theme.mask }

    background: Rectangle {
        color: Theme.bgLayer2
        radius: Theme.radiusPanel
    }

    property int tab: 0

    onOpened: {
        var win = parent ? parent.Window.window : null
        if (win && parent) {
            var p = win.contentItem.mapToItem(parent, (win.width - width) / 2,
                                              (win.height - height) / 2)
            x = p.x
            y = p.y
        }
        navCol.forceActiveFocus()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── 头部：标题 + 关闭 ──
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.barH
            Text {
                anchors.left: parent.left
                anchors.leftMargin: Theme.spaceLg
                anchors.verticalCenter: parent.verticalCenter
                text: "设置"
                color: Theme.textPrimary
            }
            IconAction {
                anchors.right: parent.right
                anchors.rightMargin: Theme.spaceMd
                anchors.verticalCenter: parent.verticalCenter
                glyph: Theme.icClose
                tooltip: "关闭"
                onTriggered: { root.close(); root.finished() }
            }
        }

        // ── 主体：左导航 + 右内容 ──
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Column {
                id: navCol
                Layout.preferredWidth: Theme.settingsNavW
                Layout.fillHeight: true
                topPadding: Theme.spaceMd
                spacing: Theme.space2xs
                focus: true
                activeFocusOnTab: true

                Repeater {
                    model: [ "常规", "图像", "系统", "快捷键" ]
                    delegate: Rectangle {
                        id: navItem
                        required property int index
                        required property string modelData

                        width: navCol.width - Theme.spaceMd * 2
                        x: Theme.spaceMd
                        height: Theme.controlMd
                        radius: Theme.radiusSm
                        color: root.tab === navItem.index
                               ? Theme.activeFill
                               : (navMa.containsMouse || navItem.activeFocus ? Theme.hoverFill : "transparent")
                        border.width: navItem.activeFocus && ui.inputFromKeyboard ? Theme.focusRingW : 0
                        border.color: Theme.brand
                        activeFocusOnTab: true

                        function pick() { root.tab = navItem.index }

                        Behavior on color { ColorAnimation { duration: Theme.durFast } }

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: Theme.spaceSm
                            anchors.verticalCenter: parent.verticalCenter
                            text: navItem.modelData
                            color: root.tab === navItem.index ? Theme.textPrimary : Theme.textSecondary
                        }
                        MouseArea {
                            id: navMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onPressed: navItem.forceActiveFocus()
                            onClicked: navItem.pick()
                        }
                        Keys.onReturnPressed: navItem.pick()
                        Keys.onEnterPressed: navItem.pick()
                        Keys.onSpacePressed: navItem.pick()
                    }
                }
            }

            Flickable {
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: width
                contentHeight: pages.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                Column {
                    id: pages
                    x: Theme.spaceLg
                    width: parent.width - Theme.spaceLg * 2
                    topPadding: Theme.spaceMd
                    bottomPadding: Theme.spaceLg

                    SettingsGeneralPage   { width: parent.width; visible: root.tab === 0 }
                    SettingsImagePage     { width: parent.width; visible: root.tab === 1 }
                    SettingsSystemPage    { width: parent.width; visible: root.tab === 2 }
                    SettingsShortcutsPage { width: parent.width; visible: root.tab === 3 }
                }
            }
        }
    }
}
