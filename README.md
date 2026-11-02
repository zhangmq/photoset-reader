# photoset-reader

[English](README.en.md)

**开发中**

只是另一个图片查看器，专注于**漫画**或**超大图集**的快速浏览。

## 功能

- **进度保存**：可使用内置收藏夹，或存为进度文件。
- **幻灯片**：间隔支持 500 ms 以下的设置，目前限制在 50 ms 以上（可能可以更快），可用于连拍镜头回放。
- **图像缩放算法**：目前支持 Nearest、Smooth (Qt)、Bicubic、Lanczos 2、Lanczos 3。
- **文件格式支持**：JPEG、PNG、GIF、BMP、WebP、TIFF、ICO、TGA、SVG、AVIF、JXL、QOI、PNM。
- **压缩文件支持**：ZIP/CBZ、RAR/CBR、7Z/CB7、TAR/CBT。

## 平台

- **Linux**：主要支持平台
- **Windows**：计划支持，尚未构建

## 构建（Linux）

依赖：CMake ≥ 3.21、C++20、Qt 6、libarchive、libexif、libheif、liblzma、zlib、libunrar。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
./build/photoset-reader -d <目录|压缩包|图片>
```

## 文档

详细使用说明在 **Wiki**：<https://github.com/zhangmq/photoset-reader/wiki>

- [安装](https://github.com/zhangmq/photoset-reader/wiki/安装) · [使用](https://github.com/zhangmq/photoset-reader/wiki/使用) · [快捷键](https://github.com/zhangmq/photoset-reader/wiki/快捷键)
- [进度与收藏夹](https://github.com/zhangmq/photoset-reader/wiki/进度与收藏夹) · [格式支持](https://github.com/zhangmq/photoset-reader/wiki/格式支持) · [设置说明](https://github.com/zhangmq/photoset-reader/wiki/设置说明)
- [常见问题](https://github.com/zhangmq/photoset-reader/wiki/常见问题) · [开发与构建](https://github.com/zhangmq/photoset-reader/wiki/开发与构建)

## 安装

> ⚠️ **预编译包尚未发布**（当前 `0.1.0` 是开发版）：AppImage 与"完整依赖版"要等正式发版。
> **现在只能用源码构建安装。**

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
2. 在 [CHANGELOG.md](CHANGELOG.md) 加一节；
3. `cmake --build build && ctest --test-dir build --output-on-failure`，
   再跑 `./scripts/update-translations.sh --check` 与 `./scripts/render-icons.sh --check`；
4. 打包：`./scripts/package-appimage.sh` 与 `./scripts/package-linux.sh`（产物文件名自动带版本）；
5. `git tag v x.y.z` 并推送。

## 已知限制

- 信息面板里的 EXIF / 色彩空间：JPEG、PNG、WebP、TIFF、AVIF 有；CR3、JP2、JXL 还没有（不影响看图）。
- HEIC/HEIF 不在支持之列：它的像素要 HEVC 解码器，而 HEVC 有专利，我们不随包分发解码器。
- 没有 ICC profile 的图，色彩空间显示 `sRGB（无 profile）`。

## 许可证

本项目按 **MIT** 发布，见 [LICENSE](LICENSE)。

运行时依赖的第三方组件与许可（含 LGPL 动态链接说明、UnRAR 声明）见 [THIRD-PARTY.md](THIRD-PARTY.md)。
