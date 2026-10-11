import QtQuick
import ImageBrowser

// 调试用：把候选图标连同码点一起渲染出来，便于挑选
Item {
    id: root
    property var items: []

    Rectangle {
        anchors.fill: parent
        color: Theme.bgBase

        Grid {
            anchors.centerIn: parent
            columns: 5
            spacing: 24

            Repeater {
                model: root.items
                delegate: Column {
                    spacing: 8
                    Rectangle {
                        width: 72; height: 72
                        radius: Theme.radiusMd
                        color: Theme.bgCard
                        border.width: 1
                        border.color: Theme.borderL1
                        Text {
                            anchors.centerIn: parent
                            text: modelData.cp
                            font.family: iconFont
                            font.pixelSize: 32
                            renderType: Text.NativeRendering
                            color: Theme.textPrimary
                        }
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: modelData.name
                        color: Theme.textSecondary
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: modelData.hex
                        color: Theme.textCaption
                    }
                }
            }
        }
    }
}
