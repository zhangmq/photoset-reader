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
    readonly property var providers: ui.decodeProviders()

    //   provider id → 人可读名字
    function providerName(id) {
        switch (id) {
        case "qt":  return qsTr("Built-in")
        case "wic": return qsTr("Windows codecs")
        case "gdk": return "gdk-pixbuf"
        case "cli": return qsTr("External programs")
        default:    return id
        }
    }

    //   后缀 → 支持组件（provider 是稳定 id）。
    //   ★ 2026-11-07（用户）：外部解码器要**说清楚用的是哪个 .so/.dll/exe** ——
    //     `row.component` 由 provider 自己报（内置那批为空，按用户要求不描述具体组件）。
    function providerLabel(row) {
        const comp = row.component !== undefined && row.component !== null ? row.component : ""
        switch (row.provider) {
        case "qt":  return qsTr("Built-in")
        case "wic": return comp !== "" ? qsTr("Windows codecs") + " (" + comp + ")"
                                       : qsTr("Windows codecs")
        case "gdk": return comp !== "" ? comp : "gdk-pixbuf"
        case "cli": return comp !== "" ? comp : qsTr("External program")
        default:    return comp !== "" ? row.provider + " (" + comp + ")" : row.provider
        }
    }

    implicitHeight: column.implicitHeight

    Column {
        id: column
        width: parent.width
        spacing: 0

        //   ── 组件总表（用户 2026-11-07）─────────────────────────────────────
        //   按**后缀**列的表看不到"不广告格式"的 provider（外部程序就是这种：只兜底），
        //   所以这里再列一遍"这台机器上有哪些解码组件、各自用的是什么 .so/.dll/exe"。
        Item {
            width: parent.width
            implicitHeight: Theme.controlMd
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.settingsKeyW
                text: qsTr("Component")
                color: Theme.textTertiary
            }
            Text {
                anchors.left: parent.left
                anchors.leftMargin: Theme.settingsKeyW + Theme.spaceLg
                anchors.right: parent.right
                anchors.rightMargin: Theme.spaceSm
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Used by")
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
            model: page.providers
            delegate: Item {
                required property var modelData

                width: column.width
                implicitHeight: Theme.controlMd

                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.settingsKeyW
                    text: page.providerName(modelData.name)
                    color: modelData.available ? Theme.textPrimary : Theme.textSecondary
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.settingsKeyW + Theme.spaceLg
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.spaceSm
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    //   内置那批按用户要求不写具体组件；其它显示 .so/.dll/exe
                    text: {
                        const comp = modelData.component || ""
                        const n = modelData.suffixCount
                        const tail = modelData.available
                                     ? (n > 0 ? qsTr("%1 formats").arg(n) : qsTr("fallback only"))
                                     : qsTr("not available")
                        return comp !== "" ? comp + " · " + tail : tail
                    }
                    color: modelData.available ? Theme.textSecondary : Theme.textCaption
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
