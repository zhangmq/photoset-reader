# photoset-reader

**开发中** —— 功能与架构都还在变，暂不写详细说明，等基本稳定后再补。

只做一件事：**离线看图**（漫画 / 连拍照片）。设计文档与调研在 `docs/`（本地保留，不进版本库）。

## 已知限制

**EXIF / 色彩空间**（OSD 信息面板里那几行）

- **有**：JPEG、PNG、WebP、TIFF、HEIC/HEIF、AVIF —— 标签 + 色彩空间。
- **暂时没有**：**CR3**（佳能 RAW）、**JP2**（JPEG 2000）、**JXL**（JPEG XL）。
  这三类也是"盒子"容器，但它们的元数据位置各不相同；**不为它们单独写容器解析，也不为此增加依赖**，
  等真有素材进来再按需补。
- 读不到时**整组不出现**（不显示 "—"，也不猜值），**不影响看图**。
- 没有 ICC profile 的文件，色彩空间一行显示 `sRGB（无 profile）`。

## 许可证

本项目按 **MIT** 发布，见 [LICENSE](LICENSE)。

运行时依赖 Qt、libarchive、libexif、libheif、libunrar 等第三方组件，各自的许可与义务
（含 LGPL 的动态链接说明、UnRAR 的声明原文）见 [THIRD-PARTY.md](THIRD-PARTY.md)。
