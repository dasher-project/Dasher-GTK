#!/usr/bin/env python3
"""Smoke tests for a built Dasher Flatpak.

Takes the build directory that flatpak-builder leaves behind (the one that
holds `metadata` and `files/`) and checks what a review of the manifest
cannot show: the permissions the build really ends up with, the files that
are (and are not) in it, that every library resolves inside the sandbox,
that the speech stack is the one built from source, that the app starts
and takes its bus name, that it can synthesize speech, and what
flatpak-builder-lint says.

    packaging/flatpak/tests/smoke-test.py BUILD_DIR
    packaging/flatpak/tests/smoke-test.py --list
    packaging/flatpak/tests/smoke-test.py BUILD_DIR --check permissions --check linkage
    packaging/flatpak/tests/smoke-test.py BUILD_DIR --host-checks --network

One line per check: PASS, FAIL or SKIP, then a summary. The exit status is
1 when a check failed, 0 otherwise. A check whose tool is missing is
skipped, visibly; `--strict` turns skips into failures (CI uses it, with
`--allow-skip` for the ones it cannot run).

WHAT A RUN TOUCHES

  By default: nothing outside the sandbox. Nothing is installed and the
  build directory is not written to. The script reads the build directory
  and the checkout, runs desktop-file-validate, appstreamcli (--no-net)
  and git on them, and starts `flatpak build --readonly` sandboxes.
  `flatpak build` on a finished build directory applies the finish-args
  of its metadata, which would hand the host's Wayland socket, devices
  and home directory to the app; every sandbox this script starts
  therefore switches all of those off again: no display, no session or
  system bus, no sound server, no devices, no network, no home
  directory. The app is started in such a sandbox on GTK's Broadway
  backend with a private session bus, both inside the sandbox, so no
  window appears anywhere and the session bus of the desktop is not
  touched. Everything that is written goes to a temporary directory: it
  is the sandboxes' /run/dasher-smoke, and it also stands in for
  /var/tmp and /var/lib, which `flatpak build` would otherwise leave
  writable in the build directory.
  One contact with the host is flatpak's own and cannot be switched off:
  where there is a systemd user instance, `flatpak build` asks it
  (/run/user/UID/systemd/private) for a scope to run the sandbox in.
  Nothing of the session reaches the sandbox that way.

  --host-checks adds the checks that have to reach outside:
    lint-manifest, lint-builddir
                    flatpak-builder-lint uses the network (it looks up the
                    app id's domain and the screenshot URL). When it comes
                    from the org.flatpak.Builder Flatpak it is started with
                    `flatpak run`, which uses the session bus and the
                    portals of the desktop as any Flatpak app does.
    audio-device    a sandbox gets the host's sound server socket
                    (PulseAudio or PipeWire); the default device is opened
                    and closed, nothing is played.
    input-devices   a sandbox gets the host's /dev; /dev/input is listed.

  --network adds speech-install: a sandbox with network access in which
  the app's own installer downloads a small pinned voice and the
  pronunciation data of its language. It downloads into a directory of
  its own next to the cached voice (see --voice-cache). Only a download
  that is complete (see voice_problems and lexicon_problems) and matches
  its pins takes the place of the cached voice, whole; until then, and
  whenever a download fails, the voice of an earlier run stays as it is.
  The synthesis check then finds a complete voice in the cache and
  runs offline, also on later runs. A download that fails, that does not
  finish in its time (it is stopped) or that leaves the pronunciation
  data incomplete is tried again; when that does not help the check is
  skipped with a warning and not failed, also under --strict (a release
  must not wait for a file server), and the synthesis uses the voice of
  an earlier run if there is one. A voice or pronunciation data with a
  wrong checksum fails.

  --installed APP_ID adds installed-permissions: the metadata of an
  installed app is read with `flatpak info`. The app is never run.

LIMITS

  - `persistent=` (--persist) has no negation in `flatpak build`: it
    creates the directory below ~/.var/app/APP_ID on the host and binds
    it into the sandbox. A build that declares one gets no sandbox from
    this script; the checks that need one are skipped and say why.
  - appstreamcli and desktop-file-validate are the host's. What
    appstreamcli reports as a warning has changed between releases
    (Ubuntu 24.04 has 1.0.x); the metainfo check names the version it
    ran, so a difference between two machines can be told from the
    output.
  - The app is stopped with SIGTERM, which it ignores today (see
    KNOWN_ISSUES), and then killed. It offers no other way to ask it to
    quit: it registers no GApplication actions, so org.gtk.Actions and
    org.freedesktop.Application have nothing to activate. A clean exit
    is therefore not tested.

WHAT TO EDIT WHEN SOMETHING CHANGES ON PURPOSE

  EXPECTED_FINISH_ARGS   a permission was added to or removed from the
                         manifest. Every change here is a change of what
                         the app may do on the user's machine: say why in
                         the commit message.
  LINT_EXPECTED_ERRORS   a lint error is accepted for now, or an accepted
                         one is fixed (the run tells you which).
  LOG_ALLOWLIST          the app logs a new warning at startup that is
                         harmless in the test environment.
  OUTPUT_ALLOWLIST       the app prints a new line at startup that is not
                         a GLib log message and is harmless.
  KNOWN_ISSUES           a known problem of the app is fixed (the run
                         tells you) or a new one has to be lived with.
  VOICE                  the pinned test voice.

Stdlib only; Python 3.9 or newer on the host. The helpers under sandbox/
run with the python3 of the GNOME runtime.
"""

import argparse
import array
import configparser
import ctypes
import hashlib
import json
import math
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import wave
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
SANDBOX_HELPERS = HERE / "sandbox"

APP_ID = "org.alternativeinterface.dasher"
COMMAND = "dasher"
# The GApplication id set in src/main.cpp, which is what the app registers
# on the session bus. Not the Flatpak app id, hence the --own-name below.
BUS_NAME = "org.dasher.gtk"

# ── Permissions ──────────────────────────────────────────────────────────
# The complete finish-args of the manifest. The build must have exactly
# these: one more is permission creep that nobody reviewed, one less breaks
# the feature named next to it. Change this list in the same commit as the
# manifest, with the reason.
EXPECTED_FINISH_ARGS = [
    "--socket=wayland",
    "--socket=fallback-x11",
    "--socket=pulseaudio",  # speech playback (ALSA default device -> sound server)
    "--share=ipc",  # X11 shared memory
    "--share=network",  # cloud speech engines, voice downloads, opt-in analytics
    "--device=all",  # joysticks and switch interfaces under /dev/input (SDL)
    f"--own-name={BUS_NAME}",  # GtkApplication registers this name at startup
    "--talk-name=org.a11y.Bus",  # AT-SPI
    "--talk-name=org.a11y.*",
    "--filesystem=home",  # open/save text files, import/export training data
]
# Asked for by name in reviews, so they get a message of their own.
FORBIDDEN_FINISH_ARGS = {
    "--socket=session-bus": "full session bus access; --own-name/--talk-name are enough",
    "--socket=system-bus": "full system bus access",
    "--filesystem=host": "the whole host file system",
    "--device=shm": "not needed",
}
# Sections flatpak-builder writes into `metadata` that are not permissions.
NEUTRAL_METADATA_SECTIONS = {"Application", "Build"}
# ... and the extension points of its own it adds (.Debug, .Locale, .Sources).
OWN_EXTENSION_PREFIX = f"Extension {APP_ID}."

# ── Files ────────────────────────────────────────────────────────────────
LICENCE_DIR = f"share/licenses/{APP_ID}"
# One directory per library that cmake unpacks into sherpa-onnx's _deps/ and
# links statically (the manifest's post-install copies their licence files).
# A directory that is not listed here is a new dependency: look at its
# licence, then add it.
SHERPA_DEP_LICENCE_DIRS = [
    "eigen",
    "hclust_cpp",
    "json",
    "kaldi_decoder",
    "kaldi_native_fbank",
    "kaldifst",
    "kissfft",
    "openfst",
    "simple-sentencepiece",
    "utf8proc",
]
EXPECTED_FILES = [
    # (path below files/, kind)
    (f"bin/{COMMAND}", "launcher"),
    ("dasher", "elf"),
    ("libdasher.so", "elf"),
    ("librust_tts_wrapper.so", "elf"),
    (f"share/applications/{APP_ID}.desktop", "file"),
    (f"share/metainfo/{APP_ID}.metainfo.xml", "file"),
    (f"share/icons/hicolor/48x48/apps/{APP_ID}.png", "file"),
    (f"share/icons/hicolor/scalable/apps/{APP_ID}.svg", "file"),
    (f"{LICENCE_DIR}/onnxruntime/LICENSE", "file"),
    (f"{LICENCE_DIR}/onnxruntime/ThirdPartyNotices.txt", "file"),
    (f"{LICENCE_DIR}/sherpa-onnx/LICENSE", "file"),
]
# What the launcher has to do: the app is a portable directory at /app, and
# the mm libraries may sit in /app/lib64 (see the manifest's post-install).
LAUNCHER_LIBRARY_PATH = "/app:/app/lib64"
# Where the libraries of the sandbox are: the app, and the library
# directories of the runtime. A search path or a resolved library anywhere
# else is a mistake.
LIBRARY_ROOTS = ("/app", "/usr/lib", "/usr/lib64")
# Below export/ (what the host's desktop sees of the app).
EXPECTED_EXPORTS = [
    f"share/applications/{APP_ID}.desktop",
    f"share/icons/hicolor/48x48/apps/{APP_ID}.png",
    f"share/icons/hicolor/scalable/apps/{APP_ID}.svg",
]
# The manifest's `cleanup:` in terms of what must not be left in files/.
# The build prefix of the speech libraries is in here too.
CLEANED_PATHS = [
    "include",
    "lib/pkgconfig",
    "lib64/pkgconfig",
    "share/pkgconfig",
    "share/aclocal",
    "share/man",
    "share/gtk-doc",
    "share/doc",
    "sherpa-onnx",
]
CLEANED_SUFFIXES = (".a", ".la")

# ── flatpak-builder-lint ─────────────────────────────────────────────────
# Errors the linter reports today and why they are accepted. Any other
# error fails the run. An entry whose error is no longer reported is
# pointed out so that it gets deleted. Warnings are printed, never fatal.
LINT_EXPECTED_ERRORS = {
    "manifest": {
        "appid-url-not-reachable": (
            "alternativeinterface.org does not resolve; needs the domain or another app id "
            "(a project decision, see the PR's open items)"
        ),
    },
    "builddir": {
        "appid-url-not-reachable": "as for the manifest",
        "appstream-missing-screenshots": (
            "the screenshot URL names a commit that is not on GitHub until this branch is "
            "merged; appstream drops an unreachable screenshot from the catalogue"
        ),
    },
}

# ── Headless start ───────────────────────────────────────────────────────
# What the app may write while it starts without failing the `log` check.
# A healthy start in the test sandbox prints exactly one line, the Broadway
# warning below. Everything else fails the check, so that an error the app
# reports with std::cerr ("InputManager: SDL joystick init failed: ...",
# "TtsService: Failed to create engine: ...") is seen and not only the ones
# that go through GLib. Keep both lists short, each pattern anchored and as
# long as the message allows, and each reason true: a pattern that is too
# wide hides the next real warning.
#
# GLib warnings: (regular expression, why it is harmless), matched against
# "<log domain>-WARNING: <message>". ERROR and CRITICAL lines are never
# allowed.
LOG_ALLOWLIST = [
    (
        r"^Gdk-WARNING: Duplicate call to gdk_frame_timings_submitted\(\)",
        "GTK's Broadway backend, which only the test uses as its display",
    ),
]
# Lines that are not GLib log messages (the app's own std::cout/std::cerr,
# a library writing to stderr): (regular expression, why it is harmless),
# matched against the whole line. Empty today, because a healthy start
# prints none. GLib messages below WARNING (Message, INFO, DEBUG) need no
# entry.
OUTPUT_ALLOWLIST = []
# Lines that are not GLib log messages and mean the start went wrong. They
# fail the check whatever OUTPUT_ALLOWLIST says.
FATAL_LOG_PATTERNS = [
    r"panicked at",
    r"Segmentation fault",
    r"core dumped",
    r"terminate called",
    r"stack smashing detected",
    r"Assertion .* failed",
]
# Problems of the app (not of the packaging) that a check would otherwise
# fail on. An entry keeps the run green; when the problem is gone the run
# says so and the entry has to go.
KNOWN_ISSUES = {
    "sigterm-ignored": (
        "the app does not exit on SIGTERM: SDL installs a handler that turns the signal "
        "into an SDL quit event, and the event loop in src/Input/InputManager.cpp ignores "
        "that event. The test stops the app with SIGKILL after 5 seconds. Reported "
        "upstream; remove this entry when the run says the app exits on SIGTERM"
    ),
}

# ── Speech ───────────────────────────────────────────────────────────────
# The voice the synthesis check speaks with: the smallest English Piper
# voice of the catalogue built into rust-tts-wrapper (20 MB download, plus
# the pronunciation data of its language, about 23 MB).
VOICE = {
    "id": "piper-en_GB-cori-medium-int8",
    # sha256 of the archive, as the built-in catalogue has to list it. The
    # wrapper's installer verifies the download against the catalogue.
    "archive_sha256": "169ca8aff3adb271f009a4924c99928a811dbf2b52eaca2dbb460e8c34478c93",
    # sha256 of the model file after installation. Checked by this script
    # before every use of the cache.
    "model_file": "en_GB-cori-medium.onnx",
    "model_sha256": "2e390818b07a426469f204a2e9c865d9c3f612af58f32839f646bc1010078574",
    "sample_rate": 22050,
    # The pronunciation data the wrapper speaks this voice with: the row for
    # British English in its src/sherpa_lang.rs, a bundle of the manifest
    # lexicons.json. No checksum is pinned here, because the wrapper takes
    # the newest release of AACTools/voicegarden-lexicons: the manifest it
    # downloads names the checksum, and the wrapper verifies the bundle
    # against it and writes that checksum next to the unpacked files.
    "lexicon_manifest": "lexicons.json",
    "lexicon": "en-GB",
}
# Where the wrapper keeps its files below the $HOME of the speech sandboxes
# (see Suite.speech_env): the voices, the record its installer writes into a
# voice's directory, and the pronunciation data.
MODELS_DIR = ".rust-tts-wrapper/sherpaonnx"
INSTALL_RECORD = ".installed"
LEXICON_DIR = "voicegarden"
# The download is tried this often, with these pauses (seconds) before the
# second and the third attempt, before the network is given up on.
INSTALL_ATTEMPTS = 3
INSTALL_BACKOFF = (10, 30)
# One attempt may take this long (seconds) before it is stopped and counts
# as failed: the wrapper's HTTP client has no timeout for a connection that
# goes quiet. A good download takes a few seconds, and the installer keeps
# the part of the voice archive it has, so a slow one goes on in the next
# attempt. All attempts and pauses together stay under ten minutes, which
# is half of what the smoke-test step of publish.yml may take
# (Scripts/tests/test_flatpak_static.py compares the two).
INSTALL_TIMEOUT = 180
# Seconds for the host's tools (the validators, git, `flatpak info`) and
# for flatpak-builder-lint, which asks the network.
TOOL_TIMEOUT = 120
LINT_TIMEOUT = 300
SYNTH_TEXT = "Dasher is a zooming predictive text entry system."
# Plausible bounds for the sentence above: it takes about 2.8 seconds.
SYNTH_SECONDS = (1.0, 15.0)
# 16-bit samples; speech from this voice has an RMS around 1300.
SYNTH_MIN_RMS = 200
# Engines the Flatpak is built with (TTS_WRAPPER_FEATURES=cloud,sherpaonnx,
# sherpa-g2p,speaker): the local one and a few of the cloud ones.
EXPECTED_ENGINES = ["sherpaonnx", "azure", "google", "polly", "openai", "elevenlabs"]
# Not built in: speech-dispatcher's socket is not in finish-args.
UNEXPECTED_ENGINES = ["system"]

# Sockets `flatpak build` knows in every release this has to run with
# (flatpak 1.14 on Ubuntu 24.04 included). Used to switch them all off.
KNOWN_SOCKETS = [
    "x11",
    "wayland",
    "fallback-x11",
    "pulseaudio",
    "session-bus",
    "system-bus",
    "ssh-auth",
    "pcsc",
    "cups",
]


# Environment variables that say where the desktop session is. `flatpak`
# is started without them, so that it has nothing to hand on even if a
# negation were missing from its command line.
SESSION_VARIABLES = (
    "DISPLAY",
    "WAYLAND_DISPLAY",
    "WAYLAND_SOCKET",
    "XAUTHORITY",
    "DBUS_SESSION_BUS_ADDRESS",
    "AT_SPI_BUS_ADDRESS",
    "SSH_AUTH_SOCK",
)
# ... and where its sound server is. Kept only for the one sandbox that
# is given the sound server socket (audio-device, with --host-checks).
SOUND_VARIABLES = ("PULSE_SERVER", "PIPEWIRE_REMOTE")
# Directories `flatpak build` binds from the build directory, writable
# even with --readonly. Every sandbox gets a scratch directory instead.
WRITABLE_BUILD_DIRS = ("/var/tmp", "/var/lib")


class Skip(Exception):
    """The check cannot run here; the message says what is missing."""


class Unreachable(Skip):
    """The check needs a server that cannot be reached. Still a skip, but
    one that --strict lets through, with a warning: a release must not
    wait for somebody else's file server."""


class Failure(Exception):
    """The check failed; one message per problem."""

    def __init__(self, *problems):
        super().__init__("; ".join(problems))
        self.problems = list(problems)


class TimedOut(Failure):
    """A command did not finish in its time and was stopped. A failure like
    any other, except for the voice download, where it is an attempt that
    did not succeed."""


# ══ Pure helpers (unit-tested in Scripts/tests/test_flatpak_smoke.py) ═════


def read_metadata(text):
    """A Flatpak `metadata` key file as {section: {key: value}}."""
    parser = configparser.RawConfigParser(delimiters=("=",), strict=False, interpolation=None)
    parser.optionxform = str  # keys are case-sensitive bus names
    parser.read_string(text)
    return {section: dict(parser.items(section)) for section in parser.sections()}


def split_list(value):
    """The entries of a `a;b;c;` metadata value."""
    return [entry for entry in (value or "").split(";") if entry]


def permissions_from_metadata(metadata):
    """The permissions a `metadata` file grants, as a set of finish-args.
    Raises ValueError for anything this script does not know how to write
    as a finish-arg, so that nothing is passed over silently."""
    args = set()
    unknown = []
    context_keys = {
        "shared": "--share",
        "sockets": "--socket",
        "devices": "--device",
        "filesystems": "--filesystem",
        "features": "--allow",
        "persistent": "--persist",
    }
    for section, values in metadata.items():
        if section in NEUTRAL_METADATA_SECTIONS or section.startswith(OWN_EXTENSION_PREFIX):
            continue
        if section == "Context":
            for key, value in values.items():
                if key not in context_keys:
                    unknown.append(f"[Context] {key}={value}")
                    continue
                args.update(f"{context_keys[key]}={entry}" for entry in split_list(value))
        elif section in ("Session Bus Policy", "System Bus Policy"):
            prefix = "--" if section.startswith("Session") else "--system-"
            for name, policy in values.items():
                if policy in ("own", "talk"):
                    args.add(f"{prefix}{policy}-name={name}")
                else:
                    unknown.append(f"[{section}] {name}={policy}")
        elif section == "Environment":
            args.update(f"--env={key}={value}" for key, value in values.items())
        else:
            # [Policy ...], [USB Devices], an extension point, ...
            unknown.extend(f"[{section}] {key}={value}" for key, value in values.items())
            if not values:
                unknown.append(f"[{section}]")
    if unknown:
        raise ValueError("; ".join(unknown))
    return args


def compare_permissions(actual, expected=None):
    """Problems (a list of messages) with the set of finish-args `actual`."""
    expected = set(EXPECTED_FINISH_ARGS if expected is None else expected)
    problems = []
    for arg in sorted(actual - expected):
        why = FORBIDDEN_FINISH_ARGS.get(arg)
        problems.append(
            f"unexpected permission {arg}"
            + (f" ({why})" if why else "")
            + ": not in EXPECTED_FINISH_ARGS. If it is wanted, add it there with the reason"
        )
    for arg in sorted(expected - actual):
        problems.append(f"missing permission {arg} (expected by EXPECTED_FINISH_ARGS)")
    return problems


def build_args_with_network(manifest):
    """Where a flatpak-builder manifest (as JSON data) gives the build
    network access: a list of places, empty when it never does."""
    found = []

    def walk(node, where):
        if isinstance(node, dict):
            args = node.get("build-options", {}).get("build-args", [])
            if "--share=network" in args:
                found.append(where)
            for module in node.get("modules", []):
                if isinstance(module, dict):
                    walk(module, f"module {module.get('name', '?')}")
            for arch, options in node.get("build-options", {}).get("arch", {}).items():
                if "--share=network" in options.get("build-args", []):
                    found.append(f"{where} (arch {arch})")

    walk(manifest, "the top level")
    return found


def elf_info(path):
    """What the checks need from an ELF file: {"needed", "rpath", "runpath",
    "comment"} (lists of strings), or None when `path` is not an ELF file.
    A parser of its own so that no binutils is needed, on any architecture."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or len(data) < 64:
        return None
    is64 = data[4] == 2
    order = "<" if data[5] == 1 else ">"
    if is64:
        (shoff,) = struct.unpack_from(order + "Q", data, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from(order + "HHH", data, 0x3A)
    else:
        (shoff,) = struct.unpack_from(order + "I", data, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from(order + "HHH", data, 0x2E)
    sections = []
    for index in range(shnum):
        at = shoff + index * shentsize
        if is64:
            name, kind, _, _, offset, size, link = struct.unpack_from(order + "IIQQQQI", data, at)
        else:
            name, kind, _, _, offset, size, link = struct.unpack_from(order + "IIIIIII", data, at)
        sections.append({"name": name, "type": kind, "offset": offset, "size": size, "link": link})

    def string_at(table, at):
        start = table["offset"] + at
        return data[start : data.index(b"\0", start)].decode(errors="replace")

    info = {"needed": [], "rpath": [], "runpath": [], "comment": []}
    names = sections[shstrndx] if shstrndx < len(sections) else None
    for section in sections:
        if section["type"] == 6:  # SHT_DYNAMIC
            strings = sections[section["link"]]
            entry = struct.Struct(order + ("qQ" if is64 else "iI"))
            for at in range(section["offset"], section["offset"] + section["size"], entry.size):
                tag, value = entry.unpack_from(data, at)
                if tag == 0:  # DT_NULL
                    break
                if tag == 1:  # DT_NEEDED
                    info["needed"].append(string_at(strings, value))
                elif tag in (15, 29):  # DT_RPATH, DT_RUNPATH
                    key = "rpath" if tag == 15 else "runpath"
                    info[key].extend(p for p in string_at(strings, value).split(":") if p)
        elif names and string_at(names, section["name"]) == ".comment":
            raw = data[section["offset"] : section["offset"] + section["size"]]
            info["comment"] = [s.decode(errors="replace") for s in raw.split(b"\0") if s]
    return info


def path_is_below(path, roots):
    """`path` is one of the directories `roots` or inside one. Compared by
    path component: /usr/libexec and /usr/lib64x are not below /usr/lib."""
    return any(path == root or path.startswith(root + "/") for root in roots)


def rpath_problem(entry, elf_dir):
    """Why the RPATH/RUNPATH entry of an ELF file installed in `elf_dir`
    (a path inside the sandbox) is not acceptable; None when it is. A
    search path has to stay inside the app or the runtime's library
    directories: anything else is a path of the build machine."""
    resolved = entry.replace("${ORIGIN}", elf_dir).replace("$ORIGIN", elf_dir)
    resolved = os.path.normpath(resolved)
    if path_is_below(resolved, LIBRARY_ROOTS):
        return None
    return f"{entry} (resolves to {resolved})"


def png_size(path):
    """(width, height) of a PNG file."""
    with open(path, "rb") as f:
        head = f.read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        raise ValueError(f"{path} is not a PNG file")
    return struct.unpack(">II", head[16:24])


RAW_GITHUB_URL = re.compile(
    r"https://raw\.githubusercontent\.com/(?P<owner>[^/]+)/(?P<repo>[^/]+)/(?P<rest>.+)"
)
# A release tag as Scripts/tag-release.sh and publish.yml accept it.
RELEASE_TAG = r"v[0-9]+\.[0-9]+\.[0-9]+(?:-rc\.[0-9]+)?"


def pinned_screenshot_url(url):
    """(ref, path in the repository) of a screenshot URL that names a
    commit or a release tag; raises ValueError with the reason otherwise.
    Flathub wants a commit or a tag here: the picture behind a branch
    changes without the metainfo changing."""
    m = RAW_GITHUB_URL.fullmatch(url)
    if not m:
        raise ValueError(f"{url} is not a raw.githubusercontent.com URL of a file in a repository")
    rest = m.group("rest")
    for pattern in (
        r"(?P<ref>[0-9a-f]{40})/(?P<path>.+)",
        rf"refs/tags/(?P<ref>{RELEASE_TAG})/(?P<path>.+)",
        rf"(?P<ref>{RELEASE_TAG})/(?P<path>.+)",
    ):
        m_ref = re.fullmatch(pattern, rest)
        if m_ref:
            return m_ref.group("ref"), m_ref.group("path")
    ref = rest.split("/", 1)[0]
    raise ValueError(
        f"{url} names '{ref}', which is neither a full commit sha nor a release tag "
        "(a branch moves, and Flathub does not accept one here)"
    )


def version_key(version):
    """Sort key for release versions such as 6.0.2 or 6.1.0-rc.1."""
    numbers = [int(n) for n in re.findall(r"[0-9]+", version.split("-")[0])]
    return (numbers, "-" not in version, version)


def metainfo_releases(root):
    """[(version, date)] of a parsed metainfo file, in document order."""
    return [(r.get("version"), r.get("date")) for r in root.findall("./releases/release")]


def release_problems(releases, project_version):
    """Problems with the metainfo's release list for `project_version`
    (CMake's DASHER_GTK_FALLBACK_VERSION, what a Flatpak build reports)."""
    if not releases:
        return ["the metainfo has no <release> entries"]
    problems = []
    newest = releases[0][0]
    highest = max((version for version, _ in releases), key=version_key)
    if newest != highest:
        problems.append(
            f"the first <release> is {newest} but {highest} is listed further down: "
            "the newest release has to be the first entry"
        )
    if project_version is not None and newest != project_version:
        problems.append(
            f"the newest <release> is {newest} but CMakeLists.txt has "
            f'DASHER_GTK_FALLBACK_VERSION "{project_version}", which is the version a '
            "Flatpak build reports (it has no git metadata). A release bumps both"
        )
    dates = [date for _, date in releases]
    if any(not re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", date or "") for date in dates):
        problems.append("a <release> has no date in the form YYYY-MM-DD")
    elif dates != sorted(dates, reverse=True):
        problems.append("the <release> entries are not ordered newest first by date")
    return problems


def cmake_fallback_version(cmake_text):
    m = re.search(r'set\(DASHER_GTK_FALLBACK_VERSION\s+"([^"]+)"', cmake_text)
    return m.group(1) if m else None


def lint_verdict(kind, report, expected=None):
    """(problems, notes) for the JSON `report` of `flatpak-builder-lint
    <kind>`: every error that is not expected is a problem; an expected
    error that is not reported any more is a note."""
    expected = LINT_EXPECTED_ERRORS[kind] if expected is None else expected
    errors = set(report.get("errors", []))
    details = {}
    for line in report.get("info", []):
        code, _, text = line.partition(": ")
        details.setdefault(code, text)
    problems = [
        f"new lint error {code}"
        + (f" ({details[code]})" if code in details else "")
        + f": fix it, or accept it in LINT_EXPECTED_ERRORS['{kind}'] with the reason"
        for code in sorted(errors - set(expected))
    ]
    notes = [
        f"lint error {code} is no longer reported: remove it from "
        f"LINT_EXPECTED_ERRORS['{kind}']"
        for code in sorted(set(expected) - errors)
    ]
    notes += [f"lint warning {code}" for code in sorted(report.get("warnings", []))]
    return problems, notes


GLIB_LOG_LINE = re.compile(
    r"^(?:\*\* )?\((?P<program>[^)]*):[0-9]+\): (?:(?P<domain>[\w.+-]+)-)?"
    r"(?P<level>ERROR|CRITICAL|WARNING|Message|INFO|DEBUG) \*\*: (?:[0-9:.]+: )?(?P<message>.*)$"
)


def log_problems(text, allowlist=None, output_allowlist=None):
    """(problems, allowed) for the app's output during the headless start.
    Problems are ERROR and CRITICAL messages, warnings that are not on
    `allowlist`, lines that look like a crash, and every other line that
    is neither a GLib message of a lower level nor on `output_allowlist`."""
    allowlist = LOG_ALLOWLIST if allowlist is None else allowlist
    output_allowlist = OUTPUT_ALLOWLIST if output_allowlist is None else output_allowlist
    problems, allowed = [], []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        m = GLIB_LOG_LINE.match(line)
        if m:
            level = m.group("level")
            entry = f"{m.group('domain') or 'app'}-{level}: {m.group('message')}"
            if level in ("ERROR", "CRITICAL"):
                problems.append(entry)
            elif level == "WARNING":
                if any(re.search(pattern, entry) for pattern, _ in allowlist):
                    allowed.append(entry)
                else:
                    problems.append(entry)
        elif any(re.search(pattern, line) for pattern in FATAL_LOG_PATTERNS):
            problems.append(line)
        elif any(re.search(pattern, line) for pattern, _ in output_allowlist):
            allowed.append(line)
        else:
            problems.append(line)
    return problems, allowed


def wav_stats(path):
    """Sample rate, channels, seconds, RMS and peak of a 16-bit WAV file."""
    with wave.open(str(path), "rb") as w:
        rate, channels, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
        frames = w.getnframes()
        raw = w.readframes(frames)
    stats = {"rate": rate, "channels": channels, "width": width, "seconds": frames / rate}
    if width != 2:
        stats.update(rms=0.0, peak=0)
        return stats
    samples = array.array("h")
    samples.frombytes(raw[: len(raw) // 2 * 2])
    if sys.byteorder == "big":
        samples.byteswap()
    stats["rms"] = math.sqrt(sum(s * s for s in samples) / len(samples)) if samples else 0.0
    stats["peak"] = max((abs(s) for s in samples), default=0)
    return stats


def audio_problems(stats, voice=None):
    """Why the synthesized audio is not plausible speech; empty when it is."""
    voice = VOICE if voice is None else voice
    problems = []
    if stats["width"] != 2:
        problems.append(f"{8 * stats['width']}-bit samples, expected 16-bit")
    if stats["rate"] != voice["sample_rate"]:
        problems.append(f"sample rate {stats['rate']} Hz, the voice is {voice['sample_rate']} Hz")
    low, high = SYNTH_SECONDS
    if not low <= stats["seconds"] <= high:
        problems.append(
            f"{stats['seconds']:.2f} s of audio, expected between {low:g} and {high:g} s"
        )
    if stats["rms"] < SYNTH_MIN_RMS:
        problems.append(
            f"the audio is silent or nearly so (RMS {stats['rms']:.0f}, peak {stats['peak']}; "
            f"speech is above {SYNTH_MIN_RMS})"
        )
    return problems


def install_failure_kind(error):
    """What kind of failure the message of rust-tts-wrapper's installer
    stands for (InstallError in its src/local_models.rs): "checksum" when
    the download is not the file the catalogue pins, "network" when a
    server could not be reached or did not send the file, "other" for
    everything else (disk, file system, an unknown model)."""
    text = error or ""
    if "checksum mismatch" in text:
        return "checksum"
    if text.startswith("network: ") or "pronunciation bundle failed to provision" in text:
        return "network"
    return "other"


BUNDLE_WARNING = re.compile(r"G2P bundle for '[^']*' unavailable \((?P<reason>.*)\)")


def bundle_warning(stderr):
    """Why the installer could not fetch the pronunciation data, from the
    one line it writes to stderr about it; None when it wrote none. For an
    English voice the installer reports success all the same (the wrapper
    can speak English without the data, though not this voice offline), so
    this line is the only place the reason is in."""
    m = BUNDLE_WARNING.search(stderr or "")
    return m.group("reason") if m else None


def voice_problems(home, voice=None):
    """What is wrong with the voice installed below `home` (the $HOME of the
    speech sandboxes), as a list of messages; empty when it is complete.

    Complete is what rust-tts-wrapper needs to load the voice, going by its
    installer (src/sherpa_model_source.rs) and engine
    (src/sherpaonnx_engine.rs) at the pinned commit:
      - the record the installer writes into the voice's directory once the
        archive is downloaded, verified against the catalogue and unpacked,
        naming this voice and the pinned archive;
      - every file of the archive that the record lists;
      - the model, which has to be the pinned file, and its token table.
    """
    voice = VOICE if voice is None else voice
    problems = []
    directory = home / MODELS_DIR / voice["id"]
    record_file = directory / INSTALL_RECORD
    try:
        record = json.loads(record_file.read_text())
        listed = [str(name) for name in record["files"]]
    except (OSError, ValueError, KeyError, TypeError):
        problems.append(
            f"{INSTALL_RECORD}, the installer's record of a finished installation, is "
            "missing or cannot be read"
        )
        listed = []
    else:
        archive = str(record.get("sha256")).lower()
        if record.get("id") != voice["id"] or archive != voice["archive_sha256"]:
            problems.append(
                f"{INSTALL_RECORD} is the record of {record.get('id')} from the archive "
                f"{archive}, pinned is {voice['id']} from {voice['archive_sha256']}"
            )
    absent = [name for name in listed if not (directory / name).is_file()]
    if absent:
        problems.append(
            f"{len(absent)} of the {len(listed)} files of the voice archive are missing "
            f"(the first: {absent[0]})"
        )
    model = directory / voice["model_file"]
    if not model.is_file():
        problems.append(f"the model {voice['model_file']} does not exist")
    else:
        digest = sha256_file(model)
        if digest != voice["model_sha256"]:
            problems.append(f"{model.name} has sha256 {digest}, pinned is {voice['model_sha256']}")
    tokens = directory / "tokens.txt"
    if not (tokens.is_file() and tokens.stat().st_size):
        problems.append("the token table tokens.txt is missing or empty")
    return problems


def lexicon_problems(home, voice=None):
    """What is wrong with the pronunciation data of the voice below `home`:
    (missing, wrong), two lists of messages, both empty when it is complete.

    Complete is what rust-tts-wrapper asks for before it speaks without a
    network request (cached_bundle_dir in its src/sherpa_g2p.rs) and what
    its front-end then opens:
      - the manifest, with an entry for the voice's bundle;
      - the bundle's dictionary (<key>.fst, <key>.pho), and the model for
        words that are not in it (phonetisaurus.fst) when the manifest says
        the bundle has one;
      - the checksum the bundle was verified against, which
        voicegarden-lexicons writes last, after unpacking, and which has to
        be the one in the manifest.
    `missing` is what a download that did not get through leaves out, and
    another attempt may bring. `wrong` is data that is there and is not
    what the manifest names: no attempt changes that."""
    voice = VOICE if voice is None else voice
    key = voice["lexicon"]
    missing, wrong = [], []
    directory = home / LEXICON_DIR
    manifest = directory / voice["lexicon_manifest"]
    entry = None
    try:
        languages = json.loads(manifest.read_text())["languages"]
        entry = next(e for e in languages if isinstance(e, dict) and e.get("lang") == key)
    except (OSError, ValueError, KeyError, TypeError):
        missing.append(f"the manifest {manifest.name}, or it cannot be read")
    except StopIteration:
        missing.append(f"an entry for the bundle {key} in {manifest.name}")
    files = [f"{key}.fst", f"{key}.pho"]
    if entry and entry.get("phonetisaurus"):
        files.append("phonetisaurus.fst")
    for name in files:
        path = directory / key / name
        if not (path.is_file() and path.stat().st_size):
            missing.append(f"{key}/{name}")
    stamp = directory / key / ".sha256"
    if not stamp.is_file():
        missing.append(f"{key}/.sha256, which is written when the bundle is unpacked")
    elif entry and stamp.read_text().strip() != entry.get("sha256"):
        wrong.append(
            f"the bundle {key} was verified against the checksum {stamp.read_text().strip()}, "
            f"{manifest.name} names {entry.get('sha256')}"
        )
    return missing, wrong


def exchange_paths(first, second):
    """Swap two paths of one file system in a single step, so that neither
    is missing at any moment (Linux: renameat2 with RENAME_EXCHANGE). False
    when the C library, the kernel or the file system cannot do that;
    nothing has changed then."""
    try:
        renameat2 = ctypes.CDLL(None).renameat2
    except (OSError, AttributeError):
        return False
    renameat2.argtypes = [
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_uint,
    ]
    at_fdcwd, rename_exchange = -100, 2
    return (
        renameat2(at_fdcwd, os.fsencode(first), at_fdcwd, os.fsencode(second), rename_exchange) == 0
    )


def run_command(cmd, *, timeout, what=None, text=True, **kwargs):
    """Run an external command to its end and return the completed process,
    its output captured. Every command of this script goes through here, so
    that none can hang a run for ever and none ends it with a traceback:
    TimedOut when it takes longer than `timeout` seconds (it is killed and
    waited for first), Failure when it cannot be started. `what` names the
    command in those messages."""
    what = what or Path(cmd[0]).name
    try:
        return subprocess.run(cmd, capture_output=True, text=text, timeout=timeout, **kwargs)
    except subprocess.TimeoutExpired:
        raise TimedOut(f"{what} did not finish within {timeout:g} s and was stopped") from None
    except OSError as e:
        raise Failure(f"{what} could not be started: {e}") from None


def tool_version(output):
    """The version number in the output of `TOOL --version`, or "?"."""
    m = re.search(r"[0-9]+(?:\.[0-9]+)+", output or "")
    return m.group(0) if m else "?"


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


# ══ The suite ═════════════════════════════════════════════════════════════


class Suite:
    """One run: the build directory under test and what the checks share."""

    def __init__(self, build_dir, repo, options):
        self.build_dir = Path(build_dir).resolve() if build_dir else None
        self.repo = Path(repo).resolve()
        self.options = options
        self.notes = []
        self._workdir = None
        self._sandbox_ok = {}
        self._headless = None
        self.voice_unreachable = None

    # ── inputs ───────────────────────────────────────────────────────────
    @property
    def files(self):
        return self.build_dir / "files"

    def metadata(self):
        return read_metadata((self.build_dir / "metadata").read_text())

    def manifest(self):
        """The manifest as flatpak-builder recorded it in the build."""
        path = self.files / "manifest.json"
        if not path.exists():
            raise Skip(f"{path} does not exist (not a flatpak-builder build directory?)")
        return json.loads(path.read_text())

    def metainfo(self):
        return ET.parse(self.files / f"share/metainfo/{APP_ID}.metainfo.xml").getroot()

    def app_elfs(self):
        """(path below files/, elf_info) of every ELF file of the app. The
        detached debug info under lib/debug is not part of the app."""
        found = []
        for path in sorted(self.files.rglob("*")):
            relative = path.relative_to(self.files).as_posix()
            if relative.startswith("lib/debug/") or path.is_symlink() or not path.is_file():
                continue
            info = elf_info(path)
            if info is not None:
                found.append((relative, info))
        return found

    def note(self, message):
        self.notes.append(message)

    # ── scratch space ────────────────────────────────────────────────────
    def workdir(self):
        """A temporary directory that is bound into the sandboxes at
        /run/dasher-smoke: the helpers, and what they write."""
        if self._workdir is None:
            self._workdir = Path(tempfile.mkdtemp(prefix="dasher-smoke-"))
            for helper in SANDBOX_HELPERS.iterdir():
                if helper.is_file():
                    shutil.copy(helper, self._workdir / helper.name)
        return self._workdir

    def cleanup(self):
        if self._workdir is None:
            return
        keep = self.options.keep_logs
        if keep:
            Path(keep).mkdir(parents=True, exist_ok=True)
            for item in self._workdir.iterdir():
                if item.suffix in (".log", ".json", ".wav"):
                    shutil.copy(item, Path(keep) / item.name)
        shutil.rmtree(self._workdir, ignore_errors=True)

    # ── sandbox ──────────────────────────────────────────────────────────
    def declared(self, key):
        """What the build's metadata declares under [Context] `key`."""
        return split_list(self.metadata().get("Context", {}).get(key, ""))

    def sandbox_command(self, *, platform=True, network=False, sockets=(), devices=()):
        """The `flatpak build` command for a sandbox on the build directory,
        up to and including the directory. Every permission the metadata
        declares is switched off, and only `network`, `sockets` and
        `devices` are switched on."""
        cmd = ["flatpak", "build", "--readonly", "--die-with-parent"]
        if platform:
            cmd.append("--runtime")
        for socket in dict.fromkeys(KNOWN_SOCKETS + self.declared("sockets")):
            cmd.append(f"--socket={socket}" if socket in sockets else f"--nosocket={socket}")
        for device in dict.fromkeys(["all"] + self.declared("devices")):
            cmd.append(f"--device={device}" if device in devices else f"--nodevice={device}")
        for share in dict.fromkeys(["network", "ipc"] + self.declared("shared")):
            on = share == "network" and network
            cmd.append(f"--share={share}" if on else f"--unshare={share}")
        for filesystem in dict.fromkeys(["home", "host"] + self.declared("filesystems")):
            cmd.append(f"--nofilesystem={filesystem.split(':')[0]}")
        for feature in self.declared("features"):
            cmd.append(f"--disallow={feature}")
        work = self.workdir()
        cmd.append(f"--bind-mount=/run/dasher-smoke={work}")
        for directory in WRITABLE_BUILD_DIRS:
            scratch = work / directory.strip("/")
            scratch.mkdir(parents=True, exist_ok=True)
            cmd.append(f"--bind-mount={directory}={scratch}")
        return cmd + [str(self.build_dir)]

    def sandbox(
        self,
        argv,
        *,
        platform=True,
        network=False,
        sockets=(),
        devices=(),
        binds=(),
        env=(),
        timeout=180,
    ):
        """Run `argv` inside the build's sandbox and return the completed
        process. The Platform runtime is /usr unless `platform` is False
        (then the Sdk). Nothing of the host is shared unless the caller
        asks for it (see sandbox_command). The build directory is
        read-only; the work directory is at /run/dasher-smoke. Raises
        TimedOut when `argv` is not done after `timeout` seconds. The
        sandbox is gone by then: `flatpak build` has become bwrap, which is
        killed, and --die-with-parent and the end of its process namespace
        take everything inside with it."""
        self.need_sandbox(platform)
        cmd = self.sandbox_command(
            platform=platform, network=network, sockets=sockets, devices=devices
        )
        build_dir = cmd.pop()
        cmd += [f"--bind-mount={dest}={src}" for dest, src in binds]
        cmd += [f"--env={key}={value}" for key, value in env]
        cmd.append(build_dir)
        cmd += argv
        if self.options.verbose:
            print("    $ " + " ".join(shlex.quote(part) for part in cmd), flush=True)
        return run_command(
            cmd,
            timeout=timeout,
            env=self.host_env(sockets),
            what=f"`{' '.join(argv[:3])}` in the sandbox",
        )

    @staticmethod
    def host_env(sockets=()):
        """The environment for `flatpak` itself: without the variables
        that say where the desktop session is. Only a sandbox that is
        given the sound server socket keeps the ones that locate it."""
        drop = set(SESSION_VARIABLES)
        if "pulseaudio" not in sockets:
            drop.update(SOUND_VARIABLES)
        return {k: v for k, v in os.environ.items() if k not in drop}

    def need_host(self, what):
        """Skip unless --host-checks was given: the check reaches outside
        the sandbox, to `what`."""
        if not self.options.host_checks:
            raise Skip(
                f"reaches outside the sandbox ({what}); run with --host-checks to include it"
            )

    def need_sandbox(self, platform=True):
        """Skip unless `flatpak build` works on this build directory with
        the runtime in question (probed once, in a sandbox that shares
        nothing) and can be kept away from the host."""
        if platform in self._sandbox_ok:
            if self._sandbox_ok[platform] is not True:
                raise Skip(self._sandbox_ok[platform])
            return
        reason = True
        persistent = self.declared("persistent")
        if persistent:
            reason = (
                f"the build declares --persist={','.join(persistent)}, which `flatpak build` "
                "cannot switch off: it would create that directory below ~/.var/app on the "
                "host and bind it into the sandbox. No sandbox is started for such a build"
            )
        elif not shutil.which("flatpak"):
            reason = "flatpak is not installed"
        else:
            cmd = self.sandbox_command(platform=platform) + ["true"]
            try:
                probe = run_command(
                    cmd, timeout=TOOL_TIMEOUT, env=self.host_env(), what="`flatpak build`"
                )
                if probe.returncode != 0:
                    message = (probe.stderr.strip() or "no message").splitlines()[-1]
                    which = "Platform" if platform else "Sdk"
                    reason = f"`flatpak build` does not run here with the {which} ({message})"
            except Failure as e:  # it hangs, or it cannot be started
                reason = f"no sandbox can be started here: {e}"
        self._sandbox_ok[platform] = reason
        if reason is not True:
            raise Skip(reason)

    def sandbox_json(self, argv, **kwargs):
        """Run a helper that prints one JSON object as its last line, and
        return that object."""
        return helper_result(self.sandbox(argv, **kwargs), argv)

    # ── the headless start, shared by four checks ────────────────────────
    def headless(self):
        """Start the app once (see sandbox/headless.py) and keep what was
        observed. The app gets nothing of the host: no device, and its
        display and session bus are inside the sandbox."""
        if self._headless is None:
            # A sandbox that cannot run here is a skip for every check that
            # asks, not an error to remember.
            self.need_sandbox()
            work = self.workdir()
            result_file = work / "headless.json"
            result_file.unlink(missing_ok=True)
            self._headless = {"error": "the sandbox could not be started", "stage": "sandbox"}
            try:
                done = self.sandbox(
                    [
                        "python3",
                        "/run/dasher-smoke/headless.py",
                        "/run/dasher-smoke/headless.json",
                        "/run/dasher-smoke",
                        COMMAND,
                        BUS_NAME,
                        str(self.options.alive_seconds),
                    ],
                    # The image loaders are started through the Flatpak portal,
                    # which the private bus does not have: see the stand-in.
                    binds=[("/usr/bin/flatpak-spawn", work / "flatpak-spawn")],
                    timeout=180 + int(self.options.alive_seconds),
                )
                try:
                    self._headless = json.loads(result_file.read_text())
                except (OSError, ValueError):
                    tail = " | ".join((done.stderr or done.stdout).strip().splitlines()[-3:])
                    self._headless = {
                        "error": f"the helper left no result (status {done.returncode}): {tail}"
                    }
                else:
                    log = work / "app.log"
                    self._headless["log"] = log.read_text(errors="replace") if log.exists() else ""
            except Failure as e:
                # A sandbox that hangs or cannot be started: every check
                # that shares this start says so, with the reason.
                self._headless = {"error": str(e), "stage": "sandbox"}
        if "error" in self._headless:
            raise Failure(
                f"the headless start did not get as far as the app "
                f"(stage {self._headless.get('stage', '?')}): {self._headless['error']}"
            )
        return self._headless

    def voice_cache(self):
        """The directory the test voice is kept in between runs. In it,
        `installed` is a complete installation (the $HOME of the synthesis
        check), `installing` is where the installer is at work, and
        `replaced` is the installation that a new one takes the place of,
        for the moment that takes (see promote_voice)."""
        if self.options.voice_cache:
            return Path(self.options.voice_cache).resolve()
        cache = os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache")
        return Path(cache) / "dasher-flatpak-smoke"

    def voice_home(self):
        return self.voice_cache() / "installed"

    @staticmethod
    def voice_model(home):
        return home / MODELS_DIR / VOICE["id"] / VOICE["model_file"]

    def cache_problems(self):
        """Why the cache holds no voice the synthesis can use, as a list of
        messages; empty when it holds a complete one with the pinned
        model."""
        home, replaced = self.voice_home(), self.voice_cache() / "replaced"
        if replaced.is_dir() and not home.exists():
            # A run that was killed half way through promote_voice.
            replaced.rename(home)
        if not home.is_dir():
            return ["nothing is installed there"]
        missing, wrong = lexicon_problems(home)
        if missing:
            missing = ["the pronunciation data is incomplete, missing: " + ", ".join(missing)]
        return voice_problems(home) + missing + wrong

    def cached_voice(self):
        """The installed model file of VOICE in the cache; None when the
        cache does not hold the complete, pinned voice."""
        return None if self.cache_problems() else self.voice_model(self.voice_home())

    def promote_voice(self, staging):
        """Put the complete installation in `staging` in the place of the
        cached one, whole: never a mixture of the two. Where the system can
        do it the two directories are swapped in one step, so that there is
        no moment without a voice (the old installation is then in
        `staging`). Where it cannot, the old one is moved aside, the new
        one moved in, and the old one put back if that fails; a run that is
        killed in between leaves the old one aside, whole, where
        cache_problems finds it."""
        home, replaced = self.voice_home(), self.voice_cache() / "replaced"
        shutil.rmtree(replaced, ignore_errors=True)
        if not home.exists():
            staging.rename(home)
        elif not exchange_paths(staging, home):
            home.rename(replaced)
            try:
                staging.rename(home)
            except OSError:
                replaced.rename(home)
                raise
            shutil.rmtree(replaced, ignore_errors=True)

    def speech_env(self):
        return [
            ("HOME", "/run/dasher-voices"),
            ("VOICEGARDEN_LEXICON_DIR", f"/run/dasher-voices/{LEXICON_DIR}"),
        ]


def helper_result(done, argv):
    """The JSON object a sandbox helper printed as its last line. `done` is
    the completed process of `argv`."""
    lines = [line for line in done.stdout.splitlines() if line.startswith("{")]
    try:
        if done.returncode == 0:
            return json.loads(lines[-1])
    except (IndexError, ValueError):
        pass
    tail = " | ".join((done.stderr or done.stdout).strip().splitlines()[-3:])
    raise Failure(f"{' '.join(argv[:3])} exited with status {done.returncode}: {tail}")


CHECKS = []


def check(name, summary, tags=()):
    def register(function):
        CHECKS.append({"name": name, "summary": summary, "tags": tags, "run": function})
        return function

    return register


# ══ Checks: the build directory alone ═════════════════════════════════════


@check("permissions", "metadata and finish-args are exactly the expected set")
def check_permissions(suite):
    metadata = suite.metadata()
    problems = []
    application = metadata.get("Application", {})
    if application.get("name") != APP_ID:
        problems.append(f"[Application] name is {application.get('name')}, expected {APP_ID}")
    if application.get("command") != COMMAND:
        problems.append(
            f"[Application] command is {application.get('command')}, expected {COMMAND}"
        )
    try:
        problems += compare_permissions(permissions_from_metadata(metadata))
    except ValueError as e:
        problems.append(
            f"the metadata grants something this check does not know: {e}. If it is "
            "wanted, teach permissions_from_metadata() and EXPECTED_FINISH_ARGS about it"
        )
    # The manifest as built: the same list, and no network during the build.
    try:
        manifest = suite.manifest()
    except Skip:
        manifest = None
        suite.note("files/manifest.json is missing: finish-args and build-args not compared")
    if manifest is not None:
        declared = set(manifest.get("finish-args", []))
        for problem in compare_permissions(declared):
            problems.append(f"manifest finish-args: {problem}")
        for where in build_args_with_network(manifest):
            problems.append(
                f"{where} of the manifest has build-args --share=network: the build must "
                "stay offline (a Flathub requirement, and what makes it reproducible)"
            )
    if problems:
        raise Failure(*problems)
    return f"{len(EXPECTED_FINISH_ARGS)} permissions, no session-bus socket, offline build"


@check("files", "launcher, binary, libraries, desktop file, metainfo, icons and data exist")
def check_files(suite):
    problems = []
    for relative, kind in EXPECTED_FILES:
        path = suite.files / relative
        if not path.is_file():
            problems.append(f"/app/{relative} is missing")
        elif path.stat().st_size == 0:
            problems.append(f"/app/{relative} is empty")
        elif kind == "elf" and elf_info(path) is None:
            problems.append(f"/app/{relative} is not an ELF file")
        elif kind == "launcher":
            text = path.read_text(errors="replace")
            if not os.access(path, os.X_OK):
                problems.append(f"/app/{relative} is not executable")
            if not text.startswith("#!/bin/sh"):
                problems.append(f"/app/{relative} is not a /bin/sh script")
            if f'LD_LIBRARY_PATH="{LAUNCHER_LIBRARY_PATH}' not in text:
                problems.append(
                    f"/app/{relative} does not put {LAUNCHER_LIBRARY_PATH} on LD_LIBRARY_PATH"
                )
            if 'exec /app/dasher "$@"' not in text:
                problems.append(f'/app/{relative} does not `exec /app/dasher "$@"`')
    for relative in EXPECTED_EXPORTS:
        if not (suite.build_dir / "export" / relative).is_file():
            problems.append(f"export/{relative} is missing (flatpak-builder exports it)")
    # The engine's data: the app resolves Data/ next to the binary.
    alphabets = list((suite.files / "Data").glob("alphabet.*.xml"))
    if not alphabets:
        problems.append("/app/Data has no alphabet.*.xml: the engine data is not installed")
    if problems:
        raise Failure(*problems)
    return f"{len(EXPECTED_FILES) + len(EXPECTED_EXPORTS)} files, {len(alphabets)} alphabets"


@check("licences", "licence texts of the statically linked speech code are installed")
def check_licences(suite):
    base = suite.files / LICENCE_DIR
    problems = []
    notices = base / "onnxruntime/ThirdPartyNotices.txt"
    if notices.is_file() and notices.stat().st_size < 50_000:
        problems.append(
            f"{notices.relative_to(suite.files)} is only {notices.stat().st_size} bytes: "
            "ONNX Runtime's collection of third-party notices is several hundred kB"
        )
    for name in ("onnxruntime/LICENSE", "onnxruntime/ThirdPartyNotices.txt", "sherpa-onnx/LICENSE"):
        if not (base / name).is_file():
            problems.append(f"/app/{LICENCE_DIR}/{name} is missing")
    sherpa = base / "sherpa-onnx"
    present = sorted(p.name for p in sherpa.iterdir() if p.is_dir()) if sherpa.is_dir() else []
    for name in SHERPA_DEP_LICENCE_DIRS:
        files = [p for p in (sherpa / name).rglob("*") if p.is_file()] if name in present else []
        if name not in present:
            problems.append(f"/app/{LICENCE_DIR}/sherpa-onnx/{name}/ is missing")
        elif not files or all(p.stat().st_size == 0 for p in files):
            problems.append(f"/app/{LICENCE_DIR}/sherpa-onnx/{name}/ has no licence text")
    for name in sorted(set(present) - set(SHERPA_DEP_LICENCE_DIRS)):
        problems.append(
            f"/app/{LICENCE_DIR}/sherpa-onnx/{name}/ is not expected: sherpa-onnx links a "
            "new dependency. Look at its licence, then add it to SHERPA_DEP_LICENCE_DIRS"
        )
    if problems:
        raise Failure(*problems)
    total = sum(1 for p in base.rglob("*") if p.is_file())
    return f"ONNX Runtime, sherpa-onnx and its {len(SHERPA_DEP_LICENCE_DIRS)} dependencies ({total} files in all)"


@check("cleanup", "no headers, static archives, .la files, pkg-config files or build prefix")
def check_cleanup(suite):
    problems = [
        f"/app/{name} was not cleaned up" for name in CLEANED_PATHS if (suite.files / name).exists()
    ]
    leftovers = []
    for path in suite.files.rglob("*"):
        relative = path.relative_to(suite.files).as_posix()
        if path.name.endswith(CLEANED_SUFFIXES) and not path.is_dir():
            leftovers.append(relative)
        elif path.is_dir() and path.name == "pkgconfig":
            leftovers.append(relative + "/")
    listed = {name for name in CLEANED_PATHS}
    leftovers = [l for l in leftovers if l.rstrip("/") not in listed]
    if leftovers:
        shown = ", ".join(f"/app/{l}" for l in sorted(leftovers)[:8])
        more = f" and {len(leftovers) - 8} more" if len(leftovers) > 8 else ""
        problems.append(f"left in the app: {shown}{more} (the manifest's cleanup removes these)")
    if problems:
        raise Failure(*problems)
    return "nothing left of include/, pkgconfig, *.a, *.la, /app/sherpa-onnx"


@check("desktop-file", "the desktop file validates and starts the app's command")
def check_desktop_file(suite):
    path = suite.files / f"share/applications/{APP_ID}.desktop"
    if not path.is_file():
        raise Failure(f"/app/share/applications/{APP_ID}.desktop is missing")
    entry = read_metadata(path.read_text()).get("Desktop Entry", {})
    problems = []
    if entry.get("Exec", "").split()[:1] != [COMMAND]:
        problems.append(f"Exec={entry.get('Exec')} does not start `{COMMAND}`")
    if entry.get("Icon") != APP_ID:
        problems.append(f"Icon={entry.get('Icon')}, expected {APP_ID} (the exported icon)")
    validator = shutil.which("desktop-file-validate")
    if validator:
        done = run_command([validator, str(path)], timeout=TOOL_TIMEOUT)
        if done.returncode != 0:
            problems.append(
                "desktop-file-validate: " + " | ".join(done.stdout.strip().splitlines())
            )
    if problems:
        raise Failure(*problems)
    if not validator:
        raise Skip("desktop-file-validate is not installed (Exec and Icon were checked)")
    return "desktop-file-validate passes; Exec and Icon are the app's"


@check("metainfo", "the AppStream metainfo validates and names this app")
def check_metainfo(suite):
    path = suite.files / f"share/metainfo/{APP_ID}.metainfo.xml"
    if not path.is_file():
        raise Failure(f"/app/share/metainfo/{APP_ID}.metainfo.xml is missing")
    root = suite.metainfo()
    problems = []
    if root.findtext("id") != APP_ID:
        problems.append(f"<id> is {root.findtext('id')}, expected {APP_ID}")
    if root.findtext("launchable") != f"{APP_ID}.desktop":
        problems.append(f"<launchable> is {root.findtext('launchable')}, expected {APP_ID}.desktop")
    for tag in ("name", "summary", "metadata_license", "project_license"):
        if not (root.findtext(tag) or "").strip():
            problems.append(f"<{tag}> is missing or empty")
    source = suite.repo / f"packaging/{APP_ID}.metainfo.xml"
    if source.is_file() and source.read_bytes() != path.read_bytes():
        suite.note(
            f"the installed metainfo differs from {source.relative_to(suite.repo)} in this "
            "checkout: the build was made from another state of the repository"
        )
    validator = shutil.which("appstreamcli")
    if validator:
        # What counts as a warning differs between appstreamcli releases:
        # the version goes into the result, pass or fail.
        version = tool_version(run_command([validator, "--version"], timeout=TOOL_TIMEOUT).stdout)
        done = run_command([validator, "validate", "--no-net", str(path)], timeout=TOOL_TIMEOUT)
        if done.returncode != 0:
            lines = [l for l in done.stdout.strip().splitlines() if l.strip()]
            problems.append(f"appstreamcli {version} validate --no-net: " + " | ".join(lines[:12]))
    if problems:
        raise Failure(*problems)
    if not validator:
        raise Skip("appstreamcli is not installed (id, launchable and required tags were checked)")
    return f"appstreamcli {version} validate --no-net passes"


@check(
    "screenshot", "the screenshot URL names a commit or tag and matches the PNG in the repository"
)
def check_screenshot(suite):
    root = suite.metainfo()
    images = root.findall("./screenshots/screenshot/image")
    if not images:
        raise Failure("the metainfo has no <screenshot> with an <image> (Flathub requires one)")
    if not root.findall("./screenshots/screenshot[@type='default']"):
        raise Failure('no <screenshot type="default">')
    problems = []
    checked = 0
    for image in images:
        url = (image.text or "").strip()
        try:
            ref, repo_path = pinned_screenshot_url(url)
        except ValueError as e:
            problems.append(str(e))
            continue
        local = suite.repo / repo_path
        if not local.is_file():
            problems.append(f"{url} points at {repo_path}, which is not in this checkout")
            continue
        try:
            width, height = png_size(local)
        except ValueError as e:
            problems.append(str(e))
            continue
        declared = (image.get("width"), image.get("height"))
        if declared != (str(width), str(height)):
            problems.append(
                f"{repo_path} is {width}x{height} but the metainfo declares "
                f"{declared[0]}x{declared[1]}"
            )
        # When the URL names a commit that this checkout has, the file in
        # that commit has to be the file in the tree (a replaced image
        # needs the URL moved to the commit that replaces it).
        if re.fullmatch(r"[0-9a-f]{40}", ref) and (suite.repo / ".git").exists():
            git = ["git", "-C", str(suite.repo)]
            try:
                blob = run_command(
                    git + ["cat-file", "blob", f"{ref}:{repo_path}"],
                    timeout=TOOL_TIMEOUT,
                    text=False,
                )
                if blob.returncode != 0:
                    # A shallow clone (CI) has only its newest commit: nothing
                    # to compare with. In a full clone the commit ought to be
                    # there, so its absence is worth a remark.
                    shallow = run_command(
                        git + ["rev-parse", "--is-shallow-repository"], timeout=TOOL_TIMEOUT
                    )
                    if shallow.stdout.strip() != "true":
                        suite.note(
                            f"commit {ref[:12]}, which the screenshot URL names, is not in "
                            "this checkout: the image in it was not compared with the one in "
                            "the tree. If the branch that added it was squashed or rebased, "
                            "the URL no longer resolves on GitHub either"
                        )
                elif blob.stdout != local.read_bytes():
                    problems.append(
                        f"{repo_path} in commit {ref[:12]}, which the URL names, is not the "
                        "file in the tree: point the URL at the commit that changed the image"
                    )
            except Failure as e:
                # No git here, or one that hangs. The URL and the size were
                # checked above; what was not done is said.
                suite.note(
                    f"the image in commit {ref[:12]}, which the screenshot URL names, was not "
                    f"compared with the one in the tree: {e}"
                )
        checked += 1
    if problems:
        raise Failure(*problems)
    return f"{checked} image(s), pinned to a commit or tag, size as declared"


@check("release", "the newest metainfo release is the version the build reports")
def check_release(suite):
    cmake = suite.repo / "CMakeLists.txt"
    project_version = cmake_fallback_version(cmake.read_text()) if cmake.is_file() else None
    if project_version is None:
        suite.note("DASHER_GTK_FALLBACK_VERSION not found in CMakeLists.txt: not compared")
    problems = release_problems(metainfo_releases(suite.metainfo()), project_version)
    binary = suite.files / "dasher"
    if project_version and binary.is_file():
        # The version is compiled in as a string (DASHER_GTK_VERSION).
        if b"\0" + project_version.encode() + b"\0" not in binary.read_bytes():
            problems.append(
                f"/app/dasher does not contain the version string {project_version}: it "
                "was built from another version than this checkout's CMakeLists.txt"
            )
    if problems:
        raise Failure(*problems)
    return f"newest release {metainfo_releases(suite.metainfo())[0][0]}, as CMake and the binary"


@check("rpath", "no RPATH or RUNPATH leads outside /app and the runtime")
def check_rpath(suite):
    elfs = suite.app_elfs()
    if not elfs:
        raise Failure("no ELF file found below files/")
    problems = []
    for relative, info in elfs:
        elf_dir = os.path.dirname(f"/app/{relative}")
        for key in ("rpath", "runpath"):
            for entry in info[key]:
                problem = rpath_problem(entry, elf_dir)
                if problem:
                    problems.append(f"/app/{relative} has {key.upper()} {problem}")
    if problems:
        raise Failure(*problems)
    return f"{len(elfs)} ELF files"


@check("speech-from-source", "the speech library carries the ONNX Runtime built by the manifest")
def check_speech_from_source(suite):
    library = suite.files / "librust_tts_wrapper.so"
    if not library.is_file():
        raise Failure("/app/librust_tts_wrapper.so is missing")
    problems = []
    data = library.read_bytes()
    # Linked statically: nothing in the app may ask for a shared copy.
    for relative, info in suite.app_elfs():
        for needed in info["needed"]:
            if re.match(r"lib(onnxruntime|sherpa-onnx)", needed):
                problems.append(
                    f"/app/{relative} needs the shared library {needed}: ONNX Runtime and "
                    "sherpa-onnx are linked statically into librust_tts_wrapper.so"
                )
    for path in suite.files.rglob("*"):
        if re.match(r"lib(onnxruntime|sherpa-onnx).*\.so", path.name):
            problems.append(f"/app/{path.relative_to(suite.files)} is a shared speech library")
    # The version the manifest says it builds (its onnxruntime module
    # asserts it against the source) has to be the one in the binary.
    version = None
    for module in suite.manifest().get("modules", []):
        if isinstance(module, dict) and module.get("name") == "onnxruntime":
            m = re.search(
                r'VERSION_NUMBER\)" = ([0-9.]+)', " ".join(module.get("build-commands", []))
            )
            version = m.group(1) if m else None
    if version is None:
        problems.append("the built manifest has no onnxruntime module that states its version")
    elif b"\0" + version.encode() + b"\0" not in data:
        problems.append(f"the library does not contain ONNX Runtime's version string {version}")
    # ONNX Runtime's build info names the git commit it was built from.
    # The manifest builds a tarball, so there is none; k2-fsa's prebuilt
    # archive, which sherpa-onnx-sys downloads when left alone, has one.
    build_info = re.search(rb"ORT Build Info: [^\0]*", data)
    if not build_info:
        problems.append("the library has no ONNX Runtime build info string")
    else:
        text = build_info.group(0).decode(errors="replace")
        commit = re.search(r"git-commit-id=([^,]*)", text)
        if commit is None or commit.group(1).strip():
            problems.append(
                f"ONNX Runtime was built from a git checkout ({text.strip()}), which the "
                "manifest does not do: this is a prebuilt ONNX Runtime, not the one from "
                "the onnxruntime module"
            )
    # Every object in the library was compiled by the SDK's compiler. The
    # app's own binary says which one that is; the prebuilt archives were
    # built with another GCC and leave its name in .comment.
    reference = elf_info(suite.files / "dasher") if (suite.files / "dasher").is_file() else None
    ours = {c for c in (reference or {}).get("comment", []) if c.startswith("GCC:")}
    theirs = {c for c in (elf_info(library) or {}).get("comment", []) if c.startswith("GCC:")}
    if not ours:
        suite.note("no GCC ident in /app/dasher's .comment: compilers not compared")
    elif theirs - ours:
        problems.append(
            "librust_tts_wrapper.so contains code from another compiler than the SDK's "
            f"({'; '.join(sorted(theirs - ours))}; the app itself: {'; '.join(sorted(ours))}): "
            "prebuilt objects were linked in"
        )
    if problems:
        raise Failure(*problems)
    return f"ONNX Runtime {version} built from a tarball with the SDK's compiler, linked statically"


# ══ Checks: inside the sandbox ════════════════════════════════════════════


@check("linkage", "every ELF file resolves all its libraries inside the sandbox", tags=("sandbox",))
def check_linkage(suite):
    elfs = [f"/app/{relative}" for relative, _ in suite.app_elfs()]
    if not elfs:
        raise Failure("no ELF file found below files/")
    # With the search path the launcher sets, in the Platform runtime: a
    # library that only the Sdk has is not found here either.
    script = 'for f in "$@"; do echo "@@ $f"; ldd "$f" 2>&1; done'
    done = suite.sandbox(
        [
            "sh",
            "-c",
            f'LD_LIBRARY_PATH="{LAUNCHER_LIBRARY_PATH}:$LD_LIBRARY_PATH"; export LD_LIBRARY_PATH; {script}',
            "sh",
        ]
        + elfs
    )
    problems, current, resolved = [], None, 0
    seen = set()
    for line in done.stdout.splitlines():
        line = line.strip()
        if line.startswith("@@ "):
            current = line[3:]
            seen.add(current)
        elif "=> not found" in line:
            problems.append(f"{current} needs {line.split()[0]}, which is not in the sandbox")
        elif "=>" in line:
            name, target = line.split("=>", 1)
            target = target.split("(")[0].strip()
            resolved += 1
            if not path_is_below(os.path.normpath(target), LIBRARY_ROOTS):
                problems.append(
                    f"{current} resolves {name.strip()} to {target}, outside /app and the runtime"
                )
            if re.match(r"lib(onnxruntime|sherpa-onnx)", name.strip()):
                problems.append(f"{current} loads the shared library {name.strip()} ({target})")
        elif "not a dynamic executable" in line or "statically linked" in line:
            pass
    if done.returncode != 0 or seen != set(elfs):
        problems.append(
            f"ldd did not report on every file (status {done.returncode}): {done.stderr.strip()[-300:]}"
        )
    if problems:
        raise Failure(*problems)
    return f"{len(elfs)} ELF files, {resolved} library references, all in /app or the runtime"


@check("help", "`dasher --help` exits 0 in the sandbox", tags=("sandbox", "headless"))
def check_help(suite):
    result = suite.headless()
    help_run = result.get("help")
    if not help_run:
        raise Failure("--help was not run")
    if help_run["status"] != 0 or "--help" not in help_run["stdout"]:
        tail = (help_run["stderr"] or help_run["stdout"]).strip().splitlines()[-1:]
        raise Failure(
            f"`{COMMAND} --help` exited with status {help_run['status']}: {' '.join(tail)}"
        )
    version_run = result.get("version")
    if version_run is None:
        return "`--help` exits 0 (the app has no --version option)"
    if version_run["status"] != 0:
        raise Failure(
            f"`{COMMAND} --version` is listed by --help but exits with status {version_run['status']}"
        )
    return f"`--help` and `--version` exit 0 ({version_run['stdout'].strip()})"


@check("start", "the app starts headless and stays alive", tags=("sandbox", "headless"))
def check_start(suite):
    result = suite.headless()
    if result.get("exited_early"):
        tail = " | ".join(result.get("log", "").strip().splitlines()[-4:])
        crash = (
            " (the crash reporter wrote pending_crash.txt)" if result.get("pending_crash") else ""
        )
        raise Failure(
            f"the app exited on its own after {result.get('alive_seconds')} s with status "
            f"{result.get('exit_status')}{crash}: {tail}"
        )
    if result.get("pending_crash"):
        raise Failure(
            "the crash reporter wrote pending_crash.txt: " + result["pending_crash"][:300]
        )
    return f"alive for {result.get('alive_seconds')} s on the Broadway display, no crash report"


@check(
    "bus-name", f"the app owns {BUS_NAME} and the metadata lets it", tags=("sandbox", "headless")
)
def check_bus_name(suite):
    result = suite.headless()
    problems = []
    if not result.get("name_owned"):
        problems.append(
            f"{BUS_NAME} was not owned on the session bus within the start-up time "
            f"(names on the bus: {', '.join(result.get('names_on_bus', [])) or 'none'})"
        )
    elif not result.get("name_owned_at_end") and not result.get("exited_early"):
        problems.append(f"{BUS_NAME} was given up while the app was running")
    # The test bus has no policy, the real one filters by the metadata: a
    # name the app takes here has to be granted there, or the app aborts
    # at startup when installed ("Failed to register: ... ServiceUnknown").
    policy = suite.metadata().get("Session Bus Policy", {})
    for name in result.get("names_owned", []):
        if policy.get(name) != "own" and name != APP_ID:
            problems.append(
                f"the app owns {name} but the metadata's session bus policy does not grant "
                f"it (--own-name={name}): the installed app could not register"
            )
    for name, value in policy.items():
        if (
            value == "own"
            and name not in result.get("names_owned", [])
            and result.get("name_owned")
        ):
            problems.append(
                f"the metadata grants --own-name={name} but the app does not take that name"
            )
    if problems:
        raise Failure(*problems)
    return f"owned after {result.get('seconds_to_name')} s; granted by the session bus policy"


@check(
    "log",
    "the app prints no error, critical or other unexpected line while starting",
    tags=("sandbox", "headless"),
)
def check_log(suite):
    result = suite.headless()
    problems, allowed = log_problems(result.get("log", ""))
    if problems:
        shown = [f"unexpected line: {line[:300]}" for line in dict.fromkeys(problems)][:10]
        raise Failure(*shown)
    lines = len(result.get("log", "").strip().splitlines())
    return f"{lines} line(s) of output, {len(allowed)} of them allowlisted"


@check(
    "shutdown", "the app stops when asked and leaves no crash report", tags=("sandbox", "headless")
)
def check_shutdown(suite):
    result = suite.headless()
    if result.get("exited_early"):
        raise Failure("the app had already exited on its own (see the start check)")
    problems = []
    known = "sigterm-ignored" in KNOWN_ISSUES
    if result.get("exited_on_sigterm"):
        status = result.get("exit_status")
        if status not in (0, -15):
            problems.append(f"after SIGTERM the app exited with status {status}")
        if known:
            suite.note("the app now exits on SIGTERM: remove 'sigterm-ignored' from KNOWN_ISSUES")
    elif not known:
        problems.append("the app did not exit within 5 s of SIGTERM and had to be killed")
    if "quit" in (result.get("actions") or []):
        # Today the app exports no action at all (see LIMITS at the top).
        suite.note(
            "the app now exports a `quit` action on the bus: make sandbox/headless.py "
            "activate it (org.gtk.Actions.Activate) and have this check expect a clean exit"
        )
    if result.get("pending_crash"):
        problems.append("a crash report was written: " + result["pending_crash"][:300])
    if not result.get("name_released"):
        problems.append(f"{BUS_NAME} still has an owner after the app was stopped")
    if problems:
        raise Failure(*problems)
    if result.get("exited_on_sigterm"):
        return "exits on SIGTERM, bus name released, no crash report"
    return (
        "stopped with SIGKILL (known issue: SIGTERM is ignored), bus name released, no crash report"
    )


@check(
    "speech-library",
    "the speech library loads and has the engines and the voice catalogue",
    tags=("sandbox",),
)
def check_speech_library(suite):
    info = suite.sandbox_json(
        ["python3", "/run/dasher-smoke/speech.py", "info", VOICE["id"]], env=[("HOME", "/tmp")]
    )
    if "load_error" in info:
        raise Failure(f"/app/librust_tts_wrapper.so does not load: {info['load_error']}")
    problems = []
    engines = info.get("engines", [])
    for engine in EXPECTED_ENGINES:
        if engine not in engines:
            problems.append(
                f"engine {engine} is not compiled in (engines: {', '.join(engines) or 'none'})"
            )
    for engine in UNEXPECTED_ENGINES:
        if engine in engines:
            problems.append(
                f"engine {engine} is compiled in, but the sandbox has no access to "
                "speech-dispatcher: the manifest leaves it out on purpose"
            )
    model = info.get("model")
    if not info.get("catalogue_size"):
        problems.append("the built-in Sherpa voice catalogue is empty")
    elif model is None:
        problems.append(
            f"the test voice {VOICE['id']} is not in the built-in catalogue: pin another in VOICE"
        )
    else:
        if model.get("sha256") != VOICE["archive_sha256"]:
            problems.append(
                f"the catalogue lists another archive for {VOICE['id']} "
                f"(sha256 {model.get('sha256')}): update VOICE"
            )
        if model.get("sample_rate") != VOICE["sample_rate"]:
            problems.append(f"the catalogue gives {model.get('sample_rate')} Hz for {VOICE['id']}")
    if problems:
        raise Failure(*problems)
    return f"{len(engines)} engines, {info['catalogue_size']} catalogue entries, test voice listed"


@check(
    "speech-install",
    "the app's installer downloads the pinned voice (needs --network)",
    tags=("sandbox", "network"),
)
def check_speech_install(suite):
    if not suite.options.network:
        raise Skip("downloads a voice; run with --network to include it")
    if "network" not in suite.declared("shared"):
        raise Failure(
            "the build does not share the network (--share=network), so the app cannot download voices"
        )
    suite.need_sandbox()
    # Always a download from nothing, into a directory of its own: the
    # installer is what is tested, and nothing it does, or fails to do,
    # touches the voice a previous run left for the synthesis.
    staging = suite.voice_cache() / "installing"
    try:
        shutil.rmtree(staging, ignore_errors=True)
        staging.mkdir(parents=True)
        try:
            attempt, seconds = download_voice(suite, staging)
            suite.promote_voice(staging)
        finally:
            # What is left here is a download that failed, or the old voice.
            shutil.rmtree(staging, ignore_errors=True)
    except OSError as e:
        raise Failure(f"the voice cache {suite.voice_cache()} cannot be used: {e}") from None
    tries = f" at attempt {attempt + 1}" if attempt else ""
    return (
        f"{VOICE['id']} installed in {seconds} s{tries}: complete, with its pronunciation "
        "data, and the model file matches its pin"
    )


def download_voice(suite, staging):
    """Have the app's installer download VOICE into the empty directory
    `staging` until a complete installation is there: (the attempt that
    did it, counted from 0, the seconds it took). Raises Unreachable when
    INSTALL_ATTEMPTS attempts did not get the files, and Failure for
    everything another attempt cannot change."""
    argv = ["python3", "/run/dasher-smoke/speech.py", "install", VOICE["id"]]
    error = None
    for attempt in range(INSTALL_ATTEMPTS):
        if attempt:
            time.sleep(INSTALL_BACKOFF[min(attempt, len(INSTALL_BACKOFF)) - 1])
        try:
            done = suite.sandbox(
                argv,
                network=True,
                binds=[("/run/dasher-voices", staging)],
                env=suite.speech_env(),
                timeout=INSTALL_TIMEOUT,
            )
        except TimedOut:
            # A connection that went quiet. The sandbox has been stopped;
            # what the installer had fetched of the voice is still there
            # for the next attempt.
            error = f"the download did not finish within {INSTALL_TIMEOUT:g} s and was stopped"
            continue
        result = helper_result(done, argv)
        if "load_error" in result:
            raise Failure(f"/app/librust_tts_wrapper.so does not load: {result['load_error']}")
        if not result.get("installed"):
            error = result.get("error")
            kind = install_failure_kind(error)
            if kind == "checksum":
                raise Failure(
                    f"the download of {VOICE['id']} is not the file the catalogue pins: {error}"
                )
            if kind != "network":
                raise Failure(f"installing {VOICE['id']} failed: {error}")
            continue
        # The installer says it is done. Take its word for nothing: the
        # voice has to be there whole, and so has the pronunciation data,
        # which the installer only warns about for an English voice.
        problems = voice_problems(staging)
        if problems:
            raise Failure(
                f"the installer reported success but {VOICE['id']} is not installed", *problems
            )
        warning = bundle_warning(done.stderr)
        if warning and "checksum mismatch" in warning:
            raise Failure(
                f"the pronunciation data of {VOICE['id']} is not the file its manifest "
                f"pins: {warning}"
            )
        missing, wrong = lexicon_problems(staging)
        if wrong:
            raise Failure(
                f"the pronunciation data of {VOICE['id']} is not what was fetched", *wrong
            )
        if not missing:
            return attempt, result["seconds"]
        error = (
            "the model was installed but its pronunciation data is incomplete, missing: "
            + ", ".join(missing)
            + (f"; the installer said: {warning}" if warning else "")
        )
    suite.voice_unreachable = (
        f"the voice could not be downloaded in {INSTALL_ATTEMPTS} attempts ({error})"
    )
    raise Unreachable(
        suite.voice_unreachable + ". Not a failure of the build: the download server "
        "did not deliver. Run again to have this checked"
    )


@check(
    "speech-synthesis", "the speech library synthesizes audible speech offline", tags=("sandbox",)
)
def check_speech_synthesis(suite):
    suite.need_sandbox()
    # Only a complete voice is spoken with: a cache that lacks a part (a run
    # that was killed, a copy an older version of this script left) would
    # fail here for a reason that has nothing to do with the build.
    unusable = suite.cache_problems()
    if unusable:
        left = (
            f"no earlier run left one in {suite.voice_cache()}"
            if not suite.voice_home().is_dir()
            else f"the one in {suite.voice_home()} cannot be used ({'; '.join(unusable)})"
        )
        if suite.voice_unreachable:
            raise Unreachable(f"no voice to speak with: {suite.voice_unreachable}, and {left}")
        raise Skip(
            f"no verified copy of the voice {VOICE['id']} ({left}): "
            "run once with --network to install it"
        )
    out = suite.workdir() / "speech.wav"
    out.unlink(missing_ok=True)
    result = suite.sandbox_json(
        [
            "python3",
            "/run/dasher-smoke/speech.py",
            "synth",
            VOICE["id"],
            "/run/dasher-smoke/speech.wav",
            SYNTH_TEXT,
        ],
        binds=[("/run/dasher-voices", suite.voice_home())],
        env=suite.speech_env(),
        timeout=300,
    )
    if "load_error" in result:
        raise Failure(f"/app/librust_tts_wrapper.so does not load: {result['load_error']}")
    if not result.get("created"):
        raise Failure(
            f"creating the sherpaonnx engine for {VOICE['id']} failed: {result.get('error')}"
        )
    if result.get("synth_status") != 0 or not out.is_file():
        raise Failure(f"synthesis failed: {result.get('error')}")
    try:
        stats = wav_stats(out)
    except (wave.Error, EOFError) as e:
        raise Failure(f"the synthesized data is not a WAV file: {e}") from None
    problems = audio_problems(stats)
    if problems:
        raise Failure(*problems)
    return (
        f"{stats['seconds']:.2f} s at {stats['rate']} Hz, RMS {stats['rms']:.0f}, "
        f"synthesized in {result['synth_seconds']} s without network"
    )


@check(
    "audio-device",
    "the default ALSA device opens through the sound server socket (needs --host-checks)",
    tags=("sandbox", "host"),
)
def check_audio_device(suite):
    suite.need_host("the sound server of the desktop")
    runtime_dir = os.environ.get("XDG_RUNTIME_DIR", "")
    if not (runtime_dir and os.path.exists(f"{runtime_dir}/pulse/native")):
        raise Skip("no PulseAudio/PipeWire socket on this host ($XDG_RUNTIME_DIR/pulse/native)")
    suite.need_sandbox()
    if "pulseaudio" not in suite.declared("sockets"):
        raise Failure(
            "the build does not declare --socket=pulseaudio: the default ALSA device is the "
            "PulseAudio plugin and cannot open without the socket"
        )
    result = suite.sandbox_json(
        ["python3", "/run/dasher-smoke/alsa.py"], sockets=["pulseaudio"], timeout=60
    )
    if result["status"] != 0:
        raise Failure(f'snd_pcm_open("default") failed: {result["error"]}')
    return 'snd_pcm_open("default") succeeds with the sockets the build declares'


@check(
    "input-devices",
    "/dev/input is visible with the devices the build declares (needs --host-checks)",
    tags=("sandbox", "host"),
)
def check_input_devices(suite):
    suite.need_host("the devices of this machine")
    if not os.path.isdir("/dev/input"):
        raise Skip("this host has no /dev/input")
    suite.need_sandbox()
    devices = suite.declared("devices")
    done = suite.sandbox(["sh", "-c", "ls /dev/input"], devices=devices, timeout=60)
    if done.returncode != 0:
        raise Failure(
            f"/dev/input is not visible in the sandbox with --device={','.join(devices) or '(none)'}: "
            "SDL cannot open joysticks or switch interfaces"
        )
    return f"visible with --device={','.join(devices)} ({len(done.stdout.split())} entries)"


# ══ Checks: flatpak-builder-lint ══════════════════════════════════════════


def lint_command(suite):
    """How to run flatpak-builder-lint here, as an argv prefix."""
    override = os.environ.get("FLATPAK_BUILDER_LINT")
    if override:
        return shlex.split(override)
    if shutil.which("flatpak-builder-lint"):
        return ["flatpak-builder-lint"]
    if shutil.which("flatpak"):
        try:
            probe = run_command(["flatpak", "info", "org.flatpak.Builder"], timeout=TOOL_TIMEOUT)
            found = probe.returncode == 0
        except Failure:  # a flatpak that cannot be asked has no linter to offer
            found = False
        if found:
            # The linter only reads: give it both trees read-only (also for
            # a build directory under /tmp, which the app would not see),
            # and the work directory to run in.
            cmd = ["flatpak", "run", "--command=flatpak-builder-lint"]
            cmd.append(f"--filesystem={suite.workdir()}")
            cmd.append(f"--filesystem={suite.repo}:ro")
            if suite.build_dir:
                cmd.append(f"--filesystem={suite.build_dir}:ro")
            return cmd + ["org.flatpak.Builder"]
    raise Skip(
        "flatpak-builder-lint is not available (install org.flatpak.Builder from Flathub, "
        "or set FLATPAK_BUILDER_LINT)"
    )


def run_lint(suite, kind, target):
    suite.need_host("the network, and the session bus when the linter is a Flatpak")
    cmd = lint_command(suite) + [kind, str(target)]
    # In the work directory: for a manifest the linter runs
    # `flatpak-builder --show-manifest`, which leaves a .flatpak-builder
    # state directory where it is started.
    done = run_command(
        cmd, timeout=LINT_TIMEOUT, cwd=suite.workdir(), what=f"flatpak-builder-lint {kind}"
    )
    try:
        report = json.loads(done.stdout) if done.stdout.strip() else {}
    except json.JSONDecodeError:
        tail = " | ".join((done.stderr or done.stdout).strip().splitlines()[-3:])
        raise Failure(
            f"flatpak-builder-lint {kind} printed no JSON (status {done.returncode}): {tail}"
        ) from None
    if done.returncode != 0 and not report.get("errors"):
        tail = " | ".join(done.stderr.strip().splitlines()[-3:])
        raise Failure(f"flatpak-builder-lint {kind} exited with status {done.returncode}: {tail}")
    problems, notes = lint_verdict(kind, report)
    for note in notes:
        suite.note(note)
    if problems:
        raise Failure(*problems)
    accepted = sorted(set(report.get("errors", [])))
    return (
        f"no unexpected error ({len(accepted)} accepted: {', '.join(accepted)})"
        if accepted
        else "no errors"
    )


@check(
    "lint-manifest",
    "flatpak-builder-lint manifest reports no new error (needs --host-checks)",
    tags=("lint", "host"),
)
def check_lint_manifest(suite):
    manifest = suite.repo / f"packaging/flatpak/{APP_ID}.yaml"
    if not manifest.is_file():
        raise Skip(f"{manifest} does not exist")
    return run_lint(suite, "manifest", manifest)


@check(
    "lint-builddir",
    "flatpak-builder-lint builddir reports no new error (needs --host-checks)",
    tags=("lint", "host"),
)
def check_lint_builddir(suite):
    return run_lint(suite, "builddir", suite.build_dir)


@check(
    "installed-permissions", "the installed app has the expected permissions (needs --installed)"
)
def check_installed_permissions(suite):
    app = suite.options.installed
    if not app:
        raise Skip(
            "no installed app named; pass --installed APP_ID after `flatpak-builder --install`"
        )
    if not shutil.which("flatpak"):
        raise Skip("flatpak is not installed")
    done = run_command(
        ["flatpak", "info", "--show-metadata", app],
        timeout=TOOL_TIMEOUT,
        what=f"`flatpak info {app}`",
    )
    if done.returncode != 0:
        raise Failure(f"`flatpak info {app}` failed: {done.stderr.strip()}")
    try:
        problems = compare_permissions(permissions_from_metadata(read_metadata(done.stdout)))
    except ValueError as e:
        problems = [f"the installed metadata grants something this check does not know: {e}"]
    if problems:
        raise Failure(*problems)
    return f"{app} as installed has the {len(EXPECTED_FINISH_ARGS)} expected permissions"


# ══ Running ═══════════════════════════════════════════════════════════════

# Checks that need the build directory (all but lint-manifest could not
# run without one; --list and unit tests work without).
NEEDS_NO_BUILD_DIR = {"lint-manifest", "installed-permissions"}


def run_checks(suite, selected, out=None):
    """Run the checks named in `selected`, print one line each and return
    {"passed": [...], "failed": [...], "skipped": [...], "unreachable":
    [...]}; the last are the skipped checks whose server did not answer."""
    out = sys.stdout if out is None else out
    actions = os.environ.get("GITHUB_ACTIONS") == "true"
    results = {"passed": [], "failed": [], "skipped": [], "unreachable": []}
    for entry in CHECKS:
        name = entry["name"]
        if name not in selected:
            continue
        suite.notes = []
        details = []
        try:
            if suite.build_dir is None and name not in NEEDS_NO_BUILD_DIR:
                raise Skip("no build directory given")
            message = entry["run"](suite)
            status = "PASS"
            results["passed"].append(name)
        except Skip as e:
            status, message = "SKIP", str(e)
            results["skipped"].append(name)
            if isinstance(e, Unreachable):
                results["unreachable"].append(name)
        except Failure as e:
            status, message = "FAIL", entry["summary"]
            details = e.problems
            results["failed"].append(name)
        except Exception as e:  # a broken check is a failed check, with the reason
            status, message = "FAIL", entry["summary"]
            details = [f"the check itself broke: {type(e).__name__}: {e}"]
            results["failed"].append(name)
        print(f"{status}  {name}: {message}", file=out, flush=True)
        if actions and name in results["unreachable"]:
            # --strict lets this skip through, so it must not go unseen.
            line = " ".join(message.split())
            print(
                f"::warning title=Flatpak smoke test: {name}::SKIPPED, not verified: {line}",
                file=out,
            )
        # One line each: an annotation ends at the first newline.
        for detail in (" ".join(str(d).split()) for d in details):
            print(f"        - {detail}", file=out)
            if actions:
                print(f"::error title=Flatpak smoke test: {name}::{detail}", file=out)
        for note in (" ".join(str(n).split()) for n in suite.notes):
            print(f"        note: {note}", file=out)
            if actions:
                print(f"::warning title=Flatpak smoke test: {name}::{note}", file=out)
    return results


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Smoke tests for a built Dasher Flatpak (see the top of this file).",
        allow_abbrev=False,
    )
    parser.add_argument(
        "build_dir", nargs="?", help="flatpak-builder build directory (holds metadata and files/)"
    )
    parser.add_argument(
        "--repo",
        default=str(HERE.parents[2]),
        help="Dasher-GTK checkout to compare with (default: the one this script is in)",
    )
    parser.add_argument("--list", action="store_true", help="list the checks and exit")
    parser.add_argument(
        "--voice-key",
        action="store_true",
        help="print a key that changes when the pinned test voice does (for a CI cache) and exit",
    )
    parser.add_argument(
        "--check",
        action="append",
        default=[],
        metavar="NAME",
        help="run only this check (repeatable, or comma-separated)",
    )
    parser.add_argument(
        "--skip",
        action="append",
        default=[],
        metavar="NAME",
        help="do not run this check (repeatable, or comma-separated)",
    )
    parser.add_argument(
        "--host-checks",
        action="store_true",
        help="also run the checks that reach outside the sandbox: flatpak-builder-lint "
        "(network; session bus when it is a Flatpak), the sound server, /dev/input",
    )
    parser.add_argument(
        "--network", action="store_true", help="also run the check that downloads the test voice"
    )
    parser.add_argument(
        "--voice-cache",
        metavar="DIR",
        help="where the test voice is kept (default: $XDG_CACHE_HOME/dasher-flatpak-smoke)",
    )
    parser.add_argument(
        "--installed",
        metavar="APP_ID",
        help="also check the permissions of this installed app (read with `flatpak info`, never run)",
    )
    parser.add_argument(
        "--alive-seconds",
        type=float,
        default=5.0,
        metavar="N",
        help="how long the app has to stay alive after taking its bus name (default 5)",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="a skipped check fails the run, unless it is named with --allow-skip or its "
        "download server could not be reached (that is reported with a warning)",
    )
    parser.add_argument(
        "--allow-skip",
        action="append",
        default=[],
        metavar="NAME",
        help="with --strict: this check may be skipped (repeatable, or comma-separated)",
    )
    parser.add_argument(
        "--keep-logs",
        metavar="DIR",
        help="copy the app's log, the helpers' results and the synthesized WAV here",
    )
    parser.add_argument("-v", "--verbose", action="store_true", help="print the sandbox commands")
    options = parser.parse_args(argv)
    for key in ("check", "skip", "allow_skip"):
        setattr(options, key, [n for value in getattr(options, key) for n in value.split(",") if n])
    known = {entry["name"] for entry in CHECKS}
    unknown = [n for n in options.check + options.skip + options.allow_skip if n not in known]
    if unknown:
        parser.error(f"unknown check(s): {', '.join(unknown)} (see --list)")
    if not (options.list or options.voice_key) and not options.build_dir:
        if not options.check or not set(options.check) <= NEEDS_NO_BUILD_DIR:
            parser.error(
                "BUILD_DIR is required (only lint-manifest and installed-permissions run without)"
            )
    if options.build_dir and not (Path(options.build_dir) / "metadata").is_file():
        parser.error(
            f"{options.build_dir} has no `metadata` file: not a finished flatpak-builder build directory"
        )
    return options


def main(argv=None):
    options = parse_args(sys.argv[1:] if argv is None else argv)
    if options.voice_key:
        print(f"{VOICE['id']}-{VOICE['archive_sha256'][:16]}-{VOICE['model_sha256'][:16]}")
        return 0
    if options.list:
        width = max(len(entry["name"]) for entry in CHECKS)
        for entry in CHECKS:
            tags = f" [{', '.join(entry['tags'])}]" if entry["tags"] else ""
            print(f"{entry['name']:<{width}}  {entry['summary']}{tags}")
        return 0
    selected = [
        entry["name"] for entry in CHECKS if not options.check or entry["name"] in options.check
    ]
    selected = [name for name in selected if name not in options.skip]
    suite = Suite(options.build_dir, options.repo, options)
    try:
        results = run_checks(suite, selected)
    finally:
        suite.cleanup()
    passed, failed, skipped = (len(results[key]) for key in ("passed", "failed", "skipped"))
    print(f"\nSummary: {passed} passed, {failed} failed, {skipped} skipped (of {len(selected)})")
    not_allowed = [
        name
        for name in results["skipped"]
        if name not in options.allow_skip and name not in results["unreachable"]
    ]
    if failed:
        print(f"Failed: {', '.join(results['failed'])}")
    if results["unreachable"]:
        print(f"Skipped, server not reachable: {', '.join(results['unreachable'])}")
    if options.strict and not_allowed:
        print(f"Skipped but required (--strict): {', '.join(not_allowed)}")
        return 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
