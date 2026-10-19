import QtQuick
import QtQuick.Layouts
import ImageBrowser

// ══ 图集库（首页）══════════════════════════════════════════
// 自上而下：
//   ① 动作条：左「添加图集」「打开▾」        右「主题」「设置」
//   ② 列表标题行：左「图集列表」+ 数量徽标   右「过滤框」「排序」
//   ③ 图集卡片列表（无数据 → 空态引导）
// ①② 都是 bar：共用 barH、满宽 + spaceLg padding、1px bottom border。
// 数据：`ui.library`（SQLite，轮询 data_version 同步）；卡片上的扫描状态/动作见 CollectionCard。
// 仍是纯 UI 的部分：过滤与排序**尚未接数据源**（信号发出、无人接收）。
Item {
    id: root
    //   ★ 键盘面：只有**当前这一屏**取焦点（三屏共存时不让隐藏屏抢走焦点）
    focus: ui.screen === UiModel.Library
    //   ★ 全局键（三屏通用）：`Esc` = **退出全屏**。键事件从焦点项沿父链上冒 ⇒
    //     菜单 / 对话框 / 输入框自己处理 `Esc` 时**轮不到这里**（不会互相抢）。
    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Escape) {
            ui.windowFullscreen = false
            event.accepted = true
        }
    }

    property string sortKey: "lastViewed"      // name | added | lastViewed
    property bool   sortAsc: false
    property string filterText: ""
    property bool   debugOpenSort: false          // 调试：截图时自动展开排序菜单
    property bool   debugOpenSettings: false      // 调试：截图时自动打开设置
    property int    debugSettingsTab: 0           // 调试：设置弹框开在哪一页（0常规/1图像/2系统/3快捷键）

    // 点当前排序项 = 切换方向；点其它项 = 切换字段（默认降序）
    function pickSort(key) {
        if (sortKey === key) sortAsc = !sortAsc
        else { sortKey = key; sortAsc = false }
    }

    signal folderRequested()
    signal archiveRequested()
    signal imageRequested()
    signal addCollectionRequested()
    signal settingsRequested()
    onSettingsRequested: settingsDialog.open()   // [设置] 按钮 → 打开设置对话框
    signal collectionActivated(int index)
    signal collectionRemoveRequested(int index)

    Rectangle {
        anchors.fill: parent
        color: Theme.bgBase

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // ══ 顶部动作条 ══════════════════════════════════
            //   左：添加图集 | 打开▾        右：主题 | 设置
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.barH

                // 满宽 + 内部 padding（不做宽度上限、不居中）
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spaceLg
                    anchors.rightMargin: Theme.spaceLg
                    spacing: Theme.spaceSm

                    // ── 左：添加图集 ──
                    Button {
                        Layout.alignment: Qt.AlignVCenter
                        glyph: Theme.icPlus
                        label: "添加图集"
                        onClicked: root.addCollectionRequested()
                    }

                    // ── 左：打开（下拉：文件夹 / 压缩包 / 图片）──
                    //   菜单由按钮自己持有（DropdownButton），父级不回填开合状态
                    DropdownButton {
                        Layout.alignment: Qt.AlignVCenter
                        glyph: Theme.icFolder
                        label: "打开"
                        menuItems: [
                            { icon: Theme.icFolder, text: "文件夹" },
                            { icon: Theme.icZip,    text: "压缩包" },
                            { icon: Theme.icImage,  text: "图片" }
                        ]
                        onItemTriggered: function (index) {
                            if (index === 0)      root.folderRequested()
                            else if (index === 1) root.archiveRequested()
                            else                  root.imageRequested()
                        }
                    }

                    Item { Layout.fillWidth: true }   // 撑开

                    // ── 右：主题（三态循环）──
                    IconAction {
                        Layout.alignment: Qt.AlignVCenter
                        glyph: ui.themeMode === 2 ? Theme.icThemeDark
                             : ui.themeMode === 1 ? Theme.icThemeLight
                             : Theme.icThemeSystem
                        tooltip: ui.themeMode === 2 ? "深色"
                               : ui.themeMode === 1 ? "浅色"
                               : "跟随系统"
                        onTriggered: ui.cycleThemeMode()
                    }
                    // ── 右：设置（占位）──
                    IconAction {
                        Layout.alignment: Qt.AlignVCenter
                        glyph: Theme.icSettings
                        tooltip: "设置"
                        onTriggered: root.settingsRequested()
                    }
                }

                Rectangle {   // 下边框（所有 bar 统一：1px bottom border）
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: Theme.hairline
                    color: Theme.borderL1
                }
            }

            // ══ 列表标题栏（与动作条同：1px bottom border）══════
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.barH
                color: Theme.bgBase

                Rectangle {   // 下边框
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: Theme.hairline
                    color: Theme.borderL1
                }

                // ★ 与动作条同一套布局规则：**满宽 + 内部 spaceLg padding**，
                //   不做宽度上限、不居中（曾经的 anchors.fill + horizontalCenter + width
                //   会互相覆盖，导致左 padding 归零、右端控件贴窗口边）
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spaceLg
                    anchors.rightMargin: Theme.spaceLg
                    spacing: Theme.spaceSm

                    // 左：标题 + 数量徽标
                    Text {
                        Layout.alignment: Qt.AlignVCenter
                        text: "图集列表"
                        color: Theme.textPrimary
                    }
                    Badge {
                        Layout.alignment: Qt.AlignVCenter
                        label: String(ui.library.count)   // 用 Badge 默认色（= DSH rowTag）
                    }

                    Item { Layout.fillWidth: true }   // 撑开

                    // 右：过滤（标准搜索框组件：前缀图标 + 输入 + 清除按钮）
                    SearchField {
                        id: filterField
                        Layout.alignment: Qt.AlignVCenter
                        placeholder: "过滤…"
                        onTextChanged: root.filterText = text
                    }

                    // 右：排序（标准图标下拉按钮；菜单由按钮自己持有）
                    IconDropdownButton {
                        id: sortBtn
                        Layout.alignment: Qt.AlignVCenter
                        glyph: Theme.icSort
                        tooltip: "排序"
                        menuItems: [
                            { icon: root.sortKey === "name" ? Theme.icCheck : "",
                              text: "标题",
                              suffix: root.sortKey === "name" ? (root.sortAsc ? Theme.icAsc : Theme.icDesc) : "",
                              selected: root.sortKey === "name" },
                            { icon: root.sortKey === "lastViewed" ? Theme.icCheck : "",
                              text: "浏览时间",
                              suffix: root.sortKey === "lastViewed" ? (root.sortAsc ? Theme.icAsc : Theme.icDesc) : "",
                              selected: root.sortKey === "lastViewed" },
                            { icon: root.sortKey === "added" ? Theme.icCheck : "",
                              text: "加入时间",
                              suffix: root.sortKey === "added" ? (root.sortAsc ? Theme.icAsc : Theme.icDesc) : "",
                              selected: root.sortKey === "added" }
                        ]
                        onItemTriggered: function (index) {
                            root.pickSort(["name", "lastViewed", "added"][index])
                        }
                        Component.onCompleted: if (root.debugOpenSort) openMenu()
                    }
                }
            }

            // ══ 卡片列表 ═══════════════════════════════════
            Loader {
                Layout.fillWidth: true
                Layout.fillHeight: true
                sourceComponent: ui.library.count > 0 ? listComp : emptyComp
            }
        }
    }

    // ── 设置对话框（三屏同一实现；当前阶段单纯 UI）──
    SettingsDialog {
        id: settingsDialog
    }
    Component.onCompleted: if (root.debugOpenSettings) {
        settingsDialog.tab = root.debugSettingsTab
        settingsDialog.open()
    }

    // ══ 浮层 ══════════════════════════════════════════════
    //   「打开」与「排序」两个菜单都不在这里 —— 由各自的按钮自己持有

    Component {
        id: listComp
        ListView {
            id: list
            clip: true
            // 图集库列表**永远来自 SQLite**（设计 11 §四）：`ui.library` 轮询 data_version 同步
            //   roles: collectionId / name / path / addedAt / lastViewedAt / progressValue /
            //          progressText / missing / cover / scanState / scannedItems / resetCommitting
            model: ui.library
            leftMargin: Theme.spaceMd
            rightMargin: Theme.spaceMd
            topMargin: Theme.spaceMd
            bottomMargin: Theme.spaceMd
            spacing: Theme.spaceMd

            delegate: CollectionCard {
                // 最大宽度限制：宽屏上卡片不会被拉得过长
                width: Math.min(list.width - Theme.spaceMd * 2, Theme.cardMaxW)
                anchors.horizontalCenter: parent ? parent.horizontalCenter : undefined
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
        }
    }

    Component { id: emptyComp; EmptyState {} }
}
