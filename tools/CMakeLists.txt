# ══ 开发工具目标（设计 14 §11.1）═══════════════════════════════════════════════
#   · **不是测试**：不进 `tests/`、不挂 `add_test`。
#   · `ib-bench-io`：控制台程序，量读盘 / 解码 / 读+解码的真实吞吐（英文输出）。
#   · `ib-bench-feed`：控制台程序，按目标 fps 喂帧，量预取能不能喂饱消费者。
#   ★ 2026-11-06 用户决定：**`ib-iconpicker` 删掉了**（挑图标码点的临时工具，查 MDI 官网更快）。
#     它绑了整套 `AppUi` QML 模块 ⇒ 任何 `.qml` 改动都要多编一份 + 多链一次，纯浪费。
#   ★ 本目录只为**独立的二进制目录**存在：`qt_add_qml_module` 的
#     `qmake_AppUi.qrc` 落在 `${CMAKE_CURRENT_BINARY_DIR}/.qt/rcc/`，
#     与根目录的 `photoset-reader` 同目录会共用同一个 qrc（qmldir 的 `typeinfo`
#     只有一个赢家）⇒ 放到 `build/tools/` 下，各拿一份。

# 二进制仍落在构建根（`./build/ib-*`），与 `tests/` 同一约定
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ── 独立实验：读盘 / 解码 / 读+解码 吞吐（用户 2026-11-05）────────────────────
#   目的：量"预取到底能不能提速" —— 慢盘（H:）/ 快盘（M:）的真实带宽、解码上限，
#   以及并发 1/3/4/6 的流水线吞吐。★ 产品的在飞上限写死 4（`kMaxInFlight`），
#   所以按用户的映射（预取 N = 本屏 1 + 前面 N−1 张）跑到 6 也**不会**比 4 更快。
#   ★ 不带 QML、不建 `ViewerModel`：只量底层；输出全英文走 stdout（避免 GBK 终端乱码）。
qt_add_executable(ib-bench-io
    ${CMAKE_CURRENT_SOURCE_DIR}/bench-io.cpp
)
target_include_directories(ib-bench-io PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ib-bench-io PRIVATE ibcore)

# ── 独立实验②：**预取能不能喂饱 60 fps 的消费者**（用户 2026-11-05 的验收口径）──────
#   间隔设成 1000/fps，让真 `ViewerModel` 去预取 + 推进，量"每屏实际间隔"：
#   ≈ 节奏 ⇒ 消费没等；明显超过 ⇒ 这一屏在等解码/读盘（miss）。
#   `-flush N` 每次测量前挤掉文件缓存（慢盘上预取越大是否反而更慢，就看这张表）。
qt_add_executable(ib-bench-feed
    ${CMAKE_CURRENT_SOURCE_DIR}/bench-feed.cpp
)
target_include_directories(ib-bench-feed PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ib-bench-feed PRIVATE ibcore)
