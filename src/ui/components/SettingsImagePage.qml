import QtQuick
import AppUi

// ══ 设置 · 图像页（**本文件自足：改这里不会影响别的页签**）══════
//   只有一个设置项：**缩放算法**（5 个一级选项，英文名照社区口径，不翻译）。
//   ★ 2026-10-20 用户定案：不再有"档位 / 预设 / 二级参数"——
//     每个算法内部的核宽、色彩空间、是否抑制振铃全部固定（见 gpuscale::Algorithm）。
//   ★ 值一律读写 ViewModel（`ui.settingsDraft.scaleAlgorithm`，键串唯一事实源在 gpuscale）。
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
            label: qsTr("Scale algorithm")
            // 只在"软件渲染"这个真实约束下才解释；平时不写废话。软件渲染是**降级**，用警告色
            description: ui.softwareBackend ? qsTr("Software rendering: Smooth (Qt) only; other options have no effect") : ""
            descriptionColor: Theme.warn
            DropdownButton {
                label: page.algorithmLabels[Math.max(0, page.algorithmKeys.indexOf(ui.settingsDraft.scaleAlgorithm))]
                //   ★ 2026-11-07（用户）：软件渲染下**禁用**其它算法，而不是只写一句说明。
                //     `ui.softwareBackend` 在第一帧之后就有值（`sceneGraphInitialized` 探的），
                //     用户能打开设置时它早已确定；`PopupMenu` 的 `enabled:false` 是整行 dim
                //     且不响应 hover/点击/键盘，所以这里逐项给它。
                //     `Smooth (Qt)` 永远可选（软件路径唯一真正生效的算法）。
                menuItems: [
                    { text: "Nearest",     selected: ui.settingsDraft.scaleAlgorithm === "nearest",
                      enabled: !ui.softwareBackend },
                    { text: "Smooth (Qt)", selected: ui.settingsDraft.scaleAlgorithm === "smooth" },
                    { text: "Bicubic",     selected: ui.settingsDraft.scaleAlgorithm === "bicubic",
                      enabled: !ui.softwareBackend },
                    { text: "Lanczos 2",   selected: ui.settingsDraft.scaleAlgorithm === "lanczos2",
                      enabled: !ui.softwareBackend },
                    { text: "Lanczos 3",   selected: ui.settingsDraft.scaleAlgorithm === "lanczos3",
                      enabled: !ui.softwareBackend }
                ]
                onItemTriggered: function (i) { ui.settingsDraft.scaleAlgorithm = page.algorithmKeys[i] }
            }
        }
    }
}
