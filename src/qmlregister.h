#pragma once

// ── QML 类型的**手工注册**（唯一实现，产品与测试共用）────────────────────────
//   为什么不用 `QML_ELEMENT` + `qmltyperegistrar`（2026-11-06 定，理由仍在）：
//     `qmltyperegistrar` 生成的注册文件按 `<gpuscaleritem.h>` 的形式找头文件，
//     而 `src/` 一旦进包含路径，libarchive 的 `<archive.h>` 就会被我们的
//     `src/archive.h` 顶掉。手工 `qmlRegisterType` 不产生这种相对包含。
//
//   ★ 为什么抽成函数而不是留在 `main.cpp`：`AppUi` 模块里的 `FullscreenView` /
//     `Smooth:` 那一路都要用 `GpuScalerItem` / `CpuScalerItem`，**任何**加载
//     `qrc:/qt/qml/AppUi/src/ui/Main.qml` 的进程都必须先注册它们 —— 否则 QML 报
//     "CpuScalerItem is not a type"，接着 `Type FullscreenView unavailable` 连锁失败。
//     产品以前是 `main.cpp` 里两句 inline，测试进程没有那条路径 ⇒ 套件加载不了界面。
//   ★ 调用时机：**在任何 QML 加载之前**（注册是全局表，之后补注册对已解析的类型无效）。
namespace app {

// 幂等：重复调用无副作用（QML 注册表本身允许多次同名注册）。
void registerQmlTypes();

}   // namespace app
