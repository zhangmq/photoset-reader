import QtQuick
//   ⚠️ 版本号**必须写**（6.9）：在 AOT 编译过的模块里，无版本 `import QtQuick.Effects`
//   会运行期报 "RectangularShadow is not a type"（编译期解析得到、运行期不认）。
import QtQuick.Effects 6.9
import AppUi

// ══ 面板投影（用户 2026-11-06）════════════════════════════════════════════
//   菜单 / 对话框"浮在内容之上"的那点高度感统一在这里给（**一处调参**）。
//   ★ 用 QtQuick.Effects 的 `RectangularShadow`：它**只画影子**、不重绘面板本身
//     ⇒ 半透明面板（`Theme.menuFill` / `bgLayer2`）不会被叠出第二层颜色。
//     （`MultiEffect` 做不到这点：阴影总是附带源内容一起画。）
//   ★ 用法：垫在面板矩形**下面**（`z: -1`），`anchors.fill` 与 `radius` 跟面板一致。
//     ⚠️ 影子会画到面板之外 ⇒ 外层不许 `clip`（Qt Quick Controls 的 Popup 默认不裁，正好）。
//   ⚠️ `RectangularShadow` 要 Qt 6.9+（CMake 的 `find_package(Qt6 …)` 版本下限就是它）。
RectangularShadow {
    id: root

    //   与面板一致（调用方一般写 `radius: panel.radius`）
    radius: Theme.radiusLg
    //   DSH `--dsw-elevation-prominent`（0 3px 8px + 0 0 20px）是给**浅色**背景调的；
    //   这里背后常常是图片内容，压得太轻会看不见，所以取"一档更实"的值。
    blur: 28
    offset: Qt.point(0, 8)
    color: Qt.rgba(0, 0, 0, 0.45)
}
