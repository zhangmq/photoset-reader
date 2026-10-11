import QtQuick
import ImageBrowser

// ══ 标准搜索 / 过滤框 ═══════════════════════════════════════════
//   样式来源：DSH 工作区搜索框 `dsh-client-ui-workspace` WorkspaceBrowser.module.css
//     `.searchExpanded` { border: .5px solid --dsw-alias-border-l4;
//                         border-radius: --dsw-radius-md; height: 30px;
//                         background: 0 0; color: --dsw-alias-label-caption }
//     `.searchButton`   { width: 28px; height: 30px; color: inherit }   ← 前缀图标（label-caption）
//     `.searchInput`    { color: --dsw-alias-label-primary; background: 0 0; border: none }
//     `.searchInput::placeholder` { color: --dsw-alias-label-tertiary }
//     `.clearButton`    { width: 24px; height: 24px; color: --dsw-alias-label-secondary;
//                         border-radius: --dsw-radius-sm }
//     `.clearButton:hover` { background: --dsw-alias-interactive-bg-hover }
//     `:focus-visible`  { outline: --dsw-focus-ring-width; outline-offset: -2px }
//   结构： [前缀图标] [输入] [清除按钮]
//
//   ⚠️ 两处与 DSH 的差异（都是本项目刻度/形态决定的）：
//     1. 前缀图标 DSH 展开态是 11px；本项目按"正常图标大小"取 `iconMd`(18)
//     2. DSH 的清除按钮在展开态**常显**（点击 = 清空并收起搜索）；
//        本项目过滤框常驻，故**仅有内容时**显示
Rectangle {
    id: root

    property alias text: input.text
    property string placeholder: ""
    signal cleared()

    function doClear() {
        input.text = ""
        root.cleared()
    }

    implicitWidth: Theme.filterW
    implicitHeight: Theme.controlMd          // 与动作条其它控件一致（DSH 是 30px）
    radius: Theme.radiusMd
    color: "transparent"
    border.width: Theme.borderW
    border.color: Theme.borderL4

    // ── 前缀图标（DSH: .searchButton, color: inherit → label-caption）──
    Text {
        id: leadIcon
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: Theme.controlSm
        horizontalAlignment: Text.AlignHCenter
        text: Theme.icSearch
        font.family: iconFont
        font.pixelSize: Theme.iconMd
        renderType: Text.NativeRendering
        color: Theme.textCaption
    }

    // ── 清除按钮（DSH: .clearButton，24×24 / label-secondary / hover interactive-bg-hover）──
    Rectangle {
        id: clearBtn
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceXs
        anchors.verticalCenter: parent.verticalCenter
        // 随输入框一起放大：框 controlMd(36) → 按钮 controlSm(28)，四周留 4px
        //   （DSH 是框 30 / 按钮 24，按比例也是这个关系）
        width: Theme.controlSm
        height: Theme.controlSm
        radius: Theme.radiusSm
        visible: input.text !== ""
        color: (clearMa.containsMouse || clearBtn.activeFocus) ? Theme.hoverFill : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.durFast } }

        activeFocusOnTab: true

        Text {
            anchors.centerIn: parent
            text: Theme.icClear
            font.family: iconFont
            font.pixelSize: Theme.iconMd
            renderType: Text.NativeRendering
            color: Theme.textSecondary
        }
        Rectangle {                              // 焦点环（§2.5）
            anchors.fill: parent
            radius: Theme.radiusSm
            color: "transparent"
            border.width: Theme.focusRingW
            border.color: Theme.brand
            visible: clearBtn.activeFocus && ui.inputFromKeyboard
        }
        MouseArea {
            id: clearMa
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onPressed: clearBtn.forceActiveFocus()
            onClicked: root.doClear()
        }
        Keys.onReturnPressed: root.doClear()
        Keys.onEnterPressed: root.doClear()
        Keys.onSpacePressed: root.doClear()
    }

    // ── 输入 ──
    TextInput {
        id: input
        anchors.left: leadIcon.right
        anchors.right: clearBtn.visible ? clearBtn.left : parent.right
        anchors.rightMargin: clearBtn.visible ? 0 : Theme.spaceSm
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        // TextInput 的 activeFocusOnTab 默认是 false（TextField 才是 true）
        activeFocusOnTab: true
        verticalAlignment: TextInput.AlignVCenter
        color: Theme.textPrimary
        selectionColor: Theme.brand
        clip: true
        // DSH: 输入框内按 Esc 清空
        Keys.onEscapePressed: input.text = ""
    }

    Text {
        anchors.left: input.left
        anchors.verticalCenter: parent.verticalCenter
        visible: input.text === "" && !input.activeFocus
        text: root.placeholder
        color: Theme.textTertiary                // DSH: placeholder = label-tertiary
    }
}
