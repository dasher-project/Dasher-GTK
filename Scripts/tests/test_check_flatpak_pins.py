#!/usr/bin/env python3
"""Tests for Scripts/check-flatpak-pins.py, the "Packaging pin sync" check.

Hermetic: no test here touches the network. Each test runs the checker in a
throwaway git repository with real gitlinks (so the checker's own
`git ls-tree` and `git ls-files` calls run for real) and small synthetic
packaging files. The two things that would leave the machine are replaced by
a fake upstream (FakeNet below): `urllib.request.urlopen`, and the
`git ls-remote` call that goes through `subprocess.run`.

A few tests (RealRepoTests) run the checker on the real manifest, lock and
crate list of this repository, with a fake upstream built from the manifest's
own values. They keep the synthetic manifest honest: if the real layout
drifts from what the checker's parsing expects, they fail.

Run from anywhere; Python 3.11 or newer and PyYAML (python3-yaml), which
the checker reads the manifest with:

    python3 -m unittest discover -s Scripts/tests -v

Environment:

  DASHER_PIN_SYNC_LIVE=1
      Also run LiveTests: the real checker in this repository against the
      real upstreams. Needs network; off by default.
  CHECK_FLATPAK_PINS_SCRIPT=/path/to/check-flatpak-pins.py
      Test another copy of the script, for example an older revision, to
      see a regression test fail on it.
"""

import contextlib
import email.message
import hashlib
import http.client
import importlib.util
import io
import json
import os
import re
import runpy
import shutil
import socket
import subprocess
import sys
import tempfile
import tomllib
import unittest
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import NamedTuple
from unittest import mock

import yaml  # the checker needs it too: without PyYAML there is nothing to test

REPO = Path(__file__).resolve().parent.parent.parent
SCRIPT = Path(
    os.environ.get("CHECK_FLATPAK_PINS_SCRIPT") or REPO / "Scripts" / "check-flatpak-pins.py"
).resolve()

# The real subprocess.run: the fake upstream passes everything that is not
# `git ls-remote` through to it.
REAL_RUN = subprocess.run


def load_checker():
    """Import the script (its file name is not a module name)."""
    spec = importlib.util.spec_from_file_location("check_flatpak_pins", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    write_bytecode, sys.dont_write_bytecode = sys.dont_write_bytecode, True
    try:
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = write_bytecode
    return module


cfp = load_checker()


# ── Synthetic fixture values ────────────────────────────────────────────


def sha1(seed: str) -> str:
    return hashlib.sha1(seed.encode()).hexdigest()


def sha256(seed: str) -> str:
    return hashlib.sha256(seed.encode()).hexdigest()


def near(commit: str) -> str:
    """A commit that differs from `commit` in its last character only. The
    two share every abbreviation, so only a comparison of all 40 characters
    tells them apart: tried next to OTHER wherever the checker compares two
    commits (its messages abbreviate to 12 characters, its comparisons must
    not)."""
    return commit[:-1] + ("1" if commit[-1] == "0" else "0")


WRAPPER = sha1("rust-tts-wrapper")
CORE = sha1("DasherCore")
SDL = sha1("SDL")
SHERPA = sha1("sherpa-onnx")
OTHER = sha1("some other commit")
SDL_TAG = "release-3.2.30"
SHERPA_TAG = "v2.0.1"
ORT = "1.28.2"
WRAPPER_VERSION = "1.16.2"

GITLINKS = {"rust-tts-wrapper": WRAPPER, "DasherCore": CORE, "Thirdparty/SDL": SDL}

MANIFEST_NAME = "packaging/flatpak/org.alternativeinterface.dasher.yaml"
LOCK_NAME = "packaging/flatpak/rust-tts-wrapper-Cargo.lock"
LOCK_COMMIT_NAME = "packaging/flatpak/rust-tts-wrapper-Cargo.lock.commit"
CARGO_SOURCES_NAME = "packaging/flatpak/cargo-sources.json"

CRATES_IO = "registry+https://github.com/rust-lang/crates.io-index"
SDL_GIT = "https://github.com/libsdl-org/SDL.git"
SHERPA_GIT = "https://github.com/k2-fsa/sherpa-onnx.git"
RAW_HOST = "raw.githubusercontent.com"
GIT_HOST = "github.com"
ORT_CMAKE = {
    "x86_64": "cmake/onnxruntime-linux-x86_64-static.cmake",
    "aarch64": "cmake/onnxruntime-linux-aarch64-static.cmake",
}


def cargo_toml_url(commit: str = WRAPPER) -> str:
    return f"https://{RAW_HOST}/AACTools/rust-tts-wrapper/{commit}/Cargo.toml"


def sherpa_raw(path: str, commit: str = SHERPA) -> str:
    return f"https://{RAW_HOST}/k2-fsa/sherpa-onnx/{commit}/{path}"


def info_refs(remote: str) -> str:
    """What the checker asks over HTTP when `git ls-remote` fails."""
    return f"{remote}/info/refs?service=git-upload-pack"


class Dep(NamedTuple):
    """One dependency tarball of sherpa-onnx, as upstream pins it."""

    key: str  # name, and what the file name starts with
    var: str  # prefix of the cmake variables
    url: str  # where cmake would download it from
    filename: str  # the local file name cmake looks for
    cmake: str  # the cmake file that pins it
    cmake_url: str  # where the checker has to read that file
    where: str  # how the checker names that file in its messages

    @property
    def sha256(self) -> str:
        return sha256(self.key)

    @property
    def transitive(self) -> bool:
        return not self.where.startswith("sherpa-onnx ")

    @property
    def pin(self) -> tuple:
        """What the checker has to read from this dep's cmake file."""
        return (self.var, self.url, self.sha256, self.filename)


def _dep(key, url, filename, *, via=None):
    cmake = f"cmake/{key}.cmake"
    if via is None:
        cmake_url, where = sherpa_raw(cmake), f"sherpa-onnx {SHERPA_TAG} {cmake}"
    else:  # pinned by a dependency: (its repository, its tag)
        repo, tag = via
        cmake_url = f"https://{RAW_HOST}/{repo}/refs/tags/{tag}/{cmake}"
        where = f"{repo.split('/')[1]} {tag} {cmake}"
    return Dep(key, key.replace("-", "_"), url, filename, cmake, cmake_url, where)


# The ten tarballs, with upstream's real names and URL forms (the checksums
# are made up). kissfft and kaldifst are the two pinned one level down.
KISSFFT_COMMIT = sha1("kissfft")
DEPS = [
    _dep(
        "kaldi-native-fbank",
        "https://github.com/csukuangfj/kaldi-native-fbank/archive/refs/tags/v1.22.4.tar.gz",
        "kaldi-native-fbank-1.22.4.tar.gz",
    ),
    _dep(
        "kissfft",
        f"https://github.com/mborgerding/kissfft/archive/{KISSFFT_COMMIT}.zip",
        f"kissfft-{KISSFFT_COMMIT}.zip",
        via=("csukuangfj/kaldi-native-fbank", "v1.22.4"),
    ),
    _dep(
        "kaldi-decoder",
        "https://github.com/k2-fsa/kaldi-decoder/archive/refs/tags/v0.3.0.tar.gz",
        "kaldi-decoder-0.3.0.tar.gz",
    ),
    _dep(
        "kaldifst",
        "https://github.com/k2-fsa/kaldifst/archive/refs/tags/v1.8.0.tar.gz",
        "kaldifst-1.8.0.tar.gz",
        via=("k2-fsa/kaldi-decoder", "v0.3.0"),
    ),
    _dep(
        "openfst",
        "https://github.com/csukuangfj/openfst/archive/refs/tags/v1.8.5-2026-07-09.tar.gz",
        "openfst-1.8.5-2026-07-09.tar.gz",
    ),
    _dep(
        "eigen",
        "https://gitlab.com/libeigen/eigen/-/archive/5.0.1/eigen-5.0.1.tar.gz",
        "eigen-5.0.1.tar.gz",
    ),
    _dep(
        "utf8proc",
        "https://github.com/JuliaStrings/utf8proc/archive/refs/tags/v2.12.0.tar.gz",
        "utf8proc-2.12.0.tar.gz",
    ),
    _dep(
        "simple-sentencepiece",
        "https://github.com/pkufool/simple-sentencepiece/archive/refs/tags/v0.7.tar.gz",
        "simple-sentencepiece-0.7.tar.gz",
    ),
    _dep(
        "json",
        "https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz",
        "json-3.12.0.tar.gz",
    ),
    _dep(
        "hclust-cpp",
        "https://github.com/csukuangfj/hclust-cpp/archive/refs/tags/2026-02-25.tar.gz",
        "hclust-cpp-2026-02-25.tar.gz",
    ),
]
DEP = {dep.key: dep for dep in DEPS}


def cmake_text(dep: Dep, *, url=None, digest=None, filename=None, url2=True) -> str:
    """A cmake file in the layout sherpa-onnx (and its dependencies) use to
    pin a tarball. `url`, `digest` and `filename` default to the dep's."""
    url = dep.url if url is None else url
    digest = dep.sha256 if digest is None else digest
    filename = dep.filename if filename is None else filename
    mirror = (
        f'  set({dep.var}_URL2 "https://hf-mirror.com/csukuangfj/sherpa-onnx-cmake-deps/'
        f'resolve/main/{filename}")\n'
        if url2
        else ""
    )
    return f"""\
function(download_{dep.var})
  include(FetchContent)

  set({dep.var}_URL  "{url}")
{mirror}  set({dep.var}_HASH "SHA256={digest}")

  # If you don't have access to the Internet,
  # please pre-download {dep.key}
  set(possible_file_locations
    $ENV{{HOME}}/Downloads/{filename}
    ${{CMAKE_SOURCE_DIR}}/{filename}
    ${{CMAKE_BINARY_DIR}}/{filename}
    /tmp/{filename}
  )

  foreach(f IN LISTS possible_file_locations)
    if(EXISTS ${{f}})
      set({dep.var}_URL  "${{f}}")
      file(TO_CMAKE_PATH "${{{dep.var}_URL}}" {dep.var}_URL)
      set({dep.var}_URL2)
      break()
    endif()
  endforeach()

  FetchContent_Declare({dep.var}
    URL               ${{{dep.var}_URL}} ${{{dep.var}_URL2}}
    URL_HASH          ${{{dep.var}_HASH}}
  )
endfunction()
"""


def ort_cmake(*versions: str) -> str:
    """A sherpa-onnx cmake file that names the ONNX Runtime release(s) its
    prebuilt static library comes from."""
    lines = [
        f'set(onnxruntime_URL  "https://github.com/csukuangfj/onnxruntime-libs/releases/'
        f'download/v{version}/onnxruntime-linux-x64-static_lib-{version}-glibc2_17.zip")'
        for version in versions
    ]
    return "\n".join(["# onnxruntime, static", *lines, 'set(onnxruntime_HASH "SHA256=0")', ""])


def cargo_toml(version: str = WRAPPER_VERSION) -> str:
    return f'[package]\nname = "rust-tts-wrapper"\nversion = "{version}"\nedition = "2021"\n'


# ── Synthetic packaging files ───────────────────────────────────────────


def git_source(url, *, tag=None, commit=None, dest=None, branch=None) -> dict:
    return {"type": "git", "url": url, "tag": tag, "branch": branch, "commit": commit, "dest": dest}


def file_source(dep: Dep, **changes) -> dict:
    """The manifest source for `dep`, as the real manifest writes it. A
    change to None drops the key."""
    source = {
        "type": "file",
        "url": dep.url,
        "sha256": dep.sha256,
        "dest": None,
        "dest-filename": dep.filename,
    }
    source.update(changes)
    return source


def tarballs(**per_dep) -> list:
    """The ten file sources; `per_dep` maps a dep key (with _ for -) to
    None (leave it out) or to the changes for its source."""
    sources = []
    for dep in DEPS:
        changes = per_dep.get(dep.var, {})
        if changes is not None:
            sources.append(file_source(dep, **changes))
    return sources


def render_source(source) -> str:
    if isinstance(source, str):
        return f"      - {source}\n"
    lines = [f"      - type: {source['type']}"]
    lines += [f"        {k}: {v}" for k, v in source.items() if k != "type" and v is not None]
    return "\n".join(lines) + "\n"


def render_manifest(
    *,
    core=CORE,
    wrapper=WRAPPER,
    sdl_tag=SDL_TAG,
    sdl_commit=SDL,
    sherpa_tag=SHERPA_TAG,
    sherpa_commit=SHERPA,
    sherpa_git=True,
    ort=ORT,
    sherpa_files=None,
    onnxruntime_files=(),
    core_branch=None,
    wrapper_branch=None,
) -> str:
    """A small manifest with the layout of the real one: the same three
    modules that matter, the same indentation and key order. None for a pin
    leaves its line out."""
    ort_check = f'      - test "$(cat VERSION_NUMBER)" = {ort}\n' if ort else ""
    sherpa_sources = []
    if sherpa_git:
        sherpa_sources.append(git_source(SHERPA_GIT, tag=sherpa_tag, commit=sherpa_commit))
    sherpa_sources += tarballs() if sherpa_files is None else sherpa_files
    dasher_sources = [
        {"type": "dir", "path": "../.."},
        git_source(
            "https://github.com/dasher-project/DasherCore.git",
            commit=core,
            branch=core_branch,
            dest="DasherCore",
        ),
        git_source(SDL_GIT, tag=sdl_tag, commit=sdl_commit, dest="Thirdparty/SDL"),
        git_source(
            "https://github.com/AACTools/rust-tts-wrapper.git",
            commit=wrapper,
            branch=wrapper_branch,
            dest="rust-tts-wrapper",
        ),
        {
            "type": "file",
            "path": "rust-tts-wrapper-Cargo.lock",
            "dest": "rust-tts-wrapper",
            "dest-filename": "Cargo.lock",
        },
        "cargo-sources.json",
    ]
    ort_sources = [
        {
            "type": "archive",
            "url": f"https://github.com/microsoft/onnxruntime/archive/{sha1('ort')}.tar.gz",
            "sha256": sha256("onnxruntime"),
        },
        {
            "type": "file",
            "url": "https://github.com/google/re2/archive/refs/tags/2024-07-02.zip",
            "sha256": sha256("re2"),
            "dest": "mirror/github.com/google/re2/archive/refs/tags",
            "dest-filename": "2024-07-02.zip",
        },
        *onnxruntime_files,
    ]
    return (
        "---\n"
        "app-id: org.alternativeinterface.dasher\n"
        "runtime: org.gnome.Platform\n"
        "runtime-version: '51'\n"
        "command: dasher\n"
        "\n"
        "finish-args:\n"
        "  - --share=ipc\n"
        "\n"
        "modules:\n"
        "  # Local speech: ONNX Runtime + sherpa-onnx, built from source\n"
        "  - name: onnxruntime\n"
        "    buildsystem: simple\n"
        "    build-commands:\n"
        "      # The version this module is meant to build.\n"
        f"{ort_check}"
        "      - cmake -S cmake -B _build -G Ninja\n"
        "    sources:\n" + "".join(map(render_source, ort_sources)) + "\n"
        "  - name: sherpa-onnx\n"
        "    buildsystem: cmake-ninja\n"
        "    config-opts:\n"
        "      - -DBUILD_SHARED_LIBS=OFF\n"
        "    sources:\n"
        "      # The tag must be v<version of sherpa-onnx-sys in the cargo lock>.\n"
        + "".join(map(render_source, sherpa_sources))
        + "\n"
        "  # Dasher-GTK (builds DasherCore + SDL3 + TTS + app together)\n"
        "  - name: dasher\n"
        "    buildsystem: simple\n"
        "    build-commands:\n"
        "      - cmake --install _build\n"
        "    sources:\n"
        "      # tag + commit: flatpak-builder verifies the tag still resolves\n"
        "      # to this commit.\n" + "".join(map(render_source, dasher_sources))
    )


def crate(name: str, version: str, **extra) -> dict:
    """A crates.io package as Cargo.lock records it."""
    return {
        "name": name,
        "version": version,
        "source": CRATES_IO,
        "checksum": sha256(f"{name}-{version}"),
        **extra,
    }


def git_crate(name: str, version: str, commit: str) -> dict:
    return {
        "name": name,
        "version": version,
        "source": f"git+https://github.com/example/{name}?rev={commit[:7]}#{commit}",
    }


def packages(*extra, wrapper_version=WRAPPER_VERSION, sherpa_sys="2.0.1") -> list:
    """The packages of a tiny lock: the workspace member, sherpa-onnx-sys
    and two ordinary crates, plus `extra`."""
    result = [crate("anyhow", "1.0.100"), crate("libc", "0.2.180")]
    if wrapper_version:
        result.append(
            {
                "name": "rust-tts-wrapper",
                "version": wrapper_version,
                "dependencies": ["anyhow", "libc", "sherpa-onnx-sys"],
            }
        )
    if sherpa_sys:
        result.append(crate("sherpa-onnx-sys", sherpa_sys))
    return [*result, *extra]


def render_lock(pkgs: list) -> str:
    out = [
        "# This file is automatically @generated by Cargo.",
        "# It is not intended for manual editing.",
        "version = 4",
        "",
    ]
    for pkg in pkgs:
        out.append("[[package]]")
        out += [
            f'{key} = "{pkg[key]}"'
            for key in ("name", "version", "source", "checksum")
            if key in pkg
        ]
        if "dependencies" in pkg:
            out += ["dependencies = [", *(f' "{d}",' for d in pkg["dependencies"]), "]"]
        out.append("")
    return "\n".join(out)


def vendored(pkgs: list) -> list:
    """cargo-sources.json for `pkgs`, in the shape flatpak-cargo-generator
    writes: an archive and an inline checksum file per crates.io crate, a
    git source per git dependency, and the cargo config last."""
    sources = []
    for pkg in pkgs:
        source = pkg.get("source", "")
        ident = f"{pkg['name']}-{pkg['version']}"
        if source == CRATES_IO and "checksum" in pkg:
            sources.append(
                {
                    "type": "archive",
                    "archive-type": "tar-gzip",
                    "url": f"https://static.crates.io/crates/{pkg['name']}/{ident}.crate",
                    "sha256": pkg["checksum"],
                    "dest": f"cargo/vendor/{ident}",
                }
            )
            sources.append(
                {
                    "type": "inline",
                    "contents": json.dumps({"package": pkg["checksum"], "files": {}}),
                    "dest": f"cargo/vendor/{ident}",
                    "dest-filename": ".cargo-checksum.json",
                }
            )
        elif source.startswith("git+"):
            sources.append(
                {
                    "type": "git",
                    "url": source[4:].split("?")[0],
                    "commit": source[-40:],
                    "dest": f"flatpak-cargo/git/{pkg['name']}-{source[-40:][:7]}",
                }
            )
    sources.append(
        {
            "type": "inline",
            "contents": '[source.crates-io]\nreplace-with = "vendored-sources"\n',
            "dest": "cargo",
            "dest-filename": "config",
        }
    )
    return sources


# ── Fake upstream ───────────────────────────────────────────────────────


class FakeResponse:
    def __init__(self, body: bytes):
        self.body = body

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def read(self) -> bytes:
        return self.body


class FakeNet:
    """GitHub, as far as the checker can tell.

    urls:     URL -> text or bytes (HTTP 200), an int (that HTTP status), or
              an exception to raise.
    remotes:  git remote URL -> {tag: commit}, or {tag: (tag object, commit)}
              for an annotated tag, or a string: `git ls-remote` then fails
              with that on stderr.
    down:     hosts that cannot be reached at all, over HTTP or by git.

    A request for anything not listed is a test bug and raises
    AssertionError, which the checker does not catch.
    """

    def __init__(self):
        self.urls = {}
        self.remotes = {}
        self.down = set()
        self.http_calls = []
        self.ls_remote_calls = []
        self.ls_remote_envs = []

    # urllib.request.urlopen
    def urlopen(self, url, timeout=None):
        self.http_calls.append(url)
        if urllib.parse.urlsplit(url).netloc in self.down:
            raise urllib.error.URLError(OSError(101, "Network is unreachable"))
        if url not in self.urls:
            raise AssertionError(f"unexpected HTTP request in a hermetic test: {url}")
        answer = self.urls[url]
        if isinstance(answer, BaseException):
            raise answer
        if isinstance(answer, int):
            reason = http.client.responses[answer]
            error = urllib.error.HTTPError(
                url, answer, reason, email.message.Message(), io.BytesIO()
            )
            error.close()  # the checker reads only the status and the reason
            raise error
        return FakeResponse(answer if isinstance(answer, bytes) else answer.encode())

    # subprocess.run
    def run(self, cmd, *args, **kwargs):
        if list(cmd[:2]) != ["git", "ls-remote"]:
            return REAL_RUN(cmd, *args, **kwargs)
        self.ls_remote_calls.append(list(cmd))
        self.ls_remote_envs.append(kwargs.get("env"))
        remote, wanted = cmd[2], cmd[3:]
        host = urllib.parse.urlsplit(remote).netloc
        if host in self.down:
            stderr = f"fatal: unable to access '{remote}/': Could not resolve host: {host}\n"
            return subprocess.CompletedProcess(cmd, 128, "", stderr)
        if remote not in self.remotes:
            raise AssertionError(f"unexpected git ls-remote in a hermetic test: {remote}")
        answer = self.remotes[remote]
        if isinstance(answer, str):
            return subprocess.CompletedProcess(cmd, 128, "", answer)
        lines = []
        for tag, target in answer.items():
            refs = {f"refs/tags/{tag}": target}
            if isinstance(target, tuple):  # annotated: the tag object, then the commit
                refs = {f"refs/tags/{tag}": target[0], f"refs/tags/{tag}^{{}}": target[1]}
            lines += [f"{sha}\t{ref}\n" for ref, sha in refs.items() if ref in wanted]
        return subprocess.CompletedProcess(cmd, 0, "".join(lines), "")

    def remotes_asked(self) -> list:
        return [call[2] for call in self.ls_remote_calls]


def good_net() -> FakeNet:
    """An upstream that agrees with the default synthetic files."""
    net = FakeNet()
    net.urls[cargo_toml_url()] = cargo_toml()
    for path in ORT_CMAKE.values():
        net.urls[sherpa_raw(path)] = ort_cmake(ORT)
    for dep in DEPS:
        net.urls[dep.cmake_url] = cmake_text(dep)
    net.remotes[SHERPA_GIT] = {SHERPA_TAG: SHERPA}
    net.remotes[SDL_GIT] = {SDL_TAG: SDL}
    return net


# Every URL a full, passing run reads, and the two remotes it asks.
ALL_URLS = [cargo_toml_url(), *map(sherpa_raw, ORT_CMAKE.values()), *(d.cmake_url for d in DEPS)]


# ── Throwaway git repositories ──────────────────────────────────────────


def git_env() -> dict:
    """An environment in which git ignores the user's configuration and
    any repository named by the caller's environment (a hook, say)."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    env.update(
        GIT_CONFIG_GLOBAL=os.devnull,
        GIT_CONFIG_NOSYSTEM="1",
        GIT_AUTHOR_NAME="Pin Sync Test",
        GIT_AUTHOR_EMAIL="pin-sync-test@example.invalid",
        GIT_COMMITTER_NAME="Pin Sync Test",
        GIT_COMMITTER_EMAIL="pin-sync-test@example.invalid",
    )
    return env


def git(repo, *args) -> str:
    result = REAL_RUN(
        ["git", "-C", str(repo), *args], capture_output=True, text=True, env=git_env()
    )
    if result.returncode != 0:
        raise AssertionError(f"git {' '.join(args)} failed: {result.stderr}")
    return result.stdout


def stage_gitlink(repo, path: str, commit: str) -> None:
    """Record `commit` as the submodule commit of `path` in the index: what
    `git add <submodule>` does after a bump, without needing the submodule."""
    git(repo, "update-index", "--add", "--cacheinfo", f"160000,{commit},{path}")


def commit_all(repo, message: str = "test") -> None:
    git(repo, "commit", "-q", "--allow-empty", "-m", message)


def make_repo(path: Path, gitlinks: dict) -> Path:
    path.mkdir(parents=True)
    git(path, "-c", "init.defaultBranch=main", "init", "-q", "--template=")
    for sub, commit in gitlinks.items():
        stage_gitlink(path, sub, commit)
    commit_all(path, "submodules")
    return path


def write_packaging(repo: Path, *, manifest=None, lock=None, lock_commit=WRAPPER, sources=None):
    """Write the four packaging files into `repo`. The defaults are in sync
    with each other and with good_net(). lock_commit=None writes no sidecar."""
    pkgs = packages()
    files = {
        MANIFEST_NAME: render_manifest() if manifest is None else manifest,
        LOCK_NAME: render_lock(pkgs) if lock is None else lock,
        CARGO_SOURCES_NAME: json.dumps(vendored(pkgs) if sources is None else sources, indent=4),
    }
    if lock_commit is not None:
        files[LOCK_COMMIT_NAME] = lock_commit + "\n"
    else:
        (repo / LOCK_COMMIT_NAME).unlink(missing_ok=True)
    for name, text in files.items():
        (repo / name).parent.mkdir(parents=True, exist_ok=True)
        (repo / name).write_text(text)


_template_dir = None
TEMPLATE_REPO = None


def setUpModule():
    """One git repository with the three gitlinks; each test copies it,
    which is much cheaper than `git init` and a commit per test."""
    global _template_dir, TEMPLATE_REPO
    _template_dir = tempfile.TemporaryDirectory(prefix="pin-sync-template-")
    TEMPLATE_REPO = make_repo(Path(_template_dir.name) / "repo", GITLINKS)


def tearDownModule():
    _template_dir.cleanup()


# ── Test base ───────────────────────────────────────────────────────────


class Run:
    """The outcome of one checker run: exit status and stdout."""

    def __init__(self, code: int, text: str):
        self.code = code
        self.text = text
        self.lines = text.splitlines()

    def starting(self, prefix: str) -> list:
        return [line for line in self.lines if line.startswith(prefix)]

    @property
    def errors(self) -> list:
        return self.starting("::error::")

    @property
    def warnings(self) -> list:
        return self.starting("::warning::")

    @property
    def oks(self) -> list:
        return self.starting("ok: ")

    @property
    def skips(self) -> list:
        return [w for w in self.warnings if "SKIPPED, not verified: " in w]

    @property
    def skipped(self) -> list:
        """What each skip says was not verified."""
        return [w.split("SKIPPED, not verified: ", 1)[1].removesuffix(".") for w in self.skips]

    @property
    def incomplete(self) -> list:
        return self.starting("::warning::INCOMPLETE RUN: ")


# Every line the checker prints is one of these. `::error::` and
# `::warning::` are GitHub workflow commands: the annotation is the rest of
# that one line, so a message must never contain a line break.
LINE_PREFIXES = ("ok: ", "hint: ", "fixed: ", "::error::", "::warning::")


def output_problems(text: str) -> list:
    """What is wrong with the form of the checker output `text`, whatever
    was being checked. Empty when it is well formed."""
    problems = []
    if text and not text.endswith("\n"):
        problems.append("output does not end with a newline")
    lines = text.splitlines()
    for number, line in enumerate(lines, 1):
        if not line.startswith(LINE_PREFIXES):
            problems.append(
                f"line {number} is not an ok/hint/fixed line or an annotation (a message "
                f"with a line break in it?): {line!r}"
            )
        elif line.startswith("::") and not re.match(r"::(error|warning)::\S", line):
            problems.append(f"line {number} is an annotation without a message: {line!r}")
        if "\r" in line or "\t" in line:
            problems.append(f"line {number} has a control character: {line!r}")
    summaries = [line for line in lines if line.startswith("::warning::INCOMPLETE RUN: ")]
    if len(summaries) > 1:
        problems.append("more than one INCOMPLETE RUN line")
    if summaries and lines[-1] != summaries[0]:
        problems.append("the INCOMPLETE RUN line is not the last line")
    return problems


class CheckerTestCase(unittest.TestCase):
    """A synthetic repository as the working directory, the fake upstream
    in place of the network, and the checker's run state reset."""

    maxDiff = None
    gitlinks = None  # None: the template's (GITLINKS)

    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="pin-sync-test-")
        self.addCleanup(tmp.cleanup)
        self.repo = Path(tmp.name) / "repo"
        if self.gitlinks is None:
            shutil.copytree(TEMPLATE_REPO, self.repo)
        else:
            make_repo(self.repo, self.gitlinks)
        self.addCleanup(os.chdir, os.getcwd())
        os.chdir(self.repo)
        # The checker's git calls must look at this repository, whatever the
        # caller's environment says.
        environ = mock.patch.dict(os.environ)
        environ.start()
        self.addCleanup(environ.stop)
        for name in [name for name in os.environ if name.startswith("GIT_")]:
            del os.environ[name]

        self.net = good_net()
        for target, name, fake in (
            (urllib.request, "urlopen", lambda *a, **k: self.net.urlopen(*a, **k)),
            (subprocess, "run", lambda *a, **k: self.net.run(*a, **k)),
        ):
            patcher = mock.patch.object(target, name, side_effect=fake)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.write()

    # Fixture files -------------------------------------------------------

    def write(self, **files):
        write_packaging(self.repo, **files)

    def reset(self, **files):
        """A fresh scenario inside one test (for subTest loops): default
        files plus `files`, a new good upstream, nothing remembered."""
        self.net = good_net()
        self.write(**files)

    def manifest(self, **pins):
        self.write(manifest=render_manifest(**pins))

    def lock(self, pkgs: list, sources=None):
        """Write a lock for `pkgs` and the crate list that goes with it
        (or `sources`, to let them disagree)."""
        self.write(lock=render_lock(pkgs), sources=vendored(pkgs) if sources is None else sources)

    def move_gitlink(self, path: str, commit: str, *, committed: bool = True):
        stage_gitlink(self.repo, path, commit)
        if committed:
            commit_all(self.repo, f"bump {path}")

    def read(self, name: str) -> str:
        return (self.repo / name).read_text()

    # Running -------------------------------------------------------------

    def _captured(self, call, *, whole_run: bool) -> Run:
        cfp.SKIPPED.clear()
        cfp.UNREACHABLE.clear()
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = call()
        self.assertEqual(err.getvalue(), "", "the checker reports on stdout only")
        run = Run(int(code), out.getvalue())
        self.assert_well_formed(run, whole_run=whole_run)
        return run

    def check(self, *argv) -> Run:
        """Run main() with `argv`; every run is checked for the invariants
        in assert_well_formed."""
        with mock.patch.object(sys, "argv", [str(SCRIPT), *argv]):
            return self._captured(cfp.main, whole_run=True)

    def speech(self, sherpa_version: str = "2.0.1") -> Run:
        """Run only the from-source speech stack check (no git calls, so
        the loops over the ten tarballs stay fast)."""
        return self._captured(lambda: cfp.check_speech_stack(sherpa_version), whole_run=False)

    def stop_message(self, call, *args) -> str:
        """`call(*args)` has to stop the run with a message (a string as the
        exit code: Python prints it to stderr and exits 1). Returns it."""
        with self.assertRaises(SystemExit) as stop:
            call(*args)
        return str(stop.exception.code)

    # Assertions ----------------------------------------------------------

    def assert_well_formed(self, run: Run, *, whole_run: bool = True):
        """What holds for every run, whatever it checked."""
        self.assertEqual(output_problems(run.text), [], run.text)
        # The exit status and the annotations tell the same story.
        self.assertEqual(run.code, 1 if run.errors else 0, run.text)
        if not whole_run:  # one check function: main() writes the summary
            self.assertEqual(run.incomplete, [])
            return
        # Skipped checks are summed up in the last line, and only then.
        self.assertEqual(bool(run.skips), bool(run.incomplete), run.text)
        if run.incomplete:
            summary = run.incomplete[0]
            self.assertIn(f"INCOMPLETE RUN: {len(run.skips)} check(s) were SKIPPED", summary)
            for what in run.skipped:
                self.assertIn(what, summary)

    def assert_passes(self, run: Run):
        self.assertEqual((run.code, run.errors, run.warnings), (0, [], []), run.text)

    def assert_one_error(self, run: Run, *fragments: str) -> str:
        """Exactly one error, exit 1, the error containing each fragment."""
        self.assertEqual(len(run.errors), 1, run.text)
        self.assertEqual(run.code, 1)
        for fragment in fragments:
            self.assertIn(fragment, run.errors[0])
        return run.errors[0]

    def assert_error(self, run: Run, *fragments: str) -> str:
        """Some error contains every fragment; exit 1."""
        self.assertEqual(run.code, 1, run.text)
        for error in run.errors:
            if all(fragment in error for fragment in fragments):
                return error
        self.fail(f"no error containing {fragments!r} in:\n{run.text}")


# ── The whole check, in sync ────────────────────────────────────────────


class PassingRunTests(CheckerTestCase):
    def test_files_in_sync_pass_with_every_check_reported(self):
        run = self.check()
        self.assert_passes(run)
        lock = "rust-tts-wrapper-Cargo.lock"
        self.assertEqual(
            run.lines,
            [
                f"ok: rust-tts-wrapper manifest pin == submodule ({WRAPPER[:12]})",
                f"ok: DasherCore manifest pin == submodule ({CORE[:12]})",
                f"ok: {lock} was generated for the pinned rust-tts-wrapper ({WRAPPER[:12]})",
                f"ok: {lock} has the pinned rust-tts-wrapper's version (1.16.2)",
                f"ok: cargo-sources.json vendors the 3 crates in {lock}",
                f"ok: sherpa-onnx source is v2.0.1 ({SHERPA[:12]}), matching sherpa-onnx-sys",
                "ok: ONNX Runtime 1.28.2 is what sherpa-onnx v2.0.1 pins (x86_64)",
                "ok: ONNX Runtime 1.28.2 is what sherpa-onnx v2.0.1 pins (aarch64)",
                "ok: the 10 dependency tarballs sherpa-onnx v2.0.1 needs (2 of them pinned by "
                "its dependencies) are sources of the sherpa-onnx module, under the file "
                "names cmake looks for",
                f"ok: SDL manifest tag == submodule ({SDL[:12]})",
            ],
        )

    def test_reads_upstream_at_the_pinned_commits_and_tags(self):
        self.check()
        # Cargo.toml at the pinned rust-tts-wrapper commit, sherpa-onnx's
        # files at the pinned sherpa-onnx commit, and the two tarballs pinned
        # one level down at the tags sherpa-onnx pins for their parents.
        self.assertEqual(sorted(self.net.http_calls), sorted(ALL_URLS))
        for url in (
            "https://raw.githubusercontent.com/csukuangfj/kaldi-native-fbank/refs/tags/v1.22.4/"
            "cmake/kissfft.cmake",
            "https://raw.githubusercontent.com/k2-fsa/kaldi-decoder/refs/tags/v0.3.0/"
            "cmake/kaldifst.cmake",
        ):
            self.assertIn(url, self.net.http_calls)

    def test_asks_each_remote_for_the_tag_and_its_peeled_commit(self):
        self.check()
        self.assertEqual(
            self.net.ls_remote_calls,
            [
                ["git", "ls-remote", SHERPA_GIT, "refs/tags/v2.0.1", "refs/tags/v2.0.1^{}"],
                [
                    "git",
                    "ls-remote",
                    SDL_GIT,
                    "refs/tags/release-3.2.30",
                    "refs/tags/release-3.2.30^{}",
                ],
            ],
        )
        # git must fail instead of asking for a password in CI.
        for env in self.net.ls_remote_envs:
            self.assertEqual(env["GIT_TERMINAL_PROMPT"], "0")
            self.assertEqual(env["PATH"], os.environ["PATH"])


# ── rust-tts-wrapper, DasherCore: manifest commit == submodule gitlink ──


HARD_PINS = {
    # name: (render_manifest keyword, its gitlink)
    "rust-tts-wrapper": ("wrapper", WRAPPER),
    "DasherCore": ("core", CORE),
}


class HardPinTests(CheckerTestCase):
    def test_manifest_commit_that_is_not_the_submodule_commit_fails(self):
        for name, (keyword, gitlink) in HARD_PINS.items():
            for commit in (OTHER, near(gitlink)):
                with self.subTest(name, commit=commit):
                    self.reset(manifest=render_manifest(**{keyword: commit}))
                    error = self.assert_one_error(
                        self.check(),
                        f"Flatpak manifest pins {name} at {commit[:12]} but the submodule is at "
                        f"{gitlink[:12]}",
                        "Scripts/check-flatpak-pins.py --fix",
                    )
                    self.assertNotIn(commit[:13], error, "SHAs are abbreviated to 12 characters")

    def test_submodule_bumped_without_the_manifest_fails(self):
        self.move_gitlink("DasherCore", OTHER)
        self.assert_one_error(
            self.check(),
            f"Flatpak manifest pins DasherCore at {CORE[:12]} but the submodule is at {OTHER[:12]}",
        )

    def test_rust_tts_wrapper_bumped_alone_fails_on_the_pin_and_on_the_lock(self):
        self.move_gitlink("rust-tts-wrapper", OTHER)
        self.net.urls[cargo_toml_url(OTHER)] = cargo_toml()
        run = self.check()
        self.assertEqual(len(run.errors), 2, run.text)
        self.assert_error(run, f"pins rust-tts-wrapper at {WRAPPER[:12]}")
        self.assert_error(
            run,
            f"was generated for rust-tts-wrapper {WRAPPER[:12]} but the submodule is at "
            f"{OTHER[:12]}",
        )
        # The version is read at the submodule's commit, not the manifest's.
        self.assertIn(cargo_toml_url(OTHER), self.net.http_calls)
        self.assertNotIn(cargo_toml_url(WRAPPER), self.net.http_calls)

    def test_source_without_a_commit_line_fails(self):
        for name, (keyword, _) in HARD_PINS.items():
            cases = {"no commit": {keyword: None}, "a branch": {f"{keyword}_branch": "main"}}
            for label, pins in cases.items():
                with self.subTest(name, source=label):
                    self.reset(manifest=render_manifest(**{keyword: None, **pins}))
                    self.assert_one_error(
                        self.check(),
                        f"No commit: pin found for the {name} git source in {MANIFEST_NAME}",
                        "would clone a moving branch",
                        "--fix",
                    )

    def test_missing_git_source_fails(self):
        text = render_manifest().replace(
            "https://github.com/dasher-project/DasherCore.git",
            "https://github.com/dasher-project/SomethingElse.git",
        )
        self.write(manifest=text)
        self.assert_one_error(self.check(), "No commit: pin found for the DasherCore git source")

    def test_path_that_is_not_a_submodule_stops_the_run(self):
        git(self.repo, "rm", "-q", "--cached", "DasherCore")
        commit_all(self.repo, "DasherCore is gone")
        self.assertRegex(
            self.stop_message(self.check), r"^error: DasherCore is not a submodule at HEAD"
        )


# ── The lock's recorded commit (rust-tts-wrapper-Cargo.lock.commit) ─────


class LockCommitTests(CheckerTestCase):
    def test_surrounding_whitespace_is_ignored(self):
        self.write(lock_commit=f"  {WRAPPER}\n")
        self.assert_passes(self.check())

    def test_lock_generated_for_another_commit_fails(self):
        for recorded in (OTHER, near(WRAPPER)):
            with self.subTest(recorded):
                self.reset(lock_commit=recorded)
                self.assert_one_error(
                    self.check(),
                    f"rust-tts-wrapper-Cargo.lock was generated for rust-tts-wrapper "
                    f"{recorded[:12]} but the submodule is at {WRAPPER[:12]}",
                    "Regenerate packaging/flatpak/rust-tts-wrapper-Cargo.lock, "
                    "rust-tts-wrapper-Cargo.lock.commit and cargo-sources.json",
                )

    def test_missing_sidecar_fails(self):
        self.write(lock_commit=None)
        self.assertFalse((self.repo / LOCK_COMMIT_NAME).exists())
        self.assert_one_error(
            self.check(),
            f"{LOCK_COMMIT_NAME} does not hold a commit SHA",
            "Regenerate",
        )

    def test_sidecar_that_is_not_one_full_sha_fails(self):
        cases = {
            "empty": "",
            "abbreviated": WRAPPER[:12],
            "one short": WRAPPER[:39],
            "one long": WRAPPER + "0",
            "upper case": WRAPPER.upper(),
            "a tag": "v1.16.2",
            "two lines": f"{WRAPPER}\n{WRAPPER}",
            "with a comment": f"{WRAPPER} # v1.16.2",
        }
        for label, content in cases.items():
            with self.subTest(label):
                self.reset(lock_commit=content)
                self.assert_one_error(self.check(), "does not hold a commit SHA")


# ── The lock's crate version against Cargo.toml at the pinned commit ────


class LockVersionTests(CheckerTestCase):
    def test_version_that_differs_from_the_pinned_commit_fails(self):
        self.net.urls[cargo_toml_url()] = cargo_toml("1.17.0")
        self.assert_one_error(
            self.check(),
            "rust-tts-wrapper-Cargo.lock was generated for rust-tts-wrapper 1.16.2 but the "
            "pinned commit is 1.17.0",
            "Regenerate",
        )

    def test_lock_without_the_wrapper_package_fails_once(self):
        self.lock(packages(wrapper_version=None))
        self.assert_one_error(
            self.check(),
            "rust-tts-wrapper-Cargo.lock has no rust-tts-wrapper package",
            "Regenerate",
        )

    def test_commit_that_is_not_upstream_fails(self):
        self.net.urls[cargo_toml_url()] = 404
        self.assert_one_error(
            self.check(),
            f"rust-tts-wrapper's Cargo.toml at {WRAPPER[:12]} is not readable upstream "
            f"(HTTP 404 Not Found for {cargo_toml_url()})",
            "Is the commit pushed to AACTools/rust-tts-wrapper?",
        )

    def test_unreachable_upstream_is_a_skip_not_a_failure(self):
        self.net.down.add(RAW_HOST)
        run = self.check()
        self.assertEqual(run.code, 0)
        self.assertEqual(
            run.skips[0],
            f"::warning::Could not reach {cargo_toml_url()} (<urlopen error [Errno 101] Network "
            "is unreachable>). SKIPPED, not verified: that rust-tts-wrapper-Cargo.lock has the "
            f"crate version of rust-tts-wrapper {WRAPPER[:12]}, and that the commit exists "
            "upstream.",
        )
        self.assertNotIn("has the pinned rust-tts-wrapper's version", run.text)


# ── cargo-sources.json against the lock ─────────────────────────────────


MISMATCH = "cargo-sources.json does not match rust-tts-wrapper-Cargo.lock"


class CargoSourcesTests(CheckerTestCase):
    def counts(self, crates_missing=0, crates_stale=0, git_missing=0, git_stale=0) -> str:
        return (
            f"({crates_missing} crates missing, {crates_stale} stale; "
            f"{git_missing} git dependencies missing, {git_stale} stale)"
        )

    def test_git_dependency_vendored_at_its_commit_passes(self):
        self.lock(packages(git_crate("espeak-rs", "0.1.0", OTHER)))
        run = self.check()
        self.assert_passes(run)
        self.assertIn(
            "ok: cargo-sources.json vendors the 3 crates and 1 git dependencies in "
            "rust-tts-wrapper-Cargo.lock",
            run.lines,
        )

    def test_path_dependency_and_workspace_member_need_no_source(self):
        pkgs = packages({"name": "local-helper", "version": "0.1.0"})
        self.lock(pkgs)
        run = self.check()
        self.assert_passes(run)
        self.assertIn("vendors the 3 crates in", run.text)

    def test_sources_that_differ_from_the_lock_fail(self):
        pkgs, zeroize = packages(), crate("zeroize", "1.8.2")
        older, rebuilt = crate("zeroize", "1.8.1"), crate("zeroize", "1.8.2", checksum=sha256("x"))
        espeak, moved = (git_crate("espeak-rs", "0.1.0", c) for c in (OTHER, sha1("older")))
        almost = git_crate("espeak-rs", "0.1.0", near(OTHER))
        # label: (the lock's packages, the vendored ones,
        #         crates missing, crates stale, git dependencies missing, stale)
        cases = {
            "crate missing from the sources": (pkgs + [zeroize], pkgs, 1, 0, 0, 0),
            "crate the lock does not name": (pkgs, pkgs + [zeroize], 0, 1, 0, 0),
            "crate at another version": (pkgs + [zeroize], pkgs + [older], 1, 1, 0, 0),
            "crate with another checksum": (pkgs + [zeroize], pkgs + [rebuilt], 1, 1, 0, 0),
            "git dependency missing from the sources": (pkgs + [espeak], pkgs, 0, 0, 1, 0),
            "git dependency at another commit": (pkgs + [espeak], pkgs + [moved], 0, 0, 1, 1),
            "git dependency at a commit one off": (pkgs + [espeak], pkgs + [almost], 0, 0, 1, 1),
            "git dependency the lock does not name": (pkgs, pkgs + [espeak], 0, 0, 0, 1),
        }
        for label, (locked, shipped, *counts) in cases.items():
            with self.subTest(label):
                self.reset()
                self.lock(locked, sources=vendored(shipped))
                self.assert_one_error(self.check(), MISMATCH, self.counts(*counts), "Regenerate")

    def test_empty_sources_fail(self):
        self.lock(packages(), sources=[])
        self.assert_one_error(self.check(), MISMATCH, self.counts(crates_missing=3))

    def test_package_from_another_registry_cannot_be_verified(self):
        other = {
            "name": "private-crate",
            "version": "0.3.0",
            "source": "registry+https://example.invalid/index",
            "checksum": sha256("private-crate"),
        }
        self.lock(packages(other))
        self.assert_one_error(
            self.check(),
            "rust-tts-wrapper-Cargo.lock has packages this check cannot match against "
            "cargo-sources.json",
            "private-crate 0.3.0 (registry+https://example.invalid/index)",
        )

    def test_crates_io_package_without_a_checksum_cannot_be_verified(self):
        pkg = crate("zeroize", "1.8.2")
        del pkg["checksum"]
        self.lock(packages(pkg))
        self.assert_one_error(self.check(), "cannot match", f"zeroize 1.8.2 ({CRATES_IO})")

    def test_git_dependency_without_a_commit_cannot_be_verified(self):
        pkg = {
            "name": "espeak-rs",
            "version": "0.1.0",
            "source": "git+https://github.com/example/espeak-rs?branch=main",
        }
        self.lock(packages(pkg), sources=vendored(packages()))
        self.assert_one_error(self.check(), "cannot match", "espeak-rs 0.1.0 (git+https://")

    def test_all_unverifiable_packages_are_named_in_one_error(self):
        first = {"name": "a", "version": "1.0.0", "source": "sparse+https://example.invalid/"}
        second = {"name": "b", "version": "2.0.0", "source": "sparse+https://example.invalid/"}
        self.lock(packages(first, second))
        error = self.assert_one_error(self.check(), "cannot match")
        self.assertIn("a 1.0.0 (sparse+https://example.invalid/), b 2.0.0 (sparse+", error)


# ── sherpa-onnx source against the locked sherpa-onnx-sys ───────────────


NO_SHERPA_PIN = f"{MANIFEST_NAME} has no k2-fsa/sherpa-onnx git source with both tag: and commit:"


class SherpaPinTests(CheckerTestCase):
    def test_lock_without_sherpa_onnx_sys_skips_the_speech_stack(self):
        self.lock(packages(sherpa_sys=None))
        run = self.check()
        self.assert_passes(run)
        self.assertNotIn("sherpa-onnx", run.text)
        self.assertNotIn("ONNX Runtime", run.text)
        self.assertEqual(self.net.http_calls, [cargo_toml_url()])
        self.assertEqual(self.net.remotes_asked(), [SDL_GIT])

    def test_manifest_tag_that_is_not_the_locked_version_fails(self):
        self.manifest(sherpa_tag="v2.0.0")
        self.net.remotes[SHERPA_GIT] = {"v2.0.0": SHERPA}
        run = self.check()
        self.assert_one_error(
            run,
            "rust-tts-wrapper-Cargo.lock locks sherpa-onnx-sys 2.0.1 but the manifest builds "
            "sherpa-onnx v2.0.0",
            "pin tag v2.0.1 with its commit",
        )
        self.assertNotIn("ok: sherpa-onnx source is", run.text)

    def test_lock_that_moved_sherpa_onnx_sys_fails(self):
        self.lock(packages(sherpa_sys="2.1.0"))
        self.assert_one_error(
            self.check(),
            "locks sherpa-onnx-sys 2.1.0 but the manifest builds sherpa-onnx v2.0.1",
            "pin tag v2.1.0",
        )

    def test_source_without_tag_or_commit_fails_before_any_lookup(self):
        cases = {
            "no tag": {"sherpa_tag": None},
            "no commit": {"sherpa_commit": None},
            "abbreviated commit": {"sherpa_commit": SHERPA[:12]},
            "commit and a second word": {"sherpa_commit": f"{SHERPA} {SHERPA_TAG}"},
            "no git source": {"sherpa_git": False},
        }
        for label, pins in cases.items():
            with self.subTest(label):
                self.reset(manifest=render_manifest(**pins))
                self.assert_one_error(self.check(), NO_SHERPA_PIN, "pin tag v2.0.1 and its commit")
                self.assertNotIn(SHERPA_GIT, self.net.remotes_asked())
                self.assertEqual(self.net.http_calls, [cargo_toml_url()])

    def test_tag_and_commit_that_disagree_upstream_fail_and_stop_the_speech_checks(self):
        for upstream in (OTHER, near(SHERPA)):
            with self.subTest(upstream):
                self.reset()
                self.net.remotes[SHERPA_GIT] = {SHERPA_TAG: upstream}
                run = self.check()
                self.assert_one_error(
                    run,
                    f"Manifest pins sherpa-onnx tag v2.0.1 with commit {SHERPA} but the tag "
                    f"resolves to {upstream} upstream",
                    "flatpak-builder refuses that",
                )
                # Nothing is read at a commit that is not the tag's.
                self.assertEqual(self.net.http_calls, [cargo_toml_url()])
                # The rest of the run goes on.
                self.assertIn(f"ok: SDL manifest tag == submodule ({SDL[:12]})", run.lines)

    def test_tag_that_does_not_exist_upstream_fails(self):
        self.net.remotes[SHERPA_GIT] = {}
        self.assert_one_error(
            self.check(),
            f"sherpa-onnx tag v2.0.1 pinned in the manifest does not resolve upstream "
            f"({SHERPA_GIT} has no tag v2.0.1)",
        )

    def test_annotated_tag_is_peeled_to_its_commit(self):
        self.net.remotes[SHERPA_GIT] = {SHERPA_TAG: (sha1("tag object"), SHERPA)}
        self.assert_passes(self.check())

    def test_remote_that_answers_with_an_http_error_fails(self):
        self.net.remotes[SHERPA_GIT] = "fatal: repository not found\n"
        self.net.urls[info_refs(SHERPA_GIT)] = 404
        self.assert_one_error(
            self.check(),
            f"sherpa-onnx tag v2.0.1 pinned in the manifest does not resolve upstream "
            f"({SHERPA_GIT} answered HTTP 404 Not Found)",
        )


# ── ONNX Runtime version against what sherpa-onnx pins ──────────────────


class OnnxRuntimeVersionTests(CheckerTestCase):
    def test_manifest_that_does_not_state_the_version_fails(self):
        self.manifest(ort=None)
        run = self.check()
        self.assert_one_error(
            run,
            f"{MANIFEST_NAME} does not state the ONNX Runtime version it builds",
            '`test "$(cat VERSION_NUMBER)" = <version>` in the onnxruntime module',
        )
        for path in ORT_CMAKE.values():
            self.assertNotIn(sherpa_raw(path), self.net.http_calls)
        # The tarballs are still checked.
        self.assertIn("ok: the 10 dependency tarballs", run.text)

    def test_cmake_file_that_does_not_pin_the_manifests_version_fails_per_arch(self):
        against = "sherpa-onnx v2.0.1 is built against ONNX Runtime"
        for arch, path in ORT_CMAKE.items():
            builds = f"on {arch} but the manifest builds 1.28.2. Move the onnxruntime module"
            # label: (what upstream answers for the cmake file, the one error)
            cases = {
                "another version": (ort_cmake("1.27.0"), f"{against} 1.27.0 {builds}"),
                "two versions": (
                    ort_cmake("1.28.2", "1.27.0"),
                    f"{against} 1.27.0, 1.28.2 {builds}",
                ),
                "no version": ("# moved to another layout\n", f"{against} (not found) {builds}"),
                "gone": (
                    404,
                    f"{path} is not readable at sherpa-onnx {SHERPA[:12]} (HTTP 404 Not Found)",
                ),
            }
            for label, (answer, error) in cases.items():
                with self.subTest(arch, cmake_file=label):
                    self.reset()
                    self.net.urls[sherpa_raw(path)] = answer
                    run = self.check()
                    self.assert_one_error(run, error)
                    # The other arch is still compared, and passes.
                    self.assertEqual(len(run.starting("ok: ONNX Runtime 1.28.2 is what")), 1)

    def test_manifest_version_bumped_alone_fails_on_both_arches(self):
        self.manifest(ort="1.29.0")
        run = self.check()
        self.assertEqual(len(run.errors), 2, run.text)
        for arch in ORT_CMAKE:
            self.assert_error(run, f"ONNX Runtime 1.28.2 on {arch} but the manifest builds 1.29.0")

    def test_value_that_is_not_a_dotted_version_is_not_taken_for_one(self):
        self.manifest(ort="28")
        self.assert_one_error(self.check(), "does not state the ONNX Runtime version it builds")

    def test_only_a_build_command_of_the_onnxruntime_module_states_the_version(self):
        stated = f'test "$(cat VERSION_NUMBER)" = {ORT}'
        text = render_manifest()
        self.assertEqual(text.count(f"      - {stated}\n"), 1)
        cases = {
            "commented out": text.replace(f"      - {stated}\n", f"      # - {stated}\n"),
            "in a comment and in no command": text.replace(
                f"      - {stated}\n", f"      - true  # {stated}\n"
            ),
            "in another module": render_manifest(ort=None).replace(
                "      - cmake --install _build\n", f"      - {stated}\n"
            ),
            "in another key of the module": render_manifest(ort=None).replace(
                "    buildsystem: simple\n    build-commands:\n      # The version",
                f"    x-version: {stated}\n    buildsystem: simple\n    build-commands:\n"
                "      # The version",
            ),
            "no build-commands": text.replace(
                "    build-commands:\n      # The version this module is meant to build.\n"
                f"      - {stated}\n      - cmake -S cmake -B _build -G Ninja\n",
                "",
            ),
            "build-commands that are a mapping": text.replace(
                "    build-commands:\n      # The version this module is meant to build.\n"
                f"      - {stated}\n      - cmake -S cmake -B _build -G Ninja\n",
                f"    build-commands:\n      check: {stated}\n",
            ),
            "build-commands that are not a list": text.replace(
                "    build-commands:\n      # The version this module is meant to build.\n"
                f"      - {stated}\n      - cmake -S cmake -B _build -G Ninja\n",
                f"    build-commands: {stated}\n",
            ),
        }
        for label, manifest in cases.items():
            with self.subTest(label):
                self.assertNotEqual(manifest, text)
                self.reset(manifest=manifest)
                self.assertIsNone(cfp.onnxruntime_version())
                self.assert_error(self.check(), "does not state the ONNX Runtime version it builds")

    def test_the_version_is_read_from_any_form_of_the_command(self):
        stated = f'test "$(cat VERSION_NUMBER)" = {ORT}'
        text = render_manifest()
        for label, written in (
            ("single quotes", f"      - '{stated}'\n"),
            ("a block scalar", f"      - |\n        set -e\n        {stated}\n"),
            ("after a mapping in the list", f"      - {{x: y}}\n      - {stated}\n"),
        ):
            with self.subTest(label):
                self.reset(manifest=text.replace(f"      - {stated}\n", written))
                self.assertEqual(cfp.onnxruntime_version(), ORT)
                self.assert_passes(self.check())

    def test_two_part_version_is_read(self):
        self.manifest(ort="1.29")
        for path in ORT_CMAKE.values():
            self.net.urls[sherpa_raw(path)] = ort_cmake("1.29")
        run = self.check()
        self.assert_passes(run)
        self.assertIn("ok: ONNX Runtime 1.29 is what sherpa-onnx v2.0.1 pins (x86_64)", run.lines)


# ── The ten dependency tarballs of the sherpa-onnx module ───────────────


NOT_SOURCES = (
    "sherpa-onnx v2.0.1 needs dependency tarballs that are not sources of the sherpa-onnx "
    f"module in {MANIFEST_NAME}: "
)
MISPLACED = (
    f"Dependency tarballs of the sherpa-onnx module in {MANIFEST_NAME} are not where cmake "
    "looks for them: "
)
TARBALLS_OK = "ok: the 10 dependency tarballs sherpa-onnx v2.0.1 needs (2 of them pinned by its"


class DependencyTarballTests(CheckerTestCase):
    """Each failure is tried on each of the ten tarballs, the two pinned one
    level down (kissfft, kaldifst) included."""

    def with_sources(self, **per_dep) -> Run:
        self.reset(manifest=render_manifest(sherpa_files=tarballs(**per_dep)))
        return self.speech()

    def missing(self, dep: Dep, digest=None) -> str:
        return f"{dep.var} ({dep.where}, sha256 {(digest or dep.sha256)[:12]}...)"

    def misplaced(self, dep: Dep, actual: str) -> str:
        return f"{dep.var}: is '{actual}', {dep.where} looks for '{dep.filename}'"

    def test_there_are_ten_and_two_are_pinned_by_dependencies(self):
        self.assertEqual(len(DEPS), 10)
        self.assertEqual([d.key for d in DEPS if d.transitive], ["kissfft", "kaldifst"])
        # Same list as the checker's: eight cmake files of sherpa-onnx, two more below.
        self.assertEqual([d.cmake for d in DEPS if not d.transitive], cfp.SHERPA_DEP_CMAKE)
        self.assertEqual(
            {
                DEP["kaldi-native-fbank"].cmake: [DEP["kissfft"].cmake],
                DEP["kaldi-decoder"].cmake: [DEP["kaldifst"].cmake],
            },
            cfp.SHERPA_TRANSITIVE_DEP_CMAKE,
        )

    def test_all_ten_in_place_pass(self):
        run = self.speech()
        self.assert_passes(run)
        self.assertTrue(run.lines[-1].startswith(TARBALLS_OK), run.text)
        self.assertEqual(sorted(self.net.http_calls), sorted(ALL_URLS[1:]))

    def test_source_that_is_missing_or_not_where_cmake_looks_fails(self):
        # label: (the changes to the dep's source, or None to leave it out;
        #         what the error says the source is, or None when cmake would
        #         find no file with the pinned checksum at all)
        cases = {
            "no source": (None, None),
            "another sha256": ({"sha256": sha256("another file")}, None),
            "wrong dest-filename": ({"dest-filename": "renamed.tar.gz"}, "renamed.tar.gz"),
            "with a dest": ({"dest": "third_party/downloads"}, "third_party/downloads/{name}"),
            # An archive source is unpacked; cmake wants the tarball itself.
            "another type": ({"type": "archive"}, "type: archive"),
        }
        for dep in DEPS:
            for label, (changes, actual) in cases.items():
                with self.subTest(dep.key, source=label):
                    run = self.with_sources(**{dep.var: changes})
                    if actual is None:
                        error = NOT_SOURCES + self.missing(dep) + "."
                        remedy = "add them next to the sherpa-onnx source"
                    else:
                        actual = actual.format(name=dep.filename)
                        error = MISPLACED + self.misplaced(dep, actual) + "."
                        remedy = "`type: file` source with that `dest-filename:` and no `dest:`"
                    self.assert_one_error(run, error, remedy)
                    self.assertNotIn(TARBALLS_OK, run.text)

    def test_a_source_that_is_not_written_as_expected_is_not_taken_for_a_good_one(self):
        """A source with no `type:`, or with a `dest:` that is not a
        scalar, is not what flatpak-builder puts where cmake looks. Neither
        is read as if the key were not there."""
        dep = DEP["kaldi-decoder"]
        written = f"url: '{dep.url}', sha256: {dep.sha256}, dest-filename: {dep.filename}"
        for label, flow, actual in (
            ("no type", f"{{{written}}}", "type: (none)"),
            ("a type that is a list", f"{{type: [file], {written}}}", "type: ['file']"),
            (
                "a dest that is a list",
                f"{{type: file, dest: [mirror], {written}}}",
                f"['mirror']/{dep.filename}",
            ),
            (
                "a dest that is a mapping",
                f"{{type: file, dest: {{to: mirror}}, {written}}}",
                f"{{'to': 'mirror'}}/{dep.filename}",
            ),
        ):
            with self.subTest(label):
                sources = [flow if d is dep else file_source(d) for d in DEPS]
                self.reset(manifest=render_manifest(sherpa_files=sources))
                self.assert_one_error(self.speech(), MISPLACED + self.misplaced(dep, actual) + ".")

    def test_cmake_file_that_changed_upstream_fails(self):
        digest = sha256("the next release")
        for dep in DEPS:
            text = cmake_text(dep)
            # label: (what upstream answers for the cmake file, the one error)
            cases = {
                "pins another sha256": (
                    cmake_text(dep, digest=digest),
                    [NOT_SOURCES + self.missing(dep, digest) + "."],
                ),
                "is gone": (
                    404,
                    [f"{dep.where} is not readable (HTTP 404 Not Found for {dep.cmake_url})."],
                ),
                "has no sha256 pin": (
                    text.replace("SHA256=", "MD5="),
                    [
                        f"No SHA256-pinned tarball found in {dep.where}.",
                        "Scripts/check-flatpak-pins.py no longer understands it",
                    ],
                ),
                "has no local file name": (
                    text.replace("${CMAKE_SOURCE_DIR}/", "${PROJECT_SOURCE_DIR}/"),
                    [
                        f"Cannot tell which file name cmake expects for {dep.var} ({dep.where}).",
                        "no longer understands it",
                    ],
                ),
            }
            for label, (answer, fragments) in cases.items():
                with self.subTest(dep.key, cmake_file=label):
                    self.reset()
                    self.net.urls[dep.cmake_url] = answer
                    run = self.speech()
                    self.assert_one_error(run, *fragments)
                    self.assertNotIn(TARBALLS_OK, run.text)

    def test_without_dest_filename_the_url_basename_has_to_be_the_name(self):
        for dep in DEPS:
            basename = dep.url.rsplit("/", 1)[1]
            with self.subTest(dep.key, basename=basename):
                run = self.with_sources(**{dep.var: {"dest-filename": None}})
                if basename == dep.filename:  # eigen: the URL ends in the name cmake wants
                    self.assert_passes(run)
                else:
                    self.assert_one_error(run, MISPLACED + self.misplaced(dep, basename) + ".")
        # Both outcomes were seen.
        self.assertEqual([d.key for d in DEPS if d.url.rsplit("/", 1)[1] == d.filename], ["eigen"])

    def test_source_in_another_module_fails(self):
        for dep in DEPS:
            with self.subTest(dep.key):
                self.reset(
                    manifest=render_manifest(
                        sherpa_files=tarballs(**{dep.var: None}),
                        onnxruntime_files=[file_source(dep)],
                    )
                )
                self.assertIn(dep.sha256, self.read(MANIFEST_NAME))
                self.assert_one_error(self.speech(), NOT_SOURCES + self.missing(dep) + ".")

    def test_every_missing_and_every_misplaced_tarball_is_named(self):
        run = self.with_sources(
            kissfft=None,
            json=None,
            eigen={"dest": "deps"},
            kaldifst={"dest-filename": "kaldifst.tar.gz"},
        )
        self.assertEqual(len(run.errors), 2, run.text)
        self.assert_error(
            run, NOT_SOURCES + f"{self.missing(DEP['kissfft'])}, {self.missing(DEP['json'])}."
        )
        self.assert_error(
            run,
            MISPLACED + self.misplaced(DEP["kaldifst"], "kaldifst.tar.gz") + "; "
            f"eigen: is 'deps/eigen-5.0.1.tar.gz', {DEP['eigen'].where} looks for "
            "'eigen-5.0.1.tar.gz'.",
        )

    def test_no_sources_at_all_names_all_ten(self):
        run = self.with_sources(**{dep.var: None for dep in DEPS})
        error = self.assert_one_error(run, NOT_SOURCES)
        for dep in DEPS:
            self.assertIn(self.missing(dep), error)

    def test_dependency_pinned_by_something_other_than_a_github_tag_fails(self):
        parents = {"kaldi-native-fbank": "kissfft", "kaldi-decoder": "kaldifst"}
        for parent_key, child_key in parents.items():
            parent, child = DEP[parent_key], DEP[child_key]
            url = f"https://example.invalid/mirror/{parent.filename}"
            with self.subTest(parent_key):
                self.reset()
                self.net.urls[parent.cmake_url] = cmake_text(parent, url=url)
                run = self.speech()
                self.assert_one_error(
                    run,
                    f"Cannot tell which release of {parent.var} sherpa-onnx v2.0.1 pins "
                    f"({parent.cmake}: {url}), so the tarball its {child.cmake} pins was not "
                    "checked.",
                    "Teach Scripts/check-flatpak-pins.py the new URL form",
                )
                self.assertNotIn(child.cmake_url, self.net.http_calls)
                self.assertNotIn(TARBALLS_OK, run.text)

    def test_dependency_without_a_url_fails(self):
        parent = DEP["kaldi-decoder"]
        self.net.urls[parent.cmake_url] = cmake_text(parent).replace(
            f'set({parent.var}_URL  "https://', f'set({parent.var}_URL  "git://'
        )
        self.assert_one_error(
            self.speech(),
            f"Cannot tell which release of {parent.var} sherpa-onnx v2.0.1 pins "
            f"({parent.cmake}: no URL found)",
        )

    def test_dependency_pinned_as_a_zip_of_a_tag_is_followed(self):
        parent, child = DEP["kaldi-native-fbank"], DEP["kissfft"]
        url = "https://github.com/csukuangfj/kaldi-native-fbank/archive/refs/tags/v1.23.0.zip"
        self.net.urls[parent.cmake_url] = cmake_text(parent, url=url)
        del self.net.urls[child.cmake_url]
        moved = f"https://{RAW_HOST}/csukuangfj/kaldi-native-fbank/refs/tags/v1.23.0/{child.cmake}"
        self.net.urls[moved] = cmake_text(child)
        self.assert_passes(self.speech())
        self.assertIn(moved, self.net.http_calls)

    def test_second_tarball_in_one_cmake_file_is_required_too(self):
        dep = DEP["openfst"]
        extra = Dep("extra", "extra", "", "extra-1.0.tar.gz", "", "", dep.where)
        self.net.urls[dep.cmake_url] = cmake_text(dep) + cmake_text(extra)
        self.assert_one_error(self.speech(), NOT_SOURCES + self.missing(extra) + ".")

    def test_a_dependency_cmake_file_that_cannot_be_reached_is_no_pass(self):
        self.net.urls[DEP["json"].cmake_url] = TimeoutError("timed out")
        run = self.speech()
        self.assertEqual(run.code, 0)
        self.assertEqual(len(run.skips), 2, run.text)  # json, and hclust-cpp after it
        self.assertIn(
            "SKIPPED, not verified: the dependency tarball pinned in sherpa-onnx v2.0.1 "
            "cmake/json.cmake.",
            run.skips[0],
        )
        self.assertNotIn(TARBALLS_OK, run.text)


# ── SDL: tag and commit ─────────────────────────────────────────────────


NO_SDL_COMMIT = "No commit: pin (a full 40-character SHA) found "
SDL_OK = f"ok: SDL manifest tag == submodule ({SDL[:12]})"


class SdlTests(CheckerTestCase):
    def test_missing_tag_fails_without_a_lookup(self):
        self.manifest(sdl_tag=None)
        self.assert_one_error(
            self.check(), "No tag: pin found for the SDL git source", "pin a release tag"
        )
        self.assertNotIn(SDL_GIT, self.net.remotes_asked())

    def test_missing_commit_fails_and_names_the_commit_to_add(self):
        self.manifest(sdl_commit=None)
        run = self.check()
        self.assert_one_error(
            run,
            NO_SDL_COMMIT + f"next to the SDL tag release-3.2.30 in {MANIFEST_NAME}.",
            "so a moved tag cannot change the build",
        )
        self.assertEqual(
            run.starting("hint: "),
            [f"hint: SDL tag release-3.2.30 resolves to {SDL} upstream: add `commit: {SDL}`."],
        )

    def test_missing_tag_and_commit_are_two_errors(self):
        self.manifest(sdl_tag=None, sdl_commit=None)
        run = self.check()
        self.assertEqual(len(run.errors), 2, run.text)
        self.assert_error(run, "No tag: pin found for the SDL git source")
        self.assert_error(run, NO_SDL_COMMIT + f"for the SDL git source in {MANIFEST_NAME}.")
        self.assertEqual(run.starting("hint: "), [])

    def test_commit_that_is_not_the_tags_fails(self):
        for commit in (OTHER, near(SDL)):
            with self.subTest(commit):
                self.reset(manifest=render_manifest(sdl_commit=commit))
                self.assert_one_error(
                    self.check(),
                    f"Manifest pins SDL tag release-3.2.30 with commit {commit[:12]} but the tag "
                    f"resolves to {SDL[:12]} upstream",
                    "flatpak-builder refuses that; update the commit together with the tag",
                )

    def test_tag_moved_upstream_fails(self):
        self.net.remotes[SDL_GIT] = {SDL_TAG: OTHER}
        self.assert_one_error(
            self.check(),
            f"Manifest pins SDL tag release-3.2.30 with commit {SDL[:12]} but the tag resolves "
            f"to {OTHER[:12]} upstream",
        )

    def test_tag_that_does_not_exist_upstream_fails(self):
        self.manifest(sdl_tag="release-3.2.999")
        self.net.remotes[SDL_GIT] = {SDL_TAG: SDL}
        self.assert_one_error(
            self.check(),
            "SDL tag release-3.2.999 pinned in the manifest does not resolve upstream "
            f"({SDL_GIT} has no tag release-3.2.999).",
        )

    def test_annotated_tag_is_peeled_to_its_commit(self):
        self.net.remotes[SDL_GIT] = {SDL_TAG: (sha1("tag object"), SDL)}
        run = self.check()
        self.assert_passes(run)
        self.assertIn(SDL_OK, run.lines)

    def test_pinning_the_tag_object_of_an_annotated_tag_fails(self):
        tag_object = sha1("tag object")
        self.manifest(sdl_commit=tag_object)
        self.net.remotes[SDL_GIT] = {SDL_TAG: (tag_object, SDL)}
        self.assert_one_error(
            self.check(), f"with commit {tag_object[:12]} but the tag resolves to {SDL[:12]}"
        )

    def test_submodule_past_the_pinned_tag_is_only_a_warning(self):
        for moved in (OTHER, near(SDL)):
            with self.subTest(moved):
                self.move_gitlink("Thirdparty/SDL", moved)
                run = self.check()
                self.assertEqual((run.code, run.errors), (0, []))
                self.assertEqual(
                    run.warnings,
                    [
                        f"::warning::Manifest pins SDL tag release-3.2.30 ({SDL[:12]}) but the "
                        f"submodule is at {moved[:12]} \N{EM DASH} the Flatpak build compiles a "
                        "different SDL than the rest of the CI. Allowed, but usually unintended."
                    ],
                )
                self.assertNotIn(SDL_OK, run.lines)

    def test_remote_that_answers_with_an_http_error_fails(self):
        self.net.remotes[SDL_GIT] = "remote: Repository not found.\nfatal: Authentication failed\n"
        self.net.urls[info_refs(SDL_GIT)] = 401
        self.assert_one_error(
            self.check(),
            "SDL tag release-3.2.30 pinned in the manifest does not resolve upstream "
            f"({SDL_GIT} answered HTTP 401 Unauthorized).",
        )

    def test_git_failure_with_a_reachable_remote_fails_with_gits_message_on_one_line(self):
        # what git writes to stderr: what the error says
        cases = {
            "fatal: unable to access\n  'https://x/': SSL certificate problem\n": (
                "fatal: unable to access 'https://x/': SSL certificate problem"
            ),
            "fatal: unable to access 'x':\n\tline two\r\nline three\n\n": (
                "fatal: unable to access 'x': line two line three"
            ),
            "": "exit status 128",
        }
        for stderr, message in cases.items():
            with self.subTest(stderr):
                self.reset()
                self.net.remotes[SDL_GIT] = stderr
                self.net.urls[info_refs(SDL_GIT)] = "001e# service=git-upload-pack\n"
                self.assert_one_error(
                    self.check(),
                    f"does not resolve upstream (git ls-remote {SDL_GIT} failed: {message}).",
                )

    def test_unreachable_remote_is_a_skip(self):
        self.net.down.add(GIT_HOST)
        run = self.check()
        self.assertEqual(run.code, 0)
        self.assertIn(
            "SKIPPED, not verified: that SDL tag release-3.2.30 resolves to the pinned commit "
            "upstream.",
            run.skips[-1],
        )
        self.assertNotIn(SDL_OK, run.lines)


# ── Network: unreachable is a skip, an error answer is a failure ────────


class NetworkTests(CheckerTestCase):
    def offline(self):
        self.net.down |= {RAW_HOST, GIT_HOST}

    def test_no_network_skips_every_upstream_check_and_exits_0(self):
        self.offline()
        run = self.check()
        self.assertEqual((run.code, run.errors), (0, []))
        # What the repository alone answers is still checked, and passes.
        self.assertEqual(
            [line.split(" (")[0] for line in run.oks],
            [
                "ok: rust-tts-wrapper manifest pin == submodule",
                "ok: DasherCore manifest pin == submodule",
                "ok: rust-tts-wrapper-Cargo.lock was generated for the pinned rust-tts-wrapper",
                "ok: cargo-sources.json vendors the 3 crates in rust-tts-wrapper-Cargo.lock",
            ],
        )
        self.assertEqual(
            run.skipped,
            [
                "that rust-tts-wrapper-Cargo.lock has the crate version of rust-tts-wrapper "
                f"{WRAPPER[:12]}, and that the commit exists upstream",
                "that sherpa-onnx tag v2.0.1 resolves to the pinned commit upstream",
                "that ONNX Runtime 1.28.2 is what sherpa-onnx v2.0.1 pins (x86_64)",
                "that ONNX Runtime 1.28.2 is what sherpa-onnx v2.0.1 pins (aarch64)",
                *(
                    f"the dependency tarball pinned in {dep.where}"
                    for dep in DEPS
                    if not dep.transitive
                ),
                "that SDL tag release-3.2.30 resolves to the pinned commit upstream",
            ],
        )
        self.assertEqual(len(run.skips), 13)
        # Every warning of this run is a skip or the summary: 13 + 1.
        self.assertEqual(run.warnings, [*run.skips, *run.incomplete])

    def test_incomplete_run_is_the_last_line_and_lists_what_was_skipped(self):
        self.offline()
        run = self.check()
        self.assertEqual(
            run.lines[-1],
            "::warning::INCOMPLETE RUN: 13 check(s) were SKIPPED because the network could not "
            f"be reached. Not verified: {'; '.join(run.skipped)}. Nothing that did run failed, so this "
            "exits 0, but it is not a full pass: run it again with network access.",
        )

    def test_skip_warning_names_the_url_the_error_and_the_check(self):
        self.offline()
        run = self.check()
        refused = "<urlopen error [Errno 101] Network is unreachable>"
        self.assertEqual(
            run.skips[1],
            f"::warning::Could not reach {info_refs(SHERPA_GIT)} ({refused}). SKIPPED, not "
            "verified: that sherpa-onnx tag v2.0.1 resolves to the pinned commit upstream.",
        )
        # A host that already failed is not tried again; the warning says so.
        self.assertEqual(
            run.skips[2],
            f"::warning::Could not reach {sherpa_raw(ORT_CMAKE['x86_64'])} ({RAW_HOST} was "
            "unreachable earlier in this run). SKIPPED, not verified: that ONNX Runtime 1.28.2 "
            "is what sherpa-onnx v2.0.1 pins (x86_64).",
        )

    def test_an_unreachable_host_is_tried_once(self):
        self.offline()
        self.check()
        # One timeout per host, not one per lookup: 13 lookups, 2 requests.
        self.assertEqual(self.net.http_calls, [cargo_toml_url(), info_refs(SHERPA_GIT)])
        self.assertEqual(self.net.remotes_asked(), [SHERPA_GIT, SDL_GIT])

    def test_every_way_of_not_getting_an_answer_is_a_skip(self):
        failures = {
            "dns": urllib.error.URLError(
                socket.gaierror(-3, "Temporary failure in name resolution")
            ),
            "refused": urllib.error.URLError(ConnectionRefusedError(111, "Connection refused")),
            "timeout": TimeoutError("The read operation timed out"),
            "reset": ConnectionResetError(104, "Connection reset by peer"),
        }
        for label, failure in failures.items():
            with self.subTest(label):
                self.reset()
                self.net.urls[cargo_toml_url()] = failure
                run = self.check()
                self.assertEqual(run.code, 0, run.text)
                self.assertIn(f"Could not reach {cargo_toml_url()} ({failure}).", run.skips[0])
                # raw.githubusercontent.com is given up on; github.com still answers.
                self.assertEqual(len(run.skips), 11, run.text)
                self.assertIn(SDL_OK, run.lines)
                self.assertIn("ok: sherpa-onnx source is v2.0.1", run.text)

    def test_only_the_git_host_unreachable(self):
        self.net.down.add(GIT_HOST)
        run = self.check()
        self.assertEqual(run.code, 0)
        self.assertEqual(len(run.skips), 2, run.text)
        for line in run.warnings:  # the two skips and the summary
            self.assertRegex(line, r"^::warning::(Could not reach https://|INCOMPLETE RUN: )")
        self.assertEqual(len(run.warnings), 3)
        # sherpa-onnx's files are still read at the commit the manifest pins.
        self.assertIn(
            "ok: ONNX Runtime 1.28.2 is what sherpa-onnx v2.0.1 pins (aarch64)", run.lines
        )
        self.assertIn(TARBALLS_OK, run.text)
        # But nothing confirmed that the tag is that commit.
        self.assertNotIn("ok: sherpa-onnx source is", run.text)

    def test_http_error_on_any_file_fails_and_is_not_a_skip(self):
        for status in (404, 500):
            for url in ALL_URLS:
                with self.subTest(status=status, url=url):
                    self.reset()
                    self.net.urls[url] = status
                    run = self.check()
                    self.assertEqual(run.code, 1, run.text)
                    self.assert_error(run, f"HTTP {status} {http.client.responses[status]}")
                    self.assertEqual((run.skips, run.incomplete), ([], []))

    def test_local_errors_still_fail_without_network(self):
        """Everything the repository alone shows fails offline too, with the
        same errors and nothing else, and the summary says the errors stand."""

        def manifest(**pins) -> dict:
            return dict(manifest=render_manifest(**pins))

        no_wrapper, unvendored = packages(wrapper_version=None), crate("zeroize", "1.8.2")
        no_sdl_commit = NO_SDL_COMMIT + "next to the SDL tag"
        # label: (the files that differ, a fragment of each error expected)
        cases = {
            "DasherCore pin": (manifest(core=OTHER), "pins DasherCore at"),
            "rust-tts-wrapper pin missing": (
                manifest(wrapper=None),
                "No commit: pin found for the rust-tts-wrapper",
            ),
            "lock commit stale": (dict(lock_commit=OTHER), "was generated for rust-tts-wrapper"),
            "lock commit missing": (dict(lock_commit=None), "does not hold a commit SHA"),
            "lock without the wrapper": (
                dict(lock=render_lock(no_wrapper)),
                "rust-tts-wrapper-Cargo.lock has no rust-tts-wrapper package",
            ),
            "crate not vendored": (
                dict(lock=render_lock(packages(unvendored))),
                "cargo-sources.json does not match",
            ),
            "sherpa-onnx tag": (
                manifest(sherpa_tag="v2.0.0"),
                "locks sherpa-onnx-sys 2.0.1 but the manifest builds sherpa-onnx v2.0.0",
            ),
            "sherpa-onnx commit missing": (manifest(sherpa_commit=None), NO_SHERPA_PIN),
            "ONNX Runtime version missing": (
                manifest(ort=None),
                "does not state the ONNX Runtime version it builds",
            ),
            "SDL tag missing": (manifest(sdl_tag=None), "No tag: pin"),
            "SDL commit missing": (manifest(sdl_commit=None), no_sdl_commit),
            "SDL commit cut short": (manifest(sdl_commit=SDL[:39]), no_sdl_commit),
            "SDL tag and commit missing": (
                manifest(sdl_tag=None, sdl_commit=None),
                "No tag: pin",
                NO_SDL_COMMIT + "for the SDL git source",
            ),
        }
        for name, (_, keyword, sha, error) in GIT_SOURCES.items():
            pins = manifest(**{keyword: f"{sha} v0.2.28"})
            cases[f"{name} commit with a second word"] = (pins, error.format(name=name))
        for label, (files, *fragments) in cases.items():
            with self.subTest(label):
                self.reset(**files)
                self.offline()
                run = self.check()
                self.assertEqual((run.code, len(run.errors)), (1, len(fragments)), run.text)
                for fragment in fragments:
                    self.assert_error(run, fragment)
                self.assertTrue(run.lines[-1].startswith("::warning::INCOMPLETE RUN: "), run.text)
                self.assertTrue(run.lines[-1].endswith(". The errors above stand."), run.text)
                self.assertNotIn("this exits 0", run.lines[-1])


# ── Regression tests for commit 6dfa773 ─────────────────────────────────


class OfflineOrderingRegressionTests(CheckerTestCase):
    """6dfa773 "pin-sync fails on locally detectable errors without network".

    Before it, the branch that skips an unreachable remote ran before the
    test for a missing pin, so an error that the manifest alone shows was
    reported as a skipped upstream lookup and the run exited 0.
    NetworkTests.test_local_errors_still_fail_without_network tries every
    such error offline, the two others this commit fixed included (a lock
    with no rust-tts-wrapper package, a manifest that does not state its
    ONNX Runtime version). Here: the case the commit was made for, in
    detail, and the one that needs the remote to answer.
    """

    def setUp(self):
        super().setUp()
        self.net.down |= {RAW_HOST, GIT_HOST}

    def test_sdl_commit_deleted_and_github_unreachable_exits_1(self):
        text = render_manifest()
        line = f"        commit: {SDL}\n"
        self.assertEqual(text.count(line), 1)
        self.write(manifest=text.replace(line, ""))
        run = self.check()
        self.assertEqual(run.code, 1, "exited 0 before 6dfa773")
        self.assert_one_error(run, NO_SDL_COMMIT + "next to the SDL tag release-3.2.30")
        # The lookup that would name the commit is what was skipped.
        self.assertIn(
            "SKIPPED, not verified: which commit SDL tag release-3.2.30 resolves to upstream.",
            run.skips[-1],
        )
        self.assertTrue(run.lines[-1].endswith("The errors above stand."))

    def test_missing_onnxruntime_version_is_reported_when_the_sherpa_tag_lookup_errors(self):
        # Online again: the remote answers, and the tag is not there. The
        # function returns at that error; the missing version line was
        # checked after it and its message was lost (the status was 1).
        self.net.down.clear()
        self.net.remotes[SHERPA_GIT] = {}
        self.manifest(ort=None)
        run = self.check()
        self.assertEqual(len(run.errors), 2, run.text)
        self.assert_error(run, "does not state the ONNX Runtime version it builds")
        self.assert_error(run, "sherpa-onnx tag v2.0.1 pinned in the manifest does not resolve")


# ── --fix ───────────────────────────────────────────────────────────────


NEW_CORE = sha1("DasherCore, bumped")
NEW_WRAPPER = sha1("rust-tts-wrapper, bumped")
LOCK_OK = (
    f"ok: rust-tts-wrapper-Cargo.lock was generated for the pinned rust-tts-wrapper "
    f"({WRAPPER[:12]})"
)
NO_LINE_TO_FIX = (
    f"error: no commit: pin to fix for dasher-project/DasherCore in {MANIFEST_NAME} (its value "
    "has to be one full SHA as YAML reads it)"
)


class FixModeTests(CheckerTestCase):
    """--fix moves the hard-pinned commits (rust-tts-wrapper, DasherCore) to
    the gitlinks staged in the index. It never touches the SDL pin, which
    follows a tag, and never the offline cargo inputs."""

    def snapshot(self) -> dict:
        names = (MANIFEST_NAME, LOCK_NAME, LOCK_COMMIT_NAME, CARGO_SOURCES_NAME)
        return {name: (self.repo / name).read_bytes() for name in names}

    def test_nothing_to_fix_changes_nothing_and_exits_0(self):
        before = self.snapshot()
        run = self.check("--fix")
        self.assertEqual((run.code, run.lines), (0, [LOCK_OK]))
        self.assertEqual(self.snapshot(), before)

    def test_moves_the_dashercore_pin_to_the_staged_gitlink(self):
        before = self.snapshot()
        self.move_gitlink("DasherCore", NEW_CORE, committed=False)
        run = self.check("--fix")
        self.assertEqual(
            (run.code, run.lines),
            (0, [f"fixed: DasherCore manifest pin -> {NEW_CORE[:12]}", LOCK_OK]),
        )
        after = self.snapshot()
        # That one SHA changed, once, and nothing else in any file.
        manifest = before.pop(MANIFEST_NAME).decode()
        self.assertEqual(manifest.count(CORE), 1)
        self.assertEqual(after.pop(MANIFEST_NAME).decode(), manifest.replace(CORE, NEW_CORE))
        self.assertEqual(after, before)

    def test_moves_a_pin_that_differs_in_the_last_character_only(self):
        self.move_gitlink("DasherCore", near(CORE), committed=False)
        self.check("--fix")
        self.assertEqual(cfp.manifest_git_pin("dasher-project/DasherCore"), (near(CORE), None))

    def test_is_idempotent(self):
        self.move_gitlink("DasherCore", NEW_CORE, committed=False)
        self.check("--fix")
        fixed = self.snapshot()
        mtime = (self.repo / MANIFEST_NAME).stat().st_mtime_ns
        run = self.check("--fix")
        self.assertEqual((run.code, run.lines), (0, [LOCK_OK]))
        self.assertEqual(self.snapshot(), fixed)
        self.assertEqual((self.repo / MANIFEST_NAME).stat().st_mtime_ns, mtime, "not rewritten")

    def test_bump_then_fix_then_commit_passes_the_check(self):
        """The shared-deps bump workflow: stage the submodule, --fix, commit."""
        self.move_gitlink("DasherCore", NEW_CORE, committed=False)
        self.check("--fix")
        # The check compares with HEAD, which the bump has not reached yet.
        self.assert_one_error(
            self.check(),
            f"pins DasherCore at {NEW_CORE[:12]} but the submodule is at {CORE[:12]}",
        )
        git(self.repo, "add", MANIFEST_NAME)
        commit_all(self.repo, "chore: bump DasherCore")
        self.assert_passes(self.check())

    def test_never_touches_the_sdl_pin(self):
        self.move_gitlink("Thirdparty/SDL", OTHER, committed=False)
        before = self.snapshot()
        run = self.check("--fix")
        self.assertEqual((run.code, run.lines), (0, [LOCK_OK]))
        self.assertEqual(self.snapshot(), before)

    def test_moved_rust_tts_wrapper_is_fixed_but_the_stale_lock_fails(self):
        before = self.snapshot()
        self.move_gitlink("rust-tts-wrapper", NEW_WRAPPER, committed=False)
        run = self.check("--fix")
        self.assertEqual(run.code, 1)
        self.assertEqual(
            run.lines[0], f"fixed: rust-tts-wrapper manifest pin -> {NEW_WRAPPER[:12]}"
        )
        self.assert_one_error(
            run,
            f"rust-tts-wrapper-Cargo.lock was generated for rust-tts-wrapper {WRAPPER[:12]} but "
            f"the submodule is at {NEW_WRAPPER[:12]}",
            "Regenerate",
        )
        after = self.snapshot()
        self.assertEqual(cfp.manifest_git_pin("AACTools/rust-tts-wrapper"), (NEW_WRAPPER, None))
        # A stale lock is never blessed: the recorded commit, the lock and
        # the crate list are exactly what they were.
        for name in (LOCK_COMMIT_NAME, LOCK_NAME, CARGO_SOURCES_NAME):
            self.assertEqual(after[name], before[name], name)
        self.assertEqual(after[LOCK_COMMIT_NAME].decode().strip(), WRAPPER)

    def test_stale_lock_keeps_failing_on_every_run(self):
        self.move_gitlink("rust-tts-wrapper", NEW_WRAPPER, committed=False)
        self.assertEqual(self.check("--fix").code, 1)
        run = self.check("--fix")
        self.assertEqual(run.code, 1)
        self.assertEqual(run.starting("fixed: "), [])
        self.assertEqual(len(run.errors), 1)

    def test_fixes_both_pins_in_one_run(self):
        self.move_gitlink("DasherCore", NEW_CORE, committed=False)
        self.move_gitlink("rust-tts-wrapper", NEW_WRAPPER, committed=False)
        self.write(lock_commit=NEW_WRAPPER)  # the cargo inputs were regenerated
        run = self.check("--fix")
        self.assertEqual(run.code, 0)
        self.assertEqual(
            run.starting("fixed: "),
            [
                f"fixed: rust-tts-wrapper manifest pin -> {NEW_WRAPPER[:12]}",
                f"fixed: DasherCore manifest pin -> {NEW_CORE[:12]}",
            ],
        )

    def test_missing_lock_commit_fails(self):
        self.write(lock_commit=None)
        run = self.check("--fix")
        self.assert_one_error(run, "does not hold a commit SHA")
        self.assertFalse((self.repo / LOCK_COMMIT_NAME).exists(), "never written by --fix")

    def test_makes_no_network_request(self):
        self.net = FakeNet()  # any request raises
        self.move_gitlink("DasherCore", NEW_CORE, committed=False)
        self.check("--fix")
        self.assertEqual((self.net.http_calls, self.net.ls_remote_calls), ([], []))

    def test_is_recognised_anywhere_among_the_arguments(self):
        self.move_gitlink("DasherCore", NEW_CORE, committed=False)
        run = self.check("--verbose", "--fix")
        self.assertIn(f"fixed: DasherCore manifest pin -> {NEW_CORE[:12]}", run.lines)

    def test_path_that_is_not_a_gitlink_in_the_index_stops_the_run(self):
        git(self.repo, "rm", "-q", "--cached", "DasherCore")
        (self.repo / "DasherCore").write_text("not a submodule\n")
        git(self.repo, "add", "DasherCore")
        self.assertRegex(
            self.stop_message(self.check, "--fix"),
            r"^error: DasherCore is not a submodule in the index: '100644 ",
        )

    def test_source_without_a_commit_line_stops_the_run(self):
        self.manifest(core=None, core_branch="main")
        before = self.snapshot()
        self.assertEqual(self.stop_message(self.check, "--fix"), NO_LINE_TO_FIX)
        self.assertEqual(self.snapshot(), before)

    def test_missing_git_source_stops_the_run(self):
        self.write(
            manifest=render_manifest().replace("AACTools/rust-tts-wrapper", "AACTools/other")
        )
        self.assertEqual(
            self.stop_message(self.check, "--fix"),
            f"error: no git source for AACTools/rust-tts-wrapper in {MANIFEST_NAME}",
        )


# ── What counts as a `commit:` pin ──────────────────────────────────────


NO_HARD_PIN = "No commit: pin found for the {name} git source"
# The four git sources whose `commit:` the checker reads.
# name: (fragment of the url, render_manifest keyword, the commit, the error
#        of a run in which that source has no commit pin; {name} is the name)
GIT_SOURCES = {
    "rust-tts-wrapper": ("AACTools/rust-tts-wrapper", "wrapper", WRAPPER, NO_HARD_PIN),
    "DasherCore": ("dasher-project/DasherCore", "core", CORE, NO_HARD_PIN),
    "SDL": ("libsdl-org/SDL", "sdl_commit", SDL, NO_SDL_COMMIT + "next to the SDL tag"),
    "sherpa-onnx": ("k2-fsa/sherpa-onnx", "sherpa_commit", SHERPA, NO_SHERPA_PIN),
}

# What may follow the SHA in a `commit:` value: blanks, and a comment after
# at least one blank. label -> the text after the SHA.
AFTER_A_PIN = {
    "nothing": "",
    "blanks": "  ",
    "a tab": "\t",
    "a comment": " # v0.2.28",
    "a comment after two blanks": "  # v0.2.28",
    "a comment after a tab": "\t# v0.2.28",
    "an empty comment": " #",
    "a comment with a # in it": " #v0.2.28 #2",
}


def not_a_pin(sha: str) -> dict:
    """`commit:` values that do not pin `sha`, label -> value: no full SHA,
    or a SHA that YAML reads together with what follows it."""
    return {
        "abbreviated": sha[:12],
        "one short": sha[:39],
        "upper case": sha.upper(),
        "not hex": "g" * 40,
        "a ref": "HEAD",
        "one more hex digit": sha + "0",
        "a letter": sha + "x",
        "a suffix": sha + "-dirty",
        "a path": sha + "/x",
        "a second word": sha + " v0.2.28",
        "a second word and a comment": sha + " v0.2.28 # the release",
        "a # with no blank before it": sha + "#v0.2.28",
        "the sha twice": f"{sha} {sha}",
        # Text on a line of its own that YAML makes part of the value. (The
        # keys of a source are indented by eight blanks in render_manifest.)
        "a continuation line": f"{sha}\n          v3.2.30",
        "a continuation line after CRLF": f"{sha}\r\n          v3.2.30",
        "a continuation line after a blank line": f"{sha}\n\n          v3.2.30",
        "a continuation line and a comment": f"{sha}\n          v3.2.30 # the release",
        "a literal block scalar": f"|\n          {sha}",
        "a folded block scalar": f">\n          {sha}",
        "a folded block scalar of two lines": f">-\n          {sha}\n          v3.2.30",
        "quotes with a blank inside": f'"{sha} "',
        "quotes around two lines": f'"{sha}\n          v3.2.30"',
        "a sequence": f"[{sha}]",
        "a mapping": f"{{sha: {sha}}}",
        "no value": "",
    }


def written_pins(sha: str) -> dict:
    """`commit:` values that YAML reads as exactly `sha` although it is not
    written plainly. label -> (value, what --fix has to put in its place for
    a new SHA; None when --fix must refuse to rewrite it)."""
    escaped = f'"\\x{ord(sha[0]):02x}{sha[1:]}"'
    return {
        "single quotes": (f"'{sha}'", "'{}'"),
        "double quotes": (f'"{sha}"', '"{}"'),
        "double quotes and a comment": (f'"{sha}"  # v0.2.28', '"{}"  # v0.2.28'),
        "a comment on the next line": (f"{sha}\n          # v0.2.28", "{}\n          # v0.2.28"),
        "an escape in double quotes": (escaped, None),
        "a stripped literal block scalar": (f"|-\n          {sha}", None),
        "a stripped folded block scalar": (f">-\n          {sha}", None),
        "an anchor": (f"&pin-{sha[:8]} {sha}", None),
        "a tag": (f"!!str {sha}", None),
    }


def cannot_rewrite(fragment: str, sha: str) -> str:
    """How the message of a --fix that refuses to rewrite a pin starts, and
    how it ends: with the SHA to set by hand."""
    return (
        rf"^error: cannot rewrite the commit: pin for {re.escape(fragment)} in "
        rf"{re.escape(MANIFEST_NAME)} \(line [0-9]+\): .* Set it to {sha} by hand\.$"
    )


class CommitValueTests(CheckerTestCase):
    """A `commit:` value is a pin only when YAML reads the 40 characters of
    a SHA from it and nothing else.

    The manifest used to be read line by line. That first took the leading
    40 hex characters and ignored the rest, so `commit: <sha>0` compared
    equal to the submodule commit and passed. Then it took anything after a
    blank for a comment, so `commit: <sha> v0.2.28` passed. Then it looked
    at one line, so a SHA with `v3.2.30` on an indented line of its own
    passed, which YAML folds into the value. flatpak-builder is handed the
    whole value every time and fails to check it out. The manifest is now
    read with a YAML parser, and these tests compare the checker with a
    second reading by PyYAML's typed loader.
    """

    def test_value_that_is_not_one_full_sha_is_reported_as_a_missing_pin(self):
        for name, (fragment, keyword, sha, error) in GIT_SOURCES.items():
            for label, value in not_a_pin(sha).items():
                with self.subTest(name, commit=label):
                    self.reset(manifest=render_manifest(**{keyword: value}))
                    self.assertIsNone(cfp.manifest_git_pin(fragment)[0])
                    self.assert_one_error(self.check(), error.format(name=name))

    def test_blanks_and_a_comment_after_the_sha_are_not_part_of_the_value(self):
        for label, tail in AFTER_A_PIN.items():
            with self.subTest(label):
                # All four sources at once.
                pins = {keyword: sha + tail for _, keyword, sha, _ in GIT_SOURCES.values()}
                self.reset(manifest=render_manifest(**pins))
                for fragment, _, sha, _ in GIT_SOURCES.values():
                    self.assertEqual(cfp.manifest_git_pin(fragment)[0], sha)
                self.assert_passes(self.check())

    def test_a_continuation_line_is_part_of_the_value(self):
        """The review finding, as it was reported: the SDL pin with the
        release on the next line. YAML reads `<sha> v3.2.30`; the check
        exited 0."""
        text = render_manifest().replace(
            f"        commit: {SDL}\n", f"        commit: {SDL}\n          v3.2.30\n"
        )
        self.assertEqual(text.count("          v3.2.30\n"), 1)
        self.write(manifest=text)
        [sdl] = [
            source
            for module in yaml.safe_load(text)["modules"]
            for source in module["sources"]
            if isinstance(source, dict) and "libsdl-org/SDL" in source.get("url", "")
        ]
        self.assertEqual(sdl["commit"], f"{SDL} v3.2.30")
        self.assertEqual(cfp.manifest_git_pin("libsdl-org/SDL"), (None, SDL_TAG))
        self.assert_one_error(self.check(), NO_SDL_COMMIT + "next to the SDL tag")

    def test_a_sha_that_is_not_written_plainly_is_still_the_pin(self):
        labels = written_pins(CORE)
        for label in labels:
            with self.subTest(label):
                # All four sources at once.
                pins = {
                    keyword: written_pins(sha)[label][0]
                    for _, keyword, sha, _ in GIT_SOURCES.values()
                }
                self.reset(manifest=render_manifest(**pins))
                for fragment, _, sha, _ in GIT_SOURCES.values():
                    self.assertEqual(cfp.manifest_git_pin(fragment)[0], sha)
                self.assert_passes(self.check())

    def test_a_pin_that_is_not_written_plainly_is_still_compared(self):
        for label, (value, _) in written_pins(OTHER).items():
            with self.subTest(label):
                self.reset(manifest=render_manifest(core=value))
                self.assert_one_error(
                    self.check(),
                    f"pins DasherCore at {OTHER[:12]} but the submodule is at {CORE[:12]}",
                )

    def test_the_checker_reads_a_commit_value_the_way_yaml_does(self):
        # libyaml is the parser flatpak-builder uses. PyYAML's own parser
        # refuses a tab after a value, which libyaml takes for a blank; with
        # only that parser the forms with a tab are left out.
        loader = getattr(yaml, "CSafeLoader", None)
        for name, (fragment, keyword, sha, _) in GIT_SOURCES.items():
            values = (
                not_a_pin(sha)
                | {label: sha + tail for label, tail in AFTER_A_PIN.items()}
                | {label: value for label, (value, _) in written_pins(sha).items()}
            )
            for label, value in values.items():
                if "\t" in value and loader is None:
                    continue
                with self.subTest(name, commit=label):
                    self.manifest(**{keyword: value})
                    manifest = yaml.load(self.read(MANIFEST_NAME), Loader=loader or yaml.SafeLoader)
                    [source] = [
                        source
                        for module in manifest["modules"]
                        for source in module["sources"]
                        if isinstance(source, dict)
                        and source["type"] == "git"
                        and fragment in source["url"]
                    ]
                    self.assertEqual(
                        cfp.manifest_git_pin(fragment)[0] == sha,
                        source.get("commit") == sha,
                        f"YAML reads {source.get('commit')!r}",
                    )

    def test_fix_replaces_the_sha_and_keeps_everything_around_it(self):
        self.move_gitlink("DasherCore", NEW_CORE, committed=False)
        for label, tail in AFTER_A_PIN.items():
            with self.subTest(label):
                line = f"commit:   {CORE}{tail}\n"  # and blanks of its own before the SHA
                text = render_manifest().replace(f"commit: {CORE}\n", line)
                self.assertIn(line, text)
                self.reset(manifest=text)
                run = self.check("--fix")
                self.assertEqual(run.code, 0, run.text)
                self.assertEqual(
                    (self.repo / MANIFEST_NAME).read_bytes(),
                    text.replace(CORE, NEW_CORE).encode(),
                )

    def test_fix_stops_at_a_value_that_is_not_one_full_sha_and_changes_nothing(self):
        self.move_gitlink("DasherCore", NEW_CORE, committed=False)
        for label, value in not_a_pin(CORE).items():
            with self.subTest(label):
                self.reset(manifest=render_manifest(core=value))
                before = (self.repo / MANIFEST_NAME).read_bytes()
                self.assertEqual(self.stop_message(self.check, "--fix"), NO_LINE_TO_FIX)
                self.assertEqual((self.repo / MANIFEST_NAME).read_bytes(), before)

    def test_fix_rewrites_a_quoted_sha_and_refuses_what_it_cannot_rewrite_in_place(self):
        """Validation and --fix agree: what is a pin can be fixed, or --fix
        says that it cannot and leaves the file alone. It never writes a
        manifest that pins something else than it reports."""
        self.move_gitlink("DasherCore", NEW_CORE, committed=False)
        for label, (value, fixed) in written_pins(CORE).items():
            with self.subTest(label):
                text = render_manifest(core=value)
                self.reset(manifest=text)
                if fixed is None:
                    self.assertRegex(
                        self.stop_message(self.check, "--fix"),
                        cannot_rewrite("dasher-project/DasherCore", NEW_CORE),
                    )
                    self.assertEqual((self.repo / MANIFEST_NAME).read_bytes(), text.encode())
                    continue
                run = self.check("--fix")
                self.assertEqual(run.code, 0, run.text)
                self.assertEqual(
                    (self.repo / MANIFEST_NAME).read_bytes(),
                    render_manifest(core=fixed.format(NEW_CORE)).encode(),
                )
                self.assertEqual(
                    cfp.manifest_git_pin("dasher-project/DasherCore"), (NEW_CORE, None)
                )


# ── The manifest is read as YAML ────────────────────────────────────────


CORE_URL = "https://github.com/dasher-project/DasherCore.git"
CORE_BLOCK = render_source(git_source(CORE_URL, commit=CORE, dest="DasherCore"))
NOT_YAML = f"error: {MANIFEST_NAME} is not valid YAML: "


def with_core_source(source: str) -> str:
    """The default manifest with its DasherCore source written as `source`
    (the lines of one list item, indented like the others)."""
    text = render_manifest()
    assert text.count(CORE_BLOCK) == 1
    return text.replace(CORE_BLOCK, source)


class YamlFormTests(CheckerTestCase):
    """The checker and flatpak-builder read the same manifest: every form
    YAML has for the same data gives the same pins, and what YAML does not
    accept, or reads in a way a person would not, stops the run."""

    def write_bytes(self, data: bytes):
        (self.repo / MANIFEST_NAME).write_bytes(data)

    def read_bytes(self) -> bytes:
        return (self.repo / MANIFEST_NAME).read_bytes()

    def assert_fix_moves_core(self, text: str):
        """--fix replaces CORE by NEW_CORE in `text` and nothing else."""
        self.move_gitlink("DasherCore", NEW_CORE, committed=False)
        run = self.check("--fix")
        self.assertEqual(run.lines[0], f"fixed: DasherCore manifest pin -> {NEW_CORE[:12]}")
        self.assertEqual(text.count(CORE), 1)
        self.assertEqual(self.read_bytes(), text.replace(CORE, NEW_CORE).encode())

    # Flow style ------------------------------------------------------------

    def test_a_source_written_as_a_flow_mapping_is_read(self):
        flow = f"      - {{type: git, url: '{CORE_URL}', commit: {CORE}, dest: DasherCore}}\n"
        self.write(manifest=with_core_source(flow))
        self.assertEqual(cfp.manifest_git_pin("dasher-project/DasherCore"), (CORE, None))
        self.assert_passes(self.check())
        self.write(manifest=with_core_source(flow.replace(CORE, OTHER)))
        self.assert_one_error(self.check(), f"pins DasherCore at {OTHER[:12]}")

    def test_fix_rewrites_the_commit_in_a_flow_mapping(self):
        text = with_core_source(
            f"      - {{type: git, url: '{CORE_URL}', commit: {CORE}, dest: DasherCore}}\n"
        )
        self.write(manifest=text)
        self.assert_fix_moves_core(text)

    def test_the_keys_of_a_source_may_come_in_any_order(self):
        self.write(
            manifest=with_core_source(
                f"      - dest: DasherCore\n        commit: {CORE}\n"
                f"        url: {CORE_URL}\n        type: git\n"
            )
        )
        self.assert_passes(self.check())

    def test_tarballs_written_as_flow_mappings_are_read(self):
        flow = [
            f"{{dest-filename: {d.filename}, sha256: {d.sha256}, url: '{d.url}', type: file}}"
            for d in DEPS
        ]
        self.manifest(sherpa_files=flow)
        self.assertEqual(
            cfp.module_sources("sherpa-onnx")[1:],
            [
                {"type": "file", "url": d.url, "sha256": d.sha256, "dest-filename": d.filename}
                for d in DEPS
            ],
        )
        self.assert_passes(self.check())

    # Anchors and aliases ---------------------------------------------------

    def aliased(self, core: str = CORE) -> str:
        """The DasherCore pin as an alias of an anchor at the top."""
        text = render_manifest(core="*core")
        return text.replace("\nmodules:\n", f"\nx-pins:\n  core: &core {core}\n\nmodules:\n")

    def test_an_alias_is_read_as_what_it_stands_for(self):
        self.write(manifest=self.aliased())
        self.assertEqual(cfp.manifest_git_pin("dasher-project/DasherCore"), (CORE, None))
        self.assert_passes(self.check())
        self.write(manifest=self.aliased(OTHER))
        self.assert_one_error(self.check(), f"pins DasherCore at {OTHER[:12]}")
        self.write(manifest=self.aliased(f"{CORE}\n    v0.2.28"))
        self.assert_one_error(self.check(), NO_HARD_PIN.format(name="DasherCore"))

    def test_fix_does_not_rewrite_an_alias_or_its_anchor(self):
        text = self.aliased()
        self.write(manifest=text)
        self.move_gitlink("DasherCore", NEW_CORE, committed=False)
        self.assertRegex(
            self.stop_message(self.check, "--fix"),
            cannot_rewrite("dasher-project/DasherCore", NEW_CORE),
        )
        self.assertEqual(self.read_bytes(), text.encode())

    def test_an_alias_that_contains_itself_stops_the_run(self):
        self.write(manifest="modules: &again\n  - name: dasher\n    modules: *again\n")
        self.assertEqual(
            self.stop_message(self.check),
            f"error: {MANIFEST_NAME} line 1: an alias that contains itself",
        )

    # Duplicate keys --------------------------------------------------------

    def test_a_key_that_is_in_a_mapping_twice_stops_the_run(self):
        """A parser keeps the last `commit:`, so flatpak-builder would build
        OTHER; a reading from the top found the first and passed."""
        twice = "is in this mapping twice"
        cases = {
            "commit, the pinned one first": with_core_source(
                CORE_BLOCK.replace("        dest:", f"        commit: {OTHER}\n        dest:")
            ),
            "commit, the pinned one last": with_core_source(
                CORE_BLOCK.replace("        commit:", f"        commit: {OTHER}\n        commit:")
            ),
            "commit, in a flow mapping": with_core_source(
                f"      - {{type: git, url: '{CORE_URL}', commit: {CORE}, commit: {OTHER}}}\n"
            ),
            "url": with_core_source(CORE_BLOCK + "        url: https://example.org/x.git\n"),
            "tag": render_manifest().replace(
                f"        tag: {SDL_TAG}\n", f"        tag: {SDL_TAG}\n        tag: release-9\n"
            ),
            "modules": render_manifest() + "modules: []\n",
        }
        for label, text in cases.items():
            key = label.split(",")[0]
            for argv in ((), ("--fix",)):
                with self.subTest(label, argv=argv):
                    self.reset(manifest=text)
                    message = self.stop_message(self.check, *argv)
                    self.assertRegex(
                        message,
                        rf"^error: {re.escape(MANIFEST_NAME)} line [0-9]+: the key `{key}` "
                        rf"{twice} \(the mapping starts on line [0-9]+\)\. ",
                    )
                    self.assertNotIn("\n", message)
                    self.assertEqual(self.read(MANIFEST_NAME), text)

    def test_the_same_key_in_two_sources_is_not_a_duplicate(self):
        # Every source has a `type:` and most a `commit:`; and the duplicate
        # check names the line of the second key and of its mapping.
        self.assert_passes(self.check())
        self.write(manifest="a: 1\nb:\n  c: 2\n  d: 3\n  c: 4\n")
        self.assertIn(
            "line 5: the key `c` is in this mapping twice (the mapping starts on line 3)",
            self.stop_message(self.check),
        )

    # Line endings, tabs, text that is not YAML --------------------------------

    def test_crlf_line_endings_are_read_and_kept(self):
        text = render_manifest().replace("\n", "\r\n")
        self.write_bytes(text.encode())
        self.assert_passes(self.check())
        for fragment, _, sha, _ in GIT_SOURCES.values():
            self.assertEqual(cfp.manifest_git_pin(fragment)[0], sha)
        self.assert_fix_moves_core(text)
        self.assertEqual(self.read_bytes().count(b"\r\n"), text.count("\n"))

    def test_crlf_line_endings_do_not_hide_a_wrong_pin(self):
        for label, core in (
            ("another commit", OTHER),
            ("a continuation line", f"{CORE}\n          v1"),
        ):
            with self.subTest(label):
                self.net = good_net()
                self.write_bytes(render_manifest(core=core).replace("\n", "\r\n").encode())
                self.assertEqual(len(self.check().errors), 1)

    def test_text_that_is_not_yaml_stops_the_run_with_one_line(self):
        good = render_manifest()
        cases = {
            "a tab as indentation": good.replace(f"        commit: {CORE}", f"\tcommit: {CORE}"),
            "a line that ends the mapping early": good.replace(
                f"        commit: {CORE}\n", f"        commit: {CORE}\n      v0.2.28\n"
            ),
            "text after a comment": good.replace(
                f"        commit: {CORE}\n", f"        commit: {CORE} # tag\n          v0.2.28\n"
            ),
            "an alias without an anchor": render_manifest(core="*nowhere"),
            "an open quote": render_manifest(core=f'"{CORE}'),
            "an open flow mapping": good + "x: {a: 1\n",
            "two documents": good + "---\nmodules: []\n",
        }
        for label, text in cases.items():
            for argv in ((), ("--fix",)):
                with self.subTest(label, argv=argv):
                    self.reset(manifest=text)
                    message = self.stop_message(self.check, *argv)
                    self.assertTrue(message.startswith(NOT_YAML), message)
                    self.assertRegex(message, r"line [0-9]+, column [0-9]+")
                    self.assertNotIn("\n", message)
                    self.assertEqual(self.read(MANIFEST_NAME), text)

    def test_a_mapping_key_that_is_not_a_scalar_stops_the_run(self):
        self.write(manifest="modules:\n  - ? [name, id]\n    : dasher\n")
        self.assertEqual(
            self.stop_message(self.check),
            f"error: {MANIFEST_NAME} line 2: a mapping key that is not a scalar",
        )

    def test_a_manifest_of_another_shape_has_no_pins_and_no_traceback(self):
        cases = {
            "empty": "",
            "only a comment": "# nothing yet\n",
            "a scalar": "manifest\n",
            "a sequence": "- modules\n- sources\n",
            "no modules": "app-id: org.example\n",
            "modules that are not a list": "modules: none\n",
            "modules given as file names": "modules:\n  - shared-modules/a.json\n  - b.yaml\n",
            "sources that are not a list": "modules:\n  - name: dasher\n    sources: none\n",
            "sources given as file names": (
                "modules:\n  - name: dasher\n    sources:\n      - cargo-sources.json\n"
            ),
            "a git source without a url": (
                f"modules:\n  - name: dasher\n    sources:\n      - type: git\n        commit: {CORE}\n"
            ),
            "a url that is not a scalar": (
                "modules:\n  - name: dasher\n    sources:\n      - type: git\n"
                f"        url: [{CORE_URL}]\n        commit: {CORE}\n"
            ),
            "a source of another type": (
                "modules:\n  - name: dasher\n    sources:\n      - type: archive\n"
                f"        url: {CORE_URL}\n        commit: {CORE}\n"
            ),
        }
        for label, text in cases.items():
            with self.subTest(label):
                self.reset(manifest=text)
                self.assertEqual(cfp.manifest_git_pin("dasher-project/DasherCore"), (None, None))
                self.assertIsNone(cfp.onnxruntime_version())
                run = self.check()
                self.assertEqual(run.code, 1)
                for name in ("rust-tts-wrapper", "DasherCore"):
                    self.assert_error(run, NO_HARD_PIN.format(name=name))
                self.assertEqual(
                    self.stop_message(self.check, "--fix"),
                    f"error: no git source for AACTools/rust-tts-wrapper in {MANIFEST_NAME}",
                )

    def test_a_git_source_of_a_module_in_a_module_is_read(self):
        nested = (
            "modules:\n"
            "  - name: outer\n"
            "    modules:\n"
            "      - inner.json\n"
            "      - name: inner\n"
            "        sources:\n"
            "          - type: git\n"
            f"            url: {CORE_URL}\n"
            f"            commit: {CORE}\n"
            f"            tag: v0.2.29\n"
        )
        self.write(manifest=nested)
        self.assertEqual(cfp.manifest_git_pin("dasher-project/DasherCore"), (CORE, "v0.2.29"))
        # Only the modules of the manifest itself are modules by name.
        self.assertEqual(cfp.module_sources("inner"), [])
        self.assertEqual(cfp.module_sources("outer"), [])

    def test_a_tag_is_the_whole_value(self):
        for label, tag, read in (
            ("two words", f"{SDL_TAG} extra", f"{SDL_TAG} extra"),
            ("quoted", f"'{SDL_TAG}'", SDL_TAG),
            ("a continuation line", f"{SDL_TAG}\n          extra", f"{SDL_TAG} extra"),
            ("empty", "", None),
            ("a sequence", f"[{SDL_TAG}]", None),
        ):
            with self.subTest(label):
                self.manifest(sdl_tag=tag)
                self.assertEqual(cfp.manifest_git_pin("libsdl-org/SDL"), (SDL, read))

    # The parser ------------------------------------------------------------

    def test_every_scalar_is_read_as_a_string(self):
        """A SHA of forty digits, a version like 1.10, a tag like `on`: a
        typed loader makes numbers and booleans of them."""
        digits = "1234567890" * 4
        text = f"a: {digits}\nb: 1.10\nc: on\nd: ~\ne:\nf: [1, {{g: 2}}]\n"
        self.assertEqual(
            cfp.parse_manifest(text),
            {"a": digits, "b": "1.10", "c": "on", "d": "~", "e": "", "f": ["1", {"g": "2"}]},
        )
        self.assertNotEqual(yaml.safe_load(text)["a"], digits)
        self.manifest(core=digits)
        self.assertEqual(cfp.manifest_git_pin("dasher-project/DasherCore"), (digits, None))

    def test_without_pyyaml_the_run_stops_and_says_what_to_install(self):
        for argv in ((), ("--fix",)):
            with self.subTest(argv=argv), mock.patch.object(cfp, "yaml", None):
                message = self.stop_message(self.check, *argv)
                self.assertRegex(message, r"^error: PyYAML is not installed, ")
                self.assertIn("python3-yaml", message)
                self.assertNotIn("\n", message)

    def test_the_script_loads_without_pyyaml(self):
        """So that the message above is what a machine without PyYAML gets,
        and not an ImportError."""
        with mock.patch.dict(sys.modules, {"yaml": None}):
            without = load_checker()
        self.assertIsNone(without.yaml)
        self.assertIsNotNone(cfp.yaml)
        with self.assertRaises(SystemExit) as stop:
            without.parse_manifest("a: 1\n")
        self.assertIn("PyYAML is not installed", str(stop.exception.code))

    def test_pyyaml_without_libyaml_reads_and_fixes_the_same(self):
        """PyYAML's own parser stands in where it was built without libyaml.
        It has to find the same pins at the same places."""
        if hasattr(yaml, "CBaseLoader"):
            self.addCleanup(setattr, yaml, "CBaseLoader", yaml.CBaseLoader)
            del yaml.CBaseLoader
        self.assert_passes(self.check())
        self.manifest(core=f"{CORE}\n          v0.2.28")
        self.assert_one_error(self.check(), NO_HARD_PIN.format(name="DasherCore"))
        for label, (value, fixed) in written_pins(CORE).items():
            if fixed is not None:
                with self.subTest(label):
                    text = "# Mañana, 20 €: a comment that is not ASCII\r\n" + render_manifest(
                        core=value
                    )
                    self.reset(manifest=text)
                    self.write_bytes(text.encode())
                    self.assert_fix_moves_core(text)
                    git(self.repo, "reset", "-q")

    def test_fix_writes_nothing_when_the_parser_did_not_say_where_the_value_is(self):
        """The two cross-checks of --fix. A byte order mark makes libyaml
        count one character less than there is, so the place it names does
        not hold the SHA; and a rewrite that parses to anything but the old
        manifest with the new pin is not written."""
        self.move_gitlink("DasherCore", NEW_CORE, committed=False)
        text = "\ufeff" + render_manifest()
        self.write_bytes(text.encode())
        self.assert_passes(self.check())
        try:
            run = self.check("--fix")
        except SystemExit as stop:
            self.assertRegex(str(stop.code), cannot_rewrite("dasher-project/DasherCore", NEW_CORE))
            self.assertEqual(self.read_bytes(), text.encode())
        else:  # a parser that counts the mark: then the fix has to be right
            self.assertEqual(run.code, 0)
            self.assertEqual(self.read_bytes(), text.replace(CORE, NEW_CORE).encode())

        self.write(manifest=render_manifest())
        before = self.read_bytes()
        real_parse = cfp.parse_manifest
        answers = iter([real_parse(before.decode()), {"modules": []}])
        with mock.patch.object(cfp, "parse_manifest", side_effect=lambda text: next(answers)):
            self.assertRegex(
                self.stop_message(cfp.fix_manifest_pin, "dasher-project/DasherCore", NEW_CORE),
                cannot_rewrite("dasher-project/DasherCore", NEW_CORE),
            )
        self.assertEqual(self.read_bytes(), before)


# ── Command line ────────────────────────────────────────────────────────


class CommandLineTests(CheckerTestCase):
    """The script as CI runs it: a process of its own, from the repo root."""

    def run_script(self, *argv, offline=True) -> subprocess.CompletedProcess:
        env = git_env()
        if offline:
            # No fake can be patched into another process. A proxy on a
            # loopback port that nothing listens on makes every lookup fail
            # at once, for urllib and for git, without leaving the machine.
            blackhole = socket.socket()
            self.addCleanup(blackhole.close)
            blackhole.bind(("127.0.0.1", 0))
            proxy = f"http://127.0.0.1:{blackhole.getsockname()[1]}"
            for name in ("http_proxy", "https_proxy", "all_proxy"):
                env[name] = env[name.upper()] = proxy
            env.pop("no_proxy", None)
            env.pop("NO_PROXY", None)
        return REAL_RUN(
            [sys.executable, "-I", str(SCRIPT), *argv],
            cwd=self.repo,
            env=env,
            capture_output=True,
            text=True,
            timeout=120,
        )

    def test_exits_0_with_the_incomplete_run_line_when_offline_and_in_sync(self):
        done = self.run_script()
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertEqual(done.stderr, "")
        self.assertEqual(output_problems(done.stdout), [])
        lines = done.stdout.splitlines()
        self.assertEqual(
            lines[0], f"ok: rust-tts-wrapper manifest pin == submodule ({WRAPPER[:12]})"
        )
        self.assertRegex(lines[-1], r"^::warning::INCOMPLETE RUN: 13 check\(s\) were SKIPPED")

    def test_exits_1_on_a_local_error_when_offline(self):
        """6dfa773 again, end to end: the SDL commit line deleted, no network."""
        self.manifest(sdl_commit=None)
        done = self.run_script()
        self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
        self.assertEqual(done.stderr, "")
        self.assertEqual(output_problems(done.stdout), [])
        errors = [line for line in done.stdout.splitlines() if line.startswith("::error::")]
        self.assertEqual(len(errors), 1, done.stdout)
        self.assertIn(NO_SDL_COMMIT + "next to the SDL tag release-3.2.30", errors[0])
        self.assertTrue(done.stdout.rstrip().endswith("The errors above stand."))

    def test_exits_1_on_a_pin_mismatch(self):
        self.manifest(core=OTHER)
        done = self.run_script()
        self.assertEqual(done.returncode, 1)
        self.assertIn(f"::error::Flatpak manifest pins DasherCore at {OTHER[:12]}", done.stdout)

    def test_fix_rewrites_the_manifest_and_exits_0(self):
        self.move_gitlink("DasherCore", NEW_CORE, committed=False)
        done = self.run_script("--fix", offline=False)  # --fix makes no request
        self.assertEqual((done.returncode, done.stderr), (0, ""), done.stdout)
        self.assertEqual(
            done.stdout.splitlines(),
            [f"fixed: DasherCore manifest pin -> {NEW_CORE[:12]}", LOCK_OK],
        )
        self.assertIn(f"commit: {NEW_CORE}\n", self.read(MANIFEST_NAME))

    def test_fix_exits_1_on_a_stale_lock(self):
        self.move_gitlink("rust-tts-wrapper", NEW_WRAPPER, committed=False)
        done = self.run_script("--fix", offline=False)
        self.assertEqual(done.returncode, 1)
        self.assertIn("::error::rust-tts-wrapper-Cargo.lock was generated for", done.stdout)
        self.assertEqual(self.read(LOCK_COMMIT_NAME).strip(), WRAPPER)

    def test_a_broken_checkout_exits_1_with_the_reason_on_stderr(self):
        git(self.repo, "rm", "-q", "--cached", "rust-tts-wrapper")
        commit_all(self.repo, "no submodule")
        done = self.run_script()
        self.assertEqual(done.returncode, 1)
        self.assertEqual(done.stdout, "")
        self.assertRegex(done.stderr, r"^error: rust-tts-wrapper is not a submodule at HEAD: ''\n$")

    def test_entry_point_exits_with_the_status_of_main(self):
        """The `if __name__ == "__main__"` line, in this process (so with
        the fake upstream): once passing, once failing."""
        for label, pins, status in (("in sync", {}, 0), ("out of sync", {"core": OTHER}, 1)):
            with self.subTest(label):
                self.reset(manifest=render_manifest(**pins))
                out = io.StringIO()
                with mock.patch.object(sys, "argv", [str(SCRIPT)]), contextlib.redirect_stdout(out):
                    with self.assertRaises(SystemExit) as stop:
                        runpy.run_path(str(SCRIPT), run_name="__main__")
                self.assertEqual(stop.exception.code, status, out.getvalue())


# ── GitHub annotations ──────────────────────────────────────────────────


class AnnotationFormatTests(CheckerTestCase):
    """Errors and warnings are GitHub workflow commands (`::error::text`):
    one line each, the message directly after the second `::`.

    CheckerTestCase.check() applies output_problems() to every run of every
    test in this file, so each failure scenario above is covered. The tests
    here pin the rule itself down and try it on the run with the most
    messages; git's multi-line output is tried in SdlTests.
    """

    def test_the_validator_rejects_malformed_output(self):
        good = "ok: fine\n::error::broken pin\n::warning::moved\nhint: do this\nfixed: that\n"
        self.assertEqual(output_problems(good), [])
        for label, bad in {
            "message with a line break": "::error::first half\nsecond half\n",
            "wrong case": "::Error::text\n",
            "legacy form": "##[error]text\n",
            "space before the message": "::error:: text\n",
            "no message": "::warning::\n",
            "plain text": "Traceback (most recent call last):\n",
            "blank line": "ok: fine\n\nok: fine\n",
            "no final newline": "ok: fine",
            "summary not last": "::warning::INCOMPLETE RUN: 1 check(s)\nok: fine\n",
        }.items():
            with self.subTest(label):
                self.assertNotEqual(output_problems(bad), [])

    def test_every_error_and_warning_is_a_one_line_annotation(self):
        # A run with as many different messages as one run can have.
        self.write(
            manifest=render_manifest(
                core=OTHER,
                wrapper=None,
                sdl_commit=None,
                ort="1.29.0",
                sherpa_files=tarballs(kissfft=None, eigen={"dest": "deps"}),
            ),
            lock=render_lock(packages(crate("zeroize", "1.8.2"), wrapper_version=None)),
            lock_commit="not a sha",
        )
        self.move_gitlink("Thirdparty/SDL", OTHER)
        run = self.check()
        self.assertEqual(run.code, 1)
        self.assertGreaterEqual(len(run.errors), 9, run.text)
        self.assertEqual(
            len(run.lines), len(run.errors) + len(run.oks) + len(run.starting("hint: "))
        )
        for line in run.errors:
            self.assertRegex(line, r"^::error::[A-Za-z`]")


# ── The helpers, one at a time ──────────────────────────────────────────


class ManifestParsingTests(CheckerTestCase):
    def test_manifest_git_pin_reads_commit_and_tag_per_source(self):
        self.assertEqual(cfp.manifest_git_pin("dasher-project/DasherCore"), (CORE, None))
        self.assertEqual(cfp.manifest_git_pin("libsdl-org/SDL"), (SDL, SDL_TAG))
        self.assertEqual(cfp.manifest_git_pin("AACTools/rust-tts-wrapper"), (WRAPPER, None))
        self.assertEqual(cfp.manifest_git_pin("k2-fsa/sherpa-onnx"), (SHERPA, SHERPA_TAG))

    def test_manifest_git_pin_without_such_a_source(self):
        self.assertEqual(cfp.manifest_git_pin("example/nothing"), (None, None))

    def test_manifest_git_pin_does_not_borrow_from_the_next_source(self):
        # DasherCore without a commit sits right above SDL, which has both.
        self.manifest(core=None)
        self.assertEqual(cfp.manifest_git_pin("dasher-project/DasherCore"), (None, None))
        # The sherpa-onnx git source is followed by ten file sources and by
        # the dasher module; none of their keys belong to it.
        self.manifest(sherpa_commit=None, sherpa_tag=None)
        self.assertEqual(cfp.manifest_git_pin("k2-fsa/sherpa-onnx"), (None, None))

    def test_manifest_git_pin_matches_only_git_sources(self):
        # kaldi-decoder is a file source whose url has k2-fsa in it.
        self.assertEqual(cfp.manifest_git_pin("k2-fsa/kaldi-decoder"), (None, None))

    def test_module_sources_of_the_sherpa_onnx_module(self):
        sources = cfp.module_sources("sherpa-onnx")
        self.assertEqual(sources[0], {"type": "git", "url": SHERPA_GIT})
        self.assertEqual(
            sources[1:],
            [
                {"type": "file", "url": d.url, "sha256": d.sha256, "dest-filename": d.filename}
                for d in DEPS
            ],
        )

    def test_module_sources_stop_at_the_next_module(self):
        sources = cfp.module_sources("onnxruntime")
        self.assertEqual([s["type"] for s in sources], ["archive", "file"])
        self.assertEqual(
            sources[1],
            {
                "type": "file",
                "url": "https://github.com/google/re2/archive/refs/tags/2024-07-02.zip",
                "sha256": sha256("re2"),
                "dest": "mirror/github.com/google/re2/archive/refs/tags",
                "dest-filename": "2024-07-02.zip",
            },
        )

    def test_module_sources_of_the_last_module_run_to_the_end(self):
        sources = cfp.module_sources("dasher")
        self.assertEqual([s["type"] for s in sources], ["dir", "git", "git", "git", "file"])
        self.assertEqual(
            sources[-1], {"type": "file", "dest": "rust-tts-wrapper", "dest-filename": "Cargo.lock"}
        )

    def test_module_sources_of_an_unknown_module(self):
        self.assertEqual(cfp.module_sources("nothing"), [])
        # A name is matched whole, not as a prefix.
        self.assertEqual(cfp.module_sources("sherpa"), [])

    def test_fix_manifest_pin_rewrites_only_that_commit(self):
        before = self.read(MANIFEST_NAME)
        self.assertIs(cfp.fix_manifest_pin("libsdl-org/SDL", OTHER), True)
        self.assertEqual(self.read(MANIFEST_NAME), before.replace(SDL, OTHER))
        self.assertIs(cfp.fix_manifest_pin("libsdl-org/SDL", OTHER), False)


class CmakeParsingTests(unittest.TestCase):
    def test_reads_name_url_sha256_and_local_file_name(self):
        for dep in DEPS:
            with self.subTest(dep.key):
                self.assertEqual(cfp.cmake_tarball_pins(cmake_text(dep)), [dep.pin])

    def test_reads_a_file_without_a_mirror_url(self):
        dep = DEP["eigen"]
        self.assertEqual(cfp.cmake_tarball_pins(cmake_text(dep, url2=False)), [dep.pin])

    def test_the_url_is_the_download_url_not_the_local_override(self):
        # set(<name>_URL "${f}") further down must not win.
        [(_, url, _, _)] = cfp.cmake_tarball_pins(cmake_text(DEP["json"]))
        self.assertEqual(url, DEP["json"].url)

    def test_without_a_pin_there_is_nothing(self):
        self.assertEqual(cfp.cmake_tarball_pins(""), [])
        self.assertEqual(cfp.cmake_tarball_pins('set(x_HASH "SHA256=abc")'), [])

    def test_file_name_is_unknown_when_names_and_pins_do_not_pair_up(self):
        dep = DEP["utf8proc"]
        text = cmake_text(dep).replace("${CMAKE_SOURCE_DIR}/", "${CMAKE_CURRENT_LIST_DIR}/")
        self.assertEqual(cfp.cmake_tarball_pins(text), [(dep.var, dep.url, dep.sha256, None)])
        # Two candidate names for one pin: do not guess.
        text = cmake_text(dep).replace(
            f"    /tmp/{dep.filename}\n",
            f"    ${{CMAKE_SOURCE_DIR}}/third_party/{dep.filename}\n",
        )
        self.assertEqual(cfp.cmake_tarball_pins(text), [(dep.var, dep.url, dep.sha256, None)])

    def test_missing_url_is_empty(self):
        dep = DEP["openfst"]
        text = cmake_text(dep).replace(
            f'set({dep.var}_URL  "https://', f'set({dep.var}_URL  "ftp://'
        )
        self.assertEqual(cfp.cmake_tarball_pins(text), [(dep.var, "", dep.sha256, dep.filename)])

    def test_two_pins_in_one_file_pair_up_in_order(self):
        first, second = DEP["kaldifst"], DEP["openfst"]
        self.assertEqual(
            cfp.cmake_tarball_pins(cmake_text(first) + cmake_text(second)), [first.pin, second.pin]
        )


class GitlinkTests(CheckerTestCase):
    def test_submodule_sha_reads_head(self):
        for path, commit in GITLINKS.items():
            self.assertEqual(cfp.submodule_sha(path), commit)

    def test_staged_submodule_sha_reads_the_index(self):
        self.move_gitlink("DasherCore", NEW_CORE, committed=False)
        self.assertEqual(cfp.staged_submodule_sha("DasherCore"), NEW_CORE)
        self.assertEqual(cfp.submodule_sha("DasherCore"), CORE)
        self.assertEqual(cfp.staged_submodule_sha("Thirdparty/SDL"), SDL)

    def test_a_regular_file_is_not_a_submodule(self):
        (self.repo / "README.md").write_text("hello\n")
        git(self.repo, "add", "README.md")
        commit_all(self.repo, "a file")
        self.assertRegex(
            self.stop_message(cfp.submodule_sha, "README.md"),
            r"^error: README.md is not a submodule at HEAD: '100644 blob ",
        )
        self.assertRegex(
            self.stop_message(cfp.staged_submodule_sha, "README.md"),
            r"^error: README.md is not a submodule in the index: ",
        )

    def test_a_path_that_does_not_exist_is_not_a_submodule(self):
        self.assertEqual(
            self.stop_message(cfp.submodule_sha, "no/such/path"),
            "error: no/such/path is not a submodule at HEAD: ''",
        )
        self.assertEqual(
            self.stop_message(cfp.staged_submodule_sha, "no/such/path"),
            "error: no/such/path is not a submodule in the index: ''",
        )


class FetchTests(CheckerTestCase):
    """fetch_text and remote_tag_commit share one contract: (value, None),
    (None, error) for an answer that is an error, (None, None) plus a
    recorded skip when there is no answer at all."""

    URL = f"https://{RAW_HOST}/example/repo/main/file.cmake"
    REMOTE = "https://github.com/example/repo.git"

    def call(self, function, *args):
        cfp.SKIPPED.clear()
        cfp.UNREACHABLE.clear()
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            result = function(*args)
        return result, out.getvalue()

    def test_fetch_text_returns_the_body(self):
        self.net.urls[self.URL] = "set(x 1)\n"
        self.assertEqual(self.call(cfp.fetch_text, self.URL, "x"), (("set(x 1)\n", None), ""))
        self.assertEqual((cfp.SKIPPED, cfp.UNREACHABLE), ([], {}))

    def test_fetch_text_survives_bytes_that_are_not_utf8(self):
        self.net.urls[self.URL] = b"caf\xe9\n"
        (text, error), _ = self.call(cfp.fetch_text, self.URL, "x")
        self.assertEqual((text, error), ("caf�\n", None))

    def test_fetch_text_returns_an_http_error_as_an_error(self):
        for status, reason in (
            (404, "Not Found"),
            (401, "Unauthorized"),
            (503, "Service Unavailable"),
        ):
            with self.subTest(status):
                self.net.urls[self.URL] = status
                result, printed = self.call(cfp.fetch_text, self.URL, "x")
                self.assertEqual(result, (None, f"HTTP {status} {reason}"))
                self.assertEqual((printed, cfp.SKIPPED, cfp.UNREACHABLE), ("", [], {}))

    def test_fetch_text_records_a_skip_when_the_host_gives_no_answer(self):
        self.net.down.add(RAW_HOST)
        result, printed = self.call(cfp.fetch_text, self.URL, "that x is y")
        self.assertEqual(result, (None, None))
        self.assertEqual(
            printed,
            f"::warning::Could not reach {self.URL} (<urlopen error [Errno 101] Network is "
            "unreachable>). SKIPPED, not verified: that x is y.\n",
        )
        self.assertEqual(cfp.SKIPPED, ["that x is y"])
        self.assertEqual(list(cfp.UNREACHABLE), [RAW_HOST])

    def test_fetch_text_does_not_ask_an_unreachable_host_again(self):
        self.net.down.add(RAW_HOST)
        self.call(cfp.fetch_text, self.URL, "first")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            result = cfp.fetch_text(self.URL + "2", "second")
        self.assertEqual(result, (None, None))
        self.assertEqual(self.net.http_calls, [self.URL])
        self.assertIn(f"({RAW_HOST} was unreachable earlier in this run)", out.getvalue())
        self.assertEqual(cfp.SKIPPED, ["first", "second"])
        # Another host is still asked.
        self.net.urls[info_refs(self.REMOTE)] = "refs"
        self.assertEqual(cfp.fetch_text(info_refs(self.REMOTE), "third"), ("refs", None))

    def test_fetch_text_uses_a_timeout(self):
        with mock.patch.object(
            urllib.request, "urlopen", return_value=FakeResponse(b"")
        ) as urlopen:
            cfp.fetch_text(self.URL, "x")
        urlopen.assert_called_once_with(self.URL, timeout=30)

    def test_remote_tag_commit_when_the_remote_or_the_host_answers(self):
        remote, refs = self.REMOTE, info_refs(self.REMOTE)
        # label: (what git answers, what HTTP answers once git has failed, the result)
        cases = {
            "lightweight tag": ({"v1": CORE, "v2": OTHER}, None, (CORE, None)),
            "annotated tag": ({"v1": (sha1("tag object"), CORE)}, None, (CORE, None)),
            "no such tag": ({"v9": CORE}, None, (None, f"{remote} has no tag v1")),
            "git fails, HTTP error": (
                "fatal: repository not found\n",
                401,
                (None, f"{remote} answered HTTP 401 Unauthorized"),
            ),
            "git fails, HTTP works": (
                "error: something else\n",
                "001e# service=git-upload-pack\n",
                (None, f"git ls-remote {remote} failed: error: something else"),
            ),
        }
        for label, (git_answer, http_answer, expected) in cases.items():
            with self.subTest(label):
                self.reset()
                self.net.remotes[remote] = git_answer
                if http_answer is not None:
                    self.net.urls[refs] = http_answer
                result, printed = self.call(cfp.remote_tag_commit, remote, "v1", "x")
                self.assertEqual((result, printed, cfp.SKIPPED), (expected, "", []))
                # HTTP is asked only when git has failed.
                self.assertEqual(self.net.http_calls, [] if http_answer is None else [refs])

    def test_remote_tag_commit_when_the_remote_cannot_be_reached(self):
        self.net.down.add(GIT_HOST)
        result, printed = self.call(cfp.remote_tag_commit, self.REMOTE, "v1", "that v1 is v1")
        self.assertEqual(result, (None, None))
        self.assertEqual(cfp.SKIPPED, ["that v1 is v1"])
        self.assertIn(f"::warning::Could not reach {info_refs(self.REMOTE)} (", printed)


# ── The real files of this repository ───────────────────────────────────


def real_gitlinks() -> dict:
    """The gitlinks recorded at HEAD of this repository, or {} when this
    is not a git checkout with the three submodules."""
    links = {}
    for path in GITLINKS:
        done = REAL_RUN(
            ["git", "-C", str(REPO), "ls-tree", "HEAD", path],
            capture_output=True,
            text=True,
            env=git_env(),
        )
        parts = done.stdout.split()
        if done.returncode != 0 or len(parts) < 3 or parts[1] != "commit":
            return {}
        links[path] = parts[2]
    return links


REAL_GITLINKS = real_gitlinks()
REAL_FILES = (MANIFEST_NAME, LOCK_NAME, LOCK_COMMIT_NAME, CARGO_SOURCES_NAME)


@unittest.skipUnless(REAL_GITLINKS, "not a git checkout with the submodule gitlinks at HEAD")
class RealRepoTests(CheckerTestCase):
    """The checker on this repository's own manifest, lock and crate list.

    Still hermetic: the upstream is a fake built from the manifest's own
    values (the tags resolve to the commits it pins, each cmake file pins
    the tarball the manifest has). So these do not show that the pins are
    right, which is what the check itself does online, but that the real
    files are internally consistent and have the layout the checker's
    parsing relies on, and that breaking them is noticed.
    """

    gitlinks = REAL_GITLINKS

    def setUp(self):
        super().setUp()
        for name in REAL_FILES:
            shutil.copyfile(REPO / name, self.repo / name)
        self.real_manifest = self.read(MANIFEST_NAME)
        self.net = self.upstream_of_the_manifest()

    def upstream_of_the_manifest(self) -> FakeNet:
        net = FakeNet()
        wrapper = REAL_GITLINKS["rust-tts-wrapper"]
        lock = tomllib.loads(self.read(LOCK_NAME))
        versions = {p["name"]: p["version"] for p in lock["package"]}
        net.urls[cargo_toml_url(wrapper)] = cargo_toml(versions["rust-tts-wrapper"])

        sdl_commit, sdl_tag = cfp.manifest_git_pin("libsdl-org/SDL")
        sherpa_commit, sherpa_tag = cfp.manifest_git_pin("k2-fsa/sherpa-onnx")
        net.remotes[SDL_GIT] = {sdl_tag: sdl_commit}
        net.remotes[SHERPA_GIT] = {sherpa_tag: sherpa_commit}

        ort = re.search(r'VERSION_NUMBER\)" = (\S+)', self.real_manifest).group(1)
        for path in ORT_CMAKE.values():
            net.urls[sherpa_raw(path, sherpa_commit)] = ort_cmake(ort)

        # One cmake file per tarball, pinning what the manifest has. The
        # manifest source of a dependency is the one whose file name starts
        # with the dependency's name.
        files = [s for s in cfp.module_sources("sherpa-onnx") if s["type"] == "file"]
        by_key = {}
        for dep in DEPS:
            [by_key[dep.key]] = [
                s for s in files if s.get("dest-filename", "").startswith(dep.key + "-")
            ]
        self.real_sources = by_key
        parents = {"kissfft": "kaldi-native-fbank", "kaldifst": "kaldi-decoder"}
        for dep in DEPS:
            source = by_key[dep.key]
            if dep.key in parents:
                repo, tag = re.fullmatch(
                    r"https://github\.com/([^/]+/[^/]+)/archive/refs/tags/(.+)\.tar\.gz",
                    by_key[parents[dep.key]]["url"],
                ).groups()
                url = f"https://{RAW_HOST}/{repo}/refs/tags/{tag}/{dep.cmake}"
            else:
                url = sherpa_raw(dep.cmake, sherpa_commit)
            net.urls[url] = cmake_text(
                dep, url=source["url"], digest=source["sha256"], filename=source["dest-filename"]
            )
        return net

    def test_the_real_files_pass(self):
        run = self.check()
        self.assert_passes(run)
        self.assertEqual(len(run.oks), 10, run.text)
        self.assertIn("ok: the 10 dependency tarballs", run.oks[8])
        self.assertIn("(2 of them pinned by its dependencies)", run.oks[8])
        self.assertRegex(run.oks[4], r"^ok: cargo-sources.json vendors the \d+ crates in ")

    def test_the_real_manifest_has_ten_tarballs_named_as_upstream_names_them(self):
        files = [s for s in cfp.module_sources("sherpa-onnx") if s["type"] == "file"]
        self.assertEqual(len(files), 10)
        self.assertEqual(
            sorted(s["dest-filename"] for s in files),
            sorted(s["dest-filename"] for s in self.real_sources.values()),
        )
        for source in files:
            self.assertNotIn("dest", source)
            self.assertRegex(source["sha256"], r"^[0-9a-f]{64}$")

    def test_the_real_files_pass_offline_with_every_upstream_check_skipped(self):
        self.net.down |= {RAW_HOST, GIT_HOST}
        run = self.check()
        self.assertEqual((run.code, len(run.skips)), (0, 13), run.text)

    def test_fix_has_nothing_to_do_on_the_real_manifest(self):
        run = self.check("--fix")
        self.assertEqual(run.code, 0)
        self.assertEqual(run.starting("fixed: "), [])
        self.assertEqual(self.read(MANIFEST_NAME), self.real_manifest)

    def test_fix_moves_the_real_dashercore_pin(self):
        self.move_gitlink("DasherCore", NEW_CORE, committed=False)
        run = self.check("--fix")
        self.assertEqual(
            run.starting("fixed: "), [f"fixed: DasherCore manifest pin -> {NEW_CORE[:12]}"]
        )
        self.assertEqual(
            self.read(MANIFEST_NAME),
            self.real_manifest.replace(REAL_GITLINKS["DasherCore"], NEW_CORE),
        )

    def edited(self, old: str, new: str, count: int = 1) -> str:
        """The real manifest with `old` replaced; `old` has to be there
        exactly `count` times, so an edit can not silently do nothing."""
        self.assertEqual(self.real_manifest.count(old), count, old)
        return self.real_manifest.replace(old, new)

    def test_breaking_the_real_manifest_is_caught(self):
        links = REAL_GITLINKS
        kissfft, kaldifst = self.real_sources["kissfft"], self.real_sources["kaldifst"]
        sdl_commit, sdl_tag = cfp.manifest_git_pin("libsdl-org/SDL")
        sherpa_commit, sherpa_tag = cfp.manifest_git_pin("k2-fsa/sherpa-onnx")
        ort = re.search(r'VERSION_NUMBER\)" = (\S+)', self.real_manifest).group(1)
        # label: (the edited manifest, a fragment of the one error expected)
        cases = {
            "DasherCore on another commit": (
                self.edited(f"commit: {links['DasherCore']}", f"commit: {OTHER}"),
                f"Flatpak manifest pins DasherCore at {OTHER[:12]}",
            ),
            "DasherCore back on a branch": (
                self.edited(f"commit: {links['DasherCore']}", "branch: main"),
                "No commit: pin found for the DasherCore git source",
            ),
            "rust-tts-wrapper commit removed": (
                self.edited(f"        commit: {links['rust-tts-wrapper']}\n", ""),
                "No commit: pin found for the rust-tts-wrapper git source",
            ),
            "SDL commit removed": (
                self.edited(f"        commit: {sdl_commit}\n", ""),
                NO_SDL_COMMIT + f"next to the SDL tag {sdl_tag}",
            ),
            "SDL commit cut short": (
                self.edited(f"commit: {sdl_commit}", f"commit: {sdl_commit[:39]}"),
                NO_SDL_COMMIT + f"next to the SDL tag {sdl_tag}",
            ),
            "SDL on another commit": (
                self.edited(f"commit: {sdl_commit}", f"commit: {OTHER}"),
                f"Manifest pins SDL tag {sdl_tag} with commit {OTHER[:12]}",
            ),
            "SDL tag removed": (
                self.edited(f"        tag: {sdl_tag}\n", ""),
                "No tag: pin found for the SDL git source",
            ),
            "sherpa-onnx commit removed": (
                self.edited(f"        commit: {sherpa_commit}\n", ""),
                "has no k2-fsa/sherpa-onnx git source with both tag: and commit:",
            ),
            "sherpa-onnx on another commit": (
                self.edited(f"commit: {sherpa_commit}", f"commit: {OTHER}"),
                f"Manifest pins sherpa-onnx tag {sherpa_tag} with commit {OTHER}",
            ),
            "ONNX Runtime version check removed": (
                self.edited(f'      - test "$(cat VERSION_NUMBER)" = {ort}\n', ""),
                "does not state the ONNX Runtime version it builds",
            ),
            "kissfft with another sha256": (
                self.edited(f"sha256: {kissfft['sha256']}", f"sha256: {sha256('other')}"),
                f"not sources of the sherpa-onnx module in {MANIFEST_NAME}: kissfft (",
            ),
            "kaldifst under another name": (
                self.edited(
                    f"dest-filename: {kaldifst['dest-filename']}", "dest-filename: kaldifst.tar.gz"
                ),
                "kaldifst: is 'kaldifst.tar.gz', kaldi-decoder ",
            ),
            "kaldifst in a subdirectory": (
                self.edited(
                    f"        dest-filename: {kaldifst['dest-filename']}\n",
                    f"        dest: deps\n        dest-filename: {kaldifst['dest-filename']}\n",
                ),
                f"kaldifst: is 'deps/{kaldifst['dest-filename']}'",
            ),
        }
        for label, (text, fragment) in cases.items():
            with self.subTest(label):
                # Upstream agrees with the manifest as it is in the repository.
                (self.repo / MANIFEST_NAME).write_text(self.real_manifest)
                self.net = self.upstream_of_the_manifest()
                (self.repo / MANIFEST_NAME).write_text(text)
                self.assert_one_error(self.check(), fragment)

    def test_breaking_the_real_cargo_inputs_is_caught(self):
        sources = json.loads(self.read(CARGO_SOURCES_NAME))
        archives = [s for s in sources if s.get("type") == "archive"]
        cases = {
            "lock commit of another commit": (LOCK_COMMIT_NAME, OTHER + "\n", "was generated for"),
            "a crate dropped from the sources": (
                CARGO_SOURCES_NAME,
                json.dumps([s for s in sources if s is not archives[0]]),
                "(1 crates missing, 0 stale; 0 git dependencies missing, 0 stale)",
            ),
        }
        for label, (name, text, fragment) in cases.items():
            with self.subTest(label):
                original = self.read(name)
                (self.repo / name).write_text(text)
                try:
                    self.assert_one_error(self.check(), fragment)
                finally:
                    (self.repo / name).write_text(original)


# ── Opt-in: the real checker against the real upstreams ─────────────────


@unittest.skipUnless(
    os.environ.get("DASHER_PIN_SYNC_LIVE") == "1",
    "live test: set DASHER_PIN_SYNC_LIVE=1 to run the checker against the real upstreams",
)
class LiveTests(unittest.TestCase):
    def test_the_repository_passes_online_with_nothing_skipped(self):
        done = REAL_RUN(
            [sys.executable, "-I", str(SCRIPT)],
            cwd=REPO,
            capture_output=True,
            text=True,
            timeout=600,
        )
        report = done.stdout + done.stderr
        self.assertEqual(done.returncode, 0, report)
        self.assertNotIn("::error::", report)
        self.assertNotIn("INCOMPLETE RUN", report, "a check was skipped: is the network up?")
        self.assertNotIn("SKIPPED", report)
        self.assertEqual(output_problems(done.stdout), [], report)
        self.assertEqual(len([l for l in done.stdout.splitlines() if l.startswith("ok: ")]), 10)


if __name__ == "__main__":
    unittest.main()
