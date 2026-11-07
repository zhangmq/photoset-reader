#pragma once

#include <QString>
#include <QList>

namespace app {

// ★★ **阅读版面**（单幅 / 双幅）—— 这个概念**唯一的一份取值类型**（用户 2026-10-18）：
//   · 库里的 `collection.reading_layout` 存它的**值**（不再存字符串、不再有翻译函数）；
//   · 会话态在 `UiModel`，行为在 `ViewerModel`；
//   · `Sequence` **不带这个概念** —— 它只有"配对表"（谁跟谁配成一对）这份**数据**。
//   ⚠️ 取值显式写死（库列存的就是它）⇒ 不许重排、不许改既有取值。
enum class Layout { Single = 0, Double = 1 };

// ★ 幻灯片间隔的**下限**（唯一事实源）：`ViewerModel` / `AppSettings` / `UiModel` 三处共用。
//   **不设人为下限，可以是 0**（用户 2026-11-03 定）：
//   · 2026-11-03 实测（`--bench-slideshow`，vol01 3288×2200 JPEG）：50 ms ⇒ 18.7 fps；
//     10 ms ⇒ 61 fps（p50 16 ms，解码成了瓶颈）—— 原来的 50 是**人为上限**，不是能力上限；
//   · 所以干脆放到 0：语义仍是"**间隔是下限**"（`到期 = 请求时刻 + 间隔`，到点没备好就等，
//     不堆积、不跳页）⇒ 0 的实际效果 = **每备好一张就立刻翻**，速度由解码 / 上屏 / 屏幕刷新率决定。
inline constexpr int kMinSlideshowIntervalMs = 0;
//   上限（与设置页 `NumberField.to` 一致）：连点"减速"不会跑到天上去
inline constexpr int kMaxSlideshowIntervalMs = 60000;

//   加速 / 减速的**档位网格**（唯一实现；2026-11-07 用户定：步长不变，但要**对称**）：
//     0..100 步 10 ｜ 150..950 步 50 ｜ 1000..9500 步 500 ｜ 10000..60000 步 1000
//   ★ 为什么不再"按当前值算步长"：那样刚跨过边界时，向上用新档步长、向下用旧档步长 ⇒
//     两个方向不是同一条梯子（实测：从 1000 往下**一步跳到 500**，550–950 在减速方向
//     不可达；10000→9000 丢掉 9500；100→50 丢掉 60–90）。现在加减速都是"在网格上移动一格"
//     ⇒ **严格互逆**，且 UI 的预设 (100/500/1000/2000/3000/4000/5000) 全都在网格上。
inline const QList<int> &slideshowLadder()
{
    static const QList<int> grid = [] {
        QList<int> g;
        for (int v = 0; v <= 100; v += 10) g << v;
        for (int v = 150; v < 1000; v += 50) g << v;
        for (int v = 1000; v < 10000; v += 500) g << v;
        for (int v = 10000; v <= 60000; v += 1000) g << v;
        return g;
    }();
    return grid;
}

//   在网格上移动一格（dir > 0 加速 / < 0 减速）。当前值不在网格上（用户手填）⇒ 先**吸附**
//   到相邻那一格：加速取"大于当前的最小格"，减速取"小于当前的最大格"。到头停在两端。
inline int slideshowNextInterval(int cur, int dir)
{
    const QList<int> &g = slideshowLadder();
    if (g.isEmpty()) return cur;
    if (dir > 0) {
        for (int v : g) if (v > cur) return v;
        return g.last();
    }
    for (int i = g.size() - 1; i >= 0; --i) if (g.at(i) < cur) return g.at(i);
    return g.first();
}

// ══ 应用设置（配置文件）════════════════════════════════════════
//   设计：docs/design/11-应用ViewModel设计.md §二 / 14 §9.3 / §10
//
//   · 位置：`~/.config/photoset-reader/settings.json`
//     （`PHOTOSET_SETTINGS_FILE` / `PHOTOSET_CONFIG_DIR` 可覆盖，照旧）
//   · 写法：**整文件全量写入**（用户定案：最后写的赢，不做事务）
//   · **只存设置**。阅读进度 / 扫描结果**不存**（2026-10-31：图集库与 SQLite 已删；
//     只有显式的「保存阅读进度」才落盘 —— 那一批还没做）。
//   · 会话级状态（循环模式 / 锁定缩放 / 窗口全屏 / 排序 / OSD 显隐）**不进这里**。
//   · ★★ 设计 14 §9.3：**没有单例、没有静态状态** —— 运行期实例**挂在 `app::UiModel`
//     上**；其他组件需要哪个值，一律**调用时传参**（不许自己读设置）。
struct AppSettings
{
    //   ★ 设置文件的**格式版本**（写进 JSON 的 `schemaVersion`）：
    //     · 没有这个字段 = 老文件（v0），`load()` 按"缺键就用默认"的方式兼容；
    //     · 比本常量**大** ⇒ 是更新版本写的（我们不认识的键会丢）⇒ 只警告、尽量用已知键；
    //     · 文件本身坏了（JSON 解析失败）⇒ 备份成 `settings.json.broken` 再用默认值，
    //       下一次保存会写出一份新的有效文件（用户 2026-11-03：正式产品要有容错）。
    static constexpr int kSchemaVersion = 1;
    void load();
    //   * 2026-11-06 (user decision): runtime state lives in MEMORY only; there are exactly
    //     two write points - the settings dialog's OK and "restore defaults". Setters do NOT
    //     write, they only mark dirty. Rationale: (1) writes per session drop from one per
    //     setter to one per dialog session; (2) with multiple instances the semantics are
    //     clear - whoever presses OK last wins.
    //     NOTE: Cancel neither writes nor rolls back (the live values stay for this session);
    //     there is no save-on-exit.
    void saveNow();                       // write the whole file once (clears dirty)
    bool dirty() const { return m_dirty; }

    // 常规
    //   ★ 2026-10-20 定案：这里是**全局默认**，不是当前阅读态 —— 每次进沉浸视图新建 viewer 时取一次；
    //     UI（底栏）上的调整只改 viewer，不回写这里。只有设置对话框才落盘。
    int  themeMode() const { return m_themeMode; }              // 0 跟随系统 / 1 浅 / 2 深
    int  intervalMs() const { return m_intervalMs; }            // 幻灯片间隔默认（0–60000 ms）
    int  defaultFitMode() const { return m_fitMode; }           // 0 原始 1 适应窗口 2 适应宽度 3 适应高度
    int  defaultLayout() const { return m_layout; }             // 0 单幅 / 1 双幅
    int  defaultLoop() const { return m_loop; }                 // 0 不循环 / 1 全部循环 / 2 文件夹循环

    // 图像
    // 缩放算法（**唯一**的画质设置）：nearest | smooth | bicubic | lanczos2 | lanczos3
    //   2026-10-20 收敛：原来的 mode/quality/filter/lobes/mitchellB/mitchellC/antiring 七个字段删掉，
    //   只留"选哪个算法"——每个算法内部全固定（见 gpuscale::Algorithm）。
    QString scaleAlgorithm() const { return m_scaleAlgorithm; }

    // 系统
    bool scanHidden() const { return m_scanHidden; }            // 默认关
    bool previewStrip() const { return m_previewStrip; }        // 时间轴悬停预览，默认开
    //   日志级别（用户 2026-11-04）：debug / info / warning / error / off，默认 warning
    QString logLevel() const { return m_logLevel; }
    int  recurseDepth() const { return m_recurseDepth; }        // 默认 5 层
    //   ★ T2d-1d-②：语义 = **预取的图数**（当前屏那 1~2 张不算预取、不占这个额度），默认 **8**。
    //     名字沿用旧名（改名按术语铁律单列报批）。
    //   ★ 2026-11-05（用户）：**0 合法** —— 预取数量就是预取数量；0 = 只备本屏、不预热邻居。
    int  prefetchWindow() const { return m_prefetchWindow; }
    // ★ v7：**内层压缩包上限**（MB）—— 超过上限的内层压缩包不展开（嵌套包不常见，
    //   避免为此付出大代价）。判定在**解压前**（从归档条目元数据取成员大小）。
    int  innerArchiveMaxMb() const { return m_innerArchiveMaxMb; }   // 默认 512，范围 64–8192
    //   ★ 2026-10-31：收藏夹路径（空 = 默认 `<配置目录>/favorites`；用户可改成任意目录，含外部盘）
    QString favoritesDir() const { return m_favoritesDir; }

    // 界面语言：**空串 = 跟随系统**；否则是语言代码（`zh_CN` / `en` …）。
    //   ★ 与 vsr-player 同口径（那边叫 `lang`，空 = 系统 locale）：**新增语言 = 加一份 .ts**，
    //     代码里不需要任何"语言清单"（`QTranslator` 按代码去 `:/i18n/` 找，找不到就回退源串=英文）。
    QString language() const { return m_language; }

    void setThemeMode(int v) { m_themeMode = v; m_dirty = true; }
    void setIntervalMs(int v) { m_intervalMs = qMax(kMinSlideshowIntervalMs, v); m_dirty = true; }
    void setDefaultFitMode(int v) { m_fitMode = qBound(0, v, 3); m_dirty = true; }
    void setDefaultLayout(int v) { m_layout = qBound(0, v, 1); m_dirty = true; }
    void setDefaultLoop(int v) { m_loop = qBound(0, v, 2); m_dirty = true; }
    void setScaleAlgorithm(const QString &v) { m_scaleAlgorithm = v; m_dirty = true; }
    void setScanHidden(bool v) { m_scanHidden = v; m_dirty = true; }
    void setPreviewStrip(bool v) { m_previewStrip = v; m_dirty = true; }
    void setLogLevel(const QString &v) { m_logLevel = v; m_dirty = true; }
    void setRecurseDepth(int v) { m_recurseDepth = qBound(1, v, 64); m_dirty = true; }
    void setPrefetchWindow(int v) { m_prefetchWindow = qBound(0, v, 8); m_dirty = true; }
    void setInnerArchiveMaxMb(int v) { m_innerArchiveMaxMb = qBound(64, v, 8192); m_dirty = true; }
    void setFavoritesDir(const QString &v) { m_favoritesDir = v; m_dirty = true; }
    void setLanguage(const QString &v) { m_language = v; m_dirty = true; }

    QString filePath() const;

private:
    int  m_themeMode = 0;
    int  m_intervalMs = 1000;
    int  m_fitMode = 1;
    int  m_layout = 0;                  // 默认版面：单幅
    int  m_loop = 0;                    // 默认循环：不循环
    QString m_scaleAlgorithm = QStringLiteral("lanczos3");
    bool m_scanHidden = false;
    bool m_previewStrip = true;
    QString m_logLevel = QStringLiteral("warning");
    int  m_recurseDepth = 5;
    //   ★ 默认 **1**（用户 2026-11-05）：并行解码数 = 预取数 + 本屏图数（上限 = 核数），
    //     默认 1 就只多开一条预取；再大是"用内存/功耗换抖动"，要就让用户自己加。
    int  m_prefetchWindow = 1;
    int  m_innerArchiveMaxMb = 512;     // ★ v7：内层压缩包上限（MB），范围 64–8192
    QString m_favoritesDir;             // 收藏夹路径（空 = 默认 `<配置目录>/favorites`）
    QString m_language;                 // 界面语言代码（空 = 跟随系统）
    bool m_dirty = false;               // unsaved changes (only saveNow() clears it)
};

} // namespace app
