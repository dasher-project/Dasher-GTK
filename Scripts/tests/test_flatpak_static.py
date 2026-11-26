#!/usr/bin/env python3
"""Static checks of the Flatpak packaging: no build, no network.

The Flatpak is only built on pushes to main, so a pull request that breaks
the manifest in a way a reader can see would otherwise be found out after
the merge. These tests read the files of the repository as they are:

  - the manifest: every source pinned (sha256 or commit, https, no
    `branch:`), no network during the build, exactly the permissions that
    packaging/flatpak/tests/smoke-test.py expects of the finished build
    (one list, EXPECTED_FINISH_ARGS, is the source of truth for both), the
    from-source speech modules in place, and the install steps that the
    smoke test later looks for the results of;
  - the metainfo and the desktop file: what Flathub requires, a screenshot
    that is pinned and matches the image in the repository, a newest
    release that is the version in CMakeLists.txt;
  - README.md and CONTRIBUTING.md: the repository paths they name exist;
  - the workflows: the unit tests run on every push and pull request, and
    the smoke test runs on both Flatpak legs after the build.

Each manifest rule is a small function, and ManifestRuleTests breaks a
copy of the real manifest once per rule to show that the rule notices.

The manifest is read with PyYAML (python3-yaml); without it the manifest
tests are skipped and the others still run. Stdlib otherwise.

    python3 -m unittest discover -s Scripts/tests -v
"""

import contextlib
import copy
import importlib.util
import io
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

try:
    import yaml
except ImportError:  # the manifest tests say so when they are skipped
    yaml = None

REPO = Path(__file__).resolve().parent.parent.parent
PACKAGING = REPO / "packaging"
MANIFEST_DIR = PACKAGING / "flatpak"


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


# The smoke test owns the expectations that both layers share.
smoke = load_script("flatpak_smoke_test_for_static", MANIFEST_DIR / "tests" / "smoke-test.py")

APP_ID = smoke.APP_ID
MANIFEST = MANIFEST_DIR / f"{APP_ID}.yaml"
METAINFO = PACKAGING / f"{APP_ID}.metainfo.xml"
DESKTOP = PACKAGING / f"{APP_ID}.desktop"

NEEDS_YAML = "PyYAML is not installed (python3-yaml): the manifest cannot be read"

# ── Manifest rules ──────────────────────────────────────────────────────
# Each takes the parsed manifest and returns a list of problems.

SOURCE_TYPES = {"archive", "file", "git", "dir", "shell", "script", "inline", "patch"}
SHA256 = re.compile(r"[0-9a-f]{64}")
COMMIT = re.compile(r"[0-9a-f]{40}")
# What sherpa-onnx-sys downloads when it is left alone: k2-fsa's prebuilt
# static libraries and the prebuilt ONNX Runtime they were linked against.
PREBUILT_SPEECH_URL = re.compile(r"sherpa-onnx/releases/download|onnxruntime-libs/releases")
# Modules that build what a later module links, in this order.
SPEECH_MODULES = ["onnxruntime", "sherpa-onnx"]
APP_MODULE = "dasher"
# Directories smoke-test.py looks for that the manifest's cleanup does not
# name. None: lib64/pkgconfig was one until the x86_64 runner showed that
# the mm libraries install their .pc files there.
NOT_IN_MANIFEST_CLEANUP = set()


def load_included(name, base=MANIFEST_DIR):
    """A file the manifest includes by name (a list of sources, a module)."""
    text = (base / name).read_text()
    return json.loads(text) if name.endswith(".json") else yaml.safe_load(text)


def modules_of(manifest, base=MANIFEST_DIR):
    """Every module, nested ones and included files too, in build order
    (a nested module is built before the module that contains it)."""
    found = []

    def walk(entries):
        for entry in entries or []:
            module = load_included(entry, base) if isinstance(entry, str) else entry
            walk(module.get("modules"))
            found.append(module)

    walk(manifest.get("modules"))
    return found


def sources_of(module, base=MANIFEST_DIR):
    """The sources of a module as dicts; a file named in place of a source
    (cargo-sources.json) is read and its entries are returned."""
    found = []
    for entry in module.get("sources") or []:
        if isinstance(entry, str):
            included = load_included(entry, base)
            found.extend(included if isinstance(included, list) else [included])
        else:
            found.append(entry)
    return found


def all_sources(manifest, base=MANIFEST_DIR):
    """(module name, source) for every source of the manifest."""
    return [
        (module.get("name", "?"), source)
        for module in modules_of(manifest, base)
        for source in sources_of(module, base)
    ]


def module_named(manifest, name):
    for module in modules_of(manifest):
        if module.get("name") == name:
            return module
    raise KeyError(name)


def source_problems(manifest, base=MANIFEST_DIR):
    """Every source is pinned: an immutable thing, fetched over https or
    read from the repository. Flathub requires it, and the pin-sync check
    builds on it."""
    problems = []
    for module, source in all_sources(manifest, base):
        kind = source.get("type")
        label = f"module {module}: {kind} source {source.get('url') or source.get('path') or ''}"
        if kind not in SOURCE_TYPES:
            problems.append(f"{label}: unknown source type {kind!r}")
            continue
        urls = [source["url"]] if "url" in source else []
        urls += source.get("mirror-urls", [])
        for url in urls:
            if not url.startswith("https://"):
                problems.append(f"{label}: {url} is not an https URL")
        if kind in ("archive", "file"):
            if "url" in source:
                if not SHA256.fullmatch(str(source.get("sha256", ""))):
                    problems.append(f"{label}: no sha256 (64 hex digits) for a downloaded file")
            elif "path" not in source:
                problems.append(f"{label}: neither url nor path")
        if kind == "git":
            if "branch" in source:
                problems.append(
                    f"{label}: `branch: {source['branch']}` moves; pin a commit (Flathub does "
                    "not accept a branch)"
                )
            if not COMMIT.fullmatch(str(source.get("commit", ""))):
                problems.append(
                    f"{label}: no commit (40 hex digits)"
                    + (f"; tag {source['tag']} alone can be moved" if "tag" in source else "")
                )
            if "url" not in source:
                problems.append(f"{label}: no url")
        if kind in ("file", "patch", "dir", "archive"):
            paths = ([source["path"]] if "path" in source else []) + source.get("paths", [])
            for path in paths:
                target = base / path
                if not (target.is_dir() if kind == "dir" else target.is_file()):
                    problems.append(f"{label}: {path} does not exist next to the manifest")
    return problems


def dir_source_problems(manifest, base=MANIFEST_DIR):
    """The working tree is built by exactly one module, the app itself."""
    dirs = [module for module, source in all_sources(manifest, base) if source.get("type") == "dir"]
    if dirs != [APP_MODULE]:
        return [
            f"`type: dir` sources in {dirs or 'no module'}: expected exactly one, in the "
            f"{APP_MODULE} module"
        ]
    return []


def finish_args_problems(manifest):
    """finish-args is the list the smoke test expects of the built app."""
    args = manifest.get("finish-args", [])
    problems = smoke.compare_permissions(set(args))
    problems += [
        f"finish-arg {arg} is listed twice" for arg in sorted(set(args)) if args.count(arg) > 1
    ]
    return problems


def build_network_problems(manifest):
    """The build has no network: nothing shares it, and cargo is offline."""
    problems = [
        f"{where} has build-args --share=network"
        for where in smoke.build_args_with_network(manifest)
    ]
    env = module_named(manifest, APP_MODULE).get("build-options", {}).get("env", {})
    if env.get("CARGO_NET_OFFLINE") != "true":
        problems.append(
            f"the {APP_MODULE} module does not set CARGO_NET_OFFLINE to 'true' "
            f"(it is {env.get('CARGO_NET_OFFLINE')!r})"
        )
    return problems


def module_problems(manifest):
    """Module names are unique, and the speech libraries are built before
    the app that links them."""
    names = [module.get("name") for module in modules_of(manifest)]
    problems = [
        f"module name {name} is used twice" for name in sorted(set(names)) if names.count(name) > 1
    ]
    for name in SPEECH_MODULES + [APP_MODULE]:
        if name not in names:
            problems.append(f"no module named {name}")
    if not problems:
        order = [names.index(name) for name in SPEECH_MODULES + [APP_MODULE]]
        if order != sorted(order):
            problems.append(
                f"the modules {', '.join(SPEECH_MODULES)} and {APP_MODULE} are not built in "
                "that order"
            )
    return problems


def speech_from_source_problems(manifest, base=MANIFEST_DIR):
    """sherpa-onnx-sys links what the manifest builds, never a download."""
    problems = []
    env = module_named(manifest, APP_MODULE).get("build-options", {}).get("env", {})
    if env.get("SHERPA_ONNX_LIB_DIR") != "/app/sherpa-onnx/lib":
        problems.append(
            f"the {APP_MODULE} module does not point SHERPA_ONNX_LIB_DIR at /app/sherpa-onnx/lib "
            f"(it is {env.get('SHERPA_ONNX_LIB_DIR')!r})"
        )
    if "SHERPA_ONNX_ARCHIVE_DIR" in json.dumps(manifest):
        problems.append("SHERPA_ONNX_ARCHIVE_DIR is set: that is the prebuilt-archive route")
    for module, source in all_sources(manifest, base):
        for url in [source.get("url", "")] + source.get("mirror-urls", []):
            if PREBUILT_SPEECH_URL.search(url):
                problems.append(f"module {module}: {url} is a prebuilt speech library")
    return problems


def cleanup_problems(manifest):
    """What the smoke test expects to be gone is what cleanup removes."""
    cleanup = manifest.get("cleanup", [])
    problems = []
    for path in smoke.CLEANED_PATHS:
        if path not in NOT_IN_MANIFEST_CLEANUP and f"/{path}" not in cleanup:
            problems.append(f"cleanup does not remove /{path}")
    for suffix in smoke.CLEANED_SUFFIXES:
        if f"*{suffix}" not in cleanup:
            problems.append(f"cleanup does not remove *{suffix}")
    return problems


# Below /app: what the app module's post-install commands put there (the
# rest of smoke-test.py's EXPECTED_FILES comes from `cmake --install` and
# from the speech modules).
INSTALLED_BY_POST_INSTALL = ("share/applications/", "share/metainfo/", "share/icons/")


def commands_of(module, key):
    return "\n".join(module.get(key, []))


def install_problems(manifest):
    """The app module installs what the smoke test looks for: the launcher
    and the desktop file, metainfo and icons under their Flatpak names."""
    post_install = commands_of(module_named(manifest, APP_MODULE), "post-install")
    problems = []
    if f'LD_LIBRARY_PATH="{smoke.LAUNCHER_LIBRARY_PATH}' not in post_install:
        problems.append(
            f"the launcher does not put {smoke.LAUNCHER_LIBRARY_PATH} on LD_LIBRARY_PATH"
        )
    if 'exec /app/dasher "$@"' not in post_install:
        problems.append('the launcher does not `exec /app/dasher "$@"`')
    for relative, kind in smoke.EXPECTED_FILES:
        if kind == "launcher" or relative.startswith(INSTALLED_BY_POST_INSTALL):
            # The whole path: /app/bin/dasher-gtk does not create /app/bin/dasher.
            if not re.search(rf"{re.escape('/app/' + relative)}(?![\w.-])", post_install):
                problems.append(f"post-install does not create /app/{relative}")
    return problems


def onnxruntime_problems(manifest):
    """The onnxruntime module states the version it builds (pin-sync and
    the smoke test read it) and installs its licence texts."""
    module = module_named(manifest, "onnxruntime")
    problems = []
    if not re.search(
        r'VERSION_NUMBER\)" = [0-9]+(\.[0-9]+)+', commands_of(module, "build-commands")
    ):
        problems.append('no `test "$(cat VERSION_NUMBER)" = <version>` build command')
    post_install = commands_of(module, "post-install")
    for name in ("LICENSE", "ThirdPartyNotices.txt"):
        if name not in post_install:
            problems.append(f"post-install does not install {name}")
    sherpa = commands_of(module_named(manifest, "sherpa-onnx"), "post-install")
    if "LICENSE" not in sherpa or "_deps" not in sherpa:
        problems.append("the sherpa-onnx module does not install the licences of its _deps")
    return problems


def top_level_problems(manifest):
    problems = []
    for key, expected in (
        ("app-id", APP_ID),
        ("command", smoke.COMMAND),
        ("runtime", "org.gnome.Platform"),
        ("sdk", "org.gnome.Sdk"),
    ):
        if manifest.get(key) != expected:
            problems.append(f"{key} is {manifest.get(key)!r}, expected {expected!r}")
    version = manifest.get("runtime-version")
    if not isinstance(version, str) or not version:
        problems.append(
            f"runtime-version is {version!r}: quote it, or YAML turns 51 into a number "
            "(and 25.08 into 25.08 the float)"
        )
    return problems


MANIFEST_RULES = [
    top_level_problems,
    source_problems,
    dir_source_problems,
    finish_args_problems,
    build_network_problems,
    module_problems,
    speech_from_source_problems,
    cleanup_problems,
    install_problems,
    onnxruntime_problems,
]


def load_manifest():
    return yaml.safe_load(MANIFEST.read_text())


# ══ The manifest as it is ═════════════════════════════════════════════════


@unittest.skipIf(yaml is None, NEEDS_YAML)
class ManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = load_manifest()

    def test_the_manifest_passes_every_rule(self):
        for rule in MANIFEST_RULES:
            with self.subTest(rule.__name__):
                self.assertEqual(rule(copy.deepcopy(self.manifest)), [])

    def test_finish_args_are_the_expected_list(self):
        args = self.manifest["finish-args"]
        self.assertEqual(sorted(args), sorted(smoke.EXPECTED_FINISH_ARGS))
        self.assertEqual(len(args), len(set(args)))
        for forbidden in smoke.FORBIDDEN_FINISH_ARGS:
            self.assertNotIn(forbidden, args)

    def test_the_expected_list_has_no_duplicates_and_nothing_forbidden(self):
        expected = smoke.EXPECTED_FINISH_ARGS
        self.assertEqual(len(expected), len(set(expected)))
        self.assertFalse(set(expected) & set(smoke.FORBIDDEN_FINISH_ARGS))

    def test_the_sources_are_all_there_to_be_checked(self):
        # Guards the walk itself: a rule that sees no sources passes.
        sources = all_sources(self.manifest)
        kinds = {source.get("type") for _, source in sources}
        self.assertLessEqual({"archive", "file", "git", "dir", "shell", "inline"}, kinds)
        self.assertGreater(len(sources), 400, "cargo-sources.json was not read")
        by_module = {}
        for module, source in sources:
            by_module.setdefault(module, []).append(source)
        self.assertEqual(len([s for s in by_module["sherpa-onnx"] if s["type"] == "file"]), 10)
        self.assertEqual(len([s for s in by_module["dasher"] if s["type"] == "git"]), 3)
        pinned = [s for _, s in sources if "sha256" in s or "commit" in s]
        self.assertGreater(len(pinned), 400)

    def test_modules_are_found_in_build_order(self):
        names = [module["name"] for module in modules_of(self.manifest)]
        self.assertEqual(names[-1], APP_MODULE)
        self.assertEqual(names[-3:-1], SPEECH_MODULES)
        self.assertIn("gtkmm", names)

    def test_cargo_sources_is_a_source_of_the_app_module(self):
        entries = module_named(self.manifest, APP_MODULE)["sources"]
        self.assertIn("cargo-sources.json", entries)
        vendored = load_included("cargo-sources.json")
        self.assertTrue(all(s["type"] in ("archive", "inline", "git", "shell") for s in vendored))
        crates = [s for s in vendored if s["type"] == "archive"]
        self.assertTrue(
            all(s["url"].startswith("https://static.crates.io/crates/") for s in crates)
        )

    def test_the_lock_is_installed_as_the_wrapper_s_cargo_lock(self):
        lock = [
            s
            for s in sources_of(module_named(self.manifest, APP_MODULE))
            if s.get("path") == "rust-tts-wrapper-Cargo.lock"
        ]
        self.assertEqual(len(lock), 1)
        self.assertEqual(
            (lock[0]["dest"], lock[0]["dest-filename"]), ("rust-tts-wrapper", "Cargo.lock")
        )
        self.assertTrue((MANIFEST_DIR / "rust-tts-wrapper-Cargo.lock.commit").is_file())

    def test_the_app_is_built_without_its_tests_and_with_the_speech_features(self):
        commands = commands_of(module_named(self.manifest, APP_MODULE), "build-commands")
        self.assertIn("-DBUILD_GTK_TESTS=OFF", commands)
        self.assertIn("-DCMAKE_INSTALL_PREFIX=/app", commands)
        features = re.search(r"-DTTS_WRAPPER_FEATURES=(\S+)", commands).group(1).split(",")
        self.assertIn("sherpaonnx", features)
        # No speech-dispatcher socket in finish-args, so no `system` engine:
        # smoke-test.py expects the same of the built library.
        for engine in smoke.UNEXPECTED_ENGINES:
            self.assertNotIn(engine, features)

    def test_the_speech_libraries_are_static_and_end_up_in_the_build_prefix(self):
        sherpa = module_named(self.manifest, "sherpa-onnx")
        options = sherpa["config-opts"]
        self.assertIn("-DBUILD_SHARED_LIBS=OFF", options)
        self.assertIn("-DCMAKE_INSTALL_PREFIX=/app/sherpa-onnx", options)
        self.assertIn("/sherpa-onnx", self.manifest["cleanup"])
        self.assertIn(
            "-Donnxruntime_BUILD_SHARED_LIB=OFF",
            commands_of(module_named(self.manifest, "onnxruntime"), "build-commands"),
        )

    def test_the_licence_directories_the_smoke_test_expects_are_the_ten_tarballs(self):
        # One licence directory per dependency tarball of the sherpa-onnx
        # module (cmake unpacks each into _deps/<name>-src).
        tarballs = [
            s for s in sources_of(module_named(self.manifest, "sherpa-onnx")) if s["type"] == "file"
        ]
        self.assertEqual(len(tarballs), len(smoke.SHERPA_DEP_LICENCE_DIRS))
        for directory in smoke.SHERPA_DEP_LICENCE_DIRS:
            stem = directory.replace("_", "-")
            self.assertTrue(
                any(s["dest-filename"].startswith(stem) for s in tarballs),
                f"no dependency tarball for the licence directory {directory}",
            )


# ══ Each rule notices its regression ══════════════════════════════════════


@unittest.skipIf(yaml is None, NEEDS_YAML)
class ManifestRuleTests(unittest.TestCase):
    def setUp(self):
        self.manifest = load_manifest()

    def module(self, name):
        return next(m for m in self.manifest["modules"] if m["name"] == name)

    def source(self, module, fragment="", kind=None):
        """The first source of `module` whose url contains `fragment` and,
        when one is given, whose type is `kind`."""
        return next(
            s
            for s in self.module(module)["sources"]
            if isinstance(s, dict) and fragment in s.get("url", "") and kind in (None, s["type"])
        )

    def assertRuleFails(self, rule, pattern, count=1):
        problems = rule(self.manifest)
        self.assertEqual(len(problems), count, problems)
        self.assertRegex(" | ".join(problems), pattern)
        # Only the rule in question: the others still pass, so each
        # regression has one owner.
        return problems

    # ── sources ──────────────────────────────────────────────────────────
    def test_a_branch_source_fails(self):
        source = self.source("dasher", "DasherCore")
        del source["commit"]
        source["branch"] = "main"
        self.assertRuleFails(
            source_problems, r"`branch: main` moves.*no commit \(40 hex digits\)", count=2
        )

    def test_a_branch_next_to_a_commit_fails(self):
        self.source("dasher", "DasherCore")["branch"] = "main"
        self.assertRuleFails(
            source_problems, r"module dasher: git source .*DasherCore.*`branch: main`"
        )

    def test_a_git_source_without_commit_fails(self):
        del self.source("dasher", "rust-tts-wrapper")["commit"]
        self.assertRuleFails(source_problems, r"rust-tts-wrapper.git: no commit \(40 hex digits\)$")

    def test_a_tag_without_commit_fails(self):
        del self.source("dasher", "libsdl-org/SDL")["commit"]
        self.assertRuleFails(
            source_problems, r"no commit \(40 hex digits\); tag release-\S+ alone can be moved"
        )
        del self.source("sherpa-onnx", "k2-fsa/sherpa-onnx")["commit"]
        self.assertRuleFails(source_problems, r"module sherpa-onnx: git source", count=2)

    def test_a_git_source_without_url_fails(self):
        del self.source("dasher", "DasherCore")["url"]
        self.assertRuleFails(source_problems, r"^module dasher: git source : no url$")

    def test_a_short_or_malformed_commit_fails(self):
        source = self.source("dasher", "DasherCore")
        for commit in (
            source["commit"][:39],
            source["commit"] + "0",
            source["commit"][:7],
            "main",
            1234567,
        ):
            with self.subTest(commit=commit):
                source["commit"] = commit
                self.assertRuleFails(source_problems, r"no commit \(40 hex digits\)")

    def test_a_download_without_sha256_fails(self):
        del self.source("gtkmm", "gtkmm-4")["sha256"]
        self.assertRuleFails(source_problems, r"module gtkmm: archive source .*: no sha256")

    def test_a_malformed_sha256_fails(self):
        source = self.source("sherpa-onnx", "kissfft")
        for digest in (
            source["sha256"][:63],
            source["sha256"] + "0",
            source["sha256"].upper(),
            "sha256:" + source["sha256"],
        ):
            with self.subTest(digest=digest):
                source["sha256"] = digest
                self.assertRuleFails(
                    source_problems, r"module sherpa-onnx: file source .*kissfft.*: no sha256"
                )

    def test_an_http_url_fails(self):
        source = self.source("mm-common", "mm-common")
        source["url"] = source["url"].replace("https://", "http://")
        self.assertRuleFails(source_problems, r"http://download.gnome.org/\S+ is not an https URL")

    def test_an_http_mirror_fails(self):
        source = self.source("onnxruntime", "microsoft/onnxruntime")
        source["mirror-urls"] = [
            url.replace("https://", "http://") for url in source["mirror-urls"]
        ]
        problems = self.assertRuleFails(
            source_problems, r"http://github.com/pykeio/onnxruntime\S+ is not an https URL", count=2
        )
        self.assertRegex(problems[1], r"http://github.com/intel/onnxruntime\S+ is not an https URL")

    def test_an_http_git_url_fails(self):
        self.source("dasher", "DasherCore")[
            "url"
        ] = "git://github.com/dasher-project/DasherCore.git"
        self.assertRuleFails(source_problems, r"git://\S+ is not an https URL")

    def test_an_unknown_source_type_fails(self):
        self.module("dasher")["sources"].append({"type": "svn", "url": "https://example.org/svn"})
        self.assertRuleFails(
            source_problems, r"module dasher: svn source .*: unknown source type 'svn'"
        )

    def test_a_source_without_type_fails(self):
        self.module("dasher")["sources"].append(
            {"url": "https://example.org/x.tar.gz", "sha256": "0" * 64}
        )
        self.assertRuleFails(source_problems, r"unknown source type None")

    def test_a_local_file_that_does_not_exist_fails(self):
        self.source("dasher", kind="file")["path"] = "no-such-Cargo.lock"
        self.assertRuleFails(
            source_problems, r"no-such-Cargo.lock does not exist next to the manifest"
        )

    def test_a_patch_that_does_not_exist_fails(self):
        self.module("gtkmm")["sources"].append(
            {"type": "patch", "paths": ["fix-a.patch", "fix-b.patch"]}
        )
        self.assertRuleFails(
            source_problems, r"fix-a.patch does not exist.*fix-b.patch does not exist", count=2
        )

    def test_a_file_source_with_nothing_to_fetch_fails(self):
        self.module("gtkmm")["sources"].append({"type": "file", "dest-filename": "x"})
        self.assertRuleFails(source_problems, r"neither url nor path")

    def test_an_included_source_list_is_checked_too(self):
        vendored = load_included("cargo-sources.json")
        crate = next(s for s in vendored if s["type"] == "archive")
        self.assertEqual(
            source_problems({"modules": [{"name": "m", "sources": [dict(crate)]}]}), []
        )
        broken = dict(crate)
        del broken["sha256"]
        self.assertEqual(len(source_problems({"modules": [{"name": "m", "sources": [broken]}]})), 1)

    def test_sources_of_nested_modules_are_checked_too(self):
        self.module("gtkmm")["modules"] = [
            {
                "name": "inner",
                "sources": [{"type": "git", "url": "https://example.org/x.git", "branch": "main"}],
            }
        ]
        self.assertRuleFails(source_problems, r"module inner: git source", count=2)
        names = [module["name"] for module in modules_of(self.manifest)]
        self.assertEqual(names.index("inner") + 1, names.index("gtkmm"))

    # ── the working tree ─────────────────────────────────────────────────
    def test_a_second_dir_source_fails(self):
        self.module("sherpa-onnx")["sources"].append({"type": "dir", "path": "."})
        self.assertRuleFails(
            dir_source_problems, r"`type: dir` sources in \['sherpa-onnx', 'dasher'\]"
        )

    def test_no_dir_source_fails(self):
        self.module("dasher")["sources"].remove(self.source("dasher", kind="dir"))
        self.assertRuleFails(dir_source_problems, r"`type: dir` sources in no module")

    def test_a_dir_source_that_points_nowhere_or_at_a_file_fails(self):
        for path in ("../../nowhere", MANIFEST.name):
            with self.subTest(path):
                self.source("dasher", kind="dir")["path"] = path
                self.assertRuleFails(
                    source_problems, rf"{re.escape(path)} does not exist next to the manifest"
                )

    # ── permissions ──────────────────────────────────────────────────────
    def test_an_extra_finish_arg_fails(self):
        for extra in (
            "--socket=session-bus",
            "--filesystem=host",
            "--talk-name=org.freedesktop.Flatpak",
        ):
            with self.subTest(extra):
                self.manifest = load_manifest()
                self.manifest["finish-args"].append(extra)
                problems = self.assertRuleFails(
                    finish_args_problems, rf"unexpected permission {extra}"
                )
                reason = smoke.FORBIDDEN_FINISH_ARGS.get(extra)
                if reason:
                    self.assertIn(reason, problems[0])

    def test_a_dropped_finish_arg_fails(self):
        for dropped in smoke.EXPECTED_FINISH_ARGS:
            with self.subTest(dropped):
                self.manifest = load_manifest()
                self.manifest["finish-args"].remove(dropped)
                problems = self.assertRuleFails(finish_args_problems, r"missing permission")
                self.assertIn(f"missing permission {dropped} ", problems[0])

    def test_a_finish_arg_listed_twice_fails(self):
        self.manifest["finish-args"].append("--share=ipc")
        self.assertRuleFails(finish_args_problems, r"finish-arg --share=ipc is listed twice")

    # ── the offline build ────────────────────────────────────────────────
    def test_network_during_the_build_fails(self):
        shared = {"build-args": ["--share=network"]}
        for where, change in (
            ("the top level", lambda: self.manifest.update({"build-options": shared})),
            ("module dasher", lambda: self.module("dasher")["build-options"].update(shared)),
            (
                r"module onnxruntime \(arch aarch64\)",
                lambda: self.module("onnxruntime")["build-options"].update(
                    arch={"aarch64": shared}
                ),
            ),
        ):
            with self.subTest(where):
                self.manifest = load_manifest()
                change()
                self.assertRuleFails(
                    build_network_problems, rf"^{where} has build-args --share=network$"
                )

    def test_cargo_that_may_go_online_fails(self):
        env = self.module("dasher")["build-options"]["env"]
        for value in (True, "false", None):
            with self.subTest(value=value):
                if value is None:
                    env.pop("CARGO_NET_OFFLINE", None)
                else:
                    env["CARGO_NET_OFFLINE"] = value
                self.assertRuleFails(
                    build_network_problems, r"does not set CARGO_NET_OFFLINE to 'true'"
                )

    # ── modules ──────────────────────────────────────────────────────────
    def test_a_module_name_used_twice_fails(self):
        self.manifest["modules"].append({"name": "gtkmm", "sources": []})
        self.assertRuleFails(module_problems, r"module name gtkmm is used twice")

    def test_a_missing_speech_module_fails(self):
        for name in SPEECH_MODULES:
            with self.subTest(name):
                self.manifest = load_manifest()
                self.manifest["modules"].remove(self.module(name))
                self.assertRuleFails(module_problems, rf"no module named {name}")

    def test_speech_modules_built_after_the_app_fail(self):
        modules = self.manifest["modules"]
        modules.append(modules.pop(modules.index(self.module("sherpa-onnx"))))
        self.assertRuleFails(module_problems, r"are not built in\s+that order")
        self.manifest = load_manifest()
        modules = self.manifest["modules"]
        ort, sherpa = modules.index(self.module("onnxruntime")), modules.index(
            self.module("sherpa-onnx")
        )
        modules[ort], modules[sherpa] = modules[sherpa], modules[ort]
        self.assertRuleFails(module_problems, r"are not built in\s+that order")

    # ── speech from source ───────────────────────────────────────────────
    def test_the_prebuilt_archive_route_fails(self):
        env = self.module("dasher")["build-options"]["env"]
        env["SHERPA_ONNX_ARCHIVE_DIR"] = env.pop("SHERPA_ONNX_LIB_DIR")
        self.assertRuleFails(
            speech_from_source_problems,
            r"does not point SHERPA_ONNX_LIB_DIR at /app/sherpa-onnx/lib.*SHERPA_ONNX_ARCHIVE_DIR is set",
            count=2,
        )

    def test_a_prebuilt_speech_library_as_a_source_fails(self):
        for url in (
            "https://github.com/k2-fsa/sherpa-onnx/releases/download/v2.0.1/sherpa-onnx-v2.0.1-linux-x64-static-lib.tar.bz2",
            "https://github.com/csukuangfj/onnxruntime-libs/releases/download/v1.28.2/onnxruntime-linux-x64-static_lib-1.28.2-glibc2_17.zip",
        ):
            with self.subTest(url):
                self.manifest = load_manifest()
                self.module("dasher")["sources"].append(
                    {"type": "file", "url": url, "sha256": "0" * 64}
                )
                self.assertRuleFails(
                    speech_from_source_problems, r"module dasher: .* is a prebuilt speech library"
                )
                self.assertEqual(source_problems(self.manifest), [])

    def test_the_from_source_tag_tarball_is_not_taken_for_a_prebuilt_library(self):
        for _, source in all_sources(self.manifest):
            self.assertIsNone(PREBUILT_SPEECH_URL.search(source.get("url", "")), source.get("url"))

    # ── cleanup ──────────────────────────────────────────────────────────
    def test_a_cleanup_entry_that_is_dropped_fails(self):
        # Everything smoke-test.py expects to be gone, one entry at a time.
        paths = [f"/{path}" for path in smoke.CLEANED_PATHS if path not in NOT_IN_MANIFEST_CLEANUP]
        for entry in paths + [f"*{suffix}" for suffix in smoke.CLEANED_SUFFIXES]:
            with self.subTest(entry):
                self.manifest = load_manifest()
                self.manifest["cleanup"].remove(entry)
                self.assertRuleFails(
                    cleanup_problems, rf"^cleanup does not remove {re.escape(entry)}$"
                )

    def test_the_cleanup_exceptions_are_not_stale(self):
        # That every other path has a cleanup pattern is the rule itself.
        cleanup = self.manifest["cleanup"]
        for path in NOT_IN_MANIFEST_CLEANUP:
            self.assertIn(path, smoke.CLEANED_PATHS, "stale entry in NOT_IN_MANIFEST_CLEANUP")
            self.assertNotIn(f"/{path}", cleanup, "the manifest removes it now: drop the exception")

    # ── install steps ────────────────────────────────────────────────────
    def edit_post_install(self, module, old, new):
        commands = self.module(module)["post-install"]
        self.assertTrue(any(old in command for command in commands), old)
        self.module(module)["post-install"] = [command.replace(old, new) for command in commands]

    def test_a_launcher_that_does_something_else_fails(self):
        for old, new, pattern in (
            ("/app:/app/lib64", "/app/lib", r"does not put /app:/app/lib64 on LD_LIBRARY_PATH"),
            ("exec /app/dasher", "exec /app/bin/dasher-real", r"does not `exec /app/dasher"),
            ("/app/bin/dasher", "/app/bin/dasher-gtk", r"does not create /app/bin/dasher$"),
        ):
            with self.subTest(old):
                self.manifest = load_manifest()
                self.edit_post_install("dasher", old, new)
                self.assertRuleFails(install_problems, pattern)

    def test_a_file_installed_under_another_name_fails(self):
        for relative, kind in smoke.EXPECTED_FILES:
            if not relative.startswith(INSTALLED_BY_POST_INSTALL):
                continue
            with self.subTest(relative):
                self.manifest = load_manifest()
                self.edit_post_install("dasher", f"/app/{relative}", f"/app/{relative}.renamed")
                self.assertRuleFails(
                    install_problems, rf"^post-install does not create /app/{re.escape(relative)}$"
                )

    def test_an_onnxruntime_module_without_version_or_licences_fails(self):
        module = self.module("onnxruntime")
        module["build-commands"] = [
            c for c in module["build-commands"] if "VERSION_NUMBER" not in c
        ]
        self.assertRuleFails(onnxruntime_problems, r"no `test .* = <version>` build command")
        self.manifest = load_manifest()
        self.edit_post_install("onnxruntime", "ThirdPartyNotices.txt", "NOTICE")
        self.assertRuleFails(
            onnxruntime_problems, r"post-install does not install ThirdPartyNotices.txt"
        )
        self.manifest = load_manifest()
        del self.module("sherpa-onnx")["post-install"]
        self.assertRuleFails(onnxruntime_problems, r"does not install the licences of its _deps")

    # ── top level ────────────────────────────────────────────────────────
    def test_wrong_top_level_keys_fail(self):
        for key, value, pattern in (
            ("app-id", "org.example.other", r"app-id is 'org.example.other'"),
            ("command", "dasher-gtk", r"command is 'dasher-gtk'"),
            ("runtime", "org.freedesktop.Platform", r"runtime is 'org.freedesktop.Platform'"),
            ("sdk", "org.freedesktop.Sdk", r"sdk is 'org.freedesktop.Sdk'"),
            ("runtime-version", 51, r"runtime-version is 51: quote it"),
            ("runtime-version", 25.08, r"runtime-version is 25.08: quote it"),
        ):
            with self.subTest(key=key, value=value):
                self.manifest = load_manifest()
                self.manifest[key] = value
                self.assertRuleFails(top_level_problems, pattern)

    def test_every_rule_is_applied_to_the_manifest(self):
        rules = {name for name, value in globals().items() if name.endswith("_problems")}
        self.assertEqual({rule.__name__ for rule in MANIFEST_RULES}, rules)
        self.assertEqual(len(MANIFEST_RULES), len(rules))

    def test_each_regression_has_one_owner(self):
        """A manifest broken for one rule still passes the others, so a
        failure in ManifestTests names the thing that is wrong."""
        self.manifest["finish-args"].append("--socket=session-bus")
        failing = [rule.__name__ for rule in MANIFEST_RULES if rule(self.manifest)]
        self.assertEqual(failing, ["finish_args_problems"])


# ══ Metainfo and desktop file ═════════════════════════════════════════════


class MetainfoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = ET.parse(METAINFO).getroot()

    def test_names_the_app_and_its_desktop_file(self):
        self.assertEqual(self.root.tag, "component")
        self.assertEqual(self.root.get("type"), "desktop-application")
        self.assertEqual(self.root.findtext("id"), APP_ID)
        launchable = self.root.find("launchable")
        self.assertEqual(launchable.get("type"), "desktop-id")
        self.assertEqual(launchable.text, DESKTOP.name)
        self.assertTrue(DESKTOP.is_file())

    def test_has_what_flathub_requires(self):
        for tag in ("name", "summary", "metadata_license", "project_license"):
            with self.subTest(tag):
                self.assertTrue(
                    (self.root.findtext(tag) or "").strip(), f"<{tag}> is missing or empty"
                )
        self.assertTrue(self.root.findall("./description/p"), "<description> has no paragraph")
        self.assertTrue(
            (self.root.findtext("./developer/name") or "").strip(), "no <developer><name>"
        )
        self.assertTrue(self.root.find("developer").get("id"), "<developer> has no id")
        homepage = self.root.find("./url[@type='homepage']")
        self.assertIsNotNone(homepage, 'no <url type="homepage">')
        self.assertTrue(homepage.text.startswith("https://"))
        rating = self.root.find("content_rating")
        self.assertIsNotNone(rating, "no <content_rating>")
        self.assertRegex(rating.get("type", ""), r"^oars-1\.[01]$")
        self.assertTrue(self.root.findall("./releases/release"), "no <release>")

    def test_urls_are_https(self):
        for url in self.root.findall("url"):
            self.assertTrue(url.text.startswith("https://"), url.text)

    def test_has_a_default_screenshot_with_caption_and_image(self):
        default = self.root.findall("./screenshots/screenshot[@type='default']")
        self.assertEqual(len(default), 1, 'exactly one <screenshot type="default">')
        self.assertTrue(
            (default[0].findtext("caption") or "").strip(), "the screenshot has no caption"
        )
        for screenshot in self.root.findall("./screenshots/screenshot"):
            self.assertTrue(screenshot.findall("image"), "a <screenshot> without <image>")

    def test_every_screenshot_is_pinned_and_matches_the_image_in_the_repository(self):
        images = self.root.findall("./screenshots/screenshot/image")
        self.assertTrue(images)
        for image in images:
            url = image.text.strip()
            with self.subTest(url):
                ref, path = smoke.pinned_screenshot_url(url)  # raises for a branch
                self.assertIn("/dasher-project/Dasher-GTK/", url)
                local = REPO / path
                self.assertTrue(local.is_file(), f"{path} is not in the repository")
                width, height = smoke.png_size(local)
                self.assertEqual(
                    (image.get("width"), image.get("height")),
                    (str(width), str(height)),
                    f"{path} is {width}x{height}",
                )
                self.assertEqual(image.get("type"), "source")
                self.compare_with_commit(ref, path, local)

    def compare_with_commit(self, ref, path, local):
        """When the URL names a commit that this checkout has, the image in
        that commit is the image in the tree. Left out (not failed) where
        git or the commit is missing, as in CI's shallow clone."""
        if not (COMMIT.fullmatch(ref) and (REPO / ".git").exists() and shutil.which("git")):
            return
        blob = subprocess.run(
            ["git", "-C", str(REPO), "cat-file", "blob", f"{ref}:{path}"], capture_output=True
        )
        if blob.returncode != 0:
            return
        self.assertEqual(
            blob.stdout,
            local.read_bytes(),
            f"{path} in commit {ref[:12]}, which the screenshot URL names, is not the file in "
            "the tree: point the URL at the commit that changed the image",
        )

    def test_the_newest_release_is_the_version_in_cmakelists(self):
        project_version = smoke.cmake_fallback_version((REPO / "CMakeLists.txt").read_text())
        self.assertIsNotNone(
            project_version, "DASHER_GTK_FALLBACK_VERSION not found in CMakeLists.txt"
        )
        self.assertEqual(
            smoke.release_problems(smoke.metainfo_releases(self.root), project_version), []
        )

    def test_releases_have_a_version_a_date_and_a_description(self):
        versions = []
        for release in self.root.findall("./releases/release"):
            version = release.get("version")
            with self.subTest(version):
                self.assertRegex(version or "", r"^[0-9]+\.[0-9]+\.[0-9]+(-rc\.[0-9]+)?$")
                self.assertRegex(release.get("date") or "", r"^[0-9]{4}-[0-9]{2}-[0-9]{2}$")
                self.assertTrue(release.findall("./description/p"), "no description")
            versions.append(version)
        self.assertEqual(len(versions), len(set(versions)), "a release is listed twice")
        self.assertEqual(versions, sorted(versions, key=smoke.version_key, reverse=True))


class DesktopFileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.entry = smoke.read_metadata(DESKTOP.read_text())["Desktop Entry"]

    def test_starts_the_command_of_the_manifest(self):
        self.assertEqual(self.entry["Exec"].split()[0], smoke.COMMAND)
        self.assertEqual(self.entry.get("TryExec", smoke.COMMAND), smoke.COMMAND)
        self.assertEqual(self.entry["Type"], "Application")
        self.assertEqual(self.entry["Terminal"], "false")

    def test_uses_the_icon_that_is_installed_under_the_app_id(self):
        self.assertEqual(self.entry["Icon"], APP_ID)
        icons = [relative for relative, _ in smoke.EXPECTED_FILES if "/icons/" in relative]
        self.assertEqual(len(icons), 2)
        for relative in icons:
            self.assertEqual(Path(relative).stem, APP_ID)

    def test_is_named_after_the_app_id(self):
        self.assertEqual(DESKTOP.name, f"{APP_ID}.desktop")
        self.assertEqual(METAINFO.name, f"{APP_ID}.metainfo.xml")
        self.assertEqual(MANIFEST.name, f"{APP_ID}.yaml")
        self.assertTrue(MANIFEST.is_file())

    def test_has_a_name_a_comment_and_categories(self):
        for key in ("Name", "Comment", "Categories"):
            self.assertTrue(self.entry.get(key, "").strip(), key)
        self.assertTrue(self.entry["Categories"].endswith(";"))

    @unittest.skipIf(yaml is None, NEEDS_YAML)
    def test_agrees_with_the_manifest(self):
        manifest = load_manifest()
        self.assertEqual(self.entry["Exec"].split()[0], manifest["command"])
        self.assertEqual(self.entry["Icon"], manifest["app-id"])


# ══ Paths named in the documentation ══════════════════════════════════════

DOCUMENTS = ["README.md", "CONTRIBUTING.md"]
PATH_PREFIXES = ("packaging/", "Scripts/", ".github/", "tests/", "src/")
# Named in the documentation without being a file of this repository:
# (document, path) -> why.
NOT_OF_THIS_REPOSITORY = {
    ("CONTRIBUTING.md", "src/data/feature-status.json"): "a file of the `website` repository",
    ("CONTRIBUTING.md", "src/your_file.cpp"): "a placeholder in an example command",
}
# References that are broken today and were left alone (nobody should have
# to fix the documentation to get this test green on an unrelated change):
# (document, path) -> why. Empty is the goal; remove an entry when the
# reference is fixed, the test says when one is stale.
KNOWN_BROKEN = {}

FENCE = re.compile(r"(?ms)^```.*?^```[ \t]*$")
CODE_SPAN = re.compile(r"`([^`\n]+)`")
LINK_TARGET = re.compile(r"\]\(([^)\s]+)\)")
PLACEHOLDER = re.compile(r"[*<>{}$]|…|\.\.\.")


def clean_token(token):
    """A word of a command or code span as a repository path, or None."""
    token = token.strip("\"'`()[],;:")
    # The README builds from outside the repository with "$repo/<path>".
    token = re.sub(r"^\$(repo|\{repo\})/", "", token)
    token = token.removeprefix("./")
    token = token.split("#", 1)[0].rstrip(".,;:")
    if not token.startswith(PATH_PREFIXES) or PLACEHOLDER.search(token):
        return None
    return token


def referenced_paths(text):
    """The repository paths a Markdown document names: in code spans and
    fenced blocks those under PATH_PREFIXES, and every relative link."""
    found = set()
    prose = FENCE.sub("\n", text)
    for block in FENCE.findall(text):
        found.update(filter(None, (clean_token(token) for token in block.split())))
    for span in CODE_SPAN.findall(prose):
        found.update(filter(None, (clean_token(token) for token in span.split())))
    for target in LINK_TARGET.findall(prose):
        if target.startswith(("http://", "https://", "mailto:", "#", "../")):
            continue
        target = target.removeprefix("./").split("#", 1)[0]
        if target and not PLACEHOLDER.search(target):
            found.add(target)
    return found


class DocumentationTests(unittest.TestCase):
    def test_paths_named_in_the_documentation_exist(self):
        for document in DOCUMENTS:
            paths = referenced_paths((REPO / document).read_text())
            self.assertTrue(paths, f"no path found in {document}: the extraction is broken")
            for path in sorted(paths):
                if (document, path) in NOT_OF_THIS_REPOSITORY or (document, path) in KNOWN_BROKEN:
                    continue
                with self.subTest(document=document, path=path):
                    self.assertTrue(
                        (REPO / path).exists(),
                        f"{document} names {path}, which does not exist in the repository",
                    )

    def test_the_exception_lists_are_not_stale(self):
        named = {(d, p) for d in DOCUMENTS for p in referenced_paths((REPO / d).read_text())}
        for key, why in list(NOT_OF_THIS_REPOSITORY.items()) + list(KNOWN_BROKEN.items()):
            with self.subTest(key):
                self.assertTrue(why.strip())
                self.assertIn(key, named, "no longer named in the document: remove the entry")
        for document, path in KNOWN_BROKEN:
            with self.subTest(document=document, path=path):
                self.assertFalse((REPO / path).exists(), "the reference is fixed: remove the entry")

    def test_the_packaging_files_the_readme_explains_are_found(self):
        paths = referenced_paths((REPO / "README.md").read_text())
        for expected in (
            f"packaging/flatpak/{APP_ID}.yaml",
            "packaging/flatpak/cargo-sources.json",
            "packaging/flatpak/rust-tts-wrapper-Cargo.lock",
            "packaging/flatpak/rust-tts-wrapper-Cargo.lock.commit",
            "Scripts/check-flatpak-pins.py",
            ".github/workflows/publish.yml",
            "packaging/build-appimage.sh",
            "Scripts/tag-release.sh",
        ):
            self.assertIn(expected, paths)

    def test_extraction(self):
        text = "\n".join(
            [
                "See `packaging/flatpak/x.yaml`, `Scripts/tests/` and `bash packaging/build.sh`.",
                "A [link](.github/workflows/publish.yml), [one](./LICENSE), [anchor](./README.md#top),",
                "[web](https://example.org/src/x), [up](../../issues), [here](#section).",
                "Globs `src/**/*.cpp`, `src/MainWindow.*`, `packaging/<name>` are not paths.",
                "Other code: `cmake --build build`, `tests`, `build/Dasher/`, `rust-tts-wrapper/src/x`.",
                "```sh",
                "python3 -m unittest discover -s Scripts/tests -v",
                'flatpak-builder build "$repo/packaging/flatpak/app.yaml"',
                "Scripts/tag-release.sh --push v0.2.12 # verifies",
                "```",
                "After the block: `tests/CMakeLists.txt`, `Scripts/notes.md#usage`.",
            ]
        )
        self.assertEqual(
            referenced_paths(text),
            {
                "packaging/flatpak/x.yaml",
                "Scripts/tests/",
                "packaging/build.sh",
                ".github/workflows/publish.yml",
                "LICENSE",
                "README.md",
                "Scripts/tests",
                "packaging/flatpak/app.yaml",
                "Scripts/tag-release.sh",
                "tests/CMakeLists.txt",
                "Scripts/notes.md",
            },
        )


# ── Workflows ───────────────────────────────────────────────────────────
WORKFLOWS = REPO / ".github" / "workflows"
SMOKE_TEST = "packaging/flatpak/tests/smoke-test.py"


@unittest.skipIf(yaml is None, NEEDS_YAML)
class WorkflowTests(unittest.TestCase):
    """The tests are only worth something while CI runs them. A workflow
    edit that drops a step, or adds a path filter in front of it, would
    go unnoticed otherwise: nothing fails when a test does not run."""

    @staticmethod
    def load(name):
        data = yaml.safe_load((WORKFLOWS / name).read_text())
        # YAML 1.1 reads the key `on` as the boolean True.
        data["on"] = data.pop(True, data.get("on"))
        return data

    def test_every_workflow_parses_and_has_jobs(self):
        names = sorted(p.name for p in WORKFLOWS.glob("*.yml"))
        self.assertIn("packaging-tests.yml", names)
        for name in names:
            with self.subTest(workflow=name):
                self.assertTrue(self.load(name).get("jobs"), "no jobs")

    def test_the_unit_tests_run_on_every_push_and_pull_request(self):
        workflow = self.load("packaging-tests.yml")
        for event in ("push", "pull_request"):
            with self.subTest(event=event):
                self.assertIn(event, workflow["on"])
                # No branch or path filter: None, or an empty mapping.
                self.assertFalse(workflow["on"][event], "the event has a filter")
        commands = [
            step.get("run", "") for job in workflow["jobs"].values() for step in job["steps"]
        ]
        self.assertTrue(
            any("unittest discover -s Scripts/tests" in command for command in commands),
            "no step runs the tests in Scripts/tests",
        )
        self.assertTrue(
            any(re.search(r"(?m)^python3 -c 'import yaml", command) for command in commands),
            "no step fails when PyYAML is missing (the manifest tests would skip themselves)",
        )

    def test_the_pin_check_has_pyyaml_wherever_a_workflow_runs_it(self):
        """Scripts/check-flatpak-pins.py reads the manifest with PyYAML and
        stops without it. Every job that runs it makes sure of PyYAML
        first, in an earlier step or earlier in the same script."""
        runs_it = []
        for path in sorted(WORKFLOWS.glob("*.yml")):
            for job in self.load(path.name)["jobs"].values():
                script = ""
                for step in job.get("steps", []):
                    before, found, _ = step.get("run", "").partition("check-flatpak-pins.py")
                    if found:
                        runs_it.append(path.name)
                        self.assertRegex(
                            script + before,
                            r"(?m)^python3 -c 'import yaml' 2>/dev/null \|\| "
                            r"\{ sudo apt-get update && sudo apt-get install -y python3-yaml; \}$",
                            f"{path.name} runs the pin check without making sure of PyYAML",
                        )
                    script += step.get("run", "") + "\n"
        self.assertEqual(runs_it, ["pin-sync.yml", "shared-deps-bump.yml"])

    # ── the smoke test in publish.yml ────────────────────────────────────
    # Asserted on what the steps do and in which order, not on their text:
    # the commands are split into words the way the shell would, so that a
    # reformatted or reordered command line changes nothing here.

    @staticmethod
    def commands(step):
        """The commands of a step's `run:` as lists of words. Comments are
        dropped, continuation lines joined, `&&` separates commands."""
        text = re.sub(r"\\\n", " ", step.get("run", ""))
        found = []
        for line in text.splitlines():
            for part in line.split("&&"):
                words = shlex.split(part, comments=True)
                if words:
                    found.append(words)
        return found

    @staticmethod
    def runner_path(value):
        """A path below the runner's temporary directory, in one spelling."""
        return value.replace("${{ runner.temp }}", "$RUNNER_TEMP")

    def flatpak_job(self):
        workflow = self.load("publish.yml")
        return workflow, workflow["jobs"]["flatpak"]

    def smoke_step(self):
        """(the flatpak job, the index of its smoke-test step, the step,
        the directory it tests, the options smoke-test.py reads from it)."""
        _, job = self.flatpak_job()
        index, step, command = self.step_running(job, SMOKE_TEST, "--strict")
        return job, index, step, *self.smoke_options(command)

    def step_using(self, job, action, accepts, missing):
        """(index, step) of the one step that uses `action` with a path
        that `accepts`; `missing` is what to say when there is none."""
        found = [
            (index, step)
            for index, step in enumerate(job["steps"])
            if step.get("uses", "").startswith(f"{action}@")
            and accepts(self.runner_path(step["with"]["path"]))
        ]
        self.assertEqual(len(found), 1, missing)
        return found[0]

    def step_running(self, job, program, *words):
        """(index, step, command) of the one step that starts `program`
        with `words` on its command line. A step that starts it inside
        `$(...)` counts when no step starts it as a command of its own;
        the command is None then."""
        started, mentioned = [], []
        for index, step in enumerate(job["steps"]):
            if not {program, *words} <= set(re.findall(r"[^\s\"'()]+", step.get("run", ""))):
                continue
            commands = [c for c in self.commands(step) if program in c[:2]]
            if commands:
                started.append((index, step, commands[0]))
            else:
                mentioned.append((index, step, None))
        found = started or mentioned
        self.assertEqual(len(found), 1, f"expected one step that runs {program} {words}")
        return found[0]

    def smoke_options(self, command):
        """The smoke test's own reading of the workflow's command line. An
        option it does not know ends the test with its usage error."""
        arguments = command[command.index(SMOKE_TEST) + 1 :]
        usage = io.StringIO()
        with tempfile.TemporaryDirectory() as build:
            (Path(build) / "metadata").write_text("[Application]\n")
            try:
                with contextlib.redirect_stderr(usage):
                    options = smoke.parse_args([build, *arguments[1:]])
            except SystemExit:
                self.fail(
                    "smoke-test.py does not take the workflow's command line: "
                    + usage.getvalue().strip().splitlines()[-1]
                )
        return arguments[0], options

    def test_the_smoke_test_runs_on_both_flatpak_legs(self):
        job, _, step, _, _ = self.smoke_step()
        arches = sorted(leg["arch"] for leg in job["strategy"]["matrix"]["include"])
        self.assertEqual(arches, ["aarch64", "x86_64"])
        # Unconditional and fatal: every leg, and a failure fails the leg.
        self.assertNotIn("if", step)
        self.assertNotIn("continue-on-error", step)
        for command in self.commands(step):
            self.assertNotIn("||", command)
            self.assertNotEqual(command[:2], ["set", "+e"])

    def test_it_tests_the_directory_that_was_built_after_build_and_upload(self):
        job, at, _, build_dir, _ = self.smoke_step()
        built, build_step, build_command = self.step_running(job, "flatpak-builder")
        self.assertLess(built, at)
        # flatpak-builder DIRECTORY MANIFEST, in the directory the step went to.
        positional = [word for word in build_command[1:] if not word.startswith("-")]
        self.assertEqual(len(positional), 2, build_command)
        directory, manifest = positional
        self.assertTrue(manifest.endswith(f"packaging/flatpak/{APP_ID}.yaml"), manifest)
        commands = self.commands(build_step)
        went_to = [c[1] for c in commands[: commands.index(build_command)] if c[0] == "cd"]
        self.assertTrue(went_to, "the build step does not say where it builds")
        self.assertEqual(build_dir, os.path.normpath(os.path.join(went_to[-1], directory)))
        # After the bundle is uploaded: a failing leg still leaves it to look at.
        uploaded, _ = self.step_using(
            job, "actions/upload-artifact", lambda path: ".flatpak" in path, "no bundle upload"
        )
        self.assertLess(uploaded, at)

    def test_a_release_needs_the_smoke_tested_legs(self):
        workflow, job = self.flatpak_job()
        self.assertIn("flatpak", workflow["jobs"]["release"]["needs"])
        # The job may be advisory for an experiment, never for a tag.
        advisory = job.get("continue-on-error", False)
        self.assertIsNot(advisory, True)
        if advisory:
            self.assertRegex(advisory, r"!\s*startsWith\(github\.ref, 'refs/tags/")

    def test_the_smoke_test_in_ci_skips_nothing_silently(self):
        options = self.smoke_step()[-1]
        self.assertTrue(options.strict)
        self.assertTrue(options.host_checks, "without it the linter does not run in CI")
        self.assertTrue(options.network, "without it the synthesis check has no voice in CI")
        self.assertEqual(options.installed, APP_ID)
        self.assertEqual(options.check, [])
        self.assertEqual(options.skip, [])
        # Only what depends on the machine may be skipped, never a check
        # of the build itself. (An unreachable voice server is let through
        # by the smoke test itself, with a warning.)
        self.assertLessEqual(set(options.allow_skip), {"audio-device", "input-devices"})

    def test_the_logs_of_the_smoke_test_are_uploaded_whatever_happens(self):
        job, at, _, _, options = self.smoke_step()
        self.assertTrue(options.keep_logs)
        index, step = self.step_using(
            job,
            "actions/upload-artifact",
            lambda path: path == options.keep_logs,
            "no step uploads the directory given to --keep-logs",
        )
        self.assertGreater(index, at)
        self.assertEqual(step.get("if"), "always()")

    def test_a_download_that_hangs_ends_well_inside_the_time_limit_of_the_step(self):
        """A voice download that never finishes is stopped and tried again,
        and then skipped with a warning. That is worth nothing if the step
        is cancelled first: all attempts and pauses together take at most
        half of the step's time limit."""
        _, _, step, _, options = self.smoke_step()
        self.assertTrue(options.network)
        limit = step.get("timeout-minutes")
        self.assertIsInstance(limit, int, "the smoke-test step has no time limit")
        worst = smoke.INSTALL_ATTEMPTS * smoke.INSTALL_TIMEOUT + sum(smoke.INSTALL_BACKOFF)
        self.assertLessEqual(worst, limit * 60 / 2)

    def test_the_voice_is_cached_under_a_key_that_follows_its_pin(self):
        job, at, _, _, options = self.smoke_step()
        index, cache = self.step_using(
            job,
            "actions/cache",
            lambda path: path == options.voice_cache,
            "no cache step for the directory given to --voice-cache",
        )
        self.assertLess(index, at)
        # A cache that cannot be restored must not stop the leg.
        self.assertNotIn("fail-on-cache-miss", cache["with"])
        # The key is what `smoke-test.py --voice-key` prints, in an earlier step.
        keyed, key_step, _ = self.step_running(job, SMOKE_TEST, "--voice-key")
        self.assertLess(keyed, index)
        output = re.search(r"steps\.([\w-]+)\.outputs\.([\w-]+)", cache["with"]["key"])
        self.assertIsNotNone(output, cache["with"]["key"])
        self.assertEqual(key_step.get("id"), output.group(1))
        self.assertRegex(key_step["run"], rf"\b{output.group(2)}=.*GITHUB_OUTPUT")
        self.assertIn("matrix.arch", cache["with"]["key"])


if __name__ == "__main__":
    unittest.main()
