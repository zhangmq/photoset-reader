import QtQuick
import AppUi

// ══ 快捷键表行结构（**只定义一次**）════════════════════════
//   两列：按键（`settingsKeyW`）/ 功能（占满剩余宽度）
//   行高 `controlMd`(36)（用户 2026-10-11：比默认小号行高大一号），行底 hairline
//   ★ 原来的第三列「适用范围」已删（用户 2026-11-02 整理设置）：只剩一个查看界面，
//     每行都是"图像查看" ⇒ 这一列恒等于同一个值，纯粹是噪音。
//   用法：SettingsKeyRow { key: "Space"; func: "播放 / 暂停幻灯片" }
Item {
    id: root

    property string key: ""
    property string func: ""

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
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceSm
        anchors.verticalCenter: parent.verticalCenter
        elide: Text.ElideRight
        text: root.func
        color: Theme.textSecondary
    }
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Theme.hairline
        color: Theme.borderL2
    }
}
