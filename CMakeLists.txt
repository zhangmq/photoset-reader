cmake_minimum_required(VERSION 3.21)
#   版本号**唯一来源**（打包脚本从这里读、C++ 用 PHOTOSET_VERSION 编译进去、`--version` 打它）
project(photoset-reader VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick Qml QuickControls2 Widgets ShaderTools DBus LinguistTools Concurrent)
# QRhi / QShader（rhi/*.h 在 Qt 的私有包含路径下；QQuickRhiItem 文档也是这么要求的）
find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
# 7z 头解析要用 liblzma（判 solid 用；不参与解码路径）
find_package(LibLZMA REQUIRED)
qt_standard_project_setup()

# ══ QML 模块的放置位置：**Qt 6.5+ 的标准布局**（2026-10-25 定案）════════════════
#   QTP0001 NEW ⇒ 模块落在 `:/qt/qml/<URI>/`，而 `qrc:/qt/qml` **本来就在** QML 引擎的
#   默认导入路径里 ⇒ `import AppUi` 不需要任何运行时配置就能找到 qmldir。
#   ⚠️ 为什么必须显式写：本工程 `cmake_minimum_required(3.21)` < 6.5 ⇒ 策略默认 OLD
#   ⇒ 模块落到 `:/<URI>/`，引擎找不到（实测：所有 `import AppUi` 都报 "X is not a type"；
#   旧策略下只能靠 `engine.addImportPath("qrc:/")` 打补丁，2026-10-25 已改用本策略）。
qt_policy(SET QTP0001 NEW)

# 测试入口（设计 14 §11）：`ctest` 能跑下面每个套件
enable_testing()


find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBARCHIVE REQUIRED libarchive)
# EXIF 解析：libexif（LGPL-2.1，动态链接）—— Qt 不向外暴露任何 EXIF 标签（见设计 17 §二）
pkg_check_modules(LIBEXIF REQUIRED libexif)
# HEIC/AVIF 的 Exif/ICC 在 BMFF box 表里：用 libheif（LGPL-3.0）按需取，不自己解容器
#   ★ 不用 Exiv2（GPL-2.0+）—— 项目要能按宽松许可分发（用户 2026-10-21 定案）
pkg_check_modules(LIBHEIF REQUIRED libheif)
find_package(ZLIB REQUIRED)

# libunrar：只读 RAR 用途；无 pkg-config，手动找
find_library(UNRAR_LIBRARY NAMES unrar REQUIRED)
find_path(UNRAR_INCLUDE_DIR NAMES unrar/dll.hpp REQUIRED)


# ══ ibcore：应用与测试**共用同一份实现**（设计 14 §11）════════════════════════
#   除 `src/main.cpp` 外的全部源都在这里；`photoset-reader` 与各 `ib-test-*` 都链它。
add_library(ibcore STATIC
    # ── 分类日志（`QLoggingCategory`）：全应用日志的唯一入口（名字 / 级别见 src/logging.h）──
    src/logging.cpp
    src/logging.h
    src/sevenzcheck.cpp
    src/sevenzcheck.h
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
    src/appsettings.cpp
    src/appsettings.h
    # ── 进度文件（`.prsave`）：收藏 / 打开进度的唯一实现（设计 20）──
    src/progressfile.cpp
    src/progressfile.h
    src/progresslistmodel.cpp
    src/progresslistmodel.h
    # ── 时间轴悬停预览：位置表 + provider（只解码缩略图）──
    src/previewprovider.cpp
    src/previewprovider.h
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
    Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::QuickControls2 Qt6::Widgets Qt6::DBus Qt6::Concurrent
    Qt6::GuiPrivate
    ${LIBARCHIVE_LIBRARIES}
    ${LIBEXIF_LIBRARIES}
    ${LIBHEIF_LIBRARIES}
    ZLIB::ZLIB
    LibLZMA::LibLZMA
    ${UNRAR_LIBRARY})
target_include_directories(ibcore PUBLIC ${LIBARCHIVE_INCLUDE_DIRS} ${LIBEXIF_INCLUDE_DIRS}
                                        ${LIBHEIF_INCLUDE_DIRS} ${UNRAR_INCLUDE_DIR})

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

#   自检用的 headless GL / raw GL 驱动（`headlessgl.*` / `rawgldriver.*`）与 GPU 等价性探针
#   （`gpuscalecheck.*`）已随 `--gpuscale-probe` 一起删除（用户 2026-11-04：测试不进产品）。

# 开发期便利：把 .testdata/vol01 作为默认目录传给程序（可用 -d 覆盖）
target_compile_definitions(ibcore PUBLIC
    DEFAULT_TEST_DIR="${CMAKE_SOURCE_DIR}/.testdata/vol01"
    PHOTOSET_VERSION="${PROJECT_VERSION}"
)


# ══ 应用：只剩启动 + 解析 + dispatch（设计 14 §10）═══════════════════════════
#   产品 CLI = "打开路径（位置参数）+ --fullscreen"（用户 2026-11-04 定：
#   测试 / 验收开关一律不进产品；探针、截图、bench 那批文件已删除）。
qt_add_executable(photoset-reader
    src/main.cpp
)
# ★ 套件全部搬去 `tests/`（T0 第四批）后，应用**不再链 `testsupport`**（设计 14 §11）：
#   `main.cpp` 里 0 个 `run*SelfTest` / 0 个套件 dispatch；dev CLI 与截图机制自带工具函数。
target_link_libraries(photoset-reader PRIVATE ibcore)

# ══ 安装规则（`cmake --install` / 打包脚本都走这里；DESTDIR 支持照常）═════════
#   装：可执行 + 桌面入口 + 全套 hicolor 图标 + 许可/第三方声明。
#   ★ 翻译 `.qm` 已编进可执行（见下），**不需要**装 translations 目录。
include(GNUInstallDirs)
install(TARGETS photoset-reader RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES assets/photoset-reader.desktop
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
foreach(_size 16 24 32 48 64 128 256 512)
    install(FILES "assets/icons/photoset-reader-${_size}.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps"
            RENAME photoset-reader.png)
endforeach()
install(FILES assets/icons/photoset-reader.svg
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
        RENAME photoset-reader.svg)
install(FILES LICENSE THIRD-PARTY.md
        DESTINATION "${CMAKE_INSTALL_DOCDIR}")

# ══ 应用图标：编进资源（窗口图标 / X11 任务栏用；桌面入口与 hicolor 由安装规则铺）══
qt_add_resources(photoset-reader "appicon"
    PREFIX "/icons"
    BASE "${CMAKE_SOURCE_DIR}/assets/icons"
    FILES "${CMAKE_SOURCE_DIR}/assets/icons/photoset-reader-256.png")

# ══ i18n：`.ts` → lrelease → `.qm` **编进资源**（`:/i18n/photoset-reader_<代码>.qm`）══
#   · 源串一律**英文**（`qsTr()`/`tr()` 里写英文），其它语言靠译文——与 vsr-player 同一路线；
#   · 本文件**只跑 lrelease**（不会覆盖译文）；`.ts` 的更新走
#     `scripts/update-translations.sh`（lupdate + 完整性检查，输入清单在脚本里）；
#   · `.qm` 进资源 ⇒ 动态版 / 静态版 / AppImage / 便携包**都不用再带一个 translations 目录**
#     （vsr-player 是放磁盘 + 运行时找目录，我们只有一份可执行，编进去更省事）。
#   · 新增语言 = 往 `i18n/` 放一份 `<程序名>_<语言>.ts`（GLOB 自动接上），代码零改动。
file(GLOB IB_TS_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/i18n/*.ts")
if(IB_TS_FILES)
    qt_add_lrelease(photoset-reader
        TS_FILES ${IB_TS_FILES}
        QM_FILES_OUTPUT_VARIABLE IB_QM_FILES)
    foreach(qm IN LISTS IB_QM_FILES)
        get_filename_component(qm_name "${qm}" NAME)
        set_source_files_properties("${qm}" PROPERTIES QT_RESOURCE_ALIAS "${qm_name}")
    endforeach()
    qt_add_resources(photoset-reader "i18n"
        PREFIX "/i18n"
        FILES ${IB_QM_FILES})
else()
    message(STATUS "i18n：i18n/ 下没有 .ts ⇒ 只有英文（源串）")
endif()

# ══ QML 模块 + 图标字体资源：挂在**每个要加载 `qrc:/qt/qml/AppUi` 的可执行**上 ══
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
        src/ui/components/PasswordDialog.qml
        src/ui/components/TimelinePreview.qml
        src/ui/components/SegmentedControl.qml
        src/ui/components/Breadcrumb.qml
        src/ui/components/Badge.qml
        src/ui/components/Toast.qml
        src/ui/components/Spinner.qml
        src/ui/components/Ring.qml
        src/ui/components/Switch.qml
        src/ui/components/NumberField.qml
        src/ui/components/SettingsDialog.qml
        src/ui/components/SettingsRow.qml
        src/ui/components/SettingsKeyRow.qml
        src/ui/components/SettingsGeneralPage.qml
        src/ui/components/SettingsImagePage.qml
        src/ui/components/SettingsSystemPage.qml
        src/ui/components/SettingsShortcutsPage.qml
        src/ui/fullscreen/FullscreenView.qml
        src/ui/fullscreen/FullscreenTopBar.qml
        src/ui/fullscreen/FullscreenBottomBar.qml
        src/ui/fullscreen/FullscreenTimeline.qml
        src/ui/fullscreen/FullscreenOsd.qml
        src/ui/fullscreen/NavBtn.qml
        src/ui/fullscreen/FavoritesPopup.qml
    )
    #   ⚠️ QML 源一律按**绝对路径**进 `QML_FILES`（本函数可能在 `tests/` 里被调用）；
    #      绝对路径**必须**配 `QT_RESOURCE_ALIAS`（相对源码根），否则 Qt 直接 FATAL_ERROR，
    #      资源树也会变成绝对路径。别名保持不变：`src/ui/...`（模块根由 `qt_add_qml_module` 决定）。
    set(qml_files_abs)
    foreach(f IN LISTS qml_files)
        set_source_files_properties("${CMAKE_SOURCE_DIR}/${f}"
            PROPERTIES QT_RESOURCE_ALIAS "${f}")
        list(APPEND qml_files_abs "${CMAKE_SOURCE_DIR}/${f}")
    endforeach()
    #   ★ 额外文件（`ARGN`）：工具自己的页面，**只进工具那个目标**的模块（不进产品）。
    #     别名取文件名 ⇒ 落在模块根，类型名 = 文件名（如 `IconPicker`）。
    foreach(f IN LISTS ARGN)
        get_filename_component(_name "${f}" NAME)
        set_source_files_properties("${f}" PROPERTIES QT_RESOURCE_ALIAS "${_name}")
        list(APPEND qml_files_abs "${f}")
    endforeach()
    qt_add_qml_module(${target}
        URI AppUi
        VERSION 1.0
        OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${target}_qml"
        QML_FILES ${qml_files_abs}
    )
endfunction()

ib_add_qml_resources(photoset-reader)

# ══ 静态 Qt：插件必须**显式导入**（2026-11-02 体积实验）══════════════════════
#   静态构建下 Qt 不会动态加载插件：平台插件 / 图片格式插件 / QML 插件都得 import，
#   否则「窗口起不来」或「JPEG 打不开」。动态构建时下面这段完全不生效。
get_target_property(_qtcore_type Qt6::Core TYPE)
if(_qtcore_type STREQUAL "STATIC_LIBRARY")
    #   ★ `NO_DEFAULT` 是关键：不加它，qt_import_plugins 会**把全部插件都导入**（INCLUDE 只是追加）。
    #   平台插件 xcb + wayland 都留（纯 X11 会话 / Wayland 会话都能原生跑）。
    #   不用 `qt_import_qml_plugins`：它会把 Controls 的**所有风格**（Fusion / Material / Universal /
    #   Imagine / FluentWinUI3）和大量用不到的模块全链进来；这里只点名要用的几块。
    target_compile_definitions(photoset-reader PRIVATE PHOTOSET_STATIC_QT=1)
    qt_import_plugins(photoset-reader NO_DEFAULT
        INCLUDE_BY_TYPE platforms
            Qt::QOffscreenIntegrationPlugin
            Qt::QXcbIntegrationPlugin
            Qt::QWaylandIntegrationPlugin
        #   xcb 自己的 GL 集成也是插件：不导入就报 "neither GLX nor EGL are enabled"
        INCLUDE_BY_TYPE xcbglintegrations
            Qt::QXcbGlxIntegrationPlugin
            Qt::QXcbEglIntegrationPlugin
        INCLUDE_BY_TYPE imageformats
            Qt::QJpegPlugin Qt::QGifPlugin Qt::QIcoPlugin
        #   Wayland 的 shell 集成 / 图形集成也是**插件**：不导入就只会报
        #   "No shell integration named xdg-shell found"
        INCLUDE_BY_TYPE wayland-shell-integration
            Qt::QWaylandXdgShellIntegrationPlugin
            Qt::QWaylandWlShellIntegrationPlugin
        INCLUDE_BY_TYPE wayland-graphics-integration-client
            Qt::QWaylandEglClientBufferPlugin
        INCLUDE_BY_TYPE wayland-decoration-client
            Qt::QWaylandBradientDecorationPlugin
        #   静态版没有系统插件（plasma-integration）⇒ 用 Qt 自带的 xdg-desktop-portal 主题，
        #   文件对话框才会是**系统对话框**（见 main.cpp 里的 PHOTOSET_STATIC_QT 分支）
        INCLUDE_BY_TYPE platformthemes
            Qt::QXdgDesktopPortalThemePlugin
        INCLUDE
            Qt::qtquick2plugin
            Qt::qmlplugin
            Qt::modelsplugin
            Qt::workerscriptplugin
            Qt::quickwindowplugin
            Qt::qtquickcontrols2plugin
            Qt::qtquicktemplates2plugin
            Qt::qtquickcontrols2implplugin
            Qt::qtquickcontrols2basicstyleplugin
            Qt::qtquickcontrols2basicstyleimplplugin
            Qt::qquicklayoutsplugin
    )
endif()


# ══ 开发工具：`ib-iconpicker`（设计 14 §11.1）══════════════════════════════════
#   自带 `main()` 的独立可执行（**不是测试**：不进 `tests/`、不挂 `add_test`）。
#   ★ 放**子目录**定义：`qt_add_qml_module` 的 `qmake_<URI>.qrc` 生成在
#     `${CMAKE_CURRENT_BINARY_DIR}/.qt/rcc/` —— 与 `photoset-reader` 同目录会让两个
#     目标的 QML 模块共用同一个 qrc（qmldir 里 `typeinfo` 指向谁就看谁最后配置）。
#     子目录 = 独立二进制目录 = 每个目标一份真资源（与 `tests/` 同一手法）。
add_subdirectory(tools)


# ══ 测试：全部在 `tests/`（源 + 目标 + add_test 都在那儿）══════════════════════
#   `enable_testing()` 在上面（根）已调用；测试目标由 `tests/CMakeLists.txt` 定义。
#   `ibcore` / `testsupport` 两个**库**留在根（应用与测试共用同一份实现）。
add_subdirectory(tests)
