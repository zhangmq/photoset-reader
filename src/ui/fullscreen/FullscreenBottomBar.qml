import QtQuick
import ImageBrowser

// ══ 全屏 · 底部类播放器控制条（浮层，照抄 vsr-player `BottomBar.qml`）══
//   · 图片占满窗口，本栏**覆盖其上**，不占布局（设计 §3.1）
//   · 背景 = **不透明** `bgBase`（用户 2026-10-11：渐变 → 纯色 → 取消半透明）
//   · **隐藏逻辑不用计时器**：可见性 = 鼠标是否在热区内
//       `implicitHeight = osdHotZoneH(40) + osdTrackH(14) + barH(48)`
//       —— 热区 + 时间轴 + 控制条**视作一个连续区域**，整块一个 HoverHandler
//       （vsr-player 反面教训：三段各自判定会在段间留 6~8px 死带 → 鼠标穿过时
//        "可见→消失→可见"）
//       `timeline.pressed` 单独保留：拖动中指针可能移出区域，但必须保持可见
//   · 用 `HoverHandler` 而非 `MouseArea`：只观察 hover，不吞鼠标事件
//   · 浮层隐藏时按钮 `enabled: false`（淡出后仍命中测试会误点）
//   · 左：[上一文件夹] [⏮（长按连续**后退**）] [⏯] [⏭（长按连续**快进**）] [下一文件夹]
//     （**纯图标**，目标名字在 tooltip；**长按对称** —— 用户 2026-10-12）
//   · 右：[画面缩放▾] [阅读版面▾] [锁定缩放] [切换速度▾] [循环▾] [全屏窗口] [OSD]
//   · 进度（序号/总数）在**顶栏**，不在本栏；OSD 面板不随本栏显隐（用户 2026-10-11）
Item {
    id: root

    property int    index: 0              // 0-based 条目序号（时间轴用）
    property int    count: 1
    property bool   playing: false
    property int    loopMode: 0           // 0=不循环 1=全部循环 2=文件夹循环
    property int    intervalMs: 300       // 切换速度（幻灯片间隔）
    readonly property var intervalPresets: [500, 1000, 2000, 3000, 4000, 5000]
    readonly property bool intervalIsPreset: intervalPresets.indexOf(root.intervalMs) >= 0
    property int    fitMode: 1            // 0=原始 1=适应屏幕 2=适应宽度 3=适应高度
    property bool   doublePage: false     // 阅读版面：单幅 / 双幅
    property bool   zoomLocked: false     // 锁定缩放（跨画面保留用户缩放/offset；行为待实现）
    property bool   windowFullscreen: false   // 窗口全屏
    property bool   osdVisible: false         // OSD 信息行显隐
    property string prevFolderName: ""
    property string nextFolderName: ""
    property bool   overlaysVisible: true

    signal prevRequested()
    signal playToggled()
    signal nextRequested()
    signal prevFolderRequested()
    signal nextFolderRequested()
    signal loopModeSelected(int mode)
    signal intervalSelected(int ms)
    signal intervalStep(int deltaMs)      // 加速 / 减速（步进 10ms）
    signal fitModeSelected(int mode)
    signal doublePageSelected(bool on)
    signal zoomLockToggled()
    signal windowFullscreenToggled()
    signal osdToggled()
    signal seeked(int index)

    readonly property bool mouseInRegion: barHover.hovered || timeline.pressed
    readonly property bool popupOpen: fitMenu.visible || layoutMenu.visible
                                      || intervalMenu.visible || loopMenu.visible
    implicitHeight: Theme.osdHotZoneH + Theme.osdTrackH + Theme.barH

    // 覆盖整个 UI 区域（40+14+48）的 hover 源 —— 见文件头注释
    HoverHandler { id: barHover }

    // ── 时间轴（贴控制条上沿）──
    FullscreenTimeline {
        id: timeline
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: bar.top
        total: root.count
        index: root.index
        overlaysVisible: root.overlaysVisible
        onSeeked: function (i) { root.seeked(i) }
    }

    // ── 控制条 ──
    Rectangle {
        id: bar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Theme.barH
        color: Theme.bgBase                  // **不透明**（用户 2026-10-11：能自动显隐就不需要半透明）
        opacity: root.overlaysVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: Theme.durMed; easing.type: Easing.OutCubic } }

        // ── 左：文件夹 + 传输 ──
        Row {
            anchors.left: parent.left
            anchors.leftMargin: Theme.spaceLg
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spaceXs

            NavBtn {   // 上一文件夹（**纯图标**；名字放 tooltip —— 用户 2026-10-11）
                anchors.verticalCenter: parent.verticalCenter
                glyph: Theme.icFolderPrev
                tooltip: root.prevFolderName !== "" ? "上一个文件夹：" + root.prevFolderName
                                                    : "上一个文件夹"
                enabled: root.overlaysVisible && root.prevFolderName !== ""
                onTriggered: root.prevFolderRequested()
            }
            NavBtn {
                id: prevBtn
                anchors.verticalCenter: parent.verticalCenter
                glyph: Theme.icPrev
                tooltip: "上一张（←）；长按 = 连续后退"
                enabled: root.overlaysVisible
                onTriggered: root.prevRequested()          // 单击 = 一张
                onPressedChanged: {                        // ★ 长按**对称**：连续后退（用户 2026-10-12）
                    if (pressed) {
                        backTimer.held = false
                        backTimer.interval = Theme.fastFwdDelayMs
                        backTimer.start()
                    } else {
                        backTimer.stop()
                    }
                }
            }
            NavBtn {
                anchors.verticalCenter: parent.verticalCenter
                glyph: root.playing ? Theme.icPause : Theme.icPlay
                tooltip: root.playing ? "暂停幻灯片（Space）" : "播放幻灯片（Space）"
                enabled: root.overlaysVisible
                onTriggered: root.playToggled()
            }
            NavBtn {
                id: nextBtn
                anchors.verticalCenter: parent.verticalCenter
                glyph: Theme.icNext
                tooltip: "下一张（→）；长按 = 连续快进"
                enabled: root.overlaysVisible
                onTriggered: root.nextRequested()          // 单击 = 一张
                onPressedChanged: {                        // 长按 = 连续快进（设计 §3.5）
                    if (pressed) {
                        repeatTimer.held = false
                        repeatTimer.interval = Theme.fastFwdDelayMs
                        repeatTimer.start()
                    } else {
                        repeatTimer.stop()
                    }
                }
            }
            Timer {   // 长按连续快进：先等 fastFwdDelayMs，之后按 fastFwdFastMs 连续推进
                id: repeatTimer
                property bool held: false
                repeat: true
                onTriggered: {
                    held = true
                    interval = Theme.fastFwdFastMs
                    root.nextRequested()
                }
            }
            Timer {   // 长按连续**后退**（与快进对称；同样先等 fastFwdDelayMs）
                id: backTimer
                property bool held: false
                repeat: true
                onTriggered: {
                    held = true
                    interval = Theme.fastFwdFastMs
                    root.prevRequested()
                }
            }
            NavBtn {   // 下一文件夹（**纯图标**；名字放 tooltip —— 用户 2026-10-11）
                anchors.verticalCenter: parent.verticalCenter
                glyph: Theme.icFolderNext
                tooltip: root.nextFolderName !== "" ? "下一个文件夹：" + root.nextFolderName
                                                    : "下一个文件夹"
                enabled: root.overlaysVisible && root.nextFolderName !== ""
                onTriggered: root.nextFolderRequested()
            }
        }

        // ── 右：循环开关 ──
        Row {
            anchors.right: parent.right
            anchors.rightMargin: Theme.spaceLg
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spaceXs

            NavBtn {   // 画面缩放模式
                id: fitBtn
                anchors.verticalCenter: parent.verticalCenter
                glyph: root.fitMode === 0 ? Theme.icFitActual
                     : root.fitMode === 1 ? Theme.icFitScreen
                     : root.fitMode === 2 ? Theme.icFitWidth
                                          : Theme.icFitHeight
                tooltip: "画面缩放：" + ["原始大小", "适应屏幕", "适应宽度", "适应高度"][root.fitMode]
                enabled: root.overlaysVisible
                onTriggered: fitMenu.visible ? fitMenu.close() : fitMenu.open()
            }
            PopupMenu {
                id: fitMenu
                anchorTarget: fitBtn
                items: [
                    { text: "原始大小", icon: Theme.icFitActual, selected: root.fitMode === 0 },
                    { text: "适应屏幕", icon: Theme.icFitScreen, selected: root.fitMode === 1 },
                    { text: "适应宽度", icon: Theme.icFitWidth,  selected: root.fitMode === 2 },
                    { text: "适应高度", icon: Theme.icFitHeight, selected: root.fitMode === 3 }
                ]
                onItemTriggered: function (i) { root.fitModeSelected(i) }
            }

            NavBtn {   // 阅读版面：单幅 / 双幅
                id: layoutBtn
                anchors.verticalCenter: parent.verticalCenter
                glyph: root.doublePage ? Theme.icDoublePage : Theme.icSinglePage
                tooltip: "阅读版面：" + (root.doublePage ? "双幅" : "单幅")
                enabled: root.overlaysVisible
                onTriggered: layoutMenu.visible ? layoutMenu.close() : layoutMenu.open()
            }
            PopupMenu {
                id: layoutMenu
                anchorTarget: layoutBtn
                items: [
                    { text: "单幅", icon: Theme.icSinglePage, selected: !root.doublePage },
                    { text: "双幅", icon: Theme.icDoublePage, selected: root.doublePage }
                ]
                onItemTriggered: function (i) { root.doublePageSelected(i === 1) }
            }

            NavBtn {   // 锁定缩放（UI；需求见设计 §3.8）
                anchors.verticalCenter: parent.verticalCenter
                glyph: root.zoomLocked ? Theme.icLock : Theme.icLockOpen
                tooltip: root.zoomLocked ? "已锁定缩放（跨画面保留）" : "锁定缩放"
                highlighted: root.zoomLocked
                enabled: root.overlaysVisible
                onTriggered: root.zoomLockToggled()
            }

            NavBtn {   // 切换速度（幻灯片间隔）：预设 + 自定义 + 加减速
                id: intervalBtn
                anchors.verticalCenter: parent.verticalCenter
                glyph: Theme.icInterval
                tooltip: "切换速度：" + root.intervalMs + " ms" + (root.intervalIsPreset ? "" : "（自定义）")
                enabled: root.overlaysVisible
                onTriggered: intervalMenu.visible ? intervalMenu.close() : intervalMenu.open()
            }
            PopupMenu {
                id: intervalMenu
                anchorTarget: intervalBtn
                items: [
                    { text: "500 ms", selected: root.intervalMs === 500 },
                    { text: "1 s",    selected: root.intervalMs === 1000 },
                    { text: "2 s",    selected: root.intervalMs === 2000 },
                    { text: "3 s",    selected: root.intervalMs === 3000 },
                    { text: "4 s",    selected: root.intervalMs === 4000 },
                    { text: "5 s",    selected: root.intervalMs === 5000 },
                    { sep: true },
                    { text: "自定义（" + root.intervalMs + " ms）", selected: !root.intervalIsPreset },
                    { sep: true },
                    { text: "加速", icon: Theme.icPlus,  keepOpen: true },   // 可连点（不关菜单）
                    { text: "减速", icon: Theme.icMinus, keepOpen: true }
                ]
                onItemTriggered: function (i) {
                    if (i < 6)              root.intervalSelected(root.intervalPresets[i])
                    else if (i === 8)       root.intervalStep(+10)      // 加速（自动切自定义）
                    else if (i === 9)       root.intervalStep(-10)      // 减速
                    // i === 7（自定义）不改值：当前值本身就是自定义值
                }
            }

            NavBtn {   // 循环模式：不循环 / 全部循环 / 文件夹循环（三态）
                id: loopBtn
                anchors.verticalCenter: parent.verticalCenter
                glyph: root.loopMode === 0 ? Theme.icLoopOff
                     : root.loopMode === 1 ? Theme.icLoop
                                           : Theme.icLoopFolder
                tooltip: "循环：" + ["不循环", "全部循环", "文件夹循环"][root.loopMode]
                highlighted: root.loopMode !== 0
                enabled: root.overlaysVisible
                onTriggered: loopMenu.visible ? loopMenu.close() : loopMenu.open()
            }
            PopupMenu {
                id: loopMenu
                anchorTarget: loopBtn
                items: [
                    { text: "不循环",     icon: Theme.icLoopOff,    selected: root.loopMode === 0 },
                    { text: "全部循环",   icon: Theme.icLoop,       selected: root.loopMode === 1 },
                    { text: "文件夹循环", icon: Theme.icLoopFolder, selected: root.loopMode === 2 }
                ]
                onItemTriggered: function (i) { root.loopModeSelected(i) }
            }
            NavBtn {   // 窗口全屏切换
                anchors.verticalCenter: parent.verticalCenter
                glyph: root.windowFullscreen ? Theme.icFullscreenExit : Theme.icFullscreen
                tooltip: root.windowFullscreen ? "退出全屏窗口" : "全屏窗口"
                highlighted: root.windowFullscreen
                enabled: root.overlaysVisible
                onTriggered: root.windowFullscreenToggled()
            }

            NavBtn {   // OSD 信息行显隐
                anchors.verticalCenter: parent.verticalCenter
                glyph: Theme.icInfo
                tooltip: root.osdVisible ? "隐藏 OSD 信息" : "显示 OSD 信息"
                highlighted: root.osdVisible
                enabled: root.overlaysVisible
                onTriggered: root.osdToggled()
            }
        }
    }
}
