import QtQuick
import QtQuick.Layouts
import AppUi

// ══ 标准菜单（popup menu）══════════════════════════════════════
//   行结构来源：vsr-player/src/client/ui/OpenMenu.qml / ContextMenu.qml
//     · 行 = [前导图标] 文本 [紧跟文本的后缀图标] … [右侧提示] [▸]，整行 hover，可插分隔线
//   外观（底 / 描边 / 圆角）继承 PopupPanel（DSH 令牌）
//
//   ★ 二级菜单（2026-10-31，用户："缩放算法参考 vsr-player 做二级菜单"）：
//     · 条目带 `submenu: [{label, value}]` ⇒ 悬停 / 点击在**右侧**展开面板；
//     · `currentValue` = 当前值（子项 `value` 相同的那一行打勾）；
//     · 子项点击 ⇒ `subItemTriggered(row, sub)`，取值与分发交给调用方；
//     · **画在同一个 Popup 里**（展开时 popup 加宽、右侧留一列），不另开第二个 popup ——
//       两个 popup 会互抢 grab，点面板会被外层判成"点击外部" ⇒ 菜单自己关掉（vsr-player 实测）。
//
//   ★ 分隔线（`sep`）**上下各留 `spaceXs`** —— 分割线不许贴着菜单项（用户 2026-10-31）。
//
//   items 每项：{ icon, text, suffix, hint, selected, sep, enabled, keepOpen, action,
//                 submenu, currentValue }
//     icon        前导图标（有任意一项带 icon 时，全表保留图标列 → 文本左沿对齐）。
//                 **选中标记就用它**（`selected` 行给 `icon: Theme.icCheck`）
//     suffix      紧跟文本之后的图标（如排序方向）
//     hint        右对齐的提示（快捷键 / **当前档位**，如二级菜单入口显示当前算法名）
//     selected    选中态（文本与图标用主色）
//     sep         分隔线行（上下留白见上）
//     enabled     `false` ⇒ 禁用（整行 dim、不响应 hover / 点击 / 键盘）
//     keepOpen    `true` ⇒ 点了不关菜单（如「加速 / 减速」可以连点）
//     action      语义动作名（调用方在 `itemTriggered` 里读它，**不要用下标**）
//
//   ★ 必须用 ColumnLayout 而不是 Column：
//     Column 的隐式宽度取**子项的 width**，而这里的行宽又要撑满列宽 →
//     互相依赖成环，最终隐式尺寸为 0，弹出框只剩 padding（表现为一个小圆点）。
//     Layout 把「测量用的 preferredWidth」与「实际 width」分开，环就断了。
PopupPanel {
    id: root

    property var items: []
    signal itemTriggered(int index)
    // 二级菜单里选中一项：`row` = 主菜单行下标，`sub` = 子项下标
    signal subItemTriggered(int row, int sub)
    //   缩放行（`{ zoomRow: true }`）：放大 / 缩小 / 点百分比回到 100%
    //   ★ 百分比走**独立属性** `zoomPercent`，**不放进 `items`** —— 放进去的话
    //     每次缩放都会让 items 数组换新 ⇒ Repeater 把整张菜单的 delegate 全重建
    //     （实测：一次缩放重建 17 行）⇒ 菜单自己闪一下（"抖动"），二级面板也会被收起。
    property real zoomPercent: 1.0
    signal zoomInRequested()
    signal zoomOutRequested()
    signal zoomResetRequested()

    readonly property int rowPadH: Theme.spaceSm
    readonly property int sepPadV: Theme.spaceXs          // ★ 分隔线上下留白（用户 2026-10-31）
    readonly property int subGap:   Theme.spaceXs         // 主列与二级面板之间
    readonly property int subMenuW: Theme.subMenuW
    //   二级面板在**内容项坐标**里的 x（主面板右沿 + 间距 - 左内边距）
    readonly property real subX: mainW + subGap - leftPadding

    // ── 二级菜单：当前展开的行（-1 = 收起）──
    property int subRow: -1
    readonly property var subItems: {
        if (subRow < 0 || subRow >= items.length)
            return []
        const it = items[subRow]
        return (it && it.submenu) ? it.submenu : []
    }
    readonly property bool subOpen: subItems.length > 0
    readonly property int subPanelH: subItems.length * Theme.controlMd
    //   二级面板与**展开它的那一行**顶对齐；放不下就整体上移（不超出主列）。
    //   ⚠️ 取委托的**实际布局位置**，不用数组下标重推行高（隐藏行会让重算错位 —— vsr-player 实测）。
    readonly property real subY: {
        if (!subOpen)
            return 0
        let y = 0
        const kids = column.children
        for (let i = 0; i < kids.length; ++i) {
            if (kids[i].itemIndex === subRow) { y = kids[i].y; break }
        }
        const maxY = Math.max(0, column.implicitHeight - subPanelH)
        return Math.min(y, maxY)
    }

    readonly property bool hasLeadingIcons: {
        for (var i = 0; i < items.length; ++i)
            if (items[i] && items[i].icon)
                return true
        return false
    }
    readonly property int leadW: hasLeadingIcons ? Theme.iconMd : 0

    // ── 键盘操作（方向键选行 / Enter 激活 / Esc 关闭由 closePolicy 处理）──
    property int currentIndex: -1

    //   可选中 = 不是分隔线、没被禁用
    function pickable(i) {
        const it = items[i]
        return !!it && it.sep !== true && it.enabled !== false && it.zoomRow !== true
    }
    function firstSelectableIndex() {
        for (var i = 0; i < items.length; ++i)
            if (pickable(i))
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
            if (pickable(i)) {
                currentIndex = i
                subRow = items[i].submenu ? i : -1   // 键盘移到带二级的行 ⇒ 展开它
                return
            }
        }
    }
    function activateCurrent() {
        if (currentIndex < 0 || !pickable(currentIndex))
            return
        const it = items[currentIndex]
        if (it.submenu) {          // 二级菜单入口：展开（子项本身鼠标点选）
            subRow = currentIndex
            return
        }
        const i = currentIndex
        close()
        itemTriggered(i)
    }

    onOpened: {
        // ★ 鼠标打开时**不高亮任何项**（否则会像"默认选中第一项"，误导用户）
        currentIndex = ui.inputFromKeyboard ? firstSelectableIndex() : -1
        subRow = -1
        column.forceActiveFocus()          // 键盘事件交给菜单内容项
    }
    onClosed: subRow = -1

    //   外观 = **两块独立面板**（主菜单 + 二级面板），与 vsr-player 的二级菜单一致：
    //   二级面板有自己的圆角与描边，靠右、与展开它的那一行顶对齐 —— 不是"一个加宽的大面板"。
    background: Item {
        Rectangle {                                        // 主面板
            width: root.mainW
            height: root.height
            color: Theme.menuFill
            radius: Theme.radiusLg
            border.width: Theme.borderW
            border.color: Theme.borderL4
        }
        Rectangle {                                        // 二级面板（只在展开时出现）
            visible: root.subOpen
            x: root.mainW + root.subGap
            y: root.topPadding + root.subY - Theme.spaceXs
            width: root.subMenuW
            height: root.subPanelH + Theme.spaceXs * 2
            color: Theme.menuFill
            radius: Theme.radiusLg
            border.width: Theme.borderW
            border.color: Theme.borderL4
        }
    }

    padding: Theme.spaceXs                      // DSH: .list { padding: 4px }
    // 主列宽固定；二级面板展开时只**加宽 popup**（主列不会跟着变宽）
    readonly property int mainW: Math.max(Theme.menuMinW,
                                          column.implicitWidth + leftPadding + rightPadding)
    width: implicitWidth
    height: implicitHeight
    // DSH: .list { min-width: 144px } —— 太窄时 radius-lg 的圆角会显得突兀
    implicitWidth: mainW + (subOpen ? subGap + subMenuW : 0)
    //   钳制时一直预留二级面板的宽度（见 PopupPanel.clampExtraWidth）
    clampExtraWidth: subGap + subMenuW
    implicitHeight: column.implicitHeight + topPadding + bottomPadding

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.top: parent.top
        width: root.mainW - root.leftPadding - root.rightPadding
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
                readonly property bool itemEnabled: !(modelData && modelData.enabled === false)
                readonly property bool selected: !!(modelData && modelData.selected)
                readonly property bool hasSub: !!(modelData && modelData.submenu)
                readonly property bool subActive: root.subRow === index && root.subOpen
                readonly property bool zoomRow: !!(modelData && modelData.zoomRow)
                readonly property int itemIndex: index

                Layout.fillWidth: true
                //   ★ 分隔线行 = 线 + 上下 `sepPadV` 的留白（不和菜单项贴在一起）
                Layout.preferredHeight: isSep ? Theme.hairline + root.sepPadV * 2
                                              : Theme.controlMd
                Layout.preferredWidth: zoomRow ? 264
                                                : root.rowPadH * 2
                    + (root.leadW > 0 ? root.leadW + Theme.spaceSm : 0)
                    + label.implicitWidth
                    + (suffix.visible ? Theme.spaceSm + suffix.implicitWidth : 0)
                    + (chevron.visible ? Theme.spaceXs + chevron.implicitWidth : 0)
                    + (hint.visible ? Theme.spaceMd + hint.implicitWidth : 0)
                opacity: row.isSep ? 1.0 : (row.itemEnabled ? 1.0 : Theme.dimOpacity)

                // ── 缩放行（Chrome 形式：`🔍 缩放        − 43% +`）──────────────
                //   ★ 这一行**不是动作行**：整行没有 hover、也没有点击行为；
                //     只有 − / 百分比 / + 三个控件可交互（用户 2026-11-02）。
                Rectangle {
                    anchors.fill: parent
                    visible: row.zoomRow
                    radius: Theme.radiusMd
                    color: Theme.menuFill

                    Text {                                    // 前导图标（与其它行的图标列对齐）
                        anchors.left: parent.left
                        anchors.leftMargin: root.rowPadH
                        anchors.verticalCenter: parent.verticalCenter
                        width: root.leadW
                        text: Theme.icSearch
                        font.family: iconFont
                        font.pixelSize: Theme.iconMd
                        renderType: Text.NativeRendering
                        color: Theme.textSecondary
                    }
                    Text {                                    // 标题（与其它行的文字列对齐）
                        anchors.left: parent.left
                        anchors.leftMargin: root.rowPadH + root.leadW + Theme.spaceSm
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("缩放")
                        color: Theme.textPrimary
                    }
                    Row {
                        anchors.right: parent.right
                        anchors.rightMargin: root.rowPadH
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.spaceXs
                        IconAction {                          // [−]（小号，菜单行里留出上下间距）
                            size: "small"
                            focusOnClick: false
                            glyph: Theme.icMinus
                            onTriggered: root.zoomOutRequested()
                        }
                        Text {                                // 百分比（固定宽度并居中，点击 = 回到 100%）
                            width: 44
                            horizontalAlignment: Text.AlignHCenter
                            anchors.verticalCenter: parent.verticalCenter
                            text: Math.round((root.zoomPercent || 1) * 100) + "%"
                            color: zPct.containsMouse ? Theme.brand : Theme.textPrimary
                            MouseArea {
                                id: zPct
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.zoomResetRequested()
                            }
                        }
                        IconAction {                          // [+]
                            size: "small"
                            focusOnClick: false
                            glyph: Theme.icPlus
                            onTriggered: root.zoomInRequested()
                        }
                    }
                }

                Rectangle {                                   // hover 底（DSH: 行圆角 = radius-md）
                    anchors.fill: parent
                    visible: !row.isSep && !row.zoomRow
                    radius: Theme.radiusMd
                    color: (row.itemEnabled
                            && (rowMa.containsMouse || row.index === root.currentIndex || row.subActive))
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
                Text {                                        // 右侧提示（快捷键 / 当前档位）
                    id: hint
                    anchors.right: chevron.visible ? chevron.left : parent.right
                    anchors.rightMargin: chevron.visible ? Theme.spaceXs : root.rowPadH
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !row.isSep && !!(modelData && modelData.hint)
                    text: (modelData && modelData.hint) ? modelData.hint : ""
                    // DSH: .shortcutKeys 用 label-caption
                    color: Theme.textCaption
                }
                Text {                                        // 二级菜单箭头（▸）
                    id: chevron
                    anchors.right: parent.right
                    anchors.rightMargin: root.rowPadH
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !row.isSep && row.hasSub
                    text: Theme.icChevronR
                    font.family: iconFont
                    font.pixelSize: Theme.iconSm
                    renderType: Text.NativeRendering
                    color: Theme.textCaption
                }
                MouseArea {
                    id: rowMa
                    anchors.fill: parent
                    visible: !row.isSep && !row.zoomRow     // 缩放行整行不可点（只有控件可点）
                    enabled: row.itemEnabled && !row.zoomRow // ★ 禁用行不响应
                    hoverEnabled: true
                    onEntered: {
                        root.currentIndex = -1             // 鼠标接管 → 清掉键盘高亮
                        root.subRow = row.hasSub ? row.index : -1
                    }
                    cursorShape: row.itemEnabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: {
                        const keep = !!(modelData && modelData.keepOpen)
                        if (row.hasSub) {                  // 二级菜单入口：只展开，不关菜单
                            root.subRow = row.index
                            return
                        }
                        //   ⚠️ 先把菜单对象**取成局部变量**：close() 之后行可能被销毁，
                        //      此时再解析 id（`root`）会报 "ReferenceError: root is not defined"。
                        const menu = root
                        const idx = row.index
                        if (!keep)
                            menu.close()
                        menu.itemTriggered(idx)
                    }
                }
            }
        }
    }

    // ── 二级面板：主列右侧（同一个 Popup 里；竖线分隔）──────────────
    Column {
        id: subColumn
        visible: root.subOpen
        x: root.subX
        y: root.subY
        width: root.subMenuW
        spacing: 0

        Repeater {
            model: root.subItems

            delegate: Item {
                id: subRowItem
                required property var modelData
                required property int index

                readonly property bool current: {
                    const mainIt = (root.subRow >= 0 && root.subRow < root.items.length)
                                   ? root.items[root.subRow] : null
                    return !!mainIt && mainIt.currentValue === modelData.value
                }

                width: subColumn.width
                height: Theme.controlMd

                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radiusMd
                    color: subMa.containsMouse ? Theme.hoverFill : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.durFast } }
                }
                Text {                                        // 选中标记（与主菜单同一套对号）
                    anchors.left: parent.left
                    anchors.leftMargin: root.rowPadH
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.iconMd
                    text: subRowItem.current ? Theme.icCheck : ""
                    font.family: iconFont
                    font.pixelSize: Theme.iconMd
                    renderType: Text.NativeRendering
                    color: Theme.textPrimary
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: root.rowPadH + Theme.iconMd + Theme.spaceSm
                    anchors.verticalCenter: parent.verticalCenter
                    text: (modelData && modelData.label) ? modelData.label : ""
                    color: Theme.textPrimary
                }
                MouseArea {
                    id: subMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        //   ⚠️ close() 会让 `subItems` 清空 ⇒ **本行被销毁**，之后解析 id
                        //      （`root`）会报 "ReferenceError: root is not defined"。
                        //      所以先把对象和下标取成局部变量，再关、再发信号。
                        const menu = root
                        const r = menu.subRow
                        const s = subRowItem.index
                        menu.close()               // 先关（调用方的动作可能会开对话框）
                        menu.subItemTriggered(r, s)
                    }
                }
            }
        }
    }
}
