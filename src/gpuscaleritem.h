#pragma once

#include <QImage>
#include <QQuickRhiItem>

#include "gpuscale.h"
#include "scaler.h"

// ══ GPU 缩放渲染组件（QML 可用）════════════════════════════════════════════
//
// 一个 Item = **一张图按 scaler::Params 缩放后的画面**。它自己就是「渲染器」这一角色
// （设计 13 §八）：fit 模式 / 交互缩放 / 平移 / 画质参数**全在这里**，
// 与 CPU 参照实现（`scaler::resize`）逐像素等价（见 `--selftest-gpuscale`）。
//
// ## 为什么是 QQuickRhiItem（而不是自定义 QSGMaterial）
//   · 我们的算法是**三段 FBO**（线性化 → H → V(+anti-ringing)），需要
//     `QRhiCommandBuffer` 的 beginPass/endPass 与自建纹理渲染目标；
//     `QSGMaterial`/`QSGGeometryNode` 只能表达**一次** draw、拿不到 pass 边界，
//     硬做要落到 `QSGRenderNode` 手写 QRhi 资源生命周期（易错且没有官方保证）。
//   · `QQuickRhiItem`（Qt 6.7+，本项目 6.12）就是官方给这件事的入口：它把 QRhi 命令缓冲、
//     颜色缓冲、与场景图的合成都管好，且**与后端无关**（OpenGL / Vulkan / D3D / Metal 同一份代码）。
//   · 代价是它把结果合成到自己那张纹理上；我们把颜色缓冲固定成**目标尺寸**（见 updatePlan），
//     于是合成是 **1:1**（不产生第二次重采样）。
//
// ## 与 CPU 的对应关系
//   `viewport`（逻辑像素）+ `fit` + `zoom` + `pan` ⇒ `gpuscale::Plan`（`makePlan`）
//   ⇒ 三段 shader；`mode/quality/filter/lobes/mitchellB/mitchellC/antiring` 与
//   `scaler::Params` 一一对应（`params()` / `setParams()`）。
//
// ## 不缓存缩放结果（设计 13 §八）
//   每次重绘都重跑 shader；中间纹理只是**一帧内**的工作区（尺寸不变才复用）。
//
// ## 无锁
//   源图/参数由 GUI 线程写，渲染线程在 `synchronize()`（GUI 线程被阻塞）里取 —— 没有互斥量。
class GpuScalerItem : public QQuickRhiItem
{
    Q_OBJECT

    // 源图（C++ 与 **QML 绑定**都可用；QImage 隐式共享，零拷贝）
    Q_PROPERTY(QImage sourceImage READ sourceImage WRITE setSourceImage NOTIFY sourceChanged)
    Q_PROPERTY(QSize viewport READ viewport WRITE setViewport NOTIFY viewportChanged)
    Q_PROPERTY(Fit fit READ fit WRITE setFit NOTIFY fitChanged)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY zoomChanged)
    Q_PROPERTY(qreal panX READ panX WRITE setPanX NOTIFY panChanged)
    Q_PROPERTY(qreal panY READ panY WRITE setPanY NOTIFY panChanged)
    Q_PROPERTY(bool autoSize READ autoSize WRITE setAutoSize NOTIFY autoSizeChanged)

    // ── scaler::Params 的逐项暴露（QML 友好；枚举值与 scaler 的枚举一一对应）──
    Q_PROPERTY(int mode READ mode WRITE setMode NOTIFY paramsChanged)
    Q_PROPERTY(int quality READ quality WRITE setQuality NOTIFY paramsChanged)
    Q_PROPERTY(int filter READ filter WRITE setFilter NOTIFY paramsChanged)
    Q_PROPERTY(int lobes READ lobes WRITE setLobes NOTIFY paramsChanged)
    Q_PROPERTY(qreal mitchellB READ mitchellB WRITE setMitchellB NOTIFY paramsChanged)
    Q_PROPERTY(qreal mitchellC READ mitchellC WRITE setMitchellC NOTIFY paramsChanged)
    Q_PROPERTY(qreal antiring READ antiring WRITE setAntiring NOTIFY paramsChanged)
    Q_PROPERTY(QString presetKey READ presetKey NOTIFY paramsChanged)

    Q_PROPERTY(QSize sourceSize READ sourceSize NOTIFY sourceChanged)
    Q_PROPERTY(QSize targetSize READ targetSize NOTIFY targetSizeChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)

public:
    // QML 侧要能写 `GpuScalerItem.Screen`；**值一一对应 gpuscale::Fit（单一事实源）**
    enum Fit {
        Original  = int(gpuscale::Fit::Original),
        Screen    = int(gpuscale::Fit::Screen),
        FitWidth  = int(gpuscale::Fit::FitWidth),
        FitHeight = int(gpuscale::Fit::FitHeight),
    };
    Q_ENUM(Fit)

    explicit GpuScalerItem(QQuickItem *parent = nullptr);

    // ── 源图（从 C++ 设；QImage 隐式共享，零拷贝）──
    void setSourceImage(const QImage &img);
    QImage sourceImage() const { return m_source; }
    quint64 sourceGeneration() const { return m_sourceGen; }

    // ── 视口 / fit / 交互缩放 / 平移 ──
    QSize viewport() const { return m_viewport; }
    void setViewport(const QSize &s);
    Fit fit() const { return m_fit; }
    void setFit(Fit f);
    qreal zoom() const { return m_zoom; }
    void setZoom(qreal z);
    qreal panX() const { return m_panX; }
    void setPanX(qreal x);
    qreal panY() const { return m_panY; }
    void setPanY(qreal y);
    bool autoSize() const { return m_autoSize; }
    void setAutoSize(bool on);

    // ── 画质参数 ──
    scaler::Params params() const;
    void setParams(const scaler::Params &p);
    int mode() const { return int(m_params.mode); }
    void setMode(int m);
    int quality() const { return int(m_params.quality); }
    void setQuality(int q);
    int filter() const { return int(m_params.filter); }
    void setFilter(int f);
    int lobes() const { return m_params.lobes; }
    void setLobes(int n);
    qreal mitchellB() const { return m_params.mitchellB; }
    void setMitchellB(qreal b);
    qreal mitchellC() const { return m_params.mitchellC; }
    void setMitchellC(qreal c);
    qreal antiring() const { return m_params.antiring; }
    void setAntiring(qreal a);
    QString presetKey() const;

    QSize sourceSize() const { return m_source.size(); }
    QSize targetSize() const { return m_plan.dst; }        // 设备像素
    QString status() const { return m_status; }

    // 当前渲染计划（渲染线程在 synchronize() 里读；纯派生值）
    gpuscale::Plan plan() const { return m_plan; }
    void setPlanForTest(const gpuscale::Plan &p) { m_plan = p; }

    // ── 便利入口（QML 可调）──
    Q_INVOKABLE bool applyPreset(const QString &key);
    Q_INVOKABLE void resetView();                    // zoom=1、pan=0
    Q_INVOKABLE void zoomAt(qreal factor, qreal cx, qreal cy);   // 以 (cx,cy) 为锚点缩放（逻辑像素）

    static const char *fitName(Fit f);

Q_SIGNALS:
    void viewportChanged();
    void fitChanged();
    void zoomChanged();
    void panChanged();
    void autoSizeChanged();
    void paramsChanged();
    void sourceChanged();
    void targetSizeChanged();
    void statusChanged();

protected:
    QQuickRhiItemRenderer *createRenderer() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    void updatePlan();
    void setStatus(const QString &s);

    QImage m_source;
    quint64 m_sourceGen = 0;

    QSize m_viewport = QSize(1, 1);
    Fit m_fit = Fit::Screen;
    qreal m_zoom = 1.0;
    qreal m_panX = 0.0;
    qreal m_panY = 0.0;
    bool m_autoSize = true;

    scaler::Params m_params;
    gpuscale::Plan m_plan;
    QString m_status;
    bool m_inUpdate = false;
    bool m_warnedQtSmooth = false;

    friend class GpuScalerRenderer;
};
