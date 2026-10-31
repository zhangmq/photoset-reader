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

## 已知限制

- 信息面板里的 EXIF / 色彩空间：JPEG、PNG、WebP、TIFF、AVIF 有；CR3、JP2、JXL 还没有（不影响看图）。
- HEIC/HEIF 不在支持之列：它的像素要 HEVC 解码器，而 HEVC 有专利，我们不随包分发解码器。
- 没有 ICC profile 的图，色彩空间显示 `sRGB（无 profile）`。

## 许可证

本项目按 **MIT** 发布，见 [LICENSE](LICENSE)。

运行时依赖的第三方组件与许可（含 LGPL 动态链接说明、UnRAR 声明）见 [THIRD-PARTY.md](THIRD-PARTY.md)。
