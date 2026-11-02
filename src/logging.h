#pragma once

// ══ 分类日志（`QLoggingCategory`）：应用日志的唯一入口 ═════════════════════════
//   · 分类名一律 `ib.*`，**按模块**分（见下）；消息**一律英文**，注释仍中文。
//   · 默认只出 info 及以上；`PHOTOSET_LOG=debug|info|warning|error|off` 改最低级别
//     （由 `iblog::init()` 落成 `QLoggingCategory::setFilterRules()`；`off` 关掉 `ib.*` 全部）。
//   · 消息格式统一：`"[模块] 事件: 细节"`，参数一律 `.arg()`（可 grep、可结构化解析）。
//   · 级别口径：启动 / 一次性 / 用户可感知的状态 ⇒ info；每条 / 每帧 / 内部细节 ⇒ debug；
//     失败 / 降级 / 异常 ⇒ warning。**不新增 error 级**（除非确实致命）。
//   · 每帧 / 每条会重复的消息必须**限流**（只打第一次）：每帧的状态用「只报一次」标志
//     （见 `gpuscaleritem.cpp` 的尺寸不一致），按 key 去重的用 `iblog::firstTime()`（见下面）。

#include <QDebug>
#include <QLoggingCategory>
#include <QString>

// 启动与工具链：main / devcli / screenshot / iconpicker / devbench / appsettings
Q_DECLARE_LOGGING_CATEGORY(lcApp)
// 界面模型：uimodel / viewermodel / progresslistmodel
Q_DECLARE_LOGGING_CATEGORY(lcUi)
// 扫描与容器：sequence / archive / sevenzcheck / dirwalker / progressfile / natural
Q_DECLARE_LOGGING_CATEGORY(lcScan)
// 解码与元数据：imageread / exifmeta
Q_DECLARE_LOGGING_CATEGORY(lcDecode)
// GPU 缩放与 GL：gpuscale / gpuscaleritem / rawgldriver / headlessgl / gpuscalecheck
Q_DECLARE_LOGGING_CATEGORY(lcGpu)

namespace iblog {

// ══ 从 `PHOTOSET_LOG` 读最低级别并落地 ════════════════════════════════════════
//   取值（大小写不敏感，两端空白忽略）：`debug` / `info` / `warning` / `error` / `off`；
//   不设 ⇒ `info`；**拼错 ⇒ 按 `info` + 一条 warning 提示**（不静默吞掉）。
//   幂等：重复调用只是按当前环境变量再设一次（测试 / 工具可以放心多调）。
void init();

// ══ 限流：同一个 `key` 在本进程里**只有第一次**返回 true ════════════════════════
//   用途：会被重复触发的诊断日志（同一容器被每个解码线程各打开一次、每帧、每条…）
//   —— **只在第一次打**，避免刷屏。线程安全（内部只护一个字符串集合，锁里不做 I/O）。
bool firstTime(const QString &key);

} // namespace iblog
