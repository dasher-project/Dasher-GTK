#!/usr/bin/env python3
"""
Dasher-GTK Launcher
===================

Cross-platform launcher that handles submodule checkout, dependency
checking, CMake configuration, and application startup for **Linux**,
**Windows**, and **macOS**.

Usage::

    python run.py                 # Configure, build if needed, then launch
    python run.py --build-only    # Configure and build, don't launch
    python run.py --tests         # Build and run the ctest suite
    python run.py --clean         # Delete the build directory first

What it does:

1. Detects the current platform (Linux / Windows / macOS).
2. Initialises the git submodules (``DasherCore``, ``Thirdparty/SDL``,
   ``rust-tts-wrapper``) if they are missing.
3. Checks for platform-specific system dependencies:
   - **All**: CMake, a C++17 compiler, and ``cargo`` for the bundled
     Rust TTS wrapper.
   - **Linux**: ``pkg-config`` plus the gtk4 / gtkmm-4.0 development
     packages, and ``glib-compile-resources`` for the GResource bundle.
     ``libspeechd-dev`` is optional; without it the build falls back to
     cloud-only TTS.
   - **macOS**: gtkmm4 from Homebrew.
   - **Windows**: a GTK prefix (``C:\\gtk`` by convention, matching CI).
4. Configures and builds with CMake into ``build/``.
5. Launches the binary **from inside** ``build/Dasher/``, which it must
   be, because the engine resolves ``Data/`` relative to the working
   directory.

See Also:
    - ``README.md`` - build prerequisites and runtime data layout.
    - ``CONTRIBUTING.md`` - code style, sanitizer builds, DCO sign-off.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent.resolve()

# Platform detection
IS_WINDOWS = sys.platform == "win32"
IS_LINUX = sys.platform.startswith("linux")
IS_MACOS = sys.platform == "darwin"

# Submodules the build cannot proceed without.
SUBMODULES = ("DasherCore", "Thirdparty/SDL", "rust-tts-wrapper")

# The OUTPUT_NAME rewrite to lowercase is UNIX AND NOT APPLE only, so macOS
# keeps the target's capitalised name and Windows adds the .exe suffix.
BINARY_NAME = "dasher" if IS_LINUX else ("Dasher.exe" if IS_WINDOWS else "Dasher")


def check_python_version():
    """Check if Python version is compatible (3.9+)."""
    if sys.version_info < (3, 9):
        print("ERROR: Python 3.9 or higher is required")
        print(f"Current version: {sys.version}")
        return False
    return True


def _have_pkg(name):
    """True if pkg-config knows about a module."""
    if not shutil.which("pkg-config"):
        return False
    return subprocess.run(
        ["pkg-config", "--exists", name], capture_output=True
    ).returncode == 0


def check_submodules(sync=False):
    """
    Initialise git submodules only if they have never been checked out.

    A plain ``git clone`` without ``--recurse-submodules`` leaves these
    directories empty, and CMake then fails partway through configuration
    with an error that doesn't mention submodules at all. Checking up
    front turns that into one obvious step.

    Uses ``git submodule status`` rather than probing for a marker file:
    the submodules don't share a layout (rust-tts-wrapper is a Cargo
    project, not a CMake one), and git already reports exactly what we
    need. A leading ``-`` means uninitialised; a leading ``+`` means
    checked out at a commit other than the one the index records.

    Only ``-`` entries are fetched. A ``+`` entry is left alone on
    purpose: that is somebody testing against a different engine revision,
    and silently resetting it would throw away their work.

    Returns:
        True if every submodule is populated (or was successfully fetched).
    """
    if not (SCRIPT_DIR / ".git").exists():
        empty = [s for s in SUBMODULES if not any((SCRIPT_DIR / s).glob("*"))]
        if empty:
            print(f"ERROR: submodules not populated ({', '.join(empty)}) "
                  "and this is not a git checkout")
            return False
        return True

    status = subprocess.run(["git", "submodule", "status"], cwd=str(SCRIPT_DIR),
                            capture_output=True, text=True)
    if status.returncode != 0:
        print("ERROR: could not read submodule status")
        return False

    uninitialised = []
    pinned_elsewhere = []
    for line in status.stdout.splitlines():
        if not line:
            continue
        path = line[1:].split()[1] if len(line.split()) > 1 else ""
        if line.startswith("-"):
            uninitialised.append(path)
        elif line.startswith("+"):
            pinned_elsewhere.append(path)

    if pinned_elsewhere and not sync:
        for path in pinned_elsewhere:
            print(f"  NOTE: {path} is checked out at a different commit than the index")
            print("        records, so it is left as-is. If the build then fails on a")
            print("        missing dasher_* symbol, that is the cause: the frontend is")
            print("        calling an engine API this checkout predates. Re-pin it to")
            print("        whichever revision you meant to test, or take the index's:")
            print("          python run.py --sync-submodules")

    to_fetch = uninitialised + (pinned_elsewhere if sync else [])
    if not to_fetch:
        return True

    verb = "Syncing" if sync and pinned_elsewhere else "Fetching"
    print(f"{verb} submodules ({', '.join(to_fetch)})...")
    r = subprocess.run(
        ["git", "submodule", "update", "--init", "--recursive", *to_fetch],
        cwd=str(SCRIPT_DIR),
    )
    if r.returncode != 0:
        print("ERROR: git submodule update failed")
        return False
    print("Submodules ready.")
    return True


def check_system_deps():
    """
    Check for platform-specific system-level dependencies.

    - **All**: CMake and cargo. The Rust TTS wrapper is built from source
      as part of the CMake run, so a missing toolchain fails the build
      rather than degrading gracefully.
    - **Linux**: gtk4 / gtkmm-4.0 development packages via pkg-config, and
      glib-compile-resources (UIStyle.css is compiled into a GResource).
      speech-dispatcher headers are optional.
    - **macOS**: gtkmm4, normally from Homebrew.
    - **Windows**: a GTK prefix; CI uses C:\\gtk.

    Returns:
        Tuple of (fatal errors, warnings). Warnings do not stop the build.
    """
    errors = []
    warnings = []

    if not shutil.which("cmake"):
        errors.append(
            "  ERROR: cmake not found.\n"
            "  Install with:\n"
            "    sudo apt install cmake        # Linux\n"
            "    brew install cmake            # macOS"
        )
    if not shutil.which("cargo"):
        errors.append(
            "  ERROR: cargo not found.\n"
            "  The bundled rust-tts-wrapper is compiled from source and needs a\n"
            "  Rust toolchain on PATH. Install from https://rustup.rs"
        )

    if IS_LINUX:
        if not shutil.which("pkg-config"):
            errors.append(
                "  ERROR: pkg-config not found.\n"
                "  Install with:\n"
                "    sudo apt install pkg-config"
            )
        else:
            for mod, pkg in (("gtk4", "libgtk-4-dev"), ("gtkmm-4.0", "libgtkmm-4.0-dev")):
                if not _have_pkg(mod):
                    errors.append(
                        f"  ERROR: {mod} development files not found.\n"
                        f"  Install with:\n"
                        f"    sudo apt install {pkg}"
                    )
        if not shutil.which("glib-compile-resources"):
            errors.append(
                "  ERROR: glib-compile-resources not found.\n"
                "  UIStyle.css is compiled into a GResource at build time.\n"
                "  Install with:\n"
                "    sudo apt install libglib2.0-dev-bin"
            )
        # Optional. Without it the default TTS feature set won't build, so we
        # fall back to cloud-only rather than failing (see cmake_configure).
        if not _have_pkg("speech-dispatcher"):
            warnings.append(
                "  NOTE: speech-dispatcher headers not found.\n"
                "  Building cloud-only TTS (-DTTS_WRAPPER_FEATURES=cloud).\n"
                "  For system speech, install:\n"
                "    sudo apt install libspeechd-dev libclang-dev\n"
                "  and then re-run with --clean. The feature set is decided at\n"
                "  configure time and cached, so installing it alongside an\n"
                "  existing build directory leaves that build cloud-only."
            )
    elif IS_MACOS:
        if not _have_pkg("gtkmm-4.0"):
            errors.append(
                "  ERROR: gtkmm-4.0 not found.\n"
                "  Install with:\n"
                "    brew install gtkmm4"
            )
    elif IS_WINDOWS:
        gtk_prefix = Path(os.environ.get("GTK_PREFIX", r"C:\gtk"))
        if not gtk_prefix.exists():
            warnings.append(
                f"  WARNING: no GTK prefix at {gtk_prefix}.\n"
                "  CMake will need to find gtk4/gtkmm-4.0 some other way.\n"
                "  Set GTK_PREFIX if yours lives elsewhere."
            )
    else:
        warnings.append(
            f"  WARNING: Unsupported platform ({sys.platform}). "
            "Dasher-GTK supports Linux, Windows, and macOS."
        )

    return errors, warnings


def wants_cloud_only_tts():
    """True when the default TTS feature set can't be built on this host."""
    return IS_LINUX and not _have_pkg("speech-dispatcher")


def _cache_vars(cache):
    """Pull the entries CMake stamps into a cache file that we care about."""
    out = {}
    try:
        for line in cache.read_text(errors="replace").splitlines():
            for key in ("CMAKE_HOME_DIRECTORY", "CMAKE_CACHEFILE_DIR",
                        "CMAKE_CONFIGURATION_TYPES"):
                if line.startswith(key + ":"):
                    out[key] = line.split("=", 1)[1].strip()
    except OSError:
        pass
    return out


def is_multi_config(build_dir):
    """
    True for a generator that picks the configuration at build time.

    Visual Studio, the default generator on Windows, is one: it ignores
    CMAKE_BUILD_TYPE at configure time, needs --config on the build line
    (without it you silently get Debug), and writes the binary into a
    per-configuration subdirectory. Ninja and Unix Makefiles do none of
    that. Only the multi-config generators list CMAKE_CONFIGURATION_TYPES
    in the cache, which makes it the cheap way to tell them apart.
    """
    types = _cache_vars(build_dir / "CMakeCache.txt").get("CMAKE_CONFIGURATION_TYPES")
    return bool(types)


def binary_path(build_dir, build_type):
    """
    Where the built executable lands.

    CMAKE_RUNTIME_OUTPUT_DIRECTORY is build/Dasher, but a multi-config
    generator appends the configuration to it, so on Windows the binary is
    build/Dasher/Release/Dasher.exe. The Data/ POST_BUILD copy is not
    per-configuration and always lands in build/Dasher, which is why the
    launch directory and the binary's directory are not the same there.
    """
    rundir = build_dir / "Dasher"
    if is_multi_config(build_dir):
        return rundir / build_type / BINARY_NAME
    return rundir / BINARY_NAME


def check_cache_matches(build_dir):
    """
    Reject a CMakeCache.txt that was created for a different tree.

    CMake bakes absolute paths into its cache. A build directory copied from
    another checkout (or a checkout that has since moved) still *has* a
    CMakeCache.txt, so an existence check says "already configured" and the
    build then dies with a VerifyGlobs.cmake error naming a path that no
    longer exists. Compare the stamped paths instead and say plainly what is
    wrong, rather than deleting somebody's build tree on a guess.

    Returns:
        True if the cache is absent or belongs to this tree.
    """
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return True

    v = _cache_vars(cache)
    home = v.get("CMAKE_HOME_DIRECTORY")
    cachedir = v.get("CMAKE_CACHEFILE_DIR")
    problems = []
    if home and Path(home) != SCRIPT_DIR:
        problems.append(f"    source dir: {home}\n            now: {SCRIPT_DIR}")
    if cachedir and Path(cachedir) != build_dir:
        problems.append(f"     build dir: {cachedir}\n            now: {build_dir}")
    if not problems:
        return True

    # If the tree it was configured for is gone, nothing can ever reuse this
    # directory, so clearing it costs nothing and saves a manual --clean. If
    # that tree still exists we leave well alone: two live checkouts sharing a
    # name is a situation where deleting the wrong build dir would hurt.
    stale_source_gone = bool(home) and not Path(home).exists()

    if stale_source_gone:
        print(f"NOTE: {build_dir} was configured for {home}, which no longer exists.")
        print("      Reconfiguring from scratch.")
        shutil.rmtree(build_dir, ignore_errors=True)
        return True

    print(f"ERROR: {cache} was configured for a different tree.")
    for p in problems:
        print(p)
    print()
    print("  CMake stores absolute paths, so this build directory cannot be reused here.")
    print(f"  That tree still exists, so this is not cleared automatically. Either:")
    print(f"    python run.py --clean --build-dir {build_dir.name}")
    print("  or build somewhere else:")
    print("    python run.py --build-dir build-local")
    return False


def cmake_configure(build_dir, build_type):
    """Run the CMake configure step if the build directory isn't set up yet."""
    if (build_dir / "CMakeCache.txt").exists():
        return True

    cmd = ["cmake", "-B", str(build_dir), f"-DCMAKE_BUILD_TYPE={build_type}"]
    if wants_cloud_only_tts():
        cmd.append("-DTTS_WRAPPER_FEATURES=cloud")

    print(f"Configuring ({build_type})...")
    if subprocess.run(cmd, cwd=str(SCRIPT_DIR)).returncode != 0:
        print("ERROR: CMake configuration failed")
        return False
    return True


def cmake_build(build_dir, jobs, build_type):
    """Build the default target."""
    print(f"Building with {jobs} job(s)...")
    cmd = ["cmake", "--build", str(build_dir), "-j", str(jobs)]
    if is_multi_config(build_dir):
        # A multi-config generator took no build type at configure time, and
        # defaults to Debug here if we don't say. Match what was asked for.
        cmd += ["--config", build_type]
    if subprocess.run(cmd, cwd=str(SCRIPT_DIR)).returncode != 0:
        print("ERROR: build failed")
        return False
    print("Build complete.")
    return True


def run_tests(build_dir, build_type):
    """Run the ctest suite."""
    cmd = ["ctest", "--test-dir", str(build_dir), "--output-on-failure"]
    if is_multi_config(build_dir):
        cmd += ["-C", build_type]
    return subprocess.run(cmd, cwd=str(SCRIPT_DIR)).returncode


def run_dasher(build_dir, build_type, extra_args):
    """
    Launch Dasher from inside build/Dasher.

    The working directory is load-bearing: DasherBridge is constructed with a
    relative data directory, so the engine resolves Data/ (alphabets, colour
    schemes, training text) against the CWD. Launched from anywhere else the
    app starts but every letter box is the same size, because the language
    model found no training data.
    """
    rundir = build_dir / "Dasher"
    binary = binary_path(build_dir, build_type)
    if not binary.exists():
        print(f"ERROR: {binary} not found. Try: python run.py --build-only")
        return 1

    try:
        return subprocess.run([str(binary), *extra_args], cwd=str(rundir)).returncode
    except Exception as e:
        print(f"ERROR: Failed to run Dasher: {e}")
        return 1


def main():
    """Main launcher function."""
    parser = argparse.ArgumentParser(
        description="Build and launch Dasher-GTK.",
        epilog="Anything after -- is passed through to the Dasher binary.",
    )
    parser.add_argument("--build-only", action="store_true",
                        help="configure and build, but don't launch")
    parser.add_argument("--tests", action="store_true",
                        help="build, then run the ctest suite instead of launching")
    parser.add_argument("--clean", action="store_true",
                        help="delete the build directory before configuring")
    parser.add_argument("--sync-submodules", action="store_true",
                        help="reset submodules to the commits the index records")
    parser.add_argument("--build-dir", default="build",
                        help="build directory (default: build)")
    parser.add_argument("--build-type", default="Release",
                        choices=("Debug", "Release", "RelWithDebInfo", "MinSizeRel"),
                        help="CMake build type (default: Release)")
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4,
                        help="parallel build jobs (default: all cores)")
    parser.add_argument("app_args", nargs=argparse.REMAINDER,
                        help="arguments forwarded to Dasher after --")
    args = parser.parse_args()

    if IS_WINDOWS:
        platform_name = "Windows"
    elif IS_LINUX:
        platform_name = "Linux"
    elif IS_MACOS:
        platform_name = "macOS"
    else:
        platform_name = sys.platform
    # Our print()s go through Python's buffer while cmake and the compiler
    # write straight to the same fd. When stdout is a pipe rather than a
    # terminal that buffer only flushes at exit, so in a captured log the
    # banner and the NOTEs land after the build output they introduce, which
    # is precisely the log somebody pastes when asking for help.
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except (AttributeError, OSError):
        pass

    print("=" * 50)
    print(f"  Dasher-GTK - Predictive Text Entry for {platform_name}")
    print("=" * 50)
    print()

    os.chdir(SCRIPT_DIR)

    if not check_python_version():
        return 1

    errors, warnings = check_system_deps()
    for w in warnings:
        print(w)
    for e in errors:
        print(e)
    if warnings or errors:
        print()
    if errors:
        return 1

    if not check_submodules(sync=args.sync_submodules):
        return 1

    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = SCRIPT_DIR / build_dir

    if args.clean and build_dir.exists():
        print(f"Removing {build_dir}...")
        shutil.rmtree(build_dir)

    if not check_cache_matches(build_dir):
        return 1
    if not cmake_configure(build_dir, args.build_type):
        return 1
    if not cmake_build(build_dir, args.jobs, args.build_type):
        return 1

    if args.tests:
        return run_tests(build_dir, args.build_type)

    if args.build_only:
        print(f"Binary: {binary_path(build_dir, args.build_type)}")
        return 0

    # argparse.REMAINDER keeps the "--" separator; drop it before forwarding.
    extra = [a for a in args.app_args if a != "--"]

    print("Starting Dasher...")
    print()

    try:
        return run_dasher(build_dir, args.build_type, extra)
    except KeyboardInterrupt:
        print("\nDasher closed.")
        return 0
    except Exception as e:
        print(f"\nERROR: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
