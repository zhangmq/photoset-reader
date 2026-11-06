#include "imagedecoder.h"

namespace decode {

// ── 可选默认实现 ─────────────────────────────────────────────────────────────
//   没有 provider 必须实现它们：`querySize()` 拿不到就算也不知道（扫描侧会退回
//   "解码后再量"），`thumbnail()` 走下面这条**我们自己的**缩放入口。

QSize Provider::querySize(const Source &src, int page, QString *fmt, int *frames)
{
    Q_UNUSED(src);
    Q_UNUSED(page);
    if (fmt) fmt->clear();
    if (frames) *frames = 1;
    return {};
}

//   ★ 默认缩略图 = 解码全图 + 我们自己的缩放。**"缩略图是我们自己的逻辑"这条
//     在接口上就成立**：provider 想省时间可以覆盖它，但不覆盖也完全正确。
QImage Provider::thumbnail(const Source &src, const QSize &target)
{
    if (!target.isValid()) return {};
    const QImage full = decode(src, 0, nullptr);
    if (full.isNull()) return {};
    if (full.width() <= target.width() && full.height() <= target.height()) return full;
    return full.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

}  // namespace decode
