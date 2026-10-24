import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ImageBrowser

// ══ 标准面包屑 ═══════════════════════════════════════════════
//   需求（用户明确，见 01-功能与UI设计.md §2.3）：
//     · **起始路径整体锁死，不拆 segment** → 首段 = 「打开时的根」，带锁图标
//     · **可点击任意层级直接跳转**（不必逐级退）；点首段 = 回到打开时的根
//     · 与「上一级」按钮同栏（顶栏第二行）
//   外观：段 = 高 `crumbItemH`、圆角 `radiusXs`；hover / 焦点用 `hoverFill`；
//         首段与中间段 `textSecondary`、**当前段** `textPrimary`；段间 chevron 分隔；
//         焦点环按 §2.5
//   行为：内容超出时**自动滚到末尾**（保证当前层级可见 —— 长路径下最要紧的是当前段）
Item {
    id: root

    property var items: []              // [{ label, locked, siblings:[{label,node}] }]
    signal activated(int index)
    signal siblingActivated(int node)   // 下拉里选了同层的另一个容器

    implicitHeight: Theme.crumbItemH

    function scrollToEnd() {
        flick.contentX = Math.max(0, flick.contentWidth - flick.width)
    }
    onItemsChanged: scrollToEnd()
    onWidthChanged: scrollToEnd()
    Component.onCompleted: scrollToEnd()

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: crumbContent.width
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Row {
            id: crumbContent
            anchors.verticalCenter: parent.verticalCenter
            spacing: 0

            Repeater {
                // ★ 用**段数**当 model：扫描期 `items` 每批都换新数组（兄弟容器在长），
                //   若直接拿数组当 model，Repeater 会把所有段销毁重建 ⇒ 按下与抬起之间
                //   目标消失，dropdown **点不着**（用户 2026-10-24 实测）。
                //   段数（= 层深度）在扫描中不变 ⇒ 段不重建，只重新求值绑定。
                model: root.items.length
                delegate: Row {
                    readonly property var crumbData: root.items[index] || ({})
                    spacing: 0
                    anchors.verticalCenter: parent.verticalCenter

                    // 分隔符（除首项）
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: index > 0
                        text: Theme.icChevronR
                        font.family: iconFont
                        font.pixelSize: Theme.iconSm
                        renderType: Text.NativeRendering
                        color: Theme.textCaption
                        leftPadding: Theme.spaceXs
                        rightPadding: Theme.spaceXs
                    }

                    // 段：首段（锁死的根）带锁图标；任意段都可点，直接跳到该层级
                    Rectangle {
                        id: seg
                        anchors.verticalCenter: parent.verticalCenter
                        width: segRow.width + Theme.spaceSm * 2
                        height: Theme.crumbItemH
                        radius: Theme.radiusXs
                        color: (segMa.containsMouse || seg.activeFocus) ? Theme.hoverFill
                                                                        : "transparent"
                        border.width: (seg.activeFocus && ui.inputFromKeyboard) ? Theme.focusRingW : 0
                        border.color: Theme.brand
                        activeFocusOnTab: true

                        readonly property bool isRoot: crumbData.locked === true
                        readonly property var siblings: crumbData.siblings || []
                        readonly property int selfNode: crumbData.node === undefined ? -1 : crumbData.node
                        readonly property bool isCurrent: index === root.items.length - 1

                        function jump() { root.activated(index) }
                        //   ★ 有同层兄弟 ⇒ **整段点开下拉**（用户 2026-10-24）；
                        //     没有兄弟 ⇒ 仍然是"跳到这一层"
                        function toggle() {
                            if (siblings.length > 1) {
                                if (segMenu.visible) segMenu.close()
                                else segMenu.open()
                            } else {
                                jump()
                            }
                        }

                        Behavior on color { ColorAnimation { duration: Theme.durFast } }

                        Row {
                            id: segRow
                            anchors.centerIn: parent
                            spacing: Theme.spaceXs

                            // 锁图标（仅根段）
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: seg.isRoot
                                text: Theme.icLock
                                font.family: iconFont
                                font.pixelSize: Theme.iconSm
                                renderType: Text.NativeRendering
                                color: Theme.textCaption
                            }
                            // ★ 文本 + 下拉箭头**成组**：箭头紧跟文本、间距用**最小那一档**
                            //   （用户 2026-10-24；此前箭头挂在段上、没有横向位置 ⇒ 压在文字下面看不见）
                            Row {
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: Theme.space2xs
                                Text {
                                    id: labelText
                                    anchors.verticalCenter: parent.verticalCenter
                                    // 限宽 + **中间省略**（路径尾部信息量最大）；tooltip 显示全名
                                    // 锁死的根段是整条路径、通常最长 → 单独压一档（crumbRootMaxW）
                                    width: Math.min(implicitWidth,
                                                    seg.isRoot ? Theme.crumbRootMaxW : Theme.crumbMaxW)
                                    text: crumbData.label
                                    elide: Text.ElideMiddle
                                    color: seg.isCurrent ? Theme.textPrimary
                                         : (segMa.containsMouse || seg.activeFocus) ? Theme.textPrimary
                                                                                    : Theme.textSecondary
                                }
                                // 同层还有其他容器时才出现；**整段都是点击区**（它自己不收点击）
                                Text {
                                    id: caret
                                    anchors.verticalCenter: parent.verticalCenter
                                    visible: seg.siblings.length > 1
                                    text: Theme.icChevronD
                                    font.family: iconFont
                                    font.pixelSize: Theme.iconSm
                                    renderType: Text.NativeRendering
                                    color: (segMa.containsMouse || seg.activeFocus) ? Theme.textPrimary
                                                                                    : Theme.textCaption
                                }
                            }
                        }
                        HintBubble {
                            anchors.fill: parent
                            text: crumbData.label
                            // 只在**被省略**时才提示，否则纯噪音
                            hovered: segMa.containsMouse && labelText.truncated
                        }
                        MouseArea {
                            id: segMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onPressed: seg.forceActiveFocus()
                            onClicked: seg.toggle()
                        }
                        // ★ 同层容器选择器（用户 2026-10-24）：**自带过滤 + 内部滚动 + 限高**。
                        //   兄弟多时不能靠菜单自然撑开（会被窗口裁掉）。
                        PopupPanel {
                            id: segMenu
                            anchorTarget: seg
                            padding: Theme.spaceSm
                            width: Theme.siblingPanelW
                            height: topPadding + filterField.implicitHeight + Theme.spaceXs
                                    + listH + bottomPadding

                            readonly property int rowH: Theme.controlMd
                            // ★ **打开那一刻的快照**（用户 2026-10-24）：`crumbs` 在扫描期每批都换新数组，
                            //   直接绑它 ⇒ 列表每批重建 ⇒ hover 一直闪。浮层打开后不再跟随扫描刷新。
                            property var snapshot: []
                            readonly property var shown: {
                                const src = snapshot
                                const q = filterField.text.trim().toLowerCase()
                                if (q === "") return src
                                return src.filter(function (x) {
                                    return String(x.label).toLowerCase().indexOf(q) >= 0
                                })
                            }
                            readonly property int listH:
                                Math.min(Theme.siblingPanelMaxH, Math.max(1, shown.length) * rowH)

                            onOpened: {
                                snapshot = seg.siblings.slice()   // 冻结：以打开时的同层容器为准
                                filterField.text = ""
                            }

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: Theme.spaceXs

                                SearchField {
                                    id: filterField
                                    Layout.fillWidth: true
                                    placeholder: "过滤同层容器…"
                                }

                                ListView {
                                    id: sibList
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    clip: true
                                    spacing: 0
                                    model: segMenu.shown
                                    boundsBehavior: Flickable.StopAtBounds

                                    delegate: Item {
                                        id: sibRow
                                        required property var modelData
                                        width: sibList.width
                                        height: segMenu.rowH
                                        readonly property bool current: modelData.node === seg.selfNode

                                        Rectangle {
                                            anchors.fill: parent
                                            radius: Theme.radiusMd
                                            color: sibMa.containsMouse ? Theme.hoverFill : "transparent"
                                        }
                                        Text {
                                            anchors.left: parent.left
                                            anchors.leftMargin: Theme.spaceSm
                                            anchors.right: parent.right
                                            anchors.rightMargin: Theme.spaceSm
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: modelData.label
                                            elide: Text.ElideMiddle
                                            color: sibRow.current ? Theme.brand : Theme.textPrimary
                                        }
                                        MouseArea {
                                            id: sibMa
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                const node = sibRow.modelData.node
                                                segMenu.close()
                                                root.siblingActivated(node)
                                            }
                                        }
                                    }

                                    ScrollBar.vertical: ScrollBar {
                                        policy: ScrollBar.AsNeeded
                                        contentItem: Rectangle {
                                            implicitWidth: Theme.scrollBarW
                                            radius: width / 2
                                            color: Theme.borderL2
                                        }
                                        background: Rectangle {
                                            implicitWidth: Theme.scrollBarW
                                            radius: width / 2
                                            color: Theme.hoverFill
                                        }
                                    }
                                }
                            }
                        }
                        Keys.onReturnPressed: seg.jump()
                        Keys.onEnterPressed: seg.jump()
                        Keys.onSpacePressed: seg.jump()
                    }
                }
            }
        }
    }
}
