# ══ 测试可执行：每个套件一个（自带 main()；不再由 `photoset-reader` 用参数跳）════
#   设计 14 §11。跑完打印 `[tag] PASS/FAIL ...` + 小结，返回 0/1；
#   需要夹具的从 `argv` 取 `-d <目录>`。
#   ★ T0 收尾批退出码约定：pass ⇒ 0 / fail ⇒ 1 / **skip ⇒ 77**
#     （整条套件跳过 = `testsupport::skip()`；ctest 侧对可能跳过的目标配
#      `SKIP_RETURN_CODE 77` ⇒ 显示 *Skipped* 而不是 *Passed*）。
#   `ibcore` / `testsupport` 两个库定义在根 `CMakeLists.txt`（应用与测试共用）——
#   这里只定义**测试目标**与 `add_test`。
#
#   ★ 2026-10-31：随「浏览 / 图集库」删除，7 个套件（browse / collection / cover /
#     library / reset / uicore / volume）连同它们测的代码一起删掉；现在只剩两个：
#       · `ib-test-scan`：`Sequence` 的目录枚举规则（**只驱动 Sequence**，不碰库）；
#       · `ib-test-meta`：EXIF / 色彩空间纯函数。

# ★ 二进制仍落在**构建根**（`./build/ib-test-*`）：目标定义挪到这层子目录后，
#   CMake 默认会把产物放进 `build/tests/` —— 验收脚本与 ctest 都按 `build/` 写。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ── 目录枚举规则（`Sequence`）：不需要库，自带临时目录夹具 ──────────────────
qt_add_executable(ib-test-scan
    selftest_scan.cpp
)
target_link_libraries(ib-test-scan PRIVATE testsupport)

# ── 进度文件（`.prsave`）读写（设计 20）：自带临时目录夹具 ────────────────
qt_add_executable(ib-test-progress
    selftest_progress.cpp
)
target_link_libraries(ib-test-progress PRIVATE testsupport)

# ── 元数据（EXIF / 色彩空间）纯函数（设计 17）：不需要临时库，只读 `.testdata` ──
qt_add_executable(ib-test-meta
    selftest_meta.cpp
)
target_link_libraries(ib-test-meta PRIVATE testsupport)

# ── 阅读屏（`ViewerModel`）：**0ms 幻灯片不许卡住**（2026-11-05 那个 bug 的回归套件）──
#   不需要素材：现造临时目录里的 PNG；`-d <目录>` 时用真图集跑同一件事（手工验收用）。
qt_add_executable(ib-test-viewer
    selftest_viewer.cpp
)
target_link_libraries(ib-test-viewer PRIVATE testsupport)

# ── 解码链（`decode::Capabilities` / `imageread` / `placeholder`）──
#   钉住三条**产品契约**：能力表自洽（列了就解得出、PDF 被策略排除）、
#   **上屏路径必须是 32-bit**（16-bit 输入黑屏的回归）、占位图几何（只缩不放 / 未知 ⇒ 方图）。
#   全部现造夹具，不依赖 `.testdata`。
qt_add_executable(ib-test-decode
    selftest_decode.cpp
)
target_link_libraries(ib-test-decode PRIVATE testsupport)

# ── 多实例契约（**两个进程必须能同时跑**）──
#   为什么需要（2026-11-06）：单实例转发是 `c78f59c` 里我自己加的（注释写的"用户选 A"
#   是编造的），后果是"带路径的第二次启动没有自己的窗口"。这类退化编译、别的套件、
#   CTest 全发现不了 —— 只有人点第二次才看得到 ⇒ 用两个真进程钉住。
#   ★ 套件自己启动 `photoset-reader`（同目录），并把设置指到临时目录。
qt_add_executable(ib-test-instance
    selftest_instance.cpp
)
target_link_libraries(ib-test-instance PRIVATE testsupport)

# ── UI 契约（QML 加载 + `HintBubble` 几何 / 弹出类型）──
#   为什么需要：解码/渲染有测试，**QML 一行覆盖都没有** ⇒ "某类型不存在"、
#   "tooltip 在缩放窗口里压住按钮并抢指针"这类事故只能靠人眼发现。
#   `ib_add_qml_resources()` 把整套 AppUi 模块挂到这个目标上（与 `ib-photoset-reader`
#   同一份 QML）⇒ 断言的是**真实 QML**，不是复制品。
#   ★ `QT_SCALE_FACTOR` 由两条 ctest 分别注入 1.0 / 1.35，同一二进制覆盖两种缩放。
qt_add_executable(ib-test-ui
    selftest_ui.cpp
)
target_link_libraries(ib-test-ui PRIVATE testsupport ibcore Qt6::Quick Qt6::Qml)
ib_add_qml_resources(ib-test-ui)

# ── ctest：相对 `.testdata` 的套件在**源码根**跑 ──────────────────────────
add_test(NAME ib-test-scan COMMAND ib-test-scan
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME ib-test-progress COMMAND ib-test-progress
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME ib-test-meta COMMAND ib-test-meta
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME ib-test-viewer COMMAND ib-test-viewer
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME ib-test-decode COMMAND ib-test-decode
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
#   多实例：套件自己起两个真进程（工作目录随意，靠 applicationDirPath 找产品）
add_test(NAME ib-test-instance COMMAND ib-test-instance
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
#   UI 契约跑两遍：缩放 1.0 与 1.35（后者是这台设备真实的条件，也是旧实现出错的地方）
add_test(NAME ib-test-ui COMMAND ib-test-ui
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME ib-test-ui-hidpi COMMAND ib-test-ui
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ── 可能**整条套件跳过**（夹具 / 环境不具备 ⇒ rc 77）：ctest 显示 *Skipped* ─────
#   ⚠️ 必须显式 `SKIP_RETURN_CODE 77`：不设的话 ctest 把 77 当**失败**。
#      全新克隆里没有 `.testdata/`（gitignore），`ib-test-meta` 会整条跳过 ——
#      没这条时 `scripts/install.sh`（新用户路径）会在 ctest 这步直接失败（实测）。
#
#   ★ 两个环境变量是 **Linux 无头环境**必须的（2026-11-06 实测，CachyOS / Plasma）：
#     · `QT_QPA_PLATFORM=offscreen`：套件会建 `QGuiApplication`，SSH 会话里没有
#       display ⇒ 四条套件全部 **Subprocess aborted**（不是断言失败，是启动就崩）；
#     · `QT_FORCE_STDERR_LOGGING=1`：Qt 6 在检测到 journald 时把日志**发给 journald**，
#       stderr 上一个字都没有 ⇒ 测试看起来"没有任何输出"，CI 里连 FAIL 都看不见。
#     两个变量在 Windows 上无害（offscreen 插件照样有、stderr 本来就是控制台）。
set_tests_properties(ib-test-scan ib-test-progress ib-test-meta ib-test-viewer ib-test-decode
                     ib-test-ui ib-test-ui-hidpi ib-test-instance
                     PROPERTIES SKIP_RETURN_CODE 77
                     ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1")
#   ★ `ENVIRONMENT` 是**整体替换**、不是追加（2026-11-06 实测：先设 offscreen 再单独
#     设 `QT_SCALE_FACTOR` ⇒ offscreen 被抹掉 ⇒ Linux 无头机上 `Subprocess aborted`）。
#     缩放那一遍必须把三个变量写全。
set_tests_properties(ib-test-ui-hidpi PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1;QT_SCALE_FACTOR=1.35")
