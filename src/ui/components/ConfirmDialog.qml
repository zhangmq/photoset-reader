import QtQuick
import QtQuick.Controls
import ImageBrowser

// ══ 标准确认对话框 ═══════════════════════════════════════════
//   样式来源：DSH `dsh-client-ui-primitives/lib/Modal.module.css`
//     · 遮罩 `--dsw-alias-bg-mask-1`（浅 24% / 深 50% 黑）；面板 `bg-layer-2`、**无描边**
//     · 圆角 `--dsw-radius-panel`(28)；宽 `min(380px,100%)`
//     · 标题行 padding 22/…/12、正文 0/24、按钮行 0/24 + 底部 24、块间距 20
//     · 标题 `label-primary`；正文本项目用 `textSecondary`（用户规则：标题 primary / 内容 secondary）
//   按钮：DSH 确认框里两个都是 **outline** 变体；破坏性确认额外套 `.deleteConfirm`
//     （描边/文字 `state-error-primary`，hover 底 `interactive-bg-hover-danger`）
//   键盘：Esc = 取消；**键盘打开**时焦点给「取消」（Enter 落到取消，避免误触破坏性动作），
//         鼠标打开时不给按钮焦点（否则按钮上会出现焦点环，像"默认选中"）
Popup {
    id: root

    property string title: ""
    property string message: ""
    property string acceptText: "确认"
    property string rejectText: "取消"
    property bool   danger: false

    signal accepted()
    signal rejected()

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    padding: 0
    // 宽与位置都按**整窗**算（父项可能是一张卡片，尺寸在布局完成前不可靠）
    readonly property var win: parent ? parent.Window.window : null
    width: Math.min(Theme.dialogW, win ? win.width - Theme.spaceXl * 2 : Theme.dialogW)
    implicitHeight: column.implicitHeight
    height: implicitHeight

    Overlay.modal: Rectangle { color: Theme.mask }

    background: Rectangle {
        color: Theme.bgLayer2
        radius: Theme.radiusPanel
    }

    // 居中：Qt 惯用写法（相对窗口 overlay），不依赖父项坐标
    anchors.centerIn: Overlay.overlay

    property bool acceptedFlag: false

    // ★ 键盘打开才把焦点给「取消」（避免鼠标打开时按钮上出现焦点环，像"默认选中"）；
    //   鼠标打开时 Popup 自身持有焦点，Esc 仍可取消。
    onOpened: if (ui.inputFromKeyboard) rejectBtn.forceActiveFocus()
    onClosed: {
        if (!acceptedFlag)
            rejected()
        acceptedFlag = false
    }

    Column {
        id: column
        width: parent.width
        spacing: Theme.spaceLg                 // DSH: gap 20px

        Item {                                 // 标题（上方 padding 22 → spaceLg）
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

        Text {                                 // 正文（左右 padding 24 → spaceLg）
            x: Theme.spaceLg
            width: parent.width - Theme.spaceLg * 2
            text: root.message
            color: Theme.textSecondary         // 标题 primary / 内容 secondary（用户规则）
            wrapMode: Text.WordWrap
        }

        Item {                                 // 按钮行（右对齐）+ 底部 padding 24 → spaceLg
            width: parent.width
            implicitHeight: footer.implicitHeight + Theme.spaceLg
            Row {
                id: footer
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.rightMargin: Theme.spaceLg
                spacing: Theme.spaceSm
                Button {
                    id: rejectBtn
                    variant: "outline"
                    label: root.rejectText
                    onClicked: root.close()
                }
                Button {
                    variant: "outline"
                    danger: root.danger
                    label: root.acceptText
                    onClicked: {
                        root.acceptedFlag = true
                        root.close()
                        root.accepted()
                    }
                }
            }
        }
    }
}
