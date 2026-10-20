#pragma once

#include <QString>

namespace app {

// ★★ **阅读版面**（单幅 / 双幅）—— 这个概念**唯一的一份取值类型**（用户 2026-10-18）：
//   · 库里的 `collection.reading_layout` 存它的**值**（不再存字符串、不再有翻译函数）；
//   · 会话态在 `UiModel`，行为在 `ViewerModel`；
//   · `Sequence` **不带这个概念** —— 它只有"配对表"（谁跟谁配成一对）这份**数据**。
//   ⚠️ 取值显式写死（库列存的就是它）⇒ 不许重排、不许改既有取值。
enum class Layout { Single = 0, Double = 1 };

// ══ 应用设置（配置文件）════════════════════════════════════════
//   设计：docs/design/11-应用ViewModel设计.md §二 / 14 §9.3 / §10
//
//   · 位置：`~/.config/photoset-reader/settings.json`
//     （`PHOTOSET_SETTINGS_FILE` / `PHOTOSET_CONFIG_DIR` 可覆盖，照旧）
//   · 写法：**整文件全量写入**（用户定案：最后写的赢，不做事务）
//   · **只存设置**。图集库、条目、进度、扫描状态全部在 SQLite（library.db），
//     旧 `state.json`（dirs + resume）按定案**放弃、不迁移**。
//   · 会话级状态（循环模式 / 锁定缩放 / 窗口全屏 / 排序 / OSD 显隐）**不进这里**。
//   · ★★ 设计 14 §9.3：**没有单例、没有静态状态** —— 运行期实例**挂在 `app::UiModel`
//     上**；其他组件需要哪个值，一律**调用时传参**（不许自己读设置）。
struct AppSettings
{
    void load();
    void save();

    // 常规
    int  themeMode() const { return m_themeMode; }              // 0 跟随系统 / 1 浅 / 2 深
    int  intervalMs() const { return m_intervalMs; }            // 幻灯片间隔，下限 50ms
    int  defaultFitMode() const { return m_fitMode; }           // 0 原始 1 适应屏幕 2 适应宽度 3 适应高度

    // 图像
    // 缩放算法（**唯一**的画质设置）：nearest | smooth | bicubic | lanczos2 | lanczos3
    //   2026-10-20 收敛：原来的 mode/quality/filter/lobes/mitchellB/mitchellC/antiring 七个字段删掉，
    //   只留"选哪个算法"——每个算法内部全固定（见 gpuscale::Algorithm）。
    QString scaleAlgorithm() const { return m_scaleAlgorithm; }

    // 系统
    bool scanHidden() const { return m_scanHidden; }            // 默认关
    int  recurseDepth() const { return m_recurseDepth; }        // 默认 5 层
    //   ★ T2d-1d-②：语义 = **两个方向的条数相加**（队列长度恒为该值），默认 **8**。
    //     名字沿用旧名（改名按术语铁律单列报批）。
    int  prefetchWindow() const { return m_prefetchWindow; }
    int  prefetchBatch() const { return m_prefetchBatch; }      // 默认 4（一次一批的批大小 = 并发解码数）
                                                                //   ★ 暂**不暴露到界面**（用户 2026-10-12）
    // ★ v7：**内层压缩包上限**（MB）—— 超过上限的内层压缩包不展开（嵌套包不常见，
    //   避免为此付出大代价）。判定在**解压前**（从归档条目元数据取成员大小）。
    int  innerArchiveMaxMb() const { return m_innerArchiveMaxMb; }   // 默认 512，范围 64–8192

    void setThemeMode(int v) { m_themeMode = v; save(); }
    void setIntervalMs(int v) { m_intervalMs = qMax(50, v); save(); }
    void setDefaultFitMode(int v) { m_fitMode = v; save(); }
    void setScaleAlgorithm(const QString &v) { m_scaleAlgorithm = v; save(); }
    void setScanHidden(bool v) { m_scanHidden = v; save(); }
    void setRecurseDepth(int v) { m_recurseDepth = qBound(1, v, 64); save(); }
    void setPrefetchWindow(int v) { m_prefetchWindow = qBound(3, v, 64); save(); }
    void setPrefetchBatch(int v) { m_prefetchBatch = qBound(1, v, 32); save(); }
    void setInnerArchiveMaxMb(int v) { m_innerArchiveMaxMb = qBound(64, v, 8192); save(); }

    QString filePath() const;

private:
    int  m_themeMode = 0;
    int  m_intervalMs = 300;
    int  m_fitMode = 1;
    QString m_scaleAlgorithm = QStringLiteral("lanczos3");
    bool m_scanHidden = false;
    int  m_recurseDepth = 5;
    int  m_prefetchWindow = 8;
    int  m_prefetchBatch = 4;
    int  m_innerArchiveMaxMb = 512;     // ★ v7：内层压缩包上限（MB），范围 64–8192
};

} // namespace app
