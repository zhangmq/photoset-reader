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

    property string debugImage: ""          // 调试：直接给图片路径（真机由 ViewModel 供图）
    property bool   debugForceUi: false     // 调试：强制显示浮层（离屏没有鼠标热区）
    property bool   debugOpenSettings: false   // 调试：截图时自动打开设置
    signal libraryRequested()   // home → 图集库
    signal backRequested()      // 返回 → 浏览
    signal settingsRequested()  // 设置
    onSettingsRequested: settingsDialog.open()   // [设置] 按钮 → 打开设置对话框

    // ── 浮层显隐：纯函数（照抄 vsr-player main.qml）──
    readonly property bool showUi: root.debugForceUi
                                   || topBar.mouseInRegion || bottomBar.mouseInRegion
                                   || bottomBar.popupOpen
    onShowUiChanged: ui.overlaysVisible = showUi
    Component.onCompleted: {
        ui.overlaysVisible = showUi          // 启动即按当前状态应用
        if (root.debugOpenSettings) settingsDialog.open()
    }

    // ── 设置对话框（三屏同一实现；当前阶段单纯 UI）──
    SettingsDialog {
        id: settingsDialog
    }
    Rectangle {
        anchors.fill: parent
        color: Theme.bgBase                 // 画布底（跟随主题；用 DSH alias 令牌）
    }

    // ── 图片：占满整个窗口（浮层不挤压它）──
    Image {
        id: img
        anchors.centerIn: parent
        source: root.debugImage
        asynchronous: true
        smooth: true
        mipmap: true
        // 画面缩放模式（用户 2026-10-11）：0=原始大小 1=适应屏幕 2=适应宽度 3=适应高度
        //   0/2/3 显式算宽高（保持比例），1 交给 PreserveAspectFit
        readonly property int mode: ui.fitMode
        readonly property real sw: Math.max(1, sourceSize.width)
        readonly property real sh: Math.max(1, sourceSize.height)
        width: mode === 0 ? sw
             : mode === 2 ? parent.width
             : mode === 3 ? sw * (parent.height / sh)
                          : parent.width
        height: mode === 0 ? sh
              : mode === 2 ? sh * (parent.width / sw)
              : mode === 3 ? parent.height
                           : parent.height
        fillMode: mode === 1 ? Image.PreserveAspectFit : Image.Stretch
    }

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
        progressText: (ui.currentPage + 1) + " / " + ui.totalPages
        relativePath: ui.relativePath
    }

    // ── 底部类播放器控制条 ──
    FullscreenBottomBar {
        id: bottomBar
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        index: ui.cursor
        count: ui.count
        playing: ui.slideshow
        loopMode: ui.loopMode
        intervalMs: ui.intervalMs
        fitMode: ui.fitMode
        doublePage: ui.doublePage
        zoomLocked: ui.zoomLocked
        windowFullscreen: ui.windowFullscreen
        osdVisible: ui.osdVisible
        prevFolderName: ui.prevFolderLabel
        nextFolderName: ui.nextFolderLabel
        overlaysVisible: ui.overlaysVisible
        onPrevRequested: ui.stepPage(-1)
        onNextRequested: ui.stepPage(1)
        onPlayToggled: ui.toggleSlideshow()
        onPrevFolderRequested: ui.prevArchive()
        onNextFolderRequested: ui.nextArchive()
        onLoopModeSelected: function (m) { ui.loopMode = m }
        onIntervalSelected: function (ms) { ui.intervalMs = ms }
        onIntervalStep: function (d) { ui.intervalBy(d) }
        onFitModeSelected: function (m) { ui.fitMode = m }
        onDoublePageSelected: function (on) { ui.doublePage = on }
        onZoomLockToggled: ui.zoomLocked = !ui.zoomLocked
        onWindowFullscreenToggled: ui.windowFullscreen = !ui.windowFullscreen
        onOsdToggled: ui.toggleOsd()
        onSeeked: function (i) { ui.cursor = i }
    }

    // ── OSD 信息面板：**只被 OSD 开关控制，不随控件条显隐**（用户 2026-10-11）──
    FullscreenOsd {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: Theme.spaceLg
        anchors.topMargin: Theme.barH + Theme.spaceSm
        osdVisible: ui.osdVisible
        positionLabel: (ui.currentPage + 1) + " / " + ui.totalPages
        fileLabel: ui.fileLabel
        pathLabel: ui.relativePath
        folderLabel: ui.folderLabelAt(ui.cursor)
        resolutionLabel: ui.sizeLabel      // 分辨率 = 像素尺寸（尺寸=文件字节大小，待接）
        kindLabel: ui.pageKindLabel
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
    focus: true
    Keys.onPressed: function (event) {
        switch (event.key) {
        case Qt.Key_Escape:      root.backRequested();       event.accepted = true; break
        case Qt.Key_Left:        ui.stepPage(-1);            event.accepted = true; break
        case Qt.Key_Right:       ui.stepPage(1);             event.accepted = true; break
        case Qt.Key_PageUp:      ui.stepPage(-1);            event.accepted = true; break
        case Qt.Key_PageDown:    ui.stepPage(1);             event.accepted = true; break
        case Qt.Key_Home:        ui.firstPage();             event.accepted = true; break
        case Qt.Key_End:         ui.lastPage();              event.accepted = true; break
        case Qt.Key_Space:       ui.toggleSlideshow();       event.accepted = true; break
        case Qt.Key_Up:          ui.prevArchive();           event.accepted = true; break
        case Qt.Key_Down:        ui.nextArchive();           event.accepted = true; break
        }
    }
}
