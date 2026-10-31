import QtQuick
import QtQuick.Controls
import AppUi

// ══ 应用入口 ══════════════════════════════════════════════════
//   ★ 2026-10-25：**两屏**（文件浏览 / 图像查看）+ **图集库抽屉**（左侧 overlay）——
//     图集库**不再是屏**、不进屏栈（设计 18 §1 / §2）。唯一环路：
//         Viewer --「浏览」--> Browse --「继续阅读」--> Viewer
//     没有来源 = Browse + 空态块（抽屉**不自动展开**）。
//   ⚠️ 根对象必须是 Window：否则 QQmlApplicationEngine 拿不到窗口（显示与截图都会失败，
//      实测报 "[shot] 根对象不是 QQuickWindow"）
ApplicationWindow {
    width: 1500
    height: 950
    visible: true
    color: Theme.bgBase
    title: "photoset-reader"

    // ★ 两屏**同时存在**、`visible` 切换、**不销毁**（设计 11 §二：视图瞬态天然保留）
    //   屏的语义状态只有一份：`ui.screen`（枚举值读 `UiModel.*`，不在界面写 0/1/2）。
    //   ⚠️ QML **读不到"实例上的枚举"**（实测 `ui.Browse` = undefined，见报告）：
    //     枚举名必须经**已注册的类型名**访问 ⇒ `main.cpp` 里注册了不可创建的 `UiModel`
    //     （只用于枚举；实例仍经上下文属性 `ui` 暴露）。
    //   ★ 键盘面：两屏各自在组件里绑了 `focus:`（并且**抽屉打开时让位**）——
    //     这里**不要**再绑 `focus`（实例级绑定会覆盖组件里的那一份，见 07 §五）。
    BrowseView {
        anchors.fill: parent
        visible: ui.screen === UiModel.Browse
        // ── 调试（dev CLI `--debug-settings`；值 = main.cpp 在 **QML 加载前**注入的上下文属性）──
        //   `devOpenSettings` 默认 false / `devSettingsTab` 默认 0 ⇒ **正常运行时零行为变化**。
        //   每屏只在自己的 `Component.onCompleted` 开对话框 ⇒ 用 `ui.screen` 限定**当前屏**，
        //   免得两屏各弹一个（隐藏屏的模态浮层也会画在窗口上）。
        debugOpenSettings: devOpenSettings && ui.screen === UiModel.Browse
        debugSettingsTab: devSettingsTab

        // ── 打开 ▾ / 空态块（对话框在 ui；QML 不写平台相关代码）──
        //   ★ 前三个 = 会话来源，`ui` 会**直达 Viewer**（等价 CLI `-d`，设计 18 §3/§4）
        onLibraryRequested: ui.libraryOpen = true            // [图集库] /「打开 ▾ → 收藏夹」
        onFolderRequested: ui.chooseOpenFolder()
        onArchiveRequested: ui.chooseOpenArchive()
        onImageRequested: ui.chooseOpenImage()
        // 单击 = **只选中**（任何行都一样）：不动进度、不解码、不进入（用户 2026-10-24 定案）
        onItemActivated: function (index) {
            if (ui.browse) ui.browse.selectRow(index)
        }
        // 双击 = 进入：容器（文件夹 / 压缩包 / 多页）→ 进入那一层，**不改阅读状态**；
            //                图 → **设置当前进度**并切「图像查看」
        onItemOpened: function (index) {
            if (!ui.browse) return
            if (ui.browse.isContainerRow(index)) ui.browse.activate(index)   // 进入容器（留在浏览屏）
            else                                 ui.browse.open(index)       // 打开那条图（⇒ 查看屏）
        }
        onUpRequested: { if (ui.browse) ui.browse.up() }              // [上一级]
        onCrumbClicked: (index) => { if (ui.browse) ui.browse.crumb(index) }
    }

    FullscreenView {
        anchors.fill: parent
        visible: ui.screen === UiModel.Viewer
        debugOpenSettings: devOpenSettings && ui.screen === UiModel.Viewer
        debugSettingsTab: devSettingsTab
        debugOpenIntervalPopup: devShowIntervalPopup && ui.screen === UiModel.Viewer

        onBrowseRequested: ui.goBrowse()                     // 「浏览」→ 文件浏览屏（Backspace 同效）
        onLibraryRequested: ui.libraryOpen = true            // [图集库] → 抽屉
        //   ★「收藏」（★）的规则见设计 18 §7 —— 段 3 接；本段按钮只占位
    }

    // ══ 图集库抽屉（左侧 overlay；**不是屏**、**不参与 Layout**）════════════════
    //   打开/关闭只动抽屉自己（`x` 滑动），两屏的尺寸与布局**一点都不变**。
    //   关闭方式：点外部（下面这层遮罩）/ `Esc`（抽屉自己处理）/ 再点按钮 /「关闭」。
    Item {
        id: drawerLayer
        anchors.fill: parent

        // 点外部关闭：遮罩**只在打开时**存在（关着时完全让开，不挡两屏的点击）。
        //   ★ 刻意不加半透明底（mask）：图集库是工作抽屉，不是模态对话框；
        //     也避免在窄屏上把后面的画面压暗（与设置对话框"不掉暗背景"同一理由）。
        MouseArea {
            anchors.fill: parent
            visible: ui.libraryOpen
            enabled: ui.libraryOpen
            onClicked: ui.libraryOpen = false
        }

        LibraryDrawer {
            id: drawer
            width: Theme.drawerW
            height: parent.height
            // 关着 = 停在窗口外（滑动）；打开 = 贴左
            x: ui.libraryOpen ? 0 : -width
            Behavior on x { NumberAnimation { duration: Theme.durMed; easing.type: Easing.OutCubic } }

            onFolderRequested: ui.chooseOpenFolder()
            // 卡片点击 → 打开集合（卡片只给行号，集合 id 由 ui 解析）；
            //   `ui.openCollection` 会**收起抽屉**并**直达 Viewer**、定位到该图集 resume
            onCollectionActivated: (index) => ui.openCollection(ui.collectionIdAt(index))
            onCollectionRemoveRequested: (index) => ui.removeCollection(index)
        }
    }

    // ★ `Ctrl+B`：开合图集库抽屉（全仓无冲突，设计 18 §2）
    Shortcut {
        sequence: "Ctrl+B"
        onActivated: ui.libraryOpen = !ui.libraryOpen
    }

    // ★ 变更同步 ①：**窗口重新获得焦点**时立刻查一次 `data_version`
    //   （设计 10 §二；② 是 `LibraryModel` 自己的 1 s 定时轮询）。**只同步列表** ——
    //   别的实例扫出来的新内容**不会**灌进正在查看的内存序列（要重新打开图集才生效）。
    //   （段 4 收窄跨进程：这两条会一起去掉，改由"打开抽屉时取快照 + 手动刷新"）
    onActiveChanged: if (active && ui.library) ui.library.pollNow()

    // ★ 瞬时提示（浮层）：**不占位**，锚在窗口底部居中 —— 显隐不改变任何布局。
    //   挂在这里（两屏之上），任何屏都能看到；文案/触发序号都来自 `ui`
    //   （失败与不支持一律走它，**不写入磁盘**）。
    Toast {
        id: toast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.spaceLg
        y: anchors.bottomMargin + (visible ? 0 : Theme.spaceSm)   // 出现时轻微上浮
        opacity: 0
    }
    Connections {
        target: ui
        function onNotificationChanged() {
            toast.show(ui.notification, ui.notificationSeq)
        }
    }
    Component.onCompleted: {
        // 开发自检：`--debug-library-open`（截图抽屉用；默认 false ⇒ 正常运行时零行为变化）
        if (devLibraryOpen)
            ui.libraryOpen = true
        if (ui.notification !== "")
            toast.show(ui.notification, ui.notificationSeq)
    }
}
