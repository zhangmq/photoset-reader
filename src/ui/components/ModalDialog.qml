import QtQuick
import QtQuick.Controls
import AppUi

// ══ 模态对话框的**语义基类**（2026-11-07 分类定稿）════════════════════════
//   两个基类之一（另一个是非模态浮层 `PopupPanel`）：
//     · `ModalDialog`  —— 要用户先处理它：设置 / 关于 / 确认 / 密码
//     · `PopupPanel`   —— 随手可关的浮层：右键菜单 / 收藏列表
//     · `HintBubble`   —— tooltip，**不接事件**，单独一类
//
//   本类**只给语义、不给外观**：各对话框自己画 background / contentItem
//   （关于框有大图标布局、确认框紧凑、密码框是表单，外观不该被基类绑死）。
//
//   ★ 语义四条（统一在这里，四类对话框不用各写一遍）：
//     1. `popupType: Popup.Item` —— **不新建表面**：键盘焦点留在主窗口，
//        Wayland 下不再出现"焦点跑到另一个 surface"/隐式 grab 那一类问题；
//     2. `modal: true` —— 挡住底下的一切输入；
//     3. `focus: true` —— B 系统的焦点导航（Tab / 方向键 / Enter）从它开始；
//     4. `closePolicy: CloseOnEscape` —— **只有 Esc 能关**；模态框点外面不关
//        （用户 2026-11-07：AboutDialog 也属于模态，不需要"点外面就关"）。
//
//   ★ 挡输入但**不压暗背景**（用户 2026-10-20）：用透明 MouseArea 吃掉事件即可，
//     这样改设置时还能对照后面的画面。
Popup {
    id: root

    popupType: Popup.Item
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    padding: 0

    Overlay.modal: MouseArea {
        acceptedButtons: Qt.AllButtons
        onWheel: (w) => { w.accepted = true }
    }
}
