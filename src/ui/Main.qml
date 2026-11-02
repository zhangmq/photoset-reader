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

    // ★ 没有来源时的占位（用户 2026-11-02）：一个虚线拖放框 + 两行说明。
    //   它是**纯展示**（内部没有 MouseArea），所以不会挡住下面 DropArea 的拖放。
    Item {
        id: emptyState
        anchors.fill: parent
        visible: ui.viewer === null

        Canvas {                       // 虚线圆角框（拖放区）
            id: dropZone
            anchors.centerIn: parent
            width: Math.min(parent.width - 160, 560)
            height: 300
            //   描边色**绑定成属性**：换主题（浅 ↔ 深）时颜色会变，Canvas 必须跟着重画
            //   （Canvas 不会因为 strokeStyle 的来源变了就自己重绘）。
            property color strokeColor: Theme.borderL5
            //   ⚠️ Canvas 的第一次 paint 可能发生在拿到尺寸之前 ⇒ 显式请求一次；
            //      尺寸/主题变了也重画（否则只画一次、还是空画布）。
            Component.onCompleted: requestPaint()
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onStrokeColorChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.strokeStyle = dropZone.strokeColor   // 虚线要当图形看 ⇒ 比浮层描边亮一档
                ctx.lineWidth = 1
                ctx.setLineDash([6, 6])
                //   Canvas 2D 没有 roundedRect ⇒ 自己拼四条边 + 四个圆角
                const w = width - 1, h = height - 1, r = Theme.radiusLg
                const HALF_PI = Math.PI / 2
                ctx.beginPath()
                ctx.moveTo(r + 0.5, 0.5)
                ctx.lineTo(w - r, 0.5)
                ctx.arc(w - r, r, r, -HALF_PI, 0)
                ctx.lineTo(w, h - r)
                ctx.arc(w - r, h - r, r, 0, HALF_PI)
                ctx.lineTo(r + 0.5, h)
                ctx.arc(r, h - r, r, HALF_PI, Math.PI)
                ctx.lineTo(0.5, r)
                ctx.arc(r, r, r, Math.PI, Math.PI + HALF_PI)
                ctx.closePath()
                ctx.stroke()
            }
        }
        Column {
            anchors.centerIn: parent
            spacing: Theme.spaceMd
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: Theme.icFolder
                font.family: iconFont
                font.pixelSize: 56
                color: Theme.textSecondary
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("把文件夹、压缩包或图片拖到这里")
                color: Theme.textPrimary
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("也可以右键 → 打开…")
                color: Theme.textCaption
                font.pointSize: Theme.fontSm
            }
        }
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
