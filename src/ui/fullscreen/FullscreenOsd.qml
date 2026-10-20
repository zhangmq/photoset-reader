import QtQuick
import ImageBrowser

// ══ 全屏 · OSD 信息面板（浮层，键值两列 / 双幅两列）═════════════════════════
//   用户 2026-10-11 指定内容：图集信息、当前文件夹、图片尺寸、类型、分辨率、**EXIF**
//   · **只被「OSD 切换」控制，不随 UI 显隐**（用户明确）—— 控件条自动淡出时本面板不动
//   · ★ **本面板是「无 UI 时的完整上下文信息」，要求不缺失**（用户 2026-10-11 明确）
//   · **无背景**（用户 2026-10-11：不要背景）→ 文本一律带 **outline**（`Text.Outline`
//     + `overlayTextOutline`），压在任意画面上都可读；键列暗、值列亮
//
//   ★★ 数据 = **按需读取**（用户 2026-10-21 定案）：`ui.viewer.osdRows()` 一次调用返回
//      "列"（单幅 1 列、双幅 2 列），每列是该幅的行 `{k, v, section}`。
//      **没有绑定、没有状态**：OSD 打开时、换屏时、图备好时各读一次。
//      EXIF **不是固定字段集**（不同图片标签不同）⇒ 行由数据决定，空则不出现。
Item {
    id: root

    property bool osdVisible: false
    property var  columns: []          // [ { rows: [ {k,v,section}, … ] }, … ]
    //   键列宽 = **本屏所有键里最宽的那个**（未映射的 EXIF 名可能很长，如
    //   `FocalLengthIn35mmFilm`）—— 固定 64 只够 4 个汉字，长键会压到值列上
    property int  keyColumnWidth: Theme.osdKeyW

    function measureKeyWidth(cols) {
        var w = Theme.osdKeyW
        for (var c = 0; c < cols.length; ++c) {
            var rows = cols[c].rows
            for (var r = 0; r < rows.length; ++r) {
                keyMetrics.text = rows[r].k
                if (keyMetrics.advanceWidth > w) w = keyMetrics.advanceWidth
            }
        }
        return Math.min(Math.ceil(w) + 4, Theme.osdKeyMaxW)   // +4：给字形留余量（贴边会被 elide 掉一个字符）
    }

    function refresh() {
        columns = (ui.viewer && root.osdVisible) ? ui.viewer.osdRows() : []
        keyColumnWidth = measureKeyWidth(columns)
    }
    onOsdVisibleChanged: if (root.osdVisible) root.refresh()
    Component.onCompleted: if (root.osdVisible) root.refresh()

    //   换屏会连着来好几个事件（currentChanged + imageAChanged + imageBChanged）——
    //   合并成一次读，避免同一屏重复取字节（包内成员每次都要解出成员）
    Timer {
        id: refreshDebounce
        interval: 40
        onTriggered: root.refresh()
    }
    function schedule() { if (root.osdVisible) refreshDebounce.restart() }

    Connections {
        target: ui.viewer
        // 换屏 ⇒ 文件名 / 序号 / 尺寸 变了；图备好 ⇒ 尺寸 / 类型 / 色彩空间 补齐
        function onCurrentChanged() { root.schedule() }
        function onImageAChanged() { root.schedule() }
        function onImageBChanged() { root.schedule() }
    }

    implicitWidth: cols.implicitWidth
    implicitHeight: cols.implicitHeight
    opacity: root.osdVisible ? 1.0 : 0.0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: Theme.durMed; easing.type: Easing.OutCubic } }

    //   隐藏的字体来源 + 量宽器（只用来算键列宽，不参与布局）
    Text {
        id: keyFontSource
        visible: false
    }
    TextMetrics {
        id: keyMetrics
        font: keyFontSource.font
    }

    Row {
        id: cols
        anchors.left: parent.left
        anchors.top: parent.top
        spacing: Theme.spaceXl

        Repeater {
            model: root.columns
            delegate: Column {
                required property var modelData
                spacing: Theme.space2xs

                Repeater {
                    model: modelData.rows
                    delegate: Row {
                        required property var modelData
                        spacing: Theme.spaceMd

                        Text {   // 键：外描边保证压在任意画面上可读（**面板无背景**）
                            width: root.keyColumnWidth
                            elide: Text.ElideRight
                            text: modelData.k
                            color: Theme.textSecondary
                            style: Text.Outline
                            styleColor: Theme.overlayTextOutline
                        }
                        Text {
                            id: val
                            text: modelData.v
                            // 宽度按**实际排版宽度**定（`TextMetrics` 不受 elide 影响；
                            // 用 implicitWidth 会与 width 互相依赖 ⇒ 双幅两列会叠在一起）
                            width: Math.min(valueMetrics.advanceWidth, Theme.osdValueMaxW)
                            elide: Text.ElideRight
                            color: Theme.textPrimary
                            style: Text.Outline
                            styleColor: Theme.overlayTextOutline

                            TextMetrics {
                                id: valueMetrics
                                font: val.font
                                text: val.text
                            }
                        }
                    }
                }
            }
        }
    }
}
