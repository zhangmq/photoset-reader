import QtQuick
import QtQuick.Controls
import AppUi

// ══ 标准提示气泡（tooltip）══════════════════════════════════════
//   样式来源：DSH `dsh-client-ui-primitives/lib/Tooltip.module.css` `.bubble`
//     · padding 3px 7px；radius: --dsw-radius-sm
//     · 底: --dsw-alias-tooltip-bg；文字: neutral-bluish-00（白）
//     · 字号比正文小一号；无描边
//     · 延迟 500ms（DSH 的 Tooltip 统一 delayMs: 500）
//
//   用法（锚在目标控件上，把目标的 hover 状态接进来）：
//     HintBubble { anchors.fill: parent; text: "…"; hovered: ma.containsMouse }
//
//   ⚠️ 不用 QtQuick.Controls 的 attached ToolTip：它的 attached 对象不暴露
//      contentItem / background，没法做统一外观；这里自绘一个 Popup。
//
//   ★★ `popupType: Popup.Item` 是**硬要求**（2026-11-06，实测事故）：
//     默认的 `Popup.Window` 在 Wayland 上是**独立的 xdg_popup 表面**，带 implicit grab ——
//     只要气泡盖住指针，主窗口就收到 `pointer.leave` ⇒ 底栏的 `HoverHandler` 丢悬停
//     ⇒ 底栏隐藏 + 气泡关闭 ⇒ 指针回到窗口 ⇒ 500ms 后又弹 …… **悬停抖动死循环**。
//     改成 `Popup.Item` 后它只是**同一个窗口里的一个 item**：没有独立表面、没有 grab，
//     结构上就不可能抢指针。（不用 `WindowTransparentForInput`：Wayland 上对 popup 无效。）
//
//   ★ 位置计算**只用 `contentItem` 一个坐标空间**（2026-11-06 另一个坑）：
//     原来混用了 `win.width/height`（QWindow 空间）和 `mapToItem(win.contentItem, …)`
//     （item 空间）。当窗口带缩放时（本机 dpr=1.35 / zoom=0.675）两者不是同一数量级，
//     比较结果失真 ⇒ 气泡被算到按钮下方/夹回可见区 ⇒ **正好压在按钮和指针上**。
//     现在全程 contentItem 空间，并且**只缩不放、绝不与目标矩形相交**。
Item {
    id: root

    property string text: ""
    property bool   hovered: false
    property int    delayMs: 500

    //   给自动化测试用的**只读观测口**（`ib-test-ui` 断言几何用）：
    //   返回气泡在**本项坐标系**里的矩形；`bubbleVisible` 是它当前是否真的显示。
    readonly property rect bubbleRect:
        Qt.rect(bubble.x, bubble.y, bubble.width, bubble.height)
    readonly property bool bubbleVisible: bubble.visible
    //   `PopupType`：Item=0 / Window=1 / Native=2。**必须是 0**（Item），否则会抢指针。
    readonly property int  bubbleType: bubble.popupType

    implicitWidth: 1
    implicitHeight: 1

    onHoveredChanged: {
        if (hovered && text !== "")
            showTimer.restart()
        else {
            showTimer.stop()
            bubble.close()
        }
    }
    onTextChanged: if (hovered && text === "") { showTimer.stop(); bubble.close() }

    Timer {
        id: showTimer
        interval: root.delayMs
        onTriggered: if (root.text !== "") bubble.open()
    }

    Popup {
        id: bubble
        parent: root
        //   ★ 见文件头：不能是独立表面（会抢指针）
        popupType: Popup.Item
        padding: Theme.spaceXs
        leftPadding: Theme.spaceSm
        rightPadding: Theme.spaceSm
        modal: false
        focus: false
        closePolicy: Popup.NoAutoClose

        background: Rectangle {
            color: Theme.tooltipBg
            radius: Theme.radiusSm
        }
        contentItem: Text {
            text: root.text
            color: Theme.tooltipText
            font.pointSize: Theme.fontSm
        }

        // ── 边缘感知定位（用户 2026-10-11：**横纵向都不许被窗口裁剪**）──
        //   纵向：优先**上方** → 放不下翻下方 → 都放不下则贴边夹住（但仍不许压住目标）
        //   横向：与目标居中对齐 → 超出窗口左右则内收
        //   ★ 全程 `contentItem` 空间；`win.width/height` 一律不用（见文件头）。
        readonly property real edgeGap: Theme.spaceXs
        function place() {
            const win = root.Window.window
            if (!win || !win.contentItem) return
            const host = win.contentItem
            const target = root.mapToItem(host, 0, 0)
            const w = bubble.implicitWidth > 0 ? bubble.implicitWidth : bubble.width
            const h = bubble.implicitHeight > 0 ? bubble.implicitHeight : bubble.height
            if (!(w > 0) || !(h > 0)) return

            //   可用边界 = 宿主 item 的自身范围（**与 target 同一个空间**）
            const maxX = Math.max(bubble.edgeGap, host.width - w - bubble.edgeGap)
            const below = target.y + root.height + bubble.edgeGap
            const above = target.y - h - bubble.edgeGap
            let cy
            if (above >= bubble.edgeGap)
                cy = above                                   // 上方（首选）
            else if (below + h <= host.height - bubble.edgeGap)
                cy = below                                   // 上方放不下 ⇒ 下方
            else
                cy = Math.max(bubble.edgeGap, Math.min(below, host.height - h - bubble.edgeGap))
            let cx = target.x + (root.width - w) / 2
            cx = Math.max(bubble.edgeGap, Math.min(cx, maxX))

            //   Popup 的 x/y 是**相对 parent（root）**的
            bubble.x = cx - target.x
            bubble.y = cy - target.y
        }
        onOpened: place()
        onVisibleChanged: if (bubble.visible) place()
        onImplicitWidthChanged: if (bubble.visible) place()
        onImplicitHeightChanged: if (bubble.visible) place()
    }
}
