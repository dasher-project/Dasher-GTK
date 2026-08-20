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
cmake --build . --config Release --parallel
```

The runtime data is placed in `build/Dasher/`, and the working directory has to
be that same directory when you start the app, or the engine resolves `Data/`
against the wrong place and every letter box comes up the same size:

```bash
cd Dasher          # i.e. build/Dasher, not build/
./dasher           # './Dasher' on macOS; '.\Release\Dasher.exe' on Windows
```

The binary is lowercase `dasher` on Linux, `Dasher` on macOS, and
`Release\Dasher.exe` on Windows, where the Visual Studio generator adds a
per-configuration subdirectory that the data files do not get.

Or `python run.py`, which does the same and launches the result. It checks the
system dependencies below, initialises submodules if you cloned without them,
and starts the binary from the directory it needs to run in. `--build-only`,
`--tests` and `--clean` do what they sound like. It is a convenience wrapper:
CMake remains the build system, and CI does not use it.

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
| `tests/`           | doctest unit tests, built and run via ctest                     |
| `packaging/`       | Flatpak manifest + AppImage build script (Linux)              |
| `DasherCore/`      | **Submodule** — the C++ engine (do not edit here; PR upstream) |
| `Thirdparty/SDL/`  | **Submodule** — SDL3 (joystick/haptic input only)             |
| `rust-tts-wrapper/` | **Submodule** — cross-platform TTS with C ABI                |
| `Resources/`       | UI style, licenses                                             |
| `InputFilterHierarchy.txt` | Reference notes: DasherCore's `CInputFilter` class tree and the parameters each filter reads |

## Code style

- **clang-format** (`.clang-format`) — CI checks only the lines your branch
  touches, across both `src` and `tests`, so format the same scope before
  pushing:

  ```bash
  git clang-format origin/main -- src tests
  ```

  The base is upstream's `main`, which is what CI compares against. The Quick
  start clone above makes `origin` upstream; if you cloned your fork instead,
  add the upstream remote once (`git remote add upstream
  https://github.com/dasher-project/Dasher-GTK.git`), `git fetch upstream`, and
  use `upstream/main` both here and in the Definition of Done.

  Use `git clang-format`, not plain `clang-format -i` over a glob: the glob form
  reformats whole files, which buries your change in unrelated churn, and
  `src/**/*.cpp` only recurses when the shell has `globstar` enabled (bash leaves
  it off by default, so it silently skips `src/main.cpp` and `src/MainWindow.*`).
  The config mirrors DasherCore's conventions (4-space indent, 120-column limit,
  LLVM base style).
- **clang-tidy** (`.clang-tidy`) — bug-finding checks (bugprone-\*, cert-\*,
  clang-analyzer-\*, performance-\*). Run via
  `clang-tidy -p build/ src/your_file.cpp`.
- **.editorconfig** — enforces indentation and line endings in editors that
  support it.

## Debugging crashes

Configure a sanitizer build in a separate directory so it doesn't clobber your
normal one:

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
      -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
      -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address"
cmake --build build-asan -j$(nproc)
```

**`src/Analytics/CrashReporter.cpp` installs its own SIGSEGV/SIGABRT/SIGILL
handlers, which will swallow the sanitizer's report.** Run with
`handle_segv=2` so ASan keeps its handler, or you will get a silent exit and a
`pending_crash.txt` instead of a stack trace:

```bash
cd build-asan/Dasher
ASAN_OPTIONS=detect_leaks=0:handle_segv=2 G_SLICE=always-malloc ./dasher
```

`G_SLICE=always-malloc` routes GLib allocations through malloc so ASan can see
them. Note that a faulting read inside uninstrumented GLib/GTK is reported as a
plain SEGV rather than a use-after-free with allocation stacks, so a clean
`AddressSanitizer can not provide additional info` does not rule out a lifetime
bug on our side.

Point the XDG directories somewhere disposable to keep test runs from touching
your real settings, analytics opt-in, or pending crash file:

```bash
XDG_DATA_HOME=/tmp/d/data XDG_CONFIG_HOME=/tmp/d/config XDG_CACHE_HOME=/tmp/d/cache ./dasher
```

Engine parameters are *not* stored under XDG — `DasherBridge` is constructed
with a relative data directory, so `dasher_settings.xml` is written to
`Data/` next to the binary you launched.

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

- [ ] clang-format clean on the lines you touched
      (`git clang-format --diff upstream/main -- src tests` reports nothing to change)
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
