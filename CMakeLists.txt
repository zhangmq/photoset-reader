cmake_minimum_required(VERSION 3.21)
project(image-browser LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick Qml)
qt_standard_project_setup()


qt_add_executable(image-browser
    src/main.cpp
    src/imageprovider.cpp
    src/imageprovider.h
    src/scaler.cpp
    src/scaler.h
    src/archive.cpp
    src/archive.h
    src/natural.cpp
    src/natural.h
    src/sequence.cpp
    src/sequence.h
    src/layout.cpp
    src/layout.h
    src/settings.cpp
    src/settings.h
    src/settingsbridge.cpp
    src/settingsbridge.h
    src/viewmodel.cpp
    src/viewmodel.h
)

# QML 打进资源，避免运行时路径问题
qt_add_resources(image-browser "qmlres"
    PREFIX "/"
    BASE "src"
    FILES
        src/assets/fonts/materialdesignicons-webfont.ttf
)

# ── 新 UI（正规 QML 模块 + 单例令牌）─────────────────────────
# 旧 UI（src/qml/*）已于 2026-10-11 弃用并移入 .archive/，不再参与构建
set_source_files_properties(src/ui/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(image-browser
    URI ImageBrowser
    VERSION 1.0
    QML_FILES
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
        src/ui/browse/PlaceholderBrowse.qml
        src/ui/fullscreen/FullscreenView.qml
        src/ui/fullscreen/FullscreenTopBar.qml
        src/ui/fullscreen/FullscreenBottomBar.qml
        src/ui/fullscreen/FullscreenTimeline.qml
        src/ui/fullscreen/FullscreenOsd.qml
        src/ui/fullscreen/NavBtn.qml
        src/ui/library/EmptyState.qml
        src/ui/library/PlaceholderLibrary.qml
)


find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBARCHIVE REQUIRED libarchive)

# libunrar：只读 RAR 用途；无 pkg-config，手动找
find_library(UNRAR_LIBRARY NAMES unrar REQUIRED)
find_path(UNRAR_INCLUDE_DIR NAMES unrar/dll.hpp REQUIRED)

target_link_libraries(image-browser PRIVATE
    Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml
    ${LIBARCHIVE_LIBRARIES}
    ${UNRAR_LIBRARY})
target_include_directories(image-browser PRIVATE ${LIBARCHIVE_INCLUDE_DIRS} ${UNRAR_INCLUDE_DIR})
target_include_directories(image-browser PRIVATE ${LIBARCHIVE_INCLUDE_DIRS})

# 开发期便利：把 .testdata/vol01 作为默认目录传给程序（可用 -d 覆盖）
target_compile_definitions(image-browser PRIVATE
    DEFAULT_TEST_DIR="${CMAKE_SOURCE_DIR}/.testdata/vol01"
)
