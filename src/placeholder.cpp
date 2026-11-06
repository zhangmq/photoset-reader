#include "placeholder.h"

#include <QColor>
#include <QPainter>
#include <QPen>

namespace placeholder {
namespace {

//   主显示的框：1080P（用户 2026-11-06 定）
constexpr int kBoxW = 1920;
constexpr int kBoxH = 1080;
//   尺寸未知时的方图
constexpr int kSquare = 1080;

const QColor kFill  (0x2a, 0x2a, 0x2e);
const QColor kBorder(0x4a, 0x4a, 0x52);
const QColor kGlyph (0x66, 0x66, 0x72);

}  // namespace

QSize screenSize(const QSize &originalSize)
{
    if (!originalSize.isValid() || originalSize.isEmpty()) return QSize(kSquare, kSquare);
    QSize s = originalSize;
    //   ★ **只缩不放**（2026-11-06 踩到）：`QSize::scale()` 对比框还小的图会**放大**
    //     —— 40×60 的图会被"等比装进 1920×1080"放大成 720×1080，占位图凭空变大 300 倍。
    //     占位图的意义是"按原版面占一屏"，原图本来就小就保持原样。
    if (s.width() > kBoxW || s.height() > kBoxH) s.scale(QSize(kBoxW, kBoxH), Qt::KeepAspectRatio);
    //   极端薄图（如 4000×20）缩完可能有一边变 0 ⇒ 夹到至少 1px
    return QSize(qMax(1, s.width()), qMax(1, s.height()));
}

QImage draw(const QSize &size)
{
    QSize s = size;
    if (!s.isValid() || s.isEmpty()) s = QSize(kSquare, kSquare);
    s = QSize(qMax(1, s.width()), qMax(1, s.height()));

    QImage img(s, QImage::Format_ARGB32_Premultiplied);
    img.fill(kFill);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    //   细边框：宽度跟着尺寸走（缩略图 1px、大图不显粗）
    const int    shortSide = qMin(s.width(), s.height());
    const qreal  line = qBound(1.0, shortSide / 200.0, 8.0);
    p.setPen(QPen(kBorder, line));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(line, line, s.width() - 2 * line, s.height() - 2 * line));
    //   中央"破损图片"字形：一个圆角方框 + 一条斜杠
    const qreal side = shortSide * 0.22;
    const QRectF box((s.width() - side) / 2.0, (s.height() - side) / 2.0, side, side);
    p.setPen(QPen(kGlyph, qBound(1.0, side / 24.0, 12.0)));
    p.drawRoundedRect(box, side * 0.08, side * 0.08);
    const qreal pad = side * 0.18;
    p.drawLine(QPointF(box.left() + pad, box.top() + pad),
               QPointF(box.right() - pad, box.bottom() - pad));
    p.end();
    return img;
}

QImage forScreen(const QSize &originalSize)
{
    return draw(screenSize(originalSize));
}

}  // namespace placeholder
