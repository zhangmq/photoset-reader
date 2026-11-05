import QtQuick
import AppUi

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
            label: qsTr("Include hidden items")
            description: qsTr("Hidden folders are skipped by default")
            Switch {
                checked: ui.scanHidden
                onToggled: ui.scanHidden = !ui.scanHidden
            }
        }

        SettingsRow {
            label: qsTr("Recursion depth")
            description: qsTr("Maximum subfolder depth to scan; deeper scans take longer and use more memory")
            NumberField {
                value: ui.recurseDepth
                from: 1
                to: 64
                suffix: qsTr("levels")
                onEdited: function (v) { ui.recurseDepth = v }
            }
        }

        //   ★ 预取窗口（2026-11-05 用户定）：**下拉、只给 0–8**（纯数字，含义写在描述里）。
        //     0 = 只解当前屏；并行解码 = min(当前屏未满足数 + 预取数, 设备线程数)
        //     ⇒ 这一项同时也是"功耗旋钮"，不需要让人填 64 那种没有意义的档位。
        SettingsRow {
            label: qsTr("Prefetch window")
            description: qsTr("Images decoded ahead; 0 = current screen only. Higher values raise decoding concurrency and power use")
            DropdownButton {
                label: String(ui.prefetchWindow)
                menuItems: [ { text: "0", selected: ui.prefetchWindow === 0 },
                             { text: "1", selected: ui.prefetchWindow === 1 },
                             { text: "2", selected: ui.prefetchWindow === 2 },
                             { text: "3", selected: ui.prefetchWindow === 3 },
                             { text: "4", selected: ui.prefetchWindow === 4 },
                             { text: "5", selected: ui.prefetchWindow === 5 },
                             { text: "6", selected: ui.prefetchWindow === 6 },
                             { text: "7", selected: ui.prefetchWindow === 7 },
                             { text: "8", selected: ui.prefetchWindow === 8 } ]
                onItemTriggered: function (i) { ui.prefetchWindow = i }
            }
        }

        SettingsRow {
            label: qsTr("Nested archive limit")
            description: qsTr("Nested archives above this size are not opened (they are rare; avoids paying a big price for them)")
            NumberField {
                value: ui.innerArchiveMaxMb
                from: 64
                to: 8192
                suffix: "MB"
                onEdited: function (v) { ui.innerArchiveMaxMb = v }
            }
        }

        //   日志级别（用户 2026-11-04）：默认 warning；排障时调低。写盘 + 立即生效；
        //   `--log-level` 可以在启动时临时覆盖一次（不写盘）。
        SettingsRow {
            label: qsTr("Log level")
            description: qsTr("How much the app writes to the log; lower levels are for troubleshooting")
            DropdownButton {
                readonly property var levels: ["debug", "info", "warning", "error", "off"]
                readonly property var labels: [qsTr("Debug"), qsTr("Info"),
                                               qsTr("Warning"), qsTr("Error"), qsTr("Off")]
                label: labels[Math.max(0, levels.indexOf(ui.logLevel))]
                menuItems: [
                    { text: labels[0], selected: ui.logLevel === "debug" },
                    { text: labels[1], selected: ui.logLevel === "info" },
                    { text: labels[2], selected: ui.logLevel === "warning" },
                    { text: labels[3], selected: ui.logLevel === "error" },
                    { text: labels[4], selected: ui.logLevel === "off" }
                ]
                onItemTriggered: function (i) { ui.logLevel = levels[i] }
            }
        }

    }
}
