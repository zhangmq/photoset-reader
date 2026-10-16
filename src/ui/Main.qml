import QtQuick
import QtQuick.Controls
import ImageBrowser

// ══ 应用入口 ══════════════════════════════════════════════════
//   图集库 = 应用的第一屏（用户 2026-10-11 的界面名：图集库 / 文件浏览 / 图像查看）
//   ⚠️ 旧入口（`src/qml/Main.qml`）已于 2026-10-11 弃用，移入 `.archive/`，不再参与构建
//   ⚠️ 根对象必须是 Window：否则 QQmlApplicationEngine 拿不到窗口（显示与截图都会失败，
//      实测报 "[shot] 根对象不是 QQuickWindow"）
ApplicationWindow {
    width: 1500
    height: 950
    visible: true
    color: Theme.bgBase
    title: "photoset-reader"

    // ★ 三屏**同时存在**、`visible` 切换、**不销毁**（设计 11 §二：视图瞬态天然保留）
    //   屏的语义状态只有一份：`ui.screen`（枚举值读 `UiModel.*`，不在界面写 0/1/2）。
    //   ⚠️ QML **读不到"实例上的枚举"**（实测 `ui.Library` = undefined，见报告）：
    //     枚举名必须经**已注册的类型名**访问 ⇒ `main.cpp` 里注册了不可创建的 `UiModel`
    //     （只用于枚举；实例仍经上下文属性 `ui` 暴露）。
    LibraryView {
        anchors.fill: parent
        visible: ui.screen === UiModel.Library
        // ── 调试（dev CLI `--debug-settings`；值 = main.cpp 在 **QML 加载前**注入的上下文属性）──
        //   `devOpenSettings` 默认 false / `devSettingsTab` 默认 0 ⇒ **正常运行时零行为变化**。
        //   每屏只在自己的 `Component.onCompleted` 开对话框 ⇒ 用 `ui.screen` 限定**当前屏**，
        //   免得三屏各弹一个（隐藏屏的模态浮层也会画在窗口上）。
        debugOpenSettings: devOpenSettings && ui.screen === UiModel.Library
        debugSettingsTab: devSettingsTab

        // ── 添加图集 / 打开▾（对话框在 ui；QML 不写平台相关代码）──
        onAddCollectionRequested: ui.chooseAddCollection()
        onFolderRequested: ui.chooseOpenFolder()
        onArchiveRequested: ui.chooseOpenArchive()
        onImageRequested: ui.chooseOpenImage()
        // 卡片点击 → 打开集合（卡片只给行号，集合 id 由 ui 解析）
        onCollectionActivated: (index) => ui.openCollection(ui.collectionIdAt(index))
        onCollectionRemoveRequested: (index) => ui.removeCollection(index)
    }

    BrowseView {
        anchors.fill: parent
        visible: ui.screen === UiModel.Browse
        debugOpenSettings: devOpenSettings && ui.screen === UiModel.Browse
        debugSettingsTab: devSettingsTab

        onLibraryRequested: ui.goLibrary()                    // [返回图集库]
        // 单击 = 选中：图行 ⇒ 移动那个唯一的游标；**容器行不在这里进入**
        //   （⚠️ `MouseArea` 的双击会**先发 clicked 再发 doubleClicked** —— 若单击就进容器，
        //     随后那次 doubleClicked 会拿同一个行号在**新的一层**上再进一次 ⇒ 进错层。
        //     容器只在双击时进入，见下。）
        onItemActivated: function (index) {
            if (ui.browse && !ui.browse.isContainerRow(index)) ui.browse.activate(index)
        }
        // 双击 = 进入：容器（文件夹 / 压缩包 / 多页）→ 进入并留在浏览屏；
        //                图 → 切「图像查看」并跳到它
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
        // 键盘面：只有**当前可见**的一屏取焦点（三屏共存时不让隐藏屏抢走焦点）
        focus: ui.screen === UiModel.Viewer
        debugOpenSettings: devOpenSettings && ui.screen === UiModel.Viewer
        debugSettingsTab: devSettingsTab

        onLibraryRequested: ui.goLibrary()                    // [home]
        onBackRequested: ui.back()                            // [返回] → 文件浏览
    }

    // ★ 变更同步 ①：**窗口重新获得焦点**时立刻查一次 `data_version`
    //   （设计 10 §二；② 是 `LibraryModel` 自己的 1 s 定时轮询）。**只同步列表** ——
    //   别的实例扫出来的新内容**不会**灌进正在查看的内存序列（要重新打开图集才生效）。
    onActiveChanged: if (active && ui.library) ui.library.pollNow()

    // ★ 瞬时提示（浮层）：**不占位**，锚在窗口底部居中 —— 显隐不改变任何布局。
    //   挂在这里（三屏之上），任何屏都能看到；文案/触发序号都来自 `ui`
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
        if (ui.notification !== "") toast.show(ui.notification, ui.notificationSeq)
    }
}
