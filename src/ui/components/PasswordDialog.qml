import QtQuick
import QtQuick.Controls
import AppUi

// ══ 密码对话框（加密压缩包；用户 2026-11-03）═══════════════════════════════
//   与 `ConfirmDialog` 同一套外观（DSH Modal 令牌），多两样东西：
//     · 密码输入框（`echoMode: Password`）；
//     · 「后续压缩包使用相同密码」checkbox（**默认勾选**）。
//   错误提示（密码不正确）就地显示，**输入框保留**（可以直接改了再试）。
//   取消 = 中止本次打开（调用方负责清空来源）。
Popup {
    id: root

    property string title: qsTr("Password required")
    property string pathLabel: ""          // 哪个容器（相对来源根）
    property string errorText: ""          // "密码不正确" 之类（空 = 不显示）
    property bool   reuseDefault: true     // checkbox 的初始值

    signal submitted(string password, bool reuseForSubsequent)
    signal cancelled()

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    padding: 0
    readonly property var win: parent ? parent.Window.window : null
    width: Math.min(Theme.dialogW, win ? win.width - Theme.spaceXl * 2 : Theme.dialogW)
    implicitHeight: column.implicitHeight
    height: implicitHeight

    Overlay.modal: Rectangle { color: Theme.mask }
    background: Rectangle {
        color: Theme.bgLayer2
        radius: Theme.radiusPanel
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
        root.close()
        root.submitted(pw, reuse)
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

        //   后续压缩包使用相同密码（自绘 indicator：Basic 风格那个黑方块与本项目令牌不搭）
        CheckBox {
            id: reuseBox
            x: Theme.spaceLg
            checked: root.reuseDefault
            text: qsTr("Use the same password for subsequent archives")
            spacing: Theme.spaceSm
            font.pixelSize: Theme.fontSm
            contentItem: Text {
                text: reuseBox.text
                font: reuseBox.font
                color: Theme.textSecondary
                leftPadding: reuseBox.indicator.width + reuseBox.spacing
                verticalAlignment: Text.AlignVCenter
            }
            indicator: Rectangle {
                implicitWidth: 16
                implicitHeight: 16
                x: reuseBox.leftPadding
                y: parent.height / 2 - height / 2
                radius: Theme.radiusXs
                color: reuseBox.checked ? Theme.brand : "transparent"
                border.width: Theme.borderW
                border.color: reuseBox.checked ? Theme.brand : Theme.borderL4
                Text {
                    anchors.centerIn: parent
                    visible: reuseBox.checked
                    text: "✓"
                    color: Theme.brandFg
                    font.pixelSize: 12
                }
            }
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
