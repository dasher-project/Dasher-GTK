# Contributing to Dasher-GTK

Thank you for your interest in improving the GTK frontend for Dasher! This guide
covers the specifics of this repository. For project-wide conventions (code of
conduct, security, RFCs), see the
[organisation CONTRIBUTING](https://github.com/dasher-project/.github/blob/main/CONTRIBUTING.md).

## Quick start

```bash
git clone --recurse-submodules https://github.com/dasher-project/Dasher-GTK.git
cd Dasher-GTK
mkdir build && cd build
cmake ..
make -j$(nproc)
```

The binary and runtime data are placed in `build/Dasher/`. Launch from there:
`./Dasher/dasher` (the binary is lowercase `dasher` on Linux, `Dasher` on
macOS/Windows).

See the [build guide](https://dasher.at/developers/build-guides/gtk/) for
platform-specific dependencies (GTK4, gtkmm, pkg-config).

## What lives where

| Directory          | Purpose                                                        |
| :----------------- | :------------------------------------------------------------ |
| `src/`             | Frontend C++ source (GTK4/gtkmm) — the code you edit           |
| `src/Engine/`      | Bridge between GTK UI and DasherCore C API                    |
| `src/Input/`       | Input device handling (SDL3 joystick, dwell-click)             |
| `src/Output/`      | TTS (rust-tts-wrapper), direct mode                            |
| `src/Preferences/` | Settings UI                                                    |
| `src/UIComponents/` | Reusable GTK widgets (Synced* controls bound to CAPI params)  |
| `src/Analytics/`   | Opt-in analytics + crash reporting (PostHog; RFC 0001/0009)    |
| `packaging/`       | Flatpak manifest + AppImage build script (Linux)              |
| `DasherCore/`      | **Submodule** — the C++ engine (do not edit here; PR upstream) |
| `Thirdparty/SDL/`  | **Submodule** — SDL3 (joystick/haptic input only)             |
| `rust-tts-wrapper/` | **Submodule** — cross-platform TTS with C ABI                |
| `Resources/`       | UI style, licenses                                             |

## Code style

- **clang-format** (`.clang-format`) — run `clang-format -i src/**/*.cpp src/**/*.h`
  before committing. The config mirrors DasherCore's conventions (4-space indent,
  120-column limit, LLVM base style).
- **clang-tidy** (`.clang-tidy`) — bug-finding checks (bugprone-\*, cert-\*,
  clang-analyzer-\*, performance-\*). Run via
  `clang-tidy -p build/ src/your_file.cpp`.
- **.editorconfig** — enforces indentation and line endings in editors that
  support it.

## DasherCore changes

DasherCore is a git submodule pointing to
[dasher-project/DasherCore](https://github.com/dasher-project/DasherCore).
**Do not modify it inside this repo.** If you need an engine change, open a PR
against DasherCore directly, then bump the submodule pin here once merged.

## CI

Several workflows run on every PR:

- **`cmake-multi-platform.yml`** — builds on Ubuntu, Windows, and macOS across
  multiple compilers. These platform builds are the **required checks** that gate
  merging into `main`.
- **`dco.yml`** — checks that every commit is signed off (see Definition of Done).
- **`validate-metadata.yml`** — validates the desktop/AppStream metadata.
- **`publish.yml`** — builds the Flatpak + AppImage, and on a `v*` tag cuts a
  GitHub Release (see
  [Packaging & releases](./README.md#packaging--releases)).

`main` is protected — land changes via PR once the required platform builds are
green.

## Definition of Done

- [ ] clang-format clean (`clang-format --dry-run -Werror src/**/*.cpp src/**/*.h`)
- [ ] Builds on Linux (and ideally macOS/Windows)
- [ ] No new clang-tidy warnings
- [ ] Commits are signed off (DCO) — `git commit -s`
- [ ] If you changed a user-visible capability, update the
      [feature status matrix](https://dasher.at/status/) (`website` repo:
      `src/data/feature-status.json`) — the PR template has a checkbox for this
- [ ] If you changed UX/hardware interaction across platforms, check whether an
      [RFC](https://github.com/dasher-project/governance/tree/main/rfcs) is needed

## Pull request process

1. Fork and branch from `main`.
2. Ensure submodules are up to date (`git submodule update --init --recursive`).
3. Open a PR — the org-level PR template will prompt you on parity, RFCs, and
   the feature matrix.
