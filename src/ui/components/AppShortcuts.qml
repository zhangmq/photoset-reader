import QtQuick
import AppUi

// ══ A 系统：浏览 / 播放控制快捷键（**不依赖任何界面**）══════════════════════
//   分类（2026-11-07 定稿）：应用里两套键盘逻辑，互不干扰 ——
//     · **A（本文件）**：浏览/播放控制，窗口级 `Shortcut`，**直达 UiModel**，
//       与"谁持有焦点"无关；覆盖底栏那些动作（翻页/播放/缩放/间隔/信息行/全屏）。
//     · **B（面板）**：右键菜单 / 设置 / 关于 / 确认 / 密码 / 收藏列表 ——
//       基于焦点的标准导航（Tab / ↑↓ / Enter / Esc），由 `PopupPanel` / `ModalDialog`
//       两个基类保证语义；**不在这里实现**。
//     · tooltip（`HintBubble`）不属于任何一套：不接事件、不参与门控。
//
//   ★ 唯一的边界规则（不点名任何面板）：
//       A 生效 ⟺ 窗口是活动窗口 ∧ (activeFocusItem 为空 ∨ 它在 `scope` 子树里)
//     · 焦点为空 ⇒ **生效** —— 这正是"窗口 active 但没人持有焦点"那种形态（旧实现用
//       `Keys.onPressed`，焦点链一断，**所有**键都收不到，点画面也救不回来）；
//     · 焦点在面板里（面板都是 `Popup.Item` + `focus:true`，其 item 挂在窗口 overlay 下，
//       不在 `scope` 子树里）⇒ **A 整体让路**，键归面板；
//     · tooltip 从不拿焦点 ⇒ 天然不影响 A。
//
//   ★ 唤出右键菜单（用户 2026-11-07）：`Menu` 键 / `Shift+F10`，位置**一律视口中心**
//     —— 键盘唤起与"鼠标右键点在哪儿"是两套互不混用的逻辑。
Item {
    id: root
    objectName: "appShortcuts"        // 测试用它定位（不要改）

    //   A 的作用域 = 查看界面根 item；焦点在它之外 ⇒ 认为面板拿着键盘
    property Item scope: null

    signal menuRequested()

    //   ★ 用**显式中间绑定**算可用性（不要写成一个大 JS 块：那样依赖追踪更脆，
    //     实测 `window.active` 变化时整块没有重新求值 ⇒ 快捷键一直 disabled）。
    readonly property var  _win:    root.Window.window
    readonly property bool _active: _win ? _win.active : false
    readonly property var  _focus:  _win ? _win.activeFocusItem : null

    function _inScope(item) {
        let p = item
        while (p) {
            if (p === root.scope) return true
            p = p.parent
        }
        return false
    }

    readonly property bool _focusInScope: _focus ? _inScope(_focus) : true
    //   ★ 不再自己检查 `_active`：`Shortcut` 本来**只在活动窗口里**才会被 Qt 触发，
    //     再读一次 `window.active` 只会在绑定层面引入脆弱性（实测它一抖就把快捷键全禁了）。
    //     这里只回答"焦点是不是被某个面板拿走了"。
    //   焦点为空、或焦点是**场景根**（`activeFocusItem` 有时就是 QQuickRootItem）
    //   ⇒ 没有面板拿着键盘 ⇒ A 生效。用 `parent === null` 判"场景根"最稳：
    //   面板里的 item 一定有 parent（挂在窗口 overlay 下）。
    readonly property bool _focusIsRoot: _focus === null || _focus.parent === null
    //   ⚠️ 不能叫 `enabled`：`Item` 自带 `enabled`，同名会遮蔽基类成员
    //     （QML 会报 "overrides a member of the base object"）。
    readonly property bool active: _focusInScope || _focusIsRoot

    //   键 → 动作（唯一实现；Shortcut 只负责"什么时候能触发"）
    function run(key) {
        switch (key) {
        case Qt.Key_Escape:      ui.windowFullscreen = false; return true
        case Qt.Key_Left:        if (ui.viewer) ui.viewer.stepPage(-1); return true
        case Qt.Key_Right:       if (ui.viewer) ui.viewer.stepPage(1); return true
        case Qt.Key_Up:          if (ui.viewer && ui.viewer.prevFolderName !== "")
                                     ui.viewer.stepFolder(-1)
                                 return true
        case Qt.Key_Down:        if (ui.viewer && ui.viewer.nextFolderName !== "")
                                     ui.viewer.stepFolder(1)
                                 return true
        case Qt.Key_PageUp:      if (ui.viewer) ui.viewer.stepPage(-1); return true
        case Qt.Key_PageDown:    if (ui.viewer) ui.viewer.stepPage(1); return true
        case Qt.Key_Space:       if (ui.viewer) ui.viewer.togglePlaying(); return true
        case Qt.Key_BracketLeft:  ui.stepSlideshowInterval(-1); return true
        case Qt.Key_BracketRight: ui.stepSlideshowInterval(1); return true
        case Qt.Key_I:           if (ui.viewer) ui.viewer.osdVisible = !ui.viewer.osdVisible
                                 return true
        case Qt.Key_Plus:
        case Qt.Key_Equal:       root.zoom("in"); return true
        case Qt.Key_Minus:       root.zoom("out"); return true
        case Qt.Key_Menu:
        case Qt.Key_F10:         root.menuRequested(); return true
        }
        return false
    }

    signal zoomRequested(string dir)

    //   `+`/`-` 的档位表在 `FullscreenView`（与右键菜单同一入口），这里只转发
    function zoom(dir) { root.zoomRequested(dir) }

    Shortcut { sequence: "Escape";      enabled: root.active; onActivated: root.run(Qt.Key_Escape) }
    Shortcut { sequence: "Left";        enabled: root.active; onActivated: root.run(Qt.Key_Left) }
    Shortcut { sequence: "Right";       enabled: root.active; onActivated: root.run(Qt.Key_Right) }
    Shortcut { sequence: "Up";          enabled: root.active; onActivated: root.run(Qt.Key_Up) }
    Shortcut { sequence: "Down";        enabled: root.active; onActivated: root.run(Qt.Key_Down) }
    Shortcut { sequence: "PageUp";      enabled: root.active; onActivated: root.run(Qt.Key_PageUp) }
    Shortcut { sequence: "PageDown";    enabled: root.active; onActivated: root.run(Qt.Key_PageDown) }
    Shortcut { sequence: "Space";       enabled: root.active; onActivated: root.run(Qt.Key_Space) }
    Shortcut { sequence: "I";           enabled: root.active; onActivated: root.run(Qt.Key_I) }
    Shortcut { sequence: "[";           enabled: root.active; onActivated: root.run(Qt.Key_BracketLeft) }
    Shortcut { sequence: "]";           enabled: root.active; onActivated: root.run(Qt.Key_BracketRight) }
    Shortcut { sequence: "+";           enabled: root.active; onActivated: root.run(Qt.Key_Plus) }
    Shortcut { sequence: "=";           enabled: root.active; onActivated: root.run(Qt.Key_Equal) }
    Shortcut { sequence: "-";           enabled: root.active; onActivated: root.run(Qt.Key_Minus) }
    //   ★ 唤出右键菜单：`Menu` 键 与 `Shift+F10`（Windows / KDE / Xfce 的通用惯例）。
    //     `Shift+F10` 在 Qt 里是 `F10` + Shift 修饰 ⇒ 单独一条带修饰键的 Shortcut。
    Shortcut { sequence: "Menu";        enabled: root.active; onActivated: root.run(Qt.Key_Menu) }
    Shortcut { sequence: "Shift+F10";   enabled: root.active; onActivated: root.run(Qt.Key_F10) }
}
