import QtQuick
import QtQuick.Controls
import ImageBrowser

// ══ 图集卡片 ═══════════════════════════════════════════════════
//   高度**不写死**：内容区（内容行 + 进度条）高 + 上下**等量**内边距。
//   进度条是**流式**的（内容区最后一行），宽度与内容同宽 —— 不浮动、不贴底。
//   原因：QML 的 `clip` 按**矩形**裁剪，圆角容器 + clip 裁不出圆角，"紧贴底部"
//   会让进度条两端戳出卡片圆角；改成流式后上下 padding 视觉一致。
//
//   ── 扫描状态（★ v6：**直接是序列的状态**；★ v8：**没有写入磁盘状态列**）──────
//   · 状态显示在**卡片上**（不是弹出/独立面板），按钮在**右上角同一簇**（移除左边）
//   · **扫描中只给文字**，不加转圈/脉冲组件：
//       空串（没扫过）/ Done → 不显示（正常态，照旧显示阅读进度）
//       NotReady / ImageFound → `扫描中 · 已发现 N`（core 的活序列正在跑）
//       ScanStoped            → `已停止 · N 张`
//   · **失败不写入磁盘**（v8）：根不可用 / 库写失败 / 压缩包打不开 ⇒ **瞬时提示浮层**，
//     所以卡片上没有"扫描失败"态、也没有 `scan_error` tooltip
//   · 操作：进行中 → 「取消扫描」（停活序列 ⇒ **不落库**；**落库窗口内**它被禁用，
//     见 `resetCommitting`）；其余 → 「重新扫描」
//   · 文案由卡片拼（模型只给 `scanState` / `scannedItems` / `resetCommitting` 三个 role）
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

    // ── 扫描（roles 与 LibraryModel::roleNames() 同名）──
    property var    collectionId: -1        // var：集合 id 是 qint64（QML 的 int 只有 32 位）
    property string scanState: ""           // ★ v6：NotReady | ImageFound | Scaned | ScanStoped
    property int    scannedItems: 0         // 已发现多少张图（GROUP BY 现算）
    property bool   resetCommitting: false  // ★ T2c-2b-2：落库窗口（原子替换进行中）

    signal activated()
    signal removeRequested()
    signal renameRequested(string newName)
    // 只**发**动作，不自己调 ViewModel（组件只读属性、只发信号）
    signal scanRequested()
    signal cancelScanRequested()

    //   进行中 = 序列的 NotReady / ImageFound（core 的活序列正在跑）
    readonly property bool scanning: scanState === "NotReady" || scanState === "ImageFound"
    // 空串 = **不在卡片上显示扫描信息**（Scaned 是正常态）
    readonly property string scanText: scanning ? "扫描中 · 已发现 " + scannedItems
                                      : scanState === "ScanStoped"
                                          ? "已停止 · " + scannedItems + " 张"
                                      : ""
    readonly property string scanActionTooltip: scanning ? "取消扫描" : "重新扫描"
    readonly property string scanActionGlyph: scanning ? Theme.icClose : Theme.icScan

    // 键盘可达：Tab 能选中、点击即获焦（弹窗/焦点链都依赖它）
    activeFocusOnTab: true
    // 右侧动作按钮簇占位（两个按钮 + 簇内间距 + 右外边距 + 与文字的间距）
    readonly property int actionReserve: Theme.controlMd * 2 + Theme.spaceXs + Theme.spaceMd * 2

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

                    // 徽标：进度 / 缺失 / 扫描状态
                    Row {
                        id: badges
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.rowGap

                        // 扫描状态（**只给文字**：扫描中 / 已停止）
                        //   ★ v6：失败走**瞬时提示浮层**（不写入磁盘）⇒ 这里没有失败态
                        Item {
                            id: scanSlot
                            visible: root.scanText !== ""
                            implicitWidth: scanBadge.implicitWidth
                            implicitHeight: scanBadge.implicitHeight

                            Badge {
                                id: scanBadge
                                anchors.fill: parent
                                label: root.scanText
                                tone:     Theme.textSecondary
                                toneBg:   Theme.bgTag
                                toneLine: Theme.borderL3
                            }
                        }

                        // 阅读进度：**非扫描态**才显示（done/idle 是正常态，照旧显示进度）
                        Badge {
                            visible: !root.missing && root.scanText === ""
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

    // ── 右上动作簇（标准图标按钮；距上/右边框各 spaceMd）──
    //   声明在 cardMa 之后 → 在上层，点击不被卡片吞掉。
    //   两者都是 IconAction（本身可 Tab 到达 + Enter/Space + 焦点环，见 07 §五.1）。
    Row {
        id: actionRow
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceMd
        anchors.top: parent.top
        anchors.topMargin: Theme.spaceMd
        spacing: Theme.spaceXs

        // 扫描 / 重新扫描 / **取消扫描**（扫描中）。文案见 scanActionTooltip。
        //   ★ T2c-2b-2：**落库窗口内取消被禁用** —— 那时扫描已结束（内容完整），
        //     正在后台原子替换；`cancelReset` 是静默 no-op（用户 2026-10-15 裁决）。
        //     ★ 禁用观感 / 点击与键盘守卫由 **`IconAction` 组件自身**负责（单一来源）：
        //       调用点只写 `enabled:`，不再写 opacity。
        IconAction {
            id: scanBtn
            glyph: root.scanActionGlyph
            tooltip: root.scanActionTooltip
            enabled: !(root.scanning && root.resetCommitting)
            onTriggered: root.scanning ? root.cancelScanRequested() : root.scanRequested()
        }

        // 移除
        IconAction {
            id: trashBtn
            glyph: Theme.icTrash
            danger: true
            tooltip: "移除"
            onTriggered: confirmRemove.open()
        }
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
