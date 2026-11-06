import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AppUi

// ══ 关于（用户 2026-11-06）════════════════════════════════════════════════
//   · 入口 = 右键菜单「关于」；
//   · 内容 = 图标 / 名称 / 版本 / 仓库地址（点地址用系统浏览器打开）；
//   · 版本号**不另写一份**：读 `Qt.application.version`（`main.cpp` 里
//     `setApplicationVersion(PHOTOSET_VERSION)`），而 `PHOTOSET_VERSION` 来自 CMake 的
//     `project(VERSION …)` + 预发布后缀 —— 与 `--version`、包名同一个来源。
//   · 外观跟设置框一致（`bgLayer2` + `radiusPanel` + 投影）；Esc / 点外部关闭。
Popup {
    id: root

    readonly property string repoUrl: "https://github.com/zhangmq/photoset-reader"

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 0
    width: 400

    //   掩膜只吃输入、不压暗（与设置框同一口径：背后画面保持可对照）
    Overlay.modal: MouseArea {
        acceptedButtons: Qt.AllButtons
        onWheel: (w) => { w.accepted = true }
    }

    //   居中（与设置框同一算法：先算窗口坐标，再换回本浮层父项坐标）
    onOpened: {
        var win = parent ? parent.Window.window : null
        if (win && parent) {
            var p = win.contentItem.mapToItem(parent, (win.width - width) / 2,
                                              (win.height - height) / 2)
            x = p.x
            y = p.y
            contentItem.forceActiveFocus()
        }
    }

    background: Item {
        Rectangle {
            id: panel
            anchors.fill: parent
            color: Theme.bgLayer2
            radius: Theme.radiusPanel
            border.width: Theme.borderW
            border.color: Theme.borderL4
        }
        SurfaceShadow {
            anchors.fill: panel
            radius: panel.radius
            z: -1
        }
    }

    contentItem: Item {
        //   ★ 加上下留白：`ColumnLayout` 的 implicitHeight 只算子项（anchors.margins 不进隐式尺寸），
        //     漏掉这 2×spaceXl 会让弹窗矮 40px、底部按钮被裁。
        implicitHeight: body.implicitHeight + Theme.spaceXl * 2

        ColumnLayout {
            id: body
            anchors.fill: parent
            anchors.margins: Theme.spaceXl
            spacing: Theme.spaceMd

            Image {                                   // 应用图标（与应用 / 任务栏同一个资源）
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 96
                Layout.preferredHeight: 96
                source: "qrc:/icons/photoset-reader-256.png"
                sourceSize.width: 192
                sourceSize.height: 192
                smooth: true
                fillMode: Image.PreserveAspectFit
            }

            Text {                                    // 名称（不翻译：产品名）
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                horizontalAlignment: Text.AlignHCenter
                text: "photoset-reader"
                color: Theme.textPrimary
                font.pixelSize: Qt.application.font.pointSize + 7
                font.weight: Font.DemiBold
            }

            Text {                                    // 版本（唯一来源见文件头）
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Version %1").arg(Qt.application.version)
                color: Theme.textSecondary
            }

            Rectangle {                               // 分隔线
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceSm
                height: Theme.hairline
                color: Theme.borderL2
            }

            RowLayout {                               // 仓库地址（可点，走系统浏览器）
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                spacing: Theme.spaceSm

                Text {
                    text: qsTr("Repository")
                    color: Theme.textCaption
                }
                Text {
                    id: link
                    Layout.fillWidth: true
                    text: root.repoUrl
                    color: linkMa.containsMouse ? Theme.brand : Theme.textSecondary
                    font.underline: linkMa.containsMouse
                    elide: Text.ElideRight
                    MouseArea {
                        id: linkMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: Qt.openUrlExternally(root.repoUrl)
                    }
                }
            }

            //   * 2026-11-07（用户）：许可证与第三方声明放进「关于」。
            //     链接指向仓库里的文件（随包安装的那两份是文档，不重复编进资源）。
            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                spacing: Theme.space2xs

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceSm
                    Text { text: qsTr("License"); color: Theme.textCaption }
                    Text {
                        Layout.fillWidth: true
                        text: "MIT · " + qsTr("view source")
                        color: licMa.containsMouse ? Theme.brand : Theme.textSecondary
                        font.underline: licMa.containsMouse
                        elide: Text.ElideRight
                        MouseArea {
                            id: licMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: Qt.openUrlExternally(root.repoUrl + "/blob/main/LICENSE")
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceSm
                    Text { text: qsTr("Third-party"); color: Theme.textCaption }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("components and licenses")
                        color: tpMa.containsMouse ? Theme.brand : Theme.textSecondary
                        font.underline: tpMa.containsMouse
                        elide: Text.ElideRight
                        MouseArea {
                            id: tpMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: Qt.openUrlExternally(root.repoUrl + "/blob/main/THIRD-PARTY.md")
                        }
                    }
                }
            }

            Button {
                Layout.alignment: Qt.AlignRight
                Layout.topMargin: Theme.spaceSm
                label: qsTr("Close")
                onClicked: root.close()
            }
        }

        IconAction {                                  // 关闭（右上角）
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: Theme.spaceMd
            glyph: Theme.icClose
            tooltip: qsTr("Close")
            onTriggered: root.close()
        }
    }
}
