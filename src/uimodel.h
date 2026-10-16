#pragma once

// ══ 应用**唯一的全局 VM**（QML 里的 `ui`）────────────────────────────────
//
//   定案（用户 2026-10-13；**2026-10-15 T2d-1b 重定**）：
//     · 全局只有一个 VM = 本类；QML 里仍叫 `ui`。
//     · **三个界面各有一个 model，并列挂在 `ui` 这一级**（照 `ui.library` 的样子）：
//         `ui.library`（LibraryModel，已有不动）/ `ui.browse`（**`app::BrowseModel`**）/
//         `ui.viewer`（**`fetch::ViewerModel`**，对外名字仍是它）。QML 一律**直绑**，
//       本类**不转发**它们的属性（转手就是两份状态，AGENTS.md「单一来源」）。
//     · ★★ **序列只有一个消费者：本类（GUI 线程）**：`Sequence::drain()` 归这里。
//       会话序列的唤醒 = `Sequence::setPublishHook`（生产者放完一批 / 写终态时调）
//       → **合并成一次** queued 投递 → 本类一次 drain 到空 → 通知 viewer（`sequenceChanged`）。
//       图集来源的序列归 `LibraryCore`，由它的 `pump()` 在**同一线程** drain（本类听 `pumped`）。
//     · `fetch::ViewerModel` = **viewer**：位置 / 预取 / 槽位表 / 上屏内容全在它内部
//       （解码在工作线程上做，解完 post 回主线程）。**本类只"给序列 + 序列变了通知"**；
//       `loading` 的判定条件也在它那边。
//     · 本类负责：三屏状态（Library / Browse / Viewer）、图集库动作、**全部设置**、
//       UI 形态、**适配规则 fitMode / 手势 zoom·pan / zoomLocked**（缩放约束只在本类与
//       GPU 缩放器之间，viewer 不需要知道），以及**图集来源的阅读位置写入磁盘**。
//     · 设计 14 §9.3：**没有 Settings 单例** —— 运行期设置就是本类持有的一个
//       `AppSettings` 成员（文件读写照旧）；**除本类外任何组件都不读设置**，
//       需要什么值由本类**调用时传参**（扫描参数 / 预取窗口 / 缩放参数…）。
//
//   ## 图集来源 vs 会话来源（**只有图集来源写入磁盘**）
//     · ★ T2c-2b-1：打开集合 = **`LibraryCore::openCollection(id)`**（core 把磁盘上的行在内存
//       重建一条**静态只读**序列）→ viewer/browse 读 `core->viewSequence()`
//       （**不做视图镜像**）
//       → 位置 = `seq->indexOfChain(decodeChain(resume_key), resume_page)`（<0 ⇒ 0）→ `jump(index)`；
//       此后游标变化 **400ms 去抖 + 5s 兜底** 写 `core->setResume(id, key, page)`
//       （**存链 + 页号，不存下标**）。
//       ⚠️ 会话序列归本类、图集序列归 core；两者都**只在本类（GUI 线程）被碰**。
//          换来源 / 回库屏时**先同步拆 viewer**（换个记号就走，不等工作线程），
//          再让 core 销毁它的序列。viewer 的解码线程只拿一份**条目信息的副本** ⇒ 不碰序列。
//     · 直接打开（文件夹 / 压缩包 / 单图）= **会话来源**：用 `Sequence::startScan` 建序列，
//       **不写入磁盘**（用户"想切就切"）。
//       ★ viewer 的解码线程只拿一份**条目信息的副本** ⇒ core 的活序列与本类的会话序列
//          都只有本类一个消费者。
//
//   ## viewer 的创建 / 更换（本类持有）
//     每次换来源 = 新建 `{Sequence, ViewerModel}` **一组**；参数只有**预取长度**
//     （从本类的 `AppSettings` 取）；位置由 `jump` 定。
//     ⚠️ 换组时**先 emit `viewerChanged`，再释放旧组**：
//       · 会话来源（旧路径，序列归本组）⇒ `deleteLater()`（QML 绑定还握着旧对象时不许同步 delete）；
//       · **core 给的序列**（T2c-2b-1）⇒ 必须**同步**拆（`teardownViewer`：emit → `releaseNow`），
//         因为 core 紧接着就会销毁那条序列（换快照 / 释放槽位）。
//         ★ `~ViewerModel` **不 join 工作线程**（只换个记号就走）⇒ 同步拆也是立刻返回。
//
//   ## 会话级 vs 设置级
//     · 会话级（不写入磁盘）：zoomLocked / zoom / panX / panY / osdVisible / overlaysVisible /
//       windowFullscreen / browseGrid / inputFromKeyboard ⇒ 本类成员。
//     · 设置级：themeMode / scanHidden / recurseDepth / prefetchWindow / cacheMb /
//       scaler* ⇒ 读写本类持有的 `AppSettings`（`m_settings`），本类只发通知。
//       `prefetchWindow`（预取长度 = viewer 的构造参数）、`scanHidden` / `recurseDepth`
//       （等价于重扫）变化 ⇒ **重建 viewer**（会话态：位置带过去，"语义不变"）。
//       `slideshowIntervalMs` 仍是本类的设置项（设置页读写），但**没有运行态**了。

#include "appsettings.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <atomic>
#include <memory>

namespace fetch { class ViewerModel; }
class Sequence;
class LibraryModel;          // 图集库列表（QAbstractListModel，定义在全局命名空间）
class LibraryCore;           // ★ T2c-2b-1：图库/序列抽象 + 重置队列（设计 14 §9.3）
namespace library { class Store; struct Collection; }
class QTimer;

namespace app {

class BrowseModel;           // ★ T2d-1b：浏览界面的模型（QML 的 `ui.browse`）

class UiModel : public QObject
{
    Q_OBJECT

public:
    // ── 三屏（定案：**一个 enum 就够**，不做每屏状态快照）──────────────────
    //   QML 侧读 **`UiModel.Library` / `UiModel.Browse` / `UiModel.Viewer`**（不写 0/1/2）。
    //   ⚠️ 实测（见报告）：QML **读不到"实例上的枚举"** —— `ui.Library` 是 `undefined`
    //      （`ui.screen` 本身读得到，是 number）。QML 只认**已注册类型名**上的枚举
    //      ⇒ `main.cpp` 里 `qmlRegisterUncreatableType<UiModel>(...)`（只为枚举名）
    enum class Screen { Library = 0, Browse = 1, Viewer = 2 };
    Q_ENUM(Screen)

    // ── viewer（只读指针；属性一律 `ui.viewer.*` 直绑）──
    Q_PROPERTY(QObject *viewer READ viewer NOTIFY viewerChanged)
    // ── ★ T2d-1b：浏览界面（**从 viewer 里搬出来**；属性一律 `ui.browse.*` 直绑）──
    Q_PROPERTY(BrowseModel *browse READ browse CONSTANT)

    // ── 三屏状态 / 图集库列表 ──
    Q_PROPERTY(Screen screen READ screen NOTIFY screenChanged)
    Q_PROPERTY(LibraryModel *library READ library CONSTANT)

    // ── ★ 瞬时提示（Phase 3）：**不占位浮层** ──────────────────────────
    //   刻意**不是**持久状态：`notificationSeq` 每次 +1 ⇒ 同一条文案重复出现也能重新触发动画。
    //   失败的来源：扫描的失败（根不可用 / 库写失败 / 库打不开）与"压缩包打不开 / 不支持"
    //   （`Sequence::unsupportedReason()`）—— **一律瞬时提示 + 日志，不写入磁盘**。
    Q_PROPERTY(QString notification READ notification NOTIFY notificationChanged)
    Q_PROPERTY(int notificationSeq READ notificationSeq NOTIFY notificationChanged)

    // ── 适配 / 手势态（**归 ui**）──
    Q_PROPERTY(int   fitMode    READ fitMode    WRITE setFitMode    NOTIFY fitModeChanged)
    Q_PROPERTY(bool  zoomLocked READ zoomLocked WRITE setZoomLocked NOTIFY zoomLockedChanged)
    Q_PROPERTY(qreal zoom       READ zoom       WRITE setZoom       NOTIFY zoomChanged)
    Q_PROPERTY(qreal panX       READ panX       WRITE setPanX       NOTIFY panXChanged)
    Q_PROPERTY(qreal panY       READ panY       WRITE setPanY       NOTIFY panYChanged)

    // ── UI 形态（**会话级**，不写入磁盘）──
    Q_PROPERTY(bool overlaysVisible READ overlaysVisible WRITE setOverlaysVisible
                                    NOTIFY overlaysVisibleChanged)
    Q_PROPERTY(bool osdVisible READ osdVisible WRITE setOsdVisible NOTIFY osdVisibleChanged)
    Q_PROPERTY(bool windowFullscreen READ windowFullscreen WRITE setWindowFullscreen
                                    NOTIFY windowFullscreenChanged)
    Q_PROPERTY(bool browseGrid READ browseGrid WRITE setBrowseGrid NOTIFY browseGridChanged)

    // ── 设置（**值全在本类持有的 `AppSettings` 里**；QML 侧名字与旧 VM 一致）──
    Q_PROPERTY(int  themeMode READ themeMode WRITE setThemeMode NOTIFY themeModeChanged)
    Q_PROPERTY(bool systemDark READ systemDark NOTIFY systemDarkChanged)
    Q_PROPERTY(bool inputFromKeyboard READ inputFromKeyboard WRITE setInputFromKeyboard
                                      NOTIFY inputFromKeyboardChanged)
    Q_PROPERTY(bool scanHidden READ scanHidden WRITE setScanHidden NOTIFY scanHiddenChanged)
    Q_PROPERTY(int  recurseDepth READ recurseDepth WRITE setRecurseDepth NOTIFY recurseDepthChanged)
    Q_PROPERTY(int  prefetchWindow READ prefetchWindow WRITE setPrefetchWindow
                                    NOTIFY prefetchWindowChanged)
    Q_PROPERTY(int  cacheMb READ cacheMb WRITE setCacheMb NOTIFY cacheMbChanged)
    //   内层压缩包上限（MB）：超过上限的内层压缩包不展开（判定在解压前）
    Q_PROPERTY(int  innerArchiveMaxMb READ innerArchiveMaxMb WRITE setInnerArchiveMaxMb
                                       NOTIFY innerArchiveMaxMbChanged)
    //   图像（缩放）——**参数才是真相**，预设只是命名组合（`scaler::presets()` 是唯一事实源）
    Q_PROPERTY(QString scalerQuality READ scalerQuality WRITE setScalerQuality
                                      NOTIFY scalerQualityChanged)
    Q_PROPERTY(QString scalerMode READ scalerMode WRITE setScalerMode NOTIFY scalerModeChanged)
    Q_PROPERTY(QString scalerFilter READ scalerFilter WRITE setScalerFilter
                                      NOTIFY scalerFilterChanged)
    Q_PROPERTY(int scalerLobes READ scalerLobes WRITE setScalerLobes NOTIFY scalerLobesChanged)
    Q_PROPERTY(double scalerMitchellB READ scalerMitchellB WRITE setScalerMitchellB
                                       NOTIFY scalerMitchellBChanged)
    Q_PROPERTY(double scalerMitchellC READ scalerMitchellC WRITE setScalerMitchellC
                                       NOTIFY scalerMitchellCChanged)
    Q_PROPERTY(double scalerAntiring READ scalerAntiring WRITE setScalerAntiring
                                      NOTIFY scalerAntiringChanged)
    //   当前参数匹配到哪个预设（空串 = 自定义）—— **派生**，不存
    Q_PROPERTY(QString scalerPreset READ scalerPreset NOTIFY scalerParamsChanged)

    // ── 幻灯片间隔（`AppSettings` 支撑）────────────────────────────────────
    //   设置项 `intervalMs` 的 QML 面（设置页读写 `ui.slideshowIntervalMs`）。
    //   ⚠️ 值的家 = 本类的 `m_settings`（viewer 不再有间隔的运行态）。
    Q_PROPERTY(int slideshowIntervalMs READ slideshowIntervalMs WRITE setSlideshowIntervalMs
                                       NOTIFY slideshowIntervalMsChanged)

public:
    explicit UiModel(library::Store *store = nullptr, QObject *parent = nullptr);
    ~UiModel() override;

    UiModel(const UiModel &) = delete;
    UiModel &operator=(const UiModel &) = delete;

    QObject *viewer() const;            // 定义在 .cpp（ViewerModel 只前向声明 ⇒ 需要完整类型转换）
    // C++ 侧（调试入口 / 用例）拿具体类型；QML 只认上面那个 QObject*
    fetch::ViewerModel *viewerCore() const { return m_src.vm; }
    // C++ 侧（调试入口 / 用例）：浏览模型（QML 走 `ui.browse`）
    BrowseModel *browse() const { return m_browse; }
    // C++ 侧（用例 / 调试）：拿到持有的 `LibraryCore`（QML 不绑它 —— 界面只经本类）
    LibraryCore *core() const { return m_core.get(); }

    // ── 三屏状态 / 导航（语义照旧 `ViewModel`；见 .cpp 的说明）──
    Screen screen() const { return m_screen; }
    LibraryModel *library() const { return m_library; }
    Q_INVOKABLE void goLibrary();       // 清屏栈直接回图集库
    Q_INVOKABLE void back();            // 出栈：Viewer → Browse → Library
    //   ⚠️ T2d-1b：`openRow / enterRow / isContainerRow`（**按浏览行号解释**）已**搬到新家**
    //      `app::BrowseModel`（QML 的 `ui.browse`）—— 浏览语义只有一处实现（AGENTS.md）。
    //      换层 / 打开 / 选中的**接线**在 .cpp 的 `onBrowseLayerChanged` /
    //      `onBrowseRowSelected` / `onBrowseOpenRequested`（本类认识 viewer 与抢占屏）。

    // ── 图集库动作（库是真相，值全在 SQLite）──
    Q_INVOKABLE qint64 collectionIdAt(int row) const;
    Q_INVOKABLE bool   openCollection(qint64 collectionId);
    Q_INVOKABLE void   removeCollection(int row);
    //   文件对话框（**QFileDialog 只在这里**；QML 不写平台相关代码）
    Q_INVOKABLE void chooseAddCollection();   // 选目录 → addCollection(..., "folder") → 自动开扫
    Q_INVOKABLE void chooseOpenFolder();      // 直接打开 = 会话来源，不建库记录
    Q_INVOKABLE void chooseOpenArchive();
    Q_INVOKABLE void chooseOpenImage();
    //   注：旧的 `startScan` / `cancelScan`（`ScanController`）**已删**（T2c-2b-2）——
    //   QML 只调下面的 `requestReset` / `cancelReset`（T2c-2b-1 已改指）。

    // ── ★★ T2c-2b-1：重置 / 取消 —— **走 `LibraryCore`**（设计 14 §9.1/§9.3）──────
    //   「重置」= 起一条**活序列**（跑完 ⇒ 后台一个事务**原子替换**该集合的行；
    //     停止 / 失败 / 卡住 ⇒ **不落库**），**不再是**"扫描任务"。
    //   ★ T2c-2b-2：`cancelReset` 在**落库窗口内**是**静默 no-op**（返回 false，
    //     不提示、不报错）；界面此时把「取消扫描」禁用（`LibraryModel` 的
    //     `resetCommitting` role + `CollectionCard.qml`）。
    Q_INVOKABLE bool requestReset(qint64 collectionId);
    Q_INVOKABLE bool cancelReset(qint64 collectionId);

    // ★ 瞬时提示（浮层）：任何失败/不支持都走它（**不写入磁盘**）
    Q_INVOKABLE void notify(const QString &text);
    QString notification() const { return m_notification; }
    int     notificationSeq() const { return m_notificationSeq; }
    // ── 幻灯片间隔（读写；值的家在 `m_settings`）──
    int  slideshowIntervalMs() const;
    void setSlideshowIntervalMs(int ms);

    // ── 建来源（目录 / 压缩包 / 单图）──
    //   会话来源（**不写入磁盘**）：直接打开 = 文件系统来源 + 文件浏览屏（照旧 VM）
    void openPath(const QString &path);

    // ── 适配 / 手势态 ──
    int   fitMode() const;
    void  setFitMode(int m);
    bool  zoomLocked() const { return m_zoomLocked; }
    void  setZoomLocked(bool on);
    qreal zoom() const { return m_zoom; }
    void  setZoom(qreal v);
    qreal panX() const { return m_panX; }
    void  setPanX(qreal v);
    qreal panY() const { return m_panY; }
    void  setPanY(qreal v);

    // ── UI 形态 ──
    bool overlaysVisible() const { return m_overlaysVisible; }
    void setOverlaysVisible(bool on);
    bool osdVisible() const { return m_osdVisible; }
    void setOsdVisible(bool on);
    bool windowFullscreen() const { return m_windowFullscreen; }
    void setWindowFullscreen(bool on);
    bool browseGrid() const { return m_browseGrid; }
    void setBrowseGrid(bool on);
    Q_INVOKABLE void toggleOsd();

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
    int  cacheMb() const;
    void setCacheMb(int v);
    //   ★ v7：内层压缩包上限（MB）—— 只影响**下一次展开**（正在看的那份序列不重建）
    int  innerArchiveMaxMb() const;
    void setInnerArchiveMaxMb(int v);
    QString scalerQuality() const;
    void setScalerQuality(const QString &v);
    QString scalerMode() const;
    void setScalerMode(const QString &v);
    QString scalerFilter() const;
    void setScalerFilter(const QString &v);
    int     scalerLobes() const;
    void    setScalerLobes(int v);
    double  scalerMitchellB() const;
    void    setScalerMitchellB(double v);
    double  scalerMitchellC() const;
    void    setScalerMitchellC(double v);
    double  scalerAntiring() const;
    void    setScalerAntiring(double v);
    QString scalerPreset() const;                        // 派生（`scaler::presetFor`）
    Q_INVOKABLE QVariantList scalerPresetList() const;   // UI 的下拉列表
    Q_INVOKABLE void applyScalerPreset(const QString &key);

Q_SIGNALS:
    void viewerChanged();
    // 三屏 / 图集库
    void screenChanged();
    void slideshowIntervalMsChanged();
    void fitModeChanged();
    void zoomLockedChanged();
    void zoomChanged();
    void panXChanged();
    void panYChanged();
    void overlaysVisibleChanged();
    void osdVisibleChanged();
    void windowFullscreenChanged();
    void browseGridChanged();
    void themeModeChanged();
    void systemDarkChanged();
    void inputFromKeyboardChanged();
    void scanHiddenChanged();
    void recurseDepthChanged();
    void prefetchWindowChanged();
    void cacheMbChanged();
    void innerArchiveMaxMbChanged();
    void scalerQualityChanged();
    void scalerModeChanged();
    void scalerFilterChanged();
    void scalerLobesChanged();
    void scalerMitchellBChanged();
    void scalerMitchellCChanged();
    void scalerAntiringChanged();
    void scalerParamsChanged();        // 任一参数变化都发（预设派生属性跟它）
    void notificationChanged();        // ★ 瞬时提示（浮层）

private:
    // 一组来源（Source group）——换来源 ⇒ 整组换掉
    //   ★ 本组只剩 `{ 序列, viewer }`（加上"序列归谁"）。
    struct Source {
        Sequence           *seq = nullptr;
        fetch::ViewerModel *vm  = nullptr;
        // 序列归谁 —— **core 给的序列不许删**（生命周期 = core 的槽位，设计 14 §9.3）；
        //   会话来源仍是本组自己的序列（`ownsSeq == true`）。
        bool                ownsSeq = true;
    };

    void buildViewer();                 // 用 m_path 新建一组；emit viewerChanged；旧组延迟释放
    // ⚠️ T2c-2b-1：旧的"图集来源：`LibraryItemSource` → 投影 → `buildFromRows`"已删 ——
    //   图集查看改走 core 的快照（`buildCollectionView`），不留两条并存的建序列路径。
    void rebuildViewer();               // 设置类变化 ⇒ 换一组（带上会话态：位置）
    void installViewer(Source s);       // 接线 + 换绑 + 旧组延迟释放
    void releaseLater(const Source &s); // 旧组：随旧 ViewerModel 的析构释放其余成员
    static void releaseNow(Source &s);
    // ── ★★ 序列 / 浏览（**全部只在 GUI 线程**）─────────────────────────────
    //   本类只负责"**给序列 + 序列变了通知**"：位置 / 预取 / 上屏都归 `fetch::ViewerModel`；
    //   浏览那一层由 `app::BrowseModel` 自己从序列取。
    void pumpSession();                 // 会话序列：drain 到空 + 通知（发布钩子的消费者）
    void onSequenceChanged();           // 序列长了 / 收尾了 ⇒ 通知 viewer + 浏览模型重算
    void onBrowseLayerChanged(int reason);   // 换层（0 = 上/面包屑；1 = 进入容器）
    void onBrowseRowSelected(int itemIndex); // 选中一条图 ⇒ 让 viewer 跳过去
    void onBrowseOpenRequested(int itemIndex);   // 打开那条图 ⇒ 切查看屏
    void resetView();                   // 换画面复位手势态（zoomLocked 时保留）
    void goTo(Screen s, bool push);     // 屏切换（push = 压栈）
    void flushResume();                 // 阅读位置写入磁盘（**存 label 链 + 页号，不存下标**）

    // ── ★★ T2c-2b-1：core 接线（设计 14 §9.1 / §9.3 / §9.4）──────────────────
    void onCorePumped(bool sessionChanged, bool viewChanged, bool resetChanged);
    // 同步拆掉当前 viewer 组（**core 接下来要销毁序列之前必须调**；`~ViewerModel` 不 join，
    //   解码线程只拿一份条目信息的副本 ⇒ 拆完立刻返回）
    void teardownViewer();
    // 用 **core 给的序列**（非拥有）建一组 viewer；序列生命周期归 core
    fetch::ViewerModel *installCoreViewer(Sequence *seq);
    // 建"图集查看"这一组：core 快照 → 收尾（pump 一次）→ 落到 `chain/page`
    bool buildCollectionView(qint64 collectionId, const QStringList &chain, int page);
    // 回到库屏 ⇒ 释放显示槽（图集查看 = 快照；会话来源 = 停 + 销毁），设计 14 §9.3
    void releaseDisplaySlot();
    library::Collection collectionById(qint64 id) const;   // 读一行（resume）

    library::Store *m_store = nullptr;
    LibraryModel   *m_library = nullptr;   // 图集库列表（永远来自 SQLite）
    // ★★ 设计 14 §9.3：图库/序列抽象（core 不认界面、不读设置）；`m_store` 由本类建并交给它
    std::unique_ptr<LibraryCore> m_core;

    // ★★ 设计 14 §9.3：**运行期设置的唯一实例**（没有单例、没有第二个副本）。
    //   构造时 `load()`（= 旧 `main`/`iconpicker`/自检里那次单例的 `load()`）；
    //   各成员函数读写它；需要值的组件一律由本类**调用时传参**。
    AppSettings m_settings;

    // 条目来源：只有**库来源**才有（阅读位置的 key ↔ 下标映射；会话来源不写入磁盘）
    //   ⚠️ T2c-2b-1：图集查看改走 core 的**静态只读快照** ⇒ 位置派生直接用
    //      `m_src.seq` 的 `chainAt/pageAt/indexOfChain`（不再经 `LibraryItemSource` 投影一份）。
    qint64 m_collectionId = -1;            // 当前所属集合（直接打开 = -1）

    Source  m_src;
    QString m_path;                     // 最近一次打开的**会话**来源（重建 viewer 要用）

    // ── ★★ T2d-1b / T2d-1c-①：浏览模型 / 唤醒合并（全部只在 GUI 线程）──────────────
    BrowseModel *m_browse = nullptr;                                   // QML 的 `ui.browse`
    std::atomic<bool> m_wakeQueued{false};   // 发布钩子 → **合并成一次** queued 投递

    // 三屏（只存语义状态；`Viewer` 的沉浸态是它的派生）
    QVector<Screen> m_screenStack;
    Screen m_screen = Screen::Library;

    // 阅读位置写入磁盘（图集来源才落）：游标变化 400ms 去抖 + 5s 定期兜底（AGENTS.md）
    QString m_notification;                // 最近的瞬时提示文案（空 = 没有）
    int     m_notificationSeq = 0;         // 每次 +1 ⇒ 同文案也能重新触发动画
    QTimer *m_sessionProbe = nullptr;      // 会话来源：查一次"不支持原因"（扫完就停）
    QString m_sessionNotice;               // ★ v7：会话来源最近的瞬时提示（去重）

    QTimer *m_resumeDebounce = nullptr;
    QTimer *m_resumeFallback = nullptr;
    QString m_lastResumeKey;               // 已写入磁盘的位置（label 链的 JSON 数组）
    int     m_lastResumePage = 0;          // ★ v5：已写入磁盘的页号（多页文件停在第 k 页）

    // 会话状态（不写入磁盘）
    bool  m_zoomLocked = false;
    qreal m_zoom = 1.0, m_panX = 0.0, m_panY = 0.0;
    bool  m_overlaysVisible = false;
    bool  m_osdVisible = false;
    bool  m_windowFullscreen = false;
    bool  m_browseGrid = true;
    bool  m_inputFromKeyboard = false;  // 焦点环只在键盘导航时显示（UI 纪律 07 §五）
};

} // namespace app
