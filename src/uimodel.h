#pragma once

// ══ 应用**唯一的全局 VM**（QML 里的 `ui`）────────────────────────────────
//
//   2026-10-31 定案（用户）：**只剩一个查看界面**，「浏览」屏与「图集库」整块砍掉。
//     · 本类 = **应用级的外壳**：打开来源 / 打开对话框 / 全部设置 / UI 形态 / 瞬时提示
//       ⇒ 后续要挂在这个界面上的东西（收藏夹等）也挂这里；
//     · `fetch::ViewerModel`（QML 的 `ui.viewer`）= **阅读态**，**随来源建毁**
//       （换来源 = 整组换掉；它不 join 工作线程）。
//       为什么不把 viewer 合并进来：两者生命周期不同（应用级 vs 来源级）——
//       合并后每次换来源都要"重置一批成员"，就是 AGENTS 禁止的"同一事实两份状态 + 同步逻辑"。
//     · QML 一律**直绑** `ui.viewer.*` / 本类属性，本类**不转发** viewer 的属性。
//
//   ## 来源（只有一种：会话来源）
//     · 打开文件夹 / 压缩包 / 文件 = `Sequence::startScan` 建序列 → 建 viewer → 看；
//     · **什么都不写盘**：进度、扫描结果都只在内存里，关掉就没了（用户 2026-10-31 定案：
//       只有显式的「保存阅读进度」那类动作才留记录 —— 那一批还没做）。
//     · 序列只有一个消费者 = 本类（GUI 线程）：`Sequence::drain()` 归这里。
//       唤醒 = `Sequence::setPublishHook`（生产者放完一批 / 写终态时调）→ 合并成一次
//       queued 投递 → 一次 drain 到空 → 通知 viewer（`sequenceChanged`）。
//
//   ## 设置
//     · 运行期设置 = 本类持有的一个 `AppSettings` 成员（没有单例；除本类外都不读设置，
//       需要什么值由本类调用时传参：扫描参数 / 预取长度 / 缩放参数…）。
//     · 会话级（不写入磁盘）：windowFullscreen / inputFromKeyboard。
//     · 阅读视图态（fit/zoom/pan/锁定/版面/循环/间隔/算法/OSD）**不在本类** —— 归 `ViewerModel`；
//       本类只在建 viewer 时把**全局默认**下发一次（`applyViewerDefaults`）。

#include "appsettings.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <atomic>

namespace fetch { class ViewerModel; }
class Sequence;
class QTimer;

namespace app {

class UiModel : public QObject
{
    Q_OBJECT

public:
    // ── viewer（只读指针；属性一律 `ui.viewer.*` 直绑；没有来源时 = null）──
    Q_PROPERTY(QObject *viewer READ viewer NOTIFY viewerChanged)

    // ── ★ 瞬时提示：**不占位浮层** ──────────────────────────────────────────
    //   刻意**不是**持久状态：`notificationSeq` 每次 +1 ⇒ 同一条文案重复出现也能重新触发动画。
    //   来源 = 扫描失败 / 压缩包打不开 / 不支持（`Sequence::unsupportedReason()`）等 ——
    //   **一律瞬时提示 + 日志，不写入磁盘**。
    Q_PROPERTY(QString notification READ notification NOTIFY notificationChanged)
    Q_PROPERTY(int notificationSeq READ notificationSeq NOTIFY notificationChanged)

    // ── 全局默认（设置页写盘；不是当前阅读态）──
    Q_PROPERTY(int   fitMode       READ fitMode       WRITE setFitMode       NOTIFY fitModeChanged)
    Q_PROPERTY(int   defaultLayout READ defaultLayout WRITE setDefaultLayout NOTIFY defaultLayoutChanged)
    Q_PROPERTY(int   defaultLoop   READ defaultLoop   WRITE setDefaultLoop   NOTIFY defaultLoopChanged)

    // ── UI 形态（**会话级**，不写入磁盘）──
    Q_PROPERTY(bool windowFullscreen READ windowFullscreen WRITE setWindowFullscreen
                                    NOTIFY windowFullscreenChanged)

    // ── 设置（值全在本类持有的 `AppSettings` 里；QML 侧名字与旧 VM 一致）──
    Q_PROPERTY(int  themeMode READ themeMode WRITE setThemeMode NOTIFY themeModeChanged)
    Q_PROPERTY(bool systemDark READ systemDark NOTIFY systemDarkChanged)
    Q_PROPERTY(bool inputFromKeyboard READ inputFromKeyboard WRITE setInputFromKeyboard
                                      NOTIFY inputFromKeyboardChanged)
    Q_PROPERTY(bool scanHidden READ scanHidden WRITE setScanHidden NOTIFY scanHiddenChanged)
    Q_PROPERTY(int  recurseDepth READ recurseDepth WRITE setRecurseDepth NOTIFY recurseDepthChanged)
    Q_PROPERTY(int  prefetchWindow READ prefetchWindow WRITE setPrefetchWindow
                                    NOTIFY prefetchWindowChanged)
    //   内层压缩包上限（MB）：超过上限的内层压缩包不展开（判定在解压前）
    Q_PROPERTY(int  innerArchiveMaxMb READ innerArchiveMaxMb WRITE setInnerArchiveMaxMb
                                       NOTIFY innerArchiveMaxMbChanged)
    //   图像（缩放）——唯一选项 = 算法（nearest | smooth | bicubic | lanczos2 | lanczos3）
    Q_PROPERTY(QString scaleAlgorithm READ scaleAlgorithm WRITE setScaleAlgorithm
                                       NOTIFY scaleAlgorithmChanged)
    //   场景图后端是不是软件渲染（只读，main.cpp 在 sceneGraphInitialized 时写一次）。
    Q_PROPERTY(bool softwareBackend READ softwareBackend NOTIFY softwareBackendChanged)

    // ── 幻灯片间隔（`AppSettings` 支撑；值的家 = `m_settings`）──────────────
    Q_PROPERTY(int slideshowIntervalMs READ slideshowIntervalMs WRITE setSlideshowIntervalMs
                                       NOTIFY slideshowIntervalMsChanged)

public:
    explicit UiModel(QObject *parent = nullptr);
    ~UiModel() override;

    UiModel(const UiModel &) = delete;
    UiModel &operator=(const UiModel &) = delete;

    QObject *viewer() const;            // 定义在 .cpp（ViewerModel 只前向声明 ⇒ 需要完整类型转换）
    // C++ 侧（调试入口 / 用例）拿具体类型；QML 只认上面那个 QObject*
    fetch::ViewerModel *viewerCore() const { return m_src.vm; }

    // ── 打开来源（文件夹 / 压缩包 / 文件 = 同一套：建序列 → 看）──────────────
    //   ★ QML 入口（拖放）给的是 `drop.urls` 的 **file: URL** ⇒ 这一版负责转成本地路径；
    //     非本地 URL 一律忽略。
    //   `index >= 0` = 定位到第 index 条（只有 CLI `--index N` 用）；不给 ⇒ 第 0 条。
    Q_INVOKABLE void openPath(const QUrl &fileUrl);
    void openPath(const QString &path, int index = -1);

    // ── 打开对话框（**QFileDialog 只在这里**；QML 不写平台相关代码）────────────
    //   界面入口随右键菜单那一批接上（用户 2026-10-31：打开相关放右键菜单）。
    Q_INVOKABLE void chooseOpenFolder();
    Q_INVOKABLE void chooseOpenArchive();
    Q_INVOKABLE void chooseOpenImage();

    // ★ 瞬时提示（浮层）：任何失败 / 不支持都走它（**不写入磁盘**）
    Q_INVOKABLE void notify(const QString &text);
    QString notification() const { return m_notification; }
    int     notificationSeq() const { return m_notificationSeq; }

    // ── 幻灯片间隔（读写；值的家在 `m_settings`）──
    int  slideshowIntervalMs() const;
    void setSlideshowIntervalMs(int ms);

    // ── 全局默认 ──
    int   fitMode() const;
    void  setFitMode(int m);
    int   defaultLayout() const;
    void  setDefaultLayout(int v);
    int   defaultLoop() const;
    void  setDefaultLoop(int v);

    // ── UI 形态 ──
    bool windowFullscreen() const { return m_windowFullscreen; }
    void setWindowFullscreen(bool on);

    // ── 设置（读写；值都在本类持有的 `m_settings` 里）──
    int  themeMode() const;
    void setThemeMode(int m);
    bool systemDark() const;
    Q_INVOKABLE void cycleThemeMode();
    bool inputFromKeyboard() const { return m_inputFromKeyboard; }
    void setInputFromKeyboard(bool on);
    bool scanHidden() const;
    void setScanHidden(bool on);
    int  recurseDepth() const;
    void setRecurseDepth(int v);
    int  prefetchWindow() const;
    void setPrefetchWindow(int v);
    int  innerArchiveMaxMb() const;
    void setInnerArchiveMaxMb(int v);
    QString scaleAlgorithm() const;
    void setScaleAlgorithm(const QString &v);
    bool softwareBackend() const { return m_softwareBackend; }
    void setSoftwareBackend(bool on);   // 只给 main.cpp 用（探测到 graphicsApi 之后写一次）

Q_SIGNALS:
    void viewerChanged();
    void notificationChanged();        // ★ 瞬时提示（浮层）
    void fitModeChanged();
    void defaultLayoutChanged();
    void defaultLoopChanged();
    void windowFullscreenChanged();
    void themeModeChanged();
    void systemDarkChanged();
    void inputFromKeyboardChanged();
    void scanHiddenChanged();
    void recurseDepthChanged();
    void prefetchWindowChanged();
    void innerArchiveMaxMbChanged();
    void scaleAlgorithmChanged();
    void softwareBackendChanged();
    void slideshowIntervalMsChanged();

private:
    // 一组来源 —— 换来源 ⇒ 整组换掉（序列归本组，viewer 也归本组）
    struct Source {
        Sequence           *seq = nullptr;
        fetch::ViewerModel *vm  = nullptr;
    };

    void buildViewer();                 // 用 m_path 新建一组；emit viewerChanged；旧组延迟释放
    void rebuildViewer();               // 设置类变化 ⇒ 换一组（带上位置）
    void installViewer(Source s);       // 接线 + 换绑 + 旧组延迟释放
    void releaseLater(const Source &s); // 旧组：随旧 ViewerModel 的析构释放其余成员
    static void releaseNow(Source &s);

    // 会话序列：本类是唯一消费者（发布钩子 → 合并成一次 queued 投递 → 一次 drain 到空）
    void pumpSession();
    void onSequenceChanged();           // 序列长了 / 收尾了 ⇒ 通知 viewer
    void applyViewerDefaults();         // 新建 viewer ⇒ 把全局默认下发一次
    void dropSource();                  // 释放当前来源（先停播，再拆组）

    // ★★ 运行期设置的**唯一实例**（没有单例、没有第二个副本）
    AppSettings m_settings;
    bool m_softwareBackend = false;     // 场景图后端 = software（main.cpp 探测后写一次）

    Source  m_src;
    QString m_path;                     // 最近一次打开的来源（重建 viewer 要用）

    std::atomic<bool> m_wakeQueued{false};   // 发布钩子 → **合并成一次** queued 投递

    // 来源失败 / 不支持 / 扫描期提示：查一次（扫描线程产出，GUI 侧轮询；扫完即停）
    QTimer *m_sessionProbe = nullptr;
    QString m_sessionNotice;            // 最近一条（去重）

    QString m_notification;             // 最近的瞬时提示文案（空 = 没有）
    int     m_notificationSeq = 0;      // 每次 +1 ⇒ 同文案也能重新触发动画

    // 会话状态（不写入磁盘）
    bool m_windowFullscreen = false;
    bool m_inputFromKeyboard = false;   // 焦点环只在键盘导航时显示（UI 纪律 07 §五）
};

} // namespace app
