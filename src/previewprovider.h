#pragma once

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>
#include <QSize>

#include <memory>

#include "imageread.h"

// ══ 时间轴悬停预览（用户 2026-11-03）═══════════════════════════════════════
//   · GUI 线程（`ViewerModel::previewStrip`）把"要显示的条目位置 + 目标像素尺寸"放进表里；
//   · provider 线程只**读表 + 解码**：锁只护内存读写，锁里不做 I/O、不解码（AGENTS 口径）；
//   · 解码走 `imageread::imageScaled()`（缩略图用途的降采样预处理，不是主显示路径）。
struct PreviewStore {
    struct Entry {
        imageread::Location loc;
        QSize               target;      // 解码目标（已按 2x 放大，供高 DPI）
    };
    QMutex               mutex;
    QHash<QString, Entry> entries;       // key = "<generation>-<index>"
};

class PreviewProvider : public QQuickImageProvider
{
public:
    explicit PreviewProvider(std::shared_ptr<PreviewStore> store);

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

private:
    std::shared_ptr<PreviewStore> m_store;
    QMutex                        m_cacheMutex;
    QHash<QString, QImage>        m_cache;   // 小缓存（换一代 URL 就自然失效）
};
