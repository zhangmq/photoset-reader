import QtQuick
import AppUi

// ══ 设置 · 常规页（**本文件自足：改这里不会影响别的页签**）══════
//   项：主题 / 默认幻灯片间隔（数字 + **预设快速输入**）/ 默认画面缩放 / 默认阅读版面 / 默认循环
//   ★ 这些全是**全局默认**（2026-10-20 定案）：新建 viewer / 进沉浸视图时取一次；
//     底栏上改的是当前阅读态，**不会**回写这里。行文案只写"默认"，不再加解释性描述。
//   行结构见 `SettingsRow.qml`（左「标题+描述」、右控件垂直居中、行底 hairline）
//   ★ 值一律**读写 ui**（`ui.settingsDraft.themeMode` / `ui.settingsDraft.slideshowIntervalMs`）：
//     设置是**语义状态**：值在 `Settings`，QML 面在 `ui`（本页不再存第二份占位属性）
Item {
    id: page

    implicitHeight: column.implicitHeight

    //   ★ 语言下拉的当前值：跟随系统时显示"跟随系统（zh_CN）"，否则显示该语言自己的名字。
    //     ⚠️ 必须定义在**根对象**上：定义在 Column 里的话 `page.languageLabel()` 找不到（实测报错）。
    function languageLabel() {
        if (ui.settingsDraft.language === "") return qsTr("Follow system") + " (" + ui.effectiveLanguage + ")"
        if (ui.settingsDraft.language === "zh_CN") return "中文"
        if (ui.settingsDraft.language === "en") return "English"
        return ui.settingsDraft.language
    }

    Column {
        id: column
        width: parent.width

        SettingsRow {
            label: qsTr("Theme")
            DropdownButton {
                label: [qsTr("Follow system"), qsTr("Light"), qsTr("Dark")][ui.settingsDraft.themeMode]
                menuItems: [ { text: qsTr("Follow system"), selected: ui.settingsDraft.themeMode === 0 },
                             { text: qsTr("Light"),         selected: ui.settingsDraft.themeMode === 1 },
                             { text: qsTr("Dark"),          selected: ui.settingsDraft.themeMode === 2 } ]
                onItemTriggered: function (i) { ui.settingsDraft.themeMode = i }
            }
        }

        //   ★ 语言（2026-11-02）：`ui.settingsDraft.language` 是**语言代码**（空串 = 跟随系统）。
        //     下拉里每项都用**它自己的语言**写（中文写"中文"、英文写"English"）——
        //     看的人认得出自己的语言，比把语言名翻成当前界面语言更可靠。
        SettingsRow {
            label: qsTr("Language")
            DropdownButton {
                label: page.languageLabel()
                menuItems: [
                    { text: qsTr("Follow system") + " (" + ui.effectiveLanguage + ")",
                      selected: ui.settingsDraft.language === "" },
                    { text: "中文",    selected: ui.settingsDraft.language === "zh_CN" },
                    { text: "English", selected: ui.settingsDraft.language === "en" }
                ]
                onItemTriggered: function (i) {
                    ui.settingsDraft.language = ["", "zh_CN", "en"][i]
                }
            }
        }

        SettingsRow {
            label: qsTr("Default slideshow interval")
            //   ★ 0 = 不设最小间隔（用户 2026-11-03）：实际速度受解码 / 上屏 / 屏幕刷新率限制
            description: qsTr("0 = no minimum delay; the actual rate is limited by decoding and screen refresh rate")
            Row {
                spacing: Theme.spaceXs
                NumberField {
                    value: ui.settingsDraft.slideshowIntervalMs
                    from: 0           // 与 app::kMinSlideshowIntervalMs 一致（下限的唯一来源；0 = 不设限）
                    to: 60000
                    step: 10
                    suffix: "ms"
                    onEdited: function (v) { ui.settingsDraft.slideshowIntervalMs = v }
                }
                // 预设只做**快速输入**：点一下把 ms 数值填进去，保存的仍是单个 ms 值
                // （与图像查看界面 `[切换速度▾]` 的预设一致；那里的"自定义/加速/减速"不搬过来）
                DropdownButton {
                    label: qsTr("Presets")
                    menuItems: [
                        { text: "100ms", selected: ui.settingsDraft.slideshowIntervalMs === 100 },
                        { text: "500ms", selected: ui.settingsDraft.slideshowIntervalMs === 500 },
                        { text: "1s",    selected: ui.settingsDraft.slideshowIntervalMs === 1000 },
                        { text: "2s",    selected: ui.settingsDraft.slideshowIntervalMs === 2000 },
                        { text: "3s",    selected: ui.settingsDraft.slideshowIntervalMs === 3000 },
                        { text: "4s",    selected: ui.settingsDraft.slideshowIntervalMs === 4000 },
                        { text: "5s",    selected: ui.settingsDraft.slideshowIntervalMs === 5000 }
                    ]
                    onItemTriggered: function (i) {
                        ui.settingsDraft.slideshowIntervalMs = [100, 500, 1000, 2000, 3000, 4000, 5000][i]
                    }
                }
            }
        }

        SettingsRow {
            label: qsTr("Timeline hover preview")
            description: qsTr("Show neighbouring pages above the progress bar while hovering it")
            Switch {
                checked: ui.settingsDraft.previewStrip
                onToggled: ui.settingsDraft.previewStrip = !ui.settingsDraft.previewStrip
            }
        }

        // ── 以下三项是**新资源 / 重进查看屏时的默认值**（不是当前阅读态）──
        SettingsRow {
            label: qsTr("Default zoom")
            DropdownButton {
                label: [qsTr("Original size"), qsTr("Fit window"), qsTr("Fit width"), qsTr("Fit height")][ui.settingsDraft.fitMode]
                menuItems: [ { text: qsTr("Original size"), selected: ui.settingsDraft.fitMode === 0 },
                             { text: qsTr("Fit window"),    selected: ui.settingsDraft.fitMode === 1 },
                             { text: qsTr("Fit width"),     selected: ui.settingsDraft.fitMode === 2 },
                             { text: qsTr("Fit height"),    selected: ui.settingsDraft.fitMode === 3 } ]
                onItemTriggered: function (i) { ui.settingsDraft.fitMode = i }
            }
        }

        SettingsRow {
            label: qsTr("Default layout")
            DropdownButton {
                label: [qsTr("Single page"), qsTr("Two pages")][ui.settingsDraft.defaultLayout]
                menuItems: [ { text: qsTr("Single page"), selected: ui.settingsDraft.defaultLayout === 0 },
                             { text: qsTr("Two pages"),   selected: ui.settingsDraft.defaultLayout === 1 } ]
                onItemTriggered: function (i) { ui.settingsDraft.defaultLayout = i }
            }
        }

        SettingsRow {
            label: qsTr("Default loop mode")
            DropdownButton {
                label: [qsTr("No loop"), qsTr("Loop all"), qsTr("Loop folder")][ui.settingsDraft.defaultLoop]
                menuItems: [ { text: qsTr("No loop"),     selected: ui.settingsDraft.defaultLoop === 0 },
                             { text: qsTr("Loop all"),    selected: ui.settingsDraft.defaultLoop === 1 },
                             { text: qsTr("Loop folder"), selected: ui.settingsDraft.defaultLoop === 2 } ]
                onItemTriggered: function (i) { ui.settingsDraft.defaultLoop = i }
            }
        }

        //   ★ 2026-10-31（设计 20）：收藏夹路径 —— 进度文件就存在这个目录里；
        //     可以指到任意目录（含外部盘）。改路径**不迁移**旧文件。
        SettingsRow {
            label: qsTr("Favorites folder")
            //   * 2026-11-07（用户）：这里要显示**当前生效的收藏路径**。
            //     设置值为空 = 用默认目录 ⇒ 显示 UiModel 算出来的实际路径（不是空串）。
            description: ui.effectiveFavoritesDir
            Button {
                label: qsTr("Change…")
                //   走**草稿**：对话框里改的东西要等「确定」才生效（原来的直改生效设置是漏网的）
                onClicked: ui.chooseFavoritesDirForDraft()
            }
        }
    }
}
