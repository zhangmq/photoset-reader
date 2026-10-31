import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AppUi

// ══ 图集库抽屉（左侧 overlay；**不是屏、不进屏栈**，设计 18 §2）══════════════
//   形状：宽 `Theme.drawerW`(360)，占满窗高；打开/关闭**不参与任何 Layout**
//         ⇒ 底下的 Browse / Viewer 不重排（它们照旧铺满窗口）。
//   内容（自上而下，两条 bar 规则照旧：满宽 + spaceLg padding + 1px bottom border）：
//     ① 标题条：左「图集库」                右「刷新」「关闭」
//     ② 卡片列表（无数据 → 空态块：文案 + 「打开一个文件夹」）
//   打开方式：Browse / Viewer 顶栏的 folder-star 按钮、`Ctrl+B`、Browse「打开 ▾ → 收藏夹」
//   关闭方式：点外部（Main.qml 的遮罩）/ `Esc` / 再点按钮 / 「关闭」
//   ★ 图集 = 收藏夹：没有「添加图集」—— 卡片上的「重新扫描」是**唯一**的内容更新手段
//     （收藏入口在 Viewer 顶栏，本段只放按钮、规则见段 3）。
//   数据：`ui.library`（SQLite）；卡片上的扫描状态/动作见 CollectionCard。
Rectangle {
    id: root

    // 键盘面：抽屉打开时它拿焦点（Esc 关抽屉；Tab 在刷新/关闭/卡片间走）
    //   ⚠️ 两屏的 `focus` 都带 `!ui.libraryOpen`（Main.qml）⇒ 同时只有一个焦点面
    focus: ui.libraryOpen
    Keys.onEscapePressed: ui.libraryOpen = false

    signal folderRequested()
    signal collectionActivated(int index)
    signal collectionRemoveRequested(int index)

    color: Theme.bgBase
    // 右缘 1px：抽屉浮在画面上，用它跟底下的屏分开（bar 规则的同一根线）
    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Theme.hairline
        color: Theme.borderL1
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ══ ① 标题条 ══════════════════════════════════
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.barH

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceLg
                anchors.rightMargin: Theme.spaceLg
                spacing: Theme.spaceSm

                Text {
                    Layout.alignment: Qt.AlignVCenter
                    text: "图集库"
                    color: Theme.textPrimary
                }

                Item { Layout.fillWidth: true }   // 撑开

                // 刷新：取一次库快照（跨进程同步；本进程的扫描进度仍由 `pumped` 实时刷）
                IconAction {
                    Layout.alignment: Qt.AlignVCenter
                    glyph: Theme.icRefresh
                    tooltip: "刷新"
                    onTriggered: ui.library.pollNow()
                }
                IconAction {
                    Layout.alignment: Qt.AlignVCenter
                    glyph: Theme.icClose
                    tooltip: "关闭"
                    onTriggered: ui.libraryOpen = false
                }
            }

            Rectangle {   // 下边框（bar 自持）
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: Theme.hairline
                color: Theme.borderL1
            }
        }

        // ══ ② 卡片列表 / 空态 ═════════════════════════
        Loader {
            Layout.fillWidth: true
            Layout.fillHeight: true
            sourceComponent: ui.library.count > 0 ? listComp : emptyComp
        }
    }

    Component {
        id: listComp
        ListView {
            id: list
            clip: true
            // roles: collectionId / name / path / addedAt / lastViewedAt / progressValue /
            //        progressText / missing / cover / scanState / scannedItems / resetCommitting
            model: ui.library
            leftMargin: Theme.spaceMd
            rightMargin: Theme.spaceMd
            topMargin: Theme.spaceMd
            bottomMargin: Theme.spaceMd
            spacing: Theme.spaceMd

            delegate: CollectionCard {
                // 抽屉里一列铺满（不再限宽）；`compact` = 只留标题/状态/缩略图（设计 18 §2 的内容）
                width: list.width - Theme.spaceMd * 2
                compact: true
                collectionId: model.collectionId
                name: model.name
                path: model.path
                addedAt: model.addedAt
                lastViewedAt: model.lastViewedAt
                progressValue: model.progressValue
                progressText: model.progressText
                missing: model.missing
                cover: model.cover
                scanState: model.scanState
                scannedItems: model.scannedItems
                resetCommitting: model.resetCommitting
                onActivated: root.collectionActivated(index)
                onRemoveRequested: root.collectionRemoveRequested(index)
                // 重置 / 取消：**走 core**（`LibraryCore::requestReset/cancelReset`，设计 14 §9.1）
                //   —— 卡片只发信号、动作由 `ui` 执行（见 07 §三）。
                onScanRequested: ui.requestReset(model.collectionId)
                onCancelScanRequested: ui.cancelReset(model.collectionId)
            }

            // 滚动条：hover 才显，拖动时保持可见（与浏览屏同一实现）
            HoverHandler { id: listHover }
            ScrollBar.vertical: ScrollBar {
                id: listBar
                policy: ScrollBar.AsNeeded
                opacity: (listHover.hovered || listBar.hovered || listBar.pressed) ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
                contentItem: Rectangle {
                    implicitWidth: Theme.scrollBarW
                    radius: width / 2
                    color: listBar.pressed ? Theme.textSecondary : Theme.borderL2
                }
                background: Rectangle {
                    implicitWidth: Theme.scrollBarW
                    radius: width / 2
                    color: Theme.hoverFill
                }
            }
        }
    }

    Component {
        id: emptyComp
        EmptyState {
            onFolderRequested: root.folderRequested()
        }
    }
}
