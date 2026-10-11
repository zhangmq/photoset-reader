#include "viewmodel.h"
#include "imageprovider.h"
#include "settings.h"
#include <QDebug>
#include <QGuiApplication>
#include <QStyleHints>

ViewModel::ViewModel(ImageProvider *provider, QObject *parent)
    : QObject(parent), m_provider(provider)
{
    m_intervalMs = Settings::instance().slideshowIntervalMs();
    m_pixelPerfect = provider->pixelPerfect();
    qInfo().noquote() << QStringLiteral("[vm] 初始化  intervalMs=%1  pixelPerfect=%2")
                             .arg(m_intervalMs).arg(m_pixelPerfect);
}

// ── 派生 ────────────────────────────────────────────────
int ViewModel::currentPage() const
{
    if (m_provider->pageCount() <= 0) return 0;
    return m_provider->pageOfItem(m_cursor);
}
int ViewModel::totalPages() const { return m_provider->pageCount(); }
int ViewModel::count() const { return m_provider->count(); }
bool ViewModel::insideArchive() const { return m_provider->insideArchive(); }
QString ViewModel::sourceLabel() const { return m_provider->sourceLabel(); }
bool ViewModel::needPassword() const { return m_provider->needPassword(); }
QString ViewModel::sourcePath() const { return m_provider->sourcePath(); }
QString ViewModel::backendLabel() const { return m_provider->backendLabel(); }
bool ViewModel::atIntegerScale(int i, int w, int h) const { return m_provider->atIntegerScale(i, w, h); }

QString ViewModel::modeLabel() const
{
    if (m_pixelPerfect) return QStringLiteral("像素完美");
    return m_provider->modeName() + QStringLiteral(" / ") + m_provider->qualityName();
}
QString ViewModel::fileLabel() const { return m_provider->pageLabelAt(currentPage()); }
QString ViewModel::relativePath() const
{
    // ★ 必须是"当前**显示的那张图**"：游标可能落在容器条目（压缩包/文件夹边界）上，
    //   那种条目在本界面不可能被显示 → 取当前页的第一个条目（设计 03 §1.3 页↔条目）
    const QVariantList items = pageItems();
    const int it = items.isEmpty() ? m_cursor : items.first().toInt();
    return m_provider->relativePathAt(it);
}
QString ViewModel::sizeLabel() const
{
    const QSize sz = m_provider->pageSourceSizeAt(currentPage());
    return sz.isValid() ? QStringLiteral("%1×%2").arg(sz.width()).arg(sz.height()) : QString();
}
QString ViewModel::pageKindLabel() const { return m_provider->pageKindAt(currentPage()); }
bool ViewModel::pageIsWebtoon() const
{
    const QVariantList items = pageItems();
    return items.size() == 1 && m_provider->isWebtoonAt(items.first().toInt());
}
QVariantList ViewModel::pageItems() const { return m_provider->pageItemsAt(currentPage()); }

// ── 列表项查询 ──────────────────────────────────────────
QString ViewModel::fileNameAt(int i) const { return m_provider->fileNameAt(i); }
QString ViewModel::folderLabelAt(int i) const { return m_provider->folderLabelAt(i); }
int     ViewModel::folderDepthAt(int i) const { return m_provider->folderDepthAt(i); }
bool    ViewModel::isArchiveAt(int i) const { return m_provider->isArchiveAt(i); }
bool    ViewModel::isBoundaryAt(int i) const { return m_provider->isBoundaryAt(i); }
bool    ViewModel::isMissingAt(int i) const { return m_provider->isMissingAt(i); }
int     ViewModel::sliceCount(int i) const { return m_provider->sliceCountAt(i); }

// ── 设置（写入 + 通知）──────────────────────────────────
void ViewModel::setCursor(int i)
{
    if (i < 0) i = 0;
    if (const int n = count(); n > 0 && i >= n) i = n - 1;
    if (m_cursor == i) return;
    m_cursor = i;
    emit cursorChanged();          // 依赖 cursor 的派生属性一起刷新
    emit currentPageChanged();
    emit totalPagesChanged();
}
void ViewModel::setImmersive(bool on)
{
    if (m_immersive == on) return;
    m_immersive = on;
    emit immersiveChanged();
    emit currentPageChanged();
}
void ViewModel::setSlideshow(bool on)
{
    if (m_slideshow == on) return;
    m_slideshow = on;
    emit slideshowChanged();
}
void ViewModel::setIntervalMs(int ms)
{
    ms = qMax(50, ms);          // 下限 50ms 是用户明确需求；上限不设（设计 §3.5：间隔无公认上限）
    if (m_intervalMs == ms) return;
    m_intervalMs = ms;
    Settings::instance().setSlideshowIntervalMs(ms);
    emit intervalMsChanged();
}
void ViewModel::setLoopMode(int m)
{
    m = qBound(0, m, 2);
    if (m_loopMode == m) return;
    m_loopMode = m;
    emit loopModeChanged();
}

void ViewModel::cycleLoop() { setLoopMode((m_loopMode + 1) % 3); }

void ViewModel::intervalBy(int deltaMs)
{
    setIntervalMs(m_intervalMs + deltaMs);   // 步进由调用方给（UI 用 10ms）
}
void ViewModel::setPixelPerfect(bool on)
{
    if (m_pixelPerfect == on) return;
    m_pixelPerfect = on;
    m_provider->setPixelPerfect(on);
    emit pixelPerfectChanged();
    emit renderChanged();
    emit cursorChanged();          // 尺寸/标签可能变
}
void ViewModel::setOverlaysVisible(bool on)
{
    if (m_overlaysVisible == on) return;
    m_overlaysVisible = on;
    emit overlaysVisibleChanged();
}

bool ViewModel::systemDark() const
{
    // Qt 6.5+：跟随系统配色
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#else
    return true;
#endif
}

void ViewModel::setThemeMode(int m)
{
    m = qBound(0, m, 2);
    if (m_themeMode == m) return;
    m_themeMode = m;
    emit themeModeChanged();
}

void ViewModel::setInputFromKeyboard(bool on)
{
    if (m_inputFromKeyboard == on) return;
    m_inputFromKeyboard = on;
    emit inputFromKeyboardChanged();
}

void ViewModel::setFitMode(int m)
{
    m = qBound(0, m, 3);
    if (m_fitMode == m) return;
    m_fitMode = m;
    emit fitModeChanged();
}

void ViewModel::setDoublePage(bool on)
{
    if (m_doublePage == on) return;
    m_doublePage = on;
    emit doublePageChanged();
}

void ViewModel::setZoomLocked(bool on)
{
    if (m_zoomLocked == on) return;
    m_zoomLocked = on;
    emit zoomLockedChanged();
}

void ViewModel::setWindowFullscreen(bool on)
{
    if (m_windowFullscreen == on) return;
    m_windowFullscreen = on;
    emit windowFullscreenChanged();
}

void ViewModel::cycleThemeMode()
{
    setThemeMode((m_themeMode + 1) % 3);    // 跟随系统 → 浅色 → 深色 → 跟随系统
}

void ViewModel::setOsdVisible(bool on)
{
    if (m_osdVisible == on) return;
    m_osdVisible = on;
    emit osdVisibleChanged();
}

// ── 动作 ────────────────────────────────────────────────
void ViewModel::step(int delta) { setCursor(m_cursor + delta); }

void ViewModel::stepPage(int delta)
{
    const int n = totalPages();
    if (n <= 0) return;
    int target = currentPage() + delta;
    if (m_loopMode == 2 && !m_slideshow) {
        // 文件夹循环：收窄到"当前条目所在文件夹"的页区间后环绕
        //   边界条目（FolderBoundary）是**标记项**：文件夹首项 = 上一个边界之后
        int lo = m_cursor, hi = m_cursor;
        while (lo > 0 && !isBoundaryAt(lo - 1)) --lo;
        for (int i = hi + 1; i < count(); ++i) {
            if (isBoundaryAt(i)) break;
            hi = i;
        }
        const int firstPage = m_provider->pageOfItem(lo);
        const int lastPage  = m_provider->pageOfItem(hi);
        if (target > lastPage) target = firstPage;
        else if (target < firstPage) target = lastPage;
    } else if (m_loopMode == 1 || m_slideshow) {
        if (target >= n) target = 0;
        if (target < 0) target = n - 1;
    } else {
        target = qBound(0, target, n - 1);
    }
    if (const int it = firstItemOfPage(target); it >= 0) setCursor(it);
}

void ViewModel::seekToPage(int page)
{
    const int n = totalPages();
    if (n <= 0) return;
    if (const int it = firstItemOfPage(qBound(0, page, n - 1)); it >= 0) setCursor(it);
}

void ViewModel::firstPage() { seekToPage(0); }
void ViewModel::lastPage()  { seekToPage(totalPages() - 1); }

int ViewModel::firstItemOfPage(int page) const
{
    const QVariantList items = m_provider->pageItemsAt(page);
    return items.isEmpty() ? -1 : items.first().toInt();
}

// ── 上一/下一「文件夹」────────────────────────────────────
//   ① 在压缩包内：退出，然后在父目录序列里找上/下一个**压缩包**并进入
//      （读多卷漫画的主路径：一卷读完直接跳下一卷）
//   ② 在目录内（递归模式有文件夹边界）：跳到上/下一个文件夹的首项
void ViewModel::nextArchive() { jumpArchive(+1); }
void ViewModel::prevArchive() { jumpArchive(-1); }

void ViewModel::openPath(const QString &path)
{
    m_provider->setSource(path, false);
    m_cursor = 0;
    emit sequenceChanged();
    emit cursorChanged();
    emit currentPageChanged();
    emit totalPagesChanged();
}

bool ViewModel::enterArchive(int i)
{
    if (!m_provider->enterArchive(i)) return false;
    m_archiveParentIndex = i;
    m_cursor = 0;
    emit sequenceChanged();
    emit cursorChanged();
    emit currentPageChanged();
    emit totalPagesChanged();
    return true;
}

void ViewModel::leaveArchive()
{
    m_provider->leaveArchive();
    m_cursor = 0;
    emit sequenceChanged();
    emit cursorChanged();
    emit currentPageChanged();
    emit totalPagesChanged();
}

void ViewModel::refresh()
{
    m_provider->refresh();
    m_cursor = 0;
    emit sequenceChanged();
    emit cursorChanged();
    emit currentPageChanged();
    emit totalPagesChanged();
}

void ViewModel::prefetch(int i, int w, int h) { m_provider->prefetch(i, w, h); }
void ViewModel::setArchivePassword(const QString &pw) { m_provider->setArchivePassword(pw); }
void ViewModel::syncRenderFlags() { emit renderChanged(); }

// 内部：跨文件夹/压缩包跳转
int ViewModel::neighborFolderIndex(int dir) const
{
    if (insideArchive())
        return -1;                        // 需先离开压缩包才能看到父序列（见头文件注释）
    const int n = count();
    for (int i = m_cursor + dir; i >= 0 && i < n; i += dir) {
        if (isBoundaryAt(i))
            return i + 1 < n ? i + 1 : i;
    }
    return -1;
}

QString ViewModel::folderLabelAtNeighbor(int dir) const
{
    const int i = neighborFolderIndex(dir);
    return i < 0 ? QString() : folderLabelAt(i);
}

void ViewModel::jumpArchive(int dir)
{
    if (insideArchive()) {
        // 退出到父目录，再从进入位置出发找下一个压缩包
        const int from = m_archiveParentIndex;
        leaveArchive();                       // 会把 cursor 归零并刷新序列
        const int n = count();
        for (int i = from + dir; i >= 0 && i < n; i += dir) {
            if (isArchiveAt(i)) {
                m_archiveParentIndex = i;
                if (enterArchive(i)) return;
            }
        }
        // 没有更多压缩包：停在父序列的边界处
        setCursor(qBound(0, from + dir, qMax(0, n - 1)));
        return;
    }
    // 目录内：按文件夹边界跳（判定与 neighborFolderIndex 同一处）
    const int target = neighborFolderIndex(dir);
    if (target >= 0) { setCursor(target); return; }
    // 没有边界（非递归目录）：直接到头/到尾
    setCursor(dir > 0 ? count() - 1 : 0);
}
