# Changelog

[中文](CHANGELOG.md)

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning follows
[Semantic Versioning](https://semver.org/) (`MAJOR.MINOR.PATCH`).

**The single source of the version number is `project(... VERSION x.y.z)` in `CMakeLists.txt`**:
`--version` prints it, and the packaged file names use it (see "Releasing" in the README).

## [Unreleased]

### Changed

- **Installing no longer sets up any automatic file association** (progress files included): the app keeps
  the ability to appear in the file manager's “Open with” list (`MimeType=` in the `.desktop` file plus a
  bundled MIME type), but choosing a default application is entirely up to the user.

### Fixed

- **Dropping a `.prsave` into the window decoded it as an image**: drops go through
  `UiModel::openPath()`, while the progress-file branch only existed on the command-line path — now
  `openPath()` routes progress files to `openProgress()`.

## [0.1.0-beta.1] — 2026-11-07

Second pre-release. Highlights since alpha: a working **Windows port**, reworked
**settings / progress / decode** pipelines, a **keyboard overhaul**, and a much smaller AppImage.

### Added

- **Windows build** (beta): portable folder (exe + DLL closure + icon), D3D11 by default; fixes the
  freeze where the whole app hung when opening the “Open file / Open folder” dialog (COM apartment
  initialisation order on the GUI thread).
- **Decode capability report**: Settings → Decode support lists the suffixes this machine can really
  decode together with the component behind each one (built-in Qt / `windowcodecs.dll` /
  `libgdk_pixbuf-2.0.so.0` / the external program's real path); one `[cap]` line per provider at startup.
- **Progress read-only mode**: a `.prsave` file may be written by only one window at a time (sidecar
  `QLockFile`); when another window holds it the app shows a warning-coloured “Progress read-only”
  badge in the top bar.
- **Software-rendering fallback**: without a usable GPU the app switches to `CpuScalerItem` (Qt smooth)
  and disables the other scaling algorithms in settings.
- **Two keyboard systems**: A = window-level shortcuts, focus-independent, dispatching straight into
  `UiModel` (including `F11` / `Alt+Enter` for fullscreen and `Menu` / `Shift+F10` opening the context
  menu **at the window centre**); B = standard focus navigation inside panels (Tab / ↑↓ / Enter / Esc),
  including **submenus** opening with `Enter`/`→`, navigable with `↑↓`, leaving with `←`.
- The shortcut list is now a **single source of truth** (`ShortcutCatalog`): the settings page table
  and the implementation cannot drift apart any more.

### Changed

- **Settings use a draft model**: edits apply and persist only on “OK”; “Discard” writes nothing and
  “Restore defaults” only resets the form. Default slideshow interval 300 → **1000 ms**.
- **Slideshow accelerate/decelerate use a symmetric gear grid** (97 steps, exact inverses); `[`/`]` and
  the bottom-bar ± share the same entry point (previously `[`/`]` modified the global default, so they
  appeared to do nothing once a source was open).
- **Panel classification unified**: `ModalDialog` (settings / about / confirm / password) and
  `PopupPanel` (menus / favorites) bases, all with `popupType: Popup.Item` (no more separate surfaces).
- The AppImage is built against **static Qt** (from the separate `photoset-reader-qt` repository) with a
  trimmed dependency set: 37.3 MB → **22.2 MB**.
- The Windows package **drops** the DirectX Shader Compiler (−15 MB), `qmltooling` and
  `qtuiotouchplugin`; requesting D3D12 without DXC downgrades to D3D11 with a log line.

### Fixed

- Windows: the “Open file / Open folder” dialog froze the whole application.
- Leaving fullscreen wiped the window manager's maximised state (`Window.Windowed` =
  `Qt::WindowNoState`).
- The context-menu submenu had **no keyboard navigation** (mouse only).
- Software rendering still offered unusable scaling algorithms (now disabled).
- The decode-support page displayed `N suffixes` where a component name belonged.

### Known limitations

- Under software rendering scaling is fixed to Qt's smooth path (measured 67–188 ms per screen for
  CPU Bicubic/Lanczos, i.e. 5–15 fps while dragging).
- HEIC/HEIF still requires an HEVC decoder, which is not shipped; D3D12 requires an external DXC.

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

### Added

- **`--log-file <file>`**: logs are also appended to a file (timestamp + category + level).
  The Windows build is a GUI-subsystem binary with no console, and under Wine nothing is visible at
  all — this is how a bug report carries its logs.

### Fixed

- **GPU scaling failed on Windows**: the tap loop used `texture()` (implicit derivatives), which made
  HLSL fail with `X3511 unable to unroll loop` → pipeline creation failed → D3D device lost.
  Switched to `textureLod(..., 0.0)`.
- **Double-clicking on Windows opened a console window first**: the app target is now a GUI subsystem
  binary (`WIN32_EXECUTABLE`).

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
