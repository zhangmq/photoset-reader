import QtQuick
import QtQuick.Controls
import AppUi

// ══ 标准浮层（panel）══════════════════════════════════════════
//   定位逻辑来源：vsr-player/src/client/ui/components/PopupBase.qml
//     · anchorTarget：水平居中于目标、贴在目标一侧；越界自动钳制、翻转
//     · 非 modal / Esc + 点击外部关闭
//   外观来源：DSH 自己的菜单/浮层样式
//     · 表面   = --dsw-specific-menu == --dsw-menu-surface-fill（半透明，深/浅不同）
//     · 描边   = elevation stroke 用色 --dsw-alias-border-l4
//     · 圆角   = --dsw-radius-lg（紧凑变体用 --dsw-radius-md）
//     · 投影   = --dsw-elevation-prominent（0 3px 8px #0000000a + 0 0 20px #0000000d）
//   ⚠️ DSH 的表面带 backdrop blur(40px) saturate(150%)；QML 无背景模糊 API，
//      这里只用半透明填充近似，柔光投影也暂未实现（见 PopupPanel 注释末尾）
//
//   ★ PopupPanel = 面板（可放任意内容，如设置分区）
//     PopupMenu  = 菜单（行式列表），两者分开，菜单是面板的一种特例
Popup {
    id: root

    property Item anchorTarget: null
    property real targetGap:  Theme.spaceSm     // 与目标的间距
    property real screenMargin: Theme.spaceSm   // 距窗口边缘的最小间距
    property bool preferBelow: true             // 动作条在顶部 → 默认向下弹
    //   右键菜单用：展开二级面板时会临时加宽 ⇒ 钳制要**按加宽后的宽度**预留，
    //   否则贴着右边缘弹出时二级面板会被顶到窗口外（vsr-player 同款处理）。
    property real clampExtraWidth: 0

    padding: Theme.spaceMd
    //   ★ 2026-11-07：**不新建表面**（与 ModalDialog 同一口径）。理由：键盘焦点留在
    //     主窗口（B 系统的焦点导航才稳），也避开 Wayland 独立表面 + 隐式 grab 那一类坑。
    //     注意：非模态浮层的语义与模态对话框不同，见 ModalDialog.qml 文件头。
    popupType: Popup.Item
    modal: false
    // ★ focus: true —— 菜单要能用键盘操作（Esc 关闭、方向键选、Enter 激活）。
    //   Qt 在弹窗关闭时会把焦点**还给打开前持有焦点的项**（实测）；
    //   前提是那个项本身可获焦 —— 所以触发它的按钮必须可获焦（Button/IconAction 已加
    //   activeFocusOnTab + 点击即获焦）。否则焦点会掉到窗口内容项，再被解析到
    //   窗口里第一个可获焦元素（曾表现为"点了菜单项，过滤框自动获得焦点"）。
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Item {
        Rectangle {
            id: backdrop
            anchors.fill: parent
            color: Theme.menuFill
            radius: Theme.radiusLg
            border.width: Theme.borderW
            border.color: Theme.borderL4
        }
        //   ★ 投影（用户 2026-11-06）：只画影子、不重绘面板 ⇒ 半透明的 `menuFill` 不会叠黑
        SurfaceShadow {
            anchors.fill: backdrop
            radius: backdrop.radius
            //   菜单比对话框小、贴着控件弹出 ⇒ 影子收紧一点，不然像"浮太高"
            blur: 20
            offset: Qt.point(0, 6)
            z: -1
        }
    }

    onOpened: positionToTarget()
    onAnchorTargetChanged: if (opened) positionToTarget()

    // ★ 右键菜单用：在 `item` 坐标 (x, y)（= 鼠标位置）弹出，**四周钳制在窗口内**。
    //   `anchorTarget` 语义不适用（没有目标控件），所以单独一套：窗口坐标算完再换回父项坐标。
    function showAt(item, x, y) {
        if (!item)
            return
        const win = item.Window.window
        const base = win ? win.contentItem : parent
        if (!base)
            return
        const p = item.mapToItem(base, x, y)
        const fullW = width + clampExtraWidth
        const nx = Math.min(Math.max(p.x, screenMargin),
                            Math.max(screenMargin, base.width - fullW - screenMargin))
        const ny = Math.min(Math.max(p.y, screenMargin),
                            Math.max(screenMargin, base.height - height - screenMargin))
        const local = base.mapToItem(parent, nx, ny)
        root.x = local.x
        root.y = local.y
        open()
    }

    function positionToTarget() {
        if (!anchorTarget)
            return
        // ★ 一律以**整窗内容项**为坐标基准：浮层可以声明在任何组件内部
        //   （例如 DropdownButton 自己持有菜单），钳制与翻转仍按整窗计算。
        var win = anchorTarget.Window.window
        var base = win ? win.contentItem : parent
        if (!base)
            return
        var p = anchorTarget.mapToItem(base, 0, 0)
        var nx = p.x + anchorTarget.width / 2 - width / 2
        nx = Math.min(Math.max(nx, screenMargin), base.width - width - screenMargin)
        var ny = preferBelow ? p.y + anchorTarget.height + targetGap
                             : p.y - height - targetGap
        if (preferBelow && ny + height > base.height - screenMargin)
            ny = p.y - height - targetGap
        else if (!preferBelow && ny < screenMargin)
            ny = p.y + anchorTarget.height + targetGap
        // 窗口坐标 → 本浮层父项坐标
        var local = base.mapToItem(parent, nx, ny)
        x = local.x
        y = local.y
    }
}
