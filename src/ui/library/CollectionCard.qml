import QtQuick
import QtQuick.Controls
import ImageBrowser

// ══ 图集卡片 ═══════════════════════════════════════════════════
//   高度**不写死**：内容区（内容行 + 进度条）高 + 上下**等量**内边距。
//   进度条是**流式**的（内容区最后一行），宽度与内容同宽 —— 不浮动、不贴底。
//   原因：QML 的 `clip` 按**矩形**裁剪，圆角容器 + clip 裁不出圆角，"紧贴底部"
//   会让进度条两端戳出卡片圆角；改成流式后上下 padding 视觉一致。
Rectangle {
    id: root

    property string name: ""
    property string path: ""
    property string addedAt: ""
    property string lastViewedAt: ""
    property real   progressValue: 0
    property string progressText: ""
    property bool   missing: false
    property string cover: ""

    signal activated()
    signal removeRequested()
    signal renameRequested(string newName)

    // 键盘可达：Tab 能选中、点击即获焦（弹窗/焦点链都依赖它）
    activeFocusOnTab: true
    // 右侧动作按钮占位（按钮宽 + 右外边距 + 与文字的间距）
    readonly property int actionReserve: Theme.controlMd + Theme.spaceMd * 2

    implicitHeight: contentCol.implicitHeight + Theme.cardPadV * 2
    radius: Theme.radiusLg                     // DSH 卡片 radius-lg(16)
    // 底与 hover 都取 DSH 产出物卡的不透明色 → 直接换色即可，不需要叠加层
    color: cardMa.containsMouse ? Theme.cardFillHover : Theme.cardFill
    border.width: Theme.borderW
    border.color: Theme.borderL2               // DSH 卡片描边 = border-l2
    Behavior on color { ColorAnimation { duration: Theme.durFast } }

    // 卡片点击区：**必须声明在内容之前** —— 否则它盖住整张卡，
    //   标题（点击改名）等内部可点区域永远收不到点击。
    //   放在内容之下后：点在内部控件上由它们消费，点在其它地方才落到卡片。
    MouseArea {
        id: cardMa
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onPressed: root.forceActiveFocus()
        onClicked: root.activated()
    }

    // ══ 内容区（上下内边距由卡片给出）════════════════════════
    Column {
        id: contentCol
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: Theme.cardPadH
        anchors.rightMargin: Theme.cardPadH
        anchors.topMargin: Theme.cardPadV
        spacing: Theme.spaceMd

        Row {
            id: contentRow
            width: parent.width
            spacing: Theme.spaceMd

            // ── 封面 ──
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.coverSize
                height: Theme.coverSize
                radius: Theme.radiusSm
                color: Theme.bgSkeleton            // 图片占位底 ← DSH --dsw-alias-bg-skeleton
                clip: true
                opacity: root.missing ? Theme.dimOpacity : 1.0

                Image {
                    anchors.fill: parent
                    source: root.cover
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    visible: root.cover !== "" && status === Image.Ready
                }
                Text {
                    anchors.centerIn: parent
                    visible: root.cover === ""
                    text: Theme.icAlbum
                    font.family: iconFont
                    font.pixelSize: Theme.iconLg          // 封面占位：比常规图标大一档（18 → 40）
                    renderType: Text.NativeRendering
                    color: Theme.textTertiary
                }
            }

            // ── 文字区 ──
            //   右侧要让开浮动的删除按钮（iconBtnSm + trashMargin）
            Column {
                id: textCol
                width: parent.width - Theme.coverSize - Theme.spaceMd - root.actionReserve
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.titleGap

                // 标题 + 徽标（紧挨着，不占满整行）
                Row {
                    id: titleRow
                    width: textCol.width      // ★ 必须显式给宽度，否则子项算宽度时 parent.width=0
                    spacing: Theme.badgeGap

                    // 标题：hover 圆角高亮 / tooltip / 点击可改
                    Item {
                        id: titleArea
                        property bool editing: false
                        // 键盘可达（实际改名功能尚未实现，Enter/Space 与点击同效）
                        activeFocusOnTab: true
                        width: Math.max(Theme.spaceLg, Math.min(titleText.implicitWidth,
                                                                titleRow.width - badges.width - Theme.badgeGap))
                        height: titleText.implicitHeight + Theme.spaceXs

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: -Theme.spaceXs
                            radius: Theme.radiusSm
                            color: titleMa.containsMouse ? Theme.hoverFill : "transparent"
                            border.width: (titleArea.activeFocus && ui.inputFromKeyboard) ? Theme.focusRingW : 0
                            border.color: Theme.brand
                            visible: !titleArea.editing
                            Behavior on color { ColorAnimation { duration: Theme.durFast } }
                        }

                        Text {
                            id: titleText
                            anchors.verticalCenter: parent.verticalCenter
                            visible: !titleArea.editing
                            width: parent.width
                            text: root.name
                            color: root.missing ? Theme.textSecondary : Theme.textPrimary
                            elide: Text.ElideRight
                        }

                        // 内联编辑
                        TextField {
                            anchors.fill: parent
                            visible: titleArea.editing
                            text: root.name
                            selectByMouse: true
                            onAccepted: { root.renameRequested(text); titleArea.editing = false }
                            onEditingFinished: titleArea.editing = false
                            Keys.onEscapePressed: titleArea.editing = false
                        }

                        MouseArea {
                            id: titleMa
                            anchors.fill: parent
                            hoverEnabled: true
                            enabled: !titleArea.editing
                            cursorShape: Qt.IBeamCursor
                            onPressed: titleArea.forceActiveFocus()
                            onClicked: { titleArea.editing = true; }
                        }

                        // 占位：与点击同效（改名功能未实现）
                        Keys.onReturnPressed: titleArea.editing = true
                        Keys.onEnterPressed: titleArea.editing = true
                        Keys.onSpacePressed: titleArea.editing = true

                        HintBubble {
                            anchors.fill: parent
                            text: "点击可修改"
                            hovered: titleMa.containsMouse && !titleArea.editing
                        }
                    }

                    // 徽标：进度 / 缺失
                    Row {
                        id: badges
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.rowGap

                        Badge {
                            visible: !root.missing
                            label: root.progressText      // 用 Badge 默认色（= DSH rowTag）
                        }
                        Badge {
                            visible: root.missing
                            label: "缺失"
                            tone: Theme.tagErrText
                            toneBg: Theme.tagErrBg
                            toneLine: Theme.tagErrLine
                        }
                    }
                }
                Text {
                    width: parent.width
                    text: root.path
                    color: Theme.textSecondary
                    elide: Text.ElideMiddle
                }
                Text {
                    width: parent.width
                    color: Theme.textSecondary
                    elide: Text.ElideRight
                    text: "加入时间 " + root.addedAt + "  ·  浏览时间 " + root.lastViewedAt
                }
            }
        }

        // ── 进度条（流式：内容区最后一行，宽度 = 内容宽度）──
        Rectangle {
            id: progressTrack
            width: parent.width
            height: Theme.progressH
            radius: Theme.radiusPill          // 会被钳到 height/2，即胶囊
            color: Theme.hoverFill            // 卡片面上的半透明填充（DSH interactive-bg-hover）

            Rectangle {
                width: parent.width * Math.max(0, Math.min(1, root.progressValue))
                height: parent.height
                radius: Theme.radiusPill
                color: root.missing ? Theme.danger : Theme.brand
                visible: !root.missing
            }
        }
    }

    Keys.onReturnPressed: root.activated()
    Keys.onEnterPressed: root.activated()
    Keys.onSpacePressed: root.activated()

    // ── 移除（标准图标按钮，右上角；距上/右边框各 spaceMd）──
    //   声明在 cardMa 之后 → 在上层，点击不被卡片吞掉
    IconAction {
        id: trashBtn
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceMd
        anchors.top: parent.top
        anchors.topMargin: Theme.spaceMd
        glyph: Theme.icTrash
        danger: true
        tooltip: "移除"
        onTriggered: confirmRemove.open()
    }

    // ── 移除确认（标准确认对话框）──
    ConfirmDialog {
        id: confirmRemove
        title: "移除图集"
        message: "「" + root.name + "」将从图集库中移除，它的浏览进度也一并删除。\n（源文件不会被删除）"
        acceptText: "移除"
        rejectText: "取消"
        danger: true
        onAccepted: root.removeRequested()
    }

    // ── 焦点环（卡片自身可获焦）──
    Rectangle {
        anchors.fill: parent
        radius: root.radius
        color: "transparent"
        border.width: Theme.focusRingW
        border.color: Theme.brand
        visible: root.activeFocus && ui.inputFromKeyboard
    }
}
