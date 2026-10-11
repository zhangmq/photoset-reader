import QtQuick
import ImageBrowser

// ══ 标准按钮（图标 + 文字）══════════════════════════════════════
//   尺寸来源：DSH 标准按钮 `dsh-client-ui-primitives/lib/Button.module.css`
//       · .md { height: 36px }；.button { gap: 4px; border-radius: radius-md; padding: 0 14px }
//   颜色来源：DSH 侧边栏「新会话」按钮 `SidebarRoot.module.css .newSession`
//       · border: .5px solid --dsw-alias-border-l3；radius: --dsw-radius-md
//       · 底: --dsw-alias-button-elevated-fill
//       · 文字/图标: --dsw-alias-label-primary（**常态就是 primary**）
//       · hover 只换底 → --dsw-alias-button-floating-hover（前景色不变）
//   ⚠️ 不做 big / medium / small 型号属性（当前用不到；将来加 size 属性即可）
Rectangle {
    id: root

    property string glyph: ""
    property string label: ""
    property string variant: "elevated"   // "elevated"（默认，DSH「新会话」按钮）| "outline"（DSH `.outline`）
    property bool   danger: false         // 破坏性动作 ← DSH `.deleteConfirm`
    property bool   hasIndicator: false   // 下拉箭头（由 DropdownButton 打开）
    property bool   expanded: false       // 菜单展开中（由 DropdownButton 提供）
    signal clicked()

    readonly property bool hot: ma.containsMouse || root.expanded
    // outline：透明底 + 描边 + hover 半透明填充（DSH `.outline`）
    // danger：描边与文字用 danger，hover 底用 dangerSoft（DSH `.deleteConfirm`）
    readonly property color fg: root.danger ? Theme.danger : Theme.textPrimary

    // 键盘可达：Tab 能选中、点击即获焦（弹窗关闭后焦点才能还给它）
    activeFocusOnTab: true

    implicitWidth: content.width + Theme.spaceMd * 2
    implicitHeight: Theme.controlMd
    radius: Theme.radiusMd
    color: root.variant === "outline"
           ? (hot ? (root.danger ? Theme.dangerSoft : Theme.hoverFill) : "transparent")
           : (hot ? Theme.btnFillHover : Theme.btnFill)
    border.width: Theme.borderW
    border.color: root.danger ? Theme.danger : Theme.borderL3
    Behavior on color { ColorAnimation { duration: Theme.durFast } }

    Row {
        id: content
        anchors.centerIn: parent
        spacing: Theme.spaceXs

        Text {                                   // 图标（与文字同色）
            anchors.verticalCenter: parent.verticalCenter
            text: root.glyph
            font.family: iconFont
            font.pixelSize: Theme.iconMd
            renderType: Text.NativeRendering
            color: root.fg
        }
        Text {                                   // 文字
            anchors.verticalCenter: parent.verticalCenter
            text: root.label
            color: root.fg
        }
        Text {                                   // 下拉箭头：颜色**与文字不同** ← DSH label-caption
            anchors.verticalCenter: parent.verticalCenter
            visible: root.hasIndicator
            text: Theme.icChevronD
            font.family: iconFont
            font.pixelSize: Theme.iconMd
            renderType: Text.NativeRendering
            color: Theme.textCaption
        }
    }

    Rectangle {                                  // 焦点环 ← DSH .iconButton:focus-visible
        anchors.fill: parent                     //   outline 2px / offset -2px（画在内部）
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
        onPressed: root.forceActiveFocus()       // 点击即获焦
        onClicked: root.clicked()
    }

    Keys.onReturnPressed: root.clicked()         // 键盘激活
    Keys.onEnterPressed: root.clicked()
    Keys.onSpacePressed: root.clicked()
}
