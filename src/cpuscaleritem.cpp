#include "cpuscaleritem.h"

#include <QPainter>
#include <QQuickWindow>

#include <atomic>

CpuScalerItem::CpuScalerItem(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setFlag(ItemHasContents, true);
    setOpaquePainting(false);        // 图可能比视口小（画布底色要透出来）
    setAntialiasing(false);          // 只是贴一张已缩放的图，不需要抗锯齿
    updatePlan();
}

void CpuScalerItem::setSourceImage(const QImage &img)
{
    if (img.isNull()) {
        m_source = QImage();
    } else if (img.format() == QImage::Format_RGB32
               || img.format() == QImage::Format_ARGB32_Premultiplied) {
        m_source = img;
    } else {
        // 统一成 QPainter 最快的两种格式（隐式共享，转换只发生一次）
        m_source = img.convertToFormat(img.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied
                                                             : QImage::Format_RGB32);
    }
    setStatus(m_source.isNull() ? QStringLiteral("无源图")
                                : QStringLiteral("源 %1×%2").arg(m_source.width()).arg(m_source.height()));
    emit sourceChanged();
    updatePlan();
}

void CpuScalerItem::setViewport(const QSize &s)
{
    const QSize v = QSize(qMax(1, s.width()), qMax(1, s.height()));
    if (v == m_viewport)
        return;
    m_viewport = v;
    emit viewportChanged();
    updatePlan();
}

void CpuScalerItem::setZoom(qreal z)
{
    const qreal nz = qBound(0.02, z, 64.0);
    if (qFuzzyCompare(nz, m_zoom))
        return;
    m_zoom = nz;
    emit zoomChanged();
    updatePlan();
}

void CpuScalerItem::setPanX(qreal x)
{
    if (qFuzzyCompare(x, m_panX))
        return;
    m_panX = x;
    emit panChanged();
    updatePlan();
}

void CpuScalerItem::setPanY(qreal y)
{
    if (qFuzzyCompare(y, m_panY))
        return;
    m_panY = y;
    emit panChanged();
    updatePlan();
}

void CpuScalerItem::setAutoSize(bool on)
{
    if (on == m_autoSize)
        return;
    m_autoSize = on;
    emit autoSizeChanged();
    updatePlan();
}

void CpuScalerItem::updatePlan()
{
    if (m_inUpdate)
        return;
    m_inUpdate = true;

    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    const QSize viewportPx(qMax(1, qRound(m_viewport.width() * dpr)),
                           qMax(1, qRound(m_viewport.height() * dpr)));
    gpuscale::Params p;                      // 算法对本项没有意义（永远是 Qt 平滑），只算几何
    //   pan 是**设备像素**（与 zoom 同单位）；viewportPx 也是设备像素
    m_plan = gpuscale::makePlan(m_source.size(), viewportPx, m_zoom, m_panX, m_panY, p);

    // 尺寸 = 目标尺寸（设备像素）/ dpr ⇒ 逻辑尺寸，绘制时 QPainter 已按 dpr 缩放
    if (!m_plan.dst.isEmpty() && m_autoSize) {
        const qreal w = qreal(m_plan.dst.width()) / dpr;
        const qreal h = qreal(m_plan.dst.height()) / dpr;
        if (!qFuzzyCompare(width(), w))
            setWidth(w);
        if (!qFuzzyCompare(height(), h))
            setHeight(h);
    }

    // 快照交给渲染线程（整份替换，原子指针）
    auto snap = std::make_shared<Snapshot>();
    snap->image = m_source;
    snap->plan = m_plan;
    std::atomic_store(&m_snapshot, std::shared_ptr<const Snapshot>(std::move(snap)));

    if (!m_source.isNull()) {
        setStatus(QStringLiteral("Smooth (Qt) · 源 %1×%2 · 出 %3×%4 · 全 %5×%6")
                      .arg(m_source.width()).arg(m_source.height())
                      .arg(m_plan.dst.width()).arg(m_plan.dst.height())
                      .arg(m_plan.full.width()).arg(m_plan.full.height()));
    }
    emit targetSizeChanged();
    update();                                 // 请求重绘（软件后端下 paint 在 GUI 线程）
    m_inUpdate = false;
}

void CpuScalerItem::paint(QPainter *painter)
{
    const std::shared_ptr<const Snapshot> snap = std::atomic_load(&m_snapshot);
    if (!snap || snap->image.isNull() || !snap->plan.valid())
        return;

    const QImage &img = snap->image;
    const gpuscale::Plan &pl = snap->plan;

    // ★ `Plan.off/dst/full` 是**输出空间**的设备像素；`drawImage` 的 source 要**源图坐标**
    const qreal sx = qreal(pl.offX) * pl.src.width() / qMax(1, pl.full.width());
    const qreal sy = qreal(pl.offY) * pl.src.height() / qMax(1, pl.full.height());
    const qreal sw = qreal(pl.dst.width()) * pl.src.width() / qMax(1, pl.full.width());
    const qreal sh = qreal(pl.dst.height()) * pl.src.height() / qMax(1, pl.full.height());
    QRectF srcRect = QRectF(sx, sy, sw, sh).intersected(QRectF(0, 0, img.width(), img.height()));
    if (srcRect.isEmpty())
        return;

    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);   // 这就是 Qt 的内置平滑
    painter->drawImage(QRectF(0, 0, width(), height()), img, srcRect);
}

void CpuScalerItem::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickPaintedItem::itemChange(change, value);
    // 进/出场景时 devicePixelRatio 才可知 ⇒ 重算一次（HiDPI 下逻辑尺寸要跟着 dpr）
    if (change == ItemSceneChange)
        updatePlan();
}

void CpuScalerItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        updatePlan();
}

void CpuScalerItem::setStatus(const QString &s)
{
    if (s == m_status)
        return;
    m_status = s;
    emit statusChanged();
}
