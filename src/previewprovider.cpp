#include "previewprovider.h"

PreviewProvider::PreviewProvider(std::shared_ptr<PreviewStore> store)
    : QQuickImageProvider(QQuickImageProvider::Image), m_store(std::move(store))
{
}

QImage PreviewProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    Q_UNUSED(requestedSize);              // 目标尺寸由 GUI 线程算好放在表里（按条目的宽高比）

    //   ① 批量并行解码已经放好 ⇒ 直接返回（这条是"整条一次性上屏"的关键）
    {
        QMutexLocker lk(&m_store->mutex);
        const auto it = m_store->images.constFind(id);
        if (it != m_store->images.constEnd()) {
            if (size) *size = it.value().size();
            return it.value();
        }
    }

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
        QMutexLocker lk(&m_store->mutex);
        m_store->images.insert(id, img);                    // 回填，下次直接命中
        if (m_store->images.size() > 512) m_store->images.clear();
    }
    if (size) *size = img.size();
    return img;
}
