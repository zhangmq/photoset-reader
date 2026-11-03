import QtQuick
import AppUi

// ══ 时间轴悬停预览条（用户 2026-11-03）══════════════════════════════════════
//   · 悬停在进度条上时，在**进度条正上方**显示邻近几页的缩略图，**紧贴**（无间距）；
//   · 有容器背景（`bgLayer2`），高度固定 `timelinePreviewH`；
//   · 图片之间、以及上下留白用**同一个间距**（`Theme.spaceSm`）；
//   · 按原图宽高比等比铺开、尽量填满窗口宽度；**鼠标指到的那一页居中**；
//   · 立即出现：条目与外框由元数据算好（不解码）；缩略图异步解码、逐张填进来。
Rectangle {
    id: root

    property var  items: []          // [{ index, w, h, url }]
    property int  hoveredIndex: -1   // 鼠标指到的那一页 ⇒ **摆到窗口中线**
    property int  stripH: Theme.timelinePreviewH

    //   间距：图与图之间、以及上下留白**同一个值**（用户 2026-11-03）
    readonly property int gap: Theme.spaceSm
    readonly property int imgH: Math.max(1, stripH - gap * 2)

    height: stripH
    width: parent ? parent.width : 0
    visible: items.length > 0
    color: Theme.barFill          // 与顶/底栏同一个面（用户 2026-11-03）
    radius: 0          // 不要圆角（用户 2026-11-03）
    clip: true

    Row {
        id: row
        anchors.verticalCenter: parent.verticalCenter
        height: root.imgH
        spacing: root.gap
        //   ★ **整条不做滑动动画**（用户 2026-11-03）：当前页始终被摆在窗口中线，
        //     条本身一跳到位即可；滑动会让"框不动、图在动"，看起来像框在乱飘。
        //     居中情形的过渡只有"框宽变化"；靠边情形才是"框位移 + 宽度变化"。

        Repeater {
            model: root.items
            delegate: Item {
                width: modelData.w
                height: root.imgH

                Image {
                    anchors.fill: parent
                    source: modelData.url
                    //   ★ 立刻占位、异步出图：条先出来，缩略图逐张填（用户 2026-11-03）
                    asynchronous: true
                    cache: true
                    fillMode: Image.PreserveAspectCrop
                    //   还没解出来时保持透明（不画空白块）
                    opacity: status === Image.Ready ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
                }
            }
        }
    }

    //   ── 定位框：几何由**上层用数据算好**传进来（`focusX` / `focusW`）──
    //   position = f(index, 该条 items 的宽度)   —— 整条该在哪 + 该条之前的累计宽
    //   width    = f(该条自己的宽高比)           —— 不看预览条里有没有它
    //   本组件**不再自己算**，只负责画与过渡动画（用户 2026-11-03）
    property real focusX: 0
    property real focusW: 0
    //   居中情形 ⇒ **位移不做过场动画**（宽度变会让 x 变几像素，别让它看起来在动）
    property bool centered: true

    Rectangle {
        visible: root.focusW > 0
        x: root.focusX
        y: (root.height - root.imgH) / 2
        width: root.focusW
        height: root.imgH
        color: "transparent"
        border.width: 2
        border.color: Theme.brand
        //   居中情形：只有宽度在变（x 跟着涨一半）；靠边情形：位移 + 宽度一起过渡
        Behavior on x {
            enabled: !root.centered        // 居中时框"不动"，只有靠边情形才位移过渡
            NumberAnimation { duration: Theme.durMed; easing.type: Easing.OutCubic }
        }
        Behavior on width  { NumberAnimation { duration: Theme.durMed; easing.type: Easing.OutCubic } }
        Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
        opacity: visible ? 1 : 0
    }

    //   整条位置：**由上层用数据算好**（`stripX`），组件不再自己按 `row.width` 推 ——
    //   `row.width` 是渲染期量，列表换代时会抖动（用户 2026-11-03 的"乱跳"就是这么来的）
    property real stripX: 0

    Binding { target: row; property: "x"; value: root.stripX }
}
