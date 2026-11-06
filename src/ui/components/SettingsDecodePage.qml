import QtQuick
import AppUi

// ══ 设置 · 解码支持页（用户 2026-11-07）══════════════════════════════════
//   把**本次启动探到的能力**按后缀列成表：`后缀 — 支持组件`。
//   · 数据来自 `ui.decodeSupport()`（启动时 `Capabilities::probe()` 的快照，只读）；
//   · "内置支持"**不描述具体组件**（用户要求）：Qt 自带那批只显示「内置」；
//   · 外部程序 provider 显示它的 `detail`（工具路径/名字，排查"为什么这个格式能用"用）；
//   · 这是**这台机器现在的能力**，不是产品承诺 —— 装/卸一个编解码器，这里就变。
//   ★ 样式与快捷键表**一致**（用户 2026-11-07）：行高 `controlMd`、行底 hairline、
//     **不做交替底色**；表头文字 `textTertiary`。
Item {
    id: page

    readonly property var rows: ui.decodeSupport()

    //   后缀 → 支持组件的显示名（provider 是稳定 id）
    function providerLabel(row) {
        switch (row.provider) {
        case "qt":  return qsTr("Built-in")
        case "wic": return qsTr("Windows codecs")
        case "gdk": return "gdk-pixbuf"
        case "cli": return row.detail !== "" ? row.detail : qsTr("External program")
        default:    return row.provider
        }
    }

    implicitHeight: column.implicitHeight

    Column {
        id: column
        width: parent.width
        spacing: 0

        SettingsRow {
            label: qsTr("Formats")
            description: qsTr("%1 file extensions on this machine (detected at startup)")
                             .arg(page.rows.length)
        }

        // 表头
        Item {
            width: parent.width
            implicitHeight: Theme.controlMd
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.settingsKeyW
                text: qsTr("Extension")
                color: Theme.textTertiary
            }
            Text {
                anchors.left: parent.left
                anchors.leftMargin: Theme.settingsKeyW + Theme.spaceLg
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Decoded by")
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

        Repeater {
            model: page.rows
            delegate: Item {
                required property var modelData

                width: column.width
                implicitHeight: Theme.controlMd

                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.settingsKeyW
                    text: modelData.suffix
                    color: Theme.textPrimary
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.settingsKeyW + Theme.spaceLg
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.spaceSm
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    text: page.providerLabel(modelData)
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
        }
    }
}
