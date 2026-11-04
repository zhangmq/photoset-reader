# photoset-reader

[English](README.en.md)

**开发中**

只是另一个图片查看器，专注于**漫画**或**超大图集**的快速浏览。

## 功能

- **进度保存**：内置收藏夹，或存成 `.prsave` 进度文件（在文件管理器里双击即可恢复）。
- **时间轴悬停预览**：鼠标停在进度条上，显示邻近页面缩略图；点缩略图直接跳转（可在设置里关）。
- **幻灯片**：间隔 0–60000 ms（`0` = 不设最小间隔，实际速度受解码与屏幕刷新率限制），
  预设 100 ms / 0.5 / 1 / 2 / 3 / 4 / 5 s，`[` `]` 按量级步进（10 / 50 / 500 / 1000 ms）。
- **图像缩放算法**：目前支持 Nearest、Smooth (Qt)、Bicubic、Lanczos 2、Lanczos 3。
- **文件格式支持**：JPEG、PNG、GIF、BMP、WebP、TIFF、ICO、TGA、SVG、AVIF、JXL、QOI、PNM。
- **压缩文件支持**：ZIP/CBZ、RAR/CBR、7Z/CB7、TAR/CBT（solid 包不支持，因为无法随机读）。
- **日志级别**：设置 → 系统 → 日志级别（默认 `warning`），或用 `--log-level` 临时覆盖一次。

## 平台

- **Linux**：主要支持平台
- **Windows**：计划支持，尚未构建

## 构建（Linux）

依赖：CMake ≥ 3.21、C++20、Qt 6（含 Concurrent）、libarchive、libexif、libheif、liblzma、zlib、libunrar。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
./build/photoset-reader <目录|压缩包|图片>   # 位置参数；launcher / 文件管理器也是这条
```

命令行：

```
photoset-reader [<路径>] [--fullscreen] [--log-level <级别>] [--log-file <文件>]
```

| 用法 | 含义 |
|---|---|
| `<路径>` | 打开文件夹 / 压缩包 / 图片 / `.prsave` 进度文件；不给 = 空窗口（可直接拖放） |
| `--fullscreen` | 启动即全屏 |
| `--log-level debug\|info\|warning\|error\|off` | 本次运行的日志级别（覆盖设置，不写盘） |
| `--log-file <文件>` | 日志**同时**追加写到这个文件（报 bug 用；Windows 下没有控制台也能带日志） |
| `-h` / `-v` | 帮助 / 版本 |

产品本体**不含任何测试/验收开关**（截图、探针、开发定时器都已移除）。

## 文档

详细使用说明在 **Wiki**：<https://github.com/zhangmq/photoset-reader/wiki>

- [安装](https://github.com/zhangmq/photoset-reader/wiki/安装) · [使用](https://github.com/zhangmq/photoset-reader/wiki/使用) · [快捷键](https://github.com/zhangmq/photoset-reader/wiki/快捷键)
- [进度与收藏夹](https://github.com/zhangmq/photoset-reader/wiki/进度与收藏夹) · [格式支持](https://github.com/zhangmq/photoset-reader/wiki/格式支持) · [设置说明](https://github.com/zhangmq/photoset-reader/wiki/设置说明)
- [常见问题](https://github.com/zhangmq/photoset-reader/wiki/常见问题) · [开发与构建](https://github.com/zhangmq/photoset-reader/wiki/开发与构建)

## 安装

**预编译包（alpha）**：从 [Releases](https://github.com/zhangmq/photoset-reader/releases) 下载，
两种形态二选一：

| 形态 | 文件 | 用法 |
|---|---|---|
| **AppImage**（自带依赖） | `photoset-reader-0.1.0-alpha.1-x86_64.AppImage` | `chmod +x` 后直接运行；需要 FUSE（没有就 `--appimage-extract-and-run`） |
| **系统依赖版**（要求系统已装 Qt 6 与第三方库） | `photoset-reader-0.1.0-alpha.1-linux-system-deps.tar.zst` | 解压后 `./install.sh`（默认装到 `~/.local`，`--prefix` 可改，`--uninstall` 卸载） |

系统依赖版里的 `install.sh` 会：检查运行期依赖（缺了按发行版给安装命令）→ 复制文件 →
把 `.desktop` 的 `Exec=` 改成绝对路径 → 重建桌面/ MIME 数据库 → 把 `.prsave` 设为默认交给本应用。

源码安装（开发用）：

```bash
git clone https://github.com/zhangmq/photoset-reader.git
cd photoset-reader
./scripts/install.sh                      # 默认装到 ~/.local（不需要 root），顺手装桌面集成
./scripts/install.sh --prefix /usr/local  # 系统级（需要 sudo）
```

脚本会：检查依赖（缺了就打印安装命令，见 `scripts/install-deps.sh`）→ Release 构建 → 跑测试 →
`cmake --install`（可执行 / `.desktop` / 全套图标 / 许可）→ 装到用户目录时顺手做桌面集成。
其它参数：`--deps-only`、`--install-deps`、`--no-tests`、`--no-desktop`、`-j N`。

手工等价步骤：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$HOME/.local"
```

依赖：CMake ≥ 3.21、C++20、Qt 6（Widgets / Quick / QuickControls2 / ShaderTools / DBus / LinguistTools）、
libarchive、libexif、libheif、liblzma、zlib、libunrar。

## 发布

1. 改 `CMakeLists.txt` 的 `project(photoset-reader VERSION x.y.z)` —— **版本号只有这一处**；
2. 在 [CHANGELOG.md](CHANGELOG.md) 加一节（英文版 [CHANGELOG.en.md](CHANGELOG.en.md) 同步）；
3. `cmake --build build && ctest --test-dir build --output-on-failure`，
   再跑 `./scripts/update-translations.sh --check` 与 `./scripts/render-icons.sh --check`；
4. 打包：`./scripts/package-appimage.sh` 与 `./scripts/package-linux.sh`（产物文件名自动带版本）；
5. 打 tag 并推送：`git tag v0.1.0-alpha.1 && git push origin main --tags`，然后
   `gh release create v0.1.0-alpha.1 dist/*.AppImage dist/*-system-deps.tar.zst --prerelease`。

## 已知限制

- 信息面板里的 EXIF / 色彩空间：JPEG、PNG、WebP、TIFF、AVIF 有；CR3、JP2、JXL 还没有（不影响看图）。
- HEIC/HEIF 不在支持之列：它的像素要 HEVC 解码器，而 HEVC 有专利，我们不随包分发解码器。
- 没有 ICC profile 的图，色彩空间显示 `sRGB（无 profile）`。

## 许可证

本项目按 **MIT** 发布，见 [LICENSE](LICENSE)。

第三方组件与许可见 [THIRD-PARTY.md](THIRD-PARTY.md)；GPL-3.0 / LGPL-3.0 全文见 [LICENSES/](LICENSES/)。
