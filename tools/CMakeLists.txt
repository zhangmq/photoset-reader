# ══ 开发工具目标（设计 14 §11.1）═══════════════════════════════════════════════
#   · **不是测试**：不进 `tests/`、不挂 `add_test`。
#   · `ib-iconpicker`：跑起来是个窗口，靠人眼看（QML 页面只在它自己的模块里）。
#   · `ib-bench-io`：控制台程序，量读盘 / 解码 / 读+解码的真实吞吐（英文输出）。
#   · ★ 本目录只为**独立的二进制目录**存在：`qt_add_qml_module` 的
#     `qmake_AppUi.qrc` 落在 `${CMAKE_CURRENT_BINARY_DIR}/.qt/rcc/`，
#     与根目录的 `photoset-reader` 同目录会共用同一个 qrc（qmldir 的 `typeinfo`
#     只有一个赢家）⇒ 放到 `build/tools/` 下，各拿一份。

# 二进制仍落在构建根（`./build/ib-*`），与 `tests/` 同一约定
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

qt_add_executable(ib-iconpicker
    ${CMAKE_SOURCE_DIR}/src/iconpicker.cpp
    ${CMAKE_SOURCE_DIR}/src/iconpicker.h
)
#   `src/` 进 include 路径即可：项目的 `archive.h` 已改名 `archivesource.h`
#   （2026-11-05），不会再顶掉 libarchive 的 `<archive.h>`；
#   也不用 `-iquote` —— MSVC 不认它，会把 `src` 当成源文件（`c1xx: C1083`）。
target_include_directories(ib-iconpicker PRIVATE ${CMAKE_SOURCE_DIR}/src)
# `ibcore`：拿 `app::UiModel`（`Theme.qml` 要看 `ui.themeMode` / `ui.systemDark`）——
#   工具只用它的主题状态（`store = nullptr`，不碰真库）；Qt 的 Quick/Qml 由它 PUBLIC 带过来。
target_link_libraries(ib-iconpicker PRIVATE ibcore Qt6::Quick Qt6::Qml)

# QML 模块（AppUi：Theme + 组件 + 主界面）+ 工具自己的页面 + 图标字体资源（`iconFont` 用）
#   ★ `IconPicker.qml` 作为**额外文件**只编译进这个工具的模块 —— 产品的 `AppUi` 里没有它
#     （用户 2026-11-04：开发工具的页面不进产品）。
ib_add_qml_resources(ib-iconpicker "${CMAKE_CURRENT_SOURCE_DIR}/IconPicker.qml")

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
