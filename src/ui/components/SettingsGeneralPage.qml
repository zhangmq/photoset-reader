import QtQuick
import AppUi

// ══ 设置 · 常规页（**本文件自足：改这里不会影响别的页签**）══════
//   项：主题 / 默认幻灯片间隔（数字 + **预设快速输入**）/ 默认画面缩放 / 默认阅读版面 / 默认循环
//   ★ 这些全是**全局默认**（2026-10-20 定案）：新建 viewer / 进沉浸视图时取一次；
//     底栏上改的是当前阅读态，**不会**回写这里。行文案只写"默认"，不再加解释性描述。
//   行结构见 `SettingsRow.qml`（左「标题+描述」、右控件垂直居中、行底 hairline）
//   ★ 值一律**读写 ui**（`ui.themeMode` / `ui.slideshowIntervalMs`）：
//     设置是**语义状态**：值在 `Settings`，QML 面在 `ui`（本页不再存第二份占位属性）
Item {
    id: page

    implicitHeight: column.implicitHeight

    //   ★ 语言下拉的当前值：跟随系统时显示"跟随系统（zh_CN）"，否则显示该语言自己的名字。
    //     ⚠️ 必须定义在**根对象**上：定义在 Column 里的话 `page.languageLabel()` 找不到（实测报错）。
    function languageLabel() {
        if (ui.language === "") return qsTr("Follow system") + " (" + ui.effectiveLanguage + ")"
        if (ui.language === "zh_CN") return "中文"
        if (ui.language === "en") return "English"
        return ui.language
    }

    Column {
        id: column
        width: parent.width

        SettingsRow {
            label: qsTr("Theme")
            DropdownButton {
                label: [qsTr("Follow system"), qsTr("Light"), qsTr("Dark")][ui.themeMode]
                menuItems: [ { text: qsTr("Follow system"), selected: ui.themeMode === 0 },
                             { text: qsTr("Light"),         selected: ui.themeMode === 1 },
                             { text: qsTr("Dark"),          selected: ui.themeMode === 2 } ]
                onItemTriggered: function (i) { ui.themeMode = i }
            }
        }

        //   ★ 语言（2026-11-02）：`ui.language` 是**语言代码**（空串 = 跟随系统）。
        //     下拉里每项都用**它自己的语言**写（中文写"中文"、英文写"English"）——
        //     看的人认得出自己的语言，比把语言名翻成当前界面语言更可靠。
        SettingsRow {
            label: qsTr("Language")
            DropdownButton {
                label: page.languageLabel()
                menuItems: [
                    { text: qsTr("Follow system") + " (" + ui.effectiveLanguage + ")",
                      selected: ui.language === "" },
                    { text: "中文",    selected: ui.language === "zh_CN" },
                    { text: "English", selected: ui.language === "en" }
                ]
                onItemTriggered: function (i) {
                    ui.language = ["", "zh_CN", "en"][i]
                }
            }
        }

        SettingsRow {
            label: qsTr("Default slideshow interval")
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
                    label: qsTr("Presets")
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

        // ── 以下三项是**新资源 / 重进查看屏时的默认值**（不是当前阅读态）──
        SettingsRow {
            label: qsTr("Default zoom")
            DropdownButton {
                label: [qsTr("Original size"), qsTr("Fit screen"), qsTr("Fit width"), qsTr("Fit height")][ui.fitMode]
                menuItems: [ { text: qsTr("Original size"), selected: ui.fitMode === 0 },
                             { text: qsTr("Fit screen"),    selected: ui.fitMode === 1 },
                             { text: qsTr("Fit width"),     selected: ui.fitMode === 2 },
                             { text: qsTr("Fit height"),    selected: ui.fitMode === 3 } ]
                onItemTriggered: function (i) { ui.fitMode = i }
            }
        }

        SettingsRow {
            label: qsTr("Default layout")
            DropdownButton {
                label: [qsTr("Single page"), qsTr("Two pages")][ui.defaultLayout]
                menuItems: [ { text: qsTr("Single page"), selected: ui.defaultLayout === 0 },
                             { text: qsTr("Two pages"),   selected: ui.defaultLayout === 1 } ]
                onItemTriggered: function (i) { ui.defaultLayout = i }
            }
        }

        SettingsRow {
            label: qsTr("Default loop mode")
            DropdownButton {
                label: [qsTr("No loop"), qsTr("Loop all"), qsTr("Loop folder")][ui.defaultLoop]
                menuItems: [ { text: qsTr("No loop"),     selected: ui.defaultLoop === 0 },
                             { text: qsTr("Loop all"),    selected: ui.defaultLoop === 1 },
                             { text: qsTr("Loop folder"), selected: ui.defaultLoop === 2 } ]
                onItemTriggered: function (i) { ui.defaultLoop = i }
            }
        }

        //   ★ 2026-10-31（设计 20）：收藏夹路径 —— 进度文件就存在这个目录里；
        //     可以指到任意目录（含外部盘）。改路径**不迁移**旧文件。
        SettingsRow {
            label: qsTr("Favorites folder")
            description: ui.favoritesDir
            Button {
                label: qsTr("Change…")
                onClicked: ui.chooseFavoritesDir()
            }
        }
    }
}
