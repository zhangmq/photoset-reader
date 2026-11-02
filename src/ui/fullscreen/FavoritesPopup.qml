import QtQuick
import QtQuick.Controls
import AppUi

// ══ 收藏夹（弹窗列表）══════════════════════════════════════════════════════
//   设计 20 §7。数据 = `ui.favorites`（`ProgressListModel`）：**只列文件名**；
//   行里的源路径 / N/M 是**按需读**的（可见行才读）。
//   形态：居中弹窗 = 顶部过滤框 + 列表；每行 = 甜甜圈（阅读百分比）+ 名称（悬停看完整路径）
//   + 当前位置/文件数量 + 移除按钮；点行 = 打开那份进度文件。
Item {
    id: root

    property var model: ui.favorites

    function open() { popup.open() }
    function close() { popup.close() }

    PopupPanel {
        id: popup
        anchors.centerIn: Overlay.overlay
        padding: Theme.spaceMd
        width: Theme.favPopupW
        height: Theme.favPopupH

        onOpened: {
            filterInput.text = ""
            root.model.setFilter("")
            root.model.reload()
            list.currentIndex = -1
            list.forceActiveFocus()
        }

        Column {
            anchors.fill: parent
            spacing: Theme.spaceMd

            SearchField {
                id: filterInput
                width: parent.width
                placeholder: qsTr("Filter by name")
                onTextChanged: root.model.setFilter(text)
            }

            Item {
                width: parent.width
                height: parent.height - filterInput.height - Theme.spaceMd

                ListView {
                    id: list
                    anchors.fill: parent
                    clip: true
                    spacing: Theme.space2xs
                    model: root.model
                    visible: count > 0
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    Keys.onUpPressed: if (currentIndex > 0) currentIndex--
                    Keys.onDownPressed: if (currentIndex < count - 1) currentIndex++
                    Keys.onReturnPressed: if (currentIndex >= 0) root.openAt(currentIndex)
                    Keys.onEnterPressed: if (currentIndex >= 0) root.openAt(currentIndex)

                    delegate: Item {
                        id: row
                        required property int index
                        required property string title
                        required property string file
                        required property string sourcePath
                        required property int done
                        required property int total
                        required property real ratio
                        required property string errorText

                        width: list.width
                        //   行内几何**照菜单行的规范**（06 §3.8）：行高 controlMd、行内边距 spaceSm
                        height: Theme.controlMd

                        Rectangle {                                   // hover 底
                            anchors.fill: parent
                            radius: Theme.radiusMd
                            color: (rowMa.containsMouse || list.currentIndex === row.index)
                                 ? Theme.hoverFill : "transparent"
                            Behavior on color { ColorAnimation { duration: Theme.durFast } }
                        }

                        // 行点击（声明在按钮之前：按钮后声明才在上层，否则 ✕ 会被这一层吃掉）
                        MouseArea {
                            id: rowMa
                            anchors.fill: parent
                            acceptedButtons: Qt.LeftButton
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.openAt(row.index)
                        }

                        // 阅读百分比：与顶栏 spin **同一份绘制**（`Ring`），只是不加动画
                        Ring {
                            anchors.left: parent.left
                            anchors.leftMargin: Theme.spaceSm
                            anchors.verticalCenter: parent.verticalCenter
                            size: Theme.donutSize
                            lineWidth: 2
                            arcDegrees: Math.max(0, Math.min(1, row.ratio)) * 360
                        }

                        //   ★ 2026-11-01（用户）：列表里显示**源的路径**（不是文件名），
                        //     放不下时用**中间省略**（`/path/to…/文件名`）—— 这是路径的常规省略方式：
                        //     头（根/上级目录）与尾（文件名）都保留，只省略中间。
                        Text {
                            id: titleText
                            anchors.left: parent.left
                            anchors.leftMargin: Theme.spaceSm + Theme.donutSize + Theme.spaceSm
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - x - progressText.width - removeBtn.width
                                   - Theme.spaceSm * 3
                            text: row.sourcePath !== "" ? row.sourcePath : row.file
                            color: Theme.textPrimary
                            elide: Text.ElideMiddle
                            wrapMode: Text.NoWrap
                        }

                        Text {
                            id: progressText
                            anchors.right: removeBtn.left
                            anchors.rightMargin: Theme.spaceSm
                            anchors.verticalCenter: parent.verticalCenter
                            text: row.errorText !== "" ? qsTr("Unreadable")
                                                       : (row.done + " / " + row.total)
                            color: row.errorText !== "" ? Theme.danger : Theme.textCaption
                        }

                        IconAction {
                            id: removeBtn
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.spaceXs
                            anchors.verticalCenter: parent.verticalCenter
                            glyph: Theme.icClose
                            tooltip: qsTr("Remove")
                            danger: true
                            onTriggered: {
                                root.pendingRemove = row.file
                                root.pendingTitle = row.title
                                removeDialog.open()
                            }
                        }

                        HintBubble {                                  // 悬停：源的完整路径
                            anchors.fill: titleText
                            text: row.sourcePath !== "" ? row.sourcePath : row.file
                            hovered: rowMa.containsMouse
                        }
                    }
                }

                // 空态（没有文件 / 过滤没命中）—— 空态是唯一允许放说明文字的地方
                Text {
                    anchors.centerIn: parent
                    width: parent.width - Theme.spaceXl * 2
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    visible: list.count === 0
                    text: filterInput.text !== ""
                              ? qsTr("No matching progress files")
                              : qsTr("No progress files yet.\nUse “Save progress to favorites” in the right-click menu to remember the current source here.")
                    color: Theme.textSecondary
                }
            }
        }
    }

    // 行操作
    property string pendingRemove: ""
    property string pendingTitle: ""
    function openAt(row) {
        const file = root.model.fileAt(row)
        popup.close()
        if (file !== "")
            ui.openProgress(file)
    }

    ConfirmDialog {
        id: removeDialog
        title: qsTr("Remove progress record")
        message: qsTr("Delete the progress record for “%1”? The source files are not deleted.")
                     .arg(root.pendingTitle)
        acceptText: qsTr("Remove")
        danger: true
        onAccepted: {
            ui.removeProgressFile(root.pendingRemove)
            root.pendingRemove = ""
            root.pendingTitle = ""
        }
    }
}
