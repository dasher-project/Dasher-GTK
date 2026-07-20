# Dasher for GTK

[![Build](https://github.com/dasher-project/Dasher-GTK/actions/workflows/cmake-multi-platform.yml/badge.svg)](https://github.com/dasher-project/Dasher-GTK/actions/workflows/cmake-multi-platform.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](./LICENSE)

Dasher is an information-efficient text-entry interface, driven by continuous
pointing gestures. It lets you write using eye gaze, a mouse, a switch, a
joystick, or touch — designed for accessibility and augmentative communication
(AAC).

This is the **GTK** frontend, built on the shared
[DasherCore](https://github.com/dasher-project/DasherCore) engine.

> **[dasher.at](https://dasher.at)** — downloads, user docs, and live demo
> **[Feature status](https://dasher.at/status/)** — what each platform supports
> **[All repos](https://github.com/dasher-project)** — engine, frontends, design guide

## Status

> **In development** — early-stage GTK4 frontend aiming to replace Dasher 5.
> See the [feature matrix](https://dasher.at/status/) for what's implemented.

## Usage

Runtime controls live in the footer bar, alongside the alphabet, speed, and the
Speech / Dwell / Keyboard toggles. The **Rate** switch (off by default) shows a
live typing-rate readout, e.g. `4.2 cps · 50 wpm` (characters per second and
words per minute). The rate is computed engine-side over a short rolling window
and refreshed about twice a second; flip the switch on to see it. See
[RFC 0012](https://github.com/dasher-project/governance/pull/19).

## Install

Prebuilt **Linux** packages are attached to each [Release](../../releases):

- **Flatpak** — needs the GNOME 50 runtime
  (`flatpak install flathub org.gnome.Platform//50`), then
  `flatpak install --user Dasher.flatpak` and
  `flatpak run org.alternativeinterface.dasher`.
- **AppImage** — `chmod +x Dasher-x86_64.AppImage && ./Dasher-x86_64.AppImage`
  (self-contained).

macOS and Windows aren't packaged yet — build from source (below). How the
artifacts are produced is described under [Packaging & releases](#packaging--releases).

## Optional runtime features

Two of the on-screen toggles depend on an external service and appear **greyed
out** until that service is available:

- **Speech** (spoken feedback and read-aloud) needs a working text-to-speech
  engine. A from-source Linux build enables the `system` feature, which drives a
  running `speech-dispatcher` (`sudo apt install speech-dispatcher`). The Flatpak
  and AppImage ship `cloud`-only, so configure a cloud engine under
  **Preferences → Speech**. With no engine available the Speech toggle stays
  disabled. See [TTS Support](#tts-support) for how the feature set is chosen at
  build time.
- **Keyboard mode** (types Dasher's output into other applications) uses
  [`ydotool`](https://github.com/ReimuNotMoe/ydotool). Install it and run the
  `ydotoold` daemon (it needs access to `/dev/uinput`); on Debian/Ubuntu that is
  `sudo apt install ydotool`. Without it the Keyboard toggle, on the top toolbar,
  is disabled.

**Dwell to click** (hover in place to click, under **Preferences → Input**) needs
no external dependency.

## Build

### Prerequisites

| Platform | Command |
|----------|---------|
| macOS | `brew install gtk4 gtkmm4 pkg-config cmake` |
| Linux (Debian/Ubuntu) | `apt-get install build-essential libgtk-4-dev libgtkmm-4.0-dev git cmake pkg-config libspeechd-dev libclang-dev speech-dispatcher ydotool` |
| Windows | Install GTK from [GVSBuild](https://github.com/wingtk/gvsbuild/releases) to `C:\gtk`, add `C:\gtk\bin` to PATH. Requires CMake, Git, and MSVC or Clang. Use an optimized release build for binary compatibility. |

All platforms additionally require a **Rust toolchain** (`cargo`) to build the bundled
`rust-tts-wrapper`. Install it from [rustup.rs](https://rustup.rs). On Linux the
`system` TTS feature binds speech-dispatcher via bindgen, hence `libspeechd-dev`
(headers) and `libclang-dev` (for bindgen) above. The `speech-dispatcher` and
`ydotool` packages are runtime dependencies for the optional Speech and
Keyboard-mode features (see [Optional runtime features](#optional-runtime-features));
the app still builds and runs without them, with those toggles disabled.

### Steps

```sh
git clone --recursive https://github.com/dasher-project/Dasher-GTK.git
cd Dasher-GTK
mkdir build && cd build
cmake ..
cmake --build . --config Release --parallel
```

The binary and all runtime files are placed in `build/Dasher/`.

### Running

Dasher must be launched from the `build/Dasher/` directory so it can find its
data files. The binary is `Dasher` on macOS/Windows and lowercase `dasher` on
Linux:

```sh
cd build/Dasher
./dasher      # 'Dasher' on macOS/Windows
```

### Running the Tests

Lightweight unit tests live in `tests/` and build alongside the app; doctest is
fetched automatically at configure time, so no extra dependency is required.
After configuring and building, run the suite with ctest:

```sh
ctest --test-dir build --output-on-failure
```

CI runs these tests on every push as part of the multi-platform workflow.

### TTS Support

The `rust-tts-wrapper` submodule provides text-to-speech support. It is included
automatically when cloning with `--recursive`. CMake builds and links it if the
submodule is present.

- **macOS**: builds with `avsynth,cloud` features (no local speech-dispatcher needed)
- **Linux**: builds with `system,cloud` features (uses speech-dispatcher + cloud engines); needs `libspeechd-dev` and `libclang-dev` (see Prerequisites)
- **Windows**: builds with `sapi,cloud` features (uses the Windows SAPI engine plus cloud engines)
- All platforms need a Rust toolchain (`cargo`) on `PATH` to compile the wrapper

Override the default feature set with `-DTTS_WRAPPER_FEATURES=...` at configure
time — e.g. `-DTTS_WRAPPER_FEATURES=cloud` for a cloud-only build with no
speech-dispatcher dependency (this is what the Flatpak uses).

### Runtime Data Files

The CMake build copies data files into `build/Dasher/Data/`. The directory layout
after building:

```
build/Dasher/
├── Dasher              # executable
├── libdasher.dylib     # macOS (libdasher.so on Linux, dasher.dll on Windows)
├── UIStyle.css
├── Data/
│   ├── alphabet.*.xml  # alphabet definitions
│   ├── color*.xml      # colour schemes
│   └── training*.txt   # language model training data (PPM)
├── Strings/
│   └── strings_*.json  # UI translations
└── Resources/
    └── License/
```

Dasher uses a PPM (Prediction by Partial Match) language model trained on text
files. Each alphabet definition specifies a `trainingFilename`
(e.g. `training_english_GB.txt`). Without training data, all letter boxes will be
the same size and prediction will not work. Training files are copied from
`DasherCore/Data/training/` during the build; if letters appear uniformly sized
after launch, run from `build/Dasher/` so the `"Data"` relative path resolves, and
rebuild if stale (`cmake --build build`).

### Known Issues

- `bad_variant_access` / `std::get: wrong index for variant` warnings on startup
  are non-fatal — the GTK UI queries some CAPI parameters with the wrong getter
  type (string vs long); they don't affect functionality. Tracked in
  [#17](../../issues/17).
- **Keyboard mode** and **system Speech** aren't available in the Flatpak and
  AppImage builds. Both need host-level access the self-contained, sandboxed
  formats can't provide: `/dev/uinput` (via `ydotoold`) for `ydotool` keyboard
  injection, and `libspeechd` plus a running `speech-dispatcher` for system TTS.
  The Flatpak is `cloud`-only for speech; Keyboard mode expects a from-source or
  native install. See [Optional runtime features](#optional-runtime-features).

## Packaging & releases

Linux is distributed as **Flatpak** and **AppImage**, both built by
[`.github/workflows/publish.yml`](.github/workflows/publish.yml).

**Flatpak** — manifest: `packaging/flatpak/org.alternativeinterface.dasher.yaml`
(GNOME 50 runtime + the `rust-stable` SDK extension; builds `rust-tts-wrapper`
with `TTS_WRAPPER_FEATURES=cloud`). The manifest bundles the working tree via a
`type: dir` source, so run `flatpak-builder` from **outside** the repo to avoid
copying its build directory into itself. `--install-deps-from=flathub` pulls the
runtime, SDK and `rust-stable` extension at the versions the manifest declares:

```sh
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
repo=$(pwd)
mkdir -p /tmp/dasher-fb && cd /tmp/dasher-fb
flatpak-builder --user --install --force-clean --install-deps-from=flathub build \
    "$repo/packaging/flatpak/org.alternativeinterface.dasher.yaml"
flatpak run org.alternativeinterface.dasher
```

**AppImage** — `bash packaging/build-appimage.sh` produces `Dasher-x86_64.AppImage`.

**Cutting a release** — push a `v*` tag; the publish workflow builds both
artifacts and attaches them to a new GitHub Release:

```sh
git tag v0.2.3 && git push origin v0.2.3
```

## Privacy & analytics

Dasher-GTK includes **opt-in** anonymous usage analytics and crash reporting to
help prioritise accessibility work. It is **off by default** — nothing is sent
until you turn it on under **Preferences → Privacy**. All Dasher frontends share
one self-hosted [PostHog](https://posthog.com) project; the complete event
schema is published in [`analytics-events.json`](./analytics-events.json).

- **Collected only after opt-in:** app launches, the input method / alphabet you
  select, which settings tab you open, and crash reports. Each event carries a
  random anonymous ID (resettable under Privacy) plus `platform`, `app_variant`,
  `app_version`, and `os_version`.
- **Never collected:** the text you type, clipboard contents, canvas contents,
  your name / email / account, training text, or game-mode targets.
- **Crash reports** capture the exception type, a stack trace, and the last
  lines of the engine log, with home-directory paths and email addresses
  scrubbed before anything leaves your device. A crash is written locally and
  only transmitted (as a PostHog `$exception`) on the next launch if you have
  opted in; otherwise it is discarded after 7 days.

Design details are in the org RFCs
[0001 (analytics)](https://github.com/dasher-project/governance/blob/main/rfcs/0001-analytics.md)
and [0009 (crash reporting)](https://github.com/dasher-project/governance/blob/main/rfcs/0009-crash-reporting.md).

## Architecture

This frontend consumes DasherCore through its **C API** (`src/Engine/DasherBridge.cpp`,
backed by `dasher.h`). `DasherBridge` owns the engine handle, feeds it GTK pointer
input, and receives draw commands that `RenderingCanvas` renders onto a GTK widget.
`InputManager`/`DwellClickHandler` translate raw input, and `TtsService` /
`DirectModeService` handle output and spoken feedback. The `Analytics` module
keeps a bounded engine-log ring buffer and installs crash handlers, powering the
opt-in analytics and crash reporting described under
[Privacy & analytics](#privacy--analytics).

```mermaid
flowchart LR
    Input["GTK pointer / keys"] --> InputMgmt["InputManager<br/>DwellClickHandler"]
    InputMgmt --> Bridge["DasherBridge"]
    Bridge <-->|"C API (dasher.h)"| Core[("DasherCore<br/>engine")]
    Core -.->|"draw commands"| Canvas["RenderingCanvas"]
    Bridge --> Output["TtsService<br/>DirectModeService"]
```

See [DasherCore's C API](https://github.com/dasher-project/DasherCore/blob/main/docs/C_API.md)
for the engine contract.

## Repository layout

| Path | Purpose |
|-------|---------|
| `src/Engine/` | `DasherBridge` + `CommandRenderer`: C API bridge to DasherCore |
| `src/Input/` | `InputManager`, `DwellClickHandler` (pointer/switch input) |
| `src/Output/` | `TtsService`, `DirectModeService` (speech + output modes) |
| `src/Preferences/` | Settings UI (`PreferencesWindow`, `SettingsSection`) |
| `src/UIComponents/` | Reusable GTK widgets (canvas, synced controls) |
| `src/Analytics/` | Opt-in analytics + crash reporting (PostHog, RFC 0001/0009) |
| `tests/` | doctest unit tests |
| `packaging/` | Flatpak manifest + AppImage build script (Linux distribution) |
| `DasherCore/` | DasherCore submodule (do not edit here — PR upstream) |
| `rust-tts-wrapper/` | TTS wrapper submodule |
| `Thirdparty/SDL` | SDL submodule (joystick input) |

## Contributing

See [CONTRIBUTING.md](./CONTRIBUTING.md) for build details, code style, and DCO
sign-off. For project-wide conventions (code of conduct, RFCs, security), see the
[org contributing guide](https://github.com/dasher-project/.github/blob/main/CONTRIBUTING.md).

Please file bug reports in the [issues](../../issues) of this repository. To join
the development group, send a pull request or reach us via
[Slack (OpenAAC)](https://join.slack.com/t/openaac/shared_invite/enQtNTQwNDgwODYyNjU5LTAwODNmZjM4ZmJmOTJkYTY2MWZkNjc0MDQ0NTcwMTRmMzY0MWI3OWJiNGYwZGIzMzc2YTk2N2FiY2JlYTI5Njc).

## License

MIT — see [LICENSE](./LICENSE).
