# ══ 开发工具目标（设计 14 §11.1）：`ib-iconpicker` ══════════════════════════════
#   · **不是测试**：不进 `tests/`、不挂 `add_test`（跑起来是个窗口，靠人眼看）。
#   · 源仍平铺在 `src/`（`src/iconpicker.{h,cpp}`）；这里只定义目标。
#   · ★ 本目录只为**独立的二进制目录**存在：`qt_add_qml_module` 的
#     `qmake_ImageBrowser.qrc` 落在 `${CMAKE_CURRENT_BINARY_DIR}/.qt/rcc/`，
#     与根目录的 `image-browser` 同目录会共用同一个 qrc（qmldir 的 `typeinfo`
#     只有一个赢家）⇒ 放到 `build/tools/` 下，各拿一份。

# 二进制仍落在构建根（`./build/ib-iconpicker`），与 `tests/` 同一约定
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

qt_add_executable(ib-iconpicker
    ${CMAKE_SOURCE_DIR}/src/iconpicker.cpp
    ${CMAKE_SOURCE_DIR}/src/iconpicker.h
)
# `-iquote`（只进双引号包含链）而不是 `-I`：`src/` 一旦进 `-I`，我们的
# `src/archive.h` 就会把 libarchive 的 `<archive.h>` 顶掉（见根 CMakeLists 同一处注释）。
target_compile_options(ib-iconpicker PRIVATE "SHELL:-iquote ${CMAKE_SOURCE_DIR}/src")
# `ibcore`：拿 `app::UiModel`（`Theme.qml` 要看 `ui.themeMode` / `ui.systemDark`）——
#   工具只用它的主题状态（`store = nullptr`，不碰真库）；Qt 的 Quick/Qml 由它 PUBLIC 带过来。
target_link_libraries(ib-iconpicker PRIVATE ibcore Qt6::Quick Qt6::Qml)

# QML 模块（ImageBrowser：Theme + IconPicker）+ 图标字体资源（`iconFont` 用）
ib_add_qml_resources(ib-iconpicker)
