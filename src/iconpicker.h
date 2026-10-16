#pragma once

// ══ 图标挑选工具 `ib-iconpicker`（开发用；设计 14 §11.1）════════════════════
//   从应用 CLI 的 `--debug-view iconpicker` **独立成目标**：自带 `main()`、
//   加载保留的 `src/ui/debug/IconPicker.qml`（QML 页保留），**不是测试**
//   （不进 `tests/`、不挂 `add_test`）。
//   ★ QML 资源（ImageBrowser 模块 + 图标字体）随本目标挂载；实现见
//     `src/iconpicker.cpp`。

// 加载 `IconPicker.qml` 并进入事件循环；返回进程退出码（QML 加载失败 = 1）。
int runIconPicker(int argc, char *argv[]);
