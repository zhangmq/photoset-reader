import QtQuick
import ImageBrowser

// ══ 下拉按钮 = 标准按钮 + 下拉箭头 + **自己持有菜单** ════════════
//   ★ 菜单的开合状态属于按钮自己：父级只给 `menuItems`、只听 `itemTriggered`，
//     不再需要回填 `menuOpen`（那是把按钮外观交给外部对象决定）。
//   菜单本体是标准组件 PopupMenu（外观不在这里重复定义）。
Button {
    id: root

    property var menuItems: []
    signal itemTriggered(int index)

    hasIndicator: true
    expanded: menu.visible
    onClicked: menu.visible ? menu.close() : menu.open()

    PopupMenu {
        id: menu
        anchorTarget: root
        items: root.menuItems
        onItemTriggered: function (index) { root.itemTriggered(index) }
    }
}
