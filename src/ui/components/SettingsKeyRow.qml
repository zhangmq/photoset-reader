import QtQuick
import ImageBrowser

// ══ 快捷键表行骨架（**结构只定义一次**）════════════════════════
//   三列：按键（`settingsKeyW`）/ 功能（占满）/ 适用范围（`settingsScopeW`，右对齐）
//   行高 `controlMd`(36)（用户 2026-10-11：比默认小号行高大一号），行底 hairline
//   用法：SettingsKeyRow { key: "Space"; func: "播放 / 暂停幻灯片"; scope: "图像查看" }
Item {
    id: root

    property string key: ""
    property string func: ""
    property string scope: ""

    width: parent.width
    implicitHeight: Theme.controlMd

    Text {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: Theme.settingsKeyW
        text: root.key
        color: Theme.textPrimary
    }
    Text {
        anchors.left: parent.left
        anchors.leftMargin: Theme.settingsKeyW + Theme.spaceLg
        anchors.right: scopeText.left
        anchors.rightMargin: Theme.spaceLg
        anchors.verticalCenter: parent.verticalCenter
        elide: Text.ElideRight
        text: root.func
        color: Theme.textSecondary
    }
    Text {
        id: scopeText
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: Theme.settingsScopeW
        horizontalAlignment: Text.AlignRight
        text: root.scope
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
