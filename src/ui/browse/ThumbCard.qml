import QtQuick
import ImageBrowser

// ══ 缩略图卡片（网格视图）═════════════════════════════════════
//   ┌──────────┐
//   │ 缩略图  ⊞ │   ← 主视觉始终是**缩略图**（图集/压缩包用内部第一张图）
//   ├──────────┤   ← 右上角小图标：类型标记（非图片）+ 缺失角标
//   │ 文件名    │   ← 单行省略（ElideLeft：漫画文件名区分度在尾部页码）
//   └──────────┘
//   底/描边/hover 与图集库卡片统一（DSH 产出物·文件变更卡）
Item {
    id: root
    property string name: ""
    property string thumb: ""
    property string kind: "image"        // image | archive | folder
    property bool   missing: false
    property bool   selected: false      // **选中**（单击；不是进度）
    property bool   isProgress: false    // **进度**落在这张卡上（"当前进度"标签）

    signal activated()
    signal openRequested()

    readonly property bool isImage: kind === "image"
    readonly property string kindGlyph: kind === "archive" ? Theme.icZip
                                      : kind === "folder" ? Theme.icFolder : ""

    Rectangle {
        id: card
        // 列/行间距由卡片自身 margin 提供（格子是首尾相接的）
        anchors.fill: parent
        anchors.margins: Theme.spaceXs        // 卡片间距（= spaceSm/2，直接用 token）
        radius: Theme.radiusLg                    // DSH 卡片 radius-lg(16)
        color: cardMa.containsMouse ? Theme.cardFillHover : Theme.cardFill
        border.width: root.selected ? Theme.borderSelW : Theme.borderW   // 选中描边（不是焦点环）
        border.color: root.selected ? Theme.brand : Theme.borderL2
        clip: true
        Behavior on color { ColorAnimation { duration: Theme.durFast } }

        // 缩略图区（图片 / 类型图标共用）
        Item {
            id: thumbArea
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: nameText.top
            anchors.margins: Theme.spaceSm
            anchors.bottomMargin: Theme.spaceXs

            // 主视觉：缩略图（图集 / 压缩包同样用**内部第一张图**）
            Image {
                anchors.fill: parent
                source: root.thumb
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                visible: root.thumb !== "" && status === Image.Ready
                opacity: root.missing ? Theme.dimOpacity : 1.0
            }
            // 无缩略图时的兜底：类型图标（非图片）→ 通用图片占位
            Text {
                anchors.centerIn: parent
                visible: root.thumb === ""
                text: root.isImage ? Theme.icAlbum : root.kindGlyph
                font.family: iconFont
                font.pixelSize: Theme.iconLg
                renderType: Text.NativeRendering
                color: root.missing ? Theme.danger : Theme.textTertiary
            }
        }

        Text {
            id: nameText
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: Theme.spaceSm
            anchors.topMargin: 0
            text: root.name
            color: root.missing ? Theme.danger : Theme.textSecondary
            // 漫画文件名公共前缀很长，区分度在**尾部页码** → 用 ElideLeft 保留尾部
            elide: Text.ElideLeft
            horizontalAlignment: Text.AlignHCenter
        }

        // 左上角：**当前进度**（文本标签；与"选中描边"是两件事）
        Badge {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.margins: Theme.spaceSm
            visible: root.isProgress
            label: "当前进度"
            tone: Theme.brandFg          // 主色底 + 反色字（够醒目）
            toneBg: Theme.brand
            toneLine: Theme.brand
        }

        // 右上角小标记：类型图标（非图片）+ 缺失角标
        //   距卡片边 = **卡片内边距 `spaceSm`**（与缩略图同一条内边线，视觉上对齐）
        Row {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: Theme.spaceSm
            spacing: Theme.spaceXs

            // 类型图标（非图片）：**纯图标、不套框**；用默认图标大小 `iconMd`
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: !root.isImage
                text: root.kindGlyph
                font.family: iconFont
                font.pixelSize: Theme.iconMd
                renderType: Text.NativeRendering
                color: Theme.brand              // 主色（primary = brand，**不是** textPrimary）
            }
            Rectangle {                                   // 缺失角标
                visible: root.missing
                width: Theme.markSm; height: Theme.markSm
                radius: Theme.radiusXs
                color: Theme.tagErrBg
                border.width: Theme.borderW
                border.color: Theme.tagErrLine
                Text {
                    anchors.centerIn: parent
                    text: Theme.icAlert
                    font.family: iconFont
                    font.pixelSize: Theme.iconSm
                    renderType: Text.NativeRendering
                    color: Theme.tagErrText
                }
            }
        }

        MouseArea {
            id: cardMa
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.activated()
            onDoubleClicked: root.openRequested()
        }
    }
}
