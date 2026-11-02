import QtQuick
import AppUi

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

    //   上一次读的时候"这一屏长什么样"：**起点 / 本屏位置数 / 版面**。
    //   兜底轮询拿它做比较 —— 只看 `cursor` 不够：双幅↔单幅切换时起点可能不变
    //   （同一页还是同一页），但一屏的位置数变了，OSD 必须重读（用户 2026-11-02 实测到）。
    property string shownKey: ""

    function refresh() {
        if (!ui.viewer || !root.osdVisible) {
            columns = []
            shownKey = ""
            return
        }
        shownKey = root.screenKey()
        columns = ui.viewer.osdRows()
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
    function schedule() {
        if (root.osdVisible) refreshDebounce.restart()
    }
    //   "这一屏长什么样"的标识：起点 + 位置数 + 版面（三个都在换屏/换版面时变）
    function screenKey() {
        if (!ui.viewer) return ""
        return ui.viewer.cursor + "/" + ui.viewer.keyCount + "/" + ui.viewer.doublePage
    }

    //   ★ 兜底轮询（**只在 OSD 显示期间跑**）：每 200ms 对一次"本屏起点"。
    //     正常路径是下面的 Connections（换屏 / 图备好就刷新）；这一条只保证
    //     **信号漏了也不会一直停在旧内容**——去抖重启会把"来不及静默 40ms"变成"永不刷新"，
    //     而这里不参与去抖，只看游标本身（读一个 int，没有 I/O）。
    Timer {
        interval: 200
        repeat: true
        running: root.osdVisible
        onTriggered: {
            if (ui.viewer && root.screenKey() !== root.shownKey)
                root.refresh()
        }
    }

    Connections {
        //   ★ 语言一变就重读：OSD 的行是 `osdRows()` 在 JS 里**算出来的**（不是绑定），
        //     引擎重算 `qsTr` 绑定管不到它 ⇒ 不重读会一直停在旧语言（用户 2026-11-02 实测到）。
        target: ui
        function onLanguageChanged() { root.refresh() }
    }

    Connections {
        target: ui.viewer
        // 换屏 ⇒ 文件名 / 序号 / 尺寸 变了；图备好 ⇒ 尺寸 / 类型 / 色彩空间 补齐
        function onCurrentChanged() { root.schedule() }
        function onImageAChanged() { root.schedule() }
        function onImageBChanged() { root.schedule() }
        //   ★ 换版面：`cursor` 常常不变（同一页还是那一页），但一屏的位置数变了 ⇒ 也要重读。
        //     这一条**不看画面有没有备好**——OSD 是"我在哪一屏"的信息，不该等到解码完才更新。
        function onDoublePageChanged() { root.refresh() }
    }

    implicitHeight: cols.height
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

    //   ★ 两列的摆放（用户 2026-11-02）：**按 OSD 自己的 bounding box 对齐** ——
    //     左列贴左边、右列贴右边（双幅时两列分别落在左右两页的外沿）；
    //     单幅只有一列 ⇒ 永远贴左。两列之间的间距由此**自动**产生，不再用固定 spacing。
    Item {
        id: cols
        anchors.left: parent.left
        anchors.top: parent.top
        width: parent.width
        height: childrenRect.height

        Repeater {
            model: root.columns
            delegate: Column {
                required property var modelData
                required property int index
                spacing: Theme.space2xs
                //   左列 x = 0；右列 x = 盒宽 − 本列宽（**贴着右沿**，不靠猜列宽）
                x: index === 0 ? 0 : cols.width - implicitWidth

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
