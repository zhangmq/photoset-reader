import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import AppUi

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
    //   * 2026-11-07（用户）：**宽度固定**、**高度自适应但保底**；两者都随窗口重新对齐。
    //     窗口太窄/太矮时按说的"裁掉设置窗"（不做缩放，布局不会被挤变形）。
    width: Theme.settingsW
    height: Math.max(Theme.settingsMinH,
                     Math.min(Theme.settingsH,
                              parent ? parent.height - Theme.spaceXl * 2 : Theme.settingsH))
    //   相对窗口居中（Popup 的坐标是相对 parent = 窗口 overlay）
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0

    // ★ 不掉暗背景（用户 2026-10-20："不要加 mask，方便对照设置调整效果"）：
    //   后面的画面保持原样，改设置时能直接看到效果。
    //   但这层仍然**吃掉输入**（透明 MouseArea）——模态语义不丢：背后不会被误拖/误滚。
    Overlay.modal: MouseArea {
        acceptedButtons: Qt.AllButtons
        onWheel: (w) => { w.accepted = true }
    }

    background: Item {
        Rectangle {
            id: panel
            anchors.fill: parent
            color: Theme.bgLayer2
            radius: Theme.radiusPanel
        }
        //   ★ 投影（用户 2026-11-06）：对话框不压暗背景，靠这层影子把"浮起来"表达出来
        SurfaceShadow {
            anchors.fill: panel
            radius: panel.radius
            z: -1
        }
    }

    property int tab: 0

    onOpened: {
        //   * 2026-11-07（用户）：打开设置就**暂停幻灯片**，关掉也不自动续播。
        if (ui.viewer)
            ui.viewer.playing = false
        ui.beginSettingsEdit()          // 打开即拷一份：编辑只改草稿，「放弃修改」什么都不用做
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
                text: qsTr("Settings")
                color: Theme.textPrimary
            }
            IconAction {
                anchors.right: parent.right
                anchors.rightMargin: Theme.spaceMd
                anchors.verticalCenter: parent.verticalCenter
                glyph: Theme.icClose
                //   这里**不会保存**：关掉 = 丢弃草稿（与左下角「放弃修改」同一个语义）
                tooltip: qsTr("Discard changes")
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
                    model: [ qsTr("General"), qsTr("Image"), qsTr("System"),
                             qsTr("Shortcuts"), qsTr("Decode support") ]
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
                    SettingsDecodePage    { width: parent.width; visible: root.tab === 4 }
                }
            }
        }

        // ── 底部：恢复默认（放在对话框底部，任何页都看得到）──
        //   ★ 2026-11-05 用户要求：加一个"重置设置"按钮；破坏性动作 ⇒ 先过 ConfirmDialog。
        //     默认值的唯一来源仍是 `AppSettings` 的成员初值（`UiModel::resetSettingsToDefaults`）。
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.controlMd + Theme.spaceMd * 2
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: Theme.hairline
                color: Theme.borderL2
            }
            Button {
                anchors.right: parent.right
                anchors.rightMargin: Theme.spaceLg
                anchors.verticalCenter: parent.verticalCenter
                variant: "outline"
                label: qsTr("Reset all settings")
                onClicked: resetConfirm.open()
            }
            //   * 2026-11-06 (user): settings live in memory and are written to disk ONLY
            //     here (OK) and on "reset to defaults". Cancel writes nothing - and does not
            //     roll back either: the live values simply stay for this session.
            //     Closing with X is treated as Cancel (same reason: no write).
            Row {
                anchors.left: parent.left
                anchors.leftMargin: Theme.spaceLg
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.spaceSm

                Button {
                    variant: "outline"
                    label: qsTr("Discard changes")
                    onClicked: root.close()
                }
                Button {
                    variant: "elevated"
                    label: qsTr("OK")
                    onClicked: {
                        ui.applySettingsEdit()      // 草稿 -> 生效设置（并整份写盘）
                        root.close()
                    }
                }
            }
        }
    }

    //   确认框（破坏性 ⇒ danger 样式；默认焦点在「取消」上，见 `ConfirmDialog` 自己的规则）
    ConfirmDialog {
        id: resetConfirm
        title: qsTr("Reset settings")
        message: qsTr("Restore every setting to its default value? This cannot be undone.")
        acceptText: qsTr("Reset")
        danger: true
        //   用户 2026-11-06：点「恢复」**只把表单重置成默认值**（草稿），
        //   对话框不关、不写盘 —— 之后「确定」才生效并保存，「放弃修改」就什么都不干。
        onAccepted: ui.resetSettingsEditToDefaults()
    }
}
