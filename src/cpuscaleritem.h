#pragma once

#include <QImage>
#include <QQuickPaintedItem>
#include <QSize>

#include "gpuscale.h"

#include <memory>

// ══ CPU 缩放渲染组件（QML 可用）════════════════════════════════════════════
//
// 与 `GpuScalerItem` **同构**：同一批属性（`sourceImage` / `viewport` / `zoom` /
// `panX` / `panY` / `autoSize`；fit 已不再是属性——它只是"算一次 zoom"的命令，见 gpuscale.h），几何由**同一个** `gpuscale::makePlan()` 算出来，
// 只是执行者换成 **Qt 的内置平滑缩放**（`QPainter::drawImage` + `SmoothPixmapTransform`）。
//
// 两处用到它（设计 16 §五）：
//   ① 用户选了 `Smooth (Qt)` 这一项 —— 有 GPU 也一样（这一项本来就是 Qt 的实现）；
//   ② 场景图后端是 software（`QQuickRhiItem` 在软件后端下什么都不画）——
//      此时**无论选了什么算法**，画面都由它出，等于"降级到 Qt 平滑"。
//
// ## 为什么用 QQuickPaintedItem
//   软件后端下 `QQuickRhiItem` 不可用；而 `QQuickPaintedItem` 是官方为"画一张图/一段 QPainter"
//   准备的通路，**软件后端可用**，RHI 后端下也能用（它自己上传纹理）。我们要的就是 Qt 的缩放内核，
//   不需要动 shader —— 这条路径的画质就是我们**不对其负责**的那一档。
//
// ## 多线程（AGENTS：无锁优先）
//   `paint()` 在**渲染线程**执行（软件后端时就是 GUI 线程）。源图与计划在 GUI 线程改，
//   ⇒ 用 `std::shared_ptr` + `std::atomic_store/load` 交接**整份快照**：GUI 线程写入新快照，
//   渲染线程取到哪个就画哪个，没有互斥量、也不会有半更新的状态。
//
// ## 为什么这里**只有** Qt 平滑，不上 Bicubic / Lanczos（2026-11-07 实测后定案）
//   高级算法只在 GPU shader 里（`src/shaders/gpuscale_resample*.frag`）：GPU 每帧重采样
//   都是亚毫秒，五个算法随便切。CPU 这条不一样 —— `tools/ib-bench-scale` 用可分离卷积
//   实测（单线程，输入→输出 1080P 级别）：
//
//     | 算法            | 2.0 MP→1.7 MP | 8.2 MP→1.7 MP |
//     |-----------------|---------------|---------------|
//     | Smooth (Qt)     |  1.7 ms       |  4.8 ms       |
//     | Nearest         |  0.8 ms       |  0.9 ms       |
//     | Bicubic         | 67 ms         | 144 ms        |
//     | Lanczos 3       | 81 ms         | 188 ms        |
//
//   翻页时一次 67–188 ms 尚可忍，但**拖动/缩放是每帧重绘** ⇒ 5–15 fps。
//   要让 CPU 也用高级算法，必须"缩放期间用 Qt 平滑、静止 150–300 ms 后台重算 + 缓存"，
//   约 150–250 行（线程 + 双图交换 + 脏标记 + 新测试），还会带来"先糊后锐"的跳变；
//   而这条路径只在**没有可用 GPU** 时才会走到。⇒ 决定：保持 Qt 平滑。
//
//   将来若真要改善软件路径画质，便宜的折中是**输出端加一道 3×3 unsharp**（约 2–5 ms），
//   观感能补回大部分锐度，不需要 Lanczos、也不需要缓存。
//
// ## 不缓存缩放结果（与 GPU 那条同一口径）
//   每次 `paint()` 都从源图采样；Qt 的平滑缩放本身很快（24MP→6MP ≈ 2.6 ms，research 11），
//   而静止时 Qt Quick 不重绘 ⇒ 没有每帧成本。
class CpuScalerItem : public QQuickPaintedItem
{
    Q_OBJECT

    Q_PROPERTY(QImage sourceImage READ sourceImage WRITE setSourceImage NOTIFY sourceChanged)
    Q_PROPERTY(QSize viewport READ viewport WRITE setViewport NOTIFY viewportChanged)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY zoomChanged)
    Q_PROPERTY(qreal panX READ panX WRITE setPanX NOTIFY panChanged)
    Q_PROPERTY(qreal panY READ panY WRITE setPanY NOTIFY panChanged)
    Q_PROPERTY(bool autoSize READ autoSize WRITE setAutoSize NOTIFY autoSizeChanged)

    Q_PROPERTY(QSize targetSize READ targetSize NOTIFY targetSizeChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)

public:
    explicit CpuScalerItem(QQuickItem *parent = nullptr);

    void setSourceImage(const QImage &img);
    QImage sourceImage() const { return m_source; }

    QSize viewport() const { return m_viewport; }
    void setViewport(const QSize &s);

    qreal zoom() const { return m_zoom; }
    void setZoom(qreal z);

    qreal panX() const { return m_panX; }
    void setPanX(qreal x);

    qreal panY() const { return m_panY; }
    void setPanY(qreal y);

    bool autoSize() const { return m_autoSize; }
    void setAutoSize(bool on);

    QSize targetSize() const { return m_plan.dst; }     // 设备像素
    QString status() const { return m_status; }

    void paint(QPainter *painter) override;

Q_SIGNALS:
    void sourceChanged();
    void viewportChanged();
    void zoomChanged();
    void panChanged();
    void autoSizeChanged();
    void targetSizeChanged();
    void statusChanged();

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    // GUI 线程写、渲染线程读的**整份**快照（图 + 计划），用原子指针交接（无锁）
    struct Snapshot {
        QImage image;
        gpuscale::Plan plan;
    };

    void updatePlan();
    void setStatus(const QString &s);

    QImage m_source;
    QSize m_viewport = QSize(1, 1);
    qreal m_zoom = 1.0;
    qreal m_panX = 0.0;
    qreal m_panY = 0.0;
    bool m_autoSize = true;

    gpuscale::Plan m_plan;                   // GUI 线程侧的"当前计划"（只读给 QML 的 targetSize）
    std::shared_ptr<const Snapshot> m_snapshot;   // 渲染线程读这一份
    QString m_status;
    bool m_inUpdate = false;
};
