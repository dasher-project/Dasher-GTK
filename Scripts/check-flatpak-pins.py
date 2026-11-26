#!/usr/bin/env python3
"""Verify the Flatpak manifest's submodule pins follow the submodules.

The Flatpak manifest (packaging/flatpak/org.alternativeinterface.dasher.yaml)
builds from the working tree but clones the submodules from their own git
sources with hand-maintained pins. When a submodule bumps and the manifest
pin is forgotten, the Flatpak build compiles a stale dependency against the
new app code — exactly how the v6.0.2 publish failed (rust-tts-wrapper
pinned at 1979957 while the C++ expected the 1.16.2 FFI surface).

Checks:
  HARD FAIL  rust-tts-wrapper, DasherCore: the manifest's `commit:` must
             equal the submodule's recorded gitlink SHA.
  HARD FAIL  SDL: the manifest pins a tag plus the commit it resolves to;
             the two must agree upstream (flatpak-builder refuses a tag
             that does not resolve to the pinned commit).
  WARNING    SDL: if the submodule has moved past the pinned tag the build
             still works, but the log says so.
  HARD FAIL  Offline cargo inputs: the Flatpak build has no network, so
             rust-tts-wrapper-Cargo.lock must be the lock for the pinned
             rust-tts-wrapper: rust-tts-wrapper-Cargo.lock.commit records
             the commit it was generated for and must equal the submodule
             SHA, and the crate version must agree with Cargo.toml at
             that commit (which must exist upstream). cargo-sources.json
             must vendor exactly the sources in that lock (crates.io
             crates by checksum, git dependencies by commit).
  HARD FAIL  From-source speech stack: sherpa-onnx-sys links the
             sherpa-onnx and ONNX Runtime libraries the manifest builds,
             so the sherpa-onnx git source must be the tag of the locked
             sherpa-onnx-sys version (tag and commit agreeing upstream),
             the ONNX Runtime version the manifest builds must be the
             one that sherpa-onnx release pins, and every dependency
             tarball sherpa-onnx pins by SHA256 in its cmake files must
             be a source of the sherpa-onnx module, under the file name
             that cmake looks for. That includes the two tarballs pinned
             one level down: kissfft (by kaldi-native-fbank) and
             kaldifst (by kaldi-decoder), read at the versions
             sherpa-onnx pins.

Network: the upstream lookups need GitHub. An answer that is an error
(HTTP 404, a tag that does not exist) FAILS the check. Only when a host
cannot be reached at all is the check that needs it skipped with a
warning; the run then ends with a line that says it was incomplete and
lists what was not verified. It still exits 0, so read that line.
What the repository alone can answer is checked before any lookup and
fails the run with or without network: a `tag:` or `commit:` pin that is
missing from the manifest (or a commit value that is not exactly one full
SHA), a lock with no rust-tts-wrapper package, a manifest that does not
state its ONNX Runtime version.

The manifest is read with a YAML parser (PyYAML, with libyaml where it is
built in: the parser flatpak-builder uses), never line by line. A pin is
the value the parser hands on, however it is written: a SHA with a comment
after it, a quoted SHA, a SHA in a flow mapping or behind an alias all pin
that SHA, and a SHA with a continuation line, a second word or a line break
after it does not.
A manifest that is not valid YAML, or that has the same key twice in one
mapping (parsers take the last one, readers the first), stops the run.

Runs from the repo root with no arguments. Needs PyYAML (python3-yaml on
Debian and Ubuntu, which the GitHub runner images ship); without it the
run stops and says so.

  --fix   Rewrite the hard-pinned `commit:` lines (rust-tts-wrapper,
          DasherCore) to the submodule SHAs staged in the index. For
          submodule bumps: `git add <submodule>`, run with --fix,
          `git add` the manifest. The shared-deps bump workflow does
          exactly this. Only the SHA is replaced, where the parser found
          it, and the result is parsed again before it is written: it
          has to differ from the manifest in that one value. A value
          written as anything but a plain or quoted SHA (a block scalar,
          an anchor, an alias) is not rewritten; --fix stops and says
          so. --fix cannot regenerate the offline cargo
          inputs: when the staged rust-tts-wrapper is not the commit
          the lock was generated for, it says so and exits non-zero,
          and the check keeps failing until they are regenerated.
"""

import json
import os
import re
import subprocess
import sys
import tomllib
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

try:
    import yaml
except ImportError:  # parse_manifest() stops the run and says what to install
    yaml = None

MANIFEST = Path("packaging/flatpak/org.alternativeinterface.dasher.yaml")
# rust-tts-wrapper commits no Cargo.lock; the manifest ships this one and
# the crate list generated from it (see the comments in the manifest).
CARGO_LOCK = MANIFEST.with_name("rust-tts-wrapper-Cargo.lock")
CARGO_SOURCES = MANIFEST.with_name("cargo-sources.json")
# The rust-tts-wrapper commit CARGO_LOCK was generated for. The crate
# version alone cannot tell a stale lock: dependencies change between
# commits that share a version.
CARGO_LOCK_COMMIT = MANIFEST.with_name("rust-tts-wrapper-Cargo.lock.commit")
REGENERATE = (
    "Regenerate packaging/flatpak/rust-tts-wrapper-Cargo.lock, "
    "rust-tts-wrapper-Cargo.lock.commit and cargo-sources.json (commands "
    "are next to the sources in the manifest)."
)
CRATES_IO = "registry+https://github.com/rust-lang/crates.io-index"
SHERPA_REPO = "k2-fsa/sherpa-onnx"
# sherpa-onnx cmake files (at the pinned commit) that say which ONNX Runtime
# release it is built against, one per Flatpak arch.
SHERPA_ORT_CMAKE = {
    "x86_64": "cmake/onnxruntime-linux-x86_64-static.cmake",
    "aarch64": "cmake/onnxruntime-linux-aarch64-static.cmake",
}
# sherpa-onnx cmake files that pin a dependency tarball by SHA256 and are
# used by the manifest's build (static libs, no binaries, no websocket,
# no portaudio, no tests, no python). Each pinned tarball must be a source
# of the sherpa-onnx module.
SHERPA_DEP_CMAKE = [
    "cmake/kaldi-native-fbank.cmake",
    "cmake/kaldi-decoder.cmake",
    "cmake/openfst.cmake",
    "cmake/eigen.cmake",
    "cmake/utf8proc.cmake",
    "cmake/simple-sentencepiece.cmake",
    "cmake/json.cmake",
    "cmake/hclust-cpp.cmake",
]
# Tarballs pinned one level down: the dependency (named by the sherpa-onnx
# cmake file that pins it) fetches one more tarball from a cmake file of
# its own, with the same local-file lookup. Read at the tag sherpa-onnx
# pins for the dependency.
SHERPA_TRANSITIVE_DEP_CMAKE = {
    "cmake/kaldi-native-fbank.cmake": ["cmake/kissfft.cmake"],
    "cmake/kaldi-decoder.cmake": ["cmake/kaldifst.cmake"],
}

# What could not be verified because a host was unreachable (no HTTP
# answer at all). Reported at the end of the run; see "Network" above.
SKIPPED: list[str] = []
# host -> error, so a network that is down costs one timeout per host and
# not one per lookup.
UNREACHABLE: dict[str, str] = {}

# A commit pin: the whole value, as YAML reads it, is one full SHA.
# `commit: <sha>0` and `commit: <sha> v0.2.28` are not pins on <sha>, and
# neither is a SHA with an indented line after it, which YAML folds into
# the value: flatpak-builder is handed the whole value and fails on it.
FULL_SHA = re.compile(r"[0-9a-f]{40}")
# What module_sources() reports of a source.
SOURCE_KEYS = ("type", "url", "sha256", "dest", "dest-filename")

# Submodules whose manifest `commit:` must equal the submodule gitlink:
# (display name, submodule path, fragment of the manifest source url).
HARD_PINS = [
    ("rust-tts-wrapper", "rust-tts-wrapper", "AACTools/rust-tts-wrapper"),
    ("DasherCore", "DasherCore", "dasher-project/DasherCore"),
]


def submodule_sha(path: str) -> str:
    """The gitlink SHA recorded for `path` at HEAD (no checkout needed)."""
    out = subprocess.run(
        ["git", "ls-tree", "HEAD", path], capture_output=True, text=True, check=True
    ).stdout.strip()
    # "<mode> <type> <sha>\t<path>" — type must be commit (a gitlink)
    parts = out.split()
    if len(parts) < 3 or parts[1] != "commit":
        raise SystemExit(f"error: {path} is not a submodule at HEAD: {out!r}")
    return parts[2]


def staged_submodule_sha(path: str) -> str:
    """The gitlink SHA staged in the index for `path` (what the next commit
    will record), so --fix works between `git add <submodule>` and commit."""
    out = subprocess.run(
        ["git", "ls-files", "--stage", "--", path], capture_output=True, text=True, check=True
    ).stdout.strip()
    # "<mode> <sha> <stage>\t<path>"; mode 160000 is a gitlink
    parts = out.split()
    if len(parts) < 3 or parts[0] != "160000":
        raise SystemExit(f"error: {path} is not a submodule in the index: {out!r}")
    return parts[1]


class Scalar(str):
    """A scalar of the manifest as YAML reads it, with the node it was read
    from: --fix needs to know where the value is written, and how."""

    node = None


def manifest_text() -> str:
    """The manifest, character for character (line endings included)."""
    with open(MANIFEST, encoding="utf-8", newline="") as f:
        return f.read()


def parse_manifest(text: str):
    """`text` as a YAML parser reads it: mappings as dicts, sequences as
    lists and every scalar as a string (a Scalar), with aliases resolved.
    No value is given a type, which is how a SHA of forty digits stays a
    string. Stops the run when PyYAML is missing, when the text is not one
    valid YAML document, and when a mapping has the same key twice: YAML
    parsers keep the last value and a reader sees the first, so there is no
    saying which one is meant."""
    if yaml is None:
        raise SystemExit(
            f"error: PyYAML is not installed, and {MANIFEST} is read with it (a line by "
            "line reading kept taking for a pin what YAML reads as something else). "
            "Install python3-yaml (Debian, Ubuntu) or `pip install pyyaml`."
        )
    try:
        # libyaml where PyYAML is built with it: the parser flatpak-builder uses.
        root = yaml.compose(text, Loader=getattr(yaml, "CBaseLoader", yaml.BaseLoader))
    except yaml.YAMLError as e:
        raise SystemExit(
            f"error: {MANIFEST} is not valid YAML: {' '.join(str(e).split())}"
        ) from None
    open_nodes = []

    def build(node):
        if isinstance(node, yaml.ScalarNode):
            scalar = Scalar(node.value)
            scalar.node = node
            return scalar
        line = node.start_mark.line + 1
        if any(node is seen for seen in open_nodes):
            raise SystemExit(f"error: {MANIFEST} line {line}: an alias that contains itself")
        open_nodes.append(node)
        if isinstance(node, yaml.SequenceNode):
            data = [build(item) for item in node.value]
        else:
            data = {}
            for key_node, value_node in node.value:
                key = build(key_node)
                if not isinstance(key, str):
                    raise SystemExit(
                        f"error: {MANIFEST} line {key_node.start_mark.line + 1}: a mapping "
                        "key that is not a scalar"
                    )
                if key in data:
                    raise SystemExit(
                        f"error: {MANIFEST} line {key_node.start_mark.line + 1}: the key "
                        f"`{key}` is in this mapping twice (the mapping starts on line "
                        f"{line}). A YAML parser keeps the last one and a reader sees the "
                        "first: remove one."
                    )
                data[key] = build(value_node)
        open_nodes.pop()
        return data

    return {} if root is None else build(root)


def load_manifest():
    """The manifest as a YAML parser reads it (see parse_manifest)."""
    return parse_manifest(manifest_text())


def entries(node, key: str) -> list[dict]:
    """The mappings in the sequence `node[key]`: the modules of a manifest
    or of a module, the sources of a module. Nothing when `node` is not a
    mapping or the value not a sequence; entries that are not mappings (a
    module or source given as a file name) are left out."""
    value = node.get(key) if isinstance(node, dict) else None
    return [entry for entry in value if isinstance(entry, dict)] if isinstance(value, list) else []


def git_source(document, url_fragment: str) -> dict | None:
    """The first `type: git` source, of any module or module in a module,
    whose url contains `url_fragment`. None when there is none."""
    found = []

    def walk(parent):
        for module in entries(parent, "modules"):
            found.extend(s for s in entries(module, "sources") if s.get("type") == "git")
            walk(module)

    walk(document)
    for source in found:
        url = source.get("url")
        if isinstance(url, str) and url_fragment in url:
            return source
    return None


def full_sha(value) -> str | None:
    """`value` when it is one full SHA and nothing else, as a plain string."""
    return str(value) if isinstance(value, str) and FULL_SHA.fullmatch(value) else None


def fix_manifest_pin(url_fragment: str, sha: str) -> bool:
    """Point the `commit:` of the git source whose url contains
    `url_fragment` at `sha`. Returns True when the manifest changed. Only
    the SHA is replaced, where the YAML parser found it: indentation, line
    endings, quotes and a trailing comment stay as they are. The new text
    is parsed again before it is written and has to be the old manifest
    with that one value changed."""
    text = manifest_text()
    document = parse_manifest(text)
    source = git_source(document, url_fragment)
    if source is None:
        raise SystemExit(f"error: no git source for {url_fragment} in {MANIFEST}")
    commit = source.get("commit")
    if full_sha(commit) is None:
        raise SystemExit(
            f"error: no commit: pin to fix for {url_fragment} in {MANIFEST} (its value has "
            "to be one full SHA as YAML reads it)"
        )
    if commit == sha:
        return False
    node = commit.node
    start, end = node.start_mark.index, node.end_mark.index
    quote = node.style if node.style in ("'", '"') else ""
    fixed = text[:start] + quote + sha + quote + text[end:]
    source["commit"] = sha  # `document` is now what the fixed text has to parse to
    if text[start:end] != quote + commit + quote or parse_manifest(fixed) != document:
        raise SystemExit(
            f"error: cannot rewrite the commit: pin for {url_fragment} in {MANIFEST} (line "
            f"{node.start_mark.line + 1}): it is not written as a plain or quoted SHA where "
            "it is used (a block scalar, an anchor, an alias or a tag?), and replacing it "
            f"there would change more than this pin. Set it to {sha} by hand."
        )
    with open(MANIFEST, "w", encoding="utf-8", newline="") as f:
        f.write(fixed)
    return True


def manifest_git_pin(url_fragment: str) -> tuple[str | None, str | None]:
    """The commit and tag the manifest pins for the git source whose url
    contains `url_fragment`, as YAML reads them. Returns (commit, tag);
    either may be None: a commit that is not one full SHA is no pin."""
    source = git_source(load_manifest(), url_fragment) or {}
    tag = source.get("tag")
    return (full_sha(source.get("commit")), str(tag) if isinstance(tag, str) and tag else None)


def check_lock_commit(wrapper_sha: str) -> bool:
    """The lock was generated for `wrapper_sha`, going by the commit recorded
    next to it. Returns True when it was not (or nothing is recorded)."""
    recorded = CARGO_LOCK_COMMIT.read_text().strip() if CARGO_LOCK_COMMIT.exists() else ""
    if not re.fullmatch(r"[0-9a-f]{40}", recorded):
        print(
            f"::error::{CARGO_LOCK_COMMIT} does not hold a commit SHA, so nothing says "
            f"which rust-tts-wrapper commit {CARGO_LOCK.name} was generated for. {REGENERATE}"
        )
        return True
    if recorded != wrapper_sha:
        print(
            f"::error::{CARGO_LOCK.name} was generated for rust-tts-wrapper "
            f"{recorded[:12]} but the submodule is at {wrapper_sha[:12]}. Its "
            "dependencies may have changed, and the Flatpak build is offline and "
            f"would fail to resolve crates. {REGENERATE}"
        )
        return True
    print(f"ok: {CARGO_LOCK.name} was generated for the pinned rust-tts-wrapper ({recorded[:12]})")
    return False


def skip(what: str, url: str, error: str) -> None:
    """Record (and warn) that `what` was not verified: `url` gave no HTTP
    answer."""
    print(f"::warning::Could not reach {url} ({error}). SKIPPED, not verified: {what}.")
    SKIPPED.append(what)


def fetch_text(url: str, what: str) -> tuple[str | None, str | None]:
    """(text, None) for a readable URL; (None, error) for an HTTP error,
    which means the file is not there and callers fail on; (None, None)
    when the host cannot be reached at all: `what` (the check that needed
    the file) is then recorded as skipped, with a warning."""
    host = urllib.parse.urlsplit(url).netloc
    if host in UNREACHABLE:
        skip(what, url, f"{host} was unreachable earlier in this run")
        return None, None
    try:
        with urllib.request.urlopen(url, timeout=30) as response:
            return response.read().decode(errors="replace"), None
    except urllib.error.HTTPError as e:
        return None, f"HTTP {e.code} {e.reason}"
    except OSError as e:  # no HTTP response: DNS, connection, timeout
        UNREACHABLE[host] = str(e)
        skip(what, url, str(e))
        return None, None


def remote_tag_commit(url: str, tag: str, what: str) -> tuple[str | None, str | None]:
    """The commit `tag` points at in the git remote `url` (an annotated tag
    is peeled to its commit). Same contract as fetch_text: (commit, None);
    (None, error) when the remote answered and the tag is not there, or it
    answered with an error; (None, None) when the remote cannot be reached
    at all, with `what` recorded as skipped."""
    result = subprocess.run(
        ["git", "ls-remote", url, f"refs/tags/{tag}", f"refs/tags/{tag}^{{}}"],
        capture_output=True,
        text=True,
        env={**os.environ, "GIT_TERMINAL_PROMPT": "0"},
    )
    if result.returncode == 0:
        refs = {}
        for line in result.stdout.splitlines():
            sha, _, ref = line.partition("\t")
            refs[ref] = sha
        commit = refs.get(f"refs/tags/{tag}^{{}}") or refs.get(f"refs/tags/{tag}")
        return (commit, None) if commit else (None, f"{url} has no tag {tag}")
    # git failed. Its messages do not say reliably whether the remote
    # answered, so ask the same endpoint over HTTP and apply the same rule
    # as everywhere else: an HTTP error fails, no answer at all is a skip.
    text, error = fetch_text(f"{url}/info/refs?service=git-upload-pack", what)
    if error:
        return None, f"{url} answered {error}"
    if text is None:
        return None, None
    stderr = " ".join(result.stderr.split()) or f"exit status {result.returncode}"
    return None, f"git ls-remote {url} failed: {stderr}"


def cmake_tarball_pins(text: str) -> list[tuple[str, str, str, str | None]]:
    """The tarballs a sherpa-onnx style cmake file pins, as (name, url,
    sha256, local file name). The files all follow one pattern:
        set(<name>_URL  "https://...")
        set(<name>_HASH "SHA256=...")
        set(possible_file_locations ... ${CMAKE_SOURCE_DIR}/<file name> ...)
    and the tarball is taken from the first location that exists, else
    downloaded. The file name is None when it cannot be told apart."""
    hashes = re.findall(r'set\(([\w-]+)_HASH\s+"SHA256=([0-9a-f]{64})"\)', text)
    names = list(dict.fromkeys(re.findall(r"\$\{CMAKE_SOURCE_DIR\}/([^\s)]+)", text)))
    pins = []
    for i, (name, digest) in enumerate(hashes):
        m_url = re.search(rf'set\({re.escape(name)}_URL\s+"(https?://[^"]+)"\)', text)
        local = names[i] if len(names) == len(hashes) else None
        pins.append((name, m_url.group(1) if m_url else "", digest, local))
    return pins


def module_sources(module: str) -> list[dict[str, str]]:
    """The sources of the manifest module `module`, each as a dict of its
    type, url, sha256, dest and dest-filename (those that are present; a
    value that is not a scalar is kept, as text, so that it is not taken
    for a key that is not there)."""
    for entry in entries(load_manifest(), "modules"):
        if entry.get("name") == module:
            return [
                {key: str(source[key]) for key in SOURCE_KEYS if key in source}
                for source in entries(entry, "sources")
            ]
    return []


def onnxruntime_version() -> str | None:
    """The ONNX Runtime version the manifest says it builds: the onnxruntime
    module asserts it against the source in a build command
    (`test "$(cat VERSION_NUMBER)" = <version>`). None when it does not."""
    for entry in entries(load_manifest(), "modules"):
        if entry.get("name") == "onnxruntime":
            commands = entry.get("build-commands")
            commands = commands if isinstance(commands, list) else []
            m = re.search(
                r'VERSION_NUMBER\)" = ([0-9]+(?:\.[0-9]+)+)',
                "\n".join(command for command in commands if isinstance(command, str)),
            )
            return m.group(1) if m else None
    return None


def check_speech_stack(sherpa_version: str) -> bool:
    """The sherpa-onnx and ONNX Runtime the manifest builds from source are
    the ones the locked sherpa-onnx-sys `sherpa_version` expects. Returns
    True when something is out of sync."""
    failed = False
    url = f"https://github.com/{SHERPA_REPO}.git"
    commit, tag = manifest_git_pin(SHERPA_REPO)
    wanted_tag = f"v{sherpa_version}"
    if commit is None or tag is None:
        print(
            f"::error::{MANIFEST} has no {SHERPA_REPO} git source with both tag: and "
            f"commit:. sherpa-onnx-sys {sherpa_version} links the sherpa-onnx the manifest "
            f"builds from source; pin tag {wanted_tag} and its commit."
        )
        return True
    if tag != wanted_tag:
        print(
            f"::error::{CARGO_LOCK.name} locks sherpa-onnx-sys {sherpa_version} but the "
            f"manifest builds sherpa-onnx {tag}. The Rust bindings and the C library must "
            f"be the same release: pin tag {wanted_tag} with its commit, then update the "
            "dependency tarballs and the ONNX Runtime version to what that release pins."
        )
        failed = True
    # ONNX Runtime: the onnxruntime module asserts the version of the source
    # it builds (`test "$(cat VERSION_NUMBER)" = <version>`). Whether it does
    # is read from the manifest, so say so before the upstream lookups: an
    # early return or a skipped lookup below must not hide it.
    ort = onnxruntime_version()
    if ort is None:
        print(
            f"::error::{MANIFEST} does not state the ONNX Runtime version it builds "
            '(expected `test "$(cat VERSION_NUMBER)" = <version>` in the onnxruntime module).'
        )
        failed = True
    # The checks further down read sherpa-onnx's files at the pinned commit,
    # so stop here when tag and commit do not belong together.
    remote, error = remote_tag_commit(
        url, tag, f"that sherpa-onnx tag {tag} resolves to the pinned commit upstream"
    )
    if error:
        print(
            f"::error::sherpa-onnx tag {tag} pinned in the manifest does not resolve "
            f"upstream ({error})."
        )
        return True
    if remote is not None and remote != commit:
        print(
            f"::error::Manifest pins sherpa-onnx tag {tag} with commit {commit} but the "
            f"tag resolves to {remote} upstream. flatpak-builder refuses that; update "
            "the commit together with the tag."
        )
        return True
    if not failed and remote is not None:
        print(f"ok: sherpa-onnx source is {tag} ({commit[:12]}), matching sherpa-onnx-sys")

    raw = f"https://raw.githubusercontent.com/{SHERPA_REPO}/{commit}"

    # The ONNX Runtime version the manifest states (read above) must be the
    # release sherpa-onnx is built against upstream.
    if ort is not None:
        for arch, path in SHERPA_ORT_CMAKE.items():
            text, error = fetch_text(
                f"{raw}/{path}",
                f"that ONNX Runtime {ort} is what sherpa-onnx {tag} pins ({arch})",
            )
            if error:
                print(f"::error::{path} is not readable at sherpa-onnx {commit[:12]} ({error}).")
                failed = True
            elif text is not None:
                pinned = set(re.findall(r"onnxruntime-libs/releases/download/v([0-9.]+)/", text))
                if pinned != {ort}:
                    print(
                        f"::error::sherpa-onnx {tag} is built against ONNX Runtime "
                        f"{', '.join(sorted(pinned)) or '(not found)'} on {arch} but the manifest "
                        f"builds {ort}. Move the onnxruntime module (source, version "
                        "check and its cmake/deps.txt mirror files) to that release."
                    )
                    failed = True
                else:
                    print(f"ok: ONNX Runtime {ort} is what sherpa-onnx {tag} pins ({arch})")

    # Dependency tarballs: cmake looks for each one next to sherpa-onnx's
    # CMakeLists.txt under a fixed file name, verifies its SHA256, and
    # downloads it when the file is not there. A missing, different or
    # differently named file therefore only fails in the (offline) Flatpak
    # build. Catch it here.
    #
    # pins: (what pins it, tarball name, sha256, file name cmake looks for)
    pins: list[tuple[str, str, str, str | None]] = []
    complete = True
    deps_failed = False

    def read_pins(cmake_url: str, where: str) -> list[tuple[str, str, str, str | None]]:
        nonlocal deps_failed, complete
        text, error = fetch_text(cmake_url, f"the dependency tarball pinned in {where}")
        if error:
            print(f"::error::{where} is not readable ({error} for {cmake_url}).")
            deps_failed = True
            return []
        if text is None:
            complete = False
            return []
        found = cmake_tarball_pins(text)
        if not found:
            print(
                f"::error::No SHA256-pinned tarball found in {where}. Its layout has "
                "changed and Scripts/check-flatpak-pins.py no longer understands it."
            )
            deps_failed = True
        pins.extend((where, name, digest, local) for name, _, digest, local in found)
        return found

    for path in SHERPA_DEP_CMAKE:
        found = read_pins(f"{raw}/{path}", f"sherpa-onnx {tag} {path}")
        for child in SHERPA_TRANSITIVE_DEP_CMAKE.get(path, []):
            # One level down: `child` is a file of the dependency itself, at
            # the release this sherpa-onnx pins (its tag, from the tarball URL).
            for name, dep_url, _, _ in found:
                m_dep = re.fullmatch(
                    r"https://github\.com/([^/]+/[^/]+)/archive/refs/tags/(.+?)\.(?:tar\.gz|zip)",
                    dep_url,
                )
                if not m_dep:
                    print(
                        f"::error::Cannot tell which release of {name} sherpa-onnx {tag} "
                        f"pins ({path}: {dep_url or 'no URL found'}), so the tarball its "
                        f"{child} pins was not checked. Teach Scripts/check-flatpak-pins.py "
                        "the new URL form."
                    )
                    deps_failed = True
                    continue
                dep_repo, dep_tag = m_dep.groups()
                read_pins(
                    f"https://raw.githubusercontent.com/{dep_repo}/refs/tags/{dep_tag}/{child}",
                    f"{dep_repo.split('/')[1]} {dep_tag} {child}",
                )

    sources = {s["sha256"]: s for s in module_sources("sherpa-onnx") if "sha256" in s}
    missing, misplaced = [], []
    for where, name, digest, local in pins:
        source = sources.get(digest)
        if source is None:
            missing.append(f"{name} ({where}, sha256 {digest[:12]}...)")
            continue
        if local is None:
            print(
                f"::error::Cannot tell which file name cmake expects for {name} ({where}). "
                "Its layout has changed and Scripts/check-flatpak-pins.py no longer "
                "understands it."
            )
            deps_failed = True
            continue
        # A `file` source lands in the source root as dest-filename (or the
        # last part of the url); anything else is not where cmake looks.
        placed = source.get("dest-filename") or source.get("url", "").rsplit("/", 1)[-1]
        kind = source.get("type", "(none)")
        if kind != "file" or "dest" in source or placed != local:
            actual = (
                f"type: {kind}"
                if kind != "file"
                else f"{source['dest']}/{placed}"
                if "dest" in source
                else placed
            )
            misplaced.append(f"{name}: is '{actual}', {where} looks for '{local}'")
    if missing:
        print(
            f"::error::sherpa-onnx {tag} needs dependency tarballs that are not sources of "
            f"the sherpa-onnx module in {MANIFEST}: {', '.join(missing)}. The build is "
            "offline and cmake would fail to download them; add them next to the "
            "sherpa-onnx source (url and sha256 are in the named cmake file)."
        )
        deps_failed = True
    if misplaced:
        print(
            f"::error::Dependency tarballs of the sherpa-onnx module in {MANIFEST} are not "
            f"where cmake looks for them: {'; '.join(misplaced)}. cmake only uses a local "
            "copy under exactly that name next to sherpa-onnx's CMakeLists.txt; otherwise "
            "it tries to download, which fails in the offline build. Make each a "
            "`type: file` source with that `dest-filename:` and no `dest:`."
        )
        deps_failed = True
    if pins and complete and not deps_failed:
        transitive = sum(1 for where, *_ in pins if not where.startswith("sherpa-onnx "))
        print(
            f"ok: the {len(pins)} dependency tarballs sherpa-onnx {tag} needs ({transitive} "
            "of them pinned by its dependencies) are sources of the sherpa-onnx module, "
            "under the file names cmake looks for"
        )
    return failed or deps_failed


def check_cargo_inputs(wrapper_sha: str) -> bool:
    """The offline cargo inputs agree with each other and with the pinned
    rust-tts-wrapper. Returns True when something is out of sync."""
    failed = check_lock_commit(wrapper_sha)
    packages = tomllib.loads(CARGO_LOCK.read_text())["package"]
    versions = {p["name"]: p["version"] for p in packages}
    # Read from the lock alone, so it fails before the upstream lookup and
    # whether or not GitHub can be reached.
    locked = versions.get("rust-tts-wrapper")
    if locked is None:
        print(
            f"::error::{CARGO_LOCK.name} has no rust-tts-wrapper package, so it is not "
            f"the lock of the pinned rust-tts-wrapper. {REGENERATE}"
        )
        failed = True

    # Cross-check the recorded commit against the lock itself: the crate
    # version in the lock must be the one in Cargo.toml at the pinned commit
    # (the submodule is not checked out in CI, so read it from GitHub). This
    # also proves the commit exists upstream, where the Flatpak build clones
    # it from: an HTTP error (404 for a commit that was never pushed) fails.
    # Only a failure to reach GitHub at all is let through with a warning.
    url = f"https://raw.githubusercontent.com/AACTools/rust-tts-wrapper/{wrapper_sha}/Cargo.toml"
    try:
        with urllib.request.urlopen(url, timeout=30) as response:
            pinned = tomllib.loads(response.read().decode())["package"]["version"]
    except urllib.error.HTTPError as e:
        print(
            f"::error::rust-tts-wrapper's Cargo.toml at {wrapper_sha[:12]} is not readable "
            f"upstream (HTTP {e.code} {e.reason} for {url}). Is the commit pushed to "
            "AACTools/rust-tts-wrapper? The Flatpak build clones it from there."
        )
        failed = True
    except OSError as e:  # no HTTP response: DNS, connection, timeout
        UNREACHABLE[urllib.parse.urlsplit(url).netloc] = str(e)
        skip(
            f"that {CARGO_LOCK.name} has the crate version of rust-tts-wrapper "
            f"{wrapper_sha[:12]}, and that the commit exists upstream",
            url,
            str(e),
        )
    else:
        if locked is None:
            pass  # reported above
        elif locked != pinned:
            print(
                f"::error::{CARGO_LOCK.name} was generated for rust-tts-wrapper {locked} "
                f"but the pinned commit is {pinned}. The Flatpak build is offline and "
                f"would fail to resolve crates. {REGENERATE}"
            )
            failed = True
        else:
            print(f"ok: {CARGO_LOCK.name} has the pinned rust-tts-wrapper's version ({pinned})")

    # cargo-sources.json vendors exactly the sources the lock names:
    # crates.io crates by checksum, git dependencies by commit. A package
    # with no source is a workspace member or path dependency and comes
    # with the checkout. Anything else could not be compared, so it fails.
    locked_sums, locked_git, unverifiable = set(), set(), []
    for p in packages:
        source = p.get("source")
        if source is None:
            continue
        if source == CRATES_IO and "checksum" in p:
            locked_sums.add(p["checksum"])
        elif source.startswith("git+") and re.search(r"#[0-9a-f]{40}$", source):
            locked_git.add(source[-40:])
        else:
            unverifiable.append(f"{p['name']} {p['version']} ({source})")
    if unverifiable:
        print(
            f"::error::{CARGO_LOCK.name} has packages this check cannot match against "
            f"{CARGO_SOURCES.name} (neither a checksummed crates.io crate nor a git "
            f"dependency at a commit): {', '.join(unverifiable)}. Use a crates.io or git "
            "source, or teach Scripts/check-flatpak-pins.py about the new kind."
        )
        failed = True
    vendored = json.loads(CARGO_SOURCES.read_text())
    vendored_sums = {s["sha256"] for s in vendored if s.get("type") == "archive"}
    vendored_git = {s.get("commit") for s in vendored if s.get("type") == "git"}
    if locked_sums != vendored_sums or locked_git != vendored_git:
        print(
            f"::error::{CARGO_SOURCES.name} does not match {CARGO_LOCK.name} "
            f"({len(locked_sums - vendored_sums)} crates missing, "
            f"{len(vendored_sums - locked_sums)} stale; "
            f"{len(locked_git - vendored_git)} git dependencies missing, "
            f"{len(vendored_git - locked_git)} stale). {REGENERATE}"
        )
        failed = True
    else:
        git_note = f" and {len(locked_git)} git dependencies" if locked_git else ""
        print(
            f"ok: {CARGO_SOURCES.name} vendors the {len(locked_sums)} crates{git_note} "
            f"in {CARGO_LOCK.name}"
        )

    # sherpa-onnx-sys links the sherpa-onnx / ONNX Runtime static libraries
    # the manifest builds from source (SHERPA_ONNX_LIB_DIR); they have to be
    # the release the bindings were written for.
    sherpa = versions.get("sherpa-onnx-sys")
    if sherpa is not None and check_speech_stack(sherpa):
        failed = True

    return failed


def main() -> int:
    failed = False

    if "--fix" in sys.argv[1:]:
        for name, path, url_fragment in HARD_PINS:
            sha = staged_submodule_sha(path)
            if fix_manifest_pin(url_fragment, sha):
                print(f"fixed: {name} manifest pin -> {sha[:12]}")
        # The checks below compare against HEAD, which a not yet committed
        # bump has not reached; the fix itself is the result here. The one
        # thing --fix cannot bring along is the offline cargo inputs, so a
        # lock generated for another rust-tts-wrapper commit is reported
        # (never re-recorded: that would bless a stale lock).
        return 1 if check_lock_commit(staged_submodule_sha("rust-tts-wrapper")) else 0

    # ── rust-tts-wrapper, DasherCore: hard equality with the submodule pin ──
    for name, path, url_fragment in HARD_PINS:
        subm = submodule_sha(path)
        commit, _ = manifest_git_pin(url_fragment)
        if commit is None:
            print(
                f"::error::No commit: pin found for the {name} git source in "
                f"{MANIFEST}: the Flatpak build would clone a moving branch. "
                "Pin it to the submodule SHA (Scripts/check-flatpak-pins.py --fix)."
            )
            failed = True
        elif commit != subm:
            print(
                f"::error::Flatpak manifest pins {name} at {commit[:12]} but the "
                f"submodule is at {subm[:12]}. The Flatpak leg would build a stale "
                f"{name} against the current app code (this is how the v6.0.2 publish "
                "failed). Update the manifest's commit to the submodule SHA "
                "(Scripts/check-flatpak-pins.py --fix)."
            )
            failed = True
        else:
            print(f"ok: {name} manifest pin == submodule ({subm[:12]})")

    # ── Offline cargo inputs follow the rust-tts-wrapper pin ────────
    if check_cargo_inputs(submodule_sha("rust-tts-wrapper")):
        failed = True

    # ── SDL: tag + commit pin; warn when the submodule has moved past it ────
    sdl_commit, tag = manifest_git_pin("libsdl-org/SDL")
    sdl_subm = submodule_sha("Thirdparty/SDL")
    # Both pins are read from the manifest, so a missing one fails here:
    # before the upstream lookup, and whether or not GitHub can be reached.
    if tag is None:
        print("::error::No tag: pin found for the SDL git source — pin a release tag.")
        failed = True
    if sdl_commit is None:
        print(
            "::error::No commit: pin (a full 40-character SHA) found "
            + (f"next to the SDL tag {tag}" if tag else "for the SDL git source")
            + f" in {MANIFEST}. Add the commit the tag resolves to, so a moved tag "
            "cannot change the build."
        )
        failed = True
    if tag is not None:
        # Resolve the tag without a checkout: ask the submodule's remote.
        remote, error = remote_tag_commit(
            "https://github.com/libsdl-org/SDL.git",
            tag,
            f"that SDL tag {tag} resolves to the pinned commit upstream"
            if sdl_commit
            else f"which commit SDL tag {tag} resolves to upstream",
        )
        if error:
            print(
                f"::error::SDL tag {tag} pinned in the manifest does not resolve "
                f"upstream ({error})."
            )
            failed = True
        elif remote is None:
            pass  # remote unreachable: skipped, reported at the end of the run
        elif sdl_commit is None:
            # The error is reported above; upstream answered, so name the commit.
            print(f"hint: SDL tag {tag} resolves to {remote} upstream: add `commit: {remote}`.")
        elif sdl_commit != remote:
            print(
                f"::error::Manifest pins SDL tag {tag} with commit {sdl_commit[:12]} "
                f"but the tag resolves to {remote[:12]} upstream. flatpak-builder "
                "refuses that; update the commit together with the tag."
            )
            failed = True
        elif remote != sdl_subm:
            print(
                f"::warning::Manifest pins SDL tag {tag} ({remote[:12]}) but the "
                f"submodule is at {sdl_subm[:12]} — the Flatpak build compiles a "
                "different SDL than the rest of the CI. Allowed, but usually unintended."
            )
        else:
            print(f"ok: SDL manifest tag == submodule ({sdl_subm[:12]})")

    # A skipped check is not a passed one: say so last, where it is read.
    if SKIPPED:
        print(
            f"::warning::INCOMPLETE RUN: {len(SKIPPED)} check(s) were SKIPPED because the "
            f"network could not be reached. Not verified: {'; '.join(SKIPPED)}. "
            + (
                "The errors above stand."
                if failed
                else "Nothing that did run failed, so this exits 0, but it is not a full "
                "pass: run it again with network access."
            )
        )
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
