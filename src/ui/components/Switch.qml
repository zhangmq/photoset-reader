import QtQuick
import ImageBrowser

// ══ 标准开关（Switch）══════════════════════════════════════════
//   样式来源：DSH `dsh-client-ui-primitives/lib/Switch.module.css`
//     · 轨道 `switchW`×`switchH`(36×20)、padding `space2xs`(2)、圆角 `radiusPill`（胶囊）
//     · **关**：轨道底 `borderL3`、滑块 `switchThumb`（浅白 / 深 nb400）
//     · **开**：轨道底 **`brand`**、滑块 `brandFg`（浅白 / 深 nb1000）
//       ⚠️ DSH 的开态底用的是 `--dsw-alias-brand-primary`，而那是**黑白最高对比色**；
//          按本项目"**primary ≡ brand**"的命名约定改用 `brand`（见 06 §5.2 命名约定）
//     · 滑块 `switchThumbD`(16) 圆形，位移与配色各 120ms（DSH `transition: transform 120ms`）
//     · 焦点环按 §2.5：`activeFocus && ui.inputFromKeyboard`，画在**外侧**（DSH `outline-offset: 2px`）
//   键盘：Tab 可获焦 + 点击即获焦 + Enter/Space 切换
//   纪律：**只读属性、只发信号** —— 点击不自行改 `checked`，由调用方更新（同 Button/IconAction）
Item {
    id: root

    property bool checked: false
    signal toggled()

    implicitWidth: Theme.switchW
    implicitHeight: Theme.switchH
    activeFocusOnTab: true

    Rectangle {   // 轨道
        anchors.fill: parent
        radius: Theme.radiusPill
        color: root.checked ? Theme.brand : Theme.borderL3
        Behavior on color { ColorAnimation { duration: Theme.durFast } }
    }

    Rectangle {   // 滑块
        id: thumb
        width: Theme.switchThumbD
        height: Theme.switchThumbD
        radius: Theme.radiusPill
        anchors.verticalCenter: parent.verticalCenter
        x: root.checked ? root.width - width - Theme.space2xs : Theme.space2xs
        color: root.checked ? Theme.brandFg : Theme.switchThumb
        Behavior on x { NumberAnimation { duration: Theme.durFast } }
        Behavior on color { ColorAnimation { duration: Theme.durFast } }
    }

    Rectangle {   // 焦点环（外侧）
        anchors.fill: parent
        anchors.margins: -Theme.spaceXs
        radius: Theme.radiusPill
        color: "transparent"
        border.width: Theme.focusRingW
        border.color: Theme.brand
        visible: root.activeFocus && ui.inputFromKeyboard
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onPressed: root.forceActiveFocus()
        onClicked: root.toggled()
    }

    Keys.onReturnPressed: root.toggled()
    Keys.onEnterPressed: root.toggled()
    Keys.onSpacePressed: root.toggled()
}
