cmake_minimum_required(VERSION 3.21)
project(image-browser LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick Qml Sql Widgets ShaderTools)
# QRhi / QShader（rhi/*.h 在 Qt 的私有包含路径下；QQuickRhiItem 文档也是这么要求的）
find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
# 7z 头解析要用 liblzma（判 solid 用；不参与解码路径）
find_package(LibLZMA REQUIRED)
qt_standard_project_setup()

# 测试入口（设计 14 §11）：`ctest` 能跑下面每个套件
enable_testing()


find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBARCHIVE REQUIRED libarchive)
# EXIF 解析：libexif（LGPL-2.1，动态链接）—— Qt 不向外暴露任何 EXIF 标签（见设计 17 §二）
pkg_check_modules(LIBEXIF REQUIRED libexif)
find_package(ZLIB REQUIRED)

# libunrar：只读 RAR 用途；无 pkg-config，手动找
find_library(UNRAR_LIBRARY NAMES unrar REQUIRED)
find_path(UNRAR_INCLUDE_DIR NAMES unrar/dll.hpp REQUIRED)


# ══ ibcore：应用与测试**共用同一份实现**（设计 14 §11）════════════════════════
#   除 `src/main.cpp` 外的全部源都在这里；`image-browser` 与各 `ib-test-*` 都链它。
add_library(ibcore STATIC
    src/imageprovider.cpp
    src/imageprovider.h
    src/sevenzcheck.cpp
    src/sevenzcheck.h
    src/thumburl.cpp
    src/thumburl.h
    src/browsemodel.cpp
    src/browsemodel.h
    # ── ★★ T2d-1b：`ui.browse`（浏览界面模型，**从 viewer 里搬出来**）──
    #     行 / 面包屑 / 能否上一级 / 范围标签 / 高亮 + 浏览动作；只吃数据包，不认引擎。
    src/browsemodelview.cpp
    src/browsemodelview.h
    src/viewermodel.cpp
    src/viewermodel.h
    # ── 应用唯一的全局 VM（`ui`）：三屏状态 / 设置 / UI 形态 / 适配与手势态 ──
    src/uimodel.cpp
    src/uimodel.h
    # ── GPU 缩放渲染路径（产品路径）──
    src/gpuscale.cpp
    src/gpuscale.h
    src/gpuscaleritem.cpp
    src/gpuscaleritem.h
    # CPU 缩放项：Smooth (Qt) 这一项 + 软件后端降级都走它（设计 16 §五）
    src/cpuscaleritem.cpp
    src/cpuscaleritem.h
    src/gpuscalecheck.cpp
    src/gpuscalecheck.h
    src/rawgldriver.cpp
    src/rawgldriver.h
    src/headlessgl.cpp
    src/headlessgl.h
    src/archive.cpp
    src/archive.h
    src/dirwalker.cpp
    src/dirwalker.h
    src/natural.cpp
    src/natural.h
    src/sequence.cpp
    src/sequence.h
    # ── 条目读取（位置 + 读图）：`Sequence` 只调用，不碰 QImageReader / QImage ──
    src/imageread.cpp
    src/imageread.h
    # ── 元数据（EXIF / 色彩空间）：纯函数，把"头字节"变成 OSD 的行 ──
    src/exifmeta.cpp
    src/exifmeta.h
    # ── 浏览的一层（`fetch::BrowseLayer` / `browseLayerOf`）：浏览界面直接从序列取 ──
    src/seqwindow.cpp
    src/seqwindow.h
    src/appsettings.cpp
    src/appsettings.h
    src/librarystore.cpp
    src/librarymodel.cpp
    src/volumeinfo.cpp
    # ── T2c-1/T2c-2b（设计 14 §9.3/§9.6）：图库/序列抽象 + 重置队列 ——
    #     图集动作**全部经 `LibraryCore`**（旧 `ScanController` / `libraryprobe` 已删）──
    src/librarycore.cpp
    src/resetqueue.cpp
    src/librarystore.h
    src/librarymodel.h
    src/volumeinfo.h
    src/librarycore.h
    src/resetqueue.h
)

# ── GPU 缩放的 shader：**qsb 烘焙**（生产代码里不塞裸 GLSL 字符串）──────
#   源是 Vulkan 风格 GLSL（#version 440），qsb 生成 GLSL 330 / 300es、SPIR-V、
#   HLSL 5.0、MSL 2.0 —— 所以 OpenGL / Vulkan / D3D / Metal 后端都能跑。
#   资源路径 = :/<文件名>.qsb（见 gpuscale.cpp 的 loadShader）
#   ★ 挂在 **ibcore** 上：应用与将来的 GPU 套件共用同一份资源（只有一处烘焙）。
qt_add_shaders(ibcore "gpuscale_shaders"
    PREFIX "/"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders"
    GLSL "440,330,300es"
    SPIRV "100"
    HLSL "50"
    MSL "20"
    FILES
        src/shaders/gpuscale.vert
        src/shaders/gpuscale_linearize.frag
        src/shaders/gpuscale_resample.frag
        src/shaders/gpuscale_resample_ar_h.frag
        src/shaders/gpuscale_resample_ar_v.frag
)

target_link_libraries(ibcore PUBLIC
    Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Sql Qt6::Widgets
    Qt6::GuiPrivate
    ${LIBARCHIVE_LIBRARIES}
    ${LIBEXIF_LIBRARIES}
    ZLIB::ZLIB
    LibLZMA::LibLZMA
    ${UNRAR_LIBRARY})
target_include_directories(ibcore PUBLIC ${LIBARCHIVE_INCLUDE_DIRS} ${LIBEXIF_INCLUDE_DIRS}
                                        ${UNRAR_INCLUDE_DIR})

# ══ testsupport：测试脚手架**独立成库**（设计 14 §11）════════════════════════
#   `check` / `waitFor` / `TempFixture` / `pipetest` / 套件入口 —— 只有**测试**该碰它，
#   `ibcore` 不再包含它（应用库保持"只是应用"）。
#   ★ 源与头都在 `tests/`（测试文件不与应用源码混放）；**库仍定义在根** ——
#     只有 `tests/` 下的 `ib-test-*` 链它（应用**不链**：套件已全部搬出，设计 14 §11）。
add_library(testsupport STATIC
    tests/testsupport.cpp
    tests/testsupport.h
)
target_include_directories(testsupport PUBLIC ${CMAKE_SOURCE_DIR}/tests)
# 套件源在 `tests/`，`#include "sequence.h"` 这类要能找到 `src/` 下的项目头。
#   ★ 用 `-iquote`（只进**双引号包含**链）而**不是** `-I`：`src/` 一旦进 `-I`，
#     我们的 `src/archive.h` 就会把 libarchive 的 `<archive.h>` 顶掉（`-I` 先于系统目录
#     被搜索）—— main.cpp 里 `qmlRegisterType` 一处已有同一个坑的注释。
#     `-iquote` 只影响 `#include "..."`，正好只给项目头用，不动 `<archive.h>`。
target_compile_options(testsupport PUBLIC "SHELL:-iquote ${CMAKE_SOURCE_DIR}/src")
target_link_libraries(testsupport PUBLIC ibcore)

# 开发/CI 用的 headless GL 引导（src/headlessgl.cpp）：EGL surfaceless + 软件栅格器。
# 生产路径不需要它（真机 Qt Quick 自己建 RHI），所以缺 EGL 只是功能退化为"不可用"。
if(UNIX AND NOT APPLE)
    find_library(EGL_LIBRARY NAMES EGL)
    find_library(GL_LIBRARY NAMES GL)
    if(EGL_LIBRARY AND GL_LIBRARY)
        target_link_libraries(ibcore PUBLIC ${EGL_LIBRARY} ${GL_LIBRARY})
        target_compile_definitions(ibcore PUBLIC GPUWIRE_HAVE_EGL=1)
    else()
        message(STATUS "未找到 libEGL/libGL —— headless GL 引导与 raw GL 自检驱动不可用")
    endif()
endif()

# 开发期便利：把 .testdata/vol01 作为默认目录传给程序（可用 -d 覆盖）
target_compile_definitions(ibcore PUBLIC
    DEFAULT_TEST_DIR="${CMAKE_SOURCE_DIR}/.testdata/vol01"
)


# ══ 应用：只剩启动 + 解析 + dispatch（设计 14 §10）═══════════════════════════
#   实现分四处：`src/devcli.{h,cpp}`（dev CLI：探针 / 预载基准 / 双实例 / `--dev-*`）、
#               `src/devbench.{h,cpp}`（探针：`--gpuscale-probe`，跑完即退；
#                 底层在 `gpuscalecheck.cpp`）、
#               `src/screenshot.{h,cpp}`（`--shot` 截图机制；原名 `debugview.{h,cpp}`，
#                 随旧调试一屏（`debugview`）删除后改名保留）、
#               `src/main.cpp`（启动 + 参数解析 + dispatch）。
qt_add_executable(image-browser
    src/main.cpp
    src/devcli.cpp
    src/devcli.h
    src/devbench.cpp
    src/devbench.h
    src/screenshot.cpp
    src/screenshot.h
)
# ★ 套件全部搬去 `tests/`（T0 第四批）后，应用**不再链 `testsupport`**（设计 14 §11）：
#   `main.cpp` 里 0 个 `run*SelfTest` / 0 个套件 dispatch；dev CLI 与截图机制自带工具函数。
target_link_libraries(image-browser PRIVATE ibcore)

# ══ QML 模块 + 图标字体资源：挂在**每个要加载 `qrc:/ImageBrowser` 的可执行**上 ══
#   设计 14 §11：每个测试目标各自独立 ⇒ 谁需要 QML 谁带一份（静态库里的 QML 模块在
#   链接期可能被丢掉；直接挂到目标上最直接、也最可验证）。
#   ⚠️ `OUTPUT_DIRECTORY` 必须**每个目标各不相同**：Qt 不允许两个 QML 模块共用同一个
#      输出目录，而 `ib-test-uiwire` / `ib-test-wiring` 同在 `tests/` 下。
#   ★ 本函数也会在 `tests/` 子目录里被调用 ⇒ 路径一律用**绝对**的
#     （`QML_FILES` 相对 `CMAKE_CURRENT_SOURCE_DIR` 解析）。
function(ib_add_qml_resources target)
    string(REPLACE "-" "_" qmlres_name "${target}")
    qt_add_resources(${target} "${qmlres_name}_qmlres"
        PREFIX "/"
        BASE "${CMAKE_SOURCE_DIR}/src"
        FILES
            "${CMAKE_SOURCE_DIR}/src/assets/fonts/materialdesignicons-webfont.ttf"
    )

    # ── 新 UI（正规 QML 模块 + 单例令牌）─────────────────────────
    # 旧 UI（src/qml/*）已于 2026-10-11 弃用并移入 .archive/，不再参与构建
    set_source_files_properties("${CMAKE_SOURCE_DIR}/src/ui/Theme.qml"
        PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
    set(qml_files
        src/ui/Theme.qml
        src/ui/Main.qml
        src/ui/components/PopupPanel.qml
        src/ui/components/PopupMenu.qml
        src/ui/components/Button.qml
        src/ui/components/DropdownButton.qml
        src/ui/components/IconDropdownButton.qml
        src/ui/components/IconAction.qml
        src/ui/components/HintBubble.qml
        src/ui/components/SearchField.qml
        src/ui/components/ConfirmDialog.qml
        src/ui/components/SegmentedControl.qml
        src/ui/components/Breadcrumb.qml
        src/ui/components/Badge.qml
        src/ui/components/Toast.qml
        src/ui/components/Switch.qml
        src/ui/components/NumberField.qml
        src/ui/components/SettingsDialog.qml
        src/ui/components/SettingsRow.qml
        src/ui/components/SettingsKeyRow.qml
        src/ui/components/SettingsGeneralPage.qml
        src/ui/components/SettingsImagePage.qml
        src/ui/components/SettingsSystemPage.qml
        src/ui/components/SettingsShortcutsPage.qml
        src/ui/library/LibraryView.qml
        src/ui/library/CollectionCard.qml
        src/ui/debug/IconPicker.qml
        src/ui/browse/BrowseView.qml
        src/ui/browse/BrowseActionBar.qml
        src/ui/browse/BrowsePathBar.qml
        src/ui/browse/ThumbCard.qml
        src/ui/browse/FileRow.qml
        src/ui/browse/FileListHeader.qml
        src/ui/fullscreen/FullscreenView.qml
        src/ui/fullscreen/FullscreenTopBar.qml
        src/ui/fullscreen/FullscreenBottomBar.qml
        src/ui/fullscreen/FullscreenTimeline.qml
        src/ui/fullscreen/FullscreenOsd.qml
        src/ui/fullscreen/NavBtn.qml
        src/ui/library/EmptyState.qml
    )
    #   ⚠️ QML 源一律按**绝对路径**进 `QML_FILES`（本函数可能在 `tests/` 里被调用）；
    #      绝对路径**必须**配 `QT_RESOURCE_ALIAS`（相对源码根），否则 Qt 直接 FATAL_ERROR，
    #      资源树也会变成绝对路径。别名与搬迁前一致：`:/ImageBrowser/src/ui/...`。
    set(qml_files_abs)
    foreach(f IN LISTS qml_files)
        set_source_files_properties("${CMAKE_SOURCE_DIR}/${f}"
            PROPERTIES QT_RESOURCE_ALIAS "${f}")
        list(APPEND qml_files_abs "${CMAKE_SOURCE_DIR}/${f}")
    endforeach()
    qt_add_qml_module(${target}
        URI ImageBrowser
        VERSION 1.0
        OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${target}_qml"
        QML_FILES ${qml_files_abs}
    )
endfunction()

ib_add_qml_resources(image-browser)


# ══ 开发工具：`ib-iconpicker`（设计 14 §11.1）══════════════════════════════════
#   自带 `main()` 的独立可执行（**不是测试**：不进 `tests/`、不挂 `add_test`）。
#   ★ 放**子目录**定义：`qt_add_qml_module` 的 `qmake_<URI>.qrc` 生成在
#     `${CMAKE_CURRENT_BINARY_DIR}/.qt/rcc/` —— 与 `image-browser` 同目录会让两个
#     目标的 QML 模块共用同一个 qrc（qmldir 里 `typeinfo` 指向谁就看谁最后配置）。
#     子目录 = 独立二进制目录 = 每个目标一份真资源（与 `tests/` 同一手法）。
add_subdirectory(tools)


# ══ 测试：全部在 `tests/`（源 + 目标 + add_test 都在那儿）══════════════════════
#   `enable_testing()` 在上面（根）已调用；测试目标由 `tests/CMakeLists.txt` 定义。
#   `ibcore` / `testsupport` 两个**库**留在根（应用与测试共用同一份实现）。
add_subdirectory(tests)
