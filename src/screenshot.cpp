#include "screenshot.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>

#include "browsemodelview.h"
#include "uimodel.h"
#include "viewermodel.h"

// ══ `--shot` 截图机制（设计 14 §11.1）════════════════════════════════════════
//   原 `src/debugview.cpp` 的截图辅助，随 `--debug-view` 全家删除后独立保留。
//
// 开发期自检：离屏运行时把窗口内容截图存盘，供人工/自动核对。
//   用法： QT_QPA_PLATFORM=offscreen ./image-browser -d <dir> --shot out.png [--delay ms]

namespace screenshot {

void setupShot(QGuiApplication &app, QQmlApplicationEngine &engine,
               const QString &outPath, int delayMs, int shotIndex,
               bool immersive, bool popup, bool osdShown, int seekPage,
               app::UiModel *uiModel)
{
    QTimer::singleShot(delayMs, &app, [&engine, outPath, shotIndex, immersive, popup, osdShown, seekPage, uiModel]() {
        const auto roots = engine.rootObjects();
        if (roots.isEmpty()) { qWarning() << "[shot] 没有根对象"; return; }
        auto *win = qobject_cast<QQuickWindow *>(roots.first());
        if (!win) { qWarning() << "[shot] 根对象不是 QQuickWindow"; return; }

        // 新壳：画的是 `ui.viewer`（阅读屏的图由 viewer 直接给）
        fetch::ViewerModel *vm = uiModel ? uiModel->viewerCore() : nullptr;
        if (shotIndex >= 0 && vm) vm->jump(shotIndex);
        // ★ 浏览的"高亮行 / 打开某行"都归 `ui.browse`
        auto *browse = uiModel ? uiModel->browse() : nullptr;
        if (immersive && browse) browse->open(browse->browseRow());
        if (osdShown && uiModel) uiModel->setOsdVisible(true);
        if (seekPage >= 0 && vm) {
            // 单幅版面下"第 N 页"== 第 N 个条目 ⇒ jump(N) 就是跳到第 N 页
            //   （多幅版面请用 `--dev-open-page`，见开发期 CLI）
            if (browse) browse->open(browse->browseRow());
            // ⚠️ 取图是**异步**的（解码线程取好再 post 回来）⇒ 这里报不出跳转后的页号；
            //    单幅版面下"第 N 页"== 第 N 条 ⇒ 投递 jump(N) 即可。真实页号看下面的 VM 状态。
            vm->jump(seekPage);
            qInfo().noquote() << QStringLiteral("[seek] 已投递 jump(%1)（当前页号 %2，异步刷新）")
                .arg(seekPage).arg(vm->currentPage());
        }
        if (popup && uiModel) {
            if (browse) browse->open(browse->browseRow());
            qWarning().noquote() << QStringLiteral(
                "[shot] ⚠ --show-interval-popup 在新壳上未接线（旧实现调的是已删除的旧 QML 方法）");
        }

        // 再等一帧，让绑定生效（popup 需要更久）
        QTimer::singleShot(popup ? 1400 : 400, win, [win, outPath, uiModel]() {
            // ★ 先确认场景图**真的走了 RHI**：software 后端下 `GpuScalerItem` 什么都不画，
            //   截图里不会有 GPU 缩放画面 —— 这种时候必须**大声报出来**
            if (auto *ri = win->rendererInterface()) {
                const auto api = ri->graphicsApi();
                if (api == QSGRendererInterface::Software)
                    qWarning().noquote()
                        << QStringLiteral("[shot] ⚠ 场景图后端 = software（不是 RHI）⇒ "
                                          "阅读屏截图不含 GPU 缩放画面，请在有 /dev/dri 的环境重跑");
                else
                    qInfo().noquote() << QStringLiteral("[shot] 场景图后端 = RHI（graphicsApi=%1）")
                                             .arg(int(api));
            }
            // 主路径的可观测状态：截图 + 这行日志一起才算"看到"
            if (uiModel) {
                qInfo().noquote() << QStringLiteral("[immersive] 屏 = %1（0=图集库 1=浏览 2=阅读）")
                                         .arg(int(uiModel->screen()));
                if (auto *v = uiModel->viewerCore()) {
                    qInfo().noquote()
                        << QStringLiteral("[immersive] VM 状态：keyCount=%1 cursor=%2 count=%3 "
                                          "totalPages=%4 currentPage=%5 loading=%6 imageA=%7x%8 imageB=%9x%10")
                               .arg(v->keyCount()).arg(v->cursor())
                               .arg(v->itemCount()).arg(v->totalPages()).arg(v->currentPage())
                               .arg(v->loading() ? QStringLiteral("true") : QStringLiteral("false"))
                               .arg(v->imageA().width()).arg(v->imageA().height())
                               .arg(v->imageB().width()).arg(v->imageB().height());
                }
            }
            const QImage img = win->grabWindow();
            if (img.isNull()) { qWarning() << "[shot] grabWindow 返回空"; return; }
            if (img.save(outPath))
                qInfo().noquote() << QStringLiteral("[shot] 已保存 %1  (%2x%3)")
                                         .arg(outPath).arg(img.width()).arg(img.height());
            else
                qWarning() << QStringLiteral("[shot] 保存失败 %1").arg(outPath);
            QCoreApplication::quit();
        });
    });
}

} // namespace screenshot
