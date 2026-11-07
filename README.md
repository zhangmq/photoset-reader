# photoset-reader

[English](README.en.md)

只是另一个图片查看器，专注于**漫画**或**超大图集**的快速浏览。缩放走自绘 GPU 管线
（`QQuickRhiItem` + 自写 shader），没有可用 GPU 时自动退回软件渲染。

## 功能

- **进度保存**：内置收藏夹，或存成 `.prsave` 进度文件（在文件管理器里双击即可恢复）。
  同一份进度文件**同时只允许一个窗口写**（`QLockFile`），抢不到就进入**进度只读**模式并在顶栏提示。
- **时间轴悬停预览**：鼠标停在进度条上，显示邻近页面缩略图；点缩略图直接跳转（可在设置里关）。
- **幻灯片**：帧驱动，间隔 0–60000 ms（`0` = 不设最小间隔，实际速度受解码与屏幕刷新率限制）。
  预设 100 ms / 0.5 / 1 / 2 / 3 / 4 / 5 s；`[` `]` 与底栏 ± 走同一张**档位网格**
  （97 档，加速与减速严格互逆）。
- **图像缩放算法**：Nearest、Smooth (Qt)、Bicubic、Lanczos 2、Lanczos 3（GPU shader 实现）。
  软件渲染降级时固定使用 Smooth (Qt)，设置里其它算法会被禁用。
- **解码能力自检**：设置 → 解码支持，列出本机实际可用的后缀、对应的解码组件
  （内置 Qt / Windows 编解码器 `windowscodecs.dll` / `libgdk_pixbuf-2.0.so.0` / 外部程序路径）。
- **文件格式支持**：JPEG、PNG、GIF、BMP、WebP、TIFF、ICO、TGA、SVG、AVIF、JXL、QOI、PNM
  等；Windows 上还有 WIC 覆盖的 RAW/HEIF 等（取决于系统装的编解码器）。
- **压缩文件支持**：ZIP/CBZ、RAR/CBR、7Z/CB7、TAR/CBT（solid 包不支持，因为无法随机读）。
  ZIP（ZipCrypto / AES-256）与 RAR 支持输入密码。
- **设置**：修改走**草稿**——只有按「确定」才应用并落盘，「放弃修改」什么都不写；
  「恢复默认」只重置表单。
- **日志级别**：设置 → 系统 → 日志级别（默认 `warning`），或用 `--log-level` 临时覆盖一次。

## 平台

| 平台 | 状态 |
|---|---|
| **Linux**（X11 / Wayland） | 主要平台；提供 AppImage（自带依赖）与安装树 |
| **Windows**（10 1809+） | 支持中：提供免安装目录（exe + DLL 闭包），默认 D3D11 渲染 |

## 构建

Linux 依赖：CMake ≥ 3.21、C++20、**Qt 6.9+**（含 Concurrent）、libarchive、libexif、liblzma、
zlib、libunrar。`libheif` **可选**（运行期 dlopen，只影响 HEIC/AVIF 的 Exif/ICC 元数据）。

一键装依赖（Arch / Debian·Ubuntu / Fedora / openSUSE 自动识别）：

```bash
./scripts/install-deps.sh --build
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
./build/photoset-reader <目录|压缩包|图片>   # 位置参数；launcher / 文件管理器也是这条
```

Windows 本地构建（依赖布局与已知坑见 `docs/windows/`，仓库内不随源码分发）：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\ci-windows.ps1 -Step all -DepsRoot C:\psr-deps -BuildDir C:\psr-build
```

命令行（两平台一致）：

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

## 快捷键

两套逻辑，互不干扰（与「设置 → 快捷键」同一份清单，单一事实源）：

**A. 浏览 / 播放控制**——窗口级快捷键，**只要窗口是活动窗口就生效**，不依赖焦点：

| 键 | 动作 |
|---|---|
| `←` `→` / `PageUp` `PageDown` | 上一页 / 下一页 |
| `↑` `↓` | 上一个 / 下一个文件夹 |
| `Space` | 播放 / 暂停幻灯片 |
| `I` | 显示 / 隐藏信息行 |
| `+` `=` / `-` | 放大 / 缩小 |
| `[` `]` | 幻灯片间隔 −/+ |
| `F11` / `Alt+Enter` | 切换全屏（Windows 没有系统级全屏快捷键，由应用实现） |
| `Esc` | 退出全屏 |
| `Menu` / `Shift+F10` | 打开右键菜单（**窗口中心**，与鼠标右键位置无关） |

**B. 面板**——右键菜单、设置、关于、确认框、密码框、收藏列表都是**标准焦点导航**：
`Tab`/`Shift+Tab` 移动、`↑`/`↓` 选择、`Enter` 确认、`Esc` 关闭；二级菜单用 `Enter`/`→` 进入。

## 安装

**预编译包**：每个 [Release](https://github.com/zhangmq/photoset-reader/releases) 都带下面三种产物，
文件名里的 `<版本>` 就是该 Release 的 tag（去掉 `v`）。这里刻意不写死版本号与体积——
都由 CI 在发版时生成，避免每次发版改 README。

| 形态 | 文件 | 用法 |
|---|---|---|
| **AppImage**（Linux，自带依赖） | `photoset-reader-<版本>-x86_64.AppImage` | `chmod +x` 后直接运行；需要 FUSE（没有就 `--appimage-extract-and-run`） |
| **安装树**（Linux，要求已装 Qt 6 与第三方库） | `photoset-reader-<版本>-linux-x86_64-install-tree.tar.gz` | 解到某个前缀即可：`tar -xzf … -C ~/.local`（`bin/` 与 `share/` 会就位） |
| **Windows 免安装** | `photoset-reader-<版本>-windows-x64.zip` | 解压后双击 `photoset-reader.exe`（DLL 已随包） |

Windows 包**不带** DirectX Shader Compiler（`dxcompiler.dll` / `dxil.dll`）：只有 D3D12
后端需要它，默认的 D3D11 用系统自带的 `d3dcompiler_47.dll`。显式请求 `QSG_RHI_BACKEND=d3d12`
而旁边没有 `dxcompiler.dll` 时，程序会自动降级到 d3d11 并记一条日志。

**安装不会替你修改任何默认打开方式**（连 `.prsave` 进度文件也不会）：本应用只会出现在文件管理器的
「打开方式」列表里（`.desktop` 声明了 `MimeType`），要不要设为默认由你在系统里自己选。

源码安装（开发用）：

```bash
git clone https://github.com/zhangmq/photoset-reader.git
cd photoset-reader
./scripts/install.sh                      # 默认装到 ~/.local（不需要 root），顺手装桌面集成
./scripts/install.sh --prefix /usr/local  # 系统级（需要 sudo）
```

脚本会：检查依赖 → Release 构建 → 跑测试 → `cmake --install`（可执行 / `.desktop` / 全套图标 /
许可）→ 装到用户目录时顺手做桌面集成。其它参数：`--deps-only`、`--install-deps`、`--no-tests`、
`--no-desktop`、`-j N`。

## 文档

用户手册在 **Wiki**：<https://github.com/zhangmq/photoset-reader/wiki>

- [安装](https://github.com/zhangmq/photoset-reader/wiki/安装) · [使用](https://github.com/zhangmq/photoset-reader/wiki/使用) · [快捷键](https://github.com/zhangmq/photoset-reader/wiki/快捷键)
- [进度与收藏夹](https://github.com/zhangmq/photoset-reader/wiki/进度与收藏夹) · [格式支持](https://github.com/zhangmq/photoset-reader/wiki/格式支持) · [设置说明](https://github.com/zhangmq/photoset-reader/wiki/设置说明)
- [常见问题](https://github.com/zhangmq/photoset-reader/wiki/常见问题) · [开发与构建](https://github.com/zhangmq/photoset-reader/wiki/开发与构建)

## 发布

1. 改 `CMakeLists.txt` 的 `project(photoset-reader VERSION x.y.z)` 与 `PHOTOSET_VERSION_SUFFIX`
   （**版本号只有这两处**，`--version` 与产物文件名都从这里来）；
   本地复用过构建目录时要显式覆盖：`cmake -S . -B build -DPHOTOSET_VERSION_SUFFIX=-<后缀>`
   （它是 `CACHE` 变量，改默认值不会覆盖已存在的缓存）；
2. 在 [CHANGELOG.md](CHANGELOG.md) 加一节（英文版 [CHANGELOG.en.md](CHANGELOG.en.md) 同步）；
3. `cmake --build build && ctest --test-dir build --output-on-failure`，
   再跑 `./scripts/update-translations.sh --check` 与 `./scripts/render-icons.sh --check`；
4. 合并到 `main`、打 tag 并推送：`git tag v<版本> && git push origin v<版本>` —— **产物由 CI 直接
   挂到 Release**（两个 workflow 的 `release` job），本地不需要下载/上传；
5. 补双语说明（只有几 KB 文本）：`gh release edit v<版本> --title … --notes-file <中英说明>`。

`main` 分支已保护：后续改动一律走 PR。

## 已知限制

- 信息面板里的 EXIF / 色彩空间：JPEG、PNG、WebP、TIFF、AVIF 有；CR3、JP2、JXL 还没有（不影响看图）。
- HEIC/HEIF 不在支持之列：它的像素要 HEVC 解码器，而 HEVC 有专利，我们不随包分发解码器。
- 没有 ICC profile 的图，色彩空间显示 `sRGB（无 profile）`。
- **软件渲染**（没有可用 GPU / 强制 `QT_QUICK_BACKEND=software`）下，缩放固定为 Qt 平滑：
  CPU 上的 Bicubic/Lanczos 实测单线程 67–188 ms 一屏，拖动时只有 5–15 fps，因此不启用。
- Windows 上 D3D12 后端需要 DirectX Shader Compiler，我们不随包分发 ⇒ 默认用 D3D11。

## 许可证

本项目按 **MIT** 发布，见 [LICENSE](LICENSE)。

第三方组件与许可见 [THIRD-PARTY.md](THIRD-PARTY.md)；GPL-3.0 / LGPL-3.0 全文见 [LICENSES/](LICENSES/)。
