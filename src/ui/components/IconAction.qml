import QtQuick
import ImageBrowser

// ══ 标准图标按钮 ═══════════════════════════════════════════════
//   状态色来源：DSH 工作区 topbar 图标按钮
//     `dsh-client-ui-workspace` WorkspaceBrowser.module.css `.iconButton`
//       · 常态 color: --dsw-alias-label-secondary
//       · **hover 只变底色**（--dsw-alias-interactive-bg-hover），前景色不变
//       · background 常态透明、无描边
//   提示气泡用标准组件 HintBubble（DSH Tooltip 样式）
Rectangle {
    id: root

    property string glyph: ""
    property string tooltip: ""
    property bool   highlighted: false    // 收藏态等强调
    property bool   expanded: false       // 菜单展开中（由 IconDropdownButton 提供）
    property bool   danger: false         // 破坏性动作 ← DSH `.danger{color:state-error-primary}`
    signal triggered()

    // 键盘可达：Tab 能选中、点击即获焦
    activeFocusOnTab: true

    // ★ 用 implicit* 而非 Layout.*：这样放进普通 Row 也能拿到尺寸
    implicitWidth: Theme.controlMd
    implicitHeight: Theme.controlMd
    radius: Theme.radiusMd
    // 破坏性动作 hover 用 danger 底 ← DSH `.danger:hover{background:interactive-bg-hover-danger}`
    color: (ma.containsMouse || root.expanded)
           ? (root.danger ? Theme.dangerSoft : Theme.hoverFill) : "transparent"
    Behavior on color { ColorAnimation { duration: Theme.durFast } }

    Text {
        anchors.centerIn: parent
        text: root.glyph
        font.family: iconFont
        font.pixelSize: Theme.iconMd
        renderType: Text.NativeRendering
        color: root.danger ? Theme.danger
             : root.highlighted ? Theme.warn : Theme.textSecondary
    }

    HintBubble {
        anchors.fill: parent
        text: root.tooltip
        hovered: ma.containsMouse
    }

    Rectangle {                                  // 焦点环 ← DSH .iconButton:focus-visible
        anchors.fill: parent
        radius: Theme.radiusMd
        color: "transparent"
        border.width: Theme.focusRingW
        border.color: Theme.brand
        visible: root.activeFocus && ui.inputFromKeyboard
    }

    MouseArea {
        id: ma
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onPressed: root.forceActiveFocus()
        onClicked: root.triggered()
    }

    Keys.onReturnPressed: root.triggered()
    Keys.onEnterPressed: root.triggered()
    Keys.onSpacePressed: root.triggered()
}
