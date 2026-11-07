pragma Singleton
import QtQuick
import AppUi

// ══ 快捷键**唯一清单**（2026-11-07）══════════════════════════════════════
//   为什么要有它：以前"行为在 `FullscreenView`、说明在设置页手抄一份"，
//   两处必然会漂移（`[`/`]` 就是这样：说明写着 ±10 ms，实现改的是全局默认值，等于没反应）。
//   现在：
//     · `AppShortcuts.qml` 用 `Instantiator` **按这份清单实例化 `Shortcut`**；
//     · 设置 → 快捷键页 **按同一份清单渲染表格**。
//   ⇒ 加一个键只改这里一处，行为与文档同时到位。
//
//   字段：
//     seq    Qt 的键序列写法（`Shortcut.sequence`）
//     key    Qt.Key_* 整数（交给 `AppShortcuts.run()` 分发）
//     label  给用户看的动作名（要翻译）
//     group  分组标题（`""` = 不分组）
//
//   ⚠️ 只放**A 系统**（浏览/播放控制，窗口级、不依赖焦点）。
//      B 系统是面板里的标准焦点导航（Tab / ↑↓ / Enter / Esc），不在这里登记。
QtObject {
    id: root

    readonly property var entries: [
        // ── 翻页 ──
        { seq: "Left",      key: Qt.Key_Left,        label: qsTr("Previous page"),        group: qsTr("Browsing") },
        { seq: "Right",     key: Qt.Key_Right,       label: qsTr("Next page"),            group: qsTr("Browsing") },
        { seq: "PageUp",    key: Qt.Key_PageUp,      label: qsTr("Previous page"),        group: qsTr("Browsing") },
        { seq: "PageDown",  key: Qt.Key_PageDown,    label: qsTr("Next page"),            group: qsTr("Browsing") },
        { seq: "Up",        key: Qt.Key_Up,          label: qsTr("Previous folder"),      group: qsTr("Browsing") },
        { seq: "Down",      key: Qt.Key_Down,        label: qsTr("Next folder"),          group: qsTr("Browsing") },

        // ── 播放 / 信息 ──
        { seq: "Space",     key: Qt.Key_Space,       label: qsTr("Play / pause slideshow"), group: qsTr("Playback") },
        { seq: "I",         key: Qt.Key_I,           label: qsTr("Show / hide info row"),  group: qsTr("Playback") },

        // ── 缩放（`=` 与 `+` 同键：美式键盘加号要 Shift）──
        { seq: "+",         key: Qt.Key_Plus,        label: qsTr("Zoom in"),              group: qsTr("Zoom") },
        { seq: "=",         key: Qt.Key_Equal,       label: qsTr("Zoom in"),              group: qsTr("Zoom") },
        { seq: "-",         key: Qt.Key_Minus,       label: qsTr("Zoom out"),             group: qsTr("Zoom") },

        // ── 幻灯片间隔（档位网格，见 appsettings.h）──
        { seq: "[",         key: Qt.Key_BracketLeft, label: qsTr("Slideshow interval −"), group: qsTr("Playback") },
        { seq: "]",         key: Qt.Key_BracketRight, label: qsTr("Slideshow interval +"), group: qsTr("Playback") },

        // ── 窗口 / 面板 ──
        //   ★ 2026-11-07（用户）：**Windows 没有系统级的"切换全屏"快捷键** ——
        //     `Alt+Enter` / `F11` 都属于"应用自己实现"的惯例（系统只管
        //     `Win+↑/↓/←/→` 那套最大化 / 贴靠）。所以我们自己实现两个：
        //     `F11`（看图器/浏览器的通用惯例）与 `Alt+Enter`（游戏/播放器惯例）。
        { seq: "F11",        key: Qt.Key_F11,         label: qsTr("Toggle full screen"),    group: qsTr("Window") },
        { seq: "Alt+Return", key: Qt.Key_Return,      label: qsTr("Toggle full screen"),    group: qsTr("Window") },
        { seq: "Escape",     key: Qt.Key_Escape,      label: qsTr("Exit full screen"),      group: qsTr("Window") },
        { seq: "Menu",      key: Qt.Key_Menu,        label: qsTr("Open context menu (at view centre)"), group: qsTr("Window") },
        { seq: "Shift+F10", key: Qt.Key_F10,         label: qsTr("Open context menu (at view centre)"), group: qsTr("Window") }
    ]
}
