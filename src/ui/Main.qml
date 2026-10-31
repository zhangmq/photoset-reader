import QtQuick
import QtQuick.Controls
import AppUi

// ══ 应用入口（**只有一个查看界面**）══════════════════════════════════════════
//   2026-10-31 定案（用户）：砍掉「浏览」屏与「图集库」抽屉 —— 没有 start screen，
//   没有空态块（无文字、无按钮）。没有来源时窗口就是空白，只保留两件事：
//     · **整窗接受文件拖放**（文件夹 / 压缩包 / 文件）⇒ `ui.openPath()`；
//     · 「打开」相关与不常用的功能**以后进右键菜单**（这一批不做）。
//   ⚠️ 根对象必须是 Window：否则 QQmlApplicationEngine 拿不到窗口（显示与截图都会失败，
//      实测报 "[shot] 根对象不是 QQuickWindow"）
ApplicationWindow {
    width: 1500
    height: 950
    visible: true
    color: Theme.bgBase
    title: "photoset-reader"

    // ★ 唯一的界面：查看。它自己管浮层显隐 / 键盘 / 设置对话框；
    //   `focus` 绑定在组件里（这里**不要**再绑 —— 实例级绑定会覆盖组件里的那一份，见 07 §五）。
    FullscreenView {
        id: viewerView
        anchors.fill: parent
        // ── 调试（dev CLI `--debug-settings`；值 = main.cpp 在 **QML 加载前**注入的上下文属性）──
        //   `devOpenSettings` 默认 false / `devSettingsTab` 默认 0 ⇒ **正常运行时零行为变化**。
        debugOpenSettings: devOpenSettings
        debugSettingsTab: devSettingsTab
        debugOpenIntervalPopup: devShowIntervalPopup
        debugOpenContextMenu: devContextMenu      // 开发自检：启动即打开右键菜单（截图用）
        debugDialog: devDialog                    // 开发自检：启动即打开某个对话框（截图用）

        // ★「收藏」（★）的规则见设计 18 §7 —— 段 2 接「保存阅读进度」；本段按钮只占位
    }

    // ★ 没有来源时**整窗接受拖放**（有来源时不接收：那是画布的手势区）。
    //   只取第一个 URL（多个 = 以第一个为准），非本地 URL 由 `ui.openPath` 忽略。
    DropArea {
        anchors.fill: parent
        enabled: ui.viewer === null
        keys: ["text/uri-list"]
        onDropped: (drop) => {
            if (drop.urls.length > 0) ui.openPath(drop.urls[0])
        }
    }

    // ★ 瞬时提示（浮层）：**不占位**，锚在窗口底部居中 —— 显隐不改变任何布局。
    //   文案/触发序号都来自 `ui`（失败与不支持一律走它，**不写入磁盘**）。
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
        if (ui.notification !== "")
            toast.show(ui.notification, ui.notificationSeq)
    }
}
