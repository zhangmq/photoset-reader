import QtQuick
import ImageBrowser

// ══ 图标下拉按钮 = 标准图标按钮 + **自己持有菜单** ══════════════
//   与 DropdownButton 同一模式（文字版）：菜单的开合状态属于按钮自己，
//   父级只给 `menuItems`、只听 `itemTriggered`，不回填 menuOpen。
//   键盘：Tab 可选中，Enter/Space 打开或关闭，菜单内 ↑/↓ + Enter（由 PopupMenu 提供）
IconAction {
    id: root

    property var menuItems: []
    signal itemTriggered(int index)

    expanded: menu.visible
    onTriggered: menu.visible ? menu.close() : menu.open()

    function openMenu() { menu.open() }      // 供调试/程序化展开

    PopupMenu {
        id: menu
        anchorTarget: root
        items: root.menuItems
        onItemTriggered: function (index) { root.itemTriggered(index) }
    }
}
