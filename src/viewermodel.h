#pragma once

// ══ viewer：阅读界面的模型（位置 / 预取 / 上屏 / 节拍）══════════════════════
//
//   ★ 本类**整个住在主线程**：来源 / 位置 / 条目表 / 上屏内容只有主线程碰；
//     解码在工作线程上做，解完只 post 回主线程。**没有任何两侧都能写的东西。**
//
//   ## 术语
//     · **条目**：序列里的一条（`Sequence::Item`）。
//     · **屏**：序列按**版面**分好的**位置集合**（单幅 1 条 / 双幅 2 条；宽图、末尾剩一张
//       ⇒ 1 条）。**位置是序列给的**（`groupAt`），本类不算分组、不做 ±屏 的算术。
//     · **上屏**（display）：把本屏各位置的图放进 `imageA` / `imageB`（没结论的位置留空）。
//     · **宣布**（announce）：这一屏**第一次**有结论 ⇒ 记下"已宣布的那一屏"并处理节拍。
//       ★ 同一屏再被重判（补图 / 预取完成）**不许**第二次宣布 —— 否则节拍会被反复重启。
//     · **loading（界面浮层）**：还没扫完 **且**（还没有条目 **或** 位置已处在已知末尾）；
//       还没有序列也算 —— 也就是「序列还没就绪 / 扫描中到达边界」。
//       ★ **正常等图片缓存（当前这一屏还没就位）不算**（用户 2026-10-17 口径）。
//     · **位置**：现在要显示的是哪一条（条目下标）；**屏 = 位置所在的那一屏**。
//     · **缺**：这一条**没有字节可读**（元数据已标 / 文件没了 / 容器打不开 / 成员不在）。
//       ★ 缺**不占**预取额度、**不重试**（结论表里记一笔就算完）；整屏都缺 ⇒ 跳屏。
//     · **占位图**：有字节、但解码器给不出图（截断 / 坏编码）⇒ 按序列里的**原图尺寸**
//       画一张中性占位图。★ 占位图**是结论**（等于"这一条有图"）：不重试、**不跳屏**、占额度。
//     · **等密码**：读失败的原因是加密容器 ⇒ 停在原地问密码（**不跳屏**）。
//       ⇒ "坏"只剩两种：**缺**（跳屏）与 **等密码**（等用户）。
//     · **pinned**：本屏那 1~2 条。**独立槽位**：不占预取额度、必取、永不淘汰。
//     · **预取**（lookahead）：本屏之外、按份额提前备好的条目。
//
//   ## 两条线（★ 2026-11-05 重构：幻灯片 0ms 卡死的根治）
//     · **后台线** `reconcile()`：算"现在该有哪些条目"（本屏 + 前后预取），派缺失的、
//       丢不要的；完成一条只发 `notifyFrontend()`。**不碰屏、不碰节拍。**
//     · **前端线** `evaluateScreen()`：只看当前屏成员的状态，决定
//       「显示 / 宣布 / 跳屏 / 问密码」。节拍**只在宣布时挂一次**（`armBeat()` 幂等）。
//     · 那个 bug 的机制：以前 `judge()` 每被调一次就可能 `screen()` → `armBeat(0)` ——
//       0ms 单发闹钟被反复重启，`onBeat` 永远轮不到 ⇒ 画面冻结。
//
//   ## 预取份额（`L` 按**图**计，双幅**不**×2）
//     `insurance(L) = (L >= 4) ? max(1, L/4) : 0`（反方向保险）；`ahead(L) = L − insurance`。
//     `L = 0` 合法（只备本屏，没有预取）。方向 `m_dir`：换屏取那一次的方向；
//     **跳转回前向**；**换版面不改方向**。份额**不随步数漂移**（没有 `f/b` 那套 ±1）：
//     要哪些条目由 `reconcile()` 每拍重算，`L` 决定数量、`m_dir` 决定谁在前。
//
//   ## 版面（单幅 / 双幅）
//     · **换版面 = 换个开关**：保留**当前位置**，屏由序列重算（`anchorOf`）⇒ 不重新分组。
//     · `stepPage(±1)` = **换一屏**：前进量交给序列（`advance`），本类收下游标。
//     · 按键那条判断按**整屏**算：本屏还有位置没结论（既没备好、也不是缺）⇒ 这次按键丢掉。
//       **缺 / 等密码算"有结论"** ⇒ 那时按键照常放行（否则"坏到末尾"会卡在末尾出不来）。
//
//   ## 退出查看 / 换来源
//     · **不 join 工作线程**（join 会把退出拖住几百毫秒）：换个记号就走；
//       在飞的回来时记号不符就被丢掉，既不写错条目表也不影响下一次进入。
//     · 析构 = **记号 + 标记失效**（见 .cpp 的 `Poster`）。

#include <QHash>
#include <QSet>
#include <atomic>
#include <QImage>
#include <QObject>

template <typename T> class QFutureWatcher;
#include <QImage>
#include <QVector>

#include <functional>
#include <memory>
#include <thread>

class QTimer;

#include "appsettings.h"   // ★ app::Layout：阅读版面 = 一份枚举（本类只"持有并投影"，不定义）
#include "sequence.h"
#include "previewprovider.h"

namespace fetch {

class ViewerModel : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QImage imageA READ imageA NOTIFY currentChanged)
    Q_PROPERTY(QImage imageB READ imageB NOTIFY currentChanged)            // 单幅 / 空位：空图
    Q_PROPERTY(int keyCount READ keyCount NOTIFY currentChanged)           // 本屏位置数（1 或 2），无屏 = 0
    //   ★ QML 边界的那一份**投影**（界面只要"是不是双幅"这一个布尔）：
    //     概念本身 = `app::Layout`（`appsettings.h`）＝ 库里 `reading_layout` 存的那份。
    Q_PROPERTY(bool doublePage READ doublePage WRITE setDoublePage NOTIFY doublePageChanged)  // 写 = 换版面
    //   循环（到头怎么办）＝ 会话级、不写盘：0 不循环 / 1 全部循环 / 2 文件夹循环
    //   （**值就是界面的菜单下标**，一份表示）
    Q_PROPERTY(int loopMode READ loopMode WRITE setLoopMode NOTIFY loopModeChanged)
    //   上一 / 下一文件夹的**目标名字**（空 ⇒ 界面把按钮灰掉）
    Q_PROPERTY(QString prevFolderName READ prevFolderName NOTIFY currentChanged)
    Q_PROPERTY(QString nextFolderName READ nextFolderName NOTIFY currentChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    //   派生值：本屏条目里有"等密码"的结论（且用户没取消过）就为真 —— 界面据此显示
    //   "无密码，无法查看"的占位说明；拿到密码后由 `retryNeedingPassword()` 自然变假。
    Q_PROPERTY(bool screenNeedsPassword READ screenNeedsPassword NOTIFY currentChanged)
    //   当前这一屏的**文件名**（双幅两页时用 " · " 连起来；多页文件是 "file [k/N]"）——
    //   顶栏显示用，派生值，不改任何状态
    Q_PROPERTY(QString currentName READ currentName NOTIFY currentChanged)
    //   当前这一屏第一幅**相对来源根的路径**（顶栏用；与 OSD 的"路径"行同一份实现）
    Q_PROPERTY(QString relativePath READ relativePath NOTIFY currentChanged)
    //   ★ 幻灯片（13 §九·补·二）：`playing` **只是一个状态**（没有幻灯片对象）
    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(int cursor READ cursor WRITE setCursor NOTIFY currentChanged)  // 写 = 跳转；= 本屏起点

    // ── 阅读视图态（**viewer 领域**，2026-10-20 定案）────────────────────────
    //   随 viewer 生灭；**初值由上层从全局设置下发**（`UiModel::applyViewerDefaults`）。
    //   UI（底栏）上的调整只改这里，**不回写**全局默认；只有设置对话框才落盘。
    //   单位：zoom = 设备像素/源像素；pan = 设备像素；视口也是设备像素（QML 报逻辑尺寸 + dpr）。
    Q_PROPERTY(int   fitMode READ fitMode WRITE setFitMode NOTIFY fitModeChanged)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY zoomChanged)
    Q_PROPERTY(qreal panX READ panX WRITE setPanX NOTIFY panChanged)
    Q_PROPERTY(qreal panY READ panY WRITE setPanY NOTIFY panChanged)
    Q_PROPERTY(bool  zoomLocked READ zoomLocked WRITE setZoomLocked NOTIFY zoomLockedChanged)
    Q_PROPERTY(bool  osdVisible READ osdVisible WRITE setOsdVisible NOTIFY osdVisibleChanged)
    Q_PROPERTY(QString scaleAlgorithm READ scaleAlgorithm WRITE setScaleAlgorithm
                                    NOTIFY scaleAlgorithmChanged)
    Q_PROPERTY(int   intervalMs READ intervalMs WRITE setIntervalMs NOTIFY intervalChanged)
    Q_PROPERTY(int count READ itemCount NOTIFY currentChanged)             // 条目总数
    Q_PROPERTY(int totalPages READ totalPages NOTIFY currentChanged)       // = count（口径不变：按条目）
    Q_PROPERTY(int currentPage READ currentPage NOTIFY currentChanged)     // = cursor（本屏起点）

public:
    explicit ViewerModel(int prefetchLength, QObject *parent = nullptr);
    //   ★ 预取长度是**运行时参数**（设置里改它不该重建 reader，更不该重扫来源）：
    //     改完立刻按新窗口派活。变短不会回收已派发的，等它们回来自然空出。
    void setPrefetchWindow(int prefetchLength);
    ~ViewerModel() override;

    ViewerModel(const ViewerModel &) = delete;
    ViewerModel &operator=(const ViewerModel &) = delete;

    void setSequence(const Sequence *seq);   // 来源（非拥有）
    void sequenceChanged();                  // 序列长进了
    Q_INVOKABLE void stepPage(int delta);    // ±1 屏（前进量由序列按版面算）
    //   ★ 初始位置（在 `setSequence` 之前给）：恢复进度 / 从第 N 条开始，
    //     **初始化时就在那儿**，不需要装载完再 `jump()`（用户 2026-11-04）。
    void setStartPosition(int imageIndex);
    Q_INVOKABLE void jump(int imageIndex);   // 跳到某一条（时间轴拖 / 浏览里选中）
    Q_INVOKABLE void stepFolder(int delta);  // ★ 给 QML 的同一件事（`delta` = ±1）
    Q_INVOKABLE void togglePlaying();        // ★ 幻灯片：播放 ⇄ 暂停（`Space` / ⏯）

    // ── 幻灯片（规则见设计 13 §九·补·二）────────────────────────────────
    //   ★ 没有幻灯片对象：只有 `playing` 一个状态 + 一个**单发闹钟**（只报时）+
    //     一个**推进等待位**（一个 bool）+ 一个**折叠器**（等缓存期间的进度事件，一个槽）。
    //   ★ 节奏一律由**单调时钟**算：`到期 = 请求时刻 + 间隔`（间隔是下限，准备耗时吃掉间隔）。
    bool playing() const { return m_playing; }
    void setPlaying(bool on);
    int  intervalMs() const { return m_intervalMs; }
    void setIntervalMs(int ms);                              // 由上层从设置下发
    using NowFn = std::function<qint64()>;                   // 时间源（自检注入虚拟时钟）
    void setNowFn(NowFn fn) { m_now = std::move(fn); }

    // ── 视图态（几何只有一处：zoom/pan + fit 命令，见 gpuscale.h）──────────
    int   fitMode() const { return m_fitMode; }
    void  setFitMode(int m);              // fit 是**命令**：算一次 zoom（pan=0）
    qreal zoom() const { return m_zoom; }
    void  setZoom(qreal v);
    qreal panX() const { return m_panX; }
    void  setPanX(qreal v);
    qreal panY() const { return m_panY; }
    void  setPanY(qreal v);
    bool  zoomLocked() const { return m_zoomLocked; }
    void  setZoomLocked(bool on);
    bool  osdVisible() const { return m_osdVisible; }
    void  setOsdVisible(bool on);
    QString scaleAlgorithm() const { return m_scaleAlgorithm; }
    void  setScaleAlgorithm(const QString &key);   // 键串（nearest|smooth|bicubic|lanczos2|lanczos3）
    Q_INVOKABLE void setViewport(qreal logicalW, qreal logicalH, qreal dpr);
    Q_INVOKABLE void applyFit();
    Q_INVOKABLE void panBy(qreal dx, qreal dy);
    //   ★ 加密压缩包（设计 22 §5.3）：用户提交了密码 ⇒ 把**所有"等密码"的结论**清掉再重取。
    //     唯一调用点 = "提交密码"，**不做任何自动重试**。
    Q_INVOKABLE void retryNeedingPassword();
    //   ★ 用户**取消**了密码输入（`Sequence::declinePassword`）：那一格从"等密码"降级成"缺"，
    //     前端要**重判一次**才能跳屏 / 挂节拍（否则幻灯片会停在一页永远等不到的图上）。
    //     与上面的区别：**不重取、不清别的结论**。
    void passwordStateChanged();
    //   ★ 时间轴悬停预览（用户 2026-11-03）：以 `center` 为中心取邻近条目，按固定高度等比铺开，
    //     返回 [{ index, w, h, url }]（url 是 `image://preview/…`）。GUI 线程调用；位置表交给 provider。
    //     `heightPx` = 预览条高度；`maxWidthPx` = 窗口宽度（填满即停，最多 12 张）。
    //   返回 { items: [...], focusX, focusW } —— **几何和条目一次算完**，避免 QML 侧
    //   分几次赋值造成的中间态（用户 2026-11-03：框来回乱跳）
    //   `gapPx` = 预览条**图片之间的间距**（由 QML 的 `Theme.spaceSm` 传进来，
    //   不能在 C++ 里另写一个数 —— 否则布局和定位框的计算会差一点，用户 2026-11-03 指出）
    Q_INVOKABLE QVariantMap previewStrip(int center, int heightPx, int maxWidthPx, int gapPx);
    //   预览条里"第 index 条"的**显示宽度**（数据来自序列本身，不看预览条里有没有它）
    Q_INVOKABLE int previewItemWidth(int index, int heightPx) const;

    //   ★ 预览缩略图的批量解码（用户 2026-11-04）：
    //     · `previewStrip()` 为当前这一批起并行解码，**解完只更新缓存**，不去"直接给界面赋值"；
    //     · `previewCacheVersion` 只在缓存变化时 +1（界面据此**按当前位置查**是否就绪）；
    //     · `previewBatchReady(batch)` = 这一批要的图现在**是否都在缓存里**（查询，不是状态推进）。
    Q_PROPERTY(int previewCacheVersion READ previewCacheVersion NOTIFY previewCacheChanged)
    int previewCacheVersion() const { return m_previewCacheVersion; }
    Q_INVOKABLE bool previewBatchReady(int batch) const;
    //   退出前用：正在解码的条目数（有界排空用）
    int inFlightCount() const { return m_inFlight; }
    //   退出前用：停掉预览批次（不停在飞的那几个，只不再派新活）
    void shutdownPreviewWork();
    //   预览用的共享表（由 `UiModel` 持有、provider 读取）
    void setPreviewStore(const std::shared_ptr<PreviewStore> &store) { m_previewStore = store; }
    //   把 pan 夹回当前 zoom/视口允许的范围（视口/dpr 变化后必须调，否则 VM 与渲染不一致）
    void clampPan();
    Q_INVOKABLE void zoomAtPoint(qreal factor, qreal anchorX, qreal anchorY);
    //   绝对倍率（1 = 100%，即 1 图像像素 = 1 设备像素）：居中显示，并让 fit 让位
    Q_INVOKABLE void zoomToRatio(qreal ratio);
    //   Chrome 式**档位**缩放：`dir > 0` 放大一档、`< 0` 缩小一档（档位见 .cpp）
    Q_INVOKABLE void zoomStep(int dir);

    // ── OSD 信息行（**按需读取**，用户 2026-10-21 定案）─────────────────────
    //   ★ 不是属性、没有 NOTIFY、不进任何成员：EXIF / 尺寸 / 色彩空间是**信息性数据**
    //     （AGENTS.md「信息性数据用 getter，不进 VM 状态」），界面要看时才读一次。
    //   返回"列"：单幅 1 列、双幅 2 列；每列 = 该幅的行（`{k, v, section}`；
    //   EXIF 行**有才出现** —— 不同图片标签集不同，绝不固化）。
    Q_INVOKABLE QVariantList osdRows();

    QImage imageA() const { return m_imageA; }
    QImage imageB() const { return m_imageB; }      // 单幅 / 坏的空位：空图
    int  keyCount() const { return m_keyCount; }
    bool doublePage() const { return m_layout == app::Layout::Double; }
    void setDoublePage(bool on);                    // 换版面（保留当前位置，屏重算）
    int  loopMode() const { return int(m_loop); }
    void setLoopMode(int mode);                     // 写 = 换循环模式（不写盘；进沉浸视图回默认）
    QString currentName() const;
    QString relativePath() const { return m_screenPath; }
    QString prevFolderName() const;
    QString nextFolderName() const;
    bool loading() const { return m_loading; }
    bool screenNeedsPassword() const;               // 本屏有空着等密码的图（派生）
    int  cursor() const { return m_cursor; }        // 上屏那一屏的起点（写 = 跳转）
    void setCursor(int index) { jump(index); }
    int  itemCount() const { return m_seq ? m_seq->count() : 0; }
    int  totalPages() const { return itemCount(); } // 口径不变：按条目
    int  currentPage() const { return m_cursor; }

Q_SIGNALS:
    void currentChanged();
    void imageAChanged();
    void imageBChanged();
    void doublePageChanged();
    void fitModeChanged();
    void zoomChanged();
    //   ★ 解码失败且原因是密码：`item` = 条目下标，`level` = 层号（0 = 最外层容器）
    void passwordRequired(int item, int level);
    void previewCacheChanged();
    void panChanged();
    void zoomLockedChanged();
    void osdVisibleChanged();
    void scaleAlgorithmChanged();
    void intervalChanged();
    void loopModeChanged();
    void loadingChanged();
    void playingChanged();
    //   瞬时提示（幻灯片到头）。上层（`UiModel`）转发到 `UiModel::notify`。
    void notice(const QString &text);

private:
    //   一个条目在**后台线**里的状态：在飞 / 备好（备好的图可能是占位图）。
    //   ★ "缺"与"等密码"**不在这张表里** —— 它们是结论（各一张小表），不占额度、不再派活。
    struct Entry {
        enum class State { InFlight, Ready };
        State  state  = State::InFlight;
        bool   pinned = false;              // 属于当前屏 ⇒ 独立槽位（不占 `L`、不淘汰）
        QImage image;                       // 只有"备好"才有
    };

    //   ★ **post 交接点没有锁**：工作线程**完全不碰 `ViewerModel`**，也**不读**下面两个字段
    //     —— 它只解码 + 把结果 post 给一个**长期存在的主线程对象**（见 .cpp 的 `preload`）。
    //     `valid` / `target` 的读（那条 lambda）与析构的写**同在主线程** ⇒ 不需要锁、不需要 atomic。
    //   ★ 每个实例有**自己的一份**：`ViewerModel` 是短的（随查看建了又毁）⇒ 销毁置空之后，
    //     还在路上的结果**全部丢弃**（正常路径，不是异常情况）。
    //   `Poster` 由 `shared_ptr` 持有 ⇒ 工作线程在结果排队期间用它也不会悬空。
    struct Poster {
        bool          valid  = true;
        ViewerModel  *target = nullptr;   // 析构置空 ⇒ 在飞结果不再碰本对象
    };

    // ── 工作线程 ────────────────────────────────────────────────────────
    //   只做两件：加载并解码（生产实现 = `imageread::load`）；解完把结果 post 回主线程。
    //   ★ **不做取消**；解码失败分三种（缺 / 等密码 / 占位图），见 `.cpp` 的 `preload`。
    void preload(const Sequence::Item &imageMeta, int item);

    // ── 结果回主线程 ────────────────────────────────────────────────────
    //   先比记号，再比"条目表里还要不要这一条"；任一条不符 ⇒ 直接丢（白干，不算数）。
    //   ★ 用**条目下标**交接（不是槽位号）：条目表是 `item → Entry`，中间没有可复用的槽。
    void onLoaded(quint64 epoch, int item, const QImage &image, int passwordLevel,
                  bool passwordUsed);

    // ══ 后台线：预取（**只**生产"某条目有结论"这一事实）══════════════════════
    //   每拍重算 desired（本屏 pinned → 行进方向 ahead 张 → 反方向 insurance 张）：
    //   派缺失的、丢不要的。**幂等**：同一状态下重算不产生任何副作用。
    void reconcile();
    //   完成一条后的通知（投递到 GUI 线程、**合并去抖**）：前端收到后自己检查当前屏。
    void notifyFrontend();

    // ══ 前端线：屏 / 节拍 ══════════════════════════════════════════════════
    //   `stepDir` = 这次是"换一屏"（±1）；0 = 跳转 / 换版面（方向回前向）。
    void act(int target, int stepDir);
    //   ★ **唯一**决定「显示 / 宣布 / 跳屏 / 问密码」的地方（以前叫 `judge()`）。
    void evaluateScreen();
    //   写 `imageA` / `imageB` / `cursor` / 文件名。★ **一屏只调一次**（全屏稳定后、宣布前）：
    //   双幅绝不"先出一格"（一屏两次渲染；两次提交挤同一帧，**后一次＝右半**会被刷新率吃掉）。
    void updateDisplay(int start, const QVector<int> &items);
    //   宣布这一屏（记"已宣布的屏" + `onScreenUp()`）；同一屏只宣布一次。
    void announce(int start, const QVector<int> &items);
    bool isAnnounced(int start, const QVector<int> &items) const;
    void clearAnnounced();
    //   本屏每个位置的**前端口径**状态：有图 / 缺 / 还没结论。
    //   ★ 区间外（文件夹循环跨边界那一格）算"缺"：不显示，也不等它。
    enum class Cell { Ready, Missing, Pending };
    Cell cellOf(int item) const;
    //   本屏每个位置都有结论（有图 / 缺 / 等密码）= **不在等缓存**（节拍能不能挂看它）。
    bool screenSettled() const;
    //   浮层的**唯一更新入口**：按当前状态当场算「该不该显示」（见类头 loading 那条）。
    void updateLoading();

    // ── 条目表（后台线写、前端只读）────────────────────────────────────────
    bool entryReady(int item) const;                 // 备好（真图或占位图）
    QImage imageOf(int item) const;                  // 没结论 / 在飞 / 区间外 ⇒ 空图
    bool isMissing(int item) const { return m_missing.contains(item); }
    bool waitsPassword(int item) const { return m_needsPassword.contains(item); }
    void markMissing(int item);                      // 缺（**不占额度、不重试**）
    void markNeedsPassword(int item, int level);     // 等密码（拿到密码后由上层清）

    // ── 版面 / 屏 ────────────────────────────────────────────────────────
    //   本屏 = 单幅的"这一条" / 双幅的"这一对"（**屏是视图的说法**，由版面选）；
    //   双幅那一对的位置由序列的**配对表**给（`pairStart` / `pairAt`），本类不自己算。
    int  screenStart(int item) const;
    QVector<int> screenItems(int start) const;
    // ── 循环（到头怎么办）────────────────────────────────────────────────
    //   三个值：**不循环**（到头就是到头）/ **全部循环**（末尾回开头）/ **文件夹循环**
    //   （只在**当前这一条所在容器**的区间里环绕）。
    //   ★ 循环**归本类**（`Sequence` 只描述序列）；判定**只看当前 index**（不按屏讨论）。
    enum class Loop { None = 0, All = 1, Folder = 2 };
    QPair<int, int> loopScope() const;          // 当前 index 所在容器的区间（O(1)，序列给）
    bool inLoopScope(int item) const;           // 这一条在不在本次循环的区间里
    int  lastScreenStart() const;               // 最后一屏的起点
    int  wrapTarget(int dir) const;             // 文件夹循环的回绕目标（区间两端所在那屏）
    // ★★ **上一个 / 下一个文件夹**（用户 2026-10-19 定案；**规则在本类**，序列只给事实表）：
    //   · 候选 = 序列的容器区间表（根不在里面）；**包含当前 index 的容器距离 ≤ 0 ⇒ 排除**
    //     （你正躺在它里面 —— 所以"在深层"与"在上层"候选集合不同，这是**看当前状态**的判定）；
    //   · 下一个 = `first_index > 当前 index` 里**最近**的（first 最小的）；
    //     上一个 = `last_index < 当前 index` 里**最近**的（last 最大的）；
    //   · **距离并列**（同一个 first / 同一个 last ⇒ 只可能是嵌套链）⇒ 取**最上层**那个
    //     （`level` 最小的）—— 名字与落点都取它；
    //   · 落点 = 那个容器的**第一条图**（双幅由配对表锚定到含它的那一屏）；
    //   · 名字 = 那个容器**自己**的 `label`（单段，不取落点那条图的 `folderLabel`）。
    //   返回**容器节点下标**；-1 = 到头（不循环 / 区间外）。
    //   ★ **文件夹内循环（`Loop::Folder`）⇒ 两个按钮禁用置灰、不许切文件夹**
    //     （用户 2026-10-19；以前到头会拿当前文件夹名顶上、还会跳到区间外）。
    int  adjacentFolderNode(int dir) const;     // dir > 0 = 下一个，< 0 = 上一个（二分）
    int  edgeFolderNode(int dir) const;         // 全部循环的回绕：第一个 / 最后一个容器
    QString folderNameOfNode(int node) const;   // 容器自己的名字（单段）
    //   换一屏：**单幅** = 相邻一条；**双幅** = 问序列的配对表（+本对张数）。到头 ⇒ -1。
    //   ⇒ "±本对张数"的算术在序列的配对表里，本类只做"这次用一条还是用一对"的选择。
    int  nextScreen(int start, int dir) const;
    //   本屏有没有"要密码"的条目（有就**不跳屏**：停在原地等用户输密码，见设计 22 §5.3）
    bool screenWaitsPassword(const QVector<int> &items) const;
    //   这一条的容器**本次会话已经被用户取消过密码** ⇒ 当"缺"处理（否则幻灯片会停在这儿不动）。
    bool passwordDeclined(int item) const;
    //   ★ 弹框**由"当前这一屏"触发**，不由任意一条解码失败触发：预取到的邻居缺密码不该打断
    //     用户（否则打开一份漫画会连着弹好几个卷）。见设计 22 §5.3。
    void maybeAskPassword(const QVector<int> &items);

    //   ★★ 解码并发上限（2026-11-05 用户定）：**预取数 + 本屏图数**，再夹到"最大核心数"。
    //     理由：预取多少张就该有多少条解码在跑（本屏那 1~2 张是必解、另算）；
    //     用户把预取调高时**最多也只给到核心数**（不越配，避免徒增功耗与互相抢占）。
    //       · 单幅：L + 1   · 双幅：L + 2   · 上限 = min(核数, …)
    //   ★ 慢盘实测（150 页、1.45 MB/页、NOBUF）：1 线程 18 页/s、4 线程 51、8 线程 121
    //     ⇒ 双幅 60 fps 要约 8 并发，所以这个上限不能写死小值（以前写死 4）。
    int inFlightLimit() const;

    int m_length;                            // 预取长度 L（按**图**计；0 = 只备本屏）
    const Sequence *m_seq = nullptr;         // 来源（非拥有；只有主线程读）
    std::shared_ptr<Poster> m_poster;        // 工作线程 post 回来的落点（见上）

    // ── 后台线状态（全在主线程）──────────────────────────────────────────
    //   条目表：在飞 / 备好。**容量只算 lookahead**（`!pinned` 的条数 ≤ `L`）；
    //   pinned 那 1~2 条独立成组，不占 `L`、不淘汰。换来源 / 换到不要的位置时直接删。
    QHash<int, Entry> m_entries;
    //   "缺"的结论：**不占**额度、**不重试**（这就是"缺不重派"的依据）；
    //   同一次查看内一直保留，换来源时清空。
    QSet<int> m_missing;
    //   "等密码"的结论：条目 → 密码层号。拿到密码（`retryNeedingPassword`）后整张清掉重取。
    QHash<int, int> m_needsPassword;
    //   前端通知的合并标记（同一轮事件里多次完成只判一次；见 `.cpp` 的 `notifyFrontend`）
    bool    m_notifyPending = false;
    struct PreviewJob { QString key; imageread::Location loc; QSize target; };
    std::shared_ptr<PreviewStore> m_previewStore;   // 预览位置表（provider 线程读）
    int m_previewBatch = 0;           // 最近一批的批号
    int m_previewCacheVersion = 0;    // 缓存变化计数（界面据此重查）
    QHash<int, QStringList> m_previewBatchKeys;   // 批号 → 这一批要的 key（查就绪用）
    //   ★ 批次调度（用户 2026-11-04）：**同一时刻只跑一批**；跑完时再看"当前要的是哪一页"，
    //     不是最新就接着为最新那页起下一批 —— 中间滑过的位置不排队、也不额外起批次。
    bool     m_previewRunning = false;
    QVector<PreviewJob> m_previewPendingJobs;   // 最近一次请求"还缺的那几张"
    int      m_previewPendingBatch = 0;         // 对应的批号
    QFutureWatcher<int> *m_previewWatcher = nullptr;
    void startPreviewBatch(const QVector<PreviewJob> &jobs);
    int             m_promptedItem = -1;   // 已经为哪一条问过密码（不重复问）
    int     m_inFlight = 0;                  // 在飞计数
    int     m_skipped  = 0;                  // 跳过（整屏都缺）计数
    quint64 m_epoch    = 0;                  // 换来源记号（单调递增）

    int    m_pos    = 0;                     // 位置：用户指到的那一条（屏 = 它所在的那一屏）
    int    m_cursor = 0;                     // 上屏那一屏的起点（只在 `updateDisplay` 里写）
    int    m_startIndex = 0;                 // `setSequence` 之后要落在哪一条（一次性）
    QImage m_imageA, m_imageB;               // 上屏内容（没结论 / 没有第二个位置 = 空图）
    int    m_keyCount = 0;                   // 本屏位置数（1 或 2），无屏 = 0
    app::Layout m_layout = app::Layout::Single;   // ★ 版面 = 一份枚举（默认单幅）
    Loop   m_loop = Loop::None;                   // ★ 循环模式（会话级；进沉浸视图回默认）
    bool   m_loading  = true;                // 界面浮层（只有 `updateLoading()` 会写）

    int m_dir = +1;                          // 行进方向（+1 前 / −1 后）：份额按它分前后

    //   ★ **已宣布的那一屏**（屏键 = 起点 + 位置集合）：同一屏只宣布一次，
    //     补图 / 预取完成再来重判时**不挂节拍**（这就是 0ms 卡死的根治点）。
    int          m_announcedStart = -1;      // -1 = 还没有宣布过（换位置 / 换版面就清）
    QVector<int> m_announcedItems;

    // ── 幻灯片（13 §九·补·二）────────────────────────────────────────────
    //   两种等待必须分开：**等闹钟**（本屏已就位，能响应）/ **等缓存**（本屏还没上屏，不能响应）。
    //   进度事件（翻页 / 跳转 / 上一·下一文件夹）在等闹钟时**立刻执行**，
    //   在等缓存时进**折叠器**（一个槽，后到覆盖先到，上屏那一刻执行一次）。
    enum class Want { None, Step, Jump, Folder };   // 折叠器里的那条"最终意图"
    bool    waitingForCache() const;          // 现算：本屏还没结论（= 等缓存）
    void    onBeat();                         // 闹钟到点（按**当时**状态计算）
    void    onScreenUp();                     // 上屏那一刻（在 `announce()` 里调）
    void    foldOrRun(Want w, int arg);       // 进度事件的统一入口（播放态）
    void    runWanted(Want w, int arg, bool folded);
    void    requestScreen(int target, int stepDir);   // 换屏 + 记下"请求时刻"
    //   挂上闹钟（**只在宣布新屏时**；幂等：同一 deadline 不重复 start；等缓存时不挂）
    void    armBeat();
    void    cancelBeat();
    void    stopPlaying(const QString &noticeText);   // 停播（到头）
    qint64  nowMs() const { return m_now ? m_now() : 0; }

    bool     m_playing = false;               // ★ 只是一个状态（没有幻灯片对象）
    int      m_intervalMs = 300;              // 间隔（初值由上层从全局默认下发；UI 改动不回写）

    // ── 视图态（viewer 领域）──
    QImage geometryImage() const;             // 双幅取有图的那一张
    int    m_fitMode = 1;                     // 0 原始 / 1 适应屏幕 / 2 适应宽度 / 3 适应高度
    qreal  m_zoom = 1.0, m_panX = 0.0, m_panY = 0.0;
    QSize  m_viewport;                        // 设备像素
    qreal  m_dpr = 1.0;
    bool   m_freeZoom = false;                // zoom 被用户改过 ⇒ fit 不再接管（pan 不算）
    bool   m_zoomLocked = false;
    bool   m_osdVisible = false;
    QString m_scaleAlgorithm = QStringLiteral("lanczos3");
    QString m_screenName;                     // 当前这一屏的文件名（上屏时按真实条目算）
    QString m_screenPath;                     // 当前这一屏的路径（同上）
    QVariantList osdColumn(int itemIndex);    // OSD 的一列（按需读；见 osdRows）
    QString      relativePathOf(int itemIndex) const;   // 相对图集根的路径（`A!/B`）
    QTimer  *m_beat = nullptr;                // 单发闹钟（只负责"到点叫醒"，不参与节奏）
    NowFn    m_now;                           // 单调时钟（可注入；生产在构造里装）
    qint64   m_reqAt = 0;                     // 上一次"成为当前屏"的动作**被请求**的时刻
    bool     m_beatArmed = false;             // 闹钟已挂（同一 deadline 不重复 start）
    qint64   m_beatDeadline = 0;              // 这一枪的到期时刻（= m_reqAt + interval）
    bool     m_pendingAdvance = false;        // 推进等待位（一个 bool；忙碌期只有一个）
    Want     m_want = Want::None;             // 折叠器（一个槽）
    int      m_wantArg = 0;
};

} // namespace fetch
