import QtQuick
import ImageBrowser

// ══ 设置 · 系统页（**本文件自足：改这里不会影响别的页签**）══════
//   项：包含隐藏项 / 递归深度 / 预取窗口 / 缓存上限
//   ★ **隐藏目录的判定交给 Qt**：用 `QFileInfo::isHidden()`，**不在应用层暴露 OS 差异**
//      （Linux/macOS 看点前缀、Windows 看隐藏属性 —— 那是各平台自己的规则，界面文案只说"隐藏目录"）
//   ★ **递归恒开**（用户 2026-10-11）：递归是默认行为，没有"关闭"这个选项；
//      层数由「递归深度」控制（默认 5 层）
//   ★ **预取窗口只给一个数值**（用户 2026-10-11）：窗口总张数由用户设定；
//      它是**滑动窗口** —— 向前/向后翻页时窗口跟着滑动，
//      **两侧各分多少由机制动态决定**，不再让用户分别配"前进/后退"两个数
//   （用户 2026-10-11：这三项从常规页拆出来独立成页）
//   ★ 值一律**读写 ViewModel**（`ui.scanHidden` / `ui.recurseDepth` / `ui.prefetchWindow` /
//     `ui.themeMode`）：设置是**语义状态**，单一来源在 ViewModel（本页不再存第二份占位属性）
Item {
    id: page

    implicitHeight: column.implicitHeight

    Column {
        id: column
        width: parent.width

        SettingsRow {
            label: "包含隐藏项"
            description: "默认跳过隐藏目录"
            Switch {
                checked: ui.scanHidden
                onToggled: ui.scanHidden = !ui.scanHidden
            }
        }

        SettingsRow {
            label: "递归深度"
            description: "扫描子目录的最大层数；设置过深会增加扫描耗时与内存占用"
            NumberField {
                value: ui.recurseDepth
                from: 1
                to: 64
                suffix: "层"
                onEdited: function (v) { ui.recurseDepth = v }
            }
        }

        SettingsRow {
            label: "预取窗口"
            description: "提前解码的图片张数（翻页时自动滑动）"
            NumberField {
                value: ui.prefetchWindow
                from: 3
                to: 64
                suffix: "张"
                onEdited: function (v) { ui.prefetchWindow = v }
            }
        }

        SettingsRow {
            label: "内层压缩包上限"
            description: "超过上限的内层压缩包不展开（嵌套包不常见，避免为此付出大代价）"
            NumberField {
                value: ui.innerArchiveMaxMb
                from: 64
                to: 8192
                suffix: "MB"
                onEdited: function (v) { ui.innerArchiveMaxMb = v }
            }
        }

    }
}
