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

## 安装

| 形态 | 产物 | 说明 |
|---|---|---|
| **完整依赖版** | `dist/photoset-reader-<版本>-linux.tar.zst`（`scripts/package-linux.sh`） | 自带 Qt 与全部第三方库，解压即用（约 190 MB） |
| **AppImage** | `dist/photoset-reader-<版本>-x86_64.AppImage`（`scripts/package-appimage.sh`） | 单文件，双击即用（约 25 MB） |
| **系统依赖版** | 用下面的 `cmake --install` 自己装 | 最小（约 9 MB），依赖发行版自带的 Qt6 与解码库；依赖清单见 `scripts/install-deps.sh --check` |

从源码安装（`cmake --install` 装可执行、桌面入口、全套图标、许可）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
sudo cmake --install build --prefix /usr/local
```

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
