import QtQuick
import ImageBrowser

// ══ 设置 · 常规页（**本文件自足：改这里不会影响别的页签**）══════
//   项：主题 / 幻灯片间隔（数字 + **预设快速输入**）/ 默认画面缩放 / 默认阅读版面
//   行结构见 `SettingsRow.qml`（左「标题+描述」、右控件垂直居中、行底 hairline）
//   ★ 值一律**读写 ui**（`ui.themeMode` / `ui.slideshowIntervalMs`）：
//     设置是**语义状态**：值在 `Settings`，QML 面在 `ui`（本页不再存第二份占位属性）
Item {
    id: page

    implicitHeight: column.implicitHeight

    Column {
        id: column
        width: parent.width

        SettingsRow {
            label: "主题"
            DropdownButton {
                label: ["跟随系统", "浅色", "深色"][ui.themeMode]
                menuItems: [ { text: "跟随系统", selected: ui.themeMode === 0 },
                             { text: "浅色",     selected: ui.themeMode === 1 },
                             { text: "深色",     selected: ui.themeMode === 2 } ]
                onItemTriggered: function (i) { ui.themeMode = i }
            }
        }

        SettingsRow {
            label: "幻灯片间隔"
            description: "幻灯片自动翻页间隔"
            Row {
                spacing: Theme.spaceXs
                NumberField {
                    value: ui.slideshowIntervalMs
                    from: 50
                    to: 60000
                    step: 10
                    suffix: "ms"
                    onEdited: function (v) { ui.slideshowIntervalMs = v }
                }
                // 预设只做**快速输入**：点一下把 ms 数值填进去，保存的仍是单个 ms 值
                // （与图像查看界面 `[切换速度▾]` 的预设一致；那里的"自定义/加速/减速"不搬过来）
                DropdownButton {
                    label: "预设"
                    menuItems: [
                        { text: "500ms", selected: ui.slideshowIntervalMs === 500 },
                        { text: "1s",    selected: ui.slideshowIntervalMs === 1000 },
                        { text: "2s",    selected: ui.slideshowIntervalMs === 2000 },
                        { text: "3s",    selected: ui.slideshowIntervalMs === 3000 },
                        { text: "4s",    selected: ui.slideshowIntervalMs === 4000 },
                        { text: "5s",    selected: ui.slideshowIntervalMs === 5000 }
                    ]
                    onItemTriggered: function (i) {
                        ui.slideshowIntervalMs = [500, 1000, 2000, 3000, 4000, 5000][i]
                    }
                }
            }
        }

    }
}
