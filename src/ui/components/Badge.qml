import QtQuick
import ImageBrowser

// 小徽标（紧跟标题，用于进度 / 缺失 / 类型）
Rectangle {
    id: root
    property string label: ""
    property string glyph: ""        // 可选前导图标（类型 tag = 图标 + 文本）
    property color  tone: Theme.textSecondary      // 文字色
    property color  toneBg: Theme.bgTag            // 底色
    property color  toneLine: Theme.borderL3       // 边框色（普通/错误不同）

    implicitWidth: badgeText.implicitWidth + Theme.badgePadH
                   + (root.glyph !== "" ? glyphText.implicitWidth + Theme.spaceXs : 0)
    // 高度由文本行盒推导（DSH `.rowTag { padding: 1px 6px }` 的模型）
    implicitHeight: badgeText.implicitHeight + Theme.space2xs
    radius: Theme.radiusXs
    color: root.toneBg
    // 描边：普通 badge 用 border-l3，错误 badge 用 state-error（← DSH 的 rowTag / brokenBadge）
    border.width: Theme.borderW
    border.color: root.toneLine

    Row {
        anchors.centerIn: parent
        spacing: Theme.spaceXs

        Text {
            id: glyphText
            anchors.verticalCenter: parent.verticalCenter
            visible: root.glyph !== ""
            text: root.glyph
            font.family: iconFont
            font.pixelSize: Theme.iconSm
            renderType: Text.NativeRendering
            color: root.tone
        }
        Text {
            id: badgeText
            anchors.verticalCenter: parent.verticalCenter
            text: root.label
            color: root.tone
            font.pointSize: Theme.fontSm        // 小一号（次要信息）
        }
    }
}
