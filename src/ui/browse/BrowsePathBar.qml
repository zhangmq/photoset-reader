import QtQuick
import ImageBrowser

// ══ 浏览界面 · 路径条（bar 2）═════════════════════════════════
//   左：面包屑（首段 = **锁死的根**，整条路径不拆 segment）
//   右：过滤框 → 上一级 → 视图切换（用户明确：这两个从 bar 1 移到 filter 之后）
//   与图集库的「列表标题行」同一套规则：高 `barH`、满宽 + `spaceLg` padding、
//   **自持 1px 下边框**
Item {
    id: root

    property var    crumbs: []
    property string filterText: ""
    property bool   canGoUp: false
    property bool   gridMode: true

    signal crumbClicked(int index)
    signal filterChanged(string text)
    signal upRequested()
    signal viewModeSelected(bool grid)

    implicitHeight: Theme.barH

    // ── 右：过滤 + 导航 + 视图切换 ──
    Row {
        id: actions
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceLg
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.spaceXs

        // 当前路径下的过滤（**只用于定位，不改序列**：序列仍是完整的那一份）
        SearchField {
            id: filterField
            anchors.verticalCenter: parent.verticalCenter
            placeholder: "过滤…"
            text: root.filterText
            // 加不等判断：避免 text 绑定与回写构成循环
            onTextChanged: if (root.filterText !== text) root.filterChanged(text)
        }
        IconAction {
            anchors.verticalCenter: parent.verticalCenter
            glyph: Theme.icUp
            tooltip: "上一级"
            enabled: root.canGoUp
            // 禁用观感由组件自身负责（IconAction：`Theme.dimOpacity`，单一来源）
            onTriggered: root.upRequested()
        }
        // 视图切换：标准分段控件（DSH SegmentedControl）；当前项有抬起指示块
        SegmentedControl {
            anchors.verticalCenter: parent.verticalCenter
            currentIndex: root.gridMode ? 0 : 1
            items: [
                { icon: Theme.icGrid },
                { icon: Theme.icList }
            ]
            onActivated: function (index) { root.viewModeSelected(index === 0) }
        }
    }

    // ── 左：面包屑（右边界让开动作组）──
    Breadcrumb {
        anchors.left: parent.left
        anchors.leftMargin: Theme.spaceLg
        anchors.right: actions.left
        anchors.rightMargin: Theme.spaceMd
        anchors.verticalCenter: parent.verticalCenter
        items: root.crumbs
        onActivated: function (index) { root.crumbClicked(index) }
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
