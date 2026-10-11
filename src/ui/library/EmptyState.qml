import QtQuick
import ImageBrowser

// ══ 空状态：图集库还没有任何图集 ═══════════════════════════════
//   空态是**唯一**允许放引导文字的地方（其余界面不放帮助文本，见 AGENTS/设计约定）。
//   文案给出两条添加途径，且直接引用界面上的真实按钮名：
//     ① 点「添加图集」直接选目录或压缩包（用户 2026-10-10 补充：压缩包同样可直接添加）
//     ② 或先「打开」它，再在浏览界面收藏
Item {
    id: root

    Column {
        anchors.centerIn: parent
        spacing: Theme.emptyGap

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: Theme.icCollections
            font.family: iconFont
            font.pixelSize: Theme.iconLg
            renderType: Text.NativeRendering
            color: Theme.textTertiary
        }

        Column {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: Theme.spaceXs

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "还没有图集"
                color: Theme.textPrimary
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "点「添加图集」，选择目录或压缩包"
                color: Theme.textSecondary
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "或「打开」后在浏览界面收藏它"
                color: Theme.textSecondary
            }
        }
    }
}
