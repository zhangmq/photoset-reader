# 更新记录

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)；
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)（`MAJOR.MINOR.PATCH`）。

**版本号的唯一来源是 `CMakeLists.txt` 的 `project(... VERSION x.y.z)`**：
`--version` 打的是它，打包产物文件名也用它（见 README「发布」）。

## [0.1.0] — 未发布

首个可运行版本，功能面：

- **看图**：文件夹 / 压缩包（ZIP·CBZ、RAR·CBR、7Z·CB7、TAR·CBT）/ 单张图片；拖放打开；
  命令行 `-d` 打开；第二实例把路径转发给已运行实例。
- **阅读**：单幅 / 双幅版面、适屏/适宽/适高/原始缩放、滚轮缩放（以指针为锚点）、拖拽平移、
  缩放锁定；幻灯片（间隔可调，50 ms 起）、循环方式（不循环 / 全部循环 / 文件夹内循环）。
- **画质**：GPU 线性光缩放 + anti-ringing，算法 Nearest / Smooth (Qt) / Bicubic / Lanczos 2 / Lanczos 3；
  非 sRGB 的 ICC 一律按 profile 转到 sRGB 后上屏。
- **进度**：收藏夹（本地目录）与 `.prsave` 进度文件；自动保存（固定间隔 + 序列销毁前）。
- **信息面板（OSD）**：序号 / 文件 / 路径 / 分辨率 / 类型 / 尺寸 / 色彩空间 / EXIF。
- **界面**：深/浅主题、中/英双语（运行时可切）、设置对话框、快捷键一览、无来源时的拖放空态。
- **系统集成**：单实例、`Ctrl+Q` 退出、`SIGTERM` 优雅退出、幻灯片期间抑制屏幕休眠、
  系统原生文件对话框（XDG portal / KDE）。

已知限制见 [README「已知限制」](README.md#已知限制)。
