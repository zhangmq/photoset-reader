import QtQuick
import ImageBrowser

// ══ 设置 · 图像页（**本文件自足：改这里不会影响别的页签**）══════
//   只有一个设置项：**缩放算法**（5 个一级选项，英文名照社区口径，不翻译）。
//   ★ 2026-10-20 用户定案：不再有"档位 / 预设 / 二级参数"——
//     每个算法内部的核宽、色彩空间、是否抑制振铃全部固定（见 gpuscale::Algorithm）。
//   ★ 值一律读写 ViewModel（`ui.scaleAlgorithm`，键串唯一事实源在 gpuscale）。
//   ★ `Smooth (Qt)` 是唯一由 Qt 实现的项：软件渲染时它也是唯一能出画面的那项。
Item {
    id: page

    // 键串 ↔ 显示名的唯一映射（键必须与 gpuscale::algorithmKey() 一致）
    readonly property var algorithmKeys: ["nearest", "smooth", "bicubic", "lanczos2", "lanczos3"]
    readonly property var algorithmLabels: ["Nearest", "Smooth (Qt)", "Bicubic", "Lanczos 2", "Lanczos 3"]

    implicitHeight: column.implicitHeight

    Column {
        id: column
        width: parent.width

        SettingsRow {
            label: "缩放算法"
            // 只在"软件渲染"这个真实约束下才解释；平时不写废话
            description: ui.softwareBackend ? "软件渲染：固定 Smooth (Qt)，其余选项不生效" : ""
            DropdownButton {
                label: page.algorithmLabels[Math.max(0, page.algorithmKeys.indexOf(ui.scaleAlgorithm))]
                menuItems: [
                    { text: "Nearest",     selected: ui.scaleAlgorithm === "nearest" },
                    { text: "Smooth (Qt)", selected: ui.scaleAlgorithm === "smooth" },
                    { text: "Bicubic",     selected: ui.scaleAlgorithm === "bicubic" },
                    { text: "Lanczos 2",   selected: ui.scaleAlgorithm === "lanczos2" },
                    { text: "Lanczos 3",   selected: ui.scaleAlgorithm === "lanczos3" }
                ]
                onItemTriggered: function (i) { ui.scaleAlgorithm = page.algorithmKeys[i] }
            }
        }
    }
}
