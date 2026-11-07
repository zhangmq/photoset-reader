# photoset-reader

[中文](README.md) · **v0.1.0-beta.1**

Just another image viewer, focused on fast browsing of **manga** or **very large image sets**.
Scaling runs through a hand-written GPU pipeline (`QQuickRhiItem` + custom shaders) and falls back
to software rendering automatically when no usable GPU is available.

## Features

- **Progress saving**: built-in favorites, or a `.prsave` progress file (double-click it in your
  file manager to jump straight back). A progress file can be opened for **writing by one window at a
  time** (`QLockFile`); if another window holds it, the app goes **progress read-only** and says so in
  the top bar.
- **Timeline hover preview**: hovering the progress bar shows thumbnails of the neighbouring pages;
  click one to jump there (can be turned off in settings).
- **Slideshow**: frame-driven, interval 0–60000 ms (`0` = no minimum delay; the real rate is limited by
  decoding and the screen refresh rate). Presets 100 ms / 0.5 / 1 / 2 / 3 / 4 / 5 s; `[` `]` and the
  bottom-bar ± buttons share one **gear grid** (97 steps, accelerate/decelerate are exact inverses).
- **Scaling algorithms**: Nearest, Smooth (Qt), Bicubic, Lanczos 2, Lanczos 3 (GPU shaders).
  Under software rendering the app always uses Smooth (Qt) and disables the other choices.
- **Decode capability report**: Settings → Decode support lists the suffixes this machine can really
  decode and the component behind each one (built-in Qt / `windowscodecs.dll` / `libgdk_pixbuf-2.0.so.0`
  / the external program's path).
- **Image formats**: JPEG, PNG, GIF, BMP, WebP, TIFF, ICO, TGA, SVG, AVIF, JXL, QOI, PNM, plus whatever
  WIC covers on Windows (RAW/HEIF, depending on installed codecs).
- **Archive formats**: ZIP/CBZ, RAR/CBR, 7Z/CB7, TAR/CBT (solid archives are not supported: they cannot
  be read randomly). ZIP (ZipCrypto / AES-256) and RAR passwords are supported.
- **Settings**: edits go through a **draft** — only “OK” applies them and writes the file, “Discard”
  writes nothing, and “Restore defaults” only resets the form.
- **Log level**: Settings → System → Log level (default `warning`), or `--log-level` for one run.

## Platforms

| Platform | Status |
|---|---|
| **Linux** (X11 / Wayland) | primary; AppImage (self-contained) and a system-deps tarball |
| **Windows** (10 1809+) | beta: portable folder (exe + DLL closure), D3D11 rendering by default |

## Build

Linux requires CMake ≥ 3.21, C++20, **Qt 6.9+** (with Concurrent), libarchive, libexif, liblzma, zlib
and libunrar. `libheif` is **optional** (loaded at runtime; it only affects EXIF/ICC for HEIC/AVIF).

```bash
./scripts/install-deps.sh --build        # Arch / Debian·Ubuntu / Fedora / openSUSE

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
./build/photoset-reader <folder|archive|image>   # positional; launchers/file managers use this
```

Windows (local build script; dependency layout and known pitfalls live in `docs/windows/`):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\ci-windows.ps1 -Step all -DepsRoot C:\psr-deps -BuildDir C:\psr-build
```

Command line (same on both platforms):

```
photoset-reader [<path>] [--fullscreen] [--log-level <level>] [--log-file <file>]
```

| Usage | Meaning |
|---|---|
| `<path>` | Open a folder / archive / image / `.prsave` progress file; omit for an empty window (drops accepted) |
| `--fullscreen` | Start in fullscreen |
| `--log-level debug\|info\|warning\|error\|off` | Log level for this run (overrides the setting, not persisted) |
| `--log-file <file>` | Also append logs to this file (for bug reports; works without a console on Windows) |
| `-h` / `-v` | Help / version |

The shipped binary contains **no test/acceptance switches** (screenshot, probes and dev timers were removed).

## Shortcuts

Two independent systems, both driven by the same list the settings page shows:

**A. Browsing / playback** — window-level shortcuts, active whenever the window is active
(no focus required):

| Key | Action |
|---|---|
| `←` `→` / `PageUp` `PageDown` | Previous / next page |
| `↑` `↓` | Previous / next folder |
| `Space` | Play / pause slideshow |
| `I` | Show / hide the info row |
| `+` `=` / `-` | Zoom in / out |
| `[` `]` | Slideshow interval −/+ |
| `F11` / `Alt+Enter` | Toggle fullscreen (Windows has no system-wide fullscreen shortcut, so the app provides one) |
| `Esc` | Leave fullscreen |
| `Menu` / `Shift+F10` | Open the context menu (**window centre**, unrelated to the mouse position) |

**B. Panels** — context menu, settings, about, confirm, password and the favorites list use **standard
focus navigation**: `Tab`/`Shift+Tab` to move, `↑`/`↓` to choose, `Enter` to confirm, `Esc` to close;
submenus open with `Enter`/`→`.

## Install

**Prebuilt packages (beta)**: grab one from [Releases](https://github.com/zhangmq/photoset-reader/releases).

| Form | File | How |
|---|---|---|
| **AppImage** (Linux, self-contained) | `photoset-reader-0.1.0-beta.1-x86_64.AppImage` | `chmod +x`, then run it; needs FUSE (otherwise `--appimage-extract-and-run`) |
| **System-deps tarball** (Linux, needs Qt 6 + third-party libs) | `photoset-reader-0.1.0-beta.1-linux-system-deps.tar.zst` | unpack, then `./install.sh` (defaults to `~/.local`; `--prefix` / `--uninstall`) |
| **Windows portable** | `photoset-reader-0.1.0-beta.1-windows-x64.zip` | unpack and run `photoset-reader.exe` (DLLs included) |

The Windows package deliberately **omits** the DirectX Shader Compiler (`dxcompiler.dll` / `dxil.dll`,
~15 MB): only the D3D12 backend needs it, and the default D3D11 path uses the OS-provided
`d3dcompiler_47.dll`. If `QSG_RHI_BACKEND=d3d12` is requested while `dxcompiler.dll` is missing, the
app downgrades to D3D11 and logs it.

From source (for development):

```bash
git clone https://github.com/zhangmq/photoset-reader.git
cd photoset-reader
./scripts/install.sh                      # installs into ~/.local (no root), sets up desktop integration
./scripts/install.sh --prefix /usr/local  # system-wide (needs sudo)
```

## Documentation

The user guide lives in the **wiki**: <https://github.com/zhangmq/photoset-reader/wiki>
(Chinese; each page has an English counterpart, e.g. [Shortcuts](https://github.com/zhangmq/photoset-reader/wiki/Shortcuts)).

## Release

1. bump `project(photoset-reader VERSION x.y.z)` and `PHOTOSET_VERSION_SUFFIX` in `CMakeLists.txt`
   (the only places a version lives);
2. add an entry to [CHANGELOG.md](CHANGELOG.md) / [CHANGELOG.en.md](CHANGELOG.en.md);
3. `ctest`, `./scripts/update-translations.sh --check`, `./scripts/render-icons.sh --check`;
4. `./scripts/package-appimage.sh`, `./scripts/package-linux.sh`, and
   `scripts\ci-windows.ps1 -Step package` for Windows;
5. tag and push: `git tag v0.1.0-beta.1 && git push origin main --tags`, then
   `gh release create v0.1.0-beta.1 <assets…> --prerelease --notes-file <notes>`.

`main` is protected: further changes go through pull requests.

## Known limitations

- EXIF / color space in the info panel: available for JPEG, PNG, WebP, TIFF and AVIF; not yet for CR3,
  JP2 or JXL (images still display fine).
- HEIC/HEIF is not supported: its pixels need an HEVC decoder, and HEVC is patent-encumbered, so no
  decoder is shipped with the app.
- Images without an ICC profile show `sRGB (no profile)` in the color space row.
- Under **software rendering** (no usable GPU, or forced via `QT_QUICK_BACKEND=software`) scaling is
  fixed to Qt's smooth path: on the CPU, Bicubic/Lanczos measured 67–188 ms per screen single-threaded
  (5–15 fps while dragging), so they are not enabled there.
- On Windows the D3D12 backend needs the DirectX Shader Compiler, which is not shipped ⇒ D3D11 is the default.

## License

Released under the **MIT** license, see [LICENSE](LICENSE).

Third-party components and their licenses: [THIRD-PARTY.en.md](THIRD-PARTY.en.md).
Full GPL-3.0 / LGPL-3.0 texts: [LICENSES/](LICENSES/).
