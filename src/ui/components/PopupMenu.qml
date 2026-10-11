import QtQuick
import QtQuick.Layouts
import ImageBrowser

// ══ 标准菜单（popup menu）══════════════════════════════════════
//   行结构来源：vsr-player/src/client/ui/OpenMenu.qml / ContextMenu.qml
//     · 行 = [前导图标] 文本 [紧跟文本的后缀图标] … [右侧提示]，整行 hover，可插分隔线
//   外观（底 / 描边 / 圆角）继承 PopupPanel
//
//   items 每项：{ icon, text, suffix, hint, selected, sep }
//     icon      前导图标（有任意一项带 icon 时，全表保留图标列 → 文本左沿对齐）
//     suffix    紧跟文本之后的图标（如排序方向）
//     hint      右对齐的提示（如快捷键）
//     selected  选中态（文本与图标用 brand 色）
//     sep       分隔线行
//
//   ★ 必须用 ColumnLayout 而不是 Column：
//     Column 的隐式宽度取**子项的 width**，而这里的行宽又要撑满列宽 →
//     互相依赖成环，最终隐式尺寸为 0，弹出框只剩 padding（表现为一个小圆点）。
//     Layout 把「测量用的 preferredWidth」与「实际 width」分开，环就断了。
PopupPanel {
    id: root

    property var items: []
    signal itemTriggered(int index)

    readonly property int rowPadH: Theme.spaceSm
    readonly property bool hasLeadingIcons: {
        for (var i = 0; i < items.length; ++i)
            if (items[i] && items[i].icon)
                return true
        return false
    }
    readonly property int leadW: hasLeadingIcons ? Theme.iconMd : 0

    // ── 键盘操作（方向键选行 / Enter 激活 / Esc 关闭由 closePolicy 处理）──
    property int currentIndex: -1

    function firstSelectableIndex() {
        for (var i = 0; i < items.length; ++i)
            if (items[i] && items[i].sep !== true)
                return i
        return -1
    }
    function moveCurrent(step) {
        var n = items.length
        if (n === 0)
            return
        var i = currentIndex
        for (var k = 0; k < n; ++k) {
            i = (i + step + n) % n
            if (items[i] && items[i].sep !== true) {
                currentIndex = i
                return
            }
        }
    }
    function activateCurrent() {
        if (currentIndex < 0)
            return
        var i = currentIndex
        close()
        itemTriggered(i)
    }

    onOpened: {
        // ★ 鼠标打开时**不高亮任何项**（否则会像"默认选中第一项"，误导用户）
        currentIndex = ui.inputFromKeyboard ? firstSelectableIndex() : -1
        column.forceActiveFocus()          // 键盘事件交给菜单内容项
    }

    padding: Theme.spaceXs                      // DSH: .list { padding: 4px }
    width: implicitWidth
    height: implicitHeight
    // DSH: .list { min-width: 144px } —— 太窄时 radius-lg 的圆角会显得突兀
    implicitWidth: Math.max(Theme.menuMinW, column.implicitWidth + leftPadding + rightPadding)
    implicitHeight: column.implicitHeight + topPadding + bottomPadding

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 0
        focus: true

        Keys.onUpPressed: root.moveCurrent(-1)
        Keys.onDownPressed: root.moveCurrent(1)
        Keys.onReturnPressed: root.activateCurrent()
        Keys.onEnterPressed: root.activateCurrent()
        Keys.onSpacePressed: root.activateCurrent()

        Repeater {
            model: root.items

            delegate: Item {
                id: row
                required property var modelData
                required property int index

                readonly property bool isSep: modelData && modelData.sep === true
                readonly property bool selected: !!(modelData && modelData.selected)

                Layout.fillWidth: true
                Layout.preferredHeight: isSep ? Theme.hairline : Theme.controlMd
                Layout.preferredWidth: root.rowPadH * 2
                    + (root.leadW > 0 ? root.leadW + Theme.spaceSm : 0)
                    + label.implicitWidth
                    + (suffix.visible ? Theme.spaceSm + suffix.implicitWidth : 0)
                    + (hint.visible ? Theme.spaceMd + hint.implicitWidth : 0)

                Rectangle {                                   // hover 底（DSH: 行圆角 = radius-md）
                    anchors.fill: parent
                    visible: !row.isSep
                    radius: Theme.radiusMd
                    color: (rowMa.containsMouse || row.index === root.currentIndex)
                         ? Theme.hoverFill : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.durFast } }
                }
                Rectangle {                                   // 分隔线（DSH: 菜单面上用 l2，l1 看不见）
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: Theme.space2xs
                    anchors.rightMargin: Theme.space2xs
                    anchors.verticalCenter: parent.verticalCenter
                    height: Theme.hairline
                    color: Theme.borderL2
                    visible: row.isSep
                }
                Text {                                        // 前导图标（固定列宽）
                    id: lead
                    anchors.left: parent.left
                    anchors.leftMargin: root.leadW > 0 ? root.rowPadH : 0
                    anchors.verticalCenter: parent.verticalCenter
                    width: root.leadW
                    visible: root.leadW > 0
                    text: (modelData && modelData.icon) ? modelData.icon : ""
                    font.family: iconFont
                    font.pixelSize: Theme.iconMd
                    renderType: Text.NativeRendering
                    // DSH: 普通行图标用 --dsw-alias-menu-icon，选中标记（对号）用 label-primary
                    color: row.selected ? Theme.textPrimary : Theme.menuIcon
                }
                Text {                                        // 文本
                    id: label
                    anchors.left: lead.right
                    anchors.leftMargin: root.leadW > 0 ? Theme.spaceSm : root.rowPadH
                    anchors.verticalCenter: parent.verticalCenter
                    text: (modelData && modelData.text) ? modelData.text : ""
                    // DSH: 选中行文字**不变色**，选中由对号表达（.selected{background:transparent}）
                    color: Theme.textPrimary
                }
                Text {                                        // 后缀图标（紧贴文本）
                    id: suffix
                    anchors.left: label.right
                    anchors.leftMargin: Theme.spaceSm
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !row.isSep && !!(modelData && modelData.suffix)
                    text: (modelData && modelData.suffix) ? modelData.suffix : ""
                    font.family: iconFont
                    font.pixelSize: Theme.iconMd
                    renderType: Text.NativeRendering
                    color: Theme.textCaption
                }
                Text {                                        // 右侧提示
                    id: hint
                    anchors.right: parent.right
                    anchors.rightMargin: root.rowPadH
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !row.isSep && !!(modelData && modelData.hint)
                    text: (modelData && modelData.hint) ? modelData.hint : ""
                    // DSH: .shortcutKeys 用 label-caption
                    color: Theme.textCaption
                }
                MouseArea {
                    id: rowMa
                    anchors.fill: parent
                    visible: !row.isSep
                    hoverEnabled: true
                    onEntered: root.currentIndex = -1      // 鼠标接管 → 清掉键盘高亮
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.close()
                        root.itemTriggered(row.index)
                    }
                }
            }
        }
    }
}
