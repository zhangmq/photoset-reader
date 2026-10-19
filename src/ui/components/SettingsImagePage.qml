import QtQuick
import ImageBrowser

// ══ 设置 · 图像页（**本文件自足：改这里不会影响别的页签**）══════
//   项：缩放方式 / Lanczos 瓣数 / 抗振铃强度
//   ★ 纪律：**参数才是真相**（2026-10-20 用户定案：画质预设已清，只留参数）。
//   ★ 值一律**读写 ViewModel**；字符串 ↔ 枚举的映射只在 ViewModel（本页只做下标换算）
Item {
    id: page

    readonly property var modeKeys: ["linear-light", "nearest", "qt-smooth"]
    readonly property var lobeValues: [2, 3, 4]
    readonly property var arValues: [0.0, 0.5, 1.0]

    implicitHeight: column.implicitHeight

    Column {
        id: column
        width: parent.width

        SettingsRow {
            label: "缩放方式"
            description: "缩放所用的色彩空间"
            DropdownButton {
                label: ["线性光（推荐）", "像素完美（整数倍 + 最近邻）", "Qt 平滑"][page.modeKeys.indexOf(ui.scalerMode)]
                menuItems: [ { text: "线性光（推荐）",             selected: ui.scalerMode === "linear-light" },
                             { text: "像素完美（整数倍 + 最近邻）", selected: ui.scalerMode === "nearest" },
                             { text: "Qt 平滑",                   selected: ui.scalerMode === "qt-smooth" } ]
                onItemTriggered: function (i) { ui.scalerMode = page.modeKeys[i] }
            }
        }

        SettingsRow {
            label: "Lanczos 瓣数"
            description: "越大越锐，振铃越多（仅 Lanczos 系生效）"
            DropdownButton {
                label: String(ui.scalerLobes)
                menuItems: [ { text: "2", selected: ui.scalerLobes === 2 },
                             { text: "3", selected: ui.scalerLobes === 3 },
                             { text: "4", selected: ui.scalerLobes === 4 } ]
                onItemTriggered: function (i) { ui.scalerLobes = page.lobeValues[i] }
            }
        }

        SettingsRow {
            label: "抗振铃强度"
            description: "把结果夹回源邻域的范围，0 = 关（仅「最佳」档生效）"
            DropdownButton {
                label: ["关", "一半", "全"][page.arValues.indexOf(ui.scalerAntiring)]
                menuItems: [ { text: "关",   selected: ui.scalerAntiring === 0.0 },
                             { text: "一半", selected: ui.scalerAntiring === 0.5 },
                             { text: "全",   selected: ui.scalerAntiring === 1.0 } ]
                onItemTriggered: function (i) { ui.scalerAntiring = page.arValues[i] }
            }
        }
    }
}
