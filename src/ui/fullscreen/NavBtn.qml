import QtQuick
import ImageBrowser

// ══ 全屏浮层按钮（照抄 vsr-player `src/client/ui/components/IconButton.qml`）══
//   · **跟随主题，颜色一律用 DSH alias 令牌**（用户 2026-10-11 定案 (A)：不再自建浮层配色）
//     常态 `overlayIcon`、hover/高亮 `overlayIconHover`、hover 底 `overlayHoverFill`
//   · 尺寸 `controlMd`(36)；带 `label` 时按文字宽度撑开（用于"上一/下一文件夹"按钮）
//   · **不做 Tab 焦点**：全屏 OSD 是键盘快捷键面、且浮层会隐藏，
//     Tab 到隐藏按钮上是错的（07 文档 §五.5，与 vsr-player 一致）
//   · 浮层隐藏时调用方须 `enabled: false`（否则淡出后仍命中测试，会误点/误出 tooltip）
Item {
    id: root

    property string glyph: ""
    property string label: ""
    property string tooltip: ""
    property bool   highlighted: false
    readonly property alias pressed: ma.pressed   // 长按连续快进用（设计 §3.5）
    signal triggered()
    // 禁用态：与其它控件一致用透明度（不再单独定义禁用色）
    opacity: root.enabled ? 1.0 : Theme.dimOpacity
    implicitHeight: Theme.controlMd
    implicitWidth: row.implicitWidth + Theme.spaceMd * 2

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusXs
        color: (ma.containsMouse || root.highlighted) ? Theme.hoverFill : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.durFast } }
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: Theme.spaceXs

        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: root.glyph !== ""
            text: root.glyph
            font.family: iconFont
            font.pixelSize: Theme.iconMd
            renderType: Text.NativeRendering
            color: (ma.containsMouse || root.highlighted) ? Theme.textPrimary
                                                           : Theme.textSecondary
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: root.label !== ""
            text: root.label
            color: (ma.containsMouse || root.highlighted) ? Theme.textPrimary
                                                           : Theme.textSecondary
        }
    }

    MouseArea {
        id: ma
        anchors.fill: parent
        hoverEnabled: true
        enabled: root.enabled
        cursorShape: Qt.PointingHandCursor
        onClicked: root.triggered()
    }

    HintBubble {
        anchors.fill: parent
        text: root.tooltip
        hovered: ma.containsMouse && root.tooltip !== "" && root.enabled
    }
}
