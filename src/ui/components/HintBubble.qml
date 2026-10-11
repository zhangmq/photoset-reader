import QtQuick
import QtQuick.Controls
import ImageBrowser

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
Item {
    id: root

    property string text: ""
    property bool   hovered: false
    property int    delayMs: 500

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
        //   纵向：优先下方 → 放不下翻上方 → 都放不下则贴边夹住
        //   横向：以目标为中心 → 超出窗口左右则内收
        //   位置在**窗口坐标**里算，再换算回父项坐标（父项可能很靠边）
        readonly property real edgeGap: Theme.spaceXs
        function place() {
            var win = root.Window.window
            if (!win)
                return
            var origin = root.mapToItem(win.contentItem, 0, 0)
            var w = bubble.implicitWidth > 0 ? bubble.implicitWidth : bubble.width
            var h = bubble.implicitHeight > 0 ? bubble.implicitHeight : bubble.height
            var maxX = Math.max(bubble.edgeGap, win.width - w - bubble.edgeGap)
            var maxY = Math.max(bubble.edgeGap, win.height - h - bubble.edgeGap)
            var below = origin.y + root.height + bubble.edgeGap
            var above = origin.y - h - bubble.edgeGap
            var cy = (below + h <= win.height) ? below
                   : (above >= 0) ? above
                                  : Math.max(bubble.edgeGap, Math.min(below, maxY))
            var cx = origin.x + (root.width - w) / 2
            cx = Math.max(bubble.edgeGap, Math.min(cx, maxX))
            bubble.x = cx - origin.x
            bubble.y = cy - origin.y
        }
        onOpened: place()
        onImplicitWidthChanged: if (bubble.visible) place()
        onImplicitHeightChanged: if (bubble.visible) place()
    }
}
