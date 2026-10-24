import QtQuick
import ImageBrowser

// ══ 文件列表行（列表视图）═════════════════════════════════════
//   ┌────┐ 文件名  [类型 tag]                    412 MB      3288×2200
//   └────┘
//   类型 tag = **图标 + 文本**，紧挨标题（只标图片之外的：压缩包 / 文件夹）
Item {
    id: root
    property string name: ""
    property string sizeText: ""       // 文件大小
    property string resText: ""        // 分辨率
    property string kind: "image"        // image | archive | folder
    property string thumb: ""
    property bool   missing: false
    property bool   selected: false      // **选中**（单击；不是进度）
    property bool   isProgress: false    // **进度**落在这一行（"当前进度"标签）
    property int    depth: 0

    signal activated()
    signal openRequested()

    readonly property bool isImage: kind === "image"
    readonly property string kindGlyph: kind === "archive" ? Theme.icZip
                                      : kind === "folder" ? Theme.icFolder : ""
    readonly property string kindLabel: kind === "archive" ? "压缩包"
                                      : kind === "folder" ? "文件夹" : ""

    implicitHeight: content.implicitHeight + Theme.spaceSm * 2

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusSm
        color: rowMa.containsMouse ? Theme.hoverFill
             : root.selected ? Theme.activeFill : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.durFast } }

        Row {
            id: content
            anchors.left: parent.left
            anchors.leftMargin: Theme.colRowPadL + root.depth * Theme.spaceLg
            anchors.right: sizeText.left
            anchors.rightMargin: Theme.colGap    // 让开右侧两列（文件大小 / 分辨率）
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spaceSm

            // 缩略图 / 类型图标
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.thumbSm
                height: Theme.thumbSm
                radius: Theme.radiusXs
                color: Theme.bgSkeleton
                clip: true
                // 图集 / 压缩包同样用**内部第一张图**做缩略图；无图时兜底显示类型图标
                Image {
                    anchors.fill: parent
                    source: root.thumb
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    visible: root.thumb !== "" && status === Image.Ready
                }
                Text {
                    anchors.centerIn: parent
                    visible: root.thumb === ""
                    text: root.kindGlyph
                    font.family: iconFont
                    font.pixelSize: Theme.iconSm
                    renderType: Text.NativeRendering
                    color: Theme.textTertiary
                }
            }

            // 文件名 + 类型 tag（紧挨标题）
            Row {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - Theme.thumbSm - Theme.spaceSm   // = Theme.colNameOffset 的剩余宽
                spacing: Theme.spaceSm

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth,
                                    parent.width
                                        - (typeBadge.visible ? typeBadge.width + Theme.spaceSm : 0)
                                        - (progressBadge.visible ? progressBadge.width + Theme.spaceSm : 0))
                    text: root.name
                    color: root.missing ? Theme.danger : Theme.textPrimary
                    elide: Text.ElideMiddle
                }
                Badge {
                    id: typeBadge
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !root.isImage
                    glyph: root.kindGlyph
                    label: root.kindLabel
                }
                // **进度**（不是选中）：文本标签，随行
                Badge {
                    id: progressBadge
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root.isProgress
                    label: "当前进度"
                    tone: Theme.brandFg
                    toneBg: Theme.brand
                    toneLine: Theme.brand
                }
            }
        }

        // 右侧两列：文件大小 / 分辨率（列宽用 token，表头共用同一组 → 列位对齐）
        Text {
            id: resText
            anchors.right: parent.right
            anchors.rightMargin: Theme.colRowPadR
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.colResW
            horizontalAlignment: Text.AlignRight
            text: root.resText
            color: Theme.textTertiary
        }
        Text {
            id: sizeText
            anchors.right: resText.left
            anchors.rightMargin: Theme.colGap
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.colSizeW
            horizontalAlignment: Text.AlignRight
            text: root.sizeText
            color: Theme.textTertiary
        }

        MouseArea {
            id: rowMa
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.activated()
            onDoubleClicked: root.openRequested()
        }
    }
}
