# Changelog

[中文](CHANGELOG.md)

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning follows
[Semantic Versioning](https://semver.org/) (`MAJOR.MINOR.PATCH`).

**The single source of the version number is `project(... VERSION x.y.z)` in `CMakeLists.txt`**:
`--version` prints it, and the packaged file names use it (see "Releasing" in the README).

## [0.1.0-alpha.1] — 2026-11-04

First alpha (pre-release). Two forms: **AppImage** (self-contained) and a **system-deps tar package**
(shipped with `install.sh`).

### Added

- **Encrypted archives**: passwords for ZIP (ZipCrypto / AES-256) and RAR; one default password per
  source, per-container exceptions, the password is kept with the source and applied automatically when
  a progress file is opened.
- **Timeline hover preview**: hovering the progress bar shows thumbnails of neighbouring pages (current
  page centred, pinned to the edge near the ends, brand-coloured focus frame); clicking a thumbnail
  jumps there. 600 ms throttle on page changes; can be turned off in settings.
- **Friendlier progress files**: `.prsave` is registered as `application/x-photoset-progress`, so
  double-clicking it in a file manager restores the session; it opens **at the saved position right
  away** (position and layout are applied before the sequence is attached).
- **Log level**: Settings → System → Log level (`debug/info/warning/error/off`, default `warning`);
  `--log-level` overrides it for a single run.
- **CLI**: `--fullscreen` (start full screen).

### Changed

- **CLI narrowed down**: `photoset-reader [<path>] [--fullscreen] [--log-level <level>]` — the path is
  positional (no more `-d` / `--index`), and the shipped binary contains **no test or acceptance
  switches** (screenshot, probe, bench, development timers and the related environment variables are all
  gone).
- **Slideshow interval 0–60000 ms** (`0` = no minimum delay), presets 100 ms / 0.5–5 s, and `[` / `]`
  step by magnitude (10 / 50 / 500 / 1000 ms).
- **Desktop integration**: the `.desktop` `Exec=` uses an absolute path (launchers do not inherit the
  shell PATH); installation rebuilds the MIME database and makes `.prsave` open with this application.
- Defaults are now explicit: follow-system theme, follow-system language, fit-screen, single page, no
  loop, `lanczos3`, prefetch window 8, recursion depth 5, nested-archive limit 512 MB, preview strip on,
  log level warning.
- **Distribution compliance**: every package ships the full `GPL-3.0` / `LGPL-3.0` texts — Qt inside the
  AppImage is **statically linked**, and `THIRD-PARTY.md` now walks through LGPL §4(a)–(e) (notice,
  license texts, relinking path); the project itself remains **MIT**.

### Fixed

- Quitting while background decoding was running could race library static destruction: in-flight work
  is now drained before exit (bounded to 300 ms).
- Thumbnail batch decoding was serialized by one global lock (1–2 s for the first archive images) — the
  lock now covers only the genuinely shared part.
- Bad bytes read concurrently from an archive made Qt's RAW plugin segfault — bytes are magic-checked
  before being handed to Qt.
- Opening a progress file showed `1/total` before jumping to the target — it now starts at the saved
  position.
- With the pointer resting on the chrome during playback, the cursor and overlays are no longer
  auto-hidden.

## [Unreleased]

Staging area for upcoming changes (merged into the matching version when releasing).

When the first stable version ships, add a section under `Added / Changed / Fixed`, for example:

```markdown
## [0.1.0] — 2026-XX-XX

### Added
- …
### Changed
- …
### Fixed
- …
```
