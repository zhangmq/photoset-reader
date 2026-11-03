import QtQuick
import AppUi

// ══ 全屏 · 顶部 OSD（浮层，照抄 vsr-player `TopBar.qml` 的自动隐藏模型）══
//   · 图片占满窗口，本栏**覆盖其上**，不占布局（设计 §3.1）
//   · 背景 = **不透明** `bgBase`（用户 2026-10-11：渐变 → 纯色 → 取消半透明）
//   · **自持 1px 下边框**（用户 2026-10-11；与浏览的 bar 同一规则）
//   · **隐藏逻辑不用计时器**：可见性 = 鼠标是否在热区内
//       `implicitHeight = barH(48) + osdHotZoneH(40)` —— 栏 + 下方热区
//       **整块一个 HoverHandler**（vsr-player 反面教训：分段判定会在段间留死带，
//        鼠标穿过时"可见→消失→可见"）
//   · 显隐用 `HoverHandler`（只观察 hover）；栏**可见时**另有一层 MouseArea 专门挡事件
//     （用户 2026-10-20：压在栏上不许是拖拽光标、不许把拖动/滚轮穿透给画布）
//   · 浮层隐藏时按钮 `enabled: false`（淡出后仍命中测试会误点）
//
//   ★ 2026-10-31（只剩查看界面 + 右键菜单）：左行 = **[spin] 进度 · 文件名 · 相对路径**，
//     右侧 = **★ 收藏状态**（唯一动作；其余动作都在右键菜单里）。
//     · **扫描中**：最左显示一个 spin（点击 ⇒ 问一下要不要取消扫描）；**扫完 / 被取消都不再显示图标**
//       （「重新扫描」在右键菜单里 —— 用户 2026-11-01）；
//     · **★**：实心 = 已收藏，空心 = 未收藏；点击 = 收藏 / 取消收藏（取消要确认）。
Item {
    id: root

    property bool   overlaysVisible: true
    property string progressText: ""      // 进度（序号/总数）——**属于顶栏，不属于 OSD**
    property string fileName: ""          // 当前文件名（双幅时两页以 " · " 相连）
    property string relativePath: ""      // 当前文件相对**来源根**的路径（用户 2026-10-11 要求）
    property bool   hasSource: true       // 有没有来源（★ 依赖它）
    property bool   scanning: false       // 扫描中（最左 spin；点击 = 问要不要取消）

    property int    discoveredCount: 0    // 已发现多少张（spin 的 tooltip）
    property bool   favorited: false      // 已收藏（★ 实心）

    signal scanCancelRequested()          // 点 spin → 上层弹「取消扫描」确认框
    signal favoriteToggled()              // 点 ★ → 上层决定收藏 / 取消收藏

    readonly property bool mouseInRegion: topHover.hovered
    implicitHeight: Theme.barH + Theme.osdHotZoneH

    HoverHandler { id: topHover }

    Rectangle {
        id: bar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: Theme.barH
        color: Theme.barFill                 // 栏面 = 顶/底栏/预览条同一个令牌（用户 2026-11-03）
        opacity: root.overlaysVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: Theme.durMed; easing.type: Easing.OutCubic } }

        // ★ 挡住事件（用户 2026-10-20）：本栏是浮层，下面是画布的拖拽/缩放手势区。
        //   不挡的话，鼠标压在栏上仍是"拖拽"光标，按下还会拖动画面（事件穿透）。
        //   声明在最前 ⇒ 栏内的按钮（后面声明）仍然在上层可点。
        MouseArea {
            anchors.fill: parent
            visible: root.overlaysVisible
            acceptedButtons: Qt.AllButtons
            cursorShape: Qt.ArrowCursor
            onWheel: (w) => { w.accepted = true }   // 滚轮也不许穿透去缩放
        }

        // 左：[spin] 进度 + 文件名 + 相对路径
        Row {
            anchors.left: parent.left
            anchors.leftMargin: Theme.spaceLg
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spaceMd

            // ── 扫描中的 spin（**最左**，用户 2026-10-31）──
            //   形状 = Loading 浮层那个 spinner（`components/Spinner.qml`，唯一实现），只是**改小**
            Item {
                id: scanSpin
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.controlSm
                height: Theme.controlSm
                visible: root.scanning

                Spinner {
                    anchors.centerIn: parent
                    size: 16
                    lineWidth: 2
                    running: root.scanning
                }
                MouseArea {
                    id: spinMa
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: root.overlaysVisible
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.scanCancelRequested()
                }
                HintBubble {
                    anchors.fill: parent
                    text: qsTr("Scanning: %1 files found (click to cancel)").arg(root.discoveredCount)
                    hovered: spinMa.containsMouse && root.overlaysVisible
                }
            }

            Text {   // 进度：**不属于 OSD**（用户 2026-10-11），只随顶栏显隐
                anchors.verticalCenter: parent.verticalCenter
                visible: root.progressText !== ""
                text: root.progressText
                color: Theme.textPrimary
            }
            Text {   // 当前文件名（长名中间省略，保留首尾辨识度）
                anchors.verticalCenter: parent.verticalCenter
                visible: root.fileName !== ""
                text: root.fileName
                color: Theme.textPrimary
                elide: Text.ElideMiddle
                width: Math.min(implicitWidth, Theme.osdPathMaxW)
            }
            Text {   // 相对图集根的路径（长路径中间省略，保留首尾辨识度）
                anchors.verticalCenter: parent.verticalCenter
                visible: root.relativePath !== "" && root.relativePath !== root.fileName
                text: root.relativePath
                color: Theme.textSecondary      // 路径是次要信息（不抢文件名）
                elide: Text.ElideMiddle
                width: Math.min(implicitWidth, Theme.osdPathMaxW)
            }
        }

        // 右：★ 收藏状态（实心 = 已收藏；点击 = 收藏 / 取消收藏）
        NavBtn {
            anchors.right: parent.right
            anchors.rightMargin: Theme.spaceLg
            anchors.verticalCenter: parent.verticalCenter
            glyph: root.favorited ? Theme.icStar : Theme.icStarO
            tooltip: root.favorited ? qsTr("Favorited (click to remove)") : qsTr("Add to favorites (saves progress)")
            //   ★ 不做"扫描中置灰"（用户 2026-10-31）：点了弹框告知，见上层
            enabled: root.overlaysVisible && root.hasSource
            onTriggered: root.favoriteToggled()
        }

        // 底边框（bar 自持）
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: Theme.hairline
            color: Theme.borderL1
        }

    }
}
