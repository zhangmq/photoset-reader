import QtQuick
import ImageBrowser

// ══ 瞬时提示（浮层）════════════════════════════════════════════
//   用途：**失败与不支持**一律走它（瞬时提示 + 日志，**不写入磁盘**）——
//         根不可用 / 库写失败 / 库打不开 / 压缩包打不开或不支持。
//
//   纪律（照 docs/design/07-UI设计经验总结.md）：
//     · **不占位**：锚在窗口底部居中，覆盖在内容之上；显隐**不改变任何布局、不引起重排**
//     · **跟随主题** + **不透明 `bgBase`**（能靠"行为"自动消失解决，就不用半透明）
//     · 进出动画 = `NumberAnimation 300ms Easing.OutCubic`
//     · 几秒后**自动消失**（这里用 QtQuick `Timer` 只服务"提示自己消失"，
//       与"幻灯片推进禁 Timer"那条规矩无关）
//   ⚠️ 颜色/间距一律取 `Theme` 令牌，不写裸 hex / 魔法数字。
Item {
    id: root

    property string text: ""
    // `token` 每次 +1 ⇒ 同一条文案再次出现也能重新触发动画（来自 `ui.notificationSeq`）
    property int token: 0

    readonly property bool shown: text !== "" && opacity > 0.01

    // ⚠️ 不写 `anchors.fill`：浮层**不占位**，尺寸只由内容决定（父级负责摆放）
    implicitWidth: box.implicitWidth
    implicitHeight: box.implicitHeight
    visible: opacity > 0.001
    z: 1000                                    // 盖在三屏之上（三屏都在 z 默认 0）

    function show(t, tok) {
        if (t === "") return;
        text = t;
        token = tok;
        hideTimer.restart();
        opacity = 1;
    }

    Rectangle {
        id: box
        anchors.fill: parent
        // 不透明 `bgBase`（07 §六）+ 描边分层（同卡片：border-l2）
        color: Theme.bgBase
        radius: Theme.radiusMd
        border.width: Theme.borderW
        border.color: Theme.borderL2

        implicitWidth: label.implicitWidth + Theme.cardPadH * 2
        implicitHeight: label.implicitHeight + Theme.spaceSm * 2

        Text {
            id: label
            anchors.centerIn: parent
            width: Math.min(implicitWidth, 720)     // 长文案换行，不让浮层横穿整屏
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            text: root.text
            color: Theme.textPrimary
        }
    }

    // 淡入：300ms OutCubic（07 §六）
    Behavior on opacity {
        NumberAnimation { duration: 300; easing.type: Easing.OutCubic }
    }
    // 出现时轻微上浮（同一条曲线；纯视觉，不影响布局）
    Behavior on y {
        NumberAnimation { duration: 300; easing.type: Easing.OutCubic }
    }

    Timer {
        id: hideTimer
        interval: 4000                              // 几秒后自动消失
        onTriggered: root.opacity = 0
    }
}
