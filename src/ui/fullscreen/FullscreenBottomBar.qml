import QtQuick
import AppUi

// ══ 全屏 · 底部类播放器控制条（浮层，照抄 vsr-player `BottomBar.qml`）══
//   · 图片占满窗口，本栏**覆盖其上**，不占布局（设计 §3.1）
//   · 背景 = **不透明** `bgBase`（用户 2026-10-11：渐变 → 纯色 → 取消半透明）
//   · **隐藏逻辑不用计时器**：可见性 = 鼠标是否在热区内
//       `implicitHeight = osdHotZoneH(40) + osdTrackH(14) + barH(48)`
//       —— 热区 + 时间轴 + 控制条**视作一个连续区域**，整块一个 HoverHandler
//       （vsr-player 反面教训：三段各自判定会在段间留 6~8px 死带 → 鼠标穿过时
//        "可见→消失→可见"）
//       `timeline.pressed` 单独保留：拖动中指针可能移出区域，但必须保持可见
//   · 显隐用 `HoverHandler`（只观察 hover）；栏**可见时**另有一层 MouseArea 专门挡事件
//     （用户 2026-10-20：压在栏上不许是拖拽光标、不许把拖动/滚轮穿透给画布）
//   · 浮层隐藏时按钮 `enabled: false`（淡出后仍命中测试会误点）
//   · 左：[上一文件夹] [⏮（长按连续**后退**）] [⏯] [⏭（长按连续**快进**）] [下一文件夹]
//     （**纯图标**，目标名字在 tooltip；**长按对称** —— 用户 2026-10-12）
//   · 右：[画面缩放▾] [阅读版面▾] [锁定缩放] [切换速度▾] [循环▾] [全屏窗口] [OSD]
//   ★ 2026-10-31：**缩放算法已挪进右键菜单**（用户）—— 本栏不再有它。
//   · 进度（序号/总数）在**顶栏**，不在本栏；OSD 面板不随本栏显隐（用户 2026-10-11）
//   ★ 2026-10-31（用户）：**依赖文件上下文的控件在没有来源时禁用**；不依赖文件的照常可用
//     （它们改的是全局默认，打开文件后生效）—— 见 `hasSource`。
Item {
    id: root

    property int    index: 0              // 0-based 条目序号（时间轴用）
    property int    count: 1
    property bool   playing: false
    property int    loopMode: 0           // 0=不循环 1=全部循环 2=文件夹循环
    property int    intervalMs: 300       // 切换速度（幻灯片间隔）
    readonly property var intervalPresets: [500, 1000, 2000, 3000, 4000, 5000]
    readonly property bool intervalIsPreset: intervalPresets.indexOf(root.intervalMs) >= 0
    //   ★ 切换速度菜单的行（**选中态 = 对号**，与库屏排序菜单同一写法）：
    //     选中行在前导列显示对号；末尾两项是加速 / 减速（`keepOpen` = 可连点、不关菜单）
    readonly property var intervalItems: [
        { icon: root.intervalMs === 500  ? Theme.icCheck : "", text: "500 ms", selected: root.intervalMs === 500 },
        { icon: root.intervalMs === 1000 ? Theme.icCheck : "", text: "1 s",    selected: root.intervalMs === 1000 },
        { icon: root.intervalMs === 2000 ? Theme.icCheck : "", text: "2 s",    selected: root.intervalMs === 2000 },
        { icon: root.intervalMs === 3000 ? Theme.icCheck : "", text: "3 s",    selected: root.intervalMs === 3000 },
        { icon: root.intervalMs === 4000 ? Theme.icCheck : "", text: "4 s",    selected: root.intervalMs === 4000 },
        { icon: root.intervalMs === 5000 ? Theme.icCheck : "", text: "5 s",    selected: root.intervalMs === 5000 },
        { sep: true },
        { icon: root.intervalIsPreset ? "" : Theme.icCheck,
          text: qsTr("Custom (%1 ms)").arg(root.intervalMs), selected: !root.intervalIsPreset },
        { sep: true },
        //   ★ 这两行**不带 +/− 图标**（用户 2026-10-19：文本前带图标容易看反方向）；
        //     `keepOpen` = 点了不关菜单，可以连点
        { text: qsTr("Faster"), keepOpen: true },
        { text: qsTr("Slower"), keepOpen: true }
    ]
    property int    fitMode: 1            // 0=原始 1=适应屏幕 2=适应宽度 3=适应高度
    property bool   doublePage: false     // 阅读版面：单幅 / 双幅
    property bool   zoomLocked: false     // 锁定缩放（跨画面保留 zoom/pan；属于 viewer 态）
    property bool   windowFullscreen: false   // 窗口全屏
    property bool   osdVisible: false         // OSD 信息行显隐
    property string prevFolderName: ""
    property string nextFolderName: ""
    property bool   overlaysVisible: true
    //   ★ 2026-10-31（没有来源时）：**依赖文件上下文的控件禁用**（上/下一张、播放、上/下一文件夹、
    //     时间轴、锁定缩放、OSD）；**不依赖文件的**（画面缩放 / 版面 / 算法 / 切换速度 / 循环 /
    //     全屏窗口）照常可用 —— 它们改的是**全局默认**，打开文件后自动生效。
    property bool   hasSource: true

    signal prevRequested()
    signal playToggled()
    signal nextRequested()
    signal prevFolderRequested()
    signal nextFolderRequested()
    signal loopModeSelected(int mode)
    signal intervalSelected(int ms)
    signal intervalStep(int deltaMs)      // 加速 / 减速（步进 10ms）
    function openIntervalMenu() { intervalMenu.open() }   // 开发自检用（截图：菜单打开态）
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
        enabled: root.hasSource
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

        // ★ 挡住事件（用户 2026-10-20）：浮层下面是画布的拖拽/缩放手势区。
        //   声明在最前 ⇒ 栏内按钮仍在上层；隐藏时自动让开（visible=false）。
        MouseArea {
            anchors.fill: parent
            visible: root.overlaysVisible
            acceptedButtons: Qt.AllButtons
            cursorShape: Qt.ArrowCursor
            onWheel: (w) => { w.accepted = true }
        }

        // ── 左：文件夹 + 传输 ──
        Row {
            anchors.left: parent.left
            anchors.leftMargin: Theme.spaceLg
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spaceXs

            NavBtn {   // 上一文件夹（**纯图标**；名字放 tooltip —— 用户 2026-10-11）
                anchors.verticalCenter: parent.verticalCenter
                glyph: Theme.icFolderPrev
                tooltip: root.prevFolderName !== "" ? qsTr("Previous folder: %1").arg(root.prevFolderName)
                                                   : qsTr("Previous folder")
                enabled: root.overlaysVisible && root.hasSource && root.prevFolderName !== ""
                onTriggered: root.prevFolderRequested()
            }
            NavBtn {
                id: prevBtn
                anchors.verticalCenter: parent.verticalCenter
                glyph: Theme.icPrev
                tooltip: qsTr("Previous page (←); hold for continuous rewind")
                enabled: root.overlaysVisible && root.hasSource
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
                tooltip: root.playing ? qsTr("Pause slideshow (Space)") : qsTr("Play slideshow (Space)")
                enabled: root.overlaysVisible && root.hasSource
                onTriggered: root.playToggled()
            }
            NavBtn {
                id: nextBtn
                anchors.verticalCenter: parent.verticalCenter
                glyph: Theme.icNext
                tooltip: qsTr("Next page (→); hold for continuous fast-forward")
                enabled: root.overlaysVisible && root.hasSource
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
                tooltip: root.nextFolderName !== "" ? qsTr("Next folder: %1").arg(root.nextFolderName)
                                                   : qsTr("Next folder")
                enabled: root.overlaysVisible && root.hasSource && root.nextFolderName !== ""
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
                tooltip: qsTr("Zoom: %1").arg([qsTr("Original size"), qsTr("Fit screen"), qsTr("Fit width"), qsTr("Fit height")][root.fitMode])
                enabled: root.overlaysVisible
                onTriggered: fitMenu.visible ? fitMenu.close() : fitMenu.open()
            }
            PopupMenu {
                id: fitMenu
                anchorTarget: fitBtn
                items: [
                    { text: qsTr("Original size"), icon: Theme.icFitActual, selected: root.fitMode === 0 },
                    { text: qsTr("Fit screen"), icon: Theme.icFitScreen, selected: root.fitMode === 1 },
                    { text: qsTr("Fit width"), icon: Theme.icFitWidth,  selected: root.fitMode === 2 },
                    { text: qsTr("Fit height"), icon: Theme.icFitHeight, selected: root.fitMode === 3 }
                ]
                onItemTriggered: function (i) { root.fitModeSelected(i) }
            }

            NavBtn {   // 阅读版面：单幅 / 双幅
                id: layoutBtn
                anchors.verticalCenter: parent.verticalCenter
                glyph: root.doublePage ? Theme.icDoublePage : Theme.icSinglePage
                tooltip: qsTr("Layout: %1").arg(root.doublePage ? qsTr("Two pages") : qsTr("Single page"))
                enabled: root.overlaysVisible
                onTriggered: layoutMenu.visible ? layoutMenu.close() : layoutMenu.open()
            }
            PopupMenu {
                id: layoutMenu
                anchorTarget: layoutBtn
                items: [
                    { text: qsTr("Single page"), icon: Theme.icSinglePage, selected: !root.doublePage },
                    { text: qsTr("Two pages"), icon: Theme.icDoublePage, selected: root.doublePage }
                ]
                onItemTriggered: function (i) { root.doublePageSelected(i === 1) }
            }

            NavBtn {   // 锁定缩放（UI；需求见设计 §3.8）
                anchors.verticalCenter: parent.verticalCenter
                glyph: root.zoomLocked ? Theme.icLock : Theme.icLockOpen
                tooltip: root.zoomLocked ? qsTr("Zoom locked (kept across screens)") : qsTr("Lock zoom")
                highlighted: root.zoomLocked
                enabled: root.overlaysVisible && root.hasSource
                onTriggered: root.zoomLockToggled()
            }

            NavBtn {   // 切换速度（幻灯片间隔）：预设 + 自定义 + 加减速
                id: intervalBtn
                anchors.verticalCenter: parent.verticalCenter
                glyph: Theme.icInterval
                tooltip: root.intervalIsPreset ? qsTr("Speed: %1 ms").arg(root.intervalMs)
                                               : qsTr("Speed: %1 ms (custom)").arg(root.intervalMs)
                enabled: root.overlaysVisible
                onTriggered: intervalMenu.visible ? intervalMenu.close() : intervalMenu.open()
            }
            PopupMenu {
                id: intervalMenu
                anchorTarget: intervalBtn
                items: root.intervalItems
                onItemTriggered: function (i) {
                    //   ★ 行序 = 数组序（分隔线也算一行）：预设 × n ｜ 自定义 ｜ 加速 ｜ 减速
                    //     —— 用**数组长度**定位末尾两项、用**预设个数**定位前缀，
                    //     不再写死下标（写死下标会随分隔线 / 档位增减而错位）。
                    //   ★ 加速 = **间隔变小**（播得更快）；减速 = 间隔变大
                    if (i === root.intervalItems.length - 2)      root.intervalStep(-10)     // 加速
                    else if (i === root.intervalItems.length - 1) root.intervalStep(+10)     // 减速
                    else if (i < root.intervalPresets.length)     root.intervalSelected(root.intervalPresets[i])
                    // 其余（自定义行）不改值：当前值本身就是自定义值
                }
            }

            NavBtn {   // 循环模式：不循环 / 全部循环 / 文件夹循环（三态）
                id: loopBtn
                anchors.verticalCenter: parent.verticalCenter
                glyph: root.loopMode === 0 ? Theme.icLoopOff
                     : root.loopMode === 1 ? Theme.icLoop
                                           : Theme.icLoopFolder
                tooltip: qsTr("Loop: %1").arg([qsTr("No loop"), qsTr("Loop all"), qsTr("Loop folder")][root.loopMode])
                highlighted: root.loopMode !== 0
                enabled: root.overlaysVisible
                onTriggered: loopMenu.visible ? loopMenu.close() : loopMenu.open()
            }
            PopupMenu {
                id: loopMenu
                anchorTarget: loopBtn
                items: [
                    { text: qsTr("No loop"),   icon: Theme.icLoopOff,    selected: root.loopMode === 0 },
                    { text: qsTr("Loop all"),  icon: Theme.icLoop,       selected: root.loopMode === 1 },
                    { text: qsTr("Loop folder"), icon: Theme.icLoopFolder, selected: root.loopMode === 2 }
                ]
                onItemTriggered: function (i) { root.loopModeSelected(i) }
            }
            NavBtn {   // 窗口全屏切换
                anchors.verticalCenter: parent.verticalCenter
                glyph: root.windowFullscreen ? Theme.icFullscreenExit : Theme.icFullscreen
                tooltip: root.windowFullscreen ? qsTr("Exit full screen") : qsTr("Full screen")
                highlighted: root.windowFullscreen
                enabled: root.overlaysVisible
                onTriggered: root.windowFullscreenToggled()
            }

            NavBtn {   // OSD 信息行显隐
                anchors.verticalCenter: parent.verticalCenter
                glyph: Theme.icInfo
                tooltip: root.osdVisible ? qsTr("Hide OSD info") : qsTr("Show OSD info")
                highlighted: root.osdVisible
                enabled: root.overlaysVisible && root.hasSource
                onTriggered: root.osdToggled()
            }
        }
    }
}
