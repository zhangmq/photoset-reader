import QtQuick
import ImageBrowser

// ══ 标准数字输入框 ═════════════════════════════════════════════
//   用途：设置页的数值项（幻灯片间隔 / 缓存上限 / 预取窗口）、全屏的间隔输入
//   外观：与 `SearchField` 同族 —— 透明底 + `borderL4` 描边 + `radiusMd`，
//         高度 `controlMd`(36)，无前缀图标；聚焦时描边变 `brand`
//   键盘：Tab 可获焦；`Esc` 还原；`Enter` 提交；焦点环按 §2.5
//   纪律：**只发信号**（`edited(value)`），不写回调用方的属性
Item {
    id: root

    property int value: 0
    property int from: 0
    property int to: 1000000
    property int step: 1
    property string suffix: ""
    signal edited(int value)

    implicitWidth: Theme.numberW
    implicitHeight: Theme.controlMd
    activeFocusOnTab: true

    function commit() {
        var v = parseInt(input.text)
        if (isNaN(v))
            v = root.value
        v = Math.max(root.from, Math.min(root.to, v))
        input.text = String(v)
        root.edited(v)
    }

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusMd
        color: "transparent"
        border.width: Theme.borderW
        border.color: root.activeFocus ? Theme.brand : Theme.borderL4
        Behavior on border.color { ColorAnimation { duration: Theme.durFast } }
    }

    Row {
        anchors.fill: parent
        anchors.leftMargin: Theme.spaceSm
        anchors.rightMargin: Theme.spaceSm
        spacing: Theme.spaceXs

        TextInput {
            id: input
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - (suffixText.visible ? suffixText.implicitWidth + Theme.spaceXs : 0)
            horizontalAlignment: TextInput.AlignRight
            color: Theme.textPrimary
            selectionColor: Theme.brand
            selectedTextColor: Theme.brandFg
            inputMethodHints: Qt.ImhDigitsOnly
            validator: IntValidator { bottom: root.from; top: root.to }
            text: String(root.value)
            activeFocusOnTab: true

            onActiveFocusChanged: if (!activeFocus) commit()
            Keys.onReturnPressed: commit()
            Keys.onEnterPressed: commit()
            Keys.onEscapePressed: input.text = String(root.value)
            onAccepted: commit()
        }
        Text {
            id: suffixText
            anchors.verticalCenter: parent.verticalCenter
            visible: root.suffix !== ""
            text: root.suffix
            color: Theme.textTertiary
        }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.NoButton
        onWheel: function (wheel) {
            var d = wheel.angleDelta.y > 0 ? root.step : -root.step
            var v = Math.max(root.from, Math.min(root.to, root.value + d))
            input.text = String(v)
            root.edited(v)
        }
    }
}
