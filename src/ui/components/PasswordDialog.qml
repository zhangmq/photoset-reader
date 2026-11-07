import QtQuick
import QtQuick.Controls
import AppUi

// ══ 密码对话框（加密压缩包；用户 2026-11-03）═══════════════════════════════
//   与 `ConfirmDialog` 同一套外观（DSH Modal 令牌），多两样东西：
//     · 密码输入框（`echoMode: Password`）；
//     · 「后续压缩包使用相同密码」checkbox（**默认勾选**）。
//   错误提示（密码不正确）就地显示，**输入框保留**（可以直接改了再试）。
//   取消 = 中止本次打开（调用方负责清空来源）。
ModalDialog {
    id: root

    //   本项目风格的勾选框（自绘 indicator：Basic 风格的方块与令牌不搭）；两个勾选项共用一份实现
    component LabeledCheck: CheckBox {
        id: cb
        spacing: Theme.spaceSm
        contentItem: Text {
            text: cb.text
            font: cb.font
            color: Theme.textSecondary
            leftPadding: cb.indicator.width + cb.spacing
            verticalAlignment: Text.AlignVCenter
        }
        indicator: Rectangle {
            implicitWidth: 16
            implicitHeight: 16
            x: cb.leftPadding
            y: parent.height / 2 - height / 2
            radius: Theme.radiusXs
            color: cb.checked ? Theme.brand : "transparent"
            border.width: Theme.borderW
            border.color: cb.checked ? Theme.brand : Theme.borderL4
            Text {
                anchors.centerIn: parent
                visible: cb.checked
                text: "✓"
                color: Theme.brandFg
            }
        }
    }

    property string title: qsTr("Password required")
    property string pathLabel: ""          // 哪个容器（相对来源根）
    property string errorText: ""          // "密码不正确" 之类（空 = 不显示）
    property bool   reuseDefault: true     // checkbox 的初始值
    //   ★ **保存密码**（用户 2026-11-03）：只在"这条路能写进度文件"时给（打开进度文件 / 收藏）
    property bool   showSavePassword: false
    property bool   savePasswordDefault: false

    signal submitted(string password, bool reuseForSubsequent, bool saveToFile)
    signal cancelled()

    padding: 0
    readonly property var win: parent ? parent.Window.window : null
    // 比标准对话框宽一点：这里要显示容器相对来源的**路径**（中间省略）
    width: Math.min(Theme.dialogWideW, win ? win.width - Theme.spaceXl * 2 : Theme.dialogWideW)
    implicitHeight: column.implicitHeight
    height: implicitHeight

    Overlay.modal: Rectangle { color: Theme.mask }
    background: Item {
        Rectangle {
            id: panel
            anchors.fill: parent
            color: Theme.bgLayer2
            radius: Theme.radiusPanel
        }
        //   ★ 与设置框 / 关于框同一份投影（用户 2026-11-06）
        SurfaceShadow {
            anchors.fill: panel
            radius: panel.radius
            z: -1
        }
    }
    anchors.centerIn: Overlay.overlay

    property bool submittedFlag: false      // 本次提交过 ⇒ 关闭时不再算"取消"
    onOpened: {
        submittedFlag = false               // ★ 每次打开都复位（否则提交过一次后，取消永远不生效）
        pwInput.text = ""
        pwInput.forceActiveFocus()
    }
    onClosed: {
        if (!submittedFlag)                  // Esc / 取消按钮 / 关闭
            cancelled()
        submittedFlag = false                // ★ 关闭后清零（下次打开重新计）
    }

    function submit() {
        if (pwInput.text.length === 0)
            return
        submittedFlag = true
        const pw = pwInput.text
        const reuse = reuseBox.checked
        const save = saveBox.visible && saveBox.checked
        root.close()
        root.submitted(pw, reuse, save)
    }

    Column {
        id: column
        width: parent.width
        spacing: Theme.spaceMd

        Item {                                     // 标题
            width: parent.width
            implicitHeight: titleText.implicitHeight + Theme.spaceLg
            Text {
                id: titleText
                anchors.left: parent.left
                anchors.leftMargin: Theme.spaceLg
                anchors.bottom: parent.bottom
                text: root.title
                color: Theme.textPrimary
            }
        }

        Text {                                     // 哪个包
            x: Theme.spaceLg
            width: parent.width - Theme.spaceLg * 2
            text: root.pathLabel
            color: Theme.textSecondary
            elide: Text.ElideMiddle
        }

        Rectangle {                                // 输入框（与 NumberField 同一套描边）
            x: Theme.spaceLg
            width: parent.width - Theme.spaceLg * 2
            height: Theme.controlMd
            radius: Theme.radiusMd
            color: "transparent"
            border.width: Theme.borderW
            border.color: pwInput.activeFocus ? Theme.brand : Theme.borderL4
            Behavior on border.color { ColorAnimation { duration: Theme.durFast } }

            TextInput {
                id: pwInput
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: Theme.spaceSm
                anchors.rightMargin: Theme.spaceSm
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.textPrimary
                selectionColor: Theme.brand
                selectedTextColor: Theme.brandFg
                echoMode: TextInput.Password
                passwordCharacter: "•"
                activeFocusOnTab: true
                //   ★ 只留 `onAccepted`（回车由它发）：再挂 `Keys.onReturnPressed` 会让一次回车
                //     提交两遍（两次重扫 / 两次 retry）。
                onAccepted: root.submit()
            }
        }

        //   后续压缩包使用相同密码
        LabeledCheck {
            id: reuseBox
            x: Theme.spaceLg
            checked: root.reuseDefault
            text: qsTr("Use the same password for subsequent archives")
        }

        //   ★ 保存密码到进度文件：只有能写文件时才出现（打开进度文件 / 收藏那条路）
        LabeledCheck {
            id: saveBox
            x: Theme.spaceLg
            visible: root.showSavePassword
            height: visible ? implicitHeight : 0
            checked: root.savePasswordDefault
            text: qsTr("Save the password in this progress file (plain text)")
        }

        Text {                                     // 错误行（"密码不正确"）
            x: Theme.spaceLg
            width: parent.width - Theme.spaceLg * 2
            visible: root.errorText !== ""
            text: root.errorText
            color: Theme.danger
            wrapMode: Text.WordWrap
        }

        Item {                                     // 按钮行
            width: parent.width
            implicitHeight: footer.implicitHeight + Theme.spaceLg
            Row {
                id: footer
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.rightMargin: Theme.spaceLg
                spacing: Theme.spaceSm
                Button {
                    variant: "outline"
                    label: qsTr("Cancel")
                    onClicked: root.close()
                }
                Button {
                    variant: "outline"
                    label: qsTr("Open")
                    enabled: pwInput.text.length > 0      // 空密码不给点（点了也没反应）
                    onClicked: root.submit()
                }
            }
        }
    }
}
