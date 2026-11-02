# photoset-reader

[中文](README.md)

**Under development**

Just another image viewer, focused on fast browsing of **manga** or **very large image sets**.

## Features

- **Progress saving**: keep progress in the built-in favorites, or save it as a progress file.
- **Slideshow**: intervals below 500 ms are supported, currently clamped to 50 ms and up (it can
  probably go faster), useful for replaying burst sequences.
- **Image scaling algorithms**: currently Nearest, Smooth (Qt), Bicubic, Lanczos 2 and Lanczos 3.
- **Image formats**: JPEG, PNG, GIF, BMP, WebP, TIFF, ICO, TGA, SVG, AVIF, JXL, QOI, PNM.
- **Archive formats**: ZIP/CBZ, RAR/CBR, 7Z/CB7, TAR/CBT.

## Platforms

- **Linux**: primary platform
- **Windows**: planned, not built yet

## Build (Linux)

Requires CMake ≥ 3.21, C++20, Qt 6, libarchive, libexif, libheif, liblzma, zlib and libunrar.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
./build/photoset-reader -d <folder|archive|image>
```

## Documentation

The user guide lives in the **wiki**: <https://github.com/zhangmq/photoset-reader/wiki> (Chinese).

## Install

> ⚠️ **No prebuilt packages yet** (`0.1.0` is a development build). AppImage and the portable
> tarball will ship with the first release; for now build from source.

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
2. add an entry to [CHANGELOG.md](CHANGELOG.md);
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

Third-party components and their licenses (including notes on LGPL dynamic linking and the UnRAR
notice) are listed in [THIRD-PARTY.md](THIRD-PARTY.md).
