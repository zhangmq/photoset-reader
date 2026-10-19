import QtQuick
import ImageBrowser

// ══ 全屏（沉浸）视图 ══════════════════════════════════════════
//   设计 §3.1：**图片用整个窗口**，所有控件是**覆盖其上的浮层**；
//   浮层显隐不改变画面尺寸、不引起重排（overlay 架构，不做任何占位布局）。
//   · 画布与浮层**跟随主题**（用户 2026-10-11 定案；浅色模式 = 浅底 + 深字）
//   · 自动隐藏**照抄 vsr-player，不用计时器**：
//       `showUi = 顶栏热区 || 底栏热区 || 弹层打开`，状态写回 ViewModel
//     （vsr-player main.qml 原话："移出热区立即隐藏，无计时器"）
//   · 状态单一来源：`ui.overlaysVisible` / `ui.*` 全部走 ViewModel，QML 不存第二份
Item {
    id: root

    property string debugImage: ""          // ⚠️ 已不用（画面换成 GPU 项后不吃路径）——旧调试机架遗留
    property bool   debugForceUi: false     // 调试：强制显示浮层（离屏没有鼠标热区）
    property bool   debugOpenSettings: false   // 调试：截图时自动打开设置
    property int    debugSettingsTab: 0        // 调试：设置弹框开在哪一页（0常规/1图像/2系统/3快捷键）
    signal libraryRequested()   // home → 图集库
    signal backRequested()      // 返回 → 浏览
    signal settingsRequested()  // 设置
    onSettingsRequested: settingsDialog.open()   // [设置] 按钮 → 打开设置对话框

    // 本屏是不是**双幅**（两个位置 → 并排两张图）；`keyCount` = 本屏位置数（1 或 2）。
    //   ★ 坏的位置**留空位**：`keyCount` 仍是 2（两格都占着），那一格只是没图。
    readonly property bool twoUp: ui.viewer ? ui.viewer.keyCount > 1 : false

    // ── 浮层显隐：纯函数（照抄 vsr-player main.qml）──
    readonly property bool showUi: root.debugForceUi
                                   || topBar.mouseInRegion || bottomBar.mouseInRegion
                                   || bottomBar.popupOpen
    onShowUiChanged: ui.overlaysVisible = showUi
    Component.onCompleted: {
        ui.overlaysVisible = showUi          // 启动即按当前状态应用
        if (root.debugOpenSettings) {
            settingsDialog.tab = root.debugSettingsTab
            settingsDialog.open()
        }
    }

    // ── 设置对话框（三屏同一实现；当前阶段单纯 UI）──
    SettingsDialog {
        id: settingsDialog
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

        // ── 空库屏守卫（2026-10-14）───────────────────────────────────────────
        //   `ui.viewer` 为空时（图集库屏 / 还没打开任何来源）**不建** GpuScalerItem：
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
                        viewport: Qt.size(canvas.boxW, canvas.height)
                        sourceImage: ui.viewer ? ui.viewer.imageA : null
                        fit: ui.fitMode              // 0 原始 / 1 适应屏幕 / 2 适应宽度 / 3 适应高度
                        zoom: ui.zoom
                        panX: ui.panX
                        panY: ui.panY
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
                        viewport: Qt.size(canvas.boxW, canvas.height)
                        sourceImage: ui.viewer ? ui.viewer.imageB : null
                        fit: ui.fitMode
                        zoom: ui.zoom
                        panX: ui.panX
                        panY: ui.panY
                    }
                }
            }
        }

        // 交互缩放（滚轮）与拖拽平移（设计 08 §1.3 / 01 §3.8）
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
            property real lastX: 0
            property real lastY: 0
            onPressed: (m) => { lastX = m.x; lastY = m.y }
            onPositionChanged: (m) => {
                if (!pressed) return
                ui.panX += m.x - lastX
                ui.panY += m.y - lastY
                lastX = m.x
                lastY = m.y
            }
            onWheel: (w) => {
                const k = w.angleDelta.y > 0 ? 1.1 : (1 / 1.1)
                ui.zoom = Math.max(0.1, Math.min(16, ui.zoom * k))
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

            Item {
                id: spinner
                width: 32
                height: 32
                anchors.horizontalCenter: parent.horizontalCenter

                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color: "transparent"
                    border.width: 3
                    border.color: Theme.borderL4
                }
                Rectangle {
                    width: 3
                    height: 11
                    radius: 1.5
                    color: Theme.textPrimary
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.topMargin: 1
                }
                RotationAnimator on rotation {
                    from: 0
                    to: 360
                    duration: 900
                    loops: Animation.Infinite
                    running: loadingLayer.visible
                }
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("载入中…")
                color: Theme.textSecondary
                font.pointSize: Theme.fontSm
            }
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
        overlaysVisible: ui.overlaysVisible
        onLibraryRequested: root.libraryRequested()
        onBackRequested: root.backRequested()
        onSettingsRequested: root.settingsRequested()
        progressText: ui.viewer ? ((ui.viewer.currentPage + 1) + " / " + ui.viewer.totalPages) : ""
        relativePath: ""
    }

    // ── 底部类播放器控制条 ──
    FullscreenBottomBar {
        id: bottomBar
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        index: ui.viewer ? ui.viewer.cursor : 0
        count: ui.viewer ? ui.viewer.count : 0
        fitMode: ui.fitMode
        doublePage: ui.viewer ? ui.viewer.doublePage : false
        loopMode: ui.viewer ? ui.viewer.loopMode : 0
        playing: ui.viewer ? ui.viewer.playing : false
        intervalMs: ui.slideshowIntervalMs
        prevFolderName: ui.viewer ? ui.viewer.prevFolderName : ""
        nextFolderName: ui.viewer ? ui.viewer.nextFolderName : ""
        zoomLocked: ui.zoomLocked
        windowFullscreen: ui.windowFullscreen
        osdVisible: ui.osdVisible
        overlaysVisible: ui.overlaysVisible
        onPrevRequested: { if (ui.viewer) ui.viewer.stepPage(-1) }
        onNextRequested: { if (ui.viewer) ui.viewer.stepPage(1) }
        onFitModeSelected: function (m) { ui.fitMode = m }
        onDoublePageSelected: function (on) { if (ui.viewer) ui.viewer.doublePage = on }
        onLoopModeSelected: function (m) { if (ui.viewer) ui.viewer.loopMode = m }
        onPlayToggled: { if (ui.viewer) ui.viewer.togglePlaying() }
        onIntervalSelected: function (ms) { ui.slideshowIntervalMs = ms }
        onIntervalStep: function (d) { ui.slideshowIntervalMs = ui.slideshowIntervalMs + d }
        onPrevFolderRequested: { if (ui.viewer) ui.viewer.stepFolder(-1) }
        onNextFolderRequested: { if (ui.viewer) ui.viewer.stepFolder(1) }
        onZoomLockToggled: ui.zoomLocked = !ui.zoomLocked
        onWindowFullscreenToggled: ui.windowFullscreen = !ui.windowFullscreen
        onOsdToggled: ui.toggleOsd()
        onSeeked: function (i) { if (ui.viewer) ui.viewer.cursor = i }
    }

    // ── OSD 信息面板：**只被 OSD 开关控制，不随控件条显隐**（用户 2026-10-11）──
    FullscreenOsd {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: Theme.spaceLg
        anchors.topMargin: Theme.barH + Theme.spaceSm
        osdVisible: ui.osdVisible
        positionLabel: ui.viewer ? ((ui.viewer.currentPage + 1) + " / " + ui.viewer.totalPages) : ""
        fileLabel: ""
        pathLabel: ""
        folderLabel: ""
        resolutionLabel: ""      // 分辨率 = 像素尺寸（尺寸=文件字节大小，待接）
        kindLabel: ""
        // 图集 / 分辨率 / EXIF：数据未接入 → 面板显示 "—"（需求见设计 §3.9）
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

    // ── 键盘（全屏是快捷键面）──
    //   ★ 方向键**不拦截自动重复**：Qt 的按键自动重复正常打进来，每个重复按键自然就是一次
    //     `stepPage(∓1)`。
    //   ★ `↑ / ↓` = 上一个 / 下一个文件夹（设计 08 §三）。**按禁用状态走**：判定条件与底栏
    //     那两个按钮**同一个**（名字为空 = 没有那个文件夹 ⇒ 按钮置灰）。所以：
    //       · 到头（不循环）⇒ 名字空 ⇒ 快捷键也不动；
    //       · **文件夹内循环** ⇒ 两个名字一律空 ⇒ 快捷键同样不许切文件夹；
    //       · 这里**不要求** `overlaysVisible` —— 那是控件条的显隐，不是"能不能切"。
    focus: true
    Keys.onPressed: function (event) {
        switch (event.key) {
        case Qt.Key_Escape:      root.backRequested();       event.accepted = true; break
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
        case Qt.Key_I:           ui.toggleOsd();             event.accepted = true; break
        case Qt.Key_Plus:
        case Qt.Key_Equal:       ui.zoom = Math.min(16, ui.zoom * 1.1)
                                 event.accepted = true; break
        case Qt.Key_Minus:       ui.zoom = Math.max(0.1, ui.zoom / 1.1)
                                 event.accepted = true; break
        }
    }
}
