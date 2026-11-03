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
#include "progressfile.h"

#include <QObject>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <atomic>

namespace fetch { class ViewerModel; }
class ProgressListModel;
class Sequence;
class QTimer;
class QTranslator;

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

    // ── ★★ 收藏夹 / 进度文件（设计 20）──────────────────────────────────
    //   · 收藏夹 = 一个目录（设置里可改，允许外部路径），里面每个 `.prsave` = 一条进度记录；
    //   · **已收藏** = 当前来源在收藏夹里**有对应的进度文件**（文件名由源路径定，见 progressfile）；
    //   · 只有**扫描完**且来源是文件夹 / 压缩包时才能收藏（单张图不需要进度）。
    Q_PROPERTY(QString favoritesDir READ favoritesDir WRITE setFavoritesDir
                                   NOTIFY favoritesDirChanged)
    Q_PROPERTY(bool sourceFavorited READ sourceFavorited NOTIFY sourceFavoritedChanged)
    //   收藏夹列表（`ui.favorites`）：模型只列文件名 + 按需读内容，见 `ProgressListModel`
    Q_PROPERTY(QObject *favorites READ favorites CONSTANT)

    // ── ★ 扫描中（顶栏那个 spin；点击 = 问一下要不要取消）──
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    Q_PROPERTY(int  discoveredCount READ discoveredCount NOTIFY scanningChanged)
    //   扫描**被取消**（`ScanStoped`）⇒ 不完整：顶栏把 spin 换成「扫描」图标（点击 = 重新扫描），
    //   保存进度前要先警告"未扫描发现的文件不会被重新发现"（用户 2026-10-31）
    Q_PROPERTY(bool scanIncomplete READ scanIncomplete NOTIFY scanIncompleteChanged)
    //   能不能「重新扫描」：有来源、不是正在扫、且来源是文件夹 / 压缩包（单张图没什么可重扫的）
    Q_PROPERTY(bool canRescan READ canRescan NOTIFY scanningChanged)

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
    //   界面语言：**空串 = 跟随系统**；否则语言代码（`zh_CN` / `en` …）。
    //   ★ 值就是存进设置的那一个；换语言 = 换 `QTranslator`（`/i18n/<代码>.qm`，没有就回退源串）。
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    //   实际生效的语言（`language` 为空时取系统 locale）—— 设置页显示"跟随系统（简体中文）"用。
    Q_PROPERTY(QString effectiveLanguage READ effectiveLanguage NOTIFY languageChanged)
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
    QString dialogStartDir() const;      // 上次选过的目录 → 当前来源父目录 → home

    // ── 收藏夹 / 进度文件（设计 20）──
    QString favoritesDir() const;
    void setFavoritesDir(const QString &dir);
    Q_INVOKABLE void chooseFavoritesDir();
    bool sourceFavorited() const { return m_sourceFavorited; }
    ProgressListModel *favorites() const { return m_favorites; }
    bool scanning() const;
    int  discoveredCount() const;
    bool scanIncomplete() const { return m_scanIncomplete; }
    bool canRescan() const;
    //   重新扫描当前来源（用户 2026-11-01）：**后台单开一条序列**跑，扫完再替换当前浏览的；
    //   扫描中**取消 / 退出应用 ⇒ 这条序列作废**（当前视图不受影响）。
    Q_INVOKABLE void rescanSource();
    //   保存 / 打开 / 取消收藏：成功 true；失败已经经 `notify` 说过原因
    //   ★ `savePasswords`：用户是否同意把**本次会话新输入、尚未保存过**的密码写进进度文件
    //     （用户 2026-11-03：已保存过的不再问；即使勾了"相同密码"也按条目分开写）。
    Q_INVOKABLE bool saveProgressToFavorites(bool savePasswords = false);
    Q_INVOKABLE bool saveProgressToFile(bool savePasswords = false);

    // ── ★ 加密压缩包（2026-11-03）：GUI 弹框要密码 ──────────────────────────
    Q_PROPERTY(bool    passwordPromptVisible READ passwordPromptVisible
                                             NOTIFY passwordPromptChanged)
    Q_PROPERTY(QString passwordPromptPath    READ passwordPromptPath
                                             NOTIFY passwordPromptChanged)
    Q_PROPERTY(QString passwordPromptError   READ passwordPromptError
                                             NOTIFY passwordPromptChanged)
    //   ★ **保存密码**勾选（用户 2026-11-03）：只有"这条路上真能写进度文件"时才给（打开进度文件 / 收藏）；
    //     默认勾不勾 = **进度文件里还没存过这个容器的密码**。直接在扫描期（没有进度文件）时不给。
    Q_PROPERTY(bool    passwordPromptCanSave READ passwordPromptCanSave
                                             NOTIFY passwordPromptChanged)
    Q_PROPERTY(bool    passwordPromptSaveDefault READ passwordPromptSaveDefault
                                             NOTIFY passwordPromptChanged)
    //   本次会话**新输入、还没保存过**的密码（保存进度时要问一次）
    Q_PROPERTY(bool    hasUnsavedPasswords   READ hasUnsavedPasswords
                                             NOTIFY passwordPromptChanged)
    //   `reuseForSubsequent` = 弹框里勾了"后续压缩包使用相同密码"
    //   `saveToFile`：解码期弹框里勾了"保存密码" ⇒ 立刻写进关联的进度文件（用户 2026-11-03）
    Q_INVOKABLE void submitPassword(const QString &pw, bool reuseForSubsequent,
                                    bool saveToFile = false);
    Q_INVOKABLE void cancelPassword();
    Q_INVOKABLE void openProgressFile();
    Q_INVOKABLE bool openProgress(const QString &filePath);
    //   删掉一份进度文件（收藏夹列表的"移除"）：删完刷新"已收藏"与列表
    Q_INVOKABLE bool removeProgressFile(const QString &filePath);
    Q_INVOKABLE bool removeFavorite();
    //   停止扫描（顶栏 spin 的确认框 → 这里）：**只是请求**，生产者自己退，界面照旧可看已有内容
    Q_INVOKABLE void cancelScan();

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
    QString language() const;            // 空 = 跟随系统
    void setLanguage(const QString &code);
    QString effectiveLanguage() const;   // 实际生效的语言代码（系统 locale 也解析过了）
    void setSoftwareBackend(bool on);   // 只给 main.cpp 用（探测到 graphicsApi 之后写一次）

Q_SIGNALS:
    void viewerChanged();
    void favoritesDirChanged();
    void sourceFavoritedChanged();
    void scanningChanged();
    void scanIncompleteChanged();
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
    void passwordPromptChanged();
    void languageChanged();
    void slideshowIntervalMsChanged();

private:
    // 一组来源 —— 换来源 ⇒ 整组换掉（序列归本组，viewer 也归本组）
    struct Source {
        Sequence           *seq = nullptr;
        fetch::ViewerModel *vm  = nullptr;
    };

    void buildViewer();                 // 用 m_path 新建一组；emit viewerChanged；旧组延迟释放
    void installViewer(Source s);       // 接线 + 换绑 + 旧组延迟释放
    void releaseLater(const Source &s); // 旧组：随旧 ViewerModel 的析构释放其余成员
    static void releaseNow(Source &s);

    // 会话序列：本类是唯一消费者（发布钩子 → 合并成一次 queued 投递 → 一次 drain 到空）
    void pumpSession();
    void onSequenceChanged();           // 序列长了 / 收尾了 ⇒ 通知 viewer
    void applyViewerDefaults();         // 新建 viewer ⇒ 把全局默认下发一次
    //   进度文件：把当前来源写成 `filePath`（保存的两条路 + 自动保存共用这一份实现）
    bool writeProgress(const QString &filePath, bool quiet = false,
                       bool savePasswords = false);
    // 加密压缩包：检查"这次扫描是不是卡在密码上"
    void checkPasswordNeeded();
    void restartScan();                 // 同一个序列再扫（提交密码后）
    bool passwordPromptVisible() const { return !m_pendingPasswordKey.isEmpty(); }
    QString passwordPromptPath() const { return m_passwordPromptPath; }
    QString passwordPromptError() const { return m_passwordPromptError; }
    bool passwordPromptCanSave() const { return m_passwordPromptCanSave; }
    bool passwordPromptSaveDefault() const { return m_passwordPromptSaveDefault; }
    bool hasUnsavedPasswords() const;
    //   弹框里的"哪个文件"：容器路径 → **相对来源**的显示文本（见 .cpp）
    QString passwordPromptLabel(const QString &key) const;
    //   `onPasswordRequired()` 延迟复核通过后的真正弹框动作
    void askPasswordNow(int item, int level);

    // ── ★ 自动保存（用户 2026-11-01）────────────────────────────────────────
    //   只有"当前来源**已经关联**某个进度文件"时才写（打开进度文件 / 刚保存过）；
    //   时机：① **固定间隔**（`kAutosaveMs`）② **运行时序列销毁前**（切来源 / 退出）。
    //   前提：扫描**完整**（`Scaned`；被取消的不覆盖旧记录）、文件还在（被删了就停止关联，
    //   不"复活"它）、且确实有变化（位置 / 版面 / 行）。
    static constexpr int kAutosaveMs = 30000;
    void autosaveProgress();            // 到点 / 销毁前：有变化且条件满足才写（静默）
    void startTrackingProgress(const QString &file);   // 关联 + 启动定时器
    void stopTrackingProgress();                       // 解除关联 + 停定时器
    void markProgressDirty() { m_progressDirty = true; }
    //   记下"文件里现在是什么状态"（**用刚写/刚读的那份 meta**，不读运行态 ——
    //   恢复时 viewer 的位置是异步落定的，读运行态会把 0 记成"已保存"）
    void snapshotSavedState(const progressfile::Meta &m, int rowCount);
    bool progressChangedSinceSave() const;
    bool canRescanNow() const;

    // ── 后台重扫的槽位（同一时刻只有一条）──
    void startRescan();
    void onRescanChanged();          // 唤醒汇（合并投递）→ drain → 完成就替换
    void applyRescan();              // 用新序列替换当前（位置按 **链 + 页号** 找回）
    void cancelRescan();             // 作废：停止 + 析构，**不替换**
    QString currentFavoritesFile() const;   // 空 = 没来源 / 来源不是文件夹或压缩包
    void refreshFavorited();                // 值真变才 emit
    void refreshScanning();
    //   幻灯片播放时抑制屏幕休眠（org.freedesktop.ScreenSaver），停下就解除。
    void updateScreenSaverInhibit();                 // 扫描中 / 已发现数（值真变才 emit）
    void dropSource();                  // 释放当前来源（先停播，再拆组）

    // ★★ 运行期设置的**唯一实例**（没有单例、没有第二个副本）
    AppSettings m_settings;
    // ── 加密压缩包：弹框**瞬态**（密码表住在 `Sequence` 里 —— 密码随来源）────────
    int                     m_pendingPasswordItem = -1;   // 解码期：等密码的条目（-1 = 扫描期）
    void onPasswordRequired(int item, int level);         // 解码期检测（设计 22 §5.3）
    QString                 m_pendingPasswordKey;   // 正在等密码的容器（空 = 没在等）
    //   ★ 弹框**延迟一拍**再决定（打开进度文件时会有"先落到 0 再跳到链位置"的瞬态）：
    //     到点再看"这一屏是不是真的还空着"，空着才弹 —— 否则会为瞬态画面连弹好几个卷。
    int                     m_passwordAskToken = 0;
    int                     m_passwordAskItem = -1;
    int                     m_passwordAskLevel = 0;
    //   ★ 这个框是**哪条序列**问的：会话序列（扫描 / 解码）或后台「重新扫描」那条
    //     （两条同时存在时，提交密码要写回"问的人"，见设计 22 §5.1）
    Sequence               *m_passwordOwner = nullptr;
    QString                 m_passwordPromptPath;
    QString                 m_passwordPromptError;
    bool                    m_passwordPromptCanSave = false;      // 这条路上有进度文件可写
    bool                    m_passwordPromptSaveDefault = false;  // 复选框默认值
    //   界面翻译：**只有一份**（换语言时摘掉旧的、装新的）。空 = 没有 .qm（源串即英文）。
    QTranslator *m_translator = nullptr;
    void applyLanguage();               // 按 m_settings.language() 装/摘翻译
    bool m_softwareBackend = false;     // 场景图后端 = software（main.cpp 探测后写一次）

    Source  m_src;
    QString m_path;                     // 最近一次打开的来源（重建 viewer 要用）

    std::atomic<bool> m_wakeQueued{false};   // 发布钩子 → **合并成一次** queued 投递

    // 来源失败 / 不支持 / 扫描期提示：查一次（扫描线程产出，GUI 侧轮询；扫完即停）
    QTimer *m_sessionProbe = nullptr;
    QString m_sessionNotice;            // 最近一条（去重）

    QString m_notification;             // 最近的瞬时提示文案（空 = 没有）
    int     m_notificationSeq = 0;      // 每次 +1 ⇒ 同文案也能重新触发动画

    // 收藏 / 扫描状态（派生值，只在这里缓存"上次 emit 的值"）
    bool m_sourceFavorited = false;
    bool m_scanning = false;
    //   扫描开始时刻（毫秒）——只用来判断"这次扫描够不够久，值得弹一条完成提示"
    qint64 m_scanStartedMs = 0;
    uint   m_inhibitCookie = 0;        // ScreenSaver Inhibit 的 cookie（0 = 没抑制）
    bool m_scanIncomplete = false;
    bool m_canRescan = false;
    //   后台重扫（与当前序列并行；完成后替换）
    Sequence  *m_rescanSeq = nullptr;
    std::atomic<bool> m_rescanQueued{false};
    QStringList m_rescanChain;
    int         m_rescanPage = 0;
    int  m_discovered = 0;
    //   CLI `--index N`：打开时序列可能还没扫出来 ⇒ 先记下来，等条目够了再跳（一次）
    int     m_pendingJump = -1;
    QString m_lastProgressDir;          // 进度文件对话框的起始目录（会话级）
    QString m_lastDialogDir;            // 打开对话框的起始目录（会话级）：上次成功选过的地方
    QString m_progressFile;             // 当前来源关联的进度文件（空 = 不自动保存）
    QTimer *m_autosave = nullptr;       // 固定间隔的自动保存
    bool    m_progressDirty = false;    // 位置 / 版面 / 行变过（没变就不重写）
    //   文件里当前记着的位置（用来判断"真的变了没有"，避免每 30 s 重写一份一样的）
    QStringList m_savedChain;
    int     m_savedPage = -1;
    int     m_savedLayout = -1;
    int     m_savedRows = 0;
    ProgressListModel *m_favorites = nullptr;   // 收藏夹列表（QML 的 `ui.favorites`）

    // 会话状态（不写入磁盘）
    bool m_windowFullscreen = false;
    bool m_inputFromKeyboard = false;   // 焦点环只在键盘导航时显示（UI 纪律 07 §五）
};

} // namespace app
