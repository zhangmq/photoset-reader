import QtQuick
import QtQuick.Layouts
import ImageBrowser

// ══ 设置项行结构（**只定义一次**）══════════════════════════
//   样式来源：DSH 真实设置弹框 `dsh-client-ui-settings-general/lib/client.js`
//     · `.row{display:flex; justify-content:space-between; align-items:center; gap:24px;
//             padding:16px 0; border-bottom:.5px solid var(--dsw-alias-border-l2)}`
//     · `.title{color:label-primary}`、`.description{color:label-secondary; margin-top:4px}`
//   ⇒ 左「标题 + 描述」两行堆叠、右控件（**垂直居中**）、行底一条 hairline
//
//   用法（**手写项**，控件直接写进来，想加什么加什么）：
//     SettingsRow {
//         label: "幻灯片间隔"
//         description: "幻灯片自动翻页间隔"
//         NumberField { … }
//     }
//   ⚠️ 本项目与 DSH 的差异：行内边距 `spaceMd`(12)（DSH 16 不在本项目间距刻度上）
Item {
    id: root

    property alias label: labelText.text
    property alias description: descText.text
    //   描述颜色：默认次要色；某些行需要表达"当前处于非正常状态"时可改成 `Theme.warn`
    property alias descriptionColor: descText.color

    // 控件槽：实例化时写进来的子项都放这里
    default property alias content: slot.data

    width: parent.width
    // ⚠️ 不能写 `childrenRect.height`：行底 hairline 锚在 `parent.bottom`，
    //    它的位置依赖本行高度 → 高度又依赖 childrenRect（含它）→ **绑定循环**。
    //    只按内容 RowLayout 算高。
    implicitHeight: rowLayout.implicitHeight + Theme.spaceMd * 2

    RowLayout {
        id: rowLayout
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: Theme.spaceMd
        spacing: Theme.spaceLg

        ColumnLayout {
            // ★ 标题**一律顶部对齐**（用户 2026-10-20）：有没有 description 都一样，
            //   否则带描述的行使标题整体下沉半行，逐行看过去标题高低不齐。
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: Theme.spaceXs

            Text {
                id: labelText
                Layout.fillWidth: true
                color: Theme.textPrimary
            }
            Text {
                id: descText
                Layout.fillWidth: true
                visible: text !== ""
                wrapMode: Text.WordWrap
                color: Theme.textSecondary   // 由 root.descriptionColor 覆盖
                font.pointSize: Theme.fontSm
            }
        }

        Item {
            id: slot
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: childrenRect.width
            implicitHeight: childrenRect.height
        }
    }

    Rectangle {   // 行底 hairline（DSH `.row{border-bottom:.5px solid border-l2}`）
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Theme.hairline
        color: Theme.borderL2
    }
}
