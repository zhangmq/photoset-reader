import QtQuick
import ImageBrowser

// ══ 设置 · 图像页（**本文件自足：改这里不会影响别的页签**）══════
//   项：缩放算法 / 缩放方式
//   ★ 不出现"快速 / 平衡 / 最佳"这类档位名 —— 直接列具体算法（用户 2026-10-11）
//   ⚠️ 当前阶段"单纯 UI"：值只存在本页属性里
Item {
    id: page

    property int algoChoice: 1
    property int pathChoice: 0

    implicitHeight: column.implicitHeight

    Column {
        id: column
        width: parent.width

        SettingsRow {
            label: "缩放算法"
            description: "缩放时使用的滤波算法"
            DropdownButton {
                label: ["面积平均（box）", "Lanczos3", "Lanczos3 + 抗振铃"][page.algoChoice]
                menuItems: [ { text: "面积平均（box）",   selected: page.algoChoice === 0 },
                             { text: "Lanczos3",          selected: page.algoChoice === 1 },
                             { text: "Lanczos3 + 抗振铃", selected: page.algoChoice === 2 } ]
                onItemTriggered: function (i) { page.algoChoice = i }
            }
        }

        SettingsRow {
            label: "缩放方式"
            description: "缩放所用的色彩空间"
            DropdownButton {
                label: ["线性光（推荐）", "像素完美（整数倍 + 最近邻）", "Qt 平滑"][page.pathChoice]
                menuItems: [ { text: "线性光（推荐）",             selected: page.pathChoice === 0 },
                             { text: "像素完美（整数倍 + 最近邻）", selected: page.pathChoice === 1 },
                             { text: "Qt 平滑",                   selected: page.pathChoice === 2 } ]
                onItemTriggered: function (i) { page.pathChoice = i }
            }
        }
    }
}
