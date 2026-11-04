#!/usr/bin/env bash
# 从源码构建并安装（**当前唯一的安装方式**：还没有发布预编译包）。
#
#   ./scripts/install.sh                 # 装到 ~/.local（不需要 root），顺手装桌面集成
#   ./scripts/install.sh --prefix /usr/local   # 系统级（需要 sudo，脚本会提示）
#   ./scripts/install.sh --deps-only     # 只检查/安装依赖，不构建
#
# 步骤：① 检查依赖（缺了就打印要执行的命令，见 scripts/install-deps.sh）
#       ② 配置 + Release 构建 + 跑测试
#       ③ cmake --install 到前缀（可执行 / .desktop / 图标 / 许可）
#       ④ 前缀在用户目录时顺手做桌面集成（.desktop + 图标 + 缓存刷新）
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

PREFIX=${PREFIX:-$HOME/.local}
BUILD_DIR=build
JOBS=$(nproc 2>/dev/null || echo 4)
DEPS_ONLY=0
RUN_TESTS=1
DESKTOP=1
INSTALL_DEPS=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)      PREFIX=$2; shift 2 ;;
        --build-dir)   BUILD_DIR=$2; shift 2 ;;
        --jobs|-j)     JOBS=$2; shift 2 ;;
        --deps-only)   DEPS_ONLY=1; shift ;;
        --install-deps) INSTALL_DEPS=1; shift ;;
        --no-tests)    RUN_TESTS=0; shift ;;
        --no-desktop)  DESKTOP=0; shift ;;
        -h|--help)     sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "未知参数：$1" >&2; exit 2 ;;
    esac
done

# ── ① 依赖 ────────────────────────────────────────────────────────────────
echo "== 依赖检查（构建依赖）=="
if ! ./scripts/install-deps.sh --check --build; then
    if [[ $INSTALL_DEPS -eq 1 ]]; then
        echo "== 安装依赖（可能要求输入 sudo 密码）=="
        ./scripts/install-deps.sh --build
    else
        cat <<MSG

缺少依赖。执行下面任一条，然后重跑本脚本：

    ./scripts/install-deps.sh --build     # 自动按发行版安装（需要 sudo）
    ./scripts/install.sh --install-deps   # 同上，装完继续构建

MSG
        exit 1
    fi
fi
[[ $DEPS_ONLY -eq 1 ]] && { echo "✓ 依赖就绪（--deps-only，到此为止）"; exit 0; }

# ── ② 构建 + 测试 ─────────────────────────────────────────────────────────
echo "== 配置 =="
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX"
echo "== 构建（-j$JOBS）=="
cmake --build "$BUILD_DIR" -j"$JOBS"

if [[ $RUN_TESTS -eq 1 ]]; then
    echo "== 测试 =="
    ctest --test-dir "$BUILD_DIR" --output-on-failure
fi

# ── ③ 安装 ────────────────────────────────────────────────────────────────
echo "== 安装到 $PREFIX =="
if ! cmake --install "$BUILD_DIR" >/dev/null 2>"$BUILD_DIR/install.err"; then
    cat "$BUILD_DIR/install.err" >&2
    echo
    echo "安装失败：$PREFIX 可能不可写。两种做法：" >&2
    echo "  sudo cmake --install $BUILD_DIR --prefix $PREFIX    # 系统级" >&2
    echo "  ./scripts/install.sh --prefix \$HOME/.local          # 用户级（不需要 root）" >&2
    exit 1
fi
echo "  可执行：  $PREFIX/bin/photoset-reader"
echo "  桌面入口：$PREFIX/share/applications/photoset-reader.desktop"
echo "  图标：    $PREFIX/share/icons/hicolor/{16..512}x…/apps/photoset-reader.png"
echo "  许可：    $PREFIX/share/doc/photoset-reader/{LICENSE,THIRD-PARTY.md,GPL-3.0-only.txt,LGPL-3.0-only.txt}"

# ── ④ 桌面集成（只在用户目录前缀时自动做；系统前缀由桌面环境自己扫）──────
if [[ $DESKTOP -eq 1 && "$PREFIX" == "$HOME"* ]]; then
    echo "== 桌面集成 =="
    #   `.prsave` 的自定义 MIME：装完必须重建 MIME 数据库，否则文件管理器不认识 *.prsave
    if command -v update-mime-database >/dev/null 2>&1; then
        update-mime-database "$PREFIX/share/mime" >/dev/null 2>&1 || true
        echo "  MIME 数据库已更新（*.prsave）"
    else
        echo "  ⚠️ 没有 update-mime-database（shared-mime-info）：*.prsave 可能不被文件管理器识别"
    fi
    ./scripts/install-desktop.sh --exec "$PREFIX/bin/photoset-reader" | sed 's/^/  /'
    #   把进度文件默认交给本应用（只在这个用户前缀下做；失败不致命）
    if command -v xdg-mime >/dev/null 2>&1; then
        xdg-mime default photoset-reader.desktop application/x-photoset-progress >/dev/null 2>&1 || true
    fi
fi

case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) echo; echo "提示：$PREFIX/bin 不在 PATH 里（把这行加进你的 shell 配置）："
       echo "  export PATH=\"$PREFIX/bin:\$PATH\"" ;;
esac
echo
echo "✓ 完成。启动：photoset-reader <文件夹|压缩包|图片>"
