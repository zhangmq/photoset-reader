import QtQuick
import AppUi

// ══ 圆环 / 弧（唯一绘制实现）══════════════════════════════════════════════
//   `Canvas` 画两笔：**整圆轨道** + **一段圆弧**（`arcDegrees`，0–360，从 12 点起）。
//   两个用处（**同一份绘制**）：
//     · `Spinner`（转圈）：外层加 `RotationAnimator`，弧沿轨道转；
//     · 收藏夹列表的「阅读百分比」甜甜圈：**不加动画**，`arcDegrees = 360 × 比例`
//       （用户 2026-10-31："复制一个这个 spinner，只需要去掉动画，让圆弧反映百分比即可"）。
//   ⚠️ Canvas 不是声明式绘制：颜色 / 尺寸变了必须 `requestPaint()`。
Item {
    id: root

    property int   size: 32
    property int   lineWidth: 3
    property real  arcDegrees: 90          // 弧段张角（0 = 只有轨道）
    property color trackColor: Theme.borderL4
    property color arcColor: Theme.textPrimary

    implicitWidth: size
    implicitHeight: size

    Canvas {
        id: canvas
        anchors.fill: parent

        property color track: root.trackColor
        property color arc: root.arcColor
        property real  degrees: root.arcDegrees
        property int   lw: root.lineWidth

        onTrackChanged: requestPaint()
        onArcChanged: requestPaint()
        onDegreesChanged: requestPaint()
        onLwChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()

        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const r = (width - lw) / 2
            const cx = width / 2
            const cy = height / 2
            if (r <= 0)
                return
            ctx.lineWidth = lw
            ctx.lineCap = "round"

            ctx.strokeStyle = track                    // 轨道（整圆）
            ctx.beginPath()
            ctx.arc(cx, cy, r, 0, Math.PI * 2)
            ctx.stroke()

            if (degrees > 0) {                         // 弧段（从 12 点起，顺时针）
                ctx.strokeStyle = arc
                const start = -Math.PI / 2
                ctx.beginPath()
                ctx.arc(cx, cy, r, start, start + Math.min(360, degrees) * Math.PI / 180)
                ctx.stroke()
            }
        }
    }
}
