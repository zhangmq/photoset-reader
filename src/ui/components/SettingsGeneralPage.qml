import QtQuick
import ImageBrowser

// ══ 设置 · 常规页（**本文件自足：改这里不会影响别的页签**）══════
//   项：主题 / 幻灯片间隔（数字 + **预设快速输入**）/ 默认画面缩放 / 默认阅读版面
//   行骨架见 `SettingsRow.qml`（左「标题+描述」、右控件垂直居中、行底 hairline）
//   ⚠️ 当前阶段"单纯 UI"：值只存在本页属性里，不接 ViewModel
Item {
    id: page

    property int themeChoice: 0
    property int intervalMs: 300
    property int fitChoice: 1
    property int layoutChoice: 0

    implicitHeight: column.implicitHeight

    Column {
        id: column
        width: parent.width

        SettingsRow {
            label: "主题"
            DropdownButton {
                label: ["跟随系统", "浅色", "深色"][page.themeChoice]
                menuItems: [ { text: "跟随系统", selected: page.themeChoice === 0 },
                             { text: "浅色",     selected: page.themeChoice === 1 },
                             { text: "深色",     selected: page.themeChoice === 2 } ]
                onItemTriggered: function (i) { page.themeChoice = i }
            }
        }

        SettingsRow {
            label: "幻灯片间隔"
            description: "幻灯片自动翻页间隔"
            Row {
                spacing: Theme.spaceXs
                NumberField {
                    value: page.intervalMs
                    from: 50
                    to: 60000
                    step: 10
                    suffix: "ms"
                    onEdited: function (v) { page.intervalMs = v }
                }
                // 预设只做**快速输入**：点一下把 ms 数值填进去，保存的仍是单个 ms 值
                // （与图像查看界面 `[切换速度▾]` 的预设一致；那里的"自定义/加速/减速"不搬过来）
                DropdownButton {
                    label: "预设"
                    menuItems: [
                        { text: "500ms", selected: page.intervalMs === 500 },
                        { text: "1s",    selected: page.intervalMs === 1000 },
                        { text: "2s",    selected: page.intervalMs === 2000 },
                        { text: "3s",    selected: page.intervalMs === 3000 },
                        { text: "4s",    selected: page.intervalMs === 4000 },
                        { text: "5s",    selected: page.intervalMs === 5000 }
                    ]
                    onItemTriggered: function (i) {
                        page.intervalMs = [500, 1000, 2000, 3000, 4000, 5000][i]
                    }
                }
            }
        }

        SettingsRow {
            label: "默认画面缩放"
            description: "打开图片时的默认缩放"
            DropdownButton {
                label: ["原始大小", "适应屏幕", "适应宽度", "适应高度"][page.fitChoice]
                menuItems: [ { text: "原始大小", selected: page.fitChoice === 0 },
                             { text: "适应屏幕", selected: page.fitChoice === 1 },
                             { text: "适应宽度", selected: page.fitChoice === 2 },
                             { text: "适应高度", selected: page.fitChoice === 3 } ]
                onItemTriggered: function (i) { page.fitChoice = i }
            }
        }

        SettingsRow {
            label: "默认阅读版面"
            description: "打开图片时的默认版面"
            DropdownButton {
                label: ["单幅", "双幅"][page.layoutChoice]
                menuItems: [ { text: "单幅", selected: page.layoutChoice === 0 },
                             { text: "双幅", selected: page.layoutChoice === 1 } ]
                onItemTriggered: function (i) { page.layoutChoice = i }
            }
        }
    }
}
