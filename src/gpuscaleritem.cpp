#include "gpuscaleritem.h"

#include "gpuscale.h"

#include <QDebug>
#include <QQuickWindow>
#include <QtGui/rhi/qrhi.h>

#include <cmath>

// ── 渲染器（渲染线程；只碰自己的成员 + synchronize() 取来的副本）──────────
class GpuScalerRenderer : public QQuickRhiItemRenderer
{
public:
    ~GpuScalerRenderer() override { delete m_pipe; }

    void initialize(QRhiCommandBuffer *) override
    {
        // ★ QQuickRhiItemNode 每次 sync 都会调 initialize()（qquickrhiitem.cpp），
        //   所以这里必须自己去重：只有 QRhi 换了（上下文丢失/重建）才重建管线。
        if (m_pipe && m_pipeRhi == rhi())
            return;
        delete m_pipe;
        m_pipe = new gpuscale::Pipeline(rhi());
        m_pipeRhi = rhi();
        if (!m_pipe->isValid())
            qWarning().noquote() << QStringLiteral("[GpuScalerItem] 管线不可用：%1").arg(m_pipe->error());
    }

    void synchronize(QQuickRhiItem *item) override
    {
        auto *it = static_cast<GpuScalerItem *>(item);
        // GUI 线程此刻被阻塞 ⇒ 直接读 item 的成员是安全的（QImage 隐式共享，零拷贝）
        m_src = it->sourceImage();
        m_plan = it->plan();
        m_have = !m_src.isNull();
    }

    void render(QRhiCommandBuffer *cb) override
    {
        if (!m_pipe || !m_pipe->isValid() || !m_have || m_plan.dst.isEmpty())
            return;
        QRhiRenderTarget *rt = renderTarget();
        if (!rt)
            return;
        // 颜色缓冲尺寸 = 目标尺寸（updatePlan 里用 fixedColorBufferSize 固定）→ 合成 1:1
        if (rt->pixelSize() != m_plan.dst) {
            if (!m_sizeWarned) {
                m_sizeWarned = true;
                qWarning().noquote()
                    << QStringLiteral("[GpuScalerItem] 颜色缓冲 %1x%2 与目标 %3x%4 不一致，本帧跳过（下一帧应已一致）")
                           .arg(rt->pixelSize().width()).arg(rt->pixelSize().height())
                           .arg(m_plan.dst.width()).arg(m_plan.dst.height());
            }
            return;
        }
        m_pipe->setSourceImage(m_src);
        QRhiResourceUpdateBatch *rub = rhi()->nextResourceUpdateBatch();
        if (!m_pipe->render(cb, rub, m_plan, rt)) {
            if (!m_sizeWarned) {
                m_sizeWarned = true;
                qWarning().noquote() << QStringLiteral("[GpuScalerItem] 渲染失败：%1").arg(m_pipe->error());
            }
        }
    }

private:
    gpuscale::Pipeline *m_pipe = nullptr;
    QRhi *m_pipeRhi = nullptr;
    QImage m_src;
    gpuscale::Plan m_plan;
    bool m_have = false;
    bool m_sizeWarned = false;
};

// ── Item ─────────────────────────────────────────────────────────────

GpuScalerItem::GpuScalerItem(QQuickItem *parent)
    : QQuickRhiItem(parent)
{
    setSampleCount(1);                                  // 目标像素精确，MSAA 无意义
    setColorBufferFormat(TextureFormat::RGBA8);         // 与 shader 的最终输出一致
    // mirrorVertically 保持默认 false：我们的颜色缓冲第 0 行就是图像第 0 行
    //   （QRhi 的纹理行序约定；见 --selftest-gpuscale 的朝向自检）
    updatePlan();
}

const char *GpuScalerItem::fitName(Fit f)
{
    switch (f) {
    case Fit::Original:  return "original";
    case Fit::Screen:    return "screen";
    case Fit::FitWidth:  return "width";
    case Fit::FitHeight: return "height";
    }
    return "?";
}

void GpuScalerItem::setSourceImage(const QImage &img)
{
    if (img.isNull()) {
        m_source = QImage();
        ++m_sourceGen;
        setStatus(QStringLiteral("无源图"));
        emit sourceChanged();
        updatePlan();
        return;
    }
    // 上传口径：RGBA8888（与 CPU 侧 gpuscale::resize 的 RGB32 同像素；alpha 一律不透明）
    m_source = (img.format() == QImage::Format_RGBA8888) ? img : img.convertToFormat(QImage::Format_RGBA8888);
    ++m_sourceGen;
    setStatus(QStringLiteral("源 %1×%2").arg(m_source.width()).arg(m_source.height()));
    emit sourceChanged();
    updatePlan();
}

void GpuScalerItem::setViewport(const QSize &s)
{
    const QSize v = QSize(qMax(1, s.width()), qMax(1, s.height()));
    if (v == m_viewport)
        return;
    m_viewport = v;
    emit viewportChanged();
    updatePlan();
}

void GpuScalerItem::setFit(Fit f)
{
    if (f == m_fit)
        return;
    m_fit = f;
    emit fitChanged();
    updatePlan();
}

void GpuScalerItem::setZoom(qreal z)
{
    const qreal nz = qBound(0.02, z, 64.0);
    if (qFuzzyCompare(nz, m_zoom))
        return;
    m_zoom = nz;
    emit zoomChanged();
    updatePlan();
}

void GpuScalerItem::setPanX(qreal x)
{
    if (qFuzzyCompare(x, m_panX))
        return;
    m_panX = x;
    emit panChanged();
    updatePlan();
}

void GpuScalerItem::setPanY(qreal y)
{
    if (qFuzzyCompare(y, m_panY))
        return;
    m_panY = y;
    emit panChanged();
    updatePlan();
}

void GpuScalerItem::setAutoSize(bool on)
{
    if (on == m_autoSize)
        return;
    m_autoSize = on;
    emit autoSizeChanged();
    updatePlan();
}

gpuscale::Params GpuScalerItem::params() const
{
    return m_params;
}

void GpuScalerItem::setParams(const gpuscale::Params &p)
{
    m_params = p;
    emit paramsChanged();
    updatePlan();
}

void GpuScalerItem::setMode(int m)
{
    const auto v = gpuscale::Mode(qBound(0, m, 2));
    if (v == m_params.mode)
        return;
    if (v == gpuscale::Mode::QtSmooth && !m_warnedQtSmooth) {
        // Qt 的 SmoothTransformation 是**非线性**的对照路径（AGENTS.md 立场 #1），GPU 路径不实现它
        m_warnedQtSmooth = true;
        qWarning().noquote() << QStringLiteral("[GpuScalerItem] mode=qt-smooth 是 CPU 对照路径，"
                                               "GPU 渲染按 linear-light 处理");
    }
    m_params.mode = v;
    emit paramsChanged();
    updatePlan();
}

void GpuScalerItem::setQuality(int q)
{
    const auto v = gpuscale::Quality(qBound(0, q, 2));
    if (v == m_params.quality)
        return;
    m_params.quality = v;
    emit paramsChanged();
    updatePlan();
}

void GpuScalerItem::setFilter(int f)
{
    const auto v = gpuscale::Filter(qBound(0, f, 3));
    if (v == m_params.filter)
        return;
    m_params.filter = v;
    emit paramsChanged();
    updatePlan();
}

void GpuScalerItem::setLobes(int n)
{
    const int v = qBound(2, n, 4);
    if (v == m_params.lobes)
        return;
    m_params.lobes = v;
    emit paramsChanged();
    updatePlan();
}

void GpuScalerItem::setMitchellB(qreal b)
{
    if (qFuzzyCompare(b, m_params.mitchellB))
        return;
    m_params.mitchellB = b;
    emit paramsChanged();
    updatePlan();
}

void GpuScalerItem::setMitchellC(qreal c)
{
    if (qFuzzyCompare(c, m_params.mitchellC))
        return;
    m_params.mitchellC = c;
    emit paramsChanged();
    updatePlan();
}

void GpuScalerItem::setAntiring(qreal a)
{
    const qreal v = qBound(0.0, a, 1.0);
    if (qFuzzyCompare(v, m_params.antiring))
        return;
    m_params.antiring = v;
    emit paramsChanged();
    updatePlan();
}

QString GpuScalerItem::presetKey() const
{
    const gpuscale::Preset *p = gpuscale::presetFor(m_params);
    return p ? QLatin1String(p->key) : QStringLiteral("custom");
}

bool GpuScalerItem::applyPreset(const QString &key)
{
    const gpuscale::Preset *p = gpuscale::presetByKey(key);
    if (!p)
        return false;
    setParams(p->params);
    return true;
}

void GpuScalerItem::resetView()
{
    m_zoom = 1.0;
    m_panX = 0.0;
    m_panY = 0.0;
    emit zoomChanged();
    emit panChanged();
    updatePlan();
}

void GpuScalerItem::zoomAt(qreal factor, qreal cx, qreal cy)
{
    if (factor <= 0)
        return;
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    const QSize oldFull = m_plan.full;
    const qreal newZoom = qBound(0.02, m_zoom * factor, 64.0);
    if (qFuzzyCompare(newZoom, m_zoom))
        return;
    // 锚点在「完整缩放图」里的位置 = 平移量 + 视口内位置；缩放后保持它不动
    const qreal ox = m_panX * dpr, oy = m_panY * dpr;
    const qreal ax = cx * dpr, ay = cy * dpr;
    const qreal oldZ = m_zoom;
    m_zoom = newZoom;
    updatePlan();
    const QSize newFull = m_plan.full;
    const qreal fx = oldFull.isEmpty() ? 1.0 : qreal(newFull.width()) / qreal(qMax(1, oldFull.width()));
    const qreal fy = oldFull.isEmpty() ? 1.0 : qreal(newFull.height()) / qreal(qMax(1, oldFull.height()));
    m_panX = ((ox + ax) * fx - ax) / dpr;
    m_panY = ((oy + ay) * fy - ay) / dpr;
    Q_UNUSED(oldZ);
    emit zoomChanged();
    emit panChanged();
    updatePlan();
}

void GpuScalerItem::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickRhiItem::itemChange(change, value);
    // 进/出场景时 devicePixelRatio 才可知 → 重算一次计划（HiDPI 下颜色缓冲尺寸必须跟着 dpr）
    if (change == ItemSceneChange)
        updatePlan();
}

void GpuScalerItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickRhiItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() == oldGeometry.size())
        return;
    // 视口是外部给的（viewport 属性），不随自身几何变；但 dpr 可能随窗口变 → 重算一次
    updatePlan();
}

void GpuScalerItem::updatePlan()
{
    if (m_inUpdate)
        return;
    m_inUpdate = true;

    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    const QSize viewportPx(qMax(1, qRound(m_viewport.width() * dpr)),
                           qMax(1, qRound(m_viewport.height() * dpr)));
    m_plan = gpuscale::makePlan(m_source.size(), viewportPx, gpuscale::Fit(int(m_fit)), m_zoom,
                                m_panX * dpr, m_panY * dpr, m_params);

    // 颜色缓冲 = 目标尺寸（设备像素）⇒ 合成 1:1，不产生第二次重采样
    if (!m_plan.dst.isEmpty()) {
        setFixedColorBufferWidth(m_plan.dst.width());
        setFixedColorBufferHeight(m_plan.dst.height());
        if (m_autoSize) {
            const qreal w = qreal(m_plan.dst.width()) / dpr;
            const qreal h = qreal(m_plan.dst.height()) / dpr;
            if (!qFuzzyCompare(width(), w))
                setWidth(w);
            if (!qFuzzyCompare(height(), h))
                setHeight(h);
        }
    }

    if (!m_source.isNull()) {
        setStatus(QStringLiteral("源 %1×%2 · 出 %3×%4 · 全 %5×%6 · %7 · %8%9")
                      .arg(m_source.width()).arg(m_source.height())
                      .arg(m_plan.dst.width()).arg(m_plan.dst.height())
                      .arg(m_plan.full.width()).arg(m_plan.full.height())
                      .arg(QLatin1String(fitName(m_fit)))
                      .arg(presetKey())
                      .arg(m_plan.offX || m_plan.offY
                               ? QStringLiteral(" · 窗(%1,%2)").arg(m_plan.offX).arg(m_plan.offY)
                               : QString()));
    }
    emit targetSizeChanged();
    update();
    m_inUpdate = false;
}

void GpuScalerItem::setStatus(const QString &s)
{
    if (s == m_status)
        return;
    m_status = s;
    emit statusChanged();
}

QQuickRhiItemRenderer *GpuScalerItem::createRenderer()
{
    return new GpuScalerRenderer;
}
