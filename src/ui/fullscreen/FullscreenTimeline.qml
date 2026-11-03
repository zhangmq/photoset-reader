import QtQuick
import AppUi

// ══ 全屏 · 时间轴（横跨整个条目序列）══════════════════════════
//   设计 §3.5：可点击 / 拖动跳转
//   用户 2026-10-11 修正三点：
//     · **已完成进度用主色**（`brand`）—— 原来只有一条灰轨道
//     · **游标跟随鼠标当前位置**（hover 时），不是停在点击位置
//     · **游标高不超过进度条高度**（原来整行 14 高，太高）
//   几何：**行高 = 轨道高 = `osdTrackH`(8)** —— hover 区就是轨道本身，不再在轨道上方留
//   一段"看不见的 hover 带"（那会让 hover 区与预览条高度对不上，用户 2026-11-03 指出）。
Item {
    id: root

    property int  total: 1
    property int  index: 0                // 0-based 条目序号
    property bool overlaysVisible: true
    signal seeked(int index)

    readonly property alias pressed: ma.pressed
    readonly property bool  hovered: ma.containsMouse
    readonly property real  progress: total > 0
                                      ? Math.max(0, Math.min(1, (index + 1) / total))
                                      : 0
    readonly property real  hoverFrac: width > 0
                                       ? Math.max(0, Math.min(1, ma.mouseX / width))
                                       : 0
    //   悬停预览用：鼠标指到的条目（预览条把这一页摆到窗口中线）。
    //   ★ **锁存**：只在悬停期间更新；指针离开后保持上一次的值 —— 否则会退回"当前页"，
    //     让预览条里的定位框瞬间失去目标、提前消失，打断过渡动画（用户 2026-11-03）。
    property int hoverIndex: index
    function indexAt(x) {
        if (root.total <= 1)
            return 0
        return Math.round(Math.max(0, Math.min(1, x / width)) * (root.total - 1))
    }
    implicitHeight: Theme.osdTrackH

    Rectangle {   // 轨道（整条）—— 高度固定，**hover 不再放大**（用户 2026-11-03）
        id: track
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Theme.osdTrackLineH
        color: Theme.overlayTrack
    }
    Rectangle {   // 已完成进度（主色）
        anchors.left: track.left
        anchors.verticalCenter: track.verticalCenter
        width: track.width * root.progress
        height: track.height
        color: Theme.brand
    }
    Rectangle {   // 游标：跟随鼠标当前位置；高 = 轨道高（**不超过进度条**）
        anchors.verticalCenter: track.verticalCenter
        x: (root.hovered ? root.hoverFrac : root.progress) * (track.width - width)
        width: Theme.osdCursorW
        height: track.height
        color: Theme.overlayCursor
    }

    MouseArea {
        id: ma
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor

        //   悬停 / 移动时更新锁存的条目（**离开不动它**，见 hoverIndex 的说明）
        onEntered: root.hoverIndex = root.indexAt(mouseX)
        // 显式声明 mouse 形参（QML 已弃用隐式注入）
        onPressed: function (mouse) { root.seeked(root.indexAt(mouse.x)) }
        onPositionChanged: function (mouse) {
            if (containsMouse)
                root.hoverIndex = root.indexAt(mouse.x)
            if (pressed)
                root.seeked(root.indexAt(mouse.x))
        }
    }

    opacity: root.overlaysVisible ? 1.0 : 0.0
    Behavior on opacity { NumberAnimation { duration: Theme.durMed; easing.type: Easing.OutCubic } }
}
