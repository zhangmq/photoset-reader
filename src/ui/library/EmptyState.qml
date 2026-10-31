import QtQuick
import AppUi

// ══ 空态块（**唯一允许放引导文字的地方**）═══════════════════════════════
//   什么时候出现（设计 18 §1 / §3）：
//     · 没有来源 ⇒ Browse 的内容区（"无来源 = Browse + 空态块"）
//     · 图集库抽屉里一个收藏都没有 ⇒ 抽屉的内容区
//   文案是**用户给定**的（2026-10-25，一字不改）；按钮 = 「打开一个文件夹」，
//   ★ 收藏当前文件夹 / 压缩包（收藏入口在 Viewer 顶栏，段 3 接；本段只是说明文案）。
//   ★ 空态不复述快捷键、不放帮助文本（AGENTS：界面不放帮助文本，空态除外）。
Item {
    id: root

    signal folderRequested()

    // 文案换行宽：不超过容器宽 - 左右 padding（宽屏上是单行，抽屉里会折行）
    readonly property real textMaxW: Math.max(Theme.spaceLg,
                                              root.width - Theme.spaceLg * 2)

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
                text: "目前还没有收藏"
                color: Theme.textPrimary
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(implicitWidth, root.textMaxW)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: "点 ★ 收藏当前文件夹 / 压缩包。"
                color: Theme.textSecondary
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(implicitWidth, root.textMaxW)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: "之后可以从这里直接打开它，并且会接着上次的阅读位置继续看。"
                color: Theme.textSecondary
            }
        }

        Button {
            anchors.horizontalCenter: parent.horizontalCenter
            glyph: Theme.icFolderOpen
            label: "打开一个文件夹"
            onClicked: root.folderRequested()
        }
    }
}
