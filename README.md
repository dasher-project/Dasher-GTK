# The Dasher Text Entry System ✨
Dasher is a zooming predictive text entry system, designed for situations
where keyboard input is impractical (for instance, accessibility or PDAs). It
is usable with highly limited amounts of physical input while still allowing
high rates of text entry.

## Dasher GTK 🖥️
Based on the DasherCore library this repository aims at implementing a fully featured new version of Dasher. Due to the usage of [GTK4](https://gtk.org/) as a frontend we strive to develop a multi-platform application that can be used regardless of the computing hardware. This project is still in its early stages of development and will need some time to be able to replace the Dasher 5 version.

## Build Instructions ⚙️🏗️

### Build Dependencies

| Platform | Command |
|----------|---------|
| macOS | `brew install gtk4 gtkmm4 pkg-config cmake` |
| Linux (Debian/Ubuntu) | `apt-get install build-essential libgtk-4-dev libgtkmm-4.0-dev git cmake pkg-config libspeechd-dev libclang-dev` |
| Windows | Install GTK from [GVSBuild](https://github.com/wingtk/gvsbuild/releases) to `C:\gtk`, add `C:\gtk\bin` to PATH. Requires CMake, Git, and MSVC or Clang. Use an optimized release build for binary compatibility. |

All platforms additionally require a **Rust toolchain** (`cargo`) to build the bundled
`rust-tts-wrapper`. Install it from [rustup.rs](https://rustup.rs). On Linux the
`system` TTS feature binds speech-dispatcher via bindgen, hence `libspeechd-dev`
(headers) and `libclang-dev` (for bindgen) above.

### Build Steps

```sh
git clone --recursive https://github.com/dasher-project/Dasher-GTK.git
cd Dasher-GTK
mkdir build && cd build
cmake ..
cmake --build . --config Release --parallel
```

The binary and all runtime files are placed in `build/Dasher/`.

### Running the Tests 🧪

Lightweight unit tests live in `tests/` and build alongside the app; doctest is
fetched automatically at configure time, so no extra dependency is required.
After configuring and building, run the suite with ctest:

```sh
ctest --test-dir build --output-on-failure
```

CI runs these tests on every push as part of the multi-platform workflow.

### Running

Dasher must be launched from the `build/Dasher/` directory so it can find its data files:

```sh
cd build/Dasher
./Dasher
```

### Runtime Data Files

The CMake build copies data files into `build/Dasher/Data/`. The directory layout after building:

```
build/Dasher/
├── Dasher              # executable
├── libdasher.dylib     # macOS (libdasher.so on Linux, dasher.dll on Windows)
├── UIStyle.css
├── Data/
│   ├── alphabet.*.xml  # alphabet definitions
│   ├── color*.xml      # colour schemes
│   ├── colour*.xml
│   └── training*.txt   # language model training data (PPM)
├── Strings/
│   └── strings_*.json  # UI translations
└── Resources/
    └── License/
```

#### Training Data and the PPM Language Model

Dasher uses a PPM (Prediction by Partial Match) language model trained on text files. Each alphabet definition specifies a `trainingFilename` (e.g. `training_english_GB.txt`). Without training data, all letter boxes will be the same size and prediction will not work.

Training files are copied from `DasherCore/Data/training/` during the build. If letters appear uniformly sized after launch:

1. Confirm training files exist: `ls build/Dasher/Data/training_*.txt`
2. Data files are copied automatically on each build. If stale, rebuild: `cmake --build build`
3. Ensure you are running from `build/Dasher/` so the `"Data"` relative path resolves correctly

#### TTS Support

The `rust-tts-wrapper` submodule provides text-to-speech support. It is included automatically when cloning with `--recursive`. CMake builds and links it if the submodule is present.

- **macOS**: builds with `avsynth,cloud` features (no local speech-dispatcher needed)
- **Linux**: builds with `system,cloud` features (uses speech-dispatcher + cloud engines); needs `libspeechd-dev` and `libclang-dev` (see Build Dependencies)
- **Windows**: builds with `sapi,cloud` features (uses the Windows SAPI engine plus cloud engines)
- All platforms need a Rust toolchain (`cargo`) on `PATH` to compile the wrapper

### Known Issues

- `bad_variant_access` warnings on startup are non-fatal. The GTK UI queries some CAPI parameters with the wrong getter type (string vs long); these do not affect functionality.

### Branches

- `main`: stable development
- `feature/v6-capi-migration`: CAPI-based GTK4 frontend (tracks the DasherCore `main` submodule, pinned at v0.1.5; the former `feature-CAPI` work has been merged into DasherCore `main`)

## License 📎

As this front-end is based on the DasherCore and we hope to attract some help from other developers and encurage everyone to extend the Dasher system, we licensed this front-end also under the MIT license.

## Support and Feedback 🗣️

Please file any bug reports in the issues of this repository. If you want to help and join the development group, either send us a pull request or get in contact using [Slack in the OpenAAC group](https://join.slack.com/t/openaac/shared_invite/enQtNTQwNDgwODYyNjU5LTAwODNmZjM4ZmJmOTJkYTY2MWZkNjc0MDQ0NTcwMTRmMzY0MWI3OWJiNGYwZGIzMzc2YTk2N2FiY2JlYTI5Njc).

You can find the Dasher website at https://dasher.at/. The source code is hosted on GitHub at:
https://github.com/dasher-project
