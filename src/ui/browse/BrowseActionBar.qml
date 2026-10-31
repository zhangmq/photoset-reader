import QtQuick
import AppUi

// ══ 浏览界面 · 动作条（bar 1）═════════════════════════════════
//   左 [folder-star 图集库] [打开 ▾] [范围指示] [继续阅读]   右 [★ 收藏] [主题] [⚙ 设置]
//   ★ 2026-10-25（两屏化，设计 18 §3）：
//     · 「返回图集库」**不再是返回** —— 图集库是抽屉 ⇒ 按钮换成 folder-star、tooltip「图集库」；
//     · 新增「打开 ▾」：文件夹 / 压缩包 / 图片 / ── / **收藏夹**（开抽屉）；
//       前三个 = 会话来源，**直达 Viewer**（等价 CLI `-d`）；
//     · 扫描状态（`扫描中 · 已发现 N` / 取消）在「打开 ▾」右侧 —— 段 3 接，本段先留位置。
//   与图集库同一套 bar 规则：高 `barH`、满宽 + `spaceLg` padding、**自持 1px 下边框**
Item {
    id: root

    property bool   favorited: false
    property string rangeLabel: ""       // 范围指示：图集《…》 / 未收藏 · 本次不记进度

    signal libraryRequested()
    signal folderRequested()
    signal archiveRequested()
    signal imageRequested()
    signal favoriteToggled()
    signal settingsRequested()
    signal continueRequested()      // 从原进度继续阅读（不改进度）

    implicitHeight: Theme.barH

    // ── 左：图集库（抽屉）+ 打开 + 范围 + 继续阅读 ──
    Row {
        anchors.left: parent.left
        anchors.leftMargin: Theme.spaceLg
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.spaceMd

        IconAction {   // 图集库（**开抽屉**，不是回屏）
            anchors.verticalCenter: parent.verticalCenter
            glyph: Theme.icFolderStar
            tooltip: "图集库"
            onTriggered: root.libraryRequested()
        }
        // 「打开 ▾」：菜单由按钮自己持有（DropdownButton），父级不回填开合状态
        DropdownButton {
            anchors.verticalCenter: parent.verticalCenter
            glyph: Theme.icFolder
            label: "打开"
            menuItems: [
                { icon: Theme.icFolder,     text: "文件夹" },
                { icon: Theme.icZip,        text: "压缩包" },
                { icon: Theme.icImage,      text: "图片" },
                { sep: true },
                { icon: Theme.icFolderStar, text: "收藏夹" }
            ]
            onItemTriggered: function (index) {
                if (index === 0)      root.folderRequested()
                else if (index === 1) root.archiveRequested()
                else if (index === 2) root.imageRequested()
                else if (index === 4) root.libraryRequested()
            }
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.rangeLabel
            color: Theme.textSecondary
        }
        // ★ 从原进度继续阅读：进查看屏、**不改阅读进度**（用户 2026-10-24）
        //   与"双击图 = 设进度并进入"分开：这是"我走开过，接着看"
        Button {
            anchors.verticalCenter: parent.verticalCenter
            label: "继续阅读"
            glyph: Theme.icPlay
            enabled: ui.viewer !== null
            onClicked: root.continueRequested()
        }
    }

    // ── 右：收藏 + 主题 + 设置 ──
    Row {
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceLg
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.spaceXs

        IconAction {
            anchors.verticalCenter: parent.verticalCenter
            glyph: root.favorited ? Theme.icStar : Theme.icStarO
            highlighted: root.favorited
            tooltip: root.favorited ? "取消收藏" : "收藏"
            onTriggered: root.favoriteToggled()
        }
        // 主题（三态循环：跟随系统 → 浅色 → 深色）
        IconAction {
            anchors.verticalCenter: parent.verticalCenter
            glyph: ui.themeMode === 2 ? Theme.icThemeDark
                 : ui.themeMode === 1 ? Theme.icThemeLight
                 : Theme.icThemeSystem
            tooltip: ui.themeMode === 2 ? "深色"
                   : ui.themeMode === 1 ? "浅色"
                   : "跟随系统"
            onTriggered: ui.cycleThemeMode()
        }
        // 设置（与阅读屏**功能完全相同**）
        IconAction {
            anchors.verticalCenter: parent.verticalCenter
            glyph: Theme.icSettings
            tooltip: "设置"
            onTriggered: root.settingsRequested()
        }
    }

    // ── 下边框（bar 自持）──
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Theme.hairline
        color: Theme.borderL1
    }
}
