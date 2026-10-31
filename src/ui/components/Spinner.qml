import QtQuick
import AppUi

// ══ 转圈（唯一实现）══════════════════════════════════════════════════════
//   形状（用户 2026-10-31 校正）：**一小段圆弧沿着轨道转圈** —— 不是"一根横线在轨道上方
//   绕自己转"。绘制 = `Ring.qml`（唯一绘制实现），这里只加**旋转**。
//   · 旋转用 `RotationAnimator`（**不是 Timer**），绕本组件中心 ⇒ 弧沿轨道走；
//   · 两个用处：Loading 浮层（size 32 / 线宽 3）、顶栏扫描中（size 16 / 线宽 2）。
Ring {
    id: root

    property bool running: true
    arcDegrees: 90

    RotationAnimator on rotation {
        from: 0
        to: 360
        duration: 900
        loops: Animation.Infinite
        running: root.running
    }
}
