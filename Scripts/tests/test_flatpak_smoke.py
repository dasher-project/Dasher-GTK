#!/usr/bin/env python3
"""Tests for packaging/flatpak/tests/smoke-test.py that need no Flatpak.

The smoke test itself wants a finished build, which only the publish
workflow has. What decides between PASS and FAIL in it is plain Python,
though, and that part is tested here on every pull request:

  - the checks that only read the build directory, on a made-up build
    directory that passes, broken in one way at a time;
  - the judging of what the sandbox helpers report (the headless start,
    the speech library, ldd, the linter), fed with canned results;
  - the `flatpak build` command line: a finished build directory carries
    the app's finish-args, so every sandbox has to switch them off again,
    or the app under test would open a window on the desktop;
  - the command line, the output format and the exit status;
  - the helpers under sandbox/ and mutation-test.py.

No test starts a sandbox: `subprocess.run` refuses every `flatpak` command
that a test has not scripted, and `shutil.which` finds no host tool unless
a test provides one. Nothing touches the network.

Run from anywhere; stdlib only:

    python3 -m unittest discover -s Scripts/tests -v

Environment:

  FLATPAK_SMOKE_TESTS_DIR=/path/to/a/copy/of/packaging/flatpak/tests
      Test another copy of smoke-test.py, mutation-test.py and sandbox/,
      for example one with a deliberate mistake, to see these tests fail.
"""

import array
import contextlib
import copy
import ctypes
import hashlib
import importlib.util
import io
import json
import math
import os
import py_compile
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import wave
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parent.parent.parent
TESTS_DIR = Path(
    os.environ.get("FLATPAK_SMOKE_TESTS_DIR") or REPO / "packaging" / "flatpak" / "tests"
).resolve()

# The real functions: the guards below pass everything through to them
# that is not a `flatpak` command or a host tool.
REAL_RUN = subprocess.run
REAL_WHICH = shutil.which
REAL_ISDIR = os.path.isdir


def load_script(module_name, path):
    """Import a script whose file name is not a module name."""
    spec = importlib.util.spec_from_file_location(module_name, path)
    module = importlib.util.module_from_spec(spec)
    write_bytecode, sys.dont_write_bytecode = sys.dont_write_bytecode, True
    try:
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = write_bytecode
    return module


smoke = load_script("flatpak_smoke_test", TESTS_DIR / "smoke-test.py")
mutation = load_script("flatpak_mutation_test", TESTS_DIR / "mutation-test.py")
headless = load_script("flatpak_headless_helper", TESTS_DIR / "sandbox" / "headless.py")

APP_ID = smoke.APP_ID
REAL_METAINFO = REPO / "packaging" / f"{APP_ID}.metainfo.xml"
REAL_DESKTOP = REPO / "packaging" / f"{APP_ID}.desktop"
LICENCES = f"share/licenses/{APP_ID}"

# The checks that read the build directory and nothing else.
STATIC_CHECKS = [
    "permissions",
    "files",
    "licences",
    "cleanup",
    "desktop-file",
    "metainfo",
    "screenshot",
    "release",
    "rpath",
    "speech-from-source",
]

GCC_IDENT = "GCC: (GNU) 16.2.0"
ORT_VERSION = "1.28.2"
ORT_BUILD_INFO = "ORT Build Info: git-branch=, git-commit-id=, fp8-kv-cache=1, build type=Release"
LAUNCHER = (
    "#!/bin/sh\n"
    'export LD_LIBRARY_PATH="/app:/app/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n'
    'exec /app/dasher "$@"\n'
)
BROADWAY_WARNING = (
    "(dasher:12): Gdk-WARNING **: 10:12:59.712: Duplicate call to "
    "gdk_frame_timings_submitted() with different values."
)


# ── Fixture helpers ─────────────────────────────────────────────────────


def make_elf(needed=(), rpath=(), runpath=(), comment=(), extra=b"", bits=64, order="<"):
    """A minimal ELF file with the sections smoke-test.py reads: .dynamic
    (with its string table) and .comment. `extra` is appended to the section
    data, for strings that a check looks for anywhere in the file."""
    dynstr = b"\0"
    entries = []

    def string(text):
        nonlocal dynstr
        at = len(dynstr)
        dynstr += text.encode() + b"\0"
        return at

    for name in needed:
        entries.append((1, string(name)))  # DT_NEEDED
    if rpath:
        entries.append((15, string(":".join(rpath))))  # DT_RPATH
    if runpath:
        entries.append((29, string(":".join(runpath))))  # DT_RUNPATH
    entries.append((0, 0))  # DT_NULL
    dyn = struct.Struct(order + ("qQ" if bits == 64 else "iI"))
    dynamic = b"".join(dyn.pack(tag, value) for tag, value in entries)
    comment_data = b"".join(c.encode() + b"\0" for c in comment)
    shstrtab, names = b"\0", {}
    for name in (".dynstr", ".dynamic", ".comment", ".shstrtab"):
        names[name] = len(shstrtab)
        shstrtab += name.encode() + b"\0"

    header_size = 64 if bits == 64 else 52
    body, where = b"", {}
    for name, data in (
        (".dynstr", dynstr),
        (".dynamic", dynamic),
        (".comment", comment_data),
        (".shstrtab", shstrtab),
    ):
        where[name] = (header_size + len(body), len(data))
        body += data
    body += extra

    def section(name, kind, offset, size, link):
        layout = "IIQQQQIIQQ" if bits == 64 else "IIIIIIIIII"
        return struct.pack(order + layout, name, kind, 0, 0, offset, size, link, 0, 1, 0)

    table = (
        section(0, 0, 0, 0, 0)
        + section(names[".dynstr"], 3, *where[".dynstr"], 0)
        + section(names[".dynamic"], 6, *where[".dynamic"], 1)
        + section(names[".comment"], 1, *where[".comment"], 0)
        + section(names[".shstrtab"], 3, *where[".shstrtab"], 0)
    )
    ident = b"\x7fELF" + bytes([2 if bits == 64 else 1, 1 if order == "<" else 2, 1]) + b"\0" * 9
    shoff = header_size + len(body)
    if bits == 64:
        header = ident + struct.pack(
            order + "HHIQQQIHHHHHH", 3, 62, 1, 0, 0, shoff, 0, 64, 0, 0, 64, 5, 4
        )
    else:
        header = ident + struct.pack(
            order + "HHIIIIIHHHHHH", 3, 3, 1, 0, 0, shoff, 0, 52, 0, 0, 40, 5, 4
        )
    return header + body + table


def metadata_text(finish_args=None, name=APP_ID, command="dasher", extra=""):
    """A `metadata` file as flatpak-builder writes it for `finish_args`."""
    finish_args = smoke.EXPECTED_FINISH_ARGS if finish_args is None else finish_args
    keys = {
        "--share": "shared",
        "--socket": "sockets",
        "--device": "devices",
        "--filesystem": "filesystems",
        "--allow": "features",
        "--persist": "persistent",
    }
    context = {name: [] for name in keys.values()}
    session = []
    for arg in finish_args:
        key, _, value = arg.partition("=")
        if key in keys:
            context[keys[key]].append(value)
        elif key in ("--own-name", "--talk-name"):
            session.append(f"{value}={key[2:-5]}")
        else:
            raise ValueError(arg)
    lines = [
        "[Application]",
        f"name={name}",
        "runtime=org.gnome.Platform/x86_64/51",
        "sdk=org.gnome.Sdk/x86_64/51",
        f"command={command}",
        "",
        "[Context]",
    ]
    lines += [
        f"{key}={''.join(v + ';' for v in values)}" for key, values in context.items() if values
    ]
    lines += ["", "[Session Bus Policy]"] + session
    lines += [
        "",
        f"[Extension {APP_ID}.Debug]",
        "directory=lib/debug",
        "autodelete=true",
        "no-autodownload=true",
        "",
        "[Build]",
        f"built-extensions={APP_ID}.Debug;",
        "",
    ]
    return "\n".join(lines) + extra


def manifest_data(finish_args=None):
    """What flatpak-builder records as files/manifest.json, cut down."""
    return {
        "id": APP_ID,
        "command": "dasher",
        "finish-args": list(smoke.EXPECTED_FINISH_ARGS if finish_args is None else finish_args),
        "cleanup": ["/include", "*.a", "*.la", "/sherpa-onnx"],
        "modules": [
            {"name": "mm-common", "buildsystem": "meson"},
            {
                "name": "onnxruntime",
                "build-options": {"cflags": "-g0"},
                "build-commands": [
                    f'test "$(cat VERSION_NUMBER)" = {ORT_VERSION}',
                    "cmake -S cmake",
                ],
            },
            {
                "name": "dasher",
                "build-options": {"env": {"CARGO_NET_OFFLINE": "true"}},
                "build-commands": ["cmake -B _build"],
            },
        ],
    }


def write(path, content=b"x", mode=None):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(content, str):
        path.write_text(content)
    else:
        path.write_bytes(content)
    if mode is not None:
        path.chmod(mode)
    return path


def real_release_version():
    """The newest release of the repository's metainfo: the fixture's
    binary and CMakeLists.txt are made to agree with it."""
    return smoke.metainfo_releases(ET.parse(REAL_METAINFO).getroot())[0][0]


def real_screenshot_path():
    """Path in the repository of the first screenshot the metainfo names."""
    image = ET.parse(REAL_METAINFO).getroot().find("./screenshots/screenshot/image")
    return smoke.pinned_screenshot_url(image.text.strip())[1]


class Fixture:
    """A made-up build directory that passes every check that needs no
    sandbox, and a made-up checkout to compare it with."""

    def __init__(self, root):
        self.root = Path(root)
        self.build = self.root / "build"
        self.files = self.build / "files"
        self.repo = self.root / "repo"
        self.version = real_release_version()
        self.metainfo = self.files / f"share/metainfo/{APP_ID}.metainfo.xml"
        self.desktop = self.files / f"share/applications/{APP_ID}.desktop"
        self.screenshot = real_screenshot_path()
        # Command line arguments every suite of this fixture gets.
        self.arguments = []
        self._populate()

    def _populate(self):
        files = self.files
        write(self.build / "metadata", metadata_text())
        write(files / "manifest.json", json.dumps(manifest_data()))
        write(files / "bin/dasher", LAUNCHER, mode=0o755)
        write(
            files / "dasher",
            make_elf(
                needed=["libdasher.so", "librust_tts_wrapper.so", "libc.so.6"],
                rpath=["$ORIGIN"],
                comment=[GCC_IDENT],
                extra=b"\0" + self.version.encode() + b"\0",
            ),
            mode=0o755,
        )
        write(files / "libdasher.so", make_elf(needed=["libc.so.6"], comment=[GCC_IDENT]))
        write(files / "librust_tts_wrapper.so", self.speech_library())
        write(files / "lib/libgtkmm-4.0.so.0.0.0", make_elf(needed=["libc.so.6"]))
        os.symlink("libgtkmm-4.0.so.0.0.0", files / "lib/libgtkmm-4.0.so.0")
        # Detached debug info is not part of the app: its odd RPATH must
        # not be looked at.
        write(files / "lib/debug/dasher.debug", make_elf(rpath=["/run/build/dasher"]))
        shutil.copy(REAL_DESKTOP, write(self.desktop))
        shutil.copy(REAL_METAINFO, write(self.metainfo))
        write(files / f"share/icons/hicolor/48x48/apps/{APP_ID}.png", b"\x89PNG fake")
        write(files / f"share/icons/hicolor/scalable/apps/{APP_ID}.svg", "<svg/>")
        write(files / LICENCES / "onnxruntime/LICENSE", "MIT\n")
        write(files / LICENCES / "onnxruntime/ThirdPartyNotices.txt", "notice\n" * 10000)
        write(files / LICENCES / "sherpa-onnx/LICENSE", "Apache-2.0\n")
        for name in smoke.SHERPA_DEP_LICENCE_DIRS:
            write(files / LICENCES / "sherpa-onnx" / name / "LICENSE", f"licence of {name}\n")
        write(files / "Data/alphabet.english.xml", "<alphabets/>")
        for relative in smoke.EXPECTED_EXPORTS:
            write(self.build / "export" / relative)
        # The checkout: only what the checks read from it.
        shutil.copy(REAL_METAINFO, write(self.repo / "packaging" / REAL_METAINFO.name))
        shutil.copy(REPO / self.screenshot, write(self.repo / self.screenshot))
        write(self.repo / "CMakeLists.txt", self.cmake(self.version))

    @staticmethod
    def cmake(version):
        return (
            "project(DasherGTK)\n"
            f'set(DASHER_GTK_FALLBACK_VERSION "{version}" CACHE STRING "fallback")\n'
        )

    @staticmethod
    def speech_library(build_info=ORT_BUILD_INFO, version=ORT_VERSION, comment=None, needed=None):
        extra = b"\0"
        if version:
            extra += version.encode() + b"\0"
        if build_info:
            extra += build_info.encode() + b"\0"
        return make_elf(
            needed=["libasound.so.2", "libc.so.6"] if needed is None else needed,
            comment=(
                ["Linker: LLD 22.1.8", GCC_IDENT, "rustc version 1.98.0"]
                if comment is None
                else comment
            ),
            extra=extra,
        )

    def edit(self, path, old, new):
        path = Path(path)
        text = path.read_text()
        if old not in text:
            raise AssertionError(f"{old!r} is not in {path}")
        path.write_text(text.replace(old, new))

    def edit_metainfo(self, old, new):
        self.edit(self.metainfo, old, new)

    def with_manifest(self):
        """Put a manifest into the checkout, for the linter to be given."""
        write(self.repo / f"packaging/flatpak/{APP_ID}.yaml", "app-id: x\n")
        return self

    def edit_metadata(self, old, new):
        self.edit(self.build / "metadata", old, new)

    def cut_metainfo(self, tag):
        """Take the element <tag>...</tag> out of the metainfo."""
        text = self.metainfo.read_text()
        start, end = text.index(f"<{tag}>"), text.index(f"</{tag}>") + len(f"</{tag}>")
        self.metainfo.write_text(text[:start] + text[end:])

    def set_metadata(self, finish_args=None, **kwargs):
        (self.build / "metadata").write_text(metadata_text(finish_args, **kwargs))

    def set_manifest(self, change):
        data = manifest_data()
        change(data)
        (self.files / "manifest.json").write_text(json.dumps(data))

    def suite(self, *arguments):
        options = smoke.parse_args(
            [str(self.build), "--repo", str(self.repo), *self.arguments, *arguments]
        )
        return smoke.Suite(self.build, self.repo, options)


def completed(stdout="", returncode=0, stderr=""):
    return subprocess.CompletedProcess([], returncode, stdout, stderr)


def status_lines(output):
    """{check: its PASS, SKIP or FAIL line} of a run's output, in order."""
    return {
        line.split()[1].rstrip(":"): line
        for line in output.splitlines()
        if line[:4] in ("PASS", "SKIP", "FAIL")
    }


def check_named(name):
    """The entry of smoke.CHECKS for the check `name`."""
    return next(entry for entry in smoke.CHECKS if entry["name"] == name)


def shared(cmd):
    """The positive permissions of a `flatpak build` command, sorted."""
    switches = ("--socket=", "--device=", "--share=", "--filesystem=", "--allow=")
    return sorted(arg for arg in cmd if arg.startswith(switches))


def bound(argv, destination):
    """The host directory a `flatpak build` command binds at `destination`."""
    prefix = f"--bind-mount={destination}="
    return next(Path(arg[len(prefix) :]) for arg in argv if arg.startswith(prefix))


class SmokeTestCase(unittest.TestCase):
    """Base class: a temporary directory, no host tools, and no `flatpak`
    command that the test has not scripted."""

    # Arguments for every suite of a fixture(); the classes that test the
    # checks behind --host-checks set it.
    ARGUMENTS = ()
    # The check a class is about: what passes(), fails(), skips() and
    # broken() run.
    CHECK = None

    def patch(self, patcher):
        """Start a mock.patch for the rest of the test."""
        self.addCleanup(patcher.stop)
        return patcher.start()

    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="dasher-smoke-unit-")
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        # Work directories of the script under test land in there too, so
        # that a test which forgets to clean one up leaves nothing behind.
        scratch = self.tmp / "tmp"
        scratch.mkdir()
        self.patch(mock.patch.object(tempfile, "tempdir", str(scratch)))
        self._fixtures = 0
        # name -> path for shutil.which; everything else is "not installed".
        self.tools = {}
        # Called with (argv, kwargs) for every `flatpak` command; None means
        # that the test expects none.
        self.flatpak = None
        self.flatpak_calls = []
        # Pauses the script asks for (between download attempts): recorded,
        # not waited for.
        self.sleeps = []
        for module, name, replacement in (
            (smoke.shutil, "which", self._which),
            (smoke.subprocess, "run", self._run),
            (smoke.time, "sleep", self.sleeps.append),
        ):
            self.patch(mock.patch.object(module, name, side_effect=replacement))

    def _which(self, name, *args, **kwargs):
        return self.tools.get(name)

    def _run(self, cmd, *args, **kwargs):
        if isinstance(cmd, (list, tuple)) and cmd and cmd[0] == "flatpak":
            self.flatpak_calls.append((list(cmd), kwargs))
            if self.flatpak is None:
                raise AssertionError(f"the test started flatpak: {cmd}")
            return self.flatpak(list(cmd), kwargs)
        return REAL_RUN(cmd, *args, **kwargs)

    def use_flatpak(self, handler=None):
        """Pretend flatpak is installed; `handler(argv, kwargs)` answers
        the commands (default: every command succeeds silently)."""
        self.tools["flatpak"] = "/usr/bin/flatpak"
        self.flatpak = handler or (lambda argv, kwargs: completed())

    def fake_host(self):
        """A machine on which the checks behind --host-checks can run: a
        sound server socket in $XDG_RUNTIME_DIR (self.runtime_dir), and a
        /dev/input for as long as self.host_has_input is true."""
        self.runtime_dir = self.tmp / "run"
        write(self.runtime_dir / "pulse/native")
        self.patch(mock.patch.dict(os.environ, {"XDG_RUNTIME_DIR": str(self.runtime_dir)}))
        self.host_has_input = True
        self.patch(
            mock.patch.object(
                smoke.os.path,
                "isdir",
                side_effect=lambda p: self.host_has_input if p == "/dev/input" else REAL_ISDIR(p),
            )
        )

    def linter(self, report, status=0, stderr=""):
        """A fake flatpak-builder-lint, named by $FLATPAK_BUILDER_LINT. It
        prints `report` and leaves its arguments in lint.args, its working
        directory in lint.cwd and a line per run in lint.ran."""
        output = report if isinstance(report, str) else json.dumps(report)
        write(self.tmp / "lint-output", output)
        script = write(
            self.tmp / "bin/fake-lint",
            "#!/bin/sh\n"
            f"echo \"$@\" > '{self.tmp}/lint.args'\n"
            f"pwd > '{self.tmp}/lint.cwd'\n"
            f"echo ran >> '{self.tmp}/lint.ran'\n"
            f"cat '{self.tmp}/lint-output'\n"
            f"echo '{stderr}' >&2\n"
            f"exit {status}\n",
            mode=0o755,
        )
        self.patch(mock.patch.dict(os.environ, {"FLATPAK_BUILDER_LINT": f"{script} --flag"}))

    def use_sandbox(self, handler):
        """Pretend flatpak is installed and its probe (`true`) succeeds;
        `handler(argv, kwargs)` answers every other command."""
        self.use_flatpak(
            lambda argv, kwargs: completed() if argv[-1] == "true" else handler(argv, kwargs)
        )

    def fixture(self, finish_args=None):
        """A build that passes, or one with the permissions `finish_args`."""
        self._fixtures += 1
        fixture = Fixture(self.tmp / f"fixture-{self._fixtures}")
        fixture.arguments = list(self.ARGUMENTS)
        if finish_args is not None:
            fixture.set_metadata(finish_args)
        return fixture

    def outcome(self, suite, name):
        """("PASS", message), ("SKIP", reason) or ("FAIL", [problems])."""
        suite.notes = []
        self.addCleanup(suite.cleanup)
        try:
            return "PASS", check_named(name)["run"](suite)
        except smoke.Skip as e:
            return "SKIP", str(e)
        except smoke.Failure as e:
            return "FAIL", e.problems

    def assertPasses(self, suite, name):
        status, detail = self.outcome(suite, name)
        self.assertEqual(status, "PASS", detail)
        return detail

    def assertSkips(self, suite, name, pattern):
        status, detail = self.outcome(suite, name)
        self.assertEqual(status, "SKIP", detail)
        self.assertRegex(detail, pattern)
        return detail

    def assertFails(self, suite, name, pattern):
        """The check fails and one of its problems matches `pattern`."""
        status, detail = self.outcome(suite, name)
        self.assertEqual(status, "FAIL", detail)
        if not any(re.search(pattern, problem) for problem in detail):
            self.fail(f"no problem matches {pattern!r}: {detail}")
        return detail

    def assertBroken(self, name, pattern, breakage, *arguments):
        """A fresh fixture, broken by `breakage(fixture)`, fails `name`."""
        fixture = self.fixture()
        breakage(fixture)
        return self.assertFails(fixture.suite(*arguments), name, pattern)

    def assertOneNote(self, suite, text):
        """The check that just ran on `suite` left one note, with `text`."""
        self.assertEqual(len(suite.notes), 1)
        self.assertIn(text, suite.notes[0])

    def noted(self, text, change):
        """A fresh fixture, changed by `change(fixture)`, still passes the
        check of the class, with one note that has `text`."""
        fixture = self.fixture()
        change(fixture)
        suite = fixture.suite()
        self.passes(suite)
        self.assertOneNote(suite, text)

    # The same for the check of the class, on a suite of a fresh fixture
    # unless one is given.
    def suite(self, *arguments):
        return self.fixture().suite(*arguments)

    def passes(self, suite=None):
        return self.assertPasses(suite or self.suite(), self.CHECK)

    def skips(self, pattern, suite=None):
        return self.assertSkips(suite or self.suite(), self.CHECK, pattern)

    def fails(self, pattern, suite=None):
        return self.assertFails(suite or self.suite(), self.CHECK, pattern)

    def broken(self, pattern, breakage, *arguments):
        return self.assertBroken(self.CHECK, pattern, breakage, *arguments)


# ══ The fixture itself ════════════════════════════════════════════════════


class FixtureTests(SmokeTestCase):
    def test_every_static_check_passes_on_the_fixture(self):
        fixture = self.fixture()
        suite = fixture.suite()
        for name in STATIC_CHECKS:
            with self.subTest(check=name):
                status, detail = self.outcome(suite, name)
                if name in ("desktop-file", "metainfo"):
                    # No validator on PATH in the tests: the rest was checked.
                    self.assertEqual(status, "SKIP", detail)
                    self.assertIn("is not installed", detail)
                else:
                    self.assertEqual(status, "PASS", detail)
                self.assertEqual(suite.notes, [])

    def test_static_checks_are_the_ones_without_a_tag(self):
        untagged = [e["name"] for e in smoke.CHECKS if not e["tags"]]
        self.assertEqual(untagged, STATIC_CHECKS + ["installed-permissions"])

    def test_the_metadata_helper_round_trips(self):
        metadata = smoke.read_metadata(metadata_text())
        self.assertEqual(smoke.permissions_from_metadata(metadata), set(smoke.EXPECTED_FINISH_ARGS))
        self.assertEqual(metadata["Application"]["name"], APP_ID)
        self.assertEqual(metadata["Session Bus Policy"]["org.a11y.*"], "talk")


# ══ ELF parsing ═══════════════════════════════════════════════════════════


class ElfTests(SmokeTestCase):
    def test_reads_needed_rpath_runpath_and_comment(self):
        for bits in (64, 32):
            for order in ("<", ">"):
                with self.subTest(bits=bits, order=order):
                    path = write(
                        self.tmp / f"elf-{bits}-{ord(order)}",
                        make_elf(
                            needed=["libdasher.so", "libc.so.6"],
                            rpath=["$ORIGIN", "/app/lib"],
                            runpath=["/usr/lib/x"],
                            comment=[GCC_IDENT, "rustc version 1.98.0"],
                            extra=b"trailing bytes",
                            bits=bits,
                            order=order,
                        ),
                    )
                    self.assertEqual(
                        smoke.elf_info(path),
                        {
                            "needed": ["libdasher.so", "libc.so.6"],
                            "rpath": ["$ORIGIN", "/app/lib"],
                            "runpath": ["/usr/lib/x"],
                            "comment": [GCC_IDENT, "rustc version 1.98.0"],
                        },
                    )

    def test_an_elf_file_without_dynamic_entries(self):
        path = write(self.tmp / "plain", make_elf())
        self.assertEqual(
            smoke.elf_info(path), {"needed": [], "rpath": [], "runpath": [], "comment": []}
        )

    def test_other_files_are_not_elf(self):
        for name, content in (
            ("text", b"#!/bin/sh\necho hello\n" * 10),
            ("empty", b""),
            ("short", b"\x7fELF\x02\x01\x01"),
            ("png", b"\x89PNG\r\n\x1a\n" + b"\0" * 100),
        ):
            with self.subTest(name):
                self.assertIsNone(smoke.elf_info(write(self.tmp / name, content)))

    def test_reads_the_running_interpreter(self):
        with open(sys.executable, "rb") as f:
            if f.read(4) != b"\x7fELF":
                self.skipTest("the Python interpreter is not an ELF file here")
        info = smoke.elf_info(sys.executable)
        self.assertEqual(set(info), {"needed", "rpath", "runpath", "comment"})
        for values in info.values():
            self.assertIsInstance(values, list)
            self.assertTrue(all(isinstance(value, str) for value in values))

    def test_app_elfs_leaves_out_symlinks_and_debug_info(self):
        fixture = self.fixture()
        self.assertEqual(
            [relative for relative, _ in fixture.suite().app_elfs()],
            ["dasher", "lib/libgtkmm-4.0.so.0.0.0", "libdasher.so", "librust_tts_wrapper.so"],
        )


# ══ permissions ═══════════════════════════════════════════════════════════


class PermissionTests(SmokeTestCase):
    CHECK = "permissions"

    def broken_metadata(self, pattern, finish_args=None, **kwargs):
        return self.broken(pattern, lambda f: f.set_metadata(finish_args, **kwargs))

    def test_passes_and_says_what_was_compared(self):
        message = self.passes()
        self.assertIn(f"{len(smoke.EXPECTED_FINISH_ARGS)} permissions", message)

    def test_an_added_permission_fails(self):
        for extra in (
            "--socket=session-bus",
            "--socket=system-bus",
            "--filesystem=host",
            "--filesystem=xdg-download:ro",
            "--device=shm",
            "--share=cups",
            "--own-name=org.example.Other",
            "--talk-name=org.freedesktop.Flatpak",
            "--allow=devel",
        ):
            with self.subTest(extra):
                problems = self.broken_metadata(
                    rf"unexpected permission {extra.replace('*', '.')}",
                    smoke.EXPECTED_FINISH_ARGS + [extra],
                )
                self.assertEqual(len(problems), 1, problems)
                reason = smoke.FORBIDDEN_FINISH_ARGS.get(extra)
                if reason:
                    self.assertIn(reason, problems[0])

    def test_x11_next_to_fallback_x11_is_what_older_flatpak_writes(self):
        # flatpak 1.14 and 1.16 record --socket=fallback-x11 as
        # sockets=x11;fallback-x11; 1.18 as fallback-x11 alone.
        fixture = self.fixture()
        fixture.set_metadata(smoke.EXPECTED_FINISH_ARGS + ["--socket=x11"])
        self.passes(fixture.suite())

    def test_x11_without_fallback_x11_fails(self):
        replaced = [
            "--socket=x11" if arg == "--socket=fallback-x11" else arg
            for arg in smoke.EXPECTED_FINISH_ARGS
        ]
        problems = self.broken_metadata(r"unexpected permission --socket=x11", replaced)
        self.assertTrue(any("missing permission --socket=fallback-x11" in p for p in problems))

    def test_x11_in_the_manifest_fails_even_next_to_fallback_x11(self):
        problems = self.broken(
            r"manifest finish-args: unexpected permission --socket=x11",
            lambda f: f.set_manifest(lambda data: data["finish-args"].append("--socket=x11")),
        )
        self.assertEqual(len(problems), 1, problems)

    def test_a_dropped_permission_fails(self):
        for dropped in smoke.EXPECTED_FINISH_ARGS:
            with self.subTest(dropped):
                rest = [arg for arg in smoke.EXPECTED_FINISH_ARGS if arg != dropped]
                problems = self.broken_metadata(r"missing permission", rest)
                self.assertEqual(len(problems), 1, problems)
                self.assertIn(f"missing permission {dropped} ", problems[0])

    def test_a_changed_bus_name_is_one_extra_and_one_missing(self):
        changed = [
            arg.replace("org.dasher.gtk", "org.dasher.other") for arg in smoke.EXPECTED_FINISH_ARGS
        ]
        problems = self.broken_metadata(
            r"unexpected permission --own-name=org.dasher.other", changed
        )
        self.assertTrue(any("missing permission --own-name=org.dasher.gtk" in p for p in problems))

    def test_sections_that_grant_something_else_fail(self):
        for label, extra, pattern in (
            (
                "system bus",
                "\n[System Bus Policy]\norg.freedesktop.login1=talk\n",
                r"unexpected permission --system-talk-name=org.freedesktop.login1",
            ),
            (
                "system bus own",
                "\n[System Bus Policy]\norg.example.Daemon=own\n",
                r"unexpected permission --system-own-name=org.example.Daemon",
            ),
            (
                "environment",
                "\n[Environment]\nGTK_DEBUG=interactive\n",
                r"unexpected permission --env=GTK_DEBUG=interactive",
            ),
            (
                "unknown section",
                "\n[Policy background]\nautostart=true\n",
                r"does not know: \[Policy background\] autostart=true",
            ),
            (
                "empty unknown section",
                "\n[USB Devices]\n",
                r"does not know: \[USB Devices\]",
            ),
            (
                "another app's extension point",
                "\n[Extension org.example.Plugin]\ndirectory=plugins\n",
                r"does not know: \[Extension org.example.Plugin\] directory=plugins",
            ),
        ):
            with self.subTest(label):
                self.broken_metadata(pattern, extra=extra)

    def test_an_unknown_context_key_or_bus_policy_fails(self):
        for old, new, pattern in (
            ("devices=all;", "devices=all;\nunknown-key=1;", r"\[Context\] unknown-key=1;"),
            ("org.a11y.Bus=talk", "org.a11y.Bus=see", r"\[Session Bus Policy\] org.a11y.Bus=see"),
        ):
            with self.subTest(new):
                self.broken(
                    rf"does not know: {pattern}", lambda f, o=old, n=new: f.edit_metadata(o, n)
                )

    def test_own_extension_points_and_persist_are_understood(self):
        fixture = self.fixture()
        fixture.set_metadata(
            extra=f"\n[Extension {APP_ID}.Locale]\ndirectory=share/runtime/locale\n"
        )
        self.passes(fixture.suite())
        fixture.edit_metadata("devices=all;", "devices=all;\npersistent=.dasher;")
        self.fails(r"unexpected permission --persist=.dasher", fixture.suite())

    def test_wrong_application_name_or_command_fails(self):
        self.broken_metadata(r"\[Application\] name is org.example.other", name="org.example.other")
        self.broken_metadata(r"\[Application\] command is dasher-gtk", command="dasher-gtk")

    def test_manifest_finish_args_that_drifted_from_the_list_fail(self):
        for change, pattern in (
            (lambda args: args.append("--socket=session-bus"), "unexpected permission --socket="),
            (lambda args: args.remove("--share=network"), "missing permission --share=network"),
        ):
            with self.subTest(pattern):
                self.broken(
                    rf"manifest finish-args: {pattern}",
                    lambda f, change=change: f.set_manifest(lambda d: change(d["finish-args"])),
                )

    def test_network_during_the_build_fails(self):
        def top_level(data):
            data["build-options"] = {"build-args": ["--share=network"]}

        def in_module(data):
            data["modules"][2]["build-options"]["build-args"] = ["--share=network"]

        def per_arch(data):
            data["modules"][1]["build-options"]["arch"] = {
                "aarch64": {"build-args": ["--share=network"]}
            }

        def nested(data):
            data["modules"][2]["modules"] = [
                {"name": "inner", "build-options": {"build-args": ["--share=network"]}}
            ]

        for label, change, where in (
            ("top level", top_level, "the top level"),
            ("module", in_module, "module dasher"),
            ("arch", per_arch, r"module onnxruntime \(arch aarch64\)"),
            ("nested module", nested, "module inner"),
        ):
            with self.subTest(label):
                self.broken(
                    rf"{where} of the manifest has build-args --share=network",
                    lambda f, change=change: f.set_manifest(change),
                )

    def test_a_missing_manifest_json_is_noted_not_failed(self):
        self.noted("files/manifest.json is missing", lambda f: (f.files / "manifest.json").unlink())

    def test_build_args_with_network_ignores_other_arguments(self):
        manifest = manifest_data()
        manifest["build-options"] = {"build-args": ["--device=dri"]}
        manifest["modules"].append("an-included-module.json")
        self.assertEqual(smoke.build_args_with_network(manifest), [])

    def test_compare_permissions_against_a_given_list(self):
        self.assertEqual(smoke.compare_permissions({"--share=ipc"}, ["--share=ipc"]), [])
        problems = smoke.compare_permissions(
            {"--share=ipc", "--device=dri"}, ["--share=ipc", "--share=network"]
        )
        self.assertEqual(len(problems), 2)
        self.assertIn(
            "unexpected permission --device=dri: not in EXPECTED_FINISH_ARGS", problems[0]
        )
        self.assertIn("missing permission --share=network", problems[1])

    def test_split_list(self):
        self.assertEqual(smoke.split_list("a;b;c;"), ["a", "b", "c"])
        self.assertEqual(smoke.split_list(""), [])
        self.assertEqual(smoke.split_list(None), [])


# ══ files, licences, cleanup ══════════════════════════════════════════════


class FilesTests(SmokeTestCase):
    CHECK = "files"

    def test_each_expected_file_missing_or_empty_fails(self):
        for relative, _ in smoke.EXPECTED_FILES:
            with self.subTest(relative, how="missing"):
                self.broken(
                    rf"^/app/{relative} is missing$",
                    lambda f, relative=relative: (f.files / relative).unlink(),
                )
            with self.subTest(relative, how="empty"):
                self.broken(
                    rf"^/app/{relative} is empty$",
                    lambda f, relative=relative: (f.files / relative).write_bytes(b""),
                )

    def test_a_binary_that_is_not_elf_fails(self):
        for relative in ("dasher", "libdasher.so", "librust_tts_wrapper.so"):
            with self.subTest(relative):
                self.broken(
                    rf"/app/{relative} is not an ELF file",
                    lambda f, relative=relative: (f.files / relative).write_text(
                        "#!/bin/sh\n" * 20
                    ),
                )

    def test_a_launcher_that_does_something_else_fails(self):
        launcher = "bin/dasher"
        for label, change, pattern in (
            ("not executable", lambda p: p.chmod(0o644), "is not executable"),
            (
                "another interpreter",
                lambda p: p.write_text(LAUNCHER.replace("#!/bin/sh", "#!/usr/bin/env bash")),
                "is not a /bin/sh script",
            ),
            (
                "another library path",
                lambda p: p.write_text(LAUNCHER.replace("/app:/app/lib64", "/app/lib")),
                "does not put /app:/app/lib64 on LD_LIBRARY_PATH",
            ),
            (
                "no library path",
                lambda p: p.write_text('#!/bin/sh\nexec /app/dasher "$@"\n'),
                "does not put /app:/app/lib64 on LD_LIBRARY_PATH",
            ),
            (
                "another binary",
                lambda p: p.write_text(
                    LAUNCHER.replace("exec /app/dasher", "exec /app/dasher-missing")
                ),
                "does not `exec /app/dasher",
            ),
            (
                "arguments dropped",
                lambda p: p.write_text(LAUNCHER.replace(' "$@"', "")),
                "does not `exec /app/dasher",
            ),
        ):
            with self.subTest(label):
                self.broken(pattern, lambda f, change=change: change(f.files / launcher))

    def test_each_missing_export_fails(self):
        for relative in smoke.EXPECTED_EXPORTS:
            with self.subTest(relative):
                self.broken(
                    rf"export/{relative} is missing",
                    lambda f, relative=relative: (f.build / "export" / relative).unlink(),
                )

    def test_missing_engine_data_fails(self):
        self.broken(r"/app/Data has no alphabet", lambda f: shutil.rmtree(f.files / "Data"))

        def other_data_only(fixture):
            (fixture.files / "Data/alphabet.english.xml").rename(fixture.files / "Data/colours.xml")

        self.broken(r"/app/Data has no alphabet", other_data_only)

    def test_every_problem_is_reported_at_once(self):
        def two(fixture):
            (fixture.files / "libdasher.so").unlink()
            (fixture.files / f"share/icons/hicolor/scalable/apps/{APP_ID}.svg").unlink()

        problems = self.broken(r"libdasher.so is missing", two)
        self.assertEqual(len(problems), 2)


class LicenceTests(SmokeTestCase):
    CHECK = "licences"

    def test_passes_and_counts(self):
        message = self.passes()
        self.assertIn(f"{len(smoke.SHERPA_DEP_LICENCE_DIRS)} dependencies", message)

    def test_each_missing_licence_file_fails(self):
        for name in (
            "onnxruntime/LICENSE",
            "onnxruntime/ThirdPartyNotices.txt",
            "sherpa-onnx/LICENSE",
        ):
            with self.subTest(name):
                self.broken(
                    rf"/app/{LICENCES}/{name} is missing",
                    lambda f, name=name: (f.files / LICENCES / name).unlink(),
                )

    def test_a_truncated_third_party_notices_file_fails(self):
        self.broken(
            r"ThirdPartyNotices.txt is only 7 bytes",
            lambda f: (f.files / LICENCES / "onnxruntime/ThirdPartyNotices.txt").write_text(
                "notice\n"
            ),
        )

    def test_each_dependency_directory_missing_or_empty_fails(self):
        for name in smoke.SHERPA_DEP_LICENCE_DIRS:
            directory = f"{LICENCES}/sherpa-onnx/{name}"
            with self.subTest(name, how="missing"):
                self.broken(
                    rf"/app/{directory}/ is missing",
                    lambda f, directory=directory: shutil.rmtree(f.files / directory),
                )
            with self.subTest(name, how="no file"):
                self.broken(
                    rf"/app/{directory}/ has no licence text",
                    lambda f, directory=directory: (f.files / directory / "LICENSE").unlink(),
                )
            with self.subTest(name, how="empty file"):
                self.broken(
                    rf"/app/{directory}/ has no licence text",
                    lambda f, directory=directory: (f.files / directory / "LICENSE").write_bytes(
                        b""
                    ),
                )

    def test_a_licence_in_a_subdirectory_counts(self):
        fixture = self.fixture()
        directory = fixture.files / LICENCES / "sherpa-onnx/kissfft"
        (directory / "LICENSE").unlink()
        write(directory / "LICENSES/BSD-3-Clause", "text\n")
        self.passes(fixture.suite())

    def test_a_dependency_nobody_listed_fails(self):
        self.broken(
            r"sherpa-onnx/newdep/ is not expected: sherpa-onnx links a new dependency",
            lambda f: write(f.files / LICENCES / "sherpa-onnx/newdep/LICENSE", "text\n"),
        )

    def test_no_licence_directory_at_all_fails(self):
        problems = self.broken(
            r"sherpa-onnx/LICENSE is missing", lambda f: shutil.rmtree(f.files / "share/licenses")
        )
        self.assertEqual(len(problems), 3 + len(smoke.SHERPA_DEP_LICENCE_DIRS))


class CleanupTests(SmokeTestCase):
    CHECK = "cleanup"

    def test_each_path_that_cleanup_removes_fails_when_present(self):
        for name in smoke.CLEANED_PATHS:
            with self.subTest(name):
                problems = self.broken(
                    rf"^/app/{name} was not cleaned up$",
                    lambda f, name=name: write(f.files / name / "leftover.h"),
                )
                self.assertEqual(len(problems), 1, problems)

    def test_static_archives_and_libtool_files_fail_anywhere(self):
        for relative in ("lib/libstray.a", "lib/gtkmm-4.0/proc/libtool.la", "libonnxruntime.a"):
            with self.subTest(relative):
                self.broken(
                    rf"left in the app: /app/{relative} ",
                    lambda f, relative=relative: write(f.files / relative),
                )

    def test_a_pkgconfig_directory_fails_anywhere(self):
        self.broken(
            r"left in the app: /app/lib/x86_64-linux-gnu/pkgconfig/",
            lambda f: write(f.files / "lib/x86_64-linux-gnu/pkgconfig/gtkmm-4.0.pc"),
        )

    def test_many_leftovers_are_summed_up(self):
        def many(fixture):
            for index in range(12):
                write(fixture.files / f"lib/lib{index:02}.a")

        problems = self.broken(r"and 4 more", many)
        self.assertEqual(len(problems), 1)

    def test_a_directory_named_like_an_archive_is_not_a_leftover(self):
        fixture = self.fixture()
        write(fixture.files / "Data/training.a/readme.txt")
        self.passes(fixture.suite())


# ══ desktop file and metainfo ═════════════════════════════════════════════


class ValidatorTestCase(SmokeTestCase):
    def validator(self, name, status, output="", version="AppStream version: 1.0.2"):
        """Put a fake validator on the PATH the script sees. Asked for its
        --version it prints `version` and nothing is recorded."""
        script = write(
            self.tmp / "bin" / name,
            "#!/bin/sh\n"
            f"if [ \"$1\" = --version ]; then echo '{version}'; exit 0; fi\n"
            f"echo \"$@\" > '{self.tmp}/{name}.args'\nprintf '%s\\n' '{output}'\nexit {status}\n",
            mode=0o755,
        )
        self.tools[name] = str(script)

    def arguments(self, name):
        return (self.tmp / f"{name}.args").read_text().split()

    def hanging(self, name):
        """The validator `name` is installed and never answers: every run
        of it ends when its time is up. Returns the times it was given."""
        self.validator(name, 0)
        real, given = self._run, []

        def hang(cmd, *args, **kwargs):
            if cmd[0] == self.tools[name]:
                given.append(kwargs.get("timeout"))
                raise subprocess.TimeoutExpired(cmd, kwargs["timeout"])
            return real(cmd, *args, **kwargs)

        self.patch(mock.patch.object(smoke.subprocess, "run", side_effect=hang))
        return given

    def assert_tool_problems_fail(self, name):
        """A validator that hangs or cannot be started fails the check of
        the class with the reason: no traceback, and no run that waits for
        ever."""
        self.tools[name] = str(self.tmp / "bin" / "not-there" / name)
        self.fails(rf"^{name} could not be started: .*No such file or directory")
        given = self.hanging(name)
        self.fails(rf"^{name} did not finish within {smoke.TOOL_TIMEOUT} s and was stopped$")
        self.assertEqual(given, [smoke.TOOL_TIMEOUT])


class DesktopFileTests(ValidatorTestCase):
    CHECK = "desktop-file"

    def test_a_validator_that_hangs_or_cannot_be_started_fails(self):
        self.assert_tool_problems_fail("desktop-file-validate")

    def test_passes_with_a_validator_that_accepts_the_file(self):
        self.validator("desktop-file-validate", 0)
        fixture = self.fixture()
        self.passes(fixture.suite())
        self.assertEqual(self.arguments("desktop-file-validate"), [str(fixture.desktop)])

    def test_fails_when_the_validator_rejects_the_file(self):
        self.validator("desktop-file-validate", 1, "error: value is not valid")
        self.fails(r"desktop-file-validate: error: value is not valid")

    def test_without_a_validator_the_skip_says_what_was_still_checked(self):
        self.skips(r"desktop-file-validate is not installed \(Exec and Icon were checked\)")

    def test_wrong_exec_or_icon_fails_even_without_a_validator(self):
        for label, old, new, pattern in (
            ("Exec", "Exec=dasher", "Exec=dasher-gtk", r"Exec=dasher-gtk does not start `dasher`"),
            ("Exec path", "Exec=dasher", "Exec=/app/dasher", r"does not start `dasher`"),
            (
                "Icon",
                f"Icon={APP_ID}",
                "Icon=dasher",
                rf"Icon=dasher, expected {APP_ID}",
            ),
        ):
            with self.subTest(label):
                self.broken(pattern, lambda f, old=old, new=new: f.edit(f.desktop, old, new))

    def test_exec_with_arguments_is_accepted(self):
        fixture = self.fixture()
        fixture.edit(fixture.desktop, "Exec=dasher", "Exec=dasher %U")
        self.skips("is not installed", fixture.suite())

    def test_a_missing_desktop_file_fails(self):
        self.broken(r"\.desktop is missing", lambda f: f.desktop.unlink())


class MetainfoTests(ValidatorTestCase):
    def test_a_validator_that_hangs_or_cannot_be_started_fails(self):
        self.assert_tool_problems_fail("appstreamcli")

    CHECK = "metainfo"

    def test_passes_with_a_validator_that_accepts_the_file(self):
        self.validator("appstreamcli", 0)
        fixture = self.fixture()
        suite = fixture.suite()
        self.assertEqual(self.passes(suite), "appstreamcli 1.0.2 validate --no-net passes")
        self.assertEqual(
            self.arguments("appstreamcli"), ["validate", "--no-net", str(fixture.metainfo)]
        )
        self.assertEqual(suite.notes, [])

    def test_the_result_names_the_version_of_the_validator(self):
        # What appstreamcli calls a warning differs between releases, so a
        # result that differs between two machines has to say which ran.
        for output, version in (
            ("AppStream version: 1.1.2", "1.1.2"),
            ("AppStream CLI tool version: 1.0.2\nAppStream library version: 1.0.2", "1.0.2"),
            ("appstreamcli", "?"),
            ("", "?"),
        ):
            with self.subTest(output):
                self.assertEqual(smoke.tool_version(output), version)
        self.validator("appstreamcli", 3, "W: org.example: some-new-warning", version="x 1.1.2")
        self.fails(r"^appstreamcli 1\.1\.2 validate --no-net: W: org.example: some-new-warning$")

    def test_fails_when_the_validator_rejects_the_file(self):
        self.validator("appstreamcli", 3, "E: org.example: tag-missing")
        self.fails(r"appstreamcli 1\.0\.2 validate --no-net: E: org.example: tag-missing")

    def test_without_a_validator_the_skip_says_what_was_still_checked(self):
        self.skips(r"appstreamcli is not installed")

    def test_wrong_id_launchable_or_missing_tags_fail(self):
        for label, old, new, pattern in (
            (
                "id",
                f"<id>{APP_ID}</id>",
                "<id>org.example.other</id>",
                r"<id> is org.example.other",
            ),
            (
                "launchable",
                f"{APP_ID}.desktop</launchable>",
                "other.desktop</launchable>",
                r"<launchable> is other.desktop",
            ),
            ("name", "<name>Dasher</name>", "", r"<name> is missing or empty"),
            (
                "project licence",
                "<project_license>MIT</project_license>",
                "<project_license> </project_license>",
                r"<project_license> is missing or empty",
            ),
            (
                "metadata licence",
                "<metadata_license>CC0-1.0</metadata_license>",
                "",
                r"<metadata_license> is missing or empty",
            ),
        ):
            with self.subTest(label):
                self.broken(pattern, lambda f, old=old, new=new: f.edit_metainfo(old, new))

    def test_a_missing_metainfo_fails(self):
        self.broken(r"metainfo.xml is missing", lambda f: f.metainfo.unlink())

    def test_a_build_from_another_state_of_the_checkout_is_noted(self):
        fixture = self.fixture()
        fixture.edit_metainfo("<name>Dasher</name>", "<name>Dasher GTK</name>")
        suite = fixture.suite()
        self.skips("is not installed", suite)
        self.assertOneNote(suite, "the installed metainfo differs from")


# ══ screenshot and release ════════════════════════════════════════════════

RAW = "https://raw.githubusercontent.com/dasher-project/Dasher-GTK"
SHA = "7096327d7ece4e07fc0353c18d56c121927d40c7"


def minimal_png(width, height):
    return (
        b"\x89PNG\r\n\x1a\n"
        + struct.pack(">I", 13)
        + b"IHDR"
        + struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
        + b"\0" * 4
    )


class ScreenshotUrlTests(unittest.TestCase):
    def test_commit_and_release_tag_urls_are_accepted(self):
        for ref, in_url in (
            (SHA, SHA),
            ("v6.0.2", "v6.0.2"),
            ("v10.12.3", "v10.12.3"),
            ("v6.1.0-rc.2", "v6.1.0-rc.2"),
            ("v6.0.2", "refs/tags/v6.0.2"),
        ):
            with self.subTest(in_url):
                self.assertEqual(
                    smoke.pinned_screenshot_url(f"{RAW}/{in_url}/packaging/screenshots/a.png"),
                    (ref, "packaging/screenshots/a.png"),
                )

    def test_branches_and_short_shas_are_rejected(self):
        for in_url, named in (
            ("main", "main"),
            ("refs/heads/main", "refs"),
            ("7096327", "7096327"),
            (SHA[:39], SHA[:39]),
            (SHA.upper(), SHA.upper()),
            ("HEAD", "HEAD"),
            ("v6.0", "v6.0"),
            ("v6.0.2-fix", "v6.0.2-fix"),
            ("release", "release"),
        ):
            with self.subTest(in_url):
                with self.assertRaisesRegex(
                    ValueError, "neither a full commit sha nor a release tag"
                ) as e:
                    smoke.pinned_screenshot_url(f"{RAW}/{in_url}/packaging/screenshots/a.png")
                self.assertIn(f"names '{named}'", str(e.exception))

    def test_other_hosts_and_forms_are_rejected(self):
        for url in (
            "https://github.com/dasher-project/Dasher-GTK/blob/main/packaging/screenshots/a.png",
            "https://example.org/a.png",
            f"http://raw.githubusercontent.com/dasher-project/Dasher-GTK/{SHA}/a.png",
            "",
        ):
            with self.subTest(url):
                with self.assertRaisesRegex(ValueError, "is not a raw.githubusercontent.com URL"):
                    smoke.pinned_screenshot_url(url)

    def test_png_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            png = write(Path(tmp) / "a.png", minimal_png(900, 610))
            self.assertEqual(smoke.png_size(png), (900, 610))
            for name, content in (
                ("text.png", b"not a picture at all, really"),
                ("empty.png", b""),
            ):
                with self.assertRaisesRegex(ValueError, "is not a PNG file"):
                    smoke.png_size(write(Path(tmp) / name, content))


class ScreenshotTests(SmokeTestCase):
    CHECK = "screenshot"

    def test_passes_on_the_real_metainfo_and_image(self):
        message = self.passes()
        self.assertIn("1 image(s)", message)

    def set_url(self, fixture, ref):
        root = ET.parse(fixture.metainfo).getroot()
        url = root.find("./screenshots/screenshot/image").text.strip()
        old_ref = url.split("/Dasher-GTK/")[1].split("/packaging/")[0]
        fixture.edit_metainfo(f"/Dasher-GTK/{old_ref}/packaging/", f"/Dasher-GTK/{ref}/packaging/")

    def test_a_url_that_is_not_pinned_fails(self):
        for ref in ("main", "refs/heads/main", "7096327", "flatpak/flathub-readiness"):
            with self.subTest(ref):
                self.broken(
                    r"neither a full commit sha nor a release tag",
                    lambda f, ref=ref: self.set_url(f, ref),
                )

    def test_a_release_tag_is_accepted(self):
        for ref in ("v6.0.2", "refs/tags/v6.0.2"):
            with self.subTest(ref):
                fixture = self.fixture()
                self.set_url(fixture, ref)
                self.passes(fixture.suite())

    def test_a_url_on_another_host_fails(self):
        self.broken(
            r"is not a raw.githubusercontent.com URL",
            lambda f: f.edit_metainfo("https://raw.githubusercontent.com/", "https://example.org/"),
        )

    def test_a_declared_size_that_is_not_the_image_s_fails(self):
        width, height = smoke.png_size(REPO / real_screenshot_path())
        for attribute, value in (("width", width), ("height", height)):
            with self.subTest(attribute):
                self.broken(
                    rf"is {width}x{height} but the metainfo declares",
                    lambda f, a=attribute, v=value: f.edit_metainfo(f'{a}="{v}"', f'{a}="{v + 1}"'),
                )

    def test_an_image_that_is_not_in_the_checkout_fails(self):
        self.broken(r"which is not in this checkout", lambda f: (f.repo / f.screenshot).unlink())

    def test_an_image_that_is_not_a_png_fails(self):
        self.broken(
            r"is not a PNG file",
            lambda f: (f.repo / f.screenshot).write_bytes(b"GIF89a" + b"\0" * 40),
        )

    def test_no_screenshot_or_no_default_screenshot_fails(self):
        self.broken(r"has no <screenshot> with an <image>", lambda f: f.cut_metainfo("screenshots"))
        self.broken(
            r'no <screenshot type="default">',
            lambda f: f.edit_metainfo('<screenshot type="default">', "<screenshot>"),
        )

    def git(self, repo, *arguments):
        settings = (
            "-c",
            "user.name=t",
            "-c",
            "user.email=t@example.org",
            "-c",
            "commit.gpgsign=false",
        )
        done = REAL_RUN(
            ["git", "-C", str(repo), *settings, *arguments], capture_output=True, text=True
        )
        self.assertEqual(done.returncode, 0, done.stderr)
        return done.stdout.strip()

    def repository(self, commit=None):
        """(fixture, sha): a fixture whose checkout is a git repository; with
        `commit` (a message) the screenshot is committed and sha names that."""
        if not REAL_WHICH("git"):
            self.skipTest("git is not installed")
        fixture = self.fixture()
        self.git(fixture.repo, "init", "-q")
        if commit is None:
            return fixture, None
        self.git(fixture.repo, "add", fixture.screenshot)
        self.git(fixture.repo, "commit", "-q", "-m", commit)
        return fixture, self.git(fixture.repo, "rev-parse", "HEAD")

    def test_the_image_in_the_named_commit_has_to_be_the_one_in_the_tree(self):
        fixture, commit = self.repository("add the screenshot")
        self.set_url(fixture, commit)
        suite = fixture.suite()
        self.passes(suite)
        self.assertEqual(suite.notes, [])

        # The image is replaced, same size, but the URL still names the old commit.
        path = fixture.repo / fixture.screenshot
        path.write_bytes(path.read_bytes() + b"\0")
        self.fails(rf"in commit {commit[:12]}.* is not the\s+file in the tree", fixture.suite())

    def test_a_commit_the_checkout_does_not_have_is_noted(self):
        suite = self.repository()[0].suite()
        self.passes(suite)
        self.assertOneNote(suite, "is not in this checkout")

    def test_without_a_git_that_answers_the_comparison_is_left_out_and_noted(self):
        """The URL and the size are checked without git; the comparison
        with the named commit is not, and the result says so."""
        fixture, commit = self.repository("add the screenshot")
        self.set_url(fixture, commit)
        real = self._run
        for label, error, reason in (
            (
                "not installed",
                lambda cmd, kwargs: FileNotFoundError(2, "No such file or directory", "git"),
                "git could not be started: ",
            ),
            (
                "hangs",
                lambda cmd, kwargs: subprocess.TimeoutExpired(cmd, kwargs["timeout"]),
                f"git did not finish within {smoke.TOOL_TIMEOUT} s and was stopped",
            ),
        ):
            with self.subTest(label):

                def no_git(cmd, *args, error=error, **kwargs):
                    if cmd[0] == "git":
                        raise error(cmd, kwargs)
                    return real(cmd, *args, **kwargs)

                suite = fixture.suite()
                with mock.patch.object(smoke.subprocess, "run", side_effect=no_git):
                    message = self.passes(suite)
                self.assertIn("1 image(s)", message)
                self.assertOneNote(
                    suite,
                    f"the image in commit {commit[:12]}, which the screenshot URL names, was "
                    f"not compared with the one in the tree: {reason}",
                )

    def test_a_shallow_clone_without_the_commit_is_not_remarked_on(self):
        """CI checks out one commit; a note on every run would only be noise."""
        fixture, head = self.repository("one commit")
        (fixture.repo / ".git" / "shallow").write_text(head + "\n")
        suite = fixture.suite()
        self.passes(suite)
        self.assertEqual(suite.notes, [])


class ReleaseTests(SmokeTestCase):
    CHECK = "release"

    def test_passes_and_names_the_version(self):
        fixture = self.fixture()
        message = self.passes(fixture.suite())
        self.assertIn(f"newest release {fixture.version}", message)

    def test_a_metainfo_that_was_not_bumped_fails(self):
        self.broken(
            r"the newest <release> is 99.0.0 but CMakeLists.txt has\s+DASHER_GTK_FALLBACK_VERSION",
            lambda f: f.edit_metainfo(
                f'<release version="{f.version}"', '<release version="99.0.0"'
            ),
        )

    def test_a_cmake_fallback_that_was_not_bumped_fails(self):
        problems = self.broken(
            r'DASHER_GTK_FALLBACK_VERSION "0.0.1"',
            lambda f: (f.repo / "CMakeLists.txt").write_text(f.cmake("0.0.1")),
        )
        # The binary of the fixture has the metainfo's version, not 0.0.1.
        self.assertTrue(any("does not contain the version string 0.0.1" in p for p in problems))

    def test_a_binary_built_from_another_version_fails(self):
        def other_binary(fixture):
            (fixture.files / "dasher").write_bytes(make_elf(extra=b"\x000.0.1\0"))

        problems = self.broken(r"/app/dasher does not contain the version string", other_binary)
        self.assertEqual(len(problems), 1)

    def test_the_version_has_to_be_a_whole_string_in_the_binary(self):
        def longer(fixture):
            (fixture.files / "dasher").write_bytes(
                make_elf(extra=b"\0" + fixture.version.encode() + b"-19-g6dfa773\0")
            )

        self.broken(r"does not contain the version string", longer)

    def test_no_fallback_version_in_cmake_is_noted(self):
        self.noted(
            "DASHER_GTK_FALLBACK_VERSION not found",
            lambda f: (f.repo / "CMakeLists.txt").write_text("project(DasherGTK)\n"),
        )

    def test_no_releases_fails(self):
        self.broken(r"has no <release> entries", lambda f: f.cut_metainfo("releases"))

    def test_release_problems(self):
        ok = [("6.0.2", "2026-11-20"), ("6.0.1", "2026-11-09"), ("0.2.16", "2026-10-28")]
        self.assertEqual(smoke.release_problems(ok, "6.0.2"), [])
        self.assertEqual(smoke.release_problems(ok, None), [])
        for label, releases, project, pattern in (
            ("none", [], "6.0.2", "no <release> entries"),
            (
                "not newest first",
                [("6.0.1", "2026-11-20"), ("6.0.2", "2026-11-09")],
                "6.0.1",
                "the first <release> is 6.0.1 but 6.0.2 is listed further down",
            ),
            ("not the project version", ok, "6.0.3", 'DASHER_GTK_FALLBACK_VERSION "6.0.3"'),
            (
                "bad date",
                [("6.0.2", "20 November 2026")],
                "6.0.2",
                "no date in the form YYYY-MM-DD",
            ),
            ("no date", [("6.0.2", None)], "6.0.2", "no date in the form YYYY-MM-DD"),
            (
                "dates out of order",
                [("6.0.2", "2026-11-01"), ("6.0.1", "2026-11-09")],
                "6.0.2",
                "not ordered newest first by date",
            ),
        ):
            with self.subTest(label):
                problems = smoke.release_problems(releases, project)
                self.assertEqual(len(problems), 1, problems)
                self.assertIn(pattern, problems[0])

    def test_version_key_orders_versions(self):
        versions = ["0.2.9", "6.0.0", "0.2.16", "6.1.0-rc.1", "6.1.0", "6.0.10", "6.0.2", "10.0.0"]
        self.assertEqual(
            sorted(versions, key=smoke.version_key),
            ["0.2.9", "0.2.16", "6.0.0", "6.0.2", "6.0.10", "6.1.0-rc.1", "6.1.0", "10.0.0"],
        )

    def test_cmake_fallback_version(self):
        self.assertEqual(smoke.cmake_fallback_version(Fixture.cmake("6.0.2")), "6.0.2")
        self.assertIsNone(
            smoke.cmake_fallback_version('set(DASHER_CORE_FALLBACK_VERSION "unknown")')
        )
        self.assertIsNotNone(smoke.cmake_fallback_version((REPO / "CMakeLists.txt").read_text()))


# ══ rpath and speech-from-source ══════════════════════════════════════════


class RpathTests(SmokeTestCase):
    CHECK = "rpath"

    def test_rpath_problem(self):
        for entry, elf_dir in (
            ("$ORIGIN", "/app"),
            ("${ORIGIN}/lib", "/app"),
            ("$ORIGIN/../lib", "/app/bin"),
            ("/app", "/app"),
            ("/app/lib", "/app"),
            ("/usr/lib", "/app"),
            ("/usr/lib/x", "/app"),
            ("/usr/lib/x86_64-linux-gnu", "/app/lib"),
            ("/usr/lib64", "/app"),
            ("/usr/lib64/gconv", "/app"),
        ):
            with self.subTest(entry, accepted=True):
                self.assertIsNone(smoke.rpath_problem(entry, elf_dir))
        for entry, elf_dir, resolved in (
            ("/run/build/dasher/_build", "/app", "/run/build/dasher/_build"),
            ("/home/builder/src", "/app", "/home/builder/src"),
            ("$ORIGIN/../../outside", "/app", "/outside"),
            ("$ORIGIN/..", "/app", "/"),
            ("/application", "/app", "/application"),
            # A prefix that is not a path component of an allowed directory.
            ("/usr/libexec", "/app", "/usr/libexec"),
            ("/usr/libexec/dasher", "/app", "/usr/libexec/dasher"),
            ("/usr/lib64x", "/app", "/usr/lib64x"),
            ("/usr/lib32/x", "/app", "/usr/lib32/x"),
            ("/usr", "/app", "/usr"),
            ("/usr/lib/../libexec", "/app", "/usr/libexec"),
            ("/tmp/xy", "/app", "/tmp/xy"),
            ("lib", "/app", "lib"),
        ):
            with self.subTest(entry, accepted=False):
                self.assertEqual(
                    smoke.rpath_problem(entry, elf_dir), f"{entry} (resolves to {resolved})"
                )

    def test_passes_and_counts_the_elf_files(self):
        self.assertEqual(self.passes(), "4 ELF files")

    def test_a_path_of_the_build_machine_fails(self):
        for key in ("rpath", "runpath"):
            with self.subTest(key):
                self.broken(
                    rf"^/app/libdasher.so has {key.upper()} /run/build/dasher/_build \(resolves",
                    lambda f, key=key: (f.files / "libdasher.so").write_bytes(
                        make_elf(**{key: ["$ORIGIN", "/run/build/dasher/_build"]})
                    ),
                )

    def test_origin_is_resolved_from_where_the_file_is_installed(self):
        fixture = self.fixture()
        (fixture.files / "lib/libgtkmm-4.0.so.0.0.0").write_bytes(make_elf(runpath=["$ORIGIN/.."]))
        self.passes(fixture.suite())
        (fixture.files / "libdasher.so").write_bytes(make_elf(runpath=["$ORIGIN/.."]))
        self.fails(r"/app/libdasher.so has RUNPATH \$ORIGIN/.. \(resolves to /\)", fixture.suite())

    def test_a_build_without_elf_files_fails(self):
        def no_elf(fixture):
            for relative, _ in fixture.suite().app_elfs():
                (fixture.files / relative).write_text("not a binary")

        self.broken(r"no ELF file found", no_elf)


class SpeechFromSourceTests(SmokeTestCase):
    CHECK = "speech-from-source"

    def broken_library(self, pattern, **kwargs):
        return self.broken(
            pattern,
            lambda f: (f.files / "librust_tts_wrapper.so").write_bytes(f.speech_library(**kwargs)),
        )

    def test_passes_and_names_the_version(self):
        message = self.passes()
        self.assertIn(f"ONNX Runtime {ORT_VERSION} built from a tarball", message)

    def test_a_prebuilt_onnxruntime_fails(self):
        problems = self.broken_library(
            r"built from a git checkout \(ORT Build Info: git-branch=HEAD, git-commit-id=33ca96282",
            build_info=ORT_BUILD_INFO.replace("git-branch=,", "git-branch=HEAD,").replace(
                "git-commit-id=,", "git-commit-id=33ca96282,"
            ),
        )
        self.assertEqual(len(problems), 1)

    def test_build_info_without_a_commit_field_fails(self):
        self.broken_library(
            r"built from a git checkout", build_info="ORT Build Info: build type=Release"
        )

    def test_missing_build_info_fails(self):
        self.broken_library(r"has no ONNX Runtime build info string", build_info="")

    def test_another_onnxruntime_version_fails(self):
        self.broken_library(
            rf"does not contain ONNX Runtime's version string {ORT_VERSION}", version="1.27.0"
        )
        self.broken_library(r"does not contain ONNX Runtime's version string", version="")

    def test_objects_from_another_compiler_fail(self):
        foreign = "GCC: (GNU) 11.2.1 20220127 (Red Hat 11.2.1-9)"
        problems = self.broken_library(
            r"contains code from another compiler than the SDK's",
            comment=[GCC_IDENT, "rustc version 1.98.0", foreign],
        )
        self.assertIn(foreign, problems[0])
        self.assertIn(GCC_IDENT, problems[0])

    def test_other_tools_in_the_comment_section_are_fine(self):
        fixture = self.fixture()
        (fixture.files / "librust_tts_wrapper.so").write_bytes(
            fixture.speech_library(comment=["clang version 22.1.0", "rustc version 1.98.0"])
        )
        self.passes(fixture.suite())

    def test_no_compiler_ident_in_the_app_is_noted(self):
        self.noted(
            "compilers not compared",
            lambda f: (f.files / "dasher").write_bytes(make_elf(rpath=["$ORIGIN"])),
        )

    def test_a_shared_speech_library_fails(self):
        for needed in ("libonnxruntime.so.1.28.2", "libsherpa-onnx-c-api.so"):
            with self.subTest(needed=needed):
                self.broken_library(
                    rf"/app/librust_tts_wrapper.so needs the shared library {needed}",
                    needed=[needed, "libc.so.6"],
                )
        for name in ("lib/libsherpa-onnx-c-api.so", "libonnxruntime.so.1.28.2"):
            with self.subTest(file=name):
                self.broken(
                    rf"/app/{name} is a shared speech library",
                    lambda f, name=name: write(f.files / name),
                )

    def test_a_manifest_that_does_not_state_the_version_fails(self):
        def no_version(data):
            data["modules"][1]["build-commands"] = ["cmake -S cmake"]

        def no_module(data):
            del data["modules"][1]

        for change in (no_version, no_module):
            with self.subTest(change.__name__):
                self.broken(
                    r"no onnxruntime module that states its version",
                    lambda f, change=change: f.set_manifest(change),
                )

    def test_a_missing_library_fails(self):
        self.broken(
            r"librust_tts_wrapper.so is missing",
            lambda f: (f.files / "librust_tts_wrapper.so").unlink(),
        )

    def test_without_manifest_json_the_check_is_skipped(self):
        fixture = self.fixture()
        (fixture.files / "manifest.json").unlink()
        self.skips(r"manifest.json does not exist", fixture.suite())


# ══ Pure judging functions ════════════════════════════════════════════════


class LintVerdictTests(unittest.TestCase):
    def test_only_expected_errors_is_no_problem(self):
        for kind, expected in smoke.LINT_EXPECTED_ERRORS.items():
            with self.subTest(kind):
                self.assertEqual(smoke.lint_verdict(kind, {"errors": list(expected)}), ([], []))

    def test_a_new_error_is_a_problem_with_the_linter_s_explanation(self):
        for kind, expected in smoke.LINT_EXPECTED_ERRORS.items():
            with self.subTest(kind):
                report = {
                    "errors": list(expected) + ["finish-args-arbitrary-dbus-access"],
                    "info": [
                        "finish-args-arbitrary-dbus-access: finish-args has socket access",
                        "appid-url-not-reachable: Tried https://alternativeinterface.org",
                    ],
                }
                problems, notes = smoke.lint_verdict(kind, report)
                self.assertEqual(notes, [])
                self.assertEqual(len(problems), 1)
                self.assertIn(
                    "new lint error finish-args-arbitrary-dbus-access "
                    "(finish-args has socket access)",
                    problems[0],
                )
                self.assertIn(f"LINT_EXPECTED_ERRORS['{kind}']", problems[0])

    def test_a_new_error_without_explanation(self):
        problems, _ = smoke.lint_verdict(
            "manifest", {"errors": ["toplevel-no-command"]}, expected={}
        )
        self.assertEqual(len(problems), 1)
        self.assertTrue(problems[0].startswith("new lint error toplevel-no-command: fix it"))

    def test_an_expected_error_that_is_gone_is_a_note(self):
        for kind, expected in smoke.LINT_EXPECTED_ERRORS.items():
            with self.subTest(kind):
                problems, notes = smoke.lint_verdict(kind, {})
                self.assertEqual(problems, [])
                self.assertEqual(
                    notes,
                    [
                        f"lint error {code} is no longer reported: remove it from "
                        f"LINT_EXPECTED_ERRORS['{kind}']"
                        for code in sorted(expected)
                    ],
                )

    def test_warnings_are_notes(self):
        report = {
            "errors": list(smoke.LINT_EXPECTED_ERRORS["builddir"]),
            "warnings": ["finish-args-flatpak-spawn-access", "another-warning"],
        }
        self.assertEqual(
            smoke.lint_verdict("builddir", report),
            ([], ["lint warning another-warning", "lint warning finish-args-flatpak-spawn-access"]),
        )

    def test_the_expected_errors_all_have_a_reason(self):
        self.assertEqual(set(smoke.LINT_EXPECTED_ERRORS), {"manifest", "builddir"})
        for kind, expected in smoke.LINT_EXPECTED_ERRORS.items():
            for code, reason in expected.items():
                self.assertTrue(reason.strip(), f"{kind}: {code} has no reason")


class LogTests(unittest.TestCase):
    def test_the_broadway_warning_is_allowed(self):
        problems, allowed = smoke.log_problems(f"\n{BROADWAY_WARNING}\n")
        self.assertEqual(problems, [])
        self.assertEqual(len(allowed), 1)
        self.assertTrue(allowed[0].startswith("Gdk-WARNING: Duplicate call"))

    def test_warnings_errors_and_criticals_are_problems(self):
        for line, entry in (
            (
                "(dasher:2): Gtk-WARNING **: 17:27:50.984: Unable to acquire session bus",
                "Gtk-WARNING: Unable to acquire session bus",
            ),
            (
                "(process:3): Gtk-WARNING **: Failed to open display",
                "Gtk-WARNING: Failed to open display",
            ),
            (
                "** (dasher:1): WARNING **: 10:00:00.000: no domain here",
                "app-WARNING: no domain here",
            ),
            (
                "(dasher:2): Gdk-ERROR **: 17:27:51.023: Resource path is not a valid image",
                "Gdk-ERROR: Resource path is not a valid image",
            ),
            (
                "(dasher:9): GLib-GObject-CRITICAL **: 09:00:00.001: g_object_unref: assertion failed",
                "GLib-GObject-CRITICAL: g_object_unref: assertion failed",
            ),
            (
                "  (dasher:9): GLib-GIO-CRITICAL **: 09:00:00.001: indented line",
                "GLib-GIO-CRITICAL: indented line",
            ),
        ):
            with self.subTest(line):
                self.assertEqual(smoke.log_problems(line), ([entry], []))

    def test_an_allowlisted_pattern_never_excuses_an_error(self):
        line = BROADWAY_WARNING.replace("Gdk-WARNING", "Gdk-CRITICAL")
        problems, allowed = smoke.log_problems(line)
        self.assertEqual(len(problems), 1)
        self.assertEqual(allowed, [])

    def test_crash_lines_are_problems(self):
        for line in (
            "thread 'main' panicked at src/lib.rs:10:5:",
            "Segmentation fault (core dumped)",
            "terminate called after throwing an instance of 'std::bad_alloc'",
            "*** stack smashing detected ***: terminated",
            "dasher: ../glib/gmem.c:106: Assertion `ptr != NULL' failed.",
        ):
            with self.subTest(line):
                self.assertEqual(smoke.log_problems(f"  {line}\n"), ([line], []))

    def test_glib_messages_below_warning_are_not_a_problem(self):
        text = "\n".join(
            [
                "(dasher:1): Gtk-Message **: 10:00:00.000: Failed to load module",
                "(dasher:1): GLib-INFO **: 10:00:00.000: info",
                "(dasher:1): Gdk-DEBUG **: 10:00:00.000: debug",
                "",
                "   ",
            ]
        )
        self.assertEqual(smoke.log_problems(text), ([], []))

    def test_output_that_is_not_a_glib_message_is_a_problem(self):
        # What the app reports with std::cerr never reaches GLib's log.
        for line in (
            "InputManager: SDL joystick init failed: no devices",
            "InputManager: Failed to open joystick: permission denied",
            "TtsService: Failed to create engine: sherpaonnx",
            "  Error: model not installed",
            "DasherBridge: could not load the alphabet",
            "sherpa: model install failed: network: timeout",
            "InputManager: Joystick added: Some Pad",
            "ALSA lib pcm.c:2721:(snd_pcm_open_noupdate) Unknown PCM default",
            "/app/dasher: error while loading shared libraries: libdasher.so",
            "hello",
        ):
            with self.subTest(line):
                self.assertEqual(smoke.log_problems(f"{line}\n"), ([line.strip()], []))

    def test_the_known_bad_lines_fail_with_the_lists_as_they_are(self):
        # Against the real LOG_ALLOWLIST and OUTPUT_ALLOWLIST: an entry that
        # is widened ("^Gdk-WARNING", ".*") lets one of these through.
        known_bad = [
            "(dasher:2): Gdk-WARNING **: 17:27:50.952: Settings portal not found",
            "(dasher:2): Gdk-WARNING **: 17:27:50.952: Duplicate call",
            "(dasher:2): Gdk-WARNING **: 17:27:50.952: Failed to open display",
            "(dasher:2): Gtk-WARNING **: 17:27:50.984: Unable to acquire session bus",
            "(dasher:2): Gtk-WARNING **: 17:27:50.984: Theme parser error: gtk.css:1:1",
            "(dasher:2): GLib-GIO-WARNING **: 17:27:50.984: Duplicate call to "
            "gdk_frame_timings_submitted() with different values.",
            "** (dasher:1): WARNING **: 10:00:00.000: Duplicate call to "
            "gdk_frame_timings_submitted() with different values.",
            "(dasher:2): Gdk-CRITICAL **: 17:27:50.952: Duplicate call to "
            "gdk_frame_timings_submitted() with different values.",
            "(dasher:2): Gdk-ERROR **: 17:27:51.023: Resource path is not a valid image",
            "InputManager: SDL joystick init failed: no devices",
            "TtsService: Failed to create engine: sherpaonnx",
            "Gdk-WARNING: Duplicate call to gdk_frame_timings_submitted() with different values.",
            "thread 'main' panicked at src/lib.rs:10:5:",
        ]
        for line in known_bad:
            with self.subTest(line):
                problems, allowed = smoke.log_problems(line)
                self.assertEqual(len(problems), 1, line)
                self.assertEqual(allowed, [])
        problems, allowed = smoke.log_problems("\n".join(known_bad + [BROADWAY_WARNING]))
        self.assertEqual(len(problems), len(known_bad))
        self.assertEqual(len(allowed), 1)

    def test_a_healthy_start_prints_only_what_the_lists_name(self):
        # The whole output of a start that is fine (see HeadlessJudgementTests).
        self.assertEqual(len(smoke.OUTPUT_ALLOWLIST), 1)
        self.assertEqual(len(smoke.LOG_ALLOWLIST), 1)
        self.assertEqual(smoke.log_problems(GOOD_START["log"])[0], [])

    def test_a_given_allowlist_replaces_the_default(self):
        line = "(dasher:2): Gtk-WARNING **: 17:27:50.984: Unable to acquire session bus"
        self.assertEqual(
            smoke.log_problems(line, [(r"^Gtk-WARNING: Unable to acquire session bus", "test")]),
            ([], ["Gtk-WARNING: Unable to acquire session bus"]),
        )
        self.assertEqual(len(smoke.log_problems(BROADWAY_WARNING, [])[0]), 1)

    def test_an_output_allowlist_excuses_plain_lines_only(self):
        output = [(r"^InputManager: Joystick added: ", "a joystick is plugged in")]
        added = "InputManager: Joystick added: Some Pad"
        self.assertEqual(smoke.log_problems(added, output_allowlist=output), ([], [added]))
        # Not a GLib warning with the same text, and never a crash line.
        warning = f"(dasher:2): Gtk-WARNING **: {added}"
        self.assertEqual(
            smoke.log_problems(warning, output_allowlist=output), ([f"Gtk-WARNING: {added}"], [])
        )
        everything = [(r".*", "too wide")]
        for crash in ("Segmentation fault (core dumped)", "thread 'x' panicked at src/a.rs:1:"):
            with self.subTest(crash):
                self.assertEqual(
                    smoke.log_problems(crash, output_allowlist=everything), ([crash], [])
                )
        # A GLib allowlist says nothing about plain lines.
        self.assertEqual(smoke.log_problems(added, [(r".*", "too wide")]), ([added], []))

    def test_the_missing_font_dirs_file_of_older_flatpak_build_is_harmless(self):
        line = (
            'Fontconfig error: Cannot load config file "/run/host/font-dirs.xml": '
            "File not found: /run/host/font-dirs.xml"
        )
        self.assertEqual(smoke.log_problems(line), ([], [line]))
        for other in (
            'Fontconfig error: Cannot load config file "/app/etc/fonts.conf": '
            "File not found: /app/etc/fonts.conf",
            line + " (and more)",
        ):
            with self.subTest(other):
                self.assertEqual(smoke.log_problems(other), ([other], []))

    def test_the_allowlist_entries_are_anchored_narrow_and_have_a_reason(self):
        for pattern, reason in smoke.LOG_ALLOWLIST + smoke.OUTPUT_ALLOWLIST:
            with self.subTest(pattern):
                self.assertTrue(pattern.startswith("^"), pattern)
                self.assertTrue(reason.strip(), pattern)
                # More than a log domain or one word: some of the message.
                self.assertGreater(len(re.sub(r"\\.|[^\w ]", "", pattern).split()), 3, pattern)


def write_wav(path, seconds=2.0, rate=22050, amplitude=3000, width=2, channels=1):
    frames = int(seconds * rate)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(width)
        w.setframerate(rate)
        if width == 2:
            samples = array.array(
                "h",
                (int(amplitude * math.sin(2 * math.pi * 220 * n / rate)) for n in range(frames)),
            )
            if sys.byteorder == "big":
                samples.byteswap()
            w.writeframes(samples.tobytes())
        else:
            w.writeframes(bytes([128]) * frames * channels)
    return path


class AudioTests(SmokeTestCase):
    def test_speech_like_audio_passes(self):
        stats = smoke.wav_stats(write_wav(self.tmp / "ok.wav"))
        self.assertEqual((stats["rate"], stats["channels"], stats["width"]), (22050, 1, 2))
        self.assertAlmostEqual(stats["seconds"], 2.0, places=2)
        self.assertAlmostEqual(stats["rms"], 3000 / math.sqrt(2), delta=20)
        self.assertAlmostEqual(stats["peak"], 3000, delta=2)
        self.assertEqual(smoke.audio_problems(stats), [])

    def test_implausible_audio_fails(self):
        for label, kwargs, pattern in (
            ("silence", {"amplitude": 0}, r"silent or nearly so \(RMS 0, peak 0"),
            ("nearly silent", {"amplitude": 100}, r"silent or nearly so"),
            ("wrong sample rate", {"rate": 16000}, r"sample rate 16000 Hz, the voice is 22050 Hz"),
            ("too short", {"seconds": 0.2}, r"0.20 s of audio, expected between 1 and 15 s"),
            ("too long", {"seconds": 16.0}, r"16.00 s of audio"),
            ("8-bit", {"width": 1}, r"8-bit samples, expected 16-bit"),
        ):
            with self.subTest(label):
                stats = smoke.wav_stats(write_wav(self.tmp / "bad.wav", **kwargs))
                problems = smoke.audio_problems(stats)
                self.assertTrue(problems)
                self.assertRegex(" | ".join(problems), pattern)

    def test_an_empty_wav_file_is_silent_not_a_crash(self):
        stats = smoke.wav_stats(write_wav(self.tmp / "empty.wav", seconds=0))
        self.assertEqual((stats["seconds"], stats["rms"], stats["peak"]), (0.0, 0.0, 0))
        self.assertEqual(len(smoke.audio_problems(stats)), 2)

    def test_another_voice_can_be_given(self):
        stats = smoke.wav_stats(write_wav(self.tmp / "16k.wav", rate=16000))
        self.assertEqual(smoke.audio_problems(stats, {"sample_rate": 16000}), [])

    def test_sha256_file(self):
        path = write(self.tmp / "data", b"abc")
        self.assertEqual(
            smoke.sha256_file(path),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        )

    def test_the_pinned_voice_is_well_formed(self):
        for key in ("archive_sha256", "model_sha256"):
            self.assertRegex(smoke.VOICE[key], r"^[0-9a-f]{64}$")
        self.assertTrue(smoke.VOICE["model_file"].endswith(".onnx"))
        low, high = smoke.SYNTH_SECONDS
        self.assertLess(low, high)


# ══ The sandbox command line ══════════════════════════════════════════════

# A build that asks for far more than the real one: every permission here
# would reach the desktop if a sandbox let it through.
GREEDY_FINISH_ARGS = smoke.EXPECTED_FINISH_ARGS + [
    "--socket=x11",
    "--socket=session-bus",
    "--socket=system-bus",
    "--socket=gpg-agent",
    "--device=dri",
    "--filesystem=host",
    "--filesystem=xdg-download:ro",
    "--allow=devel",
]
DISPLAY_SOCKETS = ("wayland", "x11", "fallback-x11", "session-bus", "system-bus")


class SandboxCommandTests(SmokeTestCase):
    def greedy(self, *arguments):
        fixture = self.fixture(GREEDY_FINISH_ARGS)
        self.use_flatpak()
        return fixture, fixture.suite(*arguments)

    def last_command(self):
        return self.flatpak_calls[-1][0]

    def assertNothingShared(self, cmd, allowed=()):
        """No positive permission in a `flatpak build` command except `allowed`."""
        self.assertEqual(shared(cmd), sorted(allowed), cmd)
        for socket in DISPLAY_SOCKETS:
            self.assertNotIn(f"--socket={socket}", cmd)

    def assertSwitchedOff(self, cmd):
        """Everything GREEDY_FINISH_ARGS declares is negated in `cmd`."""
        for expected in (
            *(f"--nosocket={socket}" for socket in smoke.KNOWN_SOCKETS + ["gpg-agent"]),
            "--nodevice=all",
            "--nodevice=dri",
            "--unshare=network",
            "--unshare=ipc",
            "--nofilesystem=home",
            "--nofilesystem=host",
            "--nofilesystem=xdg-download",
            "--disallow=devel",
        ):
            self.assertIn(expected, cmd)

    def test_every_declared_permission_is_switched_off(self):
        fixture, suite = self.greedy()
        suite.sandbox(["true"])
        cmd = self.last_command()
        self.assertEqual(
            cmd[:5], ["flatpak", "build", "--readonly", "--die-with-parent", "--runtime"]
        )
        self.assertEqual(cmd[-2:], [str(fixture.build), "true"])
        self.assertNothingShared(cmd)
        self.assertSwitchedOff(cmd)
        # Each option once: flatpak takes the last one, so a duplicate
        # could undo a negation.
        options = [arg for arg in cmd if arg.startswith("--")]
        self.assertEqual(len(options), len(set(options)))

    def test_the_probe_is_a_sandbox_like_every_other(self):
        # The probe that finds out whether `flatpak build` works here runs
        # with the same negations: it is not given the devices, the home
        # directory or the IPC namespace the metadata declares either.
        fixture, suite = self.greedy()
        suite.sandbox(["true"], platform=False)
        self.assertEqual(len(self.flatpak_calls), 2)
        probe, command = (cmd for cmd, _ in self.flatpak_calls)
        self.assertEqual(probe, command)
        self.assertEqual(probe, suite.sandbox_command(platform=False) + ["true"])
        self.assertNothingShared(probe)
        self.assertSwitchedOff(probe)

    def test_what_flatpak_build_leaves_writable_is_replaced_by_scratch(self):
        # `flatpak build --readonly` still binds var/tmp and var/lib of the
        # build directory read-write.
        fixture, suite = self.greedy()
        suite.sandbox(["true"])
        work = suite.workdir()
        self.assertEqual(smoke.WRITABLE_BUILD_DIRS, ("/var/tmp", "/var/lib"))
        for cmd, _ in self.flatpak_calls:
            self.assertIn(f"--bind-mount=/var/tmp={work / 'var/tmp'}", cmd)
            self.assertIn(f"--bind-mount=/var/lib={work / 'var/lib'}", cmd)
            # Before the build directory: after it comes the command to run.
            self.assertLess(cmd.index(f"--bind-mount=/var/lib={work / 'var/lib'}"), len(cmd) - 2)
        self.assertTrue((work / "var/tmp").is_dir())
        self.assertTrue((work / "var/lib").is_dir())
        self.assertFalse(str(work).startswith(str(fixture.build)))
        self.assertFalse((fixture.build / "var").exists())

    def test_a_build_that_persists_a_directory_gets_no_sandbox(self):
        # --persist has no negation: flatpak would create the directory
        # below ~/.var/app on the host and bind it.
        fixture = self.fixture(smoke.EXPECTED_FINISH_ARGS + ["--persist=.dasher"])
        self.use_flatpak()
        suite = fixture.suite()
        for platform in (True, False, True):
            with self.assertRaisesRegex(
                smoke.Skip, r"declares --persist=\.dasher, which `flatpak build` cannot switch off"
            ):
                suite.sandbox(["true"], platform=platform)
        self.assertEqual(self.flatpak_calls, [])
        for name in ("linkage", "start", "speech-library"):
            with self.subTest(name):
                self.assertSkips(suite, name, r"No sandbox is started for such a build")
        self.assertEqual(self.flatpak_calls, [])

    def test_only_what_the_caller_asks_for_is_switched_on(self):
        _, suite = self.greedy()
        suite.sandbox(["true"], network=True, sockets=["pulseaudio"], devices=["all"])
        cmd = self.last_command()
        self.assertNothingShared(
            cmd, allowed=["--socket=pulseaudio", "--device=all", "--share=network"]
        )
        for expected in (
            "--nosocket=wayland",
            "--nosocket=session-bus",
            "--nodevice=dri",
            "--unshare=ipc",
        ):
            self.assertIn(expected, cmd)
        for negated in ("--nosocket=pulseaudio", "--nodevice=all", "--unshare=network"):
            self.assertNotIn(negated, cmd)

    def test_ipc_and_file_systems_can_never_be_switched_on(self):
        _, suite = self.greedy()
        suite.sandbox(["true"], network=True, sockets=smoke.KNOWN_SOCKETS, devices=["all", "dri"])
        cmd = self.last_command()
        self.assertIn("--unshare=ipc", cmd)
        self.assertFalse([arg for arg in cmd if arg.startswith("--filesystem=")])
        self.assertFalse([arg for arg in cmd if arg.startswith("--share=ipc")])

    def test_platform_runtime_unless_the_sdk_is_asked_for(self):
        _, suite = self.greedy()
        suite.sandbox(["true"])
        self.assertIn("--runtime", self.last_command())
        suite.sandbox(["true"], platform=False)
        self.assertNotIn("--runtime", self.last_command())
        # One probe per runtime, then the answer is remembered.
        suite.sandbox(["true"])
        suite.sandbox(["true"], platform=False)
        self.assertEqual(len(self.flatpak_calls), 4 + 2)

    def test_work_directory_binds_and_environment(self):
        _, suite = self.greedy()
        suite.sandbox(
            ["python3", "/run/dasher-smoke/x.py"],
            binds=[("/run/dasher-voices", "/somewhere/voices")],
            env=[("HOME", "/run/dasher-voices")],
        )
        cmd = self.last_command()
        work = suite.workdir()
        self.assertIn(f"--bind-mount=/run/dasher-smoke={work}", cmd)
        self.assertIn("--bind-mount=/run/dasher-voices=/somewhere/voices", cmd)
        self.assertIn("--env=HOME=/run/dasher-voices", cmd)
        self.assertEqual(cmd[-2:], ["python3", "/run/dasher-smoke/x.py"])
        # The helpers are copies in the work directory, not the sources.
        self.assertEqual(
            sorted(p.name for p in work.iterdir() if p.is_file()),
            sorted(p.name for p in smoke.SANDBOX_HELPERS.iterdir() if p.is_file()),
        )
        self.assertTrue(os.access(work / "flatpak-spawn", os.X_OK))

    SESSION_ENVIRONMENT = {
        "DISPLAY": ":0",
        "WAYLAND_DISPLAY": "wayland-0",
        "WAYLAND_SOCKET": "5",
        "XAUTHORITY": "/run/user/1000/xauth",
        "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/user/1000/bus",
        "AT_SPI_BUS_ADDRESS": "unix:path=/run/user/1000/at-spi/bus",
        "SSH_AUTH_SOCK": "/run/user/1000/keyring/ssh",
        "PULSE_SERVER": "unix:/run/user/1000/pulse/native",
        "PIPEWIRE_REMOTE": "pipewire-0",
    }

    def test_flatpak_is_not_told_where_the_desktop_session_is(self):
        self.assertEqual(
            set(smoke.SESSION_VARIABLES) | set(smoke.SOUND_VARIABLES), set(self.SESSION_ENVIRONMENT)
        )
        _, suite = self.greedy()
        with mock.patch.dict(os.environ, dict(self.SESSION_ENVIRONMENT, KEEP_ME="1")):
            suite.sandbox(["true"], network=True, devices=["all"])
        self.assertEqual(len(self.flatpak_calls), 2)
        for cmd, kwargs in self.flatpak_calls:
            for variable in self.SESSION_ENVIRONMENT:
                self.assertNotIn(variable, kwargs["env"], cmd)
            self.assertEqual(kwargs["env"]["KEEP_ME"], "1")

    def test_only_the_sandbox_with_the_sound_socket_can_find_the_sound_server(self):
        _, suite = self.greedy()
        with mock.patch.dict(os.environ, self.SESSION_ENVIRONMENT):
            suite.sandbox(["true"], sockets=["pulseaudio"])
        probe, command = (kwargs["env"] for _, kwargs in self.flatpak_calls)
        for variable in smoke.SOUND_VARIABLES:
            self.assertNotIn(variable, probe)
            self.assertEqual(command[variable], self.SESSION_ENVIRONMENT[variable])
        for variable in smoke.SESSION_VARIABLES:
            self.assertNotIn(variable, command)

    def test_verbose_prints_the_command(self):
        _, suite = self.greedy("--verbose")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            suite.sandbox(["sh", "-c", "echo hello"])
        self.assertIn("    $ flatpak build --readonly --die-with-parent --runtime", out.getvalue())
        self.assertIn("'echo hello'", out.getvalue())

    def test_without_flatpak_the_sandbox_is_skipped(self):
        suite = self.fixture().suite()
        with self.assertRaisesRegex(smoke.Skip, "flatpak is not installed"):
            suite.sandbox(["true"])
        self.assertEqual(self.flatpak_calls, [])

    def test_a_runtime_that_is_not_installed_is_a_skip_and_asked_once(self):
        fixture = self.fixture()
        self.use_flatpak(
            lambda argv, kwargs: completed(
                returncode=1,
                stderr="warning: x\nerror: runtime/org.gnome.Platform/x86_64/51 not installed\n",
            )
        )
        suite = fixture.suite()
        for _ in range(2):
            with self.assertRaisesRegex(
                smoke.Skip,
                r"does not run here with the Platform \(error: runtime/.* not installed\)",
            ):
                suite.sandbox(["true"])
        self.assertEqual(len(self.flatpak_calls), 1)
        with self.assertRaisesRegex(smoke.Skip, "with the Sdk"):
            suite.sandbox(["true"], platform=False)

    def test_a_probe_that_hangs_is_a_skip(self):
        def hang(argv, kwargs):
            raise subprocess.TimeoutExpired(argv, kwargs["timeout"])

        self.use_flatpak(hang)
        suite = self.fixture().suite()
        for _ in range(2):
            with self.assertRaisesRegex(
                smoke.Skip,
                r"^no sandbox can be started here: `flatpak build` did not finish within "
                r"120 s and was stopped$",
            ):
                suite.sandbox(["true"])
        self.assertEqual(len(self.flatpak_calls), 1)

    def test_a_flatpak_that_cannot_be_started_is_a_skip(self):
        def gone(argv, kwargs):
            raise PermissionError(13, "Permission denied", "flatpak")

        self.use_flatpak(gone)
        suite = self.fixture().suite()
        for _ in range(2):
            with self.assertRaisesRegex(
                smoke.Skip,
                r"^no sandbox can be started here: `flatpak build` could not be started: "
                r".*Permission denied",
            ):
                suite.sandbox(["true"])
        self.assertEqual(len(self.flatpak_calls), 1)

    def test_a_command_that_hangs_is_a_failure(self):
        def hang(argv, kwargs):
            raise subprocess.TimeoutExpired(argv, kwargs["timeout"])

        self.use_sandbox(hang)
        with self.assertRaisesRegex(
            smoke.TimedOut,
            r"^`sleep 100` in the sandbox did not finish within 7 s and was stopped$",
        ) as caught:
            self.fixture().suite().sandbox(["sleep", "100"], timeout=7)
        # A failure for every check that does not say otherwise.
        self.assertIsInstance(caught.exception, smoke.Failure)
        self.assertEqual(self.flatpak_calls[-1][1]["timeout"], 7)

    def test_a_command_that_cannot_be_started_is_a_failure(self):
        def gone(argv, kwargs):
            raise OSError(7, "Argument list too long")

        self.use_sandbox(gone)
        with self.assertRaisesRegex(
            smoke.Failure, r"^`ldd a b` in the sandbox could not be started: .*too long$"
        ) as caught:
            self.fixture().suite().sandbox(["ldd", "a", "b", "c"])
        self.assertNotIsInstance(caught.exception, smoke.TimedOut)

    def test_run_command_stops_a_command_that_does_not_end(self):
        """For real: a child that would run for a minute is killed and
        waited for when its time is up, and what it started outside a
        sandbox is not this script's to answer for (inside one, bwrap's
        --die-with-parent is)."""
        pid_file = self.tmp / "pid"
        started = time.monotonic()
        with self.assertRaisesRegex(
            smoke.TimedOut, r"^sh did not finish within 0\.5 s and was stopped$"
        ):
            smoke.run_command(["sh", "-c", f"echo $$ > '{pid_file}'; exec sleep 60"], timeout=0.5)
        self.assertLess(time.monotonic() - started, 30)
        pid = int(pid_file.read_text())
        with self.assertRaises(ProcessLookupError):  # killed, and reaped
            os.kill(pid, 0)

    def test_run_command_names_a_command_that_cannot_be_started(self):
        missing = self.tmp / "no-such-tool"
        with self.assertRaisesRegex(
            smoke.Failure, r"^no-such-tool could not be started: .*No such file"
        ):
            smoke.run_command([str(missing), "--version"], timeout=5)
        with self.assertRaisesRegex(smoke.Failure, r"^the linter could not be started: "):
            smoke.run_command([str(missing)], timeout=5, what="the linter")
        done = smoke.run_command(["sh", "-c", "printf out; printf err >&2; exit 3"], timeout=30)
        self.assertEqual((done.returncode, done.stdout, done.stderr), (3, "out", "err"))
        done = smoke.run_command(["printf", "\\377"], timeout=30, text=False)
        self.assertEqual(done.stdout, b"\xff")

    def test_sandbox_json_takes_the_last_json_line(self):
        self.use_flatpak(lambda argv, kwargs: completed('noise\n{"a": 1}\n{"a": 2}\n'))
        suite = self.fixture().suite()
        self.assertEqual(suite.sandbox_json(["python3", "x.py"]), {"a": 2})

    def test_sandbox_json_fails_on_a_status_or_missing_output(self):
        answers = {"status": completed('{"a": 1}\n', returncode=3, stderr="Traceback\nboom\n")}
        answers["nothing"] = completed("no json here\n")
        for label, answer in answers.items():
            with self.subTest(label):
                self.use_sandbox(lambda argv, kwargs, answer=answer: answer)
                with self.assertRaisesRegex(smoke.Failure, r"python3 x.py arg exited with status"):
                    self.fixture().suite().sandbox_json(["python3", "x.py", "arg", "more"])


# ══ The headless start ════════════════════════════════════════════════════

GOOD_START = {
    "stage": "done",
    "help": {
        "status": 0,
        "stdout": "Usage:\n  dasher [OPTION...]\n\nHelp Options:\n  -h, --help  Show help options\n",
        "stderr": "",
    },
    "name_owned": True,
    "seconds_to_name": 0.21,
    "names_owned": ["org.dasher.gtk"],
    "names_on_bus": ["org.dasher.gtk"],
    "alive_seconds": 5.23,
    "exited_early": False,
    "name_owned_at_end": True,
    "actions": [],
    "exited_on_sigterm": False,
    "exit_status": -9,
    "name_released": True,
    "log": f"\n{BROADWAY_WARNING}\n",
}
HEADLESS_CHECKS = ["help", "start", "bus-name", "log", "shutdown"]


class HeadlessRunTests(SmokeTestCase):
    def script(self, result=None, log=None, status=0):
        """A flatpak that behaves like sandbox/headless.py in the sandbox."""

        def handler(argv, kwargs):
            work = bound(argv, "/run/dasher-smoke")
            if result is not None:
                (work / "headless.json").write_text(json.dumps(result))
            if log is not None:
                (work / "app.log").write_text(log)
            return completed(returncode=status, stderr="helper said\nsomething\n")

        self.use_sandbox(handler)

    def test_the_app_gets_nothing_of_the_host(self):
        fixture = self.fixture(GREEDY_FINISH_ARGS)
        self.script({"stage": "done", "name_owned": True}, log="hello\n")
        suite = fixture.suite("--alive-seconds", "2.5")
        result = suite.headless()
        self.assertEqual(result["log"], "hello\n")
        cmd = self.flatpak_calls[-1][0]
        self.assertEqual(shared(cmd), [])
        for socket in smoke.KNOWN_SOCKETS:
            self.assertIn(f"--nosocket={socket}", cmd)
        for negated in ("--nodevice=all", "--nodevice=dri", "--unshare=network"):
            self.assertIn(negated, cmd)
        self.assertIn("--runtime", cmd)
        work = suite.workdir()
        self.assertIn(f"--bind-mount=/usr/bin/flatpak-spawn={work / 'flatpak-spawn'}", cmd)
        self.assertEqual(
            cmd[cmd.index(str(fixture.build)) + 1 :],
            [
                "python3",
                "/run/dasher-smoke/headless.py",
                "/run/dasher-smoke/headless.json",
                "/run/dasher-smoke",
                "dasher",
                "org.dasher.gtk",
                "2.5",
            ],
        )
        self.assertEqual(self.flatpak_calls[-1][1]["timeout"], 182)

    def test_the_app_is_started_once_for_all_checks(self):
        self.script(dict(GOOD_START, log=None), log=GOOD_START["log"])
        suite = self.fixture().suite()
        for name in HEADLESS_CHECKS:
            with self.subTest(name):
                self.assertPasses(suite, name)
        self.assertEqual(len(self.flatpak_calls), 2)  # the probe and one start

    def test_a_helper_that_leaves_no_result_fails_every_check_once(self):
        self.script(None, status=1)
        suite = self.fixture().suite()
        for name in HEADLESS_CHECKS:
            with self.subTest(name):
                self.assertFails(
                    suite, name, r"left no result \(status 1\): helper said \| something"
                )
        self.assertEqual(len(self.flatpak_calls), 2)

    def test_a_result_without_a_log_file_has_an_empty_log(self):
        self.script({"stage": "done"})
        self.assertEqual(self.fixture().suite().headless()["log"], "")

    def test_an_error_of_the_helper_names_the_stage(self):
        self.script({"stage": "display", "error": "gtk4-broadwayd exited with status 1"})
        self.assertFails(
            self.fixture().suite(),
            "start",
            r"did not get as far as the app \(stage display\): gtk4-broadwayd exited",
        )

    def test_a_sandbox_that_cannot_start_is_not_retried(self):
        calls = []

        def hang(argv, kwargs):
            calls.append(argv)
            raise subprocess.TimeoutExpired(argv, kwargs["timeout"])

        self.use_sandbox(hang)
        suite = self.fixture().suite()
        # Every check that shares the start says the same, with the reason.
        for name in ("start", "bus-name", "log"):
            self.assertFails(
                suite,
                name,
                r"^the headless start did not get as far as the app \(stage sandbox\): "
                r"`python3 /run/dasher-smoke/headless.py /run/dasher-smoke/headless.json` in "
                r"the sandbox did not finish within 185 s and was stopped$",
            )
        self.assertEqual(len(calls), 1)

    def test_a_result_that_is_not_json_is_an_error_of_the_helper(self):
        def handler(argv, kwargs):
            (bound(argv, "/run/dasher-smoke") / "headless.json").write_text('{"stage": "do')
            return completed(returncode=1, stderr="killed\n")

        self.use_sandbox(handler)
        self.assertFails(
            self.fixture().suite(), "start", r"the helper left no result \(status 1\): killed"
        )

    def test_without_flatpak_the_checks_are_skipped(self):
        suite = self.fixture().suite()
        for name in HEADLESS_CHECKS:
            with self.subTest(name):
                self.assertSkips(suite, name, "flatpak is not installed")


class HeadlessJudgementTests(SmokeTestCase):
    """What the checks make of a result of sandbox/headless.py."""

    def suite_with(self, change=None, finish_args=None, **updates):
        suite = self.fixture(finish_args).suite()
        suite._headless = copy.deepcopy(GOOD_START)
        suite._headless.update(updates)
        if change:
            change(suite._headless)
        return suite

    def test_a_good_start_passes_everything(self):
        suite = self.suite_with()
        messages = {name: self.assertPasses(suite, name) for name in HEADLESS_CHECKS}
        self.assertIn("the app has no --version option", messages["help"])
        self.assertIn("alive for 5.23 s", messages["start"])
        self.assertIn("owned after 0.21 s", messages["bus-name"])
        self.assertIn("1 line(s) of output, 1 of them allowlisted", messages["log"])
        self.assertIn("stopped with SIGKILL (known issue", messages["shutdown"])

    # ── help ─────────────────────────────────────────────────────────────
    def test_help_that_fails(self):
        failed = {
            "status": 1,
            "stdout": "",
            "stderr": "warning\nGtk-WARNING: Failed to open display\n",
        }
        self.assertFails(
            self.suite_with(help=failed),
            "help",
            r"`dasher --help` exited with status 1: Gtk-WARNING: Failed to open display",
        )
        self.assertFails(
            self.suite_with(help={"status": 0, "stdout": "hello\n", "stderr": ""}),
            "help",
            r"exited with status 0",
        )
        self.assertFails(self.suite_with(lambda r: r.pop("help")), "help", r"--help was not run")

    def test_version_is_judged_when_help_lists_it(self):
        listing = dict(
            GOOD_START["help"], stdout=GOOD_START["help"]["stdout"] + "  --version  Show version\n"
        )
        ok = {"status": 0, "stdout": "Dasher 6.0.2\n", "stderr": ""}
        message = self.assertPasses(self.suite_with(help=listing, version=ok), "help")
        self.assertIn("`--help` and `--version` exit 0 (Dasher 6.0.2)", message)
        bad = {"status": 1, "stdout": "", "stderr": "Unknown option --version\n"}
        self.assertFails(
            self.suite_with(help=listing, version=bad),
            "help",
            r"`dasher --version` is listed by --help but exits with status 1",
        )

    # ── start ────────────────────────────────────────────────────────────
    def test_an_app_that_exits_on_its_own_fails(self):
        suite = self.suite_with(
            exited_early=True,
            exit_status=127,
            alive_seconds=0.02,
            log="a\nb\nc\nd\n/app/dasher: error while loading shared libraries: libdasher.so\n",
        )
        problems = self.assertFails(
            suite,
            "start",
            r"exited on its own after 0.02 s with status 127: b \| c \| d \| /app/dasher",
        )
        self.assertNotIn("pending_crash", problems[0])
        self.assertFails(suite, "shutdown", r"had already exited on its own")

    def test_a_crash_report_fails(self):
        crashed = self.suite_with(
            exited_early=True, exit_status=-11, pending_crash="signal 11\nframes"
        )
        self.assertFails(
            crashed, "start", r"status -11 \(the crash reporter wrote pending_crash.txt\)"
        )
        alive = self.suite_with(pending_crash="signal 6\nframes")
        self.assertFails(alive, "start", r"the crash reporter wrote pending_crash.txt: signal 6")
        self.assertFails(alive, "shutdown", r"a crash report was written: signal 6")

    # ── bus-name ─────────────────────────────────────────────────────────
    def test_a_name_that_is_not_owned_fails(self):
        self.assertFails(
            self.suite_with(name_owned=False, names_owned=[], names_on_bus=[]),
            "bus-name",
            r"org.dasher.gtk was not owned .* \(names on the bus: none\)",
        )
        self.assertFails(
            self.suite_with(name_owned=False, names_owned=[], names_on_bus=["org.example.A"]),
            "bus-name",
            r"names on the bus: org.example.A",
        )

    def test_a_name_that_is_given_up_fails(self):
        self.assertFails(
            self.suite_with(name_owned_at_end=False),
            "bus-name",
            r"org.dasher.gtk was given up while the app was running",
        )

    def test_a_name_the_metadata_does_not_grant_fails(self):
        other = [
            a.replace("org.dasher.gtk", "org.dasher.other") for a in smoke.EXPECTED_FINISH_ARGS
        ]
        problems = self.assertFails(
            self.suite_with(finish_args=other),
            "bus-name",
            r"owns org.dasher.gtk but the metadata's session bus policy does not grant\s+it",
        )
        self.assertTrue(
            any(
                "grants --own-name=org.dasher.other but the app does not take" in p
                for p in problems
            )
        )

    def test_another_name_the_app_takes_has_to_be_granted_too(self):
        names = ["org.dasher.gtk", "org.example.Extra"]
        self.assertFails(
            self.suite_with(names_owned=names, names_on_bus=names),
            "bus-name",
            r"owns org.example.Extra but .* \(--own-name=org.example.Extra\)",
        )

    def test_the_app_id_needs_no_grant(self):
        names = ["org.dasher.gtk", APP_ID]
        self.assertPasses(self.suite_with(names_owned=names, names_on_bus=names), "bus-name")

    def test_a_grant_the_app_does_not_use_fails(self):
        self.assertFails(
            self.suite_with(
                finish_args=smoke.EXPECTED_FINISH_ARGS + ["--own-name=org.example.Unused"]
            ),
            "bus-name",
            r"grants --own-name=org.example.Unused but the app does not take that name",
        )

    # ── log ──────────────────────────────────────────────────────────────
    def test_an_unknown_warning_or_an_error_in_the_log_fails(self):
        line = "(dasher:2): Gtk-WARNING **: 17:27:50.984: Unable to acquire session bus"
        problems = self.assertFails(
            self.suite_with(log=f"{BROADWAY_WARNING}\n{line}\n{line}\n"),
            "log",
            r"^unexpected line: Gtk-WARNING: Unable to acquire session bus$",
        )
        self.assertEqual(len(problems), 1)  # the same line twice is said once
        self.assertFails(
            self.suite_with(
                log="(dasher:2): Gdk-CRITICAL **: 17:27:50.984: gdk_x: assertion failed\n"
            ),
            "log",
            r"unexpected line: Gdk-CRITICAL",
        )

    def test_many_problems_in_the_log_are_cut_off(self):
        lines = "".join(
            f"(dasher:2): Gtk-WARNING **: 17:27:50.984: warning {n}\n" for n in range(30)
        )
        self.assertEqual(len(self.assertFails(self.suite_with(log=lines), "log", r"warning 0")), 10)

    def test_an_empty_log_passes(self):
        self.assertIn("0 line(s)", self.assertPasses(self.suite_with(log=""), "log"))

    # ── shutdown ─────────────────────────────────────────────────────────
    def test_exiting_on_sigterm_asks_for_the_known_issue_to_be_removed(self):
        for status in (0, -15):
            with self.subTest(status=status):
                suite = self.suite_with(exited_on_sigterm=True, exit_status=status)
                message = self.assertPasses(suite, "shutdown")
                self.assertIn("exits on SIGTERM", message)
                self.assertEqual(len(suite.notes), 1)
                self.assertIn("remove 'sigterm-ignored' from KNOWN_ISSUES", suite.notes[0])

    def test_ignoring_sigterm_fails_once_it_is_no_longer_a_known_issue(self):
        with mock.patch.dict(smoke.KNOWN_ISSUES, clear=True):
            self.assertFails(
                self.suite_with(),
                "shutdown",
                r"did not exit within 5 s of SIGTERM and had to be killed",
            )
            suite = self.suite_with(exited_on_sigterm=True, exit_status=-15)
            self.assertPasses(suite, "shutdown")
            self.assertEqual(suite.notes, [])

    def test_a_quit_action_on_the_bus_is_pointed_out(self):
        # Today there is none, so a clean exit cannot be asked for.
        for actions, noted in (([], False), (None, False), (["about"], False), (["quit"], True)):
            with self.subTest(actions=actions):
                suite = self.suite_with(actions=actions)
                self.assertPasses(suite, "shutdown")
                self.assertEqual(
                    [note for note in suite.notes if "exports a `quit` action" in note] != [], noted
                )
        suite = self.suite_with(lambda result: result.pop("actions", None))
        self.assertPasses(suite, "shutdown")
        self.assertEqual(suite.notes, [])

    def test_a_bad_exit_status_after_sigterm_fails(self):
        self.assertFails(
            self.suite_with(exited_on_sigterm=True, exit_status=1),
            "shutdown",
            r"after SIGTERM the app exited with status 1",
        )
        self.assertFails(
            self.suite_with(exited_on_sigterm=True, exit_status=-11),
            "shutdown",
            r"after SIGTERM the app exited with status -11",
        )

    def test_a_name_that_outlives_the_app_fails(self):
        self.assertFails(
            self.suite_with(name_released=False),
            "shutdown",
            r"org.dasher.gtk still has an owner after the app was stopped",
        )

    def test_the_known_issues_all_have_a_reason(self):
        for name, reason in smoke.KNOWN_ISSUES.items():
            self.assertTrue(reason.strip(), name)


# ══ Checks that judge what a sandbox helper prints ════════════════════════


class ScriptedSandboxTestCase(SmokeTestCase):
    def answer(self, stdout="", returncode=0, stderr="", effect=None):
        """Every sandbox command gets this answer; `effect(argv)` runs first."""

        def handler(argv, kwargs):
            if effect:
                effect(argv)
            return completed(stdout, returncode, stderr)

        self.use_sandbox(handler)

    def command(self):
        return self.flatpak_calls[-1][0]

    def positive(self):
        return shared(self.command())


def ldd_output(files, extra=""):
    lines = []
    for path in files:
        lines.append(f"@@ {path}")
        lines.append("\tlinux-vdso.so.1 (0x00007ffd0a1c2000)")
        lines.append("\tlibdasher.so => /app/libdasher.so (0x000073344bc00000)")
        lines.append("\tlibc.so.6 => /usr/lib/x86_64-linux-gnu/libc.so.6 (0x00007bcb1ce00000)")
        lines.append("\t/lib64/ld-linux-x86-64.so.2 (0x00007bcb20070000)")
    return "\n".join(lines) + "\n" + extra


ELF_PATHS = [
    "/app/dasher",
    "/app/lib/libgtkmm-4.0.so.0.0.0",
    "/app/libdasher.so",
    "/app/librust_tts_wrapper.so",
]


class LinkageTests(ScriptedSandboxTestCase):
    CHECK = "linkage"

    def test_passes_when_everything_resolves_in_the_app_or_the_runtime(self):
        self.answer(ldd_output(ELF_PATHS))
        fixture = self.fixture()
        message = self.passes(fixture.suite())
        self.assertEqual(message, "4 ELF files, 8 library references, all in /app or the runtime")
        cmd = self.command()
        self.assertEqual(self.positive(), [])
        self.assertIn("--runtime", cmd)
        self.assertEqual(cmd[-len(ELF_PATHS) :], ELF_PATHS)
        script = cmd[cmd.index("-c") + 1]
        self.assertIn('LD_LIBRARY_PATH="/app:/app/lib64:$LD_LIBRARY_PATH"', script)
        self.assertIn('ldd "$f"', script)

    def test_a_library_that_is_not_found_fails(self):
        self.answer(ldd_output(ELF_PATHS, "@@ /app/dasher\n\tlibgtkmm-4.0.so.0 => not found\n"))
        self.fails(r"^/app/dasher needs libgtkmm-4.0.so.0, which is not in the sandbox$")

    def test_a_library_from_outside_fails(self):
        for target in (
            "/run/host/usr/lib/libfoo.so.1",
            # Not below /app or /usr/lib, though the names start that way.
            "/usr/libexec/libfoo.so.1",
            "/usr/lib32/libfoo.so.1",
            "/application/libfoo.so.1",
            "/usr/lib/../libexec/libfoo.so.1",
        ):
            with self.subTest(target):
                self.answer(
                    ldd_output(ELF_PATHS, f"@@ /app/dasher\n\tlibfoo.so.1 => {target} (0x1)\n")
                )
                self.fails(rf"/app/dasher resolves libfoo.so.1 to {target}, outside /app")

    def test_the_library_directories_of_the_runtime_are_inside(self):
        extra = "".join(
            f"@@ /app/dasher\n\tlibfoo.so.1 => {target} (0x1)\n"
            for target in ("/usr/lib64/libfoo.so.1", "/usr/lib/libfoo.so.1", "/app/lib/libfoo.so.1")
        )
        self.answer(ldd_output(ELF_PATHS, extra))
        self.assertIn("11 library references", self.passes())

    def test_a_shared_onnxruntime_fails_even_when_it_resolves(self):
        self.answer(
            ldd_output(
                ELF_PATHS,
                "@@ /app/dasher\n\tlibonnxruntime.so.1 => /usr/lib/x86_64-linux-gnu/libonnxruntime.so.1 (0x1)\n",
            )
        )
        self.fails(r"/app/dasher loads the shared library libonnxruntime.so.1")

    def test_a_file_ldd_did_not_report_on_fails(self):
        self.answer(ldd_output(ELF_PATHS[:-1]))
        self.fails(r"ldd did not report on every file")
        self.answer(ldd_output(ELF_PATHS), returncode=1, stderr="sh: ldd: not found")
        self.fails(r"did not report on every file \(status 1\): sh: ldd: not found")

    def test_static_files_are_not_a_problem(self):
        self.answer(
            ldd_output(
                ELF_PATHS, "@@ /app/dasher\n\tnot a dynamic executable\n\tstatically linked\n"
            )
        )
        self.passes()

    def test_no_elf_files_fails_without_a_sandbox(self):
        fixture = self.fixture()
        for relative, _ in fixture.suite().app_elfs():
            (fixture.files / relative).write_text("not a binary")
        self.fails(r"no ELF file found", fixture.suite())
        self.assertEqual(self.flatpak_calls, [])


def speech_info(**changes):
    info = {
        "engines_status": 0,
        "engines": ["sherpaonnx", "openai", "elevenlabs", "azure", "edge", "google", "polly"],
        "catalogue_version": 2,
        "catalogue_size": 1781,
        "model": {
            "id": smoke.VOICE["id"],
            "sha256": smoke.VOICE["archive_sha256"],
            "sample_rate": smoke.VOICE["sample_rate"],
        },
        "models_dir": "/tmp/.rust-tts-wrapper/sherpaonnx",
    }
    info.update(changes)
    return json.dumps(info) + "\n"


class SpeechLibraryTests(ScriptedSandboxTestCase):
    CHECK = "speech-library"

    def test_passes_offline_with_the_expected_engines_and_voice(self):
        self.answer(speech_info())
        message = self.passes()
        self.assertEqual(message, "7 engines, 1781 catalogue entries, test voice listed")
        self.assertEqual(self.positive(), [])
        self.assertIn("--unshare=network", self.command())
        self.assertEqual(
            self.command()[-4:],
            ["python3", "/run/dasher-smoke/speech.py", "info", smoke.VOICE["id"]],
        )

    def test_what_the_library_reports_is_judged(self):
        model = json.loads(speech_info())["model"]
        for label, changes, pattern in (
            ("load error", {"load_error": "wrong ELF class"}, r"does not load: wrong ELF class"),
            (
                "engine missing",
                {"engines": ["openai", "elevenlabs", "azure", "google", "polly"]},
                r"engine sherpaonnx is not compiled in \(engines: openai, ",
            ),
            ("no engines", {"engines": []}, r"engine azure is not compiled in \(engines: none\)"),
            (
                "system engine",
                {"engines": json.loads(speech_info())["engines"] + ["system"]},
                r"engine system is compiled in, but the sandbox has no access to\s+speech-dispatcher",
            ),
            ("empty catalogue", {"catalogue_size": 0, "model": None}, r"voice catalogue is empty"),
            ("voice not listed", {"model": None}, r"is not in the built-in catalogue: pin another"),
            (
                "another archive",
                {"model": dict(model, sha256="0" * 64)},
                r"lists another archive for .* \(sha256 0{64}\): update VOICE",
            ),
            (
                "another sample rate",
                {"model": dict(model, sample_rate=16000)},
                r"the catalogue gives 16000 Hz",
            ),
        ):
            with self.subTest(label):
                self.answer(speech_info(**changes))
                self.fails(pattern)

    def test_a_helper_that_dies_fails(self):
        self.answer("", returncode=139, stderr="Segmentation fault\n")
        self.fails(r"exited with status 139: Segmentation fault")


class VoiceTestCase(ScriptedSandboxTestCase):
    """A voice cache with small files in place of the pinned voice: the
    layout the wrapper's installer leaves, as a known-good cache has it."""

    MODEL = b"not really an onnx model"
    # Where the wrapper keeps its voices below $HOME.
    MODELS_DIR = ".rust-tts-wrapper/sherpaonnx"
    VOICE_DIR = f"{MODELS_DIR}/{smoke.VOICE['id']}"
    MODEL_PATH = f"{VOICE_DIR}/{smoke.VOICE['model_file']}"
    RECORD_PATH = f"{VOICE_DIR}/.installed"
    TOKENS_PATH = f"{VOICE_DIR}/tokens.txt"
    DATA_PATH = f"{VOICE_DIR}/espeak-ng-data/phontab"
    MANIFEST_PATH = "voicegarden/lexicons.json"
    BUNDLE = "voicegarden/en-GB"
    STAMP_PATH = f"{BUNDLE}/.sha256"
    # Not needed to speak; its content tells one installation from another.
    LEXICON_PATH = f"{BUNDLE}/lexicon.txt"
    BUNDLE_SHA = hashlib.sha256(b"en-GB.tar.gz").hexdigest()
    # What voicegarden-lexicons leaves when only its manifest arrived, and
    # what the installer writes to stderr then (from a run with the real
    # library whose download of the bundle was cut off).
    BUNDLE_WARNING = (
        f"sherpa: G2P bundle for '{smoke.VOICE['id']}' unavailable (fetching bundle 'en-GB': "
        "downloading https://github.com/AACTools/voicegarden-lexicons/releases/latest/"
        "download//en-GB.tar.gz) \N{EM DASH} the auto-lexicon path will be used\n"
    )

    def setUp(self):
        super().setUp()
        self.cache = self.tmp / "voices"
        # Where a complete installation is kept, and where one is made.
        self.home = self.cache / "installed"
        self.installing = self.cache / "installing"
        self.replaced = self.cache / "replaced"
        self.model = self.home / self.MODEL_PATH
        pin = {"model_sha256": hashlib.sha256(self.MODEL).hexdigest()}
        self.patch(mock.patch.dict(smoke.VOICE, pin))

    def suite(self, *arguments, fixture=None):
        fixture = fixture or self.fixture()
        return fixture.suite("--voice-cache", str(self.cache), *arguments)

    @classmethod
    def manifest(cls, **entry):
        """lexicons.json with the bundle of the voice; `entry` changes it."""
        bundle = {
            "lang": "en-GB",
            "bcp47": "en-GB",
            "phonetisaurus": {"order": 7},
            "file": "en-GB.tar.gz",
            "sha256": cls.BUNDLE_SHA,
        }
        bundle.update(entry)
        other = {"lang": "en", "bcp47": "en-US", "file": "en.tar.gz", "sha256": "0" * 64}
        return json.dumps({"version": "v0.6.1", "languages": ["odd", other, bundle]})

    @classmethod
    def voice(cls, lexicon="lexicon", without=(), **changes):
        """The files of a complete installation, {path below $HOME:
        content}. `without` leaves paths out, `changes` replaces contents
        (the keys are the names of the *_PATH constants, lower case)."""
        record = {
            "id": smoke.VOICE["id"],
            "engine": "sherpaonnx",
            "sha256": smoke.VOICE["archive_sha256"],
            "files": ["tokens.txt", "espeak-ng-data/phontab", smoke.VOICE["model_file"]],
            "installed_at": 1795464204,
        }
        files = {
            cls.RECORD_PATH: json.dumps(record),
            cls.MODEL_PATH: cls.MODEL,
            cls.TOKENS_PATH: "_ 0\na 1\n",
            cls.DATA_PATH: "phonemes",
            cls.MANIFEST_PATH: cls.manifest(),
            f"{cls.BUNDLE}/en-GB.fst": "dictionary",
            f"{cls.BUNDLE}/en-GB.pho": "phonemes",
            f"{cls.BUNDLE}/phonetisaurus.fst": "model for unknown words",
            f"{cls.BUNDLE}/NOTICE": "licence",
            cls.LEXICON_PATH: lexicon,
            cls.STAMP_PATH: cls.BUNDLE_SHA,
        }
        for name, content in changes.items():
            files[getattr(cls, f"{name.upper()}_PATH")] = content
        return {path: content for path, content in files.items() if path not in without}

    @classmethod
    def model_only(cls):
        """What the installer leaves when the voice arrived and of the
        pronunciation data only the manifest."""
        return {
            path: content
            for path, content in cls.voice().items()
            if not path.startswith(cls.BUNDLE + "/")
        }

    def voice_from_an_earlier_run(self, **kwargs):
        for relative, content in self.voice(lexicon="earlier", **kwargs).items():
            write(self.home / relative, content)

    def snapshot(self, root=None):
        """{path below `root` (default: the cache): content} of every file."""
        root = root or self.cache
        return {
            p.relative_to(root).as_posix(): p.read_bytes()
            for p in sorted(root.rglob("*"))
            if p.is_file()
        }

    def unreachable(self, pattern, suite):
        """The check of the class gives up on the download server: a skip
        of its own kind."""
        self.addCleanup(suite.cleanup)
        with self.assertRaisesRegex(smoke.Unreachable, pattern):
            check_named(self.CHECK)["run"](suite)

    def synth(self, wav=None, **changes):
        """Script the answer of speech.py synth; `wav(path)` writes the audio."""
        result = {
            "created": True,
            "create_seconds": 0.01,
            "synth_status": 0,
            "synth_seconds": 1.7,
            "error": None,
        }
        result.update(changes)

        def effect(argv):
            if wav:
                wav(bound(argv, "/run/dasher-smoke") / "speech.wav")

        self.answer(json.dumps(result) + "\n", effect=effect)


HANGS = object()  # an attempt of SpeechInstallTests.attempts that never finishes


class SpeechInstallTests(VoiceTestCase):
    CHECK = "speech-install"

    def installed(self, **changes):
        return (
            json.dumps(dict({"installed": True, "seconds": 4.3, "error": None}, **changes)) + "\n"
        )

    def attempts(self, *outcomes):
        """One answer of speech.py per download attempt, in this order:
          - an error message the installer fails with (or None);
          - the files ({path below $HOME: content}) an installer that
            reports success leaves, or (files, what it wrote to stderr);
          - HANGS: the attempt does not finish, and `flatpak build` is
            stopped when its time is up.
        A further attempt fails the test. Returns the $HOME of each attempt;
        self.timeouts has the time each one was given."""
        pending = list(outcomes)
        homes = []
        self.timeouts = []

        def handler(argv, kwargs):
            self.assertTrue(pending, "one download attempt too many")
            home = bound(argv, "/run/dasher-voices")
            homes.append(home)
            self.timeouts.append(kwargs["timeout"])
            outcome = pending.pop(0)
            if outcome is HANGS:
                write(home / f"{self.MODELS_DIR}/.{smoke.VOICE['id']}.part", "half an archive")
                raise subprocess.TimeoutExpired(argv, kwargs["timeout"])
            if not isinstance(outcome, (dict, tuple)):
                return completed(self.installed(installed=False, error=outcome))
            files, stderr = outcome if isinstance(outcome, tuple) else (outcome, "")
            for relative, content in files.items():
                write(home / relative, content)
            return completed(self.installed(), stderr=stderr)

        self.use_sandbox(handler)
        return homes

    def assert_earlier_voice_untouched(self, before):
        """The cache is, file for file, what it was: the voice of the
        earlier run, no download in progress, nothing moved aside."""
        self.assertEqual(self.snapshot(), before)
        self.assertFalse(self.installing.exists())
        self.assertFalse(self.replaced.exists())
        self.assertEqual(self.suite().cached_voice(), self.model)

    def test_is_skipped_unless_the_network_is_asked_for(self):
        self.skips(r"run with --network to include it")
        self.assertEqual(self.flatpak_calls, [])

    def test_a_build_without_network_permission_fails_before_any_sandbox(self):
        fixture = self.fixture([a for a in smoke.EXPECTED_FINISH_ARGS if a != "--share=network"])
        self.fails(
            r"the build does not share the network \(--share=network\)",
            self.suite("--network", fixture=fixture),
        )
        self.assertEqual(self.flatpak_calls, [])

    def test_installs_from_nothing_with_network_and_nothing_else(self):
        homes = self.attempts(self.voice())
        # Left behind by a run that was interrupted: not part of this one.
        write(self.installing / "stale.part", "half a download")
        write(self.replaced / "stale", "moved aside, then killed")
        message = self.passes(self.suite("--network"))
        self.assertEqual(
            message,
            f"{smoke.VOICE['id']} installed in 4.3 s: complete, with its pronunciation data, "
            "and the model file matches its pin",
        )
        self.assertEqual(self.sleeps, [])
        self.assertEqual(homes, [self.installing])
        self.assertEqual(self.timeouts, [smoke.INSTALL_TIMEOUT])
        self.assertEqual(self.positive(), ["--share=network"])
        cmd = self.command()
        self.assertIn(f"--bind-mount=/run/dasher-voices={self.installing}", cmd)
        self.assertIn("--env=HOME=/run/dasher-voices", cmd)
        self.assertIn("--env=VOICEGARDEN_LEXICON_DIR=/run/dasher-voices/voicegarden", cmd)
        self.assertEqual(
            cmd[-4:], ["python3", "/run/dasher-smoke/speech.py", "install", smoke.VOICE["id"]]
        )
        # What was installed is now the voice the synthesis check uses, and
        # it is all there is in the cache.
        self.assertEqual(sorted(p.name for p in self.cache.iterdir()), ["installed"])
        self.assertEqual(sorted(self.snapshot(self.home)), sorted(self.voice()))
        self.assertEqual(self.suite().cached_voice(), self.model)

    def test_a_new_installation_replaces_the_one_of_an_earlier_run_whole(self):
        for label, exchange in (("in one step", None), ("in two", lambda a, b: False)):
            with self.subTest(label):
                shutil.rmtree(self.cache, ignore_errors=True)
                self.voice_from_an_earlier_run()
                write(self.home / "old-file", "old")
                self.attempts(self.voice())
                with contextlib.ExitStack() as stack:
                    if exchange:
                        stack.enter_context(
                            mock.patch.object(smoke, "exchange_paths", side_effect=exchange)
                        )
                    self.passes(self.suite("--network"))
                # No file of the old installation is left among the new ones.
                self.assertEqual(sorted(p.name for p in self.cache.iterdir()), ["installed"])
                self.assertEqual(
                    self.snapshot(self.home), {k: _bytes(v) for k, v in self.voice().items()}
                )

    def test_the_old_voice_stays_until_the_new_one_is_complete_and_in_place(self):
        """Seen from the installer: while it runs, and at every attempt,
        the voice of the earlier run is in the cache as it was."""
        self.voice_from_an_earlier_run()
        before = self.snapshot(self.home)
        seen = []

        def handler(argv, kwargs):
            seen.append(self.snapshot(self.home))
            home = bound(argv, "/run/dasher-voices")
            files = self.voice() if len(seen) == 3 else self.model_only()
            for relative, content in files.items():
                write(home / relative, content)
            return completed(self.installed())

        self.use_sandbox(handler)
        self.passes(self.suite("--network"))
        self.assertEqual(seen, [before] * 3)
        self.assertEqual((self.home / self.LEXICON_PATH).read_text(), "lexicon")

    def test_what_the_installer_reports_is_judged_at_once(self):
        # None of these is the network: one attempt, no pause, a failure,
        # and the voice of an earlier run is left as it was.
        other = self.voice(
            record=json.dumps({"id": "piper-other", "sha256": "0" * 64, "files": []})
        )
        mismatch = (
            f"sherpa: G2P bundle for '{smoke.VOICE['id']}' unavailable (fetching bundle 'en-GB': "
            "checksum mismatch for en-GB.tar.gz: manifest aa, got bb) \N{EM DASH} the auto-lexicon "
            "path will be used\n"
        )
        for label, outcome, pattern in (
            (
                "checksum",
                "checksum mismatch: expected 169ca8af, got 0badc0de",
                r"is not the file the catalogue pins: checksum mismatch: expected 169ca8af, got",
            ),
            (
                "checksum of the pronunciation data",
                "required pronunciation bundle failed to provision (checksum mismatch: "
                "expected aa, got bb); the model download is retained",
                r"is not the file the catalogue pins: required pronunciation bundle",
            ),
            (
                "checksum of the pronunciation data, as a warning",
                (self.model_only(), mismatch),
                r"pronunciation data of .* is not the file its manifest pins: fetching bundle "
                r"'en-GB': checksum mismatch for en-GB.tar.gz: manifest aa, got bb$",
            ),
            (
                "pronunciation data verified against another checksum",
                self.voice(stamp="f" * 64),
                rf"the bundle en-GB was verified against the checksum f{{64}}, lexicons.json "
                rf"names {self.BUNDLE_SHA}",
            ),
            (
                "disk",
                "insufficient disk: 9 bytes needed (with expansion headroom), 1 free",
                r"^installing .* failed: insufficient disk",
            ),
            ("unknown", None, r"^installing .* failed: None"),
            (
                "no model",
                self.voice(without=[self.MODEL_PATH]),
                r"^the model .*\.onnx does not exist$",
            ),
            (
                "nothing but pronunciation data",
                {k: v for k, v in self.voice().items() if k.startswith("voicegarden/")},
                r"^the installer reported success but .* is not installed$",
            ),
            (
                "another model",
                self.voice(model=b"something else"),
                r"\.onnx has sha256 [0-9a-f]{64}, pinned is [0-9a-f]{64}",
            ),
            (
                "no record",
                self.voice(without=[self.RECORD_PATH]),
                r"\.installed, the installer's record of a finished installation, is missing",
            ),
            (
                "a record cut short",
                self.voice(record='{"id": "pip'),
                r"\.installed, .* cannot be read",
            ),
            (
                "the record of another archive",
                other,
                rf"\.installed is the record of piper-other from the archive 0{{64}}, pinned is "
                rf"{smoke.VOICE['id']} from {smoke.VOICE['archive_sha256']}",
            ),
            (
                "a file of the archive missing",
                self.voice(without=[self.DATA_PATH]),
                r"1 of the 3 files of the voice archive are missing \(the first: "
                r"espeak-ng-data/phontab\)",
            ),
            (
                "no token table",
                self.voice(tokens=""),
                r"the token table tokens.txt is missing or empty",
            ),
        ):
            with self.subTest(label):
                shutil.rmtree(self.cache, ignore_errors=True)
                self.voice_from_an_earlier_run()
                before = self.snapshot()
                homes = self.attempts(outcome)
                self.fails(pattern, self.suite("--network"))
                self.assertEqual(len(homes), 1)
                self.assertEqual(self.sleeps, [])
                self.assert_earlier_voice_untouched(before)

    def test_a_library_that_does_not_load_fails(self):
        self.answer(json.dumps({"load_error": "no such file"}))
        self.fails(r"does not load: no such file", self.suite("--network"))

    def test_an_installer_that_prints_no_result_fails(self):
        for label, answer in (
            ("crashed", {"returncode": -11, "stderr": "Segmentation fault"}),
            ("no JSON", {"stdout": "{not json\n"}),
            ("nothing", {}),
        ):
            with self.subTest(label):
                self.voice_from_an_earlier_run()
                before = self.snapshot()
                self.answer(**answer)
                self.fails(r"speech\.py install exited with status", self.suite("--network"))
                self.assert_earlier_voice_untouched(before)

    def test_a_download_that_fails_is_tried_again_after_a_pause(self):
        homes = self.attempts(
            "network: https://example.org/voice.tar.bz2: Connection reset by peer",
            (self.model_only(), self.BUNDLE_WARNING),  # the pronunciation data did not arrive
            self.voice(),
        )
        message = self.passes(self.suite("--network"))
        self.assertIn("installed in 4.3 s at attempt 3: complete,", message)
        self.assertEqual(self.sleeps, list(smoke.INSTALL_BACKOFF))
        # The same directory every time: the installer keeps what it has.
        self.assertEqual(homes, [self.installing] * 3)
        self.assertEqual(self.suite().cached_voice(), self.model)

    def test_a_partial_download_does_not_replace_a_working_voice(self):
        """The review finding. The installer reports success for an English
        voice whose pronunciation data did not arrive (it only warns), and
        any file in the pronunciation directory, lexicons.json alone, used
        to count as the data: one attempt, a pass, the working voice of the
        earlier run replaced by one that cannot speak offline."""
        bundle = self.BUNDLE
        # label: (what the installer leaves, what is reported as missing)
        partial = {
            "only the manifest": (self.model_only(), "en-GB/en-GB.fst, en-GB/en-GB.pho, "),
            "no pronunciation directory at all": (
                {k: v for k, v in self.voice().items() if not k.startswith("voicegarden/")},
                "the manifest lexicons.json, or it cannot be read, en-GB/en-GB.fst",
            ),
            "the bundle half unpacked": (
                self.voice(
                    without=[f"{bundle}/en-GB.pho", f"{bundle}/phonetisaurus.fst", self.STAMP_PATH]
                ),
                "en-GB/en-GB.pho, en-GB/phonetisaurus.fst, en-GB/.sha256, which",
            ),
            "unpacked but not verified": (
                self.voice(without=[self.STAMP_PATH]),
                "en-GB/.sha256, which is written when the bundle is unpacked",
            ),
            "an empty dictionary": (
                dict(self.voice(), **{f"{bundle}/en-GB.fst": ""}),
                "en-GB/en-GB.fst",
            ),
            "no model for unknown words": (
                self.voice(without=[f"{bundle}/phonetisaurus.fst"]),
                "en-GB/phonetisaurus.fst",
            ),
            "a manifest cut short": (
                self.voice(manifest=self.manifest()[:40]),
                "the manifest lexicons.json, or it cannot be read",
            ),
            "a manifest that is not a manifest": (
                self.voice(manifest="[1, 2]"),
                "the manifest lexicons.json, or it cannot be read",
            ),
            "a manifest without the bundle": (
                self.voice(manifest=self.manifest(lang="en-AU")),
                "an entry for the bundle en-GB in lexicons.json",
            ),
        }
        for label, (files, missing) in partial.items():
            with self.subTest(label):
                shutil.rmtree(self.cache, ignore_errors=True)
                self.sleeps.clear()
                self.voice_from_an_earlier_run()
                before = self.snapshot()
                homes = self.attempts(*[(files, self.BUNDLE_WARNING)] * smoke.INSTALL_ATTEMPTS)
                suite = self.suite("--network")
                self.unreachable(
                    r"^the voice could not be downloaded in 3 attempts \(the model was "
                    r"installed but its pronunciation data is incomplete, missing: .*"
                    + re.escape(missing)
                    + r".*; the installer said: fetching bundle 'en-GB': downloading https:.*\)"
                    r"\. Not a failure of the build",
                    suite,
                )
                # An attempt that failed, like any other: all of them, with pauses.
                self.assertEqual(len(homes), smoke.INSTALL_ATTEMPTS)
                self.assertEqual(self.sleeps, list(smoke.INSTALL_BACKOFF))
                self.assert_earlier_voice_untouched(before)
                # ... and the synthesis still speaks with the earlier voice.
                self.synth(wav=write_wav)
                self.assertPasses(suite, "speech-synthesis")
                self.assertIn(f"--bind-mount=/run/dasher-voices={self.home}", self.command())

    def test_a_bundle_without_a_model_for_unknown_words_is_complete_when_it_has_none(self):
        self.attempts(
            self.voice(
                manifest=self.manifest(phonetisaurus=None),
                without=[f"{self.BUNDLE}/phonetisaurus.fst"],
            )
        )
        self.passes(self.suite("--network"))

    def test_a_download_that_hangs_is_stopped_and_tried_again(self):
        """The review finding. The time limit of an attempt was a failure
        of the check; it is an attempt that failed."""
        self.voice_from_an_earlier_run()
        before = self.snapshot()
        homes = self.attempts(HANGS, HANGS, HANGS)
        suite = self.suite("--network")
        self.unreachable(
            r"^the voice could not be downloaded in 3 attempts \(the download did not finish "
            rf"within {smoke.INSTALL_TIMEOUT} s and was stopped\)\. Not a failure of the build",
            suite,
        )
        self.assertEqual(homes, [self.installing] * 3)
        self.assertEqual(self.timeouts, [smoke.INSTALL_TIMEOUT] * 3)
        self.assertEqual(self.sleeps, list(smoke.INSTALL_BACKOFF))
        self.assertIn("could not be downloaded in 3 attempts", suite.voice_unreachable)
        self.assert_earlier_voice_untouched(before)
        self.synth(wav=write_wav)
        self.assertPasses(suite, "speech-synthesis")

    def test_a_download_that_hangs_once_goes_on_where_it_stopped(self):
        homes = self.attempts(HANGS, self.voice())
        message = self.passes(self.suite("--network"))
        self.assertIn("at attempt 2: complete,", message)
        self.assertEqual(self.sleeps, list(smoke.INSTALL_BACKOFF[:1]))
        # The second attempt found what the first had fetched.
        self.assertEqual(homes, [self.installing] * 2)
        self.assertIn(f"{self.MODELS_DIR}/.{smoke.VOICE['id']}.part", self.snapshot(self.home))

    def test_the_time_limit_of_an_attempt_bounds_the_whole_download(self):
        """A release leg waits at most ten minutes for a server that sends
        nothing, pauses included; and an attempt has the time a download of
        40 MB needs on a slow line."""
        worst = smoke.INSTALL_ATTEMPTS * smoke.INSTALL_TIMEOUT + sum(smoke.INSTALL_BACKOFF)
        self.assertLessEqual(worst, 10 * 60)
        self.assertGreaterEqual(smoke.INSTALL_TIMEOUT, 120)

    def test_a_network_that_stays_away_is_a_skip_not_a_failure(self):
        self.assertEqual(smoke.INSTALL_ATTEMPTS, 3)
        self.assertEqual(len(smoke.INSTALL_BACKOFF), smoke.INSTALL_ATTEMPTS - 1)
        self.assertEqual(list(smoke.INSTALL_BACKOFF), sorted(smoke.INSTALL_BACKOFF))
        for label, outcome, reason in (
            ("no answer", "network: dns error: failed to lookup address", r"\(network: dns error"),
            (
                "pronunciation data fails",
                "required pronunciation bundle failed to provision (network: timed out); "
                "the model download is retained",
                r"\(required pronunciation bundle failed to provision \(network: timed out\)",
            ),
            (
                "pronunciation data missing, and no word from the installer",
                self.model_only(),
                r"\(the model was installed but its pronunciation data is incomplete, missing: "
                r"en-GB/en-GB.fst, en-GB/en-GB.pho, en-GB/phonetisaurus.fst, en-GB/.sha256, "
                r"which is written when the bundle is unpacked\)",
            ),
        ):
            with self.subTest(label):
                shutil.rmtree(self.cache, ignore_errors=True)
                self.sleeps.clear()
                self.voice_from_an_earlier_run()
                before = self.snapshot()
                homes = self.attempts(*[outcome] * smoke.INSTALL_ATTEMPTS)
                suite = self.suite("--network")
                self.unreachable(
                    r"^the voice could not be downloaded in 3 attempts " + reason + r".*"
                    r"Not a failure of the build",
                    suite,
                )
                self.assertEqual(len(homes), 3)
                self.assertEqual(self.sleeps, list(smoke.INSTALL_BACKOFF))
                self.assertIn("could not be downloaded in 3 attempts", suite.voice_unreachable)
                # Nothing half-installed is kept, and nothing is lost.
                self.assert_earlier_voice_untouched(before)

    def test_without_an_earlier_voice_a_failed_download_leaves_the_cache_empty(self):
        for label, outcome in (("hangs", HANGS), ("partial", self.model_only())):
            with self.subTest(label):
                self.attempts(*[outcome] * smoke.INSTALL_ATTEMPTS)
                suite = self.suite("--network")
                self.unreachable(r"could not be downloaded in 3 attempts", suite)
                self.assertEqual(list(self.cache.iterdir()), [])
                self.assertIsNone(suite.cached_voice())

    def test_a_wrong_file_after_a_failed_attempt_still_fails(self):
        for label, first in (("network", "network: timed out"), ("hang", HANGS)):
            with self.subTest(label):
                homes = self.attempts(first, "checksum mismatch: expected 169ca8af, got 0badc0de")
                self.fails(r"not the file the catalogue", self.suite("--network"))
                self.assertEqual(len(homes), 2)
                self.assertFalse(self.installing.exists())

    def test_a_flatpak_that_cannot_be_started_is_a_failure_not_a_retry(self):
        calls = []

        def gone(argv, kwargs):
            calls.append(argv)
            raise FileNotFoundError(2, "No such file or directory", "flatpak")

        self.use_sandbox(gone)
        self.fails(r"could not be started: .*No such file or directory", self.suite("--network"))
        self.assertEqual(len(calls), 1)
        self.assertEqual(self.sleeps, [])

    def test_a_cache_that_cannot_be_used_fails_with_the_reason(self):
        write(self.cache, "a file where the cache directory should be")
        self.use_flatpak()
        self.fails(
            rf"^the voice cache {re.escape(str(self.cache))} cannot be used: ",
            self.suite("--network"),
        )

    def test_install_failure_kind(self):
        bundle = "required pronunciation bundle failed to provision ({}); the model download"
        for error, kind in (
            ("checksum mismatch: expected a, got b", "checksum"),
            (bundle.format("checksum mismatch: expected a, got b"), "checksum"),
            ("network: checksum mismatch: expected a, got b", "checksum"),
            ("network: connection refused", "network"),
            ("network: download implausibly small (9 bytes < 1024)", "network"),
            (
                "network: peer closed connection without sending TLS close_notify: "
                "https://docs.rs/rustls/latest/rustls/manual/_03_howto/index.html#unexpected-eof",
                "network",
            ),
            (bundle.format("network: timed out"), "network"),
            ("no network: x", "other"),
            ("insufficient disk: 9 bytes needed (with expansion headroom), 1 free", "other"),
            ("corrupt archive: short read", "other"),
            ("model id not found in any compiled-in catalogue", "other"),
            ("part rename: Permission denied", "other"),
            ("", "other"),
            (None, "other"),
        ):
            with self.subTest(error):
                self.assertEqual(smoke.install_failure_kind(error), kind)

    def test_bundle_warning(self):
        self.assertEqual(
            smoke.bundle_warning("noise\n" + self.BUNDLE_WARNING + "more\n"),
            "fetching bundle 'en-GB': downloading https://github.com/AACTools/"
            "voicegarden-lexicons/releases/latest/download//en-GB.tar.gz",
        )
        self.assertEqual(
            smoke.bundle_warning(
                "sherpa: G2P bundle for 'x' unavailable (lexicon archive: fetching https://e/"
                "lexicons.json (and no cache at /run/dasher-voices/voicegarden/lexicons.json)) "
                "\N{EM DASH} the auto-lexicon path will be used"
            ),
            "lexicon archive: fetching https://e/lexicons.json (and no cache at "
            "/run/dasher-voices/voicegarden/lexicons.json)",
        )
        for nothing in ("", None, "sherpa: generated lexicon.txt\n"):
            self.assertIsNone(smoke.bundle_warning(nothing))


def _bytes(content):
    return content if isinstance(content, bytes) else content.encode()


class VoiceCacheTests(VoiceTestCase):
    """What counts as a complete voice, and how one takes the place of
    another."""

    def install(self, root, **kwargs):
        for relative, content in self.voice(**kwargs).items():
            write(root / relative, content)
        return root

    def test_a_complete_installation_has_no_problems(self):
        home = self.install(self.tmp / "home")
        self.assertEqual(smoke.voice_problems(home), [])
        self.assertEqual(smoke.lexicon_problems(home), ([], []))
        # Another voice of the same catalogue is judged by its own pins.
        other = dict(smoke.VOICE, id="piper-other", lexicon="en")
        self.assertEqual(len(smoke.voice_problems(home, other)), 3)
        missing, wrong = smoke.lexicon_problems(home, other)
        self.assertEqual((len(missing), wrong), (3, []))

    def test_an_empty_directory_lacks_everything(self):
        home = self.tmp / "nothing"
        home.mkdir()
        self.assertEqual(len(smoke.voice_problems(home)), 3)
        missing, wrong = smoke.lexicon_problems(home)
        self.assertEqual(
            missing,
            [
                "the manifest lexicons.json, or it cannot be read",
                "en-GB/en-GB.fst",
                "en-GB/en-GB.pho",
                "en-GB/.sha256, which is written when the bundle is unpacked",
            ],
        )
        self.assertEqual(wrong, [])

    def test_the_record_is_read_whatever_is_in_it(self):
        for label, record in (
            ("not JSON", "installed"),
            ("a list", "[]"),
            ("no files", json.dumps({"id": smoke.VOICE["id"]})),
            ("files that are not a list", json.dumps({"files": 3})),
        ):
            with self.subTest(label):
                home = self.install(self.tmp / label.replace(" ", "-"), record=record)
                self.assertEqual(
                    smoke.voice_problems(home),
                    [
                        ".installed, the installer's record of a finished installation, is "
                        "missing or cannot be read"
                    ],
                )

    def test_the_archive_checksum_of_the_record_is_compared_whatever_its_case(self):
        record = json.loads(self.voice()[self.RECORD_PATH])
        record["sha256"] = record["sha256"].upper()
        home = self.install(self.tmp / "home", record=json.dumps(record))
        self.assertEqual(smoke.voice_problems(home), [])

    def test_the_record_has_to_name_this_voice_and_this_archive(self):
        good = json.loads(self.voice()[self.RECORD_PATH])
        for label, change in (
            ("another voice from the same archive", {"id": "piper-en_GB-cori-high"}),
            ("this voice from another archive", {"sha256": "0" * 64}),
            ("no archive", {"sha256": None}),
        ):
            with self.subTest(label):
                home = self.install(
                    self.tmp / label.replace(" ", "-"), record=json.dumps(dict(good, **change))
                )
                [problem] = smoke.voice_problems(home)
                self.assertRegex(
                    problem,
                    rf"^\.installed is the record of .* from the archive .*, pinned is "
                    rf"{smoke.VOICE['id']} from {smoke.VOICE['archive_sha256']}$",
                )

    def test_exchange_paths_swaps_two_directories_in_one_step(self):
        first, second = self.tmp / "first", self.tmp / "second"
        write(first / "a", "1")
        write(second / "b", "2")
        # Asked of the system here, apart from the script: can two directories
        # of this file system be exchanged? (renameat2, RENAME_EXCHANGE = 2)
        probe = [self.tmp / "probe-1", self.tmp / "probe-2"]
        for directory in probe:
            directory.mkdir()
        libc = ctypes.CDLL(None)
        if not hasattr(libc, "renameat2") or libc.renameat2(
            -100, os.fsencode(probe[0]), -100, os.fsencode(probe[1]), 2
        ):
            self.skipTest("renameat2(RENAME_EXCHANGE) is not available here")
        self.assertIs(smoke.exchange_paths(first, second), True)
        self.assertEqual(self.snapshot(first), {"b": b"2"})
        self.assertEqual(self.snapshot(second), {"a": b"1"})
        # Nothing to swap with: nothing happens.
        self.assertIs(smoke.exchange_paths(first, self.tmp / "missing"), False)
        self.assertEqual(self.snapshot(first), {"b": b"2"})

    def test_exchange_paths_says_so_where_it_cannot_be_done(self):
        first, second = self.tmp / "first", self.tmp / "second"
        write(first / "a", "1")
        write(second / "b", "2")
        for label, library in (
            ("no C library to ask", OSError("no libc")),
            ("a C library without renameat2", [object()]),
        ):
            with self.subTest(label), mock.patch.object(smoke.ctypes, "CDLL", side_effect=library):
                self.assertIs(smoke.exchange_paths(first, second), False)
                self.assertEqual(self.snapshot(first), {"a": b"1"})

    def promote(self, exchange):
        """Promote a new installation over an old one; `exchange` stands in
        for exchange_paths. Returns the suite."""
        self.voice_from_an_earlier_run()
        self.install(self.installing)
        suite = self.suite()
        with mock.patch.object(smoke, "exchange_paths", side_effect=exchange):
            suite.promote_voice(self.installing)
        return suite

    def test_promotion_without_the_one_step_swap_moves_the_old_voice_aside_first(self):
        suite = self.promote(lambda a, b: False)
        self.assertEqual(sorted(p.name for p in self.cache.iterdir()), ["installed"])
        self.assertEqual((self.home / self.LEXICON_PATH).read_text(), "lexicon")
        self.assertEqual(suite.cached_voice(), self.model)

    def test_a_promotion_that_fails_puts_the_old_voice_back(self):
        real_rename = Path.rename

        def rename(path, target):
            if path == self.installing:
                raise OSError(28, "No space left on device")
            return real_rename(path, target)

        with mock.patch.object(Path, "rename", autospec=True, side_effect=rename):
            with self.assertRaisesRegex(OSError, "No space left"):
                self.promote(lambda a, b: False)
        self.assertEqual((self.home / self.LEXICON_PATH).read_text(), "earlier")
        self.assertFalse(self.replaced.exists())
        self.assertEqual(self.suite().cached_voice(), self.model)

    def test_a_promotion_that_fails_fails_the_check_and_keeps_the_old_voice(self):
        self.voice_from_an_earlier_run()
        before = self.snapshot()
        self.CHECK = "speech-install"
        self.use_sandbox(
            lambda argv, kwargs: (
                self.install(bound(argv, "/run/dasher-voices")),
                completed(json.dumps({"installed": True, "seconds": 1.0})),
            )[1]
        )
        real_rename = Path.rename

        def rename(path, target):
            if path == self.installing:
                raise OSError(28, "No space left on device")
            return real_rename(path, target)

        with mock.patch.object(smoke, "exchange_paths", return_value=False):
            with mock.patch.object(Path, "rename", autospec=True, side_effect=rename):
                self.fails(r"cannot be used: .*No space left", self.suite("--network"))
        self.assertEqual(self.snapshot(), before)

    def test_a_voice_left_aside_by_a_run_that_was_killed_is_put_back(self):
        """Between the two renames there is no `installed`. A run that ends
        there leaves the old voice whole in `replaced`."""
        self.install(self.replaced, lexicon="earlier")
        self.install(self.installing)
        suite = self.suite()
        self.assertEqual(suite.cached_voice(), self.model)
        self.assertFalse(self.replaced.exists())
        self.assertEqual((self.home / self.LEXICON_PATH).read_text(), "earlier")
        # It is not put in the place of a voice that is there.
        self.install(self.replaced, lexicon="older still")
        self.assertEqual(suite.cached_voice(), self.model)
        self.assertEqual((self.home / self.LEXICON_PATH).read_text(), "earlier")

    def test_cache_problems_say_what_is_wrong_with_the_cached_voice(self):
        suite = self.suite()
        self.assertEqual(suite.cache_problems(), ["nothing is installed there"])
        self.voice_from_an_earlier_run()
        self.assertEqual(suite.cache_problems(), [])
        (self.home / self.STAMP_PATH).write_text("0" * 64)
        (self.home / self.BUNDLE / "en-GB.pho").unlink()
        self.model.write_bytes(b"tampered")
        problems = suite.cache_problems()
        self.assertEqual(len(problems), 3)
        self.assertRegex(problems[0], r"\.onnx has sha256 [0-9a-f]{64}, pinned is")
        self.assertEqual(
            problems[1], "the pronunciation data is incomplete, missing: en-GB/en-GB.pho"
        )
        self.assertRegex(problems[2], r"^the bundle en-GB was verified against the checksum 0{64}")
        self.assertIsNone(suite.cached_voice())


class SpeechSynthesisTests(VoiceTestCase):
    CHECK = "speech-synthesis"

    def test_is_skipped_without_a_cached_voice(self):
        self.use_flatpak()
        self.skips(
            r"^no verified copy of the voice .* \(no earlier run left one in .*/voices\): "
            r"run once with --network to install it$"
        )

    def test_a_voice_whose_download_failed_in_this_run_is_a_skip_of_that_kind(self):
        self.use_flatpak()
        suite = self.suite()
        suite.voice_unreachable = "the voice could not be downloaded in 3 attempts (network: x)"
        self.unreachable(
            r"^no voice to speak with: the voice could not be downloaded in 3 attempts "
            r"\(network: x\), and no earlier run left one in .*/voices$",
            suite,
        )
        # With the voice of an earlier run the synthesis is tested all the same.
        self.voice_from_an_earlier_run()
        self.synth(wav=write_wav)
        self.passes(suite)

    def test_an_installation_in_progress_is_not_a_voice(self):
        self.use_flatpak()
        for relative, content in self.voice().items():
            write(self.installing / relative, content)
            write(self.cache / relative, content)
        self.assertIsNone(self.suite().cached_voice())
        self.skips(r"no verified copy of the voice")

    def test_a_cached_voice_that_is_not_complete_is_not_spoken_with(self):
        """What the script used to leave in the cache after a download
        whose pronunciation data did not arrive, and everything else that
        is not the whole pinned voice: no sandbox is started for it, the
        skip says what is missing, and it is the skip of a server that is
        away when the download of this run failed."""
        bundle = self.BUNDLE
        cases = {
            "only the manifest of the pronunciation data": (
                {"without": [p for p in self.voice() if p.startswith(bundle + "/")]},
                r"the pronunciation data is incomplete, missing: en-GB/en-GB.fst, ",
            ),
            "no pronunciation data": (
                {"without": [p for p in self.voice() if p.startswith("voicegarden/")]},
                r"incomplete, missing: the manifest lexicons.json, or it cannot be read, ",
            ),
            "pronunciation data that was not verified": (
                {"without": [self.STAMP_PATH]},
                r"incomplete, missing: en-GB/.sha256, which is written when",
            ),
            "pronunciation data of another manifest": (
                {"stamp": "e" * 64},
                r"the bundle en-GB was verified against the checksum e{64}, lexicons.json names",
            ),
            "another model": ({"model": b"tampered"}, r"\.onnx has sha256 [0-9a-f]{64}, pinned is"),
            "no model": ({"without": [self.MODEL_PATH]}, r"the model .*\.onnx does not exist"),
            "no record of the installation": (
                {"without": [self.RECORD_PATH]},
                r"\.installed, the installer's record of a finished installation, is missing",
            ),
            "a file of the archive missing": (
                {"without": [self.DATA_PATH]},
                r"1 of the 3 files of the voice archive are missing",
            ),
        }
        self.use_flatpak()
        for label, (kwargs, reason) in cases.items():
            with self.subTest(label):
                shutil.rmtree(self.cache, ignore_errors=True)
                self.flatpak_calls.clear()
                self.voice_from_an_earlier_run(**kwargs)
                suite = self.suite()
                self.assertIsNone(suite.cached_voice())
                self.skips(
                    r"^no verified copy of the voice .* \(the one in .*/voices/installed cannot "
                    r"be used \(.*" + reason + r".*\)\): run once with --network",
                    suite,
                )
                self.assertEqual(len(self.flatpak_calls), 1)  # the probe only
                suite.voice_unreachable = "the voice could not be downloaded in 3 attempts (x)"
                self.unreachable(
                    r"^no voice to speak with: the voice could not be downloaded in 3 attempts "
                    r"\(x\), and the one in .*/voices/installed cannot be used \(.*" + reason,
                    suite,
                )
                self.assertEqual(len(self.flatpak_calls), 1)

    def test_is_skipped_without_flatpak(self):
        self.voice_from_an_earlier_run()
        self.skips("flatpak is not installed")

    def test_synthesizes_offline_from_the_cache(self):
        self.voice_from_an_earlier_run()
        self.synth(wav=write_wav)
        suite = self.suite()
        self.assertEqual(suite.cached_voice(), self.model)
        message = self.passes(suite)
        self.assertRegex(
            message, r"^2\.00 s at 22050 Hz, RMS 21\d\d, synthesized in 1.7 s without network$"
        )
        self.assertEqual(self.positive(), [])
        cmd = self.command()
        self.assertIn("--unshare=network", cmd)
        self.assertIn(f"--bind-mount=/run/dasher-voices={self.home}", cmd)
        self.assertEqual(
            cmd[-6:],
            [
                "python3",
                "/run/dasher-smoke/speech.py",
                "synth",
                smoke.VOICE["id"],
                "/run/dasher-smoke/speech.wav",
                smoke.SYNTH_TEXT,
            ],
        )

    def test_what_the_synthesis_produces_is_judged(self):
        self.voice_from_an_earlier_run()
        for label, kwargs, pattern in (
            ("load error", {"load_error": "missing"}, r"does not load: missing"),
            (
                "no engine",
                {"created": False, "error": "model not installed"},
                r"creating the sherpaonnx engine for .* failed: model not installed",
            ),
            ("synthesis failed", {"synth_status": -1, "error": "boom"}, r"synthesis failed: boom"),
            ("no file", {}, r"synthesis failed: None"),
            ("silence", {"wav": lambda p: write_wav(p, amplitude=0)}, r"silent or nearly so"),
            ("too short", {"wav": lambda p: write_wav(p, seconds=0.1)}, r"0.10 s of audio"),
            (
                "not a wav",
                {"wav": lambda p: p.write_bytes(b"ID3 not a wave file")},
                r"is not a WAV file",
            ),
        ):
            with self.subTest(label):
                self.synth(**kwargs)
                self.fails(pattern)

    def test_a_synthesis_that_hangs_is_a_failure(self):
        """Nothing is downloaded here, so there is nobody else to wait for."""
        self.voice_from_an_earlier_run()

        def hang(argv, kwargs):
            raise subprocess.TimeoutExpired(argv, kwargs["timeout"])

        self.use_sandbox(hang)
        self.fails(r"speech\.py synth` in the sandbox did not finish within 300 s and was stopped")

    def test_the_default_cache_is_below_xdg_cache_home(self):
        suite = self.fixture().suite()
        with mock.patch.dict(os.environ, {"XDG_CACHE_HOME": "/somewhere/cache"}):
            self.assertEqual(suite.voice_cache(), Path("/somewhere/cache/dasher-flatpak-smoke"))
        with mock.patch.dict(os.environ, {"HOME": "/home/someone"}):
            os.environ.pop("XDG_CACHE_HOME", None)
            self.assertEqual(suite.voice_cache(), Path("/home/someone/.cache/dasher-flatpak-smoke"))
            self.assertEqual(suite.voice_home(), suite.voice_cache() / "installed")


class AudioDeviceTests(ScriptedSandboxTestCase):
    CHECK = "audio-device"
    ARGUMENTS = ("--host-checks",)

    def setUp(self):
        super().setUp()
        self.fake_host()

    def test_is_skipped_without_a_sound_server_socket(self):
        (self.runtime_dir / "pulse/native").unlink()
        self.skips(r"no PulseAudio/PipeWire socket")
        with mock.patch.dict(os.environ):
            del os.environ["XDG_RUNTIME_DIR"]
            self.skips(r"no PulseAudio/PipeWire socket")
        self.assertEqual(self.flatpak_calls, [])

    def test_only_the_sound_socket_is_shared(self):
        self.answer('{"status": 0, "error": null}\n')
        fixture = self.fixture(GREEDY_FINISH_ARGS)
        self.passes(fixture.suite())
        self.assertEqual(self.positive(), ["--socket=pulseaudio"])
        for socket in DISPLAY_SOCKETS:
            self.assertIn(f"--nosocket={socket}", self.command())
        self.assertEqual(self.command()[-2:], ["python3", "/run/dasher-smoke/alsa.py"])

    def test_a_build_without_the_socket_fails_before_the_device_is_opened(self):
        self.answer('{"status": 0, "error": null}\n')
        fixture = self.fixture(
            [a for a in smoke.EXPECTED_FINISH_ARGS if a != "--socket=pulseaudio"]
        )
        self.fails(r"does not declare --socket=pulseaudio", fixture.suite())
        self.assertEqual(len(self.flatpak_calls), 1)  # the probe only

    def test_a_device_that_does_not_open_fails(self):
        self.answer('{"status": -111, "error": "Connection refused"}\n')
        self.fails(r'snd_pcm_open\("default"\) failed: Connection refused')


class InputDeviceTests(ScriptedSandboxTestCase):
    CHECK = "input-devices"
    ARGUMENTS = ("--host-checks",)

    def setUp(self):
        super().setUp()
        self.fake_host()

    def test_is_skipped_on_a_host_without_input_devices(self):
        self.host_has_input = False
        self.skips(r"this host has no /dev/input")
        self.assertEqual(self.flatpak_calls, [])

    def test_only_the_declared_devices_are_shared(self):
        self.answer("by-id\nby-path\nevent0\nevent1\n")
        message = self.passes()
        self.assertEqual(message, "visible with --device=all (4 entries)")
        self.assertEqual(self.positive(), ["--device=all"])
        self.assertEqual(self.command()[-3:], ["sh", "-c", "ls /dev/input"])

    def test_a_build_without_devices_fails(self):
        self.answer("", returncode=2, stderr="ls: cannot access '/dev/input'")
        fixture = self.fixture([a for a in smoke.EXPECTED_FINISH_ARGS if a != "--device=all"])
        self.fails(
            r"/dev/input is not visible in the sandbox with --device=\(none\)", fixture.suite()
        )
        self.assertEqual(self.positive(), [])


# ══ What reaches outside the sandbox is asked for ═════════════════════════

HOST_CHECKS = ["audio-device", "input-devices", "lint-manifest", "lint-builddir"]


class HostCheckTests(SmokeTestCase):
    """A run without --host-checks stays inside the sandbox: no linter (it
    uses the network, and the session bus when it is a Flatpak), no sound
    server socket, no device of the host."""

    def setUp(self):
        super().setUp()
        # A machine on which every host check could run.
        self.fake_host()
        self.linter({})
        self.lint_record = self.tmp / "lint.ran"

    def greedy(self, *arguments):
        fixture = self.fixture(GREEDY_FINISH_ARGS).with_manifest()
        self.use_flatpak(lambda argv, kwargs: completed('{"status": 0, "error": null}\n'))
        return fixture.suite("--voice-cache", str(self.tmp / "voices"), *arguments)

    def run_everything(self, *arguments):
        suite = self.greedy(*arguments)
        self.addCleanup(suite.cleanup)
        out = io.StringIO()
        smoke.run_checks(suite, [entry["name"] for entry in smoke.CHECKS], out=out)
        return status_lines(out.getvalue())

    def shared(self):
        """Every positive permission of every `flatpak` command of the run."""
        return sorted(arg for cmd, _ in self.flatpak_calls for arg in shared(cmd))

    def test_the_host_checks_are_the_ones_tagged_host(self):
        self.assertEqual([e["name"] for e in smoke.CHECKS if "host" in e["tags"]], HOST_CHECKS)
        for entry in smoke.CHECKS:
            if "host" in entry["tags"]:
                self.assertIn("(needs --host-checks)", entry["summary"])

    def test_each_is_skipped_before_it_does_anything(self):
        suite = self.greedy()
        for name in HOST_CHECKS:
            with self.subTest(name):
                self.assertSkips(
                    suite,
                    name,
                    r"^reaches outside the sandbox \(.+\); run with --host-checks to include it$",
                )
        self.assertEqual(self.flatpak_calls, [])
        self.assertFalse(self.lint_record.exists())

    def test_a_default_run_shares_nothing_and_starts_nothing_on_the_host(self):
        lines = self.run_everything()
        for name in HOST_CHECKS:
            self.assertRegex(lines[name], r"^SKIP  [\w-]+: reaches outside the sandbox", name)
        self.assertFalse(self.lint_record.exists())
        # The sandboxes that did run (the probe, ldd, the app, the speech
        # library): `flatpak build` each, with nothing switched on.
        self.assertGreaterEqual(len(self.flatpak_calls), 4)
        for cmd, kwargs in self.flatpak_calls:
            self.assertEqual(cmd[:4], ["flatpak", "build", "--readonly", "--die-with-parent"])
            for variable in (*smoke.SESSION_VARIABLES, *smoke.SOUND_VARIABLES):
                self.assertNotIn(variable, kwargs["env"])
        self.assertEqual(self.shared(), [])

    def test_network_alone_adds_the_network_to_one_sandbox(self):
        self.run_everything("--network")
        self.assertEqual(set(self.shared()), {"--share=network"})
        with_network = [cmd for cmd, _ in self.flatpak_calls if "--share=network" in cmd]
        self.assertTrue(with_network)
        for cmd in with_network:
            self.assertEqual(cmd[-2:], ["install", smoke.VOICE["id"]])
        self.assertFalse(self.lint_record.exists())

    def test_host_checks_adds_the_linter_the_sound_socket_and_the_devices(self):
        lines = self.run_everything("--host-checks")
        for name in HOST_CHECKS:
            self.assertNotIn("reaches outside the sandbox", lines[name])
        self.assertEqual(self.lint_record.read_text().split(), ["ran", "ran"])
        self.assertEqual(self.shared(), ["--device=all", "--device=dri", "--socket=pulseaudio"])
        for cmd, _ in self.flatpak_calls:
            # Each to the one sandbox of its check, and never the network.
            if "--socket=pulseaudio" in cmd:
                self.assertEqual(cmd[-1], "/run/dasher-smoke/alsa.py")
            if "--device=all" in cmd:
                self.assertEqual(cmd[-1], "ls /dev/input")
            self.assertEqual(cmd[1], "build")


# ══ flatpak-builder-lint ══════════════════════════════════════════════════


class LintTests(SmokeTestCase):
    CHECK = "lint-builddir"
    ARGUMENTS = ("--host-checks",)

    def with_manifest(self):
        return self.fixture().with_manifest()

    def test_the_accepted_errors_pass_and_are_named(self):
        for kind, name in (("manifest", "lint-manifest"), ("builddir", "lint-builddir")):
            with self.subTest(kind):
                expected = sorted(smoke.LINT_EXPECTED_ERRORS[kind])
                self.linter({"errors": expected}, status=1)
                fixture = self.with_manifest()
                suite = fixture.suite()
                message = self.assertPasses(suite, name)
                self.assertEqual(
                    message,
                    f"no unexpected error ({len(expected)} accepted: {', '.join(expected)})",
                )
                self.assertEqual(suite.notes, [])
                target = (
                    fixture.repo / f"packaging/flatpak/{APP_ID}.yaml"
                    if kind == "manifest"
                    else fixture.build
                )
                self.assertEqual(
                    (self.tmp / "lint.args").read_text().split(), ["--flag", kind, str(target)]
                )
                # Started in the work directory, where it may leave its state.
                self.assertEqual((self.tmp / "lint.cwd").read_text().strip(), str(suite.workdir()))

    def test_a_new_error_fails(self):
        self.linter(
            {
                "errors": ["appid-url-not-reachable", "finish-args-arbitrary-dbus-access"],
                "info": ["finish-args-arbitrary-dbus-access: finish-args has socket access"],
            },
            status=1,
        )
        self.assertFails(
            self.with_manifest().suite(),
            "lint-manifest",
            r"new lint error finish-args-arbitrary-dbus-access \(finish-args has socket access\)",
        )

    def test_a_clean_report_passes_and_asks_for_the_list_to_be_emptied(self):
        self.linter({})
        suite = self.fixture().suite()
        self.assertEqual(self.passes(suite), "no errors")
        self.assertEqual(len(suite.notes), len(smoke.LINT_EXPECTED_ERRORS["builddir"]))
        self.assertTrue(all("is no longer reported: remove it" in note for note in suite.notes))

    def test_no_output_at_all_counts_as_a_clean_report(self):
        self.linter("")
        self.passes()

    def test_warnings_are_notes(self):
        self.linter(
            {
                "errors": sorted(smoke.LINT_EXPECTED_ERRORS["builddir"]),
                "warnings": ["finish-args-flatpak-spawn-access"],
            },
            status=1,
        )
        suite = self.fixture().suite()
        self.passes(suite)
        self.assertEqual(suite.notes, ["lint warning finish-args-flatpak-spawn-access"])

    def test_a_linter_that_breaks_fails(self):
        self.linter(
            "Traceback (most recent call last):\n  boom\n", status=1, stderr="Exception: ccache"
        )
        self.fails(r"printed no JSON \(status 1\): Exception: ccache")
        self.linter({"info": []}, status=2, stderr="usage: flatpak-builder-lint")
        self.fails(r"exited with status 2: usage: flatpak-builder-lint")

    def test_a_linter_that_hangs_fails(self):
        self.linter({})
        real = self._run

        def hang(cmd, *args, **kwargs):
            if "fake-lint" in cmd[0]:
                raise subprocess.TimeoutExpired(cmd, kwargs["timeout"])
            return real(cmd, *args, **kwargs)

        with mock.patch.object(smoke.subprocess, "run", side_effect=hang):
            self.fails(
                r"^flatpak-builder-lint builddir did not finish within 300 s and was stopped$"
            )

    def test_a_linter_that_cannot_be_started_fails_with_the_reason(self):
        with mock.patch.dict(os.environ, {"FLATPAK_BUILDER_LINT": str(self.tmp / "no-linter")}):
            self.fails(r"^flatpak-builder-lint builddir could not be started: .*No such file")

    def test_a_flatpak_that_does_not_answer_has_no_linter_to_offer(self):
        fixture = self.with_manifest()
        for label, error in (
            ("hangs", lambda argv, kwargs: subprocess.TimeoutExpired(argv, kwargs["timeout"])),
            ("cannot be started", lambda argv, kwargs: OSError(8, "Exec format error")),
        ):
            with self.subTest(label), mock.patch.dict(os.environ):
                os.environ.pop("FLATPAK_BUILDER_LINT", None)

                def broken(argv, kwargs, error=error):
                    raise error(argv, kwargs)

                self.use_flatpak(broken)
                self.assertSkips(
                    fixture.suite(), "lint-builddir", "flatpak-builder-lint is not available"
                )

    def test_the_manifest_lint_is_skipped_without_a_manifest(self):
        self.linter({})
        self.assertSkips(self.fixture().suite(), "lint-manifest", r"\.yaml does not exist")

    def test_where_the_linter_is_looked_for(self):
        fixture = self.with_manifest()
        suite = fixture.suite()
        self.addCleanup(suite.cleanup)
        with mock.patch.dict(os.environ):
            os.environ.pop("FLATPAK_BUILDER_LINT", None)
            # Nothing installed.
            with self.assertRaisesRegex(smoke.Skip, "flatpak-builder-lint is not available"):
                smoke.lint_command(suite)
            # flatpak without the Builder app.
            self.use_flatpak(
                lambda argv, kwargs: completed(returncode=1, stderr="error: not installed")
            )
            with self.assertRaisesRegex(smoke.Skip, "install org.flatpak.Builder from Flathub"):
                smoke.lint_command(suite)
            self.assertEqual(self.flatpak_calls[-1][0], ["flatpak", "info", "org.flatpak.Builder"])
            # The Builder app: both trees read-only, the work directory to run in.
            self.use_flatpak()
            self.assertEqual(
                smoke.lint_command(suite),
                [
                    "flatpak",
                    "run",
                    "--command=flatpak-builder-lint",
                    f"--filesystem={suite.workdir()}",
                    f"--filesystem={fixture.repo}:ro",
                    f"--filesystem={fixture.build}:ro",
                    "org.flatpak.Builder",
                ],
            )
            # A linter on PATH comes before the Builder app ...
            self.tools["flatpak-builder-lint"] = "/usr/bin/flatpak-builder-lint"
            self.assertEqual(smoke.lint_command(suite), ["flatpak-builder-lint"])
            # ... and the environment variable before both.
            os.environ["FLATPAK_BUILDER_LINT"] = "docker run --rm 'the image' flatpak-builder-lint"
            self.assertEqual(
                smoke.lint_command(suite),
                ["docker", "run", "--rm", "the image", "flatpak-builder-lint"],
            )

    def test_without_a_linter_both_checks_are_skipped(self):
        with mock.patch.dict(os.environ):
            os.environ.pop("FLATPAK_BUILDER_LINT", None)
            suite = self.with_manifest().suite()
            for name in ("lint-manifest", "lint-builddir"):
                self.assertSkips(suite, name, "flatpak-builder-lint is not available")


class InstalledPermissionsTests(SmokeTestCase):
    CHECK = "installed-permissions"

    def test_is_skipped_unless_an_app_is_named(self):
        self.skips(r"pass --installed APP_ID")

    def test_is_skipped_without_flatpak(self):
        self.skips("flatpak is not installed", self.fixture().suite("--installed", APP_ID))

    def test_reads_the_metadata_and_never_runs_the_app(self):
        self.use_flatpak(lambda argv, kwargs: completed(metadata_text()))
        message = self.passes(self.fixture().suite("--installed", APP_ID))
        self.assertIn(f"{APP_ID} as installed has the", message)
        self.assertEqual(
            self.flatpak_calls, [(["flatpak", "info", "--show-metadata", APP_ID], mock.ANY)]
        )

    def test_an_installed_app_with_other_permissions_fails(self):
        greedy = metadata_text(smoke.EXPECTED_FINISH_ARGS + ["--socket=session-bus"])
        self.use_flatpak(lambda argv, kwargs: completed(greedy))
        self.fails(
            r"unexpected permission --socket=session-bus",
            self.fixture().suite("--installed", APP_ID),
        )
        odd = metadata_text(extra="\n[Policy background]\nautostart=true\n")
        self.use_flatpak(lambda argv, kwargs: completed(odd))
        self.fails(
            r"the installed metadata grants something this check does not know",
            self.fixture().suite("--installed", APP_ID),
        )

    def test_an_app_that_is_not_installed_fails(self):
        self.use_flatpak(
            lambda argv, kwargs: completed(
                returncode=1, stderr="error: org.x/*unspecified* not installed\n"
            )
        )
        self.fails(
            r"`flatpak info org.x` failed: error: org.x/\*unspecified\* not installed",
            self.fixture().suite("--installed", "org.x"),
        )

    def test_a_flatpak_that_hangs_or_cannot_be_started_fails(self):
        for label, error, pattern in (
            (
                "hangs",
                lambda argv, kwargs: subprocess.TimeoutExpired(argv, kwargs["timeout"]),
                rf"^`flatpak info org.x` did not finish within {smoke.TOOL_TIMEOUT} s and was "
                r"stopped$",
            ),
            (
                "cannot be started",
                lambda argv, kwargs: OSError(8, "Exec format error"),
                r"^`flatpak info org.x` could not be started: .*Exec format error$",
            ),
        ):
            with self.subTest(label):

                def broken(argv, kwargs, error=error):
                    raise error(argv, kwargs)

                self.use_flatpak(broken)
                self.fails(pattern, self.fixture().suite("--installed", "org.x"))


# ══ Runner and command line ═══════════════════════════════════════════════


class RunnerTests(SmokeTestCase):
    def run_main(self, *arguments, env=None):
        """(exit status, stdout, stderr) of smoke-test.py's main()."""
        out, err = io.StringIO(), io.StringIO()
        environment = {"GITHUB_ACTIONS": "false"}
        environment.update(env or {})
        with mock.patch.dict(os.environ, environment):
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                try:
                    status = smoke.main(list(arguments))
                except SystemExit as e:
                    status = e.code
        return status, out.getvalue(), err.getvalue()

    def arguments(self, fixture, *more):
        return [str(fixture.build), "--repo", str(fixture.repo), *more]

    def main(self, fixture, *more, env=None):
        """run_main() on the build and the checkout of `fixture`."""
        return self.run_main(*self.arguments(fixture, *more), env=env)

    def program(self, fixture, *more):
        """The script as a program, in a process of its own."""
        script = [sys.executable, "-I", "-B", str(TESTS_DIR / "smoke-test.py")]
        return REAL_RUN(
            script + self.arguments(fixture, *more), capture_output=True, text=True, cwd=self.tmp
        )

    def test_list_prints_every_check_and_needs_no_build(self):
        status, out, _ = self.run_main("--list")
        self.assertEqual(status, 0)
        lines = out.splitlines()
        self.assertEqual([line.split()[0] for line in lines], [e["name"] for e in smoke.CHECKS])
        for line, entry in zip(lines, smoke.CHECKS):
            self.assertIn(entry["summary"], line)
            if entry["tags"]:
                self.assertTrue(line.endswith(f"[{', '.join(entry['tags'])}]"), line)
        self.assertEqual(self.flatpak_calls, [])

    def test_check_names_are_unique_and_have_a_summary(self):
        names = [entry["name"] for entry in smoke.CHECKS]
        self.assertEqual(len(names), len(set(names)))
        for entry in smoke.CHECKS:
            self.assertTrue(entry["summary"].strip())
            self.assertRegex(entry["name"], r"^[a-z][a-z-]+$")
        # The voice has to be installed before it can be spoken with.
        self.assertLess(names.index("speech-install"), names.index("speech-synthesis"))

    def test_usage_errors_exit_2(self):
        fixture = self.fixture()
        for label, arguments, pattern in (
            ("no build dir", [], "BUILD_DIR is required"),
            (
                "unknown check",
                self.arguments(fixture, "--check", "nonsense"),
                r"unknown check\(s\): nonsense",
            ),
            (
                "unknown skip",
                self.arguments(fixture, "--skip", "files,nope"),
                r"unknown check\(s\): nope",
            ),
            (
                "unknown allow-skip",
                self.arguments(fixture, "--allow-skip", "nope"),
                r"unknown check\(s\): nope",
            ),
            ("not a build dir", [str(self.tmp)], "has no `metadata` file"),
            ("missing dir", [str(self.tmp / "nowhere")], "has no `metadata` file"),
            ("build dir needed", ["--check", "files"], "BUILD_DIR is required"),
            ("unknown option", self.arguments(fixture, "--frobnicate"), "unrecognized arguments"),
        ):
            with self.subTest(label):
                status, out, err = self.run_main(*arguments)
                self.assertEqual(status, 2)
                self.assertEqual(out, "")
                self.assertRegex(err, pattern)

    def test_a_run_prints_one_line_per_check_and_a_summary(self):
        fixture = self.fixture()
        status, out, _ = self.main(fixture, "--check", "permissions,files", "--check", "cleanup")
        self.assertEqual(status, 0)
        lines = out.splitlines()
        self.assertEqual(len(lines), 5, out)
        self.assertRegex(
            lines[0], r"^PASS  permissions: \d+ permissions, no session-bus socket, offline build$"
        )
        self.assertRegex(lines[1], r"^PASS  files: \d+ files, 1 alphabets$")
        self.assertRegex(lines[2], r"^PASS  cleanup: nothing left of ")
        self.assertEqual(lines[3], "")
        self.assertEqual(lines[4], "Summary: 3 passed, 0 failed, 0 skipped (of 3)")

    def test_a_failure_lists_its_problems_and_exits_1(self):
        fixture = self.fixture()
        fixture.set_metadata(
            smoke.EXPECTED_FINISH_ARGS + ["--socket=session-bus", "--filesystem=host"]
        )
        (fixture.files / "libdasher.so").unlink()
        status, out, _ = self.main(fixture, "--check", "permissions,files,licences")
        self.assertEqual(status, 1)
        lines = out.splitlines()
        self.assertEqual(
            lines[0], "FAIL  permissions: metadata and finish-args are exactly the expected set"
        )
        self.assertRegex(
            lines[1], r"^        - unexpected permission --filesystem=host \(the whole host"
        )
        self.assertRegex(
            lines[2], r"^        - unexpected permission --socket=session-bus \(full session bus"
        )
        self.assertRegex(lines[3], r"^FAIL  files: ")
        self.assertEqual(lines[4], "        - /app/libdasher.so is missing")
        self.assertRegex(lines[5], r"^PASS  licences: ")
        self.assertEqual(lines[-2], "Summary: 1 passed, 2 failed, 0 skipped (of 3)")
        self.assertEqual(lines[-1], "Failed: permissions, files")
        self.assertNotIn("::error", out)

    def test_skip_and_check_select_the_checks(self):
        fixture = self.fixture()
        status, out, _ = self.main(
            fixture, "--check", "files,licences,cleanup", "--skip", "licences"
        )
        self.assertEqual(status, 0)
        self.assertEqual([line.split()[1] for line in out.splitlines()[:2]], ["files:", "cleanup:"])
        self.assertIn("Summary: 2 passed, 0 failed, 0 skipped (of 2)", out)

    def test_everything_runs_by_default_and_what_cannot_run_is_skipped(self):
        fixture = self.fixture()
        with mock.patch.dict(os.environ, {"XDG_RUNTIME_DIR": str(self.tmp / "no-runtime-dir")}):
            os.environ.pop("FLATPAK_BUILDER_LINT", None)
            status, out, _ = self.main(fixture, "--voice-cache", str(self.tmp / "voices"))
        self.assertEqual(status, 0, out)
        ran = {name: line[:4] for name, line in status_lines(out).items()}
        self.assertEqual(list(ran), [e["name"] for e in smoke.CHECKS])
        for name in STATIC_CHECKS:
            if name not in ("desktop-file", "metainfo"):
                self.assertEqual(ran[name], "PASS", name)
        for entry in smoke.CHECKS:
            if entry["tags"] or entry["name"] in (
                "desktop-file",
                "metainfo",
                "installed-permissions",
            ):
                self.assertEqual(ran[entry["name"]], "SKIP", entry["name"])
        self.assertRegex(out, r"Summary: 8 passed, 0 failed, \d+ skipped \(of \d+\)")
        self.assertEqual(self.flatpak_calls, [])

    def test_strict_turns_a_skip_into_a_failure(self):
        fixture = self.fixture()
        arguments = self.arguments(fixture, "--check", "files,installed-permissions,speech-install")
        status, out, _ = self.run_main(*arguments)
        self.assertEqual(status, 0)
        self.assertRegex(out, r"(?m)^SKIP  installed-permissions: no installed app named")
        self.assertIn("Summary: 1 passed, 0 failed, 2 skipped (of 3)", out)

        status, out, _ = self.run_main(*arguments, "--strict")
        self.assertEqual(status, 1)
        self.assertEqual(
            out.splitlines()[-1],
            "Skipped but required (--strict): speech-install, installed-permissions",
        )

        status, out, _ = self.run_main(*arguments, "--strict", "--allow-skip", "speech-install")
        self.assertEqual(status, 1)
        self.assertEqual(
            out.splitlines()[-1], "Skipped but required (--strict): installed-permissions"
        )

        status, out, _ = self.run_main(
            *arguments, "--strict", "--allow-skip", "speech-install,installed-permissions"
        )
        self.assertEqual(status, 0)
        self.assertNotIn("Skipped but required", out)

    def unreachable_voice(self, *errors):
        """A sandbox in which every voice download fails with the next of
        `errors` (the last one from then on)."""
        errors = list(errors)

        def handler(argv, kwargs):
            error = errors.pop(0) if len(errors) > 1 else errors[0]
            return completed(json.dumps({"installed": False, "seconds": 1.0, "error": error}))

        self.use_sandbox(handler)

    def test_strict_lets_a_download_server_that_is_away_through_with_a_warning(self):
        fixture = self.fixture()
        arguments = self.arguments(
            fixture,
            *("--check", "files,speech-install,speech-synthesis", "--network", "--strict"),
            *("--voice-cache", str(self.tmp / "voices")),
        )
        self.unreachable_voice("network: connection refused")
        status, out, _ = self.run_main(*arguments)
        self.assertEqual(status, 0, out)
        self.assertRegex(
            out, r"(?m)^SKIP  speech-install: the voice could not be downloaded in 3 attempts"
        )
        self.assertRegex(out, r"(?m)^SKIP  speech-synthesis: no voice to speak with: the voice")
        lines = out.splitlines()
        self.assertEqual(lines[-2], "Summary: 1 passed, 0 failed, 2 skipped (of 3)")
        self.assertEqual(
            lines[-1], "Skipped, server not reachable: speech-install, speech-synthesis"
        )
        self.assertNotIn("::warning", out)
        self.assertNotIn("Skipped but required", out)

        # In CI the skip is an annotation: it is let through, not hidden.
        status, out, _ = self.run_main(*arguments, env={"GITHUB_ACTIONS": "true"})
        self.assertEqual(status, 0, out)
        warnings = [line for line in out.splitlines() if line.startswith("::warning")]
        self.assertEqual(len(warnings), 2)
        self.assertRegex(
            warnings[0],
            r"^::warning title=Flatpak smoke test: speech-install::SKIPPED, not verified: "
            r"the voice could not be downloaded in 3 attempts \(network: connection refused\)",
        )
        self.assertRegex(
            warnings[1], r"^::warning title=Flatpak smoke test: speech-synthesis::SKIPPED, not"
        )
        self.assertNotIn("::error", out)

    def test_strict_excuses_no_other_skip_and_no_wrong_checksum(self):
        fixture = self.fixture()
        arguments = self.arguments(
            fixture, "--check", "speech-install,speech-synthesis", "--strict"
        )
        voices = ("--voice-cache", str(self.tmp / "voices"))
        # Skipped because nobody asked for the download: required as before.
        self.use_flatpak()
        status, out, _ = self.run_main(*arguments, *voices)
        self.assertEqual(status, 1)
        self.assertEqual(
            out.splitlines()[-1],
            "Skipped but required (--strict): speech-install, speech-synthesis",
        )
        self.assertNotIn("server not reachable", out)
        # A download that is not the pinned file fails, also after a retry.
        self.unreachable_voice("network: timed out", "checksum mismatch: expected a, got b")
        status, out, _ = self.run_main(*arguments, *voices, "--network")
        self.assertEqual(status, 1)
        self.assertRegex(out, r"(?m)^FAIL  speech-install: ")
        self.assertIn("is not the file the catalogue pins: checksum mismatch", out)
        self.assertEqual(
            out.splitlines()[-2:],
            ["Failed: speech-install", "Skipped but required (--strict): speech-synthesis"],
        )

    def speech_run(self, download, *, earlier_voice):
        """A --network --strict run of the two speech checks as CI starts
        it, in which every download attempt ends as `download(home, argv,
        kwargs)` says and the synthesis, if it gets a voice, speaks.
        Returns (exit status, output, the cache directory, the number of
        download attempts)."""
        self.patch(
            mock.patch.dict(
                smoke.VOICE, {"model_sha256": hashlib.sha256(VoiceTestCase.MODEL).hexdigest()}
            )
        )
        cache = self.tmp / "voices"
        shutil.rmtree(cache, ignore_errors=True)
        if earlier_voice:
            for relative, content in VoiceTestCase.voice(lexicon="earlier").items():
                write(cache / "installed" / relative, content)
        attempts = []

        def handler(argv, kwargs):
            if "install" in argv:
                attempts.append(argv)
                return download(bound(argv, "/run/dasher-voices"), argv, kwargs)
            self.assertIn(f"--bind-mount=/run/dasher-voices={cache / 'installed'}", argv)
            write_wav(bound(argv, "/run/dasher-smoke") / "speech.wav")
            return completed(
                json.dumps({"created": True, "synth_status": 0, "synth_seconds": 1.7}) + "\n"
            )

        self.use_sandbox(handler)
        status, out, _ = self.run_main(
            *self.arguments(
                self.fixture(),
                *("--check", "speech-install,speech-synthesis", "--network", "--strict"),
                *("--voice-cache", str(cache)),
            ),
            env={"GITHUB_ACTIONS": "true"},
        )
        return status, out, cache, len(attempts)

    @staticmethod
    def hangs(home, argv, kwargs):
        raise subprocess.TimeoutExpired(argv, kwargs["timeout"])

    @staticmethod
    def partial(home, argv, kwargs):
        """The voice arrives, and of the pronunciation data the manifest."""
        for relative, content in VoiceTestCase.model_only().items():
            write(home / relative, content)
        return completed(
            json.dumps({"installed": True, "seconds": 3.0, "error": None}) + "\n",
            stderr=VoiceTestCase.BUNDLE_WARNING,
        )

    def test_a_download_that_hangs_or_stops_half_way_does_not_fail_a_release(self):
        """Both review findings as CI would see them (--network --strict):
        with no voice from an earlier run the two speech checks are skipped
        with a warning and the run exits 0."""
        for label, download, reason in (
            (
                "hangs",
                self.hangs,
                rf"the download did not finish within {smoke.INSTALL_TIMEOUT} s and was stopped",
            ),
            ("partial", self.partial, r"its pronunciation data is incomplete, missing: en-GB/"),
        ):
            with self.subTest(label):
                self.sleeps.clear()
                status, out, cache, attempts = self.speech_run(download, earlier_voice=False)
                self.assertEqual(status, 0, out)
                self.assertEqual(attempts, smoke.INSTALL_ATTEMPTS)
                self.assertEqual(self.sleeps, list(smoke.INSTALL_BACKOFF))
                self.assertRegex(
                    out,
                    r"(?m)^SKIP  speech-install: the voice could not be downloaded in 3 "
                    r"attempts \(.*" + reason,
                )
                self.assertRegex(out, r"(?m)^SKIP  speech-synthesis: no voice to speak with: ")
                warnings = [line for line in out.splitlines() if line.startswith("::warning")]
                self.assertEqual(len(warnings), 2)
                self.assertRegex(warnings[0], r"speech-install::SKIPPED, not verified: ")
                self.assertRegex(warnings[1], r"speech-synthesis::SKIPPED, not verified: ")
                self.assertNotIn("::error", out)
                self.assertNotIn("FAIL", out)
                self.assertEqual(
                    out.splitlines()[-2:],
                    [
                        "Summary: 0 passed, 0 failed, 2 skipped (of 2)",
                        "Skipped, server not reachable: speech-install, speech-synthesis",
                    ],
                )
                self.assertEqual(list(cache.iterdir()), [])

    def test_a_download_that_fails_leaves_the_voice_of_an_earlier_run_to_speak_with(self):
        for label, download in (("hangs", self.hangs), ("partial", self.partial)):
            with self.subTest(label):
                status, out, cache, attempts = self.speech_run(download, earlier_voice=True)
                self.assertEqual(status, 0, out)
                self.assertEqual(attempts, smoke.INSTALL_ATTEMPTS)
                self.assertRegex(out, r"(?m)^SKIP  speech-install: the voice could not be")
                self.assertRegex(out, r"(?m)^PASS  speech-synthesis: 2\.00 s at 22050 Hz")
                self.assertEqual(
                    out.splitlines()[-2:],
                    [
                        "Summary: 1 passed, 0 failed, 1 skipped (of 2)",
                        "Skipped, server not reachable: speech-install",
                    ],
                )
                # The cache is the earlier voice, whole, and nothing else.
                self.assertEqual([p.name for p in cache.iterdir()], ["installed"])
                self.assertEqual(
                    {
                        p.relative_to(cache / "installed").as_posix(): p.read_bytes()
                        for p in (cache / "installed").rglob("*")
                        if p.is_file()
                    },
                    {k: _bytes(v) for k, v in VoiceTestCase.voice(lexicon="earlier").items()},
                )

    def test_voice_key_names_the_pinned_voice_and_needs_no_build(self):
        status, out, _ = self.run_main("--voice-key")
        self.assertEqual(status, 0)
        key = out.strip()
        self.assertRegex(key, r"^[\w.-]+$")
        self.assertTrue(key.startswith(smoke.VOICE["id"]))
        # Another archive or another model file is another key.
        for field in ("id", "archive_sha256", "model_sha256"):
            with self.subTest(field), mock.patch.dict(smoke.VOICE, {field: "0" * 64}):
                self.assertNotEqual(self.run_main("--voice-key")[1].strip(), key)
        self.assertEqual(self.flatpak_calls, [])

    def test_allow_skip_does_not_excuse_a_failure(self):
        fixture = self.fixture()
        (fixture.files / "libdasher.so").unlink()
        status, _, _ = self.main(fixture, "--check", "files", "--strict", "--allow-skip", "files")
        self.assertEqual(status, 1)

    def test_github_actions_gets_annotations(self):
        fixture = self.fixture(smoke.EXPECTED_FINISH_ARGS + ["--socket=session-bus"])
        (fixture.repo / "CMakeLists.txt").write_text("project(DasherGTK)\n")
        status, out, _ = self.main(
            fixture, "--check", "permissions,release", env={"GITHUB_ACTIONS": "true"}
        )
        self.assertEqual(status, 1)
        lines = out.splitlines()
        errors = [line for line in lines if line.startswith("::error")]
        warnings = [line for line in lines if line.startswith("::warning")]
        self.assertEqual(len(errors), 1)
        self.assertRegex(
            errors[0],
            r"^::error title=Flatpak smoke test: permissions::unexpected permission --socket=session-bus",
        )
        self.assertEqual(len(warnings), 1)
        self.assertRegex(
            warnings[0],
            r"^::warning title=Flatpak smoke test: release::DASHER_GTK_FALLBACK_VERSION not found",
        )
        # The plain lines are still there, the note right after its check.
        at = lines.index(next(line for line in lines if line.startswith("PASS  release:")))
        self.assertTrue(
            lines[at + 1].startswith("        note: DASHER_GTK_FALLBACK_VERSION not found")
        )

    def test_a_check_that_breaks_is_a_failure_not_a_traceback(self):
        fixture = self.fixture()
        entry = check_named("files")

        def broken(suite):
            raise RuntimeError("something unforeseen")

        with mock.patch.dict(entry, {"run": broken}):
            status, out, err = self.main(fixture, "--check", "files,cleanup")
        self.assertEqual(status, 1)
        self.assertEqual(err, "")
        lines = out.splitlines()
        self.assertRegex(lines[0], r"^FAIL  files: ")
        self.assertEqual(
            lines[1], "        - the check itself broke: RuntimeError: something unforeseen"
        )
        self.assertRegex(lines[2], r"^PASS  cleanup: ")

    def test_a_malformed_build_directory_is_a_failure_not_a_traceback(self):
        fixture = self.fixture()
        (fixture.files / "manifest.json").write_text("{ not json")
        fixture.metainfo.write_text("<component><id>unclosed")
        status, out, err = self.main(fixture, "--check", "permissions,metainfo,release")
        self.assertEqual(status, 1)
        self.assertEqual(err, "")
        self.assertEqual(out.count("the check itself broke"), 3)
        self.assertIn("Summary: 0 passed, 3 failed, 0 skipped (of 3)", out)

    def test_run_checks_writes_where_it_is_told(self):
        suite = self.fixture().suite()
        out = io.StringIO()
        results = smoke.run_checks(suite, ["files", "installed-permissions"], out=out)
        self.assertEqual(
            results,
            {
                "passed": ["files"],
                "failed": [],
                "skipped": ["installed-permissions"],
                "unreachable": [],
            },
        )
        self.assertEqual(len(out.getvalue().splitlines()), 2)

    def test_without_a_build_directory_only_two_checks_can_run(self):
        self.assertEqual(smoke.NEEDS_NO_BUILD_DIR, {"lint-manifest", "installed-permissions"})
        status, out, _ = self.run_main("--check", "installed-permissions", "--repo", str(self.tmp))
        self.assertEqual(status, 0)
        self.assertRegex(out, r"^SKIP  installed-permissions: no installed app named")
        suite = smoke.Suite(None, self.tmp, smoke.parse_args(["--list"]))
        out = io.StringIO()
        results = smoke.run_checks(suite, ["files"], out=out)
        self.assertEqual(results["skipped"], ["files"])
        self.assertEqual(out.getvalue(), "SKIP  files: no build directory given\n")

    def test_the_work_directory_is_removed_after_a_run(self):
        self.linter({"errors": ["appid-url-not-reachable"]}, status=1)
        record = self.tmp / "lint.cwd"
        status, out, _ = self.main(
            self.fixture().with_manifest(), "--check", "lint-manifest", "--host-checks"
        )
        self.assertEqual(status, 0, out)
        work = Path(record.read_text().strip())
        self.assertTrue(work.name.startswith("dasher-smoke-"))
        self.assertFalse(work.exists())

    def test_keep_logs_copies_what_the_helpers_wrote(self):
        fixture = self.fixture()
        keep = self.tmp / "kept" / "logs"
        suite = fixture.suite("--keep-logs", str(keep))
        work = suite.workdir()
        self.assertIs(suite.workdir(), work)
        for name in ("app.log", "headless.json", "speech.wav", "scratch.tmp"):
            write(work / name, name)
        suite.cleanup()
        self.assertFalse(work.exists())
        self.assertEqual(
            sorted(p.name for p in keep.iterdir()), ["app.log", "headless.json", "speech.wav"]
        )
        # Without a work directory there is nothing to clean up or to keep.
        other = fixture.suite("--keep-logs", str(self.tmp / "unused"))
        other.cleanup()
        self.assertFalse((self.tmp / "unused").exists())

    def test_the_script_runs_as_a_program(self):
        fixture = self.fixture()
        done = self.program(fixture, "--check", "files,cleanup")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("Summary: 2 passed, 0 failed, 0 skipped (of 2)", done.stdout)
        (fixture.files / "libdasher.so").unlink()
        done = self.program(fixture, "--check", "files")
        self.assertEqual(done.returncode, 1)
        self.assertIn("FAIL  files:", done.stdout)


# ══ The helpers under sandbox/ ════════════════════════════════════════════


class SandboxHelperTests(SmokeTestCase):
    def test_the_python_helpers_compile(self):
        helpers = sorted(smoke.SANDBOX_HELPERS.glob("*.py"))
        self.assertLessEqual({"alsa.py", "headless.py", "speech.py"}, {p.name for p in helpers})
        for helper in helpers:
            with self.subTest(helper.name):
                # Compiled into the temporary directory, not next to the source.
                py_compile.compile(
                    str(helper), cfile=str(self.tmp / f"{helper.name}c"), doraise=True
                )

    # ── headless.py: whatever happens, no process is left behind ─────────
    class Child:
        """A process as headless.py sees it: running until killed, unless
        it has `exited` already."""

        def __init__(self, log, name, exited=None, stdout=""):
            self.log, self.name, self.returncode = log, name, exited
            self.stdout = io.StringIO(stdout)

        def poll(self):
            return self.returncode

        def kill(self):
            self.log.append(f"kill {self.name}")
            self.returncode = -9

        def wait(self, timeout=None):
            self.log.append(f"wait {self.name}")
            return self.returncode

    def test_stop_children_kills_what_runs_last_started_first(self):
        log = []
        children = [
            self.Child(log, "bus"),
            self.Child(log, "broadway", exited=1),
            self.Child(log, "app"),
        ]
        headless.stop_children(children)
        self.assertEqual(log, ["kill app", "wait app", "kill bus", "wait bus"])
        self.assertEqual([child.poll() for child in children], [-9, 1, -9])
        headless.stop_children(children)  # nothing runs any more
        self.assertEqual(len(log), 4)
        headless.stop_children([])

    def test_stop_children_does_not_wait_for_ever(self):
        class Stubborn(self.Child):
            def wait(self, timeout=None):
                self.log.append(f"wait {self.name} {timeout}")
                raise subprocess.TimeoutExpired(self.name, timeout)

        log = []
        headless.stop_children([Stubborn(log, "bus"), Stubborn(log, "app")])
        self.assertEqual(log, ["kill app", "wait app 10", "kill bus", "wait bus 10"])

    def run_headless(self, popen, run=None):
        """main() of headless.py with the processes it starts replaced.
        Returns (what was written as the result, the kill/wait log)."""
        log = []
        result_path = self.tmp / "headless.json"
        logs = self.tmp / "logs"
        logs.mkdir(exist_ok=True)

        def start(argv, **kwargs):
            for stream in (kwargs.get("stdout"), kwargs.get("stderr")):
                if hasattr(stream, "close"):
                    stream.close()
            self.environments[argv[0]] = dict(kwargs["env"])
            return popen(log, argv)

        self.environments = {}
        with (
            mock.patch.object(headless, "HOME", str(self.tmp / "home")),
            mock.patch.object(headless.subprocess, "Popen", side_effect=start),
            mock.patch.object(
                headless.subprocess, "run", side_effect=run or (lambda *a, **k: completed())
            ),
            mock.patch.object(headless.time, "sleep"),
            mock.patch.dict(
                os.environ,
                {
                    "XDG_RUNTIME_DIR": str(self.tmp / "run"),
                    "DISPLAY": ":0",
                    "WAYLAND_DISPLAY": "wayland-0",
                    "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/user/1000/bus",
                    "AT_SPI_BUS_ADDRESS": "unix:path=/run/user/1000/at-spi/bus",
                },
            ),
        ):
            outcome = None
            try:
                headless.main([str(result_path), str(logs), "dasher", "org.dasher.gtk", "0"])
            except BaseException as e:  # what main() lets through
                outcome = e
        return json.loads(result_path.read_text()), log, outcome

    def processes(self, **exited):
        """A Popen for run_headless(): the bus prints its address, the
        display leaves its socket, and whichever of bus, broadway and app
        is named in `exited` has ended with that status already."""
        self.started = []

        def popen(log, argv):
            self.started.append(argv[0])
            name = {"dbus-daemon": "bus", "gtk4-broadwayd": "broadway"}.get(argv[0], "app")
            if name == "broadway":
                write(self.tmp / "run/broadway8.socket")
            address = "unix:path=/tmp/dbus-test\n" if name == "bus" else ""
            return self.Child(log, name, exited=exited.get(name), stdout=address)

        return popen

    def test_a_bus_that_does_not_come_up_is_reported_and_stopped(self):
        result, log, _ = self.run_headless(lambda log, argv: self.Child(log, argv[0]))
        self.assertEqual(result, {"stage": "bus", "error": "dbus-daemon printed no address"})
        self.assertEqual(log, ["kill dbus-daemon", "wait dbus-daemon"])

    def test_a_display_that_exits_is_reported_and_the_bus_stopped(self):
        result, log, _ = self.run_headless(self.processes(broadway=1))
        self.assertEqual(
            result, {"stage": "display", "error": "gtk4-broadwayd exited with status 1"}
        )
        self.assertEqual(log, ["kill bus", "wait bus"])

    def test_every_child_is_stopped_when_the_helper_breaks_half_way(self):
        for label, error, outcome_type, recorded in (
            ("an error", OSError("no gdbus"), type(None), "OSError: no gdbus"),
            ("an interrupt", KeyboardInterrupt(), KeyboardInterrupt, None),
        ):
            with self.subTest(label):

                def run(argv, **kwargs):
                    if argv[0] == "gdbus":
                        raise error
                    return completed("Usage:\n  dasher [OPTION...]\n")

                result, log, outcome = self.run_headless(self.processes(), run)
                self.assertEqual(self.started, ["dbus-daemon", "gtk4-broadwayd", "dasher"])
                self.assertIsInstance(outcome, outcome_type)
                self.assertEqual(
                    log,
                    [
                        f"{verb} {name}"
                        for name in ("app", "broadway", "bus")
                        for verb in ("kill", "wait")
                    ],
                )
                # The result is written all the same, and says how far it got.
                self.assertEqual(result["stage"], "start")
                self.assertEqual(result.get("error"), recorded)
                self.assertEqual(result["help"]["status"], 0)

    def test_the_app_is_given_the_private_bus_display_and_home_only(self):
        result, _, _ = self.run_headless(self.processes(app=0))
        self.assertEqual(result["stage"], "done")
        home = str(self.tmp / "home")
        for program in ("dbus-daemon", "gtk4-broadwayd", "dasher"):
            with self.subTest(program):
                env = self.environments[program]
                for variable in ("DISPLAY", "WAYLAND_DISPLAY", "AT_SPI_BUS_ADDRESS"):
                    self.assertNotIn(variable, env)
                self.assertNotIn("/run/user/1000/bus", env.get("DBUS_SESSION_BUS_ADDRESS", ""))
                self.assertEqual(env["HOME"], home)
                self.assertEqual(env["XDG_DATA_HOME"], f"{home}/data")
        app = self.environments["dasher"]
        self.assertEqual(app["DBUS_SESSION_BUS_ADDRESS"], "unix:path=/tmp/dbus-test")
        self.assertEqual((app["GDK_BACKEND"], app["BROADWAY_DISPLAY"]), ("broadway", ":7"))
        self.assertNotIn("DBUS_SESSION_BUS_ADDRESS", self.environments["dbus-daemon"])

    def test_exported_actions_reads_what_gdbus_prints(self):
        for stdout, expected in (
            ("(@as [],)\n", []),
            ("(['quit'],)\n", ["quit"]),
            ("(['quit', 'about', 'app.new-window'],)\n", ["about", "app.new-window", "quit"]),
            ("", None),
        ):
            with self.subTest(stdout), mock.patch.object(
                headless.subprocess, "run", return_value=completed(stdout)
            ) as run:
                self.assertEqual(headless.exported_actions("org.dasher.gtk"), expected)
                self.assertEqual(
                    run.call_args[0][0][3:],
                    [
                        *("--dest", "org.dasher.gtk", "--object-path", "/org/dasher/gtk"),
                        *("--method", "org.gtk.Actions.List"),
                    ],
                )

    def spawn(self, *arguments):
        return REAL_RUN(
            ["/bin/sh", str(smoke.SANDBOX_HELPERS / "flatpak-spawn"), *arguments],
            capture_output=True,
            text=True,
        )

    def test_the_flatpak_spawn_stand_in_runs_the_command_directly(self):
        done = self.spawn(
            "--sandbox", "--watch-bus", "--directory=/", "--forward-fd=5", "echo", "hi"
        )
        self.assertEqual((done.returncode, done.stdout, done.stderr), (0, "hi\n", ""))

    def test_the_stand_in_keeps_the_command_s_own_options_and_status(self):
        done = self.spawn("--sandbox", "sh", "-c", 'echo "$1 $2"; exit 7', "sh", "--dbus-fd", "19")
        self.assertEqual((done.returncode, done.stdout), (7, "--dbus-fd 19\n"))
        done = self.spawn("printf", "%s", "--not-an-option-of-ours")
        self.assertEqual(done.stdout, "--not-an-option-of-ours")

    def test_the_stand_in_is_executable_and_a_sh_script(self):
        path = smoke.SANDBOX_HELPERS / "flatpak-spawn"
        self.assertTrue(os.access(path, os.X_OK))
        self.assertTrue(path.read_text().startswith("#!/bin/sh\n"))


# ══ mutation-test.py ══════════════════════════════════════════════════════


class MutationScriptTests(SmokeTestCase):
    def test_every_mutation_names_checks_that_exist(self):
        known = {entry["name"] for entry in smoke.CHECKS}
        names = [name for name, _, _, _ in mutation.MUTATIONS]
        self.assertEqual(len(names), len(set(names)))
        for name, summary, mutate, checks in mutation.MUTATIONS:
            with self.subTest(name):
                self.assertTrue(summary.strip())
                self.assertTrue(callable(mutate))
                self.assertTrue(checks)
                self.assertLessEqual(set(checks), known)

    def test_the_mutations_cover_at_least_eight_distinct_checks(self):
        covered = {check for _, _, _, checks in mutation.MUTATIONS for check in checks}
        self.assertGreaterEqual(len(covered), 8, sorted(covered))

    def main(self, *arguments, run=None):
        """(exit status, stdout, stderr) of mutation-test.py's main(), with
        `run` in place of its subprocess.run when one is given."""
        out, err = io.StringIO(), io.StringIO()
        with (
            mock.patch.object(mutation.subprocess, "run", side_effect=run or REAL_RUN),
            mock.patch.object(sys, "argv", ["mutation-test.py", *arguments]),
            contextlib.redirect_stdout(out),
            contextlib.redirect_stderr(err),
        ):
            try:
                status = mutation.main()
            except SystemExit as e:
                status = e.code
        return status, out.getvalue(), err.getvalue()

    def test_list_prints_every_mutation(self):
        status, out, _ = self.main("--list")
        self.assertEqual(status, 0)
        lines = out.splitlines()
        self.assertEqual([line.split()[0] for line in lines], [m[0] for m in mutation.MUTATIONS])
        self.assertTrue(all(" -> " in line for line in lines))

    def test_usage_errors_exit_2(self):
        for arguments, pattern in (
            ([], "BUILD_DIR is required"),
            ([str(self.tmp), "--only", "nonsense"], r"unknown mutation\(s\): nonsense"),
        ):
            with self.subTest(arguments):
                status, out, err = self.main(*arguments)
                self.assertEqual((status, out), (2, ""))
                self.assertRegex(err, pattern)

    def tree(self):
        original, build = self.tmp / "original", self.tmp / "copy"
        write(original / "metadata", "sockets=wayland;\ndevices=all;\n")
        write(original / "files/dasher", b"\0$ORIGIN\0rest")
        write(original / "files/share/a/licence.txt", "text")
        os.symlink("dasher", original / "files/link")
        shutil.copytree(original, build, symlinks=True)
        self.patch(mock.patch.object(mutation, "ORIGINAL", original))
        return original, build

    def snapshot(self, root):
        return {
            p.relative_to(root).as_posix(): (
                os.readlink(p) if p.is_symlink() else p.is_dir() or p.read_bytes()
            )
            for p in sorted(root.rglob("*"))
        }

    def test_the_helpers_change_the_copy_and_restore_puts_it_back(self):
        original, build = self.tree()
        before = self.snapshot(original)
        self.assertEqual(self.snapshot(build), before)
        for label, change, check in (
            (
                "edit",
                lambda: mutation.edit(build, "metadata", r"(?m)^sockets=", "sockets=session-bus;"),
                lambda: self.assertIn(
                    "sockets=session-bus;wayland;", (build / "metadata").read_text()
                ),
            ),
            (
                "patch",
                lambda: mutation.patch(build, "files/dasher", b"\0$ORIGIN\0", b"\0/tmp/xy\0"),
                lambda: self.assertEqual((build / "files/dasher").read_bytes(), b"\0/tmp/xy\0rest"),
            ),
            (
                "remove a file",
                lambda: mutation.remove(build, "files/share/a/licence.txt"),
                lambda: self.assertFalse((build / "files/share/a/licence.txt").exists()),
            ),
            (
                "remove a directory",
                lambda: mutation.remove(build, "files/share"),
                lambda: self.assertFalse((build / "files/share").exists()),
            ),
            (
                "create a file",
                lambda: mutation.create(build, "files/share/a/stray.a"),
                lambda: self.assertTrue((build / "files/share/a/stray.a").is_file()),
            ),
            (
                "create in new directories",
                lambda: mutation.create(build, "files/include/x/stray.h", b"h"),
                lambda: self.assertEqual((build / "files/include/x/stray.h").read_bytes(), b"h"),
            ),
            (
                "replace a file",
                lambda: mutation.create(build, "files/dasher", b"broken"),
                lambda: self.assertEqual((build / "files/dasher").read_bytes(), b"broken"),
            ),
        ):
            with self.subTest(label):
                touched = change()
                check()
                self.assertNotEqual(self.snapshot(build), before)
                mutation.restore(build, touched)
                self.assertEqual(self.snapshot(build), before)
        self.assertEqual(self.snapshot(original), before, "the original was changed")

    def test_create_reports_the_topmost_new_directory(self):
        _, build = self.tree()
        self.assertEqual(mutation.create(build, "files/include/x/stray.h"), ["files/include"])
        self.assertEqual(mutation.create(build, "files/share/a/stray.a"), ["files/share/a/stray.a"])

    def test_a_mutation_that_does_not_apply_stops_the_run(self):
        _, build = self.tree()
        with self.assertRaisesRegex(
            SystemExit, "mutation does not apply: .* not found in metadata"
        ):
            mutation.edit(build, "metadata", r"filesystems=", "x")
        with self.assertRaisesRegex(SystemExit, "mutation does not apply"):
            mutation.patch(build, "files/dasher", b"absent!", b"present")
        with self.assertRaisesRegex(SystemExit, "same length"):
            mutation.patch(build, "files/dasher", b"rest", b"longer")

    def test_each_mutation_is_caught_on_the_made_up_build(self):
        """The mutations of mutation-test.py, as far as checks without a
        sandbox guard against them, applied to the fixture: the same proof
        the script gives on a real build, here on every pull request."""
        applied = 0
        for name, _, mutate, checks in mutation.MUTATIONS:
            static = [check for check in checks if check in STATIC_CHECKS]
            if not static:
                continue
            with self.subTest(name):
                fixture = self.fixture()
                with mock.patch.object(mutation, "ORIGINAL", fixture.build):
                    mutate(fixture.build)
                suite = fixture.suite()
                for check in static:
                    status, detail = self.outcome(suite, check)
                    self.assertEqual(status, "FAIL", f"{check} did not catch {name}: {detail}")
                applied += 1
        self.assertGreaterEqual(applied, 15)

    def run_mutations(self, *arguments):
        """(exit status, stdout, stderr) of mutation-test.py's main(). The
        smoke test it starts is the real script in a process of its own, so
        it is only ever given checks that need no sandbox."""
        started = []

        def checked(cmd, *args, **kwargs):
            if isinstance(cmd, list) and str(mutation.SMOKE_TEST) in cmd and "--list" not in cmd:
                self.assertNotIn("--host-checks", cmd)
                names = cmd[cmd.index("--check") + 1].split(",")
                self.assertTrue(names and set(names) <= set(STATIC_CHECKS), cmd)
                started.append(names)
            return REAL_RUN(cmd, *args, **kwargs)

        return (*self.main(*arguments, run=checked), started)

    def mutation_arguments(self, fixture, only):
        scratch = self.tmp / "scratch"
        scratch.mkdir(exist_ok=True)
        build, repo = str(fixture.build), str(fixture.repo)
        return [build, "--only", only, "--scratch", str(scratch), "--", "--repo", repo]

    def test_a_run_copies_the_build_catches_the_mutations_and_cleans_up(self):
        fixture = self.fixture()
        before = self.snapshot(fixture.build)
        status, out, err, started = self.run_mutations(
            *self.mutation_arguments(fixture, "session-bus,stray-archive,screenshot-on-branch")
        )
        self.assertEqual((status, err), (0, ""), out)
        self.assertIn("baseline: 3 checks pass on the unmodified copy", out)
        for name, check in (
            ("session-bus", "permissions"),
            ("stray-archive", "cleanup"),
            ("screenshot-on-branch", "screenshot"),
        ):
            self.assertRegex(out, rf"(?m)^CAUGHT  {name}: .*\n        {check}: FAIL  - \S")
        self.assertEqual(out.splitlines()[-1], "3 of 3 mutations caught")
        # One run for the baseline, one per mutation, each with its own checks.
        self.assertEqual(
            started,
            [
                ["cleanup", "permissions", "screenshot"],
                ["permissions"],
                ["cleanup"],
                ["screenshot"],
            ],
        )
        self.assertEqual(self.snapshot(fixture.build), before, "the build directory was changed")
        self.assertEqual(list((self.tmp / "scratch").iterdir()), [], "the copy was left behind")

    def test_a_mutation_that_no_check_notices_is_missed(self):
        fixture = self.fixture()
        harmless = ("harmless", "nothing is changed", lambda build: [], ["cleanup", "files"])
        with mock.patch.object(mutation, "MUTATIONS", [harmless]):
            status, out, _, _ = self.run_mutations(*self.mutation_arguments(fixture, "harmless"))
        self.assertEqual(status, 1)
        self.assertRegex(out, r"(?m)^MISSED  harmless: nothing is changed\n        cleanup: PASS")
        self.assertEqual(out.splitlines()[-2:], ["0 of 1 mutations caught", "Missed: harmless"])

    def test_a_mutation_caught_by_only_one_of_its_checks_is_missed(self):
        fixture = self.fixture()
        half = (
            "half",
            "a static archive, said to break the files check too",
            lambda build: mutation.create(build, "files/lib/libstray.a"),
            ["cleanup", "files"],
        )
        with mock.patch.object(mutation, "MUTATIONS", [half]):
            status, out, _, _ = self.run_mutations(*self.mutation_arguments(fixture, "half"))
        self.assertEqual(status, 1)
        self.assertRegex(
            out, r"(?m)^MISSED  half: .*\n        cleanup: FAIL .*\n        files: PASS"
        )

    def test_a_build_that_does_not_pass_unmodified_proves_nothing(self):
        fixture = self.fixture()
        write(fixture.files / "lib/libalready-there.a")
        status, out, _, started = self.run_mutations(
            *self.mutation_arguments(fixture, "stray-archive")
        )
        self.assertEqual(status, 1)
        self.assertIn("BASELINE: cleanup do not pass on the unmodified copy", out)
        self.assertNotIn("CAUGHT", out)
        self.assertEqual(started, [["cleanup"]])
        self.assertEqual(list((self.tmp / "scratch").iterdir()), [])

    def test_an_empty_selection_is_refused(self):
        # smoke-test.py would take the empty list of checks as "all of them".
        fixture = self.fixture()
        for arguments, pattern in (
            (["--host-checks", "--no-lint"], r"no mutation left to run\n"),
            ([], r"no mutation left to run \(lint-new-error need --host-checks\)"),
        ):
            with self.subTest(arguments):
                status, out, err, started = self.run_mutations(
                    str(fixture.build), "--only", "lint-new-error", *arguments
                )
                self.assertEqual((status, out, started), (2, "", []))
                self.assertRegex(err, pattern)

    def test_the_host_checks_are_asked_of_the_smoke_test(self):
        self.assertEqual(mutation.host_checks(), set(HOST_CHECKS))

    def test_without_host_checks_no_mutation_expects_one_to_fail(self):
        host = set(HOST_CHECKS)
        kept, left_out = mutation.without_host_checks(mutation.MUTATIONS, host)
        self.assertEqual(left_out, ["lint-new-error"])
        self.assertEqual(
            [m[0] for m in kept], [m[0] for m in mutation.MUTATIONS if m[0] != "lint-new-error"]
        )
        expected = {name: checks for name, _, _, checks in kept}
        self.assertEqual(expected["no-devices"], ["permissions"])
        self.assertEqual(expected["no-sound-socket"], ["permissions"])
        self.assertEqual(expected["launcher-broken"], ["files", "help", "start", "bus-name"])
        for name, summary, mutate, checks in kept:
            self.assertTrue(checks, name)
            self.assertFalse(set(checks) & host, name)
        # The original list still names them, for a run with --host-checks.
        self.assertTrue(any(set(m[3]) & host for m in mutation.MUTATIONS))
        self.assertEqual(mutation.without_host_checks(mutation.MUTATIONS, set())[1], [])

    def test_a_run_without_host_checks_says_what_it_left_out(self):
        fixture = self.fixture()
        status, out, err, started = self.run_mutations(
            *self.mutation_arguments(fixture, "no-devices,lint-new-error,stray-archive")
        )
        self.assertEqual((status, err), (0, ""), out)
        self.assertIn("left out, only a check behind --host-checks notices: lint-new-error\n", out)
        self.assertEqual(started, [["cleanup", "permissions"], ["permissions"], ["cleanup"]])
        self.assertEqual(out.splitlines()[-1], "2 of 2 mutations caught")

    def test_host_checks_is_passed_on_to_the_smoke_test(self):
        commands = []

        def fake(cmd, *args, **kwargs):
            commands.append(cmd)
            if "--list" in cmd:
                return REAL_RUN(cmd, *args, **kwargs)
            names = cmd[cmd.index("--check") + 1].split(",")
            # The baseline passes; then every check fails.
            status = "PASS" if len(commands) == 1 else "FAIL"
            return completed("".join(f"{status}  {name}: x\n        - why\n" for name in names))

        fixture = self.fixture()
        arguments = [*self.mutation_arguments(fixture, "no-devices,lint-new-error"), "--no-such"]
        status, out, _ = self.main("--host-checks", *arguments, run=fake)
        self.assertEqual(status, 0)
        self.assertEqual(len(commands), 3)  # --list is not needed
        for cmd, checks in zip(
            commands,
            (
                "input-devices,lint-builddir,permissions",
                "permissions,input-devices",
                "lint-builddir",
            ),
        ):
            self.assertEqual(cmd[3:5], ["--check", checks])
            self.assertEqual(cmd[5:], ["--host-checks", "--repo", str(fixture.repo), "--no-such"])
        self.assertNotIn("left out", out)
        self.assertEqual(out.splitlines()[-1], "2 of 2 mutations caught")

    def test_run_smoke_reads_the_status_lines(self):
        output = (
            "PASS  files: 14 files, 10 alphabets\n"
            "FAIL  bus-name: the app owns org.dasher.gtk and the metadata lets it\n"
            "        - a problem\n"
            "SKIP  speech-install: downloads a voice\n"
            "\nSummary: 1 passed, 1 failed, 1 skipped (of 3)\n"
        )
        with mock.patch.object(mutation.subprocess, "run", return_value=completed(output)) as run:
            status, text = mutation.run_smoke(
                Path("/build"), ["files", "bus-name"], ["--voice-cache", "/v"]
            )
        self.assertEqual(status, {"files": "PASS", "bus-name": "FAIL", "speech-install": "SKIP"})
        self.assertEqual(text, output)
        self.assertEqual(
            run.call_args[0][0],
            [
                sys.executable,
                str(mutation.SMOKE_TEST),
                "/build",
                "--check",
                "files,bus-name",
                "--voice-cache",
                "/v",
            ],
        )
        self.assertEqual(mutation.SMOKE_TEST, TESTS_DIR / "smoke-test.py")


if __name__ == "__main__":
    unittest.main()
