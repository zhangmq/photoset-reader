import QtQuick
import ImageBrowser

// ══ 全屏 · OSD 信息面板（浮层，键值两列）═══════════════════════
//   用户 2026-10-11 指定内容：图集信息、当前文件夹、图片尺寸、类型、分辨率、**EXIF**
//   · **只被「OSD 切换」控制，不随 UI 显隐**（用户明确）—— 控件条自动淡出时本面板不动
//   · ★ **本面板是「无 UI 时的完整上下文信息」，要求不缺失**（用户 2026-10-11 明确）：
//     它**不受 §1.2「不重复」约束** —— 顶栏那点进度/路径是"UI 在时"的提示，
//     本面板是"UI 隐去后"的唯一信息来源，**该有的全都要有**（序号/文件/文件夹/图集/尺寸/类型/分辨率/EXIF）
//   · **无背景**（用户 2026-10-11：不要背景）→ 文本一律带 **outline**（`Text.Outline`
//     + `overlayTextOutline`），压在任意画面上都可读；键列暗、值列亮
//   · 数据未接入的字段显示 "—"（UI 先做全，数据后续接；需求见设计 §3.9）
Item {
    id: root

    property bool osdVisible: false

    // ── 已可用的数据（ViewModel）──
    property string collectionLabel: ""     // 图集信息（待接图集库模型）
    property string positionLabel: ""       // 序号/总数
    property string pathLabel: ""           // 当前文件相对图集根的路径（A!/B）
    property string fileLabel: ""           // 文件名
    property string folderLabel: ""         // 当前文件夹
    property string sizeLabel: ""           // 尺寸：**文件字节大小**（如 1.2 MB）——待接
    property string resolutionLabel: ""     // 分辨率：**像素尺寸**（如 3288×2200）
    property string kindLabel: ""           // 类型（如 JPEG）
    // ── EXIF（待接）──
    property string cameraLabel: ""
    property string shotAtLabel: ""
    property string exposureLabel: ""
    property string apertureLabel: ""
    property string isoLabel: ""
    property string focalLabel: ""

    readonly property var rows: [
        { k: "序号",   v: positionLabel },
        { k: "文件",   v: fileLabel },
        { k: "路径",   v: pathLabel },
        { k: "文件夹", v: folderLabel },
        { k: "图集",   v: collectionLabel },
        { k: "尺寸",   v: sizeLabel },          // 文件字节大小
        { k: "类型",   v: kindLabel },
        { k: "分辨率", v: resolutionLabel },      // 像素尺寸
        { k: "相机",   v: cameraLabel },
        { k: "拍摄",   v: shotAtLabel },
        { k: "曝光",   v: exposureLabel },
        { k: "光圈",   v: apertureLabel },
        { k: "ISO",    v: isoLabel },
        { k: "焦距",   v: focalLabel }
    ]

    implicitWidth: col.implicitWidth
    implicitHeight: col.implicitHeight
    opacity: root.osdVisible ? 1.0 : 0.0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: Theme.durMed; easing.type: Easing.OutCubic } }

    Column {
        id: col
        anchors.centerIn: parent
        spacing: Theme.space2xs

        Repeater {
            model: root.rows
            delegate: Row {
                required property var modelData
                spacing: Theme.spaceMd

                Text {   // 键：外描边保证压在任意画面上可读（**面板无背景**，用户 2026-10-11）
                    width: Theme.osdKeyW
                    text: modelData.k
                    color: Theme.textSecondary        // 键列暗、值列亮（都用 DSH alias）
                    style: Text.Outline
                    styleColor: Theme.overlayTextOutline
                }
                Text {
                    text: modelData.v !== "" ? modelData.v : "—"
                    color: Theme.textPrimary
                    style: Text.Outline
                    styleColor: Theme.overlayTextOutline
                }
            }
        }
    }
}
