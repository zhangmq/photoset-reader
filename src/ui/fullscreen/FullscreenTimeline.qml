import QtQuick
import ImageBrowser

// ══ 全屏 · 时间轴（横跨整个条目序列）══════════════════════════
//   设计 §3.5：可点击 / 拖动跳转
//   用户 2026-10-11 修正三点：
//     · **已完成进度用主色**（`brand`）—— 原来只有一条灰轨道
//     · **游标跟随鼠标当前位置**（hover 时），不是停在点击位置
//     · **游标高不超过进度条高度**（原来整行 14 高，太高）
//   几何照抄 vsr-player `ProgressSlider.qml`：行高 `osdTrackH`(14)、
//   轨道贴底、hover/拖动时轨道加高（`osdTrackLineH` 4 → `osdTrackLineHoverH` 6）
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
    implicitHeight: Theme.osdTrackH

    Rectangle {   // 轨道（整条）
        id: track
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: root.hovered || root.pressed ? Theme.osdTrackLineHoverH
                                             : Theme.osdTrackLineH
        color: Theme.overlayTrack
        Behavior on height { NumberAnimation { duration: Theme.durFast } }
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

        function indexAt(x) {
            if (root.total <= 1)
                return 0
            return Math.round(Math.max(0, Math.min(1, x / width)) * (root.total - 1))
        }
        // 显式声明 mouse 形参（QML 已弃用隐式注入）
        onPressed: function (mouse) { root.seeked(indexAt(mouse.x)) }
        onPositionChanged: function (mouse) {
            if (pressed)
                root.seeked(indexAt(mouse.x))
        }
    }

    opacity: root.overlaysVisible ? 1.0 : 0.0
    Behavior on opacity { NumberAnimation { duration: Theme.durMed; easing.type: Easing.OutCubic } }
}
