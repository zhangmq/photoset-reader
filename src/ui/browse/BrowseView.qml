import QtQuick
import QtQuick.Layouts
import ImageBrowser

// ══ 浏览界面 ══════════════════════════════════════════════
// 约束（设计 §04）：
//   · **无侧栏**（用户明确）
//   · **无"打开"能力**（用户明确）—— 打开只在图集库
//   · 聚焦当前范围内各层级浏览
//   · 上一级到「打开时的根」即禁用
// 数据：占位模型 PlaceholderBrowse（roles 与将来 C++ 模型一致）
Item {
    id: root

    property bool gridMode: true
    property bool favorited: false
    property bool canGoUp: false
    // 范围指示（设计 §04）：在图集内 → 图集《名》；无痕 → 未收藏 · 本次不记进度
    property string rangeLabel: "图集《龙珠 第01卷》"
    // 当前路径下的过滤词（**仅用于定位，不改变序列** —— 序列始终是完整的那一份）
    property string filterText: ""
    property bool   debugOpenSettings: false      // 调试：截图时自动打开设置
    // 面包屑：首段 = **锁死的根**（整体，不拆）；后续 = 根内子层级
    // 占位：**多段**（长根 + 3 级子层）—— 用于验证锁图标/分隔符/当前段/横向滚动
    property var crumbs: [
        { label: "/run/media/zmq/Home/Dragon Ball/[七龙珠完全版][DRAGON BALL][鸟山明][文传][01卷].rar", locked: true },
        { label: "01", locked: false },
        { label: "扫描", locked: false },
        { label: "已修图", locked: false }
    ]
    property int currentIndex: 0

    signal libraryRequested()
    signal settingsRequested()
    onSettingsRequested: settingsDialog.open()   // [设置] 按钮 → 打开设置对话框
    signal upRequested()
    signal refreshRequested()
    signal favoriteToggled()
    signal itemActivated(int index)
    signal itemOpened(int index)
    signal crumbClicked(int index)

    Rectangle {
        anchors.fill: parent
        color: Theme.bgBase

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // ══ bar 1：动作条（返回图集库 + 图集名称 / 收藏 + 设置）══
            BrowseActionBar {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.barH
                favorited: root.favorited
                rangeLabel: root.rangeLabel
                onLibraryRequested: root.libraryRequested()
                onFavoriteToggled: root.favoriteToggled()
                onSettingsRequested: root.settingsRequested()
            }

            // ══ bar 2：路径条（面包屑 + 过滤 + 上一级/刷新/视图切换）══
            BrowsePathBar {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.barH
                crumbs: root.crumbs
                filterText: root.filterText
                canGoUp: root.canGoUp
                gridMode: root.gridMode
                onCrumbClicked: (i) => root.crumbClicked(i)
                onFilterChanged: (text) => root.filterText = text
                onUpRequested: root.upRequested()
                onRefreshRequested: root.refreshRequested()
                onViewModeSelected: (grid) => root.gridMode = grid
            }

            // ══ 内容 ══
            Loader {
                Layout.fillWidth: true
                Layout.fillHeight: true
                sourceComponent: root.gridMode ? gridComp : listComp
            }
        }
    }

    // ── 设置对话框（三屏同一实现；当前阶段单纯 UI）──
    SettingsDialog {
        id: settingsDialog
    }
    Component.onCompleted: if (root.debugOpenSettings) settingsDialog.open()

    PlaceholderBrowse { id: items }

    // ── 网格视图 ──
    Component {
        id: gridComp
        GridView {
            id: grid
            clip: true
            model: items
            // ★ GridView **自己会把内容居中**（实测：leftMargin=0 时首卡仍在 x=160）。
            //   所以不能自己再算 sidePad（两套居中打架）。
            // ★ 也不能给 cellWidth 加"列间距补偿"——cols 按 168 算、cellWidth 变 178 后，
            //   GridView 用 178 反推只放得下 5 列（循环依赖）。
            //   正解：格子**首尾相接**铺满可用宽度，列间距由卡片自身的 margin 提供。
            readonly property int cols: Math.max(1,
                Math.floor((width - Theme.spaceLg * 2) / Theme.thumbCellW))
            cellWidth: (width - Theme.spaceLg * 2) / cols
            cellHeight: Theme.thumbCellH
            leftMargin: Theme.spaceLg
            rightMargin: Theme.spaceLg
            topMargin: Theme.spaceMd
            bottomMargin: Theme.spaceLg
            cacheBuffer: 600

            delegate: ThumbCard {
                width: grid.cellWidth          // 铺满格子；间距由卡片自身 margin 提供
                height: grid.cellHeight
                name: model.name
                thumb: model.thumb
                kind: model.kind
                missing: model.missing
                selected: index === root.currentIndex
                onActivated: root.itemActivated(index)
                onOpenRequested: root.itemOpened(index)
            }
        }
    }

    // ── 列表视图（表格：表头 + 行）──
    Component {
        id: listComp
        ColumnLayout {
            spacing: 0

            // 表头（组件；列几何与行共用 Theme 的 col* token）
            FileListHeader {
                Layout.fillWidth: true
            }

            ListView {
                id: list
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: items
                leftMargin: Theme.colListPad
                rightMargin: Theme.colListPad
                topMargin: Theme.spaceMd
                bottomMargin: Theme.spaceLg
                spacing: Theme.hairline

                delegate: FileRow {
                    width: list.width - Theme.spaceMd * 2
                    name: model.name
                    sizeText: model.sizeText
                    resText: model.resText
                    kind: model.kind
                    thumb: model.thumb
                    missing: model.missing
                    selected: index === root.currentIndex
                    onActivated: root.itemActivated(index)
                    onOpenRequested: root.itemOpened(index)
                }
            }
        }
    }
}
