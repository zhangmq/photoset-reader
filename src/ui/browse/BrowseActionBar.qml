import QtQuick
import ImageBrowser

// ══ 浏览界面 · 动作条（bar 1）═════════════════════════════════
//   左 [← 图集库] [图集名称（范围指示）]      右 [★ 收藏] [主题] [⚙ 设置]
//   （用户明确：返回图集库在最左，标题在其右侧；上一级/视图切换不在这一条）
//   与图集库动作条同一套规则：高 `barH`、满宽 + `spaceLg` padding、**自持 1px 下边框**
Item {
    id: root

    property bool   favorited: false
    property string rangeLabel: ""       // 范围指示：图集《…》 / 未收藏 · 本次不记进度

    signal libraryRequested()
    signal favoriteToggled()
    signal settingsRequested()
    signal continueRequested()      // 从原进度继续阅读（不改进度）

    implicitHeight: Theme.barH

    // ── 左：返回图集库 + 图集名称 ──
    Row {
        anchors.left: parent.left
        anchors.leftMargin: Theme.spaceLg
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.spaceMd

        IconAction {   // 返回（回图集库）
            anchors.verticalCenter: parent.verticalCenter
            glyph: Theme.icBack
            tooltip: "返回"
            onTriggered: root.libraryRequested()
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

    // ── 右：收藏 + 设置 ──
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
        // 主题（三态循环：跟随系统 → 浅色 → 深色）—— 与图集库动作条**同一实现**
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
        // 设置（与图集库动作条**功能完全相同**）
        IconAction {
            anchors.verticalCenter: parent.verticalCenter
            glyph: Theme.icSettings
            tooltip: "设置"
            onTriggered: root.settingsRequested()
        }
    }

    // ── 下边框（bar 自持，与图集库同规则）──
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Theme.hairline
        color: Theme.borderL1
    }
}
