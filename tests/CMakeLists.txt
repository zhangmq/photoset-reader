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

# ── ctest：相对 `.testdata` 的套件在**源码根**跑 ──────────────────────────
add_test(NAME ib-test-scan COMMAND ib-test-scan
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME ib-test-progress COMMAND ib-test-progress
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME ib-test-meta COMMAND ib-test-meta
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ── 可能**整条套件跳过**（夹具 / 环境不具备 ⇒ rc 77）：ctest 显示 *Skipped* ─────
#   ⚠️ 必须显式 `SKIP_RETURN_CODE 77`：不设的话 ctest 把 77 当**失败**。
#      全新克隆里没有 `.testdata/`（gitignore），`ib-test-meta` 会整条跳过 ——
#      没这条时 `scripts/install.sh`（新用户路径）会在 ctest 这步直接失败（实测）。
set_tests_properties(ib-test-scan ib-test-progress ib-test-meta
                     PROPERTIES SKIP_RETURN_CODE 77)
