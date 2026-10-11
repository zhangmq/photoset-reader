import QtQuick
import ImageBrowser

// ══ 设置 · 快捷键页（**本文件自足：改这里不会影响别的页签**）══════
//   只读帮助表：三列（按键 / 功能 / 适用范围），行骨架见 `SettingsKeyRow.qml`
//   ★ 只列**应用特定语义**的绑定：`Tab`/`Enter`/`Esc`/方向键/`Home`/`End` 是系统与框架的
//     通用交互约定，属于共识，不列入（用户 2026-10-11）
//   ★ 排列顺序 = 适用范围顺序：全局 → 文件浏览 → 图像查看（用户 2026-10-11）
//   ★ 一行一个键（不把多个键塞进一行）
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
                text: "按键"
                color: Theme.textTertiary
            }
            Text {
                anchors.left: parent.left
                anchors.leftMargin: Theme.settingsKeyW + Theme.spaceLg
                anchors.verticalCenter: parent.verticalCenter
                text: "功能"
                color: Theme.textTertiary
            }
            Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.settingsScopeW
                horizontalAlignment: Text.AlignRight
                text: "适用范围"
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

        SettingsKeyRow { key: "Backspace"; func: "返回";                 scope: "全局" }
        SettingsKeyRow { key: "F5";        func: "刷新";                 scope: "文件浏览" }
        SettingsKeyRow { key: "Space";     func: "播放 / 暂停幻灯片";    scope: "图像查看" }
        SettingsKeyRow { key: "I";         func: "显示 / 隐藏信息面板";   scope: "图像查看" }
        SettingsKeyRow { key: "+";         func: "放大画面";             scope: "图像查看" }
        SettingsKeyRow { key: "-";         func: "缩小画面";             scope: "图像查看" }
        SettingsKeyRow { key: "[";         func: "幻灯片间隔减少 10ms";   scope: "图像查看" }
        SettingsKeyRow { key: "]";         func: "幻灯片间隔增加 10ms";   scope: "图像查看" }
    }
}
