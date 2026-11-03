#include "previewprovider.h"

PreviewProvider::PreviewProvider(std::shared_ptr<PreviewStore> store)
    : QQuickImageProvider(QQuickImageProvider::Image), m_store(std::move(store))
{
}

QImage PreviewProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    Q_UNUSED(requestedSize);              // 目标尺寸由 GUI 线程算好放在表里（按条目的宽高比）

    PreviewStore::Entry entry;
    {
        QMutexLocker lk(&m_store->mutex);
        const auto it = m_store->entries.constFind(id);
        if (it == m_store->entries.constEnd()) return {};   // 已经换了一代 ⇒ 丢
        entry = it.value();
    }
    {
        QMutexLocker lk(&m_cacheMutex);
        const auto it = m_cache.constFind(id);
        if (it != m_cache.constEnd()) {
            if (size) *size = it.value().size();
            return it.value();
        }
    }

    const QImage img = imageread::imageScaled(entry.loc, entry.target);
    if (img.isNull()) return {};

    {
        QMutexLocker lk(&m_cacheMutex);
        if (m_cache.size() > 64) m_cache.clear();           // 粗暴但足够：最多 12 张一代
        m_cache.insert(id, img);
    }
    if (size) *size = img.size();
    return img;
}
