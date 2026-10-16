#pragma once

// ══ `--shot` 截图机制（设计 14 §11.1）════════════════════════════════════════
//   原 `debugview` 的截图辅助**独立保留**：`--debug-view` 全家已删，但 `--shot`
//   的主路径与验收截图都在用（库屏 / `--dev-open-collection` / `--dev-open-page`）。
//   ★ 只保留截图机制本身（`--shot` / `--delay` / `--index` / `--immersive` /
//     `--show-interval-popup` / `--show-osd` / `--seek-test`）；
//     逻辑/文案与改名前的 `setupScreenshot` **一字不差**。
//   ★ 不是测试：随 `image-browser` 一起编（不进 `ibcore`、不链 `testsupport`）。

#include <QString>

class QGuiApplication;
class QQmlApplicationEngine;
namespace app { class UiModel; }

namespace screenshot {

// 延时截图存盘（主路径与验收共用）。
//   调用方只在 `--shot` 给出非空路径时调用。
void setupShot(QGuiApplication &app, QQmlApplicationEngine &engine,
               const QString &outPath, int delayMs, int shotIndex,
               bool immersive, bool popup, bool osdShown, int seekPage,
               app::UiModel *uiModel);

} // namespace screenshot
