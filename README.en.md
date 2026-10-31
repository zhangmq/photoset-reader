# photoset-reader

[中文](README.md)

**Under development**

Just another image viewer, focused on fast browsing of **manga** or **very large image sets**.

## Features

- **Progress saving**: keep progress in the built-in favorites, or save it as a progress file.
- **Paging through large sets**: folders and archives open directly; with prefetch ready, turning a
  page does not wait.
- **Slideshow**: intervals below 500 ms are supported, currently clamped to 50 ms and up (it can
  probably go faster), useful for replaying burst sequences.
- **Image scaling algorithms**: currently Nearest, Smooth (Qt), Bicubic, Lanczos 2 and Lanczos 3.

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

## Known limitations

- EXIF / color space in the info panel: available for JPEG, PNG, WebP, TIFF, HEIC/HEIF and AVIF;
  not yet for CR3, JP2 or JXL (images still display fine).
- Images without an ICC profile show `sRGB (no profile)` in the color space row.

## License

Released under the **MIT** license, see [LICENSE](LICENSE).

Third-party components and their licenses (including notes on LGPL dynamic linking and the UnRAR
notice) are listed in [THIRD-PARTY.md](THIRD-PARTY.md).
