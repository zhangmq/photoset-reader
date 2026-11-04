#pragma once

// ══ 分类日志（`QLoggingCategory`）：应用日志的唯一入口 ═════════════════════════
//   · 分类名一律 `ib.*`，**按模块**分（见下）；消息**一律英文**，注释仍中文。
//   · **默认只出 warning 及以上**；级别由**设置项**（设置 → 系统 → 日志级别）决定，
//     启动时 `UiModel` 调 `iblog::setLevel()` 落地（`QLoggingCategory::setFilterRules()`）；
//     `--log-level <级别>` 可临时覆盖一次（不写盘）。不再有 `PHOTOSET_LOG` 环境变量。
//   · 消息格式统一：`"[模块] 事件: 细节"`，参数一律 `.arg()`（可 grep、可结构化解析）。
//   · 级别口径：启动 / 一次性 / 用户可感知的状态 ⇒ info；每条 / 每帧 / 内部细节 ⇒ debug；
//     失败 / 降级 / 异常 ⇒ warning。**不新增 error 级**（除非确实致命）。
//   · 每帧 / 每条会重复的消息必须**限流**（只打第一次）：每帧的状态用「只报一次」标志
//     （见 `gpuscaleritem.cpp` 的尺寸不一致），按 key 去重的用 `iblog::firstTime()`（见下面）。

#include <QDebug>
#include <QLoggingCategory>
#include <QString>

// 启动与工具链：main / appsettings / iconpicker
Q_DECLARE_LOGGING_CATEGORY(lcApp)
// 界面模型：uimodel / viewermodel / progresslistmodel
Q_DECLARE_LOGGING_CATEGORY(lcUi)
// 扫描与容器：sequence / archive / sevenzcheck / dirwalker / progressfile / natural
Q_DECLARE_LOGGING_CATEGORY(lcScan)
// 解码与元数据：imageread / exifmeta
Q_DECLARE_LOGGING_CATEGORY(lcDecode)
// GPU 缩放：gpuscale / gpuscaleritem
Q_DECLARE_LOGGING_CATEGORY(lcGpu)

namespace iblog {

//   日志级别：**从小到大**（越靠后越安静）
enum class Level { Debug, Info, Warning, Error, Off };

//   名字 ↔ 级别（大小写不敏感；拼错返回 false，调用方决定怎么提示）
bool levelFromName(const QString &name, Level *out);
QString levelName(Level l);

//   落地到 `QLoggingCategory`（全量替换规则，只作用于 `ib.*`）。幂等。
void setLevel(Level level);
Level level();

//   ★ 老入口保留成"按设置再落地一次"的别名，避免调用点全改：
void init();

// ══ 限流：同一个 `key` 在本进程里**只有第一次**返回 true ════════════════════════
//   用途：会被重复触发的诊断日志（同一容器被每个解码线程各打开一次、每帧、每条…）
//   —— **只在第一次打**，避免刷屏。线程安全（内部只护一个字符串集合，锁里不做 I/O）。
bool firstTime(const QString &key);

} // namespace iblog
