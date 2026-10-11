import QtQuick
import ImageBrowser

// ══ 列表视图表头（表格列名）═══════════════════════════════════
//   列几何与行**共用 Theme 的 `col*` token** —— 唯一定义处，见 §1.2「算法单一事实源」：
//     · 名称列左沿   = `colListPad` + `colNameOffset`
//     · 分辨率右沿   = `colListPad` + `colRowPadR`
//     · 文件大小右沿 = 上一列右沿 + `colResW` + `colGap`
Item {
    id: root

    implicitHeight: Theme.controlMd

    Text {
        anchors.verticalCenter: parent.verticalCenter
        x: Theme.colListPad + Theme.colNameOffset
        text: "名称"
        color: Theme.textTertiary
    }
    Text {
        anchors.verticalCenter: parent.verticalCenter
        anchors.right: parent.right
        anchors.rightMargin: Theme.colListPad + Theme.colRowPadR
        width: Theme.colResW
        horizontalAlignment: Text.AlignRight
        text: "分辨率"
        color: Theme.textTertiary
    }
    Text {
        anchors.verticalCenter: parent.verticalCenter
        anchors.right: parent.right
        anchors.rightMargin: Theme.colListPad + Theme.colRowPadR + Theme.colResW + Theme.colGap
        width: Theme.colSizeW
        horizontalAlignment: Text.AlignRight
        text: "文件大小"
        color: Theme.textTertiary
    }

    Rectangle {   // 表头下边框
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Theme.hairline
        color: Theme.borderL1
    }
}
