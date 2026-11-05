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
    //   ★ 外部只改 `opened`（用户 2026-11-06）：显隐 + 滑入滑出都在本组件里演
    property bool opened: false
    visible: opened || slide < 1
    color: Theme.barFill          // 与顶/底栏同一个面（用户 2026-11-03）
    radius: 0          // 不要圆角（用户 2026-11-03）
    clip: true

    //   ★ 滑入 / 滑出（用户 2026-11-06）：整条从**底栏后面**滑上来、再滑回去。
    //     收起位 = 下移一个条高：正好落到"进度条 + 控制条"那一块，被不透明底栏挡住
    //     （`bar` 在 FullscreenBottomBar 里声明在本组件之后 ⇒ 画在上层）。
    //   ★ 只做位移、不叠淡出：位移本身已经说明方向，再淡出会糊成一团。
    property real slide: opened ? 0 : 1
    Behavior on slide {
        NumberAnimation { duration: Theme.durMed; easing.type: Easing.OutCubic }
    }
    transform: Translate { y: root.slide * root.height }

    //   开始收起时清掉 hover：滑动途中条会扫过底栏，不能再把自己"顶"回来（会抖）
    onOpenedChanged: if (!opened) hovered = false

    //   ★ 悬停目标**由指针 x 反查数据**（用户 2026-11-04：不再用"每张图一个 HoverHandler"）：
    //     落在两张图之间的 spacing 上时取**最近的一张** —— 天然不会出现"没有目标"的空档，
    //     也就不会在间距处把 hoverIndex 清掉、让框跳回 index 再跳回来。
    //   用 MouseArea（API 最稳）：`acceptedButtons: NoButton` ⇒ 不吃点击，缩略图的 TapHandler 照常
    MouseArea {
        anchors.fill: parent
        //   ★ 收起途中不再吃 hover（否则条滑过底栏时会把自己"顶"回来，来回抖）
        enabled: root.opened
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
        onEntered: { root.hovered = true; root.updateHoverIndex(mouseX) }   // onEntered 没有 mouse 形参
        onExited:  { root.hovered = false; root.hoverIndex = -1 }
        onPositionChanged: function (mouse) { if (containsMouse) root.updateHoverIndex(mouse.x) }
    }

    //   指针 x（条内坐标）→ 条目：先找命中的，落在间距里就取最近的一条
    function updateHoverIndex(px) {
        if (items.length === 0) return
        const lx = px - stripX
        var best = -1, bestDist = Number.MAX_VALUE
        for (var i = 0; i < items.length; ++i) {
            const it = items[i]
            if (lx >= it.x && lx <= it.x + it.w) { best = it.index; bestDist = 0; break }
            const d = lx < it.x ? it.x - lx : lx - (it.x + it.w)
            if (d < bestDist) { bestDist = d; best = it.index }
        }
        if (best >= 0) hoverIndex = best
    }

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

                TapHandler {
                    cursorShape: Qt.PointingHandCursor
                    onTapped: root.itemClicked(modelData.index)
                }

                Image {
                    anchors.fill: parent
                    source: modelData.url
                    //   ★ 立刻占位、异步出图：条先出来，缩略图逐张填（用户 2026-11-03）
                    asynchronous: true
                    cache: true
                    fillMode: Image.PreserveAspectCrop
                    //   还没解出来时保持透明（不画空白块）；悬停透明度由外层容器控制
                    opacity: status === Image.Ready ? 1 : 0
                }
            }
        }
    }

    //   ── 定位框：几何由**上层用数据算好**传进来（`focusX` / `focusW`）──
    //   position = f(index, 该条 items 的宽度)   —— 整条该在哪 + 该条之前的累计宽
    //   width    = f(该条自己的宽高比)           —— 不看预览条里有没有它
    //   本组件**不再自己算**，只负责画与过渡动画（用户 2026-11-03）
    //   （不再按"整批就绪"门控：条立即出现，图片由 provider 按需给，已缓存的立刻有）
    property real focusX: 0
    property real focusW: 0
    //   预览条里"指针正指着的那一张"（-1 = 没有）。**frame 跟着它走**，
    //   而条内容仍按 `index`（进度条上那一页）组织 —— 所以内容不会因悬停而重排（用户 2026-11-04）
    property int  hoverIndex: -1
    onVisibleChanged: if (!visible) hoverIndex = -1     // 收起时清掉，避免下次带着旧值
    //   整条（`stripH` = 120）都是 hover 区（用户 2026-11-03）：
    //   hover 覆盖整个预览区，指针落在条内任何位置都算"还在预览区里"
    property bool hovered: false
    //   指针离开**整条**才清 hoverIndex（中间经过 spacing 时保持上一次的目标）
    onHoveredChanged: if (!hovered) hoverIndex = -1
    signal itemClicked(int index)     // 点某张缩略图 ⇒ 跳转（用户 2026-11-03）

    //   hoverIndex 在条内 ⇒ 用它的 (条内偏移, 宽) 算；否则退回 index 的那一份（focusX/focusW）
    readonly property var hoverItem: {
        if (hoverIndex < 0) return null
        for (var i = 0; i < items.length; ++i)
            if (items[i].index === hoverIndex) return items[i]
        return null
    }
    readonly property real frameXNow: {
        if (!hoverItem) return focusX
        var x = stripX + hoverItem.x
        return x          // 不夹取：边缘图被裁时，框仍按图片真实位置画（容器 clip 负责裁）
    }
    readonly property real frameWNow: hoverItem ? hoverItem.w : focusW

    Rectangle {
        visible: root.frameWNow > 0
        x: root.frameXNow
        y: (root.height - root.imgH) / 2
        width: root.frameWNow
        height: root.imgH
        color: "transparent"
        border.width: 2
        border.color: Theme.brand
        //   居中情形：只有宽度在变（x 跟着涨一半）；靠边情形：位移 + 宽度一起过渡
        //   index 与 hoverIndex 走**同一个**过渡（用户 2026-11-04）：
        //   位置和宽度都用 `Theme.durMed` 动画 —— 悬停换张时框是"滑过去"，不是直接跳
        Behavior on x     { NumberAnimation { duration: Theme.durMed; easing.type: Easing.OutCubic } }
        Behavior on width  { NumberAnimation { duration: Theme.durMed; easing.type: Easing.OutCubic } }
        Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
        opacity: visible ? 1 : 0
    }

    //   整条位置：**由上层用数据算好**（`stripX`），组件不再自己按 `row.width` 推 ——
    //   `row.width` 是渲染期量，列表换代时会抖动（用户 2026-11-03 的"乱跳"就是这么来的）
    property real stripX: 0

    Binding { target: row; property: "x"; value: root.stripX }
}
