# photoset-reader

[English](README.en.md)

**开发中**

只是另一个图片查看器，专注于**漫画**或**超大图集**的快速浏览。

## 功能

- **进度保存**：可使用内置收藏夹，或存为进度文件。
- **大图集翻页**：文件夹、压缩包打开就能翻，预取到位时翻页不用等。
- **幻灯片**：间隔支持 500 ms 以下的设置，目前限制在 50 ms 以上（可能可以更快），可用于连拍镜头回放。
- **图像缩放算法**：目前支持 Nearest、Smooth (Qt)、Bicubic、Lanczos 2、Lanczos 3。

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

- 信息面板里的 EXIF / 色彩空间：JPEG、PNG、WebP、TIFF、HEIC/HEIF、AVIF 有；
  CR3、JP2、JXL 还没有（不影响看图）。
- 没有 ICC profile 的图，色彩空间显示 `sRGB（无 profile）`。

## 许可证

本项目按 **MIT** 发布，见 [LICENSE](LICENSE)。

运行时依赖的第三方组件与许可（含 LGPL 动态链接说明、UnRAR 声明）见 [THIRD-PARTY.md](THIRD-PARTY.md)。
