# ══ 测试可执行：每个套件一个（自带 main()；不再由 `image-browser` 用参数跳）════
#   设计 14 §11。跑完打印 `[tag] PASS/FAIL ...` + 小结，返回 0/1；
#   需要夹具的从 `argv` 取 `-d <目录>`（如 `ib-test-flow -d .testdata/vol01`）。
#   ★ T0 收尾批退出码约定：pass ⇒ 0 / fail ⇒ 1 / **skip ⇒ 77**
#     （整条套件跳过 = `testsupport::skip()`；ctest 侧对可能跳过的目标配
#      `SKIP_RETURN_CODE 77` ⇒ 显示 *Skipped* 而不是 *Passed*）。
#   `ibcore` / `testsupport` 两个库定义在根 `CMakeLists.txt`（应用与测试共用）——
#   这里只定义**测试目标**与 `add_test`。
#
#   测试相关文件一律在 `tests/`（不与应用源码混放）；`testsupport` 的包含路径由
#   它自己的 `target_include_directories(PUBLIC ...)` 带过来。
#
# ★ 二进制仍落在**构建根**（`./build/ib-test-*`）：目标定义挪到这层子目录后，
#   CMake 默认会把产物放进 `build/tests/` —— 验收脚本与 ctest 都按 `build/` 写。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ── T0 第三批：从 `main.cpp` 搬出来的非 QML 套件 ──────────────────────────
#   ★ T2c-2b-2：`ib-test-probe` 随 `libraryprobe` 删除 ⇒ 这一批现在 8 个。
qt_add_executable(ib-test-library
    selftest_library.cpp
)
target_link_libraries(ib-test-library PRIVATE testsupport)

qt_add_executable(ib-test-scan
    selftest_scan.cpp
)
target_link_libraries(ib-test-scan PRIVATE testsupport)

qt_add_executable(ib-test-volume
    selftest_volume.cpp
)
target_link_libraries(ib-test-volume PRIVATE testsupport)

qt_add_executable(ib-test-collection
    selftest_collection.cpp
)
target_link_libraries(ib-test-collection PRIVATE testsupport)

qt_add_executable(ib-test-cover
    selftest_cover.cpp
)
target_link_libraries(ib-test-cover PRIVATE testsupport)

# ⚠️ 旧 CLI 就叫 `--selftest`（核心正确性：缩放 / 光守恒 / 版面感知），与别的套件无关
qt_add_executable(ib-test-scaler
    selftest_scaler.cpp
)
target_link_libraries(ib-test-scaler PRIVATE testsupport)

# 需要素材目录：`ib-test-flow -d .testdata/vol01`
qt_add_executable(ib-test-flow
    selftest_flow.cpp
)
target_link_libraries(ib-test-flow PRIVATE testsupport)

# ── 重新写阅读调度那一批（viewermodel / fetchpipeline / seqwindow）时**整批摘掉**的套件 ──
#    `ib-test-viewseq` / `ib-test-vmcore` / `ib-test-cursor` / `ib-test-pipeline` /
#    `ib-test-pipeline-seq` / `ib-test-slideshow` / `ib-test-seqwindow` / `ib-test-uiwire` /
#    `ib-test-wiring` / `ib-bench-op-latency` —— 它们引用的 `fetch::Cache` / `Preloader` /
#    `Scheduler` / `Window` / `WindowBuilder` 与旧 `ViewerModel` 接口已整体作废。
#    源文件留在 `tests/` 下，但**不进构建**（见本批报告）。
#
# ── dev 工具收拾批：`--selftest-gpuscale` 补搬成第 17 个套件（设计 14 §11.1）──
#    实现本来就在 `src/gpuscalecheck.cpp`（`runGpuScaleCheck`），这里只做装配 + 调用。
#    ★ **需要真机显示**：offscreen 的 software 后端下 GPU 侧没有可用 RHI（能过就过，
#      不能过是环境限制，**不改断言**）；真机验证用 `DISPLAY=:0`。
#    需要素材目录：`ib-test-gpuscale -d .testdata/vol01`
qt_add_executable(ib-test-gpuscale
    selftest_gpuscale.cpp
)
target_link_libraries(ib-test-gpuscale PRIVATE testsupport)

# ── T2c-1：新增组件（`replaceItems` / `ResetQueue` / `LibraryCore`）的套件 ──────
#    设计 14 §9.1 / §9.3 / §9.5。**不需要 `-d` 素材**（用例自己造临时库 + 临时 PNG）。
qt_add_executable(ib-test-reset
    selftest_reset.cpp
)
target_link_libraries(ib-test-reset PRIVATE testsupport)

# ── T2c-2b-1：`UiModel` ↔ `LibraryCore` 接线（设计 14 §9.1 / §9.3 / §9.4）───────
#    不需要 `-d` 素材（用例自己造临时库 + 临时 PNG + FIFO）；**不需要** QML 资源
#    （只跑 C++ 的 UiModel / LibraryModel / LibraryCore）。
qt_add_executable(ib-test-uicore
    selftest_uicore.cpp
)
target_link_libraries(ib-test-uicore PRIVATE testsupport)

# ── ctest：相对 `.testdata` 的套件在**源码根**跑 ──────────────────────────
add_test(NAME ib-test-library COMMAND ib-test-library
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME ib-test-scan COMMAND ib-test-scan
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME ib-test-volume COMMAND ib-test-volume
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME ib-test-collection COMMAND ib-test-collection
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME ib-test-cover COMMAND ib-test-cover
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME ib-test-scaler COMMAND ib-test-scaler
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME ib-test-flow COMMAND ib-test-flow -d .testdata/vol01
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME ib-test-gpuscale COMMAND ib-test-gpuscale -d .testdata/vol01
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME ib-test-reset COMMAND ib-test-reset
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME ib-test-uicore COMMAND ib-test-uicore
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ── 可能**整条套件跳过**（夹具 / 环境不具备 ⇒ rc 77）：ctest 显示 skipped ──────
#   `cover` 缺 `.testdata/test.cbz`；`gpuscale` 建不出 GL 上下文（无 GPU 设备节点）。
#   其余套件的"跳过"只发生在**个别检查**上（打印 `[tag] 跳过：…`、不计入项数），
#   整条仍然有实打实的检查 ⇒ 照常 pass/fail，不设 77。
#   ★ `reset` 同理：⑤（库目录闸门）在**没有网络 / autofs 卷**的机器上跳过那一项，
#     整条照常 pass/fail ⇒ 这里也不设 77（加了会是死配置）。
set_tests_properties(
    ib-test-cover ib-test-gpuscale
    PROPERTIES SKIP_RETURN_CODE 77)
