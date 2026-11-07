import QtQuick
import AppUi

// ══ 设置 · 快捷键页（**本文件自足：改这里不会影响别的页签**）══════
//   只读帮助表：两列（按键 / 功能），行结构见 `SettingsKeyRow.qml`
//   ★ 口径（用户 2026-11-02 校正）：**只有系统／桌面惯例**的键不列（如 `Ctrl+Q` 退出 ——
//     那是 KDE 的 Quit），**其余一律列出**（`←`/`→`/`PageUp`/`PageDown` 翻页、
//     `Esc` 退出全屏都算应用自己的语义，要列）。
//   ★ `Tab`/`Enter`/`Home`/`End` 这类**控件级**通用键仍不列：它们不触发任何应用动作。
//   ★ 一行一个键（不把多个键塞进一行）。
//   ⚠️ 改 `FullscreenView.qml` 的按键处理时**同步这张表**（曾经漏删过已失效的 `Backspace`）。
Item {
    id: page

    implicitHeight: column.implicitHeight

    Column {
        id: column
        width: parent.width
        spacing: 0

        // 表头
        Item {
            width: parent.width
            implicitHeight: Theme.controlMd
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.settingsKeyW
                text: qsTr("Key")
                color: Theme.textTertiary
            }
            Text {
                anchors.left: parent.left
                anchors.leftMargin: Theme.settingsKeyW + Theme.spaceLg
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Action")
                color: Theme.textTertiary
            }
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: Theme.hairline
                color: Theme.borderL2
            }
        }

        //   ★ 表格**按 `ShortcutCatalog.entries` 渲染**（与实现同一份清单）：
        //     以前手抄一遍，`[`/`]` 就漂移过（说明写 ±10 ms，实现改的是全局默认值）。
        Repeater {
            model: ShortcutCatalog.entries
            delegate: SettingsKeyRow {
                required property var modelData
                key: modelData.seq
                func: modelData.label
            }
        }
    }
}
