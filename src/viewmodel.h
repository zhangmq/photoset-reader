#pragma once

#include <QObject>
#include <QVariantList>
#include <QSize>

class ImageProvider;

// ViewModel —— UI 的**唯一接口**（架构对齐 ~/projects/vsr-player 的 PlayerViewModel）
//
// 规则（违反即回退到"UI 先做"的老路）：
//   1. QML **禁止**直接引用 `provider.*`；一切经 `ui.*`
//   2. QML 组件**只读**属性、**只发**信号；禁止给自己/给 ViewModel 的输入属性赋值
//   3. 任何"同一事实的两份状态 + 同步"都是错的：能派生的不许存
//
// 状态归属：
//   ViewModel 持有：游标 / 沉浸开关 / 幻灯片 / 间隔 / 循环 / 像素完美 / OSD / 浮层
//   ImageProvider 持有：序列 / 页序列 / 缓存 / 预取（数据层，QML 不直接碰）
//   Settings 持有：持久化
class ViewModel : public QObject
{
    Q_OBJECT

    // ── 导航（ViewModel 持有）────────────────────────────
    Q_PROPERTY(int  cursor      READ cursor      WRITE setCursor      NOTIFY cursorChanged)
    Q_PROPERTY(bool immersive   READ immersive   WRITE setImmersive   NOTIFY immersiveChanged)
    Q_PROPERTY(int  currentPage READ currentPage                        NOTIFY currentPageChanged)
    Q_PROPERTY(int  totalPages  READ totalPages                         NOTIFY totalPagesChanged)

    // ── 视图状态（ViewModel 持有）────────────────────────
    Q_PROPERTY(bool slideshow         READ slideshow         WRITE setSlideshow         NOTIFY slideshowChanged)
    Q_PROPERTY(int  intervalMs        READ intervalMs        WRITE setIntervalMs        NOTIFY intervalMsChanged)
    // 循环模式：0=不循环 1=全部循环 2=文件夹循环（用户 2026-10-11 要求三态）
    Q_PROPERTY(int  loopMode          READ loopMode          WRITE setLoopMode          NOTIFY loopModeChanged)
    Q_PROPERTY(bool pixelPerfect      READ pixelPerfect      WRITE setPixelPerfect      NOTIFY pixelPerfectChanged)
    Q_PROPERTY(bool osdVisible        READ osdVisible        WRITE setOsdVisible        NOTIFY osdVisibleChanged)
    // 浮层显隐（对齐 vsr-player 的 overlaysVisible）：由 QML 的**纯函数**算出来写回这里
    Q_PROPERTY(bool overlaysVisible   READ overlaysVisible   WRITE setOverlaysVisible   NOTIFY overlaysVisibleChanged)
    // 全屏：画面缩放模式（0=原始大小 1=适应屏幕 2=适应宽度 3=适应高度）
    Q_PROPERTY(int  fitMode    READ fitMode    WRITE setFitMode    NOTIFY fitModeChanged)
    // 全屏：**锁定缩放**开关（UI 状态；交互缩放/拖拽行为 demo 阶段未实现，需求见设计 §3.8）
    Q_PROPERTY(bool zoomLocked READ zoomLocked WRITE setZoomLocked NOTIFY zoomLockedChanged)
    // 窗口全屏（F11 语义；与"沉浸视图"不是一回事）
    Q_PROPERTY(bool windowFullscreen READ windowFullscreen WRITE setWindowFullscreen NOTIFY windowFullscreenChanged)
    // 全屏：阅读版面（false=单幅 true=双幅）
    Q_PROPERTY(bool doublePage READ doublePage WRITE setDoublePage NOTIFY doublePageChanged)
    // 主题：0=跟随系统 1=浅色 2=深色（用户要求：点击循环这三态）
    Q_PROPERTY(int  themeMode         READ themeMode         WRITE setThemeMode         NOTIFY themeModeChanged)
    Q_PROPERTY(bool systemDark        READ systemDark                                   NOTIFY systemDarkChanged)
    // 输入来源：最后一次交互是键盘还是鼠标 —— 供 QML 实现 focus-visible
    //   （焦点环只在**键盘**操作时显示；Qt Quick 不区分"键盘焦点/鼠标焦点"，所以自己记）
    Q_PROPERTY(bool inputFromKeyboard READ inputFromKeyboard WRITE setInputFromKeyboard NOTIFY inputFromKeyboardChanged)

    // ── 展示信息（派生 / 转发）───────────────────────────
    Q_PROPERTY(int     count          READ count                                        NOTIFY sequenceChanged)
    Q_PROPERTY(bool    insideArchive  READ insideArchive                                NOTIFY sequenceChanged)
    Q_PROPERTY(QString sourceLabel    READ sourceLabel                                  NOTIFY sequenceChanged)
    Q_PROPERTY(QString modeLabel      READ modeLabel                                    NOTIFY renderChanged)
    Q_PROPERTY(QString fileLabel      READ fileLabel                                    NOTIFY cursorChanged)
    Q_PROPERTY(QString relativePath   READ relativePath                                 NOTIFY cursorChanged)   // 相对图集根
    Q_PROPERTY(QString sizeLabel      READ sizeLabel                                    NOTIFY cursorChanged)
    Q_PROPERTY(QString pageKindLabel  READ pageKindLabel                                NOTIFY cursorChanged)
    Q_PROPERTY(bool    pageIsWebtoon  READ pageIsWebtoon                                NOTIFY cursorChanged)
    Q_PROPERTY(QVariantList pageItems READ pageItems                                    NOTIFY cursorChanged)
    Q_PROPERTY(bool    needPassword   READ needPassword                                 NOTIFY sequenceChanged)
    Q_PROPERTY(QString sourcePath     READ sourcePath                                   NOTIFY sequenceChanged)
    Q_PROPERTY(QString backendLabel   READ backendLabel                                 NOTIFY sequenceChanged)
    // 全屏底部条：按钮上显示**目标文件夹的名字**（设计 §3.5）
    Q_PROPERTY(QString prevFolderLabel READ prevFolderLabel NOTIFY cursorChanged)
    Q_PROPERTY(QString nextFolderLabel READ nextFolderLabel NOTIFY cursorChanged)

public:
    explicit ViewModel(ImageProvider *provider, QObject *parent = nullptr);

    // 导航
    int  cursor() const { return m_cursor; }
    bool immersive() const { return m_immersive; }
    int  currentPage() const;
    int  totalPages() const;

    // 视图状态
    bool slideshow() const { return m_slideshow; }
    int  intervalMs() const { return m_intervalMs; }
    int  loopMode() const { return m_loopMode; }
    bool pixelPerfect() const { return m_pixelPerfect; }
    bool osdVisible() const { return m_osdVisible; }
    bool overlaysVisible() const { return m_overlaysVisible; }
    int  themeMode() const { return m_themeMode; }
    bool systemDark() const;
    bool inputFromKeyboard() const { return m_inputFromKeyboard; }
    int  fitMode() const { return m_fitMode; }
    bool doublePage() const { return m_doublePage; }
    bool zoomLocked() const { return m_zoomLocked; }
    bool windowFullscreen() const { return m_windowFullscreen; }

    // 展示信息
    int     count() const;
    bool    insideArchive() const;
    QString sourceLabel() const;
    QString modeLabel() const;
    QString fileLabel() const;
    QString relativePath() const;
    QString sizeLabel() const;
    QString pageKindLabel() const;
    bool    pageIsWebtoon() const;
    QVariantList pageItems() const;
    bool    needPassword() const;
    QString sourcePath() const;
    QString backendLabel() const;
    QString prevFolderLabel() const { return folderLabelAtNeighbor(-1); }
    QString nextFolderLabel() const { return folderLabelAtNeighbor(+1); }
    Q_INVOKABLE bool atIntegerScale(int i, int w, int h) const;

    // 列表项查询（供委托用；QML 不再碰 provider）
    Q_INVOKABLE QString fileNameAt(int i) const;
    Q_INVOKABLE QString folderLabelAt(int i) const;
    Q_INVOKABLE int     folderDepthAt(int i) const;
    Q_INVOKABLE bool    isArchiveAt(int i) const;
    Q_INVOKABLE bool    isBoundaryAt(int i) const;
    Q_INVOKABLE bool    isMissingAt(int i) const;
    Q_INVOKABLE int     sliceCount(int i) const;

    // ── 动作（QML 只调这些）──────────────────────────────
    Q_INVOKABLE void step(int delta);              // 条目级（浏览视图）
    Q_INVOKABLE void stepPage(int delta);          // 页级（沉浸视图）
    Q_INVOKABLE void seekToPage(int page);         // 进度条点击
    Q_INVOKABLE void firstPage();
    Q_INVOKABLE void lastPage();
    Q_INVOKABLE void nextArchive();                // 下一文件夹/压缩包
    Q_INVOKABLE void prevArchive();                // 上一文件夹/压缩包
    Q_INVOKABLE void openPath(const QString &path);
    Q_INVOKABLE bool enterArchive(int i);
    Q_INVOKABLE void leaveArchive();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void prefetch(int i, int w, int h);
    Q_INVOKABLE void setArchivePassword(const QString &pw);
    Q_INVOKABLE void toggleSlideshow() { setSlideshow(!m_slideshow); }
    Q_INVOKABLE void toggleOsd() { setOsdVisible(!m_osdVisible); }
    Q_INVOKABLE void togglePixelPerfect() { setPixelPerfect(!m_pixelPerfect); }
    Q_INVOKABLE void cycleLoop();                       // 三态循环：不循环 → 全部 → 文件夹 → 不循环
    // 切换速度：加减速（步进 10ms；**下限 50ms 是用户明确需求**，上限不设）
    Q_INVOKABLE void intervalBy(int deltaMs);
    Q_INVOKABLE void closeImmersive() { setImmersive(false); }

public slots:
    void setCursor(int i);
    void setImmersive(bool on);
    void setSlideshow(bool on);
    void setIntervalMs(int ms);
    void setLoopMode(int m);
    void setPixelPerfect(bool on);
    void setOsdVisible(bool on);
    void setOverlaysVisible(bool on);
    void setThemeMode(int m);
    void setInputFromKeyboard(bool on);
    void setFitMode(int m);
    void setDoublePage(bool on);
    void setZoomLocked(bool on);
    void setWindowFullscreen(bool on);
    Q_INVOKABLE void cycleThemeMode();      // 跟随系统 → 浅色 → 深色 → 跟随系统

signals:
    void cursorChanged();
    void immersiveChanged();
    void currentPageChanged();
    void totalPagesChanged();
    void sequenceChanged();
    void renderChanged();
    void slideshowChanged();
    void intervalMsChanged();
    void loopModeChanged();
    void pixelPerfectChanged();
    void osdVisibleChanged();
    void overlaysVisibleChanged();
    void themeModeChanged();
    void systemDarkChanged();
    void inputFromKeyboardChanged();
    void fitModeChanged();
    void doublePageChanged();
    void zoomLockedChanged();
    void windowFullscreenChanged();

private:
    void syncRenderFlags();
    void jumpArchive(int dir);
    // 「上/下一个文件夹」的目标条目下标（**不导航**）；-1 = 没有。
    // 与 jumpArchive 的目录分支**同一套判定**（§1.2 算法单一事实源）。
    // 压缩包内需要先离开才能看到父序列，故此时返回 -1（按钮置灰）。
    int  neighborFolderIndex(int dir) const;
    QString folderLabelAtNeighbor(int dir) const;
    int  firstItemOfPage(int page) const;

    ImageProvider *m_provider = nullptr;
    int  m_archiveParentIndex = -1;   // 进入压缩包前，它在父序列里的索引

    int  m_cursor = 0;
    bool m_immersive = false;
    bool m_slideshow = false;
    int  m_intervalMs = 300;
    int  m_loopMode = 0;
    bool m_pixelPerfect = false;
    bool m_osdVisible = false;      // ★ 默认不显示（用户要求 #7）
    bool m_overlaysVisible = false;
    int  m_themeMode = 0;
    bool m_inputFromKeyboard = false;
    int  m_fitMode = 1;            // 默认适应屏幕
    bool m_doublePage = false;
    bool  m_zoomLocked = false;
    bool  m_windowFullscreen = false;
};
