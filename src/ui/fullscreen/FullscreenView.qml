import QtQuick
import QtQuick.Window
import AppUi

// ══ 全屏（沉浸）视图 ══════════════════════════════════════════
//   设计 §3.1：**图片用整个窗口**，所有控件是**覆盖其上的浮层**；
//   浮层显隐不改变画面尺寸、不引起重排（overlay 架构，不做任何占位布局）。
//   · 画布与浮层**跟随主题**（用户 2026-10-11 定案；浅色模式 = 浅底 + 深字）
//   · 自动隐藏**照抄 vsr-player，不用计时器**：
//       `showUi = 顶栏热区 || 底栏热区 || 弹层打开`，状态写回 ViewModel
//     （vsr-player main.qml 原话："移出热区立即隐藏，无计时器"）
//   · 控件条显隐是**纯 UI 本地状态**（鼠标热区派生）：只在本文件里算，不进任何 model
//     （用户 2026-10-20：它只是"鼠标 hover 影响的行为"，不是业务状态）
Item {
    id: root

    // ★ 2026-10-31：只剩查看界面 + **右键菜单**；本视图不再对顶栏/底栏发动作信号
    //   （打开 / 保存 / 缩放算法 / 设置 全在菜单里，见文件末尾 `contextMenu`）。

    // 本屏是不是**双幅**（两个位置 → 并排两张图）；`keyCount` = 本屏位置数（1 或 2）。
    //   ★ 坏的位置**留空位**：`keyCount` 仍是 2（两格都占着），那一格只是没图。
    readonly property bool twoUp: ui.viewer ? ui.viewer.keyCount > 1 : false

    // 这一屏的画面**由 CPU 项出**还是由 GPU 项出（设计 16 §五）：
    //   ① 选了 `Smooth (Qt)` —— 这一项本来就是 Qt 的实现（GPU 管线不实现它）；
    //   ② 场景图后端是 software —— `QQuickRhiItem` 在软件后端下什么都不画，
    //      此时**无论选了什么算法**都降级成 Qt 平滑（画质不保证，见设计 16 §五）。
    //   ⇒ 两个项都建出来、按这个布尔切 `visible`（不画的那个不参与渲染）。
    readonly property bool cpuScale: (ui.viewer && ui.viewer.scaleAlgorithm === "smooth")
                                     || ui.softwareBackend

    // 缩放状态（zoom/pan）在 `ui` 里是**设备像素**口径 ⇒ 视口要连 dpr 一起报上去。
    //   `fit` 不是属性：它只是"算一次 zoom"的命令（见 gpuscale.h / UiModel::applyFit）。
    //   ★ dpr 取**窗口的** `devicePixelRatio`（QQuickWindow 的属性），不是 `Screen` 的：
    //     窗口在**两块缩放不同的显示器之间**移动时，变的是**窗口的有效 dpr**
    //     （Wayland 按窗口缩放；`Screen` 附加属性在跨屏场景下不一定跟着变 —— 实测就是
    //     这里没更新，导致 VM 一直用旧 dpr：拖拽位移换算错、pan 越界，画面一跳）。
    //     下面的 `Connections` 监听它的变化 + `screenChanged`，两者任一变化都重推视口。
    readonly property real dpr: (Window.window && Window.window.devicePixelRatio > 0)
                                ? Window.window.devicePixelRatio : 1
    function pushViewport() {
        if (ui.viewer && canvas.width > 0 && canvas.height > 0)
            ui.viewer.setViewport(canvas.boxW, canvas.height, root.dpr)
    }
    // viewer 换了（新来源 / 重进查看屏）⇒ 把视口补推给它，否则它算不出 fit
    Connections {
        target: ui
        function onViewerChanged() { root.pushViewport() }
    }

    // ── 幻灯片时的"鼠标静止"处理（参考 vsr-player 的 CursorAutoHide）────────
    //   播放中且鼠标静止 2.5 s ⇒ 隐藏指针并收起浮层；任何鼠标移动 / 按键立刻恢复。
    readonly property bool playing: ui.viewer ? ui.viewer.playing : false
    //   指针是否在"功能区"（顶栏 / 底栏 / 预览条 / 弹层）
    readonly property bool pointerInChrome: topBar.mouseInRegion || bottomBar.mouseInRegion
                                            || bottomBar.popupOpen || bottomBar.previewStripVisible
    property bool cursorHidden: false
    function wakeUi() {
        if (root.cursorHidden)
            root.cursorHidden = false
        if (root.playing)
            idleTimer.restart()
    }
    Timer {
        id: idleTimer
        interval: 2500
        repeat: false
        running: root.playing
        //   ★ 鼠标停在功能区（顶/底栏区域、预览条）⇒ **不自动隐藏**（用户 2026-11-04）：
        //     否则播放中指针不动几秒，UI 连同指针一起消失，正在用的控件抓不到。
        onTriggered: {
            if (!root.playing || root.cursorHidden) return
            if (root.pointerInChrome) { idleTimer.restart(); return }
            root.cursorHidden = true
        }
    }
    //   指针进入功能区 ⇒ 立刻恢复指针/浮层，并重新计时（避免"鼠标没动"被当成静止）
    onPointerInChromeChanged: {
        if (!root.pointerInChrome) return
        if (root.cursorHidden) root.cursorHidden = false
        if (root.playing) idleTimer.restart()
    }
    Connections {
        target: ui.viewer
        enabled: ui.viewer !== null
        function onPlayingChanged() {
            if (ui.viewer.playing) idleTimer.restart()
            else { idleTimer.stop(); root.cursorHidden = false }
        }
    }

    // ── 浮层显隐：纯函数（照抄 vsr-player main.qml）──
    //   ★ 显示条件（用户 2026-11-04 定稿）：
    //     底栏 = 指针在底栏（轨道+控制条）内、或**预览条在屏**（不是"指针在预览条里"）；
    //     预览条 = 指针在进度条 或 预览条内（见 `FullscreenBottomBar.previewStrip.visible`）。
    readonly property bool showUi: !root.cursorHidden
                                   && (topBar.mouseInRegion || bottomBar.mouseInRegion
                                       || bottomBar.popupOpen || bottomBar.previewStripVisible)

    // ── 设置对话框（两屏同一实现；当前阶段单纯 UI）──
    SettingsDialog {
        id: settingsDialog
    }

    // ── 显示时钟（2026-11-06 用户定）：每**呈现一帧**叫一次 viewer ──
    //   ★ 幻灯片的换屏**只在这里发生**，且每帧最多换一屏 ⇒ 每一屏都至少被画出一帧，
    //     0 ms 也不会"一帧跳两三页"。节奏 = 刷新率 × 间隔（`ViewerModel::framePresented()`）。
    //   ★ 只在播放时 running：暂停时不让渲染循环空转（省电）。
    FrameAnimation {
        running: ui.viewer !== null && ui.viewer.playing
        onTriggered: ui.viewer.framePresented()
    }
    Rectangle {
        anchors.fill: parent
        color: Theme.bgBase                 // 画布底（跟随主题；用 DSH alias 令牌）
    }

    // ── 画面：当前屏的**全部条目**并排（跨页 = 两个条目 → 两张图）──
    //   ★ 版面（几张、怎么摆）是**视图**的事：解码层只按条目出图，**不做任何合成**
    //     （用户明确否决"让 provider 把两页合成一张"——那是把排版决定塞进解码层与它的缓存：
    //      放错层、版面一切换缓存即失效、还破坏"一个 id = 一个条目"。
    //      分界线见 AGENTS.md 铁律区：**资源约束归 provider，排版编排归视图**）
    //   ★ **渲染走 GPU 缩放**（设计 13 §八：线性光 + anti-ringing；CPU 实现只作测试参照）
    //   ★ 两张共用**同一套缩放与平移**（整页一个比例，正交于 fit 模式 —— 不给双幅单独配一套）
    Item {
        id: canvas
        anchors.fill: parent

        // 手势态（缩放 / 平移）**归 ui**（`ui.zoom` / `ui.panX` / `ui.panY`）——
        //   缩放约束只是 uiModel 与 GPU 缩放器之间的事，viewer 不需要知道。
        //   换画面复位由 `UiModel` 自己做（QML 里不再有那段逻辑）。
        readonly property real boxW: root.twoUp ? Math.floor(width / 2) : width

        // 视口变了（窗口尺寸 / 单双幅）⇒ 告诉 ui；它会决定"要不要按当前 fit 重算 zoom"
        onWidthChanged: root.pushViewport()
        onHeightChanged: root.pushViewport()
        //   ★ **盒宽变了也要推**：双幅时每格只有半宽，一屏从"成对（半宽）"变到
        //     "宽图单独一屏（整宽）"时窗口尺寸根本没变，只有 `boxW` 变 ——
        //     不推的话 viewer 仍拿旧的半宽算 fit ⇒ 宽图被当成半幅画（用户 2026-11-02 报的
        //     "宽图不占满窗口，像是按单幅宽度算的"就是这么来的，而且概率性出现：
        //     只有"上一次推给它的盒宽"和当前屏不一致时才看得出来）。
        onBoxWChanged: root.pushViewport()
        Component.onCompleted: root.pushViewport()

        // ── 空库屏守卫（2026-10-14）───────────────────────────────────────────
        //   `ui.viewer` 为空时（"没有来源" / 还没打开任何来源）**不建** GpuScalerItem：
        //   否则 `sourceImage: ui.viewer ? … : null` 会把 JS `null` 塞给 QImage 属性
        //   ⇒ 日志两条 `Unable to assign null to QImage`（实测）。
        //   ★ 只改"建不建"：`GpuScalerItem` / shader / 缩放路径**一字未动**。
        //   ★ 包在 `Loader` 里而不是包单个 item —— `autoSize` 时组件用 `setWidth` 设的是
        //     **显式**尺寸，Loader 只跟 `implicitWidth` ⇒ 逐个包会让两个 item 都停在 0 宽
        //     （Row 里重叠）；整段包则 Row 自己的 `anchors.centerIn: parent` 照常生效。
        //   ★ `Loader` 必须 `anchors.centerIn: parent`，**不能** `anchors.fill` ——
        //     `anchors.fill` 会把 Loader（连带它 load 的 Row）拉成整个画布宽，Row 的
        //     `anchors.centerIn` 就退化成"在与画布等宽的盒子里居中" ⇒ 内容从 x=0 起、
        //     贴着左边（实测：内容宽 2839、左边距 0、右边距 161）。居中才让 Loader 跟内容尺寸。
        Loader {
            anchors.centerIn: parent
            active: ui.viewer !== null
            sourceComponent: canvasComp
        }

        Component {
            id: canvasComp

            Row {
                anchors.centerIn: parent
                spacing: 0

                //   一个**位置**一个盒子：盒子恒为 `canvas.boxW × canvas.height`。
                //   ⇒ 坏的那一格（空图）**照样占它那一半** ——「双幅留空位」，另一页不失位。
                //   ⚠️ 盒子必须给**显式**尺寸：`GpuScalerItem` 在 `autoSize` 下自己 `setWidth`，
                //      空图时那颗不设（尺寸停在 0）⇒ 让它单独摆会缩成 0 宽、两页挤到一起。
                //   ★ 双幅：两张**贴在一起**（左盒的靠右、右盒的靠左）—— 与"Row 直接摆两张"
                //      一样；单幅 / 本屏只有一格（宽图、末尾一张）⇒ 盒子 = 整宽，画面居中。
                Item {
                    width: canvas.boxW
                    height: canvas.height

                    GpuScalerItem {
                        id: imgA
                        anchors.centerIn: parent
                        anchors.horizontalCenterOffset: root.twoUp ? (parent.width - width) / 2 : 0
                        objectName: "imgA"
                        visible: !root.cpuScale
                        algorithm: ui.viewer ? ui.viewer.scaleAlgorithm : "lanczos3"
                        viewport: Qt.size(canvas.boxW, canvas.height)
                        sourceImage: ui.viewer ? ui.viewer.imageA : null
                        zoom: ui.viewer ? ui.viewer.zoom : 1.0
                        panX: ui.viewer ? ui.viewer.panX : 0.0
                        panY: ui.viewer ? ui.viewer.panY : 0.0
                    }

                    // 同一个位置的 CPU 版（只在 `cpuScale` 时可见）——几何用同一份 makePlan
                    CpuScalerItem {
                        anchors.centerIn: parent
                        anchors.horizontalCenterOffset: root.twoUp ? (parent.width - width) / 2 : 0
                        objectName: "imgAcpu"
                        visible: root.cpuScale
                        viewport: Qt.size(canvas.boxW, canvas.height)
                        sourceImage: ui.viewer ? ui.viewer.imageA : null
                        zoom: ui.viewer ? ui.viewer.zoom : 1.0
                        panX: ui.viewer ? ui.viewer.panX : 0.0
                        panY: ui.viewer ? ui.viewer.panY : 0.0
                    }
                }
                Item {
                    width: canvas.boxW
                    height: canvas.height
                    visible: root.twoUp              // 单幅 / 只有一格：没有第二个盒子

                    GpuScalerItem {
                        id: imgB
                        anchors.centerIn: parent
                        anchors.horizontalCenterOffset: root.twoUp ? -(parent.width - width) / 2 : 0
                        objectName: "imgB"
                        visible: !root.cpuScale
                        algorithm: ui.viewer ? ui.viewer.scaleAlgorithm : "lanczos3"
                        viewport: Qt.size(canvas.boxW, canvas.height)
                        sourceImage: ui.viewer ? ui.viewer.imageB : null
                        zoom: ui.viewer ? ui.viewer.zoom : 1.0
                        panX: ui.viewer ? ui.viewer.panX : 0.0
                        panY: ui.viewer ? ui.viewer.panY : 0.0
                    }

                    CpuScalerItem {
                        anchors.centerIn: parent
                        anchors.horizontalCenterOffset: root.twoUp ? -(parent.width - width) / 2 : 0
                        objectName: "imgBcpu"
                        visible: root.cpuScale
                        viewport: Qt.size(canvas.boxW, canvas.height)
                        sourceImage: ui.viewer ? ui.viewer.imageB : null
                        zoom: ui.viewer ? ui.viewer.zoom : 1.0
                        panX: ui.viewer ? ui.viewer.panX : 0.0
                        panY: ui.viewer ? ui.viewer.panY : 0.0
                    }
                }
            }
        }

        // 交互缩放（滚轮）与拖拽平移（设计 08 §1.3 / 01 §3.8）
        MouseArea {
            anchors.fill: parent
            //   ★ 2026-10-31：右键 = **右键菜单**（用户）；拖拽平移仍只跟左键。
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            hoverEnabled: true                // 只为"鼠标静止 → 隐藏指针"服务
            property bool panning: false
            property real lastX: 0
            property real lastY: 0
            cursorShape: root.cursorHidden ? Qt.BlankCursor
                                           : (panning ? Qt.ClosedHandCursor : Qt.OpenHandCursor)

            function openContextMenu(x, y) { root.openContextMenu(canvas, x, y) }

            onClicked: (m) => {
                root.wakeUi()
                if (m.button === Qt.RightButton)
                    openContextMenu(m.x, m.y)
            }
            onPressed: (m) => {
                root.wakeUi()
                if (m.button !== Qt.LeftButton) return
                panning = true
                lastX = m.x
                lastY = m.y
            }
            onReleased: panning = false
            onCanceled: panning = false
            onPositionChanged: (m) => {
                root.wakeUi()
                if (!panning) return
                // 内容跟着指针走：**位移交给 ui 换算并夹取**（`panBy` 里知道"完整缩放后尺寸"）。
                //   ★ 别在这里直接 `ui.panX -= dx`：没有夹取就会出现"拖过头、再拖回来要空走一段"。
                if (ui.viewer) ui.viewer.panBy(m.x - lastX, m.y - lastY)
                lastX = m.x
                lastY = m.y
            }
            onWheel: (w) => {
                root.wakeUi()
                const k = w.angleDelta.y > 0 ? 1.1 : (1 / 1.1)
                // 以**鼠标位置**为锚点缩放（几何在 ui / gpuscale 里算，QML 只报位置）
                if (ui.viewer) ui.viewer.zoomAtPoint(k, w.x, w.y)
            }
        }
    }

    // ── Loading 浮层（新序列 notReady / 前进到扫描边界时显示）─────────────
    //   设计：**不占位、不重排**（浮层盖在画面之上）；跟随主题；不透明 bgBase；
    //         转圈用**声明式动画**（RotationAnimator，不是 Timer）
    //   ★ 只有两种 loading（用户 2026-10-13）：① 序列 notReady（count==0 且没扫完）
    //     ② 前进到扫描边界。**正常"图还没解码好"不算** ✓
    Rectangle {
        id: loadingLayer
        objectName: "loadingLayer"          // 截图/用例侧靠它定位
        anchors.fill: parent
        color: Theme.bgBase
        visible: ui.viewer ? ui.viewer.loading : false
        opacity: visible ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.durMed; easing.type: Easing.OutCubic } }

        Column {
            anchors.centerIn: parent
            spacing: Theme.spaceMd

            Spinner {
                anchors.horizontalCenter: parent.horizontalCenter
                size: 32
                lineWidth: 3
                running: loadingLayer.visible
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Loading…")
                color: Theme.textSecondary
                font.pointSize: Theme.fontSm
            }
        }
    }

    // ★ 拒绝了密码 ⇒ 这张图拿不到：给一句说明，别只留一块空白（用户 2026-11-03）
    //   弹框还在时不显示（由弹框说明情况）
    Text {
        anchors.centerIn: parent
        z: 1
        visible: ui.viewer !== null && ui.viewer.screenNeedsPassword && !ui.passwordPromptVisible
        text: qsTr("No password, cannot view")
        color: Theme.textSecondary
    }

    // ★ 空来源（打开成功但 0 条）：别留一整块黑。扫描中报进度，扫完给说明。
    Column {
        anchors.centerIn: parent
        spacing: Theme.spaceSm
        z: 1
        //   ★ loading 浮层（转圈）出现时**不要**同时显示这块空占位 —— 两者都居中，会叠在一起
        //     （用户 2026-11-03：等图时"spin 和空占位重叠"）
        visible: ui.viewer !== null && ui.viewer.totalPages === 0 && !ui.viewer.loading
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: ui.scanning ? qsTr("Scanning…") : qsTr("Nothing to show here")
            color: Theme.textPrimary
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: ui.scanning ? qsTr("%1 files found").arg(ui.discoveredCount)
                              : qsTr("Try another folder, or right-click → Open…")
            color: Theme.textCaption
            font.pointSize: Theme.fontSm
        }
    }

    // 切画面复位手势态：**归 `UiModel`**（它连 viewer 的 imageAChanged/imageBChanged 做，
    //   `zoomLocked` 时保留）—— QML 里不再有这段逻辑。

    // ⚠️ 2026-10-11：**移除"点击画面任意位置跳页"** —— 用户指出这不应是最终应用行为
    //    （demo 阶段曾用左右半屏跳页；最终交互待用户定义，见设计 §3.2 待定项）

    // ── 顶部 OSD 浮层 ──
    FullscreenTopBar {
        id: topBar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        overlaysVisible: root.showUi
        hasSource: ui.viewer !== null
        scanning: ui.scanning
        discoveredCount: ui.discoveredCount
        favorited: ui.sourceFavorited
        onScanCancelRequested: cancelScanDialog.open()
        onFavoriteToggled: root.toggleFavorite()
        // 没有条目时显示 `0 / 0`（不能是 `1 / 0`：空目录也是一种合法状态）
        progressText: !ui.viewer ? ""
                      : ui.viewer.totalPages > 0
                        ? ((ui.viewer.currentPage + 1) + " / " + ui.viewer.totalPages)
                        : "0 / 0"
        fileName: ui.viewer ? ui.viewer.currentName : ""
        relativePath: ui.viewer ? ui.viewer.relativePath : ""
    }

    // ── 底部类播放器控制条 ──
    //   ★ 2026-10-31（用户）：**没有来源时，不依赖文件的控件照样可用** —— 它们改的是**全局默认**
    //     （画面缩放 / 阅读版面 / 循环 / 切换速度），下一次打开来源时由
    //     `applyViewerDefaults()` 生效；依赖文件上下文的（上/下一张、播放、上/下一文件夹、时间轴、
    //     锁定缩放、OSD）由 `hasSource` 禁用。
    FullscreenBottomBar {
        id: bottomBar
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        hasSource: ui.viewer !== null
        index: ui.viewer ? ui.viewer.cursor : 0
        count: ui.viewer ? ui.viewer.count : 0
        fitMode: ui.viewer ? ui.viewer.fitMode : ui.fitMode
        doublePage: ui.viewer ? ui.viewer.doublePage : ui.defaultLayout === 1
        loopMode: ui.viewer ? ui.viewer.loopMode : ui.defaultLoop
        playing: ui.viewer ? ui.viewer.playing : false
        intervalMs: ui.viewer ? ui.viewer.intervalMs : ui.slideshowIntervalMs
        prevFolderName: ui.viewer ? ui.viewer.prevFolderName : ""
        nextFolderName: ui.viewer ? ui.viewer.nextFolderName : ""
        zoomLocked: ui.viewer ? ui.viewer.zoomLocked : false
        windowFullscreen: ui.windowFullscreen
        osdVisible: ui.viewer ? ui.viewer.osdVisible : false
        overlaysVisible: root.showUi
        previewEnabled: ui.previewStrip
        onPrevRequested: { if (ui.viewer) ui.viewer.stepPage(-1) }
        onNextRequested: { if (ui.viewer) ui.viewer.stepPage(1) }
        //   ★ 没有 viewer 时改**全局默认**（打开下一个来源时生效）；有 viewer 时只改当前阅读态
        onFitModeSelected: function (m) {
            if (ui.viewer) ui.viewer.fitMode = m
            else ui.fitMode = m
        }
        onDoublePageSelected: function (on) {
            if (ui.viewer) ui.viewer.doublePage = on
            else ui.defaultLayout = on ? 1 : 0
        }
        onLoopModeSelected: function (m) {
            if (ui.viewer) ui.viewer.loopMode = m
            else ui.defaultLoop = m
        }
        onPlayToggled: { if (ui.viewer) ui.viewer.togglePlaying() }
        onIntervalSelected: function (ms) {
            if (ui.viewer) ui.viewer.intervalMs = ms
            else ui.slideshowIntervalMs = ms
        }
        onIntervalStep: function (dir) { ui.stepSlideshowInterval(dir) }   // 分段步进在 C++（唯一实现）
        onPrevFolderRequested: { if (ui.viewer) ui.viewer.stepFolder(-1) }
        onNextFolderRequested: { if (ui.viewer) ui.viewer.stepFolder(1) }
        onZoomLockToggled: { if (ui.viewer) ui.viewer.zoomLocked = !ui.viewer.zoomLocked }
        onWindowFullscreenToggled: ui.windowFullscreen = !ui.windowFullscreen
        onOsdToggled: { if (ui.viewer) ui.viewer.osdVisible = !ui.viewer.osdVisible }
        onSeeked: function (i) { if (ui.viewer) ui.viewer.cursor = i }
        //   时间轴悬停预览：只在"条目变了"时查一次（数据在 viewer 里算，provider 解码缩略图）
        onPreviewHover: function (index) {
            //   ★ 条目与定位框几何**一次取回**（`{ items, focusX, focusW }`），
            //     避免分几次赋值产生"半新半旧"的中间态（用户 2026-11-03）
            const imgH = Theme.timelinePreviewH - Theme.spaceSm * 2
            bottomBar.previewData = (ui.previewStrip && ui.viewer)
                    ? ui.viewer.previewStrip(index, imgH, bottomBar.width, Theme.spaceSm) : ({})
            bottomBar.syncPreviewShown()      // 缓存已命中（batch 已就绪）⇒ 立即换上
        }
    }

    // ── OSD 信息面板：**只被 OSD 开关控制，不随控件条显隐**（用户 2026-10-11）──
    FullscreenOsd {
        //   铺满可用宽（左右留边）：**OSD 的 bounding box 就是"两列外沿"的基准**
        //   —— 左列贴左、右列贴右（见 FullscreenOsd.qml）
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: Theme.spaceLg
        anchors.rightMargin: Theme.spaceLg
        anchors.topMargin: Theme.barH + Theme.spaceSm
        osdVisible: ui.viewer ? ui.viewer.osdVisible : false
        //   行由 viewer 按需读（`osdRows()`）：文件 / 图像 / EXIF 三组，
        //   EXIF 是**动态的**（不同图片标签不同）⇒ 面板不再有固定字段
    }

    //   预览缩略图缓存变化 ⇒ 按**当前请求**重查一次就绪状态（不靠完成事件推进）
    Connections {
        target: ui.viewer
        ignoreUnknownSignals: true
        function onPreviewCacheChanged() { bottomBar.syncPreviewShown() }
    }

    // ── 窗口全屏：状态在 ViewModel，窗口跟随（照 vsr-player 的双向同步）──
    Connections {
        target: ui
        function onWindowFullscreenChanged() {
            var win = root.Window.window
            if (!win)
                return
            win.visibility = ui.windowFullscreen ? Window.FullScreen : Window.Windowed
        }
    }

    //   ★ dpr 变了也要重推：视图态（zoom / pan）是**设备像素**口径，VM 存的 `m_dpr`
    //     用来换算"指针锚点 / 拖拽位移"。窗口在同一尺寸下从 1x 屏移到 2x 屏时
    //     尺寸不变、只有 dpr 变 —— 不重推的话锚点会差一倍（Qt 只负责渲染侧的 dpr，
    //     我们这条"QML → VM 的视口交接"是应用自己的事）。
    Connections {
        target: root
        function onDprChanged() { root.pushViewport() }
    }
    //   ★ **窗口换屏 / 窗口 dpr 变**：这是"跨 DPI 显示器拖动"的真实信号源
    //     （`root.dpr` 的绑定变化只是它的结果；两者都接，动作幂等）。
    Connections {
        target: root.Window.window
        function onDevicePixelRatioChanged() { root.pushViewport() }
        function onScreenChanged() { root.pushViewport() }
    }

    // ══ 右键菜单（用户 2026-10-31）════════════════════════════════════════════
    //   参考 vsr-player `ContextMenu.qml`（鼠标位置弹出 / 条目数组 / 分组）；
    //   外观与交互一律走我们的 `PopupMenu`（DSH 令牌：menuFill / borderL4 / radiusLg / hoverFill）。
    //   分组 = **打开 / 保存 / 缩放算法 / 设置**（用户给定）。
    //   ★ 条目带 `action`（语义名），**不按行号分发** —— 多一组、少一组都不会错位。
    //   ★ 段 2 / 段 3 才有的动作（收藏夹、外部存档、保存）**先禁用**：后端还没有，
    //     不许"点了什么都不发生"。
    readonly property var contextMenuItems: {
        const algo = ui.viewer ? ui.viewer.scaleAlgorithm : ui.scaleAlgorithm
        const algoName = { nearest: "Nearest", smooth: "Smooth (Qt)", bicubic: "Bicubic",
                           lanczos2: "Lanczos 2", lanczos3: "Lanczos 3" }[algo] || ""
        return [
            { text: qsTr("Open favorites"), icon: Theme.icStarO, action: "open:favorites" },
            { text: qsTr("Open progress file"), action: "open:saved" },
            { text: qsTr("Open folder"), action: "open:folder" },
            { text: qsTr("Open archive"), action: "open:archive" },
            { text: qsTr("Open file"), action: "open:file" },
            { sep: true },
            //   来源级动作：**重新扫描当前来源**（扫完会按最新磁盘内容重建 rows；
            //   已关联进度文件的，之后自动保存会把新 rows 写回去）
            { text: qsTr("Rescan"), action: "rescan", enabled: ui.canRescan },
            { sep: true },
            { text: qsTr("Save progress to favorites"), action: "save:progress", enabled: ui.viewer !== null },
            { text: qsTr("Save progress to file"), action: "save:progress-as", enabled: ui.viewer !== null },
            { sep: true },
            //   Chrome 式的缩放（用户 2026-11-02）：**一行控件** `缩放  [−] 100% [+]`，
            //   点百分比回到 100%；快捷键仍是 Ctrl+= / Ctrl+-。
            { zoomRow: true },
            //   ★ 这一组将来叫「图像处理」（用户 2026-10-31）；现在组里只有缩放算法一项。
            //     二级菜单（悬停展开）—— 5 档全列在主菜单里太长（用户）。
            { text: qsTr("Scale algorithm"), kind: "scale", hint: algoName, currentValue: algo,
              submenu: [
                  { label: "Nearest",     value: "nearest" },
                  { label: "Smooth (Qt)", value: "smooth" },
                  { label: "Bicubic",     value: "bicubic" },
                  { label: "Lanczos 2",   value: "lanczos2" },
                  { label: "Lanczos 3",   value: "lanczos3" }
              ] },
            { sep: true },
            { text: qsTr("Settings"), icon: Theme.icSettings, action: "settings" },
            { sep: true },
            { text: qsTr("Quit"), icon: Theme.icPower, hint: "Ctrl+Q", action: "quit" }
        ]
    }

    function runContextAction(action) {
        if (!action)
            return
        if (action === "open:folder")         ui.chooseOpenFolder()
        else if (action === "open:archive")   ui.chooseOpenArchive()
        else if (action === "open:file")      ui.chooseOpenImage()
        else if (action === "open:favorites") favoritesPopup.open()
        else if (action === "open:saved")     ui.openProgressFile()
        else if (action === "rescan")         ui.rescanSource()
        else if (action === "save:progress")  root.requestSaveToFavorites()
        else if (action === "save:progress-as") root.requestSaveToFile()
        else if (action === "settings")       settingsDialog.open()
        else if (action === "quit")           Qt.quit()
        //   其余动作按需再加
    }

    // 在 `item` 的 (x, y)（= 鼠标位置）弹出，越界钳制在窗口内
    function openContextMenu(item, x, y) { contextMenu.showAt(item, x, y) }

    // 缩放动作（Chrome 式档位）：in = 放大一档、out = 缩小一档
    function applyZoomPreset(v) {
        if (!ui.viewer)
            return
        if (v === "in")       ui.viewer.zoomStep(1)
        else if (v === "out") ui.viewer.zoomStep(-1)
    }

    // 缩放算法：有来源 ⇒ 只改当前阅读态；没有来源 ⇒ 改**全局默认**（打开下一个来源时生效）
    function applyScaleAlgorithm(key) {
        if (!key)
            return
        if (ui.viewer) ui.viewer.scaleAlgorithm = key
        else ui.scaleAlgorithm = key
    }

    // ── 收藏夹（弹窗列表；设计 20 §7）──
    FavoritesPopup {
        id: favoritesPopup
    }

    PopupMenu {
        id: contextMenu
        items: root.contextMenuItems
        //   缩放百分比是**独立绑定**（不进 items —— 否则每缩放一次整张菜单都被重建）
        zoomPercent: ui.viewer ? ui.viewer.zoom : 1.0
        onItemTriggered: function (i) {
            const it = root.contextMenuItems[i]
            root.runContextAction(it ? it.action : "")
        }
        onZoomInRequested: root.applyZoomPreset("in")
        onZoomOutRequested: root.applyZoomPreset("out")
        onZoomResetRequested: if (ui.viewer) ui.viewer.zoomToRatio(1.0)
        onSubItemTriggered: function (row, sub) {
            const it = root.contextMenuItems[row]
            const subIt = (it && it.submenu) ? it.submenu[sub] : null
            if (!subIt)
                return
            root.applyScaleAlgorithm(subIt.value)
        }
    }

    // ── 收藏 / 保存（用户 2026-10-31）──
    //   · 没有来源 ⇒ 不做；
    //   · **扫描中** ⇒ 信息框「扫描还没完」；
    //   · **扫描被取消（不完整）** ⇒ 确认框「扫描不完整，确认保存进度吗？…」；
    //   · 都不是 ⇒ 直接保存。
    property string pendingSaveTarget: ""        // "favorites" / "file"

    function requestSaveToFavorites() {
        if (ui.viewer === null)
            return
        if (ui.scanning) { scanNotDoneDialog.open(); return }
        if (ui.scanIncomplete) { pendingSaveTarget = "favorites"; incompleteSaveDialog.open(); return }
        if (maybeAskPasswords("favorites")) return
        ui.saveProgressToFavorites(false)
    }
    function requestSaveToFile() {
        if (ui.viewer === null)
            return
        if (ui.scanning) { scanNotDoneDialog.open(); return }
        if (ui.scanIncomplete) { pendingSaveTarget = "file"; incompleteSaveDialog.open(); return }
        if (maybeAskPasswords("file")) return
        ui.saveProgressToFile(false)
    }
    function runPendingSave() {
        const target = pendingSaveTarget
        pendingSaveTarget = ""
        if (maybeAskPasswords(target)) return
        if (target === "favorites") ui.saveProgressToFavorites(false)
        else if (target === "file") ui.saveProgressToFile(false)
    }

    // ── 加密压缩包：保存进度时问一次"要不要把密码存进进度文件"（用户 2026-11-03）──
    //   只有**本次会话新输入、还没保存过**的密码才问（`ui.hasUnsavedPasswords`）。
    property string pendingPasswordSaveTarget: ""
    function maybeAskPasswords(target) {
        if (!ui.hasUnsavedPasswords)
            return false
        pendingPasswordSaveTarget = target
        savePasswordDialog.open()
        return true
    }
    function toggleFavorite() {
        if (ui.viewer === null)
            return
        if (ui.sourceFavorited) unfavoriteDialog.open()
        else requestSaveToFavorites()
    }

    //   ── 保存进度时问"要不要保存密码"──
    ConfirmDialog {
        id: savePasswordDialog
        title: qsTr("Save password?")
        message: qsTr("Save the archive password in this progress file? It is stored as plain text.")
        acceptText: qsTr("Save")
        rejectText: qsTr("Don't save")
        onAccepted: {
            if (pendingPasswordSaveTarget === "favorites") ui.saveProgressToFavorites(true)
            else if (pendingPasswordSaveTarget === "file") ui.saveProgressToFile(true)
            pendingPasswordSaveTarget = ""
        }
        onRejected: {
            if (pendingPasswordSaveTarget === "favorites") ui.saveProgressToFavorites(false)
            else if (pendingPasswordSaveTarget === "file") ui.saveProgressToFile(false)
            pendingPasswordSaveTarget = ""
        }
    }

    ConfirmDialog {
        id: scanNotDoneDialog
        title: qsTr("Scan not finished")
        message: qsTr("Wait for the scan to finish before adding to favorites.")
        acceptText: qsTr("Got it")
        showReject: false
    }
    ConfirmDialog {
        id: incompleteSaveDialog
        title: qsTr("Scan incomplete")
        message: qsTr("Scanning incomplete. Save progress anyway? Files that were not scanned will not be found again; finish the scan before saving.")
        acceptText: qsTr("Save")
        rejectText: qsTr("Cancel")
        onAccepted: root.runPendingSave()
    }
    ConfirmDialog {
        id: cancelScanDialog
        title: qsTr("Cancel scan")
        message: qsTr("Stop this scan? What has been scanned so far is still browsable.")
        acceptText: qsTr("Cancel scan")
        rejectText: qsTr("Continue scanning")
        onAccepted: ui.cancelScan()
    }
    ConfirmDialog {
        id: unfavoriteDialog
        title: qsTr("Remove favorite")
        message: qsTr("Delete this progress record? The source files are not deleted.")
        acceptText: qsTr("Remove favorite")
        danger: true
        onAccepted: ui.removeFavorite()
    }

    // ── 键盘（全屏是快捷键面）──
    //   ★ 方向键**不拦截自动重复**：Qt 的按键自动重复正常打进来，每个重复按键自然就是一次
    //     `stepPage(∓1)`。
    //   ★ `↑ / ↓` = 上一个 / 下一个文件夹（设计 08 §三）。**按禁用状态走**：判定条件与底栏
    //     那两个按钮**同一个**（名字为空 = 没有那个文件夹 ⇒ 按钮置灰）。所以：
    //       · 到头（不循环）⇒ 名字空 ⇒ 快捷键也不动；
    //       · **文件夹内循环** ⇒ 两个名字一律空 ⇒ 快捷键同样不许切文件夹；
    //       · 这里**不要求** `overlaysVisible` —— 那是控件条的显隐，不是"能不能切"。
    //   ★ 2026-10-31：只剩这一个界面 ⇒ 焦点恒在本视图（没有别的屏可切）
    focus: true
    Keys.onPressed: function (event) {
        root.wakeUi()
        switch (event.key) {
        //   ★ `Esc` = **退出全屏**（播放中也一样）；暂停**只由 `Space`** 负责
        //     （返回键已改为 `Backspace`；全屏切换的最终口径未定，这里先用 `Esc` 临时承担）
        case Qt.Key_Escape:      ui.windowFullscreen = false
                                 event.accepted = true; break
        case Qt.Key_Left:        if (ui.viewer) ui.viewer.stepPage(-1)
                                 event.accepted = true; break
        case Qt.Key_Right:       if (ui.viewer) ui.viewer.stepPage(1)
                                 event.accepted = true; break
        case Qt.Key_Up:          if (ui.viewer && ui.viewer.prevFolderName !== "")
                                     ui.viewer.stepFolder(-1)
                                 event.accepted = true; break
        case Qt.Key_Down:        if (ui.viewer && ui.viewer.nextFolderName !== "")
                                     ui.viewer.stepFolder(1)
                                 event.accepted = true; break
        case Qt.Key_PageUp:      if (ui.viewer) ui.viewer.stepPage(-1)
                                 event.accepted = true; break
        case Qt.Key_PageDown:    if (ui.viewer) ui.viewer.stepPage(1)
                                 event.accepted = true; break
        // ── 用户已确认的那几个（设计 08 §三）──
        case Qt.Key_Space:       if (ui.viewer) ui.viewer.togglePlaying()
                                 event.accepted = true; break
        case Qt.Key_BracketLeft: ui.slideshowIntervalMs = ui.slideshowIntervalMs - 10
                                 event.accepted = true; break
        case Qt.Key_BracketRight: ui.slideshowIntervalMs = ui.slideshowIntervalMs + 10
                                 event.accepted = true; break
        case Qt.Key_I:           if (ui.viewer) ui.viewer.osdVisible = !ui.viewer.osdVisible;
                                 event.accepted = true; break
        // 缩放（Chrome 式档位）：**裸 + / −**，不要求 Shift 或 Ctrl（用户 2026-11-02）
        //   `=` 与 `+` 同键（美式键盘加号要 Shift）⇒ 两个 key 都收。
        case Qt.Key_Plus:
        case Qt.Key_Equal:       root.applyZoomPreset("in")
                                 event.accepted = true; break
        case Qt.Key_Minus:       root.applyZoomPreset("out")
                                 event.accepted = true; break
        }
    }
}
