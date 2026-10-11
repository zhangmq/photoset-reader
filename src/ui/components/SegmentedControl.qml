import QtQuick
import ImageBrowser

// ══ 标准分段控件（segmented control）═══════════════════════════
//   样式来源：DSH `dsh-client-ui-primitives/lib/SegmentedControl.module.css`
//     · 轨道：`--dsw-alias-interactive-bg-hover` 半透明填充、padding 4、radius-md、gap 2
//     · 指示块：`--dsw-alias-bg-layer-1` + radius-sm，随选中项滑动（160ms）
//     · 分段：高 28、透明底、无边框；**等宽**（按最宽的内容定）
//   ⚠️ DSH 的指示块靠 `elevation-soft` 投影浮起来（本项目不做投影，见 §6.3）
//      → 改用 `border-l3` 描边，与 DSH `SegmentedTabs` 的指示块做法一致
//   键盘：每段都是 Tab 站点（Enter/Space 激活），焦点环按 §2.5
Item {
    id: root

    property var items: []              // [{ icon, text, tooltip }]
    property int currentIndex: 0
    signal activated(int index)

    readonly property int pad: Theme.spaceXs        // DSH: padding 4px
    readonly property int gap: Theme.space2xs       // DSH: gap 2px

    // 分段等宽 = 最宽内容（DSH：grid tracks sized to the widest label）
    readonly property real segW: {
        var w = Theme.controlMd
        for (var i = 0; i < segs.count; ++i) {
            var it = segs.itemAt(i)
            if (it)
                w = Math.max(w, it.implicitWidth)
        }
        return w
    }
    implicitWidth: pad * 2 + segW * items.length + gap * Math.max(0, items.length - 1)
    implicitHeight: pad * 2 + Theme.controlSm

    Rectangle {                                     // 轨道
        anchors.fill: parent
        radius: Theme.radiusMd
        color: Theme.hoverFill
    }

    Rectangle {                                     // 指示块
        x: root.pad + root.currentIndex * (root.segW + root.gap)
        y: root.pad
        width: root.segW
        height: Theme.controlSm
        radius: Theme.radiusSm
        color: Theme.bgLayer1
        border.width: Theme.borderW
        border.color: Theme.borderL3
        visible: root.currentIndex >= 0 && root.currentIndex < root.items.length
        Behavior on x { NumberAnimation { duration: Theme.durFast } }
    }

    Row {
        anchors.fill: parent
        anchors.margins: root.pad
        spacing: root.gap

        Repeater {
            id: segs
            model: root.items

            delegate: Item {
                id: seg
                required property var modelData
                required property int index

                readonly property bool active: index === root.currentIndex

                width: root.segW
                height: Theme.controlSm
                implicitWidth: Math.max(Theme.controlMd,
                                        segContent.width + Theme.spaceMd * 2)

                activeFocusOnTab: true

                function activate() {
                    root.currentIndex = seg.index
                    root.activated(seg.index)
                }

                Row {
                    id: segContent
                    anchors.centerIn: parent
                    spacing: Theme.spaceXs
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: seg.modelData.icon !== undefined && seg.modelData.icon !== ""
                        text: seg.modelData.icon !== undefined ? seg.modelData.icon : ""
                        font.family: iconFont
                        font.pixelSize: Theme.iconMd
                        renderType: Text.NativeRendering
                        color: seg.active ? Theme.textPrimary : Theme.textSecondary
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: seg.modelData.text !== undefined && seg.modelData.text !== ""
                        text: seg.modelData.text !== undefined ? seg.modelData.text : ""
                        color: seg.active ? Theme.textPrimary : Theme.textSecondary
                    }
                }

                Rectangle {                          // 焦点环（§2.5）
                    anchors.fill: parent
                    radius: Theme.radiusSm
                    color: "transparent"
                    border.width: Theme.focusRingW
                    border.color: Theme.brand
                    visible: seg.activeFocus && ui.inputFromKeyboard
                }

                MouseArea {
                    id: segMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onPressed: seg.forceActiveFocus()
                    onClicked: seg.activate()
                }

                Keys.onReturnPressed: seg.activate()
                Keys.onEnterPressed: seg.activate()
                Keys.onSpacePressed: seg.activate()
            }
        }
    }
}
