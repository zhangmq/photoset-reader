# photoset-reader

[中文](README.md)

**Under development**

Just another image viewer, focused on fast browsing of **manga** or **very large image sets**.

## Features

- **Progress saving**: built-in favorites, or a `.prsave` progress file (double-click it in your
  file manager to jump straight back).
- **Timeline hover preview**: hovering the progress bar shows thumbnails of the neighbouring pages;
  click one to jump there (can be turned off in settings).
- **Slideshow**: interval 0–60000 ms (`0` = no minimum delay; the real rate is limited by decoding
  and the screen refresh rate), presets 100 ms / 0.5 / 1 / 2 / 3 / 4 / 5 s; `[` `]` step by magnitude
  (10 / 50 / 500 / 1000 ms).
- **Image scaling algorithms**: currently Nearest, Smooth (Qt), Bicubic, Lanczos 2 and Lanczos 3.
- **Image formats**: JPEG, PNG, GIF, BMP, WebP, TIFF, ICO, TGA, SVG, AVIF, JXL, QOI, PNM.
- **Archive formats**: ZIP/CBZ, RAR/CBR, 7Z/CB7, TAR/CBT (solid archives are not supported: they
  cannot be read randomly).
- **Log level**: Settings → System → Log level (default `warning`), or `--log-level` for one run.

## Platforms

- **Linux**: primary platform
- **Windows**: planned, not built yet

## Build (Linux)

Requires CMake ≥ 3.21, C++20, Qt 6, libarchive, libexif, libheif, liblzma, zlib and libunrar.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
./build/photoset-reader <folder|archive|image>   # positional; launchers/file managers use this
```

Command line:

```
photoset-reader [<path>] [--fullscreen] [--log-level <level>]
```

| Usage | Meaning |
|---|---|
| `<path>` | Open a folder / archive / image / `.prsave` progress file; omit for an empty window (drops accepted) |
| `--fullscreen` | Start in fullscreen |
| `--log-level debug\|info\|warning\|error\|off` | Log level for this run (overrides the setting, not persisted) |
| `-h` / `-v` | Help / version |

The shipped binary contains **no test/acceptance switches** (screenshot, probes and dev timers were removed).

## Documentation

The user guide lives in the **wiki**: <https://github.com/zhangmq/photoset-reader/wiki> (Chinese).

## Install

**Prebuilt packages (alpha)**: grab one from [Releases](https://github.com/zhangmq/photoset-reader/releases):

| Form | File | How |
|---|---|---|
| **AppImage** (self-contained) | `photoset-reader-0.1.0-alpha.1-x86_64.AppImage` | `chmod +x`, then run it; needs FUSE (otherwise `--appimage-extract-and-run`) |
| **System-deps tarball** (needs Qt 6 + third-party libs installed) | `photoset-reader-0.1.0-alpha.1-linux-system-deps.tar.zst` | unpack, then `./install.sh` (defaults to `~/.local`; `--prefix` / `--uninstall`) |

The tarball's `install.sh` checks runtime dependencies (printing distro-specific install commands when
something is missing), copies the files, rewrites the `.desktop` `Exec=` to an absolute path, rebuilds
the desktop/MIME databases and makes `.prsave` files open with the app.

From source (for development):

```bash
git clone https://github.com/zhangmq/photoset-reader.git
cd photoset-reader
./scripts/install.sh                      # installs into ~/.local (no root), sets up desktop integration
./scripts/install.sh --prefix /usr/local  # system-wide (needs sudo)
```

The script checks dependencies (prints the install command when something is missing, see
`scripts/install-deps.sh`), builds Release, runs the tests, then `cmake --install`
(binary, `.desktop`, icon set, license files).

Manual equivalent:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$HOME/.local"
```

## Release

1. bump `project(photoset-reader VERSION x.y.z)` in `CMakeLists.txt` (the only place a version lives);
2. add an entry to [CHANGELOG.md](CHANGELOG.md) (keep [CHANGELOG.en.md](CHANGELOG.en.md) in sync);
3. `ctest`, `./scripts/update-translations.sh --check`, `./scripts/render-icons.sh --check`;
4. `./scripts/package-appimage.sh` and `./scripts/package-linux.sh` (output names carry the version);
5. tag and push.

## Known limitations

- EXIF / color space in the info panel: available for JPEG, PNG, WebP, TIFF and AVIF;
  not yet for CR3, JP2 or JXL (images still display fine).
- HEIC/HEIF is not supported: its pixels need an HEVC decoder, and HEVC is patent-encumbered,
  so no decoder is shipped with the app.
- Images without an ICC profile show `sRGB (no profile)` in the color space row.

## License

Released under the **MIT** license, see [LICENSE](LICENSE).

Third-party components and their licenses: [THIRD-PARTY.en.md](THIRD-PARTY.en.md).
Full GPL-3.0 / LGPL-3.0 texts: [LICENSES/](LICENSES/).
